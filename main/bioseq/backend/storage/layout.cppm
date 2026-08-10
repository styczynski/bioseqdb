/**
 * @file
 * @brief PostgreSQL-free sequence locator layout and length-bucket routing.
 */
module;

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>

export module bioseq.storage.layout;

import bioseq.common.checked_arithmetic;

export namespace bioseq::storage {

/** Serialized locator payload discriminator. Its value is a storage contract. */
inline constexpr std::uint8_t sequence_locator_payload_version = 3u;

/** Width-byte marker indicating that a flat sequence value holds a locator. */
inline constexpr std::uint8_t sequence_locator_stub_width_marker = 0x80u;

[[nodiscard]] constexpr bool is_sequence_locator_stub_width(
    std::uint8_t width) noexcept
{
    return (width & sequence_locator_stub_width_marker) != 0u;
}

/**
 * Frozen 40-byte locator payload embedded in PostgreSQL sequence stubs.
 *
 * The record remains standard-layout and trivially copyable because persisted
 * payloads use its existing host-byte-order object representation. Renaming
 * fields does not alter offsets or bytes.
 */
struct sequence_locator {
    std::uint8_t payload_version = sequence_locator_payload_version;
    std::uint8_t bucket_index = 0u;
    std::uint16_t reserved_16 = 0u;
    std::uint32_t shard_index = 0u;
    std::uint32_t fragment_count = 0u;
    std::uint32_t reserved_32 = 0u;
    std::uint64_t logical_length = 0u;
    std::uint64_t first_fragment_row = 0u;
    std::uint64_t source_epoch = 0u;
};

static_assert(sizeof(sequence_locator) == 40u);
static_assert(std::is_standard_layout_v<sequence_locator>);
static_assert(std::is_trivially_copyable_v<sequence_locator>);
static_assert(offsetof(sequence_locator, fragment_count) == 8u);
static_assert(offsetof(sequence_locator, logical_length) == 16u);
static_assert(offsetof(sequence_locator, first_fragment_row) == 24u);
static_assert(offsetof(sequence_locator, source_epoch) == 32u);
static_assert(
    static_cast<std::uint64_t>(
        (std::numeric_limits<std::uint32_t>::max)())
        * std::uint64_t{32768u}
    >= std::uint64_t{1'000'000'000'000ULL});

/** Encode the exact persisted payload without owning PostgreSQL framing. */
[[nodiscard]] inline bool encode_sequence_locator_payload(
    std::span<std::byte> output,
    const sequence_locator& locator) noexcept
{
    if (output.size() != sizeof(sequence_locator)
        || locator.payload_version != sequence_locator_payload_version
        || locator.reserved_16 != 0u
        || locator.reserved_32 != 0u) {
        return false;
    }
    std::memcpy(output.data(), std::addressof(locator), sizeof(locator));
    return true;
}

/** Decode and validate the exact persisted payload representation. */
[[nodiscard]] inline std::optional<sequence_locator>
decode_sequence_locator_payload(std::span<const std::byte> payload) noexcept
{
    if (payload.size() != sizeof(sequence_locator)) {
        return std::nullopt;
    }
    sequence_locator locator{};
    std::memcpy(std::addressof(locator), payload.data(), sizeof(locator));
    if (locator.payload_version != sequence_locator_payload_version
        || locator.reserved_16 != 0u
        || locator.reserved_32 != 0u) {
        return std::nullopt;
    }
    return locator;
}

/** Exact reason a locator cannot be consumed by flat PostgreSQL storage. */
enum class sequence_locator_validation_error : std::uint8_t {
    none = 0u,
    invalid_payload_metadata,
    logical_length_exceeds_sql_bigint,
    logical_length_exceeds_flat_storage,
    shard_index_exceeds_catalog,
    first_fragment_row_exceeds_catalog,
    inconsistent_fragment_geometry,
    fragment_row_range_exceeds_catalog,
};

/** Validate all narrowing and fragment-row geometry before opening a shard. */
[[nodiscard]] constexpr sequence_locator_validation_error
validate_sequence_locator(const sequence_locator& locator) noexcept
{
    constexpr auto sql_bigint_max = static_cast<std::uint64_t>(
        (std::numeric_limits<std::int64_t>::max)());
    if (locator.payload_version != sequence_locator_payload_version
        || locator.reserved_16 != 0u
        || locator.reserved_32 != 0u
        || locator.source_epoch == 0u
        || locator.source_epoch > sql_bigint_max) {
        return sequence_locator_validation_error::invalid_payload_metadata;
    }
    if (locator.logical_length > sql_bigint_max) {
        return sequence_locator_validation_error::
            logical_length_exceeds_sql_bigint;
    }
    if (locator.logical_length
        > static_cast<std::uint64_t>(
            (std::numeric_limits<std::uint32_t>::max)())) {
        return sequence_locator_validation_error::
            logical_length_exceeds_flat_storage;
    }
    if (locator.shard_index
        > static_cast<std::uint32_t>(
            (std::numeric_limits<std::int32_t>::max)())) {
        return sequence_locator_validation_error::shard_index_exceeds_catalog;
    }
    if (locator.first_fragment_row > sql_bigint_max) {
        return sequence_locator_validation_error::
            first_fragment_row_exceeds_catalog;
    }
    if ((locator.fragment_count == 0u) != (locator.logical_length == 0u)
        || (locator.fragment_count == 0u
            && locator.first_fragment_row != 0u)) {
        return sequence_locator_validation_error::
            inconsistent_fragment_geometry;
    }
    if (locator.fragment_count > 0u
        && locator.first_fragment_row
            > sql_bigint_max
                - static_cast<std::uint64_t>(locator.fragment_count - 1u)) {
        return sequence_locator_validation_error::
            fragment_row_range_exceeds_catalog;
    }
    return sequence_locator_validation_error::none;
}

/** Compute the number of fixed-size fragments needed for a logical length. */
[[nodiscard]] constexpr std::uint64_t expected_sequence_fragment_count(
    std::uint64_t logical_length,
    std::uint64_t letters_per_unit,
    std::uint64_t fragment_storage_units) noexcept
{
    if (logical_length == 0u) {
        return 0u;
    }
    if (letters_per_unit == 0u || fragment_storage_units == 0u) {
        return (std::numeric_limits<std::uint64_t>::max)();
    }
    const std::uint64_t storage_units = bioseq::common::ceil_div(
        logical_length, letters_per_unit);
    return bioseq::common::ceil_div(storage_units, fragment_storage_units);
}

enum class fragment_frame_error : std::uint8_t {
    none = 0u,
    shorter_than_header,
    size_mismatch,
};

[[nodiscard]] constexpr fragment_frame_error validate_fragment_frame(
    std::uint64_t actual_bytes,
    std::uint64_t header_bytes,
    std::uint64_t expected_bytes) noexcept
{
    if (actual_bytes < header_bytes) {
        return fragment_frame_error::shorter_than_header;
    }
    if (actual_bytes != expected_bytes) {
        return fragment_frame_error::size_mismatch;
    }
    return fragment_frame_error::none;
}

enum class fragment_metadata_error : std::uint8_t {
    none = 0u,
    type_mismatch,
    logical_length_mismatch,
    padding_mismatch,
};

[[nodiscard]] constexpr fragment_metadata_error validate_fragment_metadata(
    std::uint8_t actual_alphabet,
    std::uint8_t actual_width,
    std::uint64_t actual_logical_length,
    std::uint64_t actual_padding,
    std::uint8_t expected_alphabet,
    std::uint8_t expected_width,
    std::uint64_t expected_logical_length,
    std::uint64_t expected_padding) noexcept
{
    if (actual_alphabet != expected_alphabet
        || actual_width != expected_width) {
        return fragment_metadata_error::type_mismatch;
    }
    if (actual_logical_length != expected_logical_length) {
        return fragment_metadata_error::logical_length_mismatch;
    }
    if (actual_padding != expected_padding) {
        return fragment_metadata_error::padding_mismatch;
    }
    return fragment_metadata_error::none;
}

/** Build the inclusive bit-width threshold of every length bucket. */
template <class p_length_type,
          std::size_t p_minimum_bits,
          std::size_t p_maximum_bits>
[[nodiscard]] consteval auto make_sequence_bucket_bit_thresholds()
{
    static_assert(p_minimum_bits < p_maximum_bits);
    static_assert(p_maximum_bits <= sizeof(p_length_type) * 8u);

    constexpr std::size_t total_bits = sizeof(p_length_type) * 8u;
    constexpr std::size_t middle_bucket_count =
        p_maximum_bits - p_minimum_bits - 1u;
    constexpr std::size_t bucket_count = middle_bucket_count + 2u;

    std::array<std::size_t, bucket_count> thresholds{};
    thresholds.front() = p_minimum_bits;
    for (std::size_t index = 0u; index < middle_bucket_count; ++index) {
        thresholds[index + 1u] = p_minimum_bits + index + 1u;
    }
    thresholds.back() = total_bits;
    return thresholds;
}

static_assert(
    make_sequence_bucket_bit_thresholds<std::uint64_t, 8u, 42u>().size()
    == 35u);
static_assert(
    make_sequence_bucket_bit_thresholds<std::uint64_t, 8u, 42u>().front()
    == 8u);
static_assert(
    make_sequence_bucket_bit_thresholds<std::uint64_t, 8u, 42u>()[33u]
    == 41u);
static_assert(
    make_sequence_bucket_bit_thresholds<std::uint64_t, 8u, 42u>().back()
    == 64u);

/** Return the bucket-routing bit width for one sequence length. */
[[nodiscard]] [[gnu::always_inline]] constexpr std::size_t
sequence_size_bit_width(std::uint64_t sequence_size) noexcept
{
    const std::uint64_t covered_value =
        sequence_size > 1u ? sequence_size - 1u : 1u;
    return static_cast<std::size_t>(std::bit_width(covered_value));
}

template <class p_length_type,
          std::size_t p_minimum_bits,
          std::size_t p_maximum_bits>
[[nodiscard]] consteval auto make_sequence_bucket_lookup()
{
    constexpr std::size_t total_bits = sizeof(p_length_type) * 8u;
    constexpr auto thresholds = make_sequence_bucket_bit_thresholds<
        p_length_type, p_minimum_bits, p_maximum_bits>();
    std::array<std::uint8_t, total_bits + 1u> lookup{};
    for (std::size_t length_bits = 0u;
         length_bits <= total_bits;
         ++length_bits) {
        std::size_t bucket_index = thresholds.size() - 1u;
        for (std::size_t index = 0u; index < thresholds.size(); ++index) {
            if (length_bits <= thresholds[index]) {
                bucket_index = index;
                break;
            }
        }
        lookup[length_bits] = static_cast<std::uint8_t>(bucket_index);
    }
    return lookup;
}

template <class p_length_type,
          std::size_t p_minimum_bits,
          std::size_t p_maximum_bits>
inline constexpr auto sequence_bucket_lookup = make_sequence_bucket_lookup<
    p_length_type, p_minimum_bits, p_maximum_bits>();

template <class p_length_type,
          std::size_t p_minimum_bits,
          std::size_t p_maximum_bits>
inline constexpr std::size_t sequence_bucket_count =
    make_sequence_bucket_bit_thresholds<
        p_length_type, p_minimum_bits, p_maximum_bits>().size();

static_assert(sequence_bucket_count<std::uint64_t, 8u, 42u>
              <= (std::numeric_limits<std::uint8_t>::max)());

/** Route one sequence length through the precomputed branchless lookup. */
template <class p_length_type,
          std::size_t p_minimum_bits,
          std::size_t p_maximum_bits>
[[nodiscard]] [[gnu::always_inline]] constexpr std::size_t
sequence_bucket_index(std::uint64_t sequence_size) noexcept
{
    constexpr std::size_t total_bits = sizeof(p_length_type) * 8u;
    const std::size_t length_bits = sequence_size_bit_width(sequence_size);
    const std::size_t bounded_bits = std::min(length_bits, total_bits);
    return static_cast<std::size_t>(
        sequence_bucket_lookup<
            p_length_type, p_minimum_bits, p_maximum_bits>[bounded_bits]);
}

}  // namespace bioseq::storage
