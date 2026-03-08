// Couch Conduit — Session negotiation protocol (placeholder)
// TCP-based session establishment with TLS

#include <couch_conduit/common/types.h>
#include <couch_conduit/common/log.h>

namespace cc::protocol {

// TODO: Implement TCP session negotiation
// 1. TLS 1.3 handshake (self-signed cert, pinned)
// 2. Exchange capabilities (codecs, resolution, controllers)
// 3. Derive AES-128-GCM session keys via HKDF
// 4. Exchange UDP port assignments
// 5. Switch to UDP for streaming

class Session {
public:
    bool StartHost(uint16_t tcpPort) {
        CC_INFO("Session host starting on port %u (TODO: implement)", tcpPort);
        return true;
    }

    bool ConnectToHost(const char* hostAddr, uint16_t tcpPort) {
        CC_INFO("Connecting to host %s:%u (TODO: implement)", hostAddr, tcpPort);
        return true;
    }
};

}  // namespace cc::protocol
