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

    // Initialize NVENC encoder
    m_encoder = std::make_unique<NvencEncoder>();
    NvencEncoder::Config encConfig;
    encConfig.width       = m_capture->GetWidth();
    encConfig.height      = m_capture->GetHeight();
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

    CC_INFO("Host session initialized: %ux%u @ %u fps, %u kbps -> %s:%u",
            m_capture->GetWidth(), m_capture->GetHeight(),
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
    // Encode immediately — texture is in VRAM, NVENC reads directly
    m_encoder->EncodeFrame(texture, frameNum, captureTs);
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
    // INLINE injection — no queuing!
    m_inputInjector->ProcessInputPacket(hdr, payload, len);

    // Trigger immediate screen capture (input-to-capture sync)
    m_capture->TriggerCapture();
}

}  // namespace cc::host
