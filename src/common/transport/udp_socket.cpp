// Couch Conduit — UDP Socket implementation

#include <couch_conduit/common/transport.h>
#include <couch_conduit/common/log.h>
#include <couch_conduit/common/system_tuning.h>

#include <cstring>
#include <vector>

namespace cc::transport {

UdpSocket::~UdpSocket() {
    Close();
}

bool UdpSocket::EnsureCreated() {
    if (m_socket != INVALID_SOCKET) return true;

    m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket == INVALID_SOCKET) {
        CC_ERROR("Failed to create UDP socket: %d", WSAGetLastError());
        return false;
    }

    // Apply low-latency tuning
    cc::sys::ConfigureUdpSocket(m_socket);

    return true;
}

bool UdpSocket::Bind(uint16_t port) {
    if (!EnsureCreated()) return false;

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(m_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        CC_ERROR("Failed to bind UDP socket to port %u: %d", port, WSAGetLastError());
        return false;
    }

    m_bound = true;
    CC_INFO("UDP socket bound to port %u", port);
    return true;
}

void UdpSocket::SetRemote(const std::string& host, uint16_t port) {
    EnsureCreated();

    m_remote = {};
    m_remote.sin_family = AF_INET;
    m_remote.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &m_remote.sin_addr);
}

int UdpSocket::Send(const void* data, size_t len) {
    return sendto(m_socket, static_cast<const char*>(data), static_cast<int>(len),
                  0, reinterpret_cast<sockaddr*>(&m_remote), sizeof(m_remote));
}

int UdpSocket::SendTo(const void* data, size_t len, const sockaddr_in& addr) {
    return sendto(m_socket, static_cast<const char*>(data), static_cast<int>(len),
                  0, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
}

int UdpSocket::SendBatch(const std::vector<std::pair<const void*, size_t>>& packets) {
    if (packets.empty()) return 0;

    // Try to use WSASendMsg for each packet with MSG_FASTOPEN-style batching.
    // On Windows, true sendmmsg doesn't exist. We use tight loop with minimal
    // syscall overhead by sending all packets without waiting for completion.
    // The kernel will coalesce them into a single interrupt/submission.
    //
    // For true USO (UDP Segmentation Offload), we'd need UDP_SEND_MSG_SIZE
    // with a single large buffer where the NIC splits into MTU-sized datagrams.
    // That requires all packets to be same-destination & same-size, which fits
    // our video frame fan-out pattern.

    int sent = 0;

    // Attempt USO if all packets are same size (common for video data packets)
    bool allSameSize = true;
    size_t firstLen = packets[0].second;
    for (size_t i = 1; i < packets.size(); ++i) {
        if (packets[i].second != firstLen) { allSameSize = false; break; }
    }

    if (allSameSize && packets.size() > 1) {
        // Try UDP_SEND_MSG_SIZE (USO) — available on Windows 10 1903+
        // This tells the NIC to segment a single large send into MSS-sized datagrams
#ifndef UDP_SEND_MSG_SIZE
#define UDP_SEND_MSG_SIZE 2
#endif
        DWORD segSize = static_cast<DWORD>(firstLen);
        int rc = setsockopt(m_socket, IPPROTO_UDP, UDP_SEND_MSG_SIZE,
                           reinterpret_cast<char*>(&segSize), sizeof(segSize));
        if (rc == 0) {
            // Build one big buffer
            size_t totalLen = firstLen * packets.size();
            std::vector<uint8_t> bigBuf(totalLen);
            for (size_t i = 0; i < packets.size(); ++i) {
                memcpy(bigBuf.data() + i * firstLen,
                       packets[i].first, packets[i].second);
            }

            int result = sendto(m_socket, reinterpret_cast<char*>(bigBuf.data()),
                               static_cast<int>(totalLen), 0,
                               reinterpret_cast<sockaddr*>(&m_remote), sizeof(m_remote));
            if (result > 0) {
                // Reset USO segment size
                segSize = 0;
                setsockopt(m_socket, IPPROTO_UDP, UDP_SEND_MSG_SIZE,
                          reinterpret_cast<char*>(&segSize), sizeof(segSize));
                return static_cast<int>(packets.size());
            }

            // USO failed — fall through to individual sends
            segSize = 0;
            setsockopt(m_socket, IPPROTO_UDP, UDP_SEND_MSG_SIZE,
                      reinterpret_cast<char*>(&segSize), sizeof(segSize));
        }
    }

    // Fallback: tight loop of individual sends
    for (auto& [data, len] : packets) {
        int result = sendto(m_socket, static_cast<const char*>(data),
                            static_cast<int>(len), 0,
                            reinterpret_cast<sockaddr*>(&m_remote), sizeof(m_remote));
        if (result > 0) ++sent;
    }

    return sent;
}

int UdpSocket::Recv(void* buf, size_t maxLen, sockaddr_in* fromAddr) {
    sockaddr_in from = {};
    int fromLen = sizeof(from);

    int result = recvfrom(m_socket, static_cast<char*>(buf), static_cast<int>(maxLen),
                          0, reinterpret_cast<sockaddr*>(&from), &fromLen);

    if (fromAddr) *fromAddr = from;
    return result;
}

void UdpSocket::SetRecvTimeout(int ms) {
    DWORD timeout = static_cast<DWORD>(ms);
    setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<char*>(&timeout), sizeof(timeout));
}

void UdpSocket::Close() {
    if (m_socket != INVALID_SOCKET) {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }
    m_bound = false;
}

}  // namespace cc::transport
