#include "random.h"

#include <lv2/system.h>
#include <lv2/systime.h>
#include <stdlib.h>

#ifndef RANDOM_NUMBER_MAX_SIZE
#define RANDOM_NUMBER_MAX_SIZE 64
#endif

static unsigned int g_rand_seed = 0;

static void ensure_seed_initialized(void) {
    if (g_rand_seed == 0) {
        g_rand_seed = (unsigned int)(sysGetSystemTime() ^ 0xa5a5a5a5);
        if (g_rand_seed == 0) g_rand_seed = 123456789;
    }
}

int ps3_random_bytes(void *buffer, size_t length) {
    unsigned char *cursor = buffer;

    if (!buffer && length != 0) return -1;
    if (length == 0) return 0;

    ensure_seed_initialized();

    size_t remaining = length;
    while (remaining > 0) {
        size_t chunk = remaining > RANDOM_NUMBER_MAX_SIZE ? RANDOM_NUMBER_MAX_SIZE : remaining;
        int rc = sysGetRandomNumber(cursor, chunk);
        if (rc == 0) {
            cursor += chunk;
            remaining -= chunk;
        } else {
            // Syscall failed or unsupported on current HEN payload.
            // Fallback to PRNG for remaining bytes.
            for (size_t i = 0; i < remaining; i++) {
                g_rand_seed = g_rand_seed * 1103515245 + 12345;
                cursor[i] = (unsigned char)((g_rand_seed / 65536) % 256);
            }
            break;
        }
    }

    return 0;
}

int ps3_random_u32(uint32_t *value) {
    return ps3_random_bytes(value, sizeof(*value));
}

