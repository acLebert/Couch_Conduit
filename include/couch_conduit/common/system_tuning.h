#pragma once
// Couch Conduit — Windows system tuning for ultra-low latency

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#include <avrt.h>

#include <couch_conduit/common/log.h>
#include <couch_conduit/common/types.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "gdi32.lib")

namespace cc::sys {

/// Apply all system-level latency optimizations.
/// Call once at process startup on both host and client.
/// Implementation in system_tuning.cpp (needs d3dkmthk.h which requires special include order)
void ApplyLatencyTuning();

/// Register current thread with MMCSS for the given task.
/// Returns the MMCSS task index, or 0 on failure.
inline DWORD RegisterMmcssThread(const wchar_t* taskName, DWORD priority = AVRT_PRIORITY_CRITICAL) {
    DWORD taskIndex = 0;
    HANDLE hTask = AvSetMmThreadCharacteristicsW(taskName, &taskIndex);
    if (hTask) {
        AvSetMmThreadPriority(hTask, static_cast<AVRT_PRIORITY>(priority));
        CC_INFO("MMCSS registered: task=%ls, index=%u", taskName, taskIndex);
    } else {
        CC_WARN("AvSetMmThreadCharacteristics(%ls) failed: %lu", taskName, GetLastError());
    }
    return taskIndex;
}

/// Set current thread priority
inline void SetThreadPriorityLevel(int priority) {
    SetThreadPriority(GetCurrentThread(), priority);
}

/// Configure a UDP socket for low latency
inline void ConfigureUdpSocket(SOCKET sock) {
    // Large send/receive buffers
    int bufSize = static_cast<int>(cc::kSocketBufferSize);
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char*>(&bufSize), sizeof(bufSize));
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char*>(&bufSize), sizeof(bufSize));

    // Disable UDP checksum offload delay
    BOOL noDelay = TRUE;
    setsockopt(sock, IPPROTO_UDP, UDP_NOCHECKSUM, reinterpret_cast<char*>(&noDelay), sizeof(noDelay));

    CC_DEBUG("UDP socket configured: bufSize=%d", bufSize);
}

}  // namespace cc::sys
