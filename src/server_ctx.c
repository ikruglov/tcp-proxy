#define _GNU_SOURCE         /* See feature_test_macros(7) */
#include <fcntl.h>

#include "common.h"
#include "config.h"
#include "server_ctx.h"

#define MAX_SPLICE_AT_ONCE  (1<<30)

inline static void accept_cb(struct ev_loop* loop, ev_io* w, int revents);
inline static void stop_loop_cb(struct ev_loop* loop, ev_async* w, int revents);

inline static void connect_cb(struct ev_loop* loop, ev_io* w, int revents);
inline static void upstream_cb(struct ev_loop* loop, ev_io* w, int revents);
inline static void downstream_cb(struct ev_loop* loop, ev_io* w, int revents);

inline static int grow_pool(server_ctx_t* sctx, size_t size);
inline static client_ctx_t* _get_client_ctx(server_ctx_t* sctx);
inline static void _mark_client_ctx_as_used(server_ctx_t* sctx, client_ctx_t* cctx);
inline static void _mark_client_ctx_as_free(server_ctx_t* sctx, client_ctx_t* cctx);
inline static void _reset_events_mask(struct ev_loop* loop, ev_io* io, int events);

/******************************************************************
 * functions for accepting TCP connections (i.e. server routines) *
 ******************************************************************/

inline static
void accept_cb(struct ev_loop* loop, ev_io* w, int revents)
{
    int fd = -1;
    server_ctx_t* sctx = (server_ctx_t*) w->data;
    client_ctx_t* cctx = _get_client_ctx(sctx);

    if (!cctx) {
        INFO("limit of max connections reached");
        goto temp_error;
    }

    socket_t* sock = &cctx->downstream.sock;
    sock->addrlen = sizeof(sock->addr);
    fd = accept(w->fd, (struct sockaddr*) &sock->addr, &sock->addrlen);

    if (fd >= 0) {
        humanize_socket(sock);
        if (init_client_ctx(sctx, cctx, fd))
            goto error;

        _mark_client_ctx_as_used(sctx, cctx);
        _D("assigned idx %d to client_ctx_t for %s", cctx->idx, sock->to_string);

        INFO("accepted connection from %s", sock->to_string);
    } else {
        switch (errno) {
            case EINTR:
            case EAGAIN:
            case ECONNABORTED:
                break; // noop

            case ENFILE:
            case EMFILE:
            case ENOBUFS:
            case ENOMEM:
                // we have problems with various resources
                ERRP("accept() returned error reflecting exhasting of resource");
                goto temp_error;

            case EPROTO:
                ERRP("accept() returned non-critical error");
                goto temp_error;

            default:
                ERRP("accept() returned critical error");
                goto error;
        }
    }

    return;

temp_error:
    /* TODO
     * creating a busy loop,
     * better would be to pause watcher for a while */
    return;

error:
    if (fd >= 0) close(fd);
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
    sctx->ssock = ssock;
    sctx->usock = usock;
    sctx->stack = NULL;
    sctx->pool = NULL;
    sctx->io.data = sctx;
    sctx->io.fd = -1;

    int fd = setup_socket(ssock, NET_SERVER_SOCKET);
    if (fd < 0) goto error;

    sctx->loop = ev_loop_new(EVFLAG_NOSIGMASK); // libev doesn't touch sigmask
    if (!sctx->loop) goto error;

    if (grow_pool(sctx, gl_settings.minconn))
        goto error;


    // !!!!!!!!!!!!!!!!!!!!!!!!!
    // no error below this point
    // otherwise free_server_ctx() will do double close() of fd
    // !!!!!!!!!!!!!!!!!!!!!!!!!

    ev_set_userdata(sctx->loop, sctx);
    ev_io_init(&sctx->io, accept_cb, fd, EV_READ);
    ev_io_start(sctx->loop, &sctx->io);

    ev_async_init(&sctx->stop_loop, stop_loop_cb);
    ev_async_start(sctx->loop, &sctx->stop_loop);

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

    if (sctx->pool) {
        free(sctx->pool);
        sctx->pool = NULL;
    }

    if (sctx->stack) {
        stack_free(sctx->stack);
        sctx->stack = NULL;
    }
}

/******************************************************************
 * communication function (i.e. client routines)                  *
 ******************************************************************/

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

    // we have connected to upstream,
    // so stop connect_cb()
    ev_io_stop(loop, w);

    // reassign and start upstream_cb()
    ev_io_set(w, w->fd, EV_READ | EV_WRITE);
    ev_set_cb(w, upstream_cb);
    ev_io_start(loop, w);

    // start downstream_cb()
    ev_io_start(loop, &cctx->downstream.io);
    return;

connect_cb_error:
    _D("connect_cb_error");
    deinit_client_ctx(sctx, cctx);
    _mark_client_ctx_as_free(sctx, cctx);
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
    _mark_client_ctx_as_free(sctx, cctx);
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
    _mark_client_ctx_as_free(sctx, cctx);
}

// init_client_ctx() does not close fd if failed
int init_client_ctx(server_ctx_t* sctx, client_ctx_t* cctx, int fd)
{
    assert(cctx);

    cctx->upstream.size = 0;
    cctx->upstream.io.fd = -1;
    cctx->upstream.io.data = cctx;
    cctx->upstream.pipefd[0] = -1;
    cctx->upstream.pipefd[1] = -1;

    int client_fd = setup_socket(sctx->usock, 0);
    if (client_fd < 0) goto error;

    int connected = connect_client_socket(sctx->usock, client_fd);
    if (connected == -1) goto error;

    if (pipe(cctx->upstream.pipefd)) {
        ERRP("Failed to create pipe");
        goto error;
    }

    cctx->downstream.size = 0;
    cctx->downstream.io.fd = -1;
    cctx->downstream.io.data = cctx;
    cctx->downstream.pipefd[0] = -1;
    cctx->downstream.pipefd[1] = -1;

    if (pipe(cctx->downstream.pipefd)) {
        ERRP("Failed to create pipe");
        goto error;
    }

#ifdef F_SETPIPE_SZ
    if (gl_settings.pipe_size) {
        _D("Try to set pipe capacity to %zd", gl_settings.pipe_size);
        fcntl(cctx->upstream.pipefd[0], F_SETPIPE_SZ, gl_settings.pipe_size);
        fcntl(cctx->downstream.pipefd[0], F_SETPIPE_SZ, gl_settings.pipe_size);
    }
#endif

    // !!!!!!!!!!!!!!!!!!!!!!!!!
    // no error below this point
    // !!!!!!!!!!!!!!!!!!!!!!!!!

    ev_io_init(&cctx->upstream.io, connect_cb, client_fd, EV_WRITE);
    ev_io_init(&cctx->downstream.io, downstream_cb, fd, EV_READ | EV_WRITE);
    ev_io_start(sctx->loop, &cctx->upstream.io);
    return 0;

error:
    if (client_fd >= 0) close(client_fd);
    deinit_client_ctx(sctx, cctx);
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


/******************************************************************
 * helper functions                                               *
 ******************************************************************/

inline static
void _reset_events_mask(struct ev_loop* loop, ev_io* io, int events)
{
    assert(io);
    //_D("set new events mask %d for %d", events, io->fd);

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
int grow_pool(server_ctx_t* sctx, size_t size)
{
    assert(sctx);

    /* grow pool has O(n) complexety.
     * But good news is that pushing to
     * stack is very-very cache friendly */
    _D("grow_pool to size %zd", size);

    size_t old_size = 0;
    int_stack_t* stack = NULL;
    client_ctx_t* pool = NULL;

    pool = realloc(sctx->pool, size);
    if (!pool) {
        ERR("Failed to allocate pool");
        goto error;
    }

    if (sctx->stack) {
        old_size = sctx->stack->size;
        stack = stack_grow(sctx->stack, size);
    } else {
        stack = stack_init(size);
    }

    if (!stack) {
        ERR("Failed to grow stack");
        goto error;
    }

    _D("fill stack with items from %zd to %zd", size - 1, old_size);
    for (int i = size - 1; i >= (int) old_size; --i) {
        stack_push(stack, i);
    }

    sctx->pool = pool;
    sctx->stack = stack;
    return 0;

error:
    free(pool);
    free(stack);
    return -1;
}

inline static
client_ctx_t* _get_client_ctx(server_ctx_t* sctx)
{
    /* _get_client_ctx() has ammortized O(1) complexity
     * i.e. we need to scan N items no often then N calls of _get_client_ctx() */

    assert(sctx);
    assert(sctx->stack);

    int idx = stack_peek(sctx->stack);
    if (idx < 0 && sctx->stack->size < gl_settings.maxconn) {
        grow_pool(sctx, sctx->stack->size * 2 + 1); // + 1 to hanle size == 0
        idx = stack_peek(sctx->stack);
    }

    assert(idx < sctx->stack->size);
    return idx >= 0 ? &sctx->pool[idx] : NULL;
}

inline static
void _mark_client_ctx_as_used(server_ctx_t* sctx, client_ctx_t* cctx)
{
    assert(sctx);
    assert(cctx);
    assert(sctx->stack);
    assert(!stack_empty(sctx->stack));

    cctx->idx = stack_pop(sctx->stack);
    assert(cctx->idx >= 0);
    assert(cctx->idx < sctx->stack->size);
}

inline static
void _mark_client_ctx_as_free(server_ctx_t* sctx, client_ctx_t* cctx)
{
    assert(sctx);
    assert(cctx);
    assert(sctx->stack);
    assert(cctx->idx >= 0);
    assert(!stack_full(sctx->stack));

    stack_push(sctx->stack, cctx->idx);
    // TODO shrink pool
}

