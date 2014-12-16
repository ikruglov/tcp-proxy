#ifndef __SERVER_CTX_H__
#define __SERVER_CTX_H__

#include "net.h"
#include "stack.h"
#include "libev/ev.h"

typedef void (io_watcher_cb)(struct ev_loop* loop, ev_io *w, int revents);

typedef struct _client_ctx {
    struct upstream {
        ev_io io;
        int pipefd[2];                  // upstream -> pipe -> downstream
        size_t size;                    // amount of data kept in pipe's buffer
    } upstream;

    struct downstream {
        ev_io io;
        int pipefd[2];                  // downstream -> pipe -> upstream
        size_t size;                    // amount of data kept in pipe's buffer
        socket_t sock;
    } downstream;

    unsigned int idx;
} client_ctx_t;

typedef struct {
    ev_io io;                           // watcher, used only to accept() connections
    ev_async stop_loop;                 // signal to interrupt loop
    struct ev_loop *loop;               // thread EV loop

    const socket_t* ssock;              // server socket_t (shared between threads)
    const socket_t* usock;              // upstream socket_t (shared between threads)

    client_ctx_t* pool;                 // preallocated pool of client_ctx_t objects
    int_stack_t* stack;                 // stack of free indexes in pool
} server_ctx_t;

int init_server_ctx(server_ctx_t* sctx, const socket_t* ssock, const socket_t* usock);
void terminate_server_ctx(server_ctx_t* sctx);
void free_server_ctx(server_ctx_t* sctx);

int init_client_ctx(server_ctx_t* sctx, client_ctx_t* cctx, int fd);
void deinit_client_ctx(server_ctx_t* sctx, client_ctx_t* cctx);

#endif
