#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <filesystem>
#include <string>

namespace fern {

enum class LogLevel {
    Info,
    Warning,
    Error,
    Debug
};

bool InitializeLogging();
void ShutdownLogging();

std::filesystem::path GetLogDirectory();
std::filesystem::path GetLogFilePath();

void Log(LogLevel level, const std::wstring& category, const std::wstring& message);
void Log(LogLevel level, const std::string& category, const std::string& message);

void LogInfo(const std::wstring& category, const std::wstring& message);
void LogWarning(const std::wstring& category, const std::wstring& message);
void LogError(const std::wstring& category, const std::wstring& message);
void LogDebug(const std::wstring& category, const std::wstring& message);

void LogHResult(LogLevel level, const std::wstring& category, const std::wstring& message, HRESULT hr);
std::wstring FormatHResult(HRESULT hr);

void LogSystemInfo();

}
