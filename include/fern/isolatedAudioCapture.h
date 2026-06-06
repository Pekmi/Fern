#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <mfapi.h>
#include <mfidl.h>

#include "audio_activity.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

class IsolatedAudioCapture {
public:
    explicit IsolatedAudioCapture(DWORD targetPid);
    explicit IsolatedAudioCapture(std::wstring captureDeviceId);
    ~IsolatedAudioCapture();

    HRESULT Start();
    void Stop();

    WAVEFORMATEX* GetFormat() { return reinterpret_cast<WAVEFORMATEX*>(&m_mixFormat); }

    // The caller passes raw QueryPerformanceCounter ticks. Internally the
    // master clock is stored in the same 100 ns unit used by WASAPI packets.
    void SetStartTime(UINT64 rawQpc, LONGLONG timelineOffsetHns = 0);

    HRESULT GetAudioSample(ComPtr<IMFSample>& pSample);
    std::vector<fern::AudioActivityRange> GetActivityRanges();

private:
    class CompletionHandler;

    void CaptureLoop();

    UINT64 HnsToFrame(LONGLONG hns) const;
    LONGLONG FramesToHns(UINT64 frames) const;

    void AppendSilenceUntilFrameLocked(UINT64 targetFrame);
    void AppendPacketLocked(const BYTE* data, UINT32 frames, DWORD flags, UINT64 packetQpcHns);
    void AppendContinuousPacketLocked(const BYTE* data, UINT32 frames, DWORD flags, UINT64 packetQpcHns);
    void AppendConvertedFramesLocked(const BYTE* data, UINT32 frameOffset, UINT32 frames, DWORD flags);
    void RecordActivityLocked(UINT64 startFrame, const short* samples, UINT32 frames);
    void AddActivityRangeLocked(UINT64 startFrame, UINT64 frames);

    bool IsFloatMixFormat() const;
    bool IsPcmMixFormat() const;

    DWORD m_targetPid;
    bool m_useInputDevice;
    std::wstring m_captureDeviceId;
    WAVEFORMATEXTENSIBLE m_mixFormat;

    ComPtr<IAudioClient> m_audioClient;
    ComPtr<IAudioCaptureClient> m_captureClient;

    std::thread m_captureThread;
    std::atomic<bool> m_isCapturing;
    HANDLE m_activationFinishedEvent;

    std::vector<short> m_pcmBuffer;
    std::mutex m_audioMutex;

    std::atomic<UINT64> m_masterStartHns;
    UINT64 m_timelineFramesWritten;
    UINT64 m_framesSent;
    bool m_hasAlignedFirstPacket;
    std::vector<fern::AudioActivityRange> m_activityRanges;
};
