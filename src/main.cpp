#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <thread>

#include "../include/fern/capture_session.h"
#include "../include/fern/encoder.h"
#include "../include/fern/hotkey.h"
#include "../include/fern/ipc_server.h"
#include "../include/fern/settings.h"

namespace {

bool IsRedirectedStdHandle(DWORD handleId) {
    HANDLE handle = GetStdHandle(handleId);
    if (!handle || handle == INVALID_HANDLE_VALUE) return false;

    const DWORD type = GetFileType(handle);
    return type == FILE_TYPE_PIPE || type == FILE_TYPE_DISK;
}

void InitializeConsoleOutput() {
    if (IsRedirectedStdHandle(STD_OUTPUT_HANDLE) || IsRedirectedStdHandle(STD_ERROR_HANDLE)) {
        return;
    }

    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        AllocConsole();
    }

    FILE* fpOUT = nullptr;
    FILE* fpERR = nullptr;
    FILE* fpIN = nullptr;
    freopen_s(&fpOUT, "CONOUT$", "w", stdout);
    freopen_s(&fpERR, "CONOUT$", "w", stderr);
    freopen_s(&fpIN, "CONIN$", "r", stdin);
}

Settings LoadSettings() {
    Settings settings;
    settings.Load();
    settings.fps = std::clamp(settings.fps, 1, 240);
    settings.bufferDuration = std::max(1, settings.bufferDuration);
    settings.bitrate = std::max(1, settings.bitrate);

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
    Settings settings = LoadSettings();

    extern std::atomic<bool> running;
    extern std::atomic<bool> triggerSave;
    std::thread ipcThread(RunIpcServer, std::ref(running), std::ref(triggerSave));
    ipcThread.detach();
    std::thread hotkeyThread(RunHotkeyListener, std::ref(running), std::ref(triggerSave));
    hotkeyThread.detach();

    const HRESULT mfHr = InitializeMediaFoundation();
    if (SUCCEEDED(mfHr)) {
        fern::RunCaptureSession(settings);
        ShutdownMediaFoundation();
    } else {
        std::cerr << "MF: startup failed 0x" << std::hex << mfHr << std::dec << std::endl;
    }

    if (timerPeriodSet) timeEndPeriod(1);
    return 0;
}
