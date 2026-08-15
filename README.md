# Moonlight PS3

Moonlight PS3 is an open-source PlayStation 3 homebrew client for NVIDIA GameStream and Sunshine servers, built using the PSL1GHT SDK, Tiny3D graphics engine, FreeType 2 vector font renderer, `cellVdec` hardware decoder interface, and Opus audio decoding pipeline.

---

## Features

- **Hardware-Accelerated H.264 Video Decoding**: High-performance 720p60 H.264 video decoding utilizing the PS3 Cell Broadband Engine `cellVdec` hardware decoder, mapped directly to RSX graphics memory via Tiny3D. Features 4-slice frame negotiation (`CAPABILITY_SLICES_PER_FRAME`) with Sunshine/GameStream to eliminate UDP packet bursts and socket buffer overflow.
- **Full USB & Bluetooth Keyboard and Mouse Support**:
  - **Dual Mouse Modes**: Supports both **Game Mode** (unbounded relative delta motion for 3D camera control) and **Desktop Mode** (high-precision 1:1 absolute coordinate motion for desktop navigation), configurable in the Stream Settings menu.
  - **Complete Mouse Input**: Full support for Left, Right, Middle, Side X1, and Side X2 buttons, alongside high-precision vertical scroll wheel input.
  - **Universal Keyboard Translation**: Accurate Win32 Virtual Key (VK) mapping for standard US layout keyboards (A-Z, 0-9, F1-F12, navigation cluster, arrows, numpad, and punctuation) with discrete modifier tracking (Ctrl, Shift, Alt, Meta/Win).
- **Interactive Host Game & Application Selection Menu**: Automatically queries and parses `/applist` XML from your Sunshine/GameStream server, displaying all available host games and applications in a scrollable Material UI list with active selection highlight, index counter `[ X / Y ]`, and one-click launching.
- **Native PS3 GameOS System Dialogs**: Official `<sysutil/msg.h>` pop-up confirmation modals ("Do you want to quit Moonlight and return to the PS3 XMB?") preventing accidental exits.
- **Persistent Configuration Engine**: Automatically saves and restores Sunshine Host IP, target FPS (30/60), bitrate (2.5, 5.0, 10.0 Mbps), mouse mode (Game/Desktop), RSX VSync mode, HUD stats visibility, and verbose logging across console reboots in `/dev_hdd0/game/MNLT00001/USRDIR/config.ini`.
- **High-Fidelity FreeType 2 Font Rendering**: Dynamically loads official PlayStation 3 console vector fonts (`SCE-PS3-RD-R-LATIN.TTF`, `SCE-PS3-SR-R-LATIN.TTF`) from `/dev_flash/data/font/` with subpixel anti-aliasing, accompanied by custom procedural vector PlayStation button glyphs (Cross ✕, Circle ◯, Triangle △, Square ◻, D-Pad ↕).
- **Zero-Overhead RSX Hardware VSync & Telemetry HUD**: Flexible flip mode configuration (`GCM_FLIP_VSYNC` for tear-free 60Hz or `GCM_FLIP_HSYNC` for ultra-low latency) paired with an on-screen Performance Stats HUD that introduces zero CPU/GPU overhead when disabled.
- **Low-Latency Opus Multistream Audio Backend**: Custom PS3 audio backend featuring thread-safe ring buffering, low-latency 48kHz PCM playback via `sysAudio`, and automatic buffer recovery.
- **On-Screen Pairing PIN Modal**: Clean, dedicated visual badge presenting the dynamically generated 4-digit PIN for instant authorization in the Sunshine Web UI.
- **Universal NPDRM Package Build Pipeline**: Native `ppu-strip`, `fself`, and `make_self_npdrm` integration generating retail-signed PKG files compatible with both RPCS3 emulator and physical PS3 consoles (CFW / HEN).

---

## Controls & Key Bindings

| Action | Controller | Keyboard / Mouse |
| :--- | :--- | :--- |
| **Navigate Menus** | D-Pad Up / Down / Left / Right | Arrow Keys |
| **Select / Confirm / Launch** | Cross (✕) | Enter / Left Click |
| **Back / Exit to XMB (with Confirmation)** | Circle (◯) | Escape / Right Click |
| **Open On-Screen Keyboard (OSK)** | Cross (✕) on Host IP row | Physical Keyboard Direct Entry |
| **Toggle Settings Values** | Left / Right / Cross | Left / Right / Enter |
| **Emergency Stream Abort** | `Select + Start + L3 + R3` | `Ctrl + Shift + Alt + Q` |

---

## Quick Start & Building from Source

### 1. Clone the Repository
```bash
git clone https://github.com/Cruslan/PS3-Moonlight
cd PS3-Moonlight
```

### 2. Prepare the PS3 SDK Environment
Automatically downloads and extracts the pre-compiled `ps3dev` SDK and PSL1GHT toolchain for your host OS (macOS ARM64/x64 or Linux x64) into `./ps3dev`:
```bash
make prepare
```

### 3. Build the Packages
Compile the client binary and generate the signed installation packages:
```bash
make
```

### 4. Output Packages
Upon completion, the build outputs the following installable files in `build/`:
- `moonlight-ps3.pkg`: Standard package installer for physical PS3 consoles (CFW / HEN) and RPCS3 emulator.
- `moonlight-ps3.gnpdrm.pkg`: Finalized Retail NPDRM signed package.

---

## Installation & Pairing

1. Copy `moonlight-ps3.pkg` (or `moonlight-ps3.gnpdrm.pkg`) to the root of a FAT32 formatted USB drive.
2. Insert the USB drive into your jailbroken PS3 (CFW or PS3HEN).
3. On the PS3 XMB, navigate to **Game > Package Manager > Install Package Files > Standard** and install the PKG.
4. Launch **Moonlight PS3** from your XMB Game column.
5. Select the Sunshine Host IP row to enter your PC's IP address using the native PS3 On-Screen Keyboard.
6. Select **Connect / Pair to Host**. Enter the 4-digit PIN shown on screen into the Sunshine Web UI under **PIN**.
7. Once paired, select your desired game or desktop application from the interactive App List menu to begin streaming!

---

## Credits

- **[Moonlight-QT](https://github.com/moonlight-stream/moonlight-qt)**: Core client logic, GameStream/Sunshine protocol handling, icon/graphics assets, and streaming implementation are directly adapted from Moonlight-QT.
- **[Opus Interactive Audio Codec](https://opus-codec.org/)**: Audio decoding functionality is powered by the Opus codec library.
- **[Moonlight Common C](https://github.com/moonlight-stream/moonlight-common-c)**: Common GameStream client library.

## Acknowledgments & Special Thanks

- **[PS3DEV / PSL1GHT SDK](https://github.com/ps3dev/ps3dev)**: Enormous thanks to the open-source PS3 toolchain and PSL1GHT SDK developers whose foundational work, cross-compilers, hardware header definitions, and libraries made this PlayStation 3 port possible.
- **[Mohammed Asif (mohasi)](https://codeberg.org/mohasi)**: Special thanks for technical guidance, problem-solving support, and project inspiration.
- **AcidNT3.1**: Special thanks for testing and feedback during development.
- **SyrianClippy**: Special thanks for testing and feedback during development.
- **Okeanos**: Special thanks for testing and feedback during development.

---

## License

This project is released under the GNU General Public License v3.0 (GPLv3).
