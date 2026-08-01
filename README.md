# Moonlight PS3

Moonlight PS3 is an open-source PlayStation 3 homebrew client for NVIDIA GameStream and Sunshine servers, built using the PSL1GHT SDK, Tiny3D graphics library, `cellVdec` hardware decoder, and Opus audio decoding.

## Features

- **Hardware Accelerated H.264 Video Decoding**: High-performance 720p60 H.264 video decoding using the PS3 Cell Broadband Engine `cellVdec` hardware decoder interface, mapped directly to RSX graphics memory via Tiny3D.
- **Bundled Third-Party Architecture**: Third-party libraries (`moonlight-common-c` and `opus`) are integrated and modified directly in-tree inside `third_party/`, removing external Git submodule dependencies for zero-friction compilation.
- **OpenSSL Compatibility & Crypto Emulation Layer**: Custom OpenSSL abstraction layer (`src/openssl_compat.c`) translating crypto and TLS operations to PolarSSL/mbedTLS to prevent library symbol collisions on PSL1GHT.
- **Low-Latency Opus Multistream Audio**: Custom PS3 audio backend (`src/audio/ps3.c`) featuring thread-safe ring buffering, low-latency PCM playback via `sysAudio`, and automatic buffer underflow/overflow recovery.
- **Full GameStream / Sunshine Protocol Support**: Built-in HTTP/HTTPS pairing pipeline (`src/handshake.c`) supporting client certificate generation, PIN challenge handshake, RTSP stream setup, and session control.
- **DualShock 3 Input Engine**: Low-latency gamepad input processing via `sysUtil` with analog stick deadzone filtering, mapped buttons, and emergency stream abort hotkeys (`Select + Start + L3 + R3`).
- **Real-Time Instrumentation & Performance HUD**: On-screen metrics display monitoring FPS, network RTT, video decode latency, render overhead, and packet loss statistics.
- **Universal NPDRM Package Build Pipeline**: Native `ppu-strip`, `fself`, and `make_self_npdrm` integration generating retail-signed PKG files compatible with both RPCS3 emulator and physical PS3 consoles (CFW / HEN).

## Prerequisites

- **PS3 Toolchain**: `ps3dev` / PSL1GHT PPU toolchain (`ppu-gcc`, `make_self_npdrm`, `sprxlinker`, `pkg`). Automatically installed via `make prepare` on macOS ARM64, macOS x64, and Linux x64 hosts.

## Quick Start & Building from Source

1. **Clone the Repository**:
   ```bash
   git clone https://github.com/Cruslan/PS3-Moonlight
   cd moonlight-ps3
   ```

2. **Prepare the PS3 SDK Environment**:
   Automatically downloads and extracts the pre-compiled `ps3dev` SDK and PSL1GHT toolchain for your host OS (macOS ARM64/x64 or Linux x64) into `./ps3dev` for isolated, zero-friction builds:
   ```bash
   make prepare
   ```

3. **Build the Package**:
   Compile the client binary and generate the signed installation packages:
   ```bash
   make
   ```

4. **Output Packages**:
   Upon completion, the build outputs the following installable files:
   - `moonlight-ps3.pkg`: Dual-compatible PKG installer for RPCS3 emulator and physical PS3 consoles (CFW / HEN).
   - `moonlight-ps3.gnpdrm.pkg`: Finalized NPDRM PKG package.

## Usage & Pairing

1. Install `moonlight-ps3.pkg` on your PlayStation 3 console (CFW/HEN) or RPCS3 emulator.
2. Ensure your host PC running Sunshine or NVIDIA GameStream is connected to the same local network.
3. Launch Moonlight PS3.
4. Set your host PC IP address.
5. Enter the dynamically generated random four-digit PIN displayed on the Moonlight PS3 screen (and log output) into the Sunshine / GameStream Web UI on your PC.

The paired server certificate is pinned per host upon successful pairing. Existing installations upgrading from an older build must pair once more to create the host certificate fingerprint.

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

## License

This project is released under the GNU General Public License v3.0 (GPLv3).
