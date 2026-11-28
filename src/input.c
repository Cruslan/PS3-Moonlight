#include "input.h"
#include "ui.h"
#include "connection.h"
#include "net_logger.h"
#include <Limelight.h>
#include <io/pad.h>
#include <io/mouse.h>
#include <io/kb.h>
#include <string.h>
#include <sys/mutex.h>
#include <sys/thread.h>
#include <unistd.h>

#define PS3_MAX_CONTROLLERS MAX_PORT_NUM
#define PS3_SUPPORTED_BUTTON_FLAGS ( \
    A_FLAG | B_FLAG | X_FLAG | Y_FLAG | \
    UP_FLAG | DOWN_FLAG | LEFT_FLAG | RIGHT_FLAG | \
    LB_FLAG | RB_FLAG | PLAY_FLAG | BACK_FLAG | \
    LS_CLK_FLAG | RS_CLK_FLAG)

typedef struct {
    int buttons;
    short lx, ly, rx, ry;
    unsigned char lt, rt;
} ps3_port_cache_t;

typedef struct {
    u8 small_motor;
    u8 large_motor;
    int dirty;
} ps3_rumble_state_t;

static volatile int active_input_thread = 0;
static int input_thread_started = 0;
static sys_ppu_thread_t input_thread;
static ps3_pad_state_t g_pad_state = {0};
static ps3_rumble_state_t g_rumble_pending[PS3_MAX_CONTROLLERS] = {{0}};
static sys_mutex_t pad_state_mutex;
static int pad_state_mutex_initialized = 0;

// Mouse state tracking
static u8 last_mouse_buttons[MAX_MICE] = {0};

// Keyboard state tracking
static uint8_t last_keys_down[MAX_KEYBOARDS][256] = {{0}};
static KbMkey last_mkeys[MAX_KEYBOARDS] = {{{0}}};

// Comprehensive PS3 Keycode (ASCII / KB_RAWDAT / KB_KEYPAD / HID RAW) to Win32 Virtual Key (VK) conversion
static uint16_t ps3_translate_keycode_to_vk(u16 code) {
    // 1. Raw Non-ASCII and Extended Function Keys (KB_RAWDAT flag 0x8000 set)
    if (code & KB_RAWDAT) {
        u8 raw = (u8)(code & 0xFF);
        switch (raw) {
            case KB_RAWKEY_ESCAPE:         return 0x1B; // VK_ESCAPE
            case KB_RAWKEY_CAPS_LOCK:      return 0x14; // VK_CAPITAL
            case KB_RAWKEY_F1:             return 0x70; // VK_F1
            case KB_RAWKEY_F2:             return 0x71; // VK_F2
            case KB_RAWKEY_F3:             return 0x72; // VK_F3
            case KB_RAWKEY_F4:             return 0x73; // VK_F4
            case KB_RAWKEY_F5:             return 0x74; // VK_F5
            case KB_RAWKEY_F6:             return 0x75; // VK_F6
            case KB_RAWKEY_F7:             return 0x76; // VK_F7
            case KB_RAWKEY_F8:             return 0x77; // VK_F8
            case KB_RAWKEY_F9:             return 0x78; // VK_F9
            case KB_RAWKEY_F10:            return 0x79; // VK_F10
            case KB_RAWKEY_F11:            return 0x7A; // VK_F11
            case KB_RAWKEY_F12:            return 0x7B; // VK_F12
            case KB_RAWKEY_PRINTSCREEN:    return 0x2C; // VK_SNAPSHOT
            case KB_RAWKEY_SCROLL_LOCK:    return 0x91; // VK_SCROLL
            case KB_RAWKEY_PAUSE:          return 0x13; // VK_PAUSE
            case KB_RAWKEY_INSERT:         return 0x2D; // VK_INSERT
            case KB_RAWKEY_HOME:           return 0x24; // VK_HOME
            case KB_RAWKEY_PAGE_UP:        return 0x21; // VK_PRIOR
            case KB_RAWKEY_DELETE:         return 0x2E; // VK_DELETE
            case KB_RAWKEY_END:            return 0x23; // VK_END
            case KB_RAWKEY_PAGE_DOWN:      return 0x22; // VK_NEXT
            case KB_RAWKEY_RIGHT_ARROW:    return 0x27; // VK_RIGHT
            case KB_RAWKEY_LEFT_ARROW:     return 0x25; // VK_LEFT
            case KB_RAWKEY_DOWN_ARROW:     return 0x28; // VK_DOWN
            case KB_RAWKEY_UP_ARROW:       return 0x26; // VK_UP
            case KB_RAWKEY_NUM_LOCK:       return 0x90; // VK_NUMLOCK
            case KB_RAWKEY_APPLICATION:    return 0x5D; // VK_APPS
            default: break;
        }
    }

    // 2. Numeric Keypad Keys (KB_KEYPAD flag 0x4000 set)
    if (code & KB_KEYPAD) {
        u8 raw = (u8)(code & 0xFF);
        switch (raw) {
            case KB_RAWKEY_KPAD_SLASH:     return 0x6F; // VK_DIVIDE
            case KB_RAWKEY_KPAD_ASTERISK:  return 0x6A; // VK_MULTIPLY
            case KB_RAWKEY_KPAD_MINUS:     return 0x6D; // VK_SUBTRACT
            case KB_RAWKEY_KPAD_PLUS:      return 0x6B; // VK_ADD
            case KB_RAWKEY_KPAD_ENTER:     return 0x0D; // VK_RETURN
            case KB_RAWKEY_KPAD_1:         return 0x61; // VK_NUMPAD1
            case KB_RAWKEY_KPAD_2:         return 0x62; // VK_NUMPAD2
            case KB_RAWKEY_KPAD_3:         return 0x63; // VK_NUMPAD3
            case KB_RAWKEY_KPAD_4:         return 0x64; // VK_NUMPAD4
            case KB_RAWKEY_KPAD_5:         return 0x65; // VK_NUMPAD5
            case KB_RAWKEY_KPAD_6:         return 0x66; // VK_NUMPAD6
            case KB_RAWKEY_KPAD_7:         return 0x67; // VK_NUMPAD7
            case KB_RAWKEY_KPAD_8:         return 0x68; // VK_NUMPAD8
            case KB_RAWKEY_KPAD_9:         return 0x69; // VK_NUMPAD9
            case KB_RAWKEY_KPAD_0:         return 0x60; // VK_NUMPAD0
            case KB_RAWKEY_KPAD_PERIOD:    return 0x6E; // VK_DECIMAL
            default: break;
        }
    }

    // 3. ASCII Character Mapping (Standard PS3 keyboard output)
    // Lowercase a-z -> Win32 VK_A - VK_Z (0x41 - 0x5A)
    if (code >= 'a' && code <= 'z') {
        return (uint16_t)(0x41 + (code - 'a'));
    }
    // Uppercase A-Z -> Win32 VK_A - VK_Z (0x41 - 0x5A)
    if (code >= 'A' && code <= 'Z') {
        return (uint16_t)(0x41 + (code - 'A'));
    }
    // Digits 0-9 -> Win32 VK_0 - VK_9 (0x30 - 0x39)
    if (code >= '0' && code <= '9') {
        return (uint16_t)(0x30 + (code - '0'));
    }

    // Standard control & punctuation characters
    switch (code) {
        case 0x0D:
        case 0x0A: return 0x0D; // VK_RETURN
        case 0x1B: return 0x1B; // VK_ESCAPE
        case 0x08: return 0x08; // VK_BACK
        case 0x09: return 0x09; // VK_TAB
        case ' ':  return 0x20; // VK_SPACE
        case '-':
        case '_':  return 0xBD; // VK_OEM_MINUS
        case '=':
        case '+':  return 0xBB; // VK_OEM_PLUS
        case '[':
        case '{':  return 0xDB; // VK_OEM_4
        case ']':
        case '}':  return 0xDD; // VK_OEM_6
        case '\\':
        case '|':  return 0xDC; // VK_OEM_5
        case ';':
        case ':':  return 0xBA; // VK_OEM_1
        case '\'':
        case '\"': return 0xDE; // VK_OEM_7
        case '`':
        case '~':  return 0xC0; // VK_OEM_3
        case ',':
        case '<':  return 0xBC; // VK_OEM_COMMA
        case '.':
        case '>':  return 0xBE; // VK_OEM_PERIOD
        case '/':
        case '?':  return 0xBF; // VK_OEM_2
        case ')':  return 0x30; // Shift+0
        case '!':  return 0x31; // Shift+1
        case '@':  return 0x32; // Shift+2
        case '#':  return 0x33; // Shift+3
        case '$':  return 0x34; // Shift+4
        case '%':  return 0x35; // Shift+5
        case '^':  return 0x36; // Shift+6
        case '&':  return 0x37; // Shift+7
        case '*':  return 0x38; // Shift+8
        case '(':  return 0x39; // Shift+9
        default: break;
    }

    // 4. Pure Raw HID Usage ID Fallback (0x04 - 0x52)
    if (code >= 0x04 && code <= 0x1D) return (uint16_t)(0x41 + (code - 0x04)); // A-Z
    if (code >= 0x1E && code <= 0x26) return (uint16_t)(0x31 + (code - 0x1E)); // 1-9
    if (code == 0x27) return 0x30; // 0
    if (code == 0x28) return 0x0D; // Enter
    if (code == 0x29) return 0x1B; // Escape
    if (code == 0x2A) return 0x08; // Backspace
    if (code == 0x2B) return 0x09; // Tab
    if (code == 0x2C) return 0x20; // Space
    if (code == 0x2D) return 0xBD; // -
    if (code == 0x2E) return 0xBB; // =
    if (code == 0x2F) return 0xDB; // [
    if (code == 0x30) return 0xDD; // ]
    if (code == 0x31) return 0xDC; // Backslash
    if (code == 0x33) return 0xBA; // ;
    if (code == 0x34) return 0xDE; // '
    if (code == 0x35) return 0xC0; // `
    if (code == 0x36) return 0xBC; // ,
    if (code == 0x37) return 0xBE; // .
    if (code == 0x38) return 0xBF; // /
    if (code == 0x39) return 0x14; // Caps Lock
    if (code >= 0x3A && code <= 0x45) return (uint16_t)(0x70 + (code - 0x3A)); // F1-F12
    if (code == 0x46) return 0x2C; // PrintScreen
    if (code == 0x47) return 0x91; // ScrollLock
    if (code == 0x48) return 0x13; // Pause
    if (code == 0x49) return 0x2D; // Insert
    if (code == 0x4A) return 0x24; // Home
    if (code == 0x4B) return 0x21; // PageUp
    if (code == 0x4C) return 0x2E; // Delete
    if (code == 0x4D) return 0x23; // End
    if (code == 0x4E) return 0x22; // PageDown
    if (code == 0x4F) return 0x27; // Right
    if (code == 0x50) return 0x25; // Left
    if (code == 0x51) return 0x28; // Down
    if (code == 0x52) return 0x26; // Up

    return 0;
}

void ps3input_get_data(ps3_pad_state_t *state) {
    if (!state) return;
    if (!pad_state_mutex_initialized) {
        memset(state, 0, sizeof(*state));
        return;
    }

    sysMutexLock(pad_state_mutex, 0);
    *state = g_pad_state;
    g_pad_state.buttons_pressed = 0;
    sysMutexUnlock(pad_state_mutex);
}

/** Queue rumble motor speeds safely from the Moonlight callback thread for InputThread execution. */
void ps3input_set_rumble(int port, unsigned short low_freq, unsigned short high_freq) {
    if (port < 0 || port >= PS3_MAX_CONTROLLERS || !pad_state_mutex_initialized) return;

    sysMutexLock(pad_state_mutex, 0);
    g_rumble_pending[port].large_motor = (u8)(low_freq >> 8);
    g_rumble_pending[port].small_motor = ((high_freq >> 8) > 0) ? 1 : 0;
    g_rumble_pending[port].dirty = 1;
    sysMutexUnlock(pad_state_mutex);
}

static inline short clamp_s16(int v) {
    return (short)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
}

/** Decode PSL1GHT padData bitfields and axes into Moonlight controller state. */
static void decode_pad_data(const padData *paddata, ps3_port_cache_t *out) {
    int buttonFlags = 0;

    if (paddata->BTN_CROSS) buttonFlags |= A_FLAG;
    if (paddata->BTN_CIRCLE) buttonFlags |= B_FLAG;
    // Map face buttons by physical position (Cross->A, Circle->B, Square->X, Triangle->Y)
    if (paddata->BTN_SQUARE) buttonFlags |= X_FLAG;
    if (paddata->BTN_TRIANGLE) buttonFlags |= Y_FLAG;

    if (paddata->BTN_UP) buttonFlags |= UP_FLAG;
    if (paddata->BTN_DOWN) buttonFlags |= DOWN_FLAG;
    if (paddata->BTN_LEFT) buttonFlags |= LEFT_FLAG;
    if (paddata->BTN_RIGHT) buttonFlags |= RIGHT_FLAG;

    if (paddata->BTN_L1) buttonFlags |= LB_FLAG;
    if (paddata->BTN_R1) buttonFlags |= RB_FLAG;

    if (paddata->BTN_START) buttonFlags |= PLAY_FLAG;
    if (paddata->BTN_SELECT) buttonFlags |= BACK_FLAG;

    if (paddata->BTN_L3) buttonFlags |= LS_CLK_FLAG;
    if (paddata->BTN_R3) buttonFlags |= RS_CLK_FLAG;

    // PS3 analog Y ranges 0 (UP)..255 (DOWN); Moonlight expects UP positive and DOWN negative.
    out->buttons = buttonFlags;
    out->lx = clamp_s16((paddata->ANA_L_H - 128) * 256);
    out->ly = clamp_s16((paddata->ANA_L_V - 128) * -256);
    out->rx = clamp_s16((paddata->ANA_R_H - 128) * 256);
    out->ry = clamp_s16((paddata->ANA_R_V - 128) * -256);
    out->lt = paddata->BTN_L2 ? 0xFF : 0x00;
    out->rt = paddata->BTN_R2 ? 0xFF : 0x00;
}

static void input_loop(void *arg) {
    (void)arg;
    padInfo padinfo;
    padData paddata;
    ps3_port_cache_t ports[PS3_MAX_CONTROLLERS];
    int prev_buttons[PS3_MAX_CONTROLLERS] = {0};
    u8 active_small[PS3_MAX_CONTROLLERS] = {0};
    u8 active_large[PS3_MAX_CONTROLLERS] = {0};
    uint16_t announced_mask = 0;

    memset(ports, 0, sizeof(ports));

    // Initialize all input device libraries (up to 7 PS3 controller ports)
    ioPadInit(PS3_MAX_CONTROLLERS);
    ioKbInit(7);
    ioMouseInit(7);

    while(active_input_thread) {
        int stream_connected = connection_is_connected();

        // 1. Multi-Controller Polling (Ports 0..6)
        memset(&padinfo, 0, sizeof(padinfo));
        if (ioPadGetInfo(&padinfo) == 0) {
            uint16_t active_mask = 0;
            int updated[PS3_MAX_CONTROLLERS] = {0};
            int combined_down = 0;
            int combined_pressed = 0;
            short ui_lx = 0, ui_ly = 0, ui_rx = 0, ui_ry = 0;
            int primary_stick_set = 0;

            for (int i = 0; i < PS3_MAX_CONTROLLERS; i++) {
                if (padinfo.status[i]) {
                    active_mask |= (uint16_t)(1u << i);
                    memset(&paddata, 0, sizeof(paddata));
                    if (ioPadGetData((u32)i, &paddata) == 0 && paddata.len > 0) {
                        decode_pad_data(&paddata, &ports[i]);
                        updated[i] = 1;
                    }
                    combined_down |= ports[i].buttons;
                    combined_pressed |= (ports[i].buttons & ~prev_buttons[i]);
                    prev_buttons[i] = ports[i].buttons;
                    if (!primary_stick_set) {
                        ui_lx = ports[i].lx;
                        ui_ly = ports[i].ly;
                        ui_rx = ports[i].rx;
                        ui_ry = ports[i].ry;
                        primary_stick_set = 1;
                    }
                } else {
                    memset(&ports[i], 0, sizeof(ports[i]));
                    prev_buttons[i] = 0;
                    active_small[i] = 0;
                    active_large[i] = 0;
                }
            }

            // Publish aggregated pad state for menus / exit combo and fetch pending rumble updates
            ps3_rumble_state_t rumble_local[PS3_MAX_CONTROLLERS] = {{0}};
            sysMutexLock(pad_state_mutex, 0);
            g_pad_state.buttons_down = combined_down;
            g_pad_state.buttons_pressed |= combined_pressed;
            g_pad_state.lx = ui_lx;
            g_pad_state.ly = ui_ly;
            g_pad_state.rx = ui_rx;
            g_pad_state.ry = ui_ry;
            for (int i = 0; i < PS3_MAX_CONTROLLERS; i++) {
                rumble_local[i] = g_rumble_pending[i];
                g_rumble_pending[i].dirty = 0;
            }
            sysMutexUnlock(pad_state_mutex);

            // Apply rumble strictly on InputThread; stop any active motors if disconnected
            for (int i = 0; i < PS3_MAX_CONTROLLERS; i++) {
                if (!padinfo.status[i]) continue;

                u8 target_small = stream_connected
                    ? (rumble_local[i].dirty ? rumble_local[i].small_motor : active_small[i])
                    : 0;
                u8 target_large = stream_connected
                    ? (rumble_local[i].dirty ? rumble_local[i].large_motor : active_large[i])
                    : 0;

                if (target_small != active_small[i] || target_large != active_large[i]) {
                    padActParam act;
                    memset(&act, 0, sizeof(act));
                    act.small_motor = target_small;
                    act.large_motor = target_large;
                    ioPadSetActDirect((u32)i, &act);
                    active_small[i] = target_small;
                    active_large[i] = target_large;
                }
            }

            if (!stream_connected) {
                announced_mask = 0;
            } else {
                // Report disconnected ports first so host clears buttons and activeGamepadMask
                for (int i = 0; i < PS3_MAX_CONTROLLERS; i++) {
                    uint16_t bit = (uint16_t)(1u << i);
                    if ((announced_mask & bit) && !(active_mask & bit)) {
                        LiSendMultiControllerEvent((short)i, (short)active_mask, 0, 0, 0, 0, 0, 0, 0);
                        announced_mask &= (uint16_t)~bit;
                        NLOG("[INPUT] Controller %d disconnected (mask=0x%02x)", i, active_mask);
                    }
                }

                // Announce new arrivals and send state updates for active ports
                for (int i = 0; i < PS3_MAX_CONTROLLERS; i++) {
                    uint16_t bit = (uint16_t)(1u << i);
                    if (!(active_mask & bit)) continue;

                    if (!(announced_mask & bit)) {
                        if (LiSendControllerArrivalEvent((uint8_t)i, active_mask, LI_CTYPE_PS,
                                                         PS3_SUPPORTED_BUTTON_FLAGS,
                                                         LI_CCAP_ANALOG_TRIGGERS | LI_CCAP_RUMBLE) == 0) {
                            announced_mask |= bit;
                            NLOG("[INPUT] Controller %d connected (mask=0x%02x)", i, active_mask);
                            if (ports[i].buttons != 0 || ports[i].lt != 0 || ports[i].rt != 0 ||
                                ports[i].lx != 0 || ports[i].ly != 0 || ports[i].rx != 0 || ports[i].ry != 0) {
                                updated[i] = 1;
                            }
                        }
                    }

                    if ((announced_mask & bit) && updated[i]) {
                        LiSendMultiControllerEvent((short)i, (short)active_mask,
                                                   ports[i].buttons, ports[i].lt, ports[i].rt,
                                                   ports[i].lx, ports[i].ly, ports[i].rx, ports[i].ry);
                    }
                }
            }
        }

        // 2. Mouse Polling (USB / Bluetooth HID Mice)
        mouseInfo minfo;
        if (ioMouseGetInfo(&minfo) == 0 && minfo.connected > 0) {
            for (u32 m = 0; m < MAX_MICE; m++) {
                if (minfo.status[m]) {
                    mouseData mdata;
                    if (ioMouseGetData(m, &mdata) == 0 && mdata.update) {
                        // Motion: Relative (Game Mode) vs Absolute (Desktop Mode)
                        if (mdata.x_axis != 0 || mdata.y_axis != 0) {
                            if (ui_get_mouse_mode() == 0) {
                                LiSendMouseMoveEvent((short)mdata.x_axis, (short)mdata.y_axis);
                            } else {
                                LiSendMouseMoveAsMousePositionEvent((short)mdata.x_axis, (short)mdata.y_axis, ui_get_stream_width(), ui_get_stream_height());
                            }
                        }

                        // Scroll wheel
                        if (mdata.wheel != 0) {
                            LiSendScrollEvent((signed char)mdata.wheel);
                        }

                        // Mouse button state changes
                        u8 btn_diff = mdata.buttons ^ last_mouse_buttons[m];
                        if (btn_diff) {
                            // Left button (bit 0)
                            if (btn_diff & 0x01) {
                                LiSendMouseButtonEvent((mdata.buttons & 0x01) ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE, BUTTON_LEFT);
                            }
                            // Right button (bit 1)
                            if (btn_diff & 0x02) {
                                LiSendMouseButtonEvent((mdata.buttons & 0x02) ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE, BUTTON_RIGHT);
                            }
                            // Middle button (bit 2)
                            if (btn_diff & 0x04) {
                                LiSendMouseButtonEvent((mdata.buttons & 0x04) ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE, BUTTON_MIDDLE);
                            }
                            // Side button X1 (bit 3)
                            if (btn_diff & 0x08) {
                                LiSendMouseButtonEvent((mdata.buttons & 0x08) ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE, BUTTON_X1);
                            }
                            // Side button X2 (bit 4)
                            if (btn_diff & 0x10) {
                                LiSendMouseButtonEvent((mdata.buttons & 0x10) ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE, BUTTON_X2);
                            }
                            last_mouse_buttons[m] = mdata.buttons;
                        }
                    }
                }
            }
        }

        // 3. Keyboard Polling (USB / Bluetooth HID Keyboards)
        KbInfo kbinfo;
        if (ioKbGetInfo(&kbinfo) == 0 && kbinfo.connected > 0) {
            for (u32 k = 0; k < MAX_KEYBOARDS; k++) {
                if (kbinfo.status[k]) {
                    KbData kbdata;
                    if (ioKbRead(k, &kbdata) == 0) {
                        // Extract modifier mask
                        char mod_mask = 0;
                        if (kbdata.mkey._KbMkeyU._KbMkeyS.l_shift || kbdata.mkey._KbMkeyU._KbMkeyS.r_shift) mod_mask |= MODIFIER_SHIFT;
                        if (kbdata.mkey._KbMkeyU._KbMkeyS.l_ctrl || kbdata.mkey._KbMkeyU._KbMkeyS.r_ctrl) mod_mask |= MODIFIER_CTRL;
                        if (kbdata.mkey._KbMkeyU._KbMkeyS.l_alt || kbdata.mkey._KbMkeyU._KbMkeyS.r_alt) mod_mask |= MODIFIER_ALT;
                        if (kbdata.mkey._KbMkeyU._KbMkeyS.l_win || kbdata.mkey._KbMkeyU._KbMkeyS.r_win) mod_mask |= MODIFIER_META;

                        // Discrete modifier key down/up events
                        if (kbdata.mkey._KbMkeyU._KbMkeyS.l_ctrl != last_mkeys[k]._KbMkeyU._KbMkeyS.l_ctrl) {
                            LiSendKeyboardEvent(0xA2, kbdata.mkey._KbMkeyU._KbMkeyS.l_ctrl ? KEY_ACTION_DOWN : KEY_ACTION_UP, mod_mask);
                        }
                        if (kbdata.mkey._KbMkeyU._KbMkeyS.r_ctrl != last_mkeys[k]._KbMkeyU._KbMkeyS.r_ctrl) {
                            LiSendKeyboardEvent(0xA3, kbdata.mkey._KbMkeyU._KbMkeyS.r_ctrl ? KEY_ACTION_DOWN : KEY_ACTION_UP, mod_mask);
                        }
                        if (kbdata.mkey._KbMkeyU._KbMkeyS.l_shift != last_mkeys[k]._KbMkeyU._KbMkeyS.l_shift) {
                            LiSendKeyboardEvent(0xA0, kbdata.mkey._KbMkeyU._KbMkeyS.l_shift ? KEY_ACTION_DOWN : KEY_ACTION_UP, mod_mask);
                        }
                        if (kbdata.mkey._KbMkeyU._KbMkeyS.r_shift != last_mkeys[k]._KbMkeyU._KbMkeyS.r_shift) {
                            LiSendKeyboardEvent(0xA1, kbdata.mkey._KbMkeyU._KbMkeyS.r_shift ? KEY_ACTION_DOWN : KEY_ACTION_UP, mod_mask);
                        }
                        if (kbdata.mkey._KbMkeyU._KbMkeyS.l_alt != last_mkeys[k]._KbMkeyU._KbMkeyS.l_alt) {
                            LiSendKeyboardEvent(0xA4, kbdata.mkey._KbMkeyU._KbMkeyS.l_alt ? KEY_ACTION_DOWN : KEY_ACTION_UP, mod_mask);
                        }
                        if (kbdata.mkey._KbMkeyU._KbMkeyS.r_alt != last_mkeys[k]._KbMkeyU._KbMkeyS.r_alt) {
                            LiSendKeyboardEvent(0xA5, kbdata.mkey._KbMkeyU._KbMkeyS.r_alt ? KEY_ACTION_DOWN : KEY_ACTION_UP, mod_mask);
                        }
                        if (kbdata.mkey._KbMkeyU._KbMkeyS.l_win != last_mkeys[k]._KbMkeyU._KbMkeyS.l_win) {
                            LiSendKeyboardEvent(0x5B, kbdata.mkey._KbMkeyU._KbMkeyS.l_win ? KEY_ACTION_DOWN : KEY_ACTION_UP, mod_mask);
                        }
                        if (kbdata.mkey._KbMkeyU._KbMkeyS.r_win != last_mkeys[k]._KbMkeyU._KbMkeyS.r_win) {
                            LiSendKeyboardEvent(0x5C, kbdata.mkey._KbMkeyU._KbMkeyS.r_win ? KEY_ACTION_DOWN : KEY_ACTION_UP, mod_mask);
                        }
                        last_mkeys[k] = kbdata.mkey;

                        // Standard, Special, and Numpad Key events
                        uint8_t current_keys_pressed[256] = {0};
                        for (int c = 0; c < kbdata.nb_keycode; c++) {
                            u16 raw = kbdata.keycode[c];
                            uint16_t vk = ps3_translate_keycode_to_vk(raw);
                            if (vk > 0 && vk < 256) {
                                current_keys_pressed[vk] = 1;
                                if (!last_keys_down[k][vk]) {
                                    LiSendKeyboardEvent((short)vk, KEY_ACTION_DOWN, mod_mask);
                                }
                            }
                        }
                        for (int vk = 0; vk < 256; vk++) {
                            if (last_keys_down[k][vk] && !current_keys_pressed[vk]) {
                                LiSendKeyboardEvent((short)vk, KEY_ACTION_UP, mod_mask);
                            }
                            last_keys_down[k][vk] = current_keys_pressed[vk];
                        }
                    }
                }
            }
        }

        usleep(4000); // 250 Hz polling (4ms) for ultra-low latency
    }
    
    ioMouseEnd();
    ioKbEnd();
    ioPadEnd();
    sysThreadExit(0);
}

void ps3input_start() {
    if (input_thread_started) return;

    sys_mutex_attr_t attr;
    sysMutexAttrInitialize(attr);
    if (sysMutexCreate(&pad_state_mutex, &attr) != 0) return;
    pad_state_mutex_initialized = 1;

    memset(&g_pad_state, 0, sizeof(g_pad_state));
    memset(g_rumble_pending, 0, sizeof(g_rumble_pending));
    active_input_thread = 1;
    // Priority 200: Input should preempt almost everything except critical network receive (100)
    if (sysThreadCreate(&input_thread, input_loop, 0, 200, 0x8000,
                        THREAD_JOINABLE, "InputThread") != 0) {
        active_input_thread = 0;
        sysMutexDestroy(pad_state_mutex);
        pad_state_mutex_initialized = 0;
        return;
    }
    input_thread_started = 1;
}

void ps3input_stop() {
    if (!input_thread_started) return;
    active_input_thread = 0;
    u64 retval;
    sysThreadJoin(input_thread, &retval);
    input_thread_started = 0;
    if (pad_state_mutex_initialized) {
        sysMutexDestroy(pad_state_mutex);
        pad_state_mutex_initialized = 0;
    }
}
