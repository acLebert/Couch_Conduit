# Couch Conduit — Remote Play Testing Guide

Ultra-low-latency game streaming. One person hosts, the other connects as a client.

---

## What You Need

### Host (your machine — the one running the game)
- **Windows 10/11** (64-bit)
- **NVIDIA GPU** (GTX 1050+ / RTX series) with up-to-date drivers
- The game you want to stream running on your desktop
- `CouchConduit-Host.zip`

### Client (your friend's machine — the one playing remotely)
- **Windows 10/11** (64-bit)
- Any GPU (AMD/Intel/NVIDIA) — decoding uses D3D11VA
- A controller (Xbox/PS via XInput) _or_ keyboard + mouse
- `CouchConduit-Client.zip`

### Network
- Both machines on the same LAN **or** port-forwarded over the internet
- **Ports needed** (UDP): `47101` (video), `47103` (input)
- Wired ethernet strongly recommended — Wi-Fi adds 2-10ms jitter

---

## Quick Start (5 minutes)

### Step 1 — Host Setup

1. Extract `CouchConduit-Host.zip` to a folder
2. Open a terminal (PowerShell or CMD) in that folder
3. Find your local IP:
   ```
   ipconfig
   ```
   Look for your **IPv4 Address** (e.g. `192.168.1.50`)

4. Start the host, pointing it at the client's IP:
   ```
   .\cc_host.exe --client <CLIENT_IP> --encode-resolution 1920x1080
   ```
   
   Example:
   ```
   .\cc_host.exe --client 192.168.1.75 --encode-resolution 1920x1080
   ```

5. You should see:
   ```
   === Host session streaming ===
   Streaming to 192.168.1.75 — press Ctrl+C to stop
   ```

### Step 2 — Client Setup

1. Extract `CouchConduit-Client.zip` to a folder
2. Open a terminal in that folder
3. Connect to the host:
   ```
   .\cc_client.exe <HOST_IP> --resolution 1920x1080
   ```
   
   Example:
   ```
   .\cc_client.exe 192.168.1.50 --resolution 1920x1080
   ```

4. A window should open showing the host's desktop. Press **ESC** to quit.

### Step 3 — Play!

1. On the **host**, launch your game
2. The **client** should see the game in their window
3. Controller input, keyboard, and mouse are forwarded to the host
4. Play as if you're sitting at the host's desk!

---

## Host Options

| Flag | Default | Description |
|------|---------|-------------|
| `--client <IP>` | `127.0.0.1` | Client's IP address |
| `--encode-resolution <WxH>` | _(capture res)_ | Encode resolution (match to client) |
| `--bitrate <kbps>` | `20000` | Video bitrate (higher = better quality) |
| `--fps <fps>` | `60` | Target framerate |
| `--codec <h264\|hevc\|av1>` | `hevc` | Video codec |

### Bitrate Guide

| Network | Recommended Bitrate |
|---------|-------------------|
| Same PC (localhost) | `50000` |
| LAN (Gigabit) | `30000–50000` |
| LAN (100Mbps) | `15000–20000` |
| Internet (good) | `10000–15000` |
| Internet (ok) | `5000–8000` |

## Client Options

| Flag | Default | Description |
|------|---------|-------------|
| `<HOST_IP>` | _(required)_ | Host's IP address |
| `--resolution <WxH>` | `1920x1080` | Window size |

---

## Troubleshooting

### "nvEncodeAPI64.dll not found"
Install or update your **NVIDIA GPU drivers** on the host. NVENC requires a GeForce or Quadro GPU.

### Client connects but sees black/green screen
The host's first frame was a keyframe that the client missed. This should self-correct within 500ms. If it persists:
- Restart the client
- Make sure the host started **before** the client

### Poor quality or blocky video
Increase the bitrate: `--bitrate 30000` or higher.

### High latency / input feels sluggish
- Use **wired ethernet** (not Wi-Fi)
- Lower resolution: `--encode-resolution 1280x720`
- Check that no other heavy network traffic is running
- The client logs pipeline stats every 5 seconds — look for the `recv→present` time

### "ViGEmBus not available" warning on host
This is normal if you don't need virtual gamepad support. Keyboard and mouse forwarding still works. To enable gamepad injection, install [ViGEmBus](https://github.com/nefarius/ViGEmBus/releases).

### Firewall blocking connection
Allow `cc_host.exe` and `cc_client.exe` through Windows Firewall, or temporarily disable it for testing:
```powershell
# Run as Administrator on BOTH machines
netsh advfirewall firewall add rule name="CouchConduit" dir=in action=allow protocol=UDP localport=47101,47103
```

### Port forwarding (internet play)
On the **host's router**, forward these UDP ports to the host's local IP:
- `47101` (video)
- `47103` (input)

The client connects to the host's **public IP** (find it at [whatismyip.com](https://whatismyip.com)).

---

## Architecture (for the curious)

```
HOST                                          CLIENT
┌─────────────┐                              ┌─────────────┐
│ DXGI Capture│─→ D3D11 Video Processor ─→   │             │
│ (5120x1440) │   (downscale to 1920x1080)   │             │
└──────┬──────┘                              │             │
       ▼                                      │             │
┌─────────────┐    UDP :47101 (video)        ┌┴────────────┐│
│ NVENC Encode│ ════════════════════════════► │ D3D11VA     ││
│ (HEVC P1)   │                              │ Decode      ││
└─────────────┘                              └──────┬──────┘│
                                                    ▼       │
┌─────────────┐    UDP :47103 (input)        ┌─────────────┐│
│ SendInput() │ ◄════════════════════════════ │ XInput +    ││
│ + ViGEmBus  │                              │ Raw Input   ││
└─────────────┘                              └─────────────┘│
                                              │  NV12→RGBA  │
                                              │  Renderer   │
                                              └─────────────┘
```

- **Capture**: DXGI Desktop Duplication (zero-copy GPU texture)
- **Encode**: NVENC hardware encoder (HEVC, P1 preset, ultra-low-latency)  
- **Transport**: Custom UDP/RTP with XOR FEC for packet loss recovery
- **Decode**: D3D11VA hardware decoder via FFmpeg (zero-copy to render)
- **Render**: FLIP_DISCARD swap chain, tearing-enabled, MMCSS Pro Audio priority

Measured client-side latency: **< 0.5ms** (decode + render on localhost)
