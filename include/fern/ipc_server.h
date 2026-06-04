#pragma once

#include <atomic>

extern std::atomic<bool> running;
extern std::atomic<bool> triggerSave;

void RunIpcServer(std::atomic<bool>& running, std::atomic<bool>& triggerSave);
