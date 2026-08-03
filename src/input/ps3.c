#include "ps3.h"
#include <Limelight.h>
#include <io/pad.h>
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

    ioPadInit(7);

    while(active_input_thread) {
        ioPadGetInfo(&padinfo);
        for(int i = 0; i < 1; i++) { // For MVP only polling first controller (index 0)
            if(padinfo.status[i]) {
                ioPadGetData(i, &paddata);

                // ioPadGetData only fills paddata if there is a change.
                // If len is 0, paddata is zero-filled. We must ignore it.
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
                    // DOWN is negative. Therefore, we must multiply by -256 to invert the raw signs.
                    //
                    // Mathematical Overflow Prevention:
                    // When a stick is fully UP (value 0) or fully LEFT (value 0), subtracting 128 and
                    // multiplying by -256 or 256 can yield -32768 or 32768. While -32768 fits in a signed
                    // 16-bit short, positive 32768 overflows the maximum positive short boundary (32767)
                    // and wraps around to -32768 (which maps to the opposite direction, i.e., fully DOWN).
                    // We calculate the coordinates as integers first and clamp them to [-32768, 32767]
                    // before casting to short to prevent this overflow behavior.
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

        usleep(4000); // 250 Hz polling (4ms) for ultra-low latency
    }
    
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
