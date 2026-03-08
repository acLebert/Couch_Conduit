// Couch Conduit — Client session orchestrator
// Wires together: Network → Decode → Render, Input → Send

#include <couch_conduit/client/decoder.h>
#include <couch_conduit/client/renderer.h>
#include <couch_conduit/common/transport.h>
#include <couch_conduit/common/log.h>
#include <couch_conduit/common/system_tuning.h>

#include <memory>

namespace cc::client {

class ClientSession {
public:
    struct Config {
        std::string hostAddr;
        uint16_t    videoPort    = cc::kDefaultVideoPort;
        uint16_t    inputPort    = cc::kDefaultInputPort;
        uint32_t    windowWidth  = 1920;
        uint32_t    windowHeight = 1080;
        bool        vsync        = false;
        HWND        hwnd         = nullptr;
    };

    bool Init(const Config& config) {
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
        if (!m_decoder->Init(m_renderer->GetDevice(), decodeConfig, onDecoded)) {
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

        CC_INFO("Client session initialized: host=%s, video=%u, input=%u",
                config.hostAddr.c_str(), config.videoPort, config.inputPort);
        return true;
    }

    void Stop() {
        m_renderer->Stop();
        m_decoder->Stop();
        m_videoReceiver->Stop();
        CC_INFO("Client session stopped");
    }

private:
    Config m_config;
    std::unique_ptr<Renderer>                  m_renderer;
    std::unique_ptr<D3D11Decoder>              m_decoder;
    std::unique_ptr<transport::VideoReceiver>   m_videoReceiver;

    void OnFrameDecoded(AVFrame* frame, const FrameMetadata& meta) {
        // Submit decoded frame directly to renderer
        m_renderer->SubmitFrame(frame, meta);
    }
};

}  // namespace cc::client
