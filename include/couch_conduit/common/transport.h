#pragma once
// Couch Conduit — Transport layer: UDP video/audio/input channels

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <WinSock2.h>
#include <WS2tcpip.h>

#include <cstdint>
#include <vector>
#include <functional>
#include <atomic>
#include <thread>
#include <memory>
#include <mutex>

#include <couch_conduit/common/types.h>

#pragma comment(lib, "ws2_32.lib")

namespace cc::transport {

// ─── Packet header for video RTP ───────────────────────────────────────
#pragma pack(push, 1)
struct VideoPacketHeader {
    // Simplified RTP-like header
    uint8_t  version     = 2;       // Always 2
    uint8_t  payloadType = 96;      // Dynamic
    uint16_t sequence    = 0;       // Global packet sequence
    uint32_t timestamp   = 0;       // RTP timestamp (90kHz)
    uint32_t ssrc        = 0;       // Stream ID

    // Couch Conduit extensions
    uint16_t frameNumber   = 0;     // Frame counter
    uint8_t  packetIndex   = 0;     // Index within this frame
    uint8_t  totalPackets  = 0;     // Total data packets in frame
    uint16_t hostProcTime  = 0;     // Capture+encode time in 0.1ms
    uint8_t  fecGroupId    = 0;     // FEC group identifier
    uint8_t  fecIndex      = 0;     // Index within FEC group (0xFF = FEC parity)
    uint8_t  flags         = 0;     // Bit 0: IDR, Bit 1: end-of-frame
};

struct InputPacketHeader {
    InputMessageType msgType      = InputMessageType::GamepadState;
    uint8_t          controllerId = 0;
    uint16_t         sequence     = 0;
    // Followed by encrypted payload + 16-byte GCM tag
};

struct FeedbackPacket {
    uint16_t lastFrameReceived  = 0;
    uint64_t lossMap            = 0;     // Bitmap of received/lost for last 64 pkts
    uint16_t decodeTimeUs       = 0;     // Last frame decode time
    uint16_t renderTimeUs       = 0;     // Last frame render time
    uint8_t  queueDepth         = 0;     // Frames waiting in decode/render queue
    uint8_t  reserved           = 0;
    // Followed by TWCC arrival timestamp array
};
#pragma pack(pop)

// ─── UDP Socket wrapper ────────────────────────────────────────────────
class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    /// Bind to a local port for receiving
    bool Bind(uint16_t port);

    /// Set the remote endpoint for sending
    void SetRemote(const std::string& host, uint16_t port);

    /// Send data to the configured remote
    int Send(const void* data, size_t len);

    /// Send data to a specific address
    int SendTo(const void* data, size_t len, const sockaddr_in& addr);

    /// Receive data (blocking, with timeout)
    int Recv(void* buf, size_t maxLen, sockaddr_in* fromAddr = nullptr);

    /// Set receive timeout in milliseconds
    void SetRecvTimeout(int ms);

    /// Get the underlying socket handle
    SOCKET Handle() const { return m_socket; }

    /// Close the socket
    void Close();

private:
    SOCKET     m_socket = INVALID_SOCKET;
    sockaddr_in m_remote = {};
    bool       m_bound = false;

    bool EnsureCreated();
};

// ─── Video Sender (Host side) ──────────────────────────────────────────
class VideoSender {
public:
    VideoSender() = default;
    ~VideoSender();

    /// Initialize with remote client address
    bool Init(const std::string& clientHost, uint16_t clientPort);

    /// Send an encoded video frame, packetizing and adding FEC
    /// Returns number of packets sent (data + FEC)
    int SendFrame(uint32_t frameNumber, const uint8_t* data, size_t dataLen,
                  bool isIdr, uint16_t hostProcTimeUs);

    /// Set FEC percentage (0.0 to 0.5)
    void SetFecRatio(float ratio) { m_fecRatio = ratio; }

    /// Get current sequence number
    uint16_t GetSequence() const { return m_sequence; }

    void Shutdown();

private:
    UdpSocket  m_socket;
    uint16_t   m_sequence = 0;
    uint32_t   m_ssrc = 0;
    float      m_fecRatio = 0.10f;  // 10% FEC by default
    uint8_t    m_fecGroupId = 0;

    /// Generate Reed-Solomon FEC packets for a set of data packets
    std::vector<std::vector<uint8_t>> GenerateFec(
        const std::vector<std::vector<uint8_t>>& dataPackets);
};

// ─── Video Receiver (Client side) ──────────────────────────────────────
class VideoReceiver {
public:
    using FrameCallback = std::function<void(uint32_t frameNumber,
                                             const uint8_t* data, size_t len,
                                             const FrameMetadata& meta)>;

    VideoReceiver() = default;
    ~VideoReceiver();

    /// Start receiving on the given port
    bool Start(uint16_t port, FrameCallback callback);

    /// Signal that a complete frame is ready (for event-signaled decode)
    HANDLE GetFrameReadyEvent() const { return m_frameReady; }

    void Stop();

private:
    UdpSocket     m_socket;
    std::thread   m_recvThread;
    std::atomic<bool> m_running{false};
    FrameCallback m_callback;
    HANDLE        m_frameReady = nullptr;

    // Frame reassembly state
    struct PendingFrame {
        uint32_t frameNumber = 0;
        uint16_t totalPackets = 0;
        uint16_t receivedCount = 0;
        int64_t  firstPacketTime = 0;
        uint16_t hostProcTime = 0;
        bool     isIdr = false;
        std::vector<std::vector<uint8_t>> packets;  // Indexed by packetIndex
        std::vector<bool> received;
    };

    std::mutex m_frameMutex;
    PendingFrame m_currentFrame;

    void RecvLoop();
    void TryAssembleFrame();

    // FEC recovery support
    struct FecGroup {
        std::vector<uint8_t> parityData;
        uint8_t groupStart = 0;
        uint8_t groupId = 0;
        bool valid = false;
    };
    template<typename T>
    void TryFecRecovery(T& fecGroups);
};

// ─── Input Sender (Client side) ────────────────────────────────────────
class InputSender {
public:
    bool Init(const std::string& hostAddr, uint16_t hostPort);
    void SendGamepadState(const GamepadState& state);
    void SendMouseMotion(int16_t dx, int16_t dy);
    void SendKeyboard(uint16_t vkCode, bool pressed);
    void SendMouseButton(uint8_t button, bool pressed);
    void SendMouseScroll(int16_t deltaX, int16_t deltaY);
    void SendRequestIdr();
    void Shutdown();

private:
    UdpSocket m_socket;
    uint16_t  m_sequence = 0;

    void SendInput(InputMessageType type, uint8_t controllerId,
                   const void* payload, size_t payloadLen);
};

// ─── Input Receiver (Host side) ────────────────────────────────────────
class InputReceiver {
public:
    using InputCallback = std::function<void(const InputPacketHeader& hdr,
                                             const uint8_t* payload, size_t len)>;

    bool Start(uint16_t port, InputCallback callback);
    void Stop();

    /// Event signaled when input arrives — can be used to trigger capture
    HANDLE GetInputArrivedEvent() const { return m_inputArrived; }

private:
    UdpSocket     m_socket;
    std::thread   m_recvThread;
    std::atomic<bool> m_running{false};
    InputCallback m_callback;
    HANDLE        m_inputArrived = nullptr;

    void RecvLoop();
};

// ─── WSA initialization helper ─────────────────────────────────────────
inline bool InitWinsock() {
    WSADATA wsa;
    int err = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (err != 0) {
        return false;
    }
    return true;
}

inline void CleanupWinsock() {
    WSACleanup();
}

}  // namespace cc::transport
