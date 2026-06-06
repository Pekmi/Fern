#pragma once
#include <string>
#include <filesystem>

struct Settings {
    int bufferDuration = 30;
    int fps = 60;
    int bitrate = 15;
    std::wstring storagePath = L"C:\\Videos\\Fern";
    std::wstring hotkey = L"Alt+Shift+F9";
    std::wstring microphoneDeviceId;
    std::wstring microphoneDeviceName;
    std::wstring videoCodec = L"H264";
    std::wstring encoderProfile = L"High";
    std::wstring rateControl = L"VBR";
    int maxBitrateMultiplier = 200;
    int gopSeconds = 2;
    int bFrames = 2;
    bool lowLatency = false;
    int qualityVsSpeed = 70;
    int encoderIndex = 0;

    void Load(bool log = true);
    void Save();
private:
    std::filesystem::path GetSettingsPath();
};
