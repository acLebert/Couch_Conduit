// Couch Conduit — Audio Sender (Host side)
// Sends raw PCM audio samples via UDP to the client

#include <couch_conduit/common/transport.h>
#include <couch_conduit/common/log.h>

#include <cstring>

namespace cc::transport {

#pragma pack(push, 1)
struct AudioPacketHeader {
    uint32_t sequence   = 0;
    uint32_t sampleRate = 0;
    uint16_t channels   = 0;
    uint16_t frameCount = 0;   // Number of audio frames in this packet
};
#pragma pack(pop)

bool AudioSender::Init(const std::string& clientHost, uint16_t clientPort) {
    m_socket.SetRemote(clientHost, clientPort);
    CC_INFO("AudioSender initialized → %s:%u", clientHost.c_str(), clientPort);
    return true;
}

void AudioSender::SetEncryptionKey(const uint8_t key[16]) {
    m_crypto = std::make_unique<crypto::AesGcm>();
    if (!m_crypto->Init(key)) {
        CC_ERROR("Failed to init AES-GCM for AudioSender");
        m_crypto.reset();
    }
}

void AudioSender::SendAudio(const float* samples, uint32_t frameCount,
                             uint32_t sampleRate, uint32_t channels) {
    // Split into MTU-safe chunks (~300 frames per packet at 48kHz stereo)
    const uint32_t maxFramesPerPacket = (kMaxPacketSize - sizeof(AudioPacketHeader) -
                                         crypto::AesGcm::kTagSize) /
                                        (channels * sizeof(float));

    uint32_t offset = 0;
    while (offset < frameCount) {
        uint32_t chunk = std::min(maxFramesPerPacket, frameCount - offset);

        AudioPacketHeader hdr;
        hdr.sequence   = m_sequence++;
        hdr.sampleRate = sampleRate;
        hdr.channels   = static_cast<uint16_t>(channels);
        hdr.frameCount = static_cast<uint16_t>(chunk);

        const uint8_t* pcmData = reinterpret_cast<const uint8_t*>(samples + offset * channels);
        size_t pcmLen = chunk * channels * sizeof(float);

        if (m_crypto) {
            uint8_t nonce[12];
            crypto::AesGcm::BuildNonce(nonce, 0x41554449,  // 'AUDI'
                                       hdr.sequence, 0);
            std::vector<uint8_t> encrypted(pcmLen + crypto::AesGcm::kTagSize);
            size_t encLen = m_crypto->Encrypt(nonce, pcmData, pcmLen,
                                              encrypted.data(), encrypted.size());
            if (encLen > 0) {
                std::vector<uint8_t> packet(sizeof(hdr) + encLen);
                std::memcpy(packet.data(), &hdr, sizeof(hdr));
                std::memcpy(packet.data() + sizeof(hdr), encrypted.data(), encLen);
                m_socket.Send(packet.data(), packet.size());
                offset += chunk;
                continue;
            }
        }

        // Plaintext fallback
        std::vector<uint8_t> packet(sizeof(hdr) + pcmLen);
        std::memcpy(packet.data(), &hdr, sizeof(hdr));
        std::memcpy(packet.data() + sizeof(hdr), pcmData, pcmLen);
        m_socket.Send(packet.data(), packet.size());

        offset += chunk;
    }
}

void AudioSender::Shutdown() {
    m_socket.Close();
}

}  // namespace cc::transport
