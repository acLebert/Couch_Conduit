// Couch Conduit — Client input capture
// Captures controller, keyboard, and mouse input and batches for sending

#include <couch_conduit/common/transport.h>
#include <couch_conduit/common/types.h>
#include <couch_conduit/common/log.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <Xinput.h>

#include <thread>
#include <atomic>
#include <memory>

#pragma comment(lib, "xinput.lib")

namespace cc::client {

/// Client-side input capture using XInput + Raw Input
/// Polls controllers at 1000Hz and batches axis events
class InputCapture {
public:
    InputCapture() = default;
    ~InputCapture() { Stop(); }

    bool Init(const std::string& hostAddr, uint16_t hostPort) {
        if (!m_sender.Init(hostAddr, hostPort)) {
            return false;
        }

        CC_INFO("InputCapture initialized → %s:%u", hostAddr.c_str(), hostPort);
        return true;
    }

    bool Start() {
        m_running = true;

        // Controller polling thread — 1000Hz
        m_controllerThread = std::thread([this]() {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            ControllerPollLoop();
        });

        CC_INFO("Input capture started (1000Hz controller polling)");
        return true;
    }

    void Stop() {
        m_running = false;
        if (m_controllerThread.joinable()) {
            m_controllerThread.join();
        }
        m_sender.Shutdown();
    }

    /// Call from window message handler for keyboard events
    void OnKeyDown(uint16_t vkCode) {
        m_sender.SendKeyboard(vkCode, true);
    }

    void OnKeyUp(uint16_t vkCode) {
        m_sender.SendKeyboard(vkCode, false);
    }

    /// Call from window message handler for mouse events
    void OnMouseMove(int16_t dx, int16_t dy) {
        // Batch: accumulate deltas
        m_mouseAccumX += dx;
        m_mouseAccumY += dy;
    }

    void FlushMouse() {
        if (m_mouseAccumX != 0 || m_mouseAccumY != 0) {
            m_sender.SendMouseMotion(
                static_cast<int16_t>(m_mouseAccumX),
                static_cast<int16_t>(m_mouseAccumY)
            );
            m_mouseAccumX = 0;
            m_mouseAccumY = 0;
        }
    }

    void OnMouseButton(uint8_t button, bool pressed) {
        m_sender.SendMouseButton(button, pressed);
    }

    void OnMouseScroll(int16_t dx, int16_t dy) {
        m_sender.SendMouseScroll(dx, dy);
    }

private:
    cc::transport::InputSender m_sender;
    std::thread m_controllerThread;
    std::atomic<bool> m_running{false};

    // Mouse batching
    std::atomic<int32_t> m_mouseAccumX{0};
    std::atomic<int32_t> m_mouseAccumY{0};

    // Track previous gamepad state to send only on change
    XINPUT_STATE m_prevState[4] = {};
    bool m_controllerConnected[4] = {};

    void ControllerPollLoop() {
        while (m_running) {
            for (DWORD i = 0; i < 4; ++i) {
                XINPUT_STATE state = {};
                DWORD result = XInputGetState(i, &state);

                if (result == ERROR_SUCCESS) {
                    if (!m_controllerConnected[i]) {
                        m_controllerConnected[i] = true;
                        CC_INFO("Controller %u connected", i);
                        // TODO: Send ControllerConnected message
                    }

                    // Only send if state changed
                    if (state.dwPacketNumber != m_prevState[i].dwPacketNumber) {
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
                        m_prevState[i] = state;
                    }
                } else if (m_controllerConnected[i]) {
                    m_controllerConnected[i] = false;
                    CC_INFO("Controller %u disconnected", i);
                    // TODO: Send ControllerDisconnect message
                }
            }

            // Flush batched mouse movement
            FlushMouse();

            // 1ms sleep = ~1000Hz polling
            // With NtSetTimerResolution(5000) this actually achieves ~1ms
            Sleep(1);
        }
    }
};

}  // namespace cc::client
