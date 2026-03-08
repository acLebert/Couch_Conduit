// Couch Conduit — Host session orchestrator
// Wires together: Capture → Encode → Transport, Input → Inject + Trigger Capture

#include <couch_conduit/host/host_session.h>
#include <couch_conduit/common/log.h>
#include <couch_conduit/common/system_tuning.h>

#include <algorithm>
#include <cstring>

namespace cc::host {

bool HostSession::Init(const Config& config) {
    m_config = config;

    // Apply system-level latency tuning
    cc::sys::ApplyLatencyTuning();

    // Initialize input injector with rumble callback
    m_inputInjector = std::make_unique<InputInjector>();
    auto rumbleCb = [this](uint8_t controllerId, uint8_t largeMotor, uint8_t smallMotor) {
        // Send rumble/haptic feedback to all clients
        #pragma pack(push, 1)
        struct HapticMsg {
            uint8_t msgType;       // InputMessageType::HapticFeedback
            uint8_t controllerId;
            uint8_t largeMotor;
            uint8_t smallMotor;
        };
        #pragma pack(pop)
        HapticMsg msg;
        msg.msgType = static_cast<uint8_t>(InputMessageType::HapticFeedback);
        msg.controllerId = controllerId;
        msg.largeMotor = largeMotor;
        msg.smallMotor = smallMotor;

        std::lock_guard<std::mutex> lock(m_clientsMutex);
        for (auto& client : m_clients) {
            client->hapticSocket.Send(&msg, sizeof(msg));
        }
    };
    if (!m_inputInjector->Init(rumbleCb)) {
        CC_WARN("Input injector init failed — continuing without gamepad support");
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

    // Add the first client
    if (!AddClient(config.clientHost, config.sessionKey, config.encrypted)) {
        CC_ERROR("Failed to add initial client");
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

    // Stop all client feedback threads and transport
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        for (auto& client : m_clients) {
            client->feedbackRunning = false;
            if (client->feedbackThread.joinable()) client->feedbackThread.join();
            client->feedbackSocket.Close();
            if (client->inputReceiver) client->inputReceiver->Stop();
            if (client->videoSender) client->videoSender->Shutdown();
            if (client->audioSender) client->audioSender->Shutdown();
        }
        m_clients.clear();
        m_videoSender = nullptr;
        m_audioSender = nullptr;
        m_inputReceiver = nullptr;
    }

    // Legacy feedback cleanup
    m_feedbackRunning = false;
    if (m_feedbackThread.joinable()) m_feedbackThread.join();
    m_feedbackSocket.Close();

    if (m_capture)        m_capture->Stop();
    if (m_encoder)        m_encoder->Shutdown();
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

    // Fan out to all connected clients
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    for (auto& client : m_clients) {
        if (client->videoSender) {
            client->videoSender->SendFrame(frameNum, data, len, isIdr, hostProcTime);
        }
    }
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

bool HostSession::AddClient(const std::string& clientHost,
                             const std::array<uint8_t, 16>& sessionKey,
                             bool encrypted) {
    auto client = std::make_unique<ClientState>();
    client->clientHost = clientHost;

    // Video sender
    client->videoSender = std::make_unique<transport::VideoSender>();
    if (!client->videoSender->Init(clientHost, m_config.clientVideoPort)) {
        CC_ERROR("Failed to init video sender for client %s", clientHost.c_str());
        return false;
    }
    if (encrypted) {
        client->videoSender->SetEncryptionKey(sessionKey.data());
    }

    // Audio sender
    client->audioSender = std::make_unique<transport::AudioSender>();
    if (!client->audioSender->Init(clientHost, m_config.clientAudioPort)) {
        CC_WARN("Audio sender failed for client %s", clientHost.c_str());
        client->audioSender.reset();
    } else if (encrypted) {
        client->audioSender->SetEncryptionKey(sessionKey.data());
    }

    // Input receiver (per-client port: base + clientIndex)
    client->inputReceiver = std::make_unique<transport::InputReceiver>();
    uint16_t inputPort = m_config.inputListenPort;
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        inputPort = static_cast<uint16_t>(m_config.inputListenPort + m_clients.size());
    }
    auto onInput = [this](const transport::InputPacketHeader& hdr,
                          const uint8_t* payload, size_t len) {
        OnInputReceived(hdr, payload, len);
    };
    if (!client->inputReceiver->Start(inputPort, onInput)) {
        CC_WARN("Input receiver failed for client %s on port %u", clientHost.c_str(), inputPort);
    } else if (encrypted) {
        client->inputReceiver->SetEncryptionKey(sessionKey.data());
    }

    // Feedback receiver
    uint16_t fbPort;
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        fbPort = static_cast<uint16_t>(m_config.feedbackPort + m_clients.size());
    }
    if (client->feedbackSocket.Bind(fbPort)) {
        client->feedbackSocket.SetRecvTimeout(100);
        transport::CongestionEstimator::Config ccCfg;
        ccCfg.startBitrateKbps = m_config.video.bitrateKbps;
        client->congestion.Init(ccCfg);
        client->feedbackRunning = true;
    }

    // Haptic/rumble sender (host → client, reuses input port + 100 offset)
    uint16_t hapticPort;
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        hapticPort = static_cast<uint16_t>(cc::kDefaultInputPort + 100 + m_clients.size());
    }
    client->hapticSocket.SetRemote(clientHost, hapticPort);

    CC_INFO("Client added: %s (video=%u, audio=%u, input=%u, feedback=%u)",
            clientHost.c_str(), m_config.clientVideoPort, m_config.clientAudioPort,
            inputPort, fbPort);

    // Store and set legacy aliases
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        m_clients.push_back(std::move(client));
        if (m_clients.size() == 1) {
            m_videoSender  = m_clients[0]->videoSender.get();
            m_audioSender  = m_clients[0]->audioSender.get();
            m_inputReceiver = m_clients[0]->inputReceiver.get();
        }
    }

    return true;
}

void HostSession::FeedbackRecvLoop() {
    alignas(8) uint8_t buf[1400];
    while (m_feedbackRunning) {
        int received = m_feedbackSocket.Recv(buf, sizeof(buf));
        if (received < static_cast<int>(sizeof(transport::FeedbackPacket))) continue;

        transport::FeedbackPacket fb;
        std::memcpy(&fb, buf, sizeof(fb));

        // Extract loss rate from lossMap
        int receivedBits = 0;
        uint64_t map = fb.lossMap;
        while (map) { receivedBits += (map & 1); map >>= 1; }
        float lossRate = 1.0f - static_cast<float>(receivedBits) / 64.0f;

        // EWMA smoothing
        m_measuredLossRate = m_measuredLossRate * 0.7f + lossRate * 0.3f;

        // Feed loss to congestion estimator
        m_congestion.OnLossUpdate(lossRate);

        // Parse TWCC entries if present
        int twccCount = fb.twccCount;
        size_t twccOff = sizeof(transport::FeedbackPacket);
        if (twccCount > 0 &&
            received >= static_cast<int>(twccOff + twccCount * sizeof(transport::TwccEntry))) {
            // We don't have host-side send timestamps readily available here,
            // so we pass zero send times — the gradient estimator will skip those samples.
            // A future refinement would log send timestamps per-sequence in VideoSender.
            std::vector<uint16_t> seqs(twccCount);
            std::vector<int64_t> sendTimes(twccCount, 0);
            std::vector<int16_t> arrivalDeltas(twccCount);

            for (int i = 0; i < twccCount; ++i) {
                transport::TwccEntry entry;
                std::memcpy(&entry, buf + twccOff + i * sizeof(transport::TwccEntry),
                           sizeof(entry));
                seqs[i] = entry.sequence;
                arrivalDeltas[i] = entry.arrivalDelta;
            }

            m_congestion.OnTwccFeedback(seqs.data(), sendTimes.data(),
                                        arrivalDeltas.data(), twccCount);
        }

        // Compute new bitrate from congestion estimator
        uint32_t newBitrate = m_congestion.ComputeBitrate();
        if (newBitrate > 0) {
            std::lock_guard<std::mutex> lock(m_clientsMutex);
            for (auto& client : m_clients) {
                if (client->videoSender) {
                    client->videoSender->SetTargetBitrateKbps(newBitrate);
                }
            }
            // TODO: Reconfigure NVENC bitrate dynamically (encoder reconfig)
        }

        // Adjust FEC ratio periodically (every 500ms)
        int64_t now = cc::NowUsec();
        if (now - m_lastFecAdjustUs > 500000) {
            AdjustFec(m_measuredLossRate);
            m_lastFecAdjustUs = now;
        }
    }
}

void HostSession::AdjustFec(float lossRate) {
    // Adaptive FEC: target FEC ratio = ~1.5x the measured loss rate
    // with floor of 5% and ceiling of 40%
    float targetRatio = lossRate * 1.5f;
    targetRatio = std::max(0.05f, std::min(0.40f, targetRatio));

    // Smooth adjustment (don't jump instantly)
    m_fecRatio = m_fecRatio * 0.8f + targetRatio * 0.2f;

    // Apply to all clients
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        for (auto& client : m_clients) {
            if (client->videoSender) {
                client->videoSender->SetFecRatio(m_fecRatio);
            }
        }
    }

    CC_TRACE("Adaptive FEC: loss=%.1f%% → fec=%.1f%%",
             lossRate * 100.0f, m_fecRatio * 100.0f);
}

}  // namespace cc::host
