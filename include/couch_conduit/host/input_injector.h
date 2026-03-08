#pragma once
// Couch Conduit — ViGEmBus virtual controller input injection (Host side)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <array>
#include <memory>
#include <functional>
#include <thread>
#include <atomic>

#include <couch_conduit/common/types.h>
#include <couch_conduit/common/transport.h>

// Forward declarations for ViGEm types
// We load ViGEmClient dynamically
typedef struct _VIGEM_CLIENT* PVIGEM_CLIENT;
typedef struct _VIGEM_TARGET* PVIGEM_TARGET;

namespace cc::host {

/// Callback for haptic/rumble feedback to send back to client
using RumbleCallback = std::function<void(uint8_t controllerId,
                                          uint8_t largeMotor, uint8_t smallMotor)>;

/// Virtual controller manager using ViGEmBus.
///
/// Key improvement over Sunshine: Input is processed INLINE on the
/// receiving thread — no task pool queuing. This eliminates 1-2ms
/// of scheduling latency.
class InputInjector {
public:
    static constexpr int kMaxControllers = 4;

    InputInjector() = default;
    ~InputInjector();

    InputInjector(const InputInjector&) = delete;
    InputInjector& operator=(const InputInjector&) = delete;

    /// Initialize ViGEmBus connection
    bool Init(RumbleCallback rumbleCb = nullptr);

    /// Connect a virtual controller. Returns true on success.
    bool ConnectController(uint8_t controllerId);

    /// Disconnect a virtual controller
    void DisconnectController(uint8_t controllerId);

    /// Inject gamepad state — MUST be fast, called from the input receiver thread
    void InjectGamepadState(const GamepadState& state);

    /// Inject keyboard key press/release
    void InjectKeyboard(uint16_t vkCode, bool pressed);

    /// Inject mouse relative motion
    void InjectMouseMotion(int16_t dx, int16_t dy);

    /// Inject mouse button press/release
    void InjectMouseButton(uint8_t button, bool pressed);

    /// Inject mouse scroll
    void InjectMouseScroll(int16_t deltaX, int16_t deltaY);

    /// Shutdown and disconnect all controllers
    void Shutdown();

    /// Process an input packet — called directly from InputReceiver thread
    /// This is the INLINE path — no queuing.
    void ProcessInputPacket(const transport::InputPacketHeader& hdr,
                            const uint8_t* payload, size_t len);

private:
    // ViGEm (dynamically loaded)
    HMODULE        m_vigemLib = nullptr;
    PVIGEM_CLIENT  m_vigemClient = nullptr;
    bool           m_vigemAvailable = false;

    struct ControllerSlot {
        PVIGEM_TARGET target = nullptr;
        bool          connected = false;
        GamepadState  lastState = {};
    };

    std::array<ControllerSlot, kMaxControllers> m_controllers;
    RumbleCallback m_rumbleCallback;

    bool LoadViGEmApi();

    // ViGEm function pointer types (ViGEmClient.h signatures)
    using FnVigemAlloc               = PVIGEM_CLIENT(WINAPI*)();
    using FnVigemFree                = void(WINAPI*)(PVIGEM_CLIENT);
    using FnVigemConnect             = ULONG(WINAPI*)(PVIGEM_CLIENT);
    using FnVigemDisconnect          = void(WINAPI*)(PVIGEM_CLIENT);
    using FnVigemTargetX360Alloc     = PVIGEM_TARGET(WINAPI*)();
    using FnVigemTargetFree          = void(WINAPI*)(PVIGEM_TARGET);
    using FnVigemTargetAdd           = ULONG(WINAPI*)(PVIGEM_CLIENT, PVIGEM_TARGET);
    using FnVigemTargetRemove        = ULONG(WINAPI*)(PVIGEM_CLIENT, PVIGEM_TARGET);

    // XUSB_REPORT: { USHORT wButtons; BYTE bLeftTrigger, bRightTrigger;
    //                SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; }
    struct XusbReport {
        uint16_t wButtons;
        uint8_t  bLeftTrigger;
        uint8_t  bRightTrigger;
        int16_t  sThumbLX;
        int16_t  sThumbLY;
        int16_t  sThumbRX;
        int16_t  sThumbRY;
    };
    using FnVigemTargetX360Update    = ULONG(WINAPI*)(PVIGEM_CLIENT, PVIGEM_TARGET, XusbReport);

    // Notification callback type: void (PVIGEM_CLIENT, PVIGEM_TARGET, UCHAR large, UCHAR small, UCHAR led, void* userData)
    using FnVigemX360RegisterNotify  = ULONG(WINAPI*)(PVIGEM_CLIENT, PVIGEM_TARGET, void*, void*);

    struct ViGEmFunctions {
        FnVigemAlloc              alloc = nullptr;
        FnVigemFree               free = nullptr;
        FnVigemConnect            connect = nullptr;
        FnVigemDisconnect         disconnect = nullptr;
        FnVigemTargetX360Alloc    target_x360_alloc = nullptr;
        FnVigemTargetFree         target_free = nullptr;
        FnVigemTargetAdd          target_add = nullptr;
        FnVigemTargetRemove       target_remove = nullptr;
        FnVigemTargetX360Update   target_x360_update = nullptr;
        FnVigemX360RegisterNotify target_x360_register_notification = nullptr;
    } m_fn;
};

}  // namespace cc::host
