#ifndef NET_LOGGER_H
#define NET_LOGGER_H

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <net/net.h>
#include <unistd.h>
#include <sys/tty.h>

#define LOG_PC_IP   "255.255.255.255"  // Limited broadcast
#define LOG_PORT    18194

#include "ui.h"

static int _log_sock = -1;

static inline void net_logger_init() {
    _log_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (_log_sock < 0) return;
    int broadcast = 1;
    setsockopt(_log_sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
}

static inline void net_log(const char *fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    // Remove all carriage returns (\r) to prevent TTY overwriting/cursor reset issues
    int write_idx = 0;
    for (int i = 0; buf[i] != '\0'; i++) {
        if (buf[i] != '\r') {
            buf[write_idx++] = buf[i];
        }
    }
    buf[write_idx] = '\0';

    // Strip all trailing newlines
    int len = strlen(buf);
    while (len > 0 && buf[len - 1] == '\n') {
        buf[len - 1] = '\0';
        len--;
    }

    // Save stripped buffer for UI
    char clean_buf[2048];
    strncpy(clean_buf, buf, sizeof(clean_buf) - 1);
    clean_buf[sizeof(clean_buf) - 1] = '\0';

    // Append exactly one newline for TTY and network log
    strncat(buf, "\n", sizeof(buf) - strlen(buf) - 1);

    if (_log_sock >= 0) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(LOG_PORT);
        addr.sin_addr.s_addr = htonl(0xFFFFFFFF);
        sendto(_log_sock, buf, strlen(buf), 0, (struct sockaddr*)&addr, sizeof(addr));
    }
    
    // UI Update
    ui_push_log(clean_buf);

    write(1, buf, strlen(buf));
}

#define NLOG(fmt, ...) net_log("[ML-PS3] " fmt "\n", ##__VA_ARGS__)

#endif // NET_LOGGER_H
