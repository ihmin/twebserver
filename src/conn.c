/**
 * Copyright Jerily LTD. All Rights Reserved.
 * SPDX-FileCopyrightText: 2024 Neofytos Dimitriou (neo@jerily.cy)
 * SPDX-License-Identifier: MIT.
 */

#include <sys/socket.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include "conn.h"
#include "request.h"
#include "https.h"
#include "router.h"
#include "return.h"
#include <netdb.h>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <sys/event.h>
#include <assert.h>

#else

#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <assert.h>

#endif

#define MAX_EVENTS 100
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#define MAX_BUFFER_SIZE 1024
#endif

enum {
    TWS_MODE_BLOCKING,
    TWS_MODE_NONBLOCKING
};

static int tws_HandleRecv(tws_conn_t *conn);
static void tws_KeepaliveConnHandler(void *data, int mask);
static int tws_AddConnToThreadList(tws_conn_t *conn);
static int tws_HandleWrite(tws_conn_t *conn);

tws_server_t *tws_GetCurrentServer() {
    tws_thread_data_t *dataPtr = (tws_thread_data_t *) Tcl_GetThreadData(tws_GetThreadDataKey(), sizeof(tws_thread_data_t));
    return dataPtr->server;
}

static int tws_SetBlockingMode(
        int fd,
        int mode            /* Either TWS_MODE_BLOCKING or TWS_MODE_NONBLOCKING. */
) {
    int flags = fcntl(fd, F_GETFL);

    if (mode == TWS_MODE_BLOCKING) {
        flags &= ~O_NONBLOCK;
    } else {
        flags |= O_NONBLOCK;
    }
    return fcntl(fd, F_SETFL, flags);
}

static int bind_socket(Tcl_Interp *interp, int server_fd, const char *host, int port_num) {
    if (host != NULL) {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
        struct hostent *hostent = gethostbyname(host);
        if (hostent == NULL) {
            SetResult("Unable to get host by name");
            return TCL_ERROR;
        }
#else
        struct hostent *hostent;
        struct hostent hostent_data;
        char buffer[1024];
        int herrno;
        int ret = gethostbyname_r(host, &hostent_data, buffer, sizeof(buffer), &hostent, &herrno);
        if (ret != 0) {
            SetResult("Unable to get host by name");
            return TCL_ERROR;
        }
#endif

        // Iterate over the list of IP addresses returned by gethostbyname
        for (int i = 0; hostent->h_addr_list[i] != NULL; i++) {
            struct sockaddr_in6 ipv6_addr;
            struct sockaddr_in ipv4_addr;

            if (hostent->h_addrtype == AF_INET6) {
                fprintf(stderr, "ipv6\n");
                memset(&ipv6_addr, 0, sizeof(ipv6_addr));
                ipv6_addr.sin6_family = AF_INET6;
                ipv6_addr.sin6_port = htons(port_num);

                memcpy(&ipv6_addr.sin6_addr, hostent->h_addr_list[i], sizeof(struct in6_addr));

            } else if (hostent->h_addrtype == AF_INET) {
                memset(&ipv4_addr, 0, sizeof(ipv4_addr));
                ipv4_addr.sin_family = AF_INET;
                ipv4_addr.sin_port = htons(port_num);

                memcpy(&ipv4_addr.sin_addr.s_addr, hostent->h_addr_list[i], sizeof(struct in_addr));

                //convert it to an ipv6 mapped ipv4 address
                struct in6_addr v4addr = IN6ADDR_ANY_INIT;
                v4addr.s6_addr[10] = 0xff;
                v4addr.s6_addr[11] = 0xff;
                v4addr.s6_addr[12] = ipv4_addr.sin_addr.s_addr & 0xff;
                v4addr.s6_addr[13] = (ipv4_addr.sin_addr.s_addr >> 8) & 0xff;
                v4addr.s6_addr[14] = (ipv4_addr.sin_addr.s_addr >> 16) & 0xff;
                v4addr.s6_addr[15] = (ipv4_addr.sin_addr.s_addr >> 24) & 0xff;

                memset(&ipv6_addr, 0, sizeof(ipv6_addr));
                ipv6_addr.sin6_family = AF_INET6;
                ipv6_addr.sin6_port = htons(port_num);
                memcpy(&ipv6_addr.sin6_addr, &v4addr, sizeof(struct in6_addr));

            } else {
                SetResult("Unknown address family");
                return TCL_ERROR;
            }

            if (bind(server_fd, (struct sockaddr *) &ipv6_addr, sizeof(ipv6_addr)) < 0) {
                SetResult("Unable to bind ipv6 addr");
                return TCL_ERROR;
            }

            // print address
            char straddr[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, &ipv6_addr.sin6_addr, straddr, INET6_ADDRSTRLEN);
            fprintf(stderr, "bind successful on ipv6 addr: %s\n", straddr);

        }
    } else {
        // bind the socket to the wildcard address and the specified port
        struct sockaddr_in6 server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin6_family = AF_INET6;
        server_addr.sin6_addr = in6addr_any;
        server_addr.sin6_port = htons(port_num);

        if (bind(server_fd, (struct sockaddr *) &server_addr, sizeof(server_addr)) < 0) {
            SetResult("Unable to bind ipv4 addr");
            return TCL_ERROR;
        }
    }
    return TCL_OK;
}

static int create_socket(Tcl_Interp *interp, tws_server_t *server, const char *host, const char *port, int *server_sock) {
    int server_fd;

    // create an IPv6 TCP socket
    server_fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (server_fd < 0) {
        SetResult("Unable to create socket");
        return TCL_ERROR;
    }

    // disable IPV6_V6ONLY option
    int optval = 0;
    if (setsockopt(server_fd, IPPROTO_IPV6, IPV6_V6ONLY, &optval, sizeof(optval)) == -1) {
        SetResult("Unable to set IPV6_V6ONLY");
        return TCL_ERROR;
    }

    // Set the close-on-exec flag so that the socket will not get inherited by child processes.
    fcntl(server_fd, F_SETFD, FD_CLOEXEC);

    int reuseaddr = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (char *) &reuseaddr, sizeof(reuseaddr))) {
        DBG(fprintf(stderr, "setsockopt SO_REUSEADDR failed"));
    }

    int reuseport = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, (char *) &reuseport, sizeof(reuseport))) {
        DBG(fprintf(stderr, "setsockopt SO_REUSEPORT failed"));
    }

    if (server->keepalive) {
        if (setsockopt(server_fd, SOL_SOCKET, SO_KEEPALIVE, &server->keepalive, sizeof(int))) {
            DBG(fprintf(stderr, "setsockopt SO_KEEPALIVE failed"));
        }

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#else
        // Set the TCP_KEEPIDLE option on the socket
        if (setsockopt(server_fd, IPPROTO_TCP, TCP_KEEPIDLE, &server->keepidle, sizeof(int)) == -1) {
            DBG(fprintf(stderr, "setsockopt TCP_KEEPIDLE failed"));
        }

        // Set the TCP_KEEPINTVL option on the socket
        if (setsockopt(server_fd, IPPROTO_TCP, TCP_KEEPINTVL, &server->keepintvl, sizeof(int)) == -1) {
            DBG(fprintf(stderr, "setsockopt TCP_KEEPINTVL failed"));
        }

        // Set the TCP_KEEPCNT option on the socket
        if (setsockopt(server_fd, IPPROTO_TCP, TCP_KEEPCNT, &server->keepcnt, sizeof(int)) == -1) {
            DBG(fprintf(stderr, "setsockopt TCP_KEEPCNT failed"));
        }
#endif
    }

    int port_num = atoi(port);
    if (TCL_OK != bind_socket(interp, server_fd, host, port_num)) {
        return TCL_ERROR;
    }

    tws_SetBlockingMode(server_fd, TWS_MODE_NONBLOCKING);

    int backlog = server->backlog; // the maximum length to which the  queue  of pending  connections  for sockfd may grow
    if (listen(server_fd, backlog) < 0) {
        SetResult("Unable to listen");
        return TCL_ERROR;
    }

    tws_SetBlockingMode(server_fd, TWS_MODE_NONBLOCKING);

    *server_sock = server_fd;
    return TCL_OK;
}

static int create_epoll_fd(Tcl_Interp *interp, int server_fd, int *epoll_sock) {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    // Create a kqueue instance
    int epoll_fd = kqueue();
    if (epoll_fd == -1) {
        SetResult("Unable to create kqueue instance");
        return TCL_ERROR;
    }
#else
    // Create an epoll instance
    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        SetResult("Unable to create epoll instance");
        return TCL_ERROR;
    }
#endif



#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    // Add the server socket to the kqueue set
    struct kevent ev;
    EV_SET(&ev, server_fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, server_fd);
    if (kevent(epoll_fd, &ev, 1, NULL, 0, NULL) == -1) {
        SetResult("Unable to add server socket to kqueue set");
        return TCL_ERROR;
    }
#else
    // Add the server socket to the epoll set
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = server_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) == -1) {
        SetResult("Unable to add server socket to epoll set");
        return TCL_ERROR;
    }
#endif

    *epoll_sock = epoll_fd;
    return TCL_OK;
}

tws_conn_t *tws_NewConn(tws_accept_ctx_t *accept_ctx, int client, char client_ip[INET6_ADDRSTRLEN]) {
    tws_SetBlockingMode(client, TWS_MODE_NONBLOCKING);

    tws_conn_t *conn = (tws_conn_t *) Tcl_Alloc(sizeof(tws_conn_t));

    if (accept_ctx->option_http) {
        conn->ssl = NULL;
    } else {
        SSL *ssl = SSL_new(accept_ctx->ssl_ctx);
        if (ssl == NULL) {
            Tcl_Free((char *) conn);
            return NULL;
        }
        SSL_set_fd(ssl, client);
        SSL_set_accept_state(ssl);
        conn->ssl = ssl;
    }

    conn->accept_ctx = accept_ctx;
    conn->handle_conn_fn = accept_ctx->handle_conn_fn;
    conn->client = client;
    conn->compression = NO_COMPRESSION;
    conn->keepalive = 0;
    conn->created_file_handler_p = 0;
    conn->ready = 0;
    conn->handshaked = 0;
    conn->inprogress = 0;
    conn->todelete = 0;
    conn->shutdown = 0;
    conn->prevPtr = NULL;
    conn->nextPtr = NULL;
    memcpy(conn->client_ip, client_ip, INET6_ADDRSTRLEN);
    conn->encoding = Tcl_GetEncoding(accept_ctx->interp, "utf-8");
    Tcl_DStringInit(&conn->inout_ds);
    Tcl_DStringInit(&conn->parse_ds);
    conn->req_dict_ptr = NULL;
    conn->top_part_offset = 0;
    conn->write_offset = 0;
    conn->content_length = 0;
    conn->error = 0;
    conn->blank_line_offset = 0;

//        fprintf(stderr, "tws_NewConn - num_threads: %d\n", accept_ctx->server->num_threads);
//        fprintf(stderr, "tws_NewConn - client: %d\n", client);
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    conn->threadId = accept_ctx->conn_thread_ids[client % accept_ctx->num_threads];
#else
    conn->threadId = Tcl_GetCurrentThread();
#endif
    conn->start_read_millis = current_time_in_millis();
    conn->latest_millis = conn->start_read_millis;

    return conn;
}

static int tws_HandleProcessing(tws_conn_t *conn) {
    DBG(fprintf(stderr, ">>>>>>>>>>>>>>>>> HandleProcessing: %s\n", conn->handle));

    tws_accept_ctx_t *accept_ctx = conn->accept_ctx;

    // Get a pointer to the thread data for the current thread
    tws_thread_data_t *dataPtr = (tws_thread_data_t *) Tcl_GetThreadData(tws_GetThreadDataKey(), sizeof(tws_thread_data_t));
    // Get the interp from the thread data

    if (accept_ctx->server->option_router) {
        Tcl_Interp *targetInterpPtr;
        const char *targetCmdPtr;
        Tcl_Size objc;
        Tcl_Obj **objv;
        if (TCL_OK != Tcl_GetAliasObj(dataPtr->interp, Tcl_GetString(accept_ctx->server->cmdPtr), &targetInterpPtr, &targetCmdPtr, &objc, &objv)) {
            fprintf(stderr, "error getting alias\n");
            tws_CloseConn(conn, 1);
            return 1;
        }

        DBG(fprintf(stderr, "targetCmdPtr=%s\n", targetCmdPtr));

        tws_router_t *router = tws_GetInternalFromRouterName(targetCmdPtr);
        if (router == NULL) {
            fprintf(stderr, "error getting router\n");
            tws_CloseConn(conn, 1);
            return 1;
        }
        tws_HandleRouteEventInThread(router, conn, conn->req_dict_ptr);
        return 1;
    }

    Tcl_Obj *ctx_dict_ptr;
    if (TCL_OK != tws_CreateContextDict(dataPtr->interp, conn, &ctx_dict_ptr)) {
        fprintf(stderr, "error creating context dict\n");
        tws_CloseConn(conn, 1);
        return 1;
    }

    Tcl_Obj *const cmdobjv[] = {dataPtr->cmdPtr, ctx_dict_ptr, conn->req_dict_ptr, NULL};

    tws_IncrRefCountObjv(3, cmdobjv);
    Tcl_ResetResult(dataPtr->interp);
    if (TCL_OK != Tcl_EvalObjv(dataPtr->interp, 3, cmdobjv, TCL_EVAL_GLOBAL)) {
        fprintf(stderr, "error evaluating script sock=%d\n", conn->client);

        tws_DecrRefCountObjv(3, cmdobjv);
        Tcl_DecrRefCount(ctx_dict_ptr);

        Tcl_Obj *return_options_dict_ptr = Tcl_GetReturnOptions(dataPtr->interp, TCL_ERROR);
        Tcl_IncrRefCount(return_options_dict_ptr);
        Tcl_Obj *errorinfo_key_ptr = Tcl_NewStringObj("-errorinfo", -1);
        Tcl_Obj *errorinfo_ptr;
        Tcl_IncrRefCount(errorinfo_key_ptr);
        if (TCL_OK != Tcl_DictObjGet(dataPtr->interp, return_options_dict_ptr, errorinfo_key_ptr, &errorinfo_ptr)) {
            fprintf(stderr, "error getting errorinfo\n");
            Tcl_DecrRefCount(errorinfo_key_ptr);
            Tcl_DecrRefCount(return_options_dict_ptr);
            return 1;
        }
        Tcl_DecrRefCount(errorinfo_key_ptr);
        fprintf(stderr, "HandleProcessing: errorinfo=%s\n", Tcl_GetString(errorinfo_ptr));
        Tcl_DecrRefCount(return_options_dict_ptr);

        tws_CloseConn(conn, 1);
        return 1;
    }
    tws_DecrRefCountObjv(3, cmdobjv);
    Tcl_DecrRefCount(ctx_dict_ptr);

    return 1;
}

static int tws_FoundBlankLine(tws_conn_t *conn) {
    if (conn->blank_line_offset == -1) {
        return 1;
    }

    const char *s = Tcl_DStringValue(&conn->inout_ds);
    const char *p = s + conn->blank_line_offset;
    const char *end = s + Tcl_DStringLength(&conn->inout_ds);
    while (p < end) {
        if ((p < end - 3 && *p == '\r' && *(p + 1) == '\n' && *(p + 2) == '\r' && *(p + 3) == '\n') || (p < end - 1 && *p == '\n' && *(p + 1) == '\n')) {
            DBG(fprintf(stderr, "FoundBlankLine\n"));
            conn->blank_line_offset = -1;
            return 1;
        }
        p++;
    }

    DBG(fprintf(stderr, "NotFoundBlankLine\n"));
    conn->blank_line_offset = p - s;
    return 0;
}

static int tws_ShouldParseTopPart(tws_conn_t *conn) {
    return Tcl_DStringLength(&conn->parse_ds) == 0 && Tcl_DStringLength(&conn->inout_ds) > 0 && tws_FoundBlankLine(conn);
}

static int tws_ShouldParseBottomPart(tws_conn_t *conn) {
    return conn->content_length > 0;
}

static int tws_ShouldReadMore(tws_conn_t *conn) {
    if (conn->content_length > 0) {
        int content_read_but_not_processed = Tcl_DStringLength(&conn->inout_ds) - conn->top_part_offset;
        return conn->content_length - content_read_but_not_processed > 0;
    }
    return !tws_FoundBlankLine(conn);
}


static int tws_HandleRecv(tws_conn_t *conn) {
    DBG(fprintf(stderr, "HandleRecv: %d %s\n", conn->client, conn->handle));

    assert(valid_conn_handle(conn));

    if (conn->ready) {
        DBG(fprintf(stderr, "HandleRecv - already ready\n"));
        return 1;
    }

    if (tws_ShouldParseTopPart(conn)) {
        // case when we have read as much as we could after retry
        int error_num = 0;
        if (TCL_OK != tws_ParseTopPart(conn, &error_num)) {
            fprintf(stderr, "ParseTopPart failed (before rubicon): %s conn: %s\n",
                    tws_parse_error_messages[error_num], conn->handle);

            // ProcessEventInThread will return Bad Request and close the connection
            Tcl_DStringSetLength(&conn->parse_ds, 0);
            conn->ready = 1;
            return 1;
        }
    }

    // return 400 if we exceeded read timeout
    long long elapsed = current_time_in_millis() - conn->start_read_millis;
    if (elapsed > conn->accept_ctx->server->read_timeout_millis) {
        DBG(fprintf(stderr, "exceeded read timeout: %lld\n", elapsed));
        // ProcessEventInThread will return Bad Request and close the connection
        Tcl_DStringSetLength(&conn->parse_ds, 0);
        conn->ready = 1;
        return 1;
    }

    int ret = TWS_DONE;
    if (tws_ShouldReadMore(conn)) {
        Tcl_Size content_read_but_not_processed = Tcl_DStringLength(&conn->inout_ds) - conn->top_part_offset;
        Tcl_Size bytes_to_read = conn->content_length == 0 ? 0 : conn->content_length - content_read_but_not_processed;
        ret = conn->accept_ctx->read_fn(conn, &conn->inout_ds, bytes_to_read);
    }

    if (TWS_AGAIN == ret) {
        if (tws_ShouldParseTopPart(conn) || tws_ShouldReadMore(conn)) {
            DBG(fprintf(stderr, "retry dslen=%zd offset=%zd parsedslen=%zd\n", Tcl_DStringLength(&conn->inout_ds), conn->top_part_offset,
                        Tcl_DStringLength(&conn->parse_ds)));
            return 0;
        }
    } else if (TWS_ERROR == ret) {
        conn->error = 1;
        tws_CloseConn(conn, 2);
        return 1;
    } else if (TWS_DONE == ret && Tcl_DStringLength(&conn->inout_ds) == 0) {
        // peer closed connection?
        tws_CloseConn(conn, 1);
        return 1;
    }

    DBG(fprintf(stderr, "rubicon ret=%d dslen=%ld content_length=%ld\n", ret,
                Tcl_DStringLength(&conn->inout_ds), conn->content_length));

    if (tws_ShouldParseTopPart(conn)) {
        DBG(fprintf(stderr, "parse top part after without defer reqdictptr=%p\n", conn->req_dict_ptr));
        // case when we have read as much as we could without deferring
        int error_num = 0;
        if (TCL_OK != tws_ParseTopPart(conn, &error_num)) {
            fprintf(stderr, "ParseTopPart failed (after rubicon): %s\n",
                    tws_parse_error_messages[error_num]);
            // ProcessEventInThread will return Bad Request and close the connection
            Tcl_DStringSetLength(&conn->parse_ds, 0);
            conn->ready = 1;
            return 1;
        }
    }

    if (tws_ShouldParseBottomPart(conn)) {
        int error_num = 0;
        if (TCL_OK != tws_ParseBottomPart(conn, &error_num)) {
            fprintf(stderr, "ParseBottomPart failed: %s\n", tws_parse_error_messages[error_num]);
            tws_CloseConn(conn, 1);
            return 1;
        }
    } else {
        DBG(fprintf(stderr, "conn->parse_ds len: %ld\n", Tcl_DStringLength(&conn->parse_ds)));

        if (!Tcl_DStringLength(&conn->parse_ds)) {
            DBG(fprintf(stderr, "parse ds is empty, inout_ds: %s\n", Tcl_DStringValue(&conn->inout_ds)));
            conn->ready = 1;
            // ProcessEventInThread will return Bad Request and close the connection
            return 1;
        }
    }

    DBG(fprintf(stderr, "HandleRecv done\n"));

    conn->ready = 1;
    return 1;
}

int tws_HandleSslHandshake(tws_conn_t *conn) {

    assert(valid_conn_handle(conn));

    if (conn->handshaked) {
        fprintf(stderr, "HandleSslHandshake: already handshaked\n");
        return 1;
    }
    ERR_clear_error();
    int rc = SSL_accept(conn->ssl);
    if (rc == 1) {
        DBG(fprintf(stderr, "HandleHandshake: success\n"));
        conn->handshaked = 1;
        conn->handle_conn_fn = tws_HandleRecv;
        return 1;
    }

    int err = SSL_get_error(conn->ssl, rc);
    if (err == SSL_ERROR_WANT_READ) {
        DBG(fprintf(stderr, "HandleHandshake: SSL_ERROR_WANT_READ\n"));
        return 0;
    } else if (err == SSL_ERROR_WANT_WRITE) {
        DBG(fprintf(stderr, "HandleHandshake: SSL_ERROR_WANT_WRITE\n"));
        return 0;
    } else if (err == SSL_ERROR_ZERO_RETURN || ERR_peek_error() == 0) {
        fprintf(stderr, "peer closed connection in SSL handshake\n");
        conn->error = 1;
        tws_CloseConn(conn, 1);
        return 1;
    }
    fprintf(stderr, "SSL_accept <= 0 client: %d err=%s\n", conn->client, ssl_errors[err]);
    conn->error = 1;
    tws_CloseConn(conn, 1);
    ERR_print_errors_fp(stderr);
    return 1;
}

int tws_HandleTermEventInThread(Tcl_Event *evPtr, int flags) {
    tws_thread_data_t *dataPtr = (tws_thread_data_t *) Tcl_GetThreadData(tws_GetThreadDataKey(), sizeof(tws_thread_data_t));

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#else
    Tcl_DeleteFileHandler(dataPtr->server_fd);
    close(dataPtr->server_fd);
#endif
    Tcl_DeleteFileHandler(dataPtr->epoll_fd);

    dataPtr->terminate = 1;
    Tcl_ThreadAlert(Tcl_GetCurrentThread());
    return 1;
}

static int tws_SetDefaultBodyIfNeeded(Tcl_Interp *interp, tws_conn_t *conn) {
    if (!conn->content_length) {
        if (TCL_OK !=
            Tcl_DictObjPut(interp, conn->req_dict_ptr, Tcl_NewStringObj("isBase64Encoded", -1),
                           Tcl_NewBooleanObj(0))) {
            fprintf(stderr, "failed to write to dict 1\n");
            tws_CloseConn(conn, 1);
            return TCL_ERROR;
        }
        if (TCL_OK !=
            Tcl_DictObjPut(interp, conn->req_dict_ptr, Tcl_NewStringObj("body", -1), Tcl_NewStringObj("", -1))) {
            fprintf(stderr, "failed to write to dict 2\n");
            tws_CloseConn(conn, 1);
            return TCL_ERROR;
        }
    }
    return TCL_OK;
}

static int tws_HandleProcessEventInThread(Tcl_Event *evPtr, int flags) {
    tws_event_t *connEvPtr = (tws_event_t *) evPtr;
    tws_conn_t *conn = (tws_conn_t *) connEvPtr->clientData;

    assert(valid_conn_handle(conn));

    if (conn->ready || conn->shutdown) {
        DBG(fprintf(stderr, "HandleProcessEventInThread: ready: %d shutdown: %d\n", conn->ready, conn->shutdown));
        return 1;
    }

    DBG(fprintf(stderr, "HandleProcessEventInThread: %s (%p)\n", conn->handle, conn->handle_conn_fn));
    int rc = conn->handle_conn_fn(conn);
    if (!rc) {
        Tcl_ThreadAlert(conn->threadId);
        return 0;
    }
    DBG(fprintf(stderr, "HandleProcessEventInThread: ready=%d (%p)\n", conn->ready, conn->handle_conn_fn));

    int ready = conn->ready;
    if (ready) {
        if (!conn->inprogress) {
            conn->inprogress = 1;
            tws_thread_data_t *dataPtr = (tws_thread_data_t *) Tcl_GetThreadData(tws_GetThreadDataKey(), sizeof(tws_thread_data_t));
            Tcl_InterpState interp_state = Tcl_SaveInterpState(dataPtr->interp, TCL_OK);

            if (!Tcl_DStringLength(&conn->parse_ds)) {
                if (TCL_OK != tws_ReturnError(dataPtr->interp, conn, 400, "Bad Request")) {
                    tws_CloseConn(conn, 1);
                }
                Tcl_RestoreInterpState(dataPtr->interp, interp_state);
                return 1;
            }

#if TCL_MAJOR_VERSION > 8
            conn->req_dict_ptr = Tcl_DStringToObj(&conn->parse_ds);
#else
            Tcl_DStringResult(dataPtr->interp, &conn->parse_ds);
            conn->requestDictPtr = Tcl_GetObjResult(dataPtr->interp);
#endif
            Tcl_IncrRefCount(conn->req_dict_ptr);
            if (TCL_OK != tws_SetDefaultBodyIfNeeded(dataPtr->interp, conn)) {
                Tcl_RestoreInterpState(dataPtr->interp, interp_state);
                return 1;
            }
            tws_HandleProcessing(conn);
            Tcl_RestoreInterpState(dataPtr->interp, interp_state);
        }
    }
    Tcl_ThreadAlert(conn->threadId);
    return ready;
}

static void tws_QueueProcessEvent(tws_conn_t *conn) {

    assert(valid_conn_handle(conn));

    DBG(fprintf(stderr, "ThreadQueueProcessEvent - threadId: %p\n", conn->threadId));
    tws_event_t *connEvPtr = (tws_event_t *) Tcl_Alloc(sizeof(tws_event_t));
    connEvPtr->proc = tws_HandleProcessEventInThread;
    connEvPtr->nextPtr = NULL;
    connEvPtr->clientData = (ClientData *) conn;
    Tcl_QueueEvent((Tcl_Event *) connEvPtr, TCL_QUEUE_TAIL);
     Tcl_ThreadAlert(conn->threadId);
    DBG(fprintf(stderr, "ThreadQueueProcessEvent done - threadId: %p\n", conn->threadId));
}

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
static int tws_HandleConnEventInThread(Tcl_Event *evPtr, int flags) {
    tws_event_t *connEvPtr = (tws_event_t *) evPtr;
    tws_conn_t *conn = (tws_conn_t *) connEvPtr->clientData;

    DBG(fprintf(stderr, "current thread: %p conn->threadId: %p\n", Tcl_GetCurrentThread(), conn->threadId));

    if (tws_AddConnToThreadList(conn)) {
        tws_QueueProcessEvent(conn);
    }
    return 1;
}

// this is called from the main thread
// to queue the event in the thread that the connection will be processed
static void tws_ThreadQueueConnEvent(tws_conn_t *conn) {
    DBG(fprintf(stderr, "ThreadQueueConnEvent - threadId: %p\n", conn->threadId));
    tws_event_t *connEvPtr = (tws_event_t *) Tcl_Alloc(sizeof(tws_event_t));
    connEvPtr->proc = tws_HandleConnEventInThread;
    connEvPtr->nextPtr = NULL;
    connEvPtr->clientData = (ClientData *) conn;
    Tcl_ThreadQueueEvent(conn->threadId, (Tcl_Event *) connEvPtr, TCL_QUEUE_TAIL);
    Tcl_ThreadAlert(conn->threadId);
    DBG(fprintf(stderr, "ThreadQueueConnEvent done - threadId: %p\n", conn->threadId));
}

static int tws_HandleKeepaliveEventInThread(Tcl_Event *evPtr, int flags) {
    tws_event_t *connEvPtr = (tws_event_t *) evPtr;
    tws_conn_t *conn = (tws_conn_t *) connEvPtr->clientData;

    DBG(fprintf(stderr, "current thread: %p conn->threadId: %p\n", Tcl_GetCurrentThread(), conn->threadId));
    conn->start_read_millis = current_time_in_millis();
    conn->latest_millis = conn->start_read_millis;

    tws_QueueProcessEvent(conn);

    return 1;
}

static void tws_ThreadQueueKeepaliveEvent(tws_conn_t *conn) {
    DBG(fprintf(stderr, "ThreadQueueKeepaliveEvent - threadId: %p\n", conn->threadId));
    tws_event_t *connEvPtr = (tws_event_t *) Tcl_Alloc(sizeof(tws_event_t));
    connEvPtr->proc = tws_HandleKeepaliveEventInThread;
    connEvPtr->nextPtr = NULL;
    connEvPtr->clientData = (ClientData *) conn;
    Tcl_ThreadQueueEvent(conn->threadId, (Tcl_Event *) connEvPtr, TCL_QUEUE_TAIL);
    Tcl_ThreadAlert(conn->threadId);
    DBG(fprintf(stderr, "ThreadQueueKeepaliveEvent done - threadId: %p\n", conn->threadId));
}
#endif


int tws_InfoConnCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[]) {
    DBG(fprintf(stderr, "InfoConnCmd\n"));
    CheckArgs(2, 2, 1, "conn_handle");

    const char *conn_handle = Tcl_GetString(objv[1]);
    tws_conn_t *conn = tws_GetInternalFromConnName(conn_handle);
    if (!conn) {
        SetResult("info_conn: conn handle not found");
        return TCL_ERROR;
    }

    Tcl_Obj *result_ptr = Tcl_NewDictObj();
    Tcl_IncrRefCount(result_ptr);

    if (conn->req_dict_ptr) {
        if (TCL_OK != Tcl_DictObjPut(interp, result_ptr, Tcl_NewStringObj("request", -1), conn->req_dict_ptr)) {
            fprintf(stderr, "error writing to dict\n");
            Tcl_DecrRefCount(result_ptr);
            return TCL_ERROR;
        }
    }

    if (TCL_OK != Tcl_DictObjPut(interp, result_ptr, Tcl_NewStringObj("server", -1), Tcl_NewStringObj(conn->accept_ctx->server->handle, -1))) {
        fprintf(stderr, "error writing to dict\n");
        Tcl_DecrRefCount(result_ptr);
        return TCL_ERROR;
    }

    Tcl_SetObjResult(interp, result_ptr);
    Tcl_DecrRefCount(result_ptr);
    return TCL_OK;
}

static int tws_AddConnToThreadList(tws_conn_t *conn) {

    assert(valid_conn_handle(conn));

    Tcl_MutexLock(tws_GetThreadMutex());
    tws_thread_data_t *dataPtr = (tws_thread_data_t *) Tcl_GetThreadData(tws_GetThreadDataKey(), sizeof(tws_thread_data_t));

    // prefer to refuse connection if we are over the limit
    // this is to cap memory usage
    int thread_limit = conn->accept_ctx->server->thread_max_concurrent_conns;
    if (thread_limit > 0 && dataPtr->num_conns >= thread_limit) {
        fprintf(stderr, "thread limit reached, close client: %d\n", conn->client);
        shutdown(conn->client, SHUT_RDWR);
        close(conn->client);
        SSL_free(conn->ssl);
        Tcl_Free((char *) conn);
        Tcl_MutexUnlock(tws_GetThreadMutex());
        return 0;
    }

    if (dataPtr->firstConnPtr == NULL) {
        dataPtr->firstConnPtr = conn;
        dataPtr->lastConnPtr = conn;
    } else {
        dataPtr->lastConnPtr->nextPtr = conn;
        conn->prevPtr = dataPtr->lastConnPtr;
        dataPtr->lastConnPtr = conn;
    }
    dataPtr->num_conns++;

    DBG(fprintf(stderr, "AddConnToThreatList - dataKey: %p thread: %p numConns: %d FD_SETSIZE: %d thread_limit: %d\n", tws_GetThreadDataKey(), Tcl_GetCurrentThread(), dataPtr->num_conns, FD_SETSIZE, thread_limit));

    Tcl_MutexUnlock(tws_GetThreadMutex());

    return 1;
}

void tws_AcceptConn(void *data, int mask) {
    DBG(fprintf(stderr, "-------------------tws_AcceptConn\n"));

    tws_accept_ctx_t *accept_ctx = (tws_accept_ctx_t *) data;

        // new incoming connection

        struct sockaddr_in6 client_addr;
        unsigned int len = sizeof(client_addr);
        int client = accept(accept_ctx->server_fd, (struct sockaddr *) &client_addr, &len);
        DBG(fprintf(stderr, "client: %d\n", client));
        if (client < 0) {
            fprintf(stderr, "Unable to accept\n");
            return;
        }

        // get the client IP address
        char client_ip[INET6_ADDRSTRLEN];
        inet_ntop(AF_INET6, &client_addr.sin6_addr, client_ip, sizeof(client_ip));
        DBG(fprintf(stderr, "Client connected from %s\n", client_ip));

        tws_conn_t *conn = tws_NewConn(accept_ctx, client, client_ip);
        if (conn == NULL) {
            shutdown(client, SHUT_WR);
            shutdown(client, SHUT_RD);
            close(client);
            DBG(fprintf(stderr, "Unable to create SSL connection"));
            return;
        }

        CMD_CONN_NAME(conn->handle, conn);
        tws_RegisterConnName(conn->handle, conn);
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
        tws_ThreadQueueConnEvent(conn);
#else
        if (tws_AddConnToThreadList(conn)) {
            tws_QueueProcessEvent(conn);
        }
#endif

}

Tcl_ThreadCreateType tws_HandleConnThread(ClientData clientData) {

    tws_thread_ctrl_t *ctrl = (tws_thread_ctrl_t *) clientData;

    // Get a pointer to the thread data for the current thread
    tws_thread_data_t *dataPtr = (tws_thread_data_t *) Tcl_GetThreadData(tws_GetThreadDataKey(), sizeof(tws_thread_data_t));
    // Create a new interp for this thread and store it in the thread data
    dataPtr->interp = Tcl_CreateInterp();
    dataPtr->cmdPtr = Tcl_DuplicateObj(ctrl->server->cmdPtr);
    dataPtr->server = ctrl->server;
    dataPtr->thread_index = ctrl->thread_index;
    dataPtr->terminate = 0;
    dataPtr->num_requests = 0;
    dataPtr->thread_pivot = dataPtr->thread_index * (ctrl->server->garbage_collection_cleanup_threshold / ctrl->server->num_threads);
    dataPtr->num_conns = 0;
    dataPtr->firstConnPtr = NULL;
    dataPtr->lastConnPtr = NULL;
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    dataPtr->epoll_fd = kqueue();
#else
    dataPtr->epoll_fd = epoll_create1(0);
#endif
    tws_SetBlockingMode(dataPtr->epoll_fd, TWS_MODE_NONBLOCKING);

    Tcl_IncrRefCount(dataPtr->cmdPtr);

    DBG(fprintf(stderr, "created interp=%p\n", dataPtr->interp));

    Tcl_InitMemory(dataPtr->interp);
    if (TCL_OK != Tcl_Init(dataPtr->interp)) {
        DBG(fprintf(stderr, "error initializing Tcl\n"));
        Tcl_FinalizeThread();
        Tcl_ExitThread(TCL_ERROR);
        TCL_THREAD_CREATE_RETURN;
    }

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#else
    int server_fd;
    int epoll_fd;
    if (TCL_OK != create_socket(dataPtr->interp, ctrl->server, ctrl->host, ctrl->port, &server_fd) || server_fd < 0) {
        fprintf(stderr, "failed to create socket on thread\n");
        Tcl_FinalizeThread();
        Tcl_ExitThread(TCL_ERROR);
        TCL_THREAD_CREATE_RETURN;
    }

    if (TCL_OK != create_epoll_fd(dataPtr->interp, server_fd, &epoll_fd) || epoll_fd < 0) {
        fprintf(stderr, "failed to create epoll fd on thread\n");
        Tcl_FinalizeThread();
        Tcl_ExitThread(TCL_ERROR);
        TCL_THREAD_CREATE_RETURN;
    }

    DBG(fprintf(stderr, "port: %s - created listening socket on thread: %d\n", ctrl->port, ctrl->thread_index));
#endif

    tws_accept_ctx_t *accept_ctx = (tws_accept_ctx_t *) Tcl_Alloc(sizeof(tws_accept_ctx_t));

    if (ctrl->option_http) {
        accept_ctx->read_fn = tws_ReadHttpConnAsync;
        accept_ctx->write_fn = tws_WriteHttpConnAsync;
        accept_ctx->handle_conn_fn = tws_HandleRecv;
        accept_ctx->ssl_ctx = NULL;
    } else {

        accept_ctx->read_fn = tws_ReadSslConnAsync;
        accept_ctx->write_fn = tws_WriteSslConnAsync;
        accept_ctx->handle_conn_fn = tws_HandleSslHandshake;

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
        accept_ctx->ssl_ctx = NULL;
#else
        // it is an https server, so we need to create an SSL_CTX
        if (TCL_OK != tws_CreateSslContext(dataPtr->interp, &accept_ctx->ssl_ctx)) {
            Tcl_Free((char *) accept_ctx);
            Tcl_FinalizeThread();
            Tcl_ExitThread(TCL_ERROR);
            TCL_THREAD_CREATE_RETURN;
        }
        SSL_CTX_set_client_hello_cb(accept_ctx->ssl_ctx, tws_ClientHelloCallback, NULL);
#endif
    }

    accept_ctx->option_http = ctrl->option_http;
    accept_ctx->port = atoi(ctrl->port);
    accept_ctx->interp = dataPtr->interp;
    accept_ctx->server = ctrl->server;
// todo:    accept_ctx->num_threads = ctrl->option_num_threads;

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#else
    accept_ctx->server_fd = server_fd;
    dataPtr->server_fd = server_fd;
    Tcl_CreateFileHandler(server_fd, TCL_READABLE, tws_AcceptConn, accept_ctx);

#endif

    // create a file handler for the epoll fd for this thread
    Tcl_CreateFileHandler(dataPtr->epoll_fd, TCL_READABLE, tws_KeepaliveConnHandler, NULL);

    if (TCL_OK != Tcl_EvalObj(dataPtr->interp, ctrl->server->scriptPtr)) {
        fprintf(stderr, "error evaluating init script\n");
        fprintf(stderr, "error=%s\n", Tcl_GetString(Tcl_GetObjResult(dataPtr->interp)));

        Tcl_Obj *return_options_dict_ptr = Tcl_GetReturnOptions(dataPtr->interp, TCL_ERROR);
        Tcl_IncrRefCount(return_options_dict_ptr);
        Tcl_Obj *errorinfo_ptr;
        Tcl_Obj *errorinfo_key_ptr = Tcl_NewStringObj("-errorinfo", -1);
        Tcl_IncrRefCount(errorinfo_key_ptr);
        if (TCL_OK != Tcl_DictObjGet(dataPtr->interp, return_options_dict_ptr, errorinfo_key_ptr,
                                     &errorinfo_ptr)) {
            Tcl_DecrRefCount(errorinfo_key_ptr);
            Tcl_DecrRefCount(return_options_dict_ptr);
            Tcl_FinalizeThread();
            Tcl_ExitThread(TCL_ERROR);
            TCL_THREAD_CREATE_RETURN;
        }
        Tcl_DecrRefCount(errorinfo_key_ptr);
        fprintf(stderr, "HandleConnThread: errorInfo: %s\n", Tcl_GetString(errorinfo_ptr));
        Tcl_DecrRefCount(return_options_dict_ptr);

        Tcl_FinalizeThread();
        Tcl_ExitThread(TCL_ERROR);
        TCL_THREAD_CREATE_RETURN;
    }

    // notify the main thread that we are done initializing
    Tcl_ConditionNotify(ctrl->cond_wait_ptr);

    DBG(fprintf(stderr, "HandleConnThread: in (%p)\n", Tcl_GetCurrentThread()));
    do {
        Tcl_DoOneEvent(TCL_ALL_EVENTS);
        if (dataPtr->terminate && dataPtr->num_conns) {
            fprintf(stderr, "Draining connections - thread: %p num_conns: %d conn_timeout_millis: %d\n", Tcl_GetCurrentThread(), dataPtr->num_conns, accept_ctx->server->conn_timeout_millis);
            Tcl_Time block_time = {0, 10000};
            while (dataPtr->num_conns) {
                Tcl_DoOneEvent(TCL_DONT_WAIT);
                Tcl_WaitForEvent(&block_time);
                tws_CleanupConnections();
            }
        }
    } while (!dataPtr->terminate);

    // we did not close this in HandleTermEventInThread
    // because we wanted to drain keepalive connections
    close(dataPtr->epoll_fd);

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#else
    if (accept_ctx->ssl_ctx) {
        SSL_CTX_free(accept_ctx->ssl_ctx);
    }
#endif
    tws_DecrRefCountUntilZero(dataPtr->cmdPtr);
    Tcl_DeleteInterp(dataPtr->interp);
    Tcl_Free(accept_ctx);

    DBG(fprintf(stderr, "HandleConnThread: out (%p)\n", Tcl_GetCurrentThread()));

    Tcl_FinalizeThread();
    Tcl_ExitThread(TCL_OK);
    TCL_THREAD_CREATE_RETURN;
}

static void tws_KeepaliveConnHandler(void *data, int mask) {

        DBG(fprintf(stderr, "KeepaliveConnHandler\n"));


    tws_thread_data_t *dataPtr = (tws_thread_data_t *) Tcl_GetThreadData(tws_GetThreadDataKey(), sizeof(tws_thread_data_t));

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    struct timespec timeout;
    timeout.tv_sec = 0;  // 0 seconds
    timeout.tv_nsec = 1000; // 1000 nanoseconds

    struct kevent events[MAX_EVENTS];
    int nfds = kevent(dataPtr->epoll_fd, NULL, 0, events, MAX_EVENTS, &timeout);
    if (nfds == -1) {
        fprintf(stderr, "KeepaliveConnHandler: kevent failed");
        return;
    }
#else
    struct epoll_event events[MAX_EVENTS];
    int nfds = epoll_wait(dataPtr->epoll_fd, events, MAX_EVENTS, 0);
    if (nfds == -1) {
        fprintf(stderr, "KeepaliveConnHandler: epoll_wait failed");
        return;
    }
#endif

    DBG(fprintf(stderr, "KeepaliveConnHandler - nfds: %d\n", nfds));

    for (int i = 0; i < nfds; i++) {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
        tws_conn_t *conn = (tws_conn_t *) events[i].udata;

        if (!conn->handle_conn_fn) {
            conn->handle_conn_fn = tws_HandleRecv;
        }

        tws_ThreadQueueKeepaliveEvent(conn);
#else
        tws_conn_t *conn = (tws_conn_t *) events[i].data.ptr;
        DBG(fprintf(stderr, "KeepaliveConnHandler - keepalive client: %d %s\n", conn->client, conn->handle));
        conn->start_read_millis = current_time_in_millis();
        conn->latest_millis = conn->start_read_millis;

        if (!conn->handle_conn_fn) {
            conn->handle_conn_fn = tws_HandleRecv;
        }

        tws_QueueProcessEvent(conn);
#endif
    }

}

static void tws_AddListenerToServer(tws_server_t *server, tws_listener_t *listener) {
    if (server->first_listener_ptr == NULL) {
        server->first_listener_ptr = listener;
    } else {
        listener->nextPtr = server->first_listener_ptr;
        server->first_listener_ptr = listener;
    }
}

int tws_Listen(Tcl_Interp *interp, tws_server_t *server, int option_http, int option_num_threads, const char *host, const char *port) {

    tws_listener_t *listener = (tws_listener_t *) Tcl_Alloc(sizeof(tws_listener_t));
    listener->port = atoi(port);
    listener->option_http = option_http;
    listener->option_num_threads = option_num_threads;
    listener->conn_thread_ids = (Tcl_ThreadId *) Tcl_Alloc(option_num_threads * sizeof(Tcl_ThreadId));
    listener->nextPtr = NULL;
    listener->cond_wait = NULL;

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    int server_fd;
    if (TCL_OK != create_socket(interp, server, host, port, &server_fd) || server_fd < 0) {
        fprintf(stderr, "failed to create socket on main thread\n");
        SetResult("Failed to create server socket");
        return TCL_ERROR;
    }

    int epoll_fd;
    if (TCL_OK != create_epoll_fd(interp, server_fd, &epoll_fd) || epoll_fd < 0) {
        fprintf(stderr, "failed to create epoll fd on main thread\n");
        SetResult("Failed to create epoll fd");
        return TCL_ERROR;
    }

    tws_accept_ctx_t *accept_ctx = (tws_accept_ctx_t *) Tcl_Alloc(sizeof(tws_accept_ctx_t));

    if (option_http) {
        accept_ctx->read_fn = tws_ReadHttpConnAsync;
        accept_ctx->write_fn = tws_WriteHttpConnAsync;
        accept_ctx->handle_conn_fn = tws_HandleRecv;
    } else {
        accept_ctx->read_fn = tws_ReadSslConnAsync;
        accept_ctx->write_fn = tws_WriteSslConnAsync;
        accept_ctx->handle_conn_fn = tws_HandleSslHandshake;

        if (TCL_OK != tws_CreateSslContext(interp, &accept_ctx->ssl_ctx)) {
            Tcl_Free((char *) accept_ctx);
            SetResult("Failed to create SSL context");
            return TCL_ERROR;
        }
        SSL_CTX_set_client_hello_cb(accept_ctx->ssl_ctx, tws_ClientHelloCallback, NULL);

    }

    accept_ctx->option_http = option_http;
    accept_ctx->port = atoi(port);
    accept_ctx->interp = interp;
    accept_ctx->server = server;
    accept_ctx->num_threads = option_num_threads;

    accept_ctx->server_fd = server_fd;
    accept_ctx->epoll_fd = epoll_fd;

    Tcl_CreateFileHandler(server_fd, TCL_READABLE, tws_AcceptConn, accept_ctx);

    DBG(fprintf(stderr, "port: %s - created listening socket (%d) on main thread\n", port, server_fd));

    accept_ctx->conn_thread_ids = (Tcl_ThreadId *) Tcl_Alloc(option_num_threads * sizeof(Tcl_ThreadId));
    listener->server_fd = server_fd;
#else
#endif

    for (int i = 0; i < option_num_threads; i++) {
        Tcl_MutexLock(tws_GetThreadMutex());
        Tcl_ThreadId id;
        tws_thread_ctrl_t ctrl;
        ctrl.cond_wait_ptr = &listener->cond_wait;
        ctrl.server = server;
        ctrl.thread_index = i;
        ctrl.host = host;
        ctrl.port = port;
        ctrl.option_http = option_http;
        if (TCL_OK !=
            Tcl_CreateThread(&id, tws_HandleConnThread, &ctrl, server->thread_stacksize, TCL_THREAD_JOINABLE)) {
            Tcl_MutexUnlock(tws_GetThreadMutex());
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
            Tcl_Free((char *) accept_ctx);
#endif
            SetResult("Unable to create thread");
            return TCL_ERROR;
        }
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
        DBG(fprintf(stderr, "Listen - created thread: %p\n", id));
        accept_ctx->conn_thread_ids[i] = id;
        DBG(fprintf(stderr, "Listen - created thread: %p (check)\n", accept_ctx->conn_thread_ids[i]));
#endif

        listener->conn_thread_ids[i] = id;

        // Wait for the thread to start because it is using something on our stack!
        Tcl_ConditionWait(&listener->cond_wait, tws_GetThreadMutex(), NULL);
        Tcl_MutexUnlock(tws_GetThreadMutex());
        Tcl_ConditionFinalize(&listener->cond_wait);
        DBG(fprintf(stderr, "Listen - created thread: %p\n", id));
    }

    tws_AddListenerToServer(server, listener);

    return TCL_OK;
}
