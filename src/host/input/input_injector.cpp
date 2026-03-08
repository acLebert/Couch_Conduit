// Couch Conduit — ViGEmBus virtual controller input injection
//
// INLINE input processing — this is called directly from the network
// receive thread, not queued through a task pool. This eliminates
// 1-2ms of scheduling latency vs Sunshine's approach.

#include <couch_conduit/host/input_injector.h>
#include <couch_conduit/common/log.h>

#include <cstring>

namespace cc::host {

InputInjector::~InputInjector() {
    Shutdown();
}

bool InputInjector::Init(RumbleCallback rumbleCb) {
    m_rumbleCallback = std::move(rumbleCb);

    if (!LoadViGEmApi()) {
        CC_WARN("ViGEmBus not available — gamepad injection disabled. "
                "Keyboard/mouse injection will still work. "
                "Install ViGEmBus from: https://github.com/nefarius/ViGEmBus/releases");
        // Don't return false — keyboard/mouse still work without ViGEm
    }

    CC_INFO("InputInjector initialized");
    return true;
}

bool InputInjector::LoadViGEmApi() {
    m_vigemLib = LoadLibraryW(L"ViGEmClient.dll");
    if (!m_vigemLib) {
        // Try common install locations
        m_vigemLib = LoadLibraryW(L"C:\\Program Files\\Nefarius Software Solutions\\ViGEmBus\\ViGEmClient.dll");
    }

    if (!m_vigemLib) {
        CC_WARN("ViGEmClient.dll not found");
        return false;
    }

    // Load all function pointers
    m_fn.alloc = reinterpret_cast<FnVigemAlloc>(
        GetProcAddress(m_vigemLib, "vigem_alloc"));
    m_fn.free = reinterpret_cast<FnVigemFree>(
        GetProcAddress(m_vigemLib, "vigem_free"));
    m_fn.connect = reinterpret_cast<FnVigemConnect>(
        GetProcAddress(m_vigemLib, "vigem_connect"));
    m_fn.disconnect = reinterpret_cast<FnVigemDisconnect>(
        GetProcAddress(m_vigemLib, "vigem_disconnect"));
    m_fn.target_x360_alloc = reinterpret_cast<FnVigemTargetX360Alloc>(
        GetProcAddress(m_vigemLib, "vigem_target_x360_alloc"));
    m_fn.target_free = reinterpret_cast<FnVigemTargetFree>(
        GetProcAddress(m_vigemLib, "vigem_target_free"));
    m_fn.target_add = reinterpret_cast<FnVigemTargetAdd>(
        GetProcAddress(m_vigemLib, "vigem_target_add"));
    m_fn.target_remove = reinterpret_cast<FnVigemTargetRemove>(
        GetProcAddress(m_vigemLib, "vigem_target_remove"));
    m_fn.target_x360_update = reinterpret_cast<FnVigemTargetX360Update>(
        GetProcAddress(m_vigemLib, "vigem_target_x360_update"));
    m_fn.target_x360_register_notification = reinterpret_cast<FnVigemX360RegisterNotify>(
        GetProcAddress(m_vigemLib, "vigem_target_x360_register_notification"));

    if (!m_fn.alloc || !m_fn.connect || !m_fn.target_x360_alloc ||
        !m_fn.target_add || !m_fn.target_x360_update) {
        CC_WARN("ViGEmClient.dll loaded but missing required functions");
        FreeLibrary(m_vigemLib);
        m_vigemLib = nullptr;
        return false;
    }

    // Allocate and connect client
    m_vigemClient = m_fn.alloc();
    if (!m_vigemClient) {
        CC_ERROR("vigem_alloc() returned null");
        return false;
    }

    ULONG result = m_fn.connect(m_vigemClient);
    // VIGEM_ERROR_NONE = 0x20000000
    if (result != 0x20000000) {
        CC_WARN("vigem_connect() failed: 0x%08X — ViGEmBus driver may not be installed", result);
        m_fn.free(m_vigemClient);
        m_vigemClient = nullptr;
        return false;
    }

    m_vigemAvailable = true;
    CC_INFO("ViGEmClient.dll loaded and connected to ViGEmBus driver");
    return true;
}

bool InputInjector::ConnectController(uint8_t controllerId) {
    if (controllerId >= kMaxControllers) {
        CC_WARN("Controller ID %u exceeds max (%d)", controllerId, kMaxControllers);
        return false;
    }

    auto& slot = m_controllers[controllerId];
    if (slot.connected) {
        return true;  // Already connected
    }

    if (!m_vigemAvailable || !m_fn.target_x360_alloc || !m_fn.target_add) {
        // ViGEm not available — mark as connected for state tracking but no real controller
        slot.connected = true;
        CC_INFO("Virtual controller %u connected (no ViGEm — state-only)", controllerId);
        return true;
    }

    slot.target = m_fn.target_x360_alloc();
    if (!slot.target) {
        CC_ERROR("vigem_target_x360_alloc() returned null for controller %u", controllerId);
        return false;
    }

    ULONG result = m_fn.target_add(m_vigemClient, slot.target);
    // VIGEM_ERROR_NONE = 0x20000000
    if (result != 0x20000000) {
        CC_ERROR("vigem_target_add() failed for controller %u: 0x%08X", controllerId, result);
        if (m_fn.target_free) m_fn.target_free(slot.target);
        slot.target = nullptr;
        return false;
    }

    // Register for rumble notifications if callback is set
    if (m_rumbleCallback && m_fn.target_x360_register_notification) {
        // NOTE: The real ViGEm callback has signature:
        // void (PVIGEM_CLIENT, PVIGEM_TARGET, UCHAR largeMotor, UCHAR smallMotor, UCHAR led, void* userData)
        // We store the controllerId in userData
        m_fn.target_x360_register_notification(
            m_vigemClient, slot.target, nullptr,
            reinterpret_cast<void*>(static_cast<uintptr_t>(controllerId)));
    }

    slot.connected = true;
    CC_INFO("Virtual controller %u connected via ViGEmBus", controllerId);
    return true;
}

void InputInjector::DisconnectController(uint8_t controllerId) {
    if (controllerId >= kMaxControllers) return;

    auto& slot = m_controllers[controllerId];
    if (!slot.connected) return;

    if (slot.target) {
        if (m_vigemAvailable && m_fn.target_remove) {
            m_fn.target_remove(m_vigemClient, slot.target);
        }
        if (m_fn.target_free) {
            m_fn.target_free(slot.target);
        }
        slot.target = nullptr;
    }

    slot.connected = false;
    CC_INFO("Virtual controller %u disconnected", controllerId);
}

void InputInjector::InjectGamepadState(const GamepadState& state) {
    if (state.controllerId >= kMaxControllers) return;

    auto& slot = m_controllers[state.controllerId];
    if (!slot.connected) {
        // Auto-connect on first input
        ConnectController(state.controllerId);
    }

    slot.lastState = state;

    // Update via ViGEm if available
    if (m_vigemAvailable && slot.target && m_fn.target_x360_update) {
        XusbReport report = {};
        // Map our button bitmask to XUSB_GAMEPAD format
        // Our buttons are already in XInput format, so direct assignment
        report.wButtons      = state.buttons;
        report.bLeftTrigger  = state.leftTrigger;
        report.bRightTrigger = state.rightTrigger;
        report.sThumbLX      = state.leftStickX;
        report.sThumbLY      = state.leftStickY;
        report.sThumbRX      = state.rightStickX;
        report.sThumbRY      = state.rightStickY;

        m_fn.target_x360_update(m_vigemClient, slot.target, report);
    }
}

void InputInjector::InjectKeyboard(uint16_t vkCode, bool pressed) {
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vkCode;
    input.ki.dwFlags = pressed ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(INPUT));
}

void InputInjector::InjectMouseMotion(int16_t dx, int16_t dy) {
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &input, sizeof(INPUT));
}

void InputInjector::InjectMouseButton(uint8_t button, bool pressed) {
    INPUT input = {};
    input.type = INPUT_MOUSE;

    switch (button) {
        case 0:  // Left
            input.mi.dwFlags = pressed ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
            break;
        case 1:  // Right
            input.mi.dwFlags = pressed ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
            break;
        case 2:  // Middle
            input.mi.dwFlags = pressed ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
            break;
    }

    SendInput(1, &input, sizeof(INPUT));
}

void InputInjector::InjectMouseScroll(int16_t deltaX, int16_t deltaY) {
    if (deltaY != 0) {
        INPUT input = {};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_WHEEL;
        input.mi.mouseData = static_cast<DWORD>(deltaY);
        SendInput(1, &input, sizeof(INPUT));
    }
    if (deltaX != 0) {
        INPUT input = {};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_HWHEEL;
        input.mi.mouseData = static_cast<DWORD>(deltaX);
        SendInput(1, &input, sizeof(INPUT));
    }
}

void InputInjector::ProcessInputPacket(const transport::InputPacketHeader& hdr,
                                        const uint8_t* payload, size_t len) {
    // INLINE processing — no queuing!
    // This function is called directly from the InputReceiver's TIME_CRITICAL thread.

    switch (hdr.msgType) {
        case InputMessageType::GamepadState: {
            if (len >= sizeof(GamepadState)) {
                GamepadState state;
                std::memcpy(&state, payload, sizeof(state));
                InjectGamepadState(state);
            }
            break;
        }
        case InputMessageType::MouseRelativeMotion: {
            if (len >= 4) {
                int16_t dx, dy;
                std::memcpy(&dx, payload, 2);
                std::memcpy(&dy, payload + 2, 2);
                InjectMouseMotion(dx, dy);
            }
            break;
        }
        case InputMessageType::KeyboardKey: {
            if (len >= 3) {
                uint16_t vk;
                std::memcpy(&vk, payload, 2);
                bool pressed = payload[2] != 0;
                InjectKeyboard(vk, pressed);
            }
            break;
        }
        case InputMessageType::MouseButton: {
            if (len >= 2) {
                InjectMouseButton(payload[0], payload[1] != 0);
            }
            break;
        }
        case InputMessageType::MouseScroll: {
            if (len >= 4) {
                int16_t dx, dy;
                std::memcpy(&dx, payload, 2);
                std::memcpy(&dy, payload + 2, 2);
                InjectMouseScroll(dx, dy);
            }
            break;
        }
        case InputMessageType::ControllerConnected: {
            ConnectController(hdr.controllerId);
            break;
        }
        case InputMessageType::ControllerDisconnect: {
            DisconnectController(hdr.controllerId);
            break;
        }
        default:
            CC_WARN("Unknown input message type: 0x%02X", static_cast<uint8_t>(hdr.msgType));
            break;
    }
}

void InputInjector::Shutdown() {
    for (uint8_t i = 0; i < kMaxControllers; ++i) {
        DisconnectController(i);
    }

    if (m_vigemClient && m_fn.disconnect) {
        m_fn.disconnect(m_vigemClient);
    }
    if (m_vigemClient && m_fn.free) {
        m_fn.free(m_vigemClient);
        m_vigemClient = nullptr;
    }

    if (m_vigemLib) {
        FreeLibrary(m_vigemLib);
        m_vigemLib = nullptr;
    }

    m_vigemAvailable = false;
    CC_INFO("InputInjector shut down");
}

}  // namespace cc::host
