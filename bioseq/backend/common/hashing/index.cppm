/**
 * @file
 * @brief Shared FNV-1a, SplitMix64, hash-combination, and SHA-256 primitives.
 */
module;

#include <algorithm>
#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

export module bioseq.common.hashing;

export namespace bioseq::common {

/// @brief Standard 64-bit FNV-1a offset basis.
inline constexpr std::uint64_t k_fnv1a_64_standard_offset_basis =
    14695981039346656037ull;

/// @brief Alternate 64-bit FNV-1a offset basis used by identities that define this exact seed.
inline constexpr std::uint64_t k_fnv1a_64_alternate_offset_basis =
    1469598103934665603ull;

/// @brief 64-bit FNV-1a prime.
inline constexpr std::uint64_t k_fnv1a_64_prime = 1099511628211ull;

/// @brief Constant used by the 64-bit hash-combination operation.
inline constexpr std::uint64_t k_hash_combine_64_constant =
    0x9e3779b97f4a7c15ull;

/// @brief State increment used by SplitMix64.
inline constexpr std::uint64_t k_splitmix64_increment =
    0x9e3779b97f4a7c15ull;

/// @brief First multiplication constant used by SplitMix64.
inline constexpr std::uint64_t k_splitmix64_first_multiplier =
    0xbf58476d1ce4e5b9ull;

/// @brief Second multiplication constant used by SplitMix64.
inline constexpr std::uint64_t k_splitmix64_second_multiplier =
    0x94d049bb133111ebull;

/**
 * @brief Extends a 64-bit FNV-1a hash with a byte span.
 *
 * @param[in] hash Initial or previously accumulated hash value.
 * @param[in] bytes Bytes appended in iteration order.
 * @return Hash after consuming every byte.
 */
[[nodiscard, gnu::always_inline]] inline std::uint64_t extend_fnv1a_64(
    std::uint64_t hash,
    std::span<const std::byte> bytes) noexcept {
    for (const std::byte byte : bytes) {
        hash ^= std::to_integer<std::uint8_t>(byte);
        hash *= k_fnv1a_64_prime;
    }
    return hash;
}

/**
 * @brief Extends a 64-bit FNV-1a hash with a character string's bytes.
 *
 * @param[in] hash Initial or previously accumulated hash value.
 * @param[in] bytes Characters appended without terminator or framing bytes.
 * @return Hash after consuming every character byte.
 */
[[nodiscard, gnu::always_inline]] inline std::uint64_t extend_fnv1a_64(
    std::uint64_t hash,
    std::string_view bytes) noexcept {
    return extend_fnv1a_64(
        hash,
        std::as_bytes(std::span{bytes.data(), bytes.size()}));
}

/**
 * @brief Computes 64-bit FNV-1a over a byte span.
 *
 * @param[in] bytes Bytes hashed in iteration order.
 * @param[in] offset_basis Initial hash value defined by the owning identity contract.
 * @return FNV-1a hash of the supplied bytes.
 */
[[nodiscard, gnu::always_inline]] inline std::uint64_t fnv1a_64(
    std::span<const std::byte> bytes,
    std::uint64_t offset_basis = k_fnv1a_64_standard_offset_basis) noexcept {
    return extend_fnv1a_64(offset_basis, bytes);
}

/**
 * @brief Computes 64-bit FNV-1a over a character string's bytes.
 *
 * @param[in] bytes Characters hashed without a terminator or framing bytes.
 * @param[in] offset_basis Initial hash value defined by the owning identity contract.
 * @return FNV-1a hash of the supplied characters.
 */
[[nodiscard, gnu::always_inline]] inline std::uint64_t fnv1a_64(
    std::string_view bytes,
    std::uint64_t offset_basis = k_fnv1a_64_standard_offset_basis) noexcept {
    return extend_fnv1a_64(offset_basis, bytes);
}

/**
 * @brief Combines a 64-bit value with an accumulated hash.
 *
 * @param[in] seed Accumulated hash value.
 * @param[in] value Value mixed into the seed.
 * @return Combined hash value.
 */
[[nodiscard, gnu::always_inline]] inline std::uint64_t hash_combine_64(
    std::uint64_t seed,
    std::uint64_t value) noexcept {
    return seed ^
           (value + k_hash_combine_64_constant + (seed << 6u) + (seed >> 2u));
}

/**
 * @brief Combines the FNV-1a hash of a string with an accumulated hash.
 *
 * @param[in] seed Accumulated hash value.
 * @param[in] value Character bytes mixed into the seed.
 * @return Combined hash value.
 */
[[nodiscard, gnu::always_inline]] inline std::uint64_t hash_combine_64(
    std::uint64_t seed,
    std::string_view value) noexcept {
    return hash_combine_64(seed, fnv1a_64(value));
}

/**
 * @brief Applies the SplitMix64 mixing permutation to one input word.
 *
 * The increment is part of this stateless operation, matching the SplitMix64
 * word mapping used by existing identity and deterministic-data producers.
 *
 * @param[in] value Input word.
 * @return Deterministically mixed 64-bit word.
 */
[[nodiscard, gnu::always_inline]] inline std::uint64_t splitmix64(
    std::uint64_t value) noexcept {
    value += k_splitmix64_increment;
    value =
        (value ^ (value >> 30u)) * k_splitmix64_first_multiplier;
    value =
        (value ^ (value >> 27u)) * k_splitmix64_second_multiplier;
    return value ^ (value >> 31u);
}

/**
 * @brief Stateful generator for the canonical SplitMix64 word sequence.
 *
 * The object owns only its current 64-bit state and is neither synchronized
 * nor shared implicitly. Unsigned wraparound is part of the sequence.
 *
 * @code{.cpp}
 * bioseq::common::splitmix64_generator generator{42u};
 * const std::uint64_t first_word = generator.next();
 * @endcode
 */
class splitmix64_generator {
private:
    std::uint64_t state_;

public:
    /**
     * @brief Initializes a deterministic SplitMix64 sequence.
     *
     * @param[in] seed State preceding the first generated word.
     */
    explicit constexpr splitmix64_generator(std::uint64_t seed) noexcept
        : state_(seed) {}

    /**
     * @brief Generates the next SplitMix64 word and advances the state.
     *
     * @return Next word in the sequence.
     */
    [[nodiscard, gnu::always_inline]] std::uint64_t next() noexcept {
        const std::uint64_t mixed_value = splitmix64(state_);
        state_ += k_splitmix64_increment;
        return mixed_value;
    }
};

/// @brief 256-bit content digest produced by @ref sha256_hasher.
using sha256_digest = std::array<std::byte, 32>;

/**
 * @brief Incrementally computes a SHA-256 digest over unframed bytes.
 *
 * The object owns its compression state and an incomplete 64-byte block. It
 * is not synchronized. Calling @ref finalize does not mutate the object, so a
 * caller may obtain an intermediate digest and continue appending bytes.
 *
 * @code{.cpp}
 * bioseq::common::sha256_hasher hasher;
 * hasher.update(std::as_bytes(std::span{"abc", 3}));
 * const bioseq::common::sha256_digest digest = hasher.finalize();
 * @endcode
 */
class sha256_hasher {
private:
    static constexpr std::size_t k_block_size = 64u;
    using block_view = std::span<const std::byte, k_block_size>;

    std::array<std::uint32_t, 8> state_{
        0x6a09e667u,
        0xbb67ae85u,
        0x3c6ef372u,
        0xa54ff53au,
        0x510e527fu,
        0x9b05688cu,
        0x1f83d9abu,
        0x5be0cd19u};
    std::array<std::byte, k_block_size> block_buffer_{};
    std::size_t buffered_byte_count_ = 0u;
    std::uint64_t total_byte_count_ = 0u;

    inline static constexpr std::array<std::uint32_t, 64> k_round_constants{
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

    /**
     * @brief Compresses one complete SHA-256 message block into the state.
     *
     * @param[in] block Exactly @ref k_block_size readable bytes.
     */
    void transform_block(block_view block) noexcept {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t word_index = 0; word_index < 16u; ++word_index) {
            const std::size_t byte_offset = word_index * 4u;
            words[word_index] =
                static_cast<std::uint32_t>(
                    std::to_integer<std::uint8_t>(block[byte_offset]))
                    << 24u |
                static_cast<std::uint32_t>(
                    std::to_integer<std::uint8_t>(block[byte_offset + 1u]))
                    << 16u |
                static_cast<std::uint32_t>(
                    std::to_integer<std::uint8_t>(block[byte_offset + 2u]))
                    << 8u |
                static_cast<std::uint32_t>(
                    std::to_integer<std::uint8_t>(block[byte_offset + 3u]));
        }

        for (std::size_t word_index = 16u; word_index < words.size(); ++word_index) {
            const std::uint32_t small_sigma_zero =
                std::rotr(words[word_index - 15u], 7) ^
                std::rotr(words[word_index - 15u], 18) ^
                (words[word_index - 15u] >> 3u);
            const std::uint32_t small_sigma_one =
                std::rotr(words[word_index - 2u], 17) ^
                std::rotr(words[word_index - 2u], 19) ^
                (words[word_index - 2u] >> 10u);
            words[word_index] = words[word_index - 16u] + small_sigma_zero +
                                words[word_index - 7u] + small_sigma_one;
        }

        std::uint32_t state_a = state_[0];
        std::uint32_t state_b = state_[1];
        std::uint32_t state_c = state_[2];
        std::uint32_t state_d = state_[3];
        std::uint32_t state_e = state_[4];
        std::uint32_t state_f = state_[5];
        std::uint32_t state_g = state_[6];
        std::uint32_t state_h = state_[7];

        for (std::size_t round_index = 0; round_index < words.size(); ++round_index) {
            const std::uint32_t big_sigma_one =
                std::rotr(state_e, 6) ^ std::rotr(state_e, 11) ^
                std::rotr(state_e, 25);
            const std::uint32_t chosen_bits =
                (state_e & state_f) ^ (~state_e & state_g);
            const std::uint32_t round_sum =
                state_h + big_sigma_one + chosen_bits +
                k_round_constants[round_index] + words[round_index];
            const std::uint32_t big_sigma_zero =
                std::rotr(state_a, 2) ^ std::rotr(state_a, 13) ^
                std::rotr(state_a, 22);
            const std::uint32_t majority_bits =
                (state_a & state_b) ^ (state_a & state_c) ^ (state_b & state_c);
            const std::uint32_t state_sum = big_sigma_zero + majority_bits;

            state_h = state_g;
            state_g = state_f;
            state_f = state_e;
            state_e = state_d + round_sum;
            state_d = state_c;
            state_c = state_b;
            state_b = state_a;
            state_a = round_sum + state_sum;
        }

        state_[0] += state_a;
        state_[1] += state_b;
        state_[2] += state_c;
        state_[3] += state_d;
        state_[4] += state_e;
        state_[5] += state_f;
        state_[6] += state_g;
        state_[7] += state_h;
    }

    /**
     * @brief Pads the accumulated message and returns its digest.
     *
     * @return Final digest for this mutable working copy.
     */
    [[nodiscard]] sha256_digest finalize_in_place() noexcept {
        const std::uint64_t bit_length = total_byte_count_ * 8u;
        block_buffer_[buffered_byte_count_++] = std::byte{0x80};

        if (buffered_byte_count_ > 56u) {
            std::fill(
                block_buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_byte_count_),
                block_buffer_.end(),
                std::byte{0});
            transform_block(block_view{block_buffer_});
            buffered_byte_count_ = 0u;
        }

        std::fill(
            block_buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_byte_count_),
            block_buffer_.begin() + 56,
            std::byte{0});
        for (std::size_t byte_index = 0; byte_index < 8u; ++byte_index) {
            block_buffer_[63u - byte_index] = static_cast<std::byte>(
                (bit_length >> (byte_index * 8u)) & 0xffu);
        }
        transform_block(block_view{block_buffer_});

        sha256_digest digest{};
        for (std::size_t word_index = 0; word_index < state_.size(); ++word_index) {
            for (std::size_t byte_index = 0; byte_index < 4u; ++byte_index) {
                digest[word_index * 4u + byte_index] = static_cast<std::byte>(
                    (state_[word_index] >> (24u - byte_index * 8u)) & 0xffu);
            }
        }
        return digest;
    }

public:
    /**
     * @brief Appends a contiguous byte span to the running digest.
     *
     * Complete 64-byte blocks are compressed directly from the caller's span.
     * Only the incomplete suffix is copied into object storage.
     *
     * @param[in] bytes Bytes appended without framing or length prefixes.
     */
    void update(std::span<const std::byte> bytes) noexcept {
        if (bytes.empty()) {
            return;
        }
        total_byte_count_ += static_cast<std::uint64_t>(bytes.size());

        if (buffered_byte_count_ != 0u) {
            const std::size_t copied_byte_count = std::min(
                k_block_size - buffered_byte_count_,
                bytes.size());
            std::memcpy(
                block_buffer_.data() + buffered_byte_count_,
                bytes.data(),
                copied_byte_count);
            buffered_byte_count_ += copied_byte_count;
            bytes = bytes.subspan(copied_byte_count);

            if (buffered_byte_count_ == k_block_size) {
                transform_block(block_view{block_buffer_});
                buffered_byte_count_ = 0u;
            }
        }

        while (bytes.size() >= k_block_size) {
            transform_block(bytes.first<k_block_size>());
            bytes = bytes.subspan(k_block_size);
        }

        if (!bytes.empty()) {
            std::memcpy(block_buffer_.data(), bytes.data(), bytes.size());
            buffered_byte_count_ = bytes.size();
        }
    }

    /**
     * @brief Appends an unsigned integer in little-endian byte order.
     *
     * @tparam p_unsigned_integer Unsigned integer type whose full object width is encoded.
     * @param[in] value Value encoded as exactly `sizeof(p_unsigned_integer)` bytes.
     */
    template <std::unsigned_integral p_unsigned_integer>
        requires(!std::same_as<p_unsigned_integer, bool>)
    void update_little_endian(p_unsigned_integer value) noexcept {
        std::array<std::byte, sizeof(p_unsigned_integer)> bytes{};
        for (std::size_t byte_index = 0; byte_index < bytes.size(); ++byte_index) {
            bytes[byte_index] = static_cast<std::byte>(
                value & static_cast<p_unsigned_integer>(0xffu));
            value >>= 8u;
        }
        update(bytes);
    }

    /**
     * @brief Returns the digest of all bytes appended so far.
     *
     * @return Final 32-byte SHA-256 digest without mutating this hasher.
     */
    [[nodiscard]] sha256_digest finalize() const noexcept {
        sha256_hasher finalized_hasher = *this;
        return finalized_hasher.finalize_in_place();
    }
};

/**
 * @brief Renders a SHA-256 digest as 64 lowercase hexadecimal characters.
 *
 * @param[in] digest Binary digest to render.
 * @return Lowercase hexadecimal representation.
 */
[[nodiscard]] inline std::string to_hex(const sha256_digest& digest) {
    static constexpr std::array k_hex_digits{
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string hexadecimal_digest;
    hexadecimal_digest.reserve(digest.size() * 2u);
    for (const std::byte value : digest) {
        const std::uint8_t byte = std::to_integer<std::uint8_t>(value);
        hexadecimal_digest.push_back(k_hex_digits[(byte >> 4u) & 0x0fu]);
        hexadecimal_digest.push_back(k_hex_digits[byte & 0x0fu]);
    }
    return hexadecimal_digest;
}

/**
 * @brief Computes lowercase hexadecimal SHA-256 for a character string's bytes.
 *
 * @param[in] bytes Characters hashed without a terminator or framing bytes.
 * @return Lowercase 64-character SHA-256 digest.
 */
[[nodiscard]] inline std::string sha256_hex(std::string_view bytes) {
    sha256_hasher hasher{};
    hasher.update(std::as_bytes(std::span{bytes.data(), bytes.size()}));
    return to_hex(hasher.finalize());
}

} // namespace bioseq::common
