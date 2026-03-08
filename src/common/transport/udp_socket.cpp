// Couch Conduit — UDP Socket implementation

#include <couch_conduit/common/transport.h>
#include <couch_conduit/common/log.h>
#include <couch_conduit/common/system_tuning.h>

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
