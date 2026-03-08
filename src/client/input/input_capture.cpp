// Couch Conduit — Client input capture implementation
// XInput controller polling at 1000Hz + Raw Input keyboard/mouse forwarding

#include <couch_conduit/client/input_capture.h>
#include <couch_conduit/common/log.h>

#include <Xinput.h>
#pragma comment(lib, "xinput.lib")

namespace cc::client {

InputCapture::~InputCapture() {
    Stop();
}

bool InputCapture::Init(const std::string& hostAddr, uint16_t hostPort) {
    if (!m_sender.Init(hostAddr, hostPort)) {
        return false;
    }

    CC_INFO("InputCapture initialized → %s:%u", hostAddr.c_str(), hostPort);
    return true;
}

bool InputCapture::Start() {
    m_running = true;

    // Controller polling thread — 1000Hz
    m_controllerThread = std::thread([this]() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        ControllerPollLoop();
    });

    CC_INFO("Input capture started (1000Hz controller polling)");
    return true;
}

void InputCapture::Stop() {
    m_running = false;
    if (m_controllerThread.joinable()) {
        m_controllerThread.join();
    }
    m_sender.Shutdown();
}

void InputCapture::OnKeyDown(uint16_t vkCode) {
    m_sender.SendKeyboard(vkCode, true);
}

void InputCapture::OnKeyUp(uint16_t vkCode) {
    m_sender.SendKeyboard(vkCode, false);
}

void InputCapture::OnMouseMove(int16_t dx, int16_t dy) {
    // Batch: accumulate deltas between flush cycles
    m_mouseAccumX += dx;
    m_mouseAccumY += dy;
}

void InputCapture::FlushMouse() {
    int32_t dx = m_mouseAccumX.exchange(0);
    int32_t dy = m_mouseAccumY.exchange(0);
    if (dx != 0 || dy != 0) {
        m_sender.SendMouseMotion(
            static_cast<int16_t>(dx),
            static_cast<int16_t>(dy)
        );
    }
}

void InputCapture::OnMouseButton(uint8_t button, bool pressed) {
    m_sender.SendMouseButton(button, pressed);
}

void InputCapture::OnMouseScroll(int16_t dx, int16_t dy) {
    m_sender.SendMouseScroll(dx, dy);
}

void InputCapture::SendRequestIdr() {
    m_sender.SendRequestIdr();
    CC_INFO("Sent IDR request to host");
}

void InputCapture::SetEncryptionKey(const uint8_t key[16]) {
    m_sender.SetEncryptionKey(key);
}

void InputCapture::ControllerPollLoop() {
    // Track previous gamepad state to send only on change
    XINPUT_STATE prevState[4] = {};
    bool connected[4] = {};

    while (m_running) {
        for (DWORD i = 0; i < 4; ++i) {
            XINPUT_STATE state = {};
            DWORD result = XInputGetState(i, &state);

            if (result == ERROR_SUCCESS) {
                if (!connected[i]) {
                    connected[i] = true;
                    CC_INFO("Controller %u connected", i);
                }

                // Only send if state changed
                if (state.dwPacketNumber != prevState[i].dwPacketNumber) {
                    cc::GamepadState gs;
                    gs.controllerId = static_cast<uint8_t>(i);
                    gs.buttons = state.Gamepad.wButtons;
                    gs.leftStickX = state.Gamepad.sThumbLX;
                    gs.leftStickY = state.Gamepad.sThumbLY;
                    gs.rightStickX = state.Gamepad.sThumbRX;
                    gs.rightStickY = state.Gamepad.sThumbRY;
                    gs.leftTrigger = state.Gamepad.bLeftTrigger;
                    gs.rightTrigger = state.Gamepad.bRightTrigger;

                    m_sender.SendGamepadState(gs);
                    prevState[i] = state;
                }
            } else if (connected[i]) {
                connected[i] = false;
                CC_INFO("Controller %u disconnected", i);
            }
        }

        // Flush batched mouse movement
        FlushMouse();

        // 1ms sleep = ~1000Hz polling
        // With NtSetTimerResolution(5000) this actually achieves ~1ms
        Sleep(1);
    }
}

}  // namespace cc::client
