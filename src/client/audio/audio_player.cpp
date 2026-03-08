// Couch Conduit — Audio playback via WASAPI (Client side)
// Receives raw PCM float samples and plays them through the default audio endpoint

#include <couch_conduit/common/log.h>
#include <couch_conduit/common/types.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <mutex>
#include <vector>
#include <cstring>
#include <algorithm>

#pragma comment(lib, "ole32.lib")

namespace cc::client {

/// Audio player — receives PCM float samples and plays via WASAPI shared mode
class AudioPlayer {
public:
    AudioPlayer() = default;
    ~AudioPlayer() { Stop(); }

    bool Init(uint32_t sampleRate = 48000, uint32_t channels = 2) {
        m_sampleRate = sampleRate;
        m_channels = channels;

        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
            CC_ERROR("CoInitializeEx failed: 0x%08X", hr);
            return false;
        }

        // Get default audio endpoint (playback)
        IMMDeviceEnumerator* enumerator = nullptr;
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                              CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                              reinterpret_cast<void**>(&enumerator));
        if (FAILED(hr)) {
            CC_ERROR("Failed to create device enumerator: 0x%08X", hr);
            return false;
        }

        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m_device);
        enumerator->Release();
        if (FAILED(hr)) {
            CC_ERROR("GetDefaultAudioEndpoint failed: 0x%08X", hr);
            return false;
        }

        // Activate audio client
        hr = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                nullptr, reinterpret_cast<void**>(&m_audioClient));
        if (FAILED(hr)) {
            CC_ERROR("IAudioClient activation failed: 0x%08X", hr);
            return false;
        }

        // Get the mix format
        WAVEFORMATEX* mixFormat = nullptr;
        hr = m_audioClient->GetMixFormat(&mixFormat);
        if (FAILED(hr)) {
            CC_ERROR("GetMixFormat failed: 0x%08X", hr);
            return false;
        }

        m_deviceSampleRate = mixFormat->nSamplesPerSec;
        m_deviceChannels = mixFormat->nChannels;

        // Initialize in shared mode with low latency
        REFERENCE_TIME bufferDuration = 200000;  // 20ms buffer (100ns units)
        hr = m_audioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            0,
            bufferDuration,
            0,
            mixFormat,
            nullptr
        );
        CoTaskMemFree(mixFormat);

        if (FAILED(hr)) {
            CC_ERROR("IAudioClient::Initialize failed: 0x%08X", hr);
            return false;
        }

        // Get buffer size
        hr = m_audioClient->GetBufferSize(&m_bufferFrameCount);
        if (FAILED(hr)) {
            CC_ERROR("GetBufferSize failed: 0x%08X", hr);
            return false;
        }

        // Get render client
        hr = m_audioClient->GetService(__uuidof(IAudioRenderClient),
                                       reinterpret_cast<void**>(&m_renderClient));
        if (FAILED(hr)) {
            CC_ERROR("GetService(IAudioRenderClient) failed: 0x%08X", hr);
            return false;
        }

        // Pre-allocate ring buffer (500ms worth of audio)
        size_t ringSize = m_deviceSampleRate * m_deviceChannels / 2;
        m_ringBuffer.resize(ringSize, 0.0f);

        CC_INFO("AudioPlayer initialized: %uHz %uch, buffer=%u frames",
                m_deviceSampleRate, m_deviceChannels, m_bufferFrameCount);
        return true;
    }

    bool Start() {
        HRESULT hr = m_audioClient->Start();
        if (FAILED(hr)) {
            CC_ERROR("IAudioClient::Start failed: 0x%08X", hr);
            return false;
        }

        m_running = true;
        m_renderThread = std::thread([this]() {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
            RenderLoop();
        });

        CC_INFO("AudioPlayer started");
        return true;
    }

    void PlaySamples(const float* samples, uint32_t frameCount,
                     uint32_t sampleRate, uint32_t channels) {
        // Simple channel/samplerate matching — just copy for now
        // Real implementation would resample if rates differ
        std::lock_guard<std::mutex> lock(m_mutex);

        for (uint32_t i = 0; i < frameCount; ++i) {
            for (uint32_t ch = 0; ch < m_deviceChannels; ++ch) {
                float sample = 0.0f;
                if (ch < channels) {
                    sample = samples[i * channels + ch];
                }
                m_ringBuffer[m_writePos] = sample;
                m_writePos = (m_writePos + 1) % m_ringBuffer.size();
                if (m_writePos == m_readPos) {
                    // Ring buffer overflow — advance read position
                    m_readPos = (m_readPos + 1) % m_ringBuffer.size();
                }
            }
            m_availableFrames++;
        }
    }

    void Stop() {
        m_running = false;
        if (m_renderThread.joinable()) {
            m_renderThread.join();
        }
        if (m_audioClient) {
            m_audioClient->Stop();
            m_audioClient->Release();
            m_audioClient = nullptr;
        }
        if (m_renderClient) {
            m_renderClient->Release();
            m_renderClient = nullptr;
        }
        if (m_device) {
            m_device->Release();
            m_device = nullptr;
        }
        CC_INFO("AudioPlayer stopped");
    }

private:
    IMMDevice*         m_device = nullptr;
    IAudioClient*      m_audioClient = nullptr;
    IAudioRenderClient* m_renderClient = nullptr;

    uint32_t m_sampleRate = 48000;
    uint32_t m_channels = 2;
    uint32_t m_deviceSampleRate = 48000;
    uint32_t m_deviceChannels = 2;
    uint32_t m_bufferFrameCount = 0;

    // Ring buffer for audio samples
    std::vector<float> m_ringBuffer;
    size_t m_readPos = 0;
    size_t m_writePos = 0;
    std::atomic<uint32_t> m_availableFrames{0};
    std::mutex m_mutex;

    std::thread m_renderThread;
    std::atomic<bool> m_running{false};

    void RenderLoop() {
        while (m_running) {
            // Determine how many frames we can write
            UINT32 numFramesPadding = 0;
            HRESULT hr = m_audioClient->GetCurrentPadding(&numFramesPadding);
            if (FAILED(hr)) break;

            UINT32 numFramesAvailable = m_bufferFrameCount - numFramesPadding;
            if (numFramesAvailable == 0) {
                Sleep(1);
                continue;
            }

            BYTE* data = nullptr;
            hr = m_renderClient->GetBuffer(numFramesAvailable, &data);
            if (FAILED(hr)) {
                Sleep(1);
                continue;
            }

            float* outData = reinterpret_cast<float*>(data);
            DWORD flags = 0;

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                uint32_t framesToPlay = std::min(numFramesAvailable,
                                                  m_availableFrames.load());
                if (framesToPlay > 0) {
                    for (uint32_t i = 0; i < framesToPlay * m_deviceChannels; ++i) {
                        outData[i] = m_ringBuffer[m_readPos];
                        m_readPos = (m_readPos + 1) % m_ringBuffer.size();
                    }
                    m_availableFrames -= framesToPlay;

                    // Fill remaining with silence
                    for (uint32_t i = framesToPlay * m_deviceChannels;
                         i < numFramesAvailable * m_deviceChannels; ++i) {
                        outData[i] = 0.0f;
                    }
                } else {
                    flags = AUDCLNT_BUFFERFLAGS_SILENT;
                }
            }

            m_renderClient->ReleaseBuffer(numFramesAvailable, flags);
            Sleep(5);  // ~200Hz render loop
        }
    }
};

}  // namespace cc::client
