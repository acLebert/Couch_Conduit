<p align="center">
  <h1 align="center">Couch Conduit</h1>
  <p align="center">
    <strong>Ultra-low-latency remote play for couch co-op over the internet.</strong>
  </p>
  <p align="center">
    <a href="#quick-start">Quick Start</a> •
    <a href="#download">Download</a> •
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

> **Status:** Fully featured. Video, audio, input, encryption, adaptive bitrate, Reed-Solomon FEC, multi-client, gamepad/haptics, NAT traversal, room-code signaling, and a Dear ImGui connection/overlay UI are all implemented and build-verified.

---

## Features

- **DXGI Desktop Duplication** — zero-copy GPU capture, input-triggered for minimum latency
- **NVENC hardware encode** — H.264 / HEVC / AV1, P1 ultra-low-latency preset, CBR
- **D3D11VA hardware decode** — zero-copy decode via FFmpeg, event-signaled (no polling)
- **D3D11 Video Processor** — GPU-accelerated downscale (e.g. 5120×1440 ultrawide → 1920×1080)
- **Custom UDP/RTP transport** — RTP-like packetization with Reed-Solomon FEC (multi-packet recovery)
- **Full input chain** — XInput controllers at 1000 Hz, Raw Input mouse, keyboard → UDP → host `SendInput()`
- **ViGEmBus virtual gamepad** — Full Xbox 360 controller injection with haptic/rumble feedback
- **IDR feedback** — client detects corrupt/missing keyframes and requests instant IDR recovery
- **TCP session + ECDH key exchange** — P-256 key agreement, session negotiation
- **AES-128-GCM wire encryption** — All transport encrypted via Windows CNG BCrypt
- **Audio streaming** — WASAPI loopback capture → UDP → shared-mode playback on client
- **Adaptive bitrate** — GCC-like TWCC congestion control with delay-gradient analysis
- **Adaptive FEC** — Loss-driven parity ratio adjustment in real time
- **Multi-client support** — Up to 4 players, per-client transport and congestion state
- **NAT traversal** — STUN (RFC 5389) public address discovery
- **QoS / DSCP tagging** — EF (Expedited Forwarding) on all UDP sockets
- **USO batched UDP sends** — UDP Segment Offload for reduced system call overhead
- **Sub-millisecond system tuning** — 0.5 ms timer resolution, MMCSS Pro Audio, GPU REALTIME priority
- **FLIP_DISCARD + ALLOW_TEARING** — tear-free or tear-allowed present depending on V-Sync setting
- **Pipeline stats** — 5-second interval logging of FPS, decode time, render time, recv-to-present latency
- **Dear ImGui overlay** — amber/dark theme HUD with real-time stats (F3) and settings panel (F1)
- **Connection screen UI** — graphical join screen with room code and direct-IP input (no CLI needed)
- **Room code signaling** — 6-character room codes via Cloudflare Worker, so friends can connect without knowing your IP
- **Disconnect / reconnect** — F4 or in-panel button returns to connection screen without restarting

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

## Download

Grab the latest **CouchConduit-v0.1.0-win64.zip** from [Releases](https://github.com/acLebert/Couch_Conduit/releases) or build from source (see below).

The ZIP contains everything both sides need — just unzip and go:

| File | Purpose |
|------|---------|
| `Start-Host.bat` | One-click host launcher |
| `Start-Client.bat` | One-click client launcher |
| `cc_host.exe` | Host binary |
| `cc_client.exe` | Client binary + connection screen |
| `avcodec-62.dll` / `avutil-60.dll` / `swresample-6.dll` | FFmpeg (needed by client) |
| `vc_redist.x64.exe` | Visual C++ runtime installer (run once if needed) |
| `README.txt` | Quick-start guide |

> **First time?** If you get a "VCRUNTIME140.dll not found" error, run `vc_redist.x64.exe` from the ZIP.

---

## Quick Start

### Host (the PC running the game)

Double-click **Start-Host.bat** (or run `cc_host.exe` from a terminal).

It will print your **Local IP** and start listening for clients:

```
  ========================================
  ===       COUCH CONDUIT HOST         ===
  ========================================

  Local IP  : 192.168.1.42:47100

  Waiting for client to connect via TCP session...
```

Share your IP with your friend. Press **Ctrl+C** to stop.

### Client (your friend's PC)

Double-click **Start-Client.bat** (or run `cc_client.exe`).

A connection screen appears with two options:
- **Direct IP** — type the host's IP address and click **CONNECT**
- **Room Code** — type a 6-character room code and click **JOIN** (requires `--signaling-server` on both sides)

### In-Game Controls

| Key | Action |
|-----|--------|
| **F1** | Toggle settings panel (has a DISCONNECT button) |
| **F3** | Toggle real-time stats overlay (FPS, latency, bitrate) |
| **F4** | Quick disconnect (returns to connection screen) |
| **ESC** | Quit entirely |

### Room Codes (optional)

If you deploy the included Cloudflare Worker signaling server (`server/`), both host and client can use room codes so your friend never needs to know your IP:

```powershell
# Host
cc_host.exe --signaling-server https://your-worker.workers.dev

# Client
cc_client.exe --signaling-server https://your-worker.workers.dev
```

The host prints a 6-character room code; the client types it in and clicks JOIN.

You can also set the `CC_SIGNALING_URL` environment variable instead of passing the flag.

See [TESTING.md](TESTING.md) for the full step-by-step testing guide.

### Network Requirements

| Port | Protocol | Direction | Purpose |
|------|----------|-----------|---------|
| 47100 | TCP | Bidirectional | Session negotiation + ECDH key exchange |
| 47101 | UDP | Host → Client | Video stream (encrypted) |
| 47102 | UDP | Host → Client | Audio stream (encrypted) |
| 47103 | UDP | Client → Host | Input + IDR requests (encrypted) |
| 47104 | UDP | Client → Host | Feedback (loss/TWCC reports) |
| 47203+ | UDP | Host → Client | Haptic/rumble feedback (per-client) |

Both machines must be on the same LAN, or these ports must be forwarded on the host's router for internet play. Use the built-in STUN client + room codes for NAT discovery (no port forwarding needed in many cases). Wired ethernet is strongly recommended.

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

Creates `dist/CouchConduit-v0.1.0-win64.zip` (~60 MB) containing both executables, FFmpeg DLLs, VC redistributable, launcher scripts, and a README.

### Installer

Couch Conduit provides two installer options:

**Inno Setup** (recommended — produces a standard Windows `.exe` installer):
```powershell
# Install Inno Setup 6.x from https://jrsoftware.org/isinfo.php
iscc.exe installer\CouchConduit.iss
# Output: dist\CouchConduit-0.1.0-x64-setup.exe
```

**WiX v4 MSI** (produces a `.msi` package):
```powershell
# Install WiX: dotnet tool install --global wix
wix build installer\CouchConduit.wxs -o dist\CouchConduit-0.1.0-x64.msi
```

**Portable ZIP** (no installer needed):
```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-installer.ps1
# Output: dist\CouchConduit-0.1.0-x64.zip
```

All options bundle the executables, FFmpeg DLLs, and documentation. The Inno Setup installer also creates Start Menu shortcuts, adds to PATH, and checks for the ViGEmBus driver.

---

## CLI Reference

### Host (`cc_host.exe`)

| Flag | Default | Description |
|------|---------|-------------|
| `--bitrate <kbps>` | `20000` | Video bitrate |
| `--fps <N>` | `60` | Target framerate |
| `--codec <h264\|hevc\|av1>` | `hevc` | Video codec |
| `--encode-resolution <WxH>` | *(capture res)* | Encode resolution (GPU downscale if different from desktop) |
| `--signaling-server <url>` | *(none)* | Enable room codes via signaling server |
| `--no-session` | off | Skip TCP session (direct UDP, no encryption) |
| `--client <IP>` | *(auto)* | Legacy: specify client IP directly |

### Client (`cc_client.exe`)

With no arguments, opens the graphical **connection screen**. Pass a host IP to connect directly.

| Flag | Default | Description |
|------|---------|-------------|
| `<HOST_IP>` (positional) | *(none)* | Host IP — skips connection screen |
| `--resolution <WxH>` | `1920x1080` | Window resolution |
| `--fullscreen` | off | Start in fullscreen |
| `--vsync` | off | Enable V-Sync (adds latency) |
| `--signaling-server <url>` | *(none)* | Enable room code lookup |
| `--no-session` | off | Skip TCP session (direct UDP, no encryption) |

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
4. **Packetize** — RTP-style packetization with sequence numbers + Reed-Solomon FEC parity shards
5. **Transport** — AES-GCM encrypted UDP on ports 47101 (video), 47102 (audio), 47103 (input)
6. **Reassemble** — Client reassembles frame from packets, recovers multi-packet losses via RS FEC
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
| **Reed-Solomon FEC** | Multi-packet recovery with GF(2^8) Vandermonde matrix |
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
│   ├── package.ps1                   # Build + package distributable ZIP
│   └── dist-assets/                  # Launcher BATs + README bundled in ZIP
├── server/
│   └── src/index.ts                  # Cloudflare Worker signaling server (room codes)
│
├── include/couch_conduit/
│   ├── common/
│   │   ├── types.h                   # VideoConfig, FrameMetadata, GamepadState, ports
│   │   ├── transport.h               # UdpSocket, Video/Input Sender/Receiver, FEC
│   │   ├── signaling.h               # WinHTTP signaling client (room codes)
│   │   ├── stun.h                    # STUN client (RFC 5389)
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
│       ├── overlay.h                # Dear ImGui overlay (stats HUD + settings)
│       ├── connection_screen.h      # Graphical connection UI
│       └── input_capture.h          # XInput + Raw Input capture
│
├── src/
│   ├── common/                       # cc_common static library
│   │   ├── transport/                # UDP socket, video/input send/recv, FEC
│   │   ├── net/                      # Signaling client, STUN client
│   │   ├── protocol/                 # TCP session negotiation + ECDH
│   │   ├── crypto/                   # AES-128-GCM via Windows CNG
│   │   └── util/                     # System tuning, timer resolution
│   ├── host/                         # cc_host executable
│   │   ├── main.cpp                  # CLI parsing, STUN + room code, signal handling
│   │   ├── host_session.cpp          # Session orchestrator
│   │   ├── capture/                  # DXGI Desktop Duplication
│   │   ├── encode/                   # NVENC encoder
│   │   ├── input/                    # Input injection (SendInput + ViGEmBus)
│   │   └── audio/                    # WASAPI loopback capture
│   └── client/                       # cc_client executable
│       ├── main.cpp                  # Window, WndProc, connection loop
│       ├── client_session.cpp        # Session orchestrator
│       ├── decode/                   # D3D11VA decoder
│       ├── render/                   # D3D11 renderer + ImGui overlay
│       ├── ui/                       # Connection screen (Dear ImGui)
│       ├── input/                    # XInput + Raw Input
│       └── audio/                    # WASAPI audio player
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
| Transport | AES-GCM encrypted UDP with RTP packetization, Reed-Solomon FEC |
| Input | XInput (controllers), Raw Input (mouse/keyboard), `SendInput()` |
| Gamepad Injection | ViGEmBus (virtual Xbox 360 controller) |
| Audio Capture | WASAPI loopback mode |
| Encryption | AES-128-GCM via Windows CNG (`bcrypt.h`) |
| Scheduling | MMCSS Pro Audio, `NtSetTimerResolution(0.5ms)`, D3DKMT GPU REALTIME |
| Overlay UI | Dear ImGui (vendored) — DX11 + Win32 backends |
| Signaling | Cloudflare Worker + KV (room codes) |
| HTTP Client | WinHTTP (native, no dependencies) |
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
- [x] TCP session negotiation + ECDH P-256 key exchange
- [x] Wire-level AES-GCM encryption on all transport
- [x] Audio streaming (WASAPI loopback → UDP → client playback)
- [x] ViGEmBus full gamepad injection (virtual Xbox 360 controller)
- [x] Reed-Solomon FEC (multi-packet recovery)
- [x] Adaptive FEC (loss-driven parity ratio)
- [x] Adaptive bitrate (GCC-like TWCC congestion control)
- [x] QoS / DSCP packet tagging (Expedited Forwarding)
- [x] Multi-client support (up to 4-player couch co-op)
- [x] NAT traversal / STUN (RFC 5389)
- [x] Haptic / rumble feedback (XInput vibration)
- [x] USO batched UDP sends
- [x] Installer / MSI packaging (Inno Setup + WiX + portable ZIP)
- [x] Dear ImGui overlay (amber/dark theme, stats HUD, settings panel)
- [x] Connection screen UI (graphical room code + direct IP input)
- [x] Room code signaling (Cloudflare Worker + WinHTTP client)
- [x] Disconnect / reconnect flow (F4 hotkey + in-panel button)
- [x] Single-ZIP distribution (host + client + FFmpeg + VC redist + launchers)

---

## Key Differences from Sunshine/Moonlight

| Aspect | Sunshine / Moonlight | Couch Conduit |
|--------|---------------------|---------------|
| Capture trigger | Timer-based, async from input | **Input-triggered** — frame captured on input arrival |
| Decode wakeup | `SDL_Delay(2)` polling loop | **Event-signaled** — zero-wait decode |
| Frame scheduling | Best-effort delivery | **Deadline-aware** — skip encode if frame will be late |
| FEC | Static percentage overhead | **Reed-Solomon FEC** with adaptive loss-driven ratio |
| Bitrate | Fixed or manually capped | **GCC-like TWCC** adaptive bitrate with delay-gradient analysis |
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
