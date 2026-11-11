#include "logger.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <deque>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

namespace database {

namespace {

/// Default capacity for recent log lines kept in memory for the LOGS opcode
constexpr idx_t DEFAULT_RING_CAPACITY = 1000;

std::mutex g_log_mutex;
std::ofstream g_file_stream;
std::deque<std::string> g_ring;
std::atomic<idx_t> g_capacity{DEFAULT_RING_CAPACITY};

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

std::string GetCurrentThreadIdString() {
    thread_local const std::string t_tid_str = []() {
        std::ostringstream oss;
        oss << std::this_thread::get_id();
        return oss.str();
    }();
    return t_tid_str;
}

} // namespace

Logger& Logger::Instance() noexcept {
    static Logger instance;
    return instance;
}

void Logger::SetLevel(LogLevel level) noexcept {
    min_level_.store(level, std::memory_order_relaxed);
}

LogLevel Logger::GetLevel() noexcept {
    return min_level_.load(std::memory_order_relaxed);
}

void Logger::SetColorEnabled(bool enabled) noexcept {
    color_enabled_.store(enabled, std::memory_order_relaxed);
}

bool Logger::IsColorEnabled() noexcept {
    return color_enabled_.load(std::memory_order_relaxed);
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

idx_t Logger::GetCapacity() noexcept {
    return g_capacity.load(std::memory_order_relaxed);
}

void Logger::SetCapacity(idx_t capacity) noexcept {
    g_capacity.store(capacity, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_log_mutex);
    while (g_ring.size() > capacity) {
        g_ring.pop_front();
    }
}

void Logger::ClearRecentLogs() noexcept {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    g_ring.clear();
}

idx_t Logger::GetRecentLogsCount() noexcept {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    return static_cast<idx_t>(g_ring.size());
}

void Logger::WriteLog(LogLevel level,
                      std::string_view file,
                      int32_t line,
                      std::string_view message) {
    const auto now = std::chrono::system_clock::now();
    const auto epoch_micros = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();
    const std::time_t sec = epoch_micros / 1000000;
    const int32_t us = static_cast<int32_t>(epoch_micros % 1000000);

    struct tm tm_buf;
    localtime_r(&sec, &tm_buf);

    char time_str[36];
    std::snprintf(time_str, sizeof(time_str), "%04d-%02d-%02d %02d:%02d:%02d.%06d",
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, us);

    const std::string_view lvl_str = ToShortString(level);
    const std::string tid_str = GetCurrentThreadIdString();

    std::lock_guard<std::mutex> lock(g_log_mutex);

    std::ostream& out = (level >= LogLevel::Warn) ? std::cerr : std::cout;

    if (color_enabled_.load(std::memory_order_relaxed)) {
        out << COLOR_TRACE << "[" << time_str << "] "
            << GetLevelColor(level) << "[" << lvl_str << "] "
            << COLOR_LOC << "[tid:" << tid_str << "] "
            << "[" << file << ":" << line << "] "
            << COLOR_RESET << message << "\n";
    } else {
        out << "[" << time_str << "] [" << lvl_str << "] [tid:"
            << tid_str << "] [" << file << ":" << line << "] " << message << "\n";
    }

    if (g_file_stream.is_open()) {
        g_file_stream << "[" << time_str << "] [" << lvl_str << "] [tid:"
                      << tid_str << "] [" << file << ":" << line << "] " << message << "\n";
    }

    // Keep the most recent lines in memory so clients can fetch them (whisk logs)
    const idx_t cap = g_capacity.load(std::memory_order_relaxed);
    if (cap > 0) {
        while (g_ring.size() >= cap) {
            g_ring.pop_front();
        }
        g_ring.push_back(std::format("[{}] [{}] [tid:{}] [{}:{}] {}",
                                     time_str, lvl_str, tid_str, file, line, message));
    }
}

std::vector<std::string> Logger::GetRecentLogs(idx_t max_lines) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    const idx_t count = std::min<idx_t>(max_lines, static_cast<idx_t>(g_ring.size()));
    return {g_ring.end() - static_cast<std::ptrdiff_t>(count), g_ring.end()};
}

} // namespace database
