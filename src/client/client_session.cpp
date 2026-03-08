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

    // Enable encryption if session key is available
    if (config.encrypted) {
        m_videoReceiver->SetEncryptionKey(config.sessionKey.data());
        CC_INFO("Video receiver encryption enabled");
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
        // Enable encryption for input sender
        if (config.encrypted) {
            m_inputCapture->SetEncryptionKey(config.sessionKey.data());
            CC_INFO("Input sender encryption enabled");
        }
        m_inputCapture->Start();
    }

    // Initialize audio receiver
    m_audioReceiver = std::make_unique<transport::AudioReceiver>();
    // Audio callback delivers PCM to audio player (player managed elsewhere)
    auto onAudio = [](const float*, uint32_t, uint32_t, uint32_t) {
        // TODO: Route to AudioPlayer instance when integrated into window
    };
    if (!m_audioReceiver->Start(config.audioPort, onAudio)) {
        CC_WARN("Failed to start audio receiver — audio disabled");
        m_audioReceiver.reset();
    } else if (config.encrypted) {
        m_audioReceiver->SetEncryptionKey(config.sessionKey.data());
    }

    // Start feedback sender — sends periodic loss stats to host
    m_feedbackSocket.SetRemote(config.hostAddr, config.feedbackPort);
    m_feedbackRunning = true;
    m_feedbackThread = std::thread([this]() { FeedbackSendLoop(); });

    // Start haptic/rumble receiver
    uint16_t hapticPort = static_cast<uint16_t>(cc::kDefaultInputPort + 100);
    if (m_hapticSocket.Bind(hapticPort)) {
        m_hapticSocket.SetRecvTimeout(100);
        m_hapticRunning = true;
        m_hapticThread = std::thread([this]() { HapticRecvLoop(); });
        CC_INFO("Haptic receiver started on port %u", hapticPort);
    }

    CC_INFO("Client session initialized: host=%s, video=%u, audio=%u, input=%u",
            config.hostAddr.c_str(), config.videoPort, config.audioPort, config.inputPort);

    // Request IDR immediately — we joined mid-stream and need SPS/PPS
    RequestIdr();

    return true;
}

void ClientSession::Stop() {
    m_feedbackRunning = false;
    m_hapticRunning = false;

    // Close sockets FIRST to unblock any threads stuck in Recv()/Send()
    m_feedbackSocket.Close();
    m_hapticSocket.Close();

    if (m_feedbackThread.joinable()) m_feedbackThread.join();
    if (m_hapticThread.joinable()) m_hapticThread.join();
    if (m_inputCapture)   m_inputCapture->Stop();
    if (m_audioReceiver)  m_audioReceiver->Stop();
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

Overlay* ClientSession::GetOverlay() {
    return m_renderer ? m_renderer->GetOverlay() : nullptr;
}

// ─── Internal ──────────────────────────────────────────────────────────

void ClientSession::OnFrameDecoded(AVFrame* frame, const FrameMetadata& meta) {
    // Submit decoded frame directly to renderer
    m_renderer->SubmitFrame(frame, meta);

    // Update feedback bitmap — shift and set bit 0 for this frame
    uint16_t gap = static_cast<uint16_t>(meta.frameNumber - m_lastFrameRecv);
    if (gap > 0 && gap < 64) {
        m_recvBitmap <<= gap;
    } else if (gap >= 64) {
        m_recvBitmap = 0;
    }
    m_recvBitmap |= 1;
    m_lastFrameRecv = static_cast<uint16_t>(meta.frameNumber);
}

void ClientSession::FeedbackSendLoop() {
    while (m_feedbackRunning) {
        Sleep(100);  // Send feedback at 10 Hz
        if (!m_feedbackRunning) break;

        transport::FeedbackPacket fb = {};
        fb.lastFrameReceived = m_lastFrameRecv;
        fb.lossMap           = m_recvBitmap;
        fb.decodeTimeUs      = 0;  // TODO: populate from decoder stats
        fb.renderTimeUs      = 0;  // TODO: populate from renderer stats
        fb.queueDepth        = 0;
        fb.twccCount         = 0;

        m_feedbackSocket.Send(&fb, sizeof(fb));
    }
}

void ClientSession::HapticRecvLoop() {
    #pragma pack(push, 1)
    struct HapticMsg {
        uint8_t msgType;
        uint8_t controllerId;
        uint8_t largeMotor;
        uint8_t smallMotor;
    };
    #pragma pack(pop)

    HapticMsg msg;
    while (m_hapticRunning) {
        int received = m_hapticSocket.Recv(&msg, sizeof(msg));
        if (received < static_cast<int>(sizeof(msg))) continue;

        if (msg.msgType == static_cast<uint8_t>(InputMessageType::HapticFeedback)) {
            // Apply vibration via XInput (if the client has a local controller)
            // For now, log and use XInputSetState if available
            CC_DEBUG("Haptic received: controller=%u large=%u small=%u",
                     msg.controllerId, msg.largeMotor, msg.smallMotor);

            // Dynamic XInput vibration
            using XInputSetStateFn = DWORD(WINAPI*)(DWORD, void*);
            static XInputSetStateFn fnSetState = nullptr;
            static bool loaded = false;
            if (!loaded) {
                loaded = true;
                HMODULE xinput = LoadLibraryW(L"xinput1_4.dll");
                if (!xinput) xinput = LoadLibraryW(L"xinput1_3.dll");
                if (!xinput) xinput = LoadLibraryW(L"xinput9_1_0.dll");
                if (xinput) {
                    fnSetState = reinterpret_cast<XInputSetStateFn>(
                        GetProcAddress(xinput, "XInputSetState"));
                }
            }
            if (fnSetState) {
                struct { uint16_t wLeftMotorSpeed; uint16_t wRightMotorSpeed; } vibration;
                vibration.wLeftMotorSpeed  = static_cast<uint16_t>(msg.largeMotor) * 257;
                vibration.wRightMotorSpeed = static_cast<uint16_t>(msg.smallMotor) * 257;
                fnSetState(msg.controllerId, &vibration);
            }
        }
    }
}

}  // namespace cc::client
