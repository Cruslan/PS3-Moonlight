#include "ui.h"
#include <tiny3d.h>
#include <libfont.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <math.h>
#include <malloc.h>
#include <string.h>
#include <stdio.h>
#include <sys/thread.h>
#include <sys/mutex.h>
#include <sys/memory.h>
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>
#include <sysutil/sysutil.h>
#include <sysutil/osk.h>
#include <unistd.h>
#include <lv2/systime.h>
#include "input.h"
#include <Limelight.h>
#include "video.h"
#include "audio.h"

#define CONFIG_DIR  "/dev_hdd0/game/MNLT00001/USRDIR"
#define CONFIG_PATH "/dev_hdd0/game/MNLT00001/USRDIR/config.ini"

static sys_ppu_thread_t ui_thread;
static int ui_thread_started = 0;
static volatile int ui_running = 1;
static int ui_width = 1280;
static int ui_height = 720;
static float scale_x = 1.0f;
static float scale_y = 1.0f;
static float scale_font = 1.0f;
static volatile int ui_state = UI_STATE_IP_ENTRY;

#define SX(x) ((float)(x) * scale_x)
#define SY(y) ((float)(y) * scale_y)
#define SF(s) ((u32)(((float)(s) * scale_font < 8.0f) ? 8.0f : ((float)(s) * scale_font)))

// Host IP state
static int ip_octets[4] = {192, 168, 1, 1};
static char target_ip_str[64] = "192.168.1.1";

// Video and stream preferences
static int ui_fps = 60;
static int ui_bitrate_options[] = {2500, 5000, 10000};
#define NUM_BITRATE_OPTIONS (int)(sizeof(ui_bitrate_options) / sizeof(ui_bitrate_options[0]))
static int ui_bitrate_idx = 2; // Default: 10 Mbps (Maximum)
static int ui_vsync = 1; // Default: VSync ON (1)

// Navigation item counts for Main Menu and Settings Submenu
#define MAIN_MENU_ITEM_COUNT 3
static int active_main_item = 0; // 0: Sunshine Host IP, 1: Configure Settings, 2: Connect/Pair

#define SETTINGS_ITEM_COUNT 6
static int active_settings_item = 0; // 0: FPS, 1: Bitrate, 2: VSync, 3: Stats, 4: Verbose, 5: Back

static int frames_drawn_this_sec = 0;
static int ui_fps_actual = 0;
static u64 last_ui_time = 0;
static int show_stats = 0; // Default: Stats OFF (0)
static int ui_verbose = 0; // Default: Verbose Logging OFF (0)

// OSK management state
static sys_mem_container_t osk_container;
static int osk_container_created = 0;
static volatile int osk_active = 0;
static u16 osk_title[64];
static u16 osk_initial[64];
static u16 osk_output[64];

void ui_set_state(int state) { ui_state = state; }
int ui_get_state() { return ui_state; }
int ui_is_running() { return ui_running; }
int ui_get_fps() { return ui_fps; }
int ui_get_bitrate() { return ui_bitrate_options[ui_bitrate_idx]; }
int ui_get_width() { return ui_width; }
int ui_get_height() { return ui_height; }
void ui_stop() { ui_running = 0; }
int ui_get_vsync() { return ui_vsync; }
int ui_get_show_stats() { return show_stats; }
int ui_get_verbose() { return ui_verbose; }

static char pairing_pin_str[16] = "";

void ui_set_pairing_pin(const char *pin) {
    if (pin) {
        strncpy(pairing_pin_str, pin, sizeof(pairing_pin_str) - 1);
        pairing_pin_str[sizeof(pairing_pin_str) - 1] = '\0';
    } else {
        pairing_pin_str[0] = '\0';
    }
}

const char* ui_get_pairing_pin(void) {
    return pairing_pin_str;
}

// App Selection State
static ps3_app_list_t current_app_list;
static int active_app_idx = 0;
static volatile int app_selection_confirmed = 0;

void ui_set_app_list(const ps3_app_list_t *list) {
    if (list) {
        memcpy(&current_app_list, list, sizeof(current_app_list));
    } else {
        memset(&current_app_list, 0, sizeof(current_app_list));
    }
    active_app_idx = 0;
    app_selection_confirmed = 0;
}

int ui_get_selected_app_id(void) {
    if (current_app_list.count > 0 && active_app_idx >= 0 && active_app_idx < current_app_list.count) {
        return current_app_list.apps[active_app_idx].id;
    }
    return -1;
}

const char* ui_get_selected_app_name(void) {
    if (current_app_list.count > 0 && active_app_idx >= 0 && active_app_idx < current_app_list.count) {
        return current_app_list.apps[active_app_idx].name;
    }
    return "";
}

int ui_is_app_selected(void) {
    return app_selection_confirmed;
}

void ui_reset_app_selection(void) {
    app_selection_confirmed = 0;
}

void ui_set_target_ip(const char *str) {
    if (!str || !*str) return;
    int o[4];
    if (sscanf(str, "%d.%d.%d.%d", &o[0], &o[1], &o[2], &o[3]) == 4) {
        for (int i = 0; i < 4; i++) {
            if (o[i] >= 0 && o[i] <= 255) {
                ip_octets[i] = o[i];
            }
        }
        snprintf(target_ip_str, sizeof(target_ip_str), "%d.%d.%d.%d", 
                 ip_octets[0], ip_octets[1], ip_octets[2], ip_octets[3]);
    } else {
        strncpy(target_ip_str, str, sizeof(target_ip_str) - 1);
        target_ip_str[sizeof(target_ip_str) - 1] = '\0';
    }
}

const char* ui_get_target_ip() {
    if (target_ip_str[0] == '\0') {
        snprintf(target_ip_str, sizeof(target_ip_str), "%d.%d.%d.%d", 
                 ip_octets[0], ip_octets[1], ip_octets[2], ip_octets[3]);
    }
    return target_ip_str;
}

void ui_save_settings(void) {
    mkdir("/dev_hdd0/game/MNLT00001", 0700);
    mkdir(CONFIG_DIR, 0700);
    
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) {
        f = fopen("config.ini", "w");
    }
    if (f) {
        fprintf(f, "# PS3-Moonlight Configuration File\n");
        fprintf(f, "host_ip=%s\n", target_ip_str);
        fprintf(f, "fps=%d\n", ui_fps);
        fprintf(f, "bitrate_idx=%d\n", ui_bitrate_idx);
        fprintf(f, "vsync=%d\n", ui_vsync ? 1 : 0);
        fprintf(f, "stats=%d\n", show_stats ? 1 : 0);
        fprintf(f, "verbose=%d\n", ui_verbose ? 1 : 0);
        fflush(f);
        fclose(f);
    }
}

void ui_load_settings(void) {
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        f = fopen("config.ini", "r");
    }
    if (!f) return;
    
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == ';' || *p == '\0' || *p == '\r' || *p == '\n') continue;
        
        char *end = p + strlen(p) - 1;
        while (end >= p && (*end == '\r' || *end == '\n' || *end == ' ' || *end == '\t')) {
            *end = '\0';
            end--;
        }
        
        char key[64] = {0};
        char val[64] = {0};
        if (sscanf(p, "%63[^=]=%63s", key, val) == 2) {
            if (strcmp(key, "host_ip") == 0 || strcmp(key, "ip") == 0) {
                if (val[0] != '\0') {
                    ui_set_target_ip(val);
                }
            } else if (strcmp(key, "fps") == 0) {
                int v = atoi(val);
                if (v == 30 || v == 60) ui_fps = v;
            } else if (strcmp(key, "bitrate_idx") == 0) {
                int v = atoi(val);
                if (v >= 0 && v < NUM_BITRATE_OPTIONS) ui_bitrate_idx = v;
            } else if (strcmp(key, "vsync") == 0) {
                ui_vsync = (atoi(val) != 0);
            } else if (strcmp(key, "stats") == 0) {
                show_stats = (atoi(val) != 0);
            } else if (strcmp(key, "verbose") == 0) {
                ui_verbose = (atoi(val) != 0);
            }
        }
    }
    fclose(f);
}

static void ascii_to_utf16(u16 *dst, const char *src, int max_len) {
    int i = 0;
    while (src && src[i] && i < max_len - 1) {
        dst[i] = (u16)(unsigned char)src[i];
        i++;
    }
    dst[i] = 0;
}

static void utf16_to_ascii(char *dst, const u16 *src, int max_len) {
    int i = 0;
    while (src && src[i] && i < max_len - 1) {
        dst[i] = (char)(src[i] & 0xFF);
        i++;
    }
    dst[i] = 0;
}

static void ui_osk_callback(u64 status, u64 param, void *usrdata) {
    (void)param;
    (void)usrdata;
    if (status == SYSUTIL_OSK_LOADED) {
        ui_push_log("OSK: Virtual keyboard opened");
    } else if (status == SYSUTIL_OSK_DONE) {
        oskCallbackReturnParam ret_param;
        memset(&ret_param, 0, sizeof(ret_param));
        ret_param.str = osk_output;
        ret_param.len = 64;
        oskUnloadAsync(&ret_param);
        
        if (ret_param.res == OSK_OK) {
            char entered_text[64];
            utf16_to_ascii(entered_text, osk_output, sizeof(entered_text));
            ui_set_target_ip(entered_text);
            ui_save_settings();
            char log_msg[96];
            snprintf(log_msg, sizeof(log_msg), "Host saved: %s", target_ip_str);
            ui_push_log(log_msg);
        } else {
            ui_push_log("OSK: Finished");
        }
    } else if (status == SYSUTIL_OSK_INPUT_CANCELED) {
        oskCallbackReturnParam ret_param;
        memset(&ret_param, 0, sizeof(ret_param));
        oskUnloadAsync(&ret_param);
        ui_push_log("OSK: Virtual keyboard canceled");
    } else if (status == SYSUTIL_OSK_UNLOADED) {
        if (osk_container_created) {
            sysMemContainerDestroy(osk_container);
            osk_container_created = 0;
        }
        osk_active = 0;
        ui_push_log("OSK: Virtual keyboard closed");
    }
}

void ui_open_osk(void) {
    if (osk_active) return;
    
    // Allocate 4MB memory container required by GameOS OSK service
    if (sysMemContainerCreate(&osk_container, 4 * 1024 * 1024) != 0) {
        ui_push_log("OSK Error: Memory container allocation failed");
        return;
    }
    osk_container_created = 1;
    osk_active = 1;
    
    oskParam param;
    memset(&param, 0, sizeof(oskParam));
    param.allowedPanels = OSK_PANEL_TYPE_DEFAULT | OSK_PANEL_TYPE_ALPHABET | 
                          OSK_PANEL_TYPE_NUMERAL | OSK_PANEL_TYPE_URL | OSK_PANEL_TYPE_LATIN;
    param.firstViewPanel = OSK_PANEL_TYPE_URL;
    param.controlPoint.x = 0.0f;
    param.controlPoint.y = 0.0f;
    param.prohibitFlags = 0;

    ui_get_target_ip();
    ascii_to_utf16(osk_title, "Enter Sunshine Host IP / Address", 64);
    ascii_to_utf16(osk_initial, target_ip_str, 64);

    oskInputFieldInfo input_info;
    memset(&input_info, 0, sizeof(oskInputFieldInfo));
    input_info.message = osk_title;
    input_info.startText = osk_initial;
    input_info.maxLength = 63;

    oskSetKeyLayoutOption(OSK_10KEY_PANEL | OSK_FULLKEY_PANEL);
    oskSetInitialInputDevice(OSK_DEVICE_PAD);

    if (oskLoadAsync(osk_container, &param, &input_info) != 0) {
        ui_push_log("OSK Error: oskLoadAsync failed");
        sysMemContainerDestroy(osk_container);
        osk_container_created = 0;
        osk_active = 0;
    }
}

static void ui_loop(void *arg);

#define MAX_LOG_LINES 25
#define MAX_LOG_WIDTH 100

static char log_buffer[MAX_LOG_LINES][MAX_LOG_WIDTH];
static int log_count = 0;
static sys_mutex_t log_mutex;
static int log_mutex_initialized = 0;

// Simple 8x8 font bitmap (MSX style) - subset (32-127)
// This is a minimal fallback to avoid external dependencies
static const unsigned char font_8x8_basic[96][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // space
    {0x18,0x3c,0x3c,0x18,0x18,0x00,0x18,0x00}, // !
    {0x6c,0x6c,0x6c,0x00,0x00,0x00,0x00,0x00}, // "
    {0x6c,0x6c,0xfe,0x6c,0xfe,0x6c,0x6c,0x00}, // #
    {0x18,0x3e,0x60,0x3c,0x06,0x7c,0x18,0x00}, // $
    {0x00,0xc6,0xcc,0x18,0x30,0x66,0xc6,0x00}, // %
    {0x38,0x6c,0x38,0x76,0xdc,0xcc,0x76,0x00}, // &
    {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00}, // '
    {0x0c,0x18,0x30,0x30,0x30,0x18,0x0c,0x00}, // (
    {0x30,0x18,0x0c,0x0c,0x0c,0x18,0x30,0x00}, // )
    {0x00,0x66,0x3c,0xff,0x3c,0x66,0x00,0x00}, // *
    {0x00,0x18,0x18,0x7e,0x18,0x18,0x00,0x00}, // +
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, // ,
    {0x00,0x00,0x00,0xfe,0x00,0x00,0x00,0x00}, // -
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, // .
    {0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x00}, // /
    {0x3c,0x66,0x6e,0x7e,0x76,0x66,0x3c,0x00}, // 0
    {0x18,0x38,0x18,0x18,0x18,0x18,0x3c,0x00}, // 1
    {0x3c,0x66,0x06,0x0c,0x18,0x30,0x7e,0x00}, // 2
    {0x3c,0x66,0x06,0x1c,0x06,0x66,0x3c,0x00}, // 3
    {0x0c,0x1c,0x3c,0x6c,0xfe,0x0c,0x0c,0x00}, // 4
    {0x7e,0x60,0x7c,0x06,0x06,0x66,0x3c,0x00}, // 5
    {0x1c,0x30,0x60,0x7c,0x66,0x66,0x3c,0x00}, // 6
    {0x7e,0x06,0x0c,0x18,0x30,0x30,0x30,0x00}, // 7
    {0x3c,0x66,0x66,0x3c,0x66,0x66,0x3c,0x00}, // 8
    {0x3c,0x66,0x66,0x3e,0x06,0x0c,0x38,0x00}, // 9
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00}, // :
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30}, // ;
    {0x0c,0x18,0x30,0x60,0x30,0x18,0x0c,0x00}, // <
    {0x00,0x00,0x7e,0x00,0x7e,0x00,0x00,0x00}, // =
    {0x30,0x18,0x0c,0x06,0x0c,0x18,0x30,0x00}, // >
    {0x3c,0x66,0x06,0x0c,0x18,0x00,0x18,0x00}, // ?
    {0x3c,0x66,0x6e,0x6e,0x60,0x3e,0x00,0x00}, // @
    {0x18,0x3c,0x66,0x66,0x7e,0x66,0x66,0x00}, // A
    {0x7c,0x66,0x66,0x7c,0x66,0x66,0x7c,0x00}, // B
    {0x3c,0x66,0x60,0x60,0x60,0x66,0x3c,0x00}, // C
    {0x78,0x6c,0x66,0x66,0x66,0x6c,0x78,0x00}, // D
    {0x7e,0x60,0x60,0x7c,0x60,0x60,0x7e,0x00}, // E
    {0x7e,0x60,0x60,0x7c,0x60,0x60,0x60,0x00}, // F
    {0x3c,0x66,0x60,0x6e,0x66,0x66,0x3c,0x00}, // G
    {0x66,0x66,0x66,0x7e,0x66,0x66,0x66,0x00}, // H
    {0x3c,0x18,0x18,0x18,0x18,0x18,0x3c,0x00}, // I
    {0x1e,0x0c,0x0c,0x0c,0x0c,0x6c,0x38,0x00}, // J
    {0x66,0x6c,0x78,0x70,0x78,0x6c,0x66,0x00}, // K
    {0x60,0x60,0x60,0x60,0x60,0x60,0x7e,0x00}, // L
    {0x63,0x77,0x7f,0x6b,0x63,0x63,0x63,0x00}, // M
    {0x66,0x66,0x76,0x7e,0x6e,0x66,0x66,0x00}, // N
    {0x3c,0x66,0x66,0x66,0x66,0x66,0x3c,0x00}, // O
    {0x7c,0x66,0x66,0x7c,0x60,0x60,0x60,0x00}, // P
    {0x3c,0x66,0x66,0x66,0x66,0x3c,0x0e,0x02}, // Q
    {0x7c,0x66,0x66,0x7c,0x78,0x6c,0x66,0x00}, // R
    {0x3c,0x66,0x30,0x18,0x0c,0x66,0x3c,0x00}, // S
    {0x7e,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, // T
    {0x66,0x66,0x66,0x66,0x66,0x66,0x3c,0x00}, // U
    {0x66,0x66,0x66,0x66,0x66,0x3c,0x18,0x00}, // V
    {0x63,0x63,0x63,0x6b,0x7f,0x77,0x63,0x00}, // W
    {0x66,0x66,0x3c,0x18,0x3c,0x66,0x66,0x00}, // X
    {0x66,0x66,0x66,0x3c,0x18,0x18,0x18,0x00}, // Y
    {0x7e,0x06,0x0c,0x18,0x30,0x60,0x7e,0x00}, // Z
    {0x3c,0x30,0x30,0x30,0x30,0x30,0x3c,0x00}, // [
    {0x80,0x40,0x20,0x10,0x08,0x04,0x02,0x00}, // backslash
    {0x3c,0x0c,0x0c,0x0c,0x0c,0x0c,0x3c,0x00}, // ]
    {0x10,0x38,0x6c,0xc6,0x00,0x00,0x00,0x00}, // ^
    {0x00,0x00,0x00,0x00,0x00,0x00,0xff,0x00}, // _
    {0x30,0x18,0x0c,0x00,0x00,0x00,0x00,0x00}, // `
    {0x00,0x00,0x3c,0x06,0x3e,0x66,0x3e,0x00}, // a
    {0x60,0x60,0x7c,0x66,0x66,0x66,0x7c,0x00}, // b
    {0x00,0x00,0x3c,0x60,0x60,0x66,0x3c,0x00}, // c
    {0x06,0x06,0x3e,0x66,0x66,0x66,0x3e,0x00}, // d
    {0x00,0x00,0x3c,0x66,0x7e,0x60,0x3c,0x00}, // e
    {0x1c,0x30,0x7c,0x30,0x30,0x30,0x30,0x00}, // f
    {0x00,0x00,0x3e,0x66,0x66,0x3e,0x06,0x3c}, // g
    {0x60,0x60,0x7c,0x66,0x66,0x66,0x66,0x00}, // h
    {0x18,0x00,0x38,0x18,0x18,0x18,0x3c,0x00}, // i
    {0x06,0x00,0x1e,0x06,0x06,0x66,0x3c,0x00}, // j
    {0x60,0x60,0x66,0x6c,0x78,0x6c,0x66,0x00}, // k
    {0x30,0x30,0x30,0x30,0x30,0x30,0x1c,0x00}, // l
    {0x00,0x00,0x66,0x7f,0x7f,0x6b,0x63,0x00}, // m
    {0x00,0x00,0x7c,0x66,0x66,0x66,0x66,0x00}, // n
    {0x00,0x00,0x3c,0x66,0x66,0x66,0x3c,0x00}, // o
    {0x00,0x00,0x7c,0x66,0x66,0x7c,0x60,0x60}, // p
    {0x00,0x00,0x3e,0x66,0x66,0x3e,0x06,0x06}, // q
    {0x00,0x00,0x7c,0x66,0x60,0x60,0x60,0x00}, // r
    {0x00,0x00,0x3e,0x60,0x3c,0x06,0x7c,0x00}, // s
    {0x30,0x30,0x7c,0x30,0x30,0x30,0x1c,0x00}, // t
    {0x00,0x00,0x66,0x66,0x66,0x66,0x3e,0x00}, // u
    {0x00,0x00,0x66,0x66,0x66,0x3c,0x18,0x00}, // v
    {0x00,0x00,0x63,0x6b,0x7f,0x7f,0x36,0x00}, // w
    {0x00,0x00,0x66,0x3c,0x18,0x3c,0x66,0x00}, // x
    {0x00,0x00,0x66,0x66,0x66,0x3e,0x06,0x3c}, // y
    {0x00,0x00,0x7e,0x0c,0x18,0x30,0x7e,0x00}, // z
    {0x0c,0x18,0x18,0x70,0x18,0x18,0x0c,0x00}, // {
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, // |
    {0x30,0x18,0x18,0x0e,0x18,0x18,0x30,0x00}, // }
    {0x00,0x00,0x4c,0xb2,0x00,0x00,0x00,0x00}, // ~
};

static void * texture_mem = NULL;

void ui_init(int width, int height) {
    ui_width = (width > 0) ? width : 1280;
    ui_height = (height > 0) ? height : 720;
    scale_x = (float)ui_width / 1280.0f;
    scale_y = (float)ui_height / 720.0f;
    scale_font = (scale_x < scale_y) ? scale_x : scale_y;

    sys_mutex_attr_t attr;
    sysMutexAttrInitialize(attr);
    if (sysMutexCreate(&log_mutex, &attr) == 0) {
        log_mutex_initialized = 1;
    }

    // Load persisted Host IP and stream settings from HDD
    ui_load_settings();
    char cfg_log[96];
    snprintf(cfg_log, sizeof(cfg_log), "Config: Target Host [%s]", target_ip_str);
    ui_push_log(cfg_log);

    // Register sysutil callback on slot 1 for OSK keyboard lifecycle
    sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT1, ui_osk_callback, NULL);

    if (sysThreadCreate(&ui_thread, ui_loop, 0, 500, 0x10000,
                        THREAD_JOINABLE, "UI Thread") == 0) {
        ui_thread_started = 1;
    } else {
        ui_running = 0;
    }
}

void ui_push_log(const char *msg) {
    if (!msg) return;
    if (log_mutex_initialized) sysMutexLock(log_mutex, 0);
    if (log_count < MAX_LOG_LINES) {
        strncpy(log_buffer[log_count], msg, MAX_LOG_WIDTH - 1);
        log_buffer[log_count][MAX_LOG_WIDTH - 1] = '\0';
        log_count++;
    } else {
        // Shift buffer
        for (int i = 0; i < MAX_LOG_LINES - 1; i++) {
            memcpy(log_buffer[i], log_buffer[i + 1], MAX_LOG_WIDTH);
        }
        strncpy(log_buffer[MAX_LOG_LINES - 1], msg, MAX_LOG_WIDTH - 1);
        log_buffer[MAX_LOG_LINES - 1][MAX_LOG_WIDTH - 1] = '\0';
    }
    if (log_mutex_initialized) sysMutexUnlock(log_mutex);
}

static void draw_background_gradient() {
    // 1. Base Dark Gray Background (#303030 to #242424)
    tiny3d_SetPolygon(TINY3D_TRIANGLE_STRIP);
    tiny3d_VertexPos(0, 0, 65535);
    tiny3d_VertexFcolor(0.188f, 0.188f, 0.188f, 1.0f); // #303030
    tiny3d_VertexPos(ui_width, 0, 65535);
    tiny3d_VertexFcolor(0.188f, 0.188f, 0.188f, 1.0f);
    tiny3d_VertexPos(0, ui_height * 0.72f, 65535);
    tiny3d_VertexFcolor(0.141f, 0.141f, 0.141f, 1.0f); // #242424
    tiny3d_VertexPos(ui_width, ui_height * 0.72f, 65535);
    tiny3d_VertexFcolor(0.141f, 0.141f, 0.141f, 1.0f);
    tiny3d_End();

    // 2. Titlebar Header Bar (#3F51B5 Material Indigo Blue)
    tiny3d_SetPolygon(TINY3D_TRIANGLE_STRIP);
    tiny3d_VertexPos(0, 0, 65535);
    tiny3d_VertexFcolor(0.247f, 0.318f, 0.710f, 1.0f); // #3F51B5
    tiny3d_VertexPos(ui_width, 0, 65535);
    tiny3d_VertexFcolor(0.247f, 0.318f, 0.710f, 1.0f);
    tiny3d_VertexPos(0, SY(64), 65535);
    tiny3d_VertexFcolor(0.200f, 0.260f, 0.620f, 1.0f);
    tiny3d_VertexPos(ui_width, SY(64), 65535);
    tiny3d_VertexFcolor(0.200f, 0.260f, 0.620f, 1.0f);
    tiny3d_End();

    // 3. Titlebar Bottom Accent Line (#1A237E Deep Indigo)
    tiny3d_SetPolygon(TINY3D_TRIANGLE_STRIP);
    tiny3d_VertexPos(0, SY(64), 65535);
    tiny3d_VertexFcolor(0.102f, 0.137f, 0.494f, 1.0f);
    tiny3d_VertexPos(ui_width, SY(64), 65535);
    tiny3d_VertexFcolor(0.102f, 0.137f, 0.494f, 1.0f);
    tiny3d_VertexPos(0, SY(67), 65535);
    tiny3d_VertexFcolor(0.102f, 0.137f, 0.494f, 1.0f);
    tiny3d_VertexPos(ui_width, SY(67), 65535);
    tiny3d_VertexFcolor(0.102f, 0.137f, 0.494f, 1.0f);
    tiny3d_End();
}

// FreeType font engine state
static FT_Library ft_library = NULL;
static FT_Face ft_face = NULL;
static int font_is_ttf = 0;

static void render_ps_button_glyph(u8 chr, u8 *bitmap, short *w, short *h, short *y_correction) {
    *w = 26;
    *h = 28;
    *y_correction = 2; // Aligned with baseline
    
    float cx = 13.0f;
    float cy = 14.0f;
    float r_outer = 11.5f;
    float r_inner = 9.5f;
    
    for (int y = 0; y < 28; y++) {
        for (int x = 0; x < 26; x++) {
            float dx = (float)x - cx;
            float dy = (float)y - cy;
            float d = sqrtf(dx * dx + dy * dy);
            float alpha = 0.0f;
            
            if (chr == 1) {
                // Cross (✕) Button Badge
                if (d <= r_outer && d >= r_inner) {
                    float edge = (d > r_outer - 0.75f) ? (r_outer - d) / 0.75f : ((d < r_inner + 0.75f) ? (d - r_inner) / 0.75f : 1.0f);
                    if (edge > 0.0f) alpha = fmaxf(alpha, edge * 220.0f);
                }
                if (d < r_inner - 0.8f) {
                    float dist_d1 = fabsf(dx - dy) / 1.4142f;
                    float dist_d2 = fabsf(dx + dy) / 1.4142f;
                    float line_dist = fminf(dist_d1, dist_d2);
                    if (line_dist < 1.6f && d < 6.5f) {
                        float cross_alpha = (line_dist < 0.9f) ? 1.0f : (1.6f - line_dist) / 0.7f;
                        alpha = fmaxf(alpha, cross_alpha * 255.0f);
                    }
                }
            } else if (chr == 2) {
                // Circle (◯) Button Badge
                if (d <= r_outer && d >= r_inner) {
                    float edge = (d > r_outer - 0.75f) ? (r_outer - d) / 0.75f : ((d < r_inner + 0.75f) ? (d - r_inner) / 0.75f : 1.0f);
                    if (edge > 0.0f) alpha = fmaxf(alpha, edge * 220.0f);
                }
                float ir = 5.2f;
                float dist_ir = fabsf(d - ir);
                if (dist_ir < 1.6f) {
                    float ring_alpha = (dist_ir < 0.9f) ? 1.0f : (1.6f - dist_ir) / 0.7f;
                    alpha = fmaxf(alpha, ring_alpha * 255.0f);
                }
            } else if (chr == 3) {
                // Triangle (△) Button Badge
                if (d <= r_outer && d >= r_inner) {
                    float edge = (d > r_outer - 0.75f) ? (r_outer - d) / 0.75f : ((d < r_inner + 0.75f) ? (d - r_inner) / 0.75f : 1.0f);
                    if (edge > 0.0f) alpha = fmaxf(alpha, edge * 220.0f);
                }
                float ty = dy + 1.0f;
                float dist_bottom = fabsf(ty - 4.0f);
                float dist_left = fabsf(dx * 0.866f + ty * 0.5f + 1.5f);
                float dist_right = fabsf(-dx * 0.866f + ty * 0.5f + 1.5f);
                if (ty <= 4.2f && ty >= -5.5f && fabsf(dx) <= (ty + 5.5f) * 0.65f + 1.2f) {
                    float tri_dist = fminf(dist_bottom, fminf(dist_left, dist_right));
                    if (tri_dist < 1.5f) {
                        float tri_alpha = (tri_dist < 0.8f) ? 1.0f : (1.5f - tri_dist) / 0.7f;
                        alpha = fmaxf(alpha, tri_alpha * 255.0f);
                    }
                }
            } else if (chr == 4) {
                // Square (◻) Button Badge
                if (d <= r_outer && d >= r_inner) {
                    float edge = (d > r_outer - 0.75f) ? (r_outer - d) / 0.75f : ((d < r_inner + 0.75f) ? (d - r_inner) / 0.75f : 1.0f);
                    if (edge > 0.0f) alpha = fmaxf(alpha, edge * 220.0f);
                }
                float max_d = fmaxf(fabsf(dx), fabsf(dy));
                float sq_dist = fabsf(max_d - 4.8f);
                if (sq_dist < 1.5f && max_d <= 5.5f) {
                    float sq_alpha = (sq_dist < 0.8f) ? 1.0f : (1.5f - sq_dist) / 0.7f;
                    alpha = fmaxf(alpha, sq_alpha * 255.0f);
                }
            } else if (chr == 5) {
                // D-Pad Up/Down (↕) Icon
                if (fabsf(dx) <= 2.2f && fabsf(dy) <= 8.5f) {
                    alpha = 240.0f;
                }
                if (dy < -2.0f && dy >= -9.5f) {
                    float arrow_w = (dy + 9.5f) * 0.9f;
                    if (fabsf(dx) <= arrow_w + 0.8f) {
                        alpha = 255.0f;
                    }
                }
                if (dy > 2.0f && dy <= 9.5f) {
                    float arrow_w = (9.5f - dy) * 0.9f;
                    if (fabsf(dx) <= arrow_w + 0.8f) {
                        alpha = 255.0f;
                    }
                }
            } else if (chr == 6) {
                // Heart (♥) Symbol
                float d_left = sqrtf((dx + 4.0f) * (dx + 4.0f) + (dy + 2.5f) * (dy + 2.5f));
                float d_right = sqrtf((dx - 4.0f) * (dx - 4.0f) + (dy + 2.5f) * (dy + 2.5f));
                if (d_left <= 4.8f) {
                    float edge = (d_left > 4.0f) ? (4.8f - d_left) / 0.8f : 1.0f;
                    alpha = fmaxf(alpha, edge * 255.0f);
                }
                if (d_right <= 4.8f) {
                    float edge = (d_right > 4.0f) ? (4.8f - d_right) / 0.8f : 1.0f;
                    alpha = fmaxf(alpha, edge * 255.0f);
                }
                if (dy >= -2.5f && dy <= 8.5f) {
                    float max_w = (8.5f - dy) * 0.77f;
                    if (fabsf(dx) <= max_w + 0.8f) {
                        float edge = (fabsf(dx) > max_w) ? (max_w + 0.8f - fabsf(dx)) / 0.8f : 1.0f;
                        alpha = fmaxf(alpha, edge * 255.0f);
                    }
                }
            }
            
            if (alpha > 255.0f) alpha = 255.0f;
            bitmap[y * 32 + x] = (u8)alpha;
        }
    }
}

static void ttf_render_callback(u8 chr, u8 *bitmap, short *w, short *h, short *y_correction) {
    memset(bitmap, 0, 32 * 32);
    *w = 0;
    *h = 0;
    *y_correction = 0;
    
    // Check for Custom Glyph slots (1: Cross, 2: Circle, 3: Triangle, 4: Square, 5: D-Pad, 6: Heart)
    if (chr >= 1 && chr <= 6) {
        render_ps_button_glyph(chr, bitmap, w, h, y_correction);
        return;
    }
    
    if (!ft_face) return;
    
    // Custom spacing for space character
    if (chr == ' ') {
        *w = 10;
        *h = 1;
        *y_correction = 0;
        return;
    }
    
    FT_UInt glyph_index = FT_Get_Char_Index(ft_face, (FT_ULong)chr);
    if (glyph_index == 0) return;
    
    if (FT_Load_Glyph(ft_face, glyph_index, FT_LOAD_DEFAULT)) return;
    if (FT_Render_Glyph(ft_face->glyph, FT_RENDER_MODE_NORMAL)) return;
    
    FT_GlyphSlot slot = ft_face->glyph;
    int bw = slot->bitmap.width;
    int bh = slot->bitmap.rows;
    if (bw > 32) bw = 32;
    if (bh > 32) bh = 32;
    
    *w = (short)(slot->advance.x >> 6);
    if (*w <= 0) *w = (short)(bw + 2);
    *h = (short)bh;
    *y_correction = (short)(26 - slot->bitmap_top);
    if (*y_correction < 0) *y_correction = 0;
    
    for (int y = 0; y < bh; y++) {
        for (int x = 0; x < bw; x++) {
            u8 val = slot->bitmap.buffer[y * slot->bitmap.pitch + x];
            bitmap[y * 32 + x] = val;
        }
    }
}

static void ui_init_fonts() {
    ResetFont();
    font_is_ttf = 0;
    
    // PlayStation 3 internal system fonts (ordered by preference)
    static const char *font_candidates[] = {
        "/dev_flash/data/font/SCE-PS3-RD-R-LATIN.TTF",
        "/dev_flash/data/font/SCE-PS3-SR-R-LATIN.TTF",
        "/dev_flash/data/font/SCE-PS3-VR-R-LATIN.TTF",
        "/dev_flash/data/font/SCE-PS3-DH-R-CGB.TTF",
        NULL
    };
    
    if (FT_Init_FreeType(&ft_library) == 0) {
        for (int i = 0; font_candidates[i] != NULL; i++) {
            if (FT_New_Face(ft_library, font_candidates[i], 0, &ft_face) == 0) {
                FT_Set_Pixel_Sizes(ft_face, 0, 30);
                
                texture_mem = tiny3d_AllocTexture(1024 * 1024);
                if (texture_mem) {
                    AddFontFromTTF((u8 *)texture_mem, 1, 127, 32, 32, ttf_render_callback);
                    font_is_ttf = 1;
                    char log_buf[96];
                    snprintf(log_buf, sizeof(log_buf), "Font: Loaded FreeType TTF (%s)", font_candidates[i]);
                    ui_push_log(log_buf);
                    break;
                }
            }
        }
    }
    
    // Fallback to built-in bitmap font if TTF failed or files unavailable
    if (!font_is_ttf) {
        if (!texture_mem) {
            texture_mem = tiny3d_AllocTexture(64 * 1024);
        }
        if (texture_mem) {
            AddFontFromBitmapArray((u8 *)font_8x8_basic, (u8 *)texture_mem, 32, 127, 8, 8, 1, BIT7_FIRST_PIXEL);
            ui_push_log("Font: Loaded fallback 8x8 bitmap font");
        }
    }
    
    SetCurrentFont(0);
    SetFontSize(SF(16), SF(16));
    SetFontColor(0xffffffff, 0x00000000);
}

static void ui_loop(void *arg) {
    (void)arg;
    ps3_pad_state_t pad;
    
    tiny3d_Init(1024 * 1024); // 1MB vertex buffer
    ui_init_fonts();
    
    // Enable Alpha Test and Blending to eliminate solid black texture boxes around font glyphs
    tiny3d_AlphaTest(1, 0, TINY3D_ALPHA_FUNC_GREATER);
    tiny3d_BlendFunc(1, 
        TINY3D_BLEND_FUNC_SRC_RGB_SRC_ALPHA | TINY3D_BLEND_FUNC_SRC_ALPHA_SRC_ALPHA,
        TINY3D_BLEND_FUNC_DST_RGB_ONE_MINUS_SRC_ALPHA | TINY3D_BLEND_FUNC_DST_ALPHA_ONE_MINUS_SRC_ALPHA,
        TINY3D_BLEND_RGB_FUNC_ADD | TINY3D_BLEND_ALPHA_FUNC_ADD);
    
    while (ui_running) {
        // Pump sysutil event callbacks to service OSK and GameOS events
        sysUtilCheckCallback();
        ps3input_get_data(&pad);
        
        tiny3d_Clear(0x303030ff, TINY3D_CLEAR_ALL);
        
        // Handle input for UI menu states when OSK dialog is not actively capturing input
        if (ui_state == UI_STATE_IP_ENTRY) {
            if (!osk_active) {
                // Vertical navigation across main menu rows
                if (pad.buttons_pressed & UP_FLAG) {
                    active_main_item = (active_main_item + MAIN_MENU_ITEM_COUNT - 1) % MAIN_MENU_ITEM_COUNT;
                }
                if (pad.buttons_pressed & DOWN_FLAG) {
                    active_main_item = (active_main_item + 1) % MAIN_MENU_ITEM_COUNT;
                }
                
                // Action handling per main menu item
                if (active_main_item == 0) {
                    // Host IP row: Open native OSK keyboard on Cross, Left, or Right
                    if ((pad.buttons_pressed & A_FLAG) || (pad.buttons_pressed & LEFT_FLAG) || (pad.buttons_pressed & RIGHT_FLAG)) {
                        ui_open_osk();
                    }
                } else if (active_main_item == 1) {
                    // Settings Submenu: Enter stream configuration menu
                    if ((pad.buttons_pressed & A_FLAG) || (pad.buttons_pressed & RIGHT_FLAG)) {
                        ui_state = UI_STATE_SETTINGS;
                        active_settings_item = 0;
                    }
                } else if (active_main_item == 2) {
                    // Connect / Pair action button
                    if (pad.buttons_pressed & A_FLAG) {
                        ui_state = UI_STATE_PAIRING;
                    }
                }
                
                // START button initiates connection immediately from anywhere in main menu
                if (pad.buttons_pressed & PLAY_FLAG) {
                    ui_state = UI_STATE_PAIRING;
                }

                // Circle button exits Moonlight to XMB
                if (pad.buttons_pressed & B_FLAG) {
                    ui_push_log("Exiting to PS3 XMB...");
                    ui_stop();
                }
            }
        } else if (ui_state == UI_STATE_SETTINGS) {
            // Vertical navigation across settings submenu rows
            if (pad.buttons_pressed & UP_FLAG) {
                active_settings_item = (active_settings_item + SETTINGS_ITEM_COUNT - 1) % SETTINGS_ITEM_COUNT;
            }
            if (pad.buttons_pressed & DOWN_FLAG) {
                active_settings_item = (active_settings_item + 1) % SETTINGS_ITEM_COUNT;
            }
            
            if (active_settings_item == 0) {
                // Target FPS toggle (30 <-> 60)
                if ((pad.buttons_pressed & A_FLAG) || (pad.buttons_pressed & LEFT_FLAG) || (pad.buttons_pressed & RIGHT_FLAG)) {
                    ui_fps = (ui_fps == 30) ? 60 : 30;
                    ui_save_settings();
                }
            } else if (active_settings_item == 1) {
                // Target Bitrate selection
                if ((pad.buttons_pressed & A_FLAG) || (pad.buttons_pressed & RIGHT_FLAG)) {
                    ui_bitrate_idx = (ui_bitrate_idx + 1) % NUM_BITRATE_OPTIONS;
                    ui_save_settings();
                }
                if (pad.buttons_pressed & LEFT_FLAG) {
                    ui_bitrate_idx = (ui_bitrate_idx + NUM_BITRATE_OPTIONS - 1) % NUM_BITRATE_OPTIONS;
                    ui_save_settings();
                }
            } else if (active_settings_item == 2) {
                // VSync toggle
                if ((pad.buttons_pressed & A_FLAG) || (pad.buttons_pressed & LEFT_FLAG) || (pad.buttons_pressed & RIGHT_FLAG)) {
                    ui_vsync = !ui_vsync;
                    gcmSetFlipMode(ui_vsync ? GCM_FLIP_VSYNC : GCM_FLIP_HSYNC);
                    ui_save_settings();
                }
            } else if (active_settings_item == 3) {
                // Stats overlay toggle
                if ((pad.buttons_pressed & A_FLAG) || (pad.buttons_pressed & LEFT_FLAG) || (pad.buttons_pressed & RIGHT_FLAG)) {
                    show_stats = !show_stats;
                    ui_save_settings();
                }
            } else if (active_settings_item == 4) {
                // Verbose logging toggle
                if ((pad.buttons_pressed & A_FLAG) || (pad.buttons_pressed & LEFT_FLAG) || (pad.buttons_pressed & RIGHT_FLAG)) {
                    ui_verbose = !ui_verbose;
                    ui_save_settings();
                }
            } else if (active_settings_item == 5) {
                // Back to Main Menu
                if (pad.buttons_pressed & A_FLAG) {
                    ui_save_settings();
                    ui_state = UI_STATE_IP_ENTRY;
                }
            }
            
            // Circle button returns to main menu from anywhere in settings
            if (pad.buttons_pressed & B_FLAG) {
                ui_save_settings();
                ui_state = UI_STATE_IP_ENTRY;
            }
        } else if (ui_state == UI_STATE_PAIRING) {
            // Circle button to cancel pairing attempt
            if (pad.buttons_pressed & B_FLAG) {
                ui_state = UI_STATE_IP_ENTRY;
            }
        } else if (ui_state == UI_STATE_APPLIST) {
            if (current_app_list.count > 0) {
                if (pad.buttons_pressed & UP_FLAG) {
                    active_app_idx = (active_app_idx + current_app_list.count - 1) % current_app_list.count;
                }
                if (pad.buttons_pressed & DOWN_FLAG) {
                    active_app_idx = (active_app_idx + 1) % current_app_list.count;
                }
                if (pad.buttons_pressed & A_FLAG) {
                    app_selection_confirmed = 1;
                }
            }
            // Circle button returns to main menu
            if (pad.buttons_pressed & B_FLAG) {
                app_selection_confirmed = 0;
                ui_state = UI_STATE_IP_ENTRY;
            }
        }

        if (show_stats) {
            u64 now = sysGetSystemTime();
            if (last_ui_time == 0) last_ui_time = now;
            if (now - last_ui_time >= 1000000) {
                ui_fps_actual = frames_drawn_this_sec;
                frames_drawn_this_sec = 0;
                last_ui_time = now;
            }
            frames_drawn_this_sec++;
        }

        // 1. Draw UI / Video (Top 70%)
        tiny3d_UserViewportSurface(1, (float)ui_width, (float)ui_height);
        tiny3d_Project2D();
        
        // Ensure transparent alpha blending is active for all 2D text and menu overlays
        tiny3d_AlphaTest(1, 0, TINY3D_ALPHA_FUNC_GREATER);
        tiny3d_BlendFunc(1, 
            TINY3D_BLEND_FUNC_SRC_RGB_SRC_ALPHA | TINY3D_BLEND_FUNC_SRC_ALPHA_SRC_ALPHA,
            TINY3D_BLEND_FUNC_DST_RGB_ONE_MINUS_SRC_ALPHA | TINY3D_BLEND_FUNC_DST_ALPHA_ONE_MINUS_SRC_ALPHA,
            TINY3D_BLEND_RGB_FUNC_ADD | TINY3D_BLEND_ALPHA_FUNC_ADD);
        
        if (ui_state == UI_STATE_STREAMING) {
            ps3video_draw();
            
            // Draw Video Performance Stats HUD (Top Left) if enabled
            if (show_stats) {
                float sx = SX(30);
                float sy = SY(30);
                float line_h = SY(20);
                float hud_w = SX(360);
                float hud_h = 11.5f * line_h;

                // Semi-transparent dark HUD container background (#121212 with 85% alpha)
                tiny3d_SetPolygon(TINY3D_TRIANGLE_STRIP);
                tiny3d_VertexPos(sx - SX(10), sy - SY(10), 65535);
                tiny3d_VertexFcolor(0.07f, 0.07f, 0.07f, 0.85f);
                tiny3d_VertexPos(sx + hud_w, sy - SY(10), 65535);
                tiny3d_VertexFcolor(0.07f, 0.07f, 0.07f, 0.85f);
                tiny3d_VertexPos(sx - SX(10), sy + hud_h, 65535);
                tiny3d_VertexFcolor(0.07f, 0.07f, 0.07f, 0.85f);
                tiny3d_VertexPos(sx + hud_w, sy + hud_h, 65535);
                tiny3d_VertexFcolor(0.07f, 0.07f, 0.07f, 0.85f);
                tiny3d_End();

                // Top Accent Line (#3F51B5)
                tiny3d_SetPolygon(TINY3D_TRIANGLE_STRIP);
                tiny3d_VertexPos(sx - SX(10), sy - SY(10), 65535);
                tiny3d_VertexFcolor(0.247f, 0.318f, 0.710f, 1.0f);
                tiny3d_VertexPos(sx + hud_w, sy - SY(10), 65535);
                tiny3d_VertexFcolor(0.247f, 0.318f, 0.710f, 1.0f);
                tiny3d_VertexPos(sx - SX(10), sy - SY(8), 65535);
                tiny3d_VertexFcolor(0.247f, 0.318f, 0.710f, 1.0f);
                tiny3d_VertexPos(sx + hud_w, sy - SY(8), 65535);
                tiny3d_VertexFcolor(0.247f, 0.318f, 0.710f, 1.0f);
                tiny3d_End();

                SetCurrentFont(0);
                SetFontSize(SF(16), SF(16));
                SetFontColor(0x00ff00ff, 0); // Pure Matrix Green (RGBA: RR=0, GG=255, BB=0, AA=255)

                DrawFormatString(sx, sy + 0 * line_h, "Rendered FPS: %d", ps3video_get_current_fps());
                DrawFormatString(sx, sy + 1 * line_h, "Decoded FPS: %d", ps3video_get_decoded_fps());
                DrawFormatString(sx, sy + 2 * line_h, "UI Loop FPS: %d", ui_fps_actual);
                DrawFormatString(sx, sy + 3 * line_h, "Decode Latency: %d ms", ps3video_get_decode_latency());
                DrawFormatString(sx, sy + 4 * line_h, "Render Latency: %d ms", ps3video_get_render_latency());
                DrawFormatString(sx, sy + 5 * line_h, "Network Latency: %d ms", ps3video_get_net_latency() / 2);
                DrawFormatString(sx, sy + 6 * line_h, "Total Latency: %d ms", (ps3video_get_net_latency() / 2) + ps3video_get_decode_latency() + ps3video_get_render_latency());
                DrawFormatString(sx, sy + 7 * line_h, "Resolution: %dx%d", ui_width, ui_height);
                DrawFormatString(sx, sy + 8 * line_h, "Target FPS: %d FPS", ui_get_fps());
                DrawFormatString(sx, sy + 9 * line_h, "Bitrate: %d Mbps", ui_get_bitrate() / 1000);
                
                /* Real-time Hardware Telemetry Stream Link Activity Monitor */
                u32 total_frames = ps3video_get_total_decoded_frames();
                int pulse_phase = (int)((total_frames / 4) % 4);
                const char* spinner = "";
                switch (pulse_phase) {
                    case 0: spinner = "[ - ]"; break;
                    case 1: spinner = "[ \\ ]"; break;
                    case 2: spinner = "[ | ]"; break;
                    case 3: spinner = "[ / ]"; break;
                }
                DrawFormatString(sx, sy + 10 * line_h, "Stream Link: ACTIVE %s", spinner);
            }
        } else {
            draw_background_gradient();
            
            if (ui_state == UI_STATE_IP_ENTRY) {
                // Title inside #3F51B5 header bar
                SetFontSize(SF(26), SF(26));
                SetFontColor(0xffffffff, 0);
                DrawString(SX(40), SY(18), "Moonlight PS3");
                
                // Row 0: Sunshine Host
                SetFontSize(SF(24), SF(24));
                SetFontColor((active_main_item == 0) ? 0xff82b1ff : 0xffb0bec5, 0);
                float next_x = DrawString(SX(60), SY(125), "Sunshine Host:");
                
                SetFontColor((active_main_item == 0) ? 0xff82b1ff : 0xffffffff, 0);
                DrawFormatString(next_x + SX(20), SY(125), "[ %s ]", target_ip_str);

                // Row 1: Settings Sub-menu Link
                SetFontSize(SF(24), SF(24));
                SetFontColor((active_main_item == 1) ? 0xff82b1ff : 0xffffffff, 0);
                DrawString(SX(60), SY(185), "[ CONFIGURE STREAM SETTINGS ]");
                
                // Active settings summary preview
                SetFontSize(SF(18), SF(18));
                SetFontColor(0xff9e9e9e, 0);
                int kbps = ui_bitrate_options[ui_bitrate_idx];
                if (kbps % 1000 == 0) {
                    DrawFormatString(SX(60), SY(225), "Current: %d FPS  |  %d Mbps  |  VSync: %s  |  Stats: %s", 
                                     ui_fps, kbps / 1000, ui_vsync ? "ON" : "OFF", show_stats ? "ON" : "OFF");
                } else {
                    DrawFormatString(SX(60), SY(225), "Current: %d FPS  |  %.1f Mbps  |  VSync: %s  |  Stats: %s", 
                                     ui_fps, (float)kbps / 1000.0f, ui_vsync ? "ON" : "OFF", show_stats ? "ON" : "OFF");
                }

                // Row 2: Connect / Pair Action Button
                SetFontSize(SF(24), SF(24));
                SetFontColor((active_main_item == 2) ? 0xff82b1ff : 0xffffffff, 0);
                DrawString(SX(60), SY(280), "[ CONNECT / PAIR TO HOST ]");

                // Clean controls legend
                SetFontSize(SF(18), SF(18));
                SetFontColor(0xff9e9e9e, 0);
                DrawString(SX(60), SY(445), "\x05 Navigate   |   \x01 Select   |   \x02 Exit to XMB");
            } else if (ui_state == UI_STATE_SETTINGS) {
                // Title inside #3F51B5 header bar
                SetFontSize(SF(26), SF(26));
                SetFontColor(0xffffffff, 0);
                DrawString(SX(40), SY(18), "Moonlight PS3  -  Stream Settings");

                // Row 0: Target FPS
                SetFontSize(SF(22), SF(22));
                SetFontColor((active_settings_item == 0) ? 0xff82b1ff : 0xffb0bec5, 0);
                DrawString(SX(60), SY(115), "Target FPS:");
                
                SetFontColor((active_settings_item == 0) ? 0xff82b1ff : 0xffffffff, 0);
                DrawFormatString(SX(430), SY(115), "[ %d FPS ]", ui_fps);

                // Row 1: Target Bitrate
                SetFontColor((active_settings_item == 1) ? 0xff82b1ff : 0xffb0bec5, 0);
                DrawString(SX(60), SY(155), "Target Bitrate:");
                
                SetFontColor((active_settings_item == 1) ? 0xff82b1ff : 0xffffffff, 0);
                int kbps = ui_bitrate_options[ui_bitrate_idx];
                if (kbps % 1000 == 0) {
                    DrawFormatString(SX(430), SY(155), "[ %d Mbps ]", kbps / 1000);
                } else {
                    DrawFormatString(SX(430), SY(155), "[ %.1f Mbps ]", (float)kbps / 1000.0f);
                }

                // Row 2: VSync Mode
                SetFontColor((active_settings_item == 2) ? 0xff82b1ff : 0xffb0bec5, 0);
                DrawString(SX(60), SY(195), "VSync Mode:");
                
                SetFontColor((active_settings_item == 2) ? 0xff82b1ff : 0xffffffff, 0);
                DrawFormatString(SX(430), SY(195), "[ %s ]", ui_vsync ? "ON (Smooth 60Hz)" : "OFF (Low Latency)");

                // Row 3: Stats Overlay
                SetFontColor((active_settings_item == 3) ? 0xff82b1ff : 0xffb0bec5, 0);
                DrawString(SX(60), SY(235), "Stats Overlay:");
                
                SetFontColor((active_settings_item == 3) ? 0xff82b1ff : 0xffffffff, 0);
                DrawFormatString(SX(430), SY(235), "[ %s ]", show_stats ? "ON" : "OFF");

                // Row 4: Verbose Logging
                SetFontColor((active_settings_item == 4) ? 0xff82b1ff : 0xffb0bec5, 0);
                DrawString(SX(60), SY(275), "Verbose Logging:");
                
                SetFontColor((active_settings_item == 4) ? 0xff82b1ff : 0xffffffff, 0);
                DrawFormatString(SX(430), SY(275), "[ %s ]", ui_verbose ? "ON" : "OFF");

                // Row 5: Back to Main Menu Button
                SetFontSize(SF(22), SF(22));
                SetFontColor((active_settings_item == 5) ? 0xff82b1ff : 0xffffffff, 0);
                DrawString(SX(60), SY(335), "[ BACK TO MAIN MENU ]");

                // Clean controls legend
                SetFontSize(SF(18), SF(18));
                SetFontColor(0xff9e9e9e, 0);
                DrawString(SX(60), SY(445), "\x05 Navigate   |   \x01 Select / Change   |   \x02 Back");
            } else if (ui_state == UI_STATE_PAIRING) {
                // Title inside #3F51B5 header bar
                SetFontSize(SF(26), SF(26));
                SetFontColor(0xffffffff, 0);
                DrawString(SX(40), SY(18), "Moonlight PS3  -  Device Pairing");

                if (pairing_pin_str[0] != '\0') {
                    // Heading
                    SetFontSize(SF(24), SF(24));
                    SetFontColor(0xffffffff, 0);
                    DrawString(SX(60), SY(110), "Sunshine Pairing Required");

                    // Subheading instructions
                    SetFontSize(SF(18), SF(18));
                    SetFontColor(0xffb0bec5, 0);
                    DrawString(SX(60), SY(150), "Open Sunshine Web UI (PIN Tab) and enter this PIN:");

                    // PIN Badge Card Container (#1E1E1E background with #3F51B5 top accent line)
                    tiny3d_SetPolygon(TINY3D_TRIANGLE_STRIP);
                    tiny3d_VertexPos(SX(60), SY(190), 65535);
                    tiny3d_VertexFcolor(0.12f, 0.12f, 0.12f, 0.95f);
                    tiny3d_VertexPos(SX(400), SY(190), 65535);
                    tiny3d_VertexFcolor(0.12f, 0.12f, 0.12f, 0.95f);
                    tiny3d_VertexPos(SX(60), SY(275), 65535);
                    tiny3d_VertexFcolor(0.12f, 0.12f, 0.12f, 0.95f);
                    tiny3d_VertexPos(SX(400), SY(275), 65535);
                    tiny3d_VertexFcolor(0.12f, 0.12f, 0.12f, 0.95f);
                    tiny3d_End();

                    // Top Accent Line (#3F51B5)
                    tiny3d_SetPolygon(TINY3D_TRIANGLE_STRIP);
                    tiny3d_VertexPos(SX(60), SY(190), 65535);
                    tiny3d_VertexFcolor(0.247f, 0.318f, 0.710f, 1.0f);
                    tiny3d_VertexPos(SX(400), SY(190), 65535);
                    tiny3d_VertexFcolor(0.247f, 0.318f, 0.710f, 1.0f);
                    tiny3d_VertexPos(SX(60), SY(193), 65535);
                    tiny3d_VertexFcolor(0.247f, 0.318f, 0.710f, 1.0f);
                    tiny3d_VertexPos(SX(400), SY(193), 65535);
                    tiny3d_VertexFcolor(0.247f, 0.318f, 0.710f, 1.0f);
                    tiny3d_End();

                    // PIN Text in large bold highlight
                    SetFontSize(SF(34), SF(34));
                    SetFontColor(0xff82b1ff, 0); // Rose / Pink highlight
                    DrawFormatString(SX(85), SY(218), "PIN:  %s", pairing_pin_str);

                    // Waiting status
                    SetFontSize(SF(18), SF(18));
                    SetFontColor(0xff9e9e9e, 0);
                    DrawString(SX(60), SY(305), "Waiting for confirmation from host...");
                } else {
                    SetFontSize(SF(24), SF(24));
                    SetFontColor(0xff82b1ff, 0);
                    DrawString(SX(60), SY(180), "Connecting to Sunshine Host...");

                    SetFontSize(SF(18), SF(18));
                    SetFontColor(0xffb0bec5, 0);
                    DrawString(SX(60), SY(220), "Initializing handshake session...");
                }

                // Bottom cancel button legend
                SetFontSize(SF(18), SF(18));
                SetFontColor(0xff9e9e9e, 0);
                DrawString(SX(60), SY(445), "\x02 Cancel Pairing");
            } else if (ui_state == UI_STATE_APPLIST) {
                // Title inside #3F51B5 header bar
                SetFontSize(SF(26), SF(26));
                SetFontColor(0xffffffff, 0);
                DrawString(SX(40), SY(18), "Moonlight PS3  -  Host Applications");

                // Subtitle
                SetFontSize(SF(22), SF(22));
                SetFontColor(0xffffffff, 0);
                DrawString(SX(60), SY(95), "Select Game or Application to Stream:");

                if (current_app_list.count > 0) {
                    SetFontSize(SF(18), SF(18));
                    SetFontColor(0xffb0bec5, 0);
                    DrawFormatString(SX(480), SY(95), "[ %d / %d ]", active_app_idx + 1, current_app_list.count);

                    // Render visible applications list
                    #define MAX_VISIBLE_APPS 6
                    int start_idx = 0;
                    if (active_app_idx >= MAX_VISIBLE_APPS) {
                        start_idx = active_app_idx - MAX_VISIBLE_APPS + 1;
                    }
                    int visible_count = current_app_list.count - start_idx;
                    if (visible_count > MAX_VISIBLE_APPS) visible_count = MAX_VISIBLE_APPS;

                    for (int i = 0; i < visible_count; i++) {
                        int idx = start_idx + i;
                        float row_y = SY(135) + (i * SY(48));

                        if (idx == active_app_idx) {
                            // Active selection card background (#242424 with pink accent border)
                            tiny3d_SetPolygon(TINY3D_TRIANGLE_STRIP);
                            tiny3d_VertexPos(SX(55), row_y, 65535);
                            tiny3d_VertexFcolor(0.16f, 0.16f, 0.16f, 0.95f);
                            tiny3d_VertexPos(SX(850), row_y, 65535);
                            tiny3d_VertexFcolor(0.16f, 0.16f, 0.16f, 0.95f);
                            tiny3d_VertexPos(SX(55), row_y + SY(40), 65535);
                            tiny3d_VertexFcolor(0.16f, 0.16f, 0.16f, 0.95f);
                            tiny3d_VertexPos(SX(850), row_y + SY(40), 65535);
                            tiny3d_VertexFcolor(0.16f, 0.16f, 0.16f, 0.95f);
                            tiny3d_End();

                            // Left Accent Line (#FF82B1)
                            tiny3d_SetPolygon(TINY3D_TRIANGLE_STRIP);
                            tiny3d_VertexPos(SX(55), row_y, 65535);
                            tiny3d_VertexFcolor(1.0f, 0.51f, 0.69f, 1.0f);
                            tiny3d_VertexPos(SX(59), row_y, 65535);
                            tiny3d_VertexFcolor(1.0f, 0.51f, 0.69f, 1.0f);
                            tiny3d_VertexPos(SX(55), row_y + SY(40), 65535);
                            tiny3d_VertexFcolor(1.0f, 0.51f, 0.69f, 1.0f);
                            tiny3d_VertexPos(SX(59), row_y + SY(40), 65535);
                            tiny3d_VertexFcolor(1.0f, 0.51f, 0.69f, 1.0f);
                            tiny3d_End();

                            SetFontSize(SF(22), SF(22));
                            SetFontColor(0xff82b1ff, 0); // Pink highlight
                            DrawFormatString(SX(70), row_y + SY(8), "[ > ]  %s", current_app_list.apps[idx].name);
                        } else {
                            SetFontSize(SF(20), SF(20));
                            SetFontColor(0xffb0bec5, 0);
                            DrawFormatString(SX(70), row_y + SY(8), "       %s", current_app_list.apps[idx].name);
                        }
                    }
                } else {
                    SetFontSize(SF(22), SF(22));
                    SetFontColor(0xffff82b1, 0);
                    DrawString(SX(60), SY(180), "No applications found on host.");
                }

                // Clean controls legend
                SetFontSize(SF(18), SF(18));
                SetFontColor(0xff9e9e9e, 0);
                DrawString(SX(60), SY(445), "\x05 Navigate   |   \x01 Launch Game   |   \x02 Cancel");
            } else if (ui_state == UI_STATE_ERROR) {
                SetFontSize(SF(26), SF(26));
                SetFontColor(0xffff5252, 0);
                DrawString(SX(60), SY(200), "ERROR: Target unreachable or Pairing failed.");
                DrawString(SX(60), SY(260), "Press \x01 to return.");
                if (pad.buttons_pressed & A_FLAG) ui_state = UI_STATE_IP_ENTRY;
            }
        }

        // 2. Draw TTY Logs (Bottom 28%) - Only in Menus
        if (ui_state != UI_STATE_STREAMING) {
            char visible_logs[8][MAX_LOG_WIDTH];
            int visible_log_count = 0;

            if (log_mutex_initialized) sysMutexLock(log_mutex, 0);
            int start_line = (log_count > 8) ? (log_count - 8) : 0;
            for (int i = start_line; i < log_count; i++) {
                memcpy(visible_logs[visible_log_count++], log_buffer[i], MAX_LOG_WIDTH);
            }
            if (log_mutex_initialized) sysMutexUnlock(log_mutex);

            // Log container background (#1E1E1E)
            tiny3d_SetPolygon(TINY3D_TRIANGLE_STRIP);
            tiny3d_VertexPos(0, ui_height * 0.72f, 65535);
            tiny3d_VertexFcolor(0.118f, 0.118f, 0.118f, 0.95f);
            tiny3d_VertexPos(ui_width, ui_height * 0.72f, 65535);
            tiny3d_VertexFcolor(0.118f, 0.118f, 0.118f, 0.95f);
            tiny3d_VertexPos(0, ui_height, 65535);
            tiny3d_VertexFcolor(0.090f, 0.090f, 0.090f, 0.95f);
            tiny3d_VertexPos(ui_width, ui_height, 65535);
            tiny3d_VertexFcolor(0.090f, 0.090f, 0.090f, 0.95f);
            tiny3d_End();

            // Log header accent line (#3F51B5)
            tiny3d_SetPolygon(TINY3D_TRIANGLE_STRIP);
            tiny3d_VertexPos(0, ui_height * 0.72f, 65535);
            tiny3d_VertexFcolor(0.247f, 0.318f, 0.710f, 0.8f);
            tiny3d_VertexPos(ui_width, ui_height * 0.72f, 65535);
            tiny3d_VertexFcolor(0.247f, 0.318f, 0.710f, 0.8f);
            tiny3d_VertexPos(0, (ui_height * 0.72f) + SY(2), 65535);
            tiny3d_VertexFcolor(0.247f, 0.318f, 0.710f, 0.8f);
            tiny3d_VertexPos(ui_width, (ui_height * 0.72f) + SY(2), 65535);
            tiny3d_VertexFcolor(0.247f, 0.318f, 0.710f, 0.8f);
            tiny3d_End();

            SetFontSize(SF(16), SF(16));
            SetFontColor(0xff80d8ff, 0); // Light Material Cyan/Blue for log readability
            for (int i = 0; i < visible_log_count; i++) {
                DrawString(SX(20), (ui_height * 0.73f) + (i * SY(19)), visible_logs[i]);
            }
        }

        tiny3d_Flip();
        // tiny3d_Flip() waits for VBlank, so usleep is unnecessary and causes frame drops.
    }
    sysThreadExit(0);
}

void ui_shutdown() {
    ui_save_settings();
    ui_stop();
    if (osk_active) {
        oskAbort();
    }
    sysUtilUnregisterCallback(SYSUTIL_EVENT_SLOT1);
    if (osk_container_created) {
        sysMemContainerDestroy(osk_container);
        osk_container_created = 0;
    }
    if (ui_thread_started) {
        u64 retval;
        sysThreadJoin(ui_thread, &retval);
        ui_thread_started = 0;
    }
    if (ft_face) {
        FT_Done_Face(ft_face);
        ft_face = NULL;
    }
    if (ft_library) {
        FT_Done_FreeType(ft_library);
        ft_library = NULL;
    }
    if (log_mutex_initialized) {
        sysMutexDestroy(log_mutex);
        log_mutex_initialized = 0;
    }
}

