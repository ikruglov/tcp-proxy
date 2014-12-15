#ifndef __CONFIG_H__
#define __CONFIG_H__

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "common.h"

#define LOAD_DEFAULT_SETTING 0
#define LOAD_MAX_SETTING ((size_t) -1)

typedef struct {
    size_t nproc;
    size_t pipe_size;
    size_t send_size;
    size_t recv_size;
    size_t minconn;
    size_t maxconn;
} GLOBAL;

/* gl_settings should be initialized in thread-safe
 * environment i.e. inside main, before threads started.
 * Nevertheless, the struct can be shared across threads
 * later for read-only purposes */
extern GLOBAL gl_settings;
inline static
void init_global_settings(GLOBAL* gl)
{
    memset(gl, 0, sizeof(GLOBAL));
}

inline static
size_t read_proc_setting_int(const char* path)
{
    // return 0 if failed to read
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;

    char buf[128];
    int res = read(fd, buf, sizeof(buf));
    if (res <= 0) return 0;

    buf[res] = '\0'; 
    size_t val = atoll(buf);
    INFO("loaded %s = %zd", path, val);
    return val;
}

inline static
size_t run_command_int(const char* cmd)
{
    FILE* pipe = popen(cmd, "r");
    if (!pipe) return 0;

    char buf[128];
    char* str = fgets(buf, sizeof(buf), pipe);
    pclose(pipe);

    if (!str) return 0;

    size_t val = atoll(buf);
    INFO("loaded %s = %zd", cmd, val);
    return val;
}

inline static
void read_global_settings(GLOBAL* gl)
{
    if (gl->nproc == LOAD_MAX_SETTING)
        gl->nproc = run_command_int("/usr/bin/nproc");

    if (gl->pipe_size == LOAD_MAX_SETTING)
        gl->pipe_size = read_proc_setting_int("/proc/sys/fs/pipe-max-size");

    if (gl->send_size == LOAD_MAX_SETTING)
        gl->send_size = read_proc_setting_int("/proc/sys/net/core/wmem_max");

    if (gl->recv_size == LOAD_MAX_SETTING)
        gl->recv_size = read_proc_setting_int("/proc/sys/net/core/rmem_max");
}

#endif

