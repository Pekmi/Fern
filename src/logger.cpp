#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "../include/fern/logger.h"

#include <knownfolders.h>
#include <shlobj.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <vector>

namespace fern {
namespace {

constexpr size_t kMaxLogFiles = 20;

struct LoggerState {
    std::mutex mutex;
    HANDLE file = INVALID_HANDLE_VALUE;
    std::filesystem::path directory;
    std::filesystem::path filePath;
    bool initialized = false;
};

LoggerState& State() {
    static LoggerState state;
    return state;
}

std::wstring TimestampForFile();

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return "";

    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return "";

    std::string result(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return L"";

    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (size <= 1) return L"";

    std::wstring result(static_cast<size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), size);
    return result;
}

std::filesystem::path KnownFolderPath(REFKNOWNFOLDERID folderId) {
    wchar_t* path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(folderId, 0, nullptr, &path)) && path) {
        std::filesystem::path result(path);
        CoTaskMemFree(path);
        return result;
    }
    return {};
}

std::filesystem::path BuildLogDirectory() {
    std::filesystem::path root = KnownFolderPath(FOLDERID_LocalAppData);
    if (root.empty()) root = KnownFolderPath(FOLDERID_RoamingAppData);
    if (root.empty()) root = std::filesystem::current_path();

    root /= L"PekmisIndustries";
    root /= L"Fern";
    root /= L"logs";
    return root;
}

std::filesystem::path TempLogDirectory() {
    wchar_t tempPath[MAX_PATH] = {};
    const DWORD length = GetTempPathW(static_cast<DWORD>(std::size(tempPath)), tempPath);
    if (length == 0 || length >= std::size(tempPath)) return std::filesystem::current_path();

    std::filesystem::path root(tempPath);
    root /= L"Fern";
    root /= L"logs";
    return root;
}

HANDLE OpenLogFile(const std::filesystem::path& directory, std::filesystem::path& filePath) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) return INVALID_HANDLE_VALUE;

    std::wostringstream fileName;
    fileName << L"fern_" << TimestampForFile() << L"_" << GetCurrentProcessId() << L".log";
    filePath = directory / fileName.str();

    return CreateFileW(
        filePath.c_str(),
        GENERIC_WRITE | FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
}

std::wstring TimestampForFile() {
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    tm localTime{};
    localtime_s(&localTime, &now);

    wchar_t value[32] = {};
    wcsftime(value, std::size(value), L"%Y%m%d_%H%M%S", &localTime);
    return value;
}

std::string TimestampForLine() {
    const auto now = std::chrono::system_clock::now();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    auto time = std::chrono::system_clock::to_time_t(now);
    tm localTime{};
    localtime_s(&localTime, &time);

    std::ostringstream stream;
    stream << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S")
           << "." << std::setw(3) << std::setfill('0') << millis.count();
    return stream.str();
}

const char* LevelText(LogLevel level) {
    switch (level) {
    case LogLevel::Info: return "INFO";
    case LogLevel::Warning: return "WARN";
    case LogLevel::Error: return "ERROR";
    case LogLevel::Debug: return "DEBUG";
    default: return "INFO";
    }
}

std::wstring ArchitectureText(WORD architecture) {
    switch (architecture) {
    case PROCESSOR_ARCHITECTURE_AMD64: return L"x64";
    case PROCESSOR_ARCHITECTURE_ARM64: return L"arm64";
    case PROCESSOR_ARCHITECTURE_INTEL: return L"x86";
    default: return L"unknown";
    }
}

std::wstring WindowsVersionText() {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return L"unknown";

    auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
    if (!rtlGetVersion) return L"unknown";

    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (rtlGetVersion(&version) != 0) return L"unknown";

    std::wostringstream stream;
    stream << version.dwMajorVersion << L"." << version.dwMinorVersion
           << L"." << version.dwBuildNumber;
    return stream.str();
}

void PruneOldLogs(const std::filesystem::path& directory) {
    std::error_code ec;
    std::vector<std::filesystem::directory_entry> logs;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        if (entry.path().extension() == L".log") logs.push_back(entry);
    }

    if (logs.size() <= kMaxLogFiles) return;

    std::sort(logs.begin(), logs.end(), [](const auto& a, const auto& b) {
        std::error_code ecA;
        std::error_code ecB;
        return std::filesystem::last_write_time(a.path(), ecA) <
               std::filesystem::last_write_time(b.path(), ecB);
    });

    const size_t removeCount = logs.size() - kMaxLogFiles;
    for (size_t i = 0; i < removeCount; ++i) {
        std::filesystem::remove(logs[i].path(), ec);
    }
}

std::string BuildLogLine(LogLevel level, const std::string& category, const std::string& message) {
    std::ostringstream line;
    line << TimestampForLine()
         << " [" << GetCurrentProcessId() << ":" << GetCurrentThreadId() << "]"
         << " [" << LevelText(level) << "]"
         << " [" << category << "] "
         << message
         << "\n";
    return line.str();
}

void WriteLine(LogLevel level, const std::string& category, const std::string& message) {
    LoggerState& state = State();
    const std::string line = BuildLogLine(level, category, message);

    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.file != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER end{};
            SetFilePointerEx(state.file, end, nullptr, FILE_END);

            DWORD written = 0;
            WriteFile(state.file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
            FlushFileBuffers(state.file);
        }
    }

    OutputDebugStringA(line.c_str());
}

}

bool InitializeLogging() {
    LoggerState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.initialized) return state.file != INVALID_HANDLE_VALUE;

    state.directory = BuildLogDirectory();
    state.file = OpenLogFile(state.directory, state.filePath);
    if (state.file == INVALID_HANDLE_VALUE) {
        const DWORD localAppDataError = GetLastError();
        state.directory = TempLogDirectory();
        state.file = OpenLogFile(state.directory, state.filePath);
        if (state.file == INVALID_HANDLE_VALUE) {
            const DWORD tempError = GetLastError();
            state.directory = std::filesystem::current_path();
            state.file = OpenLogFile(state.directory, state.filePath);
            if (state.file == INVALID_HANDLE_VALUE) {
                const DWORD cwdError = GetLastError();
                std::wcerr << L"LOGGER: unable to open log file. localAppDataError=" << localAppDataError
                           << L" tempError=" << tempError
                           << L" cwdError=" << cwdError << std::endl;
                state.initialized = true;
                return false;
            }
        }
    }

    state.initialized = true;

    if (state.file != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{};
        if (GetFileSizeEx(state.file, &size) && size.QuadPart == 0) {
            constexpr char bom[] = "\xEF\xBB\xBF";
            DWORD written = 0;
            WriteFile(state.file, bom, 3, &written, nullptr);
            FlushFileBuffers(state.file);
        }
    }

    PruneOldLogs(state.directory);
    return state.file != INVALID_HANDLE_VALUE;
}

void ShutdownLogging() {
    LoggerState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.file != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(state.file);
        CloseHandle(state.file);
        state.file = INVALID_HANDLE_VALUE;
    }
    state.initialized = false;
}

std::filesystem::path GetLogDirectory() {
    LoggerState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.directory.empty() ? BuildLogDirectory() : state.directory;
}

std::filesystem::path GetLogFilePath() {
    LoggerState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.filePath;
}

void Log(LogLevel level, const std::wstring& category, const std::wstring& message) {
    WriteLine(level, WideToUtf8(category), WideToUtf8(message));
}

void Log(LogLevel level, const std::string& category, const std::string& message) {
    WriteLine(level, category, message);
}

void LogInfo(const std::wstring& category, const std::wstring& message) {
    Log(LogLevel::Info, category, message);
}

void LogWarning(const std::wstring& category, const std::wstring& message) {
    Log(LogLevel::Warning, category, message);
}

void LogError(const std::wstring& category, const std::wstring& message) {
    Log(LogLevel::Error, category, message);
}

void LogDebug(const std::wstring& category, const std::wstring& message) {
    Log(LogLevel::Debug, category, message);
}

std::wstring FormatHResult(HRESULT hr) {
    wchar_t buffer[16] = {};
    swprintf_s(buffer, L"0x%08X", static_cast<unsigned int>(hr));
    return buffer;
}

void LogHResult(LogLevel level, const std::wstring& category, const std::wstring& message, HRESULT hr) {
    Log(level, category, message + L" " + FormatHResult(hr));
}

void LogSystemInfo() {
    SYSTEM_INFO systemInfo{};
    GetNativeSystemInfo(&systemInfo);

    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    GlobalMemoryStatusEx(&memory);

    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));

    std::wostringstream stream;
    stream << L"pid=" << GetCurrentProcessId()
           << L" os=" << WindowsVersionText()
           << L" arch=" << ArchitectureText(systemInfo.wProcessorArchitecture)
           << L" processors=" << systemInfo.dwNumberOfProcessors
           << L" memoryMb=" << (memory.ullTotalPhys / (1024ull * 1024ull))
           << L" exe=" << exePath;
    LogInfo(L"SYSTEM", stream.str());

    LogInfo(L"LOG", L"file=" + GetLogFilePath().wstring());
}

}
