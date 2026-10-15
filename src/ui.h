#ifndef UI_H
#define UI_H

#include <stdint.h>
#include "handshake.h"

enum {
    UI_STATE_IP_ENTRY,
    UI_STATE_SETTINGS,
    UI_STATE_PAIRING,
    UI_STATE_APPLIST,
    UI_STATE_STREAMING,
    UI_STATE_ERROR
};

void ui_init(int width, int height, int is_4_3);
void ui_push_log(const char *msg);
void ui_set_state(int state);
int ui_get_state();
int ui_is_running();
int ui_get_fps();
int ui_get_bitrate();
const char* ui_get_target_ip();
int ui_get_width();
int ui_get_height();
void ui_stop();
void ui_shutdown();
void ui_open_osk(void);
void ui_open_exit_dialog(void);
void ui_set_target_ip(const char *ip);
int ui_get_vsync();
int ui_get_show_stats();
int ui_get_verbose();
int ui_get_mouse_mode(void);
int ui_get_upscaler_mode(void);
void ui_set_upscaler_mode(int mode);
void ui_save_settings(void);
void ui_flush_settings_if_dirty(void);
void ui_load_settings(void);
void ui_set_pairing_pin(const char *pin);
const char* ui_get_pairing_pin(void);

// Aspect ratio & stream resolution settings
#define ASPECT_RATIO_16_9 0
#define ASPECT_RATIO_4_3  1

#define SCALING_MODE_SHADER   0 // Adaptive Shader Pipeline: CAS (upscale), 4-Tap Box (downscale), 1:1 Direct (equal)
#define SCALING_MODE_LEGACY   1 // Legacy Pipeline: Tiny3D Bilinear / Direct (no custom shaders)

int  ui_get_aspect_ratio(void);
int  ui_get_stream_res_idx(void);
void ui_set_stream_res_idx(int idx);
int  ui_get_stream_width(void);
int  ui_get_stream_height(void);
const char* ui_get_stream_res_name(void);
int  ui_get_scaling_mode(void);
void ui_set_scaling_mode(int mode);
const char* ui_get_scaling_mode_name(void);
const char* ui_get_active_scaling_name(void);
void ui_set_active_scaling_name(const char *name);

// App selection state helpers
void ui_set_app_list(const ps3_app_list_t *list);
int ui_get_selected_app_id(void);
const char* ui_get_selected_app_name(void);
int ui_is_app_selected(void);
void ui_reset_app_selection(void);

#endif
