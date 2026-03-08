// Couch Conduit — Client entry point
//
// Usage: cc_client.exe <host-ip> [--port <port>] [--vsync] [--fullscreen]
//
// Creates a window, connects to the host, receives and renders the video stream,
// captures input and sends it back to the host.

#include <couch_conduit/common/types.h>
#include <couch_conduit/common/transport.h>
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

// Window procedure
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CLOSE:
        case WM_DESTROY:
            g_running = false;
            PostQuitMessage(0);
            return 0;

        case WM_KEYDOWN:
            // TODO: Forward to InputCapture::OnKeyDown
            if (wParam == VK_ESCAPE) {
                g_running = false;
                PostQuitMessage(0);
            }
            return 0;

        case WM_KEYUP:
            // TODO: Forward to InputCapture::OnKeyUp
            return 0;

        case WM_INPUT:
            // TODO: Process raw input for mouse
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
    CC_INFO("Connecting to host: %s:%u", args.hostAddr.c_str(), args.port);

    // Initialize Winsock
    if (!cc::transport::InitWinsock()) {
        CC_FATAL("Failed to initialize Winsock");
        return 1;
    }

    // Create window
    g_hwnd = CreateGameWindow(args.width, args.height, args.fullscreen);
    if (!g_hwnd) {
        CC_FATAL("Failed to create window");
        return 1;
    }

    CC_INFO("Window created: %ux%u, vsync=%s, fullscreen=%s",
            args.width, args.height,
            args.vsync ? "on" : "off",
            args.fullscreen ? "yes" : "no");

    // Create and initialize client session
    g_session = std::make_unique<cc::client::ClientSession>();
    cc::client::ClientSession::Config sessionConfig;
    sessionConfig.hostAddr      = args.hostAddr;
    sessionConfig.videoPort     = args.port;
    sessionConfig.windowWidth   = args.width;
    sessionConfig.windowHeight  = args.height;
    sessionConfig.vsync         = args.vsync;
    sessionConfig.hwnd          = g_hwnd;

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
