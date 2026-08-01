#ifndef NET_LOGGER_H
#define NET_LOGGER_H

void net_logger_init(void);
void net_logger_shutdown(void);
void net_log(const char *fmt, ...);

#define NLOG(fmt, ...) net_log("[ML-PS3] " fmt "\n", ##__VA_ARGS__)

#endif
