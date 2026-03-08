// Couch Conduit — AES-128-GCM encryption/decryption via Windows CNG

#include <couch_conduit/common/aes_gcm.h>
#include <couch_conduit/common/log.h>

#include <cstring>

namespace cc::crypto {

bool AesGcm::Init(const uint8_t key[kKeySize]) {
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

size_t AesGcm::Encrypt(const uint8_t* iv,
                        const uint8_t* plaintext, size_t plaintextLen,
                        uint8_t* output, size_t outputCapacity) {
    if (outputCapacity < plaintextLen + kTagSize) return 0;

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce = const_cast<PUCHAR>(iv);
    authInfo.cbNonce = static_cast<ULONG>(kIvSize);
    authInfo.pbTag = output + plaintextLen;
    authInfo.cbTag = static_cast<ULONG>(kTagSize);

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

size_t AesGcm::Decrypt(const uint8_t* iv,
                        const uint8_t* ciphertext, size_t ciphertextLen,
                        uint8_t* output, size_t outputCapacity) {
    if (ciphertextLen < kTagSize) return 0;
    size_t dataLen = ciphertextLen - kTagSize;
    if (outputCapacity < dataLen) return 0;

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce = const_cast<PUCHAR>(iv);
    authInfo.cbNonce = static_cast<ULONG>(kIvSize);
    authInfo.pbTag = const_cast<PUCHAR>(ciphertext + dataLen);
    authInfo.cbTag = static_cast<ULONG>(kTagSize);

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

void AesGcm::Shutdown() {
    if (m_hKey) { BCryptDestroyKey(m_hKey); m_hKey = nullptr; }
    if (m_hAlg) { BCryptCloseAlgorithmProvider(m_hAlg, 0); m_hAlg = nullptr; }
    m_keyObj.clear();
}

}  // namespace cc::crypto
