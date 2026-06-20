#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>

#include "../include/fern/logger.h"
#include "../include/fern/replay_export.h"


namespace fern {
namespace {

constexpr LONGLONG kMinimumTrackActiveHns = 1000000LL;

HRESULT CloneSampleForExport(IMFSample* source, ComPtr<IMFSample>& clone);
bool TryGetSampleTime(const StreamSample& sample, LONGLONG& time);
LONGLONG RelativeHns(LONGLONG value, LONGLONG offset);
void ClampDecodeTimestamp(IMFSample* sample, LONGLONG exportStart, LONGLONG& lastDts);

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return "";

    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return "";

    std::string result(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), size, nullptr, nullptr);
    return result;
}

std::string JsonEscape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);

    for (unsigned char ch : value) {
        switch (ch) {
        case '\\': escaped += "\\\\"; break;
        case '"': escaped += "\\\""; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (ch < 0x20) {
                constexpr char hex[] = "0123456789abcdef";
                escaped += "\\u00";
                escaped += hex[(ch >> 4) & 0xF];
                escaped += hex[ch & 0xF];
            } else {
                escaped += static_cast<char>(ch);
            }
            break;
        }
    }

    return escaped;
}

std::string BuildAudioTrackManifestJson(
    const std::vector<AudioTrackMetadata>& audioTrackMetadata,
    const std::vector<DWORD>& sinkStreamIndices) {
    std::ostringstream json;

    json << "{\n";
    json << "  \"version\": 2,\n";
    json << "  \"package\": \"fern\",\n";
    json << "  \"tracks\": [\n";

    std::vector<int> audioIndexByStream(sinkStreamIndices.size(), -1);
    int nextAudioIndex = 0;
    for (size_t i = 1; i < sinkStreamIndices.size(); ++i) {
        if (sinkStreamIndices[i] == std::numeric_limits<DWORD>::max()) continue;
        audioIndexByStream[i] = nextAudioIndex++;
    }

    bool first = true;
    for (const auto& track : audioTrackMetadata) {
        if (track.streamIndex == 0 || track.streamIndex >= sinkStreamIndices.size()) continue;
        if (sinkStreamIndices[track.streamIndex] == std::numeric_limits<DWORD>::max()) continue;
        const int audioIndex = audioIndexByStream[track.streamIndex];
        if (audioIndex < 0) continue;

        if (!first) json << ",\n";
        first = false;

        json << "    {";
        json << "\"streamIndex\": " << track.streamIndex << ", ";
        json << "\"audioIndex\": " << audioIndex << ", ";
        json << "\"pid\": " << track.pid << ", ";
        json << "\"name\": \"" << JsonEscape(WideToUtf8(track.name)) << "\", ";
        json << "\"activeDurationHns\": " << track.activeDurationHns << ", ";
        json << "\"activeRatio\": " << track.activeRatio;
        json << "}";
    }

    json << "\n";
    json << "  ]\n";
    json << "}\n";
    return json.str();
}

LONGLONG ClippedRangeDurationHns(const AudioActivityRange& range, LONGLONG exportStart, LONGLONG exportEnd) {
    const LONGLONG start = std::max(range.startHns, exportStart);
    const LONGLONG end = std::min(range.startHns + range.durationHns, exportEnd);
    return end > start ? end - start : 0;
}

LONGLONG CalculateActiveDurationHns(const std::vector<AudioActivityRange>& ranges, LONGLONG exportStart, LONGLONG exportEnd) {
    LONGLONG activeDuration = 0;
    for (const auto& range : ranges) {
        activeDuration += ClippedRangeDurationHns(range, exportStart, exportEnd);
    }
    return activeDuration;
}

void PrepareAudioTracksForExport(
    std::vector<ComPtr<IMFMediaType>>& types,
    std::vector<AudioTrackMetadata>& audioTrackMetadata,
    LONGLONG exportStart,
    LONGLONG exportEnd) {
    const LONGLONG exportDuration = std::max<LONGLONG>(1, exportEnd - exportStart);

    for (auto& track : audioTrackMetadata) {
        track.activeDurationHns = CalculateActiveDurationHns(track.activityRanges, exportStart, exportEnd);
        track.activeRatio = static_cast<double>(track.activeDurationHns) / static_cast<double>(exportDuration);

        if (track.streamIndex == 0 || track.streamIndex >= types.size()) continue;
        if (track.activeDurationHns < kMinimumTrackActiveHns) {
            std::wostringstream stream;
            stream << L"Disabling inactive audio track stream=" << track.streamIndex
                   << L" pid=" << track.pid
                   << L" name=" << track.name
                   << L" activeDurationHns=" << track.activeDurationHns
                   << L" activeRatio=" << track.activeRatio;
            fern::LogInfo(L"SAVE", stream.str());
            types[track.streamIndex].Reset();
        } else {
            std::wostringstream stream;
            stream << L"Keeping audio track stream=" << track.streamIndex
                   << L" pid=" << track.pid
                   << L" name=" << track.name
                   << L" activeDurationHns=" << track.activeDurationHns
                   << L" activeRatio=" << track.activeRatio;
            fern::LogInfo(L"SAVE", stream.str());
        }
    }
}

std::wstring BuildAudioBundleFilename(const std::wstring& filename) {
    const std::filesystem::path clipPath(filename);
    const std::wstring stem = clipPath.stem().wstring();
    return (clipPath.parent_path() / (stem + L".fern_audio.tmp.mp4")).wstring();
}

std::wstring BuildFernPackageFilename(const std::wstring& filename) {
    return std::filesystem::path(filename).replace_extension(L".fern").wstring();
}

bool WriteAudioBundle(
    const std::vector<StreamSample>& sortedSamples,
    const std::vector<ComPtr<IMFMediaType>>& types,
    const std::vector<DWORD>& sinkStreamIndices,
    const std::wstring& bundleFilename,
    LONGLONG exportStart,
    LONGLONG exportEnd,
    IMFAttributes* attributes) {
    std::vector<DWORD> bundleStreamIndices(types.size(), std::numeric_limits<DWORD>::max());

    ComPtr<IMFSinkWriter> writer;
    HRESULT hr = MFCreateSinkWriterFromURL(bundleFilename.c_str(), nullptr, attributes, &writer);
    if (FAILED(hr) || !writer) {
        std::wcerr << L"SAVE: audio bundle writer failed " << bundleFilename
                   << L" 0x" << std::hex << hr << std::dec << std::endl;
        fern::LogHResult(fern::LogLevel::Warning, L"SAVE", L"Audio bundle writer failed: " + bundleFilename, hr);
        return false;
    }

    int audioStreamCount = 0;
    for (size_t streamIndex = 1; streamIndex < sinkStreamIndices.size(); ++streamIndex) {
        if (sinkStreamIndices[streamIndex] == std::numeric_limits<DWORD>::max()) continue;
        if (streamIndex >= types.size() || !types[streamIndex]) continue;

        DWORD bundleStreamIndex = 0;
        hr = writer->AddStream(types[streamIndex].Get(), &bundleStreamIndex);
        if (FAILED(hr)) {
            std::wcerr << L"SAVE: audio bundle AddStream failed " << bundleFilename
                       << L" 0x" << std::hex << hr << std::dec << std::endl;
            fern::LogHResult(fern::LogLevel::Warning, L"SAVE", L"Audio bundle AddStream failed: " + bundleFilename, hr);
            return false;
        }

        bundleStreamIndices[streamIndex] = bundleStreamIndex;
        ++audioStreamCount;
    }

    if (audioStreamCount == 0) return false;
    fern::LogInfo(L"SAVE", L"Writing audio bundle streams=" + std::to_wstring(audioStreamCount));

    hr = writer->BeginWriting();
    if (FAILED(hr)) {
        std::wcerr << L"SAVE: audio bundle BeginWriting failed " << bundleFilename
                   << L" 0x" << std::hex << hr << std::dec << std::endl;
        fern::LogHResult(fern::LogLevel::Warning, L"SAVE", L"Audio bundle BeginWriting failed: " + bundleFilename, hr);
        return false;
    }

    std::vector<LONGLONG> lastTimes(types.size(), -1);
    std::vector<LONGLONG> lastDts(types.size(), -1);
    int writtenSamples = 0;

    for (const auto& sample : sortedSamples) {
        if (!sample.sample || sample.streamIndex >= bundleStreamIndices.size()) continue;
        if (bundleStreamIndices[sample.streamIndex] == std::numeric_limits<DWORD>::max()) continue;

        LONGLONG originalTime = 0;
        if (!TryGetSampleTime(sample, originalTime) || originalTime < exportStart || originalTime >= exportEnd) continue;

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
        if (originalTime + duration > exportEnd) duration = std::max<LONGLONG>(1, exportEnd - originalTime);
        output->SetSampleDuration(duration);

        ClampDecodeTimestamp(output.Get(), exportStart, lastDts[sample.streamIndex]);

        hr = writer->WriteSample(bundleStreamIndices[sample.streamIndex], output.Get());
        if (FAILED(hr)) {
            std::wcerr << L"SAVE: audio bundle WriteSample failed " << bundleFilename
                       << L" 0x" << std::hex << hr << std::dec << std::endl;
            fern::LogHResult(fern::LogLevel::Warning, L"SAVE", L"Audio bundle WriteSample failed: " + bundleFilename, hr);
            continue;
        }

        lastTimes[sample.streamIndex] = adjustedTime;
        ++writtenSamples;
    }

    hr = writer->Finalize();
    if (FAILED(hr)) {
        std::wcerr << L"SAVE: audio bundle Finalize failed " << bundleFilename
                   << L" 0x" << std::hex << hr << std::dec << std::endl;
        fern::LogHResult(fern::LogLevel::Warning, L"SAVE", L"Audio bundle Finalize failed: " + bundleFilename, hr);
        return false;
    }

    fern::LogInfo(L"SAVE", L"Audio bundle written samples=" + std::to_wstring(writtenSamples));
    return writtenSamples > 0;
}

bool WriteFernPackage(
    const std::wstring& filename,
    const std::string& manifestJson,
    const std::wstring& audioBundleFilename) {
    const std::filesystem::path packagePath = BuildFernPackageFilename(filename);
    std::ifstream audioFile(audioBundleFilename, std::ios::binary);
    if (!audioFile.is_open()) return false;

    audioFile.seekg(0, std::ios::end);
    const std::streamoff audioSize = audioFile.tellg();
    audioFile.seekg(0, std::ios::beg);
    if (audioSize <= 0) return false;

    std::ofstream packageFile(packagePath, std::ios::binary | std::ios::trunc);
    if (!packageFile.is_open()) {
        std::wcerr << L"SAVE: package open failed " << packagePath.wstring() << std::endl;
        fern::LogError(L"SAVE", L"Package open failed: " + packagePath.wstring());
        return false;
    }

    constexpr char magic[] = "FERNPKG1";
    const uint64_t jsonSize = static_cast<uint64_t>(manifestJson.size());
    const uint64_t audioSize64 = static_cast<uint64_t>(audioSize);

    packageFile.write(magic, 8);
    packageFile.write(reinterpret_cast<const char*>(&jsonSize), sizeof(jsonSize));
    packageFile.write(reinterpret_cast<const char*>(&audioSize64), sizeof(audioSize64));
    packageFile.write(manifestJson.data(), static_cast<std::streamsize>(manifestJson.size()));

    char buffer[64 * 1024];
    while (audioFile.good()) {
        audioFile.read(buffer, sizeof(buffer));
        const std::streamsize read = audioFile.gcount();
        if (read > 0) packageFile.write(buffer, read);
    }

    const bool ok = packageFile.good();
    fern::LogInfo(L"SAVE", (ok ? L"Fern package written: " : L"Fern package write failed: ") + packagePath.wstring());
    return ok;
}


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
    std::vector<AudioTrackMetadata> audioTrackMetadata,
    std::wstring filename,
    LONGLONG exportEndHns) {
    HRESULT coHr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    const bool coInitialized = SUCCEEDED(coHr);

    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        std::cerr << "SAVE: MFStartup failed 0x" << std::hex << hr << std::dec << std::endl;
        fern::LogHResult(fern::LogLevel::Error, L"SAVE", L"MFStartup failed.", hr);
        if (coInitialized) CoUninitialize();
        return;
    }

    fern::LogInfo(L"SAVE", L"Async save worker started: " + filename);

    if (samples.empty() || types.empty() || !types[0]) {
        std::cerr << "SAVE: no samples or media type." << std::endl;
        fern::LogError(L"SAVE", L"No samples or media type.");
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
        for (const auto& sample : sortedSamples) {
            if (sample.streamIndex != 0) continue;

            LONGLONG time = 0;
            if (TryGetSampleTime(sample, time) && time < exportEndHns) {
                exportStart = std::max<LONGLONG>(0, time);
                fern::LogWarning(L"SAVE", L"No video keyframe marker found; exporting from first video sample.");
                break;
            }
        }

        if (exportStart < 0) {
            std::cerr << "SAVE: no video sample found, export skipped." << std::endl;
            fern::LogWarning(L"SAVE", L"No video sample found; export skipped.");
            MFShutdown();
            if (coInitialized) CoUninitialize();
            return;
        }
    }

    PrepareAudioTracksForExport(types, audioTrackMetadata, exportStart, exportEndHns);

    ComPtr<IMFAttributes> attributes;
    MFCreateAttributes(&attributes, 1);
    if (attributes) attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);

    ComPtr<IMFSinkWriter> sinkWriter;
    hr = MFCreateSinkWriterFromURL(filename.c_str(), nullptr, attributes.Get(), &sinkWriter);
    if (FAILED(hr)) {
        std::cerr << "SAVE: MFCreateSinkWriterFromURL failed 0x" << std::hex << hr << std::dec << std::endl;
        fern::LogHResult(fern::LogLevel::Error, L"SAVE", L"MFCreateSinkWriterFromURL failed.", hr);
        MFShutdown();
        if (coInitialized) CoUninitialize();
        return;
    }

    std::vector<DWORD> sinkStreamIndices(types.size(), std::numeric_limits<DWORD>::max());

    hr = sinkWriter->AddStream(types[0].Get(), &sinkStreamIndices[0]);
    if (FAILED(hr)) {
        std::cerr << "SAVE: AddStream video failed 0x" << std::hex << hr << std::dec << std::endl;
        fern::LogHResult(fern::LogLevel::Error, L"SAVE", L"AddStream video failed.", hr);
        MFShutdown();
        if (coInitialized) CoUninitialize();
        return;
    }

    for (size_t i = 1; i < types.size(); ++i) {
        if (!types[i]) continue;

        hr = sinkWriter->AddStream(types[i].Get(), &sinkStreamIndices[i]);
        if (FAILED(hr)) {
            std::cerr << "SAVE: AddStream audio " << i << " failed 0x" << std::hex << hr << std::dec << std::endl;
            fern::LogHResult(fern::LogLevel::Error, L"SAVE", L"AddStream audio " + std::to_wstring(i) + L" failed.", hr);
            MFShutdown();
            if (coInitialized) CoUninitialize();
            return;
        }
    }

    hr = sinkWriter->BeginWriting();
    if (FAILED(hr)) {
        std::cerr << "SAVE: BeginWriting failed 0x" << std::hex << hr << std::dec << std::endl;
        fern::LogHResult(fern::LogLevel::Error, L"SAVE", L"BeginWriting failed.", hr);
        MFShutdown();
        if (coInitialized) CoUninitialize();
        return;
    }

    std::vector<LONGLONG> lastTimes(types.size(), -1);
    std::vector<LONGLONG> lastDts(types.size(), -1);
    int writtenVideo = 0;
    std::vector<int> writtenAudio(types.size(), 0);
    bool firstVideoSample = true;

    for (const auto& sample : sortedSamples) {
        if (!sample.sample || sample.streamIndex >= types.size()) continue;
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
            fern::LogHResult(fern::LogLevel::Warning, L"SAVE", L"WriteSample stream " + std::to_wstring(sample.streamIndex) + L" failed.", hr);
            continue;
        }

        lastTimes[sample.streamIndex] = adjustedTime;
        if (sample.streamIndex == 0) ++writtenVideo;
        if (sample.streamIndex > 0) ++writtenAudio[sample.streamIndex];
    }

    hr = sinkWriter->Finalize();
    if (FAILED(hr)) {
        std::cerr << "SAVE: Finalize failed 0x" << std::hex << hr << std::dec << std::endl;
        fern::LogHResult(fern::LogLevel::Error, L"SAVE", L"Finalize failed.", hr);
    } else {
        fern::LogInfo(L"SAVE", L"MP4 finalized: " + filename);
        const std::wstring audioBundleFilename = BuildAudioBundleFilename(filename);
        if (WriteAudioBundle(sortedSamples, types, sinkStreamIndices, audioBundleFilename, exportStart, exportEndHns, attributes.Get())) {
            const std::string manifestJson = BuildAudioTrackManifestJson(audioTrackMetadata, sinkStreamIndices);
            WriteFernPackage(filename, manifestJson, audioBundleFilename);
            DeleteFileW(audioBundleFilename.c_str());
        }
    }

    int writtenAudioTotal = 0;
    int audioStreamCount = 0;
    for (size_t i = 1; i < writtenAudio.size(); ++i) {
        if (sinkStreamIndices[i] == std::numeric_limits<DWORD>::max()) continue;
        ++audioStreamCount;
        writtenAudioTotal += writtenAudio[i];
    }

    std::wcout << L">>> Exportation terminee : " << filename
               << L" (video=" << writtenVideo
               << L", audioStreams=" << audioStreamCount
               << L", audioSamples=" << writtenAudioTotal << L")" << std::endl;

    {
        std::wostringstream stream;
        stream << L"Export complete file=" << filename
               << L" videoSamples=" << writtenVideo
               << L" audioStreams=" << audioStreamCount
               << L" audioSamples=" << writtenAudioTotal;
        for (size_t i = 1; i < writtenAudio.size(); ++i) {
            if (sinkStreamIndices[i] == std::numeric_limits<DWORD>::max()) continue;
            stream << L" stream" << i << L"=" << writtenAudio[i];
        }
        fern::LogInfo(L"SAVE", stream.str());
    }

    MFShutdown();
    if (coInitialized) CoUninitialize();
}

}
