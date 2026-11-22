#ifndef WHISKDB_COMMON_SHA256_HPP
#define WHISKDB_COMMON_SHA256_HPP

#include <span>
#include <string>
#include <string_view>
#include <cstdint>

namespace database {

/**
 * @brief Computes standard FIPS 180-4 SHA-256 hash.
 * @return Lowercase 64-character hexadecimal string.
 */
std::string Sha256Hex(std::span<const uint8_t> data);

/**
 * @brief Computes standard FIPS 180-4 SHA-256 hash of a string view.
 * @return Lowercase 64-character hexadecimal string.
 */
std::string Sha256Hex(std::string_view text);

} // namespace database

#endif // WHISKDB_COMMON_SHA256_HPP
