# PS3-Moonlight Architecture & Engineering Brief

## Project Overview
PlayStation 3 client for Moonlight Game Streaming protocol (NVIDIA Shield / Sunshine server compatible), ported and optimized for the Cell Broadband Engine (PPE/SPU) and RSX GPU via PSL1GHT v2.

## Key Subsystems
- **`src/main.c`**: Application lifecycle, thread initialization, network initialization.
- **`src/ui.c`**: Interface rendering using tiny3d, settings persistence (`config.ini`), OSK integration, telemetry HUD overlay.
- **`src/video.c`**: H.264 video decoding using hardware VDEC (AVC Level 4.1/4.2), frame buffer management, RSX texture mapping, upscaling and frame presentation.
- **`src/input.c`**: Controller, mouse, and keyboard input polling and mapping to Moonlight protocol.
- **`src/audio.c`**: Opus audio decoding and low-latency PS3 hardware audio output.
- **`src/shaders/`**: RSX shader sources (`cas_quad_vp.cg`, `cas_upscale_fp.cg`).

## Aspect Ratio (16:9 vs 4:3) & Dual-Directional Scaling Architecture
- **Problem Statement**: Standard Moonlight assumed a rigid 16:9 720p layout. Users connecting PS3 to CRT TVs / 4:3 monitors via analog (composite/SCART/component) or HDMI-to-VGA required native 4:3 resolutions (1024x768, 800x600 downscaled cleanly to 480p/576p). In addition, streaming 1080p to 720p displays created severe high-frequency aliasing and moire when downscaled using simple bilinear sampling.
- **Architectural Solution**:
  1. **Hardware Display-Bound Aspect Ratio Filtering**:
     - Screen aspect ratio is directly detected from PS3 hardware (`videoGetState` / `VIDEO_ASPECT_4_3` vs `VIDEO_ASPECT_16_9`).
     - Redundant manual aspect ratio menu toggles removed. Stream Resolution menu choices are strictly filtered to matching aspect ratio:
       - **16:9 Displays**: Only 16:9 options are exposed (`720p 1280x720`, `1080p 1920x1080`).
       - **4:3 Displays**: Only 4:3 options are exposed (`1024x768 XGA`, `800x600 SVGA`).
  2. **Two-Tier Scaling Pipeline (`SCALING_MODE_SHADER` vs `SCALING_MODE_LEGACY`)**:
     - **Discrete HD vs SD Shader Isolation**:
       - **HD Modes** (`screen_height >= 720`): 1080p ($1920 \times 1080$) and 720p ($1280 \times 720$). Employs dedicated HD shaders (`hd_cas_quad_vp`, `hd_cas_upscale_fp`, `hd_box_downscale_fp`).
       - **SD Modes** (`screen_height < 720`): 576p ($720 \times 576$) and 480p ($720 \times 480$) across all variations. Employs dedicated SD shaders (`sd_cas_quad_vp`, `sd_cas_upscale_fp`, `sd_box_downscale_fp`).
     - **Mode 0: Shader Pipeline (Adaptive CAS / Box / 1:1)**:
       - **Direct 1:1 Passthrough** (`input == output`): Direct hardware texture mapping via Tiny3D fixed-function pipeline (0 filter latency, 0 shader ALU overhead).
       - **Upscaling Engine (AMD CAS 5-Tap)** (`input < output`, e.g. 720p -> 1080p): Contrast-adaptive unsharp masking with dynamic texel dimensions passed via `TEXCOORD1`, eliminating blur while preventing edge halos and ringing.
       - **Downscaling Engine (4-Tap Bilinear Box)** (`input > output`, e.g. 1024x768/800x600 -> 480p/576p, or 1080p -> 720p/576p): 16-texel ($4 \times 4$) area-average downsampler in 17 fp16 instructions (`box_downscale_fp.cg`). Completely suppresses flickering, shimmering, and moire on high-contrast PC text and HUD elements.
     - **Mode 1: Legacy Pipeline (Tiny3D Bilinear / Direct)**:
       - Uses Tiny3D's fixed-function RSX texture mapping pipeline directly without custom fragment programs.
  3. **Zero In-Stream Gamepad Interference**:
     - In-stream `L3 + R3` upscaler toggle shortcut removed; L3 and R3 pass through cleanly to the host PC as standard controller thumbstick clicks.
  4. **Aspect Ratio Viewport Correction**:
     - Automatically computes destination rectangle (`dst_x, dst_y, dst_w, dst_h`) with hardware RSX viewport clipping. Prevents horizontal/vertical distortion when aspect ratios differ (automatic pillarboxing/letterboxing).
  5. **Dynamic Stream Negotiation & Mouse Mapping**:
     - `src/handshake.c`: `mode=%dx%dx%d` dynamically sent to Sunshine/GFE in HTTP launch parameters.
     - `src/main.c`: `STREAM_CONFIGURATION.width/height` set from active stream selection.
     - `src/input.c`: Absolute mouse position event dynamically scaled to active stream dimensions.
  6. **Settings & Persistence**:
     - Persisted in `config.ini` as `aspect_ratio=0/1`, `res_16_9=0/1`, `res_4_3=0/1`, `scaling_mode=0/1`.
     - Real-time telemetry HUD displays active stream input, display output, and active scaling engine status.
     - **Granular FPS Telemetry Pipeline**:
       - `Stream In`: Measures raw decode units entering from network (`ps3_video_submit_decode_unit`).
       - `Decoded FPS`: Measures real pictures retrieved from hardware `cellVdec` (`vdecGetPicture`).
       - `Rendered FPS`: Measures unique frames presented by RSX (`ps3video_draw`).
       - `Dropped FPS`: Tracks frames discarded due to VDEC hardware queue saturation (`vdec_au_pending >= VDEC_BUF_COUNT`) and display queue overflows (`ready_queue`).
       - `Display VSync`: Hardware presentation refresh rate (`tiny3d_Flip`, 60 Hz).

## Autonomous Multi-Core Profiling & Debugging Architecture (Package 3)
- **Zero-Touch Function Instrumentation (PPE + SPE)**:
  - Automatically instruments every function across all PPE translation units and SPU kernels using GCC `-finstrument-functions` without manual boilerplates.
  - PPE handled via `libps3perf.a` (`ps3perf_init()` in `src/main.c`).
  - SPU handled via `libps3perf_spu.a` in `src/spu/Makefile` (`ENABLE_PS3PERF=1`) and registered via `ps3perf_register_spu(g_thread_id, spu_opus_elf)` in `src/spu_opus_bridge.c`.
  - Kernel Isolation (`cellVdec`): The hardware H.264 video decoding pipeline runs inside the LV2 supervisor kernel context (`libvdec.sprx`), meaning it cannot be instrumented via user-space GCC `-finstrument-functions` or SPU decrementer hooks. SPU performance instrumentation is dedicated to `spu_opus`.
- **Hardware Time Base & Decrementer Telemetry**:
  - PPE: 64-bit hardware Time Base register (`mftb` / `__gettime()`) ticking at 79.8 MHz (~12.53 ns resolution) with 0 syscall overhead.
  - SPE: Hardware SPU Decrementer down-counter ticking synchronously at 79.8 MHz. Maintained entirely within ~1 KB Local Store memory table (`SpuPerfTable`), using 0 MFC DMA commands and causing zero tag collisions.
  - Transparent Bridge: PPE queries SPU Local Store via `sysSpuThreadReadLocalStorage` during `ps3perf_tick(5)`, multiplexing PPE and SPU metrics over UDP port 18195.
- **Two-Tier PKG Packaging Pipeline**:
  - `make all` concurrently generates two packages:
    1. Standard Release (`build/moonlight-ps3-$(TIMESTAMP).pkg`) with uninstrumented SPU Opus kernel.
    2. Autonomous Debug & Profiler (`build/moonlight-ps3-debug-$(TIMESTAMP).pkg`) linking `libps3perf.a` and `libps3perf_spu.a` with `sfo_debug.xml` (`Moonlight PS3 (Debug)`).
- **ps3perf CLI Telemetry Suite (`ps3perf.py` / `tools/ps3perf.py`)**:
  - `ps3perf record [-p 18195] [-o ps3perf.data]`: Captures live UDP telemetry from the console into an offline data file.
  - `ps3perf report -pkgdir=build/moonlight-ps3-debug.unstripped.elf --spu-elf=src/spu/spu_opus.elf [-i ps3perf.data]`: Resolves PPE `.opd` symbols and SPU Local Store addresses, outputting Linux `perf report`-style multi-core overhead tables (Overhead %, Total ms, Calls, Avg us, Symbol, File:Line).
  - `ps3perf live -pkgdir=build/moonlight-ps3-debug.unstripped.elf --spu-elf=src/spu/spu_opus.elf`: Real-time interactive streaming multi-core terminal dashboard.

## SPU Opus Audio Decoder Offloading Architecture (`spu_opus`)
- **Objective**: Offload compute-heavy Opus multistream audio decoding and 16-bit to 32-bit float PCM conversion from the host CPU (PPU0/PPU1) to a dedicated Synergistic Processing Unit (SPE / SPU) worker thread.
- **Architectural Naming Convention**:
  - The PlayStation 3 host CPU (PPE / PPU) is the default execution environment. Standard host subsystems remain named cleanly without redundant `_ppu` suffixes (e.g., `src/video.c` for video using hardware `cellVdec`, `src/audio.c` for audio).
  - Only offloaded SPE kernels and their bridges explicitly carry the `spu_` prefix (e.g., `src/spu/source/spu_opus.c`, `src/spu_opus_bridge.c`).
- **Root Cause & Performance Rationale**:
  - Host software Opus decoding previously introduced ~350 µs of CPU execution time per frame and caused severe `sysMutexLock` thread contention between the Moonlight depacketizer callback and the LV2 audio playback thread.
  - Offloading to SPU frees valuable host cycles for network packet processing and eliminates main CPU audio decode stalls.
- **SPU Kernel (`src/spu/source/spu_opus.c` & `src/spu/include/spu_opus.h`)**:
  - Standalone SPU ELF compiled via `spu-gcc -Os` and linked against a lightweight, fixed-point decode-only Opus static library (`libspu_opus.a`).
  - Stripped of all SILK/CELT encoder symbols to fit comfortably within the 256 KB Local Store limit (total ELF binary footprint: 258,520 bytes, with ~3.6 KB safety margin for stack).
  - Pre-allocated 22 KB `OpusMSDecoder` state buffer (`ls_decoder_state`) inside SPU BSS, avoiding dynamic heap allocation.
  - Converts decoded 16-bit signed PCM to 32-bit float PCM in-place using vector-friendly scaling (`scale = 1.0f / 32768.0f`), supporting up to 960 stereo sample pairs (20 ms at 48 kHz).
  - All communication between Main RAM and SPU Local Store uses 128-byte aligned MFC DMA transfers (`mfc_get` / `mfc_put`) triggered by SPU Signal Notification Register 1.
- **Host Bridge & In-File Fallback Mechanism (`src/spu_opus_bridge.c`, `src/spu_opus_bridge.h`, `src/audio.c`)**:
  - `spu_opus_init()` spawns SPU Thread Group and SPU Thread running the `spu_opus` kernel image embedded via `bin2s` (`spu_opus_elf`).
  - Seamless fallback: If SPU initialization fails, `src/audio.c` automatically falls back to host software Opus multistream decoding (`opus_multistream_decode_float`) without requiring separate files.
- **SPU Allocation Budget (All 6 GameOS User SPEs Utilized)**:
  - 5 SPEs dedicated to H.264 VDEC hardware video decoding (`src/video.c`).
  - 1 SPE dedicated to SPU Opus audio decoding (`src/spu_opus_bridge.c`).
  - SHA-256 handshake cryptography moved to PolarSSL on host (`src/handshake.c`), reclaiming the crypto SPU for audio.
  - Exactly 6 user SPEs active simultaneously with zero thread thrashing, no oversubscription, and optimal hardware utilization.

## LV2 Thread Priority Hierarchy & High-Throughput Network Drainer
- **LV2 Scheduling Model**:
  - Verified from PS3 SDK (`sys/thread.h`): Thread priority values range from 0 to 3071, where **smaller numbers represent higher priority** (0 is highest real-time priority; opposite of Linux `nice`).
- **Priority Layout**:
  - `VideoRecv` (UDP Network Ingestion Thread): **Priority 64** (Highest user real-time priority).
  - `Audio Thread` (Hardware Audio Playback Thread): **Priority 100**.
  - `Input Thread` (Controller/Mouse Polling): **Priority 200**.
  - `UI Thread` (Menu / Rendering Loop): **Priority 500**.
  - `VDEC Submit / Poll Thread`: **Priority 600**.
- **Burst Ingestion Buffer (`VideoStream.c`)**:
  - High-bitrate 1080p/720p IDR keyframes generate bursts of 180–210 UDP packets within milliseconds. Under LV2's 64 KB `SO_RCVBUF` kernel limit, reading in small 64-packet batches resulted in buffer overflow and "Unrecoverable frame" drops.
  - Increased `VIDEO_BATCH_SIZE` from 64 to 256 in `third_party/moonlight-common-c/src/VideoStream.c`. With Priority 64 elevation, `VideoRecv` instantly preempts all user threads upon UDP socket wakeups and drains entire IDR bursts in a single non-blocking loop.

## Target Streaming Bitrate Presets Architecture
- **Configured Presets**: Five discrete bitrate steps configured in `src/ui.c` via `ui_bitrate_options[]`:
  - `2500 Kbps` (2.5 Mbps) [Index 0]
  - `5000 Kbps` (5.0 Mbps) [Index 1]
  - `10000 Kbps` (10.0 Mbps) [Index 2, Canonical Default]
  - `12500 Kbps` (12.5 Mbps) [Index 3]
  - `15000 Kbps` (15.0 Mbps) [Index 4, Maximum Stable Ethernet Ceiling]
- **Default & Bounds Enforcement**:
  - `ui_bitrate_idx` initializes to index 2 (10 Mbps).
  - Maximum ceiling locked to 15 Mbps to ensure 100% stable, drop-free Ethernet streaming without exceeding LV2 UDP throughput limits.
- **UI & Telemetry HUD Formatting**:
  - Fractional bitrates (`% 1000 != 0`, e.g. 2.5 Mbps, 12.5 Mbps) dynamically format with `%.1f Mbps`. Integer values (5 Mbps, 10 Mbps, 15 Mbps) format with `%d Mbps`.

## H.264 VDEC Hardware SPE Allocation Architecture
- **SPE Allocation**: Hardware H.264 video decoding worker count set to 5 SPUs (`config.num_spus = 5` in `src/video.c`).
  - 1 SPU dedicated to CAVLC/CABAC entropy parsing and NAL unit slice header processing.
  - 4 SPUs dedicated to parallel macroblock reconstruction, inverse transform (IDCT), motion vector interpolation, and deblocking filtering.
- **Architectural Limit & Cell SPU Budget**:
  - 5 SPEs represents the maximum supported limit by Sony's AVC microcode in `libvdec` (`CELL_VDEC_AVC_MAX_SPUS = 5`).
  - Combined with 1 dedicated SPU for `spu_audio` (Opus decoding), exactly 6 user SPUs (out of 6 GameOS accessible SPEs) are utilized with zero thread thrashing, no preemption penalties, and maximum throughput.
- **Resilient Fallback Cascade**: `vdecOpen` iterates through `{5, 4, 2}` SPUs to ensure graceful degradation if running under restricted hypervisor environments.

## Clock Drift, Latency Stabilization & PPE Float Optimization Architecture
- **Clock Drift Analysis (60.000 Hz Host vs 59.940 Hz PS3 NTSC)**:
  - Standard PC gaming hosts stream at $60.000\text{ FPS}$, while PS3 displays operate at exact NTSC $59.940060\text{ Hz}$ ($60000 / 1001\text{ Hz}$).
  - The deterministic delta is $\Delta = 0.05994\text{ FPS}$ (accumulating exactly 1 extra frame every $16.68\text{ seconds}$, or $3.60\text{ frames/minute}$).
  - Unmanaged, this drift accumulated $36.0\text{ frames}$ ($599.4\text{ ms}$ input lag) over 10 minutes, eventually overflowing the depacketizer queue and triggering periodic multi-second freezes.
- **Three-Tier Synchronization & Latency Stabilization Solution**:
  1. **Protocol Tier (`clientRefreshRateX100`)**:
     - Configured dynamically in `src/main.c`: detects display mode via `videoGetResolution` and sets `streamConfig.clientRefreshRateX100 = 5994` (NTSC HD) or `5000` (PAL SD).
     - Modern Sunshine and GFE hosts read this property and lock their capture/encoding timers directly to $59.940\text{ Hz}$, eliminating the gross drift at the source.
  2. **Presentation Tier (Zero-Latency Drain in `ps3video_draw`)**:
     - Compensates for crystal oscillator frequency drift and transient network jitter in `src/video.c`.
     - VDEC hardware decodes every frame into its DPB, preserving all H.264 intra/inter temporal references.
     - When `ready_count > 1` on RSX VBlank, `ps3video_draw()` executes `while (ready_count > 1) { ready_tail = (ready_tail + 1) % VDEC_FRAME_COUNT; ready_count--; }`, skipping older presentation bitmaps directly to the newest ready frame.
     - Absorbs the 16.68s drift within a single VBlank with zero artifacts, permanently locking presentation input latency to 0–16 ms.
  3. **Depacketizer Tier (`decodeUnitQueue = 4`)**:
     - Lowered queue bound in `third_party/moonlight-common-c/src/VideoDepacketizer.c` from 120 frames ($2000\text{ ms}$) to 4 frames ($66\text{ ms}$).
     - Eliminates multi-second backlog accumulation and reclaims ~4.8 MB of heap RAM.
- **Cell PPE Hardware Flush-to-Zero (`FPSCR[NI]`) Optimization**:
  - Cell PPE (PowerPC 970 derivative) in standard IEEE-754 mode incurs expensive microcode assist interrupt traps (hundreds of CPU cycles) whenever floating-point operands or results enter the subnormal range ($0 < |x| < 1.17 \times 10^{-38}$).
  - Decaying audio waveforms, fading reverbs, and trailing silence in `audio_ring_buffer`, alongside floating-point latency moving averages, periodically triggered these assist traps, degrading stream smoothness over time.
  - Activated hardware Non-IEEE Flush-to-Zero (FTZ) mode by setting bit 29 of FPSCR (`asm volatile("mtfsb1 29" ::: "memory");`) in `src/main.c` (`main()`) and `src/audio.c` (`audio_loop()`).
- **Elastic 6-Buffer RSX Pool & Clean Recovery**:
  - `VDEC_FRAME_COUNT` increased from 4 to 6 in `src/video.c`, expanding `ready_queue` capacity from 1 frame to 3 frames to absorb transient RSX VSync phase shifts without frame drops.
  - On VDEC hardware queue saturation (`vdec_au_pending >= VDEC_BUF_COUNT`) or AU submission failure, `ps3_video_submit_decode_unit` returns `DR_NEED_IDR` instead of `DR_OK`, ensuring instant and clean host keyframe recovery without GOP corruption.
  - **Memory Budget**: Static systems consume $74.5\text{ MB}$; 6 buffers at 720p consume $24.0\text{ MB}$ (total $98.5\text{ MB}$), maintaining a comfortable **$114.5\text{ MB}$ free headroom** in the $213\text{ MB}$ user XDR RAM pool.
