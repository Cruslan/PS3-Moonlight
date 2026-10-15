/**
 * SPU Opus Protocol Definitions for PS3-Moonlight
 *
 * Defines communication structures and command codes between audio renderer
 * and the dedicated SPU Opus decoding worker kernel.
 */

#ifndef SPU_OPUS_H
#define SPU_OPUS_H

#include <stdint.h>

#define SPU_OPUS_CMD_NOP         0
#define SPU_OPUS_CMD_QUIT        1
#define SPU_OPUS_CMD_INIT        2
#define SPU_OPUS_CMD_DECODE      3
#define SPU_OPUS_CMD_RESET       4

#define SPU_OPUS_STATUS_IDLE     0
#define SPU_OPUS_STATUS_BUSY     1
#define SPU_OPUS_STATUS_OK       2
#define SPU_OPUS_STATUS_ERR     -1

#define SPU_OPUS_TAG             2

#define SPU_OPUS_MAX_PACKET_LEN  1024
#define SPU_OPUS_MAX_SAMPLES     960   /* Up to 20ms at 48kHz stereo */

/**
 * 128-byte aligned command block shared between host and SPU via MFC DMA.
 */
typedef struct __attribute__((aligned(128))) {
    uint32_t cmd;             /* SPU_OPUS_CMD_* */
    int32_t  status;          /* SPU_OPUS_STATUS_* */
    uint32_t sample_rate;     /* Typically 48000 */
    uint32_t channels;        /* Typically 2 (Stereo) */
    uint32_t opus_len;        /* Length of incoming Opus packet in bytes (0 for PLC) */
    uint32_t decoded_samples; /* Number of stereo sample pairs decoded */
    uint32_t elapsed_ticks;   /* Hardware decrementer execution ticks */
    uint32_t reserved;
    uint64_t ea_opus_in;      /* Effective Address of input Opus payload in Main RAM */
    uint64_t ea_pcm_out;      /* Effective Address of output float PCM in Main RAM */
    uint8_t  padding[64];     /* Strictly pad to 128 bytes */
} spu_opus_cmd_t;

#endif /* SPU_OPUS_H */
