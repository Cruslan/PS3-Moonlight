/**
 * SPU Opus Accelerator Bridge for PS3-Moonlight
 *
 * Dispatches Opus decode requests to a dedicated Cell SPU worker thread.
 * All memory transfers use 128-byte aligned DMA bounce buffers and SPU
 * Signal Notification registers for zero-overhead host offloading.
 */

#ifndef SPU_OPUS_BRIDGE_H
#define SPU_OPUS_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize SPU Opus accelerator.
 * Spawns a dedicated SPU worker thread running the spu_opus kernel.
 * Returns 0 on success, negative error code on failure.
 */
int spu_opus_init(int sample_rate, int channels);

/**
 * Shut down the SPU Opus worker thread and release resources.
 */
void spu_opus_shutdown(void);

/**
 * Decode an Opus packet on the SPU accelerator into float PCM.
 * @param opus_data Pointer to Opus packet bytes (NULL for packet loss concealment)
 * @param opus_len Length of packet in bytes
 * @param out_pcm Pointer to destination float buffer
 * @param max_samples Maximum stereo samples to decode
 * @param out_samples Output number of stereo samples decoded
 * @return 0 on success, negative on error
 */
int spu_opus_decode_float(const void *opus_data, int opus_len,
                          float *out_pcm, int max_samples, int *out_samples);

/**
 * Query whether the SPU Opus worker is initialized and active.
 */
bool spu_opus_is_active(void);

/**
 * Return elapsed execution time on SPU (in microseconds) for last decode.
 */
float spu_opus_get_last_elapsed_us(void);

#ifdef __cplusplus
}
#endif

#endif /* SPU_OPUS_BRIDGE_H */
