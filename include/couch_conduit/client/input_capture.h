#pragma once
// Couch Conduit — Client input capture header
// XInput controller polling + Raw Input keyboard/mouse

#include <couch_conduit/common/transport.h>
#include <couch_conduit/common/types.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <thread>
#include <atomic>
#include <cstdint>
#include <string>

namespace cc::client {

/// Client-side input capture using XInput + Raw Input.
/// Polls controllers at 1000Hz and provides event handlers for keyboard/mouse.
class InputCapture {
public:
    InputCapture() = default;
    ~InputCapture();

    InputCapture(const InputCapture&) = delete;
    InputCapture& operator=(const InputCapture&) = delete;

    /// Initialize with host address for input sending
    bool Init(const std::string& hostAddr, uint16_t hostPort);

    /// Start the controller polling thread
    bool Start();

    /// Stop polling and shutdown
    void Stop();

    // ─── Call from WndProc ────────────────────────────────────────────

    /// Keyboard key down
    void OnKeyDown(uint16_t vkCode);

    /// Keyboard key up
    void OnKeyUp(uint16_t vkCode);

    /// Raw mouse relative motion (from WM_INPUT)
    void OnMouseMove(int16_t dx, int16_t dy);

    /// Flush accumulated mouse deltas (called each poll cycle)
    void FlushMouse();

    /// Mouse button press/release
    void OnMouseButton(uint8_t button, bool pressed);

    /// Mouse scroll
    void OnMouseScroll(int16_t dx, int16_t dy);

    /// Request IDR from host (on connect or decode error)
    void SendRequestIdr();

    /// Enable encryption on the underlying InputSender
    void SetEncryptionKey(const uint8_t key[16]);

private:
    cc::transport::InputSender m_sender;
    std::thread m_controllerThread;
    std::atomic<bool> m_running{false};

    // Mouse batching
    std::atomic<int32_t> m_mouseAccumX{0};
    std::atomic<int32_t> m_mouseAccumY{0};

    void ControllerPollLoop();
};

}  // namespace cc::client
