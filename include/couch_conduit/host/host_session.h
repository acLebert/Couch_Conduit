#pragma once
// Couch Conduit — Host session orchestrator header

#include <couch_conduit/host/capture.h>
#include <couch_conduit/host/encoder.h>
#include <couch_conduit/host/input_injector.h>
#include <couch_conduit/common/transport.h>
#include <couch_conduit/common/types.h>

#include <d3d11_4.h>
#include <wrl/client.h>

#include <memory>
#include <atomic>
#include <string>

namespace cc::host {

using Microsoft::WRL::ComPtr;

class HostSession {
public:
    struct Config {
        VideoConfig  video;
        std::string  clientHost;
        uint16_t     clientVideoPort  = cc::kDefaultVideoPort;
        uint16_t     inputListenPort  = cc::kDefaultInputPort;
        uint32_t     encodeWidth      = 0;  // 0 = same as capture
        uint32_t     encodeHeight     = 0;  // 0 = same as capture
    };

    bool Init(const Config& config);
    bool Start();
    void Stop();
    bool IsStreaming() const { return m_streaming; }

private:
    Config m_config;
    std::atomic<bool> m_streaming{false};

    std::unique_ptr<DxgiCapture>                m_capture;
    std::unique_ptr<NvencEncoder>               m_encoder;
    std::unique_ptr<transport::VideoSender>     m_videoSender;
    std::unique_ptr<transport::InputReceiver>   m_inputReceiver;
    std::unique_ptr<InputInjector>              m_inputInjector;

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
};

}  // namespace cc::host
