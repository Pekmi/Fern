#include <windows.h>

#include "../include/fern/ipc_server.h"

#include <algorithm>
#include <cctype>
#include <string>


std::atomic<bool> running(true);
std::atomic<bool> triggerSave(false);

namespace {

std::string NormalizeCommand(const char* value) {
    std::string command = value ? value : "";
    command.erase(std::remove_if(command.begin(), command.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }), command.end());

    std::transform(command.begin(), command.end(), command.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });

    return command;
}

}

void RunIpcServer(std::atomic<bool>& running, std::atomic<bool>& triggerSave) {
    while (running) {
        //cree pipe
        HANDLE hPipe = CreateNamedPipeW(
            L"\\\\.\\pipe\\FernPipe",
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            1, 1024, 1024, 0, NULL);

        if (hPipe == INVALID_HANDLE_VALUE) {
            Sleep(100);
            continue;
        }

        //client connecté?
        if (ConnectNamedPipe(hPipe, NULL) || GetLastError() == ERROR_PIPE_CONNECTED) {
            char buffer[128];
            DWORD bytesRead;
            
            if (ReadFile(hPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL)) {
                buffer[bytesRead] = '\0';
                const std::string command = NormalizeCommand(buffer);
                if (command == "SAVE") {
                    triggerSave = true; //lance save
                } else if (command == "STOP") {
                    running = false;
                }
            }
        }

        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
    }
}
