#pragma once
// Couch Conduit — Common types and constants
// Shared between host and client

#include <cstdint>
#include <chrono>
#include <string>
#include <array>

namespace cc {

// ─── Version ───────────────────────────────────────────────────────────
inline constexpr uint32_t kVersionMajor = 0;
inline constexpr uint32_t kVersionMinor = 1;
inline constexpr uint32_t kVersionPatch = 0;

// ─── Network ───────────────────────────────────────────────────────────
inline constexpr uint16_t kDefaultControlPort  = 47100;  // TCP
inline constexpr uint16_t kDefaultVideoPort    = 47101;  // UDP
inline constexpr uint16_t kDefaultAudioPort    = 47102;  // UDP
inline constexpr uint16_t kDefaultInputPort    = 47103;  // UDP
inline constexpr uint16_t kDefaultFeedbackPort = 47104;  // UDP

inline constexpr size_t   kMaxPacketSize       = 1400;   // MTU-safe
inline constexpr size_t   kSocketBufferSize    = 2 * 1024 * 1024;  // 2 MB

// ─── Video ─────────────────────────────────────────────────────────────
enum class VideoCodec : uint8_t {
    H264  = 0,
    HEVC  = 1,
    AV1   = 2,
};

struct VideoConfig {
    uint32_t   width          = 1920;
    uint32_t   height         = 1080;
    uint32_t   fps            = 60;
    uint32_t   bitrateKbps    = 20000;  // 20 Mbps default
    VideoCodec codec          = VideoCodec::HEVC;
    uint8_t    slicesPerFrame = 1;
    bool       hdr            = false;
};

// ─── Timing ────────────────────────────────────────────────────────────
using SteadyClock  = std::chrono::steady_clock;
using TimePoint    = SteadyClock::time_point;
using Microseconds = std::chrono::microseconds;
using Milliseconds = std::chrono::milliseconds;

/// High-resolution timestamp in microseconds since epoch
inline int64_t NowUsec() {
    return std::chrono::duration_cast<Microseconds>(
        SteadyClock::now().time_since_epoch()
    ).count();
}

// ─── Frame ─────────────────────────────────────────────────────────────
struct FrameMetadata {
    uint32_t frameNumber     = 0;
    int64_t  captureTimeUs   = 0;  // Host capture timestamp
    int64_t  encodeStartUs   = 0;  // Host encode start
    int64_t  encodeEndUs     = 0;  // Host encode end
    int64_t  sendTimeUs      = 0;  // Host network send
    int64_t  recvTimeUs      = 0;  // Client network receive (first packet)
    int64_t  decodeStartUs   = 0;  // Client decode start
    int64_t  decodeEndUs     = 0;  // Client decode end
    int64_t  renderTimeUs    = 0;  // Client present
    bool     isIdr           = false;
    uint16_t totalPackets    = 0;
    uint16_t fecPackets      = 0;
};

// ─── Input ─────────────────────────────────────────────────────────────
enum class InputMessageType : uint8_t {
    GamepadState         = 0x01,
    MouseRelativeMotion  = 0x02,
    MouseAbsolutePos     = 0x03,
    MouseButton          = 0x04,
    KeyboardKey          = 0x05,
    MouseScroll          = 0x06,
    ControllerConnected  = 0x10,
    ControllerDisconnect = 0x11,
    HapticFeedback       = 0x20,  // Host → Client
};

struct GamepadState {
    uint8_t  controllerId = 0;
    uint16_t buttons      = 0;  // Bitmask
    int16_t  leftStickX   = 0;
    int16_t  leftStickY   = 0;
    int16_t  rightStickX  = 0;
    int16_t  rightStickY  = 0;
    uint8_t  leftTrigger  = 0;
    uint8_t  rightTrigger = 0;
};

// Button bitmask values
namespace Button {
    inline constexpr uint16_t A             = 0x0001;
    inline constexpr uint16_t B             = 0x0002;
    inline constexpr uint16_t X             = 0x0004;
    inline constexpr uint16_t Y             = 0x0008;
    inline constexpr uint16_t DPadUp        = 0x0010;
    inline constexpr uint16_t DPadDown      = 0x0020;
    inline constexpr uint16_t DPadLeft      = 0x0040;
    inline constexpr uint16_t DPadRight     = 0x0080;
    inline constexpr uint16_t LeftShoulder  = 0x0100;
    inline constexpr uint16_t RightShoulder = 0x0200;
    inline constexpr uint16_t LeftStick     = 0x0400;
    inline constexpr uint16_t RightStick    = 0x0800;
    inline constexpr uint16_t Start         = 0x1000;
    inline constexpr uint16_t Back          = 0x2000;
    inline constexpr uint16_t Guide         = 0x4000;
}

// ─── Statistics ────────────────────────────────────────────────────────
struct StreamStats {
    // Host-side
    float captureTimeMs    = 0;
    float encodeTimeMs     = 0;
    float hostSendTimeMs   = 0;
    uint32_t encodedFps    = 0;
    uint32_t bitrateKbps   = 0;

    // Network
    float rttMs            = 0;
    float packetLossRate   = 0;  // 0.0 – 1.0
    float fecOverhead      = 0;  // 0.0 – 1.0

    // Client-side
    float decodeTimeMs     = 0;
    float renderTimeMs     = 0;
    float pacerDelayMs     = 0;
    uint32_t decodedFps    = 0;
    uint32_t renderedFps   = 0;
    uint32_t droppedFrames = 0;

    // End-to-end
    float totalLatencyMs   = 0;  // Capture → Present
};

}  // namespace cc
