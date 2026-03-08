#pragma once
// Couch Conduit — Congestion Estimator
// GCC-like (Google Congestion Control) adaptive bitrate using TWCC feedback.
// Uses one-way delay gradient + loss-based approach to converge on optimal bitrate.

#include <cstdint>
#include <deque>
#include <chrono>

namespace cc::transport {

class CongestionEstimator {
public:
    struct Config {
        uint32_t minBitrateKbps =  2000;  //  2 Mbps floor
        uint32_t maxBitrateKbps = 80000;  // 80 Mbps ceiling
        uint32_t startBitrateKbps = 20000;
        float    rampUpFactor     = 1.08f; // +8% per interval when underused
        float    rampDownFactor   = 0.85f; // -15% on overuse
        int64_t  intervalUs       = 200000; // 200ms decision interval
    };

    void Init(const Config& cfg);

    /// Call with each TWCC feedback batch received from the client.
    /// sendTimestampsUs: the send times (host-side) for each sequence reported.
    /// arrivalDeltasUs: the inter-arrival deltas as reported by the client.
    /// count: number of entries.
    void OnTwccFeedback(const uint16_t* sequences,
                        const int64_t* sendTimesUs,
                        const int16_t* arrivalDeltaQuarterMs,
                        int count);

    /// Call with loss info from feedback packet.
    void OnLossUpdate(float lossRate);

    /// Must be called periodically (e.g. every feedback packet).
    /// Returns the recommended bitrate in kbps, or 0 if no change.
    uint32_t ComputeBitrate();

    uint32_t GetCurrentBitrateKbps() const { return m_currentBitrate; }

private:
    Config m_config;
    uint32_t m_currentBitrate = 20000;

    // Delay-gradient state
    struct DelayGradientSample {
        int64_t sendDelta;    // Inter-send time (us)
        int64_t arrivalDelta; // Inter-arrival time (us)
    };
    std::deque<DelayGradientSample> m_delaySamples;

    float    m_gradientEstimate = 0.0f;  // Smoothed delay gradient
    float    m_lossRate         = 0.0f;
    int64_t  m_lastDecisionUs   = 0;

    enum class Signal { Underuse, Normal, Overuse };
    Signal ClassifyDelay() const;
};

}  // namespace cc::transport
