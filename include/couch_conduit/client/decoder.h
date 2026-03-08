#pragma once
// Couch Conduit — D3D11VA hardware video decoder (Client side)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>
#include <functional>
#include <atomic>
#include <thread>

#include <couch_conduit/common/types.h>

// FFmpeg forward declarations
extern "C" {
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct AVBufferRef;
}

namespace cc::client {

using Microsoft::WRL::ComPtr;

/// Callback when a frame is decoded and ready for rendering.
/// The AVFrame contains a D3D11 texture in hw_frames_ctx.
using FrameDecodedCallback = std::function<void(
    AVFrame*           frame,
    const FrameMetadata& meta
)>;

/// D3D11VA hardware-accelerated video decoder.
///
/// Key improvements over Moonlight:
/// - Event-signaled wakeup instead of SDL_Delay(2) polling
/// - Separate decode D3D11 device with shared fence to render device
/// - GPU timestamp queries for precise timing
class D3D11Decoder {
public:
    D3D11Decoder() = default;
    ~D3D11Decoder();

    D3D11Decoder(const D3D11Decoder&) = delete;
    D3D11Decoder& operator=(const D3D11Decoder&) = delete;

    struct Config {
        VideoCodec codec        = VideoCodec::HEVC;
        uint32_t   width        = 1920;
        uint32_t   height       = 1080;
        bool       enableRfi    = true;   // Reference Frame Invalidation
    };

    /// Initialize decoder with a shared D3D11 device (same one used for rendering).
    /// This enables zero-copy decode→render.
    bool Init(ID3D11Device* renderDevice, const Config& config, FrameDecodedCallback callback);

    /// Start the decode thread. It will wait on the frameReadyEvent
    /// signaled by the VideoReceiver.
    bool Start(HANDLE frameReadyEvent);

    /// Submit encoded data for decoding (called by VideoReceiver)
    void SubmitFrame(uint32_t frameNumber, const uint8_t* data, size_t len,
                     const FrameMetadata& meta);

    /// Request IDR from host (on decode error)
    bool NeedsIdr() const { return m_needsIdr.load(); }
    void ClearIdrRequest() { m_needsIdr.store(false); }

    /// Stop the decode thread
    void Stop();

    /// Get average decode time in microseconds
    int64_t GetAvgDecodeTimeUs() const { return m_avgDecodeTimeUs; }

private:
    // FFmpeg/D3D11VA
    AVCodecContext*  m_codecCtx = nullptr;
    AVBufferRef*     m_hwDeviceCtx = nullptr;
    AVBufferRef*     m_hwFramesCtx = nullptr;
    AVPacket*        m_packet = nullptr;
    AVFrame*         m_frame = nullptr;

    // D3D11 device (shared with renderer for zero-copy)
    ComPtr<ID3D11Device> m_device;

    FrameDecodedCallback m_callback;
    Config               m_config;

    // Decode thread
    std::thread       m_decodeThread;
    std::atomic<bool> m_running{false};
    HANDLE            m_frameReadyEvent = nullptr;
    std::atomic<bool> m_needsIdr{false};

    // Pending frame data (written by network thread, read by decode thread)
    struct PendingFrame {
        std::vector<uint8_t> data;
        FrameMetadata        meta;
        bool                 ready = false;
    };
    PendingFrame   m_pending;
    CRITICAL_SECTION m_pendingLock;

    // Stats
    int64_t  m_avgDecodeTimeUs = 0;
    uint32_t m_decodeCount = 0;

    // GPU timestamp queries
    ComPtr<ID3D11Query> m_tsQueryStart;
    ComPtr<ID3D11Query> m_tsQueryEnd;
    ComPtr<ID3D11Query> m_tsQueryDisjoint;

    bool CreateDecoder();
    bool InitHwAccel(ID3D11Device* device);
    void DecodeLoop();
    bool DecodeFrame(const uint8_t* data, size_t len, AVFrame* outFrame);
};

}  // namespace cc::client
