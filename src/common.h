#ifndef __COMMON_H__
#define __COMMON_H__

#include <err.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>

#define FORMAT(fmt, arg...) \
    "[%llu] [tid:%llu] [%s() %s:%d] " fmt "\n", \
    (unsigned long long) time(NULL), (unsigned long long) pthread_self(), __func__, __FILE__, __LINE__, ##arg

#define ERR(fmt, arg...)        printf(FORMAT(fmt, ##arg))
#define ERRP(fmt, arg...)       printf(FORMAT(fmt ": %s", ##arg, errno ? strerror(errno) : "undefined error"))
#define ERRN(fmt, sock, arg...) printf(FORMAT(fmt "[%s]: %s", ##arg, sock->to_string, errno ? strerror(errno) : "undefined error"))
#define ERRX(fmt, arg...)       errx(EXIT_FAILURE, FORMAT(fmt, ##arg));
#define ERRPX(fmt, arg...)      errx(EXIT_FAILURE, FORMAT(fmt ": %s", ##arg, errno ? strerror(errno) : "undefined error"))

#ifdef NDEBUG
#define _D(fmt, arg...)
#else
#define _D(fmt, arg...) printf(FORMAT(fmt, ##arg))
#endif

inline static
void* malloc_or_die(size_t size) {
    void* ptr = malloc(size);
    if (!ptr) ERRPX("Failed to malloc %zu bytes", size);
    return ptr;
}

inline static
void* calloc_or_die(size_t nmemb, size_t size) {
    void* ptr = calloc(nmemb, size);
    if (!ptr) ERRPX("Failed to calloc %zu bytes", nmemb * size);
    return ptr;
}

#endif // __COMMON_H__
