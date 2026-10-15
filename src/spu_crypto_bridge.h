#ifndef SPU_CRYPTO_BRIDGE_H
#define SPU_CRYPTO_BRIDGE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize SPU Crypto accelerator.
 * Spawns a dedicated SPU worker thread running the spu_crypto kernel.
 * Returns 0 on success, negative error code on failure.
 */
int spu_crypto_init(void);

/**
 * Shut down the SPU Crypto worker thread and release resources.
 */
void spu_crypto_shutdown(void);

/**
 * Compute SHA-256 hash using the SPU accelerator.
 * @param in Input buffer (accessible to DMA)
 * @param len Byte length
 * @param out Output 32-byte hash buffer
 * @return 0 on success, negative on error
 */
int spu_crypto_sha256(const void *in, size_t len, uint8_t out[32]);

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
                                  const void *in, size_t len, void *out);

/**
 * Returns the hardware time (in microseconds) spent by the SPU
 * on the most recent crypto operation.
 */
float spu_crypto_get_last_elapsed_us(void);

#ifdef __cplusplus
}
#endif

#endif /* SPU_CRYPTO_BRIDGE_H */
