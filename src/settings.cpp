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

std::filesystem::path EnsureFernLeaf(std::filesystem::path path) {
    if (path.empty()) return path;

    if (ToLower(path.filename().wstring()) != L"fern") {
        path /= L"Fern";
    }
    return path;
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
            } catch (...) {
                std::wcerr << L"SETTINGS: Erreur de parsing pour " << key << std::endl;
            }
        }
    }

    storagePath = EnsureFernLeaf(storagePath).wstring();
    if (hotkey.empty()) hotkey = L"Alt+Shift+F9";

    if (!sawHotkey || ToLower(loadedStoragePath) != ToLower(storagePath)) {
        Save();
    }

    if (log) {
        std::wcout << L"SETTINGS: Charger. StoragePath=" << storagePath
                   << L" Hotkey=" << hotkey << std::endl;
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
        file.close();
    } else {
        std::wcerr << L"SETTINGS ERROR: Impossible d'ouvrir en ecriture: " << path.wstring() << std::endl;
    }
}
