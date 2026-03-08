// Couch Conduit — Video Receiver (Client side)
// Receives UDP/RTP video packets, reassembles frames, signals decoder

#include <couch_conduit/common/transport.h>
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

void VideoReceiver::RecvLoop() {
    std::vector<uint8_t> buf(kMaxPacketSize + 64);
    // TODO: Store FEC packets for recovery
    // std::vector<std::vector<uint8_t>> fecPackets;

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

        // Skip FEC packets for now (TODO: use for recovery)
        if (hdr.payloadType == 97) {
            continue;
        }

        const uint8_t* payload = buf.data() + sizeof(VideoPacketHeader);
        size_t payloadLen = static_cast<size_t>(received) - sizeof(VideoPacketHeader);

        std::lock_guard<std::mutex> lock(m_frameMutex);

        // New frame?
        if (hdr.frameNumber != m_currentFrame.frameNumber || !m_currentFrame.totalPackets) {
            // If we had a partial previous frame, deliver what we have
            if (m_currentFrame.receivedCount > 0 &&
                m_currentFrame.receivedCount < m_currentFrame.totalPackets) {
                CC_WARN("Frame %u incomplete: %u/%u packets",
                        m_currentFrame.frameNumber,
                        m_currentFrame.receivedCount,
                        m_currentFrame.totalPackets);
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
