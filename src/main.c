#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <stddef.h>
#include <unistd.h>

#include <net/net.h>
#include <net/netctl.h>
#include <sys/process.h>
#include <sysmodule/sysmodule.h>
#include <sysutil/sysutil.h>
#include <sysutil/video.h>
#include <tiny3d.h>

#include "audio/ps3.h"
#include "connection.h"
#include "handshake.h"
#include "net_logger.h"
#include "random.h"
#include "ui.h"
#include "video/ps3.h"
#include "input/ps3.h"

SYS_PROCESS_PARAM(1001, 0x100000)

// PS3 watchdog'u besleme - şimdilik devre dışı (crash yapıyor olabilir)
// static void pump_sysutil() { sysUtilCheckCallback(); }

static void sysutil_exit_callback(u64 status, u64 param, void *usrdata) {
  (void)param;
  (void)usrdata;
  if (status == SYSUTIL_EXIT_GAME) {
    NLOG("SYSUTIL_EXIT_GAME received. Exiting Moonlight PS3...");
    ui_stop();
  }
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  // Ağı başlat
  // Ağı başlat
  sysModuleLoad(SYSMODULE_NET);
  netInitialize();
  netCtlInit();

  // Register sysutil callback to handle game exit
  sysUtilRegisterCallback(0, sysutil_exit_callback, NULL);

  // Logger başlat
  net_logger_init();

  // Çözünürlük Tespiti
  videoState vstate;
  videoResolution vres;
  videoGetState(VIDEO_PRIMARY, 0, &vstate);
  videoGetResolution(vstate.displayMode.resolution, &vres);

  int width = vres.width;
  int height = vres.height;

  // Eğer çözünürlük tanımsızsa varsayılan 720p kullan
  if (width == 0 || height == 0) {
    width = 1280;
    height = 720;
  }

  NLOG("Detected Resolution: %dx%d", width, height);

  ui_init(width, height);
  ps3input_start();
  NLOG("Moonlight PS3 UI Initialized");
  
  while (ui_is_running()) {
    if (ui_get_state() == UI_STATE_PAIRING) {
      const char *pcIp = ui_get_target_ip();
      NLOG("Attempting to connect to: %s", pcIp);

      // Handshake
      NLOG("H: Initializing Handshake...");
      handshake_info_t hinfo = {0};
      if (hv_init(&hinfo, pcIp) != 0) {
        if (ui_get_state() == UI_STATE_IP_ENTRY) continue;
        NLOG("H: hv_init failed!");
        ui_set_state(UI_STATE_ERROR);
        continue;
      }

      uint32_t random_value;
      char pin[5];
      if (ps3_random_u32(&random_value) != 0) {
        NLOG("H: Secure random number generation failed.");
        ui_set_state(UI_STATE_ERROR);
        continue;
      }
      snprintf(pin, sizeof(pin), "%04u", (unsigned int)(random_value % 10000));
      int paired = hv_is_paired(&hinfo);
      if (ui_get_state() == UI_STATE_IP_ENTRY) continue;
      
      if (!paired) {
          NLOG("H: Attempting Pairing (Check Sunshine for PIN %s)...", pin);
          if (hv_pair(&hinfo, pin) != 0) {
            if (ui_get_state() == UI_STATE_IP_ENTRY) continue;
            // If user cancelled, they are already at UI_STATE_IP_ENTRY.
            // Only set UI_STATE_ERROR if it wasn't a deliberate cancel.
            if (ui_get_state() == UI_STATE_PAIRING) {
                NLOG("H: Pairing process failed or timed out.");
                ui_set_state(UI_STATE_ERROR);
            }
            continue;
          }
      } else {
          NLOG("H: Already paired with server. Skipping PIN entry.");
      }

      if (ui_get_state() == UI_STATE_IP_ENTRY) continue; // Safety check

      NLOG("H: Fetching server info...");
      if (hv_get_server_info(&hinfo) != 0) {
        if (ui_get_state() == UI_STATE_IP_ENTRY) continue;
        strncpy(hinfo.server_app_version, "7.1.431.0", sizeof(hinfo.server_app_version) - 1);
      }

      if (ui_get_state() == UI_STATE_IP_ENTRY) continue;

      unsigned char rikey_bin[16];
      char rikey_hex[33];
      if (ps3_random_bytes(rikey_bin, sizeof(rikey_bin)) != 0 ||
          ps3_random_u32(&random_value) != 0) {
        NLOG("H: Failed to generate secure session keys.");
        ui_set_state(UI_STATE_ERROR);
        continue;
      }
      for (int i = 0; i < 16; i++) sprintf(rikey_hex + (i * 2), "%02X", rikey_bin[i]);
      rikey_hex[32] = '\0';
      int rikeyid = (int)(random_value % 1000000);

      NLOG("H: Fetching app list...");
      int app_id = hv_get_first_appid(&hinfo);
      if (ui_get_state() == UI_STATE_IP_ENTRY) continue;
      if (app_id <= 0) {
        NLOG("H: No valid app ID found.");
        ui_set_state(UI_STATE_ERROR);
        continue;
      }

      NLOG("H: Launching App ID %d...", app_id);
      if (hv_launch(&hinfo, app_id, rikey_hex, rikeyid) != 0) {
        if (ui_get_state() == UI_STATE_IP_ENTRY) continue;
        NLOG("H: hv_launch failed.");
        ui_set_state(UI_STATE_ERROR);
        continue;
      }

      // If we reach here, launch was successful!
      // Setup Stream
      STREAM_CONFIGURATION streamConfig;
      LiInitializeStreamConfiguration(&streamConfig);
      streamConfig.width = 1280;
      streamConfig.height = 720;
      streamConfig.fps = ui_get_fps(); // Dynamic FPS from UI selection
      streamConfig.bitrate = ui_get_bitrate(); // Dynamic bitrate from UI selection
      streamConfig.packetSize = 1024; // Smaller packets reduce PS3 kernel mbuf pressure
      streamConfig.streamingRemotely = STREAM_CFG_LOCAL;
      streamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
      streamConfig.supportedVideoFormats = VIDEO_FORMAT_H264;
      memcpy(streamConfig.remoteInputAesKey, rikey_bin, 16);

      connection_callbacks_init();
      
      SERVER_INFORMATION server;
      LiInitializeServerInformation(&server);
      server.address = (char*)pcIp;
      server.serverInfoAppVersion = hinfo.server_app_version;
      server.serverInfoGfeVersion = "3.23.0.74";
      server.rtspSessionUrl = hinfo.rtsp_session_url;
      server.serverCodecModeSupport = SCM_H264;

      int ret = LiStartConnection(&server, &streamConfig, &connection_callbacks,
                                  &decoder_callbacks_ps3, &audio_callbacks_ps3,
                                  NULL, 0, NULL, 0);
      
      if (ret == 0) {
        NLOG("Connection Start initiated. Waiting for stream...");
        ui_set_state(UI_STATE_STREAMING);
        
        // Wait for connection to actually start or fail
        int timeout_wait = 0;
        while (ui_is_running() && !connection_is_connected() && connection_is_ready() && timeout_wait < 100) {
          sysUtilCheckCallback();
          usleep(100000);
          timeout_wait++;
        }

        if (ui_is_running() && connection_is_connected()) {
          NLOG("Connection fully established!");
          while (ui_is_running() && connection_is_ready() && ui_get_state() == UI_STATE_STREAMING) {
            sysUtilCheckCallback();
            
            // Check for emergency exit key combination (Select + Start + L3 + R3)
            ps3_pad_state_t pad;
            ps3input_get_data(&pad);
            int exit_combo = PLAY_FLAG | BACK_FLAG | LS_CLK_FLAG | RS_CLK_FLAG;
            if ((pad.buttons_down & exit_combo) == exit_combo) {
              NLOG("Emergency exit combo pressed! Returning to menu...");
              ui_set_state(UI_STATE_IP_ENTRY);
            }
            
            vdec_poll();
            usleep(1000); // 1000Hz poll: with DIRECT_SUBMIT, vdec_poll drains decoder output for RSX render
          }
        } else {
          NLOG("Connection failed to establish timely.");
          ui_set_state(UI_STATE_ERROR);
        }
        
        NLOG("Returning to Main Menu.");
        LiStopConnection();
        ui_set_state(UI_STATE_IP_ENTRY);
      } else {
        NLOG("LiStartConnection failed: %d", ret);
        ui_set_state(UI_STATE_ERROR);
      }
    }
    
    sysUtilCheckCallback();
    usleep(50000);
  }

  ui_shutdown();
  ps3input_stop();
  net_logger_shutdown();
  sysUtilUnregisterCallback(0);
  netCtlTerm();
  netDeinitialize();
  sysModuleUnload(SYSMODULE_NET);
  return 0;
}
