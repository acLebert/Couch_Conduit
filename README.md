# Couch Conduit

**Ultra-low-latency remote play for couch co-op over the internet.**

Couch Conduit lets your friends connect with their controllers and play as Player 2 (3, 4…) on your PC — as if they were sitting next to you. Target: **< 10 ms input-to-photon latency** on LAN, **< 30 ms** over decent internet.

## Architecture

```
┌─────────── HOST ───────────┐          ┌─────────── CLIENT ──────────┐
│                             │          │                              │
│  DXGI Desktop Duplication   │          │   SDL2 / Raw Win32 Input     │
│          │                  │          │          │                   │
│          ▼                  │          │          ▼                   │
│  NVENC H.265/AV1 Encode    │  UDP/RTP │   AES Encrypt → Send        │
│          │                  │ ◄────────┤          (input channel)     │
│          ▼                  │          │                              │
│  AES Encrypt + FEC          │ ────────►│   FEC Recover + Decrypt      │
│          │                  │  UDP/RTP │          │                   │
│  UDP Send (USO batched)     │ (video)  │          ▼                   │
│                             │          │   D3D11VA / NVDEC Decode     │
│  WASAPI Loopback Capture    │ ────────►│          │                   │
│          │                  │  (audio) │          ▼                   │
│  Opus Encode → Send         │          │   D3D11 Render (ALLOW_TEAR)  │
│                             │          │                              │
│  ViGEmBus Input Injection ◄─┤──────────┤   Opus Decode → WASAPI Play  │
│                             │  (input) │                              │
└─────────────────────────────┘          └──────────────────────────────┘
```

## Key Innovations Over Existing Solutions

| Feature | Sunshine/Moonlight | Couch Conduit |
|---------|-------------------|---------------|
| Input → Capture | Timer-based capture, async input | **Input-triggered capture** |
| FEC | Static % overhead | **Adaptive FEC** (measured loss) |
| Bitrate | Fixed / hard-capped | **Adaptive bitrate** (TWCC-style) |
| Frame scheduling | Best-effort | **Deadline-aware** (skip if late) |
| Decode wakeup | `SDL_Delay(2)` polling | **Event-signaled** (zero wait) |
| Input thread | Shared main thread / task pool | **Dedicated RT thread** |
| Render stats | CPU timestamps | **GPU timestamp queries** |

## Building

### Prerequisites

- Windows 10/11
- Visual Studio 2022 with C++ Desktop workload
- CMake 3.24+
- NVIDIA GPU (Turing or newer recommended)
- [NVIDIA Video Codec SDK](https://developer.nvidia.com/nvidia-video-codec-sdk) 12.x+
- [ViGEmBus Driver](https://github.com/nefarius/ViGEmBus/releases) (host only)

### Build

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config RelWithDebInfo
```

### Run

```powershell
# Host
./build/RelWithDebInfo/cc_host.exe

# Client
./build/RelWithDebInfo/cc_client.exe <host-ip>
```

## Project Structure

```
Couch_Conduit/
├── include/couch_conduit/     # Public headers
│   ├── common/                # Shared types, protocol, transport
│   ├── host/                  # Host-side interfaces
│   └── client/                # Client-side interfaces
├── src/
│   ├── common/                # Shared library
│   │   ├── transport/         # UDP, FEC, encryption
│   │   ├── protocol/          # Wire format, session negotiation
│   │   ├── crypto/            # AES-GCM
│   │   └── util/              # Timers, logging, threading
│   ├── host/                  # Host binary
│   │   ├── capture/           # DXGI Desktop Duplication
│   │   ├── encode/            # NVENC
│   │   ├── input/             # ViGEmBus controller injection
│   │   └── audio/             # WASAPI loopback
│   └── client/                # Client binary
│       ├── decode/            # D3D11VA / FFmpeg
│       ├── render/            # D3D11 swap chain
│       ├── input/             # Controller/KB/Mouse capture
│       └── audio/             # WASAPI playback
├── docs/                      # Architecture docs
└── third_party/               # External dependencies
```

## License

MIT
