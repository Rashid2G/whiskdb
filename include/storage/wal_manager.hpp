#ifndef WHISKDB_STORAGE_WAL_MANAGER_HPP
#define WHISKDB_STORAGE_WAL_MANAGER_HPP

#include "common/types.hpp"
#include "errors.hpp"
#include "storage/durability.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace database {

struct WalOptions {
    std::filesystem::path path;
    DurabilityMode mode = DurabilityMode::GroupCommit;
    idx_t group_commit_max_records = 64;
    uint32_t group_commit_interval_us = 2000;
};

/**
 * @brief Append-only write-ahead log with four durability modes.
 *
 * Thread-safe. LSNs are assigned in the order AppendNoWait/Append acquire the
 * internal lock and records reach the file in exactly that order.
 *
 * Open() does not scan or repair the file: run ReplayWal() (which truncates a
 * torn tail) before opening an existing WAL for appending.
 */
class WalManager {
private:
    struct PrivateTag {
        explicit PrivateTag() = default;
    };

public:
    struct Stats {
        uint64_t appended = 0;
        uint64_t fsync_calls = 0;
        uint64_t bytes_written = 0;
    };

    WalManager(PrivateTag, WalOptions options, lsn_t next_lsn);
    ~WalManager();
    WalManager(const WalManager&) = delete;
    WalManager& operator=(const WalManager&) = delete;

public:
    /// Opens/creates the file (append mode). `next_lsn` is the first LSN to assign.
    /// MemoryOnly: no file is created or touched, LSNs are still assigned.
    static Result<std::unique_ptr<WalManager>> Open(WalOptions options, lsn_t next_lsn = 1);

    /// AppendNoWait() followed by WaitDurable(): returns the LSN once the mode's
    /// durability guarantee holds for the record.
    Result<lsn_t> Append(WalRecordType type, std::span<const uint8_t> payload);

    /// Assigns the next LSN and hands the record to the write path without
    /// waiting for durability. Cheap (one short critical section, no fsync), so it
    /// is safe to call while holding a caller-side mutex to make LSN order equal
    /// the caller's order. Pair it with WaitDurable() outside that mutex.
    Result<lsn_t> AppendNoWait(WalRecordType type, std::span<const uint8_t> payload);

    /// Blocks until the mode's guarantee holds for every record <= lsn.
    /// Strict: fdatasync (concurrent waiters coalesce into one). GroupCommit: waits
    /// for the flush thread. Relaxed/MemoryOnly: returns immediately.
    Result<void> WaitDurable(lsn_t lsn);

    /// Forces write + fdatasync of everything pending.
    Result<void> Flush();

    /// Last assigned LSN (next_lsn-1 if none).
    lsn_t LastLsn() const;

    /// Atomically truncates the WAL to empty (after a snapshot checkpoint) and
    /// continues numbering at `next_lsn`. Pending records are flushed first; callers
    /// must not append concurrently with Reset.
    Result<void> Reset(lsn_t next_lsn);

    Stats GetStats() const;

private:
    Result<void> OpenFile();
    void StartThreads();
    void FlushLoop();
    void RelaxedSyncLoop();

    // Callers hold `lock` (on mu_). Ensures all records <= lsn are fdatasynced
    // (Strict/Relaxed write path). One caller becomes the sync leader; the rest wait.
    Result<void> WaitSyncedLocked(std::unique_lock<std::mutex>& lock, lsn_t lsn);

    // Serializes one record onto the end of buffer_. Caller holds mu_.
    void EncodeRecordLocked(lsn_t lsn, WalRecordType type, std::span<const uint8_t> payload);

private:
    WalOptions options_;
    int fd_ = -1;  // append-mode WAL file descriptor (-1 for MemoryOnly)

    mutable std::mutex mu_;
    std::condition_variable cv_work_;     // wakes the flush / sync thread
    std::condition_variable cv_durable_;  // wakes threads waiting for durability / idle I/O

    // All members below are guarded by mu_.
    lsn_t next_lsn_;
    lsn_t written_lsn_;       // highest LSN handed to write() (Strict/Relaxed) or flushed (Group)
    lsn_t durable_lsn_;       // highest LSN known fdatasynced
    std::vector<uint8_t> buffer_;     // serialization buffer / pending group-commit batch
    std::vector<uint8_t> io_buf_;     // batch currently owned by the flush thread
    idx_t pending_records_ = 0;
    idx_t last_batch_records_ = 1;    // adaptive hint: lone writers skip the batching delay
    std::chrono::steady_clock::time_point first_pending_time_;
    bool sync_active_ = false;        // a thread is inside write/fdatasync outside the lock
    bool flush_requested_ = false;
    bool stop_ = false;
    std::optional<Error> io_error_;   // sticky: once I/O fails the log accepts no more records
    Stats stats_;

    std::thread worker_;
};

} // namespace database

#endif // WHISKDB_STORAGE_WAL_MANAGER_HPP
