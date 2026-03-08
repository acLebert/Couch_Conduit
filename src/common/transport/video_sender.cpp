// Couch Conduit — Video Sender (Host side)
// Packetizes encoded frames and adds adaptive FEC

#include <couch_conduit/common/transport.h>
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

    // Send data packets
    int sentCount = 0;
    for (auto& pkt : dataPackets) {
        int sent = m_socket.Send(pkt.data(), pkt.size());
        if (sent > 0) ++sentCount;
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
    // Simple XOR-based FEC for now
    // TODO: Replace with Reed-Solomon for multi-packet recovery
    std::vector<std::vector<uint8_t>> fecPackets;

    int fecCount = std::max(1, static_cast<int>(dataPackets.size() * m_fecRatio));

    // Group data packets and XOR them together for each FEC packet
    int groupSize = std::max(1, static_cast<int>(dataPackets.size()) / fecCount);

    for (int g = 0; g < fecCount; ++g) {
        int start = g * groupSize;
        int end = std::min(start + groupSize, static_cast<int>(dataPackets.size()));
        if (start >= static_cast<int>(dataPackets.size())) break;

        // Find max packet size in this group
        size_t maxLen = 0;
        for (int i = start; i < end; ++i) {
            maxLen = std::max(maxLen, dataPackets[i].size());
        }

        // XOR all packets in the group
        std::vector<uint8_t> fecData(maxLen, 0);
        for (int i = start; i < end; ++i) {
            for (size_t j = 0; j < dataPackets[i].size(); ++j) {
                fecData[j] ^= dataPackets[i][j];
            }
        }

        // Create FEC packet header
        VideoPacketHeader hdr = {};
        hdr.version      = 2;
        hdr.payloadType  = 97;  // FEC payload type
        hdr.sequence     = m_sequence++;
        hdr.ssrc         = m_ssrc;
        hdr.fecGroupId   = m_fecGroupId;
        hdr.fecIndex     = 0xFF;  // Marks this as FEC parity
        hdr.flags        = static_cast<uint8_t>(start);  // Group start index

        std::vector<uint8_t> packet(sizeof(hdr) + fecData.size());
        std::memcpy(packet.data(), &hdr, sizeof(hdr));
        std::memcpy(packet.data() + sizeof(hdr), fecData.data(), fecData.size());

        fecPackets.push_back(std::move(packet));
    }

    return fecPackets;
}

void VideoSender::Shutdown() {
    m_socket.Close();
}

}  // namespace cc::transport
