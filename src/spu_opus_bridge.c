/**
 * SPU Opus Accelerator Bridge for PS3-Moonlight
 *
 * Manages the SPU thread group and lifecycle of the spu_opus kernel.
 * Transfers Opus packets and decoded float PCM using 128-byte aligned DMA bounce buffers.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <errno.h>

#include <sys/spu.h>
#include <lv2/spu.h>
#include <sys/mutex.h>

#include "spu_opus_bridge.h"
#include "spu_opus.h"
#include "net_logger.h"
#include <ps3perf.h>

#define SPU_OPUS_BOUNCE_PCM_SIZE  (SPU_OPUS_MAX_SAMPLES * 2 * sizeof(float))
#define SPU_OPUS_TIMEOUT_TB       1600000ULL /* ~20 ms at 79.8 MHz Timebase */

/* Embedded raw SPU ELF binary exported by bin2s */
extern const uint8_t spu_opus_elf[];

static bool g_initialized = false;
static sys_mutex_t g_bridge_mutex;
static sysSpuImage g_spu_image;
static u32 g_group_id = 0;
static u32 g_thread_id = 0;

/* 128-byte cache-line aligned DMA bounce buffers in Main RAM */
static spu_opus_cmd_t *g_cmd = NULL;
static uint8_t *g_bounce_opus = NULL;
static float *g_bounce_pcm = NULL;

static float g_last_elapsed_us = 0.0f;

static inline uint64_t get_timebase(void) {
    uint64_t tb;
    asm volatile("mftb %0" : "=r"(tb));
    return tb;
}

int spu_opus_init(int sample_rate, int channels) {
    if (g_initialized) return 0;

    /* Initialize mutual exclusion lock for bridge calls */
    sys_mutex_attr_t mutex_attr;
    sysMutexAttrInitialize(mutex_attr);
    if (sysMutexCreate(&g_bridge_mutex, &mutex_attr) != 0) {
        printf("[SPU OPUS] Failed to create bridge mutex\n");
        return -1;
    }

    /* Allocate 128-byte aligned command and bounce buffers */
    g_cmd = (spu_opus_cmd_t *)memalign(128, sizeof(spu_opus_cmd_t));
    g_bounce_opus = (uint8_t *)memalign(128, SPU_OPUS_MAX_PACKET_LEN);
    g_bounce_pcm = (float *)memalign(128, SPU_OPUS_BOUNCE_PCM_SIZE);

    if (!g_cmd || !g_bounce_opus || !g_bounce_pcm) {
        printf("[SPU OPUS] Memory allocation failed for DMA bounce buffers\n");
        spu_opus_shutdown();
        return -ENOMEM;
    }

    memset(g_cmd, 0, sizeof(spu_opus_cmd_t));

    /* Initialize SPU subsystem (6 SPU threads, 0 raw SPUs) */
    int ret = sysSpuInitialize(6, 0);
    if (ret != 0 && ret != (int)0x80010003) {
        printf("[SPU OPUS] sysSpuInitialize returned 0x%08x\n", ret);
    }

    /* Import embedded SPU ELF image */
    ret = sysSpuImageImport(&g_spu_image, spu_opus_elf, 0);
    if (ret != 0) {
        printf("[SPU OPUS] sysSpuImageImport failed: 0x%08x\n", ret);
        spu_opus_shutdown();
        return ret;
    }

    /* Create SPU Thread Group */
    sysSpuThreadGroupAttribute grp_attr;
    sysSpuThreadGroupAttributeInitialize(grp_attr);
    sysSpuThreadGroupAttributeName(grp_attr, "spu_opus_grp");

    ret = sysSpuThreadGroupCreate(&g_group_id, 1, 100, &grp_attr);
    if (ret != 0) {
        printf("[SPU OPUS] sysSpuThreadGroupCreate failed: 0x%08x\n", ret);
        spu_opus_shutdown();
        return ret;
    }

    /* Initialize SPU Thread */
    sysSpuThreadAttribute thr_attr;
    sysSpuThreadAttributeInitialize(thr_attr);
    sysSpuThreadAttributeName(thr_attr, "spu_opus_thr");

    sysSpuThreadArgument thr_arg;
    sysSpuThreadArgumentInitialize(thr_arg);
    thr_arg.arg0 = (u64)(uintptr_t)g_cmd;

    ret = sysSpuThreadInitialize(&g_thread_id, g_group_id, 0, &g_spu_image, &thr_attr, &thr_arg);
    if (ret != 0) {
        printf("[SPU OPUS] sysSpuThreadInitialize failed: 0x%08x\n", ret);
        spu_opus_shutdown();
        return ret;
    }

    /* Register SPU worker thread with autonomous ps3perf profiler */
    ps3perf_register_spu(g_thread_id, spu_opus_elf);

    /* Configure signal registers to overwrite mode */
    sysSpuThreadSetConfiguration(g_thread_id, SPU_SIGNAL1_OVERWRITE | SPU_SIGNAL2_OVERWRITE);

    /* Start SPU Thread Group */
    ret = sysSpuThreadGroupStart(g_group_id);
    if (ret != 0) {
        printf("[SPU OPUS] sysSpuThreadGroupStart failed: 0x%08x\n", ret);
        spu_opus_shutdown();
        return ret;
    }

    /* Send INIT command to initialize SPU-side Opus decoder */
    g_cmd->cmd = SPU_OPUS_CMD_INIT;
    g_cmd->status = SPU_OPUS_STATUS_BUSY;
    g_cmd->sample_rate = sample_rate ? sample_rate : 48000;
    g_cmd->channels = channels ? channels : 2;
    g_cmd->ea_opus_in = (u64)(uintptr_t)g_bounce_opus;
    g_cmd->ea_pcm_out = (u64)(uintptr_t)g_bounce_pcm;

    asm volatile("sync" ::: "memory");
    sysSpuThreadWriteSignal(g_thread_id, 0, SPU_OPUS_CMD_INIT);

    uint64_t t0 = get_timebase();
    while (*(volatile int32_t *)&g_cmd->status == SPU_OPUS_STATUS_BUSY) {
        asm volatile("sync" ::: "memory");
        if (get_timebase() - t0 > SPU_OPUS_TIMEOUT_TB) {
            printf("[SPU OPUS] SPU Init timeout!\n");
            spu_opus_shutdown();
            return -ETIMEDOUT;
        }
    }

    g_initialized = true;
    printf("[SPU OPUS] SPU Opus Decoder initialized (Group: 0x%x, Thread: 0x%x)\n", g_group_id, g_thread_id);
    NLOG("SPU Opus Audio Decoder initialized on SPE (Group: 0x%x, Thread: 0x%x)", g_group_id, g_thread_id);
    return 0;
}

void spu_opus_shutdown(void) {
    if (g_initialized) {
        if (g_cmd) {
            g_cmd->cmd = SPU_OPUS_CMD_QUIT;
            asm volatile("sync" ::: "memory");
            sysSpuThreadWriteSignal(g_thread_id, 0, SPU_OPUS_CMD_QUIT);
        }

        /* Unregister SPU thread from profiler */
        ps3perf_unregister_spu(g_thread_id);

        u32 cause = 0, status = 0;
        sysSpuThreadGroupJoin(g_group_id, &cause, &status);
        sysSpuThreadGroupDestroy(g_group_id);
        sysSpuImageClose(&g_spu_image);

        g_initialized = false;
    }

    if (g_cmd) { free(g_cmd); g_cmd = NULL; }
    if (g_bounce_opus) { free(g_bounce_opus); g_bounce_opus = NULL; }
    if (g_bounce_pcm) { free(g_bounce_pcm); g_bounce_pcm = NULL; }

    if (g_bridge_mutex) {
        sysMutexDestroy(g_bridge_mutex);
        g_bridge_mutex = 0;
    }
}

int spu_opus_decode_float(const void *opus_data, int opus_len,
                          float *out_pcm, int max_samples, int *out_samples) {
    if (!g_initialized || !out_pcm || !out_samples) {
        return -EINVAL;
    }

    sysMutexLock(g_bridge_mutex, 0);

    int in_len = (opus_data && opus_len > 0) ? opus_len : 0;
    if (in_len > SPU_OPUS_MAX_PACKET_LEN) in_len = SPU_OPUS_MAX_PACKET_LEN;

    if (in_len > 0) {
        memcpy(g_bounce_opus, opus_data, in_len);
    }

    g_cmd->cmd = SPU_OPUS_CMD_DECODE;
    g_cmd->status = SPU_OPUS_STATUS_BUSY;
    g_cmd->opus_len = (uint32_t)in_len;
    g_cmd->ea_opus_in = (u64)(uintptr_t)g_bounce_opus;
    g_cmd->ea_pcm_out = (u64)(uintptr_t)g_bounce_pcm;

    asm volatile("sync" ::: "memory");
    sysSpuThreadWriteSignal(g_thread_id, 0, SPU_OPUS_CMD_DECODE);

    uint64_t t0 = get_timebase();
    while (*(volatile int32_t *)&g_cmd->status == SPU_OPUS_STATUS_BUSY) {
        asm volatile("sync" ::: "memory");
        if (get_timebase() - t0 > SPU_OPUS_TIMEOUT_TB) {
            printf("[SPU OPUS] SPU Decode timeout!\n");
            sysMutexUnlock(g_bridge_mutex);
            return -ETIMEDOUT;
        }
    }

    asm volatile("sync" ::: "memory");

    int decoded = (int)g_cmd->decoded_samples;
    if (decoded > max_samples) decoded = max_samples;

    if (decoded > 0) {
        memcpy(out_pcm, g_bounce_pcm, decoded * 2 * sizeof(float));
    }

    *out_samples = decoded;
    g_last_elapsed_us = (float)g_cmd->elapsed_ticks / 79.8f;

    sysMutexUnlock(g_bridge_mutex);
    return (g_cmd->status == SPU_OPUS_STATUS_OK) ? 0 : -1;
}

bool spu_opus_is_active(void) {
    return g_initialized;
}

float spu_opus_get_last_elapsed_us(void) {
    return g_last_elapsed_us;
}
