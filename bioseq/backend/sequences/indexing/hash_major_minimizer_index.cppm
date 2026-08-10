/**
 * @file hash_major_minimizer_index.cppm
 * @brief Storage-neutral hash-major minimizer codec and selective reader.
 *
 * The module owns logical bytes only. PostgreSQL row authentication, physical
 * row addressing and publication remain adapters over these exact streams.
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
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

export module bioseq.indexing.hash_major_minimizer_index;
import bioseq.common.checked_arithmetic;
import bioseq.common.hashing;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.storage;
import bioseq.parallelism.memory_admission;

static_assert(__cplusplus >= 202302L, "C++23-or-later");

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

/** @brief Width-tagged borrowed canonical query hashes. */
struct canonical_query_hash_view {
    std::span<const std::uint32_t> values32{};
    std::span<const std::uint64_t> values64{};
    std::uint8_t width_bytes = 0u;
    std::uint8_t storage_width_bytes = 0u;
    std::array<std::byte, 32> storage_profile_digest{};

    [[nodiscard]] std::size_t size() const noexcept {
        return storage_width_bytes == sizeof(std::uint32_t)
            ? values32.size() : values64.size();
    }

    [[nodiscard]] std::uint64_t at(std::size_t ordinal) const noexcept {
        return storage_width_bytes == sizeof(std::uint32_t)
            ? static_cast<std::uint64_t>(values32[ordinal])
            : values64[ordinal];
    }

    [[nodiscard]] bool storage_matches_width() const noexcept {
        return width_bytes == storage_width_bytes
            && ((storage_width_bytes == sizeof(std::uint32_t)
                    && values64.empty())
                || (storage_width_bytes == sizeof(std::uint64_t)
                    && values32.empty()));
    }

    [[nodiscard]] std::size_t lower_bound(
            std::size_t first,
            std::size_t last,
            std::uint64_t hash) const noexcept {
        if (storage_width_bytes == sizeof(std::uint32_t)) {
            if (hash > (std::numeric_limits<std::uint32_t>::max)()) {
                return last;
            }
            const auto begin = values32.begin()
                + static_cast<std::ptrdiff_t>(first);
            const auto end = values32.begin()
                + static_cast<std::ptrdiff_t>(last);
            return static_cast<std::size_t>(
                std::lower_bound(
                    begin, end, static_cast<std::uint32_t>(hash))
                - values32.begin());
        }
        const auto begin = values64.begin()
            + static_cast<std::ptrdiff_t>(first);
        const auto end = values64.begin()
            + static_cast<std::ptrdiff_t>(last);
        return static_cast<std::size_t>(
            std::lower_bound(begin, end, hash) - values64.begin());
    }
};

/// Owning query-hash storage selected by the persisted hash width.
using canonical_query_hash_storage = std::variant<
    std::vector<std::uint32_t>, std::vector<std::uint64_t>>;

/** @brief Move-only canonical query hashes with their storage-profile identity. */
struct canonical_query_hash_image {
    canonical_query_hash_storage values{};
    std::uint8_t width_bytes = 0u;
    std::array<std::byte, 32> storage_profile_digest{};

    canonical_query_hash_image() = default;
    canonical_query_hash_image(const canonical_query_hash_image&) = delete;
    canonical_query_hash_image& operator=(const canonical_query_hash_image&) = delete;
    canonical_query_hash_image(canonical_query_hash_image&&) noexcept = default;
    canonical_query_hash_image& operator=(canonical_query_hash_image&&) noexcept =
        default;

    [[nodiscard]] canonical_query_hash_view view() const noexcept {
        return std::visit(
            [this](const auto& value) {
                using p_value_type = typename std::remove_cvref_t<
                    decltype(value)>::value_type;
                canonical_query_hash_view view{
                    .width_bytes = width_bytes,
                    .storage_width_bytes = sizeof(p_value_type),
                    .storage_profile_digest = storage_profile_digest,
                };
                if constexpr (std::is_same_v<p_value_type, std::uint32_t>) {
                    view.values32 = std::span<const p_value_type>{value};
                } else {
                    view.values64 = std::span<const p_value_type>{value};
                }
                return view;
            },
            values);
    }
};

/** @brief Exact generation identity required before a reader is admitted. */
struct expected_hash_major_minimizer_generation {
    std::uint64_t generation_id = 0u;
    std::uint64_t query_key = 0u;
    std::uint64_t source_epoch = 0u;
    std::uint64_t source_content_crc64 = 0u;
    std::uint64_t target_length = 0u;
    hash_major_minimizer_storage_profile storage_profile{};
};

/** @brief Exact count replacing one saturated compact count. */
struct exact_global_count_exception {
    std::uint32_t query_ordinal = 0;
    std::uint32_t reserved_zero = 0;
    std::uint64_t exact_count = 0;
};
static_assert(sizeof(exact_global_count_exception) == 16u);

/** @brief Authorized posting interval for one query hash. */
struct posting_request {
    std::uint64_t posting_byte_offset = 0;
    std::uint64_t exact_count = 0;
    std::uint32_t query_ordinal = 0;
    std::uint32_t flags = 0;
};
static_assert(sizeof(posting_request) == 24u);

/** @brief Admission, retention, checksum, and delivery counters for one open. */
struct hash_major_minimizer_memory_stats {
    std::uint64_t directory_stage_peak_admitted_bytes = 0u;
    std::uint64_t control_stage_peak_admitted_bytes = 0u;
    std::uint64_t posting_stage_peak_admitted_bytes = 0u;
    std::uint64_t peak_admitted_bytes = 0u;
    std::uint64_t retained_owner_bytes = 0u;
    std::uint64_t query_count = 0u;
    std::uint64_t exact_count_exception_count = 0u;
    std::uint64_t retained_position_count = 0u;
    std::uint64_t directory_entry_count = 0u;
    std::uint64_t selected_block_count = 0u;
    std::uint64_t accepted_hash_count = 0u;
    std::uint64_t directory_stream_crc_receipts = 0u;
    std::uint64_t control_block_crc_receipts = 0u;
    std::uint64_t control_semantic_receipts = 0u;
    std::uint64_t posting_ranges_delivered = 0u;
    std::uint64_t posting_range_crc_receipts = 0u;
    std::uint64_t posting_semantic_receipts = 0u;
    std::uint64_t authorized_posting_bytes = 0u;
    std::uint64_t generic_source_fallback_calls = 0u;
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

/** @brief Encodes and checksums the atomically published generation head. */
[[nodiscard]] std::expected<
    encoded_hash_major_minimizer_head, hash_major_minimizer_error>
encode_hash_major_minimizer_head(const hash_major_minimizer_head& head) noexcept;

/** @brief Decodes and authenticates one fixed-size generation head. */
[[nodiscard]] std::expected<
    hash_major_minimizer_head, hash_major_minimizer_error>
decode_hash_major_minimizer_head(std::span<const std::byte> input) noexcept;

/** @brief Encodes the complete authenticated stream manifest. */
[[nodiscard]] std::expected<
    encoded_hash_major_minimizer_manifest, hash_major_minimizer_error>
encode_hash_major_minimizer_manifest(
    const hash_major_minimizer_manifest& manifest) noexcept;

/** @brief Decodes and authenticates the complete stream manifest. */
[[nodiscard]] std::expected<
    hash_major_minimizer_manifest, hash_major_minimizer_error>
decode_hash_major_minimizer_manifest(
    std::span<const std::byte> input) noexcept;

/** @brief Encodes one fixed-size hash directory entry. */
[[nodiscard]] std::expected<
    encoded_hash_major_minimizer_directory_entry, hash_major_minimizer_error>
encode_hash_major_minimizer_directory_entry(
    const hash_major_minimizer_directory_entry& entry) noexcept;

/** @brief Decodes and validates one fixed-size hash directory entry. */
[[nodiscard]] std::expected<
    hash_major_minimizer_directory_entry, hash_major_minimizer_error>
decode_hash_major_minimizer_directory_entry(
    std::span<const std::byte> input) noexcept;

/** @brief Binds a directory entry to its control and posting checksums. */
[[nodiscard]] std::uint64_t hash_major_minimizer_block_crc64(
    const hash_major_minimizer_directory_entry& entry,
    std::uint64_t control_crc64,
    std::uint64_t posting_crc64) noexcept;

/** @brief Encodes a compressed control block without changing posting order. */
[[nodiscard]] std::expected<std::vector<std::byte>, hash_major_minimizer_error>
encode_hash_major_minimizer_control_block(
    std::span<const std::uint64_t> hashes,
    std::span<const std::uint64_t> exact_counts,
    std::uint64_t posting_byte_offset,
    std::uint64_t posting_crc64,
    std::uint8_t hash_width_bytes,
    std::uint8_t stored_posting_width_bytes);

/** @brief Decodes a control block into owning vectors. */
[[nodiscard]] std::expected<
    decoded_hash_major_minimizer_control_block, hash_major_minimizer_error>
decode_hash_major_minimizer_control_block(
    std::span<const std::byte> input,
    std::uint8_t hash_width_bytes,
    std::uint8_t stored_posting_width_bytes);

/** @brief Decodes a control block into caller-owned reusable scratch. */
[[nodiscard]] std::expected<
    decoded_hash_major_minimizer_control_block_view, hash_major_minimizer_error>
decode_hash_major_minimizer_control_block_into(
    std::span<const std::byte> input,
    std::uint8_t hash_width_bytes,
    std::uint8_t stored_posting_width_bytes,
    hash_major_minimizer_control_scratch& scratch);

} // namespace bioseq::indexing

export namespace bioseq::indexing {

/** @brief Authenticated posting byte range delivered as part of one read wave. */
struct hash_major_minimizer_authorized_posting_range {
    std::uint64_t control_block_ordinal = 0u;
    std::uint64_t posting_byte_offset = 0u;
    std::uint64_t byte_count = 0u;
    std::uint64_t expected_posting_crc64 = 0u;
    std::uint32_t request_begin = 0u;
    std::uint32_t request_count = 0u;
    std::uint32_t flags = 0u; // bit 0: complete authoritative block
    std::uint32_t reserved_zero = 0u;

    [[nodiscard]] bool operator==(
        const hash_major_minimizer_authorized_posting_range&) const = default;
};
static_assert(sizeof(hash_major_minimizer_authorized_posting_range) == 48u);

/** @brief Authenticated control-block range delivered as part of one read wave. */
struct hash_major_minimizer_authorized_control_range {
    std::uint64_t control_block_ordinal = 0u;
    std::uint64_t control_byte_offset = 0u;
    std::uint32_t byte_count = 0u;
    std::uint32_t reserved_zero = 0u;

    [[nodiscard]] bool operator==(
        const hash_major_minimizer_authorized_control_range&) const = default;
};
static_assert(sizeof(hash_major_minimizer_authorized_control_range) == 24u);

/// Callback that consumes a bounded posting fragment for one authorized range.
using hash_major_minimizer_consume_posting_bytes_fn = bool (*)(
    void*, std::uint32_t, std::uint64_t, std::span<const std::byte>);
/// Callback that batches authenticated posting-range reads.
using hash_major_minimizer_read_posting_wave_fn = bool (*)(
    void*,
    std::span<const hash_major_minimizer_authorized_posting_range>,
    void*,
    hash_major_minimizer_consume_posting_bytes_fn);
/// Callback that consumes one complete authenticated control block.
using hash_major_minimizer_consume_control_block_fn = bool (*)(
    void*, std::uint32_t, std::span<const std::byte>);
/// Callback that batches authenticated control-block reads.
using hash_major_minimizer_read_control_wave_fn = bool (*)(
    void*,
    std::span<const hash_major_minimizer_authorized_control_range>,
    void*,
    hash_major_minimizer_consume_control_block_fn);
/// Generic logical-stream read callback used when no batch route is available.
using hash_major_minimizer_read_logical_bytes_fn = bool (*)(
    void*, hash_major_minimizer_stream_kind, std::uint64_t, std::span<std::byte>);
/// Callback that returns one logical stream's exact byte size.
using hash_major_minimizer_logical_stream_size_fn = std::uint64_t (*)(
    void*, hash_major_minimizer_stream_kind);

/** @brief Non-owning callback table for logical and authenticated batch reads. */
struct hash_major_minimizer_logical_stream_source_ref {
    void* context = nullptr;
    hash_major_minimizer_read_logical_bytes_fn read_bytes = nullptr;
    hash_major_minimizer_logical_stream_size_fn logical_size = nullptr;
    hash_major_minimizer_read_control_wave_fn read_control_wave = nullptr;
    hash_major_minimizer_read_posting_wave_fn read_posting_wave = nullptr;

    [[nodiscard]] bool read(
            hash_major_minimizer_stream_kind kind,
            std::uint64_t offset,
            std::span<std::byte> output) const {
        return read_bytes != nullptr
            && read_bytes(context, kind, offset, output);
    }

    [[nodiscard]] std::uint64_t size(
            hash_major_minimizer_stream_kind kind) const {
        return logical_size == nullptr ? 0u : logical_size(context, kind);
    }
};

/**
 * @brief Memory-admitted selective hash-major reader with bounded cold reads.
 *
 * Query hashes are borrowed for the reader lifetime. Persisted bytes remain
 * owned by the callback source, and all retained decoded state is charged to
 * the shared memory-admission permit.
 */
class hash_major_minimizer_selective_reader {
private:
    struct block_authority {
        std::uint64_t directory_ordinal = 0u;
        std::uint64_t posting_byte_offset = 0u;
        std::uint64_t posting_byte_length = 0u;
        std::uint64_t posting_crc64 = 0u;
    };
    static_assert(sizeof(block_authority) == 32u);

    enum class memory_admission_stage : std::uint8_t {
        directory,
        control,
        posting,
    };

    inline static constexpr std::uint64_t k_absent_query_offset =
        (std::numeric_limits<std::uint64_t>::max)();
    inline static constexpr std::uint64_t k_unavailable_query_offset =
        (std::numeric_limits<std::uint64_t>::max)() - 1u;

    struct posting_delivery_context {
        hash_major_minimizer_selective_reader* reader = nullptr;
        const hash_major_minimizer_manifest* manifest = nullptr;
        const std::vector<posting_request>* requests = nullptr;
        const std::vector<hash_major_minimizer_authorized_posting_range>* ranges =
            nullptr;
        std::size_t next_range = 0u;
        std::size_t next_request = 0u;
        std::uint64_t next_range_byte = 0u;
        crc64_ecma182_accumulator range_crc{};
        crc64_ecma182_accumulator* posting_stream_crc = nullptr;
        std::uint64_t* posting_stream_next = nullptr;
        bool* posting_stream_complete = nullptr;
        std::optional<hash_major_minimizer_error> error{};
    };

    struct control_delivery_context {
        hash_major_minimizer_selective_reader* reader = nullptr;
        const hash_major_minimizer_manifest* manifest = nullptr;
        canonical_query_hash_view query_hashes{};
        const std::vector<hash_major_minimizer_directory_entry>* directory = nullptr;
        std::vector<block_authority>* authorities = nullptr;
        const std::vector<hash_major_minimizer_authorized_control_range>* ranges =
            nullptr;
        hash_major_minimizer_control_scratch* scratch = nullptr;
        crc64_ecma182_accumulator* control_stream_crc = nullptr;
        std::uint64_t max_occ = 0u;
        std::size_t query_cursor = 0u;
        std::size_t next_range = 0u;
        std::size_t controls_read = 0u;
        std::optional<hash_major_minimizer_error> error{};
    };

    // The permit is declared before every admitted owner so reverse member
    // destruction frees vector capacity before returning its reservation.
    bioseq::parallelism::memory_permit permit_{};
    hash_major_minimizer_manifest manifest_{};
    canonical_query_hash_view query_hashes_{}; // caller lifetime is explicit
    std::vector<std::uint16_t> primary_counts_{};
    std::vector<exact_global_count_exception> count_exceptions_{};
    std::vector<std::uint64_t> query_offsets_{};
    std::unique_ptr<std::uint32_t[]> retained_positions32_{};
    std::unique_ptr<std::uint64_t[]> retained_positions64_{};
    std::size_t retained_position_count_ = 0u;
    std::vector<std::uint32_t> canonical_radix_prefix_{};
    hash_major_minimizer_memory_stats memory_stats_{};
    std::uint64_t persistent_owner_bytes_ = 0u;
    std::size_t selected_hash_count_ = 0u;
    std::size_t control_wave_peak_owner_bytes_ = 0u;
    std::size_t posting_wave_peak_owner_bytes_ = 0u;
    std::size_t transient_authority_peak_owner_bytes_ = 0u;

    template <class p_value_type>
    [[nodiscard]] static bool vector_capacity_bytes_(
            const std::vector<p_value_type>& values,
            std::uint64_t& output) noexcept {
        return hash_major_minimizer_detail::checked_multiply(
            values.capacity(), sizeof(p_value_type), output);
    }

    [[nodiscard]] bool refresh_persistent_owner_bytes_() noexcept {
        using namespace hash_major_minimizer_detail;
        std::uint64_t total = 0u;
        const auto add = [&total](std::uint64_t bytes) {
            return checked_add(total, bytes, total);
        };
        std::uint64_t bytes = 0u;
        if (!vector_capacity_bytes_(primary_counts_, bytes) || !add(bytes)
            || !vector_capacity_bytes_(count_exceptions_, bytes) || !add(bytes)
            || !vector_capacity_bytes_(query_offsets_, bytes) || !add(bytes)
            || !checked_multiply(
                retained_position_count_,
                manifest_.storage_profile.stored_posting_width_bytes,
                bytes)
            || !add(bytes)
            || !vector_capacity_bytes_(canonical_radix_prefix_, bytes)
            || !add(bytes)) {
            return false;
        }
        persistent_owner_bytes_ = total;
        return true;
    }

    void record_admitted_peak_(
            memory_admission_stage admission_stage,
            std::uint64_t bytes) noexcept {
        memory_stats_.peak_admitted_bytes =
            std::max(memory_stats_.peak_admitted_bytes, bytes);
        std::uint64_t* stage_peak = nullptr;
        switch (admission_stage) {
            case memory_admission_stage::directory:
                stage_peak =
                    &memory_stats_.directory_stage_peak_admitted_bytes;
                break;
            case memory_admission_stage::control:
                stage_peak =
                    &memory_stats_.control_stage_peak_admitted_bytes;
                break;
            case memory_admission_stage::posting:
                stage_peak =
                    &memory_stats_.posting_stage_peak_admitted_bytes;
                break;
        }
        *stage_peak = std::max(*stage_peak, bytes);
    }

    [[nodiscard]] bool grow_permit_to_(
            std::uint64_t bytes,
            memory_admission_stage admission_stage) noexcept {
        if (!permit_.grow_to(bytes)) return false;
        record_admitted_peak_(admission_stage, permit_.bytes());
        return true;
    }

    [[nodiscard]] std::uint64_t exact_count_(
            std::size_t query_ordinal) const noexcept {
        const std::uint16_t primary = primary_counts_[query_ordinal];
        if (primary != 65'535u) return primary;
        const auto found = std::lower_bound(
            count_exceptions_.begin(),
            count_exceptions_.end(),
            query_ordinal,
            [](const exact_global_count_exception& exception,
               std::uint32_t ordinal) {
                return exception.query_ordinal < ordinal;
            });
        return found != count_exceptions_.end()
                && found->query_ordinal == query_ordinal
            ? found->exact_count : 0u;
    }

    [[nodiscard]] std::size_t canonical_ordinal_(
            std::uint64_t hash) const noexcept {
        if (hash > (std::numeric_limits<std::uint32_t>::max)()
            || canonical_radix_prefix_.size() != 65'537u) {
            return query_hashes_.size();
        }
        const std::size_t radix = static_cast<std::size_t>(hash >> 16u);
        const std::size_t begin = canonical_radix_prefix_[radix];
        const std::size_t end = canonical_radix_prefix_[radix + 1u];
        const std::size_t found =
            query_hashes_.lower_bound(begin, end, hash);
        return found < end && query_hashes_.at(found) == hash
            ? found : query_hashes_.size();
    }

    [[nodiscard]] std::size_t canonical_lower_bound_(
            std::size_t first,
            std::uint64_t hash) const noexcept {
        if (hash > (std::numeric_limits<std::uint32_t>::max)()
            || canonical_radix_prefix_.size() != 65'537u) {
            return query_hashes_.size();
        }
        const std::size_t radix = static_cast<std::size_t>(hash >> 16u);
        const std::size_t bucket_begin = canonical_radix_prefix_[radix];
        const std::size_t bucket_end = canonical_radix_prefix_[radix + 1u];
        if (first >= bucket_end) return first;
        return query_hashes_.lower_bound(
            std::max(first, bucket_begin), bucket_end, hash);
    }

    [[nodiscard]] bool canonical_contains_(std::uint64_t hash) const noexcept {
        return canonical_ordinal_(hash) != query_hashes_.size();
    }

    [[nodiscard]] static bool consume_control_block_(
            void* opaque,
            std::uint32_t range_ordinal,
            std::span<const std::byte> encoded_control) {
        using namespace hash_major_minimizer_detail;
        auto& context = *static_cast<control_delivery_context*>(opaque);
        if (context.error) return false;
        if (range_ordinal != context.next_range
            || range_ordinal >= context.ranges->size()) {
            context.error = hash_major_minimizer_error::source_read;
            return false;
        }
        const auto& range = (*context.ranges)[range_ordinal];
        if (range.reserved_zero != 0u
            || range.control_block_ordinal >= context.directory->size()) {
            context.error = hash_major_minimizer_error::control;
            return false;
        }
        const auto& entry = (*context.directory)[
            static_cast<std::size_t>(range.control_block_ordinal)];
        if (entry.control_block_ordinal != range.control_block_ordinal
            || entry.control_byte_offset != range.control_byte_offset
            || entry.control_byte_length != range.byte_count
            || encoded_control.size() != range.byte_count) {
            context.error = hash_major_minimizer_error::source_read;
            return false;
        }
        ++context.controls_read;
        context.control_stream_crc->update(encoded_control);
        auto control = decode_hash_major_minimizer_control_block_into(
            encoded_control,
            context.manifest->storage_profile.hash_width_bytes,
            context.manifest->storage_profile.stored_posting_width_bytes,
            *context.scratch);
        if (!control) {
            context.error = control.error();
            return false;
        }
        const std::uint64_t maximum_hash =
            hash_major_minimizer_max_hash(context.manifest->storage_profile);
        std::uint64_t expected_posting_bytes = 0u;
        if (control->header.first_hash != entry.first_hash
            || control->hashes.back() != entry.last_hash
            || control->hashes.back() > maximum_hash
            || control->header.control_count != entry.control_count
            || control->header.exact_posting_count
                != entry.exact_posting_count
            || control->header.posting_byte_offset
                != entry.posting_byte_offset
            || !checked_multiply(
                entry.exact_posting_count,
                context.manifest->storage_profile.stored_posting_width_bytes,
                expected_posting_bytes)
            || expected_posting_bytes != entry.posting_byte_length
            || hash_major_minimizer_block_crc64(
                entry,
                control->header.control_crc64,
                control->header.posting_crc64) != entry.block_crc64) {
            context.error = hash_major_minimizer_error::directory;
            return false;
        }
        if (context.authorities->size() == context.authorities->capacity()) {
            context.error = hash_major_minimizer_error::admission;
            return false;
        }
        context.authorities->push_back(block_authority{
            .directory_ordinal = range.control_block_ordinal,
            .posting_byte_offset = entry.posting_byte_offset,
            .posting_byte_length = entry.posting_byte_length,
            .posting_crc64 = control->header.posting_crc64,
        });
        context.query_cursor = context.reader->canonical_lower_bound_(
            context.query_cursor, entry.first_hash);
        std::uint64_t posting_prefix = 0u;
        for (std::size_t control_ordinal = 0u;
             control_ordinal < control->hashes.size();
             ++control_ordinal) {
            const std::uint64_t hash = control->hashes[control_ordinal];
            const std::uint64_t count =
                control->exact_counts[control_ordinal];
            while (context.query_cursor < context.query_hashes.size()
                   && context.query_hashes.at(context.query_cursor) < hash) {
                ++context.query_cursor;
            }
            if (context.query_cursor < context.query_hashes.size()
                && context.query_hashes.at(context.query_cursor) == hash) {
                if (count == 0u) {
                    context.error = hash_major_minimizer_error::count;
                    return false;
                }
                const std::size_t query_ordinal = context.query_cursor;
                if (context.reader->primary_counts_[query_ordinal] != 0u
                    || context.reader->query_offsets_[query_ordinal]
                        != k_absent_query_offset) {
                    context.error = hash_major_minimizer_error::order;
                    return false;
                }
                ++context.reader->selected_hash_count_;
                if (count <= 65'534u) {
                    context.reader->primary_counts_[query_ordinal] =
                        static_cast<std::uint16_t>(count);
                } else {
                    context.reader->primary_counts_[query_ordinal] = 65'535u;
                    if (context.reader->count_exceptions_.size()
                        == context.reader->count_exceptions_.capacity()) {
                        context.error = hash_major_minimizer_error::admission;
                        return false;
                    }
                    context.reader->count_exceptions_.push_back(
                        exact_global_count_exception{
                            .query_ordinal =
                                static_cast<std::uint32_t>(query_ordinal),
                            .reserved_zero = 0u,
                            .exact_count = count,
                        });
                }
                if (count <= context.max_occ) {
                    std::uint64_t posting_delta = 0u;
                    std::uint64_t posting_offset = 0u;
                    if (!checked_multiply(
                            posting_prefix,
                            context.manifest->storage_profile
                                .stored_posting_width_bytes,
                            posting_delta)
                        || !checked_add(
                            entry.posting_byte_offset,
                            posting_delta,
                            posting_offset)
                        || posting_offset >= k_unavailable_query_offset) {
                        context.error =
                            hash_major_minimizer_error::arithmetic_overflow;
                        return false;
                    }
                    context.reader->query_offsets_[query_ordinal] =
                        posting_offset;
                    ++context.reader->memory_stats_.accepted_hash_count;
                } else {
                    context.reader->query_offsets_[query_ordinal] =
                        k_unavailable_query_offset;
                }
                ++context.query_cursor;
            }
            if (!checked_add(posting_prefix, count, posting_prefix)) {
                context.error = hash_major_minimizer_error::arithmetic_overflow;
                return false;
            }
        }
        if (posting_prefix != entry.exact_posting_count) {
            context.error = hash_major_minimizer_error::count;
            return false;
        }
        ++context.next_range;
        return true;
    }

    [[nodiscard]] static bool consume_posting_bytes_(
            void* opaque,
            std::uint32_t range_ordinal,
            std::uint64_t range_byte_offset,
            std::span<const std::byte> bytes) {
        using namespace hash_major_minimizer_detail;
        auto& context = *static_cast<posting_delivery_context*>(opaque);
        if (context.error) return false;
        if (range_ordinal != context.next_range
            || range_ordinal >= context.ranges->size()
            || range_byte_offset != context.next_range_byte
            || bytes.empty()
            || bytes.size() > k_hash_major_minimizer_read_slab_bytes) {
            context.error = hash_major_minimizer_error::source_read;
            return false;
        }
        const hash_major_minimizer_authorized_posting_range& range =
            (*context.ranges)[range_ordinal];
        const std::uint8_t width = context.manifest->storage_profile
            .stored_posting_width_bytes;
        if (range.reserved_zero != 0u || (range.flags & ~1u) != 0u
            || range_byte_offset > range.byte_count
            || bytes.size() > range.byte_count - range_byte_offset
            || bytes.size() % width != 0u) {
            context.error = hash_major_minimizer_error::posting;
            return false;
        }
        std::uint64_t absolute_chunk = 0u;
        if (!checked_add(
                range.posting_byte_offset,
                range_byte_offset,
                absolute_chunk)) {
            context.error = hash_major_minimizer_error::arithmetic_overflow;
            return false;
        }
        if (*context.posting_stream_complete) {
            if (absolute_chunk != *context.posting_stream_next) {
                *context.posting_stream_complete = false;
            } else {
                context.posting_stream_crc->update(bytes);
                *context.posting_stream_next += bytes.size();
            }
        }
        if ((range.flags & 1u) != 0u) context.range_crc.update(bytes);
        const std::size_t range_request_end =
            static_cast<std::size_t>(range.request_begin)
            + range.request_count;
        if (range_request_end > context.requests->size()) {
            context.error = hash_major_minimizer_error::posting;
            return false;
        }
        std::uint64_t absolute_chunk_end = 0u;
        if (!checked_add(
                absolute_chunk, bytes.size(), absolute_chunk_end)) {
            context.error = hash_major_minimizer_error::arithmetic_overflow;
            return false;
        }
        std::size_t chunk_byte_cursor = 0u;
        while (chunk_byte_cursor < bytes.size()) {
            if (context.next_request >= range_request_end) {
                context.error = hash_major_minimizer_error::posting;
                return false;
            }
            const posting_request& request =
                (*context.requests)[context.next_request];
            std::uint64_t request_bytes = 0u;
            std::uint64_t request_end = 0u;
            if (!checked_multiply(
                    request.exact_count, width, request_bytes)
                || !checked_add(
                    request.posting_byte_offset,
                    request_bytes,
                    request_end)) {
                context.error = hash_major_minimizer_error::arithmetic_overflow;
                return false;
            }
            const std::uint64_t absolute =
                absolute_chunk + chunk_byte_cursor;
            if (request.exact_count == 0u
                || absolute < request.posting_byte_offset
                || absolute >= request_end
                || (absolute - request.posting_byte_offset) % width != 0u
                || request.query_ordinal
                    >= context.reader->query_offsets_.size()) {
                context.error = hash_major_minimizer_error::posting;
                return false;
            }
            const std::uint64_t segment_end =
                std::min(absolute_chunk_end, request_end);
            const std::uint64_t segment_bytes = segment_end - absolute;
            const std::uint64_t posting_ordinal =
                (absolute - request.posting_byte_offset) / width;
            const std::uint64_t segment_position_count =
                segment_bytes / width;
            if (segment_bytes == 0u || segment_bytes % width != 0u
                || posting_ordinal >= request.exact_count
                || segment_position_count
                    > request.exact_count - posting_ordinal) {
                context.error = hash_major_minimizer_error::posting;
                return false;
            }
            const std::uint64_t output_offset =
                context.reader->query_offsets_[request.query_ordinal];
            std::uint64_t destination = 0u;
            std::uint64_t destination_end = 0u;
            if (output_offset >= k_unavailable_query_offset
                || !checked_add(
                    output_offset, posting_ordinal, destination)
                || !checked_add(
                    destination,
                    segment_position_count,
                    destination_end)) {
                context.error = hash_major_minimizer_error::arithmetic_overflow;
                return false;
            }
            if (destination_end
                    > context.reader->retained_position_count_) {
                context.error = hash_major_minimizer_error::posting;
                return false;
            }
            const auto decode_segment =
                [&context,
                 &bytes,
                 chunk_byte_cursor,
                 destination,
                 destination_end,
                 posting_ordinal]<class p_stored_position>() noexcept -> bool {
                    p_stored_position* const retained_positions =
                        [](
                                hash_major_minimizer_selective_reader&
                                    reader) noexcept -> p_stored_position* {
                            if constexpr (std::is_same_v<
                                    p_stored_position, std::uint32_t>) {
                                return reader.retained_positions32_.get();
                            } else {
                                return reader.retained_positions64_.get();
                            }
                        }(*context.reader);
                    bool have_previous = posting_ordinal != 0u;
                    std::uint64_t previous_position = have_previous
                        ? static_cast<std::uint64_t>(
                            retained_positions[
                                static_cast<std::size_t>(
                                    destination - 1u)])
                        : 0u;
                    std::size_t input_offset = chunk_byte_cursor;
                    std::size_t output_index =
                        static_cast<std::size_t>(destination);
                    const std::size_t output_end =
                        static_cast<std::size_t>(destination_end);
                    for (; output_index < output_end;
                         ++output_index,
                         input_offset += sizeof(p_stored_position)) {
                        const std::uint64_t position =
                            read_le<p_stored_position>(bytes, input_offset);
                        if ((position >> 1u)
                                >= context.manifest->target_length
                            || (have_previous
                                && position <= previous_position)) {
                            context.error = (position >> 1u)
                                    >= context.manifest->target_length
                                ? hash_major_minimizer_error::target_range
                                : hash_major_minimizer_error::order;
                            return false;
                        }
                        retained_positions[output_index] =
                            static_cast<p_stored_position>(position);
                        previous_position = position;
                        have_previous = true;
                    }
                    return true;
                };
            if (width == sizeof(std::uint32_t)) {
                if (!decode_segment.template operator()<std::uint32_t>()) {
                    return false;
                }
            } else if (!decode_segment.template operator()<std::uint64_t>()) {
                return false;
            }
            chunk_byte_cursor += static_cast<std::size_t>(segment_bytes);
            if (segment_end == request_end) {
                ++context.next_request;
            }
        }
        context.next_range_byte += bytes.size();
        if (context.next_range_byte == range.byte_count) {
            if ((range.flags & 1u) != 0u
                && context.range_crc.checksum()
                    != range.expected_posting_crc64) {
                context.error = hash_major_minimizer_error::checksum;
                return false;
            }
            std::uint64_t ranges_delivered = 0u;
            std::uint64_t semantic_receipts = 0u;
            std::uint64_t crc_receipts =
                context.reader->memory_stats_.
                    posting_range_crc_receipts;
            if (!checked_add(
                    context.reader->memory_stats_.
                        posting_ranges_delivered,
                    1u,
                    ranges_delivered)
                || !checked_add(
                    context.reader->memory_stats_.
                        posting_semantic_receipts,
                    1u,
                    semantic_receipts)
                || ((range.flags & 1u) != 0u
                    && !checked_add(
                        crc_receipts, 1u, crc_receipts))) {
                context.error =
                    hash_major_minimizer_error::arithmetic_overflow;
                return false;
            }
            context.reader->memory_stats_.
                posting_ranges_delivered = ranges_delivered;
            context.reader->memory_stats_.
                posting_semantic_receipts = semantic_receipts;
            context.reader->memory_stats_.
                posting_range_crc_receipts = crc_receipts;
            ++context.next_range;
            context.next_range_byte = 0u;
            context.next_request = context.next_range < context.ranges->size()
                ? (*context.ranges)[context.next_range].request_begin
                : context.requests->size();
            context.range_crc = {};
        }
        return true;
    }

public:
    hash_major_minimizer_selective_reader() = default;
    hash_major_minimizer_selective_reader(
        const hash_major_minimizer_selective_reader&) = delete;
    hash_major_minimizer_selective_reader& operator=(
        const hash_major_minimizer_selective_reader&) = delete;
    hash_major_minimizer_selective_reader(
            hash_major_minimizer_selective_reader&& other) noexcept
        : permit_(std::move(other.permit_)),
          manifest_(std::exchange(
              other.manifest_, hash_major_minimizer_manifest{})),
          query_hashes_(std::exchange(
              other.query_hashes_, canonical_query_hash_view{})),
          primary_counts_(std::move(other.primary_counts_)),
          count_exceptions_(std::move(other.count_exceptions_)),
          query_offsets_(std::move(other.query_offsets_)),
          retained_positions32_(std::move(other.retained_positions32_)),
          retained_positions64_(std::move(other.retained_positions64_)),
          retained_position_count_(
              std::exchange(other.retained_position_count_, 0u)),
          canonical_radix_prefix_(
              std::move(other.canonical_radix_prefix_)),
          memory_stats_(std::exchange(
              other.memory_stats_, hash_major_minimizer_memory_stats{})),
          persistent_owner_bytes_(
              std::exchange(other.persistent_owner_bytes_, 0u)),
          selected_hash_count_(
              std::exchange(other.selected_hash_count_, 0u)),
          control_wave_peak_owner_bytes_(
              std::exchange(other.control_wave_peak_owner_bytes_, 0u)),
          posting_wave_peak_owner_bytes_(
              std::exchange(other.posting_wave_peak_owner_bytes_, 0u)),
          transient_authority_peak_owner_bytes_(
              std::exchange(
                  other.transient_authority_peak_owner_bytes_, 0u)) {}
    hash_major_minimizer_selective_reader& operator=(
        hash_major_minimizer_selective_reader&&) noexcept = delete;

    /**
     * Open against a caller-owned sorted-unique canonical image. The caller
     * keeps that image alive for the reader lifetime; no query hash is copied,
     * widened, sorted, or deduplicated here.
     */
    [[nodiscard]] static std::expected<
        hash_major_minimizer_selective_reader, hash_major_minimizer_error>
    open(
            bioseq::parallelism::memory_admission_controller& admission,
            std::span<const std::byte> encoded_head,
            std::span<const std::byte> encoded_manifest,
            const expected_hash_major_minimizer_generation& expected,
            hash_major_minimizer_logical_stream_source_ref source,
            canonical_query_hash_view query_hashes,
            std::uint64_t max_occ) {
        using namespace hash_major_minimizer_detail;
        if (source.read_bytes == nullptr || source.logical_size == nullptr) {
            return std::unexpected(hash_major_minimizer_error::state);
        }
        auto head = decode_hash_major_minimizer_head(encoded_head);
        if (!head) return std::unexpected(head.error());
        auto manifest = decode_hash_major_minimizer_manifest(encoded_manifest);
        if (!manifest) return std::unexpected(manifest.error());
        const auto expected_digest =
            hash_major_minimizer_storage_profile_digest(expected.storage_profile);
        const auto expected_source = hash_major_minimizer_source_identity(
            expected.query_key,
            expected.source_epoch,
            expected.source_content_crc64,
            expected.target_length);
        if (!is_valid_hash_major_minimizer_storage_profile(
                expected.storage_profile, expected.target_length)
            || expected.generation_id == 0u
            || head->generation_id != expected.generation_id
            || head->query_key != expected.query_key
            || head->source_epoch != expected.source_epoch
            || head->source_content_crc64 != expected.source_content_crc64
            || head->source_identity != expected_source
            || head->storage_profile_digest != expected_digest
            || manifest->generation_id != expected.generation_id
            || manifest->source_epoch != expected.source_epoch
            || manifest->source_content_crc64 != expected.source_content_crc64
            || manifest->source_identity != expected_source
            || manifest->target_length != expected.target_length
            || manifest->storage_profile != expected.storage_profile
            || manifest->storage_profile_digest != expected_digest) {
            return std::unexpected(hash_major_minimizer_error::expected_generation);
        }
        if (head->generation_id != manifest->generation_id
            || head->source_epoch != manifest->source_epoch
            || head->source_identity != manifest->source_identity
            || head->source_content_crc64 != manifest->source_content_crc64
            || head->storage_profile_digest != manifest->storage_profile_digest
            || head->manifest_payload_crc64 != crc64_ecma182(encoded_manifest)
            || head->manifest_address_bound_row_crc64
                != crc64_index_chunk_row(
                    head->generation_id,
                    0,
                    0,
                    0u,
                    static_cast<std::uint32_t>(encoded_manifest.size()),
                    encoded_manifest)) {
            return std::unexpected(hash_major_minimizer_error::checksum);
        }
        if (!query_hashes.storage_matches_width()
            || query_hashes.width_bytes
                != manifest->storage_profile.hash_width_bytes
            || query_hashes.storage_profile_digest != expected_digest
            || query_hashes.size()
                > (std::numeric_limits<std::uint32_t>::max)()) {
            return std::unexpected(hash_major_minimizer_error::storage_contract_mismatch);
        }
        std::uint64_t previous_query = 0u;
        bool have_previous_query = false;
        const std::uint64_t maximum_hash =
            hash_major_minimizer_max_hash(manifest->storage_profile);
        for (std::size_t ordinal = 0u; ordinal < query_hashes.size(); ++ordinal) {
            const std::uint64_t hash = query_hashes.at(ordinal);
            if (hash > maximum_hash
                || (have_previous_query && hash <= previous_query)) {
                return std::unexpected(
                    have_previous_query && hash <= previous_query
                        ? hash_major_minimizer_error::order
                        : hash_major_minimizer_error::storage_contract_mismatch);
            }
            previous_query = hash;
            have_previous_query = true;
        }
        hash_major_minimizer_selective_reader reader;
        reader.manifest_ = *manifest;
        reader.query_hashes_ = query_hashes;
        reader.memory_stats_.query_count = query_hashes.size();
        reader.memory_stats_.directory_entry_count =
            manifest->directory_entry_count;
        const std::uint64_t directory_bytes_u64 =
            manifest->streams[0].logical_bytes;
        if (directory_bytes_u64
                > (std::numeric_limits<std::size_t>::max)()
            || manifest->directory_entry_count
                > (std::numeric_limits<std::size_t>::max)()) {
            return std::unexpected(hash_major_minimizer_error::arithmetic_overflow);
        }
        const std::size_t query_count = query_hashes.size();
        const std::size_t directory_count =
            static_cast<std::size_t>(manifest->directory_entry_count);
        const std::size_t directory_byte_count =
            static_cast<std::size_t>(directory_bytes_u64);
        std::vector<std::byte> directory_bytes;
        std::vector<hash_major_minimizer_directory_entry> directory;
        if (directory_byte_count > directory_bytes.max_size()
            || directory_count > directory.max_size()) {
            return std::unexpected(
                hash_major_minimizer_error::arithmetic_overflow);
        }
        std::uint64_t initial_logical_bytes = 0u;
        const auto add_initial = [&initial_logical_bytes](
                std::uint64_t bytes) {
            return checked_add(
                initial_logical_bytes, bytes, initial_logical_bytes);
        };
        std::uint64_t logical = 0u;
        if (!checked_multiply(65'537u, sizeof(std::uint32_t), logical)
            || !add_initial(logical)
            || !checked_multiply(query_count, sizeof(std::uint16_t), logical)
            || !add_initial(logical)
            || !checked_multiply(query_count, sizeof(std::uint64_t), logical)
            || !add_initial(logical)
            || !add_initial(directory_bytes_u64)
            || !checked_multiply(
                directory_count,
                sizeof(hash_major_minimizer_directory_entry),
                logical)
            || !add_initial(logical)
            || !checked_multiply(
                initial_logical_bytes,
                bioseq::parallelism::k_vector_preallocation_headroom_factor,
                initial_logical_bytes)) {
            return std::unexpected(hash_major_minimizer_error::arithmetic_overflow);
        }
        auto initial_permit = admission.try_reserve(initial_logical_bytes);
        if (!initial_permit) {
            return std::unexpected(hash_major_minimizer_error::admission);
        }
        reader.permit_ = std::move(*initial_permit);
        reader.record_admitted_peak_(
            memory_admission_stage::directory, reader.permit_.bytes());
        try {
            reader.canonical_radix_prefix_.reserve(65'537u);
            reader.canonical_radix_prefix_.resize(65'537u, 0u);
            reader.primary_counts_.reserve(query_count);
            reader.primary_counts_.resize(query_count, 0u);
            reader.query_offsets_.reserve(query_count);
            reader.query_offsets_.resize(
                query_count, k_absent_query_offset);
            directory_bytes.reserve(directory_byte_count);
            directory_bytes.resize(directory_byte_count);
            directory.reserve(directory_count);
        } catch (const std::bad_alloc&) {
            return std::unexpected(hash_major_minimizer_error::admission);
        }
        if (!reader.refresh_persistent_owner_bytes_()) {
            return std::unexpected(hash_major_minimizer_error::arithmetic_overflow);
        }
        std::uint64_t directory_buffer_bytes = 0u;
        std::uint64_t directory_owner_bytes = 0u;
        std::uint64_t current_owner_bytes = 0u;
        if (!vector_capacity_bytes_(directory_bytes, directory_buffer_bytes)
            || !vector_capacity_bytes_(directory, directory_owner_bytes)
            || !checked_add(
                reader.persistent_owner_bytes_,
                directory_buffer_bytes,
                current_owner_bytes)
            || !checked_add(
                current_owner_bytes,
                directory_owner_bytes,
                current_owner_bytes)
            || current_owner_bytes > reader.permit_.bytes()) {
            return std::unexpected(hash_major_minimizer_error::admission);
        }
        reader.permit_.shrink_to(current_owner_bytes);
        for (std::size_t ordinal = 0u; ordinal < query_count; ++ordinal) {
            ++reader.canonical_radix_prefix_[
                static_cast<std::size_t>(
                    query_hashes.at(ordinal) >> 16u) + 1u];
        }
        for (std::size_t radix = 1u;
             radix < reader.canonical_radix_prefix_.size();
             ++radix) {
            reader.canonical_radix_prefix_[radix] +=
                reader.canonical_radix_prefix_[radix - 1u];
        }
        for (const auto& descriptor : manifest->streams) {
            if (source.size(descriptor.kind) != descriptor.logical_bytes) {
                return std::unexpected(
                    hash_major_minimizer_error::stream_descriptor);
            }
        }
        if (!directory_bytes.empty()
            && !source.read(
                hash_major_minimizer_stream_kind::directory, 0u, directory_bytes)) {
            return std::unexpected(hash_major_minimizer_error::source_read);
        }
        const std::uint64_t directory_crc = crc64_ecma182(directory_bytes);
        if (directory_crc != manifest->authenticated_directory_crc64
            || directory_crc != manifest->streams[0].stream_crc64) {
            return std::unexpected(hash_major_minimizer_error::checksum);
        }
        reader.memory_stats_.directory_stream_crc_receipts = 1u;
        std::uint64_t expected_control_offset = 0u;
        std::uint64_t expected_posting_offset = 0u;
        std::uint64_t expected_posting_ordinal = 0u;
        std::uint64_t observed_hashes = 0u;
        std::uint64_t observed_postings = 0u;
        for (std::size_t index = 0u;
             index < manifest->directory_entry_count;
             ++index) {
            auto entry = decode_hash_major_minimizer_directory_entry(
                std::span<const std::byte>{directory_bytes}.subspan(
                    index * k_hash_major_minimizer_directory_entry_bytes,
                    k_hash_major_minimizer_directory_entry_bytes));
            if (!entry) return std::unexpected(entry.error());
            std::uint64_t next_control = 0u;
            std::uint64_t next_posting = 0u;
            std::uint64_t next_posting_ordinal = 0u;
            std::uint64_t entry_posting_bytes = 0u;
            if (entry->control_block_ordinal != index
                || entry->control_byte_offset != expected_control_offset
                || entry->posting_byte_offset != expected_posting_offset
                || entry->first_posting_ordinal != expected_posting_ordinal
                || (index != 0u
                    && entry->first_hash <= directory.back().last_hash)
                || entry->posting_byte_offset
                    % manifest->storage_profile.stored_posting_width_bytes != 0u
                || entry->control_byte_length
                    > k_hash_major_minimizer_maximum_control_block_bytes
                || entry->last_hash > maximum_hash
                || !checked_multiply(
                    entry->exact_posting_count,
                    manifest->storage_profile.stored_posting_width_bytes,
                    entry_posting_bytes)
                || entry_posting_bytes != entry->posting_byte_length
                || !checked_add(
                    expected_control_offset,
                    entry->control_byte_length,
                    next_control)
                || !checked_add(
                    expected_posting_offset,
                    entry->posting_byte_length,
                    next_posting)
                || !checked_add(
                    expected_posting_ordinal,
                    entry->exact_posting_count,
                    next_posting_ordinal)
                || !checked_add(
                    observed_hashes, entry->control_count, observed_hashes)
                || !checked_add(
                    observed_postings,
                    entry->exact_posting_count,
                    observed_postings)) {
                return std::unexpected(hash_major_minimizer_error::directory);
            }
            expected_control_offset = next_control;
            expected_posting_offset = next_posting;
            expected_posting_ordinal = next_posting_ordinal;
            directory.push_back(*entry);
        }
        if (expected_control_offset != manifest->total_control_bytes
            || expected_posting_offset != manifest->total_posting_bytes
            || observed_hashes != manifest->unique_hash_count
            || observed_postings != manifest->total_posting_count) {
            return std::unexpected(hash_major_minimizer_error::directory);
        }
        {
            std::vector<std::byte> empty;
            directory_bytes.swap(empty);
        }
        if (!checked_add(
                reader.persistent_owner_bytes_,
                directory_owner_bytes,
                current_owner_bytes)) {
            return std::unexpected(hash_major_minimizer_error::arithmetic_overflow);
        }
        reader.permit_.shrink_to(current_owner_bytes);

        std::vector<block_authority> authorities;
        crc64_ecma182_accumulator control_stream_crc;
        std::size_t selection_query_cursor = 0u;
        std::size_t selected_block_count = 0u;
        std::uint64_t selected_control_count = 0u;
        std::uint64_t selected_posting_count = 0u;
        for (std::size_t directory_ordinal = 0u;
             directory_ordinal < directory.size();
             ++directory_ordinal) {
            const auto& entry = directory[directory_ordinal];
            selection_query_cursor = reader.canonical_lower_bound_(
                selection_query_cursor, entry.first_hash);
            if (selection_query_cursor == query_hashes.size()) break;
            if (query_hashes.at(selection_query_cursor) > entry.last_hash) {
                continue;
            }
            ++selected_block_count;
            if (!checked_add(
                    selected_control_count,
                    entry.control_count,
                    selected_control_count)
                || !checked_add(
                    selected_posting_count,
                    entry.exact_posting_count,
                    selected_posting_count)) {
                return std::unexpected(
                    hash_major_minimizer_error::arithmetic_overflow);
            }
        }
        reader.memory_stats_.selected_block_count = selected_block_count;
        const std::size_t exception_capacity = static_cast<std::size_t>(
            std::min({
                static_cast<std::uint64_t>(query_count),
                selected_control_count,
                selected_posting_count / 65'535u}));
        const std::size_t control_range_capacity = std::min(
            selected_block_count, k_hash_major_minimizer_control_read_wave_blocks);
        std::uint64_t control_vector_logical_bytes = 0u;
        std::uint64_t control_vector_bytes = 0u;
        std::uint64_t control_requested = 0u;
        if (!checked_multiply(
                selected_block_count,
                sizeof(block_authority),
                control_vector_logical_bytes)
            || !checked_multiply(
                exception_capacity,
                sizeof(exact_global_count_exception),
                logical)
            || !checked_add(
                control_vector_logical_bytes,
                logical,
                control_vector_logical_bytes)
            || !checked_multiply(
                control_range_capacity,
                sizeof(hash_major_minimizer_authorized_control_range),
                logical)
            || !checked_add(
                control_vector_logical_bytes,
                logical,
                control_vector_logical_bytes)
            || !checked_multiply(
                control_vector_logical_bytes,
                bioseq::parallelism::k_vector_preallocation_headroom_factor,
                control_vector_bytes)
            || !checked_add(
                current_owner_bytes,
                sizeof(hash_major_minimizer_control_scratch),
                control_requested)
            || !checked_add(
                control_requested,
                control_vector_bytes,
                control_requested)
            || !reader.grow_permit_to_(
                control_requested, memory_admission_stage::control)) {
            return std::unexpected(hash_major_minimizer_error::admission);
        }
        try {
            reader.count_exceptions_.reserve(exception_capacity);
            authorities.reserve(selected_block_count);
        } catch (const std::bad_alloc&) {
            return std::unexpected(hash_major_minimizer_error::admission);
        }
        std::uint64_t authority_owner_bytes = 0u;
        std::uint64_t control_range_owner_bytes = 0u;
        {
            hash_major_minimizer_control_scratch scratch;
            std::vector<hash_major_minimizer_authorized_control_range>
                control_ranges;
            try {
                control_ranges.reserve(control_range_capacity);
            } catch (const std::bad_alloc&) {
                return std::unexpected(hash_major_minimizer_error::admission);
            }
            if (!reader.refresh_persistent_owner_bytes_()
                || !vector_capacity_bytes_(
                    authorities, authority_owner_bytes)
                || !vector_capacity_bytes_(
                    control_ranges, control_range_owner_bytes)
                || !checked_add(
                    reader.persistent_owner_bytes_,
                    directory_owner_bytes,
                    current_owner_bytes)
                || !checked_add(
                    current_owner_bytes,
                    authority_owner_bytes,
                    current_owner_bytes)
                || !checked_add(
                    current_owner_bytes,
                    control_range_owner_bytes,
                    current_owner_bytes)
                || !checked_add(
                    current_owner_bytes,
                    sizeof(hash_major_minimizer_control_scratch),
                    current_owner_bytes)
                || current_owner_bytes > reader.permit_.bytes()) {
                return std::unexpected(hash_major_minimizer_error::admission);
            }
            reader.permit_.shrink_to(current_owner_bytes);
            reader.control_wave_peak_owner_bytes_ = control_ranges.capacity()
                * sizeof(hash_major_minimizer_authorized_control_range);
            control_delivery_context delivery{
                .reader = &reader,
                .manifest = &*manifest,
                .query_hashes = query_hashes,
                .directory = &directory,
                .authorities = &authorities,
                .scratch = &scratch,
                .control_stream_crc = &control_stream_crc,
                .max_occ = max_occ,
            };
            auto deliver_control_wave = [&]()
                    -> std::expected<void, hash_major_minimizer_error> {
                if (control_ranges.empty()) return {};
                delivery.ranges = &control_ranges;
                delivery.next_range = 0u;
                bool delivered = false;
                if (source.read_control_wave != nullptr) {
                    delivered = source.read_control_wave(
                        source.context,
                        control_ranges,
                        &delivery,
                        &consume_control_block_);
                } else {
                    ++reader.memory_stats_.
                        generic_source_fallback_calls;
                    delivered = true;
                    for (std::size_t ordinal = 0u;
                         ordinal < control_ranges.size();
                         ++ordinal) {
                        const auto& range = control_ranges[ordinal];
                        const auto encoded = std::span<std::byte>{
                            scratch.encoded}.first(range.byte_count);
                        if (!source.read(
                                hash_major_minimizer_stream_kind::controls,
                                range.control_byte_offset,
                                encoded)
                            || !consume_control_block_(
                                &delivery,
                                static_cast<std::uint32_t>(ordinal),
                                encoded)) {
                            delivered = false;
                            break;
                        }
                    }
                }
                if (!delivered || delivery.error
                    || delivery.next_range != control_ranges.size()) {
                    return std::unexpected(delivery.error.value_or(
                        hash_major_minimizer_error::source_read));
                }
                std::uint64_t block_receipts = 0u;
                std::uint64_t semantic_receipts = 0u;
                if (!checked_add(
                        reader.memory_stats_.
                            control_block_crc_receipts,
                        control_ranges.size(),
                        block_receipts)
                    || !checked_add(
                        reader.memory_stats_.
                            control_semantic_receipts,
                        control_ranges.size(),
                        semantic_receipts)) {
                    return std::unexpected(
                        hash_major_minimizer_error::arithmetic_overflow);
                }
                reader.memory_stats_.control_block_crc_receipts =
                    block_receipts;
                reader.memory_stats_.control_semantic_receipts =
                    semantic_receipts;
                control_ranges.clear();
                return {};
            };
            std::size_t delivery_query_cursor = 0u;
            for (std::size_t directory_ordinal = 0u;
                 directory_ordinal < directory.size();
                 ++directory_ordinal) {
                const auto& entry = directory[directory_ordinal];
                delivery_query_cursor = reader.canonical_lower_bound_(
                    delivery_query_cursor, entry.first_hash);
                if (delivery_query_cursor == query_count) break;
                if (query_hashes.at(delivery_query_cursor)
                    > entry.last_hash) {
                    continue;
                }
                control_ranges.push_back(
                    hash_major_minimizer_authorized_control_range{
                        .control_block_ordinal = directory_ordinal,
                        .control_byte_offset = entry.control_byte_offset,
                        .byte_count = entry.control_byte_length,
                    });
                if (control_ranges.size()
                    == k_hash_major_minimizer_control_read_wave_blocks) {
                    if (auto delivered = deliver_control_wave(); !delivered) {
                        return std::unexpected(delivered.error());
                    }
                }
            }
            if (auto delivered = deliver_control_wave(); !delivered) {
                return std::unexpected(delivered.error());
            }
            if (delivery.controls_read != selected_block_count) {
                return std::unexpected(hash_major_minimizer_error::source_read);
            }
            if (delivery.query_cursor > query_hashes.size()) {
                return std::unexpected(hash_major_minimizer_error::state);
            }
            if (reader.memory_stats_.control_block_crc_receipts
                    != selected_block_count
                || reader.memory_stats_.control_semantic_receipts
                    != selected_block_count) {
                return std::unexpected(hash_major_minimizer_error::state);
            }
        }
        if (selected_block_count == directory.size()
            && control_stream_crc.checksum()
                != manifest->streams[1].stream_crc64) {
            return std::unexpected(hash_major_minimizer_error::checksum);
        }
        {
            std::vector<hash_major_minimizer_directory_entry> empty;
            directory.swap(empty);
        }
        if (!reader.refresh_persistent_owner_bytes_()
            || !checked_add(
                reader.persistent_owner_bytes_,
                authority_owner_bytes,
                current_owner_bytes)) {
            return std::unexpected(hash_major_minimizer_error::arithmetic_overflow);
        }
        reader.permit_.shrink_to(current_owner_bytes);
        std::uint64_t retained_count = 0u;
        for (std::size_t query_ordinal = 0u;
             query_ordinal < query_count;
             ++query_ordinal) {
            if (reader.query_offsets_[query_ordinal]
                    < k_unavailable_query_offset
                && !checked_add(
                    retained_count,
                    reader.exact_count_(query_ordinal),
                    retained_count)) {
                return std::unexpected(
                    hash_major_minimizer_error::arithmetic_overflow);
            }
        }
        if (retained_count >= k_unavailable_query_offset
            || retained_count
                > (std::numeric_limits<std::size_t>::max)()) {
            return std::unexpected(hash_major_minimizer_error::arithmetic_overflow);
        }
        reader.memory_stats_.exact_count_exception_count =
            reader.count_exceptions_.size();
        reader.memory_stats_.retained_position_count = retained_count;
        std::uint64_t retained_logical_bytes = 0u;
        if (authority_owner_bytes
            > (std::numeric_limits<std::size_t>::max)()) {
            return std::unexpected(
                hash_major_minimizer_error::arithmetic_overflow);
        }
        reader.transient_authority_peak_owner_bytes_ =
            static_cast<std::size_t>(authority_owner_bytes);

        constexpr std::size_t posting_wave_owner_ceiling =
            k_hash_major_minimizer_read_slab_bytes;
        constexpr std::size_t bytes_per_worst_case_request =
            sizeof(posting_request)
            + sizeof(hash_major_minimizer_authorized_posting_range);
        const std::size_t wave_request_capacity = std::min(
            k_hash_major_minimizer_posting_request_wave_hashes,
            posting_wave_owner_ceiling / bytes_per_worst_case_request);
        static_assert(
            k_hash_major_minimizer_controls_per_block
            < posting_wave_owner_ceiling / bytes_per_worst_case_request);
        const std::size_t accepted_hash_count = static_cast<std::size_t>(
            reader.memory_stats_.accepted_hash_count);
        const std::size_t requested_wave_capacity = std::min(
            accepted_hash_count, wave_request_capacity);
        std::vector<posting_request> requests;
        std::vector<hash_major_minimizer_authorized_posting_range> ranges;
        std::unique_ptr<std::byte[]> fallback_slab;
        const std::uint64_t fallback_slab_bytes =
            source.read_posting_wave == nullptr && accepted_hash_count != 0u
            ? k_hash_major_minimizer_read_slab_bytes
            : 0u;
        std::uint64_t posting_vector_logical_bytes = 0u;
        std::uint64_t posting_vector_preallocation_bytes = 0u;
        std::uint64_t posting_requested = 0u;
        if (!checked_multiply(
                retained_count,
                manifest->storage_profile.stored_posting_width_bytes,
                retained_logical_bytes)
            || !checked_multiply(
                requested_wave_capacity,
                bytes_per_worst_case_request,
                posting_vector_logical_bytes)
            || !checked_multiply(
                posting_vector_logical_bytes,
                bioseq::parallelism::k_vector_preallocation_headroom_factor,
                posting_vector_preallocation_bytes)
            || !checked_add(
                current_owner_bytes,
                retained_logical_bytes,
                posting_requested)
            || !checked_add(
                posting_requested,
                posting_vector_preallocation_bytes,
                posting_requested)
            || !checked_add(
                posting_requested,
                fallback_slab_bytes,
                posting_requested)
            || !reader.grow_permit_to_(
                posting_requested, memory_admission_stage::posting)) {
            return std::unexpected(hash_major_minimizer_error::admission);
        }
        try {
            if (retained_count != 0u) {
                if (manifest->storage_profile.stored_posting_width_bytes
                        == sizeof(std::uint32_t)) {
                    reader.retained_positions32_ =
                        std::make_unique_for_overwrite<std::uint32_t[]>(
                            static_cast<std::size_t>(retained_count));
                } else {
                    reader.retained_positions64_ =
                        std::make_unique_for_overwrite<std::uint64_t[]>(
                            static_cast<std::size_t>(retained_count));
                }
            }
            reader.retained_position_count_ =
                static_cast<std::size_t>(retained_count);
            requests.reserve(requested_wave_capacity);
            ranges.reserve(requested_wave_capacity);
            if (fallback_slab_bytes != 0u) {
                fallback_slab =
                    std::make_unique_for_overwrite<std::byte[]>(
                        k_hash_major_minimizer_read_slab_bytes);
            }
        } catch (const std::bad_alloc&) {
            return std::unexpected(hash_major_minimizer_error::admission);
        }
        std::uint64_t request_capacity_bytes = 0u;
        std::uint64_t range_capacity_bytes = 0u;
        std::uint64_t wave_capacity_bytes = 0u;
        if (!checked_multiply(
                requests.capacity(), sizeof(posting_request),
                request_capacity_bytes)
            || !checked_multiply(
                ranges.capacity(),
                sizeof(hash_major_minimizer_authorized_posting_range),
                range_capacity_bytes)
            || !checked_add(
                request_capacity_bytes,
                range_capacity_bytes,
                wave_capacity_bytes)
            || wave_capacity_bytes > posting_wave_owner_ceiling
            || !reader.refresh_persistent_owner_bytes_()
            || !checked_add(
                reader.persistent_owner_bytes_,
                authority_owner_bytes,
                current_owner_bytes)
            || !checked_add(
                current_owner_bytes,
                wave_capacity_bytes,
                current_owner_bytes)
            || !checked_add(
                current_owner_bytes,
                fallback_slab_bytes,
                current_owner_bytes)
            || current_owner_bytes > reader.permit_.bytes()) {
            return std::unexpected(
                hash_major_minimizer_error::admission);
        }
        reader.permit_.shrink_to(current_owner_bytes);
        reader.posting_wave_peak_owner_bytes_ =
            static_cast<std::size_t>(wave_capacity_bytes);
        crc64_ecma182_accumulator posting_stream_crc;
        std::uint64_t posting_stream_next = 0u;
        bool posting_stream_complete = true;
        std::size_t query_cursor = 0u;
        std::size_t authority_cursor = 0u;
        std::size_t accepted_delivered = 0u;
        std::uint64_t output_cursor = 0u;
        const auto next_accepted_query =
            [&reader, query_count](std::size_t ordinal) noexcept {
                while (ordinal < query_count
                       && reader.query_offsets_[ordinal]
                           >= k_unavailable_query_offset) {
                    ++ordinal;
                }
                return ordinal;
            };
        while (accepted_delivered < accepted_hash_count) {
            requests.clear();
            ranges.clear();
            const std::size_t wave_accepted_begin = accepted_delivered;
            while (authority_cursor < authorities.size()) {
                const block_authority& authority =
                    authorities[authority_cursor];
                std::uint64_t authority_end = 0u;
                if (!checked_add(
                        authority.posting_byte_offset,
                        authority.posting_byte_length,
                        authority_end)) {
                    return std::unexpected(
                        hash_major_minimizer_error::arithmetic_overflow);
                }
                query_cursor = next_accepted_query(query_cursor);
                if (query_cursor == query_count) break;
                if (reader.query_offsets_[query_cursor] >= authority_end) {
                    ++authority_cursor;
                    continue;
                }
                if (reader.query_offsets_[query_cursor]
                    < authority.posting_byte_offset) {
                    return std::unexpected(hash_major_minimizer_error::posting);
                }
                std::size_t scan_query = query_cursor;
                std::size_t block_requests = 0u;
                for (;;) {
                    scan_query = next_accepted_query(scan_query);
                    if (scan_query == query_count
                        || reader.query_offsets_[scan_query]
                            >= authority_end) {
                        break;
                    }
                    if (reader.query_offsets_[scan_query]
                        < authority.posting_byte_offset) {
                        return std::unexpected(
                            hash_major_minimizer_error::posting);
                    }
                    ++block_requests;
                    ++scan_query;
                }
                const std::size_t block_next_query = scan_query;
                if (block_requests == 0u) {
                    ++authority_cursor;
                    continue;
                }
                if (!requests.empty()
                    && requests.size() + block_requests
                        > wave_request_capacity) {
                    break;
                }
                if (requests.size() + block_requests
                    > wave_request_capacity) {
                    return std::unexpected(hash_major_minimizer_error::state);
                }
                const std::size_t block_request_begin = requests.size();
                for (std::size_t query_ordinal = query_cursor;
                     query_ordinal < block_next_query;
                     ++query_ordinal) {
                    const std::uint64_t posting_offset =
                        reader.query_offsets_[query_ordinal];
                    if (posting_offset >= k_unavailable_query_offset) continue;
                    const std::uint64_t count =
                        reader.exact_count_(query_ordinal);
                    std::uint64_t posting_bytes = 0u;
                    std::uint64_t posting_end = 0u;
                    if (count == 0u
                        || !checked_multiply(
                            count,
                            manifest->storage_profile
                                .stored_posting_width_bytes,
                            posting_bytes)
                        || !checked_add(
                            posting_offset,
                            posting_bytes,
                            posting_end)
                        || posting_offset
                            < authority.posting_byte_offset
                        || posting_end > authority_end) {
                        return std::unexpected(
                            count == 0u
                                ? hash_major_minimizer_error::count
                                : hash_major_minimizer_error::posting);
                    }
                    requests.push_back(posting_request{
                        .posting_byte_offset = posting_offset,
                        .exact_count = count,
                        .query_ordinal =
                            static_cast<std::uint32_t>(query_ordinal),
                        .flags = 0u,
                    });
                    reader.query_offsets_[query_ordinal] = output_cursor;
                    if (!checked_add(output_cursor, count, output_cursor)) {
                        return std::unexpected(
                            hash_major_minimizer_error::arithmetic_overflow);
                    }
                }
                const std::size_t block_request_end = requests.size();
                if (block_request_end - block_request_begin
                    != block_requests) {
                    return std::unexpected(hash_major_minimizer_error::state);
                }
                for (std::size_t begin = block_request_begin;
                     begin < block_request_end;) {
                    std::uint64_t first_request_bytes = 0u;
                    std::uint64_t range_end = 0u;
                    if (!checked_multiply(
                            requests[begin].exact_count,
                            manifest->storage_profile
                                .stored_posting_width_bytes,
                            first_request_bytes)
                        || !checked_add(
                            requests[begin].posting_byte_offset,
                            first_request_bytes,
                            range_end)) {
                        return std::unexpected(
                            hash_major_minimizer_error::arithmetic_overflow);
                    }
                    std::size_t end = begin + 1u;
                    while (end < block_request_end
                           && requests[end].posting_byte_offset
                               == range_end) {
                        std::uint64_t next_request_bytes = 0u;
                        if (!checked_multiply(
                                requests[end].exact_count,
                                manifest->storage_profile
                                    .stored_posting_width_bytes,
                                next_request_bytes)
                            || !checked_add(
                                range_end,
                                next_request_bytes,
                                range_end)) {
                            return std::unexpected(
                                hash_major_minimizer_error::
                                    arithmetic_overflow);
                        }
                        ++end;
                    }
                    if (requests[begin].posting_byte_offset
                            < authority.posting_byte_offset
                        || range_end > authority_end) {
                        return std::unexpected(
                            hash_major_minimizer_error::posting);
                    }
                    const bool complete_block =
                        block_request_begin == begin
                        && end == block_request_end
                        && requests[begin].posting_byte_offset
                            == authority.posting_byte_offset
                        && range_end == authority_end;
                    ranges.push_back(
                        hash_major_minimizer_authorized_posting_range{
                            .control_block_ordinal =
                                authority.directory_ordinal,
                            .posting_byte_offset =
                                requests[begin].posting_byte_offset,
                            .byte_count = range_end
                                - requests[begin].posting_byte_offset,
                            .expected_posting_crc64 =
                                authority.posting_crc64,
                            .request_begin =
                                static_cast<std::uint32_t>(begin),
                            .request_count =
                                static_cast<std::uint32_t>(end - begin),
                            .flags = complete_block ? 1u : 0u,
                        });
                    begin = end;
                }
                accepted_delivered += block_requests;
                query_cursor = block_next_query;
                ++authority_cursor;
            }
            if (requests.empty()
                || ranges.empty()
                || requests.size() > wave_request_capacity) {
                return std::unexpected(hash_major_minimizer_error::state);
            }
            posting_delivery_context delivery{
                .reader = &reader,
                .manifest = &*manifest,
                .requests = &requests,
                .ranges = &ranges,
                .next_request = ranges.front().request_begin,
                .posting_stream_crc = &posting_stream_crc,
                .posting_stream_next = &posting_stream_next,
                .posting_stream_complete = &posting_stream_complete,
            };
            bool delivered = false;
            if (source.read_posting_wave != nullptr) {
                delivered = source.read_posting_wave(
                    source.context,
                    ranges,
                    &delivery,
                    &consume_posting_bytes_);
            } else {
                ++reader.memory_stats_.
                    generic_source_fallback_calls;
                delivered = true;
                for (std::size_t range_ordinal = 0u;
                     delivered && range_ordinal < ranges.size();
                     ++range_ordinal) {
                    const auto& range = ranges[range_ordinal];
                    std::uint64_t relative = 0u;
                    while (relative < range.byte_count) {
                        const std::size_t bytes = static_cast<std::size_t>(
                            std::min<std::uint64_t>(
                                k_hash_major_minimizer_read_slab_bytes,
                                range.byte_count - relative));
                        const auto slab = std::span<std::byte>{
                            fallback_slab.get(),
                            k_hash_major_minimizer_read_slab_bytes}.first(bytes);
                        if (!source.read(
                                hash_major_minimizer_stream_kind::postings,
                                range.posting_byte_offset + relative,
                                slab)
                            || !consume_posting_bytes_(
                                &delivery,
                                static_cast<std::uint32_t>(range_ordinal),
                                relative,
                                slab)) {
                            delivered = false;
                            break;
                        }
                        relative += bytes;
                    }
                }
            }
            if (!delivered || delivery.error
                || delivery.next_range != ranges.size()
                || delivery.next_request != requests.size()) {
                return std::unexpected(delivery.error.value_or(
                    hash_major_minimizer_error::source_read));
            }
            std::uint64_t delivered_range_bytes = 0u;
            for (const auto& range : ranges) {
                if (!checked_add(
                        delivered_range_bytes,
                        range.byte_count,
                        delivered_range_bytes)) {
                    return std::unexpected(
                        hash_major_minimizer_error::arithmetic_overflow);
                }
            }
            if (!checked_add(
                    reader.memory_stats_.authorized_posting_bytes,
                    delivered_range_bytes,
                    delivered_range_bytes)) {
                return std::unexpected(
                    hash_major_minimizer_error::arithmetic_overflow);
            }
            reader.memory_stats_.authorized_posting_bytes =
                delivered_range_bytes;
            if (accepted_delivered == wave_accepted_begin) {
                return std::unexpected(hash_major_minimizer_error::state);
            }
        }
        if (accepted_delivered != accepted_hash_count
            || output_cursor != retained_count) {
            return std::unexpected(hash_major_minimizer_error::state);
        }
        std::vector<posting_request>{}.swap(requests);
        std::vector<hash_major_minimizer_authorized_posting_range>{}.swap(ranges);
        fallback_slab.reset();
        std::vector<block_authority>{}.swap(authorities);
        if (posting_stream_complete
            && posting_stream_next == manifest->total_posting_bytes
            && posting_stream_crc.checksum()
                != manifest->streams[2].stream_crc64) {
            return std::unexpected(hash_major_minimizer_error::checksum);
        }
        if (!reader.refresh_persistent_owner_bytes_()) {
            return std::unexpected(
                hash_major_minimizer_error::arithmetic_overflow);
        }
        reader.permit_.shrink_to(reader.persistent_owner_bytes_);
        reader.memory_stats_.retained_owner_bytes =
            reader.persistent_owner_bytes_;
        return reader;
    }

    [[nodiscard]] minimizer_lookup_spans lookup_spans(
            std::uint64_t hash) const {
        const std::size_t query_ordinal = canonical_ordinal_(hash);
        if (query_ordinal == query_hashes_.size()) {
            throw persisted_minimizer_lookup_error{
                "Hash-major lookup hash is absent from the authenticated "
                "canonical query image"};
        }
        const std::uint16_t primary = primary_counts_[query_ordinal];
        if (primary == 0u) {
            return {};
        }
        const std::uint64_t count = exact_count_(query_ordinal);
        if (count == 0u) {
            throw persisted_minimizer_lookup_error{
                "Hash-major exact-count exception is missing"};
        }
        const std::uint64_t offset = query_offsets_[query_ordinal];
        if (offset == k_unavailable_query_offset) {
            return minimizer_lookup_spans::positions_unavailable(count);
        }
        if (offset == k_absent_query_offset
            || offset > retained_position_count_
            || count > retained_position_count_ - offset) {
            throw persisted_minimizer_lookup_error{
                "Hash-major retained-position offset is inconsistent"};
        }
        const std::size_t position_offset =
            static_cast<std::size_t>(offset);
        const std::size_t position_count =
            static_cast<std::size_t>(count);
        if (manifest_.storage_profile.stored_posting_width_bytes
                == sizeof(std::uint32_t)) {
            return minimizer_lookup_spans::contiguous(
                std::span<const std::uint32_t>{
                    retained_positions32_.get(),
                    retained_position_count_}.subspan(
                    position_offset, position_count));
        }
        return minimizer_lookup_spans::contiguous(
            std::span<const std::uint64_t>{
                retained_positions64_.get(),
                retained_position_count_}.subspan(
                position_offset, position_count));
    }

    [[nodiscard]] std::uint64_t target_length() const noexcept {
        return manifest_.target_length;
    }

    [[nodiscard]] std::span<const std::uint32_t>
    retained_positions32() const noexcept {
        return {
            retained_positions32_.get(),
            retained_positions32_ ? retained_position_count_ : 0u};
    }

    [[nodiscard]] std::span<const std::uint64_t>
    retained_positions64() const noexcept {
        return {
            retained_positions64_.get(),
            retained_positions64_ ? retained_position_count_ : 0u};
    }

    [[nodiscard]] std::span<const exact_global_count_exception>
    exact_count_exceptions() const noexcept {
        return count_exceptions_;
    }

    [[nodiscard]] std::size_t selected_hash_count() const noexcept {
        return selected_hash_count_;
    }

    [[nodiscard]] bool contains_prepared_hash(
            std::uint64_t hash) const noexcept {
        return canonical_contains_(hash);
    }

    [[nodiscard]] std::size_t posting_wave_peak_owner_bytes()
            const noexcept {
        return posting_wave_peak_owner_bytes_;
    }

    [[nodiscard]] std::size_t control_wave_peak_owner_bytes()
            const noexcept {
        return control_wave_peak_owner_bytes_;
    }

    [[nodiscard]] std::size_t transient_authority_peak_owner_bytes()
            const noexcept {
        return transient_authority_peak_owner_bytes_;
    }

    [[nodiscard]] static constexpr std::size_t
    transient_authority_retained_bytes() noexcept {
        return 0u;
    }

    [[nodiscard]] std::uint64_t admitted_bytes() const noexcept {
        return permit_.bytes();
    }

    [[nodiscard]] const hash_major_minimizer_memory_stats&
    memory_stats() const noexcept {
        return memory_stats_;
    }
};

static_assert(segmented_minimizer_lookup_index<hash_major_minimizer_selective_reader>);
static_assert(minimizer_lookup_index<hash_major_minimizer_selective_reader>);

} // namespace bioseq::indexing

export namespace bioseq::indexing {

/// Callback that appends bytes to one logical stream without reordering them.
using hash_major_minimizer_append_logical_bytes_fn = bool (*)(
    void*, hash_major_minimizer_stream_kind, std::span<const std::byte>);

/** @brief Non-owning append callback for the three logical output streams. */
struct hash_major_minimizer_logical_stream_sink_ref {
    void* context = nullptr;
    hash_major_minimizer_append_logical_bytes_fn append_bytes = nullptr;

    [[nodiscard]] bool append(
            hash_major_minimizer_stream_kind kind,
            std::span<const std::byte> bytes) const {
        return append_bytes != nullptr && append_bytes(context, kind, bytes);
    }
};

/** @brief Decoded and byte-exact encoded publication metadata. */
struct hash_major_minimizer_encoded_metadata {
    hash_major_minimizer_head head{};
    hash_major_minimizer_manifest manifest{};
    encoded_hash_major_minimizer_head encoded_head{};
    encoded_hash_major_minimizer_manifest encoded_manifest{};
};

/** @brief Streaming hash-major encoder that emits ordered authenticated streams. */
class hash_major_minimizer_logical_encoder {
private:
    hash_major_minimizer_generation_options options_{};
    hash_major_minimizer_logical_stream_sink_ref sink_{};
    std::array<crc64_ecma182_accumulator, 3> stream_crc_{};
    std::array<std::uint64_t, 3> stream_bytes_{};
    std::array<std::byte, k_hash_major_minimizer_row_payload_bytes> posting_buffer_{};
    std::size_t posting_buffer_bytes_ = 0u;

    std::vector<std::uint64_t> block_hashes_{};
    std::vector<std::uint64_t> block_counts_{};
    std::uint64_t block_posting_byte_offset_ = 0u;
    std::uint64_t block_first_posting_ordinal_ = 0u;
    crc64_ecma182_accumulator block_posting_crc_{};

    std::uint64_t unique_hash_count_ = 0u;
    std::uint64_t total_posting_count_ = 0u;
    std::uint64_t directory_entry_count_ = 0u;
    std::uint64_t previous_hash_ = 0u;
    bool have_previous_hash_ = false;

    std::uint64_t current_hash_ = 0u;
    std::uint64_t current_expected_count_ = 0u;
    std::uint64_t current_written_count_ = 0u;
    std::uint64_t current_previous_posting_ = 0u;
    bool current_have_posting_ = false;
    bool hash_open_ = false;
    bool finished_ = false;

    [[nodiscard]] static constexpr std::size_t stream_index_(
            hash_major_minimizer_stream_kind kind) noexcept {
        return static_cast<std::size_t>(kind) - 1u;
    }

    [[nodiscard]] std::expected<void, hash_major_minimizer_error> emit_(
            hash_major_minimizer_stream_kind kind,
            std::span<const std::byte> bytes) {
        const std::size_t index = stream_index_(kind);
        std::uint64_t next = 0u;
        if (bytes.empty()
            || !hash_major_minimizer_detail::checked_add(
                stream_bytes_[index], bytes.size(), next)) {
            return std::unexpected(
                bytes.empty() ? hash_major_minimizer_error::sink_write
                              : hash_major_minimizer_error::arithmetic_overflow);
        }
        if (!sink_.append(kind, bytes)) {
            return std::unexpected(hash_major_minimizer_error::sink_write);
        }
        stream_crc_[index].update(bytes);
        stream_bytes_[index] = next;
        return {};
    }

    [[nodiscard]] std::expected<void, hash_major_minimizer_error>
    flush_posting_buffer_() {
        if (posting_buffer_bytes_ == 0u) return {};
        if (!sink_.append(
                hash_major_minimizer_stream_kind::postings,
                std::span<const std::byte>{posting_buffer_}.first(
                    posting_buffer_bytes_))) {
            return std::unexpected(hash_major_minimizer_error::sink_write);
        }
        posting_buffer_bytes_ = 0u;
        return {};
    }

    [[nodiscard]] std::expected<void, hash_major_minimizer_error>
    append_posting_bytes_(std::span<const std::byte> bytes) {
        const std::size_t index = stream_index_(
            hash_major_minimizer_stream_kind::postings);
        std::uint64_t next = 0u;
        if (!hash_major_minimizer_detail::checked_add(
                stream_bytes_[index], bytes.size(), next)) {
            return std::unexpected(hash_major_minimizer_error::arithmetic_overflow);
        }
        if (posting_buffer_bytes_ + bytes.size() > posting_buffer_.size()) {
            if (auto flushed = flush_posting_buffer_(); !flushed) {
                return flushed;
            }
        }
        std::copy(
            bytes.begin(), bytes.end(),
            posting_buffer_.begin() + posting_buffer_bytes_);
        posting_buffer_bytes_ += bytes.size();
        stream_crc_[index].update(bytes);
        block_posting_crc_.update(bytes);
        stream_bytes_[index] = next;
        return {};
    }

    [[nodiscard]] std::expected<void, hash_major_minimizer_error>
    finish_block_() {
        using namespace hash_major_minimizer_detail;
        if (block_hashes_.empty()) return {};
        if (hash_open_) {
            return std::unexpected(hash_major_minimizer_error::state);
        }
        const std::uint8_t posting_width =
            options_.storage_profile.stored_posting_width_bytes;
        auto control = encode_hash_major_minimizer_control_block(
            block_hashes_,
            block_counts_,
            block_posting_byte_offset_,
            block_posting_crc_.checksum(),
            options_.storage_profile.hash_width_bytes,
            posting_width);
        if (!control) return std::unexpected(control.error());
        const std::uint64_t control_offset = stream_bytes_[stream_index_(
            hash_major_minimizer_stream_kind::controls)];
        if (control->size() > (std::numeric_limits<std::uint32_t>::max)()) {
            return std::unexpected(hash_major_minimizer_error::arithmetic_overflow);
        }
        const std::uint64_t block_posting_bytes =
            stream_bytes_[stream_index_(hash_major_minimizer_stream_kind::postings)]
            - block_posting_byte_offset_;
        std::uint64_t block_posting_count = 0u;
        for (const std::uint64_t count : block_counts_) {
            if (!checked_add(
                    block_posting_count, count, block_posting_count)) {
                return std::unexpected(
                    hash_major_minimizer_error::arithmetic_overflow);
            }
        }
        std::uint64_t expected_posting_bytes = 0u;
        if (!checked_multiply(
                block_posting_count,
                posting_width,
                expected_posting_bytes)
            || expected_posting_bytes != block_posting_bytes) {
            return std::unexpected(hash_major_minimizer_error::count);
        }
        // Publication order is postings first, then the control/directory
        // authority that makes those bytes discoverable.  In particular, a
        // partial final posting row must not remain buffered while metadata
        // for this block becomes externally visible.
        if (auto flushed = flush_posting_buffer_(); !flushed) {
            return flushed;
        }
        hash_major_minimizer_directory_entry entry{
            .first_hash = block_hashes_.front(),
            .last_hash = block_hashes_.back(),
            .control_block_ordinal = directory_entry_count_,
            .control_byte_offset = control_offset,
            .control_byte_length = static_cast<std::uint32_t>(control->size()),
            .control_count = static_cast<std::uint32_t>(block_hashes_.size()),
            .first_posting_ordinal = block_first_posting_ordinal_,
            .posting_byte_offset = block_posting_byte_offset_,
            .posting_byte_length = block_posting_bytes,
            .exact_posting_count = block_posting_count,
        };
        const std::uint64_t control_crc = read_le<std::uint64_t>(*control, 48u);
        entry.block_crc64 = hash_major_minimizer_block_crc64(
            entry, control_crc, block_posting_crc_.checksum());
        auto encoded_directory =
            encode_hash_major_minimizer_directory_entry(entry);
        if (!encoded_directory) {
            return std::unexpected(encoded_directory.error());
        }
        if (auto emitted = emit_(
                hash_major_minimizer_stream_kind::controls, *control);
            !emitted) {
            return emitted;
        }
        if (auto emitted = emit_(
                hash_major_minimizer_stream_kind::directory, *encoded_directory);
            !emitted) {
            return emitted;
        }
        if (directory_entry_count_
            == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(hash_major_minimizer_error::arithmetic_overflow);
        }
        ++directory_entry_count_;
        block_hashes_.clear();
        block_counts_.clear();
        block_posting_byte_offset_ = stream_bytes_[stream_index_(
            hash_major_minimizer_stream_kind::postings)];
        block_first_posting_ordinal_ = total_posting_count_;
        block_posting_crc_ = {};
        return {};
    }

public:
    hash_major_minimizer_logical_encoder(
            hash_major_minimizer_generation_options options,
            hash_major_minimizer_logical_stream_sink_ref sink)
        : options_(options), sink_(sink) {
        block_hashes_.reserve(k_hash_major_minimizer_controls_per_block);
        block_counts_.reserve(k_hash_major_minimizer_controls_per_block);
    }

    hash_major_minimizer_logical_encoder(
        const hash_major_minimizer_logical_encoder&) = delete;
    hash_major_minimizer_logical_encoder& operator=(
        const hash_major_minimizer_logical_encoder&) = delete;

    [[nodiscard]] std::expected<void, hash_major_minimizer_error> begin_hash(
            std::uint64_t hash,
            std::uint64_t exact_count) {
        if (finished_ || hash_open_ || exact_count == 0u
            || !is_valid_hash_major_minimizer_storage_profile(
                options_.storage_profile, options_.target_length)
            || hash > hash_major_minimizer_max_hash(options_.storage_profile)
            || (have_previous_hash_ && hash <= previous_hash_)) {
            return std::unexpected(
                exact_count == 0u ? hash_major_minimizer_error::count
                                  : hash_major_minimizer_error::state);
        }
        if (block_hashes_.size() == k_hash_major_minimizer_controls_per_block) {
            if (auto finished = finish_block_(); !finished) return finished;
        }
        if (block_hashes_.empty()) {
            block_posting_byte_offset_ = stream_bytes_[stream_index_(
                hash_major_minimizer_stream_kind::postings)];
            block_first_posting_ordinal_ = total_posting_count_;
        }
        block_hashes_.push_back(hash);
        block_counts_.push_back(exact_count);
        current_hash_ = hash;
        current_expected_count_ = exact_count;
        current_written_count_ = 0u;
        current_previous_posting_ = 0u;
        current_have_posting_ = false;
        hash_open_ = true;
        return {};
    }

    [[nodiscard]] std::expected<void, hash_major_minimizer_error> append_posting(
            std::uint64_t encoded_position) {
        if (!hash_open_ || current_written_count_ >= current_expected_count_) {
            return std::unexpected(hash_major_minimizer_error::state);
        }
        const std::uint64_t global_bp = encoded_position >> 1u;
        if (global_bp >= options_.target_length
            || (current_have_posting_
                && encoded_position <= current_previous_posting_)
            || (options_.storage_profile.stored_posting_width_bytes == 4u
                && encoded_position
                    > (std::numeric_limits<std::uint32_t>::max)())) {
            return std::unexpected(
                global_bp >= options_.target_length
                    ? hash_major_minimizer_error::target_range
                    : hash_major_minimizer_error::order);
        }
        std::array<std::byte, 8> encoded{};
        if (options_.storage_profile.stored_posting_width_bytes == 4u) {
            hash_major_minimizer_detail::write_le<std::uint32_t>(
                encoded, 0u, static_cast<std::uint32_t>(encoded_position));
        } else {
            hash_major_minimizer_detail::write_le(encoded, 0u, encoded_position);
        }
        if (auto appended = append_posting_bytes_(std::span<const std::byte>{
                encoded.data(),
                options_.storage_profile.stored_posting_width_bytes});
            !appended) {
            return appended;
        }
        current_previous_posting_ = encoded_position;
        current_have_posting_ = true;
        ++current_written_count_;
        return {};
    }

    [[nodiscard]] std::expected<void, hash_major_minimizer_error> end_hash() {
        if (!hash_open_ || current_written_count_ != current_expected_count_) {
            return std::unexpected(hash_major_minimizer_error::count);
        }
        std::uint64_t next_total = 0u;
        if (!hash_major_minimizer_detail::checked_add(
                total_posting_count_,
                current_expected_count_,
                next_total)) {
            return std::unexpected(hash_major_minimizer_error::arithmetic_overflow);
        }
        total_posting_count_ = next_total;
        if (unique_hash_count_
            == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(hash_major_minimizer_error::arithmetic_overflow);
        }
        ++unique_hash_count_;
        previous_hash_ = current_hash_;
        have_previous_hash_ = true;
        hash_open_ = false;
        return {};
    }

    [[nodiscard]] std::expected<void, hash_major_minimizer_error> append_hash(
            std::uint64_t hash,
            std::span<const std::uint64_t> positions) {
        if (auto begun = begin_hash(hash, positions.size()); !begun) {
            return begun;
        }
        for (const std::uint64_t position : positions) {
            if (auto appended = append_posting(position); !appended) {
                return appended;
            }
        }
        return end_hash();
    }

    [[nodiscard]] std::expected<
        hash_major_minimizer_encoded_metadata, hash_major_minimizer_error>
    finish() {
        using namespace hash_major_minimizer_detail;
        if (finished_ || hash_open_ || options_.generation_id == 0u
            || !is_valid_hash_major_minimizer_storage_profile(
                options_.storage_profile, options_.target_length)) {
            return std::unexpected(hash_major_minimizer_error::state);
        }
        if (auto block = finish_block_(); !block) {
            return std::unexpected(block.error());
        }
        if (auto flushed = flush_posting_buffer_(); !flushed) {
            return std::unexpected(flushed.error());
        }
        const auto identity = hash_major_minimizer_source_identity(
            options_.query_key,
            options_.source_epoch,
            options_.source_content_crc64,
            options_.target_length);
        const auto storage_profile_digest =
            hash_major_minimizer_storage_profile_digest(options_.storage_profile);
        hash_major_minimizer_manifest manifest{
            .generation_id = options_.generation_id,
            .source_epoch = options_.source_epoch,
            .source_identity = identity,
            .source_content_crc64 = options_.source_content_crc64,
            .target_length = options_.target_length,
            .unique_hash_count = unique_hash_count_,
            .total_posting_count = total_posting_count_,
            .storage_profile = options_.storage_profile,
            .storage_profile_digest = storage_profile_digest,
            .authenticated_directory_crc64 =
                stream_crc_[stream_index_(
                    hash_major_minimizer_stream_kind::directory)].checksum(),
            .total_control_bytes = stream_bytes_[stream_index_(
                hash_major_minimizer_stream_kind::controls)],
            .total_posting_bytes = stream_bytes_[stream_index_(
                hash_major_minimizer_stream_kind::postings)],
            .directory_entry_count = directory_entry_count_,
        };
        const std::uint64_t directory_rows = ceil_div(
            stream_bytes_[0], k_hash_major_minimizer_row_payload_bytes);
        const std::uint64_t control_rows = ceil_div(
            stream_bytes_[1], k_hash_major_minimizer_row_payload_bytes);
        const std::uint64_t posting_rows = ceil_div(
            stream_bytes_[2], k_hash_major_minimizer_row_payload_bytes);
        std::uint64_t posting_first_row = 0u;
        std::uint64_t total_rows = 0u;
        if (!checked_add(
                directory_rows, control_rows, posting_first_row)
            || !checked_add(posting_first_row, posting_rows, total_rows)) {
            return std::unexpected(
                hash_major_minimizer_error::arithmetic_overflow);
        }
        manifest.streams = {
            hash_major_minimizer_stream_descriptor{
                .kind = hash_major_minimizer_stream_kind::directory,
                .encoding_revision = 1u,
                .element_width = static_cast<std::uint8_t>(
                    k_hash_major_minimizer_directory_entry_bytes),
                .element_count = directory_entry_count_,
                .logical_bytes = stream_bytes_[0],
                .row_count = directory_rows,
                .stream_crc64 = stream_crc_[0].checksum(),
                .first_row_ordinal = 0u,
            },
            hash_major_minimizer_stream_descriptor{
                .kind = hash_major_minimizer_stream_kind::controls,
                .encoding_revision = 1u,
                .element_width = 0u,
                .element_count = unique_hash_count_,
                .logical_bytes = stream_bytes_[1],
                .row_count = control_rows,
                .stream_crc64 = stream_crc_[1].checksum(),
                .first_row_ordinal = directory_rows,
            },
            hash_major_minimizer_stream_descriptor{
                .kind = hash_major_minimizer_stream_kind::postings,
                .encoding_revision = 1u,
                .element_width = options_.storage_profile
                    .stored_posting_width_bytes,
                .element_count = total_posting_count_,
                .logical_bytes = stream_bytes_[2],
                .row_count = posting_rows,
                .stream_crc64 = stream_crc_[2].checksum(),
                .first_row_ordinal = posting_first_row,
            },
        };
        auto encoded_manifest = encode_hash_major_minimizer_manifest(manifest);
        if (!encoded_manifest) {
            return std::unexpected(encoded_manifest.error());
        }
        auto decoded_manifest = decode_hash_major_minimizer_manifest(
            *encoded_manifest);
        if (!decoded_manifest) {
            return std::unexpected(decoded_manifest.error());
        }
        manifest = *decoded_manifest;
        hash_major_minimizer_head head{
            .generation_id = options_.generation_id,
            .query_key = options_.query_key,
            .source_epoch = options_.source_epoch,
            .source_identity = identity,
            .source_content_crc64 = options_.source_content_crc64,
            .storage_profile_digest = storage_profile_digest,
            .manifest_address_bound_row_crc64 = crc64_index_chunk_row(
                options_.generation_id,
                0,
                0,
                0u,
                static_cast<std::uint32_t>(encoded_manifest->size()),
                *encoded_manifest),
            .manifest_payload_crc64 = crc64_ecma182(*encoded_manifest),
        };
        auto encoded_head = encode_hash_major_minimizer_head(head);
        if (!encoded_head) return std::unexpected(encoded_head.error());
        auto decoded_head = decode_hash_major_minimizer_head(*encoded_head);
        if (!decoded_head) return std::unexpected(decoded_head.error());
        finished_ = true;
        return hash_major_minimizer_encoded_metadata{
            .head = *decoded_head,
            .manifest = manifest,
            .encoded_head = *encoded_head,
            .encoded_manifest = *encoded_manifest,
        };
    }
};

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
