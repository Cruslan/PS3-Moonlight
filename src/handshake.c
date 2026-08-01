#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>
#include <polarssl/net.h>
#include <polarssl/ssl.h>
#include <polarssl/debug.h>
#include <polarssl/pk.h>
#include <polarssl/rsa.h>
#include <polarssl/x509_crt.h>
#include <polarssl/entropy.h>
#include <polarssl/ctr_drbg.h>
#include <polarssl/sha256.h>
#include <polarssl/aes.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <sys/time.h>
#include "uuid.h"
#include "net_logger.h"
#include "handshake.h"
#include "ui.h"
#include <net/poll.h>

static void bin_to_hex(const unsigned char *bin, size_t len, char *out);

#define SUNSHINE_HTTPS_PORT 47984
#define SUNSHINE_HTTP_PORT  47989

static int connect_with_cancel(int fd, const struct sockaddr *addr, socklen_t addrlen) {
    int nbio = 1;
    setsockopt(fd, SOL_SOCKET, SO_NBIO, &nbio, sizeof(nbio));

    int rc = connect(fd, addr, addrlen);
    if (rc < 0) {
        if (errno != EINPROGRESS && errno != EWOULDBLOCK && errno != EALREADY) {
            return -1;
        }
    } else {
        nbio = 0;
        setsockopt(fd, SOL_SOCKET, SO_NBIO, &nbio, sizeof(nbio));
        return 0;
    }

    int elapsed_ms = 0;
    while (elapsed_ms < 5000) {
        if (ui_get_state() == UI_STATE_IP_ENTRY || !ui_is_running()) {
            return -1;
        }

        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;

        int poll_ret = poll(&pfd, 1, 100);
        if (poll_ret > 0) {
            if (pfd.revents & POLLOUT) {
                int err = 0;
                socklen_t errlen = sizeof(err);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen) == 0 && err == 0) {
                    nbio = 0;
                    setsockopt(fd, SOL_SOCKET, SO_NBIO, &nbio, sizeof(nbio));
                    return 0;
                }
            }
            break;
        } else if (poll_ret < 0) {
            break;
        }

        elapsed_ms += 100;
    }

    return -1;
}

// Backwards compat: keep SUNSHINE_PORT for existing HTTPS calls
#define SUNSHINE_PORT SUNSHINE_HTTPS_PORT

struct string {
    char *ptr;
    size_t len;
};

static void init_string(struct string *s) {
    s->len = 0;
    s->ptr = malloc(s->len + 1);
    s->ptr[0] = '\0';
}

static int hv_entropy_func(void *data, unsigned char *output, size_t len) {
    (void)data;
    static int seeded = 0;
    if (!seeded) {
        // High-resolution seed using time, clock and a constant for PS3
        srand(time(NULL) ^ (clock() << 8) ^ 0xACE12345);
        for(int i=0; i<100; i++) rand(); // Warm up PRNG
        seeded = 1;
    }
    for (size_t i = 0; i < len; i++) {
        output[i] = (unsigned char)(rand() % 256 ^ (clock() & 0xFF));
    }
    return 0;
}

static const int ps3_ciphers[] = {
    TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
    TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
    TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256,
    TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA384,
    TLS_RSA_WITH_AES_128_GCM_SHA256,
    TLS_RSA_WITH_AES_256_GCM_SHA384,
    TLS_RSA_WITH_AES_256_CBC_SHA256,
    TLS_RSA_WITH_AES_128_CBC_SHA256,
    TLS_RSA_WITH_AES_256_CBC_SHA,
    TLS_RSA_WITH_AES_128_CBC_SHA,
    0
};

static void ps3_ssl_debug(void *ctx, int level, const char *str) {
    (void)ctx;
    (void)level;
    // Redirect PolarSSL internal debug to our NLOG
    char log_str[1024];
    strncpy(log_str, str, sizeof(log_str));
    if (log_str[strlen(log_str)-1] == '\n') log_str[strlen(log_str)-1] = '\0';
    NLOG("[SSL] %s", log_str);
}

// Custom HTTPS request using PolarSSL directly
static int ps3_https_request(handshake_info_t *info, const char *url_path, struct string *response) {
    int ret, fd = -1;
    ssl_context ssl;
    ssl_session ssn;
    entropy_context entropy;
    ctr_drbg_context ctr_drbg;
    x509_crt clicert;
    pk_context pkey;
    
    // Initialize structures
    memset(&ssl, 0, sizeof(ssl_context));
    memset(&ssn, 0, sizeof(ssl_session));
    x509_crt_init(&clicert);
    pk_init(&pkey);
    entropy_init(&entropy);
    ctr_drbg_init(&ctr_drbg, hv_entropy_func, NULL, NULL, 0);

    debug_set_threshold(ui_get_verbose() ? 4 : 0);

    // Load cert and key from paths
    if (x509_crt_parse_file(&clicert, info->client_cert_path) != 0 ||
        pk_parse_keyfile(&pkey, info->client_key_path, NULL) != 0) {
        NLOG("Failed to load cert/key for SSL connection");
        return -1;
    }

    // Connect using standard sockets
    struct sockaddr_in serv_addr;
    // RPCS3 Fix: Connect directly to the provided address
    // (Old hardcoded collision logic removed as it caused issues)
    const char *target_ip = info->address;

    NLOG("Connecting to %s:%d...", target_ip, SUNSHINE_PORT);
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        NLOG("socket creation failed: %d", errno);
        return -1;
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(SUNSHINE_PORT);
    if (inet_pton(AF_INET, target_ip, &serv_addr.sin_addr) <= 0) {
        NLOG("invalid address: %s", target_ip);
        close(fd); return -1;
    }

    struct timeval tv;
    tv.tv_sec = 30;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect_with_cancel(fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        NLOG("connect failed: %d", errno);
        close(fd); return -1;
    }
    NLOG("TCP Connected. Initializing SSL...");

    if ((ret = ssl_init(&ssl)) != 0) {
        NLOG("ssl_init failed: %d", ret);
        close(fd); return -1;
    }

    ssl_set_endpoint(&ssl, SSL_IS_CLIENT);
    ssl_set_authmode(&ssl, SSL_VERIFY_NONE);
    ssl_set_rng(&ssl, ctr_drbg_random, &ctr_drbg);
    ssl_set_bio(&ssl, net_recv, &fd, net_send, &fd);
    ssl_set_own_cert(&ssl, &clicert, &pkey);
    
    // SNI and Debug
    // ssl_set_hostname(&ssl, target_ip); // Disabled for compatibility
    if (ui_get_verbose()) {
        ssl_set_dbg(&ssl, ps3_ssl_debug, NULL);
    }
    ssl_set_ciphersuites(&ssl, ps3_ciphers);
    ssl_set_renegotiation(&ssl, SSL_RENEGOTIATION_ENABLED);
    
    // Relax TLS Versions
    ssl_set_min_version(&ssl, SSL_MAJOR_VERSION_3, SSL_MINOR_VERSION_1); // TLS 1.0
    ssl_set_max_version(&ssl, SSL_MAJOR_VERSION_3, SSL_MINOR_VERSION_3); // TLS 1.2

    NLOG("Starting SSL Handshake...");
    while ((ret = ssl_handshake(&ssl)) != 0) {
        if (ret != POLARSSL_ERR_NET_WANT_READ && ret != POLARSSL_ERR_NET_WANT_WRITE) {
            NLOG("ssl_handshake failed: -0x%x", -ret);
            ssl_free(&ssl); net_close(fd); return -1;
        }
    }

    // Construct and send GET request
    char request[4096];
    snprintf(request, sizeof(request), 
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "User-Agent: Moonlight-PS3\r\n"
        "Connection: close\r\n\r\n",
        url_path, info->address, SUNSHINE_PORT);

    ssl_write(&ssl, (unsigned char*)request, strlen(request));

    // Read response
    unsigned char buf[1024];
    init_string(response);
    while (1) {
        ret = ssl_read(&ssl, buf, sizeof(buf) - 1);
        if (ret == POLARSSL_ERR_NET_WANT_READ || ret == POLARSSL_ERR_NET_WANT_WRITE) continue;
        if (ret <= 0) break;
        
        size_t new_len = response->len + ret;
        response->ptr = realloc(response->ptr, new_len + 1);
        memcpy(response->ptr + response->len, buf, ret);
        response->len = new_len;
        response->ptr[new_len] = '\0';
    }

    ssl_free(&ssl);
    x509_crt_free(&clicert);
    pk_free(&pkey);
    ctr_drbg_free(&ctr_drbg);
    entropy_free(&entropy);
    close(fd);

    return 0;
}

// Plain HTTP request (no SSL) for initial pairing steps
// Sunshine's /pair endpoint is also available on plain HTTP port 47989
static int ps3_http_request(handshake_info_t *info, const char *url_path, struct string *response) {
    int fd;
    struct sockaddr_in serv_addr;
    char request[4096];
    char buf[1024];
    int ret;

    NLOG("[HTTP] Connecting to %s:%d (plain TCP)...", info->address, SUNSHINE_HTTP_PORT);

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        NLOG("[HTTP] socket failed: %d", errno);
        return -1;
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(SUNSHINE_HTTP_PORT);
    if (inet_pton(AF_INET, info->address, &serv_addr.sin_addr) <= 0) {
        NLOG("[HTTP] invalid address: %s", info->address);
        close(fd); return -1;
    }

    struct timeval tv;
    tv.tv_sec = 30;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect_with_cancel(fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        NLOG("[HTTP] connect failed: %d", errno);
        close(fd); return -1;
    }

    snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "User-Agent: Moonlight-PS3\r\n"
        "Connection: close\r\n\r\n",
        url_path, info->address, SUNSHINE_HTTP_PORT);

    if (send(fd, request, strlen(request), 0) < 0) {
        NLOG("[HTTP] send failed: %d", errno);
        close(fd); return -1;
    }

    init_string(response);
    while (1) {
        ret = recv(fd, buf, sizeof(buf) - 1, 0);
        if (ret <= 0) break;
        size_t new_len = response->len + ret;
        response->ptr = realloc(response->ptr, new_len + 1);
        memcpy(response->ptr + response->len, buf, ret);
        response->len = new_len;
        response->ptr[new_len] = '\0';
    }

    close(fd);
    NLOG("[HTTP] Response (%zu bytes)", response->len);
    return 0;
}

int hv_generate_credentials(handshake_info_t *info) {
    int ret;
    pk_context key;
    ctr_drbg_context ctr_drbg;
    entropy_context entropy;
    x509write_cert crt;
    mpi serial;

    NLOG("Generating RSA 2048 key (PolarSSL DER)...");
    pk_init(&key);
    entropy_init(&entropy);
    x509write_crt_init(&crt);
    mpi_init(&serial);

    if ((ret = ctr_drbg_init(&ctr_drbg, hv_entropy_func, NULL, NULL, 0)) != 0) {
        NLOG("ctr_drbg_init failed: %d", ret);
        return -1;
    }

    if ((ret = pk_init_ctx(&key, pk_info_from_type(POLARSSL_PK_RSA))) != 0) {
        NLOG("pk_init_ctx failed: %d", ret);
        return -1;
    }

    if ((ret = rsa_gen_key(pk_rsa(key), ctr_drbg_random, &ctr_drbg, 2048, 65537)) != 0) {
        NLOG("rsa_gen_key failed: %d", ret);
        return -1;
    }

    // Write private key
    FILE *f = fopen(info->client_key_path, "wb");
    if (f) {
        unsigned char buf[4096];
        ret = pk_write_key_pem(&key, buf, sizeof(buf));
        if (ret == 0) {
            fwrite(buf, 1, strlen((char*)buf), f);
        }
        fclose(f);
    } else {
        NLOG("Failed to open key file: %s", info->client_key_path);
        return -1;
    }

    // Create self-signed cert
    x509write_crt_set_subject_key(&crt, &key);
    x509write_crt_set_issuer_key(&crt, &key);
    x509write_crt_set_subject_name(&crt, "CN=Moonlight-PS3");
    x509write_crt_set_issuer_name(&crt, "CN=Moonlight-PS3");
    x509write_crt_set_md_alg(&crt, POLARSSL_MD_SHA256);
    
    mpi_read_string(&serial, 10, "1");
    x509write_crt_set_serial(&crt, &serial);
    x509write_crt_set_validity(&crt, "20200101000000", "20500101000000");

    // Write certificate to PEM buffer
    unsigned char pem_buf[4096];
    ret = x509write_crt_pem(&crt, pem_buf, sizeof(pem_buf), ctr_drbg_random, &ctr_drbg);
    if (ret != 0) {
        NLOG("x509write_crt_pem failed: %d", ret);
        goto cleanup;
    }

    // Save as PEM for libcurl
    f = fopen(info->client_cert_path, "w");
    if (f) {
        fprintf(f, "%s", pem_buf);
        fclose(f);
    }

    NLOG("Credentials generated successfully (PEM format)!");
    
cleanup:
    x509write_crt_free(&crt);
    pk_free(&key);
    ctr_drbg_free(&ctr_drbg);
    entropy_free(&entropy);
    mpi_free(&serial);

    return 0;
}

int hv_init(handshake_info_t *info, const char *address) {
    strncpy(info->address, address, sizeof(info->address));
    
    const char *base_path = "/dev_hdd0/game/MNLT00001/USRDIR";
    mkdir(base_path, 0777);
    
    snprintf(info->client_cert_path, sizeof(info->client_cert_path), "%s/cert.pem", base_path);
    snprintf(info->client_key_path, sizeof(info->client_key_path), "%s/key.pem", base_path);

    char id_path[256];
    snprintf(id_path, sizeof(id_path), "%s/uniqueid.dat", base_path);
    
    FILE *f = fopen(id_path, "r");
    if (f) {
        memset(info->unique_id, 0, sizeof(info->unique_id));
        size_t n = fread(info->unique_id, 1, 63, f);
        info->unique_id[n] = '\0';
        fclose(f);
        
        // Strip out any trailing garbage, newline, or whitespace that breaks HTTP Requests
        for (size_t i = 0; i < strlen(info->unique_id); i++) {
            if (info->unique_id[i] == '\n' || info->unique_id[i] == '\r' || info->unique_id[i] == '\t' || info->unique_id[i] == '`' || info->unique_id[i] == ' ') {
                info->unique_id[i] = '\0';
                break;
            }
        }

        NLOG("Loaded persistent unique_id: %s", info->unique_id);
    } else {
        uuid_t uu;
        uuid_generate_random(uu);
        uuid_unparse(uu, info->unique_id);
        f = fopen(id_path, "w");
        if (f) {
            fwrite(info->unique_id, 1, strlen(info->unique_id), f);
            fclose(f);
            NLOG("Saved new unique_id: %s", info->unique_id);
        }
    }
    
    // Only regenerate if credentials don't exist
    FILE *c1 = fopen(info->client_cert_path, "r");
    FILE *k1 = fopen(info->client_key_path, "r");
    if (c1 && k1) {
        fclose(c1);
        fclose(k1);
        NLOG("Using existing persistent credentials.");
        return 0;
    }
    if (c1) fclose(c1);
    if (k1) fclose(k1);

    return hv_generate_credentials(info);
}


static void bin_to_hex(const unsigned char *bin, size_t len, char *out) {
    for (size_t i = 0; i < len; i++) {
        sprintf(out + (i * 2), "%02X", bin[i]);
    }
    out[len * 2] = '\0';
}

static void hex_to_bin(const char *hex, unsigned char *out) {
    size_t len = strlen(hex) / 2;
    for (size_t i = 0; i < len; i++) {
        unsigned char high = hex[i * 2];
        unsigned char low = hex[i * 2 + 1];
        
        if (high >= '0' && high <= '9') high -= '0';
        else if (high >= 'A' && high <= 'F') high = high - 'A' + 10;
        else if (high >= 'a' && high <= 'f') high = high - 'a' + 10;
        
        if (low >= '0' && low <= '9') low -= '0';
        else if (low >= 'A' && low <= 'F') low = low - 'A' + 10;
        else if (low >= 'a' && low <= 'f') low = low - 'a' + 10;
        
        out[i] = (high << 4) | low;
    }
}

static char* extract_xml(const char *xml, const char *tag) {
    char start_tag[64], end_tag[64];
    snprintf(start_tag, sizeof(start_tag), "<%s>", tag);
    snprintf(end_tag, sizeof(end_tag), "</%s>", tag);
    
    char *start = strstr(xml, start_tag);
    if (!start) return NULL;
    start += strlen(start_tag);
    
    char *end = strstr(start, end_tag);
    if (!end) return NULL;
    
    size_t len = end - start;
    char *res = malloc(len + 1);
    memcpy(res, start, len);
    res[len] = '\0';
    return res;
}

int hv_is_paired(handshake_info_t *info) {
    char path[512];
    struct string s;
    snprintf(path, sizeof(path), "/serverinfo?uniqueid=%s", info->unique_id);

    NLOG("Checking pairing status via HTTPS...");
    // If we are not paired, the HTTPS handshake will typically fail on the server side
    // because Sunshine requires a trusted client certificate for HTTPS.
    if (ps3_https_request(info, path, &s) != 0) {
        NLOG("hv_is_paired: HTTPS handshake failed or rejected (not paired)");
        return 0;
    }

    char *paired_val = extract_xml(s.ptr, "PairStatus");
    if (!paired_val) paired_val = extract_xml(s.ptr, "paired");

    int paired = 0;
    if (paired_val) {
        paired = atoi(paired_val);
        free(paired_val);
    }

    free(s.ptr);
    NLOG("hv_is_paired: %s", paired ? "YES" : "NO");
    return paired;
}

int hv_pair(handshake_info_t *info, const char *pin) {
    char path[8192];
    struct string s;
    init_string(&s);

    NLOG("Starting pairing with PIN: %s", pin);

    // --- Generate random uuid (vita does this per-request, we'll use one for all HTTP steps)
    char uuid_str[40];
    {
        srand((unsigned int)time(NULL));
        snprintf(uuid_str, sizeof(uuid_str),
            "%08x-%04x-4%03x-%04x-%08x%04x",
            rand() & 0xffffffff,
            rand() & 0xffff,
            rand() & 0x0fff,
            (rand() & 0x3fff) | 0x8000,
            rand() & 0xffffffff,
            rand() & 0xffff);
    }

    // --- Generate salt (16 random bytes -> hex)
    unsigned char salt_data[16];
    char salt_hex[33];
    for (int i = 0; i < 16; i++) salt_data[i] = rand() & 0xff;
    bin_to_hex(salt_data, 16, salt_hex);

    // --- Build cert_hex: vita reads PEM file BYTES and hex-encodes them directly
    //     This is the key difference from DER encoding!
    char cert_hex[8192];
    cert_hex[0] = '\0';
    {
        FILE *cf = fopen(info->client_cert_path, "r");
        if (!cf) {
            NLOG("Cannot open cert file: %s", info->client_cert_path);
            return -1;
        }
        int c;
        int length = 0;
        while ((c = fgetc(cf)) != EOF && length < (int)(sizeof(cert_hex) - 3)) {
            sprintf(cert_hex + length, "%02x", (unsigned char)c);
            length += 2;
        }
        cert_hex[length] = '\0';
        fclose(cf);
    }

    // --- Load private key for signing (Step 4)
    pk_context key;
    entropy_context entropy;
    ctr_drbg_context ctr_drbg;
    pk_init(&key);
    entropy_init(&entropy);
    ctr_drbg_init(&ctr_drbg, hv_entropy_func, NULL, NULL, 0);
    if (pk_parse_keyfile(&key, info->client_key_path, NULL) != 0) {
        NLOG("Failed to load private key from %s", info->client_key_path);
        pk_free(&key); entropy_free(&entropy); ctr_drbg_free(&ctr_drbg);
        return -1;
    }

    // --- Load X509 cert for getting signature bytes (Step 3)
    x509_crt client_cert;
    x509_crt_init(&client_cert);
    if (x509_crt_parse_file(&client_cert, info->client_cert_path) != 0) {
        NLOG("Failed to parse client cert from %s", info->client_cert_path);
        pk_free(&key); entropy_free(&entropy); ctr_drbg_free(&ctr_drbg);
        return -1;
    }

    // STEP 1: getservercert (HTTP)
    snprintf(path, sizeof(path),
        "/pair?uniqueid=%s&uuid=%s&devicename=PS3&updateState=1&phrase=getservercert&salt=%s&clientcert=%s",
        info->unique_id, uuid_str, salt_hex, cert_hex);

    if (ps3_http_request(info, path, &s) != 0) {
        NLOG("Step 1: HTTP request failed");
        goto fail;
    }

    // Check paired=1
    {
        char *paired_val = extract_xml(s.ptr, "paired");
        if (!paired_val || strcmp(paired_val, "1") != 0) {
            NLOG("Step 1: server did not return paired=1 (response: %.500s)", s.ptr ? s.ptr : "empty");
            free(paired_val);
            goto fail;
        }
        free(paired_val);
    }

    // Extract plaincert (PEM text, hex-encoded by Sunshine)
    char plaincert[8192];
    {
        char *plaincert_hex = extract_xml(s.ptr, "plaincert");
        if (!plaincert_hex) {
            NLOG("Step 1: no plaincert in response (response: %.500s)", s.ptr ? s.ptr : "empty");
            goto fail;
        }
        size_t phlen = strlen(plaincert_hex);
        if (phlen / 2 > sizeof(plaincert) - 1) {
            NLOG("Step 1: plaincert too big");
            free(plaincert_hex);
            goto fail;
        }
        // hex → bytes (this is the PEM text as bytes)
        for (size_t count = 0; count < phlen; count += 2) {
            char hex_byte[3] = {plaincert_hex[count], plaincert_hex[count+1], '\0'};
            plaincert[count/2] = (char)(unsigned char)strtol(hex_byte, NULL, 16);
        }
        plaincert[phlen/2] = '\0';
        free(plaincert_hex);
        NLOG("Step 1 OK. plaincert len=%zu", phlen/2);
    }

    // Parse server cert from its PEM text (needed for verifySignature in Step 4)
    x509_crt server_cert;
    x509_crt_init(&server_cert);
    {
        int rc = x509_crt_parse(&server_cert,
                                (const unsigned char *)plaincert,
                                strlen(plaincert) + 1);
        if (rc != 0) {
            NLOG("Step 1: failed to parse server PEM cert (rc=%d)", rc);
            goto fail_server_cert;
        }
    }

    // --- Derive AES key: SHA256(salt | pin), take first 16 bytes
    unsigned char aes_key[16];
    {
        unsigned char salt_pin[20];
        memcpy(salt_pin, salt_data, 16);
        memcpy(salt_pin + 16, pin, 4);
        unsigned char hash[32];
        sha256(salt_pin, 20, hash, 0);
        memcpy(aes_key, hash, 16);
    }

    // STEP 2: clientchallenge (HTTP) — retry waiting for PIN entry
    unsigned char challenge_data[16];
    for (int i = 0; i < 16; i++) challenge_data[i] = rand() & 0xff;
    unsigned char challenge_enc[16];
    {
        aes_context actx;
        aes_setkey_enc(&actx, aes_key, 128);
        aes_crypt_ecb(&actx, AES_ENCRYPT, challenge_data, challenge_enc);
    }
    char challenge_hex[33];
    bin_to_hex(challenge_enc, 16, challenge_hex);

    // Save challenge_response_data for Step 3
    char challenge_response_data[64];
    int  challenge_response_data_len = 0;
    int  hash_length = 32; // Sunshine = SHA256

    int paired = 0;
    for (int retry = 0; retry < 10 && !paired; retry++) {
        free(s.ptr); init_string(&s);

        // Regenerate uuid per request (like vita does)
        snprintf(uuid_str, sizeof(uuid_str),
            "%08x-%04x-4%03x-%04x-%08x%04x",
            rand()&0xffffffff, rand()&0xffff, rand()&0x0fff,
            (rand()&0x3fff)|0x8000, rand()&0xffffffff, rand()&0xffff);

        snprintf(path, sizeof(path),
            "/pair?uniqueid=%s&uuid=%s&devicename=PS3&updateState=1&clientchallenge=%s",
            info->unique_id, uuid_str, challenge_hex);

        // Check for User Cancellation
        if (ui_get_state() == UI_STATE_IP_ENTRY) {
            NLOG("Pairing cancelled by user.");
            goto fail_server_cert;
        }

        // Single attempt with 30s socket timeout
        if (ps3_http_request(info, path, &s) != 0) {
            NLOG("Pairing request failed or timed out.");
            continue;
        }

        // Check paired=1
        char *pv = extract_xml(s.ptr, "paired");
        if (!pv || strcmp(pv, "1") != 0) {
            free(pv);
            NLOG("Waiting for PIN entry (%d/30)...", retry+1);
            sleep(2); continue;
        }
        free(pv);

        // Get challengeresponse
        char *cr_hex = extract_xml(s.ptr, "challengeresponse");
        if (!cr_hex) {
            NLOG("Step 2: no challengeresponse, retry %d", retry);
            sleep(2); continue;
        }

        size_t crlen = strlen(cr_hex);
        if (crlen / 2 > sizeof(challenge_response_data)) {
            NLOG("Step 2: challengeresponse too big");
            free(cr_hex); goto fail_server_cert;
        }
        for (size_t co = 0; co < crlen; co += 2) {
            char hb[3] = {cr_hex[co], cr_hex[co+1], '\0'};
            challenge_response_data[co/2] = (char)(unsigned char)strtol(hb, NULL, 16);
        }
        challenge_response_data_len = (int)(crlen / 2);
        free(cr_hex);

        // Decrypt challengeresponse
        {
            aes_context actx;
            unsigned char dec_buf[64];
            aes_setkey_dec(&actx, aes_key, 128);
            for (int i = 0; i < challenge_response_data_len; i += 16)
                aes_crypt_ecb(&actx, AES_DECRYPT,
                    (unsigned char*)challenge_response_data + i, dec_buf + i);
            memcpy(challenge_response_data, dec_buf, challenge_response_data_len);
        }
        NLOG("Step 2 OK. PIN accepted! Proceeding to Step 3...");
        paired = 1;
    }

    if (!paired) {
        NLOG("Pairing timed out.");
        goto fail_server_cert;
    }

    // STEP 3: serverchallengeresp (HTTP)
    // challenge_response = serverNonce(16) | clientCertSig | clientSecret(16)
    unsigned char client_secret_data[16];
    for (int i = 0; i < 16; i++) client_secret_data[i] = rand() & 0xff;

    {
        // serverNonce is the last 16 bytes of the decrypted challenge response (after hash_length)
        unsigned char *server_nonce = (unsigned char*)challenge_response_data + hash_length;
        int sig_len = (int)client_cert.sig.len;
        unsigned char *sig_bytes = client_cert.sig.p;

        unsigned char challenge_response[512];
        int cr_len = 0;
        memcpy(challenge_response + cr_len, server_nonce, 16); cr_len += 16;
        memcpy(challenge_response + cr_len, sig_bytes, sig_len); cr_len += sig_len;
        memcpy(challenge_response + cr_len, client_secret_data, 16); cr_len += 16;

        unsigned char cr_hash[32];
        sha256(challenge_response, cr_len, cr_hash, 0);

        unsigned char cr_hash_enc[32];
        {
            aes_context actx;
            aes_setkey_enc(&actx, aes_key, 128);
            for (int i = 0; i < 32; i += 16)
                aes_crypt_ecb(&actx, AES_ENCRYPT, cr_hash + i, cr_hash_enc + i);
        }
        char cr_hex[65];
        bin_to_hex(cr_hash_enc, 32, cr_hex);

        free(s.ptr); init_string(&s);
        snprintf(uuid_str, sizeof(uuid_str), "%08x-%04x-4%03x-%04x-%08x%04x",
            rand()&0xffffffff, rand()&0xffff, rand()&0x0fff,
            (rand()&0x3fff)|0x8000, rand()&0xffffffff, rand()&0xffff);
        snprintf(path, sizeof(path),
            "/pair?uniqueid=%s&uuid=%s&devicename=PS3&updateState=1&serverchallengeresp=%s",
            info->unique_id, uuid_str, cr_hex);
        if (ps3_http_request(info, path, &s) != 0) {
            NLOG("Step 3 HTTP failed"); goto fail_server_cert;
        }
    }

    // Check paired=1 in Step 3 response
    {
        char *pv = extract_xml(s.ptr, "paired");
        if (!pv || strcmp(pv, "1") != 0) {
            NLOG("Step 3: server challenge response rejected (response: %.400s)", s.ptr ? s.ptr : "");
            free(pv); goto fail_server_cert;
        }
        free(pv);
    }

    // STEP 4: clientpairingsecret (HTTP)
    // Verify server's pairingsecret, then sign our client_secret and send back
    {
        char *ps_hex = extract_xml(s.ptr, "pairingsecret");
        if (!ps_hex) { NLOG("Step 3: no pairingsecret"); goto fail_server_cert; }

        unsigned char pairing_secret[272]; // 16 + 256
        size_t pslen = strlen(ps_hex) / 2;
        for (size_t co = 0; co < strlen(ps_hex); co += 2) {
            char hb[3] = {ps_hex[co], ps_hex[co+1], '\0'};
            pairing_secret[co/2] = (unsigned char)strtol(hb, NULL, 16);
        }
        free(ps_hex);

        // verifySignature: SHA256(pairing_secret[:16]) verified with server ECDSA/RSA sig
        // vita uses EVP_DigestVerify; we use pk_verify with SHA256 of the data directly
        // Actually vita verifies: data=pairing_secret[:16], sig=pairing_secret[16:272], cert=plaincert
        // pk_verify expects the HASH of the data, not the data itself
        unsigned char ps_hash[32];
        sha256(pairing_secret, 16, ps_hash, 0);
        int verify_rc = pk_verify(&server_cert.pk, POLARSSL_MD_SHA256,
                                  ps_hash, 32,
                                  pairing_secret + 16, pslen - 16);
        if (verify_rc != 0) {
            NLOG("Step 4: MITM detected! Server signature verification failed (rc=%d)", verify_rc);
            goto fail_server_cert;
        }
        NLOG("Step 4: Server signature verified OK.");

        // Sign client_secret_data with our private key
        unsigned char cs_hash[32];
        sha256(client_secret_data, 16, cs_hash, 0);
        unsigned char client_sig[256];
        size_t client_sig_len = 0;
        if (pk_sign(&key, POLARSSL_MD_SHA256, cs_hash, 32,
                    client_sig, &client_sig_len,
                    ctr_drbg_random, &ctr_drbg) != 0) {
            NLOG("Step 4: failed to sign client secret"); goto fail_server_cert;
        }

        unsigned char cps[272];
        memcpy(cps, client_secret_data, 16);
        memcpy(cps + 16, client_sig, client_sig_len);
        char cps_hex[545];
        bin_to_hex(cps, 16 + client_sig_len, cps_hex);

        free(s.ptr); init_string(&s);
        snprintf(uuid_str, sizeof(uuid_str), "%08x-%04x-4%03x-%04x-%08x%04x",
            rand()&0xffffffff, rand()&0xffff, rand()&0x0fff,
            (rand()&0x3fff)|0x8000, rand()&0xffffffff, rand()&0xffff);
        snprintf(path, sizeof(path),
            "/pair?uniqueid=%s&uuid=%s&devicename=PS3&updateState=1&clientpairingsecret=%s",
            info->unique_id, uuid_str, cps_hex);
        if (ps3_http_request(info, path, &s) != 0) {
            NLOG("Step 4 HTTP failed"); goto fail_server_cert;
        }

        char *pv = extract_xml(s.ptr, "paired");
        if (!pv || strcmp(pv, "1") != 0) {
            NLOG("Step 4: rejected (response: %.400s)", s.ptr ? s.ptr : "");
            free(pv); goto fail_server_cert;
        }
        free(pv);
        NLOG("Step 4 OK.");
    }

    // STEP 5: pairchallenge (HTTPS)
    {
        free(s.ptr); init_string(&s);
        snprintf(uuid_str, sizeof(uuid_str), "%08x-%04x-4%03x-%04x-%08x%04x",
            rand()&0xffffffff, rand()&0xffff, rand()&0x0fff,
            (rand()&0x3fff)|0x8000, rand()&0xffffffff, rand()&0xffff);
        snprintf(path, sizeof(path),
            "/pair?uniqueid=%s&uuid=%s&devicename=PS3&updateState=1&phrase=pairchallenge",
            info->unique_id, uuid_str);
        if (ps3_https_request(info, path, &s) != 0) {
            NLOG("Step 5 HTTPS failed"); goto fail_server_cert;
        }
        char *pv = extract_xml(s.ptr, "paired");
        if (!pv || strcmp(pv, "1") != 0) {
            NLOG("Step 5: not paired (response: %.400s)", s.ptr ? s.ptr : "");
            free(pv); goto fail_server_cert;
        }
        free(pv);
        NLOG("Step 5 OK - Pairing successful!");
    }

    x509_crt_free(&server_cert);
    x509_crt_free(&client_cert);
    pk_free(&key); entropy_free(&entropy); ctr_drbg_free(&ctr_drbg);
    free(s.ptr);
    return 0;

fail_server_cert:
    x509_crt_free(&server_cert);
fail:
    x509_crt_free(&client_cert);
    pk_free(&key); entropy_free(&entropy); ctr_drbg_free(&ctr_drbg);
    free(s.ptr);
    return -1;
}


// Fetch server's appversion from /serverinfo (HTTP)
int hv_get_server_info(handshake_info_t *info) {
    char path[512];
    struct string s;
    snprintf(path, sizeof(path), "/serverinfo?uniqueid=%s", info->unique_id);
    if (ps3_http_request(info, path, &s) != 0) {
        NLOG("hv_get_server_info: failed");
        free(s.ptr);
        return -1;
    }
    char *appver = extract_xml(s.ptr, "appversion");
    if (appver) {
        strncpy(info->server_app_version, appver, sizeof(info->server_app_version) - 1);
        info->server_app_version[sizeof(info->server_app_version) - 1] = '\0';
        // Sanitize: LiStartConnection requires all 4 quad components to be >= 0.
        // Sunshine sometimes returns e.g. "7.1.431.-1" — replace negative component with 0
        char *p = info->server_app_version;
        while (*p) {
            if (p[0] == '-' && p[-1] == '.') { *p = '0'; }
            p++;
        }
        free(appver);
        NLOG("Server appversion (sanitized): %s", info->server_app_version);
    } else {
        strncpy(info->server_app_version, "7.1.431.0", sizeof(info->server_app_version) - 1);
        NLOG("Using default appversion: %s", info->server_app_version);
    }
    free(s.ptr);
    return 0;
}

// Fetch first app ID from Sunshine via HTTPS /applist
int hv_get_first_appid(handshake_info_t *info) {
    char path[512];
    struct string s;
    char uuid_str[40];
    snprintf(uuid_str, sizeof(uuid_str), "%08x-%04x-4%03x-%04x-%08x%04x",
        rand()&0xffffffff, rand()&0xffff, rand()&0x0fff,
        (rand()&0x3fff)|0x8000, rand()&0xffffffff, rand()&0xffff);
    snprintf(path, sizeof(path), "/applist?uniqueid=%s&uuid=%s", info->unique_id, uuid_str);
    if (ps3_https_request(info, path, &s) != 0) {
        NLOG("hv_get_first_appid: HTTPS request failed");
        free(s.ptr);
        return -1;
    }
    NLOG("Applist response: %.600s", s.ptr ? s.ptr : "");
    // Extract first <ID> tag
    char *id_str = extract_xml(s.ptr, "ID");
    int appid = -1;
    if (id_str) {
        appid = atoi(id_str);
        free(id_str);
        NLOG("First app ID: %d", appid);
    } else {
        NLOG("No app ID found in applist");
    }
    free(s.ptr);
    return appid;
}

// Helper: build the common launch/resume query params
static void build_launch_params(char *path, size_t pathsz,
                                const char *verb,
                                handshake_info_t *info, int app_id,
                                const char *rikey, int rikeyid) {
    char uuid_str[40];
    snprintf(uuid_str, sizeof(uuid_str), "%08x-%04x-4%03x-%04x-%08x%04x",
        rand()&0xffffffff, rand()&0xffff, rand()&0x0fff,
        (rand()&0x3fff)|0x8000, rand()&0xffffffff, rand()&0xffff);

    snprintf(path, pathsz,
        "/%s?uniqueid=%s&uuid=%s&appid=%d&mode=1280x720x60"
        "&additionalStates=1&sops=1"
        "&rikey=%s&rikeyid=%d"
        "&localAudioPlayMode=0&surroundAudioInfo=196610"
        "&remoteControllersBitmap=1&gcmap=1&corever=1",
        verb, info->unique_id, uuid_str, app_id, rikey, rikeyid);
}

// Send /cancel to end any existing session
static void hv_cancel(handshake_info_t *info) {
    char path[512];
    struct string s;
    char uuid_str[40];
    snprintf(uuid_str, sizeof(uuid_str), "%08x-%04x-4%03x-%04x-%08x%04x",
        rand()&0xffffffff, rand()&0xffff, rand()&0x0fff,
        (rand()&0x3fff)|0x8000, rand()&0xffffffff, rand()&0xffff);
    snprintf(path, sizeof(path), "/cancel?uniqueid=%s&uuid=%s", info->unique_id, uuid_str);
    NLOG("Sending /cancel to clear existing session...");
    if (ps3_https_request(info, path, &s) == 0) {
        NLOG("Cancel response: %.200s", s.ptr ? s.ptr : "");
    }
    free(s.ptr);
}

// Parse session URL from response and store in info
static void parse_session_url(handshake_info_t *info, const char *response) {
    char *session_url = extract_xml(response, "sessionUrl0");
    if (session_url) {
        strncpy(info->rtsp_session_url, session_url, sizeof(info->rtsp_session_url) - 1);
        NLOG("RTSP session URL: %s", info->rtsp_session_url);
        free(session_url);
    } else {
        NLOG("No sessionUrl0 — using default rtsp://%s:48010", info->address);
        snprintf(info->rtsp_session_url, sizeof(info->rtsp_session_url),
                 "rtsp://%s:48010", info->address);
    }
}

int hv_launch(handshake_info_t *info, int app_id, const char *rikey, int rikeyid) {
    char path[4096];
    struct string s;

    // --- Try /resume first (handles "already running" sessions)
    build_launch_params(path, sizeof(path), "resume", info, app_id, rikey, rikeyid);
    NLOG("Trying /resume: %s", path);
    if (ps3_https_request(info, path, &s) == 0 && s.ptr) {
        NLOG("Resume response: %.400s", s.ptr);
        char *rv = extract_xml(s.ptr, "resume");
        int resumed = rv ? atoi(rv) : 0;
        free(rv);
        if (resumed == 1) {
            NLOG("Session resumed successfully.");
            parse_session_url(info, s.ptr);
            free(s.ptr);
            return 0;
        }
        NLOG("/resume returned 0 or missing — will cancel and launch fresh.");
        free(s.ptr);
    } else {
        free(s.ptr);
        NLOG("/resume request failed.");
    }

    // --- /resume failed: cancel any lingering session
    hv_cancel(info);

    // Small delay to let Sunshine clean up
    sleep(1);

    // --- Try fresh /launch
    build_launch_params(path, sizeof(path), "launch", info, app_id, rikey, rikeyid);
    NLOG("Launch request: %s", path);
    init_string(&s);
    if (ps3_https_request(info, path, &s) != 0) {
        NLOG("Launch HTTPS failed.");
        free(s.ptr);
        return -1;
    }

    NLOG("Launch response: %.500s", s.ptr ? s.ptr : "");

    // Check gamesession success
    char *gsv = extract_xml(s.ptr, "gamesession");
    int gamesession = gsv ? atoi(gsv) : 0;
    free(gsv);
    if (gamesession == 0) {
        NLOG("Launch failed: gamesession=0 (Sunshine rejected launch)");
        free(s.ptr);
        return -1;
    }

    parse_session_url(info, s.ptr);
    free(s.ptr);
    NLOG("Launch successful!");
    return 0;
}


