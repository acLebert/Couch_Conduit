#pragma once
// Couch Conduit — Dear ImGui overlay for client
//
// Renders a HUD overlay on top of the D3D11 swap chain.
// Color scheme: warm amber/orange on dark charcoal (inspired by rice/Hyprland
// terminal aesthetic — dark bg, amber text, teal accents).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d11.h>

#include <string>
#include <atomic>
#include <mutex>

namespace cc::client {

/// Overlay state shared between WndProc and the render thread.
struct OverlayStats {
    float   fps               = 0.0f;
    float   decodeTimeMs      = 0.0f;
    float   renderTimeMs      = 0.0f;
    float   recvToPresentMs   = 0.0f;
    float   minPipelineMs     = 0.0f;
    float   maxPipelineMs     = 0.0f;
    float   bitrateKbps       = 0.0f;
    float   lossPercent       = 0.0f;
    float   fecRatio          = 0.0f;
    int     clientCount       = 0;
    bool    encrypted         = false;
    std::string hostAddr;
    std::string codec;
    std::string resolution;
};

/// The ImGui overlay — Init once, then call NewFrame / Render each frame
/// from the render thread. WndProc calls must be forwarded via HandleInput.
class Overlay {
public:
    Overlay() = default;
    ~Overlay();

    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    /// Initialize ImGui + DX11/Win32 backends. Call once after D3D11 device creation.
    bool Init(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* ctx);

    /// Shutdown ImGui.
    void Shutdown();

    /// Forward window messages to ImGui. Call from WndProc.
    /// Returns true if ImGui consumed the event (e.g. typing in a text field).
    bool HandleInput(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    /// Begin a new overlay frame. Call at the start of each render iteration.
    void NewFrame();

    /// Draw the overlay widgets and issue ImGui draw commands.
    /// Call between NewFrame() and Render().
    void Draw();

    /// Issue the DX11 draw calls. Call after Draw(), before Present().
    void Render();

    /// Toggle the stats HUD overlay
    void ToggleStats()  { m_showStats = !m_showStats; }

    /// Toggle the full settings panel
    void TogglePanel()  { m_showPanel = !m_showPanel; }

    /// Check if overlay wants to capture mouse/keyboard
    bool WantCaptureMouse() const;
    bool WantCaptureKeyboard() const;

    /// Update stats (thread-safe — called from any thread)
    void UpdateStats(const OverlayStats& stats);

private:
    bool m_initialized = false;

    // Visibility
    bool m_showStats = true;   // Compact stats HUD (always on by default)
    bool m_showPanel = false;  // Full settings panel (toggled with F1)

    // Stats (protected by mutex)
    std::mutex   m_statsMutex;
    OverlayStats m_stats;

    // Theme setup
    void ApplyAmberTheme();
};

}  // namespace cc::client
