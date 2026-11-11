#include "errors.hpp"
#include "logger.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <print>

using namespace Database;

// Sample function demonstrating Result<T> expected error handling
Result<int> safe_divide(int a, int b) {
    if (b == 0) {
        return make_error(ErrorCode::InvalidArgument, "division by zero is undefined");
    }
    return a / b;
}

// Sample function demonstrating Result<void>
Result<void> validate_key(std::string_view key) {
    if (key.empty()) {
        return make_error(ErrorCode::InvalidArgument, "database key cannot be empty");
    }
    if (key.size() > 256) {
        return MAKE_ERROR(ErrorCode::PayloadTooLarge, "key size {} exceeds 256 bytes", key.size());
    }
    return {};
}

void test_logger_levels() {
    std::println("--- Testing Logger Levels ---");
    Logger::set_level(LogLevel::Debug);

    LOG_TRACE("This TRACE should NOT be displayed (min level is Debug)");
    LOG_DEBUG("This is a DEBUG message: testing {}", 123);
    LOG_INFO("This is an INFO message: database status is {}", "ready");
    LOG_WARN("This is a WARN message: high memory watermark reached ({}%)", 85);
    LOG_ERROR("This is an ERROR message: failed to open mock connection to {}:{}", "127.0.0.1", 9999);
    LOG_FATAL("This is a FATAL message: critical mock subsystem alert");
}

void test_logger_file_output() {
    std::println("\n--- Testing Logger File Output ---");
    const std::string test_log_path = "./test_output.log";

    if (std::filesystem::exists(test_log_path)) {
        std::filesystem::remove(test_log_path);
    }

    assert(Logger::set_log_file(test_log_path));
    LOG_INFO("Testing file logging persistence: record #{}", 42);
    Logger::close_log_file();

    assert(std::filesystem::exists(test_log_path));

    std::ifstream file(test_log_path);
    std::string line;
    bool found_record = false;
    while (std::getline(file, line)) {
        if (line.find("Testing file logging persistence: record #42") != std::string::npos) {
            found_record = true;
            break;
        }
    }
    file.close();
    std::filesystem::remove(test_log_path);

    assert(found_record);
    std::println("File logging test PASSED!");
}

void test_logger_performance_overhead() {
    std::println("\n--- Testing Logger Performance (Disabled Level Overhead) ---");
    Logger::set_level(LogLevel::Info);

    constexpr int iterations = 10'000'000;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        // Trace and Debug are disabled. Zero format, zero heap allocation!
        LOG_DEBUG("Trace log with complex formatting {} and int {}", "parameter", i);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    double ns_per_call = static_cast<double>(elapsed_ns) / iterations;

    std::println("Executed {} disabled log calls in {:.2f} ms ({:.2f} ns per call)",
                 iterations, elapsed_ns / 1'000'000.0, ns_per_call);
    assert(ns_per_call < 50.0); // Should be a few nanoseconds on modern CPUs
    std::println("Zero-overhead benchmark PASSED!");
}

void test_expected_errors() {
    std::println("\n--- Testing Expected Errors (Result<T>) ---");

    // Success path
    auto ok_div = safe_divide(100, 5);
    assert(ok_div.has_value());
    assert(*ok_div == 20);

    // Error path
    auto err_div = safe_divide(100, 0);
    assert(!err_div.has_value());
    assert(err_div.error().code() == ErrorCode::InvalidArgument);
    assert(err_div.error().message() == "division by zero is undefined");
    std::println("Captured expected error: {}", err_div.error().to_string());

    // Result<void> test
    auto valid_key_res = validate_key("user:1001");
    assert(valid_key_res.has_value());

    auto empty_key_res = validate_key("");
    assert(!empty_key_res.has_value());
    assert(empty_key_res.error().code() == ErrorCode::InvalidArgument);

    std::string long_key(300, 'x');
    auto large_key_res = validate_key(long_key);
    assert(!large_key_res.has_value());
    assert(large_key_res.error().code() == ErrorCode::PayloadTooLarge);
    std::println("Captured formatted error: {}", large_key_res.error().to_string());

    std::println("Result<T> error tests PASSED!");
}

void test_try_catch_wrapper() {
    std::println("\n--- Testing try_catch Wrapper ---");

    auto ok_res = try_catch([]() -> int {
        return 42;
    });
    assert(ok_res.has_value() && *ok_res == 42);

    auto exc_res = try_catch([]() -> int {
        throw std::runtime_error("Simulated unexpected exception");
    });
    assert(!exc_res.has_value());
    assert(exc_res.error().code() == ErrorCode::Internal);
    std::println("Safely converted exception to Result: {}", exc_res.error().to_string());

    std::println("try_catch wrapper test PASSED!");
}

void test_fatal_exceptions() {
    std::println("\n--- Testing Fatal Exceptions ---");
    bool caught_database_exc = false;
    bool caught_corruption_exc = false;

    try {
        throw DatabaseException(ErrorCode::IoError, "Unable to sync page table to disk");
    } catch (const DatabaseException& e) {
        caught_database_exc = true;
        assert(e.code() == ErrorCode::IoError);
    }
    assert(caught_database_exc);

    try {
        throw CorruptionError("Header checksum mismatch in page 42");
    } catch (const CorruptionError& e) {
        caught_corruption_exc = true;
        assert(e.code() == ErrorCode::CorruptedData);
    }
    assert(caught_corruption_exc);

    // Test top-level handler
    try {
        throw FatalError(ErrorCode::OutOfMemory, "Failed to allocate 1GB buffer pool segment");
    } catch (...) {
        handle_top_level_exception();
    }

    std::println("Fatal exception tests PASSED!");
}

void test_invariants() {
    std::println("\n--- Testing Invariants (DB_ASSERT) ---");
    int buffer_size = 4096;
    DB_ASSERT(buffer_size > 0);
    DB_ASSERT_MSG(buffer_size == 4096, "Buffer size must match page size");
    std::println("DB_ASSERT tests PASSED!");
}

void test_logger_ring_buffer_and_filtering() {
    std::println("\n--- Testing Ring Buffer Retrieval & Level Filtering ---");

    // 1. Level filtering test
    Logger::set_level(LogLevel::Warn);
    Logger::clear_recent_logs();
    assert(Logger::get_recent_logs_count() == 0);
    assert(Logger::get_recent_logs().empty());

    // Messages below Warn should be filtered out and not enter the ring buffer
    LOG_TRACE("Should be filtered (trace)");
    LOG_DEBUG("Should be filtered (debug)");
    LOG_INFO("Should be filtered (info)");
    assert(Logger::get_recent_logs_count() == 0);

    // Messages at or above Warn should be recorded
    LOG_WARN("Allowed warning record");
    LOG_ERROR("Allowed error record: code {}", 500);
    LOG_FATAL("Allowed fatal alert");

    auto logs = Logger::get_recent_logs();
    assert(logs.size() == 3);
    assert(logs[0].find("Allowed warning record") != std::string::npos);
    assert(logs[1].find("Allowed error record: code 500") != std::string::npos);
    assert(logs[2].find("Allowed fatal alert") != std::string::npos);

    // 2. Circular buffer capacity and FIFO eviction test
    Logger::set_level(LogLevel::Debug);
    const idx_t original_cap = Logger::get_capacity();
    constexpr idx_t test_cap = 5;
    Logger::set_capacity(test_cap);
    assert(Logger::get_capacity() == test_cap);
    Logger::clear_recent_logs();
    assert(Logger::get_recent_logs_count() == 0);

    for (int i = 1; i <= 10; ++i) {
        LOG_INFO("Ring buffer message sequence #{}", i);
    }

    assert(Logger::get_recent_logs_count() == test_cap);
    auto recent = Logger::get_recent_logs(test_cap);
    assert(recent.size() == test_cap);

    // Evicted messages #1..#5; kept messages #6..#10
    assert(recent[0].find("sequence #6") != std::string::npos);
    assert(recent[1].find("sequence #7") != std::string::npos);
    assert(recent[2].find("sequence #8") != std::string::npos);
    assert(recent[3].find("sequence #9") != std::string::npos);
    assert(recent[4].find("sequence #10") != std::string::npos);

    // Partial retrieval: fetch only last 2
    auto last_2 = Logger::get_recent_logs(2);
    assert(last_2.size() == 2);
    assert(last_2[0].find("sequence #9") != std::string::npos);
    assert(last_2[1].find("sequence #10") != std::string::npos);

    // Fetch more lines than capacity
    auto all_from_large_query = Logger::get_recent_logs(100);
    assert(all_from_large_query.size() == test_cap);

    // Zero fetch
    auto zero_logs = Logger::get_recent_logs(0);
    assert(zero_logs.empty());

    // Clear logs
    Logger::clear_recent_logs();
    assert(Logger::get_recent_logs_count() == 0);
    assert(Logger::get_recent_logs().empty());

    // Restore original capacity
    Logger::set_capacity(original_cap);
    std::println("Ring buffer retrieval & level filtering test PASSED!");
}

int main() {
    std::println("=========================================");
    std::println("Running WhiskDB Logger & Error Test Suite");
    std::println("=========================================");

    test_logger_levels();
    test_logger_file_output();
    test_logger_ring_buffer_and_filtering();
    test_logger_performance_overhead();
    test_expected_errors();
    test_try_catch_wrapper();
    test_fatal_exceptions();
    test_invariants();

    std::println("\n=========================================");
    std::println("ALL TESTS PASSED SUCCESSFULLY!");
    std::println("=========================================");
    return 0;
}
