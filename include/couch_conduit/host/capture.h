#pragma once
// Couch Conduit — DXGI Desktop Duplication capture

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d11_4.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

#include <atomic>
#include <thread>
#include <functional>
#include <mutex>

#include <couch_conduit/common/types.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace cc::host {

using Microsoft::WRL::ComPtr;

/// Callback when a new frame is captured.
/// The texture is a GPU-resident D3D11 texture in VRAM — do NOT read to CPU.
/// It is valid only for the duration of the callback.
using FrameCapturedCallback = std::function<void(
    ID3D11Texture2D* texture,
    uint32_t         frameNumber,
    int64_t          captureTimestampUs
)>;

/// DXGI Desktop Duplication based screen capture.
///
/// Key design decisions over Sunshine:
/// - Input-triggered capture: external code signals via TriggerCapture()
/// - Falls back to VBlank-paced capture when no input arrives
/// - GPU texture stays in VRAM (zero CPU copy)
/// - Uses IDXGIOutput5::DuplicateOutput1() for better format support
class DxgiCapture {
public:
    DxgiCapture() = default;
    ~DxgiCapture();

    DxgiCapture(const DxgiCapture&) = delete;
    DxgiCapture& operator=(const DxgiCapture&) = delete;

    struct Config {
        uint32_t outputIndex     = 0;       // Monitor index
        uint32_t adapterIndex    = 0;       // GPU index
        uint32_t maxFps          = 60;      // Cap for timer-based fallback
        bool     captureHdr      = false;   // Request HDR format
    };

    /// Initialize capture on the specified display output.
    bool Init(const Config& config, FrameCapturedCallback callback);

    /// Start the capture thread
    bool Start();

    /// Signal the capture thread to grab a frame NOW (input-triggered)
    void TriggerCapture();

    /// Stop the capture thread
    void Stop();

    /// Get the D3D11 device used for capture (encoder needs to share this)
    ID3D11Device* GetDevice() const { return m_device.Get(); }

    /// Get the output dimensions
    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }

private:
    ComPtr<ID3D11Device>           m_device;
    ComPtr<ID3D11DeviceContext>    m_context;
    ComPtr<IDXGIOutputDuplication> m_duplication;
    ComPtr<ID3D11Texture2D>        m_stagingTexture;  // For format conversion if needed

    FrameCapturedCallback m_callback;
    Config    m_config;
    uint32_t  m_width  = 0;
    uint32_t  m_height = 0;

    std::thread       m_captureThread;
    std::atomic<bool> m_running{false};
    HANDLE            m_triggerEvent = nullptr;   // Signaled by TriggerCapture()
    HANDLE            m_vblankEvent  = nullptr;   // VBlank fallback timer

    uint32_t m_frameNumber = 0;

    bool CreateD3DDevice(uint32_t adapterIndex);
    bool InitDuplication(uint32_t outputIndex);
    void CaptureLoop();
    bool AcquireFrame(ID3D11Texture2D** outTexture, int64_t* outTimestamp);
    void ReleaseFrame();
    void ReInitDuplication();
};

}  // namespace cc::host
