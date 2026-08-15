/**
 * @file hash_major_minimizer_index/codec.cppm
 * @brief Hash-major wire types, digests, and byte-exact codecs.
 */
module;

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

export module bioseq.indexing.hash_major_minimizer_index:codec;

import bioseq.common.checked_arithmetic;
import bioseq.common.hashing;
import bioseq.indexing.storage;

namespace bioseq::indexing::hash_major_minimizer_detail {

template <class p_unsigned_integer>
requires std::is_unsigned_v<p_unsigned_integer>
constexpr void write_le(
        std::span<std::byte> output,
        std::size_t offset,
        p_unsigned_integer value) noexcept {
    if consteval {
        for (std::size_t byte = 0; byte < sizeof(p_unsigned_integer); ++byte) {
            output[offset + byte] =
                static_cast<std::byte>(value & p_unsigned_integer{0xff});
            value >>= 8u;
        }
    } else {
        if constexpr (std::endian::native == std::endian::big) {
            value = std::byteswap(value);
        }
        std::memcpy(output.data() + offset, &value, sizeof(value));
    }
}

template <class p_unsigned_integer>
requires std::is_unsigned_v<p_unsigned_integer>
[[nodiscard]] constexpr p_unsigned_integer read_le(
        std::span<const std::byte> input,
        std::size_t offset) noexcept {
    if consteval {
        p_unsigned_integer value = 0;
        for (std::size_t byte = 0; byte < sizeof(p_unsigned_integer); ++byte) {
            value |= static_cast<p_unsigned_integer>(
                std::to_integer<std::uint8_t>(input[offset + byte]))
                << (byte * 8u);
        }
        return value;
    } else {
        p_unsigned_integer value = 0;
        std::memcpy(&value, input.data() + offset, sizeof(value));
        if constexpr (std::endian::native == std::endian::big) {
            value = std::byteswap(value);
        }
        return value;
    }
}

template <std::size_t p_byte_count>
constexpr void write_bytes(
        std::span<std::byte> output,
        std::size_t offset,
        const std::array<std::byte, p_byte_count>& bytes) noexcept {
    for (std::size_t index = 0; index < p_byte_count; ++index) {
        output[offset + index] = bytes[index];
    }
}

template <std::size_t p_byte_count>
[[nodiscard]] constexpr std::array<std::byte, p_byte_count> read_bytes(
        std::span<const std::byte> input,
        std::size_t offset) noexcept {
    std::array<std::byte, p_byte_count> bytes{};
    for (std::size_t index = 0; index < p_byte_count; ++index) {
        bytes[index] = input[offset + index];
    }
    return bytes;
}

[[nodiscard]] constexpr bool all_zero(
        std::span<const std::byte> bytes) noexcept {
    for (const std::byte byte : bytes) {
        if (byte != std::byte{0}) return false;
    }
    return true;
}

[[nodiscard]] constexpr bool checked_add(
        std::uint64_t left,
        std::uint64_t right,
        std::uint64_t& output) noexcept {
    const auto sum = bioseq::common::checked_add(left, right);
    if (!sum) {
        return false;
    }
    output = *sum;
    return true;
}

[[nodiscard]] constexpr bool checked_multiply(
        std::uint64_t left,
        std::uint64_t right,
        std::uint64_t& output) noexcept {
    const auto product = bioseq::common::checked_mul(left, right);
    if (!product) {
        return false;
    }
    output = *product;
    return true;
}

[[nodiscard]] constexpr std::uint64_t ceil_div(
        std::uint64_t value,
        std::uint64_t divisor) noexcept {
    return value == 0 ? 0 : 1u + (value - 1u) / divisor;
}

inline constexpr std::array<std::byte, 8> head_magic{
    std::byte{'B'}, std::byte{'S'}, std::byte{'E'}, std::byte{'Q'},
    std::byte{'G'}, std::byte{'H'}, std::byte{'6'}, std::byte{0}};
inline constexpr std::array<std::byte, 8> manifest_magic{
    std::byte{'B'}, std::byte{'S'}, std::byte{'E'}, std::byte{'Q'},
    std::byte{'N'}, std::byte{'L'}, std::byte{'6'}, std::byte{0}};
inline constexpr std::array<std::byte, 18> source_domain{
    std::byte{'B'}, std::byte{'S'}, std::byte{'E'}, std::byte{'Q'},
    std::byte{'-'}, std::byte{'F'}, std::byte{'6'}, std::byte{'-'},
    std::byte{'S'}, std::byte{'O'}, std::byte{'U'}, std::byte{'R'},
    std::byte{'C'}, std::byte{'E'}, std::byte{'-'}, std::byte{'v'},
    std::byte{'1'}, std::byte{0}};
inline constexpr std::array<std::byte, 18> storage_profile_domain{
    std::byte{'B'}, std::byte{'S'}, std::byte{'E'}, std::byte{'Q'},
    std::byte{'-'}, std::byte{'F'}, std::byte{'6'}, std::byte{'-'},
    std::byte{'C'}, std::byte{'O'}, std::byte{'M'}, std::byte{'P'},
    std::byte{'A'}, std::byte{'T'}, std::byte{'-'}, std::byte{'v'},
    std::byte{'1'}, std::byte{0}};

} // namespace bioseq::indexing::hash_major_minimizer_detail

export namespace bioseq::indexing {

/// Current serialized hash-major layout revision.
inline constexpr std::uint16_t k_hash_major_minimizer_layout_revision = 6u;
/// Encoded publication-head size in bytes.
inline constexpr std::size_t k_hash_major_minimizer_head_bytes = 224u;
/// Fixed manifest prefix size in bytes.
inline constexpr std::size_t k_hash_major_minimizer_manifest_header_bytes = 192u;
/// Encoded logical-stream descriptor size in bytes.
inline constexpr std::size_t k_hash_major_minimizer_stream_descriptor_bytes = 48u;
/// Complete encoded manifest size in bytes.
inline constexpr std::size_t k_hash_major_minimizer_manifest_bytes = 336u;
/// Encoded directory-entry size in bytes.
inline constexpr std::size_t k_hash_major_minimizer_directory_entry_bytes = 80u;
/// Fixed control-block prefix size in bytes.
inline constexpr std::size_t k_hash_major_minimizer_control_header_bytes = 64u;
/// Maximum controls carried by one independently authenticated block.
inline constexpr std::uint32_t k_hash_major_minimizer_controls_per_block = 4096u;
/// Payload capacity of one storage row in bytes.
inline constexpr std::size_t k_hash_major_minimizer_row_payload_bytes = 8064u;
/// Maximum contiguous read slab requested by the selective reader.
inline constexpr std::size_t k_hash_major_minimizer_read_slab_bytes = 8u * 1024u * 1024u;
inline constexpr std::size_t k_hash_major_minimizer_posting_request_wave_hashes =
    1'048'576u;
inline constexpr std::size_t k_hash_major_minimizer_control_read_wave_blocks = 1'024u;
inline constexpr std::size_t k_hash_major_minimizer_maximum_control_block_bytes =
    k_hash_major_minimizer_control_header_bytes
    + k_hash_major_minimizer_controls_per_block * 4u
    + k_hash_major_minimizer_controls_per_block * 2u
    + k_hash_major_minimizer_controls_per_block * 12u;

/** @brief Closed diagnostics for hash-major encoding, opening, and lookup. */
enum class hash_major_minimizer_error : std::uint8_t {
    encoded_size,
    magic,
    layout_revision,
    declared_size,
    flags,
    reserved_nonzero,
    checksum,
    expected_generation,
    source_identity,
    storage_contract_mismatch,
    stream_descriptor,
    directory,
    control,
    posting,
    order,
    count,
    target_range,
    arithmetic_overflow,
    source_read,
    sink_write,
    admission,
    state,
};

/** @brief Independently addressed and checksummed logical byte streams. */
enum class hash_major_minimizer_stream_kind : std::uint16_t {
    directory = 1u,
    controls = 2u,
    postings = 3u,
};

/** @brief Hash, alphabet, position, and producer identity bound into an image. */
struct hash_major_minimizer_storage_profile {
    std::uint32_t hash_scheme_identity = 0;
    std::uint16_t hash_scheme_revision = 0;
    std::uint16_t alphabet_code = 0;
    std::uint16_t kmer_size = 15;
    std::uint16_t window_size = 10;
    std::uint8_t hash_width_bytes = 4;
    std::uint8_t stored_posting_width_bytes = 4;
    std::uint16_t position_encoding_revision = 1;
    std::uint16_t hash_semantics_revision = 1;
    std::uint16_t count_semantics_revision = 1;
    std::uint16_t producer_backend_identity = 1;
    std::uint32_t producer_backend_revision = 1;
    std::uint32_t controls_per_block = k_hash_major_minimizer_controls_per_block;
    std::uint64_t hash_seed = 0;

    [[nodiscard]] bool operator==(
        const hash_major_minimizer_storage_profile&) const = default;
};

/** @brief Immutable source and storage-profile inputs for one generation. */
struct hash_major_minimizer_generation_options {
    std::uint64_t generation_id = 0;
    std::uint64_t query_key = 0;
    std::uint64_t source_epoch = 0;
    std::uint64_t source_content_crc64 = 0;
    std::uint64_t target_length = 0;
    hash_major_minimizer_storage_profile storage_profile{};
};

/** @brief Optional authenticated dense-heap publication identity. */
struct hash_major_minimizer_dense_seal {
    std::int64_t sub_key = 0;
    std::int64_t position_segment = 0;
    std::uint32_t header_chunk_no = 0;
    std::uint16_t abi = 0;
    std::uint16_t storage_mode = 0;
    std::uint64_t body_chunk_count = 0;
    std::array<std::byte, 16> incarnation{};
    std::uint64_t crc64 = 0;

    [[nodiscard]] bool operator==(const hash_major_minimizer_dense_seal&) const = default;
};

/** @brief Small atomically published head selecting one complete generation. */
struct hash_major_minimizer_head {
    std::uint64_t generation_id = 0;
    std::uint64_t query_key = 0;
    std::uint64_t source_epoch = 0;
    std::array<std::byte, 16> source_identity{};
    std::uint64_t source_content_crc64 = 0;
    std::array<std::byte, 32> storage_profile_digest{};
    std::uint64_t manifest_address_bound_row_crc64 = 0;
    std::uint64_t manifest_payload_crc64 = 0;
    std::uint64_t head_crc64 = 0;
    std::optional<hash_major_minimizer_dense_seal> dense_seal{};

    [[nodiscard]] bool operator==(const hash_major_minimizer_head&) const = default;
};

/** @brief Size, row, element, and checksum metadata for one logical stream. */
struct hash_major_minimizer_stream_descriptor {
    hash_major_minimizer_stream_kind kind = hash_major_minimizer_stream_kind::directory;
    std::uint16_t encoding_revision = 1;
    std::uint8_t element_width = 0;
    std::uint64_t element_count = 0;
    std::uint64_t logical_bytes = 0;
    std::uint64_t row_count = 0;
    std::uint64_t stream_crc64 = 0;
    std::uint64_t first_row_ordinal = 0;

    [[nodiscard]] bool operator==(
        const hash_major_minimizer_stream_descriptor&) const = default;
};

/** @brief Authenticated generation manifest shared by writers and cold readers. */
struct hash_major_minimizer_manifest {
    std::uint64_t generation_id = 0;
    std::uint64_t source_epoch = 0;
    std::array<std::byte, 16> source_identity{};
    std::uint64_t source_content_crc64 = 0;
    std::uint64_t target_length = 0;
    std::uint64_t unique_hash_count = 0;
    std::uint64_t total_posting_count = 0;
    hash_major_minimizer_storage_profile storage_profile{};
    std::array<std::byte, 32> storage_profile_digest{};
    std::uint64_t authenticated_directory_crc64 = 0;
    std::uint64_t generation_crc64 = 0;
    std::uint64_t total_control_bytes = 0;
    std::uint64_t total_posting_bytes = 0;
    std::uint64_t directory_entry_count = 0;
    std::array<hash_major_minimizer_stream_descriptor, 3> streams{};

    [[nodiscard]] bool operator==(
        const hash_major_minimizer_manifest&) const = default;
};

/** @brief Directory record mapping a hash interval to control and posting bytes. */
struct hash_major_minimizer_directory_entry {
    std::uint64_t first_hash = 0;
    std::uint64_t last_hash = 0;
    std::uint64_t control_block_ordinal = 0;
    std::uint64_t control_byte_offset = 0;
    std::uint32_t control_byte_length = 0;
    std::uint32_t control_count = 0;
    std::uint64_t first_posting_ordinal = 0;
    std::uint64_t posting_byte_offset = 0;
    std::uint64_t posting_byte_length = 0;
    std::uint64_t exact_posting_count = 0;
    std::uint64_t block_crc64 = 0;

    [[nodiscard]] bool operator==(
        const hash_major_minimizer_directory_entry&) const = default;
};

/** @brief Decoded fixed prefix of one compressed hash-control block. */
struct hash_major_minimizer_control_header {
    std::uint64_t first_hash = 0;
    std::uint32_t control_count = 0;
    std::uint8_t hash_delta_bit_width = 0;
    std::uint8_t saturated_count_bit_width = 16;
    std::uint64_t exact_posting_count = 0;
    std::uint64_t posting_byte_offset = 0;
    std::uint32_t hash_bytes = 0;
    std::uint32_t count_bytes = 0;
    std::uint32_t exception_bytes = 0;
    std::uint64_t control_crc64 = 0;
    std::uint64_t posting_crc64 = 0;

    [[nodiscard]] bool operator==(
        const hash_major_minimizer_control_header&) const = default;
};

/** @brief Owning decoded control block for validation and non-hot consumers. */
struct decoded_hash_major_minimizer_control_block {
    hash_major_minimizer_control_header header{};
    std::vector<std::uint64_t> hashes{};
    std::vector<std::uint64_t> exact_counts{};
};

/** @brief Reusable bounded storage for allocation-free hot-path decoding. */
struct hash_major_minimizer_control_scratch {
    std::array<std::byte, k_hash_major_minimizer_maximum_control_block_bytes>
        encoded{};
    std::array<std::uint64_t, k_hash_major_minimizer_controls_per_block> hashes{};
    std::array<std::uint64_t, k_hash_major_minimizer_controls_per_block>
        exact_counts{};
};

/** @brief Borrowed view into one decoded control block scratch allocation. */
struct decoded_hash_major_minimizer_control_block_view {
    hash_major_minimizer_control_header header{};
    std::span<const std::uint64_t> hashes{};
    std::span<const std::uint64_t> exact_counts{};
};

using encoded_hash_major_minimizer_head =
    std::array<std::byte, k_hash_major_minimizer_head_bytes>;
using encoded_hash_major_minimizer_manifest =
    std::array<std::byte, k_hash_major_minimizer_manifest_bytes>;
using encoded_hash_major_minimizer_directory_entry =
    std::array<std::byte, k_hash_major_minimizer_directory_entry_bytes>;

/** @brief Derives the stable 128-bit source identity from publication inputs. */
[[nodiscard]] inline std::array<std::byte, 16>
hash_major_minimizer_source_identity(
        std::uint64_t query_key,
        std::uint64_t source_epoch,
        std::uint64_t source_content_crc64,
        std::uint64_t target_length) noexcept {
    bioseq::common::sha256_hasher sha;
    sha.update(hash_major_minimizer_detail::source_domain);
    sha.update_little_endian(query_key);
    sha.update_little_endian(source_epoch);
    sha.update_little_endian(source_content_crc64);
    sha.update_little_endian(target_length);
    const auto full = sha.finalize();
    std::array<std::byte, 16> identity{};
    std::copy_n(full.begin(), identity.size(), identity.begin());
    return identity;
}

/** @brief Hashes storage-profile fields with their original serialized widths. */
[[nodiscard]] inline std::array<std::byte, 32>
hash_major_minimizer_storage_profile_digest(
        const hash_major_minimizer_storage_profile& profile) noexcept {
    bioseq::common::sha256_hasher sha;
    sha.update(hash_major_minimizer_detail::storage_profile_domain);
    sha.update_little_endian(k_hash_major_minimizer_layout_revision);
    sha.update_little_endian(profile.hash_scheme_identity);
    sha.update_little_endian(profile.hash_scheme_revision);
    sha.update_little_endian(profile.alphabet_code);
    sha.update_little_endian(profile.kmer_size);
    sha.update_little_endian(profile.window_size);
    sha.update_little_endian(profile.hash_width_bytes);
    sha.update_little_endian(profile.stored_posting_width_bytes);
    sha.update_little_endian(profile.position_encoding_revision);
    sha.update_little_endian(profile.hash_semantics_revision);
    sha.update_little_endian(profile.count_semantics_revision);
    sha.update_little_endian(profile.producer_backend_identity);
    sha.update_little_endian(profile.producer_backend_revision);
    sha.update_little_endian(profile.controls_per_block);
    sha.update_little_endian(profile.hash_seed);
    return sha.finalize();
}

/** @brief Selects four-byte postings when every encoded position fits. */
[[nodiscard]] constexpr bool hash_major_minimizer_uses_u32_postings(
        std::uint64_t target_length) noexcept {
    return target_length <= (std::uint64_t{1} << 31u);
}

/** @brief Validates an authenticated storage profile for a target length. */
[[nodiscard]] constexpr bool is_valid_hash_major_minimizer_storage_profile(
        const hash_major_minimizer_storage_profile& value,
        std::uint64_t target_length) noexcept {
    if (target_length > (std::uint64_t{1} << 63u)
        || value.alphabet_code != 0u || value.kmer_size != 15u
        || value.window_size != 10u || value.hash_width_bytes != 4u
        || (value.stored_posting_width_bytes != 4u
            && value.stored_posting_width_bytes != 8u)
        || value.stored_posting_width_bytes
            != (hash_major_minimizer_uses_u32_postings(target_length) ? 4u : 8u)
        || value.position_encoding_revision != 1u
        || value.hash_semantics_revision != 1u
        || value.count_semantics_revision != 1u
        || value.controls_per_block != k_hash_major_minimizer_controls_per_block
        || value.hash_seed != 0u) {
        return false;
    }
    const bool original_scheme = value.hash_scheme_identity == 0u
        && value.hash_scheme_revision == 0u
        && ((value.producer_backend_identity == 1u
                && value.producer_backend_revision == 1u)
            || (value.producer_backend_identity == 2u
                && value.producer_backend_revision == 1u)
            || (value.producer_backend_identity == 4u
                && value.producer_backend_revision == 1u));
    const bool bounded_scheme = value.hash_scheme_identity == 1u
        && value.hash_scheme_revision == 1u
        && value.producer_backend_identity == 3u
        && value.producer_backend_revision == 1u;
    return original_scheme || bounded_scheme;
}

/** @brief Returns the maximum hash representable by the selected hash scheme. */
[[nodiscard]] constexpr std::uint64_t hash_major_minimizer_max_hash(
        const hash_major_minimizer_storage_profile& profile) noexcept {
    return profile.hash_scheme_identity == 0u
            && profile.hash_scheme_revision == 0u
        ? 0x3fffffffu : 0xffffffffu;
}

} // namespace bioseq::indexing

namespace bioseq::indexing::hash_major_minimizer_detail {

[[nodiscard]] constexpr std::uint8_t bit_width_u64(
        std::uint64_t value) noexcept {
    return value == 0u
        ? 0u
        : static_cast<std::uint8_t>(64u - std::countl_zero(value));
}

constexpr void store_packed_lsb(
        std::span<std::byte> bytes,
        std::uint64_t bit_offset,
        std::uint8_t width,
        std::uint64_t value) noexcept {
    for (std::uint8_t bit = 0; bit < width; ++bit) {
        if (((value >> bit) & 1u) == 0u) continue;
        const std::uint64_t absolute = bit_offset + bit;
        bytes[static_cast<std::size_t>(absolute / 8u)] |=
            static_cast<std::byte>(1u << (absolute % 8u));
    }
}

[[nodiscard]] constexpr std::uint64_t load_packed_lsb_wordwise(
        std::span<const std::byte> bytes,
        std::uint64_t bit_offset,
        std::uint8_t width) noexcept {
    if (width == 0u) return 0u;
    const std::size_t byte_offset = static_cast<std::size_t>(bit_offset / 8u);
    const std::uint8_t shift = static_cast<std::uint8_t>(bit_offset % 8u);
    std::uint64_t low = 0u;
    const std::size_t low_bytes = std::min<std::size_t>(
        8u, bytes.size() - byte_offset);
    for (std::size_t index = 0u; index < low_bytes; ++index) {
        low |= static_cast<std::uint64_t>(
            std::to_integer<std::uint8_t>(bytes[byte_offset + index]))
            << (index * 8u);
    }
    std::uint64_t value = low >> shift;
    if (static_cast<unsigned>(shift) + width > 64u) {
        std::uint64_t high = 0u;
        const std::size_t high_offset = byte_offset + 8u;
        const std::size_t high_bytes = high_offset < bytes.size()
            ? std::min<std::size_t>(8u, bytes.size() - high_offset)
            : 0u;
        for (std::size_t index = 0u; index < high_bytes; ++index) {
            high |= static_cast<std::uint64_t>(
                std::to_integer<std::uint8_t>(bytes[high_offset + index]))
                << (index * 8u);
        }
        value |= high << (64u - shift);
    }
    return width == 64u
        ? value : value & ((std::uint64_t{1} << width) - 1u);
}

} // namespace bioseq::indexing::hash_major_minimizer_detail

export namespace bioseq::indexing {

/** @brief Encodes a compressed control block without changing posting order. */
[[nodiscard]] inline std::expected<
    std::vector<std::byte>, hash_major_minimizer_error>
encode_hash_major_minimizer_control_block(
        std::span<const std::uint64_t> hashes,
        std::span<const std::uint64_t> exact_counts,
        std::uint64_t posting_byte_offset,
        std::uint64_t posting_crc64,
        std::uint8_t hash_width_bytes,
        std::uint8_t stored_posting_width_bytes) {
    using namespace hash_major_minimizer_detail;
    if (hashes.empty() || hashes.size() != exact_counts.size()
        || hashes.size() > k_hash_major_minimizer_controls_per_block
        || (hash_width_bytes != 4u && hash_width_bytes != 8u)
        || (stored_posting_width_bytes != 4u
            && stored_posting_width_bytes != 8u)
        || posting_byte_offset % stored_posting_width_bytes != 0u) {
        return std::unexpected(hash_major_minimizer_error::control);
    }
    std::uint64_t maximum_delta = 0u;
    std::uint64_t exact_posting_count = 0u;
    std::size_t exception_count = 0u;
    const std::uint64_t maximum_hash = hash_width_bytes == 4u
        ? (std::numeric_limits<std::uint32_t>::max)()
        : (std::numeric_limits<std::uint64_t>::max)();
    for (std::size_t index = 0; index < hashes.size(); ++index) {
        if (hashes[index] > maximum_hash) {
            return std::unexpected(hash_major_minimizer_error::control);
        }
        if (index != 0u && hashes[index] <= hashes[index - 1u]) {
            return std::unexpected(hash_major_minimizer_error::order);
        }
        if (exact_counts[index] == 0u) {
            return std::unexpected(hash_major_minimizer_error::count);
        }
        if (!checked_add(
                exact_posting_count,
                exact_counts[index],
                exact_posting_count)) {
            return std::unexpected(hash_major_minimizer_error::arithmetic_overflow);
        }
        maximum_delta = std::max(maximum_delta, hashes[index] - hashes[0]);
        if (exact_counts[index] >= 65'535u) ++exception_count;
    }
    const std::uint8_t delta_width = bit_width_u64(maximum_delta);
    if (delta_width > hash_width_bytes * 8u) {
        return std::unexpected(hash_major_minimizer_error::control);
    }
    std::uint64_t hash_bits = 0u;
    std::uint64_t hash_bytes_u64 = 0u;
    std::uint64_t count_bytes_u64 = 0u;
    std::uint64_t exception_bytes_u64 = 0u;
    if (!checked_multiply(hashes.size(), delta_width, hash_bits)
        || !checked_add(hash_bits, 7u, hash_bytes_u64)
        || !checked_multiply(hashes.size(), 2u, count_bytes_u64)
        || !checked_multiply(exception_count, 12u, exception_bytes_u64)) {
        return std::unexpected(hash_major_minimizer_error::arithmetic_overflow);
    }
    hash_bytes_u64 /= 8u;
    std::uint64_t total_bytes = k_hash_major_minimizer_control_header_bytes;
    if (!checked_add(total_bytes, hash_bytes_u64, total_bytes)
        || !checked_add(total_bytes, count_bytes_u64, total_bytes)
        || !checked_add(total_bytes, exception_bytes_u64, total_bytes)
        || total_bytes > (std::numeric_limits<std::size_t>::max)()
        || hash_bytes_u64 > (std::numeric_limits<std::uint32_t>::max)()
        || count_bytes_u64 > (std::numeric_limits<std::uint32_t>::max)()
        || exception_bytes_u64
            > (std::numeric_limits<std::uint32_t>::max)()) {
        return std::unexpected(hash_major_minimizer_error::arithmetic_overflow);
    }
    std::vector<std::byte> output(static_cast<std::size_t>(total_bytes));
    std::span<std::byte> bytes{output};
    write_le(bytes, 0u, hashes[0]);
    write_le(bytes, 8u, static_cast<std::uint32_t>(hashes.size()));
    write_le(bytes, 12u, delta_width);
    write_le<std::uint8_t>(bytes, 13u, 16u);
    write_le<std::uint16_t>(bytes, 14u, 0u);
    write_le(bytes, 16u, exact_posting_count);
    write_le(bytes, 24u, posting_byte_offset);
    write_le(bytes, 32u, static_cast<std::uint32_t>(hash_bytes_u64));
    write_le(bytes, 36u, static_cast<std::uint32_t>(count_bytes_u64));
    write_le(bytes, 40u, static_cast<std::uint32_t>(exception_bytes_u64));
    write_le<std::uint32_t>(bytes, 44u, 0u);
    const std::size_t hash_begin = k_hash_major_minimizer_control_header_bytes;
    const std::size_t count_begin = hash_begin
        + static_cast<std::size_t>(hash_bytes_u64);
    const std::size_t exception_begin = count_begin
        + static_cast<std::size_t>(count_bytes_u64);
    for (std::size_t index = 0; index < hashes.size(); ++index) {
        store_packed_lsb(
            bytes.subspan(hash_begin, static_cast<std::size_t>(hash_bytes_u64)),
            static_cast<std::uint64_t>(index) * delta_width,
            delta_width,
            hashes[index] - hashes[0]);
        write_le<std::uint16_t>(
            bytes,
            count_begin + index * 2u,
            exact_counts[index] >= 65'535u
                ? std::uint16_t{65'535u}
                : static_cast<std::uint16_t>(exact_counts[index]));
    }
    std::size_t exception_ordinal = 0u;
    for (std::size_t index = 0; index < exact_counts.size(); ++index) {
        if (exact_counts[index] < 65'535u) continue;
        const std::size_t offset = exception_begin + exception_ordinal * 12u;
        write_le<std::uint16_t>(bytes, offset, static_cast<std::uint16_t>(index));
        write_le<std::uint16_t>(bytes, offset + 2u, 0u);
        write_le(bytes, offset + 4u, exact_counts[index]);
        ++exception_ordinal;
    }
    crc64_ecma182_accumulator control_crc;
    control_crc.update(std::span<const std::byte>{output}.first(48u));
    control_crc.update(std::span<const std::byte>{output}.subspan(
        k_hash_major_minimizer_control_header_bytes));
    write_le(bytes, 48u, control_crc.checksum());
    write_le(bytes, 56u, posting_crc64);
    return output;
}

/** @brief Decodes a control block into caller-owned reusable scratch. */
[[nodiscard]] inline std::expected<
    decoded_hash_major_minimizer_control_block_view, hash_major_minimizer_error>
decode_hash_major_minimizer_control_block_into(
        std::span<const std::byte> input,
        std::uint8_t hash_width_bytes,
        std::uint8_t stored_posting_width_bytes,
        hash_major_minimizer_control_scratch& scratch) {
    using namespace hash_major_minimizer_detail;
    if (input.size() < k_hash_major_minimizer_control_header_bytes
        || (hash_width_bytes != 4u && hash_width_bytes != 8u)
        || (stored_posting_width_bytes != 4u
            && stored_posting_width_bytes != 8u)) {
        return std::unexpected(hash_major_minimizer_error::encoded_size);
    }
    hash_major_minimizer_control_header header{
        .first_hash = read_le<std::uint64_t>(input, 0u),
        .control_count = read_le<std::uint32_t>(input, 8u),
        .hash_delta_bit_width = read_le<std::uint8_t>(input, 12u),
        .saturated_count_bit_width = read_le<std::uint8_t>(input, 13u),
        .exact_posting_count = read_le<std::uint64_t>(input, 16u),
        .posting_byte_offset = read_le<std::uint64_t>(input, 24u),
        .hash_bytes = read_le<std::uint32_t>(input, 32u),
        .count_bytes = read_le<std::uint32_t>(input, 36u),
        .exception_bytes = read_le<std::uint32_t>(input, 40u),
        .control_crc64 = read_le<std::uint64_t>(input, 48u),
        .posting_crc64 = read_le<std::uint64_t>(input, 56u),
    };
    if (header.control_count == 0u
        || header.control_count > k_hash_major_minimizer_controls_per_block
        || header.hash_delta_bit_width > hash_width_bytes * 8u
        || header.saturated_count_bit_width != 16u
        || read_le<std::uint16_t>(input, 14u) != 0u
        || read_le<std::uint32_t>(input, 44u) != 0u
        || header.posting_byte_offset % stored_posting_width_bytes != 0u
        || header.count_bytes != header.control_count * 2u
        || header.exception_bytes % 12u != 0u) {
        return std::unexpected(hash_major_minimizer_error::control);
    }
    std::uint64_t hash_bits = 0u;
    std::uint64_t expected_hash_bytes = 0u;
    std::uint64_t expected_total = k_hash_major_minimizer_control_header_bytes;
    if (!checked_multiply(
            header.control_count, header.hash_delta_bit_width, hash_bits)
        || !checked_add(hash_bits, 7u, expected_hash_bytes)) {
        return std::unexpected(hash_major_minimizer_error::arithmetic_overflow);
    }
    expected_hash_bytes /= 8u;
    if (!checked_add(expected_total, expected_hash_bytes, expected_total)
        || !checked_add(expected_total, header.count_bytes, expected_total)
        || !checked_add(expected_total, header.exception_bytes, expected_total)
        || expected_hash_bytes != header.hash_bytes
        || expected_total != input.size()) {
        return std::unexpected(hash_major_minimizer_error::encoded_size);
    }
    crc64_ecma182_accumulator control_crc;
    control_crc.update(input.first(48u));
    control_crc.update(input.subspan(k_hash_major_minimizer_control_header_bytes));
    if (control_crc.checksum() != header.control_crc64) {
        return std::unexpected(hash_major_minimizer_error::checksum);
    }
    const auto hash_bytes = input.subspan(
        k_hash_major_minimizer_control_header_bytes, header.hash_bytes);
    if (hash_bits % 8u != 0u && !hash_bytes.empty()) {
        const std::uint8_t used = static_cast<std::uint8_t>(hash_bits % 8u);
        const std::uint8_t final = std::to_integer<std::uint8_t>(
            hash_bytes.back());
        if ((final & static_cast<std::uint8_t>(0xffu << used)) != 0u) {
            return std::unexpected(hash_major_minimizer_error::reserved_nonzero);
        }
    }
    const std::uint64_t maximum_hash = hash_width_bytes == 4u
        ? (std::numeric_limits<std::uint32_t>::max)()
        : (std::numeric_limits<std::uint64_t>::max)();
    if (header.first_hash > maximum_hash) {
        return std::unexpected(hash_major_minimizer_error::control);
    }
    for (std::size_t index = 0; index < header.control_count; ++index) {
        const std::uint64_t delta = load_packed_lsb_wordwise(
            hash_bytes,
            static_cast<std::uint64_t>(index)
                * header.hash_delta_bit_width,
            header.hash_delta_bit_width);
        if ((index == 0u && delta != 0u)
            || delta > maximum_hash - header.first_hash) {
            return std::unexpected(hash_major_minimizer_error::control);
        }
        scratch.hashes[index] = header.first_hash + delta;
        if (index != 0u
            && scratch.hashes[index] <= scratch.hashes[index - 1u]) {
            return std::unexpected(hash_major_minimizer_error::order);
        }
    }
    if (header.hash_delta_bit_width
        != bit_width_u64(
            scratch.hashes[header.control_count - 1u]
            - header.first_hash)) {
        return std::unexpected(hash_major_minimizer_error::control);
    }
    const std::size_t count_begin = k_hash_major_minimizer_control_header_bytes
        + header.hash_bytes;
    const std::size_t exception_begin = count_begin + header.count_bytes;
    std::size_t saturated_count = 0u;
    for (std::size_t index = 0; index < header.control_count; ++index) {
        const std::uint16_t count = read_le<std::uint16_t>(
            input, count_begin + index * 2u);
        if (count == 0u) {
            return std::unexpected(hash_major_minimizer_error::count);
        }
        if (count == 65'535u) {
            scratch.exact_counts[index] = 0u;
            ++saturated_count;
        } else {
            scratch.exact_counts[index] = count;
        }
    }
    if (header.exception_bytes != saturated_count * 12u) {
        return std::unexpected(hash_major_minimizer_error::count);
    }
    std::size_t next_sentinel = 0u;
    for (std::size_t index = 0; index < saturated_count; ++index) {
        while (next_sentinel < header.control_count
               && scratch.exact_counts[next_sentinel] != 0u) {
            ++next_sentinel;
        }
        const std::size_t offset = exception_begin + index * 12u;
        const std::uint16_t ordinal = read_le<std::uint16_t>(input, offset);
        const std::uint64_t exact_count = read_le<std::uint64_t>(
            input, offset + 4u);
        if (next_sentinel == header.control_count
            || ordinal != next_sentinel
            || read_le<std::uint16_t>(input, offset + 2u) != 0u
            || exact_count < 65'535u) {
            return std::unexpected(hash_major_minimizer_error::count);
        }
        scratch.exact_counts[ordinal] = exact_count;
        ++next_sentinel;
    }
    std::uint64_t observed_count = 0u;
    for (std::size_t index = 0u; index < header.control_count; ++index) {
        const std::uint64_t count = scratch.exact_counts[index];
        if (count == 0u) {
            return std::unexpected(hash_major_minimizer_error::count);
        }
        if (!checked_add(observed_count, count, observed_count)) {
            return std::unexpected(hash_major_minimizer_error::arithmetic_overflow);
        }
    }
    std::uint64_t posting_bytes = 0u;
    if (observed_count != header.exact_posting_count
        || !checked_multiply(
            observed_count, stored_posting_width_bytes, posting_bytes)) {
        return std::unexpected(hash_major_minimizer_error::count);
    }
    return decoded_hash_major_minimizer_control_block_view{
        .header = header,
        .hashes = std::span<const std::uint64_t>{scratch.hashes}.first(
            header.control_count),
        .exact_counts =
            std::span<const std::uint64_t>{scratch.exact_counts}.first(
                header.control_count),
    };
}

/** @brief Decodes a control block into owning vectors. */
[[nodiscard]] inline std::expected<
    decoded_hash_major_minimizer_control_block, hash_major_minimizer_error>
decode_hash_major_minimizer_control_block(
        std::span<const std::byte> input,
        std::uint8_t hash_width_bytes,
        std::uint8_t stored_posting_width_bytes) {
    hash_major_minimizer_control_scratch scratch;
    auto decoded = decode_hash_major_minimizer_control_block_into(
        input,
        hash_width_bytes,
        stored_posting_width_bytes,
        scratch);
    if (!decoded) return std::unexpected(decoded.error());
    return decoded_hash_major_minimizer_control_block{
        .header = decoded->header,
        .hashes = std::vector<std::uint64_t>(
            decoded->hashes.begin(), decoded->hashes.end()),
        .exact_counts = std::vector<std::uint64_t>(
            decoded->exact_counts.begin(), decoded->exact_counts.end()),
    };
}

} // namespace bioseq::indexing
namespace bioseq::indexing::hash_major_minimizer_detail {

[[nodiscard]] constexpr bool valid_stream_kind(
        hash_major_minimizer_stream_kind kind) noexcept {
    return kind == hash_major_minimizer_stream_kind::directory
        || kind == hash_major_minimizer_stream_kind::controls
        || kind == hash_major_minimizer_stream_kind::postings;
}

[[nodiscard]] constexpr bool valid_dense_seal(
        const hash_major_minimizer_dense_seal& seal) noexcept {
    return seal.sub_key == (std::numeric_limits<std::int64_t>::max)() - 2
        && seal.position_segment == -1
        && seal.header_chunk_no == 0u
        && seal.abi == 1u
        && (seal.storage_mode == 1u || seal.storage_mode == 2u)
        && seal.body_chunk_count != 0u
        && !all_zero(seal.incarnation);
}

constexpr void write_stream_descriptor(
        std::span<std::byte> output,
        std::size_t offset,
        const hash_major_minimizer_stream_descriptor& descriptor) noexcept {
    write_le<std::uint16_t>(
        output, offset, static_cast<std::uint16_t>(descriptor.kind));
    write_le(output, offset + 2u, descriptor.encoding_revision);
    write_le(output, offset + 4u, descriptor.element_width);
    write_le<std::uint8_t>(output, offset + 5u, 0u);
    write_le<std::uint16_t>(output, offset + 6u, 0u);
    write_le(output, offset + 8u, descriptor.element_count);
    write_le(output, offset + 16u, descriptor.logical_bytes);
    write_le(output, offset + 24u, descriptor.row_count);
    write_le(output, offset + 32u, descriptor.stream_crc64);
    write_le(output, offset + 40u, descriptor.first_row_ordinal);
}

[[nodiscard]] constexpr hash_major_minimizer_stream_descriptor
read_stream_descriptor(
        std::span<const std::byte> input,
        std::size_t offset) noexcept {
    return hash_major_minimizer_stream_descriptor{
        .kind = static_cast<hash_major_minimizer_stream_kind>(
            read_le<std::uint16_t>(input, offset)),
        .encoding_revision = read_le<std::uint16_t>(input, offset + 2u),
        .element_width = read_le<std::uint8_t>(input, offset + 4u),
        .element_count = read_le<std::uint64_t>(input, offset + 8u),
        .logical_bytes = read_le<std::uint64_t>(input, offset + 16u),
        .row_count = read_le<std::uint64_t>(input, offset + 24u),
        .stream_crc64 = read_le<std::uint64_t>(input, offset + 32u),
        .first_row_ordinal = read_le<std::uint64_t>(input, offset + 40u),
    };
}

[[nodiscard]] constexpr bool valid_stream_descriptor(
        const hash_major_minimizer_stream_descriptor& descriptor,
        hash_major_minimizer_stream_kind expected_kind,
        std::uint8_t expected_width) noexcept {
    return descriptor.kind == expected_kind
        && descriptor.encoding_revision == 1u
        && descriptor.element_width == expected_width
        && descriptor.row_count
            == ceil_div(descriptor.logical_bytes,
                        k_hash_major_minimizer_row_payload_bytes);
}

[[nodiscard]] inline bool valid_manifest_shape(
        const hash_major_minimizer_manifest& manifest) noexcept {
    if (manifest.generation_id == 0u
        || !is_valid_hash_major_minimizer_storage_profile(
            manifest.storage_profile, manifest.target_length)
        || manifest.storage_profile_digest
            != hash_major_minimizer_storage_profile_digest(
                manifest.storage_profile)
        || manifest.streams[0].element_count
            != manifest.directory_entry_count
        || manifest.streams[1].element_count
            != manifest.unique_hash_count
        || manifest.streams[2].element_count
            != manifest.total_posting_count
        || manifest.total_control_bytes != manifest.streams[1].logical_bytes
        || manifest.total_posting_bytes != manifest.streams[2].logical_bytes
        || manifest.authenticated_directory_crc64
            != manifest.streams[0].stream_crc64
        || !valid_stream_descriptor(
            manifest.streams[0],
            hash_major_minimizer_stream_kind::directory,
            static_cast<std::uint8_t>(k_hash_major_minimizer_directory_entry_bytes))
        || !valid_stream_descriptor(
            manifest.streams[1],
            hash_major_minimizer_stream_kind::controls,
            0u)
        || !valid_stream_descriptor(
            manifest.streams[2],
            hash_major_minimizer_stream_kind::postings,
            manifest.storage_profile.stored_posting_width_bytes)) {
        return false;
    }
    std::uint64_t directory_bytes = 0;
    std::uint64_t posting_bytes = 0;
    std::uint64_t posting_first_row = 0;
    std::uint64_t total_rows = 0;
    return checked_multiply(
            manifest.directory_entry_count,
            k_hash_major_minimizer_directory_entry_bytes,
            directory_bytes)
        && directory_bytes == manifest.streams[0].logical_bytes
        && checked_multiply(
            manifest.total_posting_count,
            manifest.storage_profile.stored_posting_width_bytes,
            posting_bytes)
        && posting_bytes == manifest.total_posting_bytes
        && manifest.streams[0].first_row_ordinal == 0u
        && manifest.streams[1].first_row_ordinal
            == manifest.streams[0].row_count
        && checked_add(
            manifest.streams[0].row_count,
            manifest.streams[1].row_count,
            posting_first_row)
        && manifest.streams[2].first_row_ordinal == posting_first_row
        && checked_add(
            posting_first_row,
            manifest.streams[2].row_count,
            total_rows)
        && (manifest.unique_hash_count == 0u)
            == (manifest.directory_entry_count == 0u)
        && (manifest.total_posting_count == 0u)
            == (manifest.unique_hash_count == 0u);
}

constexpr void write_manifest_fields(
        std::span<std::byte> output,
        const hash_major_minimizer_manifest& manifest,
        std::uint64_t generation_crc64) noexcept {
    write_bytes(output, 0u, manifest_magic);
    write_le<std::uint16_t>(output, 8u, k_hash_major_minimizer_layout_revision);
    write_le<std::uint16_t>(
        output, 10u, k_hash_major_minimizer_manifest_header_bytes);
    write_le<std::uint32_t>(output, 12u, 0u);
    write_le(output, 16u, manifest.generation_id);
    write_le(output, 24u, manifest.source_epoch);
    write_bytes(output, 32u, manifest.source_identity);
    write_le(output, 48u, manifest.source_content_crc64);
    write_le(output, 56u, manifest.target_length);
    write_le(output, 64u, manifest.unique_hash_count);
    write_le(output, 72u, manifest.total_posting_count);
    write_le(output, 80u, manifest.storage_profile.hash_scheme_identity);
    write_le(output, 84u, manifest.storage_profile.hash_scheme_revision);
    write_le(output, 86u, manifest.storage_profile.alphabet_code);
    write_le(output, 88u, manifest.storage_profile.kmer_size);
    write_le(output, 90u, manifest.storage_profile.window_size);
    write_le(output, 92u, manifest.storage_profile.hash_width_bytes);
    write_le(
        output, 93u, manifest.storage_profile.stored_posting_width_bytes);
    write_le<std::uint8_t>(output, 94u, 0u);
    write_le<std::uint8_t>(output, 95u, 3u);
    write_le(
        output, 96u, manifest.storage_profile.position_encoding_revision);
    write_le(output, 98u, manifest.storage_profile.hash_semantics_revision);
    write_le(output, 100u, manifest.storage_profile.count_semantics_revision);
    write_le(output, 102u, manifest.storage_profile.producer_backend_identity);
    write_le(
        output, 104u, manifest.storage_profile.producer_backend_revision);
    write_le(output, 108u, manifest.storage_profile.controls_per_block);
    write_bytes(output, 112u, manifest.storage_profile_digest);
    write_le(output, 144u, manifest.authenticated_directory_crc64);
    write_le(output, 152u, generation_crc64);
    write_le(output, 160u, manifest.total_control_bytes);
    write_le(output, 168u, manifest.total_posting_bytes);
    write_le(output, 176u, manifest.directory_entry_count);
    write_le(output, 184u, manifest.storage_profile.hash_seed);
    for (std::size_t index = 0; index < manifest.streams.size(); ++index) {
        write_stream_descriptor(
            output,
            k_hash_major_minimizer_manifest_header_bytes
                + index * k_hash_major_minimizer_stream_descriptor_bytes,
            manifest.streams[index]);
    }
}

[[nodiscard]] inline std::uint64_t generation_crc_from_encoded_manifest(
        std::span<const std::byte> encoded) noexcept {
    std::array<std::byte, k_hash_major_minimizer_manifest_bytes> normalized{};
    std::copy(encoded.begin(), encoded.end(), normalized.begin());
    std::fill(
        normalized.begin() + 48u,
        normalized.begin() + 56u,
        std::byte{0});
    std::fill(
        normalized.begin() + 144u,
        normalized.begin() + 160u,
        std::byte{0});
    crc64_ecma182_accumulator crc;
    crc.update(normalized);
    for (std::size_t index = 0; index < 3u; ++index) {
        crc.update_little_endian(read_le<std::uint64_t>(
            encoded,
            k_hash_major_minimizer_manifest_header_bytes
                + index * k_hash_major_minimizer_stream_descriptor_bytes + 32u));
    }
    return crc.checksum();
}

} // namespace bioseq::indexing::hash_major_minimizer_detail

export namespace bioseq::indexing {

/** @brief Encodes and checksums the atomically published generation head. */
[[nodiscard]] inline std::expected<
    encoded_hash_major_minimizer_head, hash_major_minimizer_error>
encode_hash_major_minimizer_head(const hash_major_minimizer_head& head) noexcept {
    using namespace hash_major_minimizer_detail;
    if (head.generation_id == 0u
        || (head.dense_seal && !valid_dense_seal(*head.dense_seal))) {
        return std::unexpected(hash_major_minimizer_error::state);
    }
    encoded_hash_major_minimizer_head output{};
    std::span<std::byte> bytes{output};
    write_bytes(bytes, 0u, head_magic);
    write_le<std::uint16_t>(bytes, 8u, k_hash_major_minimizer_layout_revision);
    write_le<std::uint16_t>(bytes, 10u, k_hash_major_minimizer_head_bytes);
    write_le<std::uint32_t>(bytes, 12u, head.dense_seal ? 1u : 0u);
    write_le(bytes, 16u, head.generation_id);
    write_le(bytes, 24u, head.query_key);
    write_le(bytes, 32u, head.source_epoch);
    write_bytes(bytes, 40u, head.source_identity);
    write_le(bytes, 56u, head.source_content_crc64);
    write_bytes(bytes, 64u, head.storage_profile_digest);
    write_le<std::uint64_t>(bytes, 96u, 0u);
    write_le<std::uint64_t>(bytes, 104u, 0u);
    write_le<std::uint64_t>(bytes, 112u, 0u);
    write_le<std::uint32_t>(bytes, 120u, k_hash_major_minimizer_manifest_bytes);
    write_le<std::uint32_t>(bytes, 124u, 1u);
    write_le(bytes, 128u, head.manifest_address_bound_row_crc64);
    write_le(bytes, 136u, head.manifest_payload_crc64);
    write_le<std::uint64_t>(bytes, 144u, 0u);
    if (head.dense_seal) {
        const auto& seal = *head.dense_seal;
        write_le(bytes, 152u, std::bit_cast<std::uint64_t>(seal.sub_key));
        write_le(
            bytes, 160u, std::bit_cast<std::uint64_t>(seal.position_segment));
        write_le(bytes, 168u, seal.header_chunk_no);
        write_le(bytes, 172u, seal.abi);
        write_le(bytes, 174u, seal.storage_mode);
        write_le(bytes, 176u, seal.body_chunk_count);
        write_bytes(bytes, 184u, seal.incarnation);
        write_le(bytes, 200u, seal.crc64);
    }
    const std::uint64_t checksum = crc64_ecma182(output);
    write_le(bytes, 144u, checksum);
    return output;
}

/** @brief Decodes and authenticates one fixed-size generation head. */
[[nodiscard]] inline std::expected<
    hash_major_minimizer_head, hash_major_minimizer_error>
decode_hash_major_minimizer_head(std::span<const std::byte> input) noexcept {
    using namespace hash_major_minimizer_detail;
    if (input.size() != k_hash_major_minimizer_head_bytes) {
        return std::unexpected(hash_major_minimizer_error::encoded_size);
    }
    if (read_bytes<8>(input, 0u) != head_magic) {
        return std::unexpected(hash_major_minimizer_error::magic);
    }
    if (read_le<std::uint16_t>(input, 8u) != k_hash_major_minimizer_layout_revision) {
        return std::unexpected(hash_major_minimizer_error::layout_revision);
    }
    if (read_le<std::uint16_t>(input, 10u) != k_hash_major_minimizer_head_bytes
        || read_le<std::uint32_t>(input, 120u)
            != k_hash_major_minimizer_manifest_bytes
        || read_le<std::uint32_t>(input, 124u) != 1u
        || read_le<std::uint64_t>(input, 96u) != 0u
        || read_le<std::uint64_t>(input, 104u) != 0u
        || read_le<std::uint64_t>(input, 112u) != 0u) {
        return std::unexpected(hash_major_minimizer_error::declared_size);
    }
    const std::uint32_t flags = read_le<std::uint32_t>(input, 12u);
    if ((flags & ~1u) != 0u) {
        return std::unexpected(hash_major_minimizer_error::flags);
    }
    if (!all_zero(input.subspan(208u, 16u))) {
        return std::unexpected(hash_major_minimizer_error::reserved_nonzero);
    }
    std::array<std::byte, k_hash_major_minimizer_head_bytes> normalized{};
    std::copy(input.begin(), input.end(), normalized.begin());
    std::fill(
        normalized.begin() + 144u,
        normalized.begin() + 152u,
        std::byte{0});
    const std::uint64_t expected_crc = crc64_ecma182(normalized);
    const std::uint64_t stored_crc = read_le<std::uint64_t>(input, 144u);
    if (expected_crc != stored_crc) {
        return std::unexpected(hash_major_minimizer_error::checksum);
    }
    hash_major_minimizer_head head{
        .generation_id = read_le<std::uint64_t>(input, 16u),
        .query_key = read_le<std::uint64_t>(input, 24u),
        .source_epoch = read_le<std::uint64_t>(input, 32u),
        .source_identity = read_bytes<16>(input, 40u),
        .source_content_crc64 = read_le<std::uint64_t>(input, 56u),
        .storage_profile_digest = read_bytes<32>(input, 64u),
        .manifest_address_bound_row_crc64 =
            read_le<std::uint64_t>(input, 128u),
        .manifest_payload_crc64 = read_le<std::uint64_t>(input, 136u),
        .head_crc64 = stored_crc,
    };
    if (head.generation_id == 0u) {
        return std::unexpected(hash_major_minimizer_error::state);
    }
    if ((flags & 1u) == 0u) {
        if (!all_zero(input.subspan(152u, 56u))) {
            return std::unexpected(hash_major_minimizer_error::reserved_nonzero);
        }
    } else {
        hash_major_minimizer_dense_seal seal{
            .sub_key = std::bit_cast<std::int64_t>(
                read_le<std::uint64_t>(input, 152u)),
            .position_segment = std::bit_cast<std::int64_t>(
                read_le<std::uint64_t>(input, 160u)),
            .header_chunk_no = read_le<std::uint32_t>(input, 168u),
            .abi = read_le<std::uint16_t>(input, 172u),
            .storage_mode = read_le<std::uint16_t>(input, 174u),
            .body_chunk_count = read_le<std::uint64_t>(input, 176u),
            .incarnation = read_bytes<16>(input, 184u),
            .crc64 = read_le<std::uint64_t>(input, 200u),
        };
        if (!valid_dense_seal(seal)) {
            return std::unexpected(hash_major_minimizer_error::flags);
        }
        head.dense_seal = seal;
    }
    return head;
}

/** @brief Encodes the complete authenticated stream manifest. */
[[nodiscard]] inline std::expected<
    encoded_hash_major_minimizer_manifest, hash_major_minimizer_error>
encode_hash_major_minimizer_manifest(
        const hash_major_minimizer_manifest& manifest) noexcept {
    using namespace hash_major_minimizer_detail;
    if (!valid_manifest_shape(manifest)) {
        return std::unexpected(hash_major_minimizer_error::stream_descriptor);
    }
    encoded_hash_major_minimizer_manifest output{};
    write_manifest_fields(output, manifest, 0u);
    const std::uint64_t generation_crc =
        generation_crc_from_encoded_manifest(output);
    write_le<std::uint64_t>(output, 152u, generation_crc);
    return output;
}

/** @brief Decodes and authenticates the complete stream manifest. */
[[nodiscard]] inline std::expected<
    hash_major_minimizer_manifest, hash_major_minimizer_error>
decode_hash_major_minimizer_manifest(
        std::span<const std::byte> input) noexcept {
    using namespace hash_major_minimizer_detail;
    if (input.size() != k_hash_major_minimizer_manifest_bytes) {
        return std::unexpected(hash_major_minimizer_error::encoded_size);
    }
    if (read_bytes<8>(input, 0u) != manifest_magic) {
        return std::unexpected(hash_major_minimizer_error::magic);
    }
    if (read_le<std::uint16_t>(input, 8u) != k_hash_major_minimizer_layout_revision) {
        return std::unexpected(hash_major_minimizer_error::layout_revision);
    }
    if (read_le<std::uint16_t>(input, 10u)
            != k_hash_major_minimizer_manifest_header_bytes
        || read_le<std::uint32_t>(input, 12u) != 0u
        || read_le<std::uint8_t>(input, 94u) != 0u
        || read_le<std::uint8_t>(input, 95u) != 3u) {
        return std::unexpected(hash_major_minimizer_error::declared_size);
    }
    for (std::size_t index = 0; index < 3u; ++index) {
        const std::size_t offset = k_hash_major_minimizer_manifest_header_bytes
            + index * k_hash_major_minimizer_stream_descriptor_bytes;
        if (read_le<std::uint8_t>(input, offset + 5u) != 0u
            || read_le<std::uint16_t>(input, offset + 6u) != 0u) {
            return std::unexpected(hash_major_minimizer_error::reserved_nonzero);
        }
    }
    hash_major_minimizer_manifest manifest{
        .generation_id = read_le<std::uint64_t>(input, 16u),
        .source_epoch = read_le<std::uint64_t>(input, 24u),
        .source_identity = read_bytes<16>(input, 32u),
        .source_content_crc64 = read_le<std::uint64_t>(input, 48u),
        .target_length = read_le<std::uint64_t>(input, 56u),
        .unique_hash_count = read_le<std::uint64_t>(input, 64u),
        .total_posting_count = read_le<std::uint64_t>(input, 72u),
        .storage_profile = hash_major_minimizer_storage_profile{
            .hash_scheme_identity = read_le<std::uint32_t>(input, 80u),
            .hash_scheme_revision = read_le<std::uint16_t>(input, 84u),
            .alphabet_code = read_le<std::uint16_t>(input, 86u),
            .kmer_size = read_le<std::uint16_t>(input, 88u),
            .window_size = read_le<std::uint16_t>(input, 90u),
            .hash_width_bytes = read_le<std::uint8_t>(input, 92u),
            .stored_posting_width_bytes = read_le<std::uint8_t>(input, 93u),
            .position_encoding_revision = read_le<std::uint16_t>(input, 96u),
            .hash_semantics_revision = read_le<std::uint16_t>(input, 98u),
            .count_semantics_revision = read_le<std::uint16_t>(input, 100u),
            .producer_backend_identity = read_le<std::uint16_t>(input, 102u),
            .producer_backend_revision = read_le<std::uint32_t>(input, 104u),
            .controls_per_block = read_le<std::uint32_t>(input, 108u),
            .hash_seed = read_le<std::uint64_t>(input, 184u),
        },
        .storage_profile_digest = read_bytes<32>(input, 112u),
        .authenticated_directory_crc64 = read_le<std::uint64_t>(input, 144u),
        .generation_crc64 = read_le<std::uint64_t>(input, 152u),
        .total_control_bytes = read_le<std::uint64_t>(input, 160u),
        .total_posting_bytes = read_le<std::uint64_t>(input, 168u),
        .directory_entry_count = read_le<std::uint64_t>(input, 176u),
    };
    for (std::size_t index = 0; index < manifest.streams.size(); ++index) {
        manifest.streams[index] = read_stream_descriptor(
            input,
            k_hash_major_minimizer_manifest_header_bytes
                + index * k_hash_major_minimizer_stream_descriptor_bytes);
    }
    if (!valid_manifest_shape(manifest)) {
        return std::unexpected(hash_major_minimizer_error::stream_descriptor);
    }
    if (generation_crc_from_encoded_manifest(input)
        != manifest.generation_crc64) {
        return std::unexpected(hash_major_minimizer_error::checksum);
    }
    return manifest;
}

/** @brief Encodes one fixed-size hash directory entry. */
[[nodiscard]] inline std::expected<
    encoded_hash_major_minimizer_directory_entry, hash_major_minimizer_error>
encode_hash_major_minimizer_directory_entry(
        const hash_major_minimizer_directory_entry& entry) noexcept {
    using namespace hash_major_minimizer_detail;
    if (entry.control_count == 0u
        || entry.control_count > k_hash_major_minimizer_controls_per_block
        || entry.first_hash > entry.last_hash
        || entry.control_byte_length < k_hash_major_minimizer_control_header_bytes
        || entry.control_byte_length
            > k_hash_major_minimizer_maximum_control_block_bytes
        || entry.exact_posting_count == 0u) {
        return std::unexpected(hash_major_minimizer_error::directory);
    }
    encoded_hash_major_minimizer_directory_entry output{};
    write_le(output, 0u, entry.first_hash);
    write_le(output, 8u, entry.last_hash);
    write_le(output, 16u, entry.control_block_ordinal);
    write_le(output, 24u, entry.control_byte_offset);
    write_le(output, 32u, entry.control_byte_length);
    write_le(output, 36u, entry.control_count);
    write_le(output, 40u, entry.first_posting_ordinal);
    write_le(output, 48u, entry.posting_byte_offset);
    write_le(output, 56u, entry.posting_byte_length);
    write_le(output, 64u, entry.exact_posting_count);
    write_le(output, 72u, entry.block_crc64);
    return output;
}

/** @brief Decodes and validates one fixed-size hash directory entry. */
[[nodiscard]] inline std::expected<
    hash_major_minimizer_directory_entry, hash_major_minimizer_error>
decode_hash_major_minimizer_directory_entry(
        std::span<const std::byte> input) noexcept {
    using namespace hash_major_minimizer_detail;
    if (input.size() != k_hash_major_minimizer_directory_entry_bytes) {
        return std::unexpected(hash_major_minimizer_error::encoded_size);
    }
    hash_major_minimizer_directory_entry entry{
        .first_hash = read_le<std::uint64_t>(input, 0u),
        .last_hash = read_le<std::uint64_t>(input, 8u),
        .control_block_ordinal = read_le<std::uint64_t>(input, 16u),
        .control_byte_offset = read_le<std::uint64_t>(input, 24u),
        .control_byte_length = read_le<std::uint32_t>(input, 32u),
        .control_count = read_le<std::uint32_t>(input, 36u),
        .first_posting_ordinal = read_le<std::uint64_t>(input, 40u),
        .posting_byte_offset = read_le<std::uint64_t>(input, 48u),
        .posting_byte_length = read_le<std::uint64_t>(input, 56u),
        .exact_posting_count = read_le<std::uint64_t>(input, 64u),
        .block_crc64 = read_le<std::uint64_t>(input, 72u),
    };
    const auto encoded = encode_hash_major_minimizer_directory_entry(entry);
    if (!encoded) return std::unexpected(encoded.error());
    return entry;
}

/** @brief Binds a directory entry to its control and posting checksums. */
[[nodiscard]] inline std::uint64_t hash_major_minimizer_block_crc64(
        const hash_major_minimizer_directory_entry& entry,
        std::uint64_t control_crc64,
        std::uint64_t posting_crc64) noexcept {
    auto encoded = encode_hash_major_minimizer_directory_entry(entry);
    if (!encoded) return 0u;
    crc64_ecma182_accumulator crc;
    crc.update(std::span<const std::byte>{*encoded}.first(72u));
    crc.update_little_endian(control_crc64);
    crc.update_little_endian(posting_crc64);
    return crc.checksum();
}

} // namespace bioseq::indexing
