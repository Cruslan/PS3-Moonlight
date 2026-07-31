#ifndef UI_H
#define UI_H

#include <stdint.h>

enum {
    UI_STATE_IP_ENTRY,
    UI_STATE_PAIRING,
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
int ui_get_vsync();
int ui_get_show_stats();
int ui_get_verbose();

#endif
