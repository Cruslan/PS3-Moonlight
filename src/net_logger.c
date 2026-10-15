#include "net_logger.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ui.h"

void net_logger_init(void) {
    /* No network socket needed: PS3HEN/CFW natively broadcasts stdout (TTY) over UDP port 18194 */
}

void net_logger_shutdown(void) {
    /* No-op */
}

void net_log(const char *fmt, ...) {
    char buf[2048];
    char clean_buf[2048];
    va_list args;
    int write_idx = 0;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    /* Strip carriage returns for clean console/TTY and UI display */
    for (int i = 0; buf[i] != '\0'; i++) {
        if (buf[i] != '\r') buf[write_idx++] = buf[i];
    }
    buf[write_idx] = '\0';

    /* Strip trailing newlines for UI HUD log queue */
    while (write_idx > 0 && buf[write_idx - 1] == '\n') {
        buf[--write_idx] = '\0';
    }

    strncpy(clean_buf, buf, sizeof(clean_buf) - 1);
    clean_buf[sizeof(clean_buf) - 1] = '\0';

    /* Push to in-app UI HUD overlay */
    ui_push_log(clean_buf);

    /* Append newline and emit directly to TTY (fd 1 / stdout).
     * PS3HEN / CFW Lv-2 kernel automatically broadcasts TTY output via UDP port 18194. */
    strncat(buf, "\n", sizeof(buf) - strlen(buf) - 1);
    write(1, buf, strlen(buf));
}
