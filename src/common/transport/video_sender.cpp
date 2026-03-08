// Couch Conduit — Video Sender (Host side)
// Packetizes encoded frames and adds Reed-Solomon FEC

#include <couch_conduit/common/transport.h>
#include <couch_conduit/common/reed_solomon.h>
#include <couch_conduit/common/log.h>

#include <algorithm>
#include <cstring>
#include <random>

namespace cc::transport {

VideoSender::~VideoSender() {
    Shutdown();
}

bool VideoSender::Init(const std::string& clientHost, uint16_t clientPort) {
    m_socket.SetRemote(clientHost, clientPort);

    // Random SSRC
    std::random_device rd;
    m_ssrc = rd();

    CC_INFO("VideoSender initialized → %s:%u (ssrc=0x%08X)", clientHost.c_str(), clientPort, m_ssrc);
    return true;
}

void VideoSender::SetEncryptionKey(const uint8_t key[16]) {
    m_crypto = std::make_unique<crypto::AesGcm>();
    if (!m_crypto->Init(key)) {
        CC_ERROR("Failed to init AES-GCM for VideoSender");
        m_crypto.reset();
    }
}

int VideoSender::SendFrame(uint32_t frameNumber, const uint8_t* data, size_t dataLen,
                           bool isIdr, uint16_t hostProcTimeUs) {
    // Calculate packet count
    const size_t maxPayload = kMaxPacketSize - sizeof(VideoPacketHeader);
    const uint16_t totalDataPackets = static_cast<uint16_t>((dataLen + maxPayload - 1) / maxPayload);

    // Build data packets
    std::vector<std::vector<uint8_t>> dataPackets;
    dataPackets.reserve(totalDataPackets);

    size_t offset = 0;
    for (uint16_t i = 0; i < totalDataPackets; ++i) {
        size_t chunkLen = std::min(maxPayload, dataLen - offset);

        VideoPacketHeader hdr = {};
        hdr.version       = 2;
        hdr.payloadType   = 96;
        hdr.sequence      = m_sequence++;
        hdr.timestamp     = frameNumber * 1500;  // 90kHz / 60fps = 1500
        hdr.ssrc          = m_ssrc;
        hdr.frameNumber   = static_cast<uint16_t>(frameNumber);
        hdr.packetIndex   = static_cast<uint8_t>(i);
        hdr.totalPackets  = static_cast<uint8_t>(totalDataPackets);
        hdr.hostProcTime  = hostProcTimeUs;
        hdr.fecGroupId    = m_fecGroupId;
        hdr.fecIndex      = static_cast<uint8_t>(i);
        hdr.flags         = (isIdr ? 0x01 : 0x00) |
                           (i == totalDataPackets - 1 ? 0x02 : 0x00);  // End-of-frame

        std::vector<uint8_t> packet(sizeof(VideoPacketHeader) + chunkLen);
        std::memcpy(packet.data(), &hdr, sizeof(hdr));
        std::memcpy(packet.data() + sizeof(hdr), data + offset, chunkLen);

        dataPackets.push_back(std::move(packet));
        offset += chunkLen;
    }

    // Send data packets (batched for lower syscall overhead)
    int sentCount = 0;

    // Build batch of encrypted packets
    for (auto& pkt : dataPackets) {
        if (m_crypto) {
            auto* hdrPtr = reinterpret_cast<VideoPacketHeader*>(pkt.data());
            uint8_t* payload = pkt.data() + sizeof(VideoPacketHeader);
            size_t payloadLen = pkt.size() - sizeof(VideoPacketHeader);

            uint8_t nonce[12];
            crypto::AesGcm::BuildNonce(nonce, m_ssrc,
                                       static_cast<uint32_t>(hdrPtr->frameNumber),
                                       static_cast<uint32_t>(hdrPtr->sequence));

            std::vector<uint8_t> encrypted(payloadLen + crypto::AesGcm::kTagSize);
            size_t encLen = m_crypto->Encrypt(nonce, payload, payloadLen,
                                              encrypted.data(), encrypted.size());
            if (encLen > 0) {
                pkt.resize(sizeof(VideoPacketHeader) + encLen);
                std::memcpy(pkt.data() + sizeof(VideoPacketHeader),
                           encrypted.data(), encLen);
            }
        }
    }

    // Batch send all data packets
    {
        std::vector<std::pair<const void*, size_t>> batch;
        batch.reserve(dataPackets.size());
        for (auto& pkt : dataPackets) {
            batch.emplace_back(pkt.data(), pkt.size());
        }
        sentCount = m_socket.SendBatch(batch);
    }

    // Generate and send FEC packets
    int fecCount = static_cast<int>(totalDataPackets * m_fecRatio);
    if (fecCount > 0 && totalDataPackets > 0) {
        auto fecPackets = GenerateFec(dataPackets);
        for (auto& fecPkt : fecPackets) {
            int sent = m_socket.Send(fecPkt.data(), fecPkt.size());
            if (sent > 0) ++sentCount;
        }
    }

    ++m_fecGroupId;

    CC_TRACE("Sent frame %u: %zu bytes, %u data + %d FEC packets",
             frameNumber, dataLen, totalDataPackets, fecCount);
    return sentCount;
}

std::vector<std::vector<uint8_t>> VideoSender::GenerateFec(
        const std::vector<std::vector<uint8_t>>& dataPackets) {
    std::vector<std::vector<uint8_t>> fecPackets;

    int k = static_cast<int>(dataPackets.size());
    if (k == 0) return fecPackets;

    int m = std::max(1, static_cast<int>(k * m_fecRatio));
    // Clamp to reasonable bounds (RS gets expensive at very high shard counts)
    if (k + m > 255) m = 255 - k;
    if (m <= 0) return fecPackets;

    // Determine uniform shard size (max payload across all packets)
    size_t shardSize = 0;
    for (auto& pkt : dataPackets) {
        size_t payloadLen = pkt.size() - sizeof(VideoPacketHeader);
        shardSize = std::max(shardSize, payloadLen);
    }
    if (shardSize == 0) return fecPackets;

    // Build data shard pointers (payload portion only, after header)
    std::vector<const uint8_t*> dataPtrs(k);
    std::vector<size_t> dataLens(k);
    // We need to provide zero-padded copies so RS sees uniform shard sizes
    std::vector<std::vector<uint8_t>> paddedShards(k);
    for (int i = 0; i < k; ++i) {
        size_t payloadLen = dataPackets[i].size() - sizeof(VideoPacketHeader);
        paddedShards[i].resize(shardSize, 0);
        std::memcpy(paddedShards[i].data(),
                    dataPackets[i].data() + sizeof(VideoPacketHeader),
                    payloadLen);
        dataPtrs[i] = paddedShards[i].data();
        dataLens[i] = shardSize;
    }

    // Encode RS parity
    fec::ReedSolomon rs(k, m);
    std::vector<std::vector<uint8_t>> parityShards;
    if (!rs.Encode(dataPtrs, dataLens, shardSize, parityShards)) {
        CC_WARN("RS encode failed for k=%d m=%d", k, m);
        return fecPackets;
    }

    // Wrap each parity shard in a FEC packet
    fecPackets.reserve(m);
    for (int p = 0; p < m; ++p) {
        VideoPacketHeader hdr = {};
        hdr.version      = 2;
        hdr.payloadType  = 97;  // FEC payload type
        hdr.sequence     = m_sequence++;
        hdr.ssrc         = m_ssrc;
        hdr.fecGroupId   = m_fecGroupId;
        hdr.fecIndex     = static_cast<uint8_t>(p);
        hdr.totalPackets = static_cast<uint8_t>(k);   // data shard count
        hdr.packetIndex  = static_cast<uint8_t>(m);   // parity shard count
        hdr.flags        = 0x04;  // Bit 2: RS FEC (vs legacy XOR)

        std::vector<uint8_t> packet(sizeof(hdr) + parityShards[p].size());
        std::memcpy(packet.data(), &hdr, sizeof(hdr));
        std::memcpy(packet.data() + sizeof(hdr),
                    parityShards[p].data(), parityShards[p].size());

        fecPackets.push_back(std::move(packet));
    }

    return fecPackets;
}

void VideoSender::Shutdown() {
    m_socket.Close();
}

}  // namespace cc::transport
