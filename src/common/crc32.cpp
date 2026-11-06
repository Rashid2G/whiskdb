#include "common/crc32.hpp"
#include "common/types.hpp"

#include <array>

namespace database {

namespace {

constexpr uint32_t kCrcPolynomial = 0xEDB88320U;
constexpr idx_t kSliceCount = 8;
constexpr idx_t kTableSize = 256;

using CrcTables = std::array<std::array<uint32_t, kTableSize>, kSliceCount>;

constexpr CrcTables BuildTables() {
    CrcTables tables{};
    for (idx_t byte_idx = 0; byte_idx < kTableSize; byte_idx++) {
        uint32_t value = static_cast<uint32_t>(byte_idx);
        for (idx_t bit_idx = 0; bit_idx < 8; bit_idx++) {
            value = (value & 1U) != 0 ? (value >> 1) ^ kCrcPolynomial : (value >> 1);
        }
        tables[0][byte_idx] = value;
    }
    for (idx_t slice_idx = 1; slice_idx < kSliceCount; slice_idx++) {
        for (idx_t byte_idx = 0; byte_idx < kTableSize; byte_idx++) {
            uint32_t prev = tables[slice_idx - 1][byte_idx];
            tables[slice_idx][byte_idx] = (prev >> 8) ^ tables[0][prev & 0xFFU];
        }
    }
    return tables;
}

constexpr CrcTables kTables = BuildTables();

inline uint32_t LoadLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

} // namespace

uint32_t Crc32(std::span<const uint8_t> data, uint32_t seed) {
    uint32_t crc = ~seed;
    const uint8_t* p = data.data();
    idx_t remaining = data.size();

    // Slicing-by-8: consume 8 bytes per iteration with 8 independent table lookups
    while (remaining >= 8) {
        uint32_t lo = LoadLe32(p) ^ crc;
        uint32_t hi = LoadLe32(p + 4);
        crc = kTables[7][lo & 0xFFU] ^ kTables[6][(lo >> 8) & 0xFFU] ^
              kTables[5][(lo >> 16) & 0xFFU] ^ kTables[4][lo >> 24] ^
              kTables[3][hi & 0xFFU] ^ kTables[2][(hi >> 8) & 0xFFU] ^
              kTables[1][(hi >> 16) & 0xFFU] ^ kTables[0][hi >> 24];
        p += 8;
        remaining -= 8;
    }
    while (remaining > 0) {
        crc = (crc >> 8) ^ kTables[0][(crc ^ *p) & 0xFFU];
        p++;
        remaining--;
    }
    return ~crc;
}

} // namespace database
