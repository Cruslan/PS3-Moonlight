/**
 * SPU Cryptography Bridge for PS3-Moonlight
 *
 * Dispatches compute-intensive cryptographic operations (SHA-256 digest
 * calculation and AES-128 CBC audio decryption) to a dedicated Cell SPU
 * worker thread. All communications use cacheline-aligned DMA and SPU
 * Signal Notification registers for zero-overhead PPU offloading.
 *
 * Live telemetry and hardware tick profiling are broadcast over UDP 18196.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <errno.h>
#include <stdarg.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <sys/spu.h>
#include <lv2/spu.h>
#include <sys/mutex.h>

#include "spu_crypto_bridge.h"
#include "spu_crypto.h"
#include "net_logger.h"
#include <ps3perf.h>

#define SPU_CRYPTO_BOUNCE_SIZE     16384 /* 16 KB bounce buffer for DMA */
#define SPU_CRYPTO_TIMEOUT_TB      8000000ULL /* ~100 ms at 79.8 MHz timebase */
#define SPU_CRYPTO_TELEMETRY_PORT  18196

/* Embedded raw SPU ELF binary exported by bin2s */
extern const uint8_t spu_crypto_elf[];

static bool g_initialized = false;
static sys_mutex_t g_crypto_mutex;
static sysSpuImage g_spu_image;
static u32 g_group_id = 0;
static u32 g_thread_id = 0;
static int g_telemetry_sock = -1;

/* Cacheline-aligned DMA buffers in Main RAM */
static spu_crypto_cmd_t *g_cmd = NULL;
static uint8_t *g_bounce_in = NULL;
static uint8_t *g_bounce_out = NULL;
static uint8_t *g_bounce_key = NULL;
static uint8_t *g_bounce_iv = NULL;
static uint8_t *g_bounce_hash = NULL;

static float g_last_elapsed_us = 0.0f;

static inline uint64_t get_timebase(void) {
    uint64_t tb;
    asm volatile("mftb %0" : "=r"(tb));
    return tb;
}

/**
 * Broadcast formatted telemetry message over UDP port 18196.
 */
static void spu_crypto_send_telemetry(const char *fmt, ...) {
    if (g_telemetry_sock < 0) return;

    char buf[512];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (len > 0) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(SPU_CRYPTO_TELEMETRY_PORT);
        addr.sin_addr.s_addr = htonl(0xFFFFFFFF);
        sendto(g_telemetry_sock, buf, len, 0, (struct sockaddr *)&addr, sizeof(addr));
    }
}

/**
 * Initialize SPU Crypto accelerator.
 * Spawns a dedicated SPU worker thread running the spu_crypto kernel.
 * Returns 0 on success, negative error code on failure.
 */
int spu_crypto_init(void) {
    if (g_initialized) {
        return 0;
    }

    /* Initialize UDP 18196 telemetry socket */
    int broadcast = 1;
    g_telemetry_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_telemetry_sock >= 0) {
        setsockopt(g_telemetry_sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
    }

    /* Initialize mutual exclusion lock */
    sys_mutex_attr_t mutex_attr;
    sysMutexAttrInitialize(mutex_attr);
    if (sysMutexCreate(&g_crypto_mutex, &mutex_attr) != 0) {
        printf("[SPU CRYPTO] Failed to create bridge mutex\n");
        return -1;
    }

    /* Allocate 128-byte aligned command and bounce buffers */
    g_cmd = (spu_crypto_cmd_t *)memalign(128, sizeof(spu_crypto_cmd_t));
    g_bounce_in = (uint8_t *)memalign(128, SPU_CRYPTO_BOUNCE_SIZE);
    g_bounce_out = (uint8_t *)memalign(128, SPU_CRYPTO_BOUNCE_SIZE);
    g_bounce_key = (uint8_t *)memalign(16, 32);
    g_bounce_iv = (uint8_t *)memalign(16, 16);
    g_bounce_hash = (uint8_t *)memalign(16, 32);

    if (!g_cmd || !g_bounce_in || !g_bounce_out || !g_bounce_key || !g_bounce_iv || !g_bounce_hash) {
        printf("[SPU CRYPTO] Memory allocation failed for DMA bounce buffers\n");
        spu_crypto_shutdown();
        return -ENOMEM;
    }

    memset(g_cmd, 0, sizeof(spu_crypto_cmd_t));

    /* Initialize SPU subsystems (6 SPU threads, 0 raw SPUs) */
    int ret = sysSpuInitialize(6, 0);
    if (ret != 0 && ret != (int)0x80010003) {
        printf("[SPU CRYPTO] sysSpuInitialize returned 0x%08x\n", ret);
    }

    /* Import embedded SPU ELF image */
    ret = sysSpuImageImport(&g_spu_image, spu_crypto_elf, 0);
    if (ret != 0) {
        printf("[SPU CRYPTO] sysSpuImageImport failed: 0x%08x\n", ret);
        spu_crypto_shutdown();
        return ret;
    }

    /* Create SPU Thread Group */
    sysSpuThreadGroupAttribute grp_attr;
    sysSpuThreadGroupAttributeInitialize(grp_attr);
    sysSpuThreadGroupAttributeName(grp_attr, "spu_crypto_grp");

    ret = sysSpuThreadGroupCreate(&g_group_id, 1, 100, &grp_attr);
    if (ret != 0) {
        printf("[SPU CRYPTO] sysSpuThreadGroupCreate failed: 0x%08x\n", ret);
        spu_crypto_shutdown();
        return ret;
    }

    /* Initialize SPU Thread */
    sysSpuThreadAttribute thr_attr;
    sysSpuThreadAttributeInitialize(thr_attr);
    sysSpuThreadAttributeName(thr_attr, "spu_crypto_thr");

    sysSpuThreadArgument thr_arg;
    sysSpuThreadArgumentInitialize(thr_arg);
    thr_arg.arg0 = (u64)(uintptr_t)g_cmd;

    ret = sysSpuThreadInitialize(&g_thread_id, g_group_id, 0, &g_spu_image, &thr_attr, &thr_arg);
    if (ret != 0) {
        printf("[SPU CRYPTO] sysSpuThreadInitialize failed: 0x%08x\n", ret);
        spu_crypto_shutdown();
        return ret;
    }

    /* Register SPU worker thread with autonomous ps3perf profiler */
    ps3perf_register_spu(g_thread_id, spu_crypto_elf);

    /* Configure signal registers to overwrite mode */
    sysSpuThreadSetConfiguration(g_thread_id, SPU_SIGNAL1_OVERWRITE | SPU_SIGNAL2_OVERWRITE);

    /* Start SPU Thread Group */
    ret = sysSpuThreadGroupStart(g_group_id);
    if (ret != 0) {
        printf("[SPU CRYPTO] sysSpuThreadGroupStart failed: 0x%08x\n", ret);
        spu_crypto_shutdown();
        return ret;
    }

    g_initialized = true;
    printf("[SPU CRYPTO] SPU Crypto Accelerator successfully initialized on Thread Group 0x%x, Thread 0x%x (Telemetry UDP:%d)\n",
           g_group_id, g_thread_id, SPU_CRYPTO_TELEMETRY_PORT);
    NLOG("SPU Crypto Accelerator initialized (Group: 0x%x, Thread: 0x%x, Telemetry UDP: %d)",
         g_group_id, g_thread_id, SPU_CRYPTO_TELEMETRY_PORT);

    spu_crypto_send_telemetry("[SPU-CRYPTO] Initialized: group=0x%x thread=0x%x telemetry_port=%d\n",
                              g_group_id, g_thread_id, SPU_CRYPTO_TELEMETRY_PORT);

    /* Run immediate startup self-test on the SPU to verify live execution and DMA */
    uint8_t test_hash[32];
    int test_ret = spu_crypto_sha256("abc", 3, test_hash);
    if (test_ret == 0 && test_hash[0] == 0xba && test_hash[1] == 0x78) {
        printf("[SPU CRYPTO] SPU Self-Test PASSED: SHA-256('abc') verified in %.2f us\n", g_last_elapsed_us);
        NLOG("SPU Self-Test PASSED: SHA-256 verified in %.2f us", g_last_elapsed_us);
    } else {
        printf("[SPU CRYPTO] SPU Self-Test FAILED (ret=%d)!\n", test_ret);
        NLOG("SPU Self-Test FAILED (ret=%d)!", test_ret);
    }
    return 0;
}

/**
 * Shut down the SPU Crypto worker thread and release resources.
 */
void spu_crypto_shutdown(void) {
    if (g_initialized) {
        spu_crypto_send_telemetry("[SPU-CRYPTO] Accelerator shutting down\n");

        /* Signal SPU worker to terminate */
        if (g_cmd) {
            g_cmd->cmd = SPU_CRYPTO_CMD_QUIT;
            sysSpuThreadWriteSignal(g_thread_id, 0, SPU_CRYPTO_CMD_QUIT);
        }

        ps3perf_unregister_spu(g_thread_id);

        /* Wait for thread group exit */
        u32 cause = 0, status = 0;
        sysSpuThreadGroupJoin(g_group_id, &cause, &status);
        sysSpuThreadGroupDestroy(g_group_id);
        sysSpuImageClose(&g_spu_image);

        g_initialized = false;
    }

    if (g_cmd) { free(g_cmd); g_cmd = NULL; }
    if (g_bounce_in) { free(g_bounce_in); g_bounce_in = NULL; }
    if (g_bounce_out) { free(g_bounce_out); g_bounce_out = NULL; }
    if (g_bounce_key) { free(g_bounce_key); g_bounce_key = NULL; }
    if (g_bounce_iv) { free(g_bounce_iv); g_bounce_iv = NULL; }
    if (g_bounce_hash) { free(g_bounce_hash); g_bounce_hash = NULL; }

    if (g_telemetry_sock >= 0) {
        close(g_telemetry_sock);
        g_telemetry_sock = -1;
    }

    if (g_crypto_mutex) {
        sysMutexDestroy(g_crypto_mutex);
        g_crypto_mutex = 0;
    }
}

/**
 * Compute SHA-256 hash using the SPU accelerator.
 * @param in Input buffer
 * @param len Byte length
 * @param out Output 32-byte hash buffer
 * @return 0 on success, negative on error
 */
int spu_crypto_sha256(const void *in, size_t len, uint8_t out[32]) {
    if (!g_initialized || !in || !out) {
        return -EINVAL;
    }

    if (len > SPU_CRYPTO_BOUNCE_SIZE) {
        printf("[SPU CRYPTO] SHA-256 payload exceeds bounce size (%zu > %d)\n", len, SPU_CRYPTO_BOUNCE_SIZE);
        return -ENOMEM;
    }

    sysMutexLock(g_crypto_mutex, 0);

    /* Copy input data into 128-byte aligned bounce buffer to prevent DMA faults */
    memcpy(g_bounce_in, in, len);

    /* Zero padding space up to 16-byte boundary to avoid uninitialized memory DMA */
    size_t padded_len = (len + 15) & ~15;
    if (padded_len > len) {
        memset(g_bounce_in + len, 0, padded_len - len);
    }

    g_cmd->cmd = SPU_CRYPTO_CMD_SHA256;
    g_cmd->status = SPU_CRYPTO_STATUS_BUSY;
    g_cmd->data_len = (uint32_t)len;
    g_cmd->ea_input = (uint64_t)(uintptr_t)g_bounce_in;
    g_cmd->ea_output = (uint64_t)(uintptr_t)g_bounce_hash;

    /* Enforce store ordering before signaling SPU */
    asm volatile("sync" ::: "memory");

    /* Send command trigger to SPU Signal 1 */
    sysSpuThreadWriteSignal(g_thread_id, 0, SPU_CRYPTO_CMD_SHA256);

    /* Poll for completion with hardware timebase timeout */
    uint64_t t0 = get_timebase();
    while (*(volatile int32_t *)&g_cmd->status == SPU_CRYPTO_STATUS_BUSY) {
        asm volatile("sync" ::: "memory");
        if (get_timebase() - t0 > SPU_CRYPTO_TIMEOUT_TB) {
            printf("[SPU CRYPTO] SHA-256 timeout on SPU!\n");
            sysMutexUnlock(g_crypto_mutex);
            return -ETIMEDOUT;
        }
    }

    /* Enforce memory barrier before reading DMA output buffer */
    asm volatile("sync" ::: "memory");

    g_last_elapsed_us = (float)g_cmd->elapsed_ticks / 79.8f;
    memcpy(out, g_bounce_hash, 32);

    /* Broadcast real-time telemetry on UDP 18196 */
    spu_crypto_send_telemetry("[SPU-CRYPTO] SHA-256 len=%zu ticks=%u elapsed=%.2f us\n",
                              len, g_cmd->elapsed_ticks, g_last_elapsed_us);

    sysMutexUnlock(g_crypto_mutex);
    return 0;
}

/**
 * Decrypt data using AES-128 CBC on the SPU accelerator.
 * @param key 16-byte AES key
 * @param iv 16-byte Initialization Vector
 * @param in Ciphertext buffer (must be multiple of 16)
 * @param len Length in bytes (multiple of 16)
 * @param out Plaintext output buffer
 * @return 0 on success, negative on error
 */
int spu_crypto_aes128_cbc_decrypt(const uint8_t key[16], const uint8_t iv[16],
                                  const void *in, size_t len, void *out) {
    if (!g_initialized || !key || !iv || !in || !out) {
        return -EINVAL;
    }

    if ((len % 16) != 0 || len == 0) {
        return -EINVAL;
    }

    if (len > SPU_CRYPTO_BOUNCE_SIZE) {
        printf("[SPU CRYPTO] AES-128 payload exceeds bounce size (%zu > %d)\n", len, SPU_CRYPTO_BOUNCE_SIZE);
        return -ENOMEM;
    }

    sysMutexLock(g_crypto_mutex, 0);

    /* Stage Key and IV in 16-byte aligned buffers */
    memcpy(g_bounce_key, key, 16);
    memcpy(g_bounce_iv, iv, 16);
    memcpy(g_bounce_in, in, len);

    g_cmd->cmd = SPU_CRYPTO_CMD_AES128_CBC;
    g_cmd->status = SPU_CRYPTO_STATUS_BUSY;
    g_cmd->data_len = (uint32_t)len;
    g_cmd->ea_key = (uint64_t)(uintptr_t)g_bounce_key;
    g_cmd->ea_iv = (uint64_t)(uintptr_t)g_bounce_iv;
    g_cmd->ea_input = (uint64_t)(uintptr_t)g_bounce_in;
    g_cmd->ea_output = (uint64_t)(uintptr_t)g_bounce_out;

    /* Enforce store ordering before signaling SPU */
    asm volatile("sync" ::: "memory");

    /* Send command trigger to SPU Signal 1 */
    sysSpuThreadWriteSignal(g_thread_id, 0, SPU_CRYPTO_CMD_AES128_CBC);

    /* Poll for completion with hardware timebase timeout */
    uint64_t t0 = get_timebase();
    while (*(volatile int32_t *)&g_cmd->status == SPU_CRYPTO_STATUS_BUSY) {
        asm volatile("sync" ::: "memory");
        if (get_timebase() - t0 > SPU_CRYPTO_TIMEOUT_TB) {
            printf("[SPU CRYPTO] AES-128 timeout on SPU!\n");
            sysMutexUnlock(g_crypto_mutex);
            return -ETIMEDOUT;
        }
    }

    /* Enforce memory barrier before reading DMA output buffer */
    asm volatile("sync" ::: "memory");

    g_last_elapsed_us = (float)g_cmd->elapsed_ticks / 79.8f;
    memcpy(out, g_bounce_out, len);

    /* Broadcast real-time telemetry on UDP 18196 */
    spu_crypto_send_telemetry("[SPU-CRYPTO] AES-128-CBC len=%zu ticks=%u elapsed=%.2f us\n",
                              len, g_cmd->elapsed_ticks, g_last_elapsed_us);

    sysMutexUnlock(g_crypto_mutex);
    return 0;
}

/**
 * Returns the hardware time (in microseconds) spent by the SPU
 * on the most recent crypto operation.
 */
float spu_crypto_get_last_elapsed_us(void) {
    return g_last_elapsed_us;
}
