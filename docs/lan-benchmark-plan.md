# Couch Conduit — LAN & Impaired Network Benchmark Plan

## Overview

After localhost benchmarks validate the pipeline in zero-network-cost conditions,
the next step is a **2-machine wired LAN** test followed by **impaired network**
simulations to characterize behavior under real-world conditions.

---

## Phase 1: Wired LAN Baseline

### Hardware Setup
| Role   | Machine | Connection |
|--------|---------|------------|
| Host   | Desktop PC with NVIDIA GPU (NVENC) | Gigabit Ethernet |
| Client | Second PC or laptop | Gigabit Ethernet, same switch |

### Network Requirements
- Direct Ethernet connection (same switch, no Wi-Fi)
- Verify with `iperf3` that bandwidth exceeds 100 Mbps
- Measure baseline RTT: `ping -n 50 <host-ip>` — expect < 1 ms

### Test Matrix
| Bitrate | Codec | FPS | Resolution | Runs | Duration |
|---------|-------|-----|------------|------|----------|
| 20 Mbps | HEVC  | 60  | 1080p      | 10   | 30s      |
| 10 Mbps | HEVC  | 60  | 1080p      | 10   | 30s      |
|  5 Mbps | HEVC  | 60  | 1080p      | 10   | 30s      |

### Running
```powershell
# On host machine:
.\build\src\host\Release\cc_host.exe --bitrate 20000 --fps 60 --codec hevc --duration 35 --csv host_20mbps.csv

# On client machine (replace HOST_IP):
.\build\src\client\Release\cc_client.exe HOST_IP --no-session --duration 30 --csv client_20mbps.csv
```

Or adapt `scripts/benchmark.ps1` to accept `--host` and `--remote` parameters for
2-machine orchestration (SSH/WinRM to start host remotely).

### Metrics to Compare vs Localhost
- **avg_pipeline_ms** — should increase by ~network RTT/2
- **fps** — should remain at 60 (network bandwidth is not the bottleneck)
- **dropped_frames** — should remain 0 on Gigabit
- **first_decode_ms / first_present_ms** — startup latency over network

---

## Phase 2: Impaired Network Simulation

### Tool: Clumsy (Windows)
[Clumsy](https://jagt.github.io/clumsy/) is a free Windows tool that intercepts
network packets and applies impairments. Run on either machine.

Alternative: `tc` / `netem` on a Linux router, or a managed switch with QoS.

### Impairment Scenarios

| Scenario | Added Latency | Packet Loss | Jitter | Notes |
|----------|---------------|-------------|--------|-------|
| Baseline LAN | 0 ms | 0% | 0 ms | Phase 1 reference |
| Moderate latency | 5 ms one-way | 0% | 0 ms | Simulates same-house Wi-Fi |
| High latency | 20 ms one-way | 0% | 0 ms | Simulates cross-city WAN |
| Light loss | 0 ms | 1% | 0 ms | Tests FEC recovery |
| Heavy loss | 0 ms | 5% | 0 ms | Stress-test FEC + IDR recovery |
| Jitter | 0 ms | 0% | 5 ms std | Simulates congested Wi-Fi |
| Combined | 10 ms | 2% | 3 ms | Realistic bad Wi-Fi |

### Per-Scenario Test Config
- Bitrate: **20 Mbps** (fixed, to isolate network effects)
- Codec: HEVC, FPS: 60, Resolution: 1080p
- Runs: **5** per scenario (fewer needed since network is the variable)
- Duration: **30s** per run
- 1 warm-up run per scenario

### Clumsy Configuration Examples

```
# Moderate latency (5ms each direction)
Clumsy: Lag = Enabled, Delay = 5ms, Chance = 100%

# 1% packet loss
Clumsy: Drop = Enabled, Chance = 1.0%

# Jitter
Clumsy: Lag = Enabled, Delay = 0ms, Chance = 100%
        + Out of order = Enabled, Chance = 25%

# Combined (realistic bad Wi-Fi)
Clumsy: Lag = Enabled, Delay = 10ms, Chance = 100%
        Drop = Enabled, Chance = 2.0%
        Tamper = Disabled
        Out of order = Enabled, Chance = 10%
```

### Key Metrics

| Metric | Expected Impact |
|--------|-----------------|
| avg_pipeline_ms | Increases with added latency |
| fps | Drops under heavy loss (decoder stalls waiting for IDR) |
| dropped_frames | Increases with loss (incomplete frames discarded) |
| min/max_pipeline_ms | Max increases with jitter |
| avg_decode_ms | Should remain constant (GPU-bound, not network) |

---

## Phase 3: Analysis & Reporting

### Comparative Summary Table
Generate a cross-scenario summary showing how each metric degrades:

```
| Scenario        | Avg Pipeline | FPS  | Dropped | Max Pipeline |
|-----------------|-------------|------|---------|-------------|
| Localhost        | X.X ms      | 60.0 | 0       | X.X ms      |
| LAN baseline     | X.X ms      | 60.0 | 0       | X.X ms      |
| +5ms latency     | X.X ms      | 60.0 | 0       | X.X ms      |
| +20ms latency    | X.X ms      | 59.X | 0       | X.X ms      |
| 1% loss          | X.X ms      | 5X.X | X       | X.X ms      |
| 5% loss          | X.X ms      | 4X.X | X       | X.X ms      |
| Combined         | X.X ms      | 5X.X | X       | X.X ms      |
```

### Actionable Insights
1. **Latency budget**: What network RTT keeps pipeline < 16.7ms (1 frame)?
2. **FEC effectiveness**: At what loss rate does FEC stop hiding drops?
3. **Jitter tolerance**: How much jitter before max_pipeline spikes?
4. **Bitrate floor**: What's the minimum bitrate for acceptable quality?

---

## Automation Notes

To fully automate 2-machine benchmarks, extend `scripts/benchmark.ps1` with:
1. `$HostMachine` parameter — SSH/WinRM target for the host PC
2. `Invoke-Command -ComputerName $HostMachine` to start/stop cc_host.exe remotely
3. `$ImpairmentTool` parameter — auto-configure Clumsy via CLI or registry
4. Collect host CSVs via SMB share or SCP after each run
