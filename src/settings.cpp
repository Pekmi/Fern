#include <fstream>
#include <shlobj.h>
#include <windows.h>
#include <iostream>

#include "../include/fern/settings.h"


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

void Settings::Load() {
    // 1. Définir le chemin par défaut vers Vidéos/Fern dynamiquement
    auto videoPath = GetKnownFolder(FOLDERID_Videos);
    if (!videoPath.empty()) {
        storagePath = (videoPath / L"Fern").wstring();
    } else {
        storagePath = L"C:\\Videos\\Fern"; 
    }

    // 2. Tenter de charger le fichier
    auto path = GetSettingsPath();
    std::wcout << L"SETTINGS: Tentative de chargement depuis " << path.wstring() << std::endl;

    if (!std::filesystem::exists(path)) {
        std::wcout << L"SETTINGS: Fichier absent, creation avec valeurs par defaut." << std::endl;
        Save();
        return;
    }

    std::wifstream file(path);
    if (!file.is_open()) {
        std::wcerr << L"SETTINGS ERROR: Impossible d'ouvrir " << path.wstring() << std::endl;
        return;
    }

    std::wstring line;
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
                else if (key == L"StoragePath") storagePath = value;
            } catch (...) {
                std::wcerr << L"SETTINGS: Erreur de parsing pour " << key << std::endl;
            }
        }
    }
    std::wcout << L"SETTINGS: Charger. StoragePath=" << storagePath << std::endl;
}

void Settings::Save() {
    auto path = GetSettingsPath();
    std::wcout << L"SETTINGS: Sauvegarde vers " << path.wstring() << std::endl;
    std::wofstream file(path);
    if (file.is_open()) {
        file << L"BufferDuration=" << bufferDuration << L"\n";
        file << L"FPS=" << fps << L"\n";
        file << L"Bitrate=" << bitrate << L"\n";
        file << L"StoragePath=" << storagePath << L"\n";
        file.close();
    } else {
        std::wcerr << L"SETTINGS ERROR: Impossible d'ouvrir en ecriture: " << path.wstring() << std::endl;
    }
}
