#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <mfidl.h>
#include <wrl/client.h>

#include "replay_export.h"
#include "settings.h"

#include <memory>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

class RingBuffer;
class IsolatedAudioCapture;

namespace fern {

class MultiAppAudioCapture {
public:
    MultiAppAudioCapture();
    ~MultiAppAudioCapture();

    HRESULT Start(const Settings& settings);
    void Stop();

    void SetStartTime(UINT64 rawQpc);
    void Pump(RingBuffer& ringBuffer);
    void AppendOutputTypes(std::vector<ComPtr<IMFMediaType>>& types) const;
    std::vector<AudioTrackMetadata> GetTrackMetadata() const;

    size_t SourceCount() const;

private:
    struct Source;

    void RefreshSources();
    bool ShouldRefreshSources(LONGLONG nowHns) const;
    bool HasSource(DWORD pid) const;
    Source* FindSource(DWORD pid) const;
    void PruneInactiveSources(LONGLONG nowHns);
    HRESULT AddSource(DWORD pid, const std::wstring& label);
    HRESULT AddMicrophoneSource(const std::wstring& deviceId);
    HRESULT AddCaptureSource(std::unique_ptr<IsolatedAudioCapture> capture, DWORD pid, const std::wstring& label);

    std::vector<std::unique_ptr<Source>> m_sources;
    bool m_isRunning;
    UINT64 m_masterStartRawQpc;
    LONGLONG m_replayBufferDurationHns;
    LONGLONG m_lastRefreshHns;
    std::wstring m_lastCandidateSignature;
    DWORD m_nextStreamIndex;
};

}
