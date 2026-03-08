// Couch Conduit — Video Receiver (Client side)
// Receives UDP/RTP video packets, reassembles frames, signals decoder

#include <couch_conduit/common/transport.h>
#include <couch_conduit/common/reed_solomon.h>
#include <couch_conduit/common/log.h>

#include <cstring>

namespace cc::transport {

VideoReceiver::~VideoReceiver() {
    Stop();
}

bool VideoReceiver::Start(uint16_t port, FrameCallback callback) {
    m_callback = std::move(callback);

    if (!m_socket.Bind(port)) {
        return false;
    }
    m_socket.SetRecvTimeout(100);  // 100ms timeout for clean shutdown

    // Create event for signaling decoder thread
    m_frameReady = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_frameReady) {
        CC_ERROR("Failed to create frame ready event");
        return false;
    }

    m_running = true;
    m_recvThread = std::thread([this]() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        RecvLoop();
    });

    CC_INFO("VideoReceiver started on port %u", port);
    return true;
}

void VideoReceiver::SetEncryptionKey(const uint8_t key[16]) {
    m_crypto = std::make_unique<crypto::AesGcm>();
    if (!m_crypto->Init(key)) {
        CC_ERROR("Failed to init AES-GCM for VideoReceiver");
        m_crypto.reset();
    }
}

void VideoReceiver::RecvLoop() {
    std::vector<uint8_t> buf(kMaxPacketSize + 64);

    // RS FEC state per frame: store parity shards grouped by fecGroupId
    struct RsFecState {
        int dataShardCount = 0;
        int parityShardCount = 0;
        size_t shardSize = 0;
        uint8_t groupId = 0;
        bool isRs = false;
        std::vector<std::vector<uint8_t>> parityShards;  // indexed by fecIndex
        std::vector<bool> parityPresent;
    };
    std::vector<RsFecState> fecStates(16);  // Circular buffer by groupId

    while (m_running) {
        int received = m_socket.Recv(buf.data(), buf.size());
        if (received <= 0) {
            if (received == SOCKET_ERROR && WSAGetLastError() == WSAETIMEDOUT) {
                continue;  // Timeout, check m_running
            }
            continue;
        }

        if (received < static_cast<int>(sizeof(VideoPacketHeader))) {
            continue;  // Runt packet
        }

        int64_t recvTime = NowUsec();

        VideoPacketHeader hdr;
        std::memcpy(&hdr, buf.data(), sizeof(hdr));

        const uint8_t* payload = buf.data() + sizeof(VideoPacketHeader);
        size_t payloadLen = static_cast<size_t>(received) - sizeof(VideoPacketHeader);

        // Decrypt payload if encryption is enabled
        std::vector<uint8_t> decryptedBuf;
        if (m_crypto && hdr.payloadType != 97) {  // Don't decrypt FEC packets yet
            uint8_t nonce[12];
            crypto::AesGcm::BuildNonce(nonce, hdr.ssrc,
                                       static_cast<uint32_t>(hdr.frameNumber),
                                       static_cast<uint32_t>(hdr.sequence));
            decryptedBuf.resize(payloadLen);
            size_t decLen = m_crypto->Decrypt(nonce, payload, payloadLen,
                                              decryptedBuf.data(), decryptedBuf.size());
            if (decLen > 0) {
                payload = decryptedBuf.data();
                payloadLen = decLen;
            } else {
                CC_WARN("Video packet decryption failed — seq=%u", hdr.sequence);
                continue;
            }
        }

        // Store FEC packets for potential recovery
        if (hdr.payloadType == 97) {
            size_t gIdx = hdr.fecGroupId % 16;
            auto& fs = fecStates[gIdx];

            bool isRs = (hdr.flags & 0x04) != 0;
            if (isRs) {
                // RS FEC: totalPackets=k, packetIndex=m, fecIndex=parity shard index
                int k = hdr.totalPackets;
                int m = hdr.packetIndex;
                int parityIdx = hdr.fecIndex;

                fs.dataShardCount = k;
                fs.parityShardCount = m;
                fs.groupId = hdr.fecGroupId;
                fs.isRs = true;
                fs.shardSize = std::max(fs.shardSize, payloadLen);

                if (static_cast<int>(fs.parityShards.size()) < m) {
                    fs.parityShards.resize(m);
                    fs.parityPresent.resize(m, false);
                }
                if (parityIdx < m) {
                    fs.parityShards[parityIdx].assign(payload, payload + payloadLen);
                    fs.parityPresent[parityIdx] = true;
                }
            }
            continue;
        }

        std::lock_guard<std::mutex> lock(m_frameMutex);

        // New frame?
        if (hdr.frameNumber != m_currentFrame.frameNumber || !m_currentFrame.totalPackets) {
            // If we had a partial previous frame, try RS FEC recovery then deliver
            if (m_currentFrame.receivedCount > 0 &&
                m_currentFrame.receivedCount < m_currentFrame.totalPackets) {

                TryRsFecRecovery(fecStates);

                if (m_currentFrame.receivedCount == m_currentFrame.totalPackets) {
                    TryAssembleFrame();
                } else {
                    CC_WARN("Frame %u incomplete: %u/%u packets",
                            m_currentFrame.frameNumber,
                            m_currentFrame.receivedCount,
                            m_currentFrame.totalPackets);
                }
            }

            // Start tracking new frame
            m_currentFrame = PendingFrame{};
            m_currentFrame.frameNumber = hdr.frameNumber;
            m_currentFrame.totalPackets = hdr.totalPackets;
            m_currentFrame.firstPacketTime = recvTime;
            m_currentFrame.hostProcTime = hdr.hostProcTime;
            m_currentFrame.isIdr = (hdr.flags & 0x01) != 0;
            m_currentFrame.packets.resize(hdr.totalPackets);
            m_currentFrame.received.resize(hdr.totalPackets, false);
        }

        // Store packet
        uint8_t idx = hdr.packetIndex;
        if (idx < m_currentFrame.totalPackets && !m_currentFrame.received[idx]) {
            m_currentFrame.packets[idx].assign(payload, payload + payloadLen);
            m_currentFrame.received[idx] = true;
            m_currentFrame.receivedCount++;
        }

        // Frame complete?
        if (m_currentFrame.receivedCount == m_currentFrame.totalPackets) {
            TryAssembleFrame();
        }
    }
}

template<typename T>
void VideoReceiver::TryRsFecRecovery(T& fecStates) {
    // Find the FEC state matching the current frame's fecGroupId
    // The current frame's packets were tagged with fecGroupId in the header.
    // We scan all stored FEC states to find a matching one.

    for (auto& fs : fecStates) {
        if (!fs.isRs || fs.dataShardCount == 0) continue;

        int k = fs.dataShardCount;
        int m = fs.parityShardCount;

        // Check our current frame has the right number of data slots
        if (k != m_currentFrame.totalPackets) continue;

        // Count missing data shards
        int missingData = 0;
        for (int i = 0; i < k; ++i) {
            if (!m_currentFrame.received[i]) ++missingData;
        }
        if (missingData == 0) continue;  // Nothing to recover

        // Count available parity shards
        int availableParity = 0;
        for (int i = 0; i < m && i < static_cast<int>(fs.parityPresent.size()); ++i) {
            if (fs.parityPresent[i]) ++availableParity;
        }

        // RS can recover if (data present + parity present) >= k
        int dataPresent = k - missingData;
        if (dataPresent + availableParity < k) {
            CC_WARN("RS: not enough shards for frame %u: data=%d/%d parity=%d/%d",
                    m_currentFrame.frameNumber, dataPresent, k, availableParity, m);
            continue;
        }

        // Build shards array: first k = data, next m = parity
        int n = k + m;
        size_t shardSize = fs.shardSize;

        // Ensure shardSize covers all data packets too
        for (int i = 0; i < k; ++i) {
            if (m_currentFrame.received[i]) {
                shardSize = std::max(shardSize, m_currentFrame.packets[i].size());
            }
        }

        std::vector<std::vector<uint8_t>> shards(n);
        std::vector<bool> present(n, false);

        // Data shards
        for (int i = 0; i < k; ++i) {
            if (m_currentFrame.received[i]) {
                shards[i].resize(shardSize, 0);
                std::memcpy(shards[i].data(),
                           m_currentFrame.packets[i].data(),
                           m_currentFrame.packets[i].size());
                present[i] = true;
            }
        }

        // Parity shards
        for (int i = 0; i < m && i < static_cast<int>(fs.parityShards.size()); ++i) {
            if (fs.parityPresent[i]) {
                shards[k + i].resize(shardSize, 0);
                std::memcpy(shards[k + i].data(),
                           fs.parityShards[i].data(),
                           std::min(fs.parityShards[i].size(), shardSize));
                present[k + i] = true;
            }
        }

        // Attempt RS decode
        fec::ReedSolomon rs(k, m);
        if (rs.Decode(shards, present, shardSize)) {
            // Copy recovered data shards back
            for (int i = 0; i < k; ++i) {
                if (!m_currentFrame.received[i]) {
                    m_currentFrame.packets[i] = std::move(shards[i]);
                    m_currentFrame.received[i] = true;
                    m_currentFrame.receivedCount++;
                    CC_DEBUG("RS recovered packet %d of frame %u", i, m_currentFrame.frameNumber);
                }
            }
        } else {
            CC_WARN("RS decode failed for frame %u (k=%d m=%d)",
                    m_currentFrame.frameNumber, k, m);
        }

        // Mark used
        fs.isRs = false;
        fs.dataShardCount = 0;
        break;
    }
}

void VideoReceiver::TryAssembleFrame() {
    // Concatenate all packets into a single bitstream
    size_t totalSize = 0;
    for (auto& pkt : m_currentFrame.packets) {
        totalSize += pkt.size();
    }

    std::vector<uint8_t> frameData;
    frameData.reserve(totalSize);
    for (auto& pkt : m_currentFrame.packets) {
        frameData.insert(frameData.end(), pkt.begin(), pkt.end());
    }

    // Build metadata
    FrameMetadata meta;
    meta.frameNumber = m_currentFrame.frameNumber;
    meta.recvTimeUs = m_currentFrame.firstPacketTime;
    meta.isIdr = m_currentFrame.isIdr;
    meta.totalPackets = m_currentFrame.totalPackets;

    // Deliver frame
    if (m_callback) {
        m_callback(m_currentFrame.frameNumber, frameData.data(), frameData.size(), meta);
    }

    // Signal decoder thread
    if (m_frameReady) {
        SetEvent(m_frameReady);
    }

    CC_TRACE("Frame %u assembled: %zu bytes from %u packets",
             m_currentFrame.frameNumber, totalSize, m_currentFrame.totalPackets);

    // Reset for next frame
    m_currentFrame = PendingFrame{};
}

void VideoReceiver::Stop() {
    m_running = false;
    if (m_recvThread.joinable()) {
        m_recvThread.join();
    }
    m_socket.Close();
    if (m_frameReady) {
        CloseHandle(m_frameReady);
        m_frameReady = nullptr;
    }
}

}  // namespace cc::transport
