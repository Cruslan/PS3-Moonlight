#ifndef HANDSHAKE_H
#define HANDSHAKE_H

#include <stdint.h>
#include <stdbool.h>
#include "Limelight.h"

typedef struct {
    char address[64];
    char client_cert_path[256];
    char client_key_path[256];
    char server_cert_hash_path[256];
    char unique_id[64];
    char rtsp_session_url[256];
    char server_app_version[32]; // e.g. "7.1.431.0" from /serverinfo
} handshake_info_t;

int hv_init(handshake_info_t *info, const char *address);
int hv_get_server_info(handshake_info_t *info);
int hv_get_first_appid(handshake_info_t *info);
int hv_is_paired(handshake_info_t *info);
int hv_pair(handshake_info_t *info, const char *pin);
int hv_launch(handshake_info_t *info, int app_id, const char *rikey, int rikeyid);

// Internal helpers (could be exposed if needed)
int hv_generate_credentials(handshake_info_t *info);

#endif
