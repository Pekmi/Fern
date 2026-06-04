#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <audioclientactivationparams.h>
#include <wrl/implements.h>
#include <mmdeviceapi.h>
#include <mferror.h>
#include <propvarutil.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>

#include "../include/fern/clock.h"
#include "../include/fern/isolatedAudioCapture.h"

using namespace Microsoft::WRL;

namespace {
constexpr UINT32 kFramesPerSample = 1024;
constexpr LONGLONG kRealtimeSilenceLagHns = 2000000LL;

short FloatToPcm16(float value) {
    value = std::clamp(value, -1.0f, 1.0f);
    return static_cast<short>(std::lrintf(value * 32767.0f));
}

short Int24ToPcm16(const BYTE* src) {
    int value = src[0] | (src[1] << 8) | (src[2] << 16);
    if (value & 0x00800000) value |= ~0x00FFFFFF;
    return static_cast<short>(value >> 8);
}
}

class IsolatedAudioCapture::CompletionHandler :
    public RuntimeClass<RuntimeClassFlags<ClassicCom>, IActivateAudioInterfaceCompletionHandler>
{
public:
    explicit CompletionHandler(IsolatedAudioCapture* parent) : m_parent(parent) {}

    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
        HRESULT hrActivate = S_OK;
        ComPtr<IUnknown> punk;
        operation->GetActivateResult(&hrActivate, &punk);
        if (SUCCEEDED(hrActivate)) punk.As(&m_parent->m_audioClient);
        if (m_parent->m_activationFinishedEvent) SetEvent(m_parent->m_activationFinishedEvent);
        return S_OK;
    }

private:
    IsolatedAudioCapture* m_parent;
};

IsolatedAudioCapture::IsolatedAudioCapture(DWORD targetPid)
    : m_targetPid(targetPid),
      m_isCapturing(false),
      m_activationFinishedEvent(NULL),
      m_masterStartHns(0),
      m_timelineFramesWritten(0),
      m_framesSent(0) {
    std::memset(&m_mixFormat, 0, sizeof(m_mixFormat));
}

IsolatedAudioCapture::~IsolatedAudioCapture() {
    Stop();
    if (m_activationFinishedEvent) CloseHandle(m_activationFinishedEvent);
}

void IsolatedAudioCapture::SetStartTime(UINT64 rawQpc) {
    std::lock_guard<std::mutex> lock(m_audioMutex);
    m_pcmBuffer.clear();
    m_timelineFramesWritten = 0;
    m_framesSent = 0;
    m_masterStartHns.store(static_cast<UINT64>(fern::RawQpcToHns(static_cast<LONGLONG>(rawQpc))), std::memory_order_release);
}

HRESULT IsolatedAudioCapture::Start() {
    if (m_isCapturing) return S_FALSE;

    if (m_activationFinishedEvent) {
        CloseHandle(m_activationFinishedEvent);
        m_activationFinishedEvent = NULL;
    }
    m_activationFinishedEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    {
        std::lock_guard<std::mutex> lock(m_audioMutex);
        m_pcmBuffer.clear();
        m_timelineFramesWritten = 0;
        m_framesSent = 0;
        m_masterStartHns.store(0, std::memory_order_release);
    }

    HRESULT hr = S_OK;
    if (m_targetPid != 0) {
        AUDIOCLIENT_ACTIVATION_PARAMS params = { AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK };
        params.ProcessLoopbackParams.TargetProcessId = m_targetPid;
        params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

        PROPVARIANT prop;
        PropVariantInit(&prop);
        prop.vt = VT_BLOB;
        prop.blob.cbSize = sizeof(params);
        prop.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

        auto handler = Make<CompletionHandler>(this);
        ComPtr<IActivateAudioInterfaceAsyncOperation> asyncOp;
        hr = ActivateAudioInterfaceAsync(
            VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
            __uuidof(IAudioClient),
            &prop,
            handler.Get(),
            &asyncOp);
        if (FAILED(hr)) return hr;
        WaitForSingleObject(m_activationFinishedEvent, 2000);
    } else {
        ComPtr<IMMDeviceEnumerator> pEnum;
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, IID_PPV_ARGS(&pEnum));
        if (FAILED(hr)) return hr;

        ComPtr<IMMDevice> pDev;
        hr = pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDev);
        if (FAILED(hr)) return hr;

        hr = pDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, reinterpret_cast<void**>(m_audioClient.GetAddressOf()));
        if (FAILED(hr)) return hr;
    }

    if (!m_audioClient) return E_FAIL;

    WAVEFORMATEX* pwfx = nullptr;
    hr = m_audioClient->GetMixFormat(&pwfx);
    if (FAILED(hr) || !pwfx) return FAILED(hr) ? hr : E_FAIL;

    if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        std::memset(&m_mixFormat, 0, sizeof(WAVEFORMATEXTENSIBLE));
        std::memcpy(&m_mixFormat, pwfx, std::min<size_t>(sizeof(WAVEFORMATEXTENSIBLE), pwfx->cbSize + sizeof(WAVEFORMATEX)));
    } else {
        std::memset(&m_mixFormat, 0, sizeof(WAVEFORMATEXTENSIBLE));
        std::memcpy(&m_mixFormat.Format, pwfx, sizeof(WAVEFORMATEX));
    }
    CoTaskMemFree(pwfx);

    hr = m_audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK,
        1000000,
        0,
        reinterpret_cast<WAVEFORMATEX*>(&m_mixFormat),
        NULL);
    if (FAILED(hr)) return hr;

    hr = m_audioClient->GetService(IID_PPV_ARGS(&m_captureClient));
    if (FAILED(hr)) return hr;

    hr = m_audioClient->Start();
    if (FAILED(hr)) return hr;

    m_isCapturing = true;
    m_captureThread = std::thread(&IsolatedAudioCapture::CaptureLoop, this);
    return S_OK;
}

void IsolatedAudioCapture::Stop() {
    m_isCapturing = false;
    if (m_captureThread.joinable()) m_captureThread.join();
    if (m_audioClient) m_audioClient->Stop();
}

UINT64 IsolatedAudioCapture::HnsToFrame(LONGLONG hns) const {
    const UINT32 sampleRate = m_mixFormat.Format.nSamplesPerSec;
    if (hns <= 0 || sampleRate == 0) return 0;

    long double frames = (static_cast<long double>(hns) * sampleRate) /
                         static_cast<long double>(fern::HnsPerSecond);
    return static_cast<UINT64>(std::llround(frames));
}

LONGLONG IsolatedAudioCapture::FramesToHns(UINT64 frames) const {
    const UINT32 sampleRate = m_mixFormat.Format.nSamplesPerSec;
    if (sampleRate == 0) return 0;

    long double hns = (static_cast<long double>(frames) * fern::HnsPerSecond) /
                      static_cast<long double>(sampleRate);
    return static_cast<LONGLONG>(std::llround(hns));
}

bool IsolatedAudioCapture::IsFloatMixFormat() const {
    const WAVEFORMATEX& fmt = m_mixFormat.Format;
    if (fmt.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    return fmt.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
           IsEqualGUID(m_mixFormat.SubFormat, MFAudioFormat_Float);
}

bool IsolatedAudioCapture::IsPcmMixFormat() const {
    const WAVEFORMATEX& fmt = m_mixFormat.Format;
    if (fmt.wFormatTag == WAVE_FORMAT_PCM) return true;
    return fmt.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
           IsEqualGUID(m_mixFormat.SubFormat, MFAudioFormat_PCM);
}

void IsolatedAudioCapture::AppendSilenceUntilFrameLocked(UINT64 targetFrame) {
    const UINT32 channels = m_mixFormat.Format.nChannels;
    if (channels == 0 || targetFrame <= m_timelineFramesWritten) return;

    const UINT64 framesToAdd = targetFrame - m_timelineFramesWritten;
    m_pcmBuffer.insert(
        m_pcmBuffer.end(),
        static_cast<size_t>(framesToAdd) * channels,
        0);
    m_timelineFramesWritten = targetFrame;
}

void IsolatedAudioCapture::AppendConvertedFramesLocked(const BYTE* data, UINT32 frameOffset, UINT32 frames, DWORD flags) {
    const WAVEFORMATEX& fmt = m_mixFormat.Format;
    const UINT32 channels = fmt.nChannels;
    if (channels == 0 || frames == 0) return;

    const size_t totalSamples = static_cast<size_t>(frames) * channels;
    const size_t destOffset = m_pcmBuffer.size();
    m_pcmBuffer.resize(destOffset + totalSamples, 0);

    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !data) return;

    short* dest = m_pcmBuffer.data() + destOffset;
    const BYTE* srcBytes = data + static_cast<size_t>(frameOffset) * fmt.nBlockAlign;

    if (IsFloatMixFormat()) {
        if (fmt.wBitsPerSample == 32) {
            const float* src = reinterpret_cast<const float*>(srcBytes);
            for (size_t i = 0; i < totalSamples; ++i) dest[i] = FloatToPcm16(src[i]);
        } else if (fmt.wBitsPerSample == 64) {
            const double* src = reinterpret_cast<const double*>(srcBytes);
            for (size_t i = 0; i < totalSamples; ++i) dest[i] = FloatToPcm16(static_cast<float>(src[i]));
        }
        return;
    }

    if (!IsPcmMixFormat()) return;

    switch (fmt.wBitsPerSample) {
    case 8: {
        for (size_t i = 0; i < totalSamples; ++i) {
            dest[i] = static_cast<short>((static_cast<int>(srcBytes[i]) - 128) << 8);
        }
        break;
    }
    case 16: {
        std::memcpy(dest, srcBytes, totalSamples * sizeof(short));
        break;
    }
    case 24: {
        for (size_t i = 0; i < totalSamples; ++i) {
            dest[i] = Int24ToPcm16(srcBytes + i * 3);
        }
        break;
    }
    case 32: {
        const int32_t* src = reinterpret_cast<const int32_t*>(srcBytes);
        for (size_t i = 0; i < totalSamples; ++i) dest[i] = static_cast<short>(src[i] >> 16);
        break;
    }
    default:
        break;
    }
}

void IsolatedAudioCapture::AppendPacketLocked(const BYTE* data, UINT32 frames, DWORD flags, UINT64 packetQpcHns) {
    const UINT64 masterStartHns = m_masterStartHns.load(std::memory_order_acquire);
    const UINT32 sampleRate = m_mixFormat.Format.nSamplesPerSec;
    if (masterStartHns == 0 || sampleRate == 0 || frames == 0) return;

    UINT64 packetStartHns = packetQpcHns;
    if ((flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) || packetStartHns == 0) {
        const UINT64 nowHns = static_cast<UINT64>(fern::CurrentQpcHns());
        const LONGLONG packetDurationHns = FramesToHns(frames);
        packetStartHns = nowHns > static_cast<UINT64>(packetDurationHns)
            ? nowHns - static_cast<UINT64>(packetDurationHns)
            : nowHns;
    }

    LONGLONG relativeHns = 0;
    if (packetStartHns >= masterStartHns) {
        relativeHns = static_cast<LONGLONG>(packetStartHns - masterStartHns);
    } else {
        const UINT64 delta = masterStartHns - packetStartHns;
        relativeHns = delta > static_cast<UINT64>(std::numeric_limits<LONGLONG>::max())
            ? std::numeric_limits<LONGLONG>::min()
            : -static_cast<LONGLONG>(delta);
    }

    long double startFrameFloat = (static_cast<long double>(relativeHns) * sampleRate) /
                                  static_cast<long double>(fern::HnsPerSecond);
    LONGLONG signedStartFrame = static_cast<LONGLONG>(std::llround(startFrameFloat));

    UINT32 frameOffset = 0;
    if (signedStartFrame < 0) {
        const long double trimFrameCount = -startFrameFloat;
        if (trimFrameCount >= static_cast<long double>(frames)) return;
        const UINT64 trimFrames = static_cast<UINT64>(std::llround(trimFrameCount));
        if (trimFrames >= frames) return;
        frameOffset = static_cast<UINT32>(trimFrames);
        signedStartFrame = 0;
    }

    const LONGLONG timelineFrame = m_timelineFramesWritten > static_cast<UINT64>(std::numeric_limits<LONGLONG>::max())
        ? std::numeric_limits<LONGLONG>::max()
        : static_cast<LONGLONG>(m_timelineFramesWritten);

    const LONGLONG jitterToleranceFrames = std::max<LONGLONG>(1, static_cast<LONGLONG>(sampleRate / 100));
    LONGLONG deltaFrames = signedStartFrame - timelineFrame;

    if (deltaFrames > jitterToleranceFrames) {
        AppendSilenceUntilFrameLocked(static_cast<UINT64>(signedStartFrame));
    } else if (deltaFrames < -jitterToleranceFrames) {
        const UINT64 overlapFrames = static_cast<UINT64>(-deltaFrames);
        if (overlapFrames >= static_cast<UINT64>(frames - frameOffset)) return;
        frameOffset += static_cast<UINT32>(overlapFrames);
    } else {
        // WASAPI packet timestamps have small scheduling/device jitter. Do not
        // insert/drop samples for sub-10 ms deltas; that creates audible clicks.
        signedStartFrame = timelineFrame;
    }

    const UINT32 framesToAppend = frames - frameOffset;
    AppendConvertedFramesLocked(data, frameOffset, framesToAppend, flags);
    m_timelineFramesWritten += framesToAppend;
}

HRESULT IsolatedAudioCapture::GetAudioSample(ComPtr<IMFSample>& pSample) {
    pSample.Reset();

    const UINT32 channels = m_mixFormat.Format.nChannels;
    if (channels == 0 || m_mixFormat.Format.nSamplesPerSec == 0) return MF_E_INVALIDTYPE;

    std::vector<short> pcm;
    LONGLONG hnsTime = 0;
    LONGLONG hnsDuration = 0;

    {
        std::lock_guard<std::mutex> lock(m_audioMutex);

        const UINT64 masterStartHns = m_masterStartHns.load(std::memory_order_acquire);
        if (masterStartHns == 0) return S_FALSE;

        const UINT64 nowHns = static_cast<UINT64>(fern::CurrentQpcHns());
        if (nowHns > masterStartHns + kRealtimeSilenceLagHns) {
            AppendSilenceUntilFrameLocked(HnsToFrame(static_cast<LONGLONG>(nowHns - masterStartHns - kRealtimeSilenceLagHns)));
        }

        const size_t samplesNeeded = static_cast<size_t>(kFramesPerSample) * channels;
        if (m_pcmBuffer.size() < samplesNeeded) return S_FALSE;

        pcm.assign(m_pcmBuffer.begin(), m_pcmBuffer.begin() + samplesNeeded);
        m_pcmBuffer.erase(m_pcmBuffer.begin(), m_pcmBuffer.begin() + samplesNeeded);

        hnsTime = FramesToHns(m_framesSent);
        hnsDuration = std::max<LONGLONG>(1, FramesToHns(m_framesSent + kFramesPerSample) - hnsTime);
        m_framesSent += kFramesPerSample;
    }

    ComPtr<IMFMediaBuffer> pBuffer;
    const DWORD bufferSize = static_cast<DWORD>(pcm.size() * sizeof(short));
    HRESULT hr = MFCreateMemoryBuffer(bufferSize, &pBuffer);
    if (FAILED(hr)) return hr;

    BYTE* pDest = nullptr;
    hr = pBuffer->Lock(&pDest, nullptr, nullptr);
    if (FAILED(hr)) return hr;

    std::memcpy(pDest, pcm.data(), bufferSize);
    pBuffer->Unlock();
    pBuffer->SetCurrentLength(bufferSize);

    hr = MFCreateSample(&pSample);
    if (FAILED(hr)) return hr;

    pSample->AddBuffer(pBuffer.Get());
    pSample->SetSampleTime(hnsTime);
    pSample->SetSampleDuration(hnsDuration);
    return S_OK;
}

void IsolatedAudioCapture::CaptureLoop() {
    HRESULT coHr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    const bool coInitialized = SUCCEEDED(coHr);

    while (m_isCapturing) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (m_masterStartHns.load(std::memory_order_acquire) == 0 || !m_captureClient) continue;

        UINT32 packetSize = 0;
        HRESULT hr = m_captureClient->GetNextPacketSize(&packetSize);
        if (FAILED(hr)) continue;

        while (packetSize > 0 && m_isCapturing) {
            BYTE* pData = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 devicePosition = 0;
            UINT64 qpcPositionHns = 0;

            hr = m_captureClient->GetBuffer(&pData, &frames, &flags, &devicePosition, &qpcPositionHns);
            if (FAILED(hr)) break;

            {
                std::lock_guard<std::mutex> lock(m_audioMutex);
                AppendPacketLocked(pData, frames, flags, qpcPositionHns);
            }

            m_captureClient->ReleaseBuffer(frames);

            hr = m_captureClient->GetNextPacketSize(&packetSize);
            if (FAILED(hr)) break;
        }
    }

    if (coInitialized) CoUninitialize();
}
