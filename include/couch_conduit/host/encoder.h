#pragma once
// Couch Conduit — NVENC hardware video encoder

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d11.h>

#include <cstdint>
#include <vector>
#include <memory>
#include <functional>

#include <couch_conduit/common/types.h>

#ifdef HAS_NVENC
#include <nvEncodeAPI.h>
#endif

namespace cc::host {

/// Callback when encoding completes. Receives the bitstream data.
using EncodeDoneCallback = std::function<void(
    uint32_t       frameNumber,
    const uint8_t* bitstreamData,
    size_t         bitstreamSize,
    bool           isIdr,
    int64_t        encodeStartUs,
    int64_t        encodeEndUs
)>;

/// NVENC hardware encoder with ultra-low-latency tuning.
///
/// Key improvements over Sunshine:
/// - Adaptive bitrate via NvEncReconfigureEncoder() (no IDR needed)
/// - Frame deadline check before encoding
/// - Reference Frame Invalidation (RFI) for packet-loss recovery
/// - Async encode with event-based completion (no polling)
class NvencEncoder {
public:
    NvencEncoder() = default;
    ~NvencEncoder();

    NvencEncoder(const NvencEncoder&) = delete;
    NvencEncoder& operator=(const NvencEncoder&) = delete;

    struct Config {
        uint32_t   width          = 1920;
        uint32_t   height         = 1080;
        uint32_t   fps            = 60;
        uint32_t   bitrateKbps    = 20000;
        VideoCodec codec          = VideoCodec::HEVC;
        uint8_t    slicesPerFrame = 1;
        bool       enableRfi      = true;  // Reference Frame Invalidation
    };

    /// Initialize encoder. Must be called with the same D3D11 device used for capture.
    bool Init(ID3D11Device* device, const Config& config, EncodeDoneCallback callback);

    /// Encode a frame. The texture must be on the same device passed to Init().
    /// Returns false if encoding should be skipped (deadline miss).
    bool EncodeFrame(ID3D11Texture2D* inputTexture, uint32_t frameNumber, int64_t captureTimeUs);

    /// Force an IDR frame on the next encode
    void ForceIdr();

    /// Invalidate a reference frame (for RFI on packet loss)
    void InvalidateRefFrame(uint32_t frameNumber);

    /// Dynamically adjust bitrate (no IDR needed)
    void SetBitrate(uint32_t bitrateKbps);

    /// Flush and shutdown
    void Shutdown();

    /// Get rolling average encode time in microseconds
    int64_t GetAvgEncodeTimeUs() const { return m_avgEncodeTimeUs; }

private:
    // NVENC API (dynamically loaded)
    HMODULE                       m_nvencLib = nullptr;
#ifdef HAS_NVENC
    NV_ENCODE_API_FUNCTION_LIST   m_nvenc = {};
    void*                         m_encoder = nullptr;

    // D3D11 resources
    ID3D11Device*                 m_device = nullptr;
    NV_ENC_REGISTERED_PTR         m_registeredInput = nullptr;

    // Encode resources
    NV_ENC_INPUT_PTR              m_mappedInput = nullptr;
    NV_ENC_OUTPUT_PTR             m_outputBuffer = nullptr;
    HANDLE                        m_completionEvent = nullptr;
#else
    void*                         m_encoder = nullptr;
    ID3D11Device*                 m_device = nullptr;
    void*                         m_registeredInput = nullptr;
    void*                         m_mappedInput = nullptr;
    void*                         m_outputBuffer = nullptr;
    HANDLE                        m_completionEvent = nullptr;
#endif

    EncodeDoneCallback            m_callback;
    Config                        m_config;

    // Bitstream output buffer
    std::vector<uint8_t>          m_bitstreamBuf;

    // Stats
    int64_t                       m_avgEncodeTimeUs = 0;
    uint32_t                      m_encodeCount = 0;
    bool                          m_forceIdr = false;

    // Frame deadline estimation
    int64_t                       m_estimatedNetworkUs = 0;
    int64_t                       m_estimatedDecodeUs = 0;

    bool LoadNvencApi();
    bool CreateEncoder(ID3D11Device* device);
    bool ConfigureEncoder();
    bool AllocateBuffers();
};

}  // namespace cc::host
