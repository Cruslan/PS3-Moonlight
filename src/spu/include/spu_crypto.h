#ifndef SPU_CRYPTO_H
#define SPU_CRYPTO_H

#include <stdint.h>

#define SPU_CRYPTO_CMD_NOP         0
#define SPU_CRYPTO_CMD_QUIT        1
#define SPU_CRYPTO_CMD_SHA256      2
#define SPU_CRYPTO_CMD_AES128_ECB  3
#define SPU_CRYPTO_CMD_AES128_CBC  4
#define SPU_CRYPTO_CMD_AES128_GCM  5

#define SPU_CRYPTO_STATUS_BUSY     0
#define SPU_CRYPTO_STATUS_OK       1
#define SPU_CRYPTO_STATUS_ERR      -1

#define SPU_CRYPTO_TAG             1

/**
 * Shared command packet between PPU and SPU.
 * Must be strictly 128-byte aligned for Cell DMA transfers.
 */
typedef struct __attribute__((aligned(128))) {
    uint32_t cmd;           /* SPU_CRYPTO_CMD_* */
    int32_t  status;        /* SPU_CRYPTO_STATUS_* */
    uint32_t data_len;      /* Byte count of payload */
    uint32_t aux_len;       /* Auxiliary length (e.g. IV or tag length) */
    uint64_t ea_input;      /* PPU Main RAM address of input data */
    uint64_t ea_output;     /* PPU Main RAM address of output data */
    uint64_t ea_key;        /* PPU Main RAM address of 16-byte AES key */
    uint64_t ea_iv;         /* PPU Main RAM address of IV (12 or 16 bytes) */
    uint64_t ea_tag;        /* PPU Main RAM address of GCM tag (16 bytes) */
    uint32_t elapsed_ticks; /* SPU execution time in hardware decrementer ticks */
    uint32_t reserved;
    uint8_t  padding[64];   /* Explicitly padded to exactly 128 bytes */
} spu_crypto_cmd_t;

#endif /* SPU_CRYPTO_H */
