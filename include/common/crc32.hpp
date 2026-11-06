#ifndef WHISKDB_COMMON_CRC32_HPP
#define WHISKDB_COMMON_CRC32_HPP

#include <cstdint>
#include <span>

namespace database {

/**
 * @brief CRC-32/ISO-HDLC (zlib / PNG / Ethernet polynomial 0xEDB88320, reflected).
 *
 * Incremental use: `Crc32(b, Crc32(a))` equals `Crc32(a || b)`. A `seed` of 0
 * starts a fresh checksum; `Crc32("123456789") == 0xCBF43926`.
 */
uint32_t Crc32(std::span<const uint8_t> data, uint32_t seed = 0);

} // namespace database

#endif // WHISKDB_COMMON_CRC32_HPP
