// Couch Conduit — DXGI Desktop Duplication capture implementation
//
// This is the first stage of the host pipeline:
//   DXGI Capture → NVENC Encode → Transport → Client
//
// Key design decisions:
// 1. Input-triggered capture via TriggerCapture() / m_triggerEvent
// 2. Falls back to VBlank-paced when no input arrives (idle desktop)
// 3. GPU texture never touches CPU — stays in VRAM for NVENC
// 4. Uses IDXGIOutput5::DuplicateOutput1() for better format support
// 5. GPU priority elevated to REALTIME via D3DKMTSetProcessSchedulingPriorityClass

#include <couch_conduit/host/capture.h>
#include <couch_conduit/common/log.h>
#include <couch_conduit/common/system_tuning.h>

#include <d3d11_4.h>
#include <dxgi1_6.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace cc::host {

DxgiCapture::~DxgiCapture() {
    Stop();
    if (m_triggerEvent) CloseHandle(m_triggerEvent);
    if (m_vblankEvent) CloseHandle(m_vblankEvent);
}

bool DxgiCapture::Init(const Config& config, FrameCapturedCallback callback) {
    m_config = config;
    m_callback = std::move(callback);

    // Create trigger event (auto-reset)
    m_triggerEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_triggerEvent) {
        CC_ERROR("Failed to create trigger event");
        return false;
    }

    // Create D3D11 device
    if (!CreateD3DDevice(config.adapterIndex)) {
        return false;
    }

    // Initialize desktop duplication
    if (!InitDuplication(config.outputIndex)) {
        return false;
    }

    CC_INFO("DXGI capture initialized: %ux%u on adapter %u, output %u",
            m_width, m_height, config.adapterIndex, config.outputIndex);
    return true;
}

bool DxgiCapture::CreateD3DDevice(uint32_t adapterIndex) {
    // Enumerate adapters
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        CC_ERROR("CreateDXGIFactory1 failed: 0x%08X", hr);
        return false;
    }

    ComPtr<IDXGIAdapter1> adapter;
    hr = factory->EnumAdapters1(adapterIndex, &adapter);
    if (FAILED(hr)) {
        CC_ERROR("EnumAdapters1(%u) failed: 0x%08X", adapterIndex, hr);
        return false;
    }

    DXGI_ADAPTER_DESC1 adapterDesc;
    adapter->GetDesc1(&adapterDesc);
    CC_INFO("Using adapter: %ls (VRAM: %zu MB)",
            adapterDesc.Description, adapterDesc.DedicatedVideoMemory / (1024 * 1024));

    // Create D3D11 device with video support
    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL actualLevel;
    hr = D3D11CreateDevice(
        adapter.Get(),
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        flags,
        featureLevels,
        _countof(featureLevels),
        D3D11_SDK_VERSION,
        &m_device,
        &actualLevel,
        &m_context
    );

    if (FAILED(hr)) {
        CC_ERROR("D3D11CreateDevice failed: 0x%08X", hr);
        return false;
    }

    CC_INFO("D3D11 device created (feature level %d.%d)",
            (actualLevel >> 12) & 0xF, (actualLevel >> 8) & 0xF);

    // Enable multithread protection (NVENC will access from another thread)
    ComPtr<ID3D10Multithread> multithread;
    hr = m_device.As(&multithread);
    if (SUCCEEDED(hr)) {
        multithread->SetMultithreadProtected(TRUE);
    }

    return true;
}

bool DxgiCapture::InitDuplication(uint32_t outputIndex) {
    // Get the DXGI device
    ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT hr = m_device.As(&dxgiDevice);
    if (FAILED(hr)) {
        CC_ERROR("Failed to get IDXGIDevice: 0x%08X", hr);
        return false;
    }

    // Get adapter from device
    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr)) {
        CC_ERROR("GetAdapter failed: 0x%08X", hr);
        return false;
    }

    // Get output
    ComPtr<IDXGIOutput> output;
    hr = adapter->EnumOutputs(outputIndex, &output);
    if (FAILED(hr)) {
        CC_ERROR("EnumOutputs(%u) failed: 0x%08X", outputIndex, hr);
        return false;
    }

    DXGI_OUTPUT_DESC outputDesc;
    output->GetDesc(&outputDesc);
    m_width = outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left;
    m_height = outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;

    // Try IDXGIOutput5::DuplicateOutput1() first (better format support)
    ComPtr<IDXGIOutput5> output5;
    hr = output.As(&output5);
    if (SUCCEEDED(hr)) {
        // Preferred formats (NVENC works best with NV12/B8G8R8A8)
        DXGI_FORMAT formats[] = {
            DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            DXGI_FORMAT_R10G10B10A2_UNORM,  // HDR
        };
        hr = output5->DuplicateOutput1(
            m_device.Get(),
            0,  // flags
            m_config.captureHdr ? 3u : 2u,
            formats,
            &m_duplication
        );
    }

    if (!m_duplication) {
        // Fallback to DuplicateOutput
        ComPtr<IDXGIOutput1> output1;
        hr = output.As(&output1);
        if (FAILED(hr)) {
            CC_ERROR("Failed to get IDXGIOutput1: 0x%08X", hr);
            return false;
        }
        hr = output1->DuplicateOutput(m_device.Get(), &m_duplication);
    }

    if (FAILED(hr)) {
        CC_ERROR("DuplicateOutput failed: 0x%08X (another app may have exclusive access)", hr);
        return false;
    }

    CC_INFO("Desktop duplication initialized: %ux%u", m_width, m_height);
    return true;
}

bool DxgiCapture::Start() {
    if (m_running) return false;

    m_running = true;
    m_frameNumber = 0;

    m_captureThread = std::thread([this]() {
        // HIGHEST priority + MMCSS "Pro Audio" for minimal scheduling latency
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        cc::sys::RegisterMmcssThread(L"Pro Audio");
        CaptureLoop();
    });

    CC_INFO("Capture thread started");
    return true;
}

void DxgiCapture::TriggerCapture() {
    if (m_triggerEvent) {
        SetEvent(m_triggerEvent);
    }
}

void DxgiCapture::CaptureLoop() {
    // Calculate fallback interval for when no input arrives
    const DWORD fallbackIntervalMs = (m_config.maxFps > 0) ? (1000 / m_config.maxFps) : 16;

    while (m_running) {
        // Wait for either:
        // 1. Input trigger (immediate capture)
        // 2. Timeout (fallback to periodic capture)
        DWORD waitResult = WaitForSingleObject(m_triggerEvent, fallbackIntervalMs);

        if (!m_running) break;

        // Acquire desktop frame
        ID3D11Texture2D* texture = nullptr;
        int64_t captureTimestamp = 0;

        if (AcquireFrame(&texture, &captureTimestamp)) {
            m_frameNumber++;

            // Deliver to encoder (stays in VRAM)
            if (m_callback) {
                m_callback(texture, m_frameNumber, captureTimestamp);
            }

            ReleaseFrame();
        }

        // Suppress unused variable warning
        (void)waitResult;
    }
}

bool DxgiCapture::AcquireFrame(ID3D11Texture2D** outTexture, int64_t* outTimestamp) {
    if (!m_duplication) {
        ReInitDuplication();
        if (!m_duplication) return false;
    }

    ComPtr<IDXGIResource> resource;
    DXGI_OUTDUPL_FRAME_INFO frameInfo;

    HRESULT hr = m_duplication->AcquireNextFrame(0, &frameInfo, &resource);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        // No new frame — desktop hasn't changed
        return false;
    }

    if (hr == DXGI_ERROR_ACCESS_LOST) {
        CC_WARN("Desktop duplication access lost — reinitializing");
        m_duplication.Reset();
        ReInitDuplication();
        return false;
    }

    if (FAILED(hr)) {
        CC_ERROR("AcquireNextFrame failed: 0x%08X", hr);
        return false;
    }

    *outTimestamp = cc::NowUsec();

    // Get the texture from the DXGI resource
    hr = resource.As(outTexture);
    if (FAILED(hr)) {
        CC_ERROR("Failed to get texture from DXGI resource: 0x%08X", hr);
        m_duplication->ReleaseFrame();
        return false;
    }

    return true;
}

void DxgiCapture::ReleaseFrame() {
    if (m_duplication) {
        m_duplication->ReleaseFrame();
    }
}

void DxgiCapture::ReInitDuplication() {
    CC_INFO("Reinitializing desktop duplication...");
    m_duplication.Reset();
    InitDuplication(m_config.outputIndex);
}

void DxgiCapture::Stop() {
    m_running = false;
    // Wake up the capture thread if it's waiting
    if (m_triggerEvent) {
        SetEvent(m_triggerEvent);
    }
    if (m_captureThread.joinable()) {
        m_captureThread.join();
    }
    m_duplication.Reset();
    CC_INFO("Capture stopped");
}

}  // namespace cc::host
