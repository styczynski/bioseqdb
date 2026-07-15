module;

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

export module bioseq.codegen.support:sha256;

// Self-contained SHA-256 for the content-addressed asset bundle, per-entity
// route hashes, and site manifest (§16). Kept in the isolated generator's
// support layer so it never depends on the main build's minimizer SHA-256; the
// §6 promotion (deduplicating that copy) is a separate main-build concern.
export namespace bioseq::codegen {

namespace sha256_detail {

inline constexpr std::array<std::uint32_t, 64> round_constants = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

[[nodiscard]] inline std::uint32_t rotr(std::uint32_t value, unsigned bits) {
    return (value >> bits) | (value << (32u - bits));
}

struct state {
    std::array<std::uint32_t, 8> h = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u,
                                      0xa54ff53au, 0x510e527fu, 0x9b05688cu,
                                      0x1f83d9abu, 0x5be0cd19u};

    void process(const unsigned char* block) {
        std::array<std::uint32_t, 64> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24)
                | (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16)
                | (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8)
                | static_cast<std::uint32_t>(block[i * 4 + 3]);
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 =
                rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 =
                rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        std::uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (std::size_t i = 0; i < 64; ++i) {
            const std::uint32_t big_s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t t1 = hh + big_s1 + ch + round_constants[i] + w[i];
            const std::uint32_t big_s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = big_s0 + maj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
};

}  // namespace sha256_detail

/// @brief Computes the lowercase-hex SHA-256 of a byte string.
/// @param data Input bytes.
/// @return 64 lowercase hex characters.
[[nodiscard]] inline std::string sha256_hex(std::string_view data) {
    sha256_detail::state hasher;
    const auto* bytes = reinterpret_cast<const unsigned char*>(data.data());
    const std::size_t length = data.size();
    const std::size_t full_blocks = length / 64;
    for (std::size_t block = 0; block < full_blocks; ++block) {
        hasher.process(bytes + block * 64);
    }
    std::array<unsigned char, 128> tail{};
    const std::size_t remaining = length - full_blocks * 64;
    for (std::size_t i = 0; i < remaining; ++i) {
        tail[i] = bytes[full_blocks * 64 + i];
    }
    tail[remaining] = 0x80;
    const std::size_t total_blocks = (remaining + 1 + 8 > 64) ? 2 : 1;
    const std::uint64_t bit_length = static_cast<std::uint64_t>(length) * 8;
    const std::size_t pad_length = total_blocks * 64;
    for (std::size_t i = 0; i < 8; ++i) {
        tail[pad_length - 1 - i] =
            static_cast<unsigned char>((bit_length >> (i * 8)) & 0xFFu);
    }
    hasher.process(tail.data());
    if (total_blocks == 2) {
        hasher.process(tail.data() + 64);
    }
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (const std::uint32_t word : hasher.h) {
        for (int nibble = 7; nibble >= 0; --nibble) {
            out.push_back(digits[(word >> (nibble * 4)) & 0xFu]);
        }
    }
    return out;
}

}  // namespace bioseq::codegen
