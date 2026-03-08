#pragma once
// Couch Conduit — TCP session negotiation + ECDH key exchange
//
// Protocol:
//   1. Host listens on TCP port 47100
//   2. Client connects
//   3. Client sends CLIENT_HELLO (version + ECDH P-256 public key)
//   4. Host sends HOST_HELLO (version + ECDH P-256 public key + stream config)
//   5. Both derive shared secret via ECDH → HKDF → AES-128-GCM session key
//   6. Client sends SESSION_READY (HMAC proving key possession)
//   7. Streaming begins over UDP with the negotiated key

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#include <bcrypt.h>

#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include <functional>

#include <couch_conduit/common/types.h>

#pragma comment(lib, "bcrypt.lib")

namespace cc::protocol {

// ─── Wire protocol ─────────────────────────────────────────────────────
enum class SessionMsgType : uint32_t {
    ClientHello   = 0x01,
    HostHello     = 0x02,
    SessionReady  = 0x03,
    SessionReject = 0xFF,
};

#pragma pack(push, 1)
struct SessionMsgHeader {
    uint32_t msgType = 0;
    uint32_t payloadLen = 0;
};

struct ClientHelloPayload {
    uint32_t version;                // CC version (major << 16 | minor << 8 | patch)
    uint8_t  ecdhPublicKey[72];      // BCRYPT_ECCPUBLIC_BLOB (8 header + 32 X + 32 Y)
    uint8_t  supportedCodecs;        // Bitmask: bit0=H264, bit1=HEVC, bit2=AV1
};

struct HostHelloPayload {
    uint32_t version;
    uint8_t  ecdhPublicKey[72];
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t bitrateKbps;
    uint8_t  codec;                  // Chosen VideoCodec
    uint16_t videoPort;
    uint16_t audioPort;
    uint16_t inputPort;
};

struct SessionReadyPayload {
    uint8_t keyProof[32];            // HMAC-SHA256("CouchConduit-Ready", sessionKey)
};
#pragma pack(pop)

// ─── Session configuration (output of negotiation) ─────────────────────
struct SessionConfig {
    std::array<uint8_t, 16> sessionKey{};  // AES-128-GCM key
    uint32_t width       = 1920;
    uint32_t height      = 1080;
    uint32_t fps         = 60;
    uint32_t bitrateKbps = 20000;
    VideoCodec codec     = VideoCodec::HEVC;
    uint16_t videoPort   = kDefaultVideoPort;
    uint16_t audioPort   = kDefaultAudioPort;
    uint16_t inputPort   = kDefaultInputPort;
    std::string peerAddr;  // Remote IP address
};

// ─── Callback for client connection (host-side, for multi-client) ──────
using ClientConnectedCallback = std::function<void(const SessionConfig& config,
                                                    const std::string& clientAddr)>;

// ─── Session class ─────────────────────────────────────────────────────
class Session {
public:
    Session() = default;
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    /// Host mode: listen on TCP, wait for client, negotiate
    /// Blocks until a client connects and negotiation completes.
    /// hostConfig provides the stream parameters the host wants to offer.
    bool HostListen(uint16_t tcpPort, const SessionConfig& hostConfig);

    /// Accept the next client connection (call after HostListen)
    /// Returns true when a client successfully completes negotiation.
    bool AcceptClient(SessionConfig& outConfig, int timeoutMs = 30000);

    /// Client mode: connect to host, negotiate
    bool ConnectToHost(const std::string& hostAddr, uint16_t tcpPort,
                       SessionConfig& outConfig, int timeoutMs = 10000);

    /// Close all sockets
    void Close();

private:
    SOCKET m_listenSocket = INVALID_SOCKET;
    SOCKET m_clientSocket = INVALID_SOCKET;

    // ECDH key pair (P-256)
    BCRYPT_ALG_HANDLE m_ecdhAlg = nullptr;
    BCRYPT_KEY_HANDLE m_ecdhKey = nullptr;
    std::vector<uint8_t> m_publicKeyBlob;  // 72 bytes

    bool GenerateEcdhKeyPair();
    bool DeriveSessionKey(const uint8_t* peerPublicBlob, size_t peerBlobLen,
                          std::array<uint8_t, 16>& outKey);
    bool ComputeKeyProof(const std::array<uint8_t, 16>& key, uint8_t outProof[32]);
    bool VerifyKeyProof(const std::array<uint8_t, 16>& key, const uint8_t proof[32]);
    void CleanupEcdh();

    // TCP helpers
    bool SendMsg(SOCKET sock, SessionMsgType type, const void* payload, uint32_t payloadLen);
    bool RecvMsg(SOCKET sock, SessionMsgHeader& hdr, std::vector<uint8_t>& payload, int timeoutMs);
    static std::string GetPeerAddr(SOCKET sock);
};

}  // namespace cc::protocol
