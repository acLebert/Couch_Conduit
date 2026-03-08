// Couch Conduit — Audio Receiver (Client side)
// Receives PCM audio samples via UDP and delivers to audio player

#include <couch_conduit/common/transport.h>
#include <couch_conduit/common/log.h>

#include <cstring>

namespace cc::transport {

#pragma pack(push, 1)
struct AudioPacketHeader {
    uint32_t sequence   = 0;
    uint32_t sampleRate = 0;
    uint16_t channels   = 0;
    uint16_t frameCount = 0;
};
#pragma pack(pop)

bool AudioReceiver::Start(uint16_t port, AudioCallback callback) {
    m_callback = std::move(callback);

    if (!m_socket.Bind(port)) {
        return false;
    }
    m_socket.SetRecvTimeout(100);

    m_running = true;
    m_recvThread = std::thread([this]() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        RecvLoop();
    });

    CC_INFO("AudioReceiver started on port %u", port);
    return true;
}

void AudioReceiver::SetEncryptionKey(const uint8_t key[16]) {
    m_crypto = std::make_unique<crypto::AesGcm>();
    if (!m_crypto->Init(key)) {
        CC_ERROR("Failed to init AES-GCM for AudioReceiver");
        m_crypto.reset();
    }
}

void AudioReceiver::RecvLoop() {
    std::vector<uint8_t> buf(kMaxPacketSize + 64);

    while (m_running) {
        int received = m_socket.Recv(buf.data(), buf.size());
        if (received <= 0) {
            if (received == SOCKET_ERROR && WSAGetLastError() == WSAETIMEDOUT) {
                continue;
            }
            continue;
        }

        if (received < static_cast<int>(sizeof(AudioPacketHeader))) {
            continue;
        }

        AudioPacketHeader hdr;
        std::memcpy(&hdr, buf.data(), sizeof(hdr));

        const uint8_t* payload = buf.data() + sizeof(AudioPacketHeader);
        size_t payloadLen = static_cast<size_t>(received) - sizeof(AudioPacketHeader);

        // Decrypt if encryption enabled
        std::vector<uint8_t> decryptedBuf;
        if (m_crypto && payloadLen > 0) {
            uint8_t nonce[12];
            crypto::AesGcm::BuildNonce(nonce, 0x41554449,  // 'AUDI'
                                       hdr.sequence, 0);
            decryptedBuf.resize(payloadLen);
            size_t decLen = m_crypto->Decrypt(nonce, payload, payloadLen,
                                              decryptedBuf.data(), decryptedBuf.size());
            if (decLen > 0) {
                payload = decryptedBuf.data();
                payloadLen = decLen;
            } else {
                CC_WARN("Audio packet decryption failed — seq=%u", hdr.sequence);
                continue;
            }
        }

        // Deliver PCM samples
        size_t expectedLen = hdr.frameCount * hdr.channels * sizeof(float);
        if (payloadLen >= expectedLen && m_callback) {
            m_callback(reinterpret_cast<const float*>(payload),
                       hdr.frameCount, hdr.sampleRate, hdr.channels);
        }
    }
}

void AudioReceiver::Stop() {
    m_running = false;
    if (m_recvThread.joinable()) {
        m_recvThread.join();
    }
    m_socket.Close();
}

}  // namespace cc::transport
