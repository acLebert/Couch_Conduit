#pragma once
// Couch Conduit — D3D11 low-latency renderer (Client side)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d11_4.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

#include <cstdint>
#include <atomic>
#include <thread>
#include <mutex>
#include <functional>

#include <couch_conduit/common/types.h>
#include <couch_conduit/client/overlay.h>

// FFmpeg forward declaration
extern "C" {
struct AVFrame;
}

namespace cc::client {

using Microsoft::WRL::ComPtr;

/// D3D11 renderer with zero-latency present.
///
/// Key design:
/// - DXGI_SWAP_EFFECT_FLIP_DISCARD with SyncInterval=0
/// - DXGI_PRESENT_ALLOW_TEARING when available
/// - Does NOT call SetMaximumFrameLatency(1) — this counter-intuitively helps
/// - Uses D3DKMTWaitForVerticalBlankEvent for VSync alignment
/// - GPU timestamp queries for precise frame timing
/// - Deferred frame free to prevent GPU stalls
class Renderer {
public:
    Renderer() = default;
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    struct Config {
        HWND     hwnd           = nullptr;
        uint32_t width          = 1920;
        uint32_t height         = 1080;
        bool     vsync          = false;  // Recommend OFF for lowest latency
        bool     fullscreen     = false;
    };

    /// Initialize the renderer and create the swap chain
    bool Init(const Config& config);

    /// Get the D3D11 device (shared with decoder for zero-copy)
    ID3D11Device* GetDevice() const { return m_device.Get(); }

    /// Submit a decoded frame for rendering
    /// The AVFrame must contain a D3D11 texture (hw_frames_ctx)
    void SubmitFrame(AVFrame* frame, const FrameMetadata& meta);

    /// Start the render thread
    bool Start();

    /// Stop the render thread
    void Stop();

    /// Resize the swap chain (window size changed)
    void Resize(uint32_t width, uint32_t height);

    /// Get current render stats
    StreamStats GetStats() const;

    /// Get the overlay (for forwarding WndProc input and toggling)
    Overlay* GetOverlay() { return &m_overlay; }

    /// Update overlay stats (thread-safe)
    void UpdateOverlayStats(const OverlayStats& stats) { m_overlay.UpdateStats(stats); }

private:
    ComPtr<ID3D11Device>           m_device;
    ComPtr<ID3D11DeviceContext>    m_context;
    ComPtr<IDXGISwapChain1>       m_swapChain;

    // Shader resources for NV12/P010 → RGBA conversion
    ComPtr<ID3D11VertexShader>    m_vertexShader;
    ComPtr<ID3D11PixelShader>     m_pixelShader;
    ComPtr<ID3D11SamplerState>    m_sampler;
    ComPtr<ID3D11RenderTargetView> m_rtv;
    ComPtr<ID3D11ShaderResourceView> m_srvY;
    ComPtr<ID3D11ShaderResourceView> m_srvUV;

    // NV12 staging texture with BIND_SHADER_RESOURCE for decoded frame copy
    ComPtr<ID3D11Texture2D>          m_nv12Staging;
    uint32_t                         m_nv12StagingW = 0;
    uint32_t                         m_nv12StagingH = 0;

    Config m_config;
    bool   m_tearingSupported = false;

    // Render thread
    std::thread       m_renderThread;
    std::atomic<bool> m_running{false};
    HANDLE            m_frameSubmittedEvent = nullptr;

    // Frame queue (single slot — always present the latest)
    struct PendingRender {
        AVFrame*      frame = nullptr;
        FrameMetadata meta;
        bool          ready = false;
    };
    PendingRender      m_pending;
    CRITICAL_SECTION   m_pendingLock;
    AVFrame*           m_deferredFreeFrame = nullptr;  // Deferred free

    // VSync via D3DKMT
    HANDLE m_vblankEvent = nullptr;

    // GPU timestamp queries
    ComPtr<ID3D11Query> m_tsStart;
    ComPtr<ID3D11Query> m_tsEnd;
    ComPtr<ID3D11Query> m_tsDisjoint;

    // Stats
    std::atomic<float> m_lastRenderTimeMs{0};
    std::atomic<uint32_t> m_renderedFps{0};

    Overlay m_overlay;

    bool CreateDevice();
    bool CreateSwapChain(HWND hwnd);
    bool CreateShaders();
    bool CheckTearingSupport();
    void RenderLoop();
    void RenderFrame(AVFrame* frame);
    void Present();
    void WaitForVBlank();
};

}  // namespace cc::client
