// Couch Conduit — NVENC hardware encoder implementation
//
// This is the second stage of the host pipeline:
//   DXGI Capture → [NVENC Encode] → Transport → Client
//
// Key design decisions:
// 1. NVENC loaded dynamically — no link-time dependency on nvEncodeAPI64.dll
// 2. D3D11 texture input — zero-copy from DXGI capture
// 3. Ultra-low-latency tuning: P1 preset, CBR, zero-latency, no B-frames
// 4. Async encoding with event-based completion (no polling)
// 5. Frame deadline checking — skip encode if we'd miss the deadline
// 6. Reference Frame Invalidation (RFI) for packet-loss recovery
// 7. Dynamic bitrate via NvEncReconfigureEncoder (no IDR needed)

#include <couch_conduit/host/encoder.h>
#include <couch_conduit/common/log.h>

#include <cstring>
#include <algorithm>

#ifdef HAS_NVENC

// NVENC DLL entry point signatures
typedef NVENCSTATUS(NVENCAPI* NvEncodeAPICreateInstanceFn)(NV_ENCODE_API_FUNCTION_LIST*);
typedef NVENCSTATUS(NVENCAPI* NvEncodeAPIGetMaxSupportedVersionFn)(uint32_t*);

namespace cc::host {

NvencEncoder::~NvencEncoder() {
    Shutdown();
}

bool NvencEncoder::Init(ID3D11Device* device, const Config& config, EncodeDoneCallback callback) {
    m_device = device;
    m_config = config;
    m_callback = std::move(callback);

    if (!LoadNvencApi()) {
        CC_ERROR("Failed to load NVENC API");
        return false;
    }

    if (!CreateEncoder(device)) {
        CC_ERROR("Failed to create NVENC encoder");
        return false;
    }

    if (!ConfigureEncoder()) {
        CC_ERROR("Failed to configure NVENC encoder");
        return false;
    }

    if (!AllocateBuffers()) {
        CC_ERROR("Failed to allocate NVENC buffers");
        return false;
    }

    // Force IDR on the first frame
    m_forceIdr = true;

    CC_INFO("NVENC encoder initialized: %ux%u @ %u fps, %u kbps, codec=%s, RFI=%s",
            config.width, config.height, config.fps, config.bitrateKbps,
            config.codec == VideoCodec::HEVC ? "HEVC" :
            config.codec == VideoCodec::AV1  ? "AV1"  : "H.264",
            config.enableRfi ? "on" : "off");
    return true;
}

bool NvencEncoder::LoadNvencApi() {
    // Load NVENC DLL dynamically — no link-time dependency
    m_nvencLib = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (!m_nvencLib) {
        CC_ERROR("nvEncodeAPI64.dll not found — is an NVIDIA GPU installed?");
        return false;
    }

    // Check max supported version
    auto getMaxVer = reinterpret_cast<NvEncodeAPIGetMaxSupportedVersionFn>(
        GetProcAddress(m_nvencLib, "NvEncodeAPIGetMaxSupportedVersion"));
    if (getMaxVer) {
        uint32_t maxVer = 0;
        NVENCSTATUS status = getMaxVer(&maxVer);
        if (status == NV_ENC_SUCCESS) {
            uint32_t major = maxVer >> 4;
            uint32_t minor = maxVer & 0xF;
            CC_INFO("NVENC driver supports API %u.%u", major, minor);
        }
    }

    // Get the function pointer table
    auto createInstance = reinterpret_cast<NvEncodeAPICreateInstanceFn>(
        GetProcAddress(m_nvencLib, "NvEncodeAPICreateInstance"));
    if (!createInstance) {
        CC_ERROR("NvEncodeAPICreateInstance not found in nvEncodeAPI64.dll");
        return false;
    }

    m_nvenc = {};
    m_nvenc.version = NV_ENCODE_API_FUNCTION_LIST_VER;

    NVENCSTATUS status = createInstance(&m_nvenc);
    if (status != NV_ENC_SUCCESS) {
        CC_ERROR("NvEncodeAPICreateInstance failed: %d", status);
        return false;
    }

    CC_INFO("NVENC API loaded successfully");
    return true;
}

bool NvencEncoder::CreateEncoder(ID3D11Device* device) {
    // Open encode session with D3D11 device
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sessionParams = {};
    sessionParams.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    sessionParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    sessionParams.device = device;
    sessionParams.apiVersion = NVENCAPI_VERSION;

    NVENCSTATUS status = m_nvenc.nvEncOpenEncodeSessionEx(&sessionParams, &m_encoder);
    if (status != NV_ENC_SUCCESS) {
        CC_ERROR("nvEncOpenEncodeSessionEx failed: %d", status);
        return false;
    }

    CC_INFO("NVENC encode session opened");
    return true;
}

bool NvencEncoder::ConfigureEncoder() {
    // Select codec GUID
    GUID codecGuid;
    GUID profileGuid;
    switch (m_config.codec) {
        case VideoCodec::H264:
            codecGuid = NV_ENC_CODEC_H264_GUID;
            profileGuid = NV_ENC_H264_PROFILE_HIGH_GUID;
            break;
        case VideoCodec::HEVC:
            codecGuid = NV_ENC_CODEC_HEVC_GUID;
            profileGuid = NV_ENC_HEVC_PROFILE_MAIN_GUID;
            break;
        case VideoCodec::AV1:
            codecGuid = NV_ENC_CODEC_AV1_GUID;
            profileGuid = NV_ENC_AV1_PROFILE_MAIN_GUID;
            break;
        default:
            codecGuid = NV_ENC_CODEC_H264_GUID;
            profileGuid = NV_ENC_H264_PROFILE_HIGH_GUID;
            break;
    }
    (void)profileGuid;  // Used implicitly by preset config

    // Get preset config as starting point
    NV_ENC_PRESET_CONFIG presetConfig = {};
    presetConfig.version = NV_ENC_PRESET_CONFIG_VER;
    presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;

    NVENCSTATUS status = m_nvenc.nvEncGetEncodePresetConfigEx(
        m_encoder,
        codecGuid,
        NV_ENC_PRESET_P1_GUID,  // P1 = fastest encode (lowest latency)
        NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY,
        &presetConfig
    );
    if (status != NV_ENC_SUCCESS) {
        CC_ERROR("nvEncGetEncodePresetConfigEx failed: %d", status);
        return false;
    }

    // Initialize encoder with ultra-low-latency settings
    NV_ENC_INITIALIZE_PARAMS initParams = {};
    initParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
    initParams.encodeGUID = codecGuid;
    initParams.presetGUID = NV_ENC_PRESET_P1_GUID;
    initParams.encodeWidth = m_config.width;
    initParams.encodeHeight = m_config.height;
    initParams.darWidth = m_config.width;
    initParams.darHeight = m_config.height;
    initParams.frameRateNum = m_config.fps;
    initParams.frameRateDen = 1;
    initParams.enablePTD = 1;          // Picture type decision by NVENC
    initParams.reportSliceOffsets = 0;
    initParams.enableSubFrameWrite = 0;
    initParams.maxEncodeWidth = m_config.width;
    initParams.maxEncodeHeight = m_config.height;
    initParams.enableEncodeAsync = 1;  // Async encoding for non-blocking
    initParams.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;

    // Copy and customize the encode config
    NV_ENC_CONFIG encodeConfig = presetConfig.presetCfg;
    initParams.encodeConfig = &encodeConfig;

    // Rate control: Constant Bitrate for predictable network usage
    encodeConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    encodeConfig.rcParams.averageBitRate = m_config.bitrateKbps * 1000;
    encodeConfig.rcParams.maxBitRate = m_config.bitrateKbps * 1000;
    encodeConfig.rcParams.vbvBufferSize = m_config.bitrateKbps * 1000 / m_config.fps;
    encodeConfig.rcParams.vbvInitialDelay = encodeConfig.rcParams.vbvBufferSize;
    encodeConfig.rcParams.multiPass = NV_ENC_MULTI_PASS_DISABLED;
    encodeConfig.rcParams.zeroReorderDelay = 1;  // No reorder delay
    encodeConfig.rcParams.enableNonRefP = 1;     // Allow non-ref P-frames for RFI

    // No B-frames for low latency
    encodeConfig.frameIntervalP = 1;  // I and P frames only

    // GOP: infinite (no periodic IDR — we use ForceIdr when needed)
    encodeConfig.gopLength = NVENC_INFINITE_GOPLENGTH;

    // Codec-specific settings
    if (m_config.codec == VideoCodec::H264) {
        auto& h264 = encodeConfig.encodeCodecConfig.h264Config;
        h264.repeatSPSPPS = 1;             // Include SPS/PPS with each IDR
        h264.idrPeriod = NVENC_INFINITE_GOPLENGTH;
        h264.sliceMode = 3;                // Slices per frame
        h264.sliceModeData = m_config.slicesPerFrame;
        h264.disableSPSPPS = 0;
        h264.outputAUD = 0;
        // Single slice, single NALU for lowest latency
        if (m_config.slicesPerFrame <= 1) {
            h264.sliceMode = 0;
            h264.sliceModeData = 0;
        }
    } else if (m_config.codec == VideoCodec::HEVC) {
        auto& hevc = encodeConfig.encodeCodecConfig.hevcConfig;
        hevc.repeatSPSPPS = 1;
        hevc.idrPeriod = NVENC_INFINITE_GOPLENGTH;
        hevc.sliceMode = 3;
        hevc.sliceModeData = m_config.slicesPerFrame;
        if (m_config.slicesPerFrame <= 1) {
            hevc.sliceMode = 0;
            hevc.sliceModeData = 0;
        }
    }

    // Initialize
    status = m_nvenc.nvEncInitializeEncoder(m_encoder, &initParams);
    if (status != NV_ENC_SUCCESS) {
        CC_ERROR("nvEncInitializeEncoder failed: %d", status);
        return false;
    }

    CC_INFO("NVENC encoder configured: P1 preset, ultra-low-latency, CBR %u kbps",
            m_config.bitrateKbps);
    return true;
}

bool NvencEncoder::AllocateBuffers() {
    // Create async completion event
    m_completionEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_completionEvent) {
        CC_ERROR("Failed to create completion event");
        return false;
    }

    // Register the completion event with NVENC
    NV_ENC_EVENT_PARAMS eventParams = {};
    eventParams.version = NV_ENC_EVENT_PARAMS_VER;
    eventParams.completionEvent = m_completionEvent;

    NVENCSTATUS status = m_nvenc.nvEncRegisterAsyncEvent(m_encoder, &eventParams);
    if (status != NV_ENC_SUCCESS) {
        CC_ERROR("nvEncRegisterAsyncEvent failed: %d", status);
        return false;
    }

    // Create output bitstream buffer
    NV_ENC_CREATE_BITSTREAM_BUFFER createBitstreamBuf = {};
    createBitstreamBuf.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;

    status = m_nvenc.nvEncCreateBitstreamBuffer(m_encoder, &createBitstreamBuf);
    if (status != NV_ENC_SUCCESS) {
        CC_ERROR("nvEncCreateBitstreamBuffer failed: %d", status);
        return false;
    }
    m_outputBuffer = createBitstreamBuf.bitstreamBuffer;

    CC_INFO("NVENC buffers allocated (async mode)");
    return true;
}

bool NvencEncoder::EncodeFrame(ID3D11Texture2D* inputTexture, uint32_t frameNumber,
                                int64_t captureTimeUs) {
    if (!m_encoder) return false;

    int64_t encodeStartUs = cc::NowUsec();

    // Frame deadline check: skip if we'd miss the display deadline
    int64_t frameIntervalUs = 1000000 / m_config.fps;
    int64_t timeSinceCapture = encodeStartUs - captureTimeUs;
    int64_t deadlineUs = frameIntervalUs - m_estimatedNetworkUs - m_estimatedDecodeUs;

    if (deadlineUs > 0 && timeSinceCapture > deadlineUs && m_encodeCount > 10) {
        CC_WARN("Skipping frame %u: %.1f ms since capture (deadline: %.1f ms)",
                frameNumber, timeSinceCapture / 1000.0, deadlineUs / 1000.0);
        return false;
    }

    // Register the D3D11 texture as an NVENC input resource
    // We register per-frame because DXGI DDA gives us a different texture each time
    NV_ENC_REGISTER_RESOURCE regResource = {};
    regResource.version = NV_ENC_REGISTER_RESOURCE_VER;
    regResource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    regResource.resourceToRegister = inputTexture;
    regResource.width = m_config.width;
    regResource.height = m_config.height;
    regResource.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;  // DXGI gives us BGRA/ARGB
    regResource.bufferUsage = NV_ENC_INPUT_IMAGE;

    NVENCSTATUS status = m_nvenc.nvEncRegisterResource(m_encoder, &regResource);
    if (status != NV_ENC_SUCCESS) {
        CC_ERROR("nvEncRegisterResource failed: %d", status);
        return false;
    }
    m_registeredInput = regResource.registeredResource;

    // Map the registered resource for encoding
    NV_ENC_MAP_INPUT_RESOURCE mapResource = {};
    mapResource.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    mapResource.registeredResource = m_registeredInput;

    status = m_nvenc.nvEncMapInputResource(m_encoder, &mapResource);
    if (status != NV_ENC_SUCCESS) {
        CC_ERROR("nvEncMapInputResource failed: %d", status);
        m_nvenc.nvEncUnregisterResource(m_encoder, m_registeredInput);
        return false;
    }
    m_mappedInput = mapResource.mappedResource;

    // Set up per-frame encode parameters
    NV_ENC_PIC_PARAMS picParams = {};
    picParams.version = NV_ENC_PIC_PARAMS_VER;
    picParams.inputBuffer = m_mappedInput;
    picParams.bufferFmt = mapResource.mappedBufferFmt;
    picParams.inputWidth = m_config.width;
    picParams.inputHeight = m_config.height;
    picParams.outputBitstream = m_outputBuffer;
    picParams.completionEvent = m_completionEvent;
    picParams.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    picParams.inputTimeStamp = frameNumber;

    // Force IDR if requested
    if (m_forceIdr) {
        picParams.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
        m_forceIdr = false;
    }

    // Submit encode — async, returns immediately
    status = m_nvenc.nvEncEncodePicture(m_encoder, &picParams);
    if (status != NV_ENC_SUCCESS && status != NV_ENC_ERR_NEED_MORE_INPUT) {
        CC_ERROR("nvEncEncodePicture failed: %d", status);
        m_nvenc.nvEncUnmapInputResource(m_encoder, m_mappedInput);
        m_nvenc.nvEncUnregisterResource(m_encoder, m_registeredInput);
        return false;
    }

    // Wait for encode completion (event-based — no polling!)
    // Timeout: 2x frame interval as safety margin
    DWORD waitResult = WaitForSingleObject(m_completionEvent,
                                            static_cast<DWORD>(frameIntervalUs / 500));
    if (waitResult == WAIT_TIMEOUT) {
        CC_WARN("Encode timeout on frame %u", frameNumber);
        m_nvenc.nvEncUnmapInputResource(m_encoder, m_mappedInput);
        m_nvenc.nvEncUnregisterResource(m_encoder, m_registeredInput);
        return false;
    }

    // Lock and read the output bitstream
    NV_ENC_LOCK_BITSTREAM lockBitstream = {};
    lockBitstream.version = NV_ENC_LOCK_BITSTREAM_VER;
    lockBitstream.outputBitstream = m_outputBuffer;

    status = m_nvenc.nvEncLockBitstream(m_encoder, &lockBitstream);
    if (status != NV_ENC_SUCCESS) {
        CC_ERROR("nvEncLockBitstream failed: %d", status);
        m_nvenc.nvEncUnmapInputResource(m_encoder, m_mappedInput);
        m_nvenc.nvEncUnregisterResource(m_encoder, m_registeredInput);
        return false;
    }

    int64_t encodeEndUs = cc::NowUsec();
    bool isIdr = (lockBitstream.pictureType == NV_ENC_PIC_TYPE_IDR);

    // Update rolling average encode time
    int64_t encodeTimeUs = encodeEndUs - encodeStartUs;
    m_encodeCount++;
    m_avgEncodeTimeUs = (m_avgEncodeTimeUs * (m_encodeCount - 1) + encodeTimeUs) / m_encodeCount;
    if (m_encodeCount > 120) m_encodeCount = 120;  // Rolling window

    CC_TRACE("Encoded frame %u: %u bytes, %s, %.2f ms (avg %.2f ms)",
             frameNumber, lockBitstream.bitstreamSizeInBytes,
             isIdr ? "IDR" : "P",
             encodeTimeUs / 1000.0, m_avgEncodeTimeUs / 1000.0);

    // Deliver encoded bitstream to transport layer
    if (m_callback) {
        m_callback(frameNumber,
                   static_cast<const uint8_t*>(lockBitstream.bitstreamBufferPtr),
                   lockBitstream.bitstreamSizeInBytes,
                   isIdr, encodeStartUs, encodeEndUs);
    }

    // Unlock and cleanup for this frame
    m_nvenc.nvEncUnlockBitstream(m_encoder, m_outputBuffer);
    m_nvenc.nvEncUnmapInputResource(m_encoder, m_mappedInput);
    m_nvenc.nvEncUnregisterResource(m_encoder, m_registeredInput);
    m_mappedInput = nullptr;
    m_registeredInput = nullptr;

    return true;
}

void NvencEncoder::ForceIdr() {
    m_forceIdr = true;
    CC_INFO("IDR requested on next frame");
}

void NvencEncoder::InvalidateRefFrame(uint32_t frameNumber) {
    if (!m_encoder || !m_config.enableRfi) return;

    // nv-codec-headers API: nvEncInvalidateRefFrames(encoder, timestamp)
    NVENCSTATUS status = m_nvenc.nvEncInvalidateRefFrames(m_encoder, static_cast<uint64_t>(frameNumber));
    if (status != NV_ENC_SUCCESS) {
        CC_WARN("nvEncInvalidateRefFrames failed: %d — falling back to IDR", status);
        ForceIdr();
    } else {
        CC_DEBUG("Invalidated reference frame %u (RFI)", frameNumber);
    }
}

void NvencEncoder::SetBitrate(uint32_t bitrateKbps) {
    if (!m_encoder || bitrateKbps == m_config.bitrateKbps) return;

    // Dynamic bitrate change via NvEncReconfigureEncoder — no IDR needed!
    // This is one of NVENC's best features for adaptive streaming.
    NV_ENC_RECONFIGURE_PARAMS reconfig = {};
    reconfig.version = NV_ENC_RECONFIGURE_PARAMS_VER;

    NV_ENC_INITIALIZE_PARAMS& reinitParams = reconfig.reInitEncodeParams;
    reinitParams.version = NV_ENC_INITIALIZE_PARAMS_VER;

    // Select codec GUID
    GUID codecGuid;
    switch (m_config.codec) {
        case VideoCodec::H264: codecGuid = NV_ENC_CODEC_H264_GUID; break;
        case VideoCodec::HEVC: codecGuid = NV_ENC_CODEC_HEVC_GUID; break;
        case VideoCodec::AV1:  codecGuid = NV_ENC_CODEC_AV1_GUID;  break;
        default:               codecGuid = NV_ENC_CODEC_H264_GUID;  break;
    }

    reinitParams.encodeGUID = codecGuid;
    reinitParams.presetGUID = NV_ENC_PRESET_P1_GUID;
    reinitParams.encodeWidth = m_config.width;
    reinitParams.encodeHeight = m_config.height;
    reinitParams.darWidth = m_config.width;
    reinitParams.darHeight = m_config.height;
    reinitParams.frameRateNum = m_config.fps;
    reinitParams.frameRateDen = 1;
    reinitParams.enablePTD = 1;
    reinitParams.enableEncodeAsync = 1;
    reinitParams.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;

    // Get preset config as base for the new config
    NV_ENC_PRESET_CONFIG presetCfg = {};
    presetCfg.version = NV_ENC_PRESET_CONFIG_VER;
    presetCfg.presetCfg.version = NV_ENC_CONFIG_VER;
    m_nvenc.nvEncGetEncodePresetConfigEx(m_encoder, codecGuid,
        NV_ENC_PRESET_P1_GUID, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &presetCfg);

    NV_ENC_CONFIG newConfig = presetCfg.presetCfg;
    reinitParams.encodeConfig = &newConfig;

    // Apply new bitrate
    newConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    newConfig.rcParams.averageBitRate = bitrateKbps * 1000;
    newConfig.rcParams.maxBitRate = bitrateKbps * 1000;
    newConfig.rcParams.vbvBufferSize = bitrateKbps * 1000 / m_config.fps;
    newConfig.rcParams.vbvInitialDelay = newConfig.rcParams.vbvBufferSize;
    newConfig.frameIntervalP = 1;
    newConfig.gopLength = NVENC_INFINITE_GOPLENGTH;

    reconfig.resetEncoder = 0;    // Don't reset — seamless transition
    reconfig.forceIDR = 0;        // No IDR needed for bitrate change

    NVENCSTATUS status = m_nvenc.nvEncReconfigureEncoder(m_encoder, &reconfig);
    if (status == NV_ENC_SUCCESS) {
        uint32_t oldBitrate = m_config.bitrateKbps;
        m_config.bitrateKbps = bitrateKbps;
        CC_INFO("Bitrate adjusted: %u -> %u kbps (no IDR)", oldBitrate, bitrateKbps);
    } else {
        CC_WARN("nvEncReconfigureEncoder failed: %d", status);
    }
}

void NvencEncoder::Shutdown() {
    if (!m_encoder) {
        if (m_nvencLib) { FreeLibrary(m_nvencLib); m_nvencLib = nullptr; }
        if (m_completionEvent) { CloseHandle(m_completionEvent); m_completionEvent = nullptr; }
        return;
    }

    // Flush encoder (send EOS)
    NV_ENC_PIC_PARAMS eosParams = {};
    eosParams.version = NV_ENC_PIC_PARAMS_VER;
    eosParams.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
    eosParams.completionEvent = m_completionEvent;
    m_nvenc.nvEncEncodePicture(m_encoder, &eosParams);
    if (m_completionEvent) {
        WaitForSingleObject(m_completionEvent, 500);
    }

    // Destroy output bitstream buffer
    if (m_outputBuffer) {
        m_nvenc.nvEncDestroyBitstreamBuffer(m_encoder, m_outputBuffer);
        m_outputBuffer = nullptr;
    }

    // Unregister async event
    if (m_completionEvent) {
        NV_ENC_EVENT_PARAMS eventParams = {};
        eventParams.version = NV_ENC_EVENT_PARAMS_VER;
        eventParams.completionEvent = m_completionEvent;
        m_nvenc.nvEncUnregisterAsyncEvent(m_encoder, &eventParams);

        CloseHandle(m_completionEvent);
        m_completionEvent = nullptr;
    }

    // Destroy encoder session
    m_nvenc.nvEncDestroyEncoder(m_encoder);
    m_encoder = nullptr;

    // Unload NVENC DLL
    if (m_nvencLib) {
        FreeLibrary(m_nvencLib);
        m_nvencLib = nullptr;
    }

    CC_INFO("NVENC encoder shut down");
}

}  // namespace cc::host

#else  // !HAS_NVENC

// ─── Stub implementation when NVENC headers are not available ──────────
namespace cc::host {

NvencEncoder::~NvencEncoder() { Shutdown(); }

bool NvencEncoder::Init(ID3D11Device* device, const Config& config, EncodeDoneCallback callback) {
    CC_ERROR("NVENC not available at compile time.");
    CC_ERROR("To enable: install nv-codec-headers or NVENC SDK.");
    CC_ERROR("Run: git clone --depth 1 https://github.com/FFmpeg/nv-codec-headers.git third_party/nv-codec-headers");
    (void)device; (void)config; (void)callback;
    return false;
}

bool NvencEncoder::EncodeFrame(ID3D11Texture2D* tex, uint32_t fn, int64_t ts) {
    (void)tex; (void)fn; (void)ts;
    return false;
}

void NvencEncoder::ForceIdr() {}
void NvencEncoder::InvalidateRefFrame(uint32_t) {}
void NvencEncoder::SetBitrate(uint32_t) {}

void NvencEncoder::Shutdown() {
    if (m_nvencLib) { FreeLibrary(m_nvencLib); m_nvencLib = nullptr; }
    if (m_completionEvent) { CloseHandle(m_completionEvent); m_completionEvent = nullptr; }
}

bool NvencEncoder::LoadNvencApi() { return false; }
bool NvencEncoder::CreateEncoder(ID3D11Device*) { return false; }
bool NvencEncoder::ConfigureEncoder() { return false; }
bool NvencEncoder::AllocateBuffers() { return false; }

}  // namespace cc::host

#endif  // HAS_NVENC
