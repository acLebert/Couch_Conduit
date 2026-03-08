#pragma once
// Couch Conduit — Client session orchestrator header

#include <couch_conduit/client/decoder.h>
#include <couch_conduit/client/renderer.h>
#include <couch_conduit/client/input_capture.h>
#include <couch_conduit/common/transport.h>
#include <couch_conduit/common/types.h>

#include <memory>
#include <string>
#include <array>

namespace cc::client {

class ClientSession {
public:
    struct Config {
        std::string hostAddr;
        uint16_t    videoPort    = cc::kDefaultVideoPort;
        uint16_t    audioPort    = cc::kDefaultAudioPort;
        uint16_t    inputPort    = cc::kDefaultInputPort;
        uint16_t    feedbackPort = cc::kDefaultFeedbackPort;
        uint32_t    windowWidth  = 1920;
        uint32_t    windowHeight = 1080;
        bool        vsync        = false;
        HWND        hwnd         = nullptr;
        std::array<uint8_t, 16> sessionKey{};
        bool        encrypted    = false;
        // Benchmark mode
        std::string csvPath;        // If non-empty, write periodic stats CSV
        int64_t     processStartUs = 0;  // For startup metrics
    };

    bool Init(const Config& config);
    void Stop();

    // ─── Input forwarding (call from WndProc) ─────────────────────
    void OnKeyDown(uint16_t vkCode);
    void OnKeyUp(uint16_t vkCode);
    void OnMouseMove(int16_t dx, int16_t dy);
    void OnMouseButton(uint8_t button, bool pressed);
    void OnMouseScroll(int16_t dx, int16_t dy);
    void RequestIdr();

    /// Get the overlay (for forwarding WndProc input and toggling)
    Overlay* GetOverlay();

private:
    Config m_config;
    std::unique_ptr<Renderer>                   m_renderer;
    std::unique_ptr<D3D11Decoder>               m_decoder;
    std::unique_ptr<transport::VideoReceiver>    m_videoReceiver;
    std::unique_ptr<transport::AudioReceiver>    m_audioReceiver;
    std::unique_ptr<InputCapture>               m_inputCapture;

    // Feedback sender for adaptive FEC / bitrate
    transport::UdpSocket m_feedbackSocket;
    std::thread          m_feedbackThread;
    std::atomic<bool>    m_feedbackRunning{false};
    uint64_t             m_recvBitmap = 0;
    uint16_t             m_lastFrameRecv = 0;

    // Haptic/rumble receiver (host → client)
    transport::UdpSocket m_hapticSocket;
    std::thread          m_hapticThread;
    std::atomic<bool>    m_hapticRunning{false};

    void OnFrameDecoded(AVFrame* frame, const FrameMetadata& meta);
    void FeedbackSendLoop();
    void HapticRecvLoop();
};

}  // namespace cc::client
