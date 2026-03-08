<p align="center">
  <h1 align="center">Couch Conduit</h1>
  <p align="center">
    <strong>Ultra-low-latency remote play for couch co-op over the internet.</strong>
  </p>
  <p align="center">
    <a href="#quick-start">Quick Start</a> •
    <a href="#building-from-source">Build</a> •
    <a href="#architecture">Architecture</a> •
    <a href="TESTING.md">Testing Guide</a> •
    <a href="#roadmap">Roadmap</a>
  </p>
</p>

---

Couch Conduit lets your friends connect with their controllers and play as Player 2 (3, 4…) on your PC — as if they were sitting next to you.

**Target latency:** < 10 ms input-to-photon on LAN, < 30 ms over decent internet.  
**Measured:** 0.43 ms average recv-to-present on localhost (decode 0.24 ms + render 0.07 ms).

> **Status:** Working end-to-end. Video streaming, input forwarding, and hardware encode/decode are fully functional. Audio streaming, encryption on the wire, and adaptive bitrate are on the roadmap.

---

## Features

- **DXGI Desktop Duplication** — zero-copy GPU capture, input-triggered for minimum latency
- **NVENC hardware encode** — H.264 / HEVC / AV1, P1 ultra-low-latency preset, CBR
- **D3D11VA hardware decode** — zero-copy decode via FFmpeg, event-signaled (no polling)
- **D3D11 Video Processor** — GPU-accelerated downscale (e.g. 5120×1440 ultrawide → 1920×1080)
- **Custom UDP/RTP transport** — RTP-like packetization with XOR forward error correction
- **Full input chain** — XInput controllers at 1000 Hz, Raw Input mouse, keyboard → UDP → host `SendInput()`
- **IDR feedback** — client detects corrupt/missing keyframes and requests instant IDR recovery
- **Sub-millisecond system tuning** — 0.5 ms timer resolution, MMCSS Pro Audio, GPU REALTIME priority
- **FLIP_DISCARD + ALLOW_TEARING** — tear-free or tear-allowed present depending on V-Sync setting
- **AES-128-GCM encryption** — implemented via Windows CNG (not yet wired to the transport layer)
- **Pipeline stats** — 5-second interval logging of FPS, decode time, render time, recv-to-present latency

## How It Works

```
HOST                                          CLIENT
┌─────────────┐                              ┌─────────────┐
│ DXGI Capture│─→ D3D11 Video Processor ─→   │             │
│ (any res)   │   (GPU downscale)            │             │
└──────┬──────┘                              │             │
       ▼                                      │             │
┌─────────────┐    UDP :47101 (video)        ┌┴────────────┐
│ NVENC Encode│ ════════════════════════════► │ D3D11VA     │
│ (HEVC P1)   │                              │ Decode      │
└─────────────┘                              └──────┬──────┘
                                                    ▼
┌─────────────┐    UDP :47103 (input)        ┌─────────────┐
│ SendInput() │ ◄════════════════════════════ │ XInput +    │
│ + ViGEmBus  │                              │ Raw Input   │
└─────────────┘                              └─────────────┘
                                              │  NV12→RGBA  │
                                              │  Renderer   │
                                              └─────────────┘
```

**Key innovation:** When input arrives from the client, the host captures a frame *immediately* instead of waiting for the next VBlank timer — cutting up to 16.7 ms of latency.

## Quick Start

### Pre-built Binaries

Download the latest zips from [Releases](https://github.com/acLebert/Couch_Conduit/releases) or build from source (see below). The host zip is ~0.3 MB, client zip is ~35 MB (includes FFmpeg DLLs).

**Host (the PC running the game):**
```powershell
.\cc_host.exe --client <CLIENT_IP> --encode-resolution 1920x1080
```

**Client (your friend's PC):**
```powershell
.\cc_client.exe <HOST_IP> --resolution 1920x1080
```

Press **ESC** on the client to quit. See [TESTING.md](TESTING.md) for the full step-by-step guide.

### Network Requirements

| Port | Protocol | Direction | Purpose |
|------|----------|-----------|---------|
| 47101 | UDP | Host → Client | Video stream |
| 47103 | UDP | Client → Host | Input + IDR requests |

Both machines must be on the same LAN, or these ports must be forwarded on the host's router for internet play. Wired ethernet is strongly recommended.

---

## Building from Source

### Prerequisites

| Requirement | Notes |
|-------------|-------|
| **Windows 10/11** | 64-bit only |
| **Visual Studio 2022** | C++ Desktop workload (or Build Tools) |
| **CMake 3.24+** | |
| **NVIDIA GPU** | Turing or newer recommended (GTX 1650+, RTX series) |
| **NVIDIA Drivers** | Up to date — provides `nvEncodeAPI64.dll` at runtime |
| **FFmpeg shared libs** | `avcodec-62`, `avutil-60`, `swresample-6` — place in `third_party/ffmpeg/` |

Optional:
- [ViGEmBus Driver](https://github.com/nefarius/ViGEmBus/releases) — virtual gamepad injection on host
- [NVIDIA Video Codec SDK](https://developer.nvidia.com/nvidia-video-codec-sdk) — only needed if modifying NVENC headers

### FFmpeg Setup

Download the BtbN shared GPL build and extract to `third_party/ffmpeg/`:

```powershell
# From project root
Invoke-WebRequest -Uri "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl-shared.zip" -OutFile ffmpeg.zip
Expand-Archive ffmpeg.zip -DestinationPath temp_ff
Move-Item temp_ff/ffmpeg-master-latest-win64-gpl-shared/* third_party/ffmpeg/
Remove-Item ffmpeg.zip, temp_ff -Recurse
```

### Build

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Binaries are produced at `build/src/host/Release/cc_host.exe` and `build/src/client/Release/cc_client.exe`.

### Package for Distribution

```powershell
powershell -ExecutionPolicy Bypass -File scripts/package.ps1
```

Creates `dist/CouchConduit-Host.zip` and `dist/CouchConduit-Client.zip` with all runtime dependencies bundled.

---

## CLI Reference

### Host (`cc_host.exe`)

| Flag | Default | Description |
|------|---------|-------------|
| `--client <IP>` | `127.0.0.1` | Client's IP address |
| `--encode-resolution <WxH>` | *(capture res)* | Encode resolution (GPU downscale if different from desktop) |
| `--bitrate <kbps>` | `20000` | Video bitrate |
| `--fps <N>` | `60` | Target framerate |
| `--codec <h264\|hevc\|av1>` | `hevc` | Video codec |

### Client (`cc_client.exe`)

| Flag | Default | Description |
|------|---------|-------------|
| `<HOST_IP>` (positional) | `127.0.0.1` | Host's IP address |
| `--port <N>` | `47101` | Video port |
| `--resolution <WxH>` | `1920x1080` | Window resolution |
| `--fullscreen` | off | Start in fullscreen |
| `--vsync` | off | Enable V-Sync (adds latency) |

### Bitrate Guide

| Network | Recommended |
|---------|-------------|
| Localhost | 50,000 kbps |
| LAN (Gigabit) | 30,000–50,000 |
| LAN (100 Mbps) | 15,000–20,000 |
| Internet (good) | 10,000–15,000 |
| Internet (okay) | 5,000–8,000 |

---

## Architecture

### Data Flow

1. **Capture** — DXGI Desktop Duplication acquires the desktop as a GPU texture (zero-copy)
2. **Downscale** — D3D11 Video Processor hardware-scales to encode resolution if needed
3. **Encode** — NVENC encodes with P1 ultra-low-latency preset, async event-based output
4. **Packetize** — RTP-style packetization with sequence numbers + XOR FEC parity packets
5. **Transport** — Raw UDP on port 47101 (video) and 47103 (input)
6. **Reassemble** — Client reassembles frame from packets, recovers single losses via XOR FEC
7. **Decode** — D3D11VA hardware decode via FFmpeg, event-signaled (no polling)
8. **Render** — NV12 → RGBA via HLSL pixel shader, presented with FLIP_DISCARD swap chain

### Threading Model

| Thread | Priority | Purpose |
|--------|----------|---------|
| Input Receiver | `TIME_CRITICAL` | Lowest-latency input processing |
| Capture | `HIGHEST` | Frame acquisition |
| Encode | `HIGHEST` | GPU encode submission |
| Decode | `HIGHEST` + MMCSS | Hardware decode |
| Render | `TIME_CRITICAL` + MMCSS Pro Audio | Present to display |
| XInput Poller | `HIGHEST` | 1000 Hz controller polling |

### Key Design Decisions

| Decision | Rationale |
|----------|-----------|
| **Input-triggered capture** | Saves up to 16.7 ms vs. VBlank-only capture |
| **Event-signaled decode** | Zero-wait vs. SDL's 2 ms polling sleep |
| **FLIP_DISCARD + ALLOW_TEARING** | Eliminates compositor latency |
| **XOR FEC** | Single-packet recovery with minimal overhead |
| **IDR feedback channel** | Client requests keyframe on corruption instead of waiting |
| **0.5 ms timer resolution** | `NtSetTimerResolution` for precise scheduling |
| **Dynamic NVENC loading** | Graceful fallback if GPU doesn't support encode |

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the full deep-dive including wire protocol format, latency budgets, and congestion control design.

---

## Project Structure

```
Couch_Conduit/
├── CMakeLists.txt                    # Root build — C++20, MSVC, project options
├── README.md                         # This file
├── TESTING.md                        # Step-by-step guide for testers
├── docs/
│   └── ARCHITECTURE.md               # Deep-dive: wire protocol, threading, latency budget
├── scripts/
│   └── package.ps1                   # Build + package distributable zips
│
├── include/couch_conduit/
│   ├── common/
│   │   ├── types.h                   # VideoConfig, FrameMetadata, GamepadState, ports
│   │   ├── transport.h               # UdpSocket, Video/Input Sender/Receiver, FEC
│   │   ├── log.h                     # CC_INFO / CC_ERROR / CC_TRACE macros
│   │   └── system_tuning.h           # Timer resolution, MMCSS, GPU priority
│   ├── host/
│   │   ├── host_session.h            # Orchestrator: capture → encode → send
│   │   ├── capture.h                 # DxgiCapture — Desktop Duplication
│   │   ├── encoder.h                 # NvencEncoder — NVENC hardware encode
│   │   └── input_injector.h          # ViGEmBus + SendInput()
│   └── client/
│       ├── client_session.h          # Orchestrator: recv → decode → render
│       ├── decoder.h                 # D3D11Decoder — FFmpeg D3D11VA
│       ├── renderer.h               # D3D11 swap chain + NV12→RGBA shader
│       └── input_capture.h          # XInput + Raw Input capture
│
├── src/
│   ├── common/                       # cc_common static library
│   │   ├── transport/                # UDP socket, video/input send/recv, FEC
│   │   ├── protocol/                 # Session negotiation (stub)
│   │   ├── crypto/                   # AES-128-GCM via Windows CNG
│   │   └── util/                     # System tuning, timer resolution
│   ├── host/                         # cc_host executable
│   │   ├── main.cpp                  # CLI parsing, signal handling
│   │   ├── host_session.cpp          # Session orchestrator
│   │   ├── capture/                  # DXGI Desktop Duplication
│   │   ├── encode/                   # NVENC encoder
│   │   ├── input/                    # Input injection (SendInput + ViGEmBus)
│   │   └── audio/                    # WASAPI loopback capture
│   └── client/                       # cc_client executable
│       ├── main.cpp                  # Window creation, WndProc, message loop
│       ├── client_session.cpp        # Session orchestrator
│       ├── decode/                   # D3D11VA decoder
│       ├── render/                   # D3D11 renderer + HLSL shaders
│       ├── input/                    # XInput + Raw Input
│       └── audio/                    # Audio player (stub)
│
└── third_party/
    ├── ffmpeg/                       # FFmpeg shared libs (not committed)
    └── nv-codec-headers/             # NVENC API headers (not committed)
```

---

## Technology Stack

| Layer | Technology |
|-------|-----------|
| Language | C++20 (MSVC, `/permissive-`) |
| Build | CMake 3.24+, Visual Studio 2022 |
| Capture | DXGI Desktop Duplication (`IDXGIOutput5::DuplicateOutput1`) |
| Encode | NVIDIA NVENC (dynamically loaded `nvEncodeAPI64.dll`) |
| Decode | FFmpeg D3D11VA (`avcodec-62`, `avutil-60`) |
| Render | Direct3D 11 — HLSL SM 5.0, FLIP_DISCARD, ALLOW_TEARING |
| Transport | Raw UDP with RTP-style packetization, XOR FEC |
| Input | XInput (controllers), Raw Input (mouse/keyboard), `SendInput()` |
| Gamepad Injection | ViGEmBus (virtual Xbox 360 controller) |
| Audio Capture | WASAPI loopback mode |
| Encryption | AES-128-GCM via Windows CNG (`bcrypt.h`) |
| Scheduling | MMCSS Pro Audio, `NtSetTimerResolution(0.5ms)`, D3DKMT GPU REALTIME |
| COM | WRL `ComPtr` smart pointers |

---

## Roadmap

- [x] DXGI Desktop Duplication capture
- [x] NVENC hardware encode (H.264 / HEVC / AV1)
- [x] Custom UDP/RTP transport with XOR FEC
- [x] D3D11VA hardware decode via FFmpeg
- [x] D3D11 NV12→RGBA renderer with tearing support
- [x] Full input pipeline (XInput + Raw Input → UDP → SendInput)
- [x] IDR feedback and recovery
- [x] D3D11 Video Processor GPU downscale
- [x] Sub-millisecond system tuning
- [x] AES-128-GCM implementation
- [x] Packaging + distribution scripts
- [ ] Wire-level encryption (AES-GCM on transport)
- [ ] Audio streaming (WASAPI → Opus → UDP → client playback)
- [ ] Adaptive bitrate (TWCC-style congestion control)
- [ ] Adaptive FEC (loss-driven parity ratio)
- [ ] TCP session negotiation + key exchange
- [ ] Reed-Solomon FEC (multi-packet recovery)
- [ ] ViGEmBus full gamepad injection
- [ ] NAT traversal / STUN / TURN
- [ ] Multi-client support (3-4 player couch co-op)
- [ ] Haptic / rumble feedback
- [ ] QoS / DSCP packet tagging
- [ ] USO batched UDP sends
- [ ] Installer / MSI packaging

---

## Key Differences from Sunshine/Moonlight

| Aspect | Sunshine / Moonlight | Couch Conduit |
|--------|---------------------|---------------|
| Capture trigger | Timer-based, async from input | **Input-triggered** — frame captured on input arrival |
| Decode wakeup | `SDL_Delay(2)` polling loop | **Event-signaled** — zero-wait decode |
| Frame scheduling | Best-effort delivery | **Deadline-aware** — skip encode if frame will be late |
| FEC | Static percentage overhead | **XOR FEC** now, adaptive ratio planned |
| Bitrate | Fixed or manually capped | Dynamic `SetBitrate()`, TWCC planned |
| Input thread | Shared with main/task pool | **Dedicated TIME_CRITICAL thread** |
| Render present | Standard swap chain | **FLIP_DISCARD + ALLOW_TEARING** |
| Timer resolution | System default (15.6 ms) | **0.5 ms** via `NtSetTimerResolution` |

---

## Contributing

1. Fork the repo
2. Create a feature branch (`git checkout -b feat/my-feature`)
3. Commit with descriptive messages
4. Open a Pull Request

Please follow existing code style — C++20, `PascalCase` for types, `camelCase` for locals, `m_` prefix for members.

---

## License

MIT — see [LICENSE](LICENSE) for details.
