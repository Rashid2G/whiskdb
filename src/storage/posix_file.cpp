#include "posix_file.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace database::posix {

void UniqueFd::Close() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

std::unexpected<Error> IoError(std::string_view op, const std::filesystem::path& path, int err,
                               std::source_location loc) {
    return MakeError(ErrorCode::IoError,
                     std::format("{} '{}' failed: {}", op, path.string(), std::strerror(err)), loc);
}

Result<void> WriteAll(int fd, std::span<const uint8_t> data, const std::filesystem::path& path) {
    const uint8_t* p = data.data();
    idx_t remaining = data.size();
    while (remaining > 0) {
        ssize_t n = ::write(fd, p, remaining);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return IoError("write", path, errno);
        }
        p += n;
        remaining -= static_cast<idx_t>(n);
    }
    return {};
}

Result<idx_t> ReadUpTo(int fd, std::span<uint8_t> dst, const std::filesystem::path& path) {
    idx_t total = 0;
    while (total < dst.size()) {
        ssize_t n = ::read(fd, dst.data() + total, dst.size() - total);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return IoError("read", path, errno);
        }
        if (n == 0) {
            break;
        }
        total += static_cast<idx_t>(n);
    }
    return total;
}

Result<void> ReadWholeFile(int fd, std::vector<uint8_t>& out, const std::filesystem::path& path) {
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        return IoError("fstat", path, errno);
    }
    out.resize(static_cast<idx_t>(st.st_size));
    auto read_result = ReadUpTo(fd, out, path);
    if (!read_result) {
        return std::unexpected(read_result.error());
    }
    out.resize(*read_result);
    return {};
}

Result<void> SyncDirectory(const std::filesystem::path& dir) {
    std::filesystem::path target = dir.empty() ? std::filesystem::path(".") : dir;
    UniqueFd fd(::open(target.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!fd.IsValid()) {
        return IoError("open directory", target, errno);
    }
    if (::fsync(fd.Get()) != 0) {
        return IoError("fsync directory", target, errno);
    }
    return {};
}

Result<void> DataSync(int fd, const std::filesystem::path& path) {
    while (::fdatasync(fd) != 0) {
        if (errno != EINTR) {
            return IoError("fdatasync", path, errno);
        }
    }
    return {};
}

} // namespace database::posix
