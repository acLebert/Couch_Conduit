// Couch Conduit — Dear ImGui overlay implementation
//
// Color theme derived from Hyprland/terminal dark-amber aesthetic:
//   Background:  #0D0D0D / #141414  (near-black charcoal)
//   Primary:     #E8A84C            (warm amber/gold — main text & accents)
//   Secondary:   #C47D2A            (darker amber — borders, inactive)
//   Accent:      #4EC9B0            (teal/cyan — highlights, links)
//   Dim text:    #7A7A6E            (muted gray — secondary info)
//   Error/warn:  #D46A6A / #D4A76A  (muted red / yellow)
//   Surface:     #1A1A1A            (panel backgrounds)

#include <couch_conduit/client/overlay.h>
#include <couch_conduit/common/log.h>

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

// Forward declare the Win32 message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace cc::client {

// ─── Color constants (linear float RGBA) ────────────────────────────────
namespace colors {
    static constexpr ImVec4 kBgDark       = {0.039f, 0.039f, 0.039f, 0.96f};  // #0A0A0A, nearly opaque
    static constexpr ImVec4 kBgPanel      = {0.082f, 0.082f, 0.082f, 0.97f};  // #151515
    static constexpr ImVec4 kBgPopup      = {0.090f, 0.078f, 0.059f, 0.98f};  // warm-tinted
    static constexpr ImVec4 kAmber        = {1.000f, 0.730f, 0.200f, 1.00f};  // #FFB933 — vivid amber
    static constexpr ImVec4 kAmberDim     = {0.900f, 0.560f, 0.120f, 1.00f};  // #E58F1F
    static constexpr ImVec4 kAmberDark    = {0.620f, 0.400f, 0.100f, 1.00f};  // #9E661A
    static constexpr ImVec4 kTeal         = {0.200f, 0.920f, 0.780f, 1.00f};  // #33EBC7 — vivid teal
    static constexpr ImVec4 kTealDim      = {0.150f, 0.700f, 0.580f, 1.00f};
    static constexpr ImVec4 kDimText      = {0.580f, 0.580f, 0.520f, 1.00f};  // #949485 — brighter gray
    static constexpr ImVec4 kWhite        = {0.960f, 0.940f, 0.900f, 1.00f};  // warm white
    static constexpr ImVec4 kRed          = {1.000f, 0.400f, 0.400f, 1.00f};  // #FF6666 — vivid red
    static constexpr ImVec4 kYellow       = {1.000f, 0.780f, 0.280f, 1.00f};  // #FFC747 — vivid yellow
    static constexpr ImVec4 kGreen        = {0.300f, 1.000f, 0.500f, 1.00f};  // #4DFF80 — vivid green
    static constexpr ImVec4 kTransparent  = {0.0f, 0.0f, 0.0f, 0.0f};
}

// ─── Lifecycle ──────────────────────────────────────────────────────────

bool Overlay::Init(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* ctx) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;  // Don't save layout to disk
    io.LogFilename = nullptr;

    // Initialize backends
    if (!ImGui_ImplWin32_Init(hwnd)) {
        CC_ERROR("ImGui Win32 backend init failed");
        return false;
    }
    if (!ImGui_ImplDX11_Init(device, ctx)) {
        CC_ERROR("ImGui DX11 backend init failed");
        ImGui_ImplWin32_Shutdown();
        return false;
    }

    ApplyAmberTheme();

    m_initialized = true;
    CC_INFO("ImGui overlay initialized (amber theme)");
    return true;
}

Overlay::~Overlay() {
    Shutdown();
}

void Overlay::Shutdown() {
    if (!m_initialized) return;
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    m_initialized = false;
}

// ─── Input ──────────────────────────────────────────────────────────────

bool Overlay::HandleInput(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (!m_initialized) return false;
    return ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam) != 0;
}

bool Overlay::WantCaptureMouse() const {
    if (!m_initialized) return false;
    return ImGui::GetIO().WantCaptureMouse;
}

bool Overlay::WantCaptureKeyboard() const {
    if (!m_initialized) return false;
    return ImGui::GetIO().WantCaptureKeyboard;
}

// ─── Stats ──────────────────────────────────────────────────────────────

void Overlay::UpdateStats(const OverlayStats& stats) {
    std::lock_guard lock(m_statsMutex);
    m_stats = stats;
}

// ─── Frame ──────────────────────────────────────────────────────────────

void Overlay::NewFrame() {
    if (!m_initialized) return;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void Overlay::Render() {
    if (!m_initialized) return;
    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

// ─── Draw ───────────────────────────────────────────────────────────────

static void TextColored(const ImVec4& col, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextV(fmt, args);
    ImGui::PopStyleColor();
    va_end(args);
}

void Overlay::Draw() {
    if (!m_initialized) return;

    // Snapshot stats
    OverlayStats stats;
    {
        std::lock_guard lock(m_statsMutex);
        stats = m_stats;
    }

    // ─── Compact Stats HUD (top-left) ──────────────────────────────────
    if (m_showStats) {
        ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.92f);

        ImGuiWindowFlags statsFlags =
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoMove;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 8));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, colors::kBgDark);
        ImGui::PushStyleColor(ImGuiCol_Border, colors::kAmberDark);

        if (ImGui::Begin("##stats_hud", nullptr, statsFlags)) {
            // Title bar
            ImGui::PushFont(nullptr);  // Use default font
            TextColored(colors::kAmber, "COUCH CONDUIT");
            ImGui::SameLine();
            TextColored(colors::kDimText, " v0.1.0");
            ImGui::Separator();

            // FPS — color code: green > 55, yellow > 30, red < 30
            ImVec4 fpsColor = colors::kGreen;
            if (stats.fps < 30.0f)      fpsColor = colors::kRed;
            else if (stats.fps < 55.0f) fpsColor = colors::kYellow;

            TextColored(colors::kDimText, "FPS");
            ImGui::SameLine(80);
            TextColored(fpsColor, "%.0f", stats.fps);

            // Decode time
            TextColored(colors::kDimText, "Decode");
            ImGui::SameLine(80);
            TextColored(colors::kAmber, "%.2f ms", stats.decodeTimeMs);

            // Render time
            TextColored(colors::kDimText, "Render");
            ImGui::SameLine(80);
            TextColored(colors::kAmber, "%.2f ms", stats.renderTimeMs);

            // Pipeline latency (recv → present)
            ImVec4 latColor = colors::kGreen;
            if (stats.recvToPresentMs > 10.0f)     latColor = colors::kRed;
            else if (stats.recvToPresentMs > 5.0f)  latColor = colors::kYellow;

            TextColored(colors::kDimText, "Latency");
            ImGui::SameLine(80);
            TextColored(latColor, "%.2f ms", stats.recvToPresentMs);

            // Loss
            if (stats.lossPercent > 0.01f) {
                ImVec4 lossColor = stats.lossPercent > 5.0f ? colors::kRed : colors::kYellow;
                TextColored(colors::kDimText, "Loss");
                ImGui::SameLine(80);
                TextColored(lossColor, "%.1f%%", stats.lossPercent);
            }

            // Connection info
            ImGui::Separator();
            TextColored(colors::kDimText, "Host");
            ImGui::SameLine(80);
            TextColored(colors::kTeal, "%s", stats.hostAddr.empty() ? "—" : stats.hostAddr.c_str());

            if (stats.encrypted) {
                ImGui::SameLine();
                TextColored(colors::kGreen, " [AES]");
            }

            if (!stats.codec.empty()) {
                TextColored(colors::kDimText, "Codec");
                ImGui::SameLine(80);
                TextColored(colors::kAmberDim, "%s", stats.codec.c_str());
                ImGui::SameLine();
                TextColored(colors::kDimText, "@ %.0f kbps", stats.bitrateKbps);
            }

            if (!stats.resolution.empty()) {
                TextColored(colors::kDimText, "Res");
                ImGui::SameLine(80);
                TextColored(colors::kAmberDim, "%s", stats.resolution.c_str());
            }

            ImGui::PopFont();
        }
        ImGui::End();

        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);
    }

    // ─── Full Settings Panel (F1 toggle) ───────────────────────────────
    if (m_showPanel) {
        ImGui::SetNextWindowPos(ImVec2(16, 300), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(380, 400), ImGuiCond_FirstUseEver);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 12));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);

        if (ImGui::Begin("Settings", &m_showPanel,
                         ImGuiWindowFlags_NoCollapse)) {

            // Connection section
            if (ImGui::CollapsingHeader("Connection", ImGuiTreeNodeFlags_DefaultOpen)) {
                TextColored(colors::kDimText, "Host:  ");
                ImGui::SameLine();
                TextColored(colors::kTeal, "%s", stats.hostAddr.c_str());

                TextColored(colors::kDimText, "Encrypted: ");
                ImGui::SameLine();
                TextColored(stats.encrypted ? colors::kGreen : colors::kRed,
                           "%s", stats.encrypted ? "Yes (AES-128-GCM)" : "No");
            }

            // Performance section
            if (ImGui::CollapsingHeader("Performance", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Text("FPS: %.1f", stats.fps);
                ImGui::Text("Decode: %.2f ms", stats.decodeTimeMs);
                ImGui::Text("Render: %.2f ms", stats.renderTimeMs);
                ImGui::Text("Pipeline: %.2f ms (min %.2f / max %.2f)",
                           stats.recvToPresentMs, stats.minPipelineMs, stats.maxPipelineMs);

                // Pipeline latency bar
                float normalized = stats.recvToPresentMs / 16.67f;  // Relative to 60fps frame
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, colors::kAmber);
                ImGui::ProgressBar(normalized > 1.0f ? 1.0f : normalized, ImVec2(-1, 0),
                                   "");
                ImGui::PopStyleColor();
                TextColored(colors::kDimText, "%.2f / 16.67 ms (1 frame @ 60fps)", stats.recvToPresentMs);
            }

            // Network section
            if (ImGui::CollapsingHeader("Network")) {
                ImGui::Text("Bitrate: %.0f kbps", stats.bitrateKbps);
                ImGui::Text("Packet Loss: %.1f%%", stats.lossPercent);
                ImGui::Text("FEC Ratio: %.0f%%", stats.fecRatio * 100.0f);
            }

            ImGui::Separator();

            // Disconnect button
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.18f, 0.18f, 0.90f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.25f, 0.25f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.55f, 0.12f, 0.12f, 1.00f));
            if (ImGui::Button("DISCONNECT", ImVec2(-1, 36))) {
                m_disconnectRequested.store(true);
            }
            ImGui::PopStyleColor(3);

            ImGui::Spacing();
            ImGui::Separator();
            TextColored(colors::kDimText, "F3 stats | F1 panel | F4 disconnect | ESC quit");
        }
        ImGui::End();

        ImGui::PopStyleVar(2);
    }

    // ─── Minimal hint when stats are hidden ────────────────────────────
    if (!m_showStats && !m_showPanel) {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->Size.x - 160, 8), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.4f);

        ImGuiWindowFlags hintFlags =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;

        ImGui::PushStyleColor(ImGuiCol_WindowBg, colors::kBgDark);
        if (ImGui::Begin("##hint", nullptr, hintFlags)) {
            TextColored(colors::kDimText, "F3 stats | F1 panel | F4 disconnect");
        }
        ImGui::End();
        ImGui::PopStyleColor();
    }
}

// ─── Theme ──────────────────────────────────────────────────────────────

void Overlay::ApplyAmberTheme() {
    ImGuiStyle& s = ImGui::GetStyle();

    // Rounding & spacing
    s.WindowRounding    = 6.0f;
    s.FrameRounding     = 4.0f;
    s.GrabRounding      = 3.0f;
    s.ScrollbarRounding = 4.0f;
    s.TabRounding       = 4.0f;
    s.PopupRounding     = 4.0f;
    s.ChildRounding     = 4.0f;

    s.WindowPadding     = ImVec2(12, 8);
    s.FramePadding      = ImVec2(8, 4);
    s.ItemSpacing       = ImVec2(8, 4);
    s.ItemInnerSpacing  = ImVec2(6, 4);
    s.WindowBorderSize  = 1.0f;
    s.FrameBorderSize   = 0.0f;
    s.PopupBorderSize   = 1.0f;
    s.ScrollbarSize     = 12.0f;
    s.GrabMinSize       = 8.0f;

    // Alpha
    s.Alpha             = 1.0f;
    s.DisabledAlpha     = 0.4f;

    // Colors
    ImVec4* c = s.Colors;

    // Backgrounds
    c[ImGuiCol_WindowBg]            = colors::kBgPanel;
    c[ImGuiCol_ChildBg]             = colors::kTransparent;
    c[ImGuiCol_PopupBg]             = colors::kBgPopup;
    c[ImGuiCol_MenuBarBg]           = {0.08f, 0.08f, 0.08f, 1.0f};

    // Borders
    c[ImGuiCol_Border]              = colors::kAmberDark;
    c[ImGuiCol_BorderShadow]        = colors::kTransparent;

    // Text
    c[ImGuiCol_Text]                = colors::kAmber;
    c[ImGuiCol_TextDisabled]        = colors::kDimText;
    c[ImGuiCol_TextSelectedBg]      = {0.910f, 0.659f, 0.298f, 0.30f};

    // Frames (input boxes, checkboxes)
    c[ImGuiCol_FrameBg]             = {0.12f, 0.10f, 0.08f, 1.0f};
    c[ImGuiCol_FrameBgHovered]      = {0.18f, 0.14f, 0.10f, 1.0f};
    c[ImGuiCol_FrameBgActive]       = {0.22f, 0.16f, 0.10f, 1.0f};

    // Title bar
    c[ImGuiCol_TitleBg]             = {0.06f, 0.06f, 0.06f, 1.0f};
    c[ImGuiCol_TitleBgActive]       = {0.10f, 0.08f, 0.06f, 1.0f};
    c[ImGuiCol_TitleBgCollapsed]    = {0.06f, 0.06f, 0.06f, 0.75f};

    // Buttons
    c[ImGuiCol_Button]              = colors::kAmberDark;
    c[ImGuiCol_ButtonHovered]       = colors::kAmberDim;
    c[ImGuiCol_ButtonActive]        = colors::kAmber;

    // Headers (collapsing headers, selectable)
    c[ImGuiCol_Header]              = {0.18f, 0.14f, 0.08f, 1.0f};
    c[ImGuiCol_HeaderHovered]       = {0.24f, 0.18f, 0.10f, 1.0f};
    c[ImGuiCol_HeaderActive]        = {0.30f, 0.22f, 0.12f, 1.0f};

    // Separator
    c[ImGuiCol_Separator]           = colors::kAmberDark;
    c[ImGuiCol_SeparatorHovered]    = colors::kAmberDim;
    c[ImGuiCol_SeparatorActive]     = colors::kAmber;

    // Scrollbar
    c[ImGuiCol_ScrollbarBg]         = {0.06f, 0.06f, 0.06f, 0.5f};
    c[ImGuiCol_ScrollbarGrab]       = colors::kAmberDark;
    c[ImGuiCol_ScrollbarGrabHovered]= colors::kAmberDim;
    c[ImGuiCol_ScrollbarGrabActive] = colors::kAmber;

    // Slider / grab
    c[ImGuiCol_SliderGrab]          = colors::kAmberDim;
    c[ImGuiCol_SliderGrabActive]    = colors::kAmber;

    // Check / radio
    c[ImGuiCol_CheckMark]           = colors::kTeal;

    // Tabs
    c[ImGuiCol_Tab]                 = {0.12f, 0.10f, 0.08f, 1.0f};
    c[ImGuiCol_TabHovered]          = colors::kAmberDim;
    c[ImGuiCol_TabSelected]         = colors::kAmberDark;
    c[ImGuiCol_TabSelectedOverline] = colors::kAmber;
    c[ImGuiCol_TabDimmed]           = {0.08f, 0.07f, 0.06f, 1.0f};
    c[ImGuiCol_TabDimmedSelected]   = {0.14f, 0.11f, 0.08f, 1.0f};

    // Resize grip
    c[ImGuiCol_ResizeGrip]          = {0.553f, 0.365f, 0.122f, 0.4f};
    c[ImGuiCol_ResizeGripHovered]   = {0.769f, 0.490f, 0.165f, 0.6f};
    c[ImGuiCol_ResizeGripActive]    = {0.910f, 0.659f, 0.298f, 0.9f};

    // Plot
    c[ImGuiCol_PlotLines]           = colors::kAmber;
    c[ImGuiCol_PlotLinesHovered]    = colors::kTeal;
    c[ImGuiCol_PlotHistogram]       = colors::kAmber;
    c[ImGuiCol_PlotHistogramHovered]= colors::kTeal;

    // Nav highlight
    c[ImGuiCol_NavHighlight]        = colors::kTeal;
    c[ImGuiCol_NavWindowingHighlight]= {1.0f, 1.0f, 1.0f, 0.7f};
    c[ImGuiCol_NavWindowingDimBg]   = {0.06f, 0.06f, 0.06f, 0.6f};

    // Modal dim
    c[ImGuiCol_ModalWindowDimBg]    = {0.0f, 0.0f, 0.0f, 0.65f};

    CC_INFO("Applied amber/dark overlay theme");
}

}  // namespace cc::client
