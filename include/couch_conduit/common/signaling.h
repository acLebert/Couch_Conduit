#pragma once
// Couch Conduit — Signaling Client
//
// Talks to the signaling server (Cloudflare Worker or self-hosted)
// to create / resolve / delete room codes via HTTP.
// Uses WinHTTP — no external dependencies.

#include <string>
#include <cstdint>

namespace cc::net {

class SignalingClient {
public:
    /// Host: create a room on the signaling server.
    /// Returns true and sets outCode to the 6-char room code.
    static bool CreateRoom(const std::string& serverUrl,
                           const std::string& hostIp,
                           uint16_t hostPort,
                           std::string& outCode);

    /// Client: resolve a room code to a host endpoint.
    static bool ResolveRoom(const std::string& serverUrl,
                            const std::string& code,
                            std::string& outIp,
                            uint16_t& outPort);

    /// Host: delete a room when shutting down.
    static bool DeleteRoom(const std::string& serverUrl,
                           const std::string& code);

    /// Check signaling server health.
    static bool HealthCheck(const std::string& serverUrl);

    /// Get the local machine's primary IPv4 address (e.g. "192.168.1.100")
    static std::string GetLocalIp();
};

}  // namespace cc::net
