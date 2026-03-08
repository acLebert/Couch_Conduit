// Couch Conduit — STUN Client implementation (RFC 5389)

#include <couch_conduit/common/stun.h>
#include <couch_conduit/common/log.h>

#include <cstring>
#include <random>

#pragma comment(lib, "ws2_32.lib")

namespace cc::net {

const std::vector<std::string>& StunClient::DefaultServers() {
    static std::vector<std::string> servers = {
        "stun.l.google.com",
        "stun1.l.google.com",
        "stun2.l.google.com",
        "stun3.l.google.com",
        "stun4.l.google.com",
    };
    return servers;
}

StunResult StunClient::Discover(const std::string& stunServer,
                                 uint16_t stunPort,
                                 SOCKET localSocket,
                                 int timeoutMs) {
    StunResult result;

    // Resolve STUN server address
    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    addrinfo* addrResult = nullptr;
    char portStr[16];
    snprintf(portStr, sizeof(portStr), "%u", stunPort);

    if (getaddrinfo(stunServer.c_str(), portStr, &hints, &addrResult) != 0) {
        CC_WARN("STUN: Failed to resolve %s", stunServer.c_str());
        return result;
    }

    sockaddr_in serverAddr = {};
    std::memcpy(&serverAddr, addrResult->ai_addr, sizeof(serverAddr));
    freeaddrinfo(addrResult);

    // Create socket if not provided
    bool ownSocket = (localSocket == INVALID_SOCKET);
    SOCKET sock = localSocket;
    if (ownSocket) {
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCKET) {
            CC_ERROR("STUN: Failed to create socket");
            return result;
        }
    }

    // Set timeout
    DWORD timeout = static_cast<DWORD>(timeoutMs);
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<char*>(&timeout), sizeof(timeout));

    // Build STUN Binding Request
    StunHeader request = {};
    request.type = htons(kStunBindingRequest);
    request.length = 0;  // No attributes
    request.magicCookie = htonl(kStunFullMagicCookie);

    // Random transaction ID
    std::random_device rd;
    for (int i = 0; i < 12; ++i) {
        request.transactionId[i] = static_cast<uint8_t>(rd() & 0xFF);
    }

    // Send
    int sent = sendto(sock, reinterpret_cast<char*>(&request), sizeof(request), 0,
                      reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr));
    if (sent != sizeof(request)) {
        CC_WARN("STUN: Failed to send binding request to %s:%u", stunServer.c_str(), stunPort);
        if (ownSocket) closesocket(sock);
        return result;
    }

    // Receive response
    uint8_t buf[512];
    sockaddr_in fromAddr = {};
    int fromLen = sizeof(fromAddr);
    int received = recvfrom(sock, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                            reinterpret_cast<sockaddr*>(&fromAddr), &fromLen);

    if (ownSocket) closesocket(sock);

    if (received < static_cast<int>(sizeof(StunHeader))) {
        CC_WARN("STUN: No response from %s:%u (timeout or error)", stunServer.c_str(), stunPort);
        return result;
    }

    // Parse response header
    StunHeader response;
    std::memcpy(&response, buf, sizeof(response));
    response.type = ntohs(response.type);
    response.length = ntohs(response.length);

    if (response.type != kStunBindingResponse) {
        CC_WARN("STUN: Unexpected response type 0x%04X", response.type);
        return result;
    }

    // Verify transaction ID matches
    if (std::memcmp(response.transactionId, request.transactionId, 12) != 0) {
        CC_WARN("STUN: Transaction ID mismatch");
        return result;
    }

    // Parse attributes
    size_t offset = sizeof(StunHeader);
    size_t end = sizeof(StunHeader) + response.length;
    if (end > static_cast<size_t>(received)) end = static_cast<size_t>(received);

    // Keep the network-order header for XOR operations
    StunHeader netHeader;
    std::memcpy(&netHeader, buf, sizeof(netHeader));

    while (offset + 4 <= end) {
        uint16_t attrType;
        uint16_t attrLen;
        std::memcpy(&attrType, buf + offset, 2);
        std::memcpy(&attrLen, buf + offset + 2, 2);
        attrType = ntohs(attrType);
        attrLen = ntohs(attrLen);
        offset += 4;

        if (offset + attrLen > end) break;

        if (attrType == kAttrXorMappedAddress) {
            if (ParseXorMappedAddress(buf + offset, attrLen, netHeader, result)) {
                result.success = true;
                CC_INFO("STUN: Public address = %s:%u (via %s)",
                        result.publicIp.c_str(), result.publicPort, stunServer.c_str());
                return result;
            }
        } else if (attrType == kAttrMappedAddress) {
            if (ParseMappedAddress(buf + offset, attrLen, result)) {
                result.success = true;
                CC_INFO("STUN: Public address = %s:%u (MAPPED-ADDRESS via %s)",
                        result.publicIp.c_str(), result.publicPort, stunServer.c_str());
                return result;
            }
        }

        // Align to 4-byte boundary
        offset += attrLen;
        if (attrLen % 4) offset += (4 - attrLen % 4);
    }

    CC_WARN("STUN: No mapped address found in response from %s", stunServer.c_str());
    return result;
}

bool StunClient::ParseXorMappedAddress(const uint8_t* data, size_t len,
                                        const StunHeader& netHeader,
                                        StunResult& result) {
    if (len < 8) return false;

    uint8_t family = data[1];
    if (family != 0x01) return false;  // Only IPv4

    uint16_t xPort;
    std::memcpy(&xPort, data + 2, 2);
    xPort = ntohs(xPort) ^ static_cast<uint16_t>(kStunMagicCookie);
    result.publicPort = xPort;

    uint32_t xAddr;
    std::memcpy(&xAddr, data + 4, 4);
    xAddr ^= netHeader.magicCookie;

    struct in_addr addr;
    addr.s_addr = xAddr;
    char ipStr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr, ipStr, sizeof(ipStr));
    result.publicIp = ipStr;

    return true;
}

bool StunClient::ParseMappedAddress(const uint8_t* data, size_t len,
                                     StunResult& result) {
    if (len < 8) return false;

    uint8_t family = data[1];
    if (family != 0x01) return false;  // Only IPv4

    uint16_t port;
    std::memcpy(&port, data + 2, 2);
    result.publicPort = ntohs(port);

    struct in_addr addr;
    std::memcpy(&addr.s_addr, data + 4, 4);
    char ipStr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr, ipStr, sizeof(ipStr));
    result.publicIp = ipStr;

    return true;
}

StunResult StunClient::DiscoverAny(SOCKET localSocket, int timeoutMs) {
    for (auto& server : DefaultServers()) {
        StunResult r = Discover(server, 19302, localSocket, timeoutMs);
        if (r.success) return r;
    }
    CC_WARN("STUN: All servers failed");
    return StunResult{};
}

}  // namespace cc::net
