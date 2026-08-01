#include "openssl_compat.h"
#include <string.h>
#include <stdlib.h>

// MbedTLS Emulation Layer for PolarSSL
void mbedtls_entropy_init(entropy_context *ctx) {
    entropy_init(ctx);
}

void mbedtls_ctr_drbg_init(ctr_drbg_context *ctx) {
    memset(ctx, 0, sizeof(ctr_drbg_context));
}

int mbedtls_ctr_drbg_seed(ctr_drbg_context *ctx, int (*f_entropy)(void *, unsigned char *, size_t), void *p_entropy, const unsigned char *custom, size_t len) {
    return ctr_drbg_init(ctx, f_entropy, p_entropy, custom, len);
}

int mbedtls_ctr_drbg_random(void *p_rng, unsigned char *output, size_t output_len) {
    return ctr_drbg_random(p_rng, output, output_len);
}

int mbedtls_entropy_func(void *data, unsigned char *output, size_t len) {
    for (size_t i = 0; i < len; i++) output[i] = rand() % 256;
    return 0;
}

// Cipher API
void mbedtls_cipher_init(cipher_context_t *ctx) {
    cipher_init(ctx);
}

void mbedtls_cipher_free(cipher_context_t *ctx) {
    cipher_free(ctx);
}

int mbedtls_cipher_setup(cipher_context_t *ctx, const cipher_info_t *cipher_info) {
    return cipher_init_ctx(ctx, cipher_info);
}

int mbedtls_cipher_setkey(cipher_context_t *ctx, const unsigned char *key, int key_bitlen, int operation) {
    return cipher_setkey(ctx, key, key_bitlen, (operation_t)operation);
}

int mbedtls_cipher_set_iv(cipher_context_t *ctx, const unsigned char *iv, size_t iv_len) {
    return cipher_set_iv(ctx, iv, iv_len);
}

int mbedtls_cipher_reset(cipher_context_t *ctx) {
    return cipher_reset(ctx);
}

int mbedtls_cipher_update(cipher_context_t *ctx, const unsigned char *input, size_t ilen, unsigned char *output, size_t *olen) {
    return cipher_update(ctx, input, ilen, output, olen);
}

int mbedtls_cipher_finish(cipher_context_t *ctx, unsigned char *output, size_t *olen) {
    return cipher_finish(ctx, output, olen);
}

int mbedtls_cipher_auth_encrypt(cipher_context_t *ctx, const unsigned char *iv, size_t iv_len, const unsigned char *ad, size_t ad_len, const unsigned char *input, size_t ilen, unsigned char *output, size_t *olen, unsigned char *tag, size_t tag_len) {
    return cipher_auth_encrypt(ctx, iv, iv_len, ad, ad_len, input, ilen, output, olen, tag, tag_len);
}

int mbedtls_cipher_auth_decrypt(cipher_context_t *ctx, const unsigned char *iv, size_t iv_len, const unsigned char *ad, size_t ad_len, const unsigned char *input, size_t ilen, unsigned char *output, size_t *olen, const unsigned char *tag, size_t tag_len) {
    return cipher_auth_decrypt(ctx, iv, iv_len, ad, ad_len, input, ilen, output, olen, tag, tag_len);
}

/*
 * Emulates the MbedTLS 3.x one-shot authenticated encryption function.
 * Since PolarSSL expects the authentication tag to be written to a separate
 * buffer, we pass the end of the output buffer (output + ilen) as the tag
 * target, and update the output length (*olen) to include both the ciphertext
 * and the appended authentication tag.
 */
int mbedtls_cipher_auth_encrypt_ext(cipher_context_t *ctx, const unsigned char *iv, size_t iv_len,
                                    const unsigned char *ad, size_t ad_len,
                                    const unsigned char *input, size_t ilen,
                                    unsigned char *output, size_t output_buf_len,
                                    size_t *olen, size_t tag_len) {
    if (output_buf_len < ilen + tag_len) {
        return -1;
    }
    int ret = cipher_auth_encrypt(ctx, iv, iv_len, ad, ad_len, input, ilen, output, olen, output + ilen, tag_len);
    if (ret == 0 && olen != NULL) {
        *olen = ilen + tag_len;
    }
    return ret;
}

/*
 * Emulates the MbedTLS 3.x one-shot authenticated decryption function.
 * In MbedTLS 3.x, the authentication tag is expected to be appended to the end
 * of the input (ciphertext) buffer. We compute the ciphertext length (ilen - tag_len),
 * point the tag argument to the start of the tag within the input buffer (input + ilen - tag_len),
 * and call the PolarSSL decryption API.
 */
int mbedtls_cipher_auth_decrypt_ext(cipher_context_t *ctx, const unsigned char *iv, size_t iv_len,
                                    const unsigned char *ad, size_t ad_len,
                                    const unsigned char *input, size_t ilen,
                                    unsigned char *output, size_t output_buf_len,
                                    size_t *olen, size_t tag_len) {
    if (ilen < tag_len || output_buf_len < ilen - tag_len) {
        return -1;
    }
    const unsigned char *tag = input + ilen - tag_len;
    size_t cipher_len = ilen - tag_len;
    return cipher_auth_decrypt(ctx, iv, iv_len, ad, ad_len, input, cipher_len, output, olen, tag, tag_len);
}

const cipher_info_t * mbedtls_cipher_info_from_values(int cipher_id, int key_bitlen, int mode) {
    return cipher_info_from_values((cipher_id_t)cipher_id, key_bitlen, (cipher_mode_t)mode);
}

static int dummy_entropy_func(void *data, unsigned char *output, size_t len) {
    for (size_t i = 0; i < len; i++) output[i] = rand() & 0xFF;
    return 0;
}

int RAND_bytes(unsigned char *buf, int num) {
    static ctr_drbg_context ctr_drbg;
    static int initialized = 0;

    if (!initialized) {
        ctr_drbg_init(&ctr_drbg, dummy_entropy_func, NULL, NULL, 0);
        initialized = 1;
    }

    return ctr_drbg_random(&ctr_drbg, buf, num) == 0 ? 1 : 0;
}

const void* EVP_sha256() {
    return md_info_from_type(POLARSSL_MD_SHA256);
}

int EVP_DigestSignInit(EVP_MD_CTX *ctx, void **pctx, const void *type, void *e, EVP_PKEY *pkey) {
    md_init(&ctx->md_ctx);
    md_init_ctx(&ctx->md_ctx, (const md_info_t *)type);
    md_starts(&ctx->md_ctx);
    ctx->pk = pkey;
    return 1;
}

int EVP_DigestSignUpdate(EVP_MD_CTX *ctx, const void *d, size_t cnt) {
    md_update(&ctx->md_ctx, d, cnt);
    return 1;
}

static int rand_wrapper(void* p_rng, unsigned char* output, size_t output_len) {
    return RAND_bytes(output, (int)output_len) == 1 ? 0 : -1;
}

int EVP_DigestSignFinal(EVP_MD_CTX *ctx, unsigned char *sig, size_t *siglen) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    md_finish(&ctx->md_ctx, hash);
    pk_sign(ctx->pk, POLARSSL_MD_SHA256, hash, 0, sig, siglen, rand_wrapper, NULL);
    return 1;
}

int EVP_DigestVerifyInit(EVP_MD_CTX *ctx, void **pctx, const void *type, void *e, EVP_PKEY *pkey) {
    md_init(&ctx->md_ctx);
    md_init_ctx(&ctx->md_ctx, (const md_info_t *)type);
    md_starts(&ctx->md_ctx);
    ctx->pk = pkey;
    return 1;
}

int EVP_DigestVerifyUpdate(EVP_MD_CTX *ctx, const void *d, size_t cnt) {
    md_update(&ctx->md_ctx, d, cnt);
    return 1;
}

int EVP_DigestVerifyFinal(EVP_MD_CTX *ctx, const unsigned char *sig, size_t siglen) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    md_finish(&ctx->md_ctx, hash);
    return pk_verify(ctx->pk, POLARSSL_MD_SHA256, hash, 0, sig, siglen) == 0 ? 1 : 0;
}

X509* PEM_read_X509(FILE *fp, X509 **x, void *cb, void *u) {
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    unsigned char *buf = malloc(fsize + 1);
    fread(buf, 1, fsize, fp);
    buf[fsize] = 0;

    X509 *cert = *x ? *x : malloc(sizeof(X509));
    x509_crt_init(cert);
    x509_crt_parse(cert, buf, fsize + 1);
    free(buf);
    return cert;
}

EVP_PKEY* PEM_read_PrivateKey(FILE *fp, EVP_PKEY **x, void *cb, void *u) {
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    unsigned char *buf = malloc(fsize + 1);
    fread(buf, 1, fsize, fp);
    buf[fsize] = 0;

    EVP_PKEY *pk = *x ? *x : malloc(sizeof(EVP_PKEY));
    pk_init(pk);
    pk_parse_key(pk, buf, fsize + 1, NULL, 0);
    free(buf);
    return pk;
}
