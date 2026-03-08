#pragma once
// Couch Conduit — Host session orchestrator header

#include <couch_conduit/host/capture.h>
#include <couch_conduit/host/encoder.h>
#include <couch_conduit/host/input_injector.h>
#include <couch_conduit/common/transport.h>
#include <couch_conduit/common/congestion.h>
#include <couch_conduit/common/types.h>

#include <d3d11_4.h>
#include <wrl/client.h>

#include <memory>
#include <atomic>
#include <string>
#include <array>

namespace cc::host {

using Microsoft::WRL::ComPtr;

class HostSession {
public:
    struct Config {
        VideoConfig  video;
        std::string  clientHost;
        uint16_t     clientVideoPort  = cc::kDefaultVideoPort;
        uint16_t     clientAudioPort  = cc::kDefaultAudioPort;
        uint16_t     inputListenPort  = cc::kDefaultInputPort;
        uint16_t     feedbackPort     = cc::kDefaultFeedbackPort;
        uint32_t     encodeWidth      = 0;  // 0 = same as capture
        uint32_t     encodeHeight     = 0;  // 0 = same as capture
        std::array<uint8_t, 16> sessionKey{};  // AES-128 key from ECDH
        bool         encrypted        = false;
    };

    bool Init(const Config& config);
    bool Start();
    void Stop();
    bool IsStreaming() const { return m_streaming; }

    /// Add an additional client to this session (multi-client fan-out).
    /// Can be called while streaming is active.
    bool AddClient(const std::string& clientHost,
                   const std::array<uint8_t, 16>& sessionKey,
                   bool encrypted);

private:
    Config m_config;
    std::atomic<bool> m_streaming{false};

    std::unique_ptr<DxgiCapture>                m_capture;
    std::unique_ptr<NvencEncoder>               m_encoder;

    // Multi-client support: each client gets its own sender/receiver set
    struct ClientState {
        std::string                                   clientHost;
        std::unique_ptr<transport::VideoSender>       videoSender;
        std::unique_ptr<transport::AudioSender>       audioSender;
        std::unique_ptr<transport::InputReceiver>     inputReceiver;
        transport::UdpSocket                          feedbackSocket;
        transport::UdpSocket                          hapticSocket;  // Host → Client rumble
        std::thread                                   feedbackThread;
        std::atomic<bool>                             feedbackRunning{false};
        transport::CongestionEstimator                congestion;
        float                                         measuredLossRate = 0.0f;
        float                                         fecRatio = 0.10f;
        int64_t                                       lastFecAdjustUs = 0;
    };

    std::mutex                                  m_clientsMutex;
    std::vector<std::unique_ptr<ClientState>>   m_clients;

    // Legacy single-client pointers (aliases into m_clients[0] for backward compat)
    transport::VideoSender*                     m_videoSender     = nullptr;
    transport::AudioSender*                     m_audioSender     = nullptr;
    transport::InputReceiver*                   m_inputReceiver   = nullptr;
    std::unique_ptr<InputInjector>              m_inputInjector;

    // Feedback receiver for adaptive FEC/bitrate (legacy single-client)
    transport::UdpSocket m_feedbackSocket;
    std::thread          m_feedbackThread;
    std::atomic<bool>    m_feedbackRunning{false};

    // Adaptive FEC state
    float  m_measuredLossRate = 0.0f;
    float  m_fecRatio         = 0.10f;  // Current FEC ratio
    int64_t m_lastFecAdjustUs = 0;

    // Congestion estimator for adaptive bitrate
    transport::CongestionEstimator m_congestion;

    // Optional downscale (when encode resolution != capture resolution)
    ComPtr<ID3D11VideoDevice>                   m_videoDevice;
    ComPtr<ID3D11VideoContext>                  m_videoContext;
    ComPtr<ID3D11VideoProcessorEnumerator>      m_vpEnum;
    ComPtr<ID3D11VideoProcessor>                m_videoProcessor;
    ComPtr<ID3D11Texture2D>                     m_downscaleTexture;
    uint32_t m_encodeWidth  = 0;
    uint32_t m_encodeHeight = 0;
    bool m_needsDownscale = false;

    bool InitVideoProcessor();

    void OnFrameCaptured(ID3D11Texture2D* texture, uint32_t frameNum, int64_t captureTs);
    void OnEncodeDone(uint32_t frameNum, const uint8_t* data, size_t len,
                      bool isIdr, int64_t encStart, int64_t encEnd);
    void OnInputReceived(const transport::InputPacketHeader& hdr,
                         const uint8_t* payload, size_t len);
    void FeedbackRecvLoop();
    void AdjustFec(float lossRate);
};

}  // namespace cc::host
