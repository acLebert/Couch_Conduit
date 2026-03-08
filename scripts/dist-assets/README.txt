
  ╔══════════════════════════════════════════════╗
  ║          COUCH CONDUIT  v0.1.0              ║
  ║     Ultra-Low-Latency Game Streaming        ║
  ╚══════════════════════════════════════════════╝


  QUICK START
  ───────────

  1.  FIRST TIME ONLY — if you get a "VCRUNTIME140.dll not found" error,
      run  vc_redist.x64.exe  to install the Visual C++ runtime.

  2.  HOST  (the PC running the game):
        Double-click  Start-Host.bat
        It will print your Local IP address. Share it with friends.

  3.  CLIENT (the PC that wants to watch/play):
        Double-click  Start-Client.bat
        A connection screen appears — enter the host's IP and click CONNECT.


  REQUIREMENTS
  ────────────

  Host PC:
    • Windows 10/11 (64-bit)
    • NVIDIA GPU (GTX 900-series or newer) — NVENC hardware encoder
    • Game running on the desktop

  Client PC:
    • Windows 10/11 (64-bit)
    • Any GPU with D3D11 support


  IN-GAME CONTROLS (CLIENT)
  ─────────────────────────

    F1  — Open/close settings panel (has a DISCONNECT button)
    F3  — Toggle stats overlay (FPS, latency, bitrate)
    F4  — Quick disconnect (returns to connection screen)
    ESC — Quit entirely


  COMMAND-LINE OPTIONS
  ────────────────────

  Host (cc_host.exe):
    --bitrate <kbps>         Video bitrate (default: 20000)
    --fps <fps>              Target framerate (default: 60)
    --codec <h264|hevc|av1>  Video codec (default: hevc)
    --encode-resolution WxH  Encode at a different resolution
    --help                   Show all options

  Client (cc_client.exe):
    cc_client.exe <ip>       Connect directly (skips connection screen)
    --vsync                  Enable V-Sync
    --fullscreen             Start in fullscreen
    --resolution WxH         Window size (default: 1920x1080)
    --help                   Show all options


  CONNECTING OVER THE INTERNET
  ────────────────────────────

  For LAN (same Wi-Fi/network): just use the IP shown by the host.

  For internet play you need either:
    a) Port-forward TCP 47100 and UDP 47101-47104 on the host's router, OR
    b) Use a signaling server with room codes:
         cc_host.exe   --signaling-server https://your-worker.workers.dev
         cc_client.exe --signaling-server https://your-worker.workers.dev


  FILES IN THIS ZIP
  ─────────────────

    cc_host.exe         Host application (streams your desktop)
    cc_client.exe       Client application (receives and renders stream)
    avcodec-62.dll      FFmpeg decoder library  (needed by client)
    avutil-60.dll       FFmpeg utility library   (needed by client)
    swresample-6.dll    FFmpeg resampler library (needed by client)
    vc_redist.x64.exe   Visual C++ Runtime installer (run if needed)
    Start-Host.bat      One-click host launcher
    Start-Client.bat    One-click client launcher
    README.txt          This file

