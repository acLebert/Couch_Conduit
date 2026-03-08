// Couch Conduit — Client entry point
//
// Usage: cc_client.exe <host-ip> [--port <port>] [--vsync] [--fullscreen]
//
// Creates a window, connects to the host, receives and renders the video stream,
// captures input and sends it back to the host.

#include <couch_conduit/common/types.h>
#include <couch_conduit/common/transport.h>
#include <couch_conduit/common/session.h>
#include <couch_conduit/common/log.h>
#include <couch_conduit/client/client_session.h>
#include <couch_conduit/client/connection_screen.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <atomic>
#include <memory>

static std::atomic<bool> g_running{true};
static std::atomic<bool> g_disconnectRequested{false};
static HWND g_hwnd = nullptr;
static std::unique_ptr<cc::client::ClientSession> g_session;
static bool g_inSizeMove = false;  // True while user is dragging/resizing the window

// Window procedure — forwards input events to ClientSession → InputCapture → Host
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Forward input to ImGui overlay first
    if (g_session) {
        auto* overlay = g_session->GetOverlay();
        if (overlay && overlay->HandleInput(hwnd, msg, wParam, lParam)) {
            // If overlay wants mouse/keyboard, don't forward to game input
            if (overlay->WantCaptureMouse() || overlay->WantCaptureKeyboard())
                return 0;
        }
    }

    switch (msg) {
        case WM_CLOSE:
        case WM_DESTROY:
            g_running = false;
            PostQuitMessage(0);
            return 0;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            // ESC to quit (client-only shortcut)
            if (wParam == VK_ESCAPE) {
                g_running = false;
                PostQuitMessage(0);
                return 0;
            }
            // F1 = toggle settings panel, F3 = toggle stats HUD
            if (g_session) {
                auto* overlay = g_session->GetOverlay();
                if (overlay && !(lParam & (1 << 30))) {
                    if (wParam == VK_F1) { overlay->TogglePanel(); return 0; }
                    if (wParam == VK_F3) { overlay->ToggleStats(); return 0; }
                    if (wParam == VK_F4) { overlay->RequestDisconnect(); return 0; }
                }
            }
            // Forward all other keys to host
            if (g_session && !(lParam & (1 << 30))) {  // Bit 30 = was already down (auto-repeat)
                g_session->OnKeyDown(static_cast<uint16_t>(wParam));
            }
            return 0;

        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (g_session) {
                g_session->OnKeyUp(static_cast<uint16_t>(wParam));
            }
            return 0;

        case WM_LBUTTONDOWN:
            if (g_session) g_session->OnMouseButton(0, true);
            return 0;
        case WM_LBUTTONUP:
            if (g_session) g_session->OnMouseButton(0, false);
            return 0;
        case WM_RBUTTONDOWN:
            if (g_session) g_session->OnMouseButton(1, true);
            return 0;
        case WM_RBUTTONUP:
            if (g_session) g_session->OnMouseButton(1, false);
            return 0;
        case WM_MBUTTONDOWN:
            if (g_session) g_session->OnMouseButton(2, true);
            return 0;
        case WM_MBUTTONUP:
            if (g_session) g_session->OnMouseButton(2, false);
            return 0;
        case WM_XBUTTONDOWN:
            if (g_session) {
                uint8_t btn = (GET_XBUTTON_WPARAM(wParam) == XBUTTON1) ? 3 : 4;
                g_session->OnMouseButton(btn, true);
            }
            return TRUE;
        case WM_XBUTTONUP:
            if (g_session) {
                uint8_t btn = (GET_XBUTTON_WPARAM(wParam) == XBUTTON1) ? 3 : 4;
                g_session->OnMouseButton(btn, false);
            }
            return TRUE;

        case WM_MOUSEWHEEL:
            if (g_session) {
                int16_t delta = static_cast<int16_t>(GET_WHEEL_DELTA_WPARAM(wParam));
                g_session->OnMouseScroll(0, delta);
            }
            return 0;
        case WM_MOUSEHWHEEL:
            if (g_session) {
                int16_t delta = static_cast<int16_t>(GET_WHEEL_DELTA_WPARAM(wParam));
                g_session->OnMouseScroll(delta, 0);
            }
            return 0;

        case WM_INPUT: {
            // Raw Input for mouse — gives sub-pixel relative motion.
            //
            // CRITICAL: Two guards prevent a feedback loop on localhost:
            //  1. Skip events with hDevice==0 — those are synthetic events
            //     injected by SendInput() on the host side. Physical mice
            //     always have a non-zero device handle.
            //  2. Skip events while the user is dragging/resizing the window
            //     (WM_ENTERSIZEMOVE modal loop) — mouse deltas during a
            //     window drag should not be forwarded to the host.
            if (g_inSizeMove) {
                return DefWindowProcW(hwnd, msg, wParam, lParam);
            }
            UINT dataSize = 0;
            GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam),
                            RID_INPUT, nullptr, &dataSize, sizeof(RAWINPUTHEADER));
            if (dataSize > 0 && dataSize <= 256) {
                alignas(RAWINPUT) uint8_t buf[256];
                if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam),
                                    RID_INPUT, buf, &dataSize,
                                    sizeof(RAWINPUTHEADER)) == dataSize) {
                    RAWINPUT* raw = reinterpret_cast<RAWINPUT*>(buf);
                    if (raw->header.dwType == RIM_TYPEMOUSE &&
                        raw->header.hDevice != nullptr &&  // Filter injected (SendInput) events
                        (raw->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
                        if (g_session && (raw->data.mouse.lLastX != 0 || raw->data.mouse.lLastY != 0)) {
                            g_session->OnMouseMove(
                                static_cast<int16_t>(raw->data.mouse.lLastX),
                                static_cast<int16_t>(raw->data.mouse.lLastY)
                            );
                        }
                    }
                }
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        case WM_ENTERSIZEMOVE:
            g_inSizeMove = true;
            return 0;
        case WM_EXITSIZEMOVE:
            g_inSizeMove = false;
            return 0;

        case WM_SIZE: {
            // TODO: Notify renderer of resize
            break;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

HWND CreateGameWindow(uint32_t width, uint32_t height, bool fullscreen) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"CouchConduitClient";
    RegisterClassExW(&wc);

    DWORD style = fullscreen ? WS_POPUP : (WS_OVERLAPPEDWINDOW);
    RECT rect = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
    AdjustWindowRect(&rect, style, FALSE);

    HWND hwnd = CreateWindowExW(
        0,
        L"CouchConduitClient",
        L"Couch Conduit",
        style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left,
        rect.bottom - rect.top,
        nullptr, nullptr,
        GetModuleHandleW(nullptr),
        nullptr
    );

    if (hwnd) {
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);

        // Register for raw input (mouse)
        // NOTE: Do NOT use RIDEV_INPUTSINK — it captures mouse input even when
        // the window isn't focused, causing a feedback loop on localhost where
        // SendInput() on the host generates new Raw Input events → amplified DPI.
        RAWINPUTDEVICE rid = {};
        rid.usUsagePage = 0x01;  // Generic Desktop
        rid.usUsage = 0x02;      // Mouse
        rid.dwFlags = 0;         // Only receive input when focused
        rid.hwndTarget = hwnd;
        RegisterRawInputDevices(&rid, 1, sizeof(rid));
    }

    return hwnd;
}

struct CliArgs {
    std::string hostAddr = "127.0.0.1";
    uint16_t    port = cc::kDefaultVideoPort;
    uint32_t    width = 1920;
    uint32_t    height = 1080;
    bool        vsync = false;
    bool        fullscreen = false;
    bool        noSession = false;
    bool        hasHostArg = false;        // True if user specified a host on CLI
    std::string signalingServer;           // Room code signaling server URL
};

CliArgs ParseArgs(int argc, char* argv[]) {
    CliArgs args;

    if (argc >= 2 && argv[1][0] != '-') {
        args.hostAddr = argv[1];
        args.hasHostArg = true;
    }

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            args.port = static_cast<uint16_t>(atoi(argv[++i]));
        } else if (strcmp(argv[i], "--vsync") == 0) {
            args.vsync = true;
        } else if (strcmp(argv[i], "--fullscreen") == 0) {
            args.fullscreen = true;
        } else if (strcmp(argv[i], "--no-session") == 0) {
            args.noSession = true;
        } else if (strcmp(argv[i], "--signaling-server") == 0 && i + 1 < argc) {
            args.signalingServer = argv[++i];
        } else if (strcmp(argv[i], "--resolution") == 0 && i + 1 < argc) {
            ++i;
            sscanf(argv[i], "%ux%u", &args.width, &args.height);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Couch Conduit Client v%u.%u.%u\n\n",
                   cc::kVersionMajor, cc::kVersionMinor, cc::kVersionPatch);
            printf("Usage: cc_client.exe <host-ip> [options]\n\n");
            printf("Options:\n");
            printf("  --port <port>          Video port (default: %u)\n", cc::kDefaultVideoPort);
            printf("  --vsync                Enable V-Sync (adds latency)\n");
            printf("  --fullscreen           Start in fullscreen mode\n");
            printf("  --no-session           Skip TCP session (direct UDP)\n");
            printf("  --signaling-server <url> Signaling server for room codes\n");
            printf("  --resolution <WxH>     Window resolution (default: 1920x1080)\n");
            printf("  --help                 Show this help\n");
            exit(0);
        }
    }

    return args;
}

int main(int argc, char* argv[]) {
    auto args = ParseArgs(argc, argv);

    // Check environment variable for signaling server
    if (args.signalingServer.empty()) {
        const char* envUrl = std::getenv("CC_SIGNALING_URL");
        if (envUrl && envUrl[0]) args.signalingServer = envUrl;
    }

    printf("\n");
    printf("  ========================================\n");
    printf("  ===      COUCH CONDUIT CLIENT        ===\n");
    printf("  ===   Ultra-Low-Latency Game Stream  ===\n");
    printf("  ========================================\n");
    printf("\n");

    CC_INFO("Couch Conduit Client v%u.%u.%u starting...",
            cc::kVersionMajor, cc::kVersionMinor, cc::kVersionPatch);

    // Initialize Winsock
    if (!cc::transport::InitWinsock()) {
        CC_FATAL("Failed to initialize Winsock");
        return 1;
    }

    // ===================================================================
    // If no host specified on CLI and not in direct/no-session mode,
    // show the connection screen so friends can just double-click & go.
    // ===================================================================
    bool showConnectionScreen = !args.hasHostArg && !args.noSession;

    // Create window early (shared between connection screen and game)
    g_hwnd = CreateGameWindow(args.width, args.height, args.fullscreen);
    if (!g_hwnd) {
        CC_FATAL("Failed to create window");
        cc::transport::CleanupWinsock();
        return 1;
    }

    if (showConnectionScreen) {
        CC_INFO("No host specified — showing connection screen");

        // ===============================================================
        // Main connection loop: show UI → connect → stream → disconnect
        // → back to connection UI.  Exits when user closes the window.
        // ===============================================================
        while (g_running) {
            auto connResult = cc::client::ShowConnectionScreen(
                g_hwnd, args.width, args.height, args.signalingServer);

            if (!connResult.connected || connResult.cancelled) {
                CC_INFO("Connection cancelled by user");
                break;
            }

            args.hostAddr = connResult.hostAddr;
            CC_INFO("Connecting to %s:%u", args.hostAddr.c_str(), connResult.controlPort);

            // ── Session negotiation ──────────────────────────────────
            std::array<uint8_t, 16> sessionKey{};
            bool encrypted = false;
            uint32_t negotiatedWidth = args.width;
            uint32_t negotiatedHeight = args.height;

            {
                CC_INFO("Connecting to host via TCP session (ECDH key exchange)...");
                cc::protocol::Session tcpSession;
                cc::protocol::SessionConfig negotiated;
                if (tcpSession.ConnectToHost(args.hostAddr, connResult.controlPort,
                                              negotiated, 10000)) {
                    sessionKey = negotiated.sessionKey;
                    encrypted = true;
                    negotiatedWidth = negotiated.width;
                    negotiatedHeight = negotiated.height;
                    CC_INFO("Session established — %ux%u, encryption ON",
                            negotiatedWidth, negotiatedHeight);
                } else {
                    CC_WARN("TCP session failed — falling back to direct UDP");
                }
            }

            // ── Create session & stream ─────────────────────────
            g_session = std::make_unique<cc::client::ClientSession>();
            cc::client::ClientSession::Config cfg;
            cfg.hostAddr      = args.hostAddr;
            cfg.videoPort     = cc::kDefaultVideoPort;
            cfg.windowWidth   = negotiatedWidth;
            cfg.windowHeight  = negotiatedHeight;
            cfg.vsync         = args.vsync;
            cfg.hwnd          = g_hwnd;
            cfg.sessionKey    = sessionKey;
            cfg.encrypted     = encrypted;

            if (!g_session->Init(cfg)) {
                CC_ERROR("Session init failed for %s", args.hostAddr.c_str());
                g_session.reset();
                continue;  // back to connection screen
            }

            CC_INFO("Connected to %s — F4 to disconnect, ESC to quit", args.hostAddr.c_str());

            // Message loop — runs until user quits or disconnects
            g_disconnectRequested = false;
            MSG msg = {};
            while (g_running && !g_disconnectRequested) {
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    if (msg.message == WM_QUIT) {
                        g_running = false;
                        break;
                    }
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }

                // Check overlay disconnect button
                if (g_session) {
                    auto* overlay = g_session->GetOverlay();
                    if (overlay && overlay->IsDisconnectRequested()) {
                        g_disconnectRequested = true;
                    }
                }

                if (g_running && !g_disconnectRequested) {
                    Sleep(1);
                }
            }

            // Tear down session
            CC_INFO("Disconnecting...");
            if (g_session) {
                g_session->Stop();
                g_session.reset();
            }

            if (!g_running) break;  // window closed — exit completely

            CC_INFO("Returning to connection screen");
            // Loop back to ShowConnectionScreen
        }

        // Clean exit
        DestroyWindow(g_hwnd);
        cc::transport::CleanupWinsock();
        return 0;
    }

    CC_INFO("Connecting to host: %s", args.hostAddr.c_str());

    // Session negotiation variables
    std::array<uint8_t, 16> sessionKey{};
    bool encrypted = false;
    uint32_t negotiatedWidth = args.width;
    uint32_t negotiatedHeight = args.height;

    if (!args.noSession) {
        CC_INFO("Connecting to host via TCP session (ECDH key exchange)...");

        cc::protocol::Session tcpSession;
        cc::protocol::SessionConfig negotiated;

        if (!tcpSession.ConnectToHost(args.hostAddr, cc::kDefaultControlPort,
                                       negotiated, 10000)) {
            CC_WARN("TCP session failed — falling back to direct UDP (no encryption)");
        } else {
            sessionKey = negotiated.sessionKey;
            encrypted = true;
            negotiatedWidth = negotiated.width;
            negotiatedHeight = negotiated.height;
            CC_INFO("Session established — %ux%u, encryption ON",
                    negotiatedWidth, negotiatedHeight);
        }
    }

    // Resize window if negotiated resolution differs
    if (g_hwnd && (negotiatedWidth != args.width || negotiatedHeight != args.height)) {
        DWORD style = static_cast<DWORD>(GetWindowLongW(g_hwnd, GWL_STYLE));
        RECT rect = {0, 0, static_cast<LONG>(negotiatedWidth), static_cast<LONG>(negotiatedHeight)};
        AdjustWindowRect(&rect, style, FALSE);
        SetWindowPos(g_hwnd, nullptr, 0, 0,
                     rect.right - rect.left, rect.bottom - rect.top,
                     SWP_NOMOVE | SWP_NOZORDER);
    }

    CC_INFO("Window created: %ux%u, vsync=%s, fullscreen=%s, encrypted=%s",
            negotiatedWidth, negotiatedHeight,
            args.vsync ? "on" : "off",
            args.fullscreen ? "yes" : "no",
            encrypted ? "yes" : "no");

    // Create and initialize client session
    g_session = std::make_unique<cc::client::ClientSession>();
    cc::client::ClientSession::Config sessionConfig;
    sessionConfig.hostAddr      = args.hostAddr;
    sessionConfig.videoPort     = args.port;
    sessionConfig.windowWidth   = negotiatedWidth;
    sessionConfig.windowHeight  = negotiatedHeight;
    sessionConfig.vsync         = args.vsync;
    sessionConfig.hwnd          = g_hwnd;
    sessionConfig.sessionKey    = sessionKey;
    sessionConfig.encrypted     = encrypted;

    if (!g_session->Init(sessionConfig)) {
        CC_ERROR("Client session init failed — window will remain but no streaming");
        CC_INFO("Check that the host is running and reachable at %s", args.hostAddr.c_str());
    }

    CC_INFO("Connected to %s — press ESC to close", args.hostAddr.c_str());

    // Message loop
    MSG msg = {};
    while (g_running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (g_running) {
            // Yield CPU — the render thread drives display in the background
            Sleep(1);
        }
    }

    CC_INFO("Shutting down...");
    if (g_session) {
        g_session->Stop();
        g_session.reset();
    }
    DestroyWindow(g_hwnd);
    cc::transport::CleanupWinsock();

    return 0;
}
