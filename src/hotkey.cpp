#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../include/fern/hotkey.h"
#include "../include/fern/settings.h"

namespace {

constexpr DWORD kReloadIntervalMs = 2000;

struct ParsedHotkey {
    UINT modifiers = 0;
    UINT virtualKey = VK_F9;
    std::wstring display = L"Alt+Shift+F9";
};

std::wstring Trim(std::wstring value) {
    const wchar_t* spaces = L" \t\r\n";
    const size_t first = value.find_first_not_of(spaces);
    if (first == std::wstring::npos) return L"";

    const size_t last = value.find_last_not_of(spaces);
    return value.substr(first, last - first + 1);
}

std::wstring Upper(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(towupper(c));
    });
    return value;
}

std::vector<std::wstring> SplitHotkey(const std::wstring& hotkey) {
    std::vector<std::wstring> parts;
    std::wstringstream stream(hotkey);
    std::wstring part;

    while (std::getline(stream, part, L'+')) {
        part = Upper(Trim(part));
        if (!part.empty()) parts.push_back(part);
    }

    return parts;
}

bool TryParseVirtualKey(const std::wstring& token, UINT& virtualKey) {
    if (token.size() == 1) {
        const wchar_t ch = token[0];
        if ((ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9')) {
            virtualKey = static_cast<UINT>(ch);
            return true;
        }
    }

    if (token.size() >= 2 && token[0] == L'F') {
        try {
            const int number = std::stoi(token.substr(1));
            if (number >= 1 && number <= 24) {
                virtualKey = VK_F1 + number - 1;
                return true;
            }
        } catch (...) {
            return false;
        }
    }

    if (token == L"SPACE" || token == L"ESPACE") virtualKey = VK_SPACE;
    else if (token == L"TAB") virtualKey = VK_TAB;
    else if (token == L"ENTER" || token == L"ENTREE" || token == L"RETURN") virtualKey = VK_RETURN;
    else if (token == L"ESC" || token == L"ESCAPE" || token == L"ECHAP") virtualKey = VK_ESCAPE;
    else if (token == L"INSERT" || token == L"INS") virtualKey = VK_INSERT;
    else if (token == L"DELETE" || token == L"DEL" || token == L"SUPPR") virtualKey = VK_DELETE;
    else if (token == L"HOME") virtualKey = VK_HOME;
    else if (token == L"END") virtualKey = VK_END;
    else if (token == L"PAGEUP" || token == L"PGUP") virtualKey = VK_PRIOR;
    else if (token == L"PAGEDOWN" || token == L"PGDN") virtualKey = VK_NEXT;
    else if (token == L"UP") virtualKey = VK_UP;
    else if (token == L"DOWN") virtualKey = VK_DOWN;
    else if (token == L"LEFT") virtualKey = VK_LEFT;
    else if (token == L"RIGHT") virtualKey = VK_RIGHT;
    else return false;

    return true;
}

ParsedHotkey ParseHotkey(std::wstring hotkey) {
    ParsedHotkey parsed;
    hotkey = Trim(hotkey);
    if (hotkey.empty()) return parsed;

    UINT key = 0;
    UINT modifiers = 0;

    for (const auto& token : SplitHotkey(hotkey)) {
        if (token == L"CTRL" || token == L"CONTROL") modifiers |= MOD_CONTROL;
        else if (token == L"ALT") modifiers |= MOD_ALT;
        else if (token == L"SHIFT" || token == L"MAJ") modifiers |= MOD_SHIFT;
        else if (token == L"WIN" || token == L"WINDOWS" || token == L"SUPER") modifiers |= MOD_WIN;
        else TryParseVirtualKey(token, key);
    }

    if (key == 0) return parsed;

    parsed.modifiers = modifiers;
    parsed.virtualKey = key;
    parsed.display = hotkey;
    return parsed;
}

ParsedHotkey LoadHotkey() {
    Settings settings;
    settings.Load(false);
    return ParseHotkey(settings.hotkey);
}

bool RegisterParsedHotkey(const ParsedHotkey& hotkey) {
    return RegisterHotKey(NULL, FERN_HOTKEY_ID, hotkey.modifiers | MOD_NOREPEAT, hotkey.virtualKey) != FALSE;
}

}

void RunHotkeyListener(std::atomic<bool>& running, std::atomic<bool>& triggerSave) {
    ParsedHotkey activeHotkey = LoadHotkey();
    bool registered = RegisterParsedHotkey(activeHotkey);
    if (!registered) {
        DWORD error = GetLastError();
        std::wcerr << L"[Hotkey] ECHEC " << activeHotkey.display << L" (Erreur : " << error << L")" << std::endl;
    } else {
        std::wcout << L"[Hotkey] Actif: " << activeHotkey.display << std::endl;
    }

    auto nextReload = std::chrono::steady_clock::now() + std::chrono::milliseconds(kReloadIntervalMs);

    while (running) {
        MSG msg = {};
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_HOTKEY && msg.wParam == FERN_HOTKEY_ID) {
                std::cout << "[Hotkey] Capture" << std::endl;
                triggerSave.store(true);
            }

            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= nextReload) {
            ParsedHotkey newHotkey = LoadHotkey();
            if (newHotkey.display != activeHotkey.display ||
                newHotkey.modifiers != activeHotkey.modifiers ||
                newHotkey.virtualKey != activeHotkey.virtualKey) {
                if (registered) UnregisterHotKey(NULL, FERN_HOTKEY_ID);
                registered = RegisterParsedHotkey(newHotkey);
                if (registered) {
                    activeHotkey = newHotkey;
                    std::wcout << L"[Hotkey] Actif: " << activeHotkey.display << std::endl;
                } else {
                    DWORD error = GetLastError();
                    std::wcerr << L"[Hotkey] ECHEC " << newHotkey.display << L" (Erreur : " << error << L")" << std::endl;
                    registered = RegisterParsedHotkey(activeHotkey);
                }
            }

            nextReload = now + std::chrono::milliseconds(kReloadIntervalMs);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    if (registered) UnregisterHotKey(NULL, FERN_HOTKEY_ID);
    std::cout << "[Hotkey] Libere." << std::endl;
}
