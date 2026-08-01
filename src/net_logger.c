#include "net_logger.h"

#include <net/net.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ui.h"

#ifndef ENABLE_NETWORK_LOGGING
#define ENABLE_NETWORK_LOGGING 0
#endif

#define LOG_PORT 18194

static int log_sock = -1;

void net_logger_init(void) {
#if ENABLE_NETWORK_LOGGING
    int broadcast = 1;

    log_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (log_sock >= 0) {
        setsockopt(log_sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
    }
#endif
}

void net_logger_shutdown(void) {
    if (log_sock >= 0) {
        close(log_sock);
        log_sock = -1;
    }
}

void net_log(const char *fmt, ...) {
    char buf[2048];
    char clean_buf[2048];
    va_list args;
    int write_idx = 0;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    for (int i = 0; buf[i] != '\0'; i++) {
        if (buf[i] != '\r') buf[write_idx++] = buf[i];
    }
    buf[write_idx] = '\0';

    while (write_idx > 0 && buf[write_idx - 1] == '\n') {
        buf[--write_idx] = '\0';
    }

    strncpy(clean_buf, buf, sizeof(clean_buf) - 1);
    clean_buf[sizeof(clean_buf) - 1] = '\0';
    strncat(buf, "\n", sizeof(buf) - strlen(buf) - 1);

    if (log_sock >= 0) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(LOG_PORT);
        addr.sin_addr.s_addr = htonl(0xFFFFFFFF);
        sendto(log_sock, buf, strlen(buf), 0, (struct sockaddr *)&addr, sizeof(addr));
    }

    ui_push_log(clean_buf);
    write(1, buf, strlen(buf));
}
