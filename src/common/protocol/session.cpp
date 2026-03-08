// Couch Conduit — TCP session negotiation + ECDH P-256 key exchange
//
// Implements a 3-message handshake:
//   CLIENT_HELLO  → (version, ECDH pubkey, supported codecs)
//   HOST_HELLO    ← (version, ECDH pubkey, stream config)
//   SESSION_READY → (HMAC key proof)
//
// Both sides derive a shared AES-128 key via:
//   ECDH shared secret → HMAC-SHA256("CouchConduit-SessionKey") → first 16 bytes

#include <couch_conduit/common/session.h>
#include <couch_conduit/common/log.h>

#include <cstring>
#include <algorithm>

namespace cc::protocol {

// ─── Lifecycle ─────────────────────────────────────────────────────────

Session::~Session() {
    Close();
}

void Session::Close() {
    CleanupEcdh();
    if (m_clientSocket != INVALID_SOCKET) { closesocket(m_clientSocket); m_clientSocket = INVALID_SOCKET; }
    if (m_listenSocket != INVALID_SOCKET) { closesocket(m_listenSocket); m_listenSocket = INVALID_SOCKET; }
}

// ─── ECDH Key Generation (P-256 via Windows CNG) ──────────────────────

bool Session::GenerateEcdhKeyPair() {
    CleanupEcdh();

    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &m_ecdhAlg, BCRYPT_ECDH_P256_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(status)) {
        CC_ERROR("BCryptOpenAlgorithmProvider(ECDH_P256) failed: 0x%08X", status);
        return false;
    }

    status = BCryptGenerateKeyPair(m_ecdhAlg, &m_ecdhKey, 256, 0);
    if (!BCRYPT_SUCCESS(status)) {
        CC_ERROR("BCryptGenerateKeyPair failed: 0x%08X", status);
        return false;
    }

    status = BCryptFinalizeKeyPair(m_ecdhKey, 0);
    if (!BCRYPT_SUCCESS(status)) {
        CC_ERROR("BCryptFinalizeKeyPair failed: 0x%08X", status);
        return false;
    }

    // Export public key blob (8-byte header + 32-byte X + 32-byte Y = 72 bytes)
    ULONG blobSize = 0;
    status = BCryptExportKey(m_ecdhKey, nullptr, BCRYPT_ECCPUBLIC_BLOB,
                             nullptr, 0, &blobSize, 0);
    if (!BCRYPT_SUCCESS(status) || blobSize == 0) {
        CC_ERROR("BCryptExportKey (size query) failed: 0x%08X", status);
        return false;
    }

    m_publicKeyBlob.resize(blobSize);
    status = BCryptExportKey(m_ecdhKey, nullptr, BCRYPT_ECCPUBLIC_BLOB,
                             m_publicKeyBlob.data(), blobSize, &blobSize, 0);
    if (!BCRYPT_SUCCESS(status)) {
        CC_ERROR("BCryptExportKey failed: 0x%08X", status);
        return false;
    }

    CC_INFO("ECDH P-256 key pair generated (%u byte public blob)", blobSize);
    return true;
}

bool Session::DeriveSessionKey(const uint8_t* peerPublicBlob, size_t peerBlobLen,
                                std::array<uint8_t, 16>& outKey) {
    // Import peer's public key
    BCRYPT_KEY_HANDLE hPeerKey = nullptr;
    NTSTATUS status = BCryptImportKeyPair(
        m_ecdhAlg, nullptr, BCRYPT_ECCPUBLIC_BLOB,
        &hPeerKey,
        const_cast<PUCHAR>(peerPublicBlob),
        static_cast<ULONG>(peerBlobLen), 0);
    if (!BCRYPT_SUCCESS(status)) {
        CC_ERROR("BCryptImportKeyPair (peer) failed: 0x%08X", status);
        return false;
    }

    // Derive shared secret
    BCRYPT_SECRET_HANDLE hSecret = nullptr;
    status = BCryptSecretAgreement(m_ecdhKey, hPeerKey, &hSecret, 0);
    BCryptDestroyKey(hPeerKey);
    if (!BCRYPT_SUCCESS(status)) {
        CC_ERROR("BCryptSecretAgreement failed: 0x%08X", status);
        return false;
    }

    // Derive key via HMAC-SHA256 with label "CouchConduit-SessionKey"
    const wchar_t* kdfHashAlg = BCRYPT_SHA256_ALGORITHM;
    BCryptBuffer kdfParams[2] = {};

    // Parameter 0: Hash algorithm
    kdfParams[0].cbBuffer = static_cast<ULONG>((wcslen(kdfHashAlg) + 1) * sizeof(wchar_t));
    kdfParams[0].BufferType = KDF_HASH_ALGORITHM;
    kdfParams[0].pvBuffer = const_cast<wchar_t*>(kdfHashAlg);

    // Parameter 1: Label
    const char label[] = "CouchConduit-SessionKey";
    kdfParams[1].cbBuffer = sizeof(label);
    kdfParams[1].BufferType = KDF_LABEL;
    kdfParams[1].pvBuffer = const_cast<char*>(label);

    BCryptBufferDesc paramList = {};
    paramList.ulVersion = BCRYPTBUFFER_VERSION;
    paramList.cBuffers = 2;
    paramList.pBuffers = kdfParams;

    uint8_t derivedKey[32] = {};
    ULONG resultSize = 0;
    status = BCryptDeriveKey(hSecret, BCRYPT_KDF_HMAC, &paramList,
                             derivedKey, sizeof(derivedKey), &resultSize, 0);
    BCryptDestroySecret(hSecret);
    if (!BCRYPT_SUCCESS(status)) {
        CC_ERROR("BCryptDeriveKey failed: 0x%08X", status);
        return false;
    }

    // Use first 16 bytes as AES-128 key
    std::memcpy(outKey.data(), derivedKey, 16);
    SecureZeroMemory(derivedKey, sizeof(derivedKey));

    CC_INFO("Session key derived successfully");
    return true;
}

bool Session::ComputeKeyProof(const std::array<uint8_t, 16>& key, uint8_t outProof[32]) {
    // HMAC-SHA256(key, "CouchConduit-Ready")
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM,
                                                  nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (!BCRYPT_SUCCESS(status)) return false;

    BCRYPT_HASH_HANDLE hHash = nullptr;
    status = BCryptCreateHash(hAlg, &hHash, nullptr, 0,
                              const_cast<PUCHAR>(key.data()), 16, 0);
    if (!BCRYPT_SUCCESS(status)) { BCryptCloseAlgorithmProvider(hAlg, 0); return false; }

    const char msg[] = "CouchConduit-Ready";
    BCryptHashData(hHash, reinterpret_cast<PUCHAR>(const_cast<char*>(msg)),
                   sizeof(msg) - 1, 0);
    BCryptFinishHash(hHash, outProof, 32, 0);
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return true;
}

bool Session::VerifyKeyProof(const std::array<uint8_t, 16>& key, const uint8_t proof[32]) {
    uint8_t expected[32];
    if (!ComputeKeyProof(key, expected)) return false;
    bool match = (std::memcmp(proof, expected, 32) == 0);
    SecureZeroMemory(expected, sizeof(expected));
    return match;
}

void Session::CleanupEcdh() {
    if (m_ecdhKey) { BCryptDestroyKey(m_ecdhKey); m_ecdhKey = nullptr; }
    if (m_ecdhAlg) { BCryptCloseAlgorithmProvider(m_ecdhAlg, 0); m_ecdhAlg = nullptr; }
    m_publicKeyBlob.clear();
}

// ─── TCP Helpers ───────────────────────────────────────────────────────

bool Session::SendMsg(SOCKET sock, SessionMsgType type, const void* payload, uint32_t payloadLen) {
    SessionMsgHeader hdr;
    hdr.msgType = static_cast<uint32_t>(type);
    hdr.payloadLen = payloadLen;

    // Send header
    int sent = send(sock, reinterpret_cast<const char*>(&hdr), sizeof(hdr), 0);
    if (sent != sizeof(hdr)) return false;

    // Send payload
    if (payloadLen > 0 && payload) {
        const char* ptr = static_cast<const char*>(payload);
        uint32_t remaining = payloadLen;
        while (remaining > 0) {
            sent = send(sock, ptr, remaining, 0);
            if (sent <= 0) return false;
            ptr += sent;
            remaining -= sent;
        }
    }
    return true;
}

bool Session::RecvMsg(SOCKET sock, SessionMsgHeader& hdr, std::vector<uint8_t>& payload, int timeoutMs) {
    // Set timeout
    DWORD timeout = static_cast<DWORD>(timeoutMs);
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));

    // Receive header
    char* hdrPtr = reinterpret_cast<char*>(&hdr);
    int remaining = sizeof(hdr);
    while (remaining > 0) {
        int received = recv(sock, hdrPtr, remaining, 0);
        if (received <= 0) {
            CC_ERROR("Session recv header failed: %d", WSAGetLastError());
            return false;
        }
        hdrPtr += received;
        remaining -= received;
    }

    // Sanity check
    if (hdr.payloadLen > 4096) {
        CC_ERROR("Session message too large: %u bytes", hdr.payloadLen);
        return false;
    }

    // Receive payload
    payload.resize(hdr.payloadLen);
    if (hdr.payloadLen > 0) {
        char* ptr = reinterpret_cast<char*>(payload.data());
        remaining = hdr.payloadLen;
        while (remaining > 0) {
            int received = recv(sock, ptr, remaining, 0);
            if (received <= 0) {
                CC_ERROR("Session recv payload failed: %d", WSAGetLastError());
                return false;
            }
            ptr += received;
            remaining -= received;
        }
    }
    return true;
}

std::string Session::GetPeerAddr(SOCKET sock) {
    sockaddr_in addr = {};
    int addrLen = sizeof(addr);
    getpeername(sock, reinterpret_cast<sockaddr*>(&addr), &addrLen);
    char buf[64];
    inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf));
    return std::string(buf);
}

// ─── Host Mode ─────────────────────────────────────────────────────────

bool Session::HostListen(uint16_t tcpPort, const SessionConfig&) {
    m_listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_listenSocket == INVALID_SOCKET) {
        CC_ERROR("Failed to create TCP listen socket: %d", WSAGetLastError());
        return false;
    }

    // Allow port reuse
    BOOL reuse = TRUE;
    setsockopt(m_listenSocket, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<char*>(&reuse), sizeof(reuse));

    // Disable Nagle for low latency
    BOOL noDelay = TRUE;
    setsockopt(m_listenSocket, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<char*>(&noDelay), sizeof(noDelay));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(tcpPort);

    if (bind(m_listenSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        CC_ERROR("TCP bind to port %u failed: %d", tcpPort, WSAGetLastError());
        return false;
    }

    if (listen(m_listenSocket, 4) == SOCKET_ERROR) {
        CC_ERROR("TCP listen failed: %d", WSAGetLastError());
        return false;
    }

    CC_INFO("Session host listening on TCP port %u", tcpPort);
    return true;
}

bool Session::AcceptClient(SessionConfig& outConfig, int timeoutMs) {
    // Set accept timeout
    DWORD timeout = static_cast<DWORD>(timeoutMs);
    setsockopt(m_listenSocket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<char*>(&timeout), sizeof(timeout));

    sockaddr_in clientAddr = {};
    int addrLen = sizeof(clientAddr);
    m_clientSocket = accept(m_listenSocket, reinterpret_cast<sockaddr*>(&clientAddr), &addrLen);
    if (m_clientSocket == INVALID_SOCKET) {
        CC_ERROR("TCP accept failed: %d", WSAGetLastError());
        return false;
    }

    // Disable Nagle
    BOOL noDelay = TRUE;
    setsockopt(m_clientSocket, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<char*>(&noDelay), sizeof(noDelay));

    std::string peerAddr = GetPeerAddr(m_clientSocket);
    CC_INFO("Client connected from %s", peerAddr.c_str());

    // Generate our ECDH key pair
    if (!GenerateEcdhKeyPair()) {
        CC_ERROR("ECDH key generation failed");
        return false;
    }

    // 1. Receive CLIENT_HELLO
    SessionMsgHeader hdr;
    std::vector<uint8_t> payload;
    if (!RecvMsg(m_clientSocket, hdr, payload, timeoutMs)) {
        CC_ERROR("Failed to receive CLIENT_HELLO");
        return false;
    }
    if (hdr.msgType != static_cast<uint32_t>(SessionMsgType::ClientHello) ||
        payload.size() < sizeof(ClientHelloPayload)) {
        CC_ERROR("Invalid CLIENT_HELLO");
        return false;
    }

    ClientHelloPayload clientHello;
    std::memcpy(&clientHello, payload.data(), sizeof(clientHello));

    uint32_t clientVer = clientHello.version;
    CC_INFO("Client version: %u.%u.%u, codecs=0x%02X",
            (clientVer >> 16) & 0xFF, (clientVer >> 8) & 0xFF, clientVer & 0xFF,
            clientHello.supportedCodecs);

    // 2. Derive session key from client's public key + our private key
    std::array<uint8_t, 16> sessionKey{};
    if (!DeriveSessionKey(clientHello.ecdhPublicKey, sizeof(clientHello.ecdhPublicKey), sessionKey)) {
        CC_ERROR("Session key derivation failed");
        return false;
    }

    // 3. Send HOST_HELLO
    HostHelloPayload hostHello = {};
    hostHello.version = (kVersionMajor << 16) | (kVersionMinor << 8) | kVersionPatch;
    std::memcpy(hostHello.ecdhPublicKey, m_publicKeyBlob.data(),
                std::min(m_publicKeyBlob.size(), sizeof(hostHello.ecdhPublicKey)));
    hostHello.width = outConfig.width;
    hostHello.height = outConfig.height;
    hostHello.fps = outConfig.fps;
    hostHello.bitrateKbps = outConfig.bitrateKbps;
    hostHello.codec = static_cast<uint8_t>(outConfig.codec);
    hostHello.videoPort = outConfig.videoPort;
    hostHello.audioPort = outConfig.audioPort;
    hostHello.inputPort = outConfig.inputPort;

    if (!SendMsg(m_clientSocket, SessionMsgType::HostHello, &hostHello, sizeof(hostHello))) {
        CC_ERROR("Failed to send HOST_HELLO");
        return false;
    }

    // 4. Receive SESSION_READY with key proof
    if (!RecvMsg(m_clientSocket, hdr, payload, timeoutMs)) {
        CC_ERROR("Failed to receive SESSION_READY");
        return false;
    }
    if (hdr.msgType != static_cast<uint32_t>(SessionMsgType::SessionReady) ||
        payload.size() < sizeof(SessionReadyPayload)) {
        CC_ERROR("Invalid SESSION_READY");
        return false;
    }

    SessionReadyPayload ready;
    std::memcpy(&ready, payload.data(), sizeof(ready));

    if (!VerifyKeyProof(sessionKey, ready.keyProof)) {
        CC_ERROR("Session key proof verification failed — possible MITM or bug");
        return false;
    }

    // Populate output config
    outConfig.sessionKey = sessionKey;
    outConfig.peerAddr = peerAddr;

    CC_INFO("Session negotiated with %s — key exchange complete", peerAddr.c_str());
    return true;
}

// ─── Client Mode ───────────────────────────────────────────────────────

bool Session::ConnectToHost(const std::string& hostAddr, uint16_t tcpPort,
                             SessionConfig& outConfig, int timeoutMs) {
    m_clientSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_clientSocket == INVALID_SOCKET) {
        CC_ERROR("Failed to create TCP socket: %d", WSAGetLastError());
        return false;
    }

    // Disable Nagle
    BOOL noDelay = TRUE;
    setsockopt(m_clientSocket, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<char*>(&noDelay), sizeof(noDelay));

    // Connect with timeout
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(tcpPort);
    inet_pton(AF_INET, hostAddr.c_str(), &addr.sin_addr);

    // Set non-blocking for connect timeout
    u_long nonBlocking = 1;
    ioctlsocket(m_clientSocket, FIONBIO, &nonBlocking);

    int result = connect(m_clientSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (result == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set writeSet;
        FD_ZERO(&writeSet);
        FD_SET(m_clientSocket, &writeSet);
        timeval tv = {};
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;

        result = select(0, nullptr, &writeSet, nullptr, &tv);
        if (result <= 0) {
            CC_ERROR("TCP connect to %s:%u timed out", hostAddr.c_str(), tcpPort);
            return false;
        }

        // Check if connection actually succeeded
        int err = 0;
        int errLen = sizeof(err);
        getsockopt(m_clientSocket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &errLen);
        if (err != 0) {
            CC_ERROR("TCP connect to %s:%u failed: %d", hostAddr.c_str(), tcpPort, err);
            return false;
        }
    } else if (result == SOCKET_ERROR) {
        CC_ERROR("TCP connect to %s:%u failed: %d", hostAddr.c_str(), tcpPort, WSAGetLastError());
        return false;
    }

    // Back to blocking mode
    nonBlocking = 0;
    ioctlsocket(m_clientSocket, FIONBIO, &nonBlocking);

    CC_INFO("TCP connected to %s:%u", hostAddr.c_str(), tcpPort);

    // Generate our ECDH key pair
    if (!GenerateEcdhKeyPair()) {
        CC_ERROR("ECDH key generation failed");
        return false;
    }

    // 1. Send CLIENT_HELLO
    ClientHelloPayload clientHello = {};
    clientHello.version = (kVersionMajor << 16) | (kVersionMinor << 8) | kVersionPatch;
    std::memcpy(clientHello.ecdhPublicKey, m_publicKeyBlob.data(),
                std::min(m_publicKeyBlob.size(), sizeof(clientHello.ecdhPublicKey)));
    clientHello.supportedCodecs = 0x07;  // H264 | HEVC | AV1

    if (!SendMsg(m_clientSocket, SessionMsgType::ClientHello, &clientHello, sizeof(clientHello))) {
        CC_ERROR("Failed to send CLIENT_HELLO");
        return false;
    }

    // 2. Receive HOST_HELLO
    SessionMsgHeader hdr;
    std::vector<uint8_t> payload;
    if (!RecvMsg(m_clientSocket, hdr, payload, timeoutMs)) {
        CC_ERROR("Failed to receive HOST_HELLO");
        return false;
    }
    if (hdr.msgType != static_cast<uint32_t>(SessionMsgType::HostHello) ||
        payload.size() < sizeof(HostHelloPayload)) {
        CC_ERROR("Invalid HOST_HELLO");
        return false;
    }

    HostHelloPayload hostHello;
    std::memcpy(&hostHello, payload.data(), sizeof(hostHello));

    // 3. Derive session key from host's public key + our private key
    std::array<uint8_t, 16> sessionKey{};
    if (!DeriveSessionKey(hostHello.ecdhPublicKey, sizeof(hostHello.ecdhPublicKey), sessionKey)) {
        CC_ERROR("Session key derivation failed");
        return false;
    }

    // 4. Send SESSION_READY with key proof
    SessionReadyPayload ready = {};
    if (!ComputeKeyProof(sessionKey, ready.keyProof)) {
        CC_ERROR("Failed to compute key proof");
        return false;
    }
    if (!SendMsg(m_clientSocket, SessionMsgType::SessionReady, &ready, sizeof(ready))) {
        CC_ERROR("Failed to send SESSION_READY");
        return false;
    }

    // Populate output config from host's offer
    outConfig.sessionKey = sessionKey;
    outConfig.width = hostHello.width;
    outConfig.height = hostHello.height;
    outConfig.fps = hostHello.fps;
    outConfig.bitrateKbps = hostHello.bitrateKbps;
    outConfig.codec = static_cast<VideoCodec>(hostHello.codec);
    outConfig.videoPort = hostHello.videoPort;
    outConfig.audioPort = hostHello.audioPort;
    outConfig.inputPort = hostHello.inputPort;
    outConfig.peerAddr = hostAddr;

    uint32_t hostVer = hostHello.version;
    CC_INFO("Session established with %s — v%u.%u.%u, %ux%u@%ufps %ukbps %s",
            hostAddr.c_str(),
            (hostVer >> 16) & 0xFF, (hostVer >> 8) & 0xFF, hostVer & 0xFF,
            outConfig.width, outConfig.height, outConfig.fps, outConfig.bitrateKbps,
            outConfig.codec == VideoCodec::HEVC ? "HEVC" :
            outConfig.codec == VideoCodec::AV1  ? "AV1"  : "H.264");
    return true;
}

}  // namespace cc::protocol
