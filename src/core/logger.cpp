#include "logger.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <iostream>
#include <chrono>
#include <iomanip>

namespace modloader {

Logger& Logger::Instance() {
    static Logger instance;
    return instance;
}

Logger::~Logger() {
    Shutdown();
}

void Logger::Init(bool openConsole) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (openConsole && !consoleOpen_) {
        if (AllocConsole()) {
            FILE* fDummy;
            freopen_s(&fDummy, "CONOUT$", "w", stdout);
            freopen_s(&fDummy, "CONOUT$", "w", stderr);
            freopen_s(&fDummy, "CONIN$", "r", stdin);
            SetConsoleTitleA("DiRT 3 Modloader Console");
            consoleOpen_ = true;
        }
    }

    // Open log file in current directory
    HANDLE hFile = CreateFileW(
        L"DiRT3Modloader.log",
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (hFile != INVALID_HANDLE_VALUE) {
        logFileHandle_ = (void*)hFile;
    }

    // Write header
    std::string header = "=== DiRT 3 Modloader Log Initialized ===\r\n";
    DWORD written = 0;
    if (logFileHandle_) {
        WriteFile((HANDLE)logFileHandle_, header.data(), (DWORD)header.size(), &written, nullptr);
    }
    if (consoleOpen_) {
        std::cout << header;
    }
}

void Logger::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (logFileHandle_) {
        std::string footer = "=== DiRT 3 Modloader Shut Down ===\r\n";
        DWORD written = 0;
        WriteFile((HANDLE)logFileHandle_, footer.data(), (DWORD)footer.size(), &written, nullptr);
        CloseHandle((HANDLE)logFileHandle_);
        logFileHandle_ = nullptr;
    }
    if (consoleOpen_) {
        FreeConsole();
        consoleOpen_ = false;
    }
}

void Logger::Log(LogLevel level, std::string_view component, std::string_view message) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Format timestamp
    SYSTEMTIME st;
    GetLocalTime(&st);

    const char* levelStr = "INFO";
    WORD color = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE; // default white

    switch (level) {
    case LogLevel::Debug:
        levelStr = "DEBUG";
        color = FOREGROUND_INTENSITY; // gray
        break;
    case LogLevel::Info:
        levelStr = "INFO";
        color = FOREGROUND_GREEN | FOREGROUND_INTENSITY; // green
        break;
    case LogLevel::Warn:
        levelStr = "WARN";
        color = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY; // yellow
        break;
    case LogLevel::Error:
        levelStr = "ERROR";
        color = FOREGROUND_RED | FOREGROUND_INTENSITY; // bright red
        break;
    }

    std::ostringstream ss;
    ss << "[" 
       << std::setfill('0') << std::setw(2) << st.wHour << ":"
       << std::setfill('0') << std::setw(2) << st.wMinute << ":"
       << std::setfill('0') << std::setw(2) << st.wSecond << "."
       << std::setfill('0') << std::setw(3) << st.wMilliseconds << "] "
       << "[" << levelStr << "] "
       << "[" << component << "] "
       << message << "\r\n";

    std::string line = ss.str();

    // Write to file
    if (logFileHandle_) {
        DWORD written = 0;
        WriteFile((HANDLE)logFileHandle_, line.data(), (DWORD)line.size(), &written, nullptr);
        FlushFileBuffers((HANDLE)logFileHandle_);
    }

    // Write to console
    if (consoleOpen_) {
        HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        GetConsoleScreenBufferInfo(hConsole, &csbi);
        SetConsoleTextAttribute(hConsole, color);
        std::cout << line;
        SetConsoleTextAttribute(hConsole, csbi.wAttributes);
    }

    // Also send to OutputDebugString for debugger
    OutputDebugStringA(line.c_str());
}

} // namespace modloader
