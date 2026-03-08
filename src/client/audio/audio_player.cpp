// Couch Conduit — Audio playback via WASAPI (Client side)
// Placeholder for Opus decode → WASAPI exclusive/shared mode playback

#include <couch_conduit/common/log.h>
#include <couch_conduit/common/types.h>

namespace cc::client {

/// Audio player — receives Opus-encoded audio and plays via WASAPI
class AudioPlayer {
public:
    bool Init() {
        CC_INFO("AudioPlayer initialized (stub — Opus + WASAPI playback TBD)");
        return true;
    }

    void PlaySamples(const float* samples, uint32_t frameCount,
                     uint32_t sampleRate, uint32_t channels) {
        // TODO: Implement WASAPI shared-mode playback
        // 1. Initialize IAudioClient in shared mode
        // 2. Get IAudioRenderClient
        // 3. Write decoded PCM samples to the render buffer
        (void)samples; (void)frameCount; (void)sampleRate; (void)channels;
    }

    void Stop() {
        CC_INFO("AudioPlayer stopped");
    }
};

}  // namespace cc::client
