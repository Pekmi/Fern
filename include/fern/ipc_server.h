#pragma once

#include <atomic>


void RunIpcServer(std::atomic<bool>& running, std::atomic<bool>& triggerSave);