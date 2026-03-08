// Couch Conduit — Host entry point
//
// Usage: cc_host.exe [--client <ip>] [--bitrate <kbps>] [--fps <fps>] [--codec <h264|hevc|av1>]
//
// The host captures the desktop, encodes with NVENC, and streams to the client.
// It also receives input from the client and injects it via ViGEmBus.

#include <couch_conduit/common/types.h>
#include <couch_conduit/common/transport.h>
#include <couch_conduit/common/session.h>
#include <couch_conduit/common/signaling.h>
#include <couch_conduit/common/stun.h>
#include <couch_conduit/common/log.h>
#include <couch_conduit/host/host_session.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <csignal>
#include <atomic>
#include <memory>

static std::atomic<bool> g_running{true};

void SignalHandler(int) {
    g_running = false;
}

struct CliArgs {
    std::string clientHost;  // Empty = wait for TCP session
    uint32_t    bitrateKbps = 20000;
    uint32_t    fps = 60;
    uint32_t    encodeWidth = 0;   // 0 = same as capture
    uint32_t    encodeHeight = 0;
    cc::VideoCodec codec = cc::VideoCodec::HEVC;
    bool        noSession = false;  // Skip TCP session negotiation
    std::string signalingServer;    // Room code signaling server URL
    uint32_t    duration = 0;       // Benchmark: auto-exit after N seconds (0 = disabled)
    std::string csvPath;            // Benchmark: write periodic encode stats CSV
};

CliArgs ParseArgs(int argc, char* argv[]) {
    CliArgs args;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--client") == 0 && i + 1 < argc) {
            args.clientHost = argv[++i];
        } else if (strcmp(argv[i], "--bitrate") == 0 && i + 1 < argc) {
            args.bitrateKbps = static_cast<uint32_t>(atoi(argv[++i]));
        } else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            args.fps = static_cast<uint32_t>(atoi(argv[++i]));
        } else if (strcmp(argv[i], "--codec") == 0 && i + 1 < argc) {
            ++i;
            if (strcmp(argv[i], "h264") == 0) args.codec = cc::VideoCodec::H264;
            else if (strcmp(argv[i], "hevc") == 0) args.codec = cc::VideoCodec::HEVC;
            else if (strcmp(argv[i], "av1") == 0) args.codec = cc::VideoCodec::AV1;
        } else if (strcmp(argv[i], "--encode-resolution") == 0 && i + 1 < argc) {
            ++i;
            if (sscanf(argv[i], "%ux%u", &args.encodeWidth, &args.encodeHeight) != 2) {
                fprintf(stderr, "Invalid resolution format: %s (expected WxH)\n", argv[i]);
                exit(1);
            }
        } else if (strcmp(argv[i], "--no-session") == 0) {
            args.noSession = true;
        } else if (strcmp(argv[i], "--signaling-server") == 0 && i + 1 < argc) {
            args.signalingServer = argv[++i];
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            args.duration = static_cast<uint32_t>(atoi(argv[++i]));
        } else if (strcmp(argv[i], "--csv") == 0 && i + 1 < argc) {
            args.csvPath = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Couch Conduit Host v%u.%u.%u\n\n",
                   cc::kVersionMajor, cc::kVersionMinor, cc::kVersionPatch);
            printf("Usage: cc_host.exe [options]\n\n");
            printf("Options:\n");
            printf("  --client <ip>        Client IP (default: wait for TCP session)\n");
            printf("  --bitrate <kbps>     Video bitrate in kbps (default: 20000)\n");
            printf("  --fps <fps>          Target framerate (default: 60)\n");
            printf("  --codec <codec>      Video codec: h264, hevc, av1 (default: hevc)\n");
            printf("  --encode-resolution <WxH>  Encode resolution (default: capture res)\n");
            printf("  --no-session         Skip TCP session/key exchange (direct UDP)\n");
            printf("  --signaling-server <url>  Signaling server for room codes\n");
            printf("  --duration <seconds> Auto-exit after N seconds (benchmark mode)\n");
            printf("  --csv <path>         Write periodic encode stats to CSV file\n");
            printf("  --help               Show this help\n");
            exit(0);
        }
    }

    // Legacy mode requires --client
    if (args.noSession && args.clientHost.empty()) {
        args.clientHost = "127.0.0.1";
    }

    // Check environment variable for signaling server
    if (args.signalingServer.empty()) {
        const char* envUrl = std::getenv("CC_SIGNALING_URL");
        if (envUrl && envUrl[0]) args.signalingServer = envUrl;
    }

    return args;
}

int main(int argc, char* argv[]) {
    // Set up signal handler for clean shutdown
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    auto args = ParseArgs(argc, argv);

    printf("\n");
    printf("  ========================================\n");
    printf("  ===       COUCH CONDUIT HOST         ===\n");
    printf("  ===   Ultra-Low-Latency Game Stream  ===\n");
    printf("  ========================================\n");
    printf("\n");

    CC_INFO("Couch Conduit Host v%u.%u.%u starting...",
            cc::kVersionMajor, cc::kVersionMinor, cc::kVersionPatch);

    // Initialize Winsock
    if (!cc::transport::InitWinsock()) {
        CC_FATAL("Failed to initialize Winsock");
        return 1;
    }

    // Build host session config
    cc::host::HostSession::Config sessionConfig;
    sessionConfig.video.fps         = args.fps;
    sessionConfig.video.bitrateKbps = args.bitrateKbps;
    sessionConfig.video.codec       = args.codec;
    sessionConfig.encodeWidth       = args.encodeWidth;
    sessionConfig.encodeHeight      = args.encodeHeight;
    sessionConfig.csvPath           = args.csvPath;

    // ─── Room Code Registration ────────────────────────────────────
    std::string roomCode;
    std::string localIp = cc::net::SignalingClient::GetLocalIp();

    if (!args.noSession && !args.signalingServer.empty()) {
        CC_INFO("Discovering public IP via STUN...");
        auto stun = cc::net::StunClient::DiscoverAny();

        if (stun.success) {
            CC_INFO("Public endpoint: %s:%u", stun.publicIp.c_str(), stun.publicPort);

            if (cc::net::SignalingClient::CreateRoom(
                    args.signalingServer, stun.publicIp,
                    cc::kDefaultControlPort, roomCode)) {
                printf("\n");
                printf("  +--------------------------------------+\n");
                printf("  |                                      |\n");
                printf("  |   Room Code:   %-6s               |\n", roomCode.c_str());
                printf("  |                                      |\n");
                printf("  +--------------------------------------+\n");
                printf("  Share this code with friends to connect!\n\n");
                printf("  Public IP : %s:%u\n", stun.publicIp.c_str(), cc::kDefaultControlPort);
            }
        } else {
            CC_WARN("STUN discovery failed — room code unavailable");
        }
    }

    printf("  Local IP  : %s:%u\n\n", localIp.c_str(), cc::kDefaultControlPort);

    // TCP session negotiation (unless --no-session)
    if (!args.noSession) {
        CC_INFO("Waiting for client to connect via TCP session...");

        cc::protocol::Session tcpSession;
        cc::protocol::SessionConfig negotiated;
        negotiated.width       = 1920;  // Defaults for the offer
        negotiated.height      = 1080;
        negotiated.fps         = args.fps;
        negotiated.bitrateKbps = args.bitrateKbps;
        negotiated.codec       = args.codec;

        if (!tcpSession.HostListen(cc::kDefaultControlPort, negotiated)) {
            CC_FATAL("Failed to start TCP session listener");
            cc::transport::CleanupWinsock();
            return 1;
        }

        CC_INFO("Listening on TCP port %u — waiting for client...", cc::kDefaultControlPort);

        if (!tcpSession.AcceptClient(negotiated, 120000)) { // 2 min timeout
            CC_FATAL("No client connected within timeout");
            cc::transport::CleanupWinsock();
            return 1;
        }

        // Use negotiated values
        sessionConfig.clientHost       = negotiated.peerAddr;
        sessionConfig.sessionKey       = negotiated.sessionKey;
        sessionConfig.encrypted        = true;

        CC_INFO("Session established with %s — encryption ON", negotiated.peerAddr.c_str());
    } else {
        // Legacy direct mode
        sessionConfig.clientHost = args.clientHost;
        sessionConfig.encrypted  = false;

        CC_INFO("Direct mode: streaming to %s (no encryption)", args.clientHost.c_str());
    }

    CC_INFO("Target: %s @ %u kbps, %u fps, codec=%s, encrypted=%s",
            sessionConfig.clientHost.c_str(), args.bitrateKbps, args.fps,
            args.codec == cc::VideoCodec::HEVC ? "HEVC" :
            args.codec == cc::VideoCodec::AV1  ? "AV1"  : "H.264",
            sessionConfig.encrypted ? "yes" : "no");

    // Create and initialize host session
    auto session = std::make_unique<cc::host::HostSession>();

    if (!session->Init(sessionConfig)) {
        CC_FATAL("Host session initialization failed");
        cc::transport::CleanupWinsock();
        return 1;
    }

    if (!session->Start()) {
        CC_FATAL("Host session failed to start");
        cc::transport::CleanupWinsock();
        return 1;
    }

    CC_INFO("Streaming to %s — press Ctrl+C to stop", sessionConfig.clientHost.c_str());

    int64_t streamStartUs = cc::NowUsec();
    while (g_running && session->IsStreaming()) {
        Sleep(100);

        // Benchmark auto-exit after --duration seconds
        if (args.duration > 0) {
            int64_t elapsed = cc::NowUsec() - streamStartUs;
            if (elapsed > static_cast<int64_t>(args.duration) * 1000000LL) {
                CC_INFO("Benchmark duration %u seconds reached — exiting", args.duration);
                break;
            }
        }
    }

    CC_INFO("Shutting down...");
    session->Stop();
    session.reset();

    // Clean up room code from signaling server
    if (!roomCode.empty() && !args.signalingServer.empty()) {
        cc::net::SignalingClient::DeleteRoom(args.signalingServer, roomCode);
    }

    cc::transport::CleanupWinsock();

    return 0;
}
