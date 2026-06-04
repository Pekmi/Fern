#pragma once
#include <atomic>

//id du raccourci
#define FERN_HOTKEY_ID 0xF001

//écoute hotkey
void RunHotkeyListener(std::atomic<bool>& running, std::atomic<bool>& triggerSave);
