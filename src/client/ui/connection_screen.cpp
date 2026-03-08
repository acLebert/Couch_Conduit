// Couch Conduit — Connection Screen (D3D11 + Dear ImGui)
//
// Self-contained mini-renderer that shows the "Join Game" UI.
// Creates its own D3D11 device/swap chain and ImGui context,
// tears them down cleanly before returning so the real Renderer
// can start fresh on the same HWND.

#include <couch_conduit/client/connection_screen.h>
#include <couch_conduit/common/signaling.h>
#include <couch_conduit/common/log.h>

#include <d3d11.h>
#include <dxgi.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <string>
#include <cstring>
#include <algorithm>
#include <atomic>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// ImGui Win32 backend handler (declared in imgui_impl_win32.h but we
// need the extern decl to call it from our temporary WndProc).
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

// ─── State shared between WndProc and render loop ──────────────────────
static std::atomic<bool> s_quit{false};
static WNDPROC           s_origWndProc = nullptr;

static LRESULT CALLBACK ConnectionWndProc(HWND hwnd, UINT msg,
                                          WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return 0;

    switch (msg) {
        case WM_CLOSE:
        case WM_DESTROY:
            s_quit = true;
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ─── Amber / dark-charcoal theme ────────────────────────────────────────
void ApplyConnectionTheme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding   = 8.0f;
    s.FrameRounding    = 5.0f;
    s.GrabRounding     = 5.0f;
    s.WindowPadding    = ImVec2(20, 20);
    s.FramePadding     = ImVec2(12, 7);
    s.ItemSpacing      = ImVec2(10, 12);
    s.WindowBorderSize = 1.0f;
    s.FrameBorderSize  = 0.0f;

    auto* c = s.Colors;
    // Base
    c[ImGuiCol_WindowBg]        = ImVec4(0.045f, 0.045f, 0.040f, 0.97f);
    c[ImGuiCol_ChildBg]         = ImVec4(0.055f, 0.055f, 0.050f, 0.60f);
    c[ImGuiCol_PopupBg]         = ImVec4(0.065f, 0.065f, 0.058f, 0.96f);
    c[ImGuiCol_Border]          = ImVec4(1.00f, 0.725f, 0.20f, 0.22f);
    // Text
    c[ImGuiCol_Text]            = ImVec4(0.93f, 0.89f, 0.79f, 1.00f);
    c[ImGuiCol_TextDisabled]    = ImVec4(0.55f, 0.53f, 0.48f, 1.00f);
    // Inputs
    c[ImGuiCol_FrameBg]         = ImVec4(0.10f, 0.10f, 0.08f, 0.90f);
    c[ImGuiCol_FrameBgHovered]  = ImVec4(0.16f, 0.15f, 0.11f, 1.00f);
    c[ImGuiCol_FrameBgActive]   = ImVec4(0.22f, 0.20f, 0.14f, 1.00f);
    // Title bar
    c[ImGuiCol_TitleBg]         = ImVec4(0.06f, 0.06f, 0.05f, 1.00f);
    c[ImGuiCol_TitleBgActive]   = ImVec4(0.12f, 0.11f, 0.08f, 1.00f);
    // Buttons — vivid amber
    c[ImGuiCol_Button]          = ImVec4(1.00f, 0.725f, 0.20f, 0.80f);
    c[ImGuiCol_ButtonHovered]   = ImVec4(1.00f, 0.780f, 0.35f, 1.00f);
    c[ImGuiCol_ButtonActive]    = ImVec4(0.85f, 0.60f, 0.15f, 1.00f);
    // Misc
    c[ImGuiCol_Separator]       = ImVec4(1.00f, 0.725f, 0.20f, 0.18f);
    c[ImGuiCol_CheckMark]       = ImVec4(1.00f, 0.725f, 0.20f, 1.00f);
    c[ImGuiCol_SliderGrab]      = ImVec4(1.00f, 0.725f, 0.20f, 0.80f);
    c[ImGuiCol_Header]          = ImVec4(1.00f, 0.725f, 0.20f, 0.15f);
    c[ImGuiCol_HeaderHovered]   = ImVec4(1.00f, 0.725f, 0.20f, 0.30f);
    c[ImGuiCol_HeaderActive]    = ImVec4(1.00f, 0.725f, 0.20f, 0.45f);
    c[ImGuiCol_NavHighlight]    = ImVec4(1.00f, 0.725f, 0.20f, 0.80f);
}

}  // anonymous namespace

// ════════════════════════════════════════════════════════════════════════

namespace cc::client {

ConnectionResult ShowConnectionScreen(HWND hwnd,
                                      uint32_t width, uint32_t height,
                                      const std::string& signalingUrl) {
    ConnectionResult result;
    s_quit = false;

    // Get actual client area (accounts for DPI scaling)
    RECT clientRect;
    GetClientRect(hwnd, &clientRect);
    uint32_t actualWidth  = static_cast<uint32_t>(clientRect.right  - clientRect.left);
    uint32_t actualHeight = static_cast<uint32_t>(clientRect.bottom - clientRect.top);
    if (actualWidth  == 0) actualWidth  = width;
    if (actualHeight == 0) actualHeight = height;

    // ── D3D11 device + swap chain ──────────────────────────────────────
    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount                  = 1;
    scd.BufferDesc.Width             = actualWidth;
    scd.BufferDesc.Height            = actualHeight;
    scd.BufferDesc.Format            = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.RefreshRate       = {60, 1};
    scd.BufferUsage                  = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow                 = hwnd;
    scd.SampleDesc.Count             = 1;
    scd.Windowed                     = TRUE;
    scd.SwapEffect                   = DXGI_SWAP_EFFECT_DISCARD;

    ID3D11Device*        device    = nullptr;
    ID3D11DeviceContext*  ctx      = nullptr;
    IDXGISwapChain*       swapChain = nullptr;
    D3D_FEATURE_LEVEL     featureLevel;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        &scd, &swapChain, &device, &featureLevel, &ctx);

    if (FAILED(hr)) {
        CC_ERROR("Connection screen: D3D11 init failed (0x%08X)", hr);
        result.cancelled = true;
        return result;
    }

    // Render target
    ID3D11Texture2D*        backBuf = nullptr;
    ID3D11RenderTargetView* rtv     = nullptr;
    swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuf));
    device->CreateRenderTargetView(backBuf, nullptr, &rtv);
    backBuf->Release();

    // ── ImGui init ─────────────────────────────────────────────────────
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename  = nullptr;              // Don't save .ini layout

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(device, ctx);
    ApplyConnectionTheme();

    // ── Subclass the window so ImGui receives input ────────────────────
    s_origWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(ConnectionWndProc)));

    // ── UI state ───────────────────────────────────────────────────────
    char roomCode[16]   = "";
    char directIp[64]   = "";
    char directPort[8]  = "47100";

    std::string statusText = "Enter a room code or host IP to connect.";
    bool statusError       = false;
    bool connecting        = false;
    bool roomCodesEnabled  = !signalingUrl.empty();

    // ── Render loop ────────────────────────────────────────────────────
    while (!s_quit) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { s_quit = true; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (s_quit && !result.connected) {
            result.cancelled = true;
            break;
        }
        if (s_quit) break;

        // ── New ImGui frame ────────────────────────────────────────────
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // ── Centered panel ─────────────────────────────────────────────
        // Use ImGui's actual display size (DPI-aware) rather than the
        // logical width/height passed in, so hit-testing matches visuals.
        const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
        const ImVec2 panelSize(460.0f, roomCodesEnabled ? 510.0f : 370.0f);
        const ImVec2 panelPos(displaySize.x * 0.5f - panelSize.x * 0.5f,
                              displaySize.y * 0.5f - panelSize.y * 0.5f);

        ImGui::SetNextWindowPos(panelPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(panelSize, ImGuiCond_Always);
        ImGui::Begin("##connect", nullptr,
                     ImGuiWindowFlags_NoResize   | ImGuiWindowFlags_NoMove     |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoScrollbar);

        // ── Title ──────────────────────────────────────────────────────
        {
            // Version string
            const char* ver = "v0.1.0";
            const char* title = "COUCH CONDUIT";
            const char* subtitle = "Ultra-Low-Latency Game Streaming";

            float titleW = ImGui::CalcTextSize(title).x;
            ImGui::SetCursorPosX((panelSize.x - titleW) * 0.5f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.725f, 0.20f, 1.0f));
            ImGui::Text("%s", title);
            ImGui::PopStyleColor();

            // Version on same line to the right
            ImGui::SameLine(panelSize.x - ImGui::CalcTextSize(ver).x - 28);
            ImGui::TextDisabled("%s", ver);

            float subW = ImGui::CalcTextSize(subtitle).x;
            ImGui::SetCursorPosX((panelSize.x - subW) * 0.5f);
            ImGui::TextDisabled("%s", subtitle);
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::Spacing();

        // ── Room Code section ──────────────────────────────────────────
        if (roomCodesEnabled) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.725f, 0.20f, 1.0f));
            ImGui::Text("JOIN WITH ROOM CODE");
            ImGui::PopStyleColor();
            ImGui::Spacing();

            ImGui::Text("Code:");
            ImGui::SameLine();
            ImGui::PushItemWidth(200);
            ImGuiInputTextFlags codeFlags =
                ImGuiInputTextFlags_CharsUppercase |
                ImGuiInputTextFlags_CharsNoBlank;
            bool codeEnter = ImGui::InputText("##roomcode", roomCode, sizeof(roomCode),
                                              codeFlags | ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::PopItemWidth();
            ImGui::SameLine();

            bool joinClicked = ImGui::Button("JOIN", ImVec2(80, 0));

            if ((joinClicked || codeEnter) && !connecting && strlen(roomCode) >= 4) {
                connecting  = true;
                statusText  = "Looking up room " + std::string(roomCode) + " ...";
                statusError = false;

                // Resolve (blocks briefly — <200ms typical)
                std::string hostIp;
                uint16_t    hostPort = 0;
                if (cc::net::SignalingClient::ResolveRoom(
                        signalingUrl, roomCode, hostIp, hostPort)) {
                    result.connected   = true;
                    result.hostAddr    = hostIp;
                    result.controlPort = hostPort;
                    s_quit = true;
                } else {
                    statusText  = "Room not found — check the code and try again.";
                    statusError = true;
                    connecting  = false;
                }
            }

            ImGui::Spacing();
            ImGui::Spacing();

            // "or" divider
            {
                float textW = ImGui::CalcTextSize("or connect directly").x;
                ImGui::SetCursorPosX((panelSize.x - textW) * 0.5f);
                ImGui::TextDisabled("or connect directly");
            }

            ImGui::Spacing();
            ImGui::Spacing();
        }

        // ── Direct IP section ──────────────────────────────────────────
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.725f, 0.20f, 1.0f));
        ImGui::Text("DIRECT CONNECTION");
        ImGui::PopStyleColor();
        ImGui::Spacing();

        ImGui::Text("Host IP:");
        ImGui::PushItemWidth(-1);
        bool ipEnter = ImGui::InputText("##ip", directIp, sizeof(directIp),
                                        ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopItemWidth();

        ImGui::Text("Port:");
        ImGui::PushItemWidth(140);
        ImGui::InputText("##port", directPort, sizeof(directPort),
                         ImGuiInputTextFlags_CharsDecimal);
        ImGui::PopItemWidth();

        ImGui::Spacing();
        ImGui::Spacing();

        bool connectClicked = ImGui::Button("CONNECT", ImVec2(-1, 40));

        if ((connectClicked || ipEnter) && !connecting && strlen(directIp) > 0) {
            result.connected   = true;
            result.hostAddr    = directIp;
            result.controlPort = static_cast<uint16_t>(atoi(directPort));
            if (result.controlPort == 0) result.controlPort = 47100;
            s_quit = true;
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ── Status line ────────────────────────────────────────────────
        if (statusError)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.40f, 0.40f, 1.0f));
        ImGui::TextWrapped("%s", statusText.c_str());
        if (statusError)
            ImGui::PopStyleColor();

        ImGui::End();

        // ── Present ────────────────────────────────────────────────────
        ImGui::Render();
        const float clear[4] = {0.025f, 0.025f, 0.022f, 1.0f};
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->ClearRenderTargetView(rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        swapChain->Present(1, 0);   // VSync to save GPU
    }

    // ── Cleanup ────────────────────────────────────────────────────────
    if (s_origWndProc) {
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(s_origWndProc));
        s_origWndProc = nullptr;
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    if (rtv)       rtv->Release();
    if (swapChain) swapChain->Release();
    if (ctx)       ctx->Release();
    if (device)    device->Release();

    return result;
}

}  // namespace cc::client
