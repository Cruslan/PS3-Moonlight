#ifndef PS3_COMPAT_H
#define PS3_COMPAT_H

#include <sys/socket.h>
#include <net/net.h>
#include <net/select.h>
#include <net/socket.h>
#include <net/poll.h>
#include <netdb.h>

#ifdef AF_INET6
#undef AF_INET6
#endif

// PS3 sockaddr_in has 1-byte sin_len and 1-byte sin_family.
// We make sockaddr_storage match this layout.
struct sockaddr_storage {
    uint8_t ss_len;
    uint8_t ss_family;
    char __ss_pad[126];
};

#define errno net_errno
#define h_errno net_h_errno

#define SO_NBIO 0x1100
#define TCP_NODELAY 1
#define TCP_MAXSEG 2

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res);
void freeaddrinfo(struct addrinfo *res);
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);
int inet_pton(int af, const char *src, void *dst);

#endif
