#ifndef WHISKDB_ERRORS_HPP
#define WHISKDB_ERRORS_HPP

#include "common/types.hpp"
#include "logger.hpp"

#include <cstdlib>
#include <expected>
#include <format>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace database {

/**
 * @brief Categorized error codes for expected operational failures in WhiskDB.
 *
 * 3-Tier Error Policy:
 * 1. Expected errors (e.g. bad syntax, client disconnected, unknown opcode) -> Return Result<T>
 * 2. Logic errors / Developer invariants (e.g. index out of bounds)          -> D_ASSERT(cond)
 * 3. Fatal errors (e.g. disk checksum corruption, OOM)                       -> Throw FatalError
 */
enum class ErrorCode : uint16_t {
    Ok = 0,

    // General & Internal
    Unknown,
    Internal,
    InvalidArgument,
    NotImplemented,
    Timeout,

    // Storage & Data (including SQL relational entity errors)
    NotFound,
    KeyNotFound,
    TableDoesNotExist,
    AlreadyExists,
    CorruptedData,
    ChecksumMismatch,
    IoError,
    OutOfMemory,

    // Network & Wire Protocol
    ConnectionClosed,
    ConnectionReset,
    PayloadTooLarge,
    ProtocolError,
    UnknownOpcode,
    AccessDenied,

    // WASM Runtime
    WasmError,
    WasmTrap,
    WasmFuelExhausted,
    WasmMemoryOutOfBounds
};

/**
 * @brief Get human-readable string view of an ErrorCode.
 */
constexpr std::string_view ToString(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Ok:                    return "Ok";
        case ErrorCode::Unknown:               return "Unknown";
        case ErrorCode::Internal:              return "Internal";
        case ErrorCode::InvalidArgument:       return "InvalidArgument";
        case ErrorCode::NotImplemented:        return "NotImplemented";
        case ErrorCode::Timeout:               return "Timeout";
        case ErrorCode::NotFound:              return "NotFound";
        case ErrorCode::KeyNotFound:           return "KeyNotFound";
        case ErrorCode::TableDoesNotExist:     return "TableDoesNotExist";
        case ErrorCode::AlreadyExists:         return "AlreadyExists";
        case ErrorCode::CorruptedData:         return "CorruptedData";
        case ErrorCode::ChecksumMismatch:      return "ChecksumMismatch";
        case ErrorCode::IoError:               return "IoError";
        case ErrorCode::OutOfMemory:           return "OutOfMemory";
        case ErrorCode::ConnectionClosed:      return "ConnectionClosed";
        case ErrorCode::ConnectionReset:       return "ConnectionReset";
        case ErrorCode::PayloadTooLarge:       return "PayloadTooLarge";
        case ErrorCode::ProtocolError:         return "ProtocolError";
        case ErrorCode::UnknownOpcode:         return "UnknownOpcode";
        case ErrorCode::AccessDenied:          return "AccessDenied";
        case ErrorCode::WasmError:             return "WasmError";
        case ErrorCode::WasmTrap:              return "WasmTrap";
        case ErrorCode::WasmFuelExhausted:     return "WasmFuelExhausted";
        case ErrorCode::WasmMemoryOutOfBounds: return "WasmMemoryOutOfBounds";
    }
    return "Unknown";
}

constexpr std::string_view to_string(ErrorCode code) noexcept {
    return ToString(code);
}

/**
 * @brief Represents an operational error with code, message, and origin location.
 */
class Error {
public:
    Error() noexcept = default;

    Error(ErrorCode code,
          std::string message = "",
          std::source_location loc = std::source_location::current()) noexcept
        : code_(code), message_(std::move(message)), location_(loc) {}

public:
    ErrorCode Code() const noexcept { return code_; }
    ErrorCode code() const noexcept { return code_; }

    const std::string& Message() const noexcept { return message_; }
    const std::string& message() const noexcept { return message_; }

    const std::source_location& Location() const noexcept { return location_; }
    const std::source_location& location() const noexcept { return location_; }

    bool IsOk() const noexcept { return code_ == ErrorCode::Ok; }
    bool is_ok() const noexcept { return IsOk(); }

    explicit operator bool() const noexcept { return !IsOk(); }

    std::string ToString() const {
        std::string_view file = extract_filename(location_.file_name());
        if (message_.empty()) {
            return std::format("{}:{} [{}]", file, location_.line(), database::ToString(code_));
        }
        return std::format("{}:{} [{}]: {}", file, location_.line(), database::ToString(code_), message_);
    }

    std::string to_string() const { return ToString(); }

private:
    ErrorCode code_{ErrorCode::Unknown};
    std::string message_;
    std::source_location location_{std::source_location::current()};
};

/**
 * @brief Result<T> is an alias for std::expected<T, Error>.
 */
template <typename T>
using Result = std::expected<T, Error>;

/**
 * @brief Helper to construct an unexpected Error for Result<T>.
 */
inline auto MakeError(ErrorCode code,
                      std::string message = "",
                      std::source_location loc = std::source_location::current()) -> std::unexpected<Error> {
    return std::unexpected(Error(code, std::move(message), loc));
}

inline auto make_error(ErrorCode code,
                       std::string message = "",
                       std::source_location loc = std::source_location::current()) -> std::unexpected<Error> {
    return MakeError(code, std::move(message), loc);
}

/**
 * @brief Formatted error helper with compile-time checked format strings.
 */
template <typename... Args>
inline auto FormatError(ErrorCode code,
                        std::source_location loc,
                        std::format_string<Args...> fmt,
                        Args&&... args) -> std::unexpected<Error> {
    return std::unexpected(Error(code, std::format(fmt, std::forward<Args>(args)...), loc));
}

template <typename... Args>
inline auto format_error(ErrorCode code,
                         std::source_location loc,
                         std::format_string<Args...> fmt,
                         Args&&... args) -> std::unexpected<Error> {
    return FormatError(code, loc, fmt, std::forward<Args>(args)...);
}

#define MAKE_ERROR(code, ...) \
    database::FormatError(code, std::source_location::current(), __VA_ARGS__)

/**
 * @brief Base exception for fatal / terminating conditions in WhiskDB.
 */
class DatabaseException : public std::runtime_error {
public:
    DatabaseException(ErrorCode code,
                      const std::string& message,
                      std::source_location loc = std::source_location::current())
        : std::runtime_error(std::format("{}:{} [{}]: {}",
                             extract_filename(loc.file_name()),
                             loc.line(),
                             ToString(code),
                             message)),
          code_(code),
          location_(loc) {}

public:
    ErrorCode Code() const noexcept { return code_; }
    ErrorCode code() const noexcept { return code_; }

    const std::source_location& Location() const noexcept { return location_; }
    const std::source_location& location() const noexcept { return location_; }

private:
    ErrorCode code_;
    std::source_location location_;
};

/**
 * @brief Fatal error requiring operation abort or system recovery.
 */
class FatalError : public DatabaseException {
public:
    using DatabaseException::DatabaseException;
};

/**
 * @brief Specialized corruption exception (e.g. invalid checksum, corrupted file header).
 */
class CorruptionError : public FatalError {
public:
    explicit CorruptionError(const std::string& message,
                             std::source_location loc = std::source_location::current())
        : FatalError(ErrorCode::CorruptedData, message, loc) {}
};

/**
 * @brief Top-level global exception handler for logging unhandled exceptions.
 */
void HandleTopLevelException(std::source_location loc = std::source_location::current()) noexcept;
inline void handle_top_level_exception(std::source_location loc = std::source_location::current()) noexcept {
    HandleTopLevelException(loc);
}

/**
 * @brief Safely executes a callable, catching exceptions and converting them to Result<T>.
 */
template <typename Func>
auto TryCatch(Func&& fn, std::source_location loc = std::source_location::current())
    -> Result<std::invoke_result_t<Func>> {
    try {
        if constexpr (std::is_void_v<std::invoke_result_t<Func>>) {
            fn();
            return {};
        } else {
            return fn();
        }
    } catch (const DatabaseException& e) {
        return MakeError(e.Code(), e.what(), e.Location());
    } catch (const std::exception& e) {
        return MakeError(ErrorCode::Internal, e.what(), loc);
    } catch (...) {
        return MakeError(ErrorCode::Unknown, "Unknown fatal exception caught", loc);
    }
}

template <typename Func>
auto try_catch(Func&& fn, std::source_location loc = std::source_location::current())
    -> Result<std::invoke_result_t<Func>> {
    return TryCatch(std::forward<Func>(fn), loc);
}

} // namespace database

namespace Database = database;

// DuckDB-style Invariant Assertions (Tier 2: Logic Errors / Developer Bugs)
#ifndef NDEBUG
    #define D_ASSERT_MSG(cond, msg) \
        do { \
            if (!(cond)) { \
                LOG_FATAL("Assertion failed: ({}) - {} [{}:{}:{}]", \
                          #cond, msg, database::extract_filename(__FILE__), __LINE__, __func__); \
                std::abort(); \
            } \
        } while (false)

    #define D_ASSERT(cond) D_ASSERT_MSG(cond, "Developer invariant violation")
#else
    #define D_ASSERT_MSG(cond, msg) do { (void)sizeof(cond); } while (false)
    #define D_ASSERT(cond)          do { (void)sizeof(cond); } while (false)
#endif

#define DB_ASSERT_MSG(cond, msg) D_ASSERT_MSG(cond, msg)
#define DB_ASSERT(cond)          D_ASSERT(cond)

// Formatters for std::format and std::print
template <>
struct std::formatter<database::ErrorCode> : std::formatter<std::string_view> {
    auto format(database::ErrorCode code, std::format_context& ctx) const {
        return std::formatter<std::string_view>::format(database::ToString(code), ctx);
    }
};

template <>
struct std::formatter<database::Error> : std::formatter<std::string> {
    auto format(const database::Error& err, std::format_context& ctx) const {
        return std::formatter<std::string>::format(err.ToString(), ctx);
    }
};

#endif // WHISKDB_ERRORS_HPP
