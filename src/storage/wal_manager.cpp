#include "storage/wal_manager.hpp"

#include "common/crc32.hpp"
#include "posix_file.hpp"
#include "wal_format.hpp"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace database {

namespace {

using namespace wal_format;

constexpr auto kRelaxedSyncInterval = std::chrono::milliseconds(100);

uint64_t NowMicros() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
}

bool IsKnownType(WalRecordType type) {
    return type == WalRecordType::SqlStatement || type == WalRecordType::Checkpoint;
}

} // namespace

WalManager::WalManager(PrivateTag, WalOptions options, lsn_t next_lsn)
    : options_(std::move(options)),
      next_lsn_(next_lsn),
      written_lsn_(next_lsn - 1),
      durable_lsn_(next_lsn - 1) {}

WalManager::~WalManager() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    cv_work_.notify_all();
    if (worker_.joinable()) {
        // The group-commit flush thread drains pending records before exiting
        worker_.join();
    }
    if (fd_ >= 0) {
        // Strict/Relaxed records are already in the page cache; make them durable
        if (durable_lsn_ < written_lsn_) {
            if (auto sync_result = posix::DataSync(fd_, options_.path); !sync_result) {
                LOG_ERROR("WAL close: {}", sync_result.error().ToString());
            }
        }
        ::close(fd_);
        fd_ = -1;
    }
}

Result<std::unique_ptr<WalManager>> WalManager::Open(WalOptions options, lsn_t next_lsn) {
    if (next_lsn == 0) {
        return MakeError(ErrorCode::InvalidArgument, "WAL next_lsn must be >= 1");
    }
    if (options.group_commit_max_records == 0) {
        options.group_commit_max_records = 1;
    }
    bool memory_only = options.mode == DurabilityMode::MemoryOnly;
    if (!memory_only && options.path.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "WAL path is required unless memory_only");
    }
    auto wal = std::make_unique<WalManager>(PrivateTag{}, std::move(options), next_lsn);
    if (!memory_only) {
        if (auto open_result = wal->OpenFile(); !open_result) {
            return std::unexpected(open_result.error());
        }
        wal->StartThreads();
    }
    return wal;
}

Result<void> WalManager::OpenFile() {
    const auto& path = options_.path;
    posix::UniqueFd fd(::open(path.c_str(), O_RDWR | O_APPEND | O_CREAT | O_CLOEXEC, 0644));
    if (!fd.IsValid()) {
        return posix::IoError("open", path, errno);
    }
    struct stat st {};
    if (::fstat(fd.Get(), &st) != 0) {
        return posix::IoError("fstat", path, errno);
    }
    auto header = MakeFileHeader();
    auto size = static_cast<idx_t>(st.st_size);

    if (size >= kFileHeaderSize) {
        std::array<uint8_t, kFileHeaderSize> existing{};
        auto read_result = posix::ReadUpTo(fd.Get(), existing, path);
        if (!read_result) {
            return std::unexpected(read_result.error());
        }
        if (existing != header) {
            return MAKE_ERROR(ErrorCode::CorruptedData,
                              "'{}' is not a WhiskDB WAL (bad magic or version)", path.string());
        }
        fd_ = fd.Release();
        return {};
    }

    if (size > 0) {
        // Crash while creating the file: only a prefix of the header may be present
        std::array<uint8_t, kFileHeaderSize> partial{};
        auto read_result = posix::ReadUpTo(fd.Get(), std::span<uint8_t>(partial.data(), size), path);
        if (!read_result) {
            return std::unexpected(read_result.error());
        }
        if (!std::equal(partial.begin(), partial.begin() + static_cast<std::ptrdiff_t>(size), header.begin())) {
            return MAKE_ERROR(ErrorCode::CorruptedData, "'{}' is not a WhiskDB WAL (short garbage file)",
                              path.string());
        }
        if (::ftruncate(fd.Get(), 0) != 0) {
            return posix::IoError("ftruncate", path, errno);
        }
    }

    if (auto write_result = posix::WriteAll(fd.Get(), header, path); !write_result) {
        return write_result;
    }
    if (auto sync_result = posix::DataSync(fd.Get(), path); !sync_result) {
        return sync_result;
    }
    if (auto dir_result = posix::SyncDirectory(path.parent_path()); !dir_result) {
        return dir_result;
    }
    fd_ = fd.Release();
    return {};
}

void WalManager::StartThreads() {
    if (options_.mode == DurabilityMode::GroupCommit) {
        worker_ = std::thread([this] { FlushLoop(); });
    } else if (options_.mode == DurabilityMode::Relaxed) {
        worker_ = std::thread([this] { RelaxedSyncLoop(); });
    }
}

void WalManager::EncodeRecordLocked(lsn_t lsn, WalRecordType type, std::span<const uint8_t> payload) {
    idx_t total = kRecordOverhead + payload.size();
    idx_t start = buffer_.size();
    buffer_.resize(start + total);
    uint8_t* p = buffer_.data() + start;
    PutU64(p, lsn);
    PutU64(p + 8, NowMicros());
    p[16] = static_cast<uint8_t>(type);
    PutU32(p + 17, static_cast<uint32_t>(payload.size()));
    if (!payload.empty()) {
        std::copy(payload.begin(), payload.end(), p + kRecordHeaderSize);
    }
    uint32_t crc = Crc32(std::span<const uint8_t>(p, total - kRecordTrailerSize));
    PutU32(p + total - kRecordTrailerSize, crc);
}

Result<lsn_t> WalManager::Append(WalRecordType type, std::span<const uint8_t> payload) {
    auto lsn = AppendNoWait(type, payload);
    if (!lsn) {
        return lsn;
    }
    if (auto wait_result = WaitDurable(*lsn); !wait_result) {
        return std::unexpected(wait_result.error());
    }
    return lsn;
}

Result<lsn_t> WalManager::AppendNoWait(WalRecordType type, std::span<const uint8_t> payload) {
    if (!IsKnownType(type)) {
        return MakeError(ErrorCode::InvalidArgument, "unknown WAL record type");
    }
    if (payload.size() > kMaxPayloadSize) {
        return MAKE_ERROR(ErrorCode::PayloadTooLarge, "WAL payload of {} bytes exceeds limit {}",
                          payload.size(), kMaxPayloadSize);
    }

    std::lock_guard<std::mutex> lock(mu_);
    if (io_error_) {
        return std::unexpected(*io_error_);
    }
    lsn_t lsn = next_lsn_++;
    stats_.appended++;
    if (options_.mode == DurabilityMode::MemoryOnly) {
        return lsn;
    }

    EncodeRecordLocked(lsn, type, payload);

    if (options_.mode == DurabilityMode::GroupCommit) {
        pending_records_++;
        if (pending_records_ == 1) {
            first_pending_time_ = std::chrono::steady_clock::now();
            cv_work_.notify_one();
        } else if (pending_records_ >= options_.group_commit_max_records) {
            cv_work_.notify_one();
        }
        return lsn;
    }

    // Strict / Relaxed: write under the lock so file order equals LSN order
    auto write_result = posix::WriteAll(fd_, buffer_, options_.path);
    idx_t record_bytes = buffer_.size();
    buffer_.clear();
    if (!write_result) {
        io_error_ = write_result.error();
        cv_durable_.notify_all();
        return std::unexpected(write_result.error());
    }
    stats_.bytes_written += record_bytes;
    written_lsn_ = lsn;
    return lsn;
}

Result<void> WalManager::WaitSyncedLocked(std::unique_lock<std::mutex>& lock, lsn_t lsn) {
    while (true) {
        if (durable_lsn_ >= lsn) {
            return {};
        }
        if (io_error_) {
            return std::unexpected(*io_error_);
        }
        if (sync_active_) {
            cv_durable_.wait(lock);
            continue;
        }
        // Become the sync leader: everything written so far is covered by one fdatasync
        sync_active_ = true;
        lsn_t target = written_lsn_;
        lock.unlock();
        auto sync_result = posix::DataSync(fd_, options_.path);
        lock.lock();
        sync_active_ = false;
        stats_.fsync_calls++;
        if (sync_result) {
            durable_lsn_ = std::max(durable_lsn_, target);
        } else {
            io_error_ = sync_result.error();
        }
        cv_durable_.notify_all();
    }
}

Result<void> WalManager::WaitDurable(lsn_t lsn) {
    if (options_.mode == DurabilityMode::MemoryOnly || options_.mode == DurabilityMode::Relaxed) {
        return {};
    }
    std::unique_lock<std::mutex> lock(mu_);
    if (lsn >= next_lsn_) {
        return MAKE_ERROR(ErrorCode::InvalidArgument, "WaitDurable({}) beyond last assigned LSN {}", lsn,
                          next_lsn_ - 1);
    }
    if (options_.mode == DurabilityMode::Strict) {
        return WaitSyncedLocked(lock, lsn);
    }
    cv_durable_.wait(lock, [&] { return durable_lsn_ >= lsn || io_error_.has_value(); });
    if (durable_lsn_ >= lsn) {
        return {};
    }
    return std::unexpected(*io_error_);
}

Result<void> WalManager::Flush() {
    if (options_.mode == DurabilityMode::MemoryOnly) {
        return {};
    }
    std::unique_lock<std::mutex> lock(mu_);
    if (options_.mode == DurabilityMode::GroupCommit) {
        lsn_t target = next_lsn_ - 1;
        if (durable_lsn_ < target && !io_error_) {
            flush_requested_ = true;
            cv_work_.notify_one();
        }
        cv_durable_.wait(lock, [&] { return durable_lsn_ >= target || io_error_.has_value(); });
        if (durable_lsn_ >= target) {
            return {};
        }
        return std::unexpected(*io_error_);
    }
    return WaitSyncedLocked(lock, written_lsn_);
}

lsn_t WalManager::LastLsn() const {
    std::lock_guard<std::mutex> lock(mu_);
    return next_lsn_ - 1;
}

WalManager::Stats WalManager::GetStats() const {
    std::lock_guard<std::mutex> lock(mu_);
    return stats_;
}

Result<void> WalManager::Reset(lsn_t next_lsn) {
    if (next_lsn == 0) {
        return MakeError(ErrorCode::InvalidArgument, "WAL next_lsn must be >= 1");
    }
    std::unique_lock<std::mutex> lock(mu_);
    if (options_.mode == DurabilityMode::MemoryOnly) {
        next_lsn_ = next_lsn;
        written_lsn_ = durable_lsn_ = next_lsn - 1;
        return {};
    }

    // Drain: no I/O in flight and nothing pending, then keep the lock until done
    while (true) {
        if (io_error_) {
            return std::unexpected(*io_error_);
        }
        if (sync_active_) {
            cv_durable_.wait(lock);
            continue;
        }
        if (pending_records_ > 0) {
            flush_requested_ = true;
            cv_work_.notify_one();
            cv_durable_.wait(lock);
            continue;
        }
        break;
    }
    // Invariant: the lock is held and the log is quiescent, so truncation cannot race a writer
    D_ASSERT(!sync_active_ && pending_records_ == 0);

    if (::ftruncate(fd_, static_cast<off_t>(kFileHeaderSize)) != 0) {
        return posix::IoError("ftruncate", options_.path, errno);
    }
    if (auto sync_result = posix::DataSync(fd_, options_.path); !sync_result) {
        io_error_ = sync_result.error();
        return sync_result;
    }
    stats_.fsync_calls++;
    next_lsn_ = next_lsn;
    written_lsn_ = durable_lsn_ = next_lsn - 1;
    flush_requested_ = false;
    return {};
}

void WalManager::FlushLoop() {
    std::unique_lock<std::mutex> lock(mu_);
    const auto interval = std::chrono::microseconds(options_.group_commit_interval_us);
    while (true) {
        cv_work_.wait(lock, [this] { return stop_ || pending_records_ > 0 || flush_requested_; });
        if (pending_records_ == 0) {
            flush_requested_ = false;
            cv_durable_.notify_all();
            if (stop_) {
                break;
            }
            continue;
        }

        // Batching delay: wait for more writers unless we are shutting down, were
        // asked to flush, or the previous batch was a lone record (idle fast path)
        if (!stop_ && !flush_requested_ && last_batch_records_ > 1 &&
            pending_records_ < options_.group_commit_max_records) {
            cv_work_.wait_until(lock, first_pending_time_ + interval, [this] {
                return stop_ || flush_requested_ ||
                       pending_records_ >= options_.group_commit_max_records;
            });
        }

        // Take the whole batch; appenders keep filling the (now empty) buffer_
        sync_active_ = true;
        io_buf_.swap(buffer_);
        idx_t batch_records = pending_records_;
        pending_records_ = 0;
        lsn_t batch_last_lsn = next_lsn_ - 1;
        lock.unlock();

        auto io_result = posix::WriteAll(fd_, io_buf_, options_.path);
        if (io_result) {
            io_result = posix::DataSync(fd_, options_.path);
        }
        idx_t batch_bytes = io_buf_.size();
        io_buf_.clear();

        lock.lock();
        sync_active_ = false;
        stats_.fsync_calls++;
        last_batch_records_ = batch_records;
        if (io_result) {
            stats_.bytes_written += batch_bytes;
            written_lsn_ = batch_last_lsn;
            durable_lsn_ = batch_last_lsn;
        } else {
            io_error_ = io_result.error();
            LOG_ERROR("WAL flush failed: {}", io_error_->ToString());
            // Records after the failure can never be made durable; drop them
            buffer_.clear();
            pending_records_ = 0;
        }
        cv_durable_.notify_all();
    }
}

void WalManager::RelaxedSyncLoop() {
    std::unique_lock<std::mutex> lock(mu_);
    while (!stop_) {
        cv_work_.wait_for(lock, kRelaxedSyncInterval, [this] { return stop_; });
        if (stop_) {
            break;
        }
        if (durable_lsn_ < written_lsn_ && !io_error_) {
            if (auto sync_result = WaitSyncedLocked(lock, written_lsn_); !sync_result) {
                LOG_ERROR("WAL background fdatasync failed: {}", sync_result.error().ToString());
            }
        }
    }
}

} // namespace database
