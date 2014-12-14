#ifndef __NET_H__
#define __NET_H__

#include <sys/socket.h>

#define NET_SERVER_SOCKET 0x1
#define NET_SOCKET_STRING_SIZE 32

typedef struct {
    socklen_t addrlen;
    struct sockaddr_storage addr;
    char to_string[NET_SOCKET_STRING_SIZE];
} socket_t;

// turn string like 'localhost:1111' into socket_t structure
socket_t* socketize(const char* arg, int flags);
int setup_socket(const socket_t* sock, int flags);
int connect_client_socket(const socket_t* sock, int fd);
void humanize_socket(socket_t* sock);

#endif
