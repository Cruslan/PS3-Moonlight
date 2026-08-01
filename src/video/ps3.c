#include <lv2/systime.h>
#include <codec/vdec.h>
#include <io/pad.h>
#include <malloc.h>
#include <ppu-asm.h>
#include <ppu-types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/process.h>
#include <sysmodule/sysmodule.h>
#include <sysutil/sysutil.h>
#include <tiny3d.h>
#include <unistd.h>

// Unused SPS header from libgamestream
// #include "../../libgamestream/sps.h"
#include "ps3.h"
#include "../ui.h"
#include <Limelight.h>
#include <rsx/rsx.h>
#include <rsx/gcm_sys.h>
#include <sys/mutex.h>

#define VDEC_MEM_SIZE (10 * 1024 * 1024)

static u64 last_fps_time = 0;
static int frames_this_second = 0;
static int current_video_fps = 0;

static int decoded_frames_this_second = 0;
static int current_decoded_fps = 0;

#define SUBMIT_QUEUE_SIZE 16
static u64 submit_queue[SUBMIT_QUEUE_SIZE];
static int submit_head = 0;
static int submit_tail = 0;
static int submit_count = 0;

static u64 decode_latency_sum = 0;
static u32 decode_latency_count = 0;
static u64 render_latency_sum = 0;
static u32 render_latency_count = 0;

static int current_decode_latency_ms = 0;
static int current_render_latency_ms = 0;
static int current_net_latency_ms = 0;

// Triple buffering state
static int front_buf = -1; // Currently displaying
static int write_buf = 0;  // Currently decoding into
static sys_mutex_t frame_mutex;
static int mutex_initialized = 0;

static u32 vdec_handle = 0;
static void *vdec_mem_addr = NULL;

// Ping-pong bitstream buffers: one is submitted to VDEC ("back"),
// the other is filled by the network thread ("front").
// We swap them only after AUDONE to guarantee VDEC never reads a
// buffer that is being written or realloc'd.
#define VDEC_BUF_COUNT 4
#define VDEC_DECODE_BUF_INITIAL (512 * 1024)
static u8  *vdec_buf[VDEC_BUF_COUNT]   = {NULL, NULL, NULL, NULL};
static u32  vdec_buf_size[VDEC_BUF_COUNT] = {0, 0, 0, 0};
static int  vdec_front    = 0; // index we fill
static int  vdec_back     = 0; // index last submitted
// Legacy aliases kept for minimal diff in the rest of the file:
#define vdec_decode_buffer      vdec_buf[vdec_front]
#define vdec_decode_buffer_size vdec_buf_size[vdec_front]

// Decoded frame buffer (system RAM — VDEC cannot write directly to RSX)
#define VDEC_FRAME_COUNT 4
static void *vdec_frame_bufs[VDEC_FRAME_COUNT] = {NULL, NULL, NULL, NULL}; // memalign'd, size = width*height*4
static u64 frame_ready_time[VDEC_FRAME_COUNT];
static u32 vdec_frame_buf_size = 0;

static volatile int vdec_picout_pending = 0;
// set by callback, cleared by poll

// RSX render target — mapped from system RAM (zero-copy: VDEC writes here, RSX reads directly)
static u32 video_texture_rsx_offsets[VDEC_FRAME_COUNT] = {0, 0, 0, 0};  // RSX offsets of vdec_frame_bufs

static int video_width = 1280;
static int video_height = 720;
static u32 vdec_detected_pitch = 0; // Set in setup

static int ready_queue[VDEC_FRAME_COUNT];
static int ready_head = 0;
static int ready_tail = 0;
static int ready_count = 0;

static volatile int vdec_au_pending  = 0; // count of AUs in hardware queue
static volatile int vdec_seq_done    = 0; // set by SEQDONE callback

// vdec_callback: runs on RPCS3's internal HLE thread.
// CRITICAL: must NOT make any PS3/LV2 API calls here (causes reentrant crash).
// Just set flags; the submit / main threads act on them.
//
// Callback types per PSL1GHT / Movian:
//   VDEC_CALLBACK_AUDONE  = 1  — AU consumed, buffer free
//   VDEC_CALLBACK_PICOUT  = 2  — decoded picture ready
//   VDEC_CALLBACK_SEQDONE = 3  — EndSequence has completed (async!)
//   VDEC_CALLBACK_ERROR   = 4  — fatal decoder error
static u32 vdec_callback(u32 handle, u32 msgtype, u32 msgdata, u32 arg) {
  (void)handle;
  (void)msgdata;
  (void)arg;
  if (msgtype == VDEC_CALLBACK_PICOUT) {
    vdec_picout_pending = 1;
  } else if (msgtype == VDEC_CALLBACK_AUDONE) {
    if (vdec_au_pending > 0)
        vdec_au_pending--;
  } else if (msgtype == VDEC_CALLBACK_SEQDONE) {
    // vdecEndSequence() has completed asynchronously.
    vdec_seq_done = 1;
  } else if (msgtype == 4 /* VDEC_CALLBACK_ERROR */) {
    printf("VDEC CALLBACK ERROR: 0x%x\n", msgdata);
    vdec_au_pending = 0;
    vdec_seq_done   = 1; // unblock any waiter
  }
  return 0;
}

// Called from the main render thread to consume a pending decoded picture.
void vdec_poll(void) {
  if (!ps3video_is_active() || !vdec_frame_bufs[0] || !vdec_handle)
    return;

  vdec_picout_pending = 0;

  vdecPictureFormat fmt;
  memset(&fmt, 0, sizeof(fmt));
  fmt.format_type = 0; // VDEC_PICFMT_ARGB32
  fmt.color_matrix = VDEC_COLOR_MATRIX_BT709;
  fmt.alpha = 255;

  while (1) {
    s32 ret = vdecGetPicture(vdec_handle, &fmt, vdec_frame_bufs[write_buf]);
    if (ret != 0) break; // No more pictures

    if (ui_get_show_stats()) {
        if (submit_count > 0) {
            u64 submit_time = submit_queue[submit_tail];
            submit_tail = (submit_tail + 1) % SUBMIT_QUEUE_SIZE;
            submit_count--;
            decode_latency_sum += (sysGetSystemTime() - submit_time);
            decode_latency_count++;
        }
    }

    u32 pic_addr = 0;
    if (vdecGetPicItem(vdec_handle, &pic_addr) == 0 && pic_addr != 0) {
      vdecPicture *pic = (vdecPicture *)(uintptr_t)pic_addr;
      vdecH264Info *info = (vdecH264Info *)(uintptr_t)pic->codec_specific_addr;
      if (info && info->height > 0) {
        u32 expected_argb = info->width * info->height * 4;
        u32 expected_yuv  = info->width * info->height * 3 / 2;
        if (pic->picture_size == expected_yuv) {
          vdec_detected_pitch = info->width;
        } else if (pic->picture_size >= expected_argb) {
          vdec_detected_pitch = pic->picture_size / info->height;
        } else {
          vdec_detected_pitch = info->width * 4;
        }
      }
    }
    
    if (ui_get_show_stats()) {
        frame_ready_time[write_buf] = sysGetSystemTime();
    }

    if (mutex_initialized) sysMutexLock(frame_mutex, 0);
    
    if (ready_count < VDEC_FRAME_COUNT - 2) {
        ready_queue[ready_head] = write_buf;
        ready_head = (ready_head + 1) % VDEC_FRAME_COUNT;
        ready_count++;
    } else {
        // Drop the oldest frame to make room
        ready_queue[ready_head] = write_buf;
        ready_head = (ready_head + 1) % VDEC_FRAME_COUNT;
        ready_tail = (ready_tail + 1) % VDEC_FRAME_COUNT;
    }
    
    // Find next available write_buf
    int next_write = (write_buf + 1) % VDEC_FRAME_COUNT;
    while(1) {
        int is_free = 1;
        if (next_write == front_buf) is_free = 0;
        for (int j = 0; j < ready_count; j++) {
            if (ready_queue[(ready_tail + j) % VDEC_FRAME_COUNT] == next_write) {
                is_free = 0;
                break;
            }
        }
        if (is_free) break;
        next_write = (next_write + 1) % VDEC_FRAME_COUNT;
    }
    write_buf = next_write;
    
    if (ui_get_show_stats()) {
        decoded_frames_this_second++;
    }
    if (mutex_initialized) sysMutexUnlock(frame_mutex);
  }
}

static int ps3_video_setup(int videoFormat, int width, int height,
                           int redrawRate, void *context, int drFlags) {
  printf("PS3 Video Setup: %dx%d @ %d fps\n", width, height, redrawRate);

  // Load VDEC base module. H264 codec support is built into the main VDEC
  // module on real PS3 hardware. SYSMODULE_VDEC_H264 is a separate SPU firmware
  // module that VDEC loads internally — we must NOT manually load it.
  s32 mod_rc = sysModuleLoad(SYSMODULE_VDEC);
  if (mod_rc != 0) {
    printf(
        "sysModuleLoad(SYSMODULE_VDEC) returned %d (may already be loaded)\n",
        mod_rc);
  }

  // profile_level for AVC (H264): CELL_VDEC_AVC_LEVEL_* enum values
  // From RPCS3/cellVdec.h: LEVEL_1P0=10, LEVEL_2P0=20, LEVEL_3P0=30,
  //                        LEVEL_4P0=40, LEVEL_4P1=41, LEVEL_4P2=42
  // Memory required per RPCS3 source: Level 4.1 = 0x33A5FFD (~54MB)
  // We must provide at least that much memory or vdecOpen returns
  // VDEC_ERROR_ARG.
  static const u32 profile_levels[] = {41, 42, 40, 32, 31};
  // mem_sizes in MB — must be >= 54MB for Level 4.1:
  static const u32 mem_sizes[] = {55, 60, 64, 70};

  vdecType type;
  type.codec_type = VDEC_CODEC_TYPE_H264;

  vdecConfig config;
  memset(&config, 0, sizeof(config));
  config.ppu_thread_prio = 500;
  config.ppu_thread_stack_size = 0x10000;
  config.spu_thread_prio = 250;
  config.num_spus = 6;

  static opd32 vdec_callback_opd;
  vdecClosure closure;
  closure.fn = __build_opd32(vdec_callback, &vdec_callback_opd);
  closure.arg = (u32)0;

  int opened = 0;
  for (int mi = 0; mi < 4 && !opened; mi++) {
    u32 mem_bytes = mem_sizes[mi] * 1024 * 1024;
    if (vdec_mem_addr) {
      free(vdec_mem_addr);
      vdec_mem_addr = NULL;
    }
    vdec_mem_addr = memalign(1024 * 1024, mem_bytes);
    if (!vdec_mem_addr) {
      printf("Failed to allocate %uMB for VDEC\n", mem_sizes[mi]);
      continue;
    }
    config.mem_addr = (u32)(uintptr_t)vdec_mem_addr;
    config.mem_size = mem_bytes;

    for (int li = 0; li < 5 && !opened; li++) {
      type.profile_level = profile_levels[li];
      int rc = vdecOpen(&type, &config, &closure, &vdec_handle);
      if (rc == 0) {
        printf("vdecOpen OK: profile_level=%u, mem=%uMB\n", profile_levels[li],
               mem_sizes[mi]);
        opened = 1;
      } else {
        printf("vdecOpen failed: profile_level=%u, mem=%uMB, rc=0x%x\n",
               profile_levels[li], mem_sizes[mi], rc);
      }
    }
  }

  if (!opened) {
    printf("All vdecOpen attempts failed — no H264 decoder available.\n");
    if (vdec_mem_addr) {
      free(vdec_mem_addr);
      vdec_mem_addr = NULL;
    }
    return -1;
  }

  vdecStartSequence(vdec_handle);

  // Allocate all buffers.
  for (int bi = 0; bi < VDEC_BUF_COUNT; bi++) {
    if (!vdec_buf[bi]) {
      vdec_buf[bi] = memalign(128, VDEC_DECODE_BUF_INITIAL);
      vdec_buf_size[bi] = VDEC_DECODE_BUF_INITIAL;
    }
  }
  vdec_front = 0;
  vdec_back  = 0;

  video_width = width;
  video_height = height;

  // Allocate frame decode buffer in system RAM.
  // gcmMapMainMemory requires 1MB alignment and size rounded up to 1MB.
  vdec_frame_buf_size = (u32)(width * height * 4);
  u32 align_size = (vdec_frame_buf_size + 0xFFFFF) & ~0xFFFFF;
  vdec_detected_pitch = width * 4;

  for (int i = 0; i < VDEC_FRAME_COUNT; i++) {
      if (!vdec_frame_bufs[i]) {
          vdec_frame_bufs[i] = memalign(1024 * 1024, align_size);
          if (!vdec_frame_bufs[i]) {
              printf("Failed to alloc vdec_frame_bufs[%d] (%u bytes)\n", i, align_size);
              vdecEndSequence(vdec_handle);
              vdecClose(vdec_handle);
              free(vdec_mem_addr);
              vdec_mem_addr = NULL;
              return -1;
          }
          memset(vdec_frame_bufs[i], 0, align_size);
          printf("vdec_frame_bufs[%d] allocated: %u bytes (aligned %u) @ %p\n",
                 i, vdec_frame_buf_size, align_size, vdec_frame_bufs[i]);

          s32 map_rc = gcmMapMainMemory(vdec_frame_bufs[i], align_size, &video_texture_rsx_offsets[i]);
          if (map_rc != 0) {
              printf("gcmMapMainMemory failed: 0x%x — video disabled\n", map_rc);
              video_texture_rsx_offsets[i] = 0;
          } else {
              printf("vdec_frame_bufs[%d] mapped to RSX offset: 0x%x\n", i, video_texture_rsx_offsets[i]);
          }
      }
  }

  // No separate RSX texture allocation needed:
  // vdec_frame_bufs are in system RAM and will be used directly via rsxAddressToOffset.

  sys_mutex_attr_t attr;
  sysMutexAttrInitialize(attr);
  if (sysMutexCreate(&frame_mutex, &attr) == 0) {
      mutex_initialized = 1;
      printf("frame_mutex initialized\n");
  } else {
      printf("Failed to initialize frame_mutex\n");
  }

  // Set GCM flip mode: VSYNC (Default) or HSYNC (Immediate Flip / VSync OFF) based on UI settings
  if (ui_get_vsync()) {
      gcmSetFlipMode(GCM_FLIP_VSYNC);
      printf("PS3 Video Setup: VSync ENABLED\n");
  } else {
      gcmSetFlipMode(GCM_FLIP_HSYNC);
      printf("PS3 Video Setup: VSync DISABLED (Immediate Flip)\n");
  }

  ps3video_start();
  printf("PS3 Video Setup complete.\n");
  return 0;
}

static void ps3_video_cleanup() {
  printf("PS3 Video Cleanup\n");
  ps3video_stop();
  usleep(32000); // Wait 2 frames to ensure main thread exits ps3video_draw

  if (mutex_initialized) {
      sysMutexDestroy(frame_mutex);
      mutex_initialized = 0;
  }

  // End VDEC sequence and wait for async SEQDONE before closing.
  vdec_seq_done = 0;
  vdecEndSequence(vdec_handle);
  {
    int t = 2000;
    while (!vdec_seq_done && t-- > 0) usleep(100);
  }
  vdecClose(vdec_handle);
  if (vdec_mem_addr) {
    free(vdec_mem_addr);
    vdec_mem_addr = NULL;
  }
  for (int bi = 0; bi < VDEC_BUF_COUNT; bi++) {
    if (vdec_buf[bi]) {
      free(vdec_buf[bi]);
      vdec_buf[bi] = NULL;
      vdec_buf_size[bi] = 0;
    }
  }
  for (int i = 0; i < VDEC_FRAME_COUNT; i++) {
      if (vdec_frame_bufs[i]) {
          free(vdec_frame_bufs[i]);
          vdec_frame_bufs[i] = NULL;
      }
  }
  sysModuleUnload(SYSMODULE_VDEC);
}

static int ps3_video_submit_decode_unit(PDECODE_UNIT decodeUnit) {
  // Non-blocking check: if VDEC hardware queue is full, drop this frame
  // immediately.  We must NEVER block here because in queue-based mode the
  // decoder thread must stay responsive to keep draining the decode unit
  // queue.  The main thread's vdec_poll() (1000Hz) is responsible for
  // freeing VDEC slots by consuming decoded pictures.
  if (vdec_au_pending >= VDEC_BUF_COUNT) {
    return DR_OK; // silently drop — main thread will free slots via vdec_poll()
  }

  // We now own vdec_front exclusively (VDEC owns vdec_back and it is done).
  // Grow the front buffer if needed — safe because VDEC is NOT reading it.
  u32 needed = (u32)decodeUnit->fullLength + 64;
  if (vdec_buf_size[vdec_front] < needed) {
    u32 new_size = needed + 2048;
    u8 *new_buf = memalign(128, new_size);
    if (new_buf) {
      free(vdec_buf[vdec_front]);
      vdec_buf[vdec_front] = new_buf;
      vdec_buf_size[vdec_front] = new_size;
    } else {
      printf("VDEC: failed to grow front buffer to %u\n", new_size);
      return DR_OK;
    }
  }

  // Step 1: Concatenate all fragments (entries) into one continuous buffer
  u32 length = 0;
  PLENTRY entry = decodeUnit->bufferList;
  while (entry != NULL) {
    if (length + entry->length <= vdec_decode_buffer_size) {
      memcpy(vdec_decode_buffer + length, entry->data, entry->length);
      length += entry->length;
    }
    entry = entry->next;
  }

  // Step 2: Convert from AVCC (length-prefixed) to Annex B (start-code prefixed)
  // if necessary, handling multi-NALU packets correctly.
  //
  // Moonlight sends H.264 in AVCC format: each NAL unit is prefixed with a
  // 4-byte big-endian length field.  libvdec requires Annex B format where
  // every NAL unit is instead prefixed with the 4-byte start code 00 00 00 01.
  // We must walk the entire buffer and replace ALL length fields, not just the
  // first one, otherwise subsequent NAL units in the same AU are corrupted.
  const u8 start_code[] = {0, 0, 0, 1};
  if (length > 4) {
    // Detect whether the buffer is already in Annex B format.
    int already_annexb = (memcmp(vdec_decode_buffer, start_code, 4) == 0) ||
                         (memcmp(vdec_decode_buffer, start_code + 1, 3) == 0);

    if (!already_annexb) {
      // Assume AVCC: walk the buffer replacing each 4-byte length with
      // 00 00 00 01.  The in-place replacement is safe because both are
      // the same width (4 bytes).
      u32 pos = 0;
      int converted = 0;
      while (pos + 4 <= length) {
        u32 nalu_len = ((u32)vdec_decode_buffer[pos]     << 24) |
                       ((u32)vdec_decode_buffer[pos + 1] << 16) |
                       ((u32)vdec_decode_buffer[pos + 2] <<  8) |
                        (u32)vdec_decode_buffer[pos + 3];

        if (nalu_len == 0 || pos + 4 + nalu_len > length) {
          // Malformed AVCC or not AVCC at all — stop conversion.
          break;
        }

        // Replace the 4-byte length field with the Annex B start code.
        memcpy(vdec_decode_buffer + pos, start_code, 4);
        converted++;
        pos += 4 + nalu_len;
      }

      if (!converted) {
        // Not AVCC and no start code — prepend one start code and submit.
        if (length + 4 <= vdec_decode_buffer_size) {
          memmove(vdec_decode_buffer + 4, vdec_decode_buffer, length);
          memcpy(vdec_decode_buffer, start_code, 4);
          length += 4;
        }
      }
    }
  }

  // Submit the FRONT buffer to VDEC.  After this call VDEC takes ownership
  // of vdec_buf[vdec_front]; the AUDONE callback will swap front/back.
  vdecAU au;
  au.packet_addr = (u32)(uintptr_t)vdec_buf[vdec_front];
  au.packet_size = length;
  au.pts.low = 0;
  au.pts.hi = 0;
  au.dts.low = 0;
  au.dts.hi = 0;
  au.userdata = 0;

  vdec_au_pending++;

  s32 dec_rc = vdecDecodeAu(vdec_handle, VDEC_DECODER_MODE_NORMAL, &au);
  if (dec_rc != 0) {
    // 0x80610004 = VDEC_ERROR_BUSY (input queue full)
    if ((u32)dec_rc == 0x80610004) {
      // Retry once immediately in case a slot just freed up
      dec_rc = vdecDecodeAu(vdec_handle, VDEC_DECODER_MODE_NORMAL, &au);
    }
    
    if (dec_rc != 0) {
        if ((u32)dec_rc == 0x80610004) {
            printf("vdecDecodeAu: BUSY (overflow) — dropping AU\n");
        } else {
            printf("vdecDecodeAu failed: 0x%x\n", (u32)dec_rc);
        }
        vdec_au_pending--;
        return DR_OK;
    }
  }

  // Success! Cycle the front buffer index
  if (ui_get_show_stats()) {
      submit_queue[submit_head] = sysGetSystemTime();
      submit_head = (submit_head + 1) % SUBMIT_QUEUE_SIZE;
      submit_count++;
  }

  vdec_back = vdec_front;
  vdec_front = (vdec_front + 1) % VDEC_BUF_COUNT;

  return DR_OK;
}

DECODER_RENDERER_CALLBACKS decoder_callbacks_ps3 = {
    .setup = ps3_video_setup,
    .cleanup = ps3_video_cleanup,
    .submitDecodeUnit = ps3_video_submit_decode_unit,
    // Queue-based mode: a separate decoder thread dequeues frames and calls
    // submitDecodeUnit(), keeping the recv thread free to drain the socket.
    // The decode unit queue is sized to 120 frames on PS3 to absorb transient
    // VDEC stalls without overflowing.
    .capabilities = 0};

// Video Thread flag
static int active_video_thread = 0;

void ps3video_start() { active_video_thread = 1; }

void ps3video_stop() { active_video_thread = 0; }

int ps3video_is_active() { return active_video_thread; }

int ps3video_get_current_fps() { return current_video_fps; }
int ps3video_get_decoded_fps() { return current_decoded_fps; }
int ps3video_get_decode_latency() { return current_decode_latency_ms; }
int ps3video_get_render_latency() { return current_render_latency_ms; }
int ps3video_get_net_latency() { return current_net_latency_ms; }

void ps3video_draw() {
  if (!ps3video_is_active() || !vdec_frame_bufs[0])
    return;

  if (ui_get_show_stats()) {
      u64 now = sysGetSystemTime();
      if (last_fps_time == 0) last_fps_time = now;
      if (now - last_fps_time >= 1000000) {
        current_video_fps = frames_this_second;
        current_decoded_fps = decoded_frames_this_second;
        
        if (decode_latency_count > 0) {
            current_decode_latency_ms = (int)((decode_latency_sum / decode_latency_count) / 1000);
        } else {
            current_decode_latency_ms = 0;
        }
        if (render_latency_count > 0) {
            current_render_latency_ms = (int)((render_latency_sum / render_latency_count) / 1000);
        } else {
            current_render_latency_ms = 0;
        }

        uint32_t rtt = 0;
        if (LiGetEstimatedRttInfo(&rtt, NULL)) {
            current_net_latency_ms = (int)rtt;
        } else {
            current_net_latency_ms = 0;
        }

        decode_latency_sum = 0;
        decode_latency_count = 0;
        render_latency_sum = 0;
        render_latency_count = 0;

        frames_this_second = 0;
        decoded_frames_this_second = 0;
        last_fps_time = now;
      }
  }

  if (mutex_initialized) sysMutexLock(frame_mutex, 0);
  if (ready_count > 0) {
      front_buf = ready_queue[ready_tail];
      ready_tail = (ready_tail + 1) % VDEC_FRAME_COUNT;
      ready_count--;
      if (ui_get_show_stats()) {
          frames_this_second++;
          render_latency_sum += (sysGetSystemTime() - frame_ready_time[front_buf]);
          render_latency_count++;
      }
  }
  int draw_idx = front_buf;
  if (mutex_initialized) sysMutexUnlock(frame_mutex);

  if (draw_idx < 0 || video_texture_rsx_offsets[draw_idx] == 0) return;

  // tiny3d_SetTexture() hardcodes GCM_LOCATION_RSX (local memory).
  // Our frame buffer is in main (Cell) memory via gcmMapMainMemory.
  // We must use rsxLoadTexture() directly with GCM_LOCATION_CELL.
  gcmContextData *ctx = (gcmContextData *)tiny3d_Get_GCM_Context();
  if (ctx) {
    gcmTexture tex;
    memset(&tex, 0, sizeof(tex));
    // Linear ARGB32 texture format:
    // GCM_TEXTURE_FORMAT_LIN (0x20) | GCM_TEXTURE_FORMAT_A8R8G8B8 (0x85)
    tex.format    = 0x20 | 0x85; // LIN | A8R8G8B8
    tex.mipmap    = 1;
    tex.dimension = 2; // GCM_TEXTURE_DIMS_2D
    tex.cubemap   = 0;
    // Identity remap (ARGB -> ARGB):
    // Source components (indices): A=0, R=1, G=2, B=3
    // Destination (out): B=bits 6-7, G=bits 4-5, R=bits 2-3, A=bits 0-1
    tex.remap     = (2 << 14) | (2 << 12) | (2 << 10) | (2 << 8) | // All types = REMAP
                    (3 << 6)  | (2 << 4)  | (1 << 2)  | (0 << 0);  // B=3, G=2, R=1, A=0
    tex.width     = (u16)video_width;
    tex.height    = (u16)video_height;
    tex.depth     = 1;
    tex.location  = GCM_LOCATION_CELL; // 1 = main RAM (IOMMU mapped)
    tex.pitch     = (u32)(video_width * 4);
    tex.offset    = video_texture_rsx_offsets[draw_idx]; // from gcmMapMainMemory

    tiny3d_DoCmd_Space(64); // ensure command buffer has space
    rsxLoadTexture(ctx, 0, &tex);
    rsxTextureControl(ctx, 0, GCM_TRUE, 0, 12 << 8, 1); // enable, min/mag lod, maxaniso=1
    rsxTextureFilter(ctx, 0, 0, GCM_TEXTURE_LINEAR, GCM_TEXTURE_LINEAR,
                     GCM_TEXTURE_CONVOLUTION_QUINCUNX);
    rsxTextureWrapMode(ctx, 0, GCM_TEXTURE_CLAMP_TO_EDGE,
                       GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE,
                       0, GCM_TEXTURE_ZFUNC_LESS, 0);
  }

  int screen_width = ui_get_width();
  int screen_height = ui_get_height();
  if (screen_width <= 0 || screen_height <= 0) {
      screen_width = video_width;
      screen_height = video_height;
  }

  tiny3d_SetPolygon(TINY3D_TRIANGLE_STRIP);

  // Top-Left
  tiny3d_VertexPos(0, 0, 65535);
  tiny3d_VertexTexture(0.0f, 0.0f);

  // Top-Right
  tiny3d_VertexPos(screen_width, 0, 65535);
  tiny3d_VertexTexture(1.0f, 0.0f);

  // Bottom-Left
  tiny3d_VertexPos(0, screen_height, 65535);
  tiny3d_VertexTexture(0.0f, 1.0f);

  // Bottom-Right
  tiny3d_VertexPos(screen_width, screen_height, 65535);
  tiny3d_VertexTexture(1.0f, 1.0f);

  tiny3d_End();
}
