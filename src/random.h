#ifndef PS3_RANDOM_H
#define PS3_RANDOM_H

#include <stddef.h>
#include <stdint.h>

int ps3_random_bytes(void *buffer, size_t length);
int ps3_random_u32(uint32_t *value);

#endif
