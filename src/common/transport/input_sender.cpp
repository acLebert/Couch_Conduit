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

void InputSender::SetEncryptionKey(const uint8_t key[16]) {
    m_crypto = std::make_unique<crypto::AesGcm>();
    if (!m_crypto->Init(key)) {
        CC_ERROR("Failed to init AES-GCM for InputSender");
        m_crypto.reset();
    }
}

void InputSender::SendInput(InputMessageType type, uint8_t controllerId,
                            const void* payload, size_t payloadLen) {
    InputPacketHeader hdr = {};
    hdr.msgType = type;
    hdr.controllerId = controllerId;
    hdr.sequence = m_sequence++;

    if (m_crypto && payloadLen > 0) {
        // Encrypt payload with AES-GCM
        uint8_t nonce[12];
        crypto::AesGcm::BuildNonce(nonce, 0x494E5054,  // 'INPT'
                                   static_cast<uint32_t>(controllerId),
                                   static_cast<uint32_t>(hdr.sequence));

        std::vector<uint8_t> encrypted(payloadLen + crypto::AesGcm::kTagSize);
        size_t encLen = m_crypto->Encrypt(nonce,
            static_cast<const uint8_t*>(payload), payloadLen,
            encrypted.data(), encrypted.size());
        if (encLen > 0) {
            std::vector<uint8_t> packet(sizeof(hdr) + encLen);
            std::memcpy(packet.data(), &hdr, sizeof(hdr));
            std::memcpy(packet.data() + sizeof(hdr), encrypted.data(), encLen);
            m_socket.Send(packet.data(), packet.size());
            return;
        }
    }

    // Plaintext fallback
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

void InputSender::SendRequestIdr() {
    // No payload needed — the message type is enough
    SendInput(InputMessageType::RequestIdr, 0, nullptr, 0);
}

void InputSender::SendRumble(uint8_t controllerId, uint8_t largeMotor, uint8_t smallMotor) {
    struct { uint8_t large; uint8_t small; } payload = { largeMotor, smallMotor };
    SendInput(InputMessageType::HapticFeedback, controllerId, &payload, sizeof(payload));
}

void InputSender::Shutdown() {
    m_socket.Close();
}

}  // namespace cc::transport
