// Couch Conduit — Input Sender (Client side)

#include <couch_conduit/common/transport.h>
#include <couch_conduit/common/log.h>

#include <cstring>

namespace cc::transport {

bool InputSender::Init(const std::string& hostAddr, uint16_t hostPort) {
    m_socket.SetRemote(hostAddr, hostPort);
    CC_INFO("InputSender initialized → %s:%u", hostAddr.c_str(), hostPort);
    return true;
}

void InputSender::SendInput(InputMessageType type, uint8_t controllerId,
                            const void* payload, size_t payloadLen) {
    InputPacketHeader hdr = {};
    hdr.msgType = type;
    hdr.controllerId = controllerId;
    hdr.sequence = m_sequence++;

    // TODO: Add AES-GCM encryption
    // For now, send plaintext (development only)
    std::vector<uint8_t> packet(sizeof(hdr) + payloadLen);
    std::memcpy(packet.data(), &hdr, sizeof(hdr));
    if (payloadLen > 0) {
        std::memcpy(packet.data() + sizeof(hdr), payload, payloadLen);
    }

    m_socket.Send(packet.data(), packet.size());
}

void InputSender::SendGamepadState(const GamepadState& state) {
    SendInput(InputMessageType::GamepadState, state.controllerId,
              &state, sizeof(state));
}

void InputSender::SendMouseMotion(int16_t dx, int16_t dy) {
    struct { int16_t dx; int16_t dy; } payload = { dx, dy };
    SendInput(InputMessageType::MouseRelativeMotion, 0, &payload, sizeof(payload));
}

void InputSender::SendKeyboard(uint16_t vkCode, bool pressed) {
    struct { uint16_t vk; uint8_t pressed; } payload = { vkCode, static_cast<uint8_t>(pressed) };
    SendInput(InputMessageType::KeyboardKey, 0, &payload, sizeof(payload));
}

void InputSender::SendMouseButton(uint8_t button, bool pressed) {
    struct { uint8_t btn; uint8_t pressed; } payload = { button, static_cast<uint8_t>(pressed) };
    SendInput(InputMessageType::MouseButton, 0, &payload, sizeof(payload));
}

void InputSender::SendMouseScroll(int16_t deltaX, int16_t deltaY) {
    struct { int16_t dx; int16_t dy; } payload = { deltaX, deltaY };
    SendInput(InputMessageType::MouseScroll, 0, &payload, sizeof(payload));
}

void InputSender::Shutdown() {
    m_socket.Close();
}

}  // namespace cc::transport
