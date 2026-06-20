#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <fstream>
#include <shlobj.h>
#include <windows.h>
#include <iostream>
#include <algorithm>
#include <cwctype>

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

std::filesystem::path EnsureFernLeaf(std::filesystem::path path) {
    if (path.empty()) return path;

    if (ToLower(path.filename().wstring()) != L"fern") {
        path /= L"Fern";
    }
    return path;
}

bool ParseBool(const std::wstring& value) {
    const std::wstring lower = ToLower(value);
    return lower == L"1" || lower == L"true" || lower == L"yes" || lower == L"on";
}

std::wstring BoolText(bool value) {
    return value ? L"true" : L"false";
}
}

std::filesystem::path GetKnownFolder(REFKNOWNFOLDERID rfid) {
    wchar_t* path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(rfid, 0, NULL, &path))) {
        std::filesystem::path fsPath(path);
        CoTaskMemFree(path);
        return fsPath;
    }
    return L"";
}

std::filesystem::path Settings::GetSettingsPath() {
    auto path = GetKnownFolder(FOLDERID_RoamingAppData);
    if (!path.empty()) {
        path /= L"PekmisIndustries";
        path /= L"Fern";
        std::error_code ec;
        if (!std::filesystem::exists(path)) {
            std::filesystem::create_directories(path, ec);
        }
        return path / L"settings.txt";
    }
    return L"settings.txt";
}

void Settings::Load(bool log) {
    // 1. Définir le chemin par défaut vers Vidéos/Fern dynamiquement
    auto videoPath = GetKnownFolder(FOLDERID_Videos);
    if (!videoPath.empty()) {
        storagePath = EnsureFernLeaf(videoPath).wstring();
    } else {
        storagePath = L"C:\\Videos\\Fern"; 
    }
    hotkey = L"Alt+Shift+F9";
    microphoneDeviceId.clear();
    microphoneDeviceName.clear();
    videoCodec = L"H264";
    encoderProfile = L"High";
    rateControl = L"VBR";
    maxBitrateMultiplier = 200;
    gopSeconds = 2;
    bFrames = 2;
    lowLatency = false;
    qualityVsSpeed = 70;
    encoderIndex = 0;
    targetScreenName.clear();

    // 2. Tenter de charger le fichier
    auto path = GetSettingsPath();
    if (log) {
        std::wcout << L"SETTINGS: Tentative de chargement depuis " << path.wstring() << std::endl;
    }

    if (!std::filesystem::exists(path)) {
        if (log) {
            std::wcout << L"SETTINGS: Fichier absent, creation avec valeurs par defaut." << std::endl;
        }
        Save();
        return;
    }

    std::wifstream file(path);
    if (!file.is_open()) {
        std::wcerr << L"SETTINGS ERROR: Impossible d'ouvrir " << path.wstring() << std::endl;
        return;
    }

    std::wstring line;
    bool sawHotkey = false;
    bool sawMicrophoneDeviceId = false;
    bool sawMicrophoneDeviceName = false;
    bool sawVideoCodec = false;
    bool sawEncoderProfile = false;
    bool sawRateControl = false;
    bool sawMaxBitrateMultiplier = false;
    bool sawGopSeconds = false;
    bool sawBFrames = false;
    bool sawLowLatency = false;
    bool sawQualityVsSpeed = false;
    bool sawEncoderIndex = false;
    bool sawTargetScreenName = false;
    std::wstring loadedStoragePath = storagePath;
    while (std::getline(file, line)) {
        size_t pos = line.find(L'=');
        if (pos != std::wstring::npos) {
            std::wstring key = line.substr(0, pos);
            std::wstring value = line.substr(pos + 1);
            
            if (!value.empty() && value.back() == L'\r') value.pop_back();

            try {
                if (key == L"BufferDuration") bufferDuration = std::stoi(value);
                else if (key == L"FPS") fps = std::stoi(value);
                else if (key == L"Bitrate") bitrate = std::stoi(value);
                else if (key == L"StoragePath") {
                    storagePath = value;
                    loadedStoragePath = value;
                }
                else if (key == L"Hotkey") {
                    hotkey = value;
                    sawHotkey = true;
                }
                else if (key == L"MicrophoneDeviceId") {
                    microphoneDeviceId = value;
                    sawMicrophoneDeviceId = true;
                }
                else if (key == L"MicrophoneDeviceName") {
                    microphoneDeviceName = value;
                    sawMicrophoneDeviceName = true;
                }
                else if (key == L"VideoCodec") {
                    videoCodec = value;
                    sawVideoCodec = true;
                }
                else if (key == L"EncoderProfile") {
                    encoderProfile = value;
                    sawEncoderProfile = true;
                }
                else if (key == L"RateControl") {
                    rateControl = value;
                    sawRateControl = true;
                }
                else if (key == L"MaxBitrateMultiplier") {
                    maxBitrateMultiplier = std::stoi(value);
                    sawMaxBitrateMultiplier = true;
                }
                else if (key == L"GopSeconds") {
                    gopSeconds = std::stoi(value);
                    sawGopSeconds = true;
                }
                else if (key == L"BFrames") {
                    bFrames = std::stoi(value);
                    sawBFrames = true;
                }
                else if (key == L"LowLatency") {
                    lowLatency = ParseBool(value);
                    sawLowLatency = true;
                }
                else if (key == L"QualityVsSpeed") {
                    qualityVsSpeed = std::stoi(value);
                    sawQualityVsSpeed = true;
                }
                else if (key == L"EncoderIndex") {
                    encoderIndex = std::stoi(value);
                    sawEncoderIndex = true;
                }
                else if (key == L"TargetScreenName") {
                    targetScreenName = value;
                    sawTargetScreenName = true;
                }
            } catch (...) {
                std::wcerr << L"SETTINGS: Erreur de parsing pour " << key << std::endl;
            }
        }
    }

    storagePath = EnsureFernLeaf(storagePath).wstring();
    if (hotkey.empty()) hotkey = L"Alt+Shift+F9";
    const std::wstring codec = CompactLower(videoCodec);
    videoCodec = (codec == L"hevc" || codec == L"h265") ? L"HEVC" : L"H264";
    encoderProfile = CompactLower(encoderProfile) == L"main" ? L"Main" : L"High";
    const std::wstring rate = CompactLower(rateControl);
    if (rate == L"cbr") rateControl = L"CBR";
    else if (rate == L"lowdelayvbr") rateControl = L"LowDelayVBR";
    else rateControl = L"VBR";
    maxBitrateMultiplier = std::clamp(maxBitrateMultiplier, 100, 400);
    gopSeconds = std::clamp(gopSeconds, 1, 10);
    bFrames = std::clamp(bFrames, 0, 4);
    qualityVsSpeed = std::clamp(qualityVsSpeed, 0, 100);
    encoderIndex = std::max(0, encoderIndex);

    if (!sawHotkey || !sawMicrophoneDeviceId || !sawMicrophoneDeviceName ||
        !sawVideoCodec || !sawEncoderProfile || !sawRateControl || !sawMaxBitrateMultiplier ||
        !sawGopSeconds || !sawBFrames || !sawLowLatency || !sawQualityVsSpeed || !sawEncoderIndex ||
        !sawTargetScreenName || ToLower(loadedStoragePath) != ToLower(storagePath)) {
        Save();
    }

    if (log) {
        std::wcout << L"SETTINGS: Charger. StoragePath=" << storagePath
                   << L" Hotkey=" << hotkey
                   << L" MicrophoneDeviceName=" << microphoneDeviceName
                   << L" VideoCodec=" << videoCodec
                   << L" RateControl=" << rateControl << std::endl;
    }
}

void Settings::Save() {
    auto path = GetSettingsPath();
    std::wcout << L"SETTINGS: Sauvegarde vers " << path.wstring() << std::endl;
    std::wofstream file(path);
    if (file.is_open()) {
        file << L"BufferDuration=" << bufferDuration << L"\n";
        file << L"FPS=" << fps << L"\n";
        file << L"Bitrate=" << bitrate << L"\n";
        file << L"StoragePath=" << EnsureFernLeaf(storagePath).wstring() << L"\n";
        file << L"Hotkey=" << (hotkey.empty() ? L"Alt+Shift+F9" : hotkey) << L"\n";
        file << L"MicrophoneDeviceId=" << microphoneDeviceId << L"\n";
        file << L"MicrophoneDeviceName=" << microphoneDeviceName << L"\n";
        file << L"VideoCodec=" << videoCodec << L"\n";
        file << L"EncoderProfile=" << encoderProfile << L"\n";
        file << L"RateControl=" << rateControl << L"\n";
        file << L"MaxBitrateMultiplier=" << std::clamp(maxBitrateMultiplier, 100, 400) << L"\n";
        file << L"GopSeconds=" << std::clamp(gopSeconds, 1, 10) << L"\n";
        file << L"BFrames=" << std::clamp(bFrames, 0, 4) << L"\n";
        file << L"LowLatency=" << BoolText(lowLatency) << L"\n";
        file << L"QualityVsSpeed=" << std::clamp(qualityVsSpeed, 0, 100) << L"\n";
        file << L"EncoderIndex=" << std::max(0, encoderIndex) << L"\n";
        file << L"TargetScreenName=" << targetScreenName << L"\n";
        file.close();
    } else {
        std::wcerr << L"SETTINGS ERROR: Impossible d'ouvrir en ecriture: " << path.wstring() << std::endl;
    }
}
