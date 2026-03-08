// Couch Conduit — Reed-Solomon FEC (Forward Error Correction)
// GF(2^8) Vandermonde matrix RS encoder/decoder

#include <couch_conduit/common/reed_solomon.h>
#include <cstring>
#include <algorithm>

namespace cc::fec {

// ─── Reed-Solomon implementation ──────────────────────────────────────

ReedSolomon::ReedSolomon(int dataShards, int parityShards)
    : m_dataShards(dataShards), m_parityShards(parityShards) {
    BuildEncMatrix();
}

void ReedSolomon::BuildEncMatrix() {
    // Build a Vandermonde matrix for systematic encoding.
    // Top k×k portion is identity (systematic), bottom m×k is parity.
    auto& gf = GaloisField::Instance();
    int n = m_dataShards + m_parityShards;
    int k = m_dataShards;

    // Start with a Vandermonde matrix of size n × k
    // V[i][j] = i^j  in GF(2^8)
    std::vector<std::vector<uint8_t>> vand(n, std::vector<uint8_t>(k, 0));
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < k; ++j) {
            if (j == 0) {
                vand[i][j] = 1;
            } else if (i == 0) {
                vand[i][j] = 0;
            } else {
                vand[i][j] = gf.Pow(static_cast<uint8_t>(i), j);
            }
        }
    }

    // Extract top k×k and invert it
    std::vector<std::vector<uint8_t>> topK(k, std::vector<uint8_t>(k, 0));
    for (int i = 0; i < k; ++i) {
        topK[i] = std::vector<uint8_t>(vand[i].begin(), vand[i].begin() + k);
    }
    InvertMatrix(topK, k);

    // Multiply: result = vand × topK_inverse → systematic form
    m_encMatrix.resize(n, std::vector<uint8_t>(k, 0));
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < k; ++j) {
            uint8_t val = 0;
            for (int c = 0; c < k; ++c) {
                val ^= gf.Mul(vand[i][c], topK[c][j]);
            }
            m_encMatrix[i][j] = val;
        }
    }
}

bool ReedSolomon::InvertMatrix(std::vector<std::vector<uint8_t>>& matrix, int size) {
    auto& gf = GaloisField::Instance();

    // Augment with identity
    std::vector<std::vector<uint8_t>> work(size, std::vector<uint8_t>(size * 2, 0));
    for (int i = 0; i < size; ++i) {
        for (int j = 0; j < size; ++j) {
            work[i][j] = matrix[i][j];
        }
        work[i][size + i] = 1;
    }

    // Gaussian elimination
    for (int col = 0; col < size; ++col) {
        // Find pivot
        if (work[col][col] == 0) {
            int swapRow = -1;
            for (int row = col + 1; row < size; ++row) {
                if (work[row][col] != 0) {
                    swapRow = row;
                    break;
                }
            }
            if (swapRow < 0) return false;  // Singular
            std::swap(work[col], work[swapRow]);
        }

        // Scale pivot row
        uint8_t inv = gf.Inv(work[col][col]);
        for (int j = 0; j < size * 2; ++j) {
            work[col][j] = gf.Mul(work[col][j], inv);
        }

        // Eliminate column
        for (int row = 0; row < size; ++row) {
            if (row == col) continue;
            uint8_t factor = work[row][col];
            if (factor == 0) continue;
            for (int j = 0; j < size * 2; ++j) {
                work[row][j] ^= gf.Mul(factor, work[col][j]);
            }
        }
    }

    // Extract inverse
    for (int i = 0; i < size; ++i) {
        for (int j = 0; j < size; ++j) {
            matrix[i][j] = work[i][size + j];
        }
    }
    return true;
}

bool ReedSolomon::Encode(const std::vector<const uint8_t*>& data,
                          const std::vector<size_t>& dataLens,
                          size_t shardSize,
                          std::vector<std::vector<uint8_t>>& parityOut) {
    auto& gf = GaloisField::Instance();
    int k = m_dataShards;
    int m = m_parityShards;

    if (static_cast<int>(data.size()) != k) return false;

    parityOut.resize(m);
    for (int i = 0; i < m; ++i) {
        parityOut[i].assign(shardSize, 0);
    }

    // For each parity shard p (row k+p of encoding matrix):
    //   parityOut[p] = sum_j( encMatrix[k+p][j] * data[j] )  in GF(2^8)
    for (int p = 0; p < m; ++p) {
        for (int j = 0; j < k; ++j) {
            uint8_t coeff = m_encMatrix[k + p][j];
            if (coeff == 0) continue;

            size_t len = std::min(dataLens[j], shardSize);
            for (size_t b = 0; b < len; ++b) {
                parityOut[p][b] ^= gf.Mul(coeff, data[j][b]);
            }
        }
    }
    return true;
}

bool ReedSolomon::Decode(std::vector<std::vector<uint8_t>>& shards,
                          const std::vector<bool>& shardPresent,
                          size_t shardSize) {
    auto& gf = GaloisField::Instance();
    int k = m_dataShards;
    int n = TotalShards();

    if (static_cast<int>(shards.size()) != n) return false;
    if (static_cast<int>(shardPresent.size()) != n) return false;

    // Count available shards
    int available = 0;
    for (int i = 0; i < n; ++i) {
        if (shardPresent[i]) ++available;
    }
    if (available < k) return false;  // Not enough shards

    // If all data shards present, nothing to do
    bool allDataPresent = true;
    for (int i = 0; i < k; ++i) {
        if (!shardPresent[i]) { allDataPresent = false; break; }
    }
    if (allDataPresent) return true;

    // Pick first k available shards
    std::vector<int> subIds;
    subIds.reserve(k);
    for (int i = 0; i < n && static_cast<int>(subIds.size()) < k; ++i) {
        if (shardPresent[i]) subIds.push_back(i);
    }
    if (static_cast<int>(subIds.size()) < k) return false;

    // Build sub-matrix from encoding matrix rows corresponding to available shards
    std::vector<std::vector<uint8_t>> subMatrix(k, std::vector<uint8_t>(k, 0));
    for (int i = 0; i < k; ++i) {
        for (int j = 0; j < k; ++j) {
            subMatrix[i][j] = m_encMatrix[subIds[i]][j];
        }
    }

    // Invert sub-matrix
    if (!InvertMatrix(subMatrix, k)) return false;

    // Recover missing data shards
    for (int i = 0; i < k; ++i) {
        if (shardPresent[i]) continue;

        // data[i] = sum_j( subMatrix_inv[i][j] * available_shard[j] )
        shards[i].assign(shardSize, 0);
        for (int j = 0; j < k; ++j) {
            uint8_t coeff = subMatrix[i][j];
            if (coeff == 0) continue;

            const auto& src = shards[subIds[j]];
            size_t len = std::min(src.size(), shardSize);
            for (size_t b = 0; b < len; ++b) {
                shards[i][b] ^= gf.Mul(coeff, src[b]);
            }
        }
    }

    return true;
}

}  // namespace cc::fec
