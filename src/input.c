#include "input.h"
#include "ui.h"
#include <Limelight.h>
#include <io/pad.h>
#include <io/mouse.h>
#include <io/kb.h>
#include <string.h>
#include <sys/mutex.h>
#include <sys/thread.h>
#include <unistd.h>

static volatile int active_input_thread = 0;
static int input_thread_started = 0;
static sys_ppu_thread_t input_thread;
static ps3_pad_state_t g_pad_state = {0};
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

static void input_loop(void *arg) {
    (void)arg;
    padInfo padinfo;
    padData paddata;
    int last_buttons = 0;

    int buttonFlags = 0;
    short leftStickX = 0;
    short leftStickY = 0;
    short rightStickX = 0;
    short rightStickY = 0;
    unsigned char leftTrigger = 0;
    unsigned char rightTrigger = 0;

    // Initialize all input device libraries
    ioPadInit(7);
    ioKbInit(7);
    ioMouseInit(7);

    while(active_input_thread) {
        // 1. Controller Polling
        ioPadGetInfo(&padinfo);
        for(int i = 0; i < 1; i++) { // Polling primary controller (index 0)
            if(padinfo.status[i]) {
                ioPadGetData(i, &paddata);

                if (paddata.len > 0) {
                    buttonFlags = 0;
                    
                    if (paddata.BTN_CROSS) buttonFlags |= A_FLAG;
                    if (paddata.BTN_CIRCLE) buttonFlags |= B_FLAG;
                    // Swap Square and Triangle mappings to match user's physical-to-virtual layout
                    if (paddata.BTN_SQUARE) buttonFlags |= Y_FLAG;
                    if (paddata.BTN_TRIANGLE) buttonFlags |= X_FLAG;
                    
                    if (paddata.BTN_UP) buttonFlags |= UP_FLAG;
                    if (paddata.BTN_DOWN) buttonFlags |= DOWN_FLAG;
                    if (paddata.BTN_LEFT) buttonFlags |= LEFT_FLAG;
                    if (paddata.BTN_RIGHT) buttonFlags |= RIGHT_FLAG;
                    
                    if (paddata.BTN_L1) buttonFlags |= LB_FLAG;
                    if (paddata.BTN_R1) buttonFlags |= RB_FLAG;
                    
                    if (paddata.BTN_START) buttonFlags |= PLAY_FLAG;
                    if (paddata.BTN_SELECT) buttonFlags |= BACK_FLAG;
                    
                    if (paddata.BTN_L3) buttonFlags |= LS_CLK_FLAG;
                    if (paddata.BTN_R3) buttonFlags |= RS_CLK_FLAG;

                    // Center is ~128. The PS3 controller Y-axis ranges from 0 (UP) to 255 (DOWN).
                    // Subtracting 128 yields negative values for UP and positive for DOWN.
                    // Sunshine/Moonlight expects standard Y-axis coordinates where UP is positive and
                    // DOWN is negative. Therefore, we multiply by -256 to invert the raw signs.
                    int tempLX = (paddata.ANA_L_H - 128) * 256;
                    int tempLY = (paddata.ANA_L_V - 128) * -256;
                    int tempRX = (paddata.ANA_R_H - 128) * 256;
                    int tempRY = (paddata.ANA_R_V - 128) * -256;

                    leftStickX = tempLX > 32767 ? 32767 : (tempLX < -32768 ? -32768 : tempLX);
                    leftStickY = tempLY > 32767 ? 32767 : (tempLY < -32768 ? -32768 : tempLY);
                    rightStickX = tempRX > 32767 ? 32767 : (tempRX < -32768 ? -32768 : tempRX);
                    rightStickY = tempRY > 32767 ? 32767 : (tempRY < -32768 ? -32768 : tempRY);

                    leftTrigger = paddata.BTN_L2 ? 0xFF : 0x00;
                    rightTrigger = paddata.BTN_R2 ? 0xFF : 0x00;

                    // Send controller event to Moonlight/Sunshine server
                    LiSendControllerEvent(buttonFlags, leftTrigger, rightTrigger, leftStickX, leftStickY, rightStickX, rightStickY);

                    // Update shared state for UI
                    sysMutexLock(pad_state_mutex, 0);
                    g_pad_state.buttons_down = buttonFlags;
                    g_pad_state.buttons_pressed |= (buttonFlags & ~last_buttons);
                    g_pad_state.lx = leftStickX;
                    g_pad_state.ly = leftStickY;
                    g_pad_state.rx = rightStickX;
                    g_pad_state.ry = rightStickY;
                    sysMutexUnlock(pad_state_mutex);

                    last_buttons = buttonFlags;
                }
            }
        }

        if (!padinfo.status[0] && last_buttons != 0) {
            LiSendControllerEvent(0, 0, 0, 0, 0, 0, 0);
            sysMutexLock(pad_state_mutex, 0);
            memset(&g_pad_state, 0, sizeof(g_pad_state));
            sysMutexUnlock(pad_state_mutex);
            last_buttons = 0;
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
                                LiSendMouseMoveAsMousePositionEvent((short)mdata.x_axis, (short)mdata.y_axis, 1280, 720);
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
