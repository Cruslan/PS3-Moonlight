#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <sys/spu_thread.h>
#include <stdint.h>
#include <string.h>

#include "spu_crypto.h"

#define MAX_CHUNK_SIZE 16384 /* 16 KB max per DMA chunk in Local Store */

/* Memory-aligned Local Store staging buffers */
static spu_crypto_cmd_t g_cmd __attribute__((aligned(128)));
static uint8_t g_buffer_in[MAX_CHUNK_SIZE] __attribute__((aligned(128)));
static uint8_t g_buffer_out[MAX_CHUNK_SIZE] __attribute__((aligned(128)));
static uint8_t g_key[32] __attribute__((aligned(16)));
static uint8_t g_iv[16] __attribute__((aligned(16)));

static inline void wait_dma(uint32_t tag) {
    mfc_write_tag_mask(1 << tag);
    spu_mfcstat(MFC_TAG_UPDATE_ALL);
}

/* ========================================================================= */
/* Verified Standalone SHA-256 Engine                                        */
/* ========================================================================= */

#define GET_UINT32_BE(n, b, i)                          \
{                                                       \
    (n) = ( (uint32_t) (b)[(i)    ] << 24 )             \
        | ( (uint32_t) (b)[(i) + 1] << 16 )             \
        | ( (uint32_t) (b)[(i) + 2] <<  8 )             \
        | ( (uint32_t) (b)[(i) + 3]       );            \
}

#define PUT_UINT32_BE(n, b, i)                          \
{                                                       \
    (b)[(i)    ] = (uint8_t) ( (n) >> 24 );             \
    (b)[(i) + 1] = (uint8_t) ( (n) >> 16 );             \
    (b)[(i) + 2] = (uint8_t) ( (n) >>  8 );             \
    (b)[(i) + 3] = (uint8_t) ( (n)       );             \
}

#define SHR(x, n)  (((x) & 0xFFFFFFFFU) >> (n))
#define ROTR(x, n) (SHR(x, n) | ((x) << (32 - (n))))

#define S0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ SHR(x, 3))
#define S1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ SHR(x, 10))
#define S2(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define S3(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))

#define F0(x, y, z) (((x) & (y)) | ((z) & ((x) | (y))))
#define F1(x, y, z) ((z) ^ ((x) & ((y) ^ (z))))

#define R(t)                                    \
(                                               \
    W[t] = S1(W[t -  2]) + W[t -  7] +          \
           S0(W[t - 15]) + W[t - 16]            \
)

#define P(a, b, c, d, e, f, g, h, x, K)         \
{                                               \
    temp1 = h + S3(e) + F1(e, f, g) + K + x;    \
    temp2 = S2(a) + F0(a, b, c);                \
    d += temp1; h = temp1 + temp2;              \
}

typedef struct {
    uint32_t total[2];
    uint32_t state[8];
    uint8_t  buffer[64];
} spu_sha256_ctx_t;

static void spu_sha256_process(spu_sha256_ctx_t *ctx, const uint8_t data[64]) {
    uint32_t temp1, temp2, W[64];
    uint32_t A, B, C, D, E, F, G, H;

    GET_UINT32_BE( W[ 0], data,  0 );
    GET_UINT32_BE( W[ 1], data,  4 );
    GET_UINT32_BE( W[ 2], data,  8 );
    GET_UINT32_BE( W[ 3], data, 12 );
    GET_UINT32_BE( W[ 4], data, 16 );
    GET_UINT32_BE( W[ 5], data, 20 );
    GET_UINT32_BE( W[ 6], data, 24 );
    GET_UINT32_BE( W[ 7], data, 28 );
    GET_UINT32_BE( W[ 8], data, 32 );
    GET_UINT32_BE( W[ 9], data, 36 );
    GET_UINT32_BE( W[10], data, 40 );
    GET_UINT32_BE( W[11], data, 44 );
    GET_UINT32_BE( W[12], data, 48 );
    GET_UINT32_BE( W[13], data, 52 );
    GET_UINT32_BE( W[14], data, 56 );
    GET_UINT32_BE( W[15], data, 60 );

    A = ctx->state[0];
    B = ctx->state[1];
    C = ctx->state[2];
    D = ctx->state[3];
    E = ctx->state[4];
    F = ctx->state[5];
    G = ctx->state[6];
    H = ctx->state[7];

    P( A, B, C, D, E, F, G, H, W[ 0], 0x428A2F98 );
    P( H, A, B, C, D, E, F, G, W[ 1], 0x71374491 );
    P( G, H, A, B, C, D, E, F, W[ 2], 0xB5C0FBCF );
    P( F, G, H, A, B, C, D, E, W[ 3], 0xE9B5DBA5 );
    P( E, F, G, H, A, B, C, D, W[ 4], 0x3956C25B );
    P( D, E, F, G, H, A, B, C, W[ 5], 0x59F111F1 );
    P( C, D, E, F, G, H, A, B, W[ 6], 0x923F82A4 );
    P( B, C, D, E, F, G, H, A, W[ 7], 0xAB1C5ED5 );
    P( A, B, C, D, E, F, G, H, W[ 8], 0xD807AA98 );
    P( H, A, B, C, D, E, F, G, W[ 9], 0x12835B01 );
    P( G, H, A, B, C, D, E, F, W[10], 0x243185BE );
    P( F, G, H, A, B, C, D, E, W[11], 0x550C7DC3 );
    P( E, F, G, H, A, B, C, D, W[12], 0x72BE5D74 );
    P( D, E, F, G, H, A, B, C, W[13], 0x80DEB1FE );
    P( C, D, E, F, G, H, A, B, W[14], 0x9BDC06A7 );
    P( B, C, D, E, F, G, H, A, W[15], 0xC19BF174 );
    P( A, B, C, D, E, F, G, H, R(16), 0xE49B69C1 );
    P( H, A, B, C, D, E, F, G, R(17), 0xEFBE4786 );
    P( G, H, A, B, C, D, E, F, R(18), 0x0FC19DC6 );
    P( F, G, H, A, B, C, D, E, R(19), 0x240CA1CC );
    P( E, F, G, H, A, B, C, D, R(20), 0x2DE92C6F );
    P( D, E, F, G, H, A, B, C, R(21), 0x4A7484AA );
    P( C, D, E, F, G, H, A, B, R(22), 0x5CB0A9DC );
    P( B, C, D, E, F, G, H, A, R(23), 0x76F988DA );
    P( A, B, C, D, E, F, G, H, R(24), 0x983E5152 );
    P( H, A, B, C, D, E, F, G, R(25), 0xA831C66D );
    P( G, H, A, B, C, D, E, F, R(26), 0xB00327C8 );
    P( F, G, H, A, B, C, D, E, R(27), 0xBF597FC7 );
    P( E, F, G, H, A, B, C, D, R(28), 0xC6E00BF3 );
    P( D, E, F, G, H, A, B, C, R(29), 0xD5A79147 );
    P( C, D, E, F, G, H, A, B, R(30), 0x06CA6351 );
    P( B, C, D, E, F, G, H, A, R(31), 0x14292967 );
    P( A, B, C, D, E, F, G, H, R(32), 0x27B70A85 );
    P( H, A, B, C, D, E, F, G, R(33), 0x2E1B2138 );
    P( G, H, A, B, C, D, E, F, R(34), 0x4D2C6DFC );
    P( F, G, H, A, B, C, D, E, R(35), 0x53380D13 );
    P( E, F, G, H, A, B, C, D, R(36), 0x650A7354 );
    P( D, E, F, G, H, A, B, C, R(37), 0x766A0ABB );
    P( C, D, E, F, G, H, A, B, R(38), 0x81C2C92E );
    P( B, C, D, E, F, G, H, A, R(39), 0x92722C85 );
    P( A, B, C, D, E, F, G, H, R(40), 0xA2BFE8A1 );
    P( H, A, B, C, D, E, F, G, R(41), 0xA81A664B );
    P( G, H, A, B, C, D, E, F, R(42), 0xC24B8B70 );
    P( F, G, H, A, B, C, D, E, R(43), 0xC76C51A3 );
    P( E, F, G, H, A, B, C, D, R(44), 0xD192E819 );
    P( D, E, F, G, H, A, B, C, R(45), 0xD6990624 );
    P( C, D, E, F, G, H, A, B, R(46), 0xF40E3585 );
    P( B, C, D, E, F, G, H, A, R(47), 0x106AA070 );
    P( A, B, C, D, E, F, G, H, R(48), 0x19A4C116 );
    P( H, A, B, C, D, E, F, G, R(49), 0x1E376C08 );
    P( G, H, A, B, C, D, E, F, R(50), 0x2748774C );
    P( F, G, H, A, B, C, D, E, R(51), 0x34B0BCB5 );
    P( E, F, G, H, A, B, C, D, R(52), 0x391C0CB3 );
    P( D, E, F, G, H, A, B, C, R(53), 0x4ED8AA4A );
    P( C, D, E, F, G, H, A, B, R(54), 0x5B9CCA4F );
    P( B, C, D, E, F, G, H, A, R(55), 0x682E6FF3 );
    P( A, B, C, D, E, F, G, H, R(56), 0x748F82EE );
    P( H, A, B, C, D, E, F, G, R(57), 0x78A5636F );
    P( G, H, A, B, C, D, E, F, R(58), 0x84C87814 );
    P( F, G, H, A, B, C, D, E, R(59), 0x8CC70208 );
    P( E, F, G, H, A, B, C, D, R(60), 0x90BEFFFA );
    P( D, E, F, G, H, A, B, C, R(61), 0xA4506CEB );
    P( C, D, E, F, G, H, A, B, R(62), 0xBEF9A3F7 );
    P( B, C, D, E, F, G, H, A, R(63), 0xC67178F2 );

    ctx->state[0] += A;
    ctx->state[1] += B;
    ctx->state[2] += C;
    ctx->state[3] += D;
    ctx->state[4] += E;
    ctx->state[5] += F;
    ctx->state[6] += G;
    ctx->state[7] += H;
}

static void spu_sha256_starts(spu_sha256_ctx_t *ctx) {
    ctx->total[0] = 0;
    ctx->total[1] = 0;
    ctx->state[0] = 0x6A09E667;
    ctx->state[1] = 0xBB67AE85;
    ctx->state[2] = 0x3C6EF372;
    ctx->state[3] = 0xA54FF53A;
    ctx->state[4] = 0x510E527F;
    ctx->state[5] = 0x9B05688C;
    ctx->state[6] = 0x1F83D9AB;
    ctx->state[7] = 0x5BE0CD19;
}

static void spu_sha256_update(spu_sha256_ctx_t *ctx, const uint8_t *input, size_t ilen) {
    size_t fill;
    uint32_t left;

    if (ilen == 0) return;

    left = ctx->total[0] & 0x3F;
    fill = 64 - left;

    ctx->total[0] += (uint32_t)ilen;
    if (ctx->total[0] < (uint32_t)ilen)
        ctx->total[1]++;

    if (left && ilen >= fill) {
        memcpy(ctx->buffer + left, input, fill);
        spu_sha256_process(ctx, ctx->buffer);
        input += fill;
        ilen  -= fill;
        left = 0;
    }

    while (ilen >= 64) {
        spu_sha256_process(ctx, input);
        input += 64;
        ilen  -= 64;
    }

    if (ilen > 0) {
        memcpy(ctx->buffer + left, input, ilen);
    }
}

static const uint8_t sha256_padding[64] = {
    0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static void spu_sha256_finish(spu_sha256_ctx_t *ctx, uint8_t output[32]) {
    uint32_t last, padn;
    uint32_t high, low;
    uint8_t msglen[8];

    high = (ctx->total[0] >> 29) | (ctx->total[1] << 3);
    low  = (ctx->total[0] << 3);

    PUT_UINT32_BE(high, msglen, 0);
    PUT_UINT32_BE(low,  msglen, 4);

    last = ctx->total[0] & 0x3F;
    padn = (last < 56) ? (56 - last) : (120 - last);

    spu_sha256_update(ctx, sha256_padding, padn);
    spu_sha256_update(ctx, msglen, 8);

    PUT_UINT32_BE(ctx->state[0], output,  0);
    PUT_UINT32_BE(ctx->state[1], output,  4);
    PUT_UINT32_BE(ctx->state[2], output,  8);
    PUT_UINT32_BE(ctx->state[3], output, 12);
    PUT_UINT32_BE(ctx->state[4], output, 16);
    PUT_UINT32_BE(ctx->state[5], output, 20);
    PUT_UINT32_BE(ctx->state[6], output, 24);
    PUT_UINT32_BE(ctx->state[7], output, 28);
}

/* ========================================================================= */
/* Verified SPU AES-128 Engine                                               */
/* ========================================================================= */

static const uint8_t sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5e,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static const uint8_t rsbox[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

static const uint8_t Rcon[11] = {
    0x8d, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36
};

static uint8_t round_keys[176];

static void spu_aes128_key_expansion(const uint8_t key[16]) {
    uint8_t temp[4];
    int i, k;

    for (i = 0; i < 16; i++) round_keys[i] = key[i];

    for (i = 16; i < 176; i += 4) {
        for (k = 0; k < 4; k++) temp[k] = round_keys[i - 4 + k];

        if (i % 16 == 0) {
            uint8_t u = temp[0];
            temp[0] = sbox[temp[1]] ^ Rcon[i / 16];
            temp[1] = sbox[temp[2]];
            temp[2] = sbox[temp[3]];
            temp[3] = sbox[u];
        }

        for (k = 0; k < 4; k++)
            round_keys[i + k] = round_keys[i - 16 + k] ^ temp[k];
    }
}

static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    while (a && b) {
        if (b & 1) p ^= a;
        if (a & 0x80) a = (a << 1) ^ 0x1b;
        else a <<= 1;
        b >>= 1;
    }
    return p;
}

static void spu_aes128_decrypt_block(const uint8_t in[16], uint8_t out[16]) {
    uint8_t state[16];
    int r, i;

    for (i = 0; i < 16; i++) state[i] = in[i] ^ round_keys[160 + i];

    for (r = 9; r > 0; r--) {
        uint8_t temp[16];
        temp[0] = state[0];   temp[1] = state[13]; temp[2] = state[10]; temp[3] = state[7];
        temp[4] = state[4];   temp[5] = state[1];  temp[6] = state[14]; temp[7] = state[11];
        temp[8] = state[8];   temp[9] = state[5];  temp[10]= state[2];  temp[11]= state[15];
        temp[12]= state[12];  temp[13]= state[9];  temp[14]= state[6];  temp[15]= state[3];

        for (i = 0; i < 16; i++) temp[i] = rsbox[temp[i]];

        for (i = 0; i < 16; i++) temp[i] ^= round_keys[r * 16 + i];

        for (i = 0; i < 4; i++) {
            uint8_t a = temp[i*4], b = temp[i*4+1], c = temp[i*4+2], d = temp[i*4+3];
            state[i*4]   = gmul(a, 0x0e) ^ gmul(b, 0x0b) ^ gmul(c, 0x0d) ^ gmul(d, 0x09);
            state[i*4+1] = gmul(a, 0x09) ^ gmul(b, 0x0e) ^ gmul(c, 0x0b) ^ gmul(d, 0x0d);
            state[i*4+2] = gmul(a, 0x0d) ^ gmul(b, 0x09) ^ gmul(c, 0x0e) ^ gmul(d, 0x0b);
            state[i*4+3] = gmul(a, 0x0b) ^ gmul(b, 0x0d) ^ gmul(c, 0x09) ^ gmul(d, 0x0e);
        }
    }

    uint8_t temp[16];
    temp[0] = state[0];   temp[1] = state[13]; temp[2] = state[10]; temp[3] = state[7];
    temp[4] = state[4];   temp[5] = state[1];  temp[6] = state[14]; temp[7] = state[11];
    temp[8] = state[8];   temp[9] = state[5];  temp[10]= state[2];  temp[11]= state[15];
    temp[12]= state[12];  temp[13]= state[9];  temp[14]= state[6];  temp[15]= state[3];

    for (i = 0; i < 16; i++) out[i] = rsbox[temp[i]] ^ round_keys[i];
}

/* ========================================================================= */
/* SPU Main Dispatcher Loop                                                  */
/* ========================================================================= */

int main(uint64_t ea_cmd, uint64_t arg2, uint64_t arg3, uint64_t arg4) {
    (void)arg2; (void)arg3; (void)arg4;

    while (1) {
        /* Blocking wait on PPU Signal 1 */
        uint32_t sig = spu_read_signal1();
        if (sig == SPU_CRYPTO_CMD_QUIT) {
            break;
        }

        uint32_t t_start = spu_read_decrementer();

        /* Fetch 128-byte command packet via DMA */
        mfc_get(&g_cmd, ea_cmd, sizeof(spu_crypto_cmd_t), SPU_CRYPTO_TAG, 0, 0);
        wait_dma(SPU_CRYPTO_TAG);

        if (g_cmd.cmd == SPU_CRYPTO_CMD_SHA256) {
            spu_sha256_ctx_t ctx;
            spu_sha256_starts(&ctx);

            uint32_t remaining = g_cmd.data_len;
            uint64_t ea_curr = g_cmd.ea_input;

            while (remaining > 0) {
                uint32_t chunk = (remaining > MAX_CHUNK_SIZE) ? MAX_CHUNK_SIZE : remaining;
                uint32_t dma_size = (chunk + 15) & ~15; /* 16-byte aligned DMA transfer */

                mfc_get(g_buffer_in, ea_curr, dma_size, SPU_CRYPTO_TAG, 0, 0);
                wait_dma(SPU_CRYPTO_TAG);

                spu_sha256_update(&ctx, g_buffer_in, chunk);
                ea_curr += chunk;
                remaining -= chunk;
            }

            uint8_t hash[32] __attribute__((aligned(16)));
            spu_sha256_finish(&ctx, hash);

            /* Write 32-byte digest back to PPU */
            mfc_put(hash, g_cmd.ea_output, 32, SPU_CRYPTO_TAG, 0, 0);
            wait_dma(SPU_CRYPTO_TAG);
        }
        else if (g_cmd.cmd == SPU_CRYPTO_CMD_AES128_CBC) {
            /* DMA key (16 bytes) and IV (16 bytes) */
            mfc_get(g_key, g_cmd.ea_key, 16, SPU_CRYPTO_TAG, 0, 0);
            mfc_get(g_iv, g_cmd.ea_iv, 16, SPU_CRYPTO_TAG, 0, 0);
            wait_dma(SPU_CRYPTO_TAG);

            spu_aes128_key_expansion(g_key);

            uint32_t remaining = g_cmd.data_len;
            uint64_t ea_in = g_cmd.ea_input;
            uint64_t ea_out = g_cmd.ea_output;

            uint8_t prev_block[16];
            memcpy(prev_block, g_iv, 16);

            while (remaining > 0) {
                uint32_t chunk = (remaining > MAX_CHUNK_SIZE) ? MAX_CHUNK_SIZE : remaining;
                uint32_t dma_size = (chunk + 15) & ~15;

                mfc_get(g_buffer_in, ea_in, dma_size, SPU_CRYPTO_TAG, 0, 0);
                wait_dma(SPU_CRYPTO_TAG);

                uint32_t chunk_blocks = chunk / 16;
                for (uint32_t b = 0; b < chunk_blocks; ++b) {
                    uint8_t *ct = &g_buffer_in[b * 16];
                    uint8_t *pt = &g_buffer_out[b * 16];
                    uint8_t next_prev[16];
                    memcpy(next_prev, ct, 16);

                    spu_aes128_decrypt_block(ct, pt);

                    for (int i = 0; i < 16; i++) pt[i] ^= prev_block[i];
                    memcpy(prev_block, next_prev, 16);
                }

                mfc_put(g_buffer_out, ea_out, dma_size, SPU_CRYPTO_TAG, 0, 0);
                wait_dma(SPU_CRYPTO_TAG);

                ea_in += chunk;
                ea_out += chunk;
                remaining -= chunk;
            }
        }

        uint32_t t_end = spu_read_decrementer();
        g_cmd.elapsed_ticks = t_start - t_end;
        g_cmd.status = SPU_CRYPTO_STATUS_OK;

        /* Write updated command back to PPU */
        mfc_put(&g_cmd, ea_cmd, sizeof(spu_crypto_cmd_t), SPU_CRYPTO_TAG, 0, 0);
        wait_dma(SPU_CRYPTO_TAG);
    }

    spu_thread_exit(0);
    return 0;
}
