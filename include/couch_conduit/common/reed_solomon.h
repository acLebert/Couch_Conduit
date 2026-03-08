#pragma once
// Couch Conduit — Reed-Solomon FEC (Forward Error Correction)
//
// GF(2^8) based RS encoder/decoder for multi-packet loss recovery.
// Uses a Vandermonde matrix approach for systematic encoding.
//
// Compared to the old XOR parity (1 lost packet per group), RS(n, k)
// can recover up to (n - k) lost packets from any k of n.

#include <cstdint>
#include <cstddef>
#include <vector>

namespace cc::fec {

/// GF(2^8) arithmetic with primitive polynomial 0x11D (x^8 + x^4 + x^3 + x^2 + 1)
class GaloisField {
public:
    static constexpr uint16_t kPrimitive = 0x11D;

    static GaloisField& Instance() {
        static GaloisField gf;
        return gf;
    }

    uint8_t Mul(uint8_t a, uint8_t b) const {
        if (a == 0 || b == 0) return 0;
        return m_exp[(m_log[a] + m_log[b]) % 255];
    }

    uint8_t Div(uint8_t a, uint8_t b) const {
        if (b == 0) return 0;  // Division by zero
        if (a == 0) return 0;
        return m_exp[(m_log[a] + 255 - m_log[b]) % 255];
    }

    uint8_t Inv(uint8_t a) const {
        if (a == 0) return 0;
        return m_exp[255 - m_log[a]];
    }

    uint8_t Pow(uint8_t a, int n) const {
        if (a == 0) return 0;
        return m_exp[(m_log[a] * n) % 255];
    }

private:
    uint8_t m_exp[512] = {};
    uint8_t m_log[256] = {};

    GaloisField() {
        // Generate exp and log tables
        uint16_t x = 1;
        for (int i = 0; i < 255; ++i) {
            m_exp[i] = static_cast<uint8_t>(x);
            m_log[x] = static_cast<uint8_t>(i);
            x <<= 1;
            if (x & 0x100) x ^= kPrimitive;
        }
        // Extend exp table for easy modular access
        for (int i = 255; i < 512; ++i) {
            m_exp[i] = m_exp[i - 255];
        }
    }
};

/// Reed-Solomon encoder/decoder
class ReedSolomon {
public:
    /// Create an RS codec with k data shards and m parity shards.
    /// Total shards = k + m. Can recover from up to m losses.
    ReedSolomon(int dataShards, int parityShards);

    /// Encode: given k data shards (each of size shardSize), produce m parity shards.
    /// dataShards: vector of k data shard buffers (each shardSize bytes)
    /// parityOut: will be resized to m buffers of shardSize bytes each
    bool Encode(const std::vector<const uint8_t*>& data,
                const std::vector<size_t>& dataLens,
                size_t shardSize,
                std::vector<std::vector<uint8_t>>& parityOut);

    /// Decode: given n total shards (some may be missing), recover k data shards.
    /// shards: vector of n shard buffers (nullptr for missing ones)
    /// shardPresent: which shards are available
    /// shardSize: size of each shard
    /// Returns true if recovery succeeded
    bool Decode(std::vector<std::vector<uint8_t>>& shards,
                const std::vector<bool>& shardPresent,
                size_t shardSize);

    int DataShards() const { return m_dataShards; }
    int ParityShards() const { return m_parityShards; }
    int TotalShards() const { return m_dataShards + m_parityShards; }

private:
    int m_dataShards;
    int m_parityShards;

    // Encoding matrix (Vandermonde-derived) — dimensions: totalShards × dataShards
    std::vector<std::vector<uint8_t>> m_encMatrix;

    void BuildEncMatrix();
    bool InvertMatrix(std::vector<std::vector<uint8_t>>& matrix, int size);
};

}  // namespace cc::fec
