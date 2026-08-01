#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/time.h>
#include <sys/socket.h>

#include "../third_party/moonlight-common-c/src/ps3_compat.h"

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res) {
    struct hostent *he;
    struct addrinfo *ai;
    struct sockaddr_in *sin;

    if (node == NULL) return -1;

    he = gethostbyname(node);
    if (!he) return -1;

    ai = malloc(sizeof(struct addrinfo));
    if (!ai) return -1;

    memset(ai, 0, sizeof(struct addrinfo));
    ai->ai_family = AF_INET;
    ai->ai_socktype = hints ? hints->ai_socktype : SOCK_STREAM;
    ai->ai_protocol = hints ? hints->ai_protocol : IPPROTO_TCP;
    ai->ai_addrlen = sizeof(struct sockaddr_in);

    sin = malloc(sizeof(struct sockaddr_in));
    if (!sin) {
        free(ai);
        return -1;
    }

    memset(sin, 0, sizeof(struct sockaddr_in));
    sin->sin_len = sizeof(struct sockaddr_in);
    sin->sin_family = AF_INET;
    if (service) {
        sin->sin_port = htons(atoi(service));
    }
    memcpy(&sin->sin_addr, he->h_addr, he->h_length);

    ai->ai_addr = (struct sockaddr *)sin;
    ai->ai_next = NULL;

    *res = ai;
    return 0;
}

void freeaddrinfo(struct addrinfo *res) {
    struct addrinfo *next;
    while (res) {
        next = res->ai_next;
        if (res->ai_addr) free(res->ai_addr);
        if (res->ai_canonname) free(res->ai_canonname);
        free(res);
        res = next;
    }
}


#include "uuid.h"
#include "random.h"

void uuid_generate_random(uuid_t out) {
    if (ps3_random_bytes(out, sizeof(uuid_t)) != 0) {
        memset(out, 0, sizeof(uuid_t));
        return;
    }
    // Set UUID version to 4
    out[6] = (out[6] & 0x0F) | 0x40;
    out[8] = (out[8] & 0x3F) | 0x80;
}

void uuid_unparse(const uuid_t uu, char *out) {
    sprintf(out,
            "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            uu[0], uu[1], uu[2], uu[3],
            uu[4], uu[5],
            uu[6], uu[7],
            uu[8], uu[9],
            uu[10], uu[11], uu[12], uu[13], uu[14], uu[15]);
}

int sigaction(int signum, const void *act, void *oldact) {
    (void)signum;
    (void)act;
    (void)oldact;
    return 0;
}
