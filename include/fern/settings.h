#pragma once
#include <string>
#include <filesystem>

struct Settings {
    int bufferDuration = 30;
    int fps = 60;
    int bitrate = 15;
    std::wstring storagePath = L"C:\\Videos\\Fern";
    std::wstring hotkey = L"Alt+Shift+F9";

    void Load(bool log = true);
    void Save();
private:
    std::filesystem::path GetSettingsPath();
};
