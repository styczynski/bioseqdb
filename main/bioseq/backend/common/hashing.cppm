/**
 * @file hashing.cppm
 * @brief Shared hashing helpers: 64-bit fnv1a for logging/caches and the
 *        canonical streaming SHA-256 used for content addresses and evidence.
 */
module;

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

export module bioseq.common.hashing;

export namespace bioseq::common {

[[nodiscard, gnu::always_inline]]
inline std::uint64_t fnv1a_64(std::string_view bytes) noexcept
{
    std::uint64_t hash = 14695981039346656037ull;
    for (const char byte : bytes) {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= 1099511628211ull;
    }
    return hash;
}

[[nodiscard, gnu::always_inline]]
inline std::uint64_t hash_combine_64(
    std::uint64_t seed,
    std::uint64_t value) noexcept
{
    return seed ^ (value + 0x9e3779b97f4a7c15ull +
                   (seed << 6) + (seed >> 2));
}

[[nodiscard, gnu::always_inline]]
inline std::uint64_t hash_combine_64(
    std::uint64_t seed,
    std::string_view value) noexcept
{
    return hash_combine_64(seed, fnv1a_64(value));
}

/// @brief 256-bit content digest produced by @ref sha256_hasher.
using sha256_digest = std::array<std::byte, 32>;

/// @brief Streaming SHA-256 (FIPS 180-4) over byte spans.
///
/// The single canonical implementation for the whole codebase: MPS spill
/// evidence, content-addressed caches, and the codegen asset bundle / site
/// manifest all hash through this class. No external crypto dependency is
/// introduced.
class sha256_hasher {
private:
    std::array<std::uint32_t, 8> state_{
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    std::array<std::byte, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t total_bytes_ = 0;

    inline static constexpr std::array<std::uint32_t, 64> round_constants{
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
        0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
        0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
        0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

    void transform_(const std::byte* block) noexcept {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16; ++index) {
            const std::size_t offset = index * 4u;
            words[index] =
                static_cast<std::uint32_t>(
                    std::to_integer<std::uint8_t>(block[offset])) << 24u
                | static_cast<std::uint32_t>(
                    std::to_integer<std::uint8_t>(block[offset + 1u])) << 16u
                | static_cast<std::uint32_t>(
                    std::to_integer<std::uint8_t>(block[offset + 2u])) << 8u
                | static_cast<std::uint32_t>(
                    std::to_integer<std::uint8_t>(block[offset + 3u]));
        }
        for (std::size_t index = 16; index < words.size(); ++index) {
            const std::uint32_t s0 = std::rotr(words[index - 15u], 7)
                ^ std::rotr(words[index - 15u], 18)
                ^ (words[index - 15u] >> 3u);
            const std::uint32_t s1 = std::rotr(words[index - 2u], 17)
                ^ std::rotr(words[index - 2u], 19)
                ^ (words[index - 2u] >> 10u);
            words[index] = words[index - 16u] + s0
                + words[index - 7u] + s1;
        }
        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        std::uint32_t f = state_[5];
        std::uint32_t g = state_[6];
        std::uint32_t h = state_[7];
        for (std::size_t index = 0; index < words.size(); ++index) {
            const std::uint32_t sum1 = std::rotr(e, 6)
                ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const std::uint32_t choose = (e & f) ^ (~e & g);
            const std::uint32_t temporary1 = h + sum1 + choose
                + round_constants[index] + words[index];
            const std::uint32_t sum0 = std::rotr(a, 2)
                ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temporary2 = sum0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

public:
    /// @brief Feeds a contiguous byte span into the running digest.
    void update(std::span<const std::byte> bytes) noexcept {
        for (const std::byte value : bytes) {
            buffer_[buffered_++] = value;
            ++total_bytes_;
            if (buffered_ == buffer_.size()) {
                transform_(buffer_.data());
                buffered_ = 0;
            }
        }
    }

    /// @brief Feeds a 64-bit value in little-endian byte order.
    void update_little_endian(std::uint64_t value) noexcept {
        std::array<std::byte, 8> bytes{};
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            bytes[index] = static_cast<std::byte>(value & 0xffu);
            value >>= 8u;
        }
        update(bytes);
    }

    /// @brief Pads and returns the final 32-byte digest.
    [[nodiscard]] sha256_digest finalize() noexcept {
        const std::uint64_t bit_length = total_bytes_ * 8u;
        buffer_[buffered_++] = std::byte{0x80};
        if (buffered_ > 56u) {
            while (buffered_ < buffer_.size()) {
                buffer_[buffered_++] = std::byte{0};
            }
            transform_(buffer_.data());
            buffered_ = 0;
        }
        while (buffered_ < 56u) {
            buffer_[buffered_++] = std::byte{0};
        }
        for (std::size_t index = 0; index < 8u; ++index) {
            buffer_[63u - index] = static_cast<std::byte>(
                (bit_length >> (index * 8u)) & 0xffu);
        }
        transform_(buffer_.data());
        sha256_digest digest{};
        for (std::size_t word = 0; word < state_.size(); ++word) {
            for (std::size_t byte = 0; byte < 4u; ++byte) {
                digest[word * 4u + byte] = static_cast<std::byte>(
                    (state_[word] >> (24u - byte * 8u)) & 0xffu);
            }
        }
        return digest;
    }
};

/// @brief Lowercase-hex (64 character) rendering of a SHA-256 digest.
[[nodiscard]] inline std::string to_hex(const sha256_digest& digest) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (const std::byte value : digest) {
        const auto byte = std::to_integer<std::uint8_t>(value);
        out.push_back(digits[(byte >> 4u) & 0xFu]);
        out.push_back(digits[byte & 0xFu]);
    }
    return out;
}

/// @brief One-shot lowercase-hex SHA-256 of a byte string.
[[nodiscard]] inline std::string sha256_hex(std::string_view data) {
    sha256_hasher hasher;
    hasher.update(std::as_bytes(std::span{data.data(), data.size()}));
    return to_hex(hasher.finalize());
}

} // namespace bioseq::common
