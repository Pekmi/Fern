#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <mfidl.h>
#include <wrl/client.h>

#include "replay_export.h"

#include <memory>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

class RingBuffer;

namespace fern {

class MultiAppAudioCapture {
public:
    MultiAppAudioCapture();
    ~MultiAppAudioCapture();

    HRESULT Start();
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
    HRESULT AddSource(DWORD pid, const std::wstring& label);

    std::vector<std::unique_ptr<Source>> m_sources;
    bool m_isRunning;
    UINT64 m_masterStartRawQpc;
    LONGLONG m_lastRefreshHns;
    DWORD m_nextStreamIndex;
};

}
