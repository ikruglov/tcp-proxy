#include <signal.h>
#include <pthread.h>

#include "net.h"
#include "config.h"
#include "common.h"
#include "server_ctx.h"

// https://domsch.com/linux/lpc2010/Scaling_techniques_for_servers_with_high_connection%20rates.pdf
// https://gist.github.com/carun/8146981

// see commnect in config.h
GLOBAL gl_settings;

pthread_t start_thread(void *(*routine) (void*), void* arg)
{
    pthread_t tid;
    pthread_create(&tid, NULL, routine, arg);
    return tid;
}

void* run_event_loop(void* arg)
{
    // blocking all signals in threads is a good practise
    sigset_t sigs_to_block;
    sigfillset(&sigs_to_block);
    pthread_sigmask(SIG_BLOCK, &sigs_to_block, NULL);

    _D("ev_run");
    ev_run((struct ev_loop*) arg, EVFLAG_NOSIGMASK);
    _D("exit ev_run");

    return NULL;
}

volatile static int g_should_exit = 0;
void sig_handler(int signum)
{
    switch(signum) {
        case SIGTERM:
            INFO("caugth signal SIGTERM");
            g_should_exit = 1;
            break;

        case SIGINT:
            INFO("caugth signal SIGINT");
            g_should_exit = 1;
            break;

        default:
            INFO("IGNORE: unexpected signal %d", signum);
    }
}

int main(int argc, char** argv)
{
    struct sigaction sigact;
    memset(&sigact, 0, sizeof(sigact));
    sigact.sa_handler = sig_handler;
    sigact.sa_flags = SA_SIGINFO;

    sigaction(SIGINT, &sigact, NULL);
    sigaction(SIGTERM, &sigact, NULL);
    sigaction(SIGPIPE, &sigact, NULL);

    // read global settings
    init_global_settings((GLOBAL*) &gl_settings);
    gl_settings.nproc = LOAD_MAX_SETTING;
    gl_settings.pipe_size = LOAD_MAX_SETTING;
    gl_settings.recv_size = LOAD_MAX_SETTING;
    gl_settings.send_size = LOAD_MAX_SETTING;
    gl_settings.minconn = 1000;
    gl_settings.maxconn = 10 * gl_settings.minconn;
    read_global_settings((GLOBAL*) &gl_settings);

    const char* from = argv[1];
    socket_t* ssock = socketize(from, NET_SERVER_SOCKET);

    const char* to = argv[2];
    socket_t* usock = socketize(to, 0);

    const size_t threads = gl_settings.nproc;
    pthread_t server_ctx_ids[threads];
    server_ctx_t server_ctxs[threads];
    INFO("starting %zu eventloops", threads);

    for (size_t i = 0; i < threads; ++i) {
        if (init_server_ctx(&server_ctxs[i], ssock, usock))
            ERRX("Failed to initialize one of server contexts");

        server_ctx_ids[i] = start_thread(run_event_loop, server_ctxs[i].loop);
    }

    while (!g_should_exit) usleep(100000); // 0.1s

    INFO("Signaling all eventloops to exit");
    for (size_t i = 0; i < threads; ++i) {
        terminate_server_ctx(&server_ctxs[i]);
    }

    // giving threads 2 sec to gracefull terminate
    // and if failed, terminate threads

    int still_alive = 1;
    for (size_t t = 0; t < 20 && still_alive; ++t) {
        still_alive = 0;
        usleep(100000); // yes, start with a small sleep

        for (size_t i = 0; i < threads; ++i) {
            still_alive |= (pthread_kill(server_ctx_ids[i], 0) != ESRCH);
        }
    }

    if (still_alive) {
        INFO("Some threads still alive, kill them! Won't correctly free internal structures. Hopefully, kernel will do this!");
        return EXIT_SUCCESS;
    }

    for (size_t i = 0; i < threads; ++i) {
        free_server_ctx(&server_ctxs[i]);
    }

    INFO("Exiting...");
    return EXIT_SUCCESS;
}
