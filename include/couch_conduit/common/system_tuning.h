#pragma once
// Couch Conduit — Windows system tuning for ultra-low latency

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <dwmapi.h>
#include <avrt.h>
#include <d3dkmthk.h>

#include <couch_conduit/common/log.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "gdi32.lib")

namespace cc::sys {

/// Apply all system-level latency optimizations.
/// Call once at process startup on both host and client.
inline void ApplyLatencyTuning() {
    // 1. Set process priority to HIGH
    if (!SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS)) {
        CC_WARN("Failed to set HIGH_PRIORITY_CLASS: %lu", GetLastError());
    } else {
        CC_INFO("Process priority set to HIGH_PRIORITY_CLASS");
    }

    // 2. Set timer resolution to 0.5ms
    // NtSetTimerResolution is an undocumented ntdll function
    using NtSetTimerResolutionFn = LONG(WINAPI*)(ULONG, BOOLEAN, PULONG);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        auto NtSetTimerResolution = reinterpret_cast<NtSetTimerResolutionFn>(
            GetProcAddress(ntdll, "NtSetTimerResolution"));
        if (NtSetTimerResolution) {
            ULONG actual = 0;
            LONG status = NtSetTimerResolution(5000, TRUE, &actual);  // 0.5ms in 100ns units
            if (status == 0) {
                CC_INFO("Timer resolution set to %.1f ms", actual / 10000.0);
            } else {
                CC_WARN("NtSetTimerResolution failed: 0x%08X", status);
            }
        }
    }

    // 3. Enable MMCSS for DWM
    HRESULT hr = DwmEnableMMCSS(TRUE);
    if (SUCCEEDED(hr)) {
        CC_INFO("DWM MMCSS enabled");
    } else {
        CC_WARN("DwmEnableMMCSS failed: 0x%08X", hr);
    }

    // 4. Set GPU scheduling priority to REALTIME
    D3DKMT_SETPROCESSSCHEDULINGPRIORITYCLASS setPriority = {};
    setPriority.hProcess = GetCurrentProcess();
    setPriority.Priority = D3DKMT_SCHEDULINGPRIORITYCLASS_REALTIME;
    NTSTATUS nts = D3DKMTSetProcessSchedulingPriorityClass(&setPriority);
    if (nts == 0) {
        CC_INFO("GPU priority set to REALTIME");
    } else {
        CC_WARN("D3DKMTSetProcessSchedulingPriorityClass failed: 0x%08X (may need admin)", nts);
    }
}

/// Register current thread with MMCSS for the given task.
/// Returns the MMCSS task handle, or 0 on failure.
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
