// Couch Conduit — WASAPI loopback audio capture (Host side)
// Captures system audio output for streaming to client

#include <couch_conduit/common/log.h>
#include <couch_conduit/common/types.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

#include <thread>
#include <atomic>
#include <functional>
#include <vector>

#pragma comment(lib, "ole32.lib")

namespace cc::host {

/// WASAPI loopback audio capture
/// Captures the default audio endpoint's output via loopback mode
class WasapiCapture {
public:
    using AudioCallback = std::function<void(const float* samples, uint32_t frameCount,
                                             uint32_t sampleRate, uint32_t channels)>;

    WasapiCapture() = default;
    ~WasapiCapture() { Stop(); }

    bool Init(AudioCallback callback) {
        m_callback = std::move(callback);

        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
            CC_ERROR("CoInitializeEx failed: 0x%08X", hr);
            return false;
        }

        // Get default audio endpoint
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

        // Get mix format
        WAVEFORMATEX* mixFormat = nullptr;
        hr = m_audioClient->GetMixFormat(&mixFormat);
        if (FAILED(hr)) {
            CC_ERROR("GetMixFormat failed: 0x%08X", hr);
            return false;
        }

        m_sampleRate = mixFormat->nSamplesPerSec;
        m_channels = mixFormat->nChannels;

        // Initialize in loopback mode with low latency
        REFERENCE_TIME bufferDuration = 200000;  // 20ms buffer (100ns units)
        hr = m_audioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK,
            bufferDuration,
            0,
            mixFormat,
            nullptr
        );
        CoTaskMemFree(mixFormat);

        if (FAILED(hr)) {
            CC_ERROR("IAudioClient::Initialize (loopback) failed: 0x%08X", hr);
            return false;
        }

        hr = m_audioClient->GetService(__uuidof(IAudioCaptureClient),
                                       reinterpret_cast<void**>(&m_captureClient));
        if (FAILED(hr)) {
            CC_ERROR("GetService(IAudioCaptureClient) failed: 0x%08X", hr);
            return false;
        }

        CC_INFO("WASAPI loopback initialized: %uHz, %u channels", m_sampleRate, m_channels);
        return true;
    }

    bool Start() {
        HRESULT hr = m_audioClient->Start();
        if (FAILED(hr)) {
            CC_ERROR("IAudioClient::Start failed: 0x%08X", hr);
            return false;
        }

        m_running = true;
        m_captureThread = std::thread([this]() {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
            CaptureLoop();
        });

        CC_INFO("WASAPI capture started");
        return true;
    }

    void Stop() {
        m_running = false;
        if (m_captureThread.joinable()) {
            m_captureThread.join();
        }
        if (m_audioClient) {
            m_audioClient->Stop();
            m_audioClient->Release();
            m_audioClient = nullptr;
        }
        if (m_captureClient) {
            m_captureClient->Release();
            m_captureClient = nullptr;
        }
        if (m_device) {
            m_device->Release();
            m_device = nullptr;
        }
    }

private:
    IMMDevice*           m_device = nullptr;
    IAudioClient*        m_audioClient = nullptr;
    IAudioCaptureClient* m_captureClient = nullptr;

    AudioCallback m_callback;
    uint32_t      m_sampleRate = 0;
    uint32_t      m_channels = 0;

    std::thread       m_captureThread;
    std::atomic<bool> m_running{false};

    void CaptureLoop() {
        while (m_running) {
            // Check for available audio data
            UINT32 packetLength = 0;
            HRESULT hr = m_captureClient->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) break;

            while (packetLength > 0) {
                BYTE* data = nullptr;
                UINT32 numFrames = 0;
                DWORD flags = 0;

                hr = m_captureClient->GetBuffer(&data, &numFrames, &flags, nullptr, nullptr);
                if (FAILED(hr)) break;

                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && m_callback) {
                    m_callback(reinterpret_cast<float*>(data), numFrames,
                               m_sampleRate, m_channels);
                }

                m_captureClient->ReleaseBuffer(numFrames);
                m_captureClient->GetNextPacketSize(&packetLength);
            }

            // Sleep briefly to avoid busy-waiting
            // 5ms = ~200Hz polling, well within audio buffer size
            Sleep(5);
        }
    }
};

}  // namespace cc::host
