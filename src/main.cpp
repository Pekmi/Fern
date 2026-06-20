#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <timeapi.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <thread>
#include <cwctype>
#include <shellapi.h>

#include "../include/fern/capture_session.h"
#include "../include/fern/encoder.h"
#include "../include/fern/hotkey.h"
#include "../include/fern/ipc_server.h"
#include "../include/fern/logger.h"
#include "../include/fern/settings.h"


namespace {

std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(towlower(c));
    });
    return value;
}

std::wstring CompactLower(std::wstring value) {
    value = ToLower(value);
    value.erase(std::remove_if(value.begin(), value.end(), [](wchar_t c) {
        return c == L'-' || c == L'_' || c == L'.' || iswspace(c);
    }), value.end());
    return value;
}

bool IsRedirectedStdHandle(DWORD handleId) {
    HANDLE handle = GetStdHandle(handleId);
    if (!handle || handle == INVALID_HANDLE_VALUE) return false;

    const DWORD type = GetFileType(handle);
    return type == FILE_TYPE_PIPE || type == FILE_TYPE_DISK;
}

bool ShouldForceConsoleOutput() {
    wchar_t value[16] = {};
    if (GetEnvironmentVariableW(L"FERN_DAEMON_CONSOLE", value, static_cast<DWORD>(std::size(value))) == 0) {
        return false;
    }

    const std::wstring normalized = ToLower(value);
    return normalized == L"1" || normalized == L"true" || normalized == L"yes" || normalized == L"on";
}

void InitializeConsoleOutput() {
    if (IsRedirectedStdHandle(STD_OUTPUT_HANDLE) || IsRedirectedStdHandle(STD_ERROR_HANDLE)) {
        return;
    }

    bool hasConsole = AttachConsole(ATTACH_PARENT_PROCESS) != FALSE;
    if (!hasConsole && ShouldForceConsoleOutput()) {
        hasConsole = AllocConsole() != FALSE;
    }

    if (!hasConsole) {
        return;
    }

    FILE* fpOUT = nullptr;
    FILE* fpERR = nullptr;
    FILE* fpIN = nullptr;
    freopen_s(&fpOUT, "CONOUT$", "w", stdout);
    freopen_s(&fpERR, "CONOUT$", "w", stderr);
    freopen_s(&fpIN, "CONIN$", "r", stdin);
}

std::wstring NormalizeArgumentName(std::wstring value) {
    while (!value.empty() && value.front() == L'-') {
        value.erase(value.begin());
    }
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
        if (c == L'_') return L'-';
        return static_cast<wchar_t>(towlower(c));
    });
    return value;
}

bool IsOptionToken(const wchar_t* value) {
    return value && value[0] == L'-' && value[1] == L'-';
}

bool TryParseInt(const std::wstring& value, int& result) {
    wchar_t* end = nullptr;
    errno = 0;
    const long parsed = wcstol(value.c_str(), &end, 10);
    if (value.empty() || end == value.c_str() || *end != L'\0' || errno == ERANGE ||
        parsed < INT_MIN || parsed > INT_MAX) {
        return false;
    }

    result = static_cast<int>(parsed);
    return true;
}

bool TryParseBool(const std::wstring& value, bool& result) {
    const std::wstring lowered = ToLower(value);
    if (lowered == L"1" || lowered == L"true" || lowered == L"yes" || lowered == L"on" || lowered == L"oui") {
        result = true;
        return true;
    }
    if (lowered == L"0" || lowered == L"false" || lowered == L"no" || lowered == L"off" || lowered == L"non") {
        result = false;
        return true;
    }
    return false;
}

void ApplyIntArgument(const std::wstring& name, const std::wstring& value, int& target) {
    int parsed = 0;
    if (TryParseInt(value, parsed)) {
        target = parsed;
    } else {
        std::wcerr << L"ARGUMENT: valeur invalide pour --" << name << L": " << value << std::endl;
    }
}

void ApplyBoolArgument(const std::wstring& name, const std::wstring& value, bool& target) {
    bool parsed = false;
    if (TryParseBool(value, parsed)) {
        target = parsed;
    } else {
        std::wcerr << L"ARGUMENT: valeur invalide pour --" << name << L": " << value << std::endl;
    }
}

void ApplyArgument(Settings& settings, const std::wstring& name, const std::wstring& value) {
    if (name == L"buffer" || name == L"buffer-duration" || name == L"bufferduration") {
        ApplyIntArgument(name, value, settings.bufferDuration);
    } else if (name == L"fps") {
        ApplyIntArgument(name, value, settings.fps);
    } else if (name == L"bitrate" || name == L"bitrate-mbps" || name == L"bitratembps") {
        ApplyIntArgument(name, value, settings.bitrate);
    } else if (name == L"storage-path" || name == L"storagepath" || name == L"output-path" || name == L"output") {
        settings.storagePath = value;
    } else if (name == L"hotkey") {
        settings.hotkey = value;
    } else if (name == L"microphone-device-id" || name == L"microphone-deviceid" ||
               name == L"microphone-id" || name == L"microphone") {
        settings.microphoneDeviceId = value;
    } else if (name == L"microphone-device-name" || name == L"microphone-name") {
        settings.microphoneDeviceName = value;
    } else if (name == L"video-codec" || name == L"videocodec" || name == L"codec") {
        settings.videoCodec = value;
    } else if (name == L"encoder-profile" || name == L"encoderprofile" || name == L"profile") {
        settings.encoderProfile = value;
    } else if (name == L"rate-control" || name == L"ratecontrol") {
        settings.rateControl = value;
    } else if (name == L"max-bitrate-multiplier" || name == L"maxbitratemultiplier" ||
               name == L"peak-bitrate-multiplier") {
        ApplyIntArgument(name, value, settings.maxBitrateMultiplier);
    } else if (name == L"gop" || name == L"gop-seconds" || name == L"gopseconds") {
        ApplyIntArgument(name, value, settings.gopSeconds);
    } else if (name == L"b-frames" || name == L"bframes") {
        ApplyIntArgument(name, value, settings.bFrames);
    } else if (name == L"low-latency" || name == L"lowlatency") {
        ApplyBoolArgument(name, value, settings.lowLatency);
    } else if (name == L"quality-vs-speed" || name == L"qualityvsspeed" || name == L"quality-speed") {
        ApplyIntArgument(name, value, settings.qualityVsSpeed);
    } else if (name == L"encoder-index" || name == L"encoderindex") {
        ApplyIntArgument(name, value, settings.encoderIndex);
    } else {
        std::wcerr << L"ARGUMENT: option ignoree --" << name << std::endl;
    }
}

void ApplyCommandLineOverrides(Settings& settings) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return;

    for (int i = 1; i < argc; ++i) {
        std::wstring raw = argv[i];
        if (!IsOptionToken(raw.c_str())) continue;

        std::wstring name;
        std::wstring value;
        bool hasValue = false;

        const size_t separator = raw.find(L'=');
        if (separator != std::wstring::npos) {
            name = raw.substr(0, separator);
            value = raw.substr(separator + 1);
            hasValue = true;
        } else {
            name = raw;
        }

        name = NormalizeArgumentName(name);
        if (name == L"h264") {
            settings.videoCodec = L"H264";
            continue;
        }
        if (name == L"h265" || name == L"hevc") {
            settings.videoCodec = L"HEVC";
            continue;
        }
        if (name == L"no-low-latency" || name == L"nolowlatency") {
            settings.lowLatency = false;
            continue;
        }
        if ((name == L"low-latency" || name == L"lowlatency") && !hasValue) {
            if (i + 1 < argc && !IsOptionToken(argv[i + 1])) {
                value = argv[++i];
                hasValue = true;
            } else {
                settings.lowLatency = true;
                continue;
            }
        }

        if (!hasValue) {
            if (i + 1 >= argc || IsOptionToken(argv[i + 1])) {
                std::wcerr << L"ARGUMENT: valeur manquante pour --" << name << std::endl;
                continue;
            }
            value = argv[++i];
            hasValue = true;
        }

        ApplyArgument(settings, name, value);
    }

    LocalFree(argv);
}

void NormalizeSettings(Settings& settings) {
    settings.fps = std::clamp(settings.fps, 1, 240);
    settings.bufferDuration = std::max(1, settings.bufferDuration);
    settings.bitrate = std::max(1, settings.bitrate);
    const std::wstring codec = CompactLower(settings.videoCodec);
    if (codec == L"hevc" || codec == L"h265") settings.videoCodec = L"HEVC";
    else settings.videoCodec = L"H264";
    settings.encoderProfile = CompactLower(settings.encoderProfile) == L"main" ? L"Main" : L"High";
    const std::wstring rateControl = CompactLower(settings.rateControl);
    if (rateControl == L"cbr") settings.rateControl = L"CBR";
    else if (rateControl == L"lowdelayvbr") settings.rateControl = L"LowDelayVBR";
    else settings.rateControl = L"VBR";
    settings.maxBitrateMultiplier = std::clamp(settings.maxBitrateMultiplier, 100, 400);
    settings.gopSeconds = std::clamp(settings.gopSeconds, 1, 10);
    settings.bFrames = std::clamp(settings.bFrames, 0, 4);
    settings.qualityVsSpeed = std::clamp(settings.qualityVsSpeed, 0, 100);
    settings.encoderIndex = std::max(0, settings.encoderIndex);
}

Settings LoadSettings() {
    Settings settings;
    settings.Load();
    ApplyCommandLineOverrides(settings);
    NormalizeSettings(settings);

    std::error_code ec;
    std::filesystem::create_directories(settings.storagePath, ec);
    if (ec) {
        std::wcerr << L"SETTINGS: impossible de creer " << settings.storagePath
                   << L" (" << ec.message().c_str() << L")" << std::endl;
    }

    return settings;
}

}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const bool timerPeriodSet = (timeBeginPeriod(1) == TIMERR_NOERROR);

    InitializeConsoleOutput();
    fern::InitializeLogging();
    fern::LogInfo(L"APP", L"Fern engine starting.");
    fern::LogSystemInfo();

    Settings settings = LoadSettings();
    {
        std::wostringstream stream;
        stream << L"settings fps=" << settings.fps
               << L" bufferDuration=" << settings.bufferDuration
               << L" bitrateMbps=" << settings.bitrate
               << L" codec=" << settings.videoCodec
               << L" profile=" << settings.encoderProfile
               << L" rateControl=" << settings.rateControl
               << L" lowLatency=" << (settings.lowLatency ? L"true" : L"false")
               << L" encoderIndex=" << settings.encoderIndex
               << L" microphoneSelected=" << (!settings.microphoneDeviceId.empty() ? L"true" : L"false");
        fern::LogInfo(L"APP", stream.str());
    }

    extern std::atomic<bool> running;
    extern std::atomic<bool> triggerSave;
    std::thread ipcThread(RunIpcServer, std::ref(running), std::ref(triggerSave));
    ipcThread.detach();
    std::thread hotkeyThread(RunHotkeyListener, std::ref(running), std::ref(triggerSave));
    hotkeyThread.detach();

    const HRESULT mfHr = InitializeMediaFoundation();
    if (SUCCEEDED(mfHr)) {
        fern::LogInfo(L"MF", L"Media Foundation initialized.");
        fern::RunCaptureSession(settings);
        ShutdownMediaFoundation();
    } else {
        std::cerr << "MF: startup failed 0x" << std::hex << mfHr << std::dec << std::endl;
        fern::LogHResult(fern::LogLevel::Error, L"MF", L"Media Foundation startup failed.", mfHr);
    }

    fern::LogInfo(L"APP", L"Fern engine stopped.");
    fern::ShutdownLogging();
    if (timerPeriodSet) timeEndPeriod(1);
    return 0;
}
