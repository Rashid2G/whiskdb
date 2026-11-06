#ifndef WHISKDB_STORAGE_POSIX_FILE_HPP
#define WHISKDB_STORAGE_POSIX_FILE_HPP

// Internal (src/storage only): thin RAII + error helpers over POSIX file APIs.

#include "errors.hpp"

#include <cstdint>
#include <filesystem>
#include <source_location>
#include <span>
#include <string_view>
#include <vector>

namespace database::posix {

/// Owns a file descriptor and closes it on destruction.
class UniqueFd {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : fd_(other.Release()) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            Close();
            fd_ = other.Release();
        }
        return *this;
    }
    ~UniqueFd() { Close(); }

public:
    int Get() const noexcept { return fd_; }
    bool IsValid() const noexcept { return fd_ >= 0; }
    int Release() noexcept {
        int fd = fd_;
        fd_ = -1;
        return fd;
    }
    void Close() noexcept;

private:
    int fd_ = -1;
};

/// Builds an IoError carrying the failed operation, path and strerror(err).
std::unexpected<Error> IoError(std::string_view op, const std::filesystem::path& path, int err,
                               std::source_location loc = std::source_location::current());

/// write() until all bytes are written (handles EINTR and short writes).
Result<void> WriteAll(int fd, std::span<const uint8_t> data, const std::filesystem::path& path);

/// read() until `dst` is full or EOF; returns the number of bytes read.
Result<idx_t> ReadUpTo(int fd, std::span<uint8_t> dst, const std::filesystem::path& path);

/// Reads the whole file into `out` (replacing its contents).
Result<void> ReadWholeFile(int fd, std::vector<uint8_t>& out, const std::filesystem::path& path);

/// fsync() on the directory so a create/rename inside it is durable.
Result<void> SyncDirectory(const std::filesystem::path& dir);

/// fdatasync() wrapper retrying on EINTR.
Result<void> DataSync(int fd, const std::filesystem::path& path);

} // namespace database::posix

#endif // WHISKDB_STORAGE_POSIX_FILE_HPP
