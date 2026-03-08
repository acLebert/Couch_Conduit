#pragma once
// Couch Conduit — Client session orchestrator header

#include <couch_conduit/client/decoder.h>
#include <couch_conduit/client/renderer.h>
#include <couch_conduit/client/input_capture.h>
#include <couch_conduit/common/transport.h>
#include <couch_conduit/common/types.h>

#include <memory>
#include <string>

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

    bool Init(const Config& config);
    void Stop();

    // ─── Input forwarding (call from WndProc) ─────────────────────
    void OnKeyDown(uint16_t vkCode);
    void OnKeyUp(uint16_t vkCode);
    void OnMouseMove(int16_t dx, int16_t dy);
    void OnMouseButton(uint8_t button, bool pressed);
    void OnMouseScroll(int16_t dx, int16_t dy);
    void RequestIdr();

private:
    Config m_config;
    std::unique_ptr<Renderer>                   m_renderer;
    std::unique_ptr<D3D11Decoder>               m_decoder;
    std::unique_ptr<transport::VideoReceiver>    m_videoReceiver;
    std::unique_ptr<InputCapture>               m_inputCapture;

    void OnFrameDecoded(AVFrame* frame, const FrameMetadata& meta);
};

}  // namespace cc::client
