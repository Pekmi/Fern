#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <windows.h>
#include <algorithm>
#include <iostream>
#include <limits>

#include "../include/fern/replay_export.h"


namespace fern {
namespace {


HRESULT CloneSampleForExport(IMFSample* source, ComPtr<IMFSample>& clone) {
    if (!source) return E_POINTER;

    HRESULT hr = MFCreateSample(&clone);
    if (FAILED(hr)) return hr;

    source->CopyAllItems(clone.Get());

    DWORD bufferCount = 0;
    hr = source->GetBufferCount(&bufferCount);
    if (FAILED(hr)) return hr;

    for (DWORD i = 0; i < bufferCount; ++i) {
        ComPtr<IMFMediaBuffer> buffer;
        hr = source->GetBufferByIndex(i, &buffer);
        if (FAILED(hr)) return hr;
        clone->AddBuffer(buffer.Get());
    }

    return S_OK;
}

bool TryGetSampleTime(const StreamSample& sample, LONGLONG& time) {
    if (!sample.sample) return false;
    return SUCCEEDED(sample.sample->GetSampleTime(&time));
}

bool IsVideoKeyframe(const StreamSample& sample) {
    if (!sample.sample || sample.streamIndex != 0) return false;

    UINT32 isCleanPoint = 0;
    return SUCCEEDED(sample.sample->GetUINT32(MFSampleExtension_CleanPoint, &isCleanPoint)) && isCleanPoint;
}

LONGLONG RelativeHns(LONGLONG value, LONGLONG offset) {
    if (value <= offset) return 0;
    return value - offset;
}

LONGLONG RelativeUnsignedHns(UINT64 value, LONGLONG offset) {
    if (offset <= 0) {
        return value > static_cast<UINT64>(std::numeric_limits<LONGLONG>::max())
            ? std::numeric_limits<LONGLONG>::max()
            : static_cast<LONGLONG>(value);
    }

    const UINT64 unsignedOffset = static_cast<UINT64>(offset);
    if (value <= unsignedOffset) return 0;

    const UINT64 result = value - unsignedOffset;
    return result > static_cast<UINT64>(std::numeric_limits<LONGLONG>::max())
        ? std::numeric_limits<LONGLONG>::max()
        : static_cast<LONGLONG>(result);
}

void ClampDecodeTimestamp(IMFSample* sample, LONGLONG exportStart, LONGLONG& lastDts) {
    UINT64 dts = 0;
    if (!sample || FAILED(sample->GetUINT64(MFSampleExtension_DecodeTimestamp, &dts))) return;

    LONGLONG adjusted = RelativeUnsignedHns(dts, exportStart);
    if (lastDts >= 0 && adjusted <= lastDts) adjusted = lastDts + 1;

    sample->SetUINT64(MFSampleExtension_DecodeTimestamp, static_cast<UINT64>(adjusted));
    lastDts = adjusted;
}

}

void AsyncSaveWorker(
    std::deque<StreamSample> samples,
    std::vector<ComPtr<IMFMediaType>> types,
    std::wstring filename,
    LONGLONG exportEndHns) {
    HRESULT coHr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    const bool coInitialized = SUCCEEDED(coHr);

    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        std::cerr << "SAVE: MFStartup failed 0x" << std::hex << hr << std::dec << std::endl;
        if (coInitialized) CoUninitialize();
        return;
    }

    if (samples.empty() || types.empty() || !types[0]) {
        std::cerr << "SAVE: no samples or media type." << std::endl;
        MFShutdown();
        if (coInitialized) CoUninitialize();
        return;
    }

    std::vector<StreamSample> sortedSamples(samples.begin(), samples.end());
    std::sort(sortedSamples.begin(), sortedSamples.end(), [](const StreamSample& a, const StreamSample& b) {
        LONGLONG tA = 0;
        LONGLONG tB = 0;
        TryGetSampleTime(a, tA);
        TryGetSampleTime(b, tB);
        if (tA == tB) return a.streamIndex < b.streamIndex;
        return tA < tB;
    });

    LONGLONG exportStart = -1;
    for (const auto& sample : sortedSamples) {
        if (!IsVideoKeyframe(sample)) continue;

        LONGLONG time = 0;
        if (TryGetSampleTime(sample, time) && time < exportEndHns) {
            exportStart = std::max<LONGLONG>(0, time);
            break;
        }
    }

    if (exportStart < 0) {
        std::cerr << "SAVE: no video keyframe found, export skipped." << std::endl;
        MFShutdown();
        if (coInitialized) CoUninitialize();
        return;
    }

    const bool hasAudioType = types.size() > 1 && types[1];

    ComPtr<IMFAttributes> attributes;
    MFCreateAttributes(&attributes, 1);
    if (attributes) attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);

    ComPtr<IMFSinkWriter> sinkWriter;
    hr = MFCreateSinkWriterFromURL(filename.c_str(), nullptr, attributes.Get(), &sinkWriter);
    if (FAILED(hr)) {
        std::cerr << "SAVE: MFCreateSinkWriterFromURL failed 0x" << std::hex << hr << std::dec << std::endl;
        MFShutdown();
        if (coInitialized) CoUninitialize();
        return;
    }

    std::vector<DWORD> sinkStreamIndices(types.size(), std::numeric_limits<DWORD>::max());

    hr = sinkWriter->AddStream(types[0].Get(), &sinkStreamIndices[0]);
    if (FAILED(hr)) {
        std::cerr << "SAVE: AddStream video failed 0x" << std::hex << hr << std::dec << std::endl;
        MFShutdown();
        if (coInitialized) CoUninitialize();
        return;
    }

    if (hasAudioType) {
        hr = sinkWriter->AddStream(types[1].Get(), &sinkStreamIndices[1]);
        if (FAILED(hr)) {
            std::cerr << "SAVE: AddStream audio failed 0x" << std::hex << hr << std::dec << std::endl;
            MFShutdown();
            if (coInitialized) CoUninitialize();
            return;
        }
    }

    hr = sinkWriter->BeginWriting();
    if (FAILED(hr)) {
        std::cerr << "SAVE: BeginWriting failed 0x" << std::hex << hr << std::dec << std::endl;
        MFShutdown();
        if (coInitialized) CoUninitialize();
        return;
    }

    std::vector<LONGLONG> lastTimes(types.size(), -1);
    std::vector<LONGLONG> lastDts(types.size(), -1);
    int writtenVideo = 0;
    int writtenAudio = 0;
    bool firstVideoSample = true;

    for (const auto& sample : sortedSamples) {
        if (!sample.sample || sample.streamIndex >= types.size()) continue;
        if (sample.streamIndex == 1 && !hasAudioType) continue;
        if (sinkStreamIndices[sample.streamIndex] == std::numeric_limits<DWORD>::max()) continue;

        LONGLONG originalTime = 0;
        if (!TryGetSampleTime(sample, originalTime) || originalTime < exportStart || originalTime >= exportEndHns) continue;

        ComPtr<IMFSample> output;
        hr = CloneSampleForExport(sample.sample.Get(), output);
        if (FAILED(hr) || !output) continue;

        LONGLONG adjustedTime = RelativeHns(originalTime, exportStart);
        if (lastTimes[sample.streamIndex] >= 0 && adjustedTime <= lastTimes[sample.streamIndex]) {
            adjustedTime = lastTimes[sample.streamIndex] + 1;
        }

        output->SetSampleTime(adjustedTime);

        LONGLONG duration = 0;
        if (FAILED(sample.sample->GetSampleDuration(&duration)) || duration <= 0) duration = 1;
        if (originalTime + duration > exportEndHns) duration = std::max<LONGLONG>(1, exportEndHns - originalTime);
        output->SetSampleDuration(duration);

        ClampDecodeTimestamp(output.Get(), exportStart, lastDts[sample.streamIndex]);

        if (sample.streamIndex == 0 && firstVideoSample) {
            output->SetUINT32(MFSampleExtension_Discontinuity, TRUE);
            firstVideoSample = false;
        }

        hr = sinkWriter->WriteSample(sinkStreamIndices[sample.streamIndex], output.Get());
        if (FAILED(hr)) {
            std::cerr << "SAVE: WriteSample stream " << sample.streamIndex
                      << " failed 0x" << std::hex << hr << std::dec << std::endl;
            continue;
        }

        lastTimes[sample.streamIndex] = adjustedTime;
        if (sample.streamIndex == 0) ++writtenVideo;
        if (sample.streamIndex == 1) ++writtenAudio;
    }

    hr = sinkWriter->Finalize();
    if (FAILED(hr)) {
        std::cerr << "SAVE: Finalize failed 0x" << std::hex << hr << std::dec << std::endl;
    }

    std::wcout << L">>> Exportation terminee : " << filename
               << L" (video=" << writtenVideo << L", audio=" << writtenAudio << L")" << std::endl;

    MFShutdown();
    if (coInitialized) CoUninitialize();
}

}
