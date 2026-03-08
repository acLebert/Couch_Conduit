// Couch Conduit — System tuning implementation
// We dynamically load D3DKMTSetProcessSchedulingPriorityClass to avoid
// d3dkmthk.h include issues (NTSTATUS type conflicts with WIN32_LEAN_AND_MEAN)

#include <couch_conduit/common/system_tuning.h>
#include <couch_conduit/common/log.h>

#include <dwmapi.h>

namespace cc::sys {

void ApplyLatencyTuning() {
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
    // We call D3DKMTSetProcessSchedulingPriorityClass via GetProcAddress
    // to avoid pulling in d3dkmthk.h (which has NTSTATUS issues with WIN32_LEAN_AND_MEAN)
    //
    // D3DKMT_SCHEDULINGPRIORITYCLASS_REALTIME = 4
    // The struct layout is: { HANDLE hProcess; int Priority; }
    struct D3DKMT_SetPriorityClass {
        HANDLE hProcess;
        int    priority;
    };

    using SetPriorityClassFn = LONG(WINAPI*)(const D3DKMT_SetPriorityClass*);
    HMODULE gdi32 = GetModuleHandleW(L"gdi32.dll");
    if (gdi32) {
        auto fn = reinterpret_cast<SetPriorityClassFn>(
            GetProcAddress(gdi32, "D3DKMTSetProcessSchedulingPriorityClass"));
        if (fn) {
            D3DKMT_SetPriorityClass params = {};
            params.hProcess = GetCurrentProcess();
            params.priority = 4;  // D3DKMT_SCHEDULINGPRIORITYCLASS_REALTIME
            LONG nts = fn(&params);
            if (nts == 0) {
                CC_INFO("GPU priority set to REALTIME");
            } else {
                CC_WARN("D3DKMTSetProcessSchedulingPriorityClass failed: 0x%08X (may need admin)", nts);
            }
        }
    }
}

}  // namespace cc::sys
