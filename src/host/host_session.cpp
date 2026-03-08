// Couch Conduit — Host session orchestrator
// Wires together: Capture → Encode → Transport, Input → Inject + Trigger Capture

#include <couch_conduit/host/host_session.h>
#include <couch_conduit/common/log.h>
#include <couch_conduit/common/system_tuning.h>

#include <algorithm>

namespace cc::host {

bool HostSession::Init(const Config& config) {
    m_config = config;

    // Apply system-level latency tuning
    cc::sys::ApplyLatencyTuning();

    // Initialize input injector
    m_inputInjector = std::make_unique<InputInjector>();
    if (!m_inputInjector->Init()) {
        CC_WARN("Input injector init failed — continuing without gamepad support");
    }

    // Initialize video sender (transport)
    m_videoSender = std::make_unique<transport::VideoSender>();
    if (!m_videoSender->Init(config.clientHost, config.clientVideoPort)) {
        CC_ERROR("Failed to init video sender");
        return false;
    }

    // Initialize DXGI capture
    m_capture = std::make_unique<DxgiCapture>();
    DxgiCapture::Config captureConfig;
    captureConfig.maxFps = config.video.fps;

    auto onFrameCaptured = [this](ID3D11Texture2D* texture, uint32_t frameNum, int64_t captureTs) {
        OnFrameCaptured(texture, frameNum, captureTs);
    };
    if (!m_capture->Init(captureConfig, onFrameCaptured)) {
        CC_ERROR("Failed to init DXGI capture");
        return false;
    }

    // Determine encode resolution (use explicit config or fall back to capture res)
    m_encodeWidth  = config.encodeWidth  > 0 ? config.encodeWidth  : m_capture->GetWidth();
    m_encodeHeight = config.encodeHeight > 0 ? config.encodeHeight : m_capture->GetHeight();
    m_needsDownscale = (m_encodeWidth != m_capture->GetWidth() || m_encodeHeight != m_capture->GetHeight());

    // If encode resolution differs from capture, set up video processor for GPU downscale
    if (m_needsDownscale) {
        if (!InitVideoProcessor()) {
            CC_ERROR("Failed to init video processor for downscale");
            return false;
        }
        CC_INFO("Downscale: %ux%u → %ux%u (D3D11 Video Processor)",
                m_capture->GetWidth(), m_capture->GetHeight(),
                m_encodeWidth, m_encodeHeight);
    }

    // Initialize NVENC encoder at the (possibly downscaled) resolution
    m_encoder = std::make_unique<NvencEncoder>();
    NvencEncoder::Config encConfig;
    encConfig.width       = m_encodeWidth;
    encConfig.height      = m_encodeHeight;
    encConfig.fps         = config.video.fps;
    encConfig.bitrateKbps = config.video.bitrateKbps;
    encConfig.codec       = config.video.codec;

    auto onEncodeDone = [this](uint32_t frameNum, const uint8_t* data, size_t len,
                               bool isIdr, int64_t encStart, int64_t encEnd) {
        OnEncodeDone(frameNum, data, len, isIdr, encStart, encEnd);
    };
    if (!m_encoder->Init(m_capture->GetDevice(), encConfig, onEncodeDone)) {
        CC_ERROR("Failed to init NVENC encoder");
        return false;
    }

    // Initialize input receiver
    m_inputReceiver = std::make_unique<transport::InputReceiver>();
    auto onInput = [this](const transport::InputPacketHeader& hdr,
                          const uint8_t* payload, size_t len) {
        OnInputReceived(hdr, payload, len);
    };
    if (!m_inputReceiver->Start(config.inputListenPort, onInput)) {
        CC_ERROR("Failed to start input receiver");
        return false;
    }

    CC_INFO("Host session initialized: capture %ux%u, encode %ux%u @ %u fps, %u kbps -> %s:%u",
            m_capture->GetWidth(), m_capture->GetHeight(),
            m_encodeWidth, m_encodeHeight,
            config.video.fps, config.video.bitrateKbps,
            config.clientHost.c_str(), config.clientVideoPort);
    return true;
}

bool HostSession::Start() {
    if (!m_capture->Start()) {
        CC_ERROR("Failed to start capture");
        return false;
    }

    CC_INFO("=== Host session streaming ===");
    m_streaming = true;
    return true;
}

void HostSession::Stop() {
    m_streaming = false;
    if (m_inputReceiver)  m_inputReceiver->Stop();
    if (m_capture)        m_capture->Stop();
    if (m_encoder)        m_encoder->Shutdown();
    if (m_videoSender)    m_videoSender->Shutdown();
    if (m_inputInjector)  m_inputInjector->Shutdown();
    CC_INFO("Host session stopped");
}

void HostSession::OnFrameCaptured(ID3D11Texture2D* texture, uint32_t frameNum, int64_t captureTs) {
    ID3D11Texture2D* encodeTexture = texture;

    // Downscale if needed
    if (m_needsDownscale && m_videoProcessor && m_downscaleTexture) {
        ComPtr<ID3D11VideoProcessorInputView> inputView;
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc = {};
        inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        inputDesc.Texture2D.MipSlice = 0;

        HRESULT hr = m_videoDevice->CreateVideoProcessorInputView(
            texture, m_vpEnum.Get(), &inputDesc, &inputView);
        if (SUCCEEDED(hr)) {
            ComPtr<ID3D11VideoProcessorOutputView> outputView;
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc = {};
            outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            outputDesc.Texture2D.MipSlice = 0;

            hr = m_videoDevice->CreateVideoProcessorOutputView(
                m_downscaleTexture.Get(), m_vpEnum.Get(), &outputDesc, &outputView);
            if (SUCCEEDED(hr)) {
                D3D11_VIDEO_PROCESSOR_STREAM stream = {};
                stream.Enable = TRUE;
                stream.pInputSurface = inputView.Get();

                hr = m_videoContext->VideoProcessorBlt(
                    m_videoProcessor.Get(), outputView.Get(), 0, 1, &stream);
                if (SUCCEEDED(hr)) {
                    encodeTexture = m_downscaleTexture.Get();
                } else {
                    CC_WARN("VideoProcessorBlt failed: 0x%08X — using original texture", hr);
                }
            }
        }
    }

    // Encode immediately — texture is in VRAM, NVENC reads directly
    m_encoder->EncodeFrame(encodeTexture, frameNum, captureTs);
}

bool HostSession::InitVideoProcessor() {
    // Get the video device interface
    HRESULT hr = m_capture->GetDevice()->QueryInterface(IID_PPV_ARGS(&m_videoDevice));
    if (FAILED(hr)) {
        CC_ERROR("Failed to get ID3D11VideoDevice: 0x%08X", hr);
        return false;
    }

    ComPtr<ID3D11DeviceContext> ctx;
    m_capture->GetDevice()->GetImmediateContext(&ctx);
    hr = ctx.As(&m_videoContext);
    if (FAILED(hr)) {
        CC_ERROR("Failed to get ID3D11VideoContext: 0x%08X", hr);
        return false;
    }

    // Create video processor enumerator
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc = {};
    contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    contentDesc.InputWidth = m_capture->GetWidth();
    contentDesc.InputHeight = m_capture->GetHeight();
    contentDesc.OutputWidth = m_encodeWidth;
    contentDesc.OutputHeight = m_encodeHeight;
    contentDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    hr = m_videoDevice->CreateVideoProcessorEnumerator(&contentDesc, &m_vpEnum);
    if (FAILED(hr)) {
        CC_ERROR("Failed to create VP enumerator: 0x%08X", hr);
        return false;
    }

    // Create video processor
    hr = m_videoDevice->CreateVideoProcessor(m_vpEnum.Get(), 0, &m_videoProcessor);
    if (FAILED(hr)) {
        CC_ERROR("Failed to create video processor: 0x%08X", hr);
        return false;
    }

    // Disable auto-processing for lowest latency
    m_videoContext->VideoProcessorSetStreamAutoProcessingMode(m_videoProcessor.Get(), 0, FALSE);

    // Set output color space
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE colorSpace = {};
    colorSpace.Usage = 0;  // Playback
    colorSpace.RGB_Range = 0;  // Full range
    m_videoContext->VideoProcessorSetOutputColorSpace(m_videoProcessor.Get(), &colorSpace);
    m_videoContext->VideoProcessorSetStreamColorSpace(m_videoProcessor.Get(), 0, &colorSpace);

    // Create output texture at encode resolution
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = m_encodeWidth;
    texDesc.Height = m_encodeHeight;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;  // Same format as captured desktop
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    texDesc.MiscFlags = 0;

    hr = m_capture->GetDevice()->CreateTexture2D(&texDesc, nullptr, &m_downscaleTexture);
    if (FAILED(hr)) {
        CC_ERROR("Failed to create downscale texture: 0x%08X", hr);
        return false;
    }

    CC_INFO("Video processor initialized for downscale %ux%u → %ux%u",
            m_capture->GetWidth(), m_capture->GetHeight(),
            m_encodeWidth, m_encodeHeight);
    return true;
}

void HostSession::OnEncodeDone(uint32_t frameNum, const uint8_t* data, size_t len,
                                bool isIdr, int64_t encStart, int64_t encEnd) {
    // Calculate host processing time in 0.1ms units
    uint16_t hostProcTime = static_cast<uint16_t>(
        std::min<int64_t>((encEnd - encStart) / 100, UINT16_MAX));

    // Send to client via UDP
    m_videoSender->SendFrame(frameNum, data, len, isIdr, hostProcTime);
}

void HostSession::OnInputReceived(const transport::InputPacketHeader& hdr,
                                   const uint8_t* payload, size_t len) {
    // Handle control messages before input injection
    if (hdr.msgType == InputMessageType::RequestIdr) {
        CC_INFO("Client requested IDR frame");
        m_encoder->ForceIdr();
        return;
    }

    // INLINE injection — no queuing!
    m_inputInjector->ProcessInputPacket(hdr, payload, len);

    // Trigger immediate screen capture (input-to-capture sync)
    m_capture->TriggerCapture();
}

}  // namespace cc::host
