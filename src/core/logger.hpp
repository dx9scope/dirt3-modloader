#pragma once

#include <string>
#include <string_view>
#include <sstream>
#include <format>
#include <mutex>

namespace modloader {

enum class LogLevel {
    Debug,
    Info,
    Warn,
    Error
};

class Logger {
public:
    static Logger& Instance();

    void Init(bool openConsole = false);
    void Shutdown();

    void Log(LogLevel level, std::string_view component, std::string_view message);

    template <typename... Args>
    void LogFmt(LogLevel level, std::string_view component, std::format_string<Args...> fmt, Args&&... args) {
        std::string formatted = std::format(fmt, std::forward<Args>(args)...);
        Log(level, component, formatted);
    }

private:
    Logger() = default;
    ~Logger();

    std::mutex mutex_;
    void* logFileHandle_{ nullptr };
    bool consoleOpen_{ false };
};

} // namespace modloader

#define MOD_LOG_DEBUG(comp, ...) ::modloader::Logger::Instance().LogFmt(::modloader::LogLevel::Debug, comp, __VA_ARGS__)
#define MOD_LOG_INFO(comp, ...)  ::modloader::Logger::Instance().LogFmt(::modloader::LogLevel::Info,  comp, __VA_ARGS__)
#define MOD_LOG_WARN(comp, ...)  ::modloader::Logger::Instance().LogFmt(::modloader::LogLevel::Warn,  comp, __VA_ARGS__)
#define MOD_LOG_ERROR(comp, ...) ::modloader::Logger::Instance().LogFmt(::modloader::LogLevel::Error, comp, __VA_ARGS__)
