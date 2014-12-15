#define _GNU_SOURCE         /* See feature_test_macros(7) */
#include <fcntl.h>

#include "common.h"
#include "server_ctx.h"

#define MAX_SPLICE_AT_ONCE  (1<<30)

/******************************************************************
 * functions for accepting TCP connections (i.e. server routines) *
 ******************************************************************/
inline static
void accept_cb(struct ev_loop* loop, ev_io* w, int revents)
{
    server_ctx_t* sctx = (server_ctx_t*) w->data;
    client_ctx_t* cctx = get_client_ctx(sctx);

    socket_t* sock = &cctx->downstream.sock;
    sock->addrlen = sizeof(sock->addr);

    int fd = accept(w->fd, (struct sockaddr*) &sock->addr, &sock->addrlen);

    if (fd >= 0) {
        humanize_socket(sock);
        if (init_client_ctx(sctx, cctx, fd))
            goto accept_cb_error;

        mark_client_ctx_as_used(sctx, cctx);
        INFO("accepted connection from %s", sock->to_string);
    } else if (errno != EINTR && errno != EAGAIN) {
        ERRP("accept() returned error");
        goto accept_cb_error;
    }

    return;

accept_cb_error:
    ev_io_stop(loop, w);
    close(w->fd);
    w->fd = -1;
}

inline static
void stop_loop_cb(struct ev_loop* loop, ev_async* w, int revents) {
    _D("Async signal received in server context. Break evloop");
    ev_break(loop, EVBREAK_ALL);
}

int init_server_ctx(server_ctx_t* sctx, const socket_t* ssock, const socket_t* usock)
{
    assert(sctx);
    assert(ssock);

    sctx->loop = NULL;
    sctx->free_pool = NULL;
    sctx->used_pool = NULL;
    sctx->ssock = ssock;
    sctx->usock = usock;
    sctx->io.data = sctx;
    sctx->io.fd = -1;

    int fd = setup_socket(ssock, NET_SERVER_SOCKET);
    if (fd < 0) goto error;

    sctx->loop = ev_loop_new(EVFLAG_NOSIGMASK); // libev doesn't touch sigmask
    if (!sctx->loop) goto error;

    // !!!!!!!!!!!!!!!!!!!!!!!!!
    // no error below this point
    // otherwise free_server_ctx() will do double close() of fd
    // !!!!!!!!!!!!!!!!!!!!!!!!!

    ev_set_userdata(sctx->loop, sctx);
    ev_io_init(&sctx->io, accept_cb, fd, EV_READ);
    ev_io_start(sctx->loop, &sctx->io);

    ev_async_init(&sctx->stop_loop, stop_loop_cb);
    ev_async_start(sctx->loop, &sctx->stop_loop);

    // TODO allocate free pool
    return 0;

error:
    if (fd >= 0) close(fd);
    free_server_ctx(sctx);
    return -1;
}

void terminate_server_ctx(server_ctx_t* sctx)
{
    assert(sctx);
    ev_async_send(sctx->loop, &sctx->stop_loop);
}

void free_server_ctx(server_ctx_t* sctx)
{
    if (!sctx) return;

    if (sctx->io.fd >= 0) {
        close(sctx->io.fd);
        sctx->io.fd = -1;
    }

    if (sctx->loop) {
        ev_loop_destroy(sctx->loop);
        sctx->loop = NULL;
    }

    // TODO free pool
}

/******************************************************************
 * communication function (i.e. client routines)                  *
 ******************************************************************/

inline static
void _reset_events_mask(struct ev_loop* loop, ev_io* io, int events)
{
    assert(io);
    _D("set new events mask %d for %d", events, io->fd);

    if (events == 0) {
        ev_io_stop(loop, io);
    } else if (!ev_is_active(io)) {
        ev_io_set(io, io->fd, events);
        ev_io_start(loop, io);
    } else if (io->events != events) {
        ev_io_stop(loop, io);
        ev_io_set(io, io->fd, events);
        ev_io_start(loop, io);
    }
}

inline static
void connect_cb(struct ev_loop* loop, ev_io* w, int revents)
{
    server_ctx_t* sctx = (server_ctx_t*) ev_userdata(loop);
    client_ctx_t* cctx = (client_ctx_t*) w->data;

    errno = 0;
    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(w->fd, SOL_SOCKET, SO_ERROR, &err, &len) || err) {
        _D("getsockopt() tells that connect() failed: %s", strerror(errno | err));
        goto connect_cb_error;
    }

    INFO("connected to %s", sctx->usock->to_string);

    ev_io_stop(loop, w);
    ev_io_start(loop, &cctx->upstream.io);
    ev_io_start(loop, &cctx->downstream.io);
    return;

connect_cb_error:
    _D("connect_cb_error");
    deinit_client_ctx(sctx, cctx);
    mark_client_ctx_as_free(sctx, cctx);
}

inline static
void upstream_cb(struct ev_loop* loop, ev_io* w, int revents)
{
    int events = w->events;
    server_ctx_t* sctx = (server_ctx_t*) ev_userdata(loop);
    client_ctx_t* cctx = (client_ctx_t*) w->data;

    if (revents & EV_WRITE) {
        // (downstream ->) pipe -> upstream
        while (cctx->downstream.size) {
            ssize_t ret = splice(cctx->downstream.pipefd[0], NULL,
                                 w->fd, NULL,
                                 cctx->downstream.size, SPLICE_F_MOVE | SPLICE_F_NONBLOCK);

            if (ret > 0) {
                cctx->downstream.size -= ret;

                // there is free space in pipe's buffer
                // activate downstream read communication which fills it
                ev_io* downstream_io = &cctx->downstream.io;
                _reset_events_mask(loop, downstream_io, downstream_io->events | EV_READ);
            } else {
                if (ret == 0 || errno == EAGAIN) {
                    events &= ~EV_WRITE;
                    break;
                } else if (errno == EINTR) {
                    continue;
                } else {
                    ERRP("splice failed when writting to %s", sctx->usock->to_string);
                    goto upstream_cb_error;
                }
            }
        }

        if (!cctx->downstream.size) {
            // there is no data in pipe,
            // so nothing to write in the socket
            events &= ~EV_WRITE;
        }
    }

    if (revents & EV_READ) {
        // upstream -> pipe (-> downstream)
        ssize_t ret = splice(w->fd, NULL,
                             cctx->upstream.pipefd[1], NULL,
                             MAX_SPLICE_AT_ONCE, SPLICE_F_MOVE | SPLICE_F_NONBLOCK);

        if (ret > 0) {
            cctx->upstream.size += ret;

            // there is new data in pipe
            // activate downstream write communication which reads data from pipe
            ev_io* downstream_io = &cctx->downstream.io;
            _reset_events_mask(loop, downstream_io, downstream_io->events | EV_WRITE);
        } else {
            /*
             * ret == 0 - upstream closed connection
             * EINTR - ignore
             * EAGAIN has two meanings:
             * - socket buffer is empty
             * - pipe is full
             * as result of both cases we need to disable EV_READ events
             */

            if (ret == 0) {
                // connection closed
                goto upstream_cb_error;
            }

            if (errno == EAGAIN) {
                events &= ~EV_READ;
            } else if (errno == EINTR) {
                // noop
            } else {
                ERRP("splice failed when reading from %s", sctx->usock->to_string);
                goto upstream_cb_error;
            }
        }
    }

    _reset_events_mask(loop, w, events);
    return;

upstream_cb_error:
    _D("upstream_cb_error");
    deinit_client_ctx(sctx, cctx);
    mark_client_ctx_as_free(sctx, cctx);
}

inline static
void downstream_cb(struct ev_loop* loop, ev_io* w, int revents)
{
    int events = w->events;
    server_ctx_t* sctx = (server_ctx_t*) ev_userdata(loop);
    client_ctx_t* cctx = (client_ctx_t*) w->data;

    if (revents & EV_WRITE) {
        // (upstream ->) pipe -> downstream
        while (cctx->upstream.size) {
            ssize_t ret = splice(cctx->upstream.pipefd[0], NULL,
                                 w->fd, NULL,
                                 cctx->upstream.size, SPLICE_F_MOVE | SPLICE_F_NONBLOCK);

            if (ret > 0) {
                cctx->upstream.size -= ret;

                // there is free space in pipe's buffer
                // activate upstream read communication which fills it
                ev_io* upstream_io = &cctx->upstream.io;
                _reset_events_mask(loop, upstream_io, upstream_io->events | EV_READ);
            } else {
                if (ret == 0 || errno == EAGAIN) {
                    events &= ~EV_WRITE;
                    break;
                } else if (errno == EINTR) {
                    continue;
                } else {
                    ERRP("splice failed when writting to %s", cctx->downstream.sock.to_string);
                    goto downstream_cb_error;
                }
            }
        }

        if (!cctx->upstream.size) {
            // there is no data in pipe,
            // so nothing to write in the socket
            events &= ~EV_WRITE;
        }
    }

    if (revents & EV_READ) {
        // downstream -> pipe (-> upstream)
        ssize_t ret = splice(w->fd, NULL,
                             cctx->downstream.pipefd[1], NULL,
                             MAX_SPLICE_AT_ONCE, SPLICE_F_MOVE | SPLICE_F_NONBLOCK);

        if (ret > 0) {
            cctx->downstream.size += ret;

            // there is new data in pipe
            // activate upstream write communication which reads data from pipe
            ev_io* upstream_io = &cctx->upstream.io;
            _reset_events_mask(loop, upstream_io, upstream_io->events | EV_WRITE);
        } else {
            /*
             * ret == 0 - upstream closed connection
             * EINTR - ignore
             * EAGAIN has two meanings:
             * - socket buffer is empty
             * - pipe is full
             * as result of both cases we need to disable EV_READ events
             */

            if (ret == 0) {
                // connection close TODO
                goto downstream_cb_error;
            }

            if (errno == EAGAIN) {
                events &= ~EV_READ;
            } else if (errno == EINTR) {
                // noop
            } else {
                ERRP("splice failed when reading from %s", cctx->downstream.sock.to_string);
                goto downstream_cb_error;
            }
        }
    }

    _reset_events_mask(loop, w, events);
    return;

downstream_cb_error:
    _D("downstream_cb_error");
    deinit_client_ctx(sctx, cctx);
    mark_client_ctx_as_free(sctx, cctx);
}

int init_client_ctx(server_ctx_t* sctx, client_ctx_t* cctx, int fd)
{
    assert(cctx);

    int client_fd = setup_socket(sctx->usock, 0);
    if (client_fd < 0) return -1;

    int connected = connect_client_socket(sctx->usock, client_fd);
    if (connected == -1) goto init_client_ctx_error;

    cctx->upstream.size = 0;
    cctx->upstream.io.fd = -1;
    cctx->upstream.io.data = cctx;
    cctx->upstream.pipefd[0] = -1;
    cctx->upstream.pipefd[1] = -1;

    if (pipe(cctx->upstream.pipefd)) {
        ERRP("Failed to create pipe");
        goto init_client_ctx_error;
    }

    cctx->downstream.size = 0;
    cctx->downstream.io.fd = -1;
    cctx->downstream.io.data = cctx;
    cctx->downstream.pipefd[0] = -1;
    cctx->downstream.pipefd[1] = -1;

    if (pipe(cctx->downstream.pipefd)) {
        ERRP("Failed to create pipe");
        goto init_client_ctx_error;
    }

    cctx->connect.io.fd = -1;
    cctx->connect.io.data = cctx;

    // those are started by connect_cb once connected to upstream
    ev_io_init(&cctx->upstream.io, upstream_cb, client_fd, EV_READ | EV_WRITE);
    ev_io_init(&cctx->downstream.io, downstream_cb, fd, EV_READ | EV_WRITE);

    ev_io_init(&cctx->connect.io, connect_cb, client_fd, EV_WRITE);
    ev_io_start(sctx->loop, &cctx->connect.io);

    return 0;

init_client_ctx_error:
    close(client_fd);

    if (cctx->upstream.pipefd[0] >= 0) close(cctx->upstream.pipefd[0]);
    if (cctx->upstream.pipefd[1] >= 0) close(cctx->upstream.pipefd[1]);

    if (cctx->downstream.pipefd[0] >= 0) close(cctx->downstream.pipefd[0]);
    if (cctx->downstream.pipefd[1] >= 0) close(cctx->downstream.pipefd[1]);
    return -1;
}

void deinit_client_ctx(server_ctx_t* sctx, client_ctx_t* cctx)
{
    assert(sctx);
    if (!cctx) return;

    if (cctx->upstream.io.fd >= 0) {
        ev_io_stop(sctx->loop, &cctx->upstream.io);
        close(cctx->upstream.io.fd);
        cctx->upstream.io.fd = -1;
    }

    if (cctx->downstream.io.fd >= 0) {
        ev_io_stop(sctx->loop, &cctx->downstream.io);
        close(cctx->downstream.io.fd);
        cctx->downstream.io.fd = -1;
    }

    if (cctx->upstream.pipefd[0] >= 0) {
        close(cctx->upstream.pipefd[0]);
        cctx->upstream.pipefd[0] = -1;
    }

    if (cctx->upstream.pipefd[1] >= 0) {
        close(cctx->upstream.pipefd[1]);
        cctx->upstream.pipefd[1] = -1;
    }

    if (cctx->downstream.pipefd[0] >= 0) {
        close(cctx->downstream.pipefd[0]);
        cctx->downstream.pipefd[0] = -1;
    }

    if (cctx->downstream.pipefd[1] >= 0) {
        close(cctx->downstream.pipefd[1]);
        cctx->downstream.pipefd[1] = -1;
    }
}

client_ctx_t* get_client_ctx(server_ctx_t* sctx)
{
    assert(sctx);

    client_ctx_t* cctx = sctx->free_pool;
    if (cctx) return cctx;

    cctx = malloc_or_die(sizeof(client_ctx_t));
    cctx->next = NULL;
    sctx->free_pool = cctx;
    return cctx;
}

void mark_client_ctx_as_used(server_ctx_t* sctx, client_ctx_t* cctx)
{
    assert(sctx);
    assert(cctx);
    assert(sctx->free_pool == cctx);

    sctx->free_pool = cctx->next;
    cctx->next = sctx->used_pool;
    sctx->used_pool = cctx;
}

void mark_client_ctx_as_free(server_ctx_t* sctx, client_ctx_t* cctx)
{
    assert(sctx);
    assert(cctx);
    // TODO
}

