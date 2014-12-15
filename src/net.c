#include <netdb.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "net.h"
#include "common.h"

socket_t* socketize(const char* arg, int flags)
{
    assert(arg);
    int ai_flags = flags & NET_SERVER_SOCKET ? AI_PASSIVE : 0;

    char* hostname = strdup(arg);
    socket_t* sock = malloc_or_die(sizeof(socket_t));

    char* colon = strrchr(hostname, ':');
    if (!colon) ERRPX("Unknown format for conf-string, ex: localhost:6379");

    *colon = '\0';
    const char* port = colon + 1;

    struct addrinfo hints, *result;
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_flags    = ai_flags;    /* For wildcard IP address */
    hints.ai_family   = AF_INET;     //AF_UNSPEC;     /* Allow IPv4 or IPv6 */
    hints.ai_socktype = SOCK_STREAM; /* Stream socket */
    hints.ai_protocol = IPPROTO_TCP; /* TPC protocol */

    // FIXME do we need support of multihomed hosts???
    int e = getaddrinfo(hostname, port, &hints, &result);
    if (e) ERRX("Failed to parse/resolve %s: %s", hostname, gai_strerror(e));

    assert(result);
    assert(sizeof(sock->addr) >= result->ai_addrlen); // just in case ;-)

    // TODO prefer IPv4 over IPv6 when both available
    sock->addrlen = result->ai_addrlen;
    memcpy(&sock->addr, result->ai_addr, result->ai_addrlen);

    humanize_socket(sock);
    INFO("socketize: %s -> %s", arg, sock->to_string);

    freeaddrinfo(result);
    free(hostname);
    return sock;
}

int setup_socket(const socket_t* sock, int flags)
{
    int fd = socket(sock->addr.ss_family, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP);
    if (fd < 0) {
        ERRP("Failed to create socket %s", sock->to_string);
        return -1;
    }

    if (flags & NET_SERVER_SOCKET) {
        int yes = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes))) {
            ERRP("Failed to setsockopt SO_REUSEADDR on %s", sock->to_string);
            goto error;
        }

        if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes))) {
            ERRP("Failed to setsockopt SO_REUSEPORT on %s", sock->to_string);
            goto error;
        }

    //    if (sendbuf > 0 && setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sendbuf, sizeof(sendbuf))) {
    //        ERRP("Failed to setsockopt SO_SNDBUF on %s", sock->to_string);
    //        goto error;
    //    }
    //
    //    if (recvbuf > 0 && setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &recvbuf, sizeof(recvbuf))) {
    //        ERRP("Failed to setsockopt SO_RCVBUF on %s", sock->to_string);
    //        goto error;
    //    }

        if (bind(fd, (struct sockaddr *) &sock->addr, sock->addrlen)) {
            ERRP("Failed to bind socket to %s", sock->to_string);
            goto error;
        }

        if (listen(fd, SOMAXCONN)) {
            ERRP("Failed to listen on socket %s", sock->to_string);
            goto error;
        }
    }

    _D("get fd %d for %s", fd, sock->to_string);
    return fd;

error:
    close(fd);
    return -1;
}

int connect_client_socket(const socket_t* sock, int fd)
{
    while (1) {
        int ret = connect(fd, (const struct sockaddr*) &sock->addr, sock->addrlen);
        if (ret == 0) {
            return 1;
        } else {
            if (errno == EINTR) {
                continue;
            } else if (errno == EINPROGRESS) {
                return 0;
            } else {
                ERRP("Failed to connect to %s", sock->to_string);
                return -1;
            }
        }
    }

    return -1;
}

void humanize_socket(socket_t* sock)
{
    assert(sock);
    assert(sock->addr.ss_family == AF_INET || sock->addr.ss_family == AF_INET6);

    char buf[24]; // SOCKET_STRING_SIZE - 8 (yes, this's magic number!)
    if (sock->addr.ss_family == AF_INET) {
        struct sockaddr_in* in = (struct sockaddr_in*) &sock->addr;
        snprintf(sock->to_string,
                 NET_SOCKET_STRING_SIZE, "%s:%d",
                 inet_ntop(AF_INET, &in->sin_addr, buf, sizeof(buf)),
                 ntohs(in->sin_port));
    } else {
        struct sockaddr_in6* in = (struct sockaddr_in6*) &sock->addr;
        snprintf(sock->to_string,
                 NET_SOCKET_STRING_SIZE, "[%s]:%d",
                 inet_ntop(AF_INET6, &in->sin6_addr, buf, sizeof(buf)),
                 ntohs(in->sin6_port));
    }
}
