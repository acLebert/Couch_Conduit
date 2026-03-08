// Couch Conduit — Congestion Estimator
// GCC-like adaptive bitrate controller

#include <couch_conduit/common/congestion.h>
#include <couch_conduit/common/types.h>
#include <couch_conduit/common/log.h>

#include <algorithm>
#include <cmath>

namespace cc::transport {

void CongestionEstimator::Init(const Config& cfg) {
    m_config = cfg;
    m_currentBitrate = cfg.startBitrateKbps;
    m_lastDecisionUs = NowUsec();
}

void CongestionEstimator::OnTwccFeedback(const uint16_t* /*sequences*/,
                                          const int64_t* sendTimesUs,
                                          const int16_t* arrivalDeltaQuarterMs,
                                          int count) {
    if (count < 2) return;

    // Convert arrival deltas to cumulative arrival times
    // arrivalDelta is in 0.25ms units → multiply by 250 to get microseconds
    int64_t arrivalAccum = 0;

    for (int i = 1; i < count; ++i) {
        int64_t sendDelta = sendTimesUs[i] - sendTimesUs[i - 1];
        int64_t arrivalDelta = static_cast<int64_t>(arrivalDeltaQuarterMs[i]) * 250;
        arrivalAccum += arrivalDelta;

        DelayGradientSample sample;
        sample.sendDelta = sendDelta;
        sample.arrivalDelta = arrivalDelta;
        m_delaySamples.push_back(sample);

        // Keep sliding window of recent samples (~500ms worth)
        while (m_delaySamples.size() > 50) {
            m_delaySamples.pop_front();
        }
    }

    // Compute delay gradient: (arrivalDelta - sendDelta) / sendDelta
    // Positive = congestion building, negative = draining
    if (!m_delaySamples.empty()) {
        float sumGradient = 0.0f;
        int validCount = 0;
        for (auto& s : m_delaySamples) {
            if (s.sendDelta > 0) {
                float g = static_cast<float>(s.arrivalDelta - s.sendDelta)
                        / static_cast<float>(s.sendDelta);
                sumGradient += g;
                ++validCount;
            }
        }
        if (validCount > 0) {
            float raw = sumGradient / static_cast<float>(validCount);
            // EWMA smoothing
            m_gradientEstimate = m_gradientEstimate * 0.85f + raw * 0.15f;
        }
    }
}

void CongestionEstimator::OnLossUpdate(float lossRate) {
    m_lossRate = m_lossRate * 0.7f + lossRate * 0.3f;
}

CongestionEstimator::Signal CongestionEstimator::ClassifyDelay() const {
    // Thresholds based on GCC paper
    constexpr float kOveruseThreshold  =  0.03f;  // 3% delay gradient → overuse
    constexpr float kUnderuseThreshold = -0.02f;   // Negative gradient → underuse

    if (m_gradientEstimate > kOveruseThreshold) return Signal::Overuse;
    if (m_gradientEstimate < kUnderuseThreshold) return Signal::Underuse;
    return Signal::Normal;
}

uint32_t CongestionEstimator::ComputeBitrate() {
    int64_t now = NowUsec();
    if (now - m_lastDecisionUs < m_config.intervalUs) {
        return 0;  // Not time yet
    }
    m_lastDecisionUs = now;

    Signal sig = ClassifyDelay();

    // Loss-based override: any significant loss → treat as overuse
    if (m_lossRate > 0.05f) {
        sig = Signal::Overuse;
    }

    uint32_t newBitrate = m_currentBitrate;

    switch (sig) {
    case Signal::Underuse:
        // Ramp up multiplicatively
        newBitrate = static_cast<uint32_t>(
            static_cast<float>(m_currentBitrate) * m_config.rampUpFactor);
        break;

    case Signal::Normal:
        // Small additive increase
        newBitrate = m_currentBitrate + 200;  // +200kbps
        break;

    case Signal::Overuse:
        // Multiplicative decrease
        newBitrate = static_cast<uint32_t>(
            static_cast<float>(m_currentBitrate) * m_config.rampDownFactor);

        // Extra penalty for high loss
        if (m_lossRate > 0.15f) {
            newBitrate = static_cast<uint32_t>(
                static_cast<float>(newBitrate) * 0.9f);
        }
        break;
    }

    // Clamp
    newBitrate = std::max(m_config.minBitrateKbps, std::min(m_config.maxBitrateKbps, newBitrate));

    if (newBitrate != m_currentBitrate) {
        CC_TRACE("Congestion: gradient=%.3f loss=%.1f%% signal=%s bitrate %u→%u kbps",
                 m_gradientEstimate, m_lossRate * 100.0f,
                 sig == Signal::Overuse ? "OVERUSE" :
                 sig == Signal::Underuse ? "UNDERUSE" : "NORMAL",
                 m_currentBitrate, newBitrate);
        m_currentBitrate = newBitrate;
    }
    return m_currentBitrate;
}

}  // namespace cc::transport
