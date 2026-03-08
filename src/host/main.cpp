// Couch Conduit — Host entry point
//
// Usage: cc_host.exe [--client <ip>] [--bitrate <kbps>] [--fps <fps>] [--codec <h264|hevc|av1>]
//
// The host captures the desktop, encodes with NVENC, and streams to the client.
// It also receives input from the client and injects it via ViGEmBus.

#include <couch_conduit/common/types.h>
#include <couch_conduit/common/transport.h>
#include <couch_conduit/common/log.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <csignal>
#include <atomic>

// Forward declaration — defined in host_session.cpp
namespace cc::host {
    class HostSession;
}

static std::atomic<bool> g_running{true};

void SignalHandler(int) {
    g_running = false;
}

struct CliArgs {
    std::string clientHost = "127.0.0.1";
    uint32_t    bitrateKbps = 20000;
    uint32_t    fps = 60;
    cc::VideoCodec codec = cc::VideoCodec::HEVC;
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
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Couch Conduit Host v%u.%u.%u\n\n",
                   cc::kVersionMajor, cc::kVersionMinor, cc::kVersionPatch);
            printf("Usage: cc_host.exe [options]\n\n");
            printf("Options:\n");
            printf("  --client <ip>        Client IP address (default: 127.0.0.1)\n");
            printf("  --bitrate <kbps>     Video bitrate in kbps (default: 20000)\n");
            printf("  --fps <fps>          Target framerate (default: 60)\n");
            printf("  --codec <codec>      Video codec: h264, hevc, av1 (default: hevc)\n");
            printf("  --help               Show this help\n");
            exit(0);
        }
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
    CC_INFO("Target: %s @ %u kbps, %u fps, codec=%s",
            args.clientHost.c_str(), args.bitrateKbps, args.fps,
            args.codec == cc::VideoCodec::HEVC ? "HEVC" :
            args.codec == cc::VideoCodec::AV1  ? "AV1"  : "H.264");

    // Initialize Winsock
    if (!cc::transport::InitWinsock()) {
        CC_FATAL("Failed to initialize Winsock");
        return 1;
    }

    // TODO: Create and run HostSession
    // For now, demonstrate the architecture by logging initialization
    CC_INFO("Winsock initialized");
    CC_INFO("Host initialization complete — awaiting full pipeline implementation");
    CC_INFO("Press Ctrl+C to stop");

    while (g_running) {
        Sleep(100);
    }

    CC_INFO("Shutting down...");
    cc::transport::CleanupWinsock();

    return 0;
}
