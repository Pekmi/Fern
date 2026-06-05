#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "../include/fern/multi_app_audio_capture.h"

#include "../include/fern/buffer.h"
#include "../include/fern/clock.h"
#include "../include/fern/encoder.h"
#include "../include/fern/isolatedAudioCapture.h"

#include <audiopolicy.h>
#include <mferror.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <filesystem>
#include <iostream>

using Microsoft::WRL::ComPtr;

namespace {

constexpr LONGLONG kSourceRefreshIntervalHns = fern::HnsPerSecond;

struct AudioProcessCandidate {
    DWORD pid = 0;
    std::wstring label;
};

std::wstring TrimExtension(std::wstring value) {
    const std::filesystem::path path(value);
    const std::wstring stem = path.stem().wstring();
    return stem.empty() ? value : stem;
}

std::wstring ProcessLabel(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return L"Application " + std::to_wstring(pid);

    wchar_t path[MAX_PATH] = {};
    DWORD length = static_cast<DWORD>(std::size(path));
    std::wstring label;
    if (QueryFullProcessImageNameW(process, 0, path, &length) && length > 0) {
        label = TrimExtension(std::filesystem::path(path).filename().wstring());
    }

    CloseHandle(process);
    if (label.empty()) label = L"Application " + std::to_wstring(pid);
    return label;
}

std::wstring SessionDisplayName(IAudioSessionControl* control) {
    if (!control) return L"";

    LPWSTR displayName = nullptr;
    HRESULT hr = control->GetDisplayName(&displayName);
    if (FAILED(hr) || !displayName) return L"";

    std::wstring value(displayName);
    CoTaskMemFree(displayName);

    if (value.empty() || value[0] == L'@') return L"";
    return value;
}

void AddActiveSessionsFromDevice(IMMDevice* device, std::vector<AudioProcessCandidate>& candidates) {
    if (!device) return;

    ComPtr<IAudioSessionManager2> sessionManager;
    HRESULT hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(sessionManager.GetAddressOf()));
    if (FAILED(hr) || !sessionManager) return;

    ComPtr<IAudioSessionEnumerator> sessions;
    hr = sessionManager->GetSessionEnumerator(&sessions);
    if (FAILED(hr) || !sessions) return;

    int count = 0;
    hr = sessions->GetCount(&count);
    if (FAILED(hr) || count <= 0) return;

    const DWORD currentPid = GetCurrentProcessId();
    for (int i = 0; i < count; ++i) {
        ComPtr<IAudioSessionControl> session;
        if (FAILED(sessions->GetSession(i, &session)) || !session) continue;

        AudioSessionState state = AudioSessionStateInactive;
        if (FAILED(session->GetState(&state)) || state != AudioSessionStateActive) continue;

        ComPtr<IAudioSessionControl2> session2;
        if (FAILED(session.As(&session2)) || !session2) continue;

        DWORD pid = 0;
        if (FAILED(session2->GetProcessId(&pid)) || pid == 0 || pid == currentPid) continue;

        const auto duplicate = std::find_if(candidates.begin(), candidates.end(), [pid](const AudioProcessCandidate& candidate) {
            return candidate.pid == pid;
        });
        if (duplicate != candidates.end()) continue;

        std::wstring label = SessionDisplayName(session.Get());
        if (label.empty()) label = ProcessLabel(pid);
        candidates.push_back({ pid, label });
    }
}

std::vector<AudioProcessCandidate> EnumerateActiveAudioProcesses() {
    std::vector<AudioProcessCandidate> candidates;

    ComPtr<IMMDeviceEnumerator> deviceEnumerator;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        IID_PPV_ARGS(&deviceEnumerator));
    if (FAILED(hr) || !deviceEnumerator) return candidates;

    ComPtr<IMMDeviceCollection> devices;
    hr = deviceEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices);
    if (FAILED(hr) || !devices) return candidates;

    UINT count = 0;
    hr = devices->GetCount(&count);
    if (FAILED(hr)) return candidates;

    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (SUCCEEDED(devices->Item(i, &device)) && device) {
            AddActiveSessionsFromDevice(device.Get(), candidates);
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const AudioProcessCandidate& a, const AudioProcessCandidate& b) {
        if (a.label == b.label) return a.pid < b.pid;
        return a.label < b.label;
    });

    return candidates;
}

void DrainAudioEncoder(IMFTransform* encoder, RingBuffer& ringBuffer, DWORD streamIndex) {
    if (!encoder) return;

    for (;;) {
        ComPtr<IMFSample> output;
        HRESULT hr = PullSampleFromEncoder(encoder, output);
        if (hr != S_OK || !output) break;
        ringBuffer.AddSample(output.Get(), streamIndex);
    }
}

}

namespace fern {

struct MultiAppAudioCapture::Source {
    DWORD pid = 0;
    DWORD streamIndex = 0;
    std::wstring label;
    std::unique_ptr<IsolatedAudioCapture> capture;
    ComPtr<IMFTransform> encoder;
    ComPtr<IMFMediaType> outputType;
};

MultiAppAudioCapture::MultiAppAudioCapture()
    : m_isRunning(false),
      m_masterStartRawQpc(0),
      m_lastRefreshHns(0),
      m_nextStreamIndex(1) {
}

MultiAppAudioCapture::~MultiAppAudioCapture() {
    Stop();
}

HRESULT MultiAppAudioCapture::Start() {
    if (m_isRunning) return S_FALSE;

    m_isRunning = true;
    m_lastRefreshHns = 0;
    RefreshSources();
    return S_OK;
}

void MultiAppAudioCapture::Stop() {
    m_isRunning = false;
    for (auto& source : m_sources) {
        if (source && source->capture) source->capture->Stop();
    }
}

void MultiAppAudioCapture::SetStartTime(UINT64 rawQpc) {
    m_masterStartRawQpc = rawQpc;
    for (auto& source : m_sources) {
        if (source && source->capture) source->capture->SetStartTime(rawQpc);
    }
}

void MultiAppAudioCapture::Pump(RingBuffer& ringBuffer) {
    if (!m_isRunning) return;

    const LONGLONG nowHns = CurrentQpcHns();
    if (ShouldRefreshSources(nowHns)) {
        RefreshSources();
    }

    for (auto& source : m_sources) {
        if (!source || !source->capture || !source->encoder) continue;

        for (int i = 0; i < 8; ++i) {
            ComPtr<IMFSample> input;
            HRESULT hr = source->capture->GetAudioSample(input);
            if (hr != S_OK || !input) break;

            hr = PushAudioToEncoder(source->encoder.Get(), input.Get());
            if (hr == MF_E_NOTACCEPTING) break;
            if (FAILED(hr)) {
                std::wcerr << L"AUDIO: ProcessInput failed for " << source->label
                           << L" (pid=" << source->pid << L") 0x"
                           << std::hex << hr << std::dec << std::endl;
                break;
            }

            DrainAudioEncoder(source->encoder.Get(), ringBuffer, source->streamIndex);
        }
    }
}

void MultiAppAudioCapture::AppendOutputTypes(std::vector<ComPtr<IMFMediaType>>& types) const {
    for (const auto& source : m_sources) {
        if (!source || source->streamIndex == 0) continue;

        if (types.size() <= source->streamIndex) {
            types.resize(static_cast<size_t>(source->streamIndex) + 1);
        }

        ComPtr<IMFMediaType> currentType;
        if (source->encoder && SUCCEEDED(source->encoder->GetOutputCurrentType(0, &currentType)) && currentType) {
            types[source->streamIndex] = currentType;
        } else if (source->outputType) {
            types[source->streamIndex] = source->outputType;
        }
    }
}

std::vector<AudioTrackMetadata> MultiAppAudioCapture::GetTrackMetadata() const {
    std::vector<AudioTrackMetadata> metadata;
    metadata.reserve(m_sources.size());

    for (const auto& source : m_sources) {
        if (!source || source->streamIndex == 0) continue;

        AudioTrackMetadata track;
        track.streamIndex = source->streamIndex;
        track.pid = source->pid;
        track.name = source->label;
        if (source->capture) {
            track.activityRanges = source->capture->GetActivityRanges();
        }
        metadata.push_back(std::move(track));
    }

    return metadata;
}

size_t MultiAppAudioCapture::SourceCount() const {
    return m_sources.size();
}

void MultiAppAudioCapture::RefreshSources() {
    if (!m_isRunning) return;

    const std::vector<AudioProcessCandidate> candidates = EnumerateActiveAudioProcesses();
    for (const auto& candidate : candidates) {
        if (candidate.pid == 0 || HasSource(candidate.pid)) continue;
        AddSource(candidate.pid, candidate.label);
    }

    m_lastRefreshHns = CurrentQpcHns();
}

bool MultiAppAudioCapture::ShouldRefreshSources(LONGLONG nowHns) const {
    return m_lastRefreshHns == 0 || nowHns - m_lastRefreshHns >= kSourceRefreshIntervalHns;
}

bool MultiAppAudioCapture::HasSource(DWORD pid) const {
    return std::any_of(m_sources.begin(), m_sources.end(), [pid](const std::unique_ptr<Source>& source) {
        return source && source->pid == pid;
    });
}

HRESULT MultiAppAudioCapture::AddSource(DWORD pid, const std::wstring& label) {
    auto capture = std::make_unique<IsolatedAudioCapture>(pid);
    HRESULT hr = capture->Start();
    if (FAILED(hr)) {
        std::wcerr << L"AUDIO: capture process-loopback failed for " << label
                   << L" (pid=" << pid << L") 0x" << std::hex << hr << std::dec << std::endl;
        return hr;
    }

    if (m_masterStartRawQpc != 0) {
        const LONGLONG masterStartHns = RawQpcToHns(static_cast<LONGLONG>(m_masterStartRawQpc));
        const LONGLONG timelineOffsetHns = std::max<LONGLONG>(0, CurrentQpcHns() - masterStartHns);
        capture->SetStartTime(m_masterStartRawQpc, timelineOffsetHns);
    }

    ComPtr<IMFTransform> encoder;
    hr = InitializeAudioEncoder(encoder, capture->GetFormat());
    if (FAILED(hr) || !encoder) {
        std::wcerr << L"AUDIO: encoder init failed for " << label
                   << L" (pid=" << pid << L") 0x" << std::hex << hr << std::dec << std::endl;
        capture->Stop();
        return FAILED(hr) ? hr : E_FAIL;
    }

    encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    ComPtr<IMFMediaType> outputType;
    hr = encoder->GetOutputCurrentType(0, &outputType);
    if (FAILED(hr) || !outputType) {
        std::wcerr << L"AUDIO: output type unavailable for " << label
                   << L" (pid=" << pid << L") 0x" << std::hex << hr << std::dec << std::endl;
        capture->Stop();
        return FAILED(hr) ? hr : E_FAIL;
    }

    auto source = std::make_unique<Source>();
    source->pid = pid;
    source->streamIndex = m_nextStreamIndex++;
    source->label = label;
    source->capture = std::move(capture);
    source->encoder = encoder;
    source->outputType = outputType;

    std::wcout << L"AUDIO: piste " << source->streamIndex << L" -> "
               << source->label << L" (pid=" << source->pid << L")" << std::endl;

    m_sources.push_back(std::move(source));
    return S_OK;
}

}
