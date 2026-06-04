#include <windows.h>
#include <iostream>

#include "../include/fern/hotkey.h"

void RunHotkeyListener(std::atomic<bool>& running, std::atomic<bool>& triggerSave) {
    if (!RegisterHotKey(NULL, FERN_HOTKEY_ID, MOD_ALT | MOD_SHIFT, VK_F9)) { //alt shift F9
        DWORD error = GetLastError();
        std::cerr << "[Hotkey] ECHEC (Erreur : " << error << ")" << std::endl;
        return;
    }

    // std::cout << "[Hotkey] Enregistre avec SUCCES" << std::endl;

    MSG msg = { 0 };
    while (running && GetMessage(&msg, NULL, 0, 0)) {
        if (msg.message == WM_HOTKEY) {
            if (msg.wParam == FERN_HOTKEY_ID) {
                std::cout << "[Hotkey] Capture" << std::endl;
                triggerSave.store(true);
            }
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UnregisterHotKey(NULL, FERN_HOTKEY_ID);
    std::cout << "[Hotkey] Libere." << std::endl;
}
