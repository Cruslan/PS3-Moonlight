#ifndef OPENSSL_COMPAT_H
#define OPENSSL_COMPAT_H

#include <polarssl/sha1.h>
#include <polarssl/sha256.h>
#include <polarssl/aes.h>
#include <polarssl/ctr_drbg.h>
#include <polarssl/entropy.h>
#include <polarssl/x509_crt.h>
#include <polarssl/pk.h>
#include <polarssl/md.h>
#include <polarssl/cipher.h>
#include <stdio.h>
#include <stdlib.h>

// MbedTLS to PolarSSL mappings for symbols needed by moonlight-common-c
#define mbedtls_entropy_context entropy_context
#define mbedtls_ctr_drbg_context ctr_drbg_context
#define mbedtls_sha1_context sha1_context
#define mbedtls_sha256_context sha256_context
#define mbedtls_aes_context aes_context
#define mbedtls_md_context_t md_context_t
#define mbedtls_pk_context pk_context
#define mbedtls_x509_crt x509_crt
#define mbedtls_md_info_t md_info_t
#define mbedtls_cipher_context_t cipher_context_t
#define mbedtls_cipher_mode_t cipher_mode_t
#define mbedtls_cipher_type_t cipher_type_t
#define mbedtls_cipher_info_t cipher_info_t

#define MBEDTLS_MD_SHA256 POLARSSL_MD_SHA256
#define MBEDTLS_AES_ENCRYPT POLARSSL_ENCRYPT
#define MBEDTLS_AES_DECRYPT POLARSSL_DECRYPT
#define MBEDTLS_ENCRYPT POLARSSL_ENCRYPT
#define MBEDTLS_DECRYPT POLARSSL_DECRYPT
#define MBEDTLS_MODE_CBC POLARSSL_MODE_CBC
#define MBEDTLS_MODE_GCM POLARSSL_MODE_GCM
#define MBEDTLS_CIPHER_ID_AES POLARSSL_CIPHER_ID_AES

// Function stubs for MbedTLS symbols
void mbedtls_entropy_init(entropy_context *ctx);
void mbedtls_ctr_drbg_init(ctr_drbg_context *ctx);
int mbedtls_ctr_drbg_seed(ctr_drbg_context *ctx, int (*f_entropy)(void *, unsigned char *, size_t), void *p_entropy, const unsigned char *custom, size_t len);
int mbedtls_ctr_drbg_random(void *p_rng, unsigned char *output, size_t output_len);
int mbedtls_entropy_func(void *data, unsigned char *output, size_t len);

void mbedtls_cipher_init(cipher_context_t *ctx);
void mbedtls_cipher_free(cipher_context_t *ctx);
int mbedtls_cipher_setup(cipher_context_t *ctx, const cipher_info_t *cipher_info);
int mbedtls_cipher_setkey(cipher_context_t *ctx, const unsigned char *key, int key_bitlen, int operation);
int mbedtls_cipher_set_iv(cipher_context_t *ctx, const unsigned char *iv, size_t iv_len);
int mbedtls_cipher_reset(cipher_context_t *ctx);
int mbedtls_cipher_update(cipher_context_t *ctx, const unsigned char *input, size_t ilen, unsigned char *output, size_t *olen);
int mbedtls_cipher_finish(cipher_context_t *ctx, unsigned char *output, size_t *olen);
int mbedtls_cipher_auth_encrypt(cipher_context_t *ctx, const unsigned char *iv, size_t iv_len, const unsigned char *ad, size_t ad_len, const unsigned char *input, size_t ilen, unsigned char *output, size_t *olen, unsigned char *tag, size_t tag_len);
int mbedtls_cipher_auth_decrypt(cipher_context_t *ctx, const unsigned char *iv, size_t iv_len, const unsigned char *ad, size_t ad_len, const unsigned char *input, size_t ilen, unsigned char *output, size_t *olen, const unsigned char *tag, size_t tag_len);
/* MbedTLS 3.x AEAD wrappers for one-shot authenticated encryption/decryption.
   The tag is appended directly to/extracted from the end of the ciphertext. */
int mbedtls_cipher_auth_encrypt_ext(cipher_context_t *ctx, const unsigned char *iv, size_t iv_len, const unsigned char *ad, size_t ad_len, const unsigned char *input, size_t ilen, unsigned char *output, size_t output_buf_len, size_t *olen, size_t tag_len);
int mbedtls_cipher_auth_decrypt_ext(cipher_context_t *ctx, const unsigned char *iv, size_t iv_len, const unsigned char *ad, size_t ad_len, const unsigned char *input, size_t ilen, unsigned char *output, size_t output_buf_len, size_t *olen, size_t tag_len);
const cipher_info_t * mbedtls_cipher_info_from_values(int cipher_id, int key_bitlen, int mode);

// SHA
#define SHA_DIGEST_LENGTH 20
typedef sha1_context SHA_CTX;
static inline void SHA1_Init(SHA_CTX *c) { sha1_starts(c); }
static inline void SHA1_Update(SHA_CTX *c, const void *data, size_t len) { sha1_update(c, (unsigned char*)data, len); }
static inline void SHA1_Final(unsigned char *md, SHA_CTX *c) { sha1_finish(c, md); }

#define SHA256_DIGEST_LENGTH 32
typedef sha256_context SHA256_CTX;
static inline void SHA256_Init(SHA256_CTX *c) { sha256_starts(c, 0); }
static inline void SHA256_Update(SHA256_CTX *c, const void *data, size_t len) { sha256_update(c, (unsigned char*)data, len); }
static inline void SHA256_Final(unsigned char *md, SHA256_CTX *c) { sha256_finish(c, md); }

// AES
#define AES_ENCRYPT 1
#define AES_DECRYPT 0
typedef aes_context AES_KEY;
static inline void AES_set_encrypt_key(const unsigned char *userKey, const int bits, AES_KEY *key) { aes_setkey_enc(key, userKey, bits); }
static inline void AES_ecb_encrypt(const unsigned char *in, unsigned char *out, const AES_KEY *key, const int enc) { aes_crypt_ecb((aes_context*)key, enc == AES_ENCRYPT ? AES_ENCRYPT : AES_DECRYPT, in, out); }

// RAND
int RAND_bytes(unsigned char *buf, int num);

// X509 & EVP
typedef x509_crt X509;
typedef pk_context EVP_PKEY;

#define EVP_MAX_MD_SIZE 64
typedef struct {
    md_context_t md_ctx;
    pk_context *pk;
} EVP_MD_CTX;

static inline EVP_MD_CTX* EVP_MD_CTX_new() { return calloc(1, sizeof(EVP_MD_CTX)); }
static inline void EVP_MD_CTX_free(EVP_MD_CTX* ctx) { if(ctx) { md_free(&ctx->md_ctx); free(ctx); } }

const void* EVP_sha256();

int EVP_DigestSignInit(EVP_MD_CTX *ctx, void **pctx, const void *type, void *e, EVP_PKEY *pkey);
int EVP_DigestSignUpdate(EVP_MD_CTX *ctx, const void *d, size_t cnt);
int EVP_DigestSignFinal(EVP_MD_CTX *ctx, unsigned char *sig, size_t *siglen);

int EVP_DigestVerifyInit(EVP_MD_CTX *ctx, void **pctx, const void *type, void *e, EVP_PKEY *pkey);
int EVP_DigestVerifyUpdate(EVP_MD_CTX *ctx, const void *d, size_t cnt);
int EVP_DigestVerifyFinal(EVP_MD_CTX *ctx, const unsigned char *sig, size_t siglen);

// PEM
X509* PEM_read_X509(FILE *fp, X509 **x, void *cb, void *u);
EVP_PKEY* PEM_read_PrivateKey(FILE *fp, EVP_PKEY **x, void *cb, void *u);

#endif
