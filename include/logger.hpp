#ifndef WHISKDB_LOGGER_HPP
#define WHISKDB_LOGGER_HPP

#include "common/types.hpp"

#include <atomic>
#include <cstdint>
#include <format>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

namespace database {

/**
 * @brief Severity levels for database logging.
 */
enum class LogLevel : uint8_t {
    Trace = 0,
    Debug,
    Info,
    Warn,
    Error,
    Fatal,
    Off
};

constexpr std::string_view ToString(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
        case LogLevel::Off:   return "OFF";
    }
    return "UNKNOWN";
}

constexpr std::string_view to_string(LogLevel level) noexcept {
    return ToString(level);
}

constexpr std::string_view ToShortString(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace: return "TRAC";
        case LogLevel::Debug: return "DEBG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERRO";
        case LogLevel::Fatal: return "FATL";
        case LogLevel::Off:   return "OFF ";
    }
    return "UNKN";
}

constexpr std::string_view to_short_string(LogLevel level) noexcept {
    return ToShortString(level);
}

constexpr std::string_view ExtractFilename(std::string_view path) noexcept {
    auto pos = path.find_last_of("/\\");
    return (pos == std::string_view::npos) ? path : path.substr(pos + 1);
}

constexpr std::string_view extract_filename(std::string_view path) noexcept {
    return ExtractFilename(path);
}

class Logger {
public:
    static void SetLevel(LogLevel level) noexcept;
    static void set_level(LogLevel level) noexcept { SetLevel(level); }

    static LogLevel GetLevel() noexcept;
    static LogLevel get_level() noexcept { return GetLevel(); }

    static bool IsEnabled(LogLevel level) noexcept {
        return level >= min_level_.load(std::memory_order_relaxed);
    }
    static bool is_enabled(LogLevel level) noexcept { return IsEnabled(level); }

    static void SetColorEnabled(bool enabled) noexcept;
    static void set_color_enabled(bool enabled) noexcept { SetColorEnabled(enabled); }

    static bool SetLogFile(const std::string& filepath);
    static bool set_log_file(const std::string& filepath) { return SetLogFile(filepath); }

    static void CloseLogFile() noexcept;
    static void close_log_file() noexcept { CloseLogFile(); }

    /// Returns up to `max_lines` of the most recent log lines (oldest first), thread-safe.
    static std::vector<std::string> GetRecentLogs(idx_t max_lines = 100);

    static void WriteLog(LogLevel level,
                         std::string_view file,
                         int32_t line,
                         std::string_view message);
    static void write_log(LogLevel level,
                          std::string_view file,
                          int32_t line,
                          std::string_view message) {
        WriteLog(level, file, line, message);
    }

    template <typename... Args>
    static void Log(LogLevel level,
                    std::source_location loc,
                    std::format_string<Args...> fmt,
                    Args&&... args) {
        if (!IsEnabled(level)) {
            return;
        }
        std::string message = std::format(fmt, std::forward<Args>(args)...);
        WriteLog(level, ExtractFilename(loc.file_name()), static_cast<int32_t>(loc.line()), message);
    }

    template <typename... Args>
    static void log(LogLevel level,
                    std::source_location loc,
                    std::format_string<Args...> fmt,
                    Args&&... args) {
        Log(level, loc, fmt, std::forward<Args>(args)...);
    }

private:
    static inline std::atomic<LogLevel> min_level_{LogLevel::Info};
    static inline std::atomic<bool> color_enabled_{true};
};

} // namespace database

namespace Database = database;

#define LOG_TRACE(...) \
    do { \
        if (database::Logger::IsEnabled(database::LogLevel::Trace)) { \
            database::Logger::Log(database::LogLevel::Trace, std::source_location::current(), __VA_ARGS__); \
        } \
    } while (false)

#define LOG_DEBUG(...) \
    do { \
        if (database::Logger::IsEnabled(database::LogLevel::Debug)) { \
            database::Logger::Log(database::LogLevel::Debug, std::source_location::current(), __VA_ARGS__); \
        } \
    } while (false)

#define LOG_INFO(...) \
    do { \
        if (database::Logger::IsEnabled(database::LogLevel::Info)) { \
            database::Logger::Log(database::LogLevel::Info, std::source_location::current(), __VA_ARGS__); \
        } \
    } while (false)

#define LOG_WARN(...) \
    do { \
        if (database::Logger::IsEnabled(database::LogLevel::Warn)) { \
            database::Logger::Log(database::LogLevel::Warn, std::source_location::current(), __VA_ARGS__); \
        } \
    } while (false)

#define LOG_ERROR(...) \
    do { \
        if (database::Logger::IsEnabled(database::LogLevel::Error)) { \
            database::Logger::Log(database::LogLevel::Error, std::source_location::current(), __VA_ARGS__); \
        } \
    } while (false)

#define LOG_FATAL(...) \
    do { \
        if (database::Logger::IsEnabled(database::LogLevel::Fatal)) { \
            database::Logger::Log(database::LogLevel::Fatal, std::source_location::current(), __VA_ARGS__); \
        } \
    } while (false)

#endif // WHISKDB_LOGGER_HPP
