// Couch Conduit — D3D11VA hardware decoder implementation
//
// Event-signaled decode: instead of polling with SDL_Delay(2) like Moonlight,
// we use WaitForSingleObject on an event signaled by the VideoReceiver.
// Wake-up latency: < 0.1ms vs Moonlight's 2-15ms.
//
// Zero-copy: Decoded frames stay as D3D11 textures in VRAM. The renderer
// reads them directly without any CPU-side copies.

#include <couch_conduit/client/decoder.h>
#include <couch_conduit/common/log.h>

// Note: Full implementation requires FFmpeg dev libraries
// Install via: vcpkg install ffmpeg[avcodec,avutil]:x64-windows
// Or download from https://github.com/BtbN/FFmpeg-Builds/releases

#ifdef HAS_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/imgutils.h>
}
#endif

namespace cc::client {

D3D11Decoder::~D3D11Decoder() {
    Stop();

#ifdef HAS_FFMPEG
    if (m_frame) av_frame_free(&m_frame);
    if (m_packet) av_packet_free(&m_packet);
    if (m_codecCtx) avcodec_free_context(&m_codecCtx);
    if (m_hwFramesCtx) av_buffer_unref(&m_hwFramesCtx);
    if (m_hwDeviceCtx) av_buffer_unref(&m_hwDeviceCtx);
#endif

    DeleteCriticalSection(&m_pendingLock);
}

bool D3D11Decoder::Init(ID3D11Device* renderDevice, const Config& config,
                         FrameDecodedCallback callback) {
    m_config = config;
    m_callback = std::move(callback);
    m_device = renderDevice;

    InitializeCriticalSectionAndSpinCount(&m_pendingLock, 4000);

    // Create decoder
    if (!CreateDecoder()) {
        CC_ERROR("Failed to create video decoder");
        return false;
    }

    CC_INFO("D3D11VA decoder initialized: %ux%u, codec=%s, RFI=%s",
            config.width, config.height,
            config.codec == VideoCodec::HEVC ? "HEVC" :
            config.codec == VideoCodec::AV1  ? "AV1"  : "H.264",
            config.enableRfi ? "on" : "off");
    return true;
}

bool D3D11Decoder::CreateDecoder() {
#ifdef HAS_FFMPEG
    // Find decoder
    const AVCodec* codec = nullptr;
    switch (m_config.codec) {
        case VideoCodec::H264: codec = avcodec_find_decoder(AV_CODEC_ID_H264); break;
        case VideoCodec::HEVC: codec = avcodec_find_decoder(AV_CODEC_ID_HEVC); break;
        case VideoCodec::AV1:  codec = avcodec_find_decoder(AV_CODEC_ID_AV1);  break;
    }
    if (!codec) {
        CC_ERROR("Failed to find video decoder");
        return false;
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        CC_ERROR("Failed to allocate codec context");
        return false;
    }

    // Ultra-low-latency decode settings (matching Moonlight's approach)
    m_codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;       // No internal buffering
    m_codecCtx->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;   // Show partial frames
    m_codecCtx->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;       // Show all frames
    m_codecCtx->thread_count = 1;                         // Single thread for HW decode
    m_codecCtx->width = m_config.width;
    m_codecCtx->height = m_config.height;

    // Initialize D3D11VA hardware acceleration
    if (!InitHwAccel(m_device.Get())) {
        CC_WARN("D3D11VA init failed — falling back to software decode");
        // Continue without HW accel
    }

    int ret = avcodec_open2(m_codecCtx, codec, nullptr);
    if (ret < 0) {
        CC_ERROR("avcodec_open2 failed: %d", ret);
        return false;
    }

    m_packet = av_packet_alloc();
    m_frame = av_frame_alloc();

    CC_INFO("Video decoder opened: %s, hw=%s",
            codec->name,
            m_codecCtx->hw_device_ctx ? "D3D11VA" : "software");
    return true;
#else
    CC_INFO("FFmpeg not available at compile time — decoder is a stub");
    CC_INFO("To enable: install FFmpeg dev, add -DHAS_FFMPEG=1 to CMake");
    return true;
#endif
}

bool D3D11Decoder::InitHwAccel(ID3D11Device* device) {
#ifdef HAS_FFMPEG
    // Create hardware device context using the shared D3D11 device
    m_hwDeviceCtx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!m_hwDeviceCtx) {
        CC_ERROR("Failed to alloc D3D11VA device context");
        return false;
    }

    auto* deviceCtx = reinterpret_cast<AVHWDeviceContext*>(m_hwDeviceCtx->data);
    auto* d3d11DeviceCtx = reinterpret_cast<AVD3D11VADeviceContext*>(deviceCtx->hwctx);

    // Use the SAME device as the renderer — zero-copy decode→render
    d3d11DeviceCtx->device = device;
    device->AddRef();  // FFmpeg will Release on cleanup

    int ret = av_hwdevice_ctx_init(m_hwDeviceCtx);
    if (ret < 0) {
        CC_ERROR("av_hwdevice_ctx_init failed: %d", ret);
        av_buffer_unref(&m_hwDeviceCtx);
        return false;
    }

    m_codecCtx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
    return true;
#else
    (void)device;
    return false;
#endif
}

bool D3D11Decoder::Start(HANDLE frameReadyEvent) {
    m_frameReadyEvent = frameReadyEvent;
    m_running = true;

    m_decodeThread = std::thread([this]() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        DecodeLoop();
    });

    CC_INFO("Decode thread started (HIGHEST priority, event-signaled)");
    return true;
}

void D3D11Decoder::SubmitFrame(uint32_t frameNumber, const uint8_t* data, size_t len,
                                const FrameMetadata& meta) {
    EnterCriticalSection(&m_pendingLock);
    m_pending.data.assign(data, data + len);
    m_pending.meta = meta;
    m_pending.meta.frameNumber = frameNumber;
    m_pending.ready = true;
    LeaveCriticalSection(&m_pendingLock);
}

void D3D11Decoder::DecodeLoop() {
    while (m_running) {
        // Event-signaled wait — this is the key improvement over Moonlight's SDL_Delay(2)
        // WaitForSingleObject wakes up in < 0.1ms when the event is signaled,
        // vs Moonlight's 2-15ms polling delay.
        DWORD result = WaitForSingleObject(m_frameReadyEvent, 100);

        if (!m_running) break;
        if (result == WAIT_TIMEOUT) continue;

        // Grab pending frame data
        std::vector<uint8_t> frameData;
        FrameMetadata meta;
        bool hasFrame = false;

        EnterCriticalSection(&m_pendingLock);
        if (m_pending.ready) {
            frameData = std::move(m_pending.data);
            meta = m_pending.meta;
            m_pending.ready = false;
            hasFrame = true;
        }
        LeaveCriticalSection(&m_pendingLock);

        if (!hasFrame) continue;

        int64_t decodeStart = cc::NowUsec();
        meta.decodeStartUs = decodeStart;

#ifdef HAS_FFMPEG
        // Decode
        AVFrame* decodedFrame = av_frame_alloc();
        if (DecodeFrame(frameData.data(), frameData.size(), decodedFrame)) {
            int64_t decodeEnd = cc::NowUsec();
            meta.decodeEndUs = decodeEnd;

            // Update stats
            int64_t decodeTimeUs = decodeEnd - decodeStart;
            m_decodeCount++;
            m_avgDecodeTimeUs = (m_avgDecodeTimeUs * (m_decodeCount - 1) + decodeTimeUs) / m_decodeCount;
            if (m_decodeCount > 120) m_decodeCount = 120;

            CC_TRACE("Decoded frame %u in %.2f ms (avg %.2f ms)",
                     meta.frameNumber, decodeTimeUs / 1000.0, m_avgDecodeTimeUs / 1000.0);

            // Deliver to renderer
            if (m_callback) {
                m_callback(decodedFrame, meta);
            }
        } else {
            av_frame_free(&decodedFrame);
            m_needsIdr = true;
            CC_WARN("Decode failed for frame %u — requesting IDR", meta.frameNumber);
        }
#else
        // Stub: just pass metadata through
        meta.decodeEndUs = cc::NowUsec();
        CC_TRACE("Decode stub: frame %u, %zu bytes", meta.frameNumber, frameData.size());
        if (m_callback) {
            m_callback(nullptr, meta);
        }
#endif
    }
}

#ifdef HAS_FFMPEG
bool D3D11Decoder::DecodeFrame(const uint8_t* data, size_t len, AVFrame* outFrame) {
    m_packet->data = const_cast<uint8_t*>(data);
    m_packet->size = static_cast<int>(len);

    int ret = avcodec_send_packet(m_codecCtx, m_packet);
    if (ret < 0) {
        CC_ERROR("avcodec_send_packet failed: %d", ret);
        return false;
    }

    ret = avcodec_receive_frame(m_codecCtx, outFrame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        return false;  // Need more data
    }
    if (ret < 0) {
        CC_ERROR("avcodec_receive_frame failed: %d", ret);
        return false;
    }

    return true;
}
#endif

void D3D11Decoder::Stop() {
    m_running = false;
    if (m_decodeThread.joinable()) {
        m_decodeThread.join();
    }
}

}  // namespace cc::client
