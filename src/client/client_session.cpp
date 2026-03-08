// Couch Conduit — Client session orchestrator
// Wires together: Network → Decode → Render, Input → Send

#include <couch_conduit/client/client_session.h>
#include <couch_conduit/common/log.h>
#include <couch_conduit/common/system_tuning.h>

namespace cc::client {

bool ClientSession::Init(const Config& config) {
    m_config = config;

    // Apply system-level latency tuning
    cc::sys::ApplyLatencyTuning();

    // Initialize renderer (creates D3D11 device)
    m_renderer = std::make_unique<Renderer>();
    Renderer::Config renderConfig;
    renderConfig.hwnd   = config.hwnd;
    renderConfig.width  = config.windowWidth;
    renderConfig.height = config.windowHeight;
    renderConfig.vsync  = config.vsync;

    if (!m_renderer->Init(renderConfig)) {
        CC_ERROR("Failed to init renderer");
        return false;
    }

    // Initialize decoder (shares D3D11 device with renderer for zero-copy)
    m_decoder = std::make_unique<D3D11Decoder>();
    D3D11Decoder::Config decodeConfig;
    decodeConfig.width  = config.windowWidth;
    decodeConfig.height = config.windowHeight;

    auto onDecoded = [this](AVFrame* frame, const FrameMetadata& meta) {
        OnFrameDecoded(frame, meta);
    };
    auto onIdrNeeded = [this]() {
        RequestIdr();
    };
    if (!m_decoder->Init(m_renderer->GetDevice(), decodeConfig, onDecoded, onIdrNeeded)) {
        CC_ERROR("Failed to init decoder");
        return false;
    }

    // Initialize video receiver
    m_videoReceiver = std::make_unique<transport::VideoReceiver>();
    auto onFrame = [this](uint32_t frameNum, const uint8_t* data, size_t len,
                          const FrameMetadata& meta) {
        m_decoder->SubmitFrame(frameNum, data, len, meta);
    };
    if (!m_videoReceiver->Start(config.videoPort, onFrame)) {
        CC_ERROR("Failed to start video receiver");
        return false;
    }

    // Start decoder with the VideoReceiver's frame-ready event
    // This is the event-signaled decode — no polling!
    if (!m_decoder->Start(m_videoReceiver->GetFrameReadyEvent())) {
        CC_ERROR("Failed to start decoder");
        return false;
    }

    // Start renderer thread
    if (!m_renderer->Start()) {
        CC_ERROR("Failed to start renderer");
        return false;
    }

    // Initialize input capture (XInput polling + keyboard/mouse forwarding → host)
    m_inputCapture = std::make_unique<InputCapture>();
    if (!m_inputCapture->Init(config.hostAddr, config.inputPort)) {
        CC_WARN("Failed to init input capture — input forwarding disabled");
    } else {
        m_inputCapture->Start();
    }

    CC_INFO("Client session initialized: host=%s, video=%u, input=%u",
            config.hostAddr.c_str(), config.videoPort, config.inputPort);

    // Request IDR immediately — we joined mid-stream and need SPS/PPS
    RequestIdr();

    return true;
}

void ClientSession::Stop() {
    if (m_inputCapture)   m_inputCapture->Stop();
    if (m_renderer)       m_renderer->Stop();
    if (m_decoder)        m_decoder->Stop();
    if (m_videoReceiver)  m_videoReceiver->Stop();
    CC_INFO("Client session stopped");
}

// ─── Input forwarding from WndProc → InputCapture → UDP → Host ─────────

void ClientSession::OnKeyDown(uint16_t vkCode) {
    if (m_inputCapture) m_inputCapture->OnKeyDown(vkCode);
}

void ClientSession::OnKeyUp(uint16_t vkCode) {
    if (m_inputCapture) m_inputCapture->OnKeyUp(vkCode);
}

void ClientSession::OnMouseMove(int16_t dx, int16_t dy) {
    if (m_inputCapture) m_inputCapture->OnMouseMove(dx, dy);
}

void ClientSession::OnMouseButton(uint8_t button, bool pressed) {
    if (m_inputCapture) m_inputCapture->OnMouseButton(button, pressed);
}

void ClientSession::OnMouseScroll(int16_t dx, int16_t dy) {
    if (m_inputCapture) m_inputCapture->OnMouseScroll(dx, dy);
}

void ClientSession::RequestIdr() {
    if (m_inputCapture) m_inputCapture->SendRequestIdr();
}

// ─── Internal ──────────────────────────────────────────────────────────

void ClientSession::OnFrameDecoded(AVFrame* frame, const FrameMetadata& meta) {
    // Submit decoded frame directly to renderer
    m_renderer->SubmitFrame(frame, meta);
}

}  // namespace cc::client
