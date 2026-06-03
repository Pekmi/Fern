#include <windows.h>

#include "../include/fern/ipc_server.h"


std::atomic<bool> running(true);
std::atomic<bool> triggerSave(false);


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
                if (strcmp(buffer, "SAVE") == 0) {
                    triggerSave = true; //lance save
                }
            }
        }

        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
    }
}