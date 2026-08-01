# Changelog

All notable changes to the Moonlight PS3 project will be documented in this file.

## Unreleased

### Security and reliability

- Use the PS3 system random-number service for credentials, UUIDs, pairing data, and stream input keys.
- Generate a fresh pairing PIN and pin each paired host's TLS certificate fingerprint.
- Validate HTTP status, response sizes, allocations, and pairing hex payloads before processing them.
- Make controller input startup idempotent and keep the UI controller service alive across reconnects.
- Reset VDEC queues and release RSX mappings on teardown and partial initialization failures.
- Disable UDP broadcast logging by default and centralize logger ownership and shutdown.

### Build

- Select the official ps3dev SDK archive for macOS ARM64, macOS x64, or Linux x64 automatically.
- Use absolute compiler paths and generate header dependency files for incremental builds.

## [v1.0.0] - 2026-08-01 - Initial Release

### Features & Highlights

- **Hardware Accelerated Video Decoding**: High-performance 720p60 H.264 video decoding utilizing the PlayStation 3 Cell Broadband Engine `cellVdec` hardware decoder interface, mapped directly to RSX graphics memory via Tiny3D.
- **Low-Latency Opus Audio Engine**: Custom PS3 audio backend (`src/audio/ps3.c`) featuring thread-safe ring buffering, low-latency PCM playback via `sysAudio`, and automatic buffer underflow/overflow recovery.
- **GameStream & Sunshine Protocol Compatibility**: Native HTTP/HTTPS pairing pipeline with client certificate generation, dynamic 4-digit random PIN challenge handshake, RTSP stream setup, and session control.
- **DualShock 3 Input Engine**: Low-latency gamepad input processing via `sysUtil` with analog stick deadzone filtering, button mapping, and stream abort hotkey (`Select + Start + L3 + R3`).
- **Real-Time Performance HUD**: On-screen overlay displaying metrics for FPS, network RTT, video decode latency, render overhead, and packet loss statistics.
- **OpenSSL Compatibility Layer**: Custom OpenSSL abstraction layer translating crypto and TLS operations to PolarSSL/mbedTLS to avoid symbol collisions on PSL1GHT.
- **Dual Compatibility PKG Installer**: Signed retail NPDRM PKG packages compatible with both physical PS3 consoles (CFW / HEN) and the RPCS3 emulator.

### ⚠️ Known Issues & Early Access Notes

- **Audio Subsystem Status**: The Opus audio decoder and `sysAudio` PCM output backend are functional, but audio stream stability has not been extensively stress-tested across all PS3 hardware revisions and audio configurations yet. Minor buffer stutters or sample rate synchronization quirks may occur.
- **Feedback & Bug Reports Welcomed**: Since this is the initial public release, user feedback, audio/video sync reports, and community contributions via Pull Requests are warmly welcomed to help polish future updates!

### Acknowledgments & Special Thanks

- **[PS3DEV / PSL1GHT SDK Developers](https://github.com/ps3dev/ps3dev)**: Enormous gratitude to the open-source PS3 toolchain and PSL1GHT SDK maintainers and developers. Their years of effort building cross-compilers, toolchains, and hardware libraries made this port possible.
- **Mohammed Asif (mohasi)**: Technical guidance, problem-solving support, and project inspiration.
- **AcidNT3.1**, **SyrianClippy**, **Okeanos**: Testing, hardware validation, and feedback during development.
