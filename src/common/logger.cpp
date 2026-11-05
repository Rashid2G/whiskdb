#include "logger.hpp"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>

namespace database {

namespace {

std::mutex g_log_mutex;
std::ofstream g_file_stream;

constexpr const char* COLOR_RESET = "\033[0m";
constexpr const char* COLOR_TRACE = "\033[90m";
constexpr const char* COLOR_DEBUG = "\033[36m";
constexpr const char* COLOR_INFO  = "\033[32m";
constexpr const char* COLOR_WARN  = "\033[33m";
constexpr const char* COLOR_ERROR = "\033[31m";
constexpr const char* COLOR_FATAL = "\033[1;35m";
constexpr const char* COLOR_LOC   = "\033[34m";

const char* GetLevelColor(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace: return COLOR_TRACE;
        case LogLevel::Debug: return COLOR_DEBUG;
        case LogLevel::Info:  return COLOR_INFO;
        case LogLevel::Warn:  return COLOR_WARN;
        case LogLevel::Error: return COLOR_ERROR;
        case LogLevel::Fatal: return COLOR_FATAL;
        default:              return COLOR_RESET;
    }
}

} // namespace

void Logger::SetLevel(LogLevel level) noexcept {
    min_level_.store(level, std::memory_order_relaxed);
}

LogLevel Logger::GetLevel() noexcept {
    return min_level_.load(std::memory_order_relaxed);
}

void Logger::SetColorEnabled(bool enabled) noexcept {
    color_enabled_.store(enabled, std::memory_order_relaxed);
}

bool Logger::SetLogFile(const std::string& filepath) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_file_stream.is_open()) {
        g_file_stream.close();
    }
    g_file_stream.open(filepath, std::ios::out | std::ios::app);
    return g_file_stream.is_open();
}

void Logger::CloseLogFile() noexcept {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_file_stream.is_open()) {
        g_file_stream.close();
    }
}

void Logger::WriteLog(LogLevel level,
                      std::string_view file,
                      int32_t line,
                      std::string_view message) {
    const auto now = std::chrono::system_clock::now();
    const auto epoch_millis = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    const std::time_t sec = epoch_millis / 1000;
    const int32_t ms = static_cast<int32_t>(epoch_millis % 1000);

    struct tm tm_buf;
    localtime_r(&sec, &tm_buf);

    char time_str[32];
    std::snprintf(time_str, sizeof(time_str), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, ms);

    const std::string_view lvl_str = ToShortString(level);

    std::lock_guard<std::mutex> lock(g_log_mutex);

    std::ostream& out = (level >= LogLevel::Warn) ? std::cerr : std::cout;

    if (color_enabled_.load(std::memory_order_relaxed)) {
        out << COLOR_TRACE << "[" << time_str << "] "
            << GetLevelColor(level) << "[" << lvl_str << "] "
            << COLOR_LOC << "[" << file << ":" << line << "] "
            << COLOR_RESET << message << "\n";
    } else {
        out << "[" << time_str << "] [" << lvl_str << "] ["
            << file << ":" << line << "] " << message << "\n";
    }

    if (g_file_stream.is_open()) {
        g_file_stream << "[" << time_str << "] [" << lvl_str << "] ["
                      << file << ":" << line << "] " << message << "\n";
    }
}

} // namespace database
