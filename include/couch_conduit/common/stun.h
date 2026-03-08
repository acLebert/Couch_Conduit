#pragma once
// Couch Conduit — STUN Client for NAT traversal
// Implements RFC 5389 STUN Binding Request to discover public IP:port.
// Used during session negotiation so peers behind NATs can reach each other.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <WinSock2.h>
#include <WS2tcpip.h>

#include <cstdint>
#include <string>
#include <vector>

namespace cc::net {

/// Result of a STUN binding request
struct StunResult {
    bool     success = false;
    std::string publicIp;
    uint16_t    publicPort = 0;
};

/// Minimal STUN client (RFC 5389 Binding Request only)
class StunClient {
public:
    /// Discover public IP:port by querying a STUN server.
    /// @param stunServer  STUN server hostname (e.g., "stun.l.google.com")
    /// @param stunPort    STUN server port (default 19302)
    /// @param localSocket If INVALID_SOCKET, creates a temporary socket.
    ///                    Otherwise, sends from the given socket (useful for
    ///                    discovering the public mapping of an existing socket).
    /// @param timeoutMs   Query timeout in milliseconds
    static StunResult Discover(const std::string& stunServer = "stun.l.google.com",
                               uint16_t stunPort = 19302,
                               SOCKET localSocket = INVALID_SOCKET,
                               int timeoutMs = 3000);

    /// Default STUN servers to try in order
    static const std::vector<std::string>& DefaultServers();

    /// Try multiple STUN servers, return the first successful result
    static StunResult DiscoverAny(SOCKET localSocket = INVALID_SOCKET,
                                   int timeoutMs = 2000);

private:
    // STUN message constants
    static constexpr uint16_t kStunBindingRequest  = 0x0001;
    static constexpr uint16_t kStunBindingResponse  = 0x0101;
    static constexpr uint16_t kStunMagicCookie      = 0x2112;
    static constexpr uint32_t kStunFullMagicCookie  = 0x2112A442;

    // Attribute types
    static constexpr uint16_t kAttrMappedAddress     = 0x0001;
    static constexpr uint16_t kAttrXorMappedAddress  = 0x0020;

    struct StunHeader {
        uint16_t type;
        uint16_t length;
        uint32_t magicCookie;
        uint8_t  transactionId[12];
    };

    static bool ParseXorMappedAddress(const uint8_t* data, size_t len,
                                       const StunHeader& header,
                                       StunResult& result);
    static bool ParseMappedAddress(const uint8_t* data, size_t len,
                                    StunResult& result);
};

}  // namespace cc::net
