// Couch Conduit — D3D11 low-latency renderer implementation
//
// Key techniques:
// - FLIP_DISCARD swap chain with SyncInterval=0 
// - DXGI_PRESENT_ALLOW_TEARING for zero-wait Present()
// - Does NOT call SetMaximumFrameLatency(1) — this is counterintuitive
//   but AVOIDS blocking Present() calls (learned from Moonlight)
// - Uses D3DKMTWaitForVerticalBlankEvent for VSync alignment
// - Deferred frame free prevents GPU stalls
// - GPU timestamp queries for precise timing

#include <couch_conduit/client/renderer.h>
#include <couch_conduit/common/log.h>
#include <couch_conduit/common/system_tuning.h>

#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <dwmapi.h>

#ifdef HAS_FFMPEG
extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}
#endif

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

// NV12 → RGBA pixel shader (HLSL)
static const char* kPixelShaderHlsl = R"(
Texture2D<float>  texY  : register(t0);
Texture2D<float2> texUV : register(t1);
SamplerState      samp  : register(s0);

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(PS_INPUT input) : SV_TARGET {
    float  y  = texY.Sample(samp, input.uv);
    float2 uv = texUV.Sample(samp, input.uv);

    // BT.709 YUV → RGB conversion
    float r = y + 1.5748 * (uv.x - 0.5);
    float g = y - 0.1873 * (uv.y - 0.5) - 0.4681 * (uv.x - 0.5);
    float b = y + 1.8556 * (uv.y - 0.5);

    return float4(saturate(r), saturate(g), saturate(b), 1.0);
}
)";

static const char* kVertexShaderHlsl = R"(
struct VS_OUTPUT {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VS_OUTPUT main(uint id : SV_VertexID) {
    VS_OUTPUT output;
    // Full-screen triangle (3 vertices, no vertex buffer needed)
    output.uv = float2((id << 1) & 2, id & 2);
    output.pos = float4(output.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return output;
}
)";

namespace cc::client {

Renderer::~Renderer() {
    Stop();
    if (m_frameSubmittedEvent) CloseHandle(m_frameSubmittedEvent);
    DeleteCriticalSection(&m_pendingLock);
}

bool Renderer::Init(const Config& config) {
    m_config = config;

    InitializeCriticalSectionAndSpinCount(&m_pendingLock, 4000);
    m_frameSubmittedEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if (!CreateDevice()) return false;
    if (!CheckTearingSupport()) {
        CC_WARN("Tearing not supported — V-Sync off mode will have extra latency");
    }
    if (!CreateSwapChain(config.hwnd)) return false;
    if (!CreateShaders()) return false;

    CC_INFO("Renderer initialized: %ux%u, vsync=%s, tearing=%s",
            config.width, config.height,
            config.vsync ? "on" : "off",
            m_tearingSupported ? "supported" : "not supported");
    return true;
}

bool Renderer::CreateDevice() {
    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL actualLevel;
    HRESULT hr = D3D11CreateDevice(
        nullptr,  // Default adapter
        D3D_DRIVER_TYPE_HARDWARE,
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

    // IMPORTANT: Do NOT call SetMaximumFrameLatency(1) on the device!
    // This counterintuitively INCREASES latency because it makes
    // SyncInterval=0 Present() calls block on DWM (acting like SyncInterval=1).
    // Learned from Moonlight's d3d11va.cpp.

    // Enable multithread protection
    ComPtr<ID3D10Multithread> mt;
    hr = m_device.As(&mt);
    if (SUCCEEDED(hr)) mt->SetMultithreadProtected(TRUE);

    // Enable MMCSS for DWM
    DwmEnableMMCSS(TRUE);

    CC_INFO("Render D3D11 device created");
    return true;
}

bool Renderer::CheckTearingSupport() {
    ComPtr<IDXGIFactory5> factory5;

    ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT hr = m_device.As(&dxgiDevice);
    if (FAILED(hr)) return false;

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr)) return false;

    ComPtr<IDXGIFactory4> factory4;
    hr = adapter->GetParent(IID_PPV_ARGS(&factory4));
    if (FAILED(hr)) return false;

    hr = factory4.As(&factory5);
    if (FAILED(hr)) return false;

    BOOL allowTearing = FALSE;
    hr = factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                       &allowTearing, sizeof(allowTearing));
    m_tearingSupported = SUCCEEDED(hr) && allowTearing;
    return m_tearingSupported;
}

bool Renderer::CreateSwapChain(HWND hwnd) {
    ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT hr = m_device.As(&dxgiDevice);
    if (FAILED(hr)) return false;

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr)) return false;

    ComPtr<IDXGIFactory2> factory;
    hr = adapter->GetParent(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return false;

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = m_config.width;
    desc.Height = m_config.height;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;

    // 3 front buffers + 1 back + 1 for DWM DirectFlip = 5 total
    // The extra buffers don't increase latency with SyncInterval=0,
    // but they allow DWM DirectFlip (zero-copy to display) when the
    // swap chain matches the monitor resolution.
    desc.BufferCount = 5;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Flags = m_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;

    hr = factory->CreateSwapChainForHwnd(m_device.Get(), hwnd, &desc,
                                          nullptr, nullptr, &m_swapChain);
    if (FAILED(hr)) {
        CC_ERROR("CreateSwapChainForHwnd failed: 0x%08X", hr);
        return false;
    }

    // Create render target view
    ComPtr<ID3D11Texture2D> backBuffer;
    hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) return false;

    hr = m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_rtv);
    if (FAILED(hr)) return false;

    CC_INFO("Swap chain created: %ux%u, %u buffers, tearing=%s",
            desc.Width, desc.Height, desc.BufferCount,
            m_tearingSupported ? "enabled" : "disabled");
    return true;
}

bool Renderer::CreateShaders() {
    // Compile vertex shader
    ComPtr<ID3DBlob> vsBlob, vsErrors;
    HRESULT hr = D3DCompile(kVertexShaderHlsl, strlen(kVertexShaderHlsl),
                            "vs_main", nullptr, nullptr, "main", "vs_5_0",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                            &vsBlob, &vsErrors);
    if (FAILED(hr)) {
        CC_ERROR("Vertex shader compile failed: %s",
                 vsErrors ? static_cast<char*>(vsErrors->GetBufferPointer()) : "unknown");
        return false;
    }

    hr = m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                       nullptr, &m_vertexShader);
    if (FAILED(hr)) return false;

    // Compile pixel shader
    ComPtr<ID3DBlob> psBlob, psErrors;
    hr = D3DCompile(kPixelShaderHlsl, strlen(kPixelShaderHlsl),
                    "ps_main", nullptr, nullptr, "main", "ps_5_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                    &psBlob, &psErrors);
    if (FAILED(hr)) {
        CC_ERROR("Pixel shader compile failed: %s",
                 psErrors ? static_cast<char*>(psErrors->GetBufferPointer()) : "unknown");
        return false;
    }

    hr = m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
                                      nullptr, &m_pixelShader);
    if (FAILED(hr)) return false;

    // Create sampler (bilinear filtering for UV plane upsampling)
    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    hr = m_device->CreateSamplerState(&samplerDesc, &m_sampler);
    if (FAILED(hr)) return false;

    // Create GPU timestamp queries for precise render timing
    D3D11_QUERY_DESC queryDesc = {};
    queryDesc.Query = D3D11_QUERY_TIMESTAMP;
    m_device->CreateQuery(&queryDesc, &m_tsStart);
    m_device->CreateQuery(&queryDesc, &m_tsEnd);

    queryDesc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    m_device->CreateQuery(&queryDesc, &m_tsDisjoint);

    CC_INFO("Shaders compiled and GPU timestamp queries created");
    return true;
}

void Renderer::SubmitFrame(AVFrame* frame, const FrameMetadata& meta) {
    EnterCriticalSection(&m_pendingLock);
    // If there was an old pending frame we never rendered, free it
    // (deferred to prevent GPU stalls)
    if (m_pending.ready && m_pending.frame) {
        // Swap with deferred free slot
        if (m_deferredFreeFrame) {
#ifdef HAS_FFMPEG
            av_frame_free(&m_deferredFreeFrame);
#endif
        }
        m_deferredFreeFrame = m_pending.frame;
    }
    m_pending.frame = frame;
    m_pending.meta = meta;
    m_pending.ready = true;
    LeaveCriticalSection(&m_pendingLock);

    SetEvent(m_frameSubmittedEvent);
}

bool Renderer::Start() {
    m_running = true;
    m_renderThread = std::thread([this]() {
        // TIME_CRITICAL + MMCSS for minimum jitter
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        cc::sys::RegisterMmcssThread(L"Pro Audio");
        RenderLoop();
    });

    CC_INFO("Render thread started (TIME_CRITICAL + MMCSS Pro Audio)");
    return true;
}

void Renderer::RenderLoop() {
    while (m_running) {
        // Wait for a frame to be submitted
        DWORD result = WaitForSingleObject(m_frameSubmittedEvent, 100);
        if (!m_running) break;
        if (result == WAIT_TIMEOUT) continue;

        // Grab the latest frame
        AVFrame* frame = nullptr;
        FrameMetadata meta;

        EnterCriticalSection(&m_pendingLock);
        if (m_pending.ready) {
            frame = m_pending.frame;
            meta = m_pending.meta;
            m_pending.frame = nullptr;
            m_pending.ready = false;
        }
        LeaveCriticalSection(&m_pendingLock);

        if (!frame && meta.frameNumber == 0) continue;  // Spurious wake

        // Begin GPU timestamp
        if (m_tsDisjoint) {
            m_context->Begin(m_tsDisjoint.Get());
            m_context->End(m_tsStart.Get());
        }

        // Render frame to swap chain
        if (frame) {
            RenderFrame(frame);
        }

        // Present with zero latency
        Present();

        // End GPU timestamp
        if (m_tsDisjoint) {
            m_context->End(m_tsEnd.Get());
            m_context->End(m_tsDisjoint.Get());

            // Read back GPU timestamps (from previous frame to avoid stall)
            // TODO: Implement double-buffered query readback
        }

        meta.renderTimeUs = cc::NowUsec();

        // Deferred free of previous frame
        if (m_deferredFreeFrame) {
#ifdef HAS_FFMPEG
            av_frame_free(&m_deferredFreeFrame);
#endif
            m_deferredFreeFrame = nullptr;
        }
        m_deferredFreeFrame = frame;  // Free THIS frame on the NEXT iteration
    }
}

void Renderer::RenderFrame(AVFrame* frame) {
    // Set render target
    m_context->OMSetRenderTargets(1, m_rtv.GetAddressOf(), nullptr);

    // Set viewport
    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(m_config.width);
    vp.Height = static_cast<float>(m_config.height);
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);

    // Set shaders
    m_context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    m_context->PSSetShader(m_pixelShader.Get(), nullptr, 0);
    m_context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());

#ifdef HAS_FFMPEG
    // Extract D3D11 texture from AVFrame (hw_frames_ctx / D3D11VA)
    // frame->data[0] is the ID3D11Texture2D*
    // frame->data[1] is the array index (subresource) as intptr_t
    if (frame && frame->format == AV_PIX_FMT_D3D11) {
        auto* texture = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
        auto  subIndex = static_cast<UINT>(reinterpret_cast<intptr_t>(frame->data[1]));

        if (texture) {
            D3D11_TEXTURE2D_DESC desc;
            texture->GetDesc(&desc);

            // Create Y plane SRV (R8_UNORM view of the NV12 luma)
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDescY = {};
            srvDescY.Format = DXGI_FORMAT_R8_UNORM;
            srvDescY.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            srvDescY.Texture2DArray.MostDetailedMip = 0;
            srvDescY.Texture2DArray.MipLevels = 1;
            srvDescY.Texture2DArray.FirstArraySlice = subIndex;
            srvDescY.Texture2DArray.ArraySize = 1;

            ComPtr<ID3D11ShaderResourceView> srvY;
            HRESULT hr = m_device->CreateShaderResourceView(texture, &srvDescY, &srvY);
            if (FAILED(hr)) {
                CC_ERROR("Failed to create Y plane SRV: 0x%08X", hr);
                return;
            }

            // Create UV plane SRV (R8G8_UNORM view of the NV12 chroma)
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDescUV = {};
            srvDescUV.Format = DXGI_FORMAT_R8G8_UNORM;
            srvDescUV.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            srvDescUV.Texture2DArray.MostDetailedMip = 0;
            srvDescUV.Texture2DArray.MipLevels = 1;
            srvDescUV.Texture2DArray.FirstArraySlice = subIndex;
            srvDescUV.Texture2DArray.ArraySize = 1;

            ComPtr<ID3D11ShaderResourceView> srvUV;
            hr = m_device->CreateShaderResourceView(texture, &srvDescUV, &srvUV);
            if (FAILED(hr)) {
                CC_ERROR("Failed to create UV plane SRV: 0x%08X", hr);
                return;
            }

            // Bind SRVs to pixel shader
            ID3D11ShaderResourceView* srvs[] = { srvY.Get(), srvUV.Get() };
            m_context->PSSetShaderResources(0, 2, srvs);
        }
    }
#else
    (void)frame;
    // Without FFmpeg, clear to a dark blue so the window isn't garbage
    const float clearColor[] = { 0.0f, 0.0f, 0.15f, 1.0f };
    m_context->ClearRenderTargetView(m_rtv.Get(), clearColor);
#endif

    // Draw fullscreen triangle (3 vertices, no vertex buffer)
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->Draw(3, 0);

    // Unbind SRVs to prevent state leaking
    ID3D11ShaderResourceView* nullSrvs[] = { nullptr, nullptr };
    m_context->PSSetShaderResources(0, 2, nullSrvs);
}

void Renderer::Present() {
    UINT syncInterval = m_config.vsync ? 1 : 0;
    UINT presentFlags = 0;

    if (!m_config.vsync && m_tearingSupported) {
        presentFlags = DXGI_PRESENT_ALLOW_TEARING;
    }

    HRESULT hr = m_swapChain->Present(syncInterval, presentFlags);
    if (FAILED(hr) && hr != DXGI_ERROR_WAS_STILL_DRAWING) {
        CC_ERROR("Present failed: 0x%08X", hr);
    }
}

void Renderer::Resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;

    m_config.width = width;
    m_config.height = height;

    // Release old RTV
    m_rtv.Reset();

    HRESULT hr = m_swapChain->ResizeBuffers(0, width, height,
                                             DXGI_FORMAT_UNKNOWN,
                                             m_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);
    if (FAILED(hr)) {
        CC_ERROR("ResizeBuffers failed: 0x%08X", hr);
        return;
    }

    // Recreate RTV
    ComPtr<ID3D11Texture2D> backBuffer;
    m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_rtv);

    CC_INFO("Swap chain resized to %ux%u", width, height);
}

StreamStats Renderer::GetStats() const {
    StreamStats stats;
    stats.renderTimeMs = m_lastRenderTimeMs.load();
    stats.renderedFps = m_renderedFps.load();
    return stats;
}

void Renderer::Stop() {
    m_running = false;
    if (m_frameSubmittedEvent) SetEvent(m_frameSubmittedEvent);
    if (m_renderThread.joinable()) {
        m_renderThread.join();
    }
}

}  // namespace cc::client
