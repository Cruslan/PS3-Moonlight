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

void ui_init(int width, int height);
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
void ui_save_settings(void);
void ui_load_settings(void);
void ui_set_pairing_pin(const char *pin);
const char* ui_get_pairing_pin(void);

// App selection state helpers
void ui_set_app_list(const ps3_app_list_t *list);
int ui_get_selected_app_id(void);
const char* ui_get_selected_app_name(void);
int ui_is_app_selected(void);
void ui_reset_app_selection(void);

#endif
