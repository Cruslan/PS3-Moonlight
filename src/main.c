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

#include "audio.h"
#include "connection.h"
#include "handshake.h"
#include "net_logger.h"
#include "random.h"
#include "ui.h"
#include "video.h"
#include "input.h"
#include <ps3perf.h>

SYS_PROCESS_PARAM(1001, 0x100000)

// PS3 watchdog'u besleme - şimdilik devre dışı (crash yapıyor olabilir)
// static void pump_sysutil() { sysUtilCheckCallback(); }

/* PSL1GHT libnet gatekeeper: every socket call checks LIBNET_INITIALZED (__netMemory != NULL).
 * When using a custom high-throughput network pool via netInitializeNetworkEx, we MUST set __netMemory,
 * otherwise socket(), connect(), send(), etc. will fail immediately with errno = ENOSYS (88). */
extern void *__netMemory;

/* Dedicated high-throughput sys_net memory pool to prevent mbuf starvation and Lv-2 kernel DSI */
static void *g_net_memory_pool = NULL;

static int ps3_net_init(void) {
  sysModuleLoad(SYSMODULE_NET);

  /* Allocate a dedicated 4MB / 2MB memory pool for the PS3 kernel network subsystem.
   * PSL1GHT's default netInitialize() only allocates 128 KB (0x20000). Moonlight's
   * video (64KB) and audio (64KB) UDP socket receive buffers consume the entire 128KB,
   * leading to kernel mbuf pool exhaustion (ENOBUFS) and Lv-2 Data Storage Interrupt
   * crashes (DAR=0x18 null pointer dereference) under continuous streaming. */
  static const uint32_t pool_sizes[] = {
    4 * 1024 * 1024,  /* 4 MB (32x default) */
    2 * 1024 * 1024,  /* 2 MB (16x default) */
    1024 * 1024,      /* 1 MB (8x default) */
    512 * 1024        /* 512 KB (4x default) */
  };

  for (size_t i = 0; i < sizeof(pool_sizes) / sizeof(pool_sizes[0]); i++) {
    uint32_t sz = pool_sizes[i];
    g_net_memory_pool = memalign(4096, sz);
    if (!g_net_memory_pool) continue;

    netInitParam param;
    memset(&param, 0, sizeof(param));
    param.memory = (u32)(uintptr_t)g_net_memory_pool;
    param.memory_size = sz;
    param.flags = 0;

    int ret = netInitializeNetworkEx(&param);
    if (ret == 0) {
      __netMemory = g_net_memory_pool;
      netCtlInit();
      return 0;
    }

    free(g_net_memory_pool);
    g_net_memory_pool = NULL;
  }

  // Fallback to standard 128KB default if custom sizes fail
  int ret = netInitialize();
  netCtlInit();
  return ret;
}

static void ps3_net_shutdown(void) {
  netCtlTerm();
  if (g_net_memory_pool) {
    __netMemory = NULL;
    netFinalizeNetwork();
    free(g_net_memory_pool);
    g_net_memory_pool = NULL;
  } else {
    netDeinitialize();
  }
  sysModuleUnload(SYSMODULE_NET);
}

static void sysutil_exit_callback(u64 status, u64 param, void *usrdata) {
  (void)param;
  (void)usrdata;
  if (status == SYSUTIL_EXIT_GAME) {
    NLOG("SYSUTIL_EXIT_GAME received. Exiting Moonlight PS3...");
    LiInterruptConnection();
    ui_stop();
  }
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  /* Enable Hardware Flush-to-Zero (Non-IEEE mode) on Cell PPE.
   * Bit 29 of FPSCR is NI (Non-IEEE mode bit). Setting it forces subnormal
   * float operands/results to zero in 1 cycle, preventing high-latency microcode
   * assist interrupt traps during quiet audio, fading sound, or floating-point stats. */
  __asm__ volatile("mtfsb1 29" ::: "memory");

  // Ağı başlat (Genişletilmiş yüksek verimli bellek havuzu ile)
  ps3_net_init();

  // Register sysutil callback to handle game exit
  sysUtilRegisterCallback(0, sysutil_exit_callback, NULL);

  // Logger başlat
  net_logger_init();
  ps3perf_init();


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

  int is_4_3 = 0;
  if (vstate.displayMode.aspect == VIDEO_ASPECT_4_3) {
    is_4_3 = 1;
  } else if (vstate.displayMode.aspect == VIDEO_ASPECT_16_9) {
    is_4_3 = 0;
  } else if (height > 0 && ((float)width / (float)height) < 1.5f) {
    is_4_3 = 1;
  }

  NLOG("Detected Display: %dx%d (%s)", width, height, is_4_3 ? "4:3 Standard/CRT" : "16:9 Widescreen");

  ui_init(width, height, is_4_3);
  ps3input_start();
  NLOG("Moonlight PS3 UI Initialized");
  
  while (ui_is_running()) {
    if (ui_get_state() == UI_STATE_PAIRING) {
      const char *pcIp = ui_get_target_ip();
      NLOG("Attempting to connect to: %s", pcIp);

      ui_set_pairing_pin("");

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
          ui_set_pairing_pin(pin);
          char pin_log[96];
          snprintf(pin_log, sizeof(pin_log), "Pairing required! PIN: %s (Enter in Sunshine)", pin);
          ui_push_log(pin_log);
          NLOG("H: Attempting Pairing (Check Sunshine for PIN %s)...", pin);
          if (hv_pair(&hinfo, pin) != 0) {
            ui_set_pairing_pin("");
            if (ui_get_state() == UI_STATE_IP_ENTRY) continue;
            // If user cancelled, they are already at UI_STATE_IP_ENTRY.
            // Only set UI_STATE_ERROR if it wasn't a deliberate cancel.
            if (ui_get_state() == UI_STATE_PAIRING) {
                NLOG("H: Pairing process failed or timed out.");
                ui_set_state(UI_STATE_ERROR);
            }
            continue;
          }
          ui_push_log("H: Pairing succeeded!");
      } else {
          NLOG("H: Already paired with server. Skipping PIN entry.");
      }

      ui_set_pairing_pin("");

      if (ui_get_state() == UI_STATE_IP_ENTRY) continue; // Safety check

      NLOG("H: Fetching server info...");
      if (hv_get_server_info(&hinfo) != 0) {
        if (ui_get_state() == UI_STATE_IP_ENTRY) continue;
        strncpy(hinfo.server_app_version, "7.1.431.0", sizeof(hinfo.server_app_version) - 1);
      }

      if (ui_get_state() == UI_STATE_IP_ENTRY) continue;

      NLOG("H: Fetching app list...");
      ps3_app_list_t app_list = {0};
      if (hv_get_app_list(&hinfo, &app_list) != 0 || app_list.count == 0) {
        if (ui_get_state() == UI_STATE_IP_ENTRY) continue;
        NLOG("H: Failed to fetch app list or no apps found.");
        ui_set_state(UI_STATE_ERROR);
        continue;
      }

      // Transition to App Selection state
      ui_set_app_list(&app_list);
      ui_reset_app_selection();
      ui_set_state(UI_STATE_APPLIST);
      NLOG("H: Waiting for user to select an app from UI (found %d apps)...", app_list.count);

      // Wait for user confirmation or cancellation
      while (ui_is_running() && ui_get_state() == UI_STATE_APPLIST && !ui_is_app_selected()) {
        sysUtilCheckCallback();
        usleep(20000);
      }

      if (!ui_is_running() || ui_get_state() != UI_STATE_APPLIST) {
        NLOG("H: App selection cancelled or aborted.");
        continue;
      }

      int app_id = ui_get_selected_app_id();
      const char *app_name = ui_get_selected_app_name();
      NLOG("H: User selected App '%s' (ID: %d)", app_name, app_id);

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

      NLOG("H: Launching App '%s' (ID %d)...", app_name, app_id);
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
      streamConfig.width = ui_get_stream_width();
      streamConfig.height = ui_get_stream_height();
      streamConfig.fps = ui_get_fps(); // Dynamic FPS from UI selection
      streamConfig.bitrate = ui_get_bitrate(); // Dynamic bitrate from UI selection
      streamConfig.packetSize = 1024; // Smaller packets reduce PS3 kernel mbuf pressure
      streamConfig.streamingRemotely = STREAM_CFG_LOCAL;
      streamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
      streamConfig.supportedVideoFormats = VIDEO_FORMAT_H264;

      /* Protocol-level frame pacing: Notify Sunshine / GFE of PS3 hardware refresh rate.
       * PS3 NTSC displays operate at 59.94 Hz (5994). When notified, modern host capture
       * pipelines pace frames at exactly 59.940 Hz instead of 60.000 Hz, eliminating
       * the 1-frame-every-16.7s clock drift right at the source. */
      if (vres.height == 576) {
        streamConfig.clientRefreshRateX100 = 5000; // 50.00 Hz PAL standard
      } else {
        streamConfig.clientRefreshRateX100 = 5994; // 59.94 Hz NTSC standard
      }

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
            ps3perf_tick(5);
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
    ps3perf_tick(5);
    usleep(50000);
  }

  NLOG("Terminating Moonlight PS3 application...");
  LiStopConnection();
  ps3audio_stop();
  ps3video_stop();
  ps3input_stop();
  ui_shutdown();
  ps3perf_shutdown();
  net_logger_shutdown();
  sysUtilUnregisterCallback(0);
  usleep(100000); // 100ms graceful drain for final network sockets
  ps3_net_shutdown();
  return 0;
}
