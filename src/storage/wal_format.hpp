#ifndef WHISKDB_STORAGE_WAL_FORMAT_HPP
#define WHISKDB_STORAGE_WAL_FORMAT_HPP

// Internal (src/storage only): on-disk WAL layout shared by writer and reader.
//
// File:   [magic "WHSKWAL1" 8][version u32][reserved u32]  (16 bytes) then records.
// Record: [LSN u64][timestamp_us u64][type u8][payload_len u32][payload][CRC32 u32]
//         all integers little-endian, CRC32 covers every byte before it.

#include "common/types.hpp"

#include <array>
#include <cstdint>

namespace database::wal_format {

inline constexpr std::array<uint8_t, 8> kMagic = {'W', 'H', 'S', 'K', 'W', 'A', 'L', '1'};
inline constexpr uint32_t kVersion = 1;
inline constexpr idx_t kFileHeaderSize = 16;
inline constexpr idx_t kRecordHeaderSize = 8 + 8 + 1 + 4;
inline constexpr idx_t kRecordTrailerSize = 4;
inline constexpr idx_t kRecordOverhead = kRecordHeaderSize + kRecordTrailerSize;
inline constexpr idx_t kMaxPayloadSize = 256ULL * 1024 * 1024;

inline void PutU32(uint8_t* dst, uint32_t v) {
    for (idx_t i = 0; i < 4; i++) {
        dst[i] = static_cast<uint8_t>(v >> (8 * i));
    }
}

inline void PutU64(uint8_t* dst, uint64_t v) {
    for (idx_t i = 0; i < 8; i++) {
        dst[i] = static_cast<uint8_t>(v >> (8 * i));
    }
}

inline uint32_t GetU32(const uint8_t* src) {
    uint32_t v = 0;
    for (idx_t i = 0; i < 4; i++) {
        v |= static_cast<uint32_t>(src[i]) << (8 * i);
    }
    return v;
}

inline uint64_t GetU64(const uint8_t* src) {
    uint64_t v = 0;
    for (idx_t i = 0; i < 8; i++) {
        v |= static_cast<uint64_t>(src[i]) << (8 * i);
    }
    return v;
}

/// Builds the canonical 16-byte file header.
inline std::array<uint8_t, kFileHeaderSize> MakeFileHeader() {
    std::array<uint8_t, kFileHeaderSize> header{};
    for (idx_t i = 0; i < kMagic.size(); i++) {
        header[i] = kMagic[i];
    }
    PutU32(header.data() + 8, kVersion);
    PutU32(header.data() + 12, 0);
    return header;
}

} // namespace database::wal_format

#endif // WHISKDB_STORAGE_WAL_FORMAT_HPP
