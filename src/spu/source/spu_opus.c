/**
 * SPU Opus Audio Decoder Kernel for PS3-Moonlight
 *
 * Runs on a dedicated Synergistic Processing Element (SPE) core.
 * Performs real-time Opus packet decoding and 16-bit to 32-bit float PCM
 * conversion, eliminating ~350 us per-frame CPU latency and mutex locks.
 *
 * All memory transfers with Main RAM use 128-byte aligned MFC DMA.
 */

#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <stdint.h>
#include <string.h>

#include "spu_opus.h"
#include "opus_multistream.h"

#define DECODER_STATE_BUF_SIZE 22528 /* 22 KB pre-allocated decoder state */
#define MAX_CHANNELS           2
#define SPU_DECODE_TAG         1
#define SPU_SAMPLES_FRAME      SPU_OPUS_MAX_SAMPLES   /* Up to 20ms frame at 48kHz (960 samples) */

/* Static Local Store buffers aligned to 128 bytes */
static spu_opus_cmd_t g_cmd __attribute__((aligned(128)));
static uint8_t ls_opus_in[SPU_OPUS_MAX_PACKET_LEN] __attribute__((aligned(128)));
static int16_t ls_pcm_short[SPU_SAMPLES_FRAME * MAX_CHANNELS] __attribute__((aligned(128)));
static float   ls_pcm_float[SPU_SAMPLES_FRAME * MAX_CHANNELS] __attribute__((aligned(128)));
static uint8_t ls_decoder_state[DECODER_STATE_BUF_SIZE] __attribute__((aligned(128)));

static OpusMSDecoder *g_ms_decoder = NULL;

static inline uint32_t read_decrementer(void) {
    return spu_readch(13); /* SPU Channel 13: Decrementer */
}


/**
 * @brief Initializes the Opus multistream decoder state in SPU Local Store.
 *
 * @param sample_rate Audio sample rate in Hz (e.g., 48000).
 * @param channels Number of audio channels (e.g., 2 for stereo).
 * @return OPUS_OK on success, negative error code otherwise.
 * Algorithmic Complexity: O(1) fixed initialization overhead.
 */
static int spu_opus_init_decoder(uint32_t sample_rate, uint32_t channels) __attribute__((noinline));
static int spu_opus_init_decoder(uint32_t sample_rate, uint32_t channels) {
    unsigned char mapping[2] = {0, 1};
    g_ms_decoder = (OpusMSDecoder *)ls_decoder_state;
    return opus_multistream_decoder_init(
        g_ms_decoder,
        sample_rate ? sample_rate : 48000,
        channels ? channels : 2,
        1, /* 1 stream */
        1, /* 1 coupled stream (stereo) */
        mapping
    );
}

/**
 * @brief Converts 16-bit signed integer PCM samples to 32-bit normalized IEEE-754 floats.
 *
 * Converts range [-32768, 32767] to [-1.0f, 1.0f] for the PS3 hardware audio mixer.
 *
 * @param src Pointer to input int16 PCM buffer in SPU Local Store.
 * @param dst Pointer to output float PCM buffer in SPU Local Store.
 * @param count Total number of scalar samples to convert.
 * Algorithmic Complexity: O(N) linear pass over PCM samples.
 */
static void spu_opus_pcm16_to_float(const int16_t *src, float *dst, int count) __attribute__((noinline));
static void spu_opus_pcm16_to_float(const int16_t *src, float *dst, int count) {
    const float scale = 1.0f / 32768.0f;
    for (int i = 0; i < count; i++) {
        dst[i] = (float)src[i] * scale;
    }
}

/**
 * @brief Performs DMA transfers, Opus decoding, and float conversion for an audio packet.
 *
 * Dispatches DMA GET for incoming Opus packet from Main RAM, calls the Opus decoder,
 * converts int16 PCM to float PCM, and dispatches DMA PUT to write results back to Main RAM.
 *
 * @param cmd Pointer to the command descriptor residing in SPU Local Store.
 * @return 0 on success, negative error code on failure.
 * Algorithmic Complexity: O(M) bounded by Opus frame decode complexity.
 */
static int spu_opus_decode_frame(spu_opus_cmd_t *cmd) __attribute__((noinline));
static int spu_opus_decode_frame(spu_opus_cmd_t *cmd) {
    if (!g_ms_decoder) {
        int init_ret = spu_opus_init_decoder(48000, 2);
        if (init_ret != OPUS_OK) {
            cmd->decoded_samples = 0;
            cmd->status = SPU_OPUS_STATUS_ERR;
            return init_ret;
        }
    }

    int in_bytes = (int)cmd->opus_len;
    if (in_bytes > SPU_OPUS_MAX_PACKET_LEN) {
        in_bytes = SPU_OPUS_MAX_PACKET_LEN;
    }

    /* DMA Opus packet into SPU Local Store if present */
    if (in_bytes > 0 && cmd->ea_opus_in != 0) {
        /* DMA transfer length must be aligned to 16 bytes for MFC hardware */
        uint32_t dma_len = (in_bytes + 15) & ~15;
        mfc_get(ls_opus_in, cmd->ea_opus_in, dma_len, SPU_DECODE_TAG, 0, 0);
        mfc_write_tag_mask(1 << SPU_DECODE_TAG);
        mfc_read_tag_status_all();
    }

    /* Decode Opus packet to 16-bit integer PCM */
    int decoded = opus_multistream_decode(
        g_ms_decoder,
        (in_bytes > 0) ? ls_opus_in : NULL,
        in_bytes,
        ls_pcm_short,
        SPU_SAMPLES_FRAME,
        0 /* decode_fec = 0 */
    );

    if (decoded > 0) {
        int total_samples = decoded * 2; /* Stereo */
        if (total_samples > SPU_SAMPLES_FRAME * MAX_CHANNELS) {
            total_samples = SPU_SAMPLES_FRAME * MAX_CHANNELS;
        }

        /* Fast vectorized / scalar conversion from int16 to normalized float */
        spu_opus_pcm16_to_float(ls_pcm_short, ls_pcm_float, total_samples);

        /* DMA decoded float PCM samples back to Main RAM */
        uint32_t out_bytes = total_samples * sizeof(float);
        uint32_t dma_out_bytes = (out_bytes + 15) & ~15;
        mfc_put(ls_pcm_float, cmd->ea_pcm_out, dma_out_bytes, SPU_DECODE_TAG, 0, 0);
        mfc_write_tag_mask(1 << SPU_DECODE_TAG);
        mfc_read_tag_status_all();

        cmd->decoded_samples = (uint32_t)decoded;
        cmd->status = SPU_OPUS_STATUS_OK;
        return 0;
    } else {
        cmd->decoded_samples = 0;
        cmd->status = SPU_OPUS_STATUS_ERR;
        return -1;
    }
}

int main(unsigned long long spu_id, unsigned long long arg0, unsigned long long arg1) {
    (void)spu_id;
    (void)arg1;

    uint64_t ea_cmd = (uint64_t)arg0;

    while (1) {
        /* Block waiting for execution signal from Signal 1 register */
        spu_read_signal1();

        /* Fetch command block from Main RAM */
        mfc_get(&g_cmd, ea_cmd, sizeof(spu_opus_cmd_t), SPU_DECODE_TAG, 0, 0);
        mfc_write_tag_mask(1 << SPU_DECODE_TAG);
        mfc_read_tag_status_all();

        if (g_cmd.cmd == SPU_OPUS_CMD_QUIT) {
            g_cmd.status = SPU_OPUS_STATUS_OK;
            mfc_put(&g_cmd, ea_cmd, sizeof(spu_opus_cmd_t), SPU_DECODE_TAG, 0, 0);
            mfc_write_tag_mask(1 << SPU_DECODE_TAG);
            mfc_read_tag_status_all();
            break;
        }

        uint32_t t_start = read_decrementer();

        if (g_cmd.cmd == SPU_OPUS_CMD_INIT) {
            int ret = spu_opus_init_decoder(g_cmd.sample_rate, g_cmd.channels);
            g_cmd.status = (ret == OPUS_OK) ? SPU_OPUS_STATUS_OK : SPU_OPUS_STATUS_ERR;
            g_cmd.decoded_samples = 0;
        }
        else if (g_cmd.cmd == SPU_OPUS_CMD_DECODE) {
            spu_opus_decode_frame(&g_cmd);
        } else {
            g_cmd.status = SPU_OPUS_STATUS_OK;
        }

        uint32_t t_end = read_decrementer();
        g_cmd.elapsed_ticks = t_start - t_end;

        /* Write updated command block back to Main RAM */
        mfc_put(&g_cmd, ea_cmd, sizeof(spu_opus_cmd_t), SPU_DECODE_TAG, 0, 0);
        mfc_write_tag_mask(1 << SPU_DECODE_TAG);
        mfc_read_tag_status_all();
    }

    return 0;
}
