// Couch Conduit — Input Receiver (Host side)
// Receives input from client and signals for immediate capture

#include <couch_conduit/common/transport.h>
#include <couch_conduit/common/log.h>

#include <cstring>

namespace cc::transport {

bool InputReceiver::Start(uint16_t port, InputCallback callback) {
    m_callback = std::move(callback);

    if (!m_socket.Bind(port)) {
        return false;
    }
    m_socket.SetRecvTimeout(100);

    // Create event for signaling capture thread
    m_inputArrived = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_inputArrived) {
        CC_ERROR("Failed to create input arrived event");
        return false;
    }

    m_running = true;
    m_recvThread = std::thread([this]() {
        // TIME_CRITICAL priority — this is the fastest path in the system
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        RecvLoop();
    });

    CC_INFO("InputReceiver started on port %u (TIME_CRITICAL priority)", port);
    return true;
}

void InputReceiver::SetEncryptionKey(const uint8_t key[16]) {
    m_crypto = std::make_unique<crypto::AesGcm>();
    if (!m_crypto->Init(key)) {
        CC_ERROR("Failed to init AES-GCM for InputReceiver");
        m_crypto.reset();
    }
}

void InputReceiver::RecvLoop() {
    uint8_t buf[512];  // Input packets are small

    while (m_running) {
        sockaddr_in from;
        int received = m_socket.Recv(buf, sizeof(buf), &from);

        if (received <= 0) {
            if (received == SOCKET_ERROR && WSAGetLastError() == WSAETIMEDOUT) {
                continue;
            }
            continue;
        }

        if (received < static_cast<int>(sizeof(InputPacketHeader))) {
            continue;
        }

        InputPacketHeader hdr;
        std::memcpy(&hdr, buf, sizeof(hdr));

        const uint8_t* payload = buf + sizeof(InputPacketHeader);
        size_t payloadLen = static_cast<size_t>(received) - sizeof(InputPacketHeader);

        // Decrypt if encryption is enabled
        uint8_t decryptedBuf[512];
        if (m_crypto && payloadLen > 0) {
            uint8_t nonce[12];
            crypto::AesGcm::BuildNonce(nonce, 0x494E5054,  // 'INPT'
                                       static_cast<uint32_t>(hdr.controllerId),
                                       static_cast<uint32_t>(hdr.sequence));
            size_t decLen = m_crypto->Decrypt(nonce, payload, payloadLen,
                                              decryptedBuf, sizeof(decryptedBuf));
            if (decLen > 0) {
                payload = decryptedBuf;
                payloadLen = decLen;
            } else {
                // Auth failure — may be plaintext from old client, try anyway
                CC_WARN("Input packet decryption failed — seq=%u", hdr.sequence);
            }
        }

        // CRITICAL: Process input INLINE — no queuing!
        // This is the key latency improvement over Sunshine's task pool approach.
        if (m_callback) {
            m_callback(hdr, payload, payloadLen);
        }

        // Signal capture thread to grab a frame immediately
        if (m_inputArrived) {
            SetEvent(m_inputArrived);
        }
    }
}

void InputReceiver::Stop() {
    m_running = false;
    if (m_recvThread.joinable()) {
        m_recvThread.join();
    }
    m_socket.Close();
    if (m_inputArrived) {
        CloseHandle(m_inputArrived);
        m_inputArrived = nullptr;
    }
}

}  // namespace cc::transport
