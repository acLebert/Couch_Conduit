#pragma once
// Couch Conduit — AES-128-GCM encryption/decryption via Windows CNG (bcrypt.h)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <bcrypt.h>

#include <cstdint>
#include <cstddef>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace cc::crypto {

class AesGcm {
public:
    static constexpr size_t kKeySize = 16;    // AES-128
    static constexpr size_t kIvSize  = 12;    // 96-bit nonce
    static constexpr size_t kTagSize = 16;    // 128-bit authentication tag

    AesGcm() = default;
    ~AesGcm() { Shutdown(); }
    AesGcm(const AesGcm&) = delete;
    AesGcm& operator=(const AesGcm&) = delete;

    /// Initialize with a 16-byte AES-128 key
    bool Init(const uint8_t key[kKeySize]);

    /// Encrypt in-place. Output = ciphertext ‖ 16-byte tag.
    /// Returns total output size (plaintextLen + kTagSize), or 0 on failure.
    size_t Encrypt(const uint8_t* iv,
                   const uint8_t* plaintext, size_t plaintextLen,
                   uint8_t* output, size_t outputCapacity);

    /// Decrypt in-place. Input = ciphertext ‖ 16-byte tag.
    /// Returns plaintext size, or 0 on auth failure.
    size_t Decrypt(const uint8_t* iv,
                   const uint8_t* ciphertext, size_t ciphertextLen,
                   uint8_t* output, size_t outputCapacity);

    void Shutdown();

    /// Build a 12-byte nonce from three 4-byte fields
    static void BuildNonce(uint8_t nonce[kIvSize],
                           uint32_t a, uint32_t b, uint32_t c) {
        std::memcpy(nonce + 0, &a, 4);
        std::memcpy(nonce + 4, &b, 4);
        std::memcpy(nonce + 8, &c, 4);
    }

private:
    BCRYPT_ALG_HANDLE m_hAlg = nullptr;
    BCRYPT_KEY_HANDLE m_hKey = nullptr;
    std::vector<uint8_t> m_keyObj;
};

}  // namespace cc::crypto
