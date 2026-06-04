#pragma once
#include <string>
#include <filesystem>

struct Settings {
    int bufferDuration = 30;
    int fps = 60;
    int bitrate = 15;
    std::wstring storagePath = L"C:\\Videos\\Fern";

    void Load();
    void Save();
private:
    std::filesystem::path GetSettingsPath();
};
