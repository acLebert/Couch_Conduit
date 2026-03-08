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
static HWND g_hwnd = nullptr;
static std::unique_ptr<cc::client::ClientSession> g_session;

// Window procedure — forwards input events to ClientSession → InputCapture → Host
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
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
            // Raw Input for mouse — gives sub-pixel relative motion
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
        RAWINPUTDEVICE rid = {};
        rid.usUsagePage = 0x01;  // Generic Desktop
        rid.usUsage = 0x02;      // Mouse
        rid.dwFlags = RIDEV_INPUTSINK;
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
};

CliArgs ParseArgs(int argc, char* argv[]) {
    CliArgs args;

    if (argc >= 2 && argv[1][0] != '-') {
        args.hostAddr = argv[1];
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
            printf("  --resolution <WxH>     Window resolution (default: 1920x1080)\n");
            printf("  --help                 Show this help\n");
            exit(0);
        }
    }

    return args;
}

int main(int argc, char* argv[]) {
    auto args = ParseArgs(argc, argv);

    printf("\n");
    printf("  ========================================\n");
    printf("  ===      COUCH CONDUIT CLIENT        ===\n");
    printf("  ===   Ultra-Low-Latency Game Stream  ===\n");
    printf("  ========================================\n");
    printf("\n");

    CC_INFO("Couch Conduit Client v%u.%u.%u starting...",
            cc::kVersionMajor, cc::kVersionMinor, cc::kVersionPatch);
    CC_INFO("Connecting to host: %s", args.hostAddr.c_str());

    // Initialize Winsock
    if (!cc::transport::InitWinsock()) {
        CC_FATAL("Failed to initialize Winsock");
        return 1;
    }

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

    // Create window (use negotiated or CLI resolution)
    g_hwnd = CreateGameWindow(negotiatedWidth, negotiatedHeight, args.fullscreen);
    if (!g_hwnd) {
        CC_FATAL("Failed to create window");
        return 1;
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
