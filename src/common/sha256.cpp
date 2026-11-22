#include "common/sha256.hpp"
#include "common/types.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace database {

namespace {

constexpr std::array<uint32_t, 64> K = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

inline constexpr uint32_t Ch(uint32_t x, uint32_t y, uint32_t z) noexcept {
    return (x & y) ^ (~x & z);
}

inline constexpr uint32_t Maj(uint32_t x, uint32_t y, uint32_t z) noexcept {
    return (x & y) ^ (x & z) ^ (y & z);
}

inline constexpr uint32_t BigSigma0(uint32_t x) noexcept {
    return std::rotr(x, 2) ^ std::rotr(x, 13) ^ std::rotr(x, 22);
}

inline constexpr uint32_t BigSigma1(uint32_t x) noexcept {
    return std::rotr(x, 6) ^ std::rotr(x, 11) ^ std::rotr(x, 25);
}

inline constexpr uint32_t SmallSigma0(uint32_t x) noexcept {
    return std::rotr(x, 7) ^ std::rotr(x, 18) ^ (x >> 3);
}

inline constexpr uint32_t SmallSigma1(uint32_t x) noexcept {
    return std::rotr(x, 17) ^ std::rotr(x, 19) ^ (x >> 10);
}

class Sha256Context {
public:
    Sha256Context() noexcept {
        Reset();
    }

    void Reset() noexcept {
        h_[0] = 0x6a09e667U;
        h_[1] = 0xbb67ae85U;
        h_[2] = 0x3c6ef372U;
        h_[3] = 0xa54ff53aU;
        h_[4] = 0x510e527fU;
        h_[5] = 0x9b05688cU;
        h_[6] = 0x1f83d9abU;
        h_[7] = 0x5be0cd19U;
        total_len_ = 0;
        buffer_len_ = 0;
    }

    void Update(std::span<const uint8_t> data) noexcept {
        idx_t offset = 0;
        total_len_ += data.size();

        if (buffer_len_ > 0) {
            idx_t to_copy = std::min<idx_t>(64 - buffer_len_, data.size());
            for (idx_t i = 0; i < to_copy; ++i) {
                buffer_[buffer_len_ + i] = data[i];
            }
            buffer_len_ += to_copy;
            offset += to_copy;
            if (buffer_len_ == 64) {
                ProcessBlock(buffer_.data());
                buffer_len_ = 0;
            }
        }

        while (offset + 64 <= data.size()) {
            ProcessBlock(data.data() + offset);
            offset += 64;
        }

        while (offset < data.size()) {
            buffer_[buffer_len_++] = data[offset++];
        }
    }

    std::array<uint8_t, 32> Finalize() noexcept {
        uint64_t total_bits = total_len_ * 8ULL;

        // Append 0x80 byte
        buffer_[buffer_len_++] = 0x80U;

        if (buffer_len_ > 56) {
            while (buffer_len_ < 64) {
                buffer_[buffer_len_++] = 0x00U;
            }
            ProcessBlock(buffer_.data());
            buffer_len_ = 0;
        }

        while (buffer_len_ < 56) {
            buffer_[buffer_len_++] = 0x00U;
        }

        for (int i = 7; i >= 0; --i) {
            buffer_[buffer_len_++] = static_cast<uint8_t>((total_bits >> (i * 8)) & 0xFFU);
        }

        ProcessBlock(buffer_.data());

        std::array<uint8_t, 32> digest{};
        for (idx_t i = 0; i < 8; ++i) {
            digest[i * 4 + 0] = static_cast<uint8_t>((h_[i] >> 24) & 0xFFU);
            digest[i * 4 + 1] = static_cast<uint8_t>((h_[i] >> 16) & 0xFFU);
            digest[i * 4 + 2] = static_cast<uint8_t>((h_[i] >> 8) & 0xFFU);
            digest[i * 4 + 3] = static_cast<uint8_t>(h_[i] & 0xFFU);
        }
        return digest;
    }

private:
    void ProcessBlock(const uint8_t* block) noexcept {
        std::array<uint32_t, 64> w{};
        for (idx_t t = 0; t < 16; ++t) {
            w[t] = (static_cast<uint32_t>(block[t * 4 + 0]) << 24) |
                   (static_cast<uint32_t>(block[t * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(block[t * 4 + 2]) << 8) |
                   (static_cast<uint32_t>(block[t * 4 + 3]));
        }
        for (idx_t t = 16; t < 64; ++t) {
            w[t] = SmallSigma1(w[t - 2]) + w[t - 7] + SmallSigma0(w[t - 15]) + w[t - 16];
        }

        uint32_t a = h_[0];
        uint32_t b = h_[1];
        uint32_t c = h_[2];
        uint32_t d = h_[3];
        uint32_t e = h_[4];
        uint32_t f = h_[5];
        uint32_t g = h_[6];
        uint32_t h = h_[7];

        for (idx_t t = 0; t < 64; ++t) {
            uint32_t t1 = h + BigSigma1(e) + Ch(e, f, g) + K[t] + w[t];
            uint32_t t2 = BigSigma0(a) + Maj(a, b, c);
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }

        h_[0] += a;
        h_[1] += b;
        h_[2] += c;
        h_[3] += d;
        h_[4] += e;
        h_[5] += f;
        h_[6] += g;
        h_[7] += h;
    }

    std::array<uint32_t, 8> h_{};
    std::array<uint8_t, 64> buffer_{};
    idx_t total_len_{0};
    idx_t buffer_len_{0};
};

} // namespace

std::string Sha256Hex(std::span<const uint8_t> data) {
    Sha256Context ctx;
    ctx.Update(data);
    auto digest = ctx.Finalize();

    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string hex;
    hex.resize(64);
    for (idx_t i = 0; i < 32; ++i) {
        hex[i * 2 + 0] = kHexDigits[(digest[i] >> 4) & 0x0FU];
        hex[i * 2 + 1] = kHexDigits[digest[i] & 0x0FU];
    }
    return hex;
}

std::string Sha256Hex(std::string_view text) {
    return Sha256Hex(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}

} // namespace database
