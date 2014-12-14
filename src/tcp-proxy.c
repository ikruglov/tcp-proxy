#include <signal.h>
#include <pthread.h>

#include "net.h"
#include "common.h"
#include "server_ctx.h"

// https://domsch.com/linux/lpc2010/Scaling_techniques_for_servers_with_high_connection%20rates.pdf
// https://gist.github.com/carun/8146981

pthread_t start_detached_thread(void *(*routine) (void*), void* arg)
{
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&tid, &attr, routine, arg);
    pthread_attr_destroy(&attr);
    return tid;
}

void* run_event_loop(void* arg) {
    // blocking all signals in threads is a good practise
    sigset_t sigs_to_block;
    sigfillset(&sigs_to_block);
    pthread_sigmask(SIG_BLOCK, &sigs_to_block, NULL);

    ev_run((struct ev_loop*) arg, EVFLAG_NOSIGMASK);
    _D("exit ev_run");
    return NULL;
}

int main(int argc, char** argv)
{
    const char* from = argv[1];
    socket_t* ssock = socketize(from, NET_SERVER_SOCKET);

    const char* to = argv[2];
    socket_t* usock = socketize(to, 0);

    const size_t threads = 1;
    pthread_t server_ctx_ids[threads];
    server_ctx_t server_ctxs[threads];

    for (size_t i = 0; i < threads; ++i) {
        // TODO do this work in threads
        if (init_server_ctx(&server_ctxs[i], ssock, usock))
            ERRX("Failed to initialize one of server contexts");

        server_ctx_ids[i] = start_detached_thread(run_event_loop, server_ctxs[i].loop);
    }

    // TODO proper signal handling
    while (1)
        sleep(1);

    // terminate threads
    for (size_t i = 0; i < threads; ++i) {
        terminate_server_ctx(&server_ctxs[i]);
        //free_server_ctx(&server_ctxs[i]);
    }
}
