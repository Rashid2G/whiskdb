#include "logger.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <deque>
#include <fstream>
#include <iostream>
#include <mutex>

namespace database {

namespace {

/// Number of recent log lines kept in memory for the LOGS opcode
constexpr idx_t LOG_RING_CAPACITY = 1024;

std::mutex g_log_mutex;
std::ofstream g_file_stream;
std::deque<std::string> g_ring;

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

    // Keep the most recent lines in memory so clients can fetch them (whisk logs)
    if (g_ring.size() >= LOG_RING_CAPACITY) {
        g_ring.pop_front();
    }
    g_ring.push_back(std::format("[{}] [{}] [{}:{}] {}", time_str, lvl_str, file, line, message));
}

std::vector<std::string> Logger::GetRecentLogs(idx_t max_lines) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    const idx_t count = std::min<idx_t>(max_lines, g_ring.size());
    return {g_ring.end() - static_cast<std::ptrdiff_t>(count), g_ring.end()};
}

} // namespace database
