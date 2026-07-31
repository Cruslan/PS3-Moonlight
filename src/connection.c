#include "connection.h"
#include <Limelight.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "video/ps3.h"
#include "audio/ps3.h"
#include "input/ps3.h"
#include "net_logger.h"

static int connection_status = LI_DISCONNECTED;
int connection_stage = 0;

void ps3_debug_log(const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    NLOG("[LI] %s", buf);
}

static void cb_stage_starting(int stage) {
    connection_stage = stage;
    NLOG("stage_starting: %s", LiGetStageName(stage));
}

static void cb_stage_complete(int stage) {
    connection_stage = stage;
    NLOG("stage_complete: %s", LiGetStageName(stage));
}

static void cb_stage_failed(int stage, int code) {
    connection_stage = stage;
    NLOG("stage_failed: %s code=%d", LiGetStageName(stage), code);
}

static void cb_connection_started(void) {
    NLOG("connectionStarted");
    connection_status = LI_CONNECTED;
    ps3video_start();
    ps3audio_start();
    ps3input_start();
}

static void cb_connection_terminated(int error_code) {
    NLOG("connectionTerminated err=%d", error_code);
    LiStopConnection();
    if (connection_status == LI_CONNECTED) {
        ps3video_stop();
        ps3audio_stop();
        ps3input_stop();
    }
    connection_status = LI_DISCONNECTED;
}

static void cb_rumble(unsigned short c, unsigned short l, unsigned short h) {}
static void cb_status_update(int status) { NLOG("statusUpdate: %d", status); }
static void cb_set_hdr(bool enabled) {}
static void cb_rumble_triggers(uint16_t c, uint16_t l, uint16_t r) {}
static void cb_set_motion(uint16_t c, uint8_t t, uint16_t r) {}
static void cb_set_led(uint16_t c, uint8_t r, uint8_t g, uint8_t b) {}
static void cb_set_adaptive_triggers(uint16_t c, uint8_t* lp, uint32_t ll, uint8_t* rp, uint32_t rl) {}

bool connection_is_ready(void) { return connection_status != LI_DISCONNECTED; }
bool connection_is_connected(void) { return connection_status == LI_CONNECTED; }
int connection_get_status(void) { return connection_status; }

CONNECTION_LISTENER_CALLBACKS connection_callbacks;

void connection_callbacks_init(void) {
    NLOG("cb_init: start");
    LiInitializeConnectionCallbacks(&connection_callbacks);
    NLOG("cb_init: after LiInitializeConnectionCallbacks");
    
    // Geçici olarak assignment'ları kapatıp adım adım deneyelim, veya log'dan izleyelim
    connection_callbacks.stageStarting        = cb_stage_starting;
    connection_callbacks.stageComplete        = cb_stage_complete;
    connection_callbacks.stageFailed          = cb_stage_failed;
    connection_callbacks.connectionStarted    = cb_connection_started;
    connection_callbacks.connectionTerminated = cb_connection_terminated;
    NLOG("cb_init: part 1 assigned");
    
    connection_callbacks.logMessage           = ps3_debug_log;
    connection_callbacks.rumble               = cb_rumble;
    connection_callbacks.connectionStatusUpdate = cb_status_update;
    connection_callbacks.setHdrMode           = cb_set_hdr;
    connection_callbacks.rumbleTriggers       = cb_rumble_triggers;
    connection_callbacks.setMotionEventState  = cb_set_motion;
    connection_callbacks.setControllerLED     = cb_set_led;
    NLOG("cb_init: done");
}
