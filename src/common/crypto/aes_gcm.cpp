// Couch Conduit — AES-GCM encryption/decryption using Windows CNG (bcrypt.h)
// Placeholder — will be used for encrypting video/audio/input streams

#include <couch_conduit/common/log.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <bcrypt.h>

#include <cstdint>
#include <vector>
#include <cstring>

#pragma comment(lib, "bcrypt.lib")

namespace cc::crypto {

class AesGcm {
public:
    static constexpr size_t kKeySize = 16;    // AES-128
    static constexpr size_t kIvSize  = 12;    // 96-bit IV
    static constexpr size_t kTagSize = 16;    // 128-bit auth tag

    AesGcm() = default;
    ~AesGcm() { Shutdown(); }

    bool Init(const uint8_t key[kKeySize]) {
        NTSTATUS status = BCryptOpenAlgorithmProvider(
            &m_hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0);
        if (!BCRYPT_SUCCESS(status)) {
            CC_ERROR("BCryptOpenAlgorithmProvider failed: 0x%08X", status);
            return false;
        }

        status = BCryptSetProperty(m_hAlg, BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
            sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
        if (!BCRYPT_SUCCESS(status)) {
            CC_ERROR("Failed to set GCM mode: 0x%08X", status);
            return false;
        }

        DWORD keyObjLen = 0;
        DWORD cbResult = 0;
        BCryptGetProperty(m_hAlg, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&keyObjLen), sizeof(keyObjLen), &cbResult, 0);

        m_keyObj.resize(keyObjLen);
        status = BCryptGenerateSymmetricKey(m_hAlg, &m_hKey,
            m_keyObj.data(), keyObjLen,
            const_cast<PUCHAR>(key), kKeySize, 0);
        if (!BCRYPT_SUCCESS(status)) {
            CC_ERROR("BCryptGenerateSymmetricKey failed: 0x%08X", status);
            return false;
        }

        CC_INFO("AES-128-GCM initialized");
        return true;
    }

    /// Encrypt in-place. Appends 16-byte auth tag to the end.
    /// iv must be 12 bytes. Returns total output size (data + tag).
    size_t Encrypt(const uint8_t* iv,
                   const uint8_t* plaintext, size_t plaintextLen,
                   uint8_t* output, size_t outputCapacity) {
        if (outputCapacity < plaintextLen + kTagSize) return 0;

        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
        BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
        authInfo.pbNonce = const_cast<PUCHAR>(iv);
        authInfo.cbNonce = kIvSize;
        authInfo.pbTag = output + plaintextLen;  // Tag goes after ciphertext
        authInfo.cbTag = kTagSize;

        DWORD cbResult = 0;
        NTSTATUS status = BCryptEncrypt(m_hKey,
            const_cast<PUCHAR>(plaintext), static_cast<ULONG>(plaintextLen),
            &authInfo, nullptr, 0,
            output, static_cast<ULONG>(plaintextLen),
            &cbResult, 0);

        if (!BCRYPT_SUCCESS(status)) {
            CC_ERROR("AES-GCM encrypt failed: 0x%08X", status);
            return 0;
        }

        return plaintextLen + kTagSize;
    }

    /// Decrypt in-place. Last 16 bytes of input are the auth tag.
    /// Returns plaintext size, or 0 on auth failure.
    size_t Decrypt(const uint8_t* iv,
                   const uint8_t* ciphertext, size_t ciphertextLen,
                   uint8_t* output, size_t outputCapacity) {
        if (ciphertextLen < kTagSize) return 0;
        size_t dataLen = ciphertextLen - kTagSize;
        if (outputCapacity < dataLen) return 0;

        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
        BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
        authInfo.pbNonce = const_cast<PUCHAR>(iv);
        authInfo.cbNonce = kIvSize;
        authInfo.pbTag = const_cast<PUCHAR>(ciphertext + dataLen);
        authInfo.cbTag = kTagSize;

        DWORD cbResult = 0;
        NTSTATUS status = BCryptDecrypt(m_hKey,
            const_cast<PUCHAR>(ciphertext), static_cast<ULONG>(dataLen),
            &authInfo, nullptr, 0,
            output, static_cast<ULONG>(dataLen),
            &cbResult, 0);

        if (!BCRYPT_SUCCESS(status)) {
            CC_WARN("AES-GCM decrypt failed (auth failure?): 0x%08X", status);
            return 0;
        }

        return dataLen;
    }

    void Shutdown() {
        if (m_hKey) { BCryptDestroyKey(m_hKey); m_hKey = nullptr; }
        if (m_hAlg) { BCryptCloseAlgorithmProvider(m_hAlg, 0); m_hAlg = nullptr; }
        m_keyObj.clear();
    }

private:
    BCRYPT_ALG_HANDLE m_hAlg = nullptr;
    BCRYPT_KEY_HANDLE m_hKey = nullptr;
    std::vector<uint8_t> m_keyObj;
};

}  // namespace cc::crypto
