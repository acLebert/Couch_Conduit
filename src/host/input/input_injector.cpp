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

    // TODO: Load function pointers:
    // vigem_alloc, vigem_connect, vigem_target_x360_alloc,
    // vigem_target_add, vigem_target_x360_update, etc.

    CC_INFO("ViGEmClient.dll loaded");
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

    // TODO: Create and plug in ViGEm virtual Xbox 360 controller
    // slot.target = vigem_target_x360_alloc();
    // vigem_target_add(m_vigemClient, slot.target);
    // vigem_target_x360_register_notification(m_vigemClient, slot.target, rumbleCallback, this);

    slot.connected = true;
    CC_INFO("Virtual controller %u connected", controllerId);
    return true;
}

void InputInjector::DisconnectController(uint8_t controllerId) {
    if (controllerId >= kMaxControllers) return;

    auto& slot = m_controllers[controllerId];
    if (!slot.connected) return;

    // TODO: vigem_target_remove(m_vigemClient, slot.target);
    // vigem_target_free(slot.target);

    slot.connected = false;
    slot.target = nullptr;
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

    // TODO: Convert to XUSB_REPORT and call vigem_target_x360_update()
    //
    // XUSB_REPORT report = {};
    // report.wButtons = state.buttons;  // Map our button bits to XUSB bits
    // report.bLeftTrigger = state.leftTrigger;
    // report.bRightTrigger = state.rightTrigger;
    // report.sThumbLX = state.leftStickX;
    // report.sThumbLY = state.leftStickY;
    // report.sThumbRX = state.rightStickX;
    // report.sThumbRY = state.rightStickY;
    // vigem_target_x360_update(m_vigemClient, slot.target, report);
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

    if (m_vigemLib) {
        // TODO: vigem_disconnect(m_vigemClient); vigem_free(m_vigemClient);
        FreeLibrary(m_vigemLib);
        m_vigemLib = nullptr;
    }

    CC_INFO("InputInjector shut down");
}

}  // namespace cc::host
