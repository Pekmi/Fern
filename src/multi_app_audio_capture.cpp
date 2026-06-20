#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "../include/fern/multi_app_audio_capture.h"

#include "../include/fern/buffer.h"
#include "../include/fern/clock.h"
#include "../include/fern/encoder.h"
#include "../include/fern/isolatedAudioCapture.h"
#include "../include/fern/logger.h"

#include <audiopolicy.h>
#include <mferror.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <limits>
#include <sstream>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace {

constexpr LONGLONG kSourceRefreshIntervalHns = fern::HnsPerSecond;
constexpr LONGLONG kEncoderTimestampRebaseToleranceHns = fern::HnsPerSecond / 20;
constexpr LONGLONG kInactiveSourceGraceHns = 5 * fern::HnsPerSecond;
constexpr int kMaxAudioSamplesPerPump = 64;

struct AudioEncoderTimeline {
    bool hasFirstInputTime = false;
    LONGLONG firstInputTimeHns = 0;
    bool hasOutputOffset = false;
    LONGLONG outputOffsetHns = 0;
    LONGLONG lastOutputTimeHns = -1;
};

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

std::wstring WaveFormatText(const WAVEFORMATEX* format) {
    if (!format) return L"<null format>";

    std::wostringstream stream;
    stream << L"tag=0x" << std::hex << format->wFormatTag << std::dec
           << L" channels=" << format->nChannels
           << L" sampleRate=" << format->nSamplesPerSec
           << L" bits=" << format->wBitsPerSample
           << L" blockAlign=" << format->nBlockAlign
           << L" avgBytesPerSec=" << format->nAvgBytesPerSec;
    return stream.str();
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

void NoteAcceptedAudioInput(IMFSample* sample, AudioEncoderTimeline& timeline) {
    if (!sample || timeline.hasFirstInputTime) return;

    LONGLONG inputTime = 0;
    if (SUCCEEDED(sample->GetSampleTime(&inputTime))) {
        timeline.firstInputTimeHns = inputTime;
        timeline.hasFirstInputTime = true;
    }
}

LONGLONG SaturatingAddHns(LONGLONG value, LONGLONG offset) {
    if (offset > 0 && value > std::numeric_limits<LONGLONG>::max() - offset) {
        return std::numeric_limits<LONGLONG>::max();
    }
    if (offset < 0 && value < std::numeric_limits<LONGLONG>::min() - offset) {
        return std::numeric_limits<LONGLONG>::min();
    }
    return value + offset;
}

void OffsetDecodeTimestamp(IMFSample* sample, LONGLONG offset) {
    if (!sample || offset <= 0) return;

    UINT64 dts = 0;
    if (FAILED(sample->GetUINT64(MFSampleExtension_DecodeTimestamp, &dts))) return;

    const UINT64 unsignedOffset = static_cast<UINT64>(offset);
    const UINT64 adjusted = dts > std::numeric_limits<UINT64>::max() - unsignedOffset
        ? std::numeric_limits<UINT64>::max()
        : dts + unsignedOffset;
    sample->SetUINT64(MFSampleExtension_DecodeTimestamp, adjusted);
}

void AlignEncodedAudioTimestamp(IMFSample* sample, AudioEncoderTimeline& timeline) {
    if (!sample) return;

    LONGLONG outputTime = 0;
    if (FAILED(sample->GetSampleTime(&outputTime))) return;

    if (!timeline.hasOutputOffset) {
        timeline.outputOffsetHns = 0;
        if (timeline.hasFirstInputTime &&
            timeline.firstInputTimeHns > kEncoderTimestampRebaseToleranceHns &&
            outputTime + kEncoderTimestampRebaseToleranceHns < timeline.firstInputTimeHns) {
            timeline.outputOffsetHns = timeline.firstInputTimeHns - outputTime;
        }
        timeline.hasOutputOffset = true;
    }

    if (timeline.outputOffsetHns != 0) {
        outputTime = SaturatingAddHns(outputTime, timeline.outputOffsetHns);
        sample->SetSampleTime(outputTime);
        OffsetDecodeTimestamp(sample, timeline.outputOffsetHns);
    }

    if (timeline.lastOutputTimeHns >= 0 && outputTime <= timeline.lastOutputTimeHns) {
        outputTime = timeline.lastOutputTimeHns + 1;
        sample->SetSampleTime(outputTime);
    }

    timeline.lastOutputTimeHns = outputTime;
}

void DrainAudioEncoder(IMFTransform* encoder, RingBuffer& ringBuffer, DWORD streamIndex, AudioEncoderTimeline& timeline) {
    if (!encoder) return;

    for (;;) {
        ComPtr<IMFSample> output;
        HRESULT hr = PullSampleFromEncoder(encoder, output);
        if (hr != S_OK || !output) break;
        AlignEncodedAudioTimestamp(output.Get(), timeline);
        ringBuffer.AddSample(output.Get(), streamIndex);
    }
}

}

namespace fern {

struct MultiAppAudioCapture::Source {
    DWORD pid = 0;
    DWORD streamIndex = 0;
    LONGLONG lastSeenHns = 0;
    std::wstring label;
    std::unique_ptr<IsolatedAudioCapture> capture;
    ComPtr<IMFTransform> encoder;
    ComPtr<IMFMediaType> outputType;
    AudioEncoderTimeline timeline;
};

MultiAppAudioCapture::MultiAppAudioCapture()
    : m_isRunning(false),
      m_masterStartRawQpc(0),
      m_replayBufferDurationHns(0),
      m_lastRefreshHns(0),
      m_nextStreamIndex(1) {
}

MultiAppAudioCapture::~MultiAppAudioCapture() {
    Stop();
}

HRESULT MultiAppAudioCapture::Start(const Settings& settings) {
    if (m_isRunning) return S_FALSE;

    m_isRunning = true;
    m_replayBufferDurationHns = std::max<LONGLONG>(0, static_cast<LONGLONG>(settings.bufferDuration)) * HnsPerSecond;
    m_lastRefreshHns = 0;
    m_lastCandidateSignature.clear();
    fern::LogInfo(L"AUDIO", L"Multi-app audio capture starting.");
    AddMicrophoneSource(settings.microphoneDeviceId);
    RefreshSources();
    return S_OK;
}

void MultiAppAudioCapture::Stop() {
    if (m_isRunning) {
        fern::LogInfo(L"AUDIO", L"Multi-app audio capture stopping.");
    }
    m_isRunning = false;
    for (auto& source : m_sources) {
        if (source && source->capture) source->capture->Stop();
    }
}

void MultiAppAudioCapture::SetStartTime(UINT64 rawQpc) {
    m_masterStartRawQpc = rawQpc;
    for (auto& source : m_sources) {
        if (source && source->capture) source->capture->SetStartTime(rawQpc, 0, m_replayBufferDurationHns);
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

        for (int i = 0; i < kMaxAudioSamplesPerPump; ++i) {
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

            NoteAcceptedAudioInput(input.Get(), source->timeline);
            DrainAudioEncoder(source->encoder.Get(), ringBuffer, source->streamIndex, source->timeline);
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
    std::wostringstream signature;
    std::wostringstream details;
    details << L"Active audio session candidates=" << candidates.size();
    for (const auto& candidate : candidates) {
        signature << candidate.pid << L":" << candidate.label << L";";
        details << L" [" << candidate.label << L" pid=" << candidate.pid << L"]";
    }

    const std::wstring candidateSignature = signature.str();
    if (candidateSignature != m_lastCandidateSignature) {
        fern::LogInfo(L"AUDIO", details.str());
        m_lastCandidateSignature = candidateSignature;
    }

    const LONGLONG nowHns = CurrentQpcHns();
    for (const auto& candidate : candidates) {
        if (candidate.pid == 0) continue;

        if (Source* source = FindSource(candidate.pid)) {
            source->lastSeenHns = nowHns;
            continue;
        }

        AddSource(candidate.pid, candidate.label);
    }

    PruneInactiveSources(nowHns);
    m_lastRefreshHns = nowHns;
}

bool MultiAppAudioCapture::ShouldRefreshSources(LONGLONG nowHns) const {
    return m_lastRefreshHns == 0 || nowHns - m_lastRefreshHns >= kSourceRefreshIntervalHns;
}

bool MultiAppAudioCapture::HasSource(DWORD pid) const {
    return FindSource(pid) != nullptr;
}

MultiAppAudioCapture::Source* MultiAppAudioCapture::FindSource(DWORD pid) const {
    for (const auto& source : m_sources) {
        if (source && source->pid == pid) return source.get();
    }
    return nullptr;
}

void MultiAppAudioCapture::PruneInactiveSources(LONGLONG nowHns) {
    const LONGLONG retainHns = m_replayBufferDurationHns + kInactiveSourceGraceHns;

    m_sources.erase(
        std::remove_if(m_sources.begin(), m_sources.end(), [&](const std::unique_ptr<Source>& source) {
            if (!source || source->pid == 0 || source->lastSeenHns <= 0) return false;
            if (nowHns - source->lastSeenHns <= retainHns) return false;

            std::wostringstream stream;
            stream << L"Removing inactive track " << source->streamIndex
                   << L" label=" << source->label
                   << L" pid=" << source->pid;
            fern::LogInfo(L"AUDIO", stream.str());

            if (source->capture) source->capture->Stop();
            return true;
        }),
        m_sources.end());
}

HRESULT MultiAppAudioCapture::AddSource(DWORD pid, const std::wstring& label) {
    auto capture = std::make_unique<IsolatedAudioCapture>(pid);
    return AddCaptureSource(std::move(capture), pid, label);
}

HRESULT MultiAppAudioCapture::AddMicrophoneSource(const std::wstring& deviceId) {
    auto capture = std::make_unique<IsolatedAudioCapture>(deviceId);
    fern::LogInfo(L"AUDIO", deviceId.empty()
        ? L"Adding default microphone source."
        : L"Adding configured microphone source.");
    return AddCaptureSource(std::move(capture), 0, L"Micro");
}

HRESULT MultiAppAudioCapture::AddCaptureSource(std::unique_ptr<IsolatedAudioCapture> capture, DWORD pid, const std::wstring& label) {
    if (!capture) return E_POINTER;

    HRESULT hr = capture->Start();
    if (FAILED(hr)) {
        std::wcerr << L"AUDIO: capture failed for " << label;
        if (pid != 0) std::wcerr << L" (pid=" << pid << L")";
        std::wcerr << L" 0x" << std::hex << hr << std::dec << std::endl;
        std::wostringstream stream;
        stream << L"Capture failed for " << label;
        if (pid != 0) stream << L" pid=" << pid;
        fern::LogHResult(fern::LogLevel::Warning, L"AUDIO", stream.str(), hr);
        return hr;
    }

    if (m_masterStartRawQpc != 0) {
        const LONGLONG masterStartHns = RawQpcToHns(static_cast<LONGLONG>(m_masterStartRawQpc));
        const LONGLONG timelineOffsetHns = std::max<LONGLONG>(0, CurrentQpcHns() - masterStartHns);
        capture->SetStartTime(m_masterStartRawQpc, timelineOffsetHns, m_replayBufferDurationHns);
    }

    ComPtr<IMFTransform> encoder;
    hr = InitializeAudioEncoder(encoder, capture->GetFormat());
    if (FAILED(hr) || !encoder) {
        std::wcerr << L"AUDIO: encoder init failed for " << label;
        if (pid != 0) std::wcerr << L" (pid=" << pid << L")";
        std::wcerr << L" 0x" << std::hex << hr << std::dec << std::endl;
        std::wostringstream stream;
        stream << L"Encoder init failed for " << label;
        if (pid != 0) stream << L" pid=" << pid;
        fern::LogHResult(fern::LogLevel::Warning, L"AUDIO", stream.str(), hr);
        capture->Stop();
        return FAILED(hr) ? hr : E_FAIL;
    }

    encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    ComPtr<IMFMediaType> outputType;
    hr = encoder->GetOutputCurrentType(0, &outputType);
    if (FAILED(hr) || !outputType) {
        std::wcerr << L"AUDIO: output type unavailable for " << label;
        if (pid != 0) std::wcerr << L" (pid=" << pid << L")";
        std::wcerr << L" 0x" << std::hex << hr << std::dec << std::endl;
        std::wostringstream stream;
        stream << L"Output type unavailable for " << label;
        if (pid != 0) stream << L" pid=" << pid;
        fern::LogHResult(fern::LogLevel::Warning, L"AUDIO", stream.str(), hr);
        capture->Stop();
        return FAILED(hr) ? hr : E_FAIL;
    }

    auto source = std::make_unique<Source>();
    source->pid = pid;
    source->streamIndex = m_nextStreamIndex++;
    source->lastSeenHns = CurrentQpcHns();
    source->label = label;
    source->capture = std::move(capture);
    source->encoder = encoder;
    source->outputType = outputType;

    std::wcout << L"AUDIO: piste " << source->streamIndex << L" -> "
               << source->label;
    if (source->pid != 0) std::wcout << L" (pid=" << source->pid << L")";
    std::wcout << std::endl;

    {
        std::wostringstream stream;
        stream << L"Track " << source->streamIndex << L" added label=" << source->label;
        if (source->pid != 0) stream << L" pid=" << source->pid;
        stream << L" inputFormat={" << WaveFormatText(source->capture ? source->capture->GetFormat() : nullptr) << L"}";
        fern::LogInfo(L"AUDIO", stream.str());
    }

    m_sources.push_back(std::move(source));
    return S_OK;
}

}
