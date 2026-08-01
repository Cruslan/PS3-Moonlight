#include "random.h"

#include <lv2/system.h>

int ps3_random_bytes(void *buffer, size_t length) {
    unsigned char *cursor = buffer;

    if (!buffer && length != 0) return -1;

    while (length > 0) {
        size_t chunk = length > RANDOM_NUMBER_MAX_SIZE ? RANDOM_NUMBER_MAX_SIZE : length;
        if (sysGetRandomNumber(cursor, chunk) != 0) return -1;
        cursor += chunk;
        length -= chunk;
    }

    return 0;
}

int ps3_random_u32(uint32_t *value) {
    return ps3_random_bytes(value, sizeof(*value));
}
