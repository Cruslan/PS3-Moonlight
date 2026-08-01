#include "ui.h"
#include <tiny3d.h>
#include <libfont.h>
#include <malloc.h>
#include <string.h>
#include <stdio.h>
#include <sys/thread.h>
#include <unistd.h>
#include <lv2/systime.h>
#include "input/ps3.h"
#include <Limelight.h>
#include "video/ps3.h"
#include "audio/ps3.h"

static sys_ppu_thread_t ui_thread;
static int ui_running = 1;
static int ui_width = 1280;
static int ui_height = 720;
static int ui_state = UI_STATE_IP_ENTRY;

// IP state
static int ip_octets[4] = {192, 168, 1, 1};
static int ui_fps = 60;
static int ui_bitrate_options[] = {5000, 10000};
static int ui_bitrate_idx = 1; // Default 10 Mbps
static int ui_vsync = 1; // Default: VSync ON (1)
static int active_octet = 0; // 0-3: IP, 4: FPS, 5: Bitrate, 6: VSync, 7: Stats, 8: Verbose
static char target_ip_str[64];

static int frames_drawn_this_sec = 0;
static int ui_fps_actual = 0;
static u64 last_ui_time = 0;
static int show_stats = 0; // Default: Stats OFF (0)
static int ui_verbose = 0; // Default: Verbose Logging OFF (0)

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

const char* ui_get_target_ip() {
    snprintf(target_ip_str, sizeof(target_ip_str), "%d.%d.%d.%d", 
             ip_octets[0], ip_octets[1], ip_octets[2], ip_octets[3]);
    return target_ip_str;
}

static void ui_loop(void *arg);

#define MAX_LOG_LINES 25
#define MAX_LOG_WIDTH 100

static char log_buffer[MAX_LOG_LINES][MAX_LOG_WIDTH];
static int log_head = 0;
static int log_count = 0;

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
    ui_width = width;
    ui_height = height;
    tiny3d_Init(1024 * 1024); // 1MB vertex buffer
    
    // Alloc RSX memory for font texture
    texture_mem = tiny3d_AllocTexture(64 * 1024);
    if (!texture_mem) return;
    
    ResetFont();
    // Add our embedded font as font 0
    AddFontFromBitmapArray((u8 *)font_8x8_basic, (u8 *)texture_mem, 32, 127, 8, 8, 1, BIT7_FIRST_PIXEL);
    
    SetCurrentFont(0);
    SetFontSize(16, 16);  // Proportional font sizing
    SetFontColor(0xffffffff, 0x00000000); // White on transparent
    
    sysThreadCreate(&ui_thread, ui_loop, 0, 500, 0x4000, THREAD_JOINABLE, "UI Thread");
}

void ui_push_log(const char *msg) {
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
}

static void draw_background_gradient() {
    tiny3d_SetPolygon(TINY3D_TRIANGLE_STRIP);
    
    // Top-Left (Dark Blue)
    tiny3d_VertexPos(0, 0, 65535);
    tiny3d_VertexFcolor(0.0f, 0.0f, 0.2f, 1.0f);
    
    // Top-Right (Dark Blue)
    tiny3d_VertexPos(ui_width, 0, 65535);
    tiny3d_VertexFcolor(0.0f, 0.0f, 0.2f, 1.0f);
    
    // Bottom-Left (Black)
    tiny3d_VertexPos(0, ui_height * 0.7f, 65535);
    tiny3d_VertexFcolor(0.0f, 0.0f, 0.0f, 1.0f);
    
    // Bottom-Right (Black)
    tiny3d_VertexPos(ui_width, ui_height * 0.7f, 65535);
    tiny3d_VertexFcolor(0.0f, 0.0f, 0.0f, 1.0f);
    
    tiny3d_End();
}

static void ui_loop(void *arg) {
    ps3_pad_state_t pad;
    
    while (ui_running) {
        ps3input_get_data(&pad);
        
        tiny3d_Clear(0x000000ff, TINY3D_CLEAR_ALL);
        
        // Handle Input for Menu
        if (ui_state == UI_STATE_IP_ENTRY) {
            if (pad.buttons_pressed & LEFT_FLAG) active_octet = (active_octet + 8) % 9;
            if (pad.buttons_pressed & RIGHT_FLAG) active_octet = (active_octet + 1) % 9;
            
            if (active_octet < 4) {
                if (pad.buttons_pressed & UP_FLAG) ip_octets[active_octet] = (ip_octets[active_octet] + 1) % 256;
                if (pad.buttons_pressed & DOWN_FLAG) ip_octets[active_octet] = (ip_octets[active_octet] + 255) % 256;
            } else if (active_octet == 4) {
                // Toggle between 30 and 60
                if ((pad.buttons_pressed & UP_FLAG) || (pad.buttons_pressed & DOWN_FLAG)) {
                    ui_fps = (ui_fps == 30) ? 60 : 30;
                }
            } else if (active_octet == 5) {
                // Bitrate options: {5000, 10000}
                if (pad.buttons_pressed & UP_FLAG) ui_bitrate_idx = (ui_bitrate_idx + 1) % 2;
                if (pad.buttons_pressed & DOWN_FLAG) ui_bitrate_idx = (ui_bitrate_idx + 1) % 2;
            } else if (active_octet == 6) {
                // Toggle VSync between 1 (ON) and 0 (OFF)
                if ((pad.buttons_pressed & UP_FLAG) || (pad.buttons_pressed & DOWN_FLAG)) {
                    ui_vsync = !ui_vsync;
                }
            } else if (active_octet == 7) {
                // Toggle Stats between 1 (ON) and 0 (OFF)
                if ((pad.buttons_pressed & UP_FLAG) || (pad.buttons_pressed & DOWN_FLAG)) {
                    show_stats = !show_stats;
                }
            } else if (active_octet == 8) {
                // Toggle Verbose Logging between 1 (ON) and 0 (OFF)
                if ((pad.buttons_pressed & UP_FLAG) || (pad.buttons_pressed & DOWN_FLAG)) {
                    ui_verbose = !ui_verbose;
                }
            }
            
            // X to start pairing/launch
            if (pad.buttons_pressed & A_FLAG) {
                ui_state = UI_STATE_PAIRING;
            }
        } else if (ui_state == UI_STATE_PAIRING) {
            // Circle to cancel
            if (pad.buttons_pressed & B_FLAG) {
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
        
        if (ui_state == UI_STATE_STREAMING) {
            ps3video_draw();
            
            // Draw Video FPS Counter (Top Left) if enabled
            if (show_stats) {
                SetFontSize(16, 16);
                SetFontColor(0xff00ff00, 0); // Neo-Matrix green
                DrawFormatString(30, 30, "Rendered FPS: %d", ps3video_get_current_fps());
                DrawFormatString(30, 50, "Decoded FPS: %d", ps3video_get_decoded_fps());
                DrawFormatString(30, 70, "UI Loop FPS: %d", ui_fps_actual);
                DrawFormatString(30, 90, "Decode Latency: %d ms", ps3video_get_decode_latency());
                DrawFormatString(30, 110, "Render Latency: %d ms", ps3video_get_render_latency());
                DrawFormatString(30, 130, "Network Latency: %d ms", ps3video_get_net_latency() / 2);
                DrawFormatString(30, 150, "Total Latency: %d ms", (ps3video_get_net_latency() / 2) + ps3video_get_decode_latency() + ps3video_get_render_latency());
                DrawFormatString(30, 170, "Resolution: 1280x720");
                DrawFormatString(30, 190, "Target FPS: %d FPS", ui_get_fps());
                DrawFormatString(30, 210, "Bitrate: %d Mbps", ui_get_bitrate() / 1000);
                
                /* Draw temporary visual debug info for audio stream activity */
                u32 audio_pkts = ps3audio_get_decoded_packets();
                const char* spinner = "";
                switch ((audio_pkts / 8) % 4) {
                    case 0: spinner = "[ - ]"; break;
                    case 1: spinner = "[ \\ ]"; break;
                    case 2: spinner = "[ | ]"; break;
                    case 3: spinner = "[ / ]"; break;
                }
                DrawFormatString(30, 230, "Audio Packets: %u %s", audio_pkts, spinner);
            }
        } else {
            draw_background_gradient();
            
            SetFontSize(32, 32);
            SetFontColor(0xffffffff, 0);
            DrawString(100, 100, "Moonlight PS3 - Server Setup");
            
            if (ui_state == UI_STATE_IP_ENTRY) {
                SetFontSize(24, 24);
                DrawString(100, 160, "Enter Sunshine IP address:");
                
                for (int i = 0; i < 4; i++) {
                    int x = 120 + (i * 110); 
                    int y = 210;

                    if (i == active_octet) {
                        SetFontColor(0xff00ff00, 0);
                    } else {
                        SetFontColor(0xffffffff, 0);
                    }
                    
                    DrawFormatString(x, y, "%d", ip_octets[i]);
                    
                    SetFontColor(0xffffffff, 0);
                    if (i < 3) DrawString(x + 75, y, ".");
                }
                
                SetFontSize(20, 20);
                SetFontColor(0xffaaaaaa, 0);
                DrawString(100, 280, "Target FPS:");
                
                if (active_octet == 4) {
                    SetFontColor(0xff00ff00, 0);
                } else {
                    SetFontColor(0xffffffff, 0);
                }
                DrawFormatString(450, 280, "[ %d FPS ]", ui_fps);

                SetFontColor(0xffaaaaaa, 0);
                DrawString(100, 315, "Target Bitrate:");
                
                if (active_octet == 5) {
                    SetFontColor(0xff00ff00, 0);
                } else {
                    SetFontColor(0xffffffff, 0);
                }
                DrawFormatString(450, 315, "[ %d Mbps ]", ui_bitrate_options[ui_bitrate_idx] / 1000);

                SetFontColor(0xffaaaaaa, 0);
                DrawString(100, 350, "VSync:");
                
                if (active_octet == 6) {
                    SetFontColor(0xff00ff00, 0);
                } else {
                    SetFontColor(0xffffffff, 0);
                }
                DrawFormatString(450, 350, "[ %s ]", ui_vsync ? "ON" : "OFF");

                SetFontColor(0xffaaaaaa, 0);
                DrawString(100, 385, "Stats Overlay:");
                
                if (active_octet == 7) {
                    SetFontColor(0xff00ff00, 0);
                } else {
                    SetFontColor(0xffffffff, 0);
                }
                DrawFormatString(450, 385, "[ %s ]", show_stats ? "ON" : "OFF");

                SetFontColor(0xffaaaaaa, 0);
                DrawString(100, 420, "Verbose Logging:");
                
                if (active_octet == 8) {
                    SetFontColor(0xff00ff00, 0);
                } else {
                    SetFontColor(0xffffffff, 0);
                }
                DrawFormatString(450, 420, "[ %s ]", ui_verbose ? "ON" : "OFF");

                SetFontSize(22, 22);
                SetFontColor(0xffaaaaaa, 0);
                DrawString(100, 470, "PRESS [X] TO CONNECT / PAIR");
            } else if (ui_state == UI_STATE_PAIRING) {
                SetFontColor(0xffffff00, 0);
                DrawString(100, 240, "Pairing / Connecting... Please check host.");
                SetFontSize(20, 20);
                SetFontColor(0xffaaaaaa, 0);
                DrawString(100, 300, "PRESS (O) TO CANCEL");
            } else if (ui_state == UI_STATE_ERROR) {
                SetFontColor(0xffff0000, 0);
                DrawString(100, 240, "ERROR: Target unreachable or Pairing failed.");
                DrawString(100, 300, "Press [X] to return.");
                if (pad.buttons_pressed & A_FLAG) ui_state = UI_STATE_IP_ENTRY;
            }
        }

        // 2. Draw TTY Logs (Bottom 30%) - Only in Menus
        if (ui_state != UI_STATE_STREAMING) {
            tiny3d_SetPolygon(TINY3D_TRIANGLE_STRIP);
            tiny3d_VertexPos(0, ui_height * 0.7f, 65535);
            tiny3d_VertexFcolor(0.0f, 0.0f, 0.0f, 0.8f);
            tiny3d_VertexPos(ui_width, ui_height * 0.7f, 65535);
            tiny3d_VertexFcolor(0.0f, 0.0f, 0.0f, 0.8f);
            tiny3d_VertexPos(0, ui_height, 65535);
            tiny3d_VertexFcolor(0.05f, 0.05f, 0.05f, 0.8f);
            tiny3d_VertexPos(ui_width, ui_height, 65535);
            tiny3d_VertexFcolor(0.05f, 0.05f, 0.05f, 0.8f);
            tiny3d_End();

            SetFontSize(16, 16);
            SetFontColor(0xff00ff00, 0); // Neo-Matrix green for debug logs
            int start_line = (log_count > 8) ? (log_count - 8) : 0;
            for (int i = start_line; i < log_count; i++) {
                DrawString(20, ui_height * 0.71f + ((i - start_line) * 20), log_buffer[i]);
            }
        }

        tiny3d_Flip();
        // tiny3d_Flip() waits for VBlank, so usleep is unnecessary and causes frame drops.
    }
    sysThreadExit(0);
}
