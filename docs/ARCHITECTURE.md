# Couch Conduit — Architecture Deep Dive

## Latency Budget (Target)

| Stage | Target | Technique |
|-------|--------|-----------|
| Input capture → send | < 0.5 ms | Dedicated RT thread, raw WinAPI |
| Network (input) | 1–5 ms | UDP, no reliability layer for input |
| Input injection | < 0.2 ms | Inline ViGEm call, no queuing |
| **Input-triggered capture** | < 0.5 ms | Capture on input arrival, not timer |
| Encode (NVENC) | 1–2 ms | Ultra-low-latency tuning, P1 preset |
| Network (video) | 1–5 ms | USO batched UDP, adaptive FEC |
| Decode (D3D11VA) | 0.5–1 ms | Event-signaled, zero-copy |
| Render + present | 0.5–2 ms | SyncInterval=0, ALLOW_TEARING |
| **Total** | **5–16 ms** | |

## Core Design Principles

### 1. Input-Triggered Capture

Unlike Sunshine (timer-based capture at display refresh rate), we trigger a capture
immediately when input arrives from the client. This cuts up to one full frame of
latency from the pipeline.

```
Traditional:  Input arrives → [waits up to 16.7ms] → Next capture → Encode → Send
Ours:         Input arrives → Immediate capture → Encode → Send
```

We still maintain a minimum capture interval to avoid overwhelming the encoder, but
the phase is aligned to input events rather than a free-running timer.

### 2. Dedicated Input Thread (Host)

Sunshine queues input events through a task pool, adding scheduling latency.
We use a dedicated thread at THREAD_PRIORITY_TIME_CRITICAL that:
1. Receives UDP input packets
2. Decrypts inline
3. Calls ViGEm/SendInput **immediately** on the same thread
4. Signals the capture thread to grab a frame

### 3. Adaptive FEC

Instead of static 20% FEC overhead, we measure packet loss over a sliding window:
- 0% loss → 5% FEC (minimal overhead)
- 1% loss → 15% FEC
- 5%+ loss → 30% FEC + reduce bitrate
- FEC adjustment happens every 100ms

### 4. Adaptive Bitrate

TWCC-style (Transport-Wide Congestion Control):
- Client timestamps every received video packet
- Sends feedback every 50ms with arrival times
- Host computes one-way delay gradient
- Increases bitrate when delay is stable, decreases on increasing delay
- Adjusts NVENC bitrate via NvEncReconfigureEncoder() — no IDR needed

### 5. Frame Deadline Scheduling

Before encoding a frame, the host estimates:
- Encode time (rolling average)
- Network transit time (RTT/2)
- Client decode time (reported by client)

If the frame would miss the client's next VBlank deadline → skip encoding,
wait for the next capture. This prevents wasting bandwidth on late frames.

### 6. Event-Signaled Decode

Moonlight's decoder polls with SDL_Delay(2) which can sleep 2-15ms.
We use a Windows Event (CreateEvent + WaitForSingleObject) signaled by the
network receive thread when a complete frame is ready. Wake-up latency: < 0.1ms.

### 7. GPU-Direct Pipeline

The video frame texture never touches CPU memory:
```
Host: DXGI DDA → GPU texture → NVENC (reads VRAM) → bitstream (CPU) → network
Client: network → bitstream (CPU) → D3D11VA (writes VRAM) → render (reads VRAM) → present
```

The only CPU↔GPU transfers are the compressed bitstream, which is ~100x smaller
than the raw frame.

## Wire Protocol

### Session Establishment (TCP)

1. Client connects to host TCP port 47100
2. TLS 1.3 handshake (self-signed cert, pinned on first connection)
3. Exchange capabilities (codecs, max resolution, controller types)
4. Derive AES-128-GCM session keys via HKDF
5. Exchange UDP port assignments
6. Switch to UDP for all streaming data

### Video Stream (UDP/RTP)

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|V=2|P|X|  CC   |M|     PT      |         Sequence Number         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                           Timestamp                             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                              SSRC                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|       Frame Number            |  Pkt Index    | Total Pkts      |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| Host Proc Time (0.1ms units) |    FEC Group  |  FEC Index      |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    AES-GCM Encrypted Payload                    |
|                          ...                                    |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    AES-GCM Authentication Tag (16 bytes)        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

### Input Stream (UDP)

```
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| Msg Type (1B) | Controller ID | Sequence (2B)                   |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    AES-GCM Encrypted Payload                    |
|                          ...                                    |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    AES-GCM Authentication Tag (16 bytes)        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

Input message types:
- 0x01: Gamepad state (buttons, axes, triggers)
- 0x02: Mouse relative motion
- 0x03: Mouse absolute position
- 0x04: Mouse button
- 0x05: Keyboard key
- 0x06: Mouse scroll
- 0x10: Controller connect/disconnect
- 0x20: Haptic/rumble feedback (host → client)

### Feedback Stream (UDP, Client → Host)

Sent every 50ms:
- Last received frame number
- Packet loss bitmap (last 64 packets)
- Decode time (last frame, microseconds)
- Render time (last frame, microseconds)
- Display queue depth
- Arrival timestamps for TWCC bitrate adaptation

## Threading Model

### Host Threads

| Thread | Priority | Affinity | Role |
|--------|----------|----------|------|
| Input Receiver | TIME_CRITICAL | Core 0 | UDP recv → decrypt → ViGEm inject → signal capture |
| Capture | HIGHEST | Core 1 | DXGI DDA WaitForVBlank / input signal → AcquireNextFrame |
| Encode | HIGHEST | Core 2 | NVENC encode → packetize → encrypt → UDP send |
| Audio | HIGH | any | WASAPI loopback → Opus encode → UDP send |
| Control | NORMAL | any | TCP session management, stats, negotiation |
| Network Send | HIGH | Core 3 | Batched UDP sends with USO |

### Client Threads

| Thread | Priority | Affinity | Role |
|--------|----------|----------|------|
| Network Recv | HIGHEST | Core 0 | UDP recv → reassemble → signal decoder |
| Decode | HIGHEST | Core 1 | D3D11VA decode → submit to pacer |
| Render/Pacer | TIME_CRITICAL | Core 2 | VSync align → present |
| Input | HIGH | Core 3 | SDL/RawInput poll → encrypt → UDP send |
| Audio | HIGH | any | Recv → Opus decode → WASAPI render |
| Control | NORMAL | any | TCP session, stats, feedback send |

## System Tuning

The host and client both apply on startup:
- `NtSetTimerResolution(5000, TRUE)` — 0.5ms timer resolution
- `SetPriorityClass(HIGH_PRIORITY_CLASS)`
- `SetProcessAffinityMask()` per-thread as above
- `D3DKMTSetProcessSchedulingPriorityClass(REALTIME)` — GPU priority
- `DwmEnableMMCSS(TRUE)` — DWM uses MMCSS
- `AvSetMmThreadCharacteristicsW(L"Pro Audio")` for critical threads
- `SO_SNDBUF` / `SO_RCVBUF` = 2MB on all UDP sockets
- `SIO_SET_PRIORITY_HINT` = `IoPriorityHintHigh` on UDP sockets
- DSCP/QoS tagging via `qWAVE` API
