/**
 * @file compact_hash32_direct_index.cppm
 * @brief storage-boundary-free mmap-ready hash32 target-global direct inverted-index implementation.
 *
 * The persisted image contains no native object representation and no native
 * pointers.  Every scalar is encoded explicitly in little-endian order.
 * Lookup uses an occupancy/rank open-addressed dictionary, so a query touches
 * only its probe chain and selected run summaries; it never scans all target
 * controls.  Postings are hash-major and, within a hash (and optional target
 * fragment), strand-major.  Consequently exact forward/reverse output sizes
 * are known before any selected position is read.
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
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

export module bioseq.indexing.compact_hash32_direct_index;

import bioseq.common.checked_arithmetic;
import bioseq.indexing.storage;

static_assert(__cplusplus >= 202302L, "C++23-or-later");

namespace bioseq::indexing::direct_index_detail {

template <class p_unsigned_integer>
requires std::is_unsigned_v<p_unsigned_integer>
constexpr void write_le(
        std::span<std::byte> output,
        std::size_t offset,
        p_unsigned_integer value) noexcept {
    if consteval {
        for (std::size_t byte = 0u;
                byte < sizeof(p_unsigned_integer); ++byte) {
            output[offset + byte] = static_cast<std::byte>(
                value & p_unsigned_integer{0xffu});
            value >>= 8u;
        }
    } else {
        if constexpr (std::endian::native == std::endian::big) {
            value = std::byteswap(value);
        }
        std::memcpy(
            output.data() + offset, &value, sizeof(p_unsigned_integer));
    }
}

template <class p_unsigned_integer>
requires std::is_unsigned_v<p_unsigned_integer>
[[nodiscard]] constexpr p_unsigned_integer read_le(
        std::span<const std::byte> input,
        std::size_t offset) noexcept {
    if consteval {
        p_unsigned_integer value = 0u;
        for (std::size_t byte = 0u;
                byte < sizeof(p_unsigned_integer); ++byte) {
            value |= static_cast<p_unsigned_integer>(
                std::to_integer<std::uint8_t>(input[offset + byte]))
                << (byte * 8u);
        }
        return value;
    } else {
        p_unsigned_integer value = 0u;
        std::memcpy(
            &value, input.data() + offset, sizeof(p_unsigned_integer));
        if constexpr (std::endian::native == std::endian::big) {
            value = std::byteswap(value);
        }
        return value;
    }
}

[[nodiscard]] constexpr bool checked_add(
        std::uint64_t left,
        std::uint64_t right,
        std::uint64_t& result) noexcept {
    const auto sum = bioseq::common::checked_add(left, right);
    if (!sum.has_value()) {
        return false;
    }
    result = *sum;
    return true;
}

[[nodiscard]] constexpr bool checked_multiply(
        std::uint64_t left,
        std::uint64_t right,
        std::uint64_t& result) noexcept {
    const auto product = bioseq::common::checked_mul(left, right);
    if (!product.has_value()) {
        return false;
    }
    result = *product;
    return true;
}

[[nodiscard]] constexpr bool align_up(
        std::uint64_t value,
        std::uint64_t alignment,
        std::uint64_t& result) noexcept {
    const auto aligned = bioseq::common::checked_align_up(value, alignment);
    if (!aligned.has_value()) {
        return false;
    }
    result = *aligned;
    return true;
}

[[nodiscard]] constexpr std::uint64_t ceil_div(
        std::uint64_t value,
        std::uint64_t divisor) noexcept {
    return bioseq::common::ceil_div(value, divisor);
}

[[nodiscard]] constexpr bool power_of_two(std::uint64_t value) noexcept {
    return value != 0u && (value & (value - 1u)) == 0u;
}

[[nodiscard]] constexpr std::optional<std::uint64_t> next_power_of_two(
        std::uint64_t value) noexcept {
    if (value <= 1u) return 1u;
    if (value > (std::uint64_t{1u} << 63u)) return std::nullopt;
    return std::uint64_t{1u} << (64u - std::countl_zero(value - 1u));
}

[[nodiscard]] constexpr std::uint32_t mix_hash32(
        std::uint32_t value) noexcept {
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}

using crc64_state = bioseq::indexing::crc64_ecma182_accumulator;

[[nodiscard]] inline std::uint64_t crc64(
        std::span<const std::byte> bytes) noexcept {
    return bioseq::indexing::crc64_ecma182(bytes);
}

[[nodiscard]] inline std::uint64_t header_crc(
        std::span<const std::byte> header,
        std::size_t checksum_offset,
        std::size_t checksum_bytes) noexcept {
    crc64_state crc;
    crc.update(header.first(checksum_offset));
    std::array<std::byte, 8> zero{};
    crc.update(std::span<const std::byte>{zero}.first(checksum_bytes));
    crc.update(header.subspan(checksum_offset + checksum_bytes));
    return crc.checksum();
}

[[nodiscard]] inline bool all_zero(std::span<const std::byte> bytes) noexcept {
    for (const std::byte byte : bytes) {
        if (byte != std::byte{0}) return false;
    }
    return true;
}

} // namespace bioseq::indexing::direct_index_detail

export namespace bioseq::indexing {

inline constexpr std::uint64_t k_direct_inverted_index_magic =
    0x3132494432334842ULL; // "Bhash32DI21", byte-order-independent numeric tag.
inline constexpr std::uint32_t k_direct_inverted_index_layout_revision = 1u;
inline constexpr std::uint32_t k_direct_inverted_index_header_bytes = 256u;
inline constexpr std::uint32_t k_direct_inverted_index_control_bytes = 48u;
inline constexpr std::uint32_t k_direct_inverted_index_run_summary_bytes = 32u;
inline constexpr std::uint32_t k_direct_inverted_index_rank_group_buckets = 256u;
inline constexpr std::uint32_t k_direct_inverted_index_hash_mix_id = 1u;
inline constexpr std::uint64_t k_direct_inverted_index_section_alignment = 64u;
inline constexpr std::size_t k_direct_inverted_index_occupancy_crc_offset = 176u;
inline constexpr std::size_t k_direct_inverted_index_rank_crc_offset = 184u;
inline constexpr std::size_t k_direct_inverted_index_controls_crc_offset = 192u;
inline constexpr std::size_t k_direct_inverted_index_summaries_crc_offset = 200u;
inline constexpr std::size_t k_direct_inverted_index_postings_crc_offset = 208u;
inline constexpr std::size_t k_direct_inverted_index_header_crc_offset = 216u;
inline constexpr std::size_t k_direct_inverted_index_checksum_bytes = 8u;
inline constexpr std::uint32_t k_direct_inverted_index_no_selection =
    (std::numeric_limits<std::uint32_t>::max)();
inline constexpr std::uint32_t k_direct_inverted_index_no_fragment =
    (std::numeric_limits<std::uint32_t>::max)();

enum class direct_inverted_index_error : std::uint8_t {
    arithmetic_overflow,
    admission,
    allocation,
    invalid_options,
    occurrence_range,
    bucket_count,
    image_size,
    bad_magic,
    bad_layout_revision,
    bad_header,
    bad_flags,
    reserved_nonzero,
    identity_mismatch,
    header_checksum,
    section_checksum,
    section_layout,
    dictionary,
    control,
    run_summary,
    posting,
    query_size,
    query_plan,
    visitor_rejected,
    source_read,
    order,
    fixed_width,
    replay_mismatch,
    storage_contract_mismatch,
    receipt_mismatch,
};

struct direct_index_occurrence {
    std::uint32_t hash = 0u;
    std::uint64_t target_bp = 0u;
    bool target_strand_one = false;

    [[nodiscard]] constexpr bool operator==(
        const direct_index_occurrence&) const noexcept = default;
};

struct direct_index_identity {
    std::uint64_t generation_id = 0u;
    std::uint64_t source_identity = 0u;
    std::uint64_t storage_contract_checksum = 0u;
    std::uint64_t target_length = 0u;

    [[nodiscard]] constexpr bool operator==(
        const direct_index_identity&) const noexcept = default;
};

struct direct_index_build_options {
    direct_index_identity identity{};
    /// Zero disables summaries.  Otherwise one summary is emitted for every
    /// nonempty (hash, target-fragment) pair.
    std::uint64_t fragment_span_bp = 0u;
    /// Tests may pin a small power-of-two table.  Zero selects the production
    /// full-u32 dictionary at a maximum 7/8 load factor.
    std::uint64_t bucket_count_override = 0u;
    std::uint64_t max_occurrences =
        (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t max_unique_hashes =
        (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t max_bucket_count =
        (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t max_run_summaries =
        (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t max_image_bytes =
        (std::numeric_limits<std::uint64_t>::max)();
    /// Excludes the caller-owned input and final returned image.
    std::uint64_t max_build_scratch_bytes =
        (std::numeric_limits<std::uint64_t>::max)();
};

struct direct_index_image_plan {
    /**
     * Exact wire equations (all section starts are align64):
     *   O = B/8
     *   R = (ceil(B/256)+1)*8
     *   C = U*48
     *   S = run_count*32
     *   P = N*W, W in {4,8}
     *   image = postings_offset+P.
     * `estimated_build_scratch_bytes` excludes the returned image and equals
     * N*sizeof(direct_index_prepared_occurrence)+N*sizeof(direct_index_hash_group)+B*8 in this implementation.
     */
    std::uint8_t posting_width_bytes = 0u;
    std::uint64_t occurrence_count = 0u;
    std::uint64_t unique_hash_count = 0u;
    std::uint64_t bucket_count = 0u;
    std::uint64_t run_summary_count = 0u;
    std::uint64_t occupancy_offset = 0u;
    std::uint64_t occupancy_bytes = 0u;
    std::uint64_t rank_offset = 0u;
    std::uint64_t rank_bytes = 0u;
    std::uint64_t controls_offset = 0u;
    std::uint64_t controls_bytes = 0u;
    std::uint64_t summaries_offset = 0u;
    std::uint64_t summaries_bytes = 0u;
    std::uint64_t postings_offset = 0u;
    std::uint64_t postings_bytes = 0u;
    std::uint64_t image_bytes = 0u;
    std::uint64_t estimated_build_scratch_bytes = 0u;

    [[nodiscard]] constexpr bool operator==(
        const direct_index_image_plan&) const noexcept = default;
};

struct direct_index_header {
    std::uint32_t flags = 0u;
    std::uint8_t posting_width_bytes = 0u;
    direct_index_identity identity{};
    std::uint64_t occurrence_count = 0u;
    std::uint64_t unique_hash_count = 0u;
    std::uint64_t bucket_count = 0u;
    std::uint64_t occupancy_offset = 0u;
    std::uint64_t occupancy_bytes = 0u;
    std::uint64_t rank_offset = 0u;
    std::uint64_t rank_bytes = 0u;
    std::uint64_t controls_offset = 0u;
    std::uint64_t controls_bytes = 0u;
    std::uint64_t summaries_offset = 0u;
    std::uint64_t summaries_bytes = 0u;
    std::uint64_t postings_offset = 0u;
    std::uint64_t postings_bytes = 0u;
    std::uint64_t fragment_span_bp = 0u;
    std::uint64_t run_summary_count = 0u;
    std::uint64_t occupancy_crc64 = 0u;
    std::uint64_t rank_crc64 = 0u;
    std::uint64_t controls_crc64 = 0u;
    std::uint64_t summaries_crc64 = 0u;
    std::uint64_t postings_crc64 = 0u;
    std::uint64_t header_crc64 = 0u;
    std::uint64_t image_bytes = 0u;
};

struct direct_inverted_index_view {
    std::span<const std::byte> image{};
    direct_index_header header{};

    [[nodiscard]] constexpr bool has_run_summaries() const noexcept {
        return (header.flags & 1u) != 0u;
    }
};

struct direct_index_query_occurrence {
    std::uint32_t hash = 0u;
    bool query_strand_one = false;
};

struct direct_index_posting_span {
    std::uint64_t byte_offset = 0u;
    std::uint64_t count = 0u;
    std::uint32_t selection_ordinal = 0u;
    std::uint32_t fragment_ordinal = k_direct_inverted_index_no_fragment;
    bool target_strand_one = false;
};

struct direct_index_selection {
    std::uint32_t hash = 0u;
    std::uint64_t global_count = 0u;
    std::uint64_t target_strand_zero_count = 0u;
    std::uint64_t first_span = 0u;
    std::uint64_t span_count = 0u;
};

struct direct_index_query_result {
    bool present = false;
    bool positions_selected = false;
    std::uint32_t selection_ordinal = k_direct_inverted_index_no_selection;
    std::uint64_t global_count = 0u;
    std::uint64_t forward_count = 0u;
    std::uint64_t reverse_count = 0u;
};

struct direct_index_query_stats {
    std::uint64_t query_occurrence_count = 0u;
    std::uint64_t dictionary_slots_examined = 0u;
    std::uint64_t control_records_examined = 0u;
    std::uint64_t run_summaries_examined = 0u;
    std::uint64_t unique_selected_hash_count = 0u;
    std::uint64_t selected_posting_count = 0u;
    std::uint64_t selected_posting_bytes = 0u;
};

struct direct_index_query_plan {
    std::uint64_t image_header_crc64 = 0u;
    std::uint8_t posting_width_bytes = 0u;
    std::vector<direct_index_selection> selections{};
    std::vector<direct_index_posting_span> spans{};
    std::vector<direct_index_query_result> results{};
    direct_index_query_stats stats{};
};

struct direct_index_posting {
    std::uint32_t selection_ordinal = 0u;
    std::uint32_t fragment_ordinal = k_direct_inverted_index_no_fragment;
    std::uint64_t encoded_position = 0u;
    bool target_strand_one = false;

    [[nodiscard]] constexpr std::uint64_t target_bp() const noexcept {
        return encoded_position >> 1u;
    }
};

using direct_index_posting_visitor = bool (*)(
    void*, const direct_index_posting&);

struct direct_index_posting_visit_stats {
    std::uint64_t spans_visited = 0u;
    std::uint64_t postings_visited = 0u;
};

[[nodiscard]] inline std::uint64_t direct_inverted_index_crc64(
        std::span<const std::byte> bytes) noexcept {
    return direct_index_detail::crc64(bytes);
}

} // namespace bioseq::indexing

// --------------------------------------------------------------------------
// Compact production-oriented hash32 layout.
// --------------------------------------------------------------------------

namespace bioseq::indexing::compact_hash32_detail {
struct compact_hash32_unique_query_plan_access;
}

export namespace bioseq::indexing {

inline constexpr std::uint64_t k_compact_hash32_direct_index_magic =
    0x3143443233485342ULL;
/// Persisted compact-hash image revision; current images retain byte value 2.
inline constexpr std::uint32_t k_compact_hash32_direct_index_layout_revision = 2u;
inline constexpr std::uint32_t k_compact_hash32_direct_index_header_bytes = 256u;
/// `{u32 posting_or_inline,u16 count,u16 strand0}`. A P32 singleton is
/// encoded in the first word; every other run names its first posting ordinal.
inline constexpr std::uint16_t k_compact_hash32_direct_index_descriptor_bytes = 8u;
inline constexpr std::uint32_t k_compact_hash32_direct_index_rank_group_bits = 256u;
inline constexpr std::uint8_t k_compact_hash32_full_hash_bits = 32u;
inline constexpr std::uint8_t k_compact_hash32_canonical_k15_hash_bits = 30u;
inline constexpr std::uint64_t k_compact_hash32_full_membership_bytes =
    std::uint64_t{1u} << 29u; // 2^32 exact membership bits.
inline constexpr std::uint64_t k_compact_hash32_full_rank_bytes =
    std::uint64_t{1u} << 26u; // 2^24 uint32 checkpoints.
inline constexpr std::uint64_t k_compact_hash32_canonical_k15_membership_bytes =
    std::uint64_t{1u} << 27u; // 2^30 exact membership bits.
inline constexpr std::uint64_t k_compact_hash32_canonical_k15_rank_bytes =
    std::uint64_t{1u} << 24u; // 2^22 uint32 checkpoints.
inline constexpr std::size_t k_compact_hash32_membership_crc_offset = 176u;
inline constexpr std::size_t k_compact_hash32_rank_crc_offset = 184u;
inline constexpr std::size_t k_compact_hash32_descriptors_crc_offset = 192u;
inline constexpr std::size_t k_compact_hash32_postings_crc_offset = 200u;
inline constexpr std::size_t k_compact_hash32_header_crc_offset = 208u;
inline constexpr std::size_t k_compact_hash32_stream_crc_offset = 224u;
inline constexpr std::size_t k_compact_hash32_inline_singleton_count_offset = 236u;

struct compact_hash32_address {
    std::uint64_t membership_byte_ordinal = 0u;
    std::uint8_t membership_bit = 0u;
    std::uint64_t rank_checkpoint_ordinal = 0u;
    std::uint16_t bit_within_rank_group = 0u;

    [[nodiscard]] constexpr bool operator==(
        const compact_hash32_address&) const noexcept = default;
};

/// The canonical K=15 profile binds d=30/max=2^30-1. Full-width hash32
/// binds d=32/max=UINT32_MAX.  Values [9,29] and 31 exist solely so unit tests
/// can exercise the same codec/rank algorithms without allocating either
/// production prefix.  Every image authenticates both d and max_hash.
struct compact_hash32_geometry {
    std::uint8_t hash_domain_bits = k_compact_hash32_canonical_k15_hash_bits;
    std::uint32_t max_hash = (std::uint32_t{1u}
        << k_compact_hash32_canonical_k15_hash_bits) - 1u;

    [[nodiscard]] constexpr bool canonical_k15() const noexcept {
        return hash_domain_bits == k_compact_hash32_canonical_k15_hash_bits
            && max_hash == 0x3fffffffu;
    }

    [[nodiscard]] constexpr bool full_h32() const noexcept {
        return hash_domain_bits == k_compact_hash32_full_hash_bits
            && max_hash
                == (std::numeric_limits<std::uint32_t>::max)();
    }

    [[nodiscard]] constexpr bool operator==(
        const compact_hash32_geometry&) const noexcept = default;
};

[[nodiscard]] constexpr std::expected<compact_hash32_address,
    direct_inverted_index_error>
resolve_compact_hash32_address(
        std::uint32_t hash,
        compact_hash32_geometry geometry = {}) noexcept {
    if (geometry.hash_domain_bits < 9u
        || geometry.hash_domain_bits > k_compact_hash32_full_hash_bits) {
        return std::unexpected(direct_inverted_index_error::occurrence_range);
    }
    const std::uint32_t expected_max = geometry.hash_domain_bits == 32u
        ? (std::numeric_limits<std::uint32_t>::max)()
        : static_cast<std::uint32_t>(
            (std::uint64_t{1u} << geometry.hash_domain_bits) - 1u);
    if (geometry.max_hash != expected_max || hash > geometry.max_hash) {
        return std::unexpected(direct_inverted_index_error::occurrence_range);
    }
    return compact_hash32_address{
        .membership_byte_ordinal = hash / 8u,
        .membership_bit = static_cast<std::uint8_t>(hash % 8u),
        .rank_checkpoint_ordinal =
            hash / k_compact_hash32_direct_index_rank_group_bits,
        .bit_within_rank_group = static_cast<std::uint16_t>(
            hash % k_compact_hash32_direct_index_rank_group_bits),
    };
}

struct compact_hash32_sorted_run_header {
    std::uint32_t hash = 0u;
    std::uint64_t global_count = 0u;
    std::uint64_t target_strand_zero_count = 0u;
};

using compact_hash32_begin_run = std::expected<void,
    direct_inverted_index_error> (*)(
        void*, const compact_hash32_sorted_run_header&);
using compact_hash32_consume_positions = std::expected<void,
    direct_inverted_index_error> (*)(
        void*, std::span<const std::uint64_t>);

struct compact_hash32_sorted_run_receiver {
    void* context = nullptr;
    compact_hash32_begin_run begin_run_fn = nullptr;
    compact_hash32_consume_positions consume_positions_fn = nullptr;

    [[nodiscard]] std::expected<void, direct_inverted_index_error>
    begin_run(const compact_hash32_sorted_run_header& header) const {
        if (context == nullptr || begin_run_fn == nullptr) {
            return std::unexpected(direct_inverted_index_error::source_read);
        }
        return begin_run_fn(context, header);
    }

    [[nodiscard]] std::expected<void, direct_inverted_index_error>
    consume_positions(
            std::span<const std::uint64_t> encoded_positions) const {
        if (context == nullptr || consume_positions_fn == nullptr) {
            return std::unexpected(direct_inverted_index_error::source_read);
        }
        return consume_positions_fn(context, encoded_positions);
    }
};

using compact_hash32_scan_sorted_runs = std::expected<void,
    direct_inverted_index_error> (*)(
        void*, const compact_hash32_sorted_run_receiver&);

/// Replayable, strict-hash-monotonic source.  A run may deliver its positions
/// in arbitrarily sized chunks, but must deliver exactly global_count values
/// in [all strand zero ordered by bp][all strand one ordered by bp] order.
struct compact_hash32_sorted_run_source {
    void* context = nullptr;
    compact_hash32_scan_sorted_runs scan_sorted_runs_fn = nullptr;

    [[nodiscard]] std::expected<void, direct_inverted_index_error>
    scan_sorted_runs(const compact_hash32_sorted_run_receiver& receiver) const {
        if (context == nullptr || scan_sorted_runs_fn == nullptr) {
            return std::unexpected(direct_inverted_index_error::source_read);
        }
        return scan_sorted_runs_fn(context, receiver);
    }
};

struct compact_hash32_build_options {
    direct_index_identity identity{};
    std::uint32_t max_occ_cap = 0u;
    compact_hash32_geometry geometry{};
    std::uint64_t max_image_bytes =
        (std::numeric_limits<std::uint64_t>::max)();
};

struct compact_hash32_image_plan {
    direct_index_identity identity{};
    std::uint32_t layout_revision = k_compact_hash32_direct_index_layout_revision;
    std::uint16_t descriptor_width_bytes =
        k_compact_hash32_direct_index_descriptor_bytes;
    std::uint32_t max_occ_cap = 0u;
    std::uint8_t posting_width_bytes = 0u;
    compact_hash32_geometry geometry{};
    std::uint64_t source_hash_count = 0u;
    std::uint64_t source_posting_count = 0u;
    std::uint64_t accepted_hash_count = 0u;
    std::uint64_t accepted_posting_count = 0u;
    std::uint64_t inline_singleton_count = 0u;
    std::uint64_t materialized_hash_count = 0u;
    std::uint64_t materialized_posting_count = 0u;
    std::uint64_t rejected_hash_count = 0u;
    std::uint64_t rejected_posting_count = 0u;
    std::uint64_t membership_offset = 0u;
    std::uint64_t membership_bytes = 0u;
    std::uint64_t rank_offset = 0u;
    std::uint64_t rank_bytes = 0u;
    std::uint64_t descriptors_offset = 0u;
    std::uint64_t descriptors_bytes = 0u;
    std::uint64_t postings_offset = 0u;
    std::uint64_t postings_bytes = 0u;
    std::uint64_t image_bytes = 0u;
    std::uint64_t sorted_stream_crc64 = 0u;

    [[nodiscard]] constexpr bool operator==(
        const compact_hash32_image_plan&) const noexcept = default;
};

struct compact_hash32_header : compact_hash32_image_plan {
    std::uint64_t membership_crc64 = 0u;
    std::uint64_t rank_crc64 = 0u;
    std::uint64_t descriptors_crc64 = 0u;
    std::uint64_t postings_crc64 = 0u;
    std::uint64_t header_crc64 = 0u;
};

struct compact_hash32_direct_index_view {
    std::span<const std::byte> image{};
    compact_hash32_header header{};
};

/// Storage-contract receipt supplied by the enclosing persisted-image format.
/// The bounded opener authenticates and decodes only the fixed direct-index
/// header; it deliberately does not fault in or checksum any payload page.
struct compact_hash32_bounded_open_options {
    direct_index_identity expected_identity{};
    std::uint32_t expected_max_occ_cap = 0u;
    compact_hash32_geometry expected_geometry{};
    /// Optional minimizer-fragment receipt: ordinary CRC64/ECMA over the finalized fixed
    /// 256-byte direct header, including its embedded checksum field.
    std::optional<std::uint64_t> expected_direct_header_crc64{};
};

struct compact_hash32_bounded_open_stats {
    std::uint64_t direct_section_bytes = 0u;
    std::uint64_t header_bytes_examined = 0u;
    std::uint64_t payload_bytes_examined = 0u;
    bool expected_direct_header_crc64_checked = false;
};

/// Alias-bound direct-index view returned by the bounded timed-open path.  The
/// shared pointer is an alias whose stored address is the beginning of the
/// nested direct section while its control block preserves the supplied owner
/// token.  A genuinely owning token can retain the enclosing mmap; a
/// transaction-bound or otherwise non-owning token still requires its external
/// mapping lease to outlive this view.  The direct section may begin at any
/// nonzero offset; the token's stored pointer need not equal image.data().
class compact_hash32_bounded_open_view {
public:
    compact_hash32_bounded_open_view(
            std::shared_ptr<const std::byte> image_owner,
            compact_hash32_direct_index_view view,
            compact_hash32_bounded_open_stats stats) noexcept
        : image_owner_{std::move(image_owner)},
          view_{view},
          stats_{stats} {}

    [[nodiscard]] const compact_hash32_direct_index_view& view() const noexcept {
        return view_;
    }

    [[nodiscard]] const std::shared_ptr<const std::byte>&
    image_owner() const noexcept {
        return image_owner_;
    }

    [[nodiscard]] const compact_hash32_bounded_open_stats&
    stats() const noexcept {
        return stats_;
    }

private:
    std::shared_ptr<const std::byte> image_owner_{};
    compact_hash32_direct_index_view view_{};
    compact_hash32_bounded_open_stats stats_{};
};

struct compact_hash32_query_occurrence {
    std::uint32_t hash = 0u;
    bool query_strand_one = false;
};

struct compact_hash32_query_result {
    bool accepted = false;
    std::uint32_t selection_ordinal = k_direct_inverted_index_no_selection;
    std::uint64_t dense_ordinal = 0u;
    std::uint64_t global_count = 0u;
    std::uint64_t forward_count = 0u;
    std::uint64_t reverse_count = 0u;
};

struct compact_hash32_selection {
    std::uint32_t hash = 0u;
    std::uint64_t dense_ordinal = 0u;
    std::uint64_t global_count = 0u;
    std::uint64_t target_strand_zero_count = 0u;
    std::uint64_t first_span = 0u;
    std::uint64_t span_count = 0u;
};

struct compact_hash32_query_stats {
    std::uint64_t query_occurrence_count = 0u;
    std::uint64_t distinct_query_hash_count = 0u;
    std::uint64_t rank_checkpoints_examined = 0u;
    /// Physical membership-word reads.  The unique-query path reads all four
    /// words once when it enters a 256-hash rank group, then serves every
    /// remaining hash in that group from the cache.
    std::uint64_t membership_words_examined = 0u;
    std::uint64_t descriptors_examined = 0u;
    std::uint64_t unique_selected_hash_count = 0u;
    std::uint64_t selected_posting_count = 0u;
    std::uint64_t selected_inline_singleton_count = 0u;
    std::uint64_t selected_materialized_hash_count = 0u;
    std::uint64_t selected_materialized_posting_count = 0u;
    std::uint64_t runtime_rejected_hash_count = 0u;
    std::uint64_t runtime_rejected_posting_count = 0u;
    std::uint64_t rejected_posting_reads = 0u;
    /// Unique-query owner accounting. A fresh owner reserves U once, never
    /// grows while filling, then compacts to exact accepted capacity c.
    std::uint64_t selection_reserve_call_count = 0u;
    std::uint64_t selection_reserve_requested_elements = 0u;
    std::uint64_t selection_initial_capacity_elements = 0u;
    std::uint64_t selection_fill_reallocation_count = 0u;
    std::uint64_t selection_compaction_allocation_count = 0u;
    std::uint64_t selection_storage_allocation_count = 0u;
    std::uint64_t selection_peak_capacity_bytes = 0u;
    std::uint64_t selection_retained_capacity_elements = 0u;
    std::uint64_t selection_retained_capacity_bytes = 0u;

    [[nodiscard]] constexpr bool operator==(
        const compact_hash32_query_stats&) const noexcept = default;
};

struct compact_hash32_query_plan {
    std::uint64_t image_header_crc64 = 0u;
    std::uint32_t max_occ_cap = 0u;
    std::uint32_t runtime_max_occ = 0u;
    std::uint8_t posting_width_bytes = 0u;
    std::vector<compact_hash32_selection> selections{};
    std::vector<direct_index_posting_span> spans{};
    std::vector<compact_hash32_query_result> results{};
    compact_hash32_query_stats stats{};
};

struct compact_hash32_unique_selection {
    std::uint32_t dense_ordinal = 0u;
    std::uint32_t query_ordinal = 0u;
};
static_assert(sizeof(compact_hash32_unique_selection) == 8u);

/// Accepted-only result for a strictly increasing unique-hash probe.  Each
/// retained mapping names the dense descriptor directly, so later posting
/// traversal never repeats membership/rank lookup.  The private seal binds
/// the public accounting and mappings to the factory-built plan; a copied
/// plan whose public fields are mutated is rejected before traversal.
struct compact_hash32_unique_query_plan {
    std::uint64_t image_header_crc64 = 0u;
    std::uint32_t max_occ_cap = 0u;
    std::uint32_t runtime_max_occ = 0u;
    std::uint8_t posting_width_bytes = 0u;
    std::vector<compact_hash32_unique_selection> selections{};
    compact_hash32_query_stats stats{};

    [[nodiscard]] std::uint64_t retained_selection_capacity_bytes()
            const noexcept {
        const std::uint64_t capacity = selections.capacity();
        return capacity
                > (std::numeric_limits<std::uint64_t>::max)()
                    / sizeof(compact_hash32_unique_selection)
            ? (std::numeric_limits<std::uint64_t>::max)()
            : capacity * sizeof(compact_hash32_unique_selection);
    }

private:
    std::uint64_t authentication_crc64_ = 0u;

    [[nodiscard]] static constexpr std::uint64_t mix_authentication_(
            std::uint64_t state,
            std::uint64_t value) noexcept {
        value += 0x9e3779b97f4a7c15ULL;
        value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27u)) * 0x94d049bb133111ebULL;
        value ^= value >> 31u;
        return std::rotl(state ^ value, 27u)
            * 0x3c79ac492ba7b653ULL + 0x1c69b3f74ac4ae35ULL;
    }

    [[nodiscard]] std::uint64_t computed_authentication_() const noexcept {
        std::uint64_t state = 0x43483332504c414eULL;
        const auto mix = [&state](std::uint64_t value) noexcept {
            state = mix_authentication_(state, value);
        };
        mix(image_header_crc64);
        mix(max_occ_cap);
        mix(runtime_max_occ);
        mix(posting_width_bytes);
        mix(stats.query_occurrence_count);
        mix(stats.distinct_query_hash_count);
        mix(stats.rank_checkpoints_examined);
        mix(stats.membership_words_examined);
        mix(stats.descriptors_examined);
        mix(stats.unique_selected_hash_count);
        mix(stats.selected_posting_count);
        mix(stats.selected_inline_singleton_count);
        mix(stats.selected_materialized_hash_count);
        mix(stats.selected_materialized_posting_count);
        mix(stats.runtime_rejected_hash_count);
        mix(stats.runtime_rejected_posting_count);
        mix(stats.rejected_posting_reads);
        mix(stats.selection_reserve_call_count);
        mix(stats.selection_reserve_requested_elements);
        mix(stats.selection_initial_capacity_elements);
        mix(stats.selection_fill_reallocation_count);
        mix(stats.selection_compaction_allocation_count);
        mix(stats.selection_storage_allocation_count);
        mix(stats.selection_peak_capacity_bytes);
        mix(stats.selection_retained_capacity_elements);
        mix(stats.selection_retained_capacity_bytes);
        mix(selections.size());
        for (const compact_hash32_unique_selection selection : selections) {
            mix((static_cast<std::uint64_t>(selection.dense_ordinal) << 32u)
                | selection.query_ordinal);
        }
        state = mix_authentication_(state, 0x5345414c45445032ULL);
        return state == 0u ? 0x5345414c45445032ULL : state;
    }

    friend struct compact_hash32_detail::compact_hash32_unique_query_plan_access;
};

/// Exact typed view of one authoritative strand-major posting extent.  The
/// bulk traversal constructs these only after descriptor and section bounds
/// validation.  Callers can loop positions inline without an indirect
/// callback per posting.
struct compact_hash32_posting_span_view {
    std::span<const std::byte> encoded_bytes{};
    std::uint64_t count = 0u;
    std::uint64_t target_length = 0u;
    std::uint8_t posting_width_bytes = 0u;
    bool target_strand_one = false;

    [[nodiscard]] std::expected<std::uint64_t,
        direct_inverted_index_error>
    encoded_position_at(std::uint64_t ordinal) const noexcept {
        if (ordinal >= count
            || (posting_width_bytes != 4u
                && posting_width_bytes != 8u)) {
            return std::unexpected(direct_inverted_index_error::query_plan);
        }
        std::uint64_t byte_offset = 0u;
        if (!direct_index_detail::checked_multiply(
                ordinal, posting_width_bytes, byte_offset)
            || byte_offset > encoded_bytes.size()
            || posting_width_bytes
                > encoded_bytes.size() - byte_offset) {
            return std::unexpected(direct_inverted_index_error::query_plan);
        }
        const std::uint64_t encoded = posting_width_bytes == 4u
            ? static_cast<std::uint64_t>(
                direct_index_detail::read_le<std::uint32_t>(
                    encoded_bytes, static_cast<std::size_t>(byte_offset)))
            : direct_index_detail::read_le<std::uint64_t>(
                encoded_bytes, static_cast<std::size_t>(byte_offset));
        if (((encoded & 1u) != 0u) != target_strand_one
            || (encoded >> 1u) >= target_length) {
            return std::unexpected(direct_inverted_index_error::query_plan);
        }
        return encoded;
    }

    /// Decode this already-bounded span sequentially after one exact extent
    /// check and one P32/P64 dispatch.  The typed inner loop advances byte
    /// pointers, avoiding the ordinal multiply, repeated extent check, and
    /// runtime-width branch performed by random access.  Every decoded value
    /// still receives release-active strand and target-range validation.
    template <class p_visitor>
    [[nodiscard, gnu::always_inline]] inline
    std::expected<std::uint64_t, direct_inverted_index_error>
    visit_encoded_positions(p_visitor&& visitor) const {
        std::uint64_t expected_bytes = 0u;
        if ((posting_width_bytes != 4u && posting_width_bytes != 8u)
            || !direct_index_detail::checked_multiply(
                count, posting_width_bytes, expected_bytes)
            || expected_bytes != encoded_bytes.size()
            || (expected_bytes != 0u && encoded_bytes.data() == nullptr)) {
            return std::unexpected(direct_inverted_index_error::query_plan);
        }
        if (expected_bytes == 0u) return std::uint64_t{0u};
        if (posting_width_bytes == 4u) {
            return visit_encoded_positions_width_<std::uint32_t>(
                std::forward<p_visitor>(visitor));
        }
        return visit_encoded_positions_width_<std::uint64_t>(
            std::forward<p_visitor>(visitor));
    }

private:
    template <class p_encoded_integer, class p_visitor>
    [[nodiscard, gnu::always_inline]] inline
    std::expected<std::uint64_t, direct_inverted_index_error>
    visit_encoded_positions_width_(p_visitor&& visitor) const {
        const std::byte* cursor = encoded_bytes.data();
        const std::byte* const end = cursor + encoded_bytes.size();
        std::uint64_t decoded_count = 0u;
        while (cursor != end) {
            p_encoded_integer encoded_width = 0u;
            std::memcpy(&encoded_width, cursor, sizeof(p_encoded_integer));
            if constexpr (std::endian::native == std::endian::big) {
                encoded_width = std::byteswap(encoded_width);
            }
            const std::uint64_t encoded = encoded_width;
            if (((encoded & 1u) != 0u) != target_strand_one
                || (encoded >> 1u) >= target_length) {
                return std::unexpected(
                    direct_inverted_index_error::query_plan);
            }
            if (!static_cast<bool>(visitor(encoded))) {
                return std::unexpected(
                    direct_inverted_index_error::visitor_rejected);
            }
            cursor += sizeof(p_encoded_integer);
            ++decoded_count;
        }
        if (decoded_count != count) {
            return std::unexpected(direct_inverted_index_error::query_plan);
        }
        return decoded_count;
    }
};

/// One callback value per accepted selection.  Both optional strand spans are
/// bundled into the value so the API never calls once per posting.
struct compact_hash32_selection_posting_spans {
    std::uint32_t selection_ordinal = 0u;
    std::uint32_t dense_ordinal = 0u;
    std::uint32_t query_ordinal = 0u;
    std::uint32_t global_count = 0u;
    std::uint32_t strand_zero_count = 0u;
    std::array<compact_hash32_posting_span_view, 2u> spans{};
    std::uint8_t span_count = 0u;
};

struct compact_hash32_selection_span_visit_stats {
    std::uint64_t selection_callbacks = 0u;
    std::uint64_t spans_exposed = 0u;
    std::uint64_t postings_exposed = 0u;
    std::uint64_t inline_selection_callbacks = 0u;
    std::uint64_t materialized_selection_callbacks = 0u;
    std::uint64_t inline_postings_exposed = 0u;
    std::uint64_t materialized_postings_exposed = 0u;
    std::uint64_t descriptors_examined = 0u;
    std::uint64_t rank_checkpoints_examined = 0u;
    std::uint64_t membership_words_examined = 0u;
};

/// Authenticated, non-owning binding of one accepted-only query plan to one
/// direct-index view.  Construction authenticates the whole plan exactly
/// once; range visits thereafter validate only constant-size binding state and
/// the descriptors in their requested half-open range.  The bound plan,
/// selection storage, and image mapping must remain alive and unmodified for
/// the accessor lifetime.  Const range visits have no shared mutable state and
/// may therefore run concurrently with independent visitors.
class compact_hash32_selection_span_range_accessor {
public:
    compact_hash32_selection_span_range_accessor(
        const compact_hash32_selection_span_range_accessor&) noexcept = default;
    compact_hash32_selection_span_range_accessor& operator=(
        const compact_hash32_selection_span_range_accessor&) noexcept = default;
    compact_hash32_selection_span_range_accessor(
        compact_hash32_selection_span_range_accessor&&) noexcept = default;
    compact_hash32_selection_span_range_accessor& operator=(
        compact_hash32_selection_span_range_accessor&&) noexcept = default;
    ~compact_hash32_selection_span_range_accessor() = default;

    [[nodiscard]] std::uint64_t selection_count() const noexcept {
        return selections_.size();
    }

private:
    compact_hash32_direct_index_view view_{};
    const compact_hash32_unique_query_plan* plan_ = nullptr;
    std::span<const compact_hash32_unique_selection> selections_{};
    compact_hash32_query_stats plan_stats_{};
    std::uint64_t plan_authentication_receipt_ = 0u;
    std::uint64_t plan_image_header_crc64_ = 0u;
    std::uint64_t descriptors_end_ = 0u;
    std::uint64_t postings_end_ = 0u;
    std::size_t selection_capacity_ = 0u;
    std::uint32_t plan_max_occ_cap_ = 0u;
    std::uint32_t runtime_max_occ_ = 0u;
    std::uint8_t plan_posting_width_bytes_ = 0u;

    compact_hash32_selection_span_range_accessor(
            const compact_hash32_direct_index_view& view,
            const compact_hash32_unique_query_plan& plan,
            std::uint64_t plan_authentication_receipt,
            std::uint64_t descriptors_end,
            std::uint64_t postings_end) noexcept
        : view_{view},
          plan_{std::addressof(plan)},
          selections_{plan.selections.data(), plan.selections.size()},
          plan_stats_{plan.stats},
          plan_authentication_receipt_{plan_authentication_receipt},
          plan_image_header_crc64_{plan.image_header_crc64},
          descriptors_end_{descriptors_end},
          postings_end_{postings_end},
          selection_capacity_{plan.selections.capacity()},
          plan_max_occ_cap_{plan.max_occ_cap},
          runtime_max_occ_{plan.runtime_max_occ},
          plan_posting_width_bytes_{plan.posting_width_bytes} {}

    friend std::expected<compact_hash32_selection_span_range_accessor,
        direct_inverted_index_error>
    bind_compact_hash32_direct_index_selection_spans(
        const compact_hash32_direct_index_view&,
        const compact_hash32_unique_query_plan&);

    template <class p_visitor>
    friend std::expected<compact_hash32_selection_span_visit_stats,
        direct_inverted_index_error>
    visit_compact_hash32_direct_index_selection_span_range(
        const compact_hash32_selection_span_range_accessor&,
        std::uint64_t,
        std::uint64_t,
        p_visitor&&);
};

struct compact_hash32_query_touch_estimate {
    std::uint64_t query_hashes = 0u;
    std::uint64_t distinct_rank_groups = 0u;
    std::uint64_t accepted_query_hashes = 0u;
    std::uint64_t maximum_membership_bytes = 0u;
    std::uint64_t maximum_rank_bytes = 0u;
    std::uint64_t maximum_descriptor_bytes = 0u;
    std::uint64_t maximum_metadata_bytes = 0u;
};

struct compact_hash32_image_estimate {
    compact_hash32_geometry geometry{};
    std::uint8_t posting_width_bytes = 0u;
    std::uint64_t accepted_hash_count = 0u;
    std::uint64_t accepted_posting_count = 0u;
    std::uint64_t inline_singleton_count = 0u;
    std::uint64_t materialized_hash_count = 0u;
    std::uint64_t materialized_posting_count = 0u;
    std::uint64_t membership_bytes = 0u;
    std::uint64_t rank_bytes = 0u;
    std::uint64_t descriptor_bytes = 0u;
    std::uint64_t posting_bytes = 0u;
    std::uint64_t image_bytes = 0u;
};

[[nodiscard]] constexpr std::expected<compact_hash32_image_estimate,
    direct_inverted_index_error>
estimate_compact_hash32_image_bytes(
        compact_hash32_geometry geometry,
        std::uint64_t accepted_hash_count,
        std::uint64_t accepted_posting_count,
        std::uint8_t posting_width_bytes) noexcept {
    if (geometry.hash_domain_bits < 9u
        || geometry.hash_domain_bits > k_compact_hash32_full_hash_bits
        || (posting_width_bytes != 4u && posting_width_bytes != 8u)
        || accepted_posting_count
            > (std::numeric_limits<std::uint32_t>::max)()) {
        return std::unexpected(direct_inverted_index_error::invalid_options);
    }
    const std::uint32_t expected_max = geometry.hash_domain_bits == 32u
        ? (std::numeric_limits<std::uint32_t>::max)()
        : static_cast<std::uint32_t>(
            (std::uint64_t{1u} << geometry.hash_domain_bits) - 1u);
    if (geometry.max_hash != expected_max) {
        return std::unexpected(direct_inverted_index_error::storage_contract_mismatch);
    }
    const std::uint64_t domain_hashes =
        std::uint64_t{1u} << geometry.hash_domain_bits;
    if (accepted_hash_count > domain_hashes
        || accepted_hash_count > accepted_posting_count) {
        return std::unexpected(direct_inverted_index_error::invalid_options);
    }
    compact_hash32_image_estimate estimate{
        .geometry = geometry,
        .posting_width_bytes = posting_width_bytes,
        .accepted_hash_count = accepted_hash_count,
        .accepted_posting_count = accepted_posting_count,
        // With U nonempty runs and P postings, at least 2U-P runs are
        // singletons.  The compact P32 encoding can inline that guaranteed
        // population; the estimate is therefore a conservative upper bound
        // when the caller has only aggregate U/P stats.
        .inline_singleton_count = posting_width_bytes == 4u
                && accepted_hash_count
                    > accepted_posting_count - accepted_hash_count
            ? accepted_hash_count
                - (accepted_posting_count - accepted_hash_count)
            : 0u,
        .membership_bytes =
            (std::uint64_t{1u} << geometry.hash_domain_bits) / 8u,
        .rank_bytes =
            ((std::uint64_t{1u} << geometry.hash_domain_bits)
                / k_compact_hash32_direct_index_rank_group_bits) * 4u,
    };
    estimate.materialized_posting_count =
        accepted_posting_count - estimate.inline_singleton_count;
    estimate.materialized_hash_count =
        accepted_hash_count - estimate.inline_singleton_count;
    if (!direct_index_detail::checked_multiply(
            accepted_hash_count,
            k_compact_hash32_direct_index_descriptor_bytes,
            estimate.descriptor_bytes)
        || !direct_index_detail::checked_multiply(
            estimate.materialized_posting_count,
            posting_width_bytes,
            estimate.posting_bytes)) {
        return std::unexpected(
            direct_inverted_index_error::arithmetic_overflow);
    }
    std::uint64_t cursor = k_compact_hash32_direct_index_header_bytes;
    std::uint64_t offset = 0u;
    if (!direct_index_detail::align_up(cursor,
            k_direct_inverted_index_section_alignment, offset)
        || !direct_index_detail::checked_add(
            offset, estimate.membership_bytes, cursor)
        || !direct_index_detail::align_up(cursor,
            k_direct_inverted_index_section_alignment, offset)
        || !direct_index_detail::checked_add(
            offset, estimate.rank_bytes, cursor)
        || !direct_index_detail::align_up(cursor,
            k_direct_inverted_index_section_alignment, offset)
        || !direct_index_detail::checked_add(
            offset, estimate.descriptor_bytes, cursor)
        || !direct_index_detail::align_up(cursor,
            k_direct_inverted_index_section_alignment, offset)
        || !direct_index_detail::checked_add(
            offset, estimate.posting_bytes, estimate.image_bytes)) {
        return std::unexpected(
            direct_inverted_index_error::arithmetic_overflow);
    }
    return estimate;
}

[[nodiscard]] constexpr std::expected<compact_hash32_query_touch_estimate,
    direct_inverted_index_error>
estimate_compact_hash32_query_touches(
        std::uint64_t query_hashes,
        std::uint64_t distinct_rank_groups,
        std::uint64_t accepted_query_hashes) noexcept {
    if (distinct_rank_groups > query_hashes
        || accepted_query_hashes > query_hashes
        || distinct_rank_groups > (std::uint64_t{1u} << 24u)) {
        return std::unexpected(direct_inverted_index_error::invalid_options);
    }
    compact_hash32_query_touch_estimate estimate{
        .query_hashes = query_hashes,
        .distinct_rank_groups = distinct_rank_groups,
        .accepted_query_hashes = accepted_query_hashes,
    };
    // One hash32 lookup popcounts at most four 64-bit words.  Monotonic batches
    // reuse the 4-byte rank checkpoint for every query in the same group.
    if (!direct_index_detail::checked_multiply(
            query_hashes, 32u, estimate.maximum_membership_bytes)
        || !direct_index_detail::checked_multiply(
            distinct_rank_groups, 4u, estimate.maximum_rank_bytes)
        || !direct_index_detail::checked_multiply(
            accepted_query_hashes,
            k_compact_hash32_direct_index_descriptor_bytes,
            estimate.maximum_descriptor_bytes)
        || !direct_index_detail::checked_add(
            estimate.maximum_membership_bytes,
            estimate.maximum_rank_bytes,
            estimate.maximum_metadata_bytes)
        || !direct_index_detail::checked_add(
            estimate.maximum_metadata_bytes,
            estimate.maximum_descriptor_bytes,
            estimate.maximum_metadata_bytes)) {
        return std::unexpected(
            direct_inverted_index_error::arithmetic_overflow);
    }
    return estimate;
}

} // namespace bioseq::indexing

namespace bioseq::indexing::compact_hash32_detail {

using direct_index_detail::crc64_state;
using direct_index_detail::align_up;
using direct_index_detail::checked_add;
using direct_index_detail::checked_multiply;
using direct_index_detail::crc64;
using direct_index_detail::header_crc;
using direct_index_detail::read_le;
using direct_index_detail::write_le;

struct compact_hash32_unique_query_plan_access {
    static void seal(compact_hash32_unique_query_plan& plan) noexcept {
        plan.authentication_crc64_ = plan.computed_authentication_();
    }

    [[nodiscard]] static bool authenticated(
            const compact_hash32_unique_query_plan& plan) noexcept {
        return plan.authentication_crc64_ != 0u
            && plan.authentication_crc64_
                == plan.computed_authentication_();
    }

    /// Return the already-authenticated seal without rescanning selections.
    /// Callers must first establish `authenticated(plan)` and retain the plan
    /// immutably while using this constant-time receipt.
    [[nodiscard]] static std::uint64_t authentication_receipt(
            const compact_hash32_unique_query_plan& plan) noexcept {
        return plan.authentication_crc64_;
    }
};

struct compact_hash32_geometry_counts {
    std::uint64_t hash_count = 0u;
    std::uint64_t membership_bytes = 0u;
    std::uint64_t rank_checkpoint_count = 0u;
    std::uint64_t rank_bytes = 0u;
};

[[nodiscard]] constexpr std::expected<compact_hash32_geometry_counts,
    direct_inverted_index_error>
geometry_counts(compact_hash32_geometry geometry) noexcept {
    if (geometry.hash_domain_bits < 9u
        || geometry.hash_domain_bits > k_compact_hash32_full_hash_bits) {
        return std::unexpected(direct_inverted_index_error::invalid_options);
    }
    const std::uint32_t expected_max = geometry.hash_domain_bits == 32u
        ? (std::numeric_limits<std::uint32_t>::max)()
        : static_cast<std::uint32_t>(
            (std::uint64_t{1u} << geometry.hash_domain_bits) - 1u);
    if (geometry.max_hash != expected_max) {
        return std::unexpected(direct_inverted_index_error::storage_contract_mismatch);
    }
    const std::uint64_t hash_count =
        std::uint64_t{1u} << geometry.hash_domain_bits;
    return compact_hash32_geometry_counts{
        .hash_count = hash_count,
        .membership_bytes = hash_count / 8u,
        .rank_checkpoint_count =
            hash_count / k_compact_hash32_direct_index_rank_group_bits,
        .rank_bytes =
            hash_count / k_compact_hash32_direct_index_rank_group_bits * 4u,
    };
}

template <class p_unsigned_integer>
requires std::is_unsigned_v<p_unsigned_integer>
void crc_update_le(
        crc64_state& crc, p_unsigned_integer value) noexcept {
    crc.update_little_endian(value);
}

struct compact_hash32_scan_receipt {
    std::uint64_t source_hash_count = 0u;
    std::uint64_t source_posting_count = 0u;
    std::uint64_t accepted_hash_count = 0u;
    std::uint64_t accepted_posting_count = 0u;
    std::uint64_t inline_singleton_count = 0u;
    std::uint64_t rejected_hash_count = 0u;
    std::uint64_t rejected_posting_count = 0u;
    std::uint64_t sorted_stream_crc64 = 0u;

    [[nodiscard]] constexpr bool operator==(
        const compact_hash32_scan_receipt&) const noexcept = default;
};

struct compact_hash32_writer_state {
    std::span<std::byte> image{};
    const compact_hash32_image_plan* plan = nullptr;
    std::uint64_t next_rank_checkpoint = 0u;
    std::uint64_t inline_singletons_written = 0u;
    std::uint64_t materialized_hashes_written = 0u;
    std::uint64_t materialized_positions_written = 0u;
};

class compact_hash32_scan_state {
private:
    const compact_hash32_build_options& options_;
    compact_hash32_geometry_counts geometry_{};
    compact_hash32_writer_state* writer_ = nullptr;
    compact_hash32_scan_receipt receipt_{};
    crc64_state stream_crc_{};
    std::optional<direct_inverted_index_error> error_{};
    compact_hash32_sorted_run_header active_{};
    bool active_run_ = false;
    bool active_accepted_ = false;
    bool have_previous_hash_ = false;
    std::uint32_t previous_hash_ = 0u;
    std::uint64_t active_positions_seen_ = 0u;
    std::uint64_t active_descriptor_offset_ = 0u;
    bool active_inline_singleton_ = false;
    std::array<std::uint64_t, 2> previous_bp_{};
    std::array<bool, 2> have_previous_bp_{};

    [[nodiscard]] std::expected<void, direct_inverted_index_error>
    fail_(direct_inverted_index_error error) noexcept {
        if (!error_) error_ = error;
        return std::unexpected(*error_);
    }

    [[nodiscard]] bool writer_range_within_(
            std::uint64_t offset,
            std::uint64_t bytes,
            std::uint64_t section_offset,
            std::uint64_t section_bytes) const noexcept {
        if (writer_ == nullptr || writer_->plan == nullptr) return false;
        std::uint64_t end = 0u;
        std::uint64_t section_end = 0u;
        return checked_add(offset, bytes, end)
            && checked_add(section_offset, section_bytes, section_end)
            && offset >= section_offset
            && end <= section_end
            && end <= writer_->image.size();
    }

    [[nodiscard]] std::expected<void, direct_inverted_index_error>
    validate_replay_prefix_(
            bool accepted,
            std::uint64_t accepted_hash_before,
            std::uint64_t accepted_posting_before,
            const compact_hash32_sorted_run_header& header) noexcept {
        if (writer_ == nullptr) return {};
        if (writer_->plan == nullptr
            || receipt_.source_hash_count
                > writer_->plan->source_hash_count
            || receipt_.source_posting_count
                > writer_->plan->source_posting_count
            || receipt_.accepted_hash_count
                > writer_->plan->accepted_hash_count
            || receipt_.accepted_posting_count
                > writer_->plan->accepted_posting_count
            || receipt_.inline_singleton_count
                > writer_->plan->inline_singleton_count
            || receipt_.rejected_hash_count
                > writer_->plan->rejected_hash_count
            || receipt_.rejected_posting_count
                > writer_->plan->rejected_posting_count
            || (accepted
                && (accepted_hash_before
                        >= writer_->plan->accepted_hash_count
                    || accepted_posting_before
                        > writer_->plan->accepted_posting_count
                    || header.global_count
                        > writer_->plan->accepted_posting_count
                            - accepted_posting_before))) {
            return fail_(direct_inverted_index_error::replay_mismatch);
        }
        return {};
    }

    [[nodiscard]] std::expected<void, direct_inverted_index_error>
    finalize_active_() noexcept {
        if (!active_run_) return {};
        if (active_positions_seen_ != active_.global_count) {
            return fail_(direct_inverted_index_error::source_read);
        }
        active_run_ = false;
        active_accepted_ = false;
        return {};
    }

    [[nodiscard]] std::expected<void, direct_inverted_index_error>
    write_rank_through_(
            std::uint64_t checkpoint,
            std::uint64_t accepted_before) noexcept {
        if (writer_ == nullptr) return {};
        while (writer_->next_rank_checkpoint <= checkpoint) {
            if (accepted_before
                > (std::numeric_limits<std::uint32_t>::max)()) {
                return fail_(direct_inverted_index_error::fixed_width);
            }
            std::uint64_t byte_delta = 0u;
            std::uint64_t byte_offset = 0u;
            if (writer_->next_rank_checkpoint
                    >= geometry_.rank_checkpoint_count
                || !checked_multiply(writer_->next_rank_checkpoint,
                    4u, byte_delta)
                || !checked_add(writer_->plan->rank_offset,
                    byte_delta, byte_offset)
                || !writer_range_within_(byte_offset, 4u,
                    writer_->plan->rank_offset,
                    writer_->plan->rank_bytes)) {
                return fail_(direct_inverted_index_error::replay_mismatch);
            }
            write_le<std::uint32_t>(
                writer_->image,
                static_cast<std::size_t>(byte_offset),
                static_cast<std::uint32_t>(accepted_before));
            ++writer_->next_rank_checkpoint;
        }
        return {};
    }

public:
    compact_hash32_scan_state(
            const compact_hash32_build_options& options,
            compact_hash32_geometry_counts geometry,
            compact_hash32_writer_state* writer) noexcept
        : options_(options), geometry_(geometry), writer_(writer) {}

    [[nodiscard]] std::expected<void, direct_inverted_index_error>
    begin_run(const compact_hash32_sorted_run_header& header) noexcept {
        if (error_) return std::unexpected(*error_);
        if (auto finalized = finalize_active_(); !finalized) {
            return finalized;
        }
        if (header.global_count == 0u
            || header.target_strand_zero_count > header.global_count
            || (have_previous_hash_ && header.hash <= previous_hash_)
            || static_cast<std::uint64_t>(header.hash)
                >= geometry_.hash_count) {
            return fail_(have_previous_hash_ && header.hash <= previous_hash_
                ? direct_inverted_index_error::order
                : direct_inverted_index_error::occurrence_range);
        }
        std::uint64_t next = 0u;
        if (!checked_add(
                receipt_.source_hash_count, 1u,
                receipt_.source_hash_count)
            || !checked_add(
                receipt_.source_posting_count, header.global_count,
                receipt_.source_posting_count)) {
            return fail_(direct_inverted_index_error::arithmetic_overflow);
        }
        const bool accepted = header.global_count <= options_.max_occ_cap;
        const bool inline_singleton = accepted
            && options_.identity.target_length
                <= (std::uint64_t{1u} << 31u)
            && header.global_count == 1u;
        const std::uint64_t accepted_hash_before =
            receipt_.accepted_hash_count;
        const std::uint64_t accepted_posting_before =
            receipt_.accepted_posting_count;
        if (accepted) {
            if (header.global_count
                    > (std::numeric_limits<std::uint32_t>::max)()
                || header.target_strand_zero_count
                    > (std::numeric_limits<std::uint32_t>::max)()
                || !checked_add(receipt_.accepted_hash_count, 1u,
                    receipt_.accepted_hash_count)
                || !checked_add(receipt_.accepted_posting_count,
                    header.global_count,
                    receipt_.accepted_posting_count)
                || (inline_singleton
                    && !checked_add(receipt_.inline_singleton_count, 1u,
                        receipt_.inline_singleton_count))
                || receipt_.accepted_posting_count
                    > (std::numeric_limits<std::uint32_t>::max)()) {
                return fail_(direct_inverted_index_error::fixed_width);
            }
        } else if (!checked_add(
                receipt_.rejected_hash_count, 1u,
                receipt_.rejected_hash_count)
            || !checked_add(receipt_.rejected_posting_count,
                header.global_count,
                receipt_.rejected_posting_count)) {
            return fail_(direct_inverted_index_error::arithmetic_overflow);
        }
        if (!checked_add(receipt_.accepted_hash_count,
                receipt_.rejected_hash_count, next)
            || next != receipt_.source_hash_count) {
            return fail_(direct_inverted_index_error::arithmetic_overflow);
        }

        if (auto prefix = validate_replay_prefix_(
                accepted,
                accepted_hash_before,
                accepted_posting_before,
                header); !prefix) {
            return prefix;
        }

        crc_update_le(stream_crc_, header.hash);
        crc_update_le(stream_crc_, header.global_count);
        crc_update_le(stream_crc_, header.target_strand_zero_count);

        if (writer_ != nullptr) {
            const auto address = resolve_compact_hash32_address(
                header.hash, options_.geometry);
            if (!address) return fail_(address.error());
            if (auto ranks = write_rank_through_(
                    address->rank_checkpoint_ordinal,
                    accepted_hash_before); !ranks) {
                return ranks;
            }
            if (accepted) {
                std::uint64_t membership_offset = 0u;
                std::uint64_t descriptor_delta = 0u;
                std::uint64_t descriptor_offset = 0u;
                if (address->membership_byte_ordinal
                        >= writer_->plan->membership_bytes
                    || !checked_add(writer_->plan->membership_offset,
                        address->membership_byte_ordinal,
                        membership_offset)
                    || !writer_range_within_(membership_offset, 1u,
                        writer_->plan->membership_offset,
                        writer_->plan->membership_bytes)
                    || accepted_hash_before
                        >= writer_->plan->accepted_hash_count
                    || !checked_multiply(accepted_hash_before,
                        writer_->plan->descriptor_width_bytes,
                        descriptor_delta)
                    || !checked_add(writer_->plan->descriptors_offset,
                        descriptor_delta, descriptor_offset)
                    || !writer_range_within_(descriptor_offset,
                        writer_->plan->descriptor_width_bytes,
                        writer_->plan->descriptors_offset,
                        writer_->plan->descriptors_bytes)
                    || writer_->plan->layout_revision
                        != k_compact_hash32_direct_index_layout_revision
                    || writer_->plan->descriptor_width_bytes
                        != k_compact_hash32_direct_index_descriptor_bytes
                    || header.global_count
                        > (std::numeric_limits<std::uint16_t>::max)()
                    || header.target_strand_zero_count
                        > (std::numeric_limits<std::uint16_t>::max)()) {
                    return fail_(
                        direct_inverted_index_error::replay_mismatch);
                }
                writer_->image[static_cast<std::size_t>(
                    membership_offset)] |= static_cast<std::byte>(
                    std::uint8_t{1u} << address->membership_bit);
                write_le<std::uint32_t>(writer_->image,
                    static_cast<std::size_t>(descriptor_offset),
                    inline_singleton
                        ? 0u
                        : static_cast<std::uint32_t>(
                            writer_->materialized_positions_written));
                write_le<std::uint16_t>(writer_->image,
                    static_cast<std::size_t>(descriptor_offset + 4u),
                    static_cast<std::uint16_t>(header.global_count));
                write_le<std::uint16_t>(writer_->image,
                    static_cast<std::size_t>(descriptor_offset + 6u),
                    static_cast<std::uint16_t>(
                        header.target_strand_zero_count));
                active_descriptor_offset_ = descriptor_offset;
                active_inline_singleton_ = inline_singleton;
                if (!inline_singleton) {
                    ++writer_->materialized_hashes_written;
                }
            }
        }

        active_ = header;
        active_run_ = true;
        active_accepted_ = accepted;
        active_positions_seen_ = 0u;
        previous_bp_.fill(0u);
        have_previous_bp_.fill(false);
        previous_hash_ = header.hash;
        have_previous_hash_ = true;
        return {};
    }

    [[nodiscard]] std::expected<void, direct_inverted_index_error>
    consume_positions(
            std::span<const std::uint64_t> encoded_positions) noexcept {
        if (error_) return std::unexpected(*error_);
        if (!active_run_
            || encoded_positions.size()
                > active_.global_count - active_positions_seen_) {
            return fail_(direct_inverted_index_error::source_read);
        }
        for (const std::uint64_t encoded : encoded_positions) {
            const bool strand_one = (encoded & 1u) != 0u;
            const bool expected_strand = active_positions_seen_
                >= active_.target_strand_zero_count;
            const std::uint64_t bp = encoded >> 1u;
            if (strand_one != expected_strand
                || bp >= options_.identity.target_length
                || (have_previous_bp_[strand_one]
                    && bp < previous_bp_[strand_one])) {
                return fail_(strand_one != expected_strand
                    ? direct_inverted_index_error::order
                    : direct_inverted_index_error::posting);
            }
            crc_update_le(stream_crc_, encoded);
            if (writer_ != nullptr && active_accepted_) {
                if (active_inline_singleton_) {
                    if (writer_->plan == nullptr
                        || writer_->plan->posting_width_bytes != 4u
                        || active_.global_count != 1u
                        || active_positions_seen_ != 0u
                        || encoded
                            > (std::numeric_limits<std::uint32_t>::max)()
                        || !writer_range_within_(
                            active_descriptor_offset_, 4u,
                            writer_->plan->descriptors_offset,
                            writer_->plan->descriptors_bytes)) {
                        return fail_(
                            direct_inverted_index_error::replay_mismatch);
                    }
                    write_le<std::uint32_t>(writer_->image,
                        static_cast<std::size_t>(
                            active_descriptor_offset_),
                        static_cast<std::uint32_t>(encoded));
                    ++writer_->inline_singletons_written;
                    previous_bp_[strand_one] = bp;
                    have_previous_bp_[strand_one] = true;
                    ++active_positions_seen_;
                    continue;
                }
                std::uint64_t posting_delta = 0u;
                std::uint64_t posting_offset = 0u;
                if (writer_->plan == nullptr
                    || writer_->materialized_positions_written
                        >= writer_->plan->materialized_posting_count
                    || !checked_multiply(
                        writer_->materialized_positions_written,
                        writer_->plan->posting_width_bytes,
                        posting_delta)
                    || !checked_add(writer_->plan->postings_offset,
                        posting_delta, posting_offset)
                    || !writer_range_within_(posting_offset,
                        writer_->plan->posting_width_bytes,
                        writer_->plan->postings_offset,
                        writer_->plan->postings_bytes)) {
                    return fail_(
                        direct_inverted_index_error::replay_mismatch);
                }
                if (writer_->plan->posting_width_bytes == 4u) {
                    if (encoded
                        > (std::numeric_limits<std::uint32_t>::max)()) {
                        return fail_(direct_inverted_index_error::posting);
                    }
                    write_le<std::uint32_t>(writer_->image,
                        static_cast<std::size_t>(posting_offset),
                        static_cast<std::uint32_t>(encoded));
                } else {
                    write_le<std::uint64_t>(writer_->image,
                        static_cast<std::size_t>(posting_offset), encoded);
                }
                ++writer_->materialized_positions_written;
            }
            previous_bp_[strand_one] = bp;
            have_previous_bp_[strand_one] = true;
            ++active_positions_seen_;
        }
        return {};
    }

    [[nodiscard]] std::expected<compact_hash32_scan_receipt,
        direct_inverted_index_error>
    finish() noexcept {
        if (error_) return std::unexpected(*error_);
        if (auto finalized = finalize_active_(); !finalized) {
            return std::unexpected(finalized.error());
        }
        if (writer_ != nullptr) {
            if (receipt_.accepted_hash_count
                    > (std::numeric_limits<std::uint32_t>::max)()
                && writer_->next_rank_checkpoint
                    < geometry_.rank_checkpoint_count) {
                return std::unexpected(
                    direct_inverted_index_error::fixed_width);
            }
            while (writer_->next_rank_checkpoint
                    < geometry_.rank_checkpoint_count) {
                std::uint64_t delta = 0u;
                std::uint64_t offset = 0u;
                if (!checked_multiply(writer_->next_rank_checkpoint,
                        4u, delta)
                    || !checked_add(writer_->plan->rank_offset,
                        delta, offset)
                    || !writer_range_within_(offset, 4u,
                        writer_->plan->rank_offset,
                        writer_->plan->rank_bytes)) {
                    return std::unexpected(
                        direct_inverted_index_error::replay_mismatch);
                }
                write_le<std::uint32_t>(writer_->image,
                    static_cast<std::size_t>(offset),
                    static_cast<std::uint32_t>(
                        receipt_.accepted_hash_count));
                ++writer_->next_rank_checkpoint;
            }
            if (writer_->inline_singletons_written
                    != receipt_.inline_singleton_count
                || writer_->inline_singletons_written
                    != writer_->plan->inline_singleton_count
                || writer_->materialized_hashes_written
                    != writer_->plan->materialized_hash_count
                || writer_->materialized_positions_written
                    != writer_->plan->materialized_posting_count) {
                return std::unexpected(
                    direct_inverted_index_error::replay_mismatch);
            }
        }
        receipt_.sorted_stream_crc64 = stream_crc_.checksum();
        return receipt_;
    }

    [[nodiscard]] static std::expected<void,
        direct_inverted_index_error>
    begin_thunk(
            void* context,
            const compact_hash32_sorted_run_header& header) noexcept {
        return static_cast<compact_hash32_scan_state*>(context)->begin_run(header);
    }

    [[nodiscard]] static std::expected<void,
        direct_inverted_index_error>
    consume_thunk(
            void* context,
            std::span<const std::uint64_t> positions) noexcept {
        return static_cast<compact_hash32_scan_state*>(context)->consume_positions(positions);
    }
};

[[nodiscard]] inline std::expected<compact_hash32_scan_receipt,
    direct_inverted_index_error>
scan_source(
        const compact_hash32_sorted_run_source& source,
        const compact_hash32_build_options& options,
        compact_hash32_writer_state* writer = nullptr) {
    const auto geometry = geometry_counts(options.geometry);
    if (!geometry) return std::unexpected(geometry.error());
    compact_hash32_scan_state state{options, *geometry, writer};
    const compact_hash32_sorted_run_receiver receiver{
        .context = &state,
        .begin_run_fn = &compact_hash32_scan_state::begin_thunk,
        .consume_positions_fn = &compact_hash32_scan_state::consume_thunk,
    };
    if (auto scanned = source.scan_sorted_runs(receiver); !scanned) {
        return std::unexpected(scanned.error());
    }
    return state.finish();
}

[[nodiscard]] constexpr std::expected<compact_hash32_image_plan,
    direct_inverted_index_error>
layout_from_receipt(
        const compact_hash32_build_options& options,
        const compact_hash32_scan_receipt& receipt) noexcept {
    const auto geometry = geometry_counts(options.geometry);
    if (!geometry) return std::unexpected(geometry.error());
    const std::uint8_t posting_width_bytes =
        options.identity.target_length <= (std::uint64_t{1u} << 31u)
        ? 4u : 8u;
    if (options.identity.target_length > (std::uint64_t{1u} << 63u)
        || options.identity.generation_id == 0u
        || options.identity.source_identity == 0u
        || options.identity.storage_contract_checksum == 0u
        || (options.max_occ_cap
            > (std::numeric_limits<std::uint16_t>::max)())) {
        return std::unexpected(direct_inverted_index_error::invalid_options);
    }
    if (receipt.source_hash_count > geometry->hash_count
        || receipt.accepted_hash_count > receipt.accepted_posting_count
        || receipt.inline_singleton_count
            > receipt.accepted_hash_count
        || receipt.inline_singleton_count
            > receipt.accepted_posting_count
        || (posting_width_bytes == 8u
            && receipt.inline_singleton_count != 0u)
        || receipt.accepted_posting_count
            > (std::numeric_limits<std::uint32_t>::max)()) {
        return std::unexpected(direct_inverted_index_error::fixed_width);
    }
    std::uint64_t source_hash_count = 0u;
    std::uint64_t source_posting_count = 0u;
    if (!checked_add(receipt.accepted_hash_count,
            receipt.rejected_hash_count, source_hash_count)
        || source_hash_count != receipt.source_hash_count
        || !checked_add(receipt.accepted_posting_count,
            receipt.rejected_posting_count, source_posting_count)
        || source_posting_count != receipt.source_posting_count) {
        return std::unexpected(direct_inverted_index_error::storage_contract_mismatch);
    }
    compact_hash32_image_plan plan{
        .identity = options.identity,
        .layout_revision = k_compact_hash32_direct_index_layout_revision,
        .descriptor_width_bytes =
            k_compact_hash32_direct_index_descriptor_bytes,
        .max_occ_cap = options.max_occ_cap,
        .posting_width_bytes = posting_width_bytes,
        .geometry = options.geometry,
        .source_hash_count = receipt.source_hash_count,
        .source_posting_count = receipt.source_posting_count,
        .accepted_hash_count = receipt.accepted_hash_count,
        .accepted_posting_count = receipt.accepted_posting_count,
        .inline_singleton_count = receipt.inline_singleton_count,
        .materialized_hash_count = receipt.accepted_hash_count
            - receipt.inline_singleton_count,
        .materialized_posting_count = receipt.accepted_posting_count
            - receipt.inline_singleton_count,
        .rejected_hash_count = receipt.rejected_hash_count,
        .rejected_posting_count = receipt.rejected_posting_count,
        .membership_bytes = geometry->membership_bytes,
        .rank_bytes = geometry->rank_bytes,
        .sorted_stream_crc64 = receipt.sorted_stream_crc64,
    };
    if (!checked_multiply(plan.accepted_hash_count,
            plan.descriptor_width_bytes,
            plan.descriptors_bytes)
        || !checked_multiply(plan.materialized_posting_count,
            plan.posting_width_bytes, plan.postings_bytes)) {
        return std::unexpected(
            direct_inverted_index_error::arithmetic_overflow);
    }
    std::uint64_t cursor = k_compact_hash32_direct_index_header_bytes;
    if (!align_up(cursor, k_direct_inverted_index_section_alignment,
            plan.membership_offset)
        || !checked_add(plan.membership_offset,
            plan.membership_bytes, cursor)
        || !align_up(cursor, k_direct_inverted_index_section_alignment,
            plan.rank_offset)
        || !checked_add(plan.rank_offset, plan.rank_bytes, cursor)
        || !align_up(cursor, k_direct_inverted_index_section_alignment,
            plan.descriptors_offset)
        || !checked_add(plan.descriptors_offset,
            plan.descriptors_bytes, cursor)
        || !align_up(cursor, k_direct_inverted_index_section_alignment,
            plan.postings_offset)
        || !checked_add(plan.postings_offset,
            plan.postings_bytes, plan.image_bytes)) {
        return std::unexpected(
            direct_inverted_index_error::arithmetic_overflow);
    }
    return plan;
}

struct compact_hash32_decoded_descriptor {
    std::uint32_t posting_ordinal = 0u;
    std::uint32_t inline_encoded_position = 0u;
    std::uint32_t global_count = 0u;
    std::uint32_t strand_zero_count = 0u;
    bool inline_singleton = false;
};

[[nodiscard]] constexpr compact_hash32_decoded_descriptor decode_descriptor(
        std::span<const std::byte> image,
        std::uint64_t offset,
        std::uint8_t posting_width_bytes) noexcept {
    const std::uint32_t posting_or_inline = read_le<std::uint32_t>(
        image, static_cast<std::size_t>(offset));
    const std::uint32_t global_count = read_le<std::uint16_t>(
        image, static_cast<std::size_t>(offset + 4u));
    const bool inline_singleton = posting_width_bytes == 4u
        && global_count == 1u;
    return compact_hash32_decoded_descriptor{
        .posting_ordinal = inline_singleton ? 0u : posting_or_inline,
        .inline_encoded_position = inline_singleton
            ? posting_or_inline : 0u,
        .global_count = global_count,
        .strand_zero_count = read_le<std::uint16_t>(
            image, static_cast<std::size_t>(offset + 6u)),
        .inline_singleton = inline_singleton,
    };
}

[[nodiscard]] constexpr std::uint64_t decode_posting(
        std::span<const std::byte> image,
        std::uint64_t offset,
        std::uint8_t width) noexcept {
    return width == 4u
        ? static_cast<std::uint64_t>(read_le<std::uint32_t>(
            image, static_cast<std::size_t>(offset)))
        : read_le<std::uint64_t>(
            image, static_cast<std::size_t>(offset));
}

} // namespace bioseq::indexing::compact_hash32_detail

export namespace bioseq::indexing {

[[nodiscard]] inline std::expected<compact_hash32_image_plan,
    direct_inverted_index_error>
plan_compact_hash32_direct_index(
        const compact_hash32_sorted_run_source& source,
        const compact_hash32_build_options& options) {
    if (options.max_occ_cap
            > (std::numeric_limits<std::uint16_t>::max)()) {
        return std::unexpected(direct_inverted_index_error::invalid_options);
    }
    const auto receipt = compact_hash32_detail::scan_source(source, options);
    if (!receipt) return std::unexpected(receipt.error());
    auto plan = compact_hash32_detail::layout_from_receipt(options, *receipt);
    if (!plan) return std::unexpected(plan.error());
    if (plan->image_bytes > options.max_image_bytes
        || plan->image_bytes
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::size_t>::max)())) {
        return std::unexpected(direct_inverted_index_error::admission);
    }
    return plan;
}

[[nodiscard]] inline std::expected<void, direct_inverted_index_error>
build_compact_hash32_direct_index_into(
        const compact_hash32_sorted_run_source& source,
        const compact_hash32_build_options& options,
        const compact_hash32_image_plan& planned,
        std::span<std::byte> image) {
    using namespace compact_hash32_detail;
    const compact_hash32_scan_receipt claimed_receipt{
        .source_hash_count = planned.source_hash_count,
        .source_posting_count = planned.source_posting_count,
        .accepted_hash_count = planned.accepted_hash_count,
        .accepted_posting_count = planned.accepted_posting_count,
        .inline_singleton_count = planned.inline_singleton_count,
        .rejected_hash_count = planned.rejected_hash_count,
        .rejected_posting_count = planned.rejected_posting_count,
        .sorted_stream_crc64 = planned.sorted_stream_crc64,
    };
    const auto canonical = layout_from_receipt(options, claimed_receipt);
    if (!canonical || *canonical != planned
        || planned.image_bytes > options.max_image_bytes
        || planned.image_bytes
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::size_t>::max)())
        || image.size() != planned.image_bytes) {
        return std::unexpected(direct_inverted_index_error::storage_contract_mismatch);
    }
    std::fill(image.begin(), image.end(), std::byte{0});
    compact_hash32_writer_state writer{
        .image = image,
        .plan = &planned,
    };
    const auto replay = scan_source(source, options, &writer);
    if (!replay) return std::unexpected(replay.error());
    const compact_hash32_scan_receipt expected{
        .source_hash_count = planned.source_hash_count,
        .source_posting_count = planned.source_posting_count,
        .accepted_hash_count = planned.accepted_hash_count,
        .accepted_posting_count = planned.accepted_posting_count,
        .inline_singleton_count = planned.inline_singleton_count,
        .rejected_hash_count = planned.rejected_hash_count,
        .rejected_posting_count = planned.rejected_posting_count,
        .sorted_stream_crc64 = planned.sorted_stream_crc64,
    };
    if (*replay != expected) {
        return std::unexpected(direct_inverted_index_error::replay_mismatch);
    }

    const auto section = [&image](
            std::uint64_t offset, std::uint64_t bytes) noexcept {
        return std::span<const std::byte>{image}.subspan(
            static_cast<std::size_t>(offset),
            static_cast<std::size_t>(bytes));
    };
    const std::uint64_t membership_crc = crc64(section(
        planned.membership_offset, planned.membership_bytes));
    const std::uint64_t rank_crc = crc64(section(
        planned.rank_offset, planned.rank_bytes));
    const std::uint64_t descriptors_crc = crc64(section(
        planned.descriptors_offset, planned.descriptors_bytes));
    const std::uint64_t postings_crc = crc64(section(
        planned.postings_offset, planned.postings_bytes));

    write_le<std::uint64_t>(image, 0u, k_compact_hash32_direct_index_magic);
    write_le<std::uint32_t>(image, 8u,
        planned.layout_revision);
    write_le<std::uint32_t>(image, 12u,
        k_compact_hash32_direct_index_header_bytes);
    write_le<std::uint32_t>(image, 16u, 0u);
    image[20u] = static_cast<std::byte>(planned.posting_width_bytes);
    image[21u] = static_cast<std::byte>(
        planned.geometry.hash_domain_bits);
    write_le<std::uint16_t>(image, 22u,
        planned.descriptor_width_bytes);
    write_le<std::uint64_t>(image, 24u,
        planned.identity.generation_id);
    write_le<std::uint64_t>(image, 32u,
        planned.identity.source_identity);
    write_le<std::uint64_t>(image, 40u,
        planned.identity.storage_contract_checksum);
    write_le<std::uint64_t>(image, 48u,
        planned.identity.target_length);
    write_le<std::uint32_t>(image, 56u, planned.max_occ_cap);
    write_le<std::uint32_t>(image, 60u,
        k_compact_hash32_direct_index_rank_group_bits);
    write_le<std::uint64_t>(image, 64u, planned.source_hash_count);
    write_le<std::uint64_t>(image, 72u,
        planned.source_posting_count);
    write_le<std::uint64_t>(image, 80u,
        planned.accepted_hash_count);
    write_le<std::uint64_t>(image, 88u,
        planned.accepted_posting_count);
    write_le<std::uint64_t>(image, 96u,
        planned.rejected_hash_count);
    write_le<std::uint64_t>(image, 104u,
        planned.rejected_posting_count);
    write_le<std::uint64_t>(image, 112u, planned.membership_offset);
    write_le<std::uint64_t>(image, 120u, planned.membership_bytes);
    write_le<std::uint64_t>(image, 128u, planned.rank_offset);
    write_le<std::uint64_t>(image, 136u, planned.rank_bytes);
    write_le<std::uint64_t>(image, 144u, planned.descriptors_offset);
    write_le<std::uint64_t>(image, 152u, planned.descriptors_bytes);
    write_le<std::uint64_t>(image, 160u, planned.postings_offset);
    write_le<std::uint64_t>(image, 168u, planned.postings_bytes);
    write_le<std::uint64_t>(image,
        k_compact_hash32_membership_crc_offset, membership_crc);
    write_le<std::uint64_t>(image,
        k_compact_hash32_rank_crc_offset, rank_crc);
    write_le<std::uint64_t>(image,
        k_compact_hash32_descriptors_crc_offset, descriptors_crc);
    write_le<std::uint64_t>(image,
        k_compact_hash32_postings_crc_offset, postings_crc);
    write_le<std::uint64_t>(image, 216u, planned.image_bytes);
    write_le<std::uint64_t>(image,
        k_compact_hash32_stream_crc_offset, planned.sorted_stream_crc64);
    write_le<std::uint32_t>(image, 232u, planned.geometry.max_hash);
    write_le<std::uint64_t>(image,
        k_compact_hash32_inline_singleton_count_offset,
        planned.inline_singleton_count);
    const std::uint64_t header_checksum = header_crc(
        std::span<const std::byte>{image}.first(
            k_compact_hash32_direct_index_header_bytes),
        k_compact_hash32_header_crc_offset,
        k_direct_inverted_index_checksum_bytes);
    write_le<std::uint64_t>(image,
        k_compact_hash32_header_crc_offset, header_checksum);
    return {};
}

[[nodiscard]] inline std::expected<std::vector<std::byte>,
    direct_inverted_index_error>
build_compact_hash32_direct_index(
        const compact_hash32_sorted_run_source& source,
        const compact_hash32_build_options& options) {
    const auto plan = plan_compact_hash32_direct_index(source, options);
    if (!plan) return std::unexpected(plan.error());
    try {
        std::vector<std::byte> image(
            static_cast<std::size_t>(plan->image_bytes), std::byte{0});
        if (auto encoded = build_compact_hash32_direct_index_into(
                source, options, *plan, image); !encoded) {
            return std::unexpected(encoded.error());
        }
        return image;
    } catch (const std::bad_alloc&) {
        return std::unexpected(direct_inverted_index_error::allocation);
    } catch (const std::length_error&) {
        return std::unexpected(direct_inverted_index_error::allocation);
    }
}

/// Bounded timed-open for a compact-hash32 section nested in an owning mapped
/// image.  This function reads exactly the fixed 256-byte direct header.  It
/// validates all persisted sizes and offsets arithmetically, but leaves the
/// intentionally O(image) payload CRC/padding/dictionary scrub to
/// validate_compact_hash32_direct_index().
[[nodiscard]] inline std::expected<compact_hash32_bounded_open_view,
    direct_inverted_index_error>
open_compact_hash32_direct_index_bounded(
        std::shared_ptr<const void> mapped_owner,
        std::span<const std::byte> direct_section,
        const compact_hash32_bounded_open_options& options) {
    using namespace compact_hash32_detail;
    if (!mapped_owner) {
        return std::unexpected(direct_inverted_index_error::invalid_options);
    }
    if (direct_section.size() < k_compact_hash32_direct_index_header_bytes
        || direct_section.data() == nullptr) {
        return std::unexpected(direct_inverted_index_error::image_size);
    }
    if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
        if (direct_section.size()
                > (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(direct_inverted_index_error::image_size);
        }
    }

    // From this point until the owning view is constructed, every load is
    // confined to this fixed span.  In particular, no padding byte or section
    // checksum target is examined by the timed-open path.
    const std::span<const std::byte> header_bytes =
        direct_section.first(k_compact_hash32_direct_index_header_bytes);
    if (read_le<std::uint64_t>(header_bytes, 0u)
            != k_compact_hash32_direct_index_magic) {
        return std::unexpected(direct_inverted_index_error::bad_magic);
    }
    const std::uint32_t layout_revision =
        read_le<std::uint32_t>(header_bytes, 8u);
    if (layout_revision != k_compact_hash32_direct_index_layout_revision) {
        return std::unexpected(direct_inverted_index_error::bad_layout_revision);
    }
    const std::uint16_t descriptor_width_bytes =
        read_le<std::uint16_t>(header_bytes, 22u);
    constexpr std::uint16_t expected_descriptor_width =
        k_compact_hash32_direct_index_descriptor_bytes;
    if (read_le<std::uint32_t>(header_bytes, 12u)
            != k_compact_hash32_direct_index_header_bytes
        || read_le<std::uint32_t>(header_bytes, 16u) != 0u
        || descriptor_width_bytes != expected_descriptor_width
        || read_le<std::uint32_t>(header_bytes, 60u)
            != k_compact_hash32_direct_index_rank_group_bits) {
        return std::unexpected(direct_inverted_index_error::bad_header);
    }

    compact_hash32_header header{};
    header.layout_revision = layout_revision;
    header.descriptor_width_bytes = descriptor_width_bytes;
    header.identity = direct_index_identity{
        .generation_id = read_le<std::uint64_t>(header_bytes, 24u),
        .source_identity = read_le<std::uint64_t>(header_bytes, 32u),
        .storage_contract_checksum = read_le<std::uint64_t>(header_bytes, 40u),
        .target_length = read_le<std::uint64_t>(header_bytes, 48u),
    };
    header.max_occ_cap = read_le<std::uint32_t>(header_bytes, 56u);
    header.posting_width_bytes =
        std::to_integer<std::uint8_t>(header_bytes[20u]);
    header.geometry = compact_hash32_geometry{
        .hash_domain_bits =
            std::to_integer<std::uint8_t>(header_bytes[21u]),
        .max_hash = read_le<std::uint32_t>(header_bytes, 232u),
    };
    header.source_hash_count = read_le<std::uint64_t>(header_bytes, 64u);
    header.source_posting_count = read_le<std::uint64_t>(header_bytes, 72u);
    header.accepted_hash_count = read_le<std::uint64_t>(header_bytes, 80u);
    header.accepted_posting_count =
        read_le<std::uint64_t>(header_bytes, 88u);
    header.inline_singleton_count = read_le<std::uint64_t>(
        header_bytes, k_compact_hash32_inline_singleton_count_offset);
    header.rejected_hash_count = read_le<std::uint64_t>(header_bytes, 96u);
    header.rejected_posting_count =
        read_le<std::uint64_t>(header_bytes, 104u);
    header.membership_offset = read_le<std::uint64_t>(header_bytes, 112u);
    header.membership_bytes = read_le<std::uint64_t>(header_bytes, 120u);
    header.rank_offset = read_le<std::uint64_t>(header_bytes, 128u);
    header.rank_bytes = read_le<std::uint64_t>(header_bytes, 136u);
    header.descriptors_offset = read_le<std::uint64_t>(header_bytes, 144u);
    header.descriptors_bytes = read_le<std::uint64_t>(header_bytes, 152u);
    header.postings_offset = read_le<std::uint64_t>(header_bytes, 160u);
    header.postings_bytes = read_le<std::uint64_t>(header_bytes, 168u);
    header.membership_crc64 = read_le<std::uint64_t>(header_bytes, 176u);
    header.rank_crc64 = read_le<std::uint64_t>(header_bytes, 184u);
    header.descriptors_crc64 = read_le<std::uint64_t>(header_bytes, 192u);
    header.postings_crc64 = read_le<std::uint64_t>(header_bytes, 200u);
    header.header_crc64 = read_le<std::uint64_t>(header_bytes, 208u);
    header.image_bytes = read_le<std::uint64_t>(header_bytes, 216u);
    header.sorted_stream_crc64 = read_le<std::uint64_t>(header_bytes, 224u);

    if (header_crc(header_bytes,
            k_compact_hash32_header_crc_offset,
            k_direct_inverted_index_checksum_bytes)
            != header.header_crc64) {
        return std::unexpected(direct_inverted_index_error::header_checksum);
    }
    if (options.expected_direct_header_crc64.has_value()
        && *options.expected_direct_header_crc64 != crc64(header_bytes)) {
        return std::unexpected(direct_inverted_index_error::receipt_mismatch);
    }
    if (header.identity != options.expected_identity
        || header.max_occ_cap != options.expected_max_occ_cap
        || header.geometry != options.expected_geometry) {
        return std::unexpected(direct_inverted_index_error::identity_mismatch);
    }

    const auto geometry = geometry_counts(header.geometry);
    if (!geometry) return std::unexpected(geometry.error());
    const std::uint8_t expected_width =
        header.identity.target_length <= (std::uint64_t{1u} << 31u)
        ? 4u : 8u;
    const bool reserved_header_bytes_zero = direct_index_detail::all_zero(
        header_bytes.subspan(
            k_compact_hash32_inline_singleton_count_offset
                + sizeof(std::uint64_t),
            12u));
    if (header.identity.target_length > (std::uint64_t{1u} << 63u)
        || header.posting_width_bytes != expected_width
        || header.max_occ_cap
            > (std::numeric_limits<std::uint16_t>::max)()
        || !reserved_header_bytes_zero) {
        return std::unexpected(direct_inverted_index_error::bad_header);
    }

    const std::uint64_t direct_section_bytes =
        static_cast<std::uint64_t>(direct_section.size());
    if (header.image_bytes != direct_section_bytes
        || header.inline_singleton_count > header.accepted_hash_count
        || header.inline_singleton_count > header.accepted_posting_count
        || (header.posting_width_bytes == 8u
            && header.inline_singleton_count != 0u)
        || header.accepted_posting_count
            > (std::numeric_limits<std::uint32_t>::max)()) {
        return std::unexpected(direct_inverted_index_error::image_size);
    }
    std::uint64_t count_sum = 0u;
    std::uint64_t posting_sum = 0u;
    if (!checked_add(header.accepted_hash_count,
            header.rejected_hash_count, count_sum)
        || count_sum != header.source_hash_count
        || !checked_add(header.accepted_posting_count,
            header.rejected_posting_count, posting_sum)
        || posting_sum != header.source_posting_count) {
        return std::unexpected(direct_inverted_index_error::control);
    }
    header.materialized_hash_count = header.accepted_hash_count
        - header.inline_singleton_count;
    header.materialized_posting_count = header.accepted_posting_count
        - header.inline_singleton_count;

    // Validate every untrusted interval directly before deriving the stricter
    // canonical layout.  No span/subspan is formed for these payload ranges.
    const std::array sections{
        std::pair{header.membership_offset, header.membership_bytes},
        std::pair{header.rank_offset, header.rank_bytes},
        std::pair{header.descriptors_offset, header.descriptors_bytes},
        std::pair{header.postings_offset, header.postings_bytes},
    };
    std::uint64_t previous_end = k_compact_hash32_direct_index_header_bytes;
    for (const auto [offset, bytes] : sections) {
        std::uint64_t end = 0u;
        if (offset % k_direct_inverted_index_section_alignment != 0u
            || offset < previous_end
            || !checked_add(offset, bytes, end)
            || end > header.image_bytes) {
            return std::unexpected(direct_inverted_index_error::section_layout);
        }
        previous_end = end;
    }
    if (previous_end != header.image_bytes) {
        return std::unexpected(direct_inverted_index_error::section_layout);
    }

    const compact_hash32_scan_receipt receipt{
        .source_hash_count = header.source_hash_count,
        .source_posting_count = header.source_posting_count,
        .accepted_hash_count = header.accepted_hash_count,
        .accepted_posting_count = header.accepted_posting_count,
        .inline_singleton_count = header.inline_singleton_count,
        .rejected_hash_count = header.rejected_hash_count,
        .rejected_posting_count = header.rejected_posting_count,
        .sorted_stream_crc64 = header.sorted_stream_crc64,
    };
    const compact_hash32_build_options layout_options{
        .identity = header.identity,
        .max_occ_cap = header.max_occ_cap,
        .geometry = header.geometry,
    };
    const auto layout = layout_from_receipt(layout_options, receipt);
    if (!layout
        || layout->layout_revision != header.layout_revision
        || layout->descriptor_width_bytes
            != header.descriptor_width_bytes
        || layout->inline_singleton_count
            != header.inline_singleton_count
        || layout->materialized_hash_count
            != header.materialized_hash_count
        || layout->materialized_posting_count
            != header.materialized_posting_count
        || layout->membership_offset != header.membership_offset
        || layout->membership_bytes != header.membership_bytes
        || layout->rank_offset != header.rank_offset
        || layout->rank_bytes != header.rank_bytes
        || layout->descriptors_offset != header.descriptors_offset
        || layout->descriptors_bytes != header.descriptors_bytes
        || layout->postings_offset != header.postings_offset
        || layout->postings_bytes != header.postings_bytes
        || layout->image_bytes != header.image_bytes) {
        return std::unexpected(direct_inverted_index_error::section_layout);
    }

    std::shared_ptr<const std::byte> image_owner{
        std::move(mapped_owner), direct_section.data()};
    return compact_hash32_bounded_open_view{
        std::move(image_owner),
        compact_hash32_direct_index_view{
            .image = direct_section,
            .header = header,
        },
        compact_hash32_bounded_open_stats{
            .direct_section_bytes = direct_section_bytes,
            .header_bytes_examined = k_compact_hash32_direct_index_header_bytes,
            .payload_bytes_examined = 0u,
            .expected_direct_header_crc64_checked =
                options.expected_direct_header_crc64.has_value(),
        },
    };
}

[[nodiscard]] inline std::expected<compact_hash32_direct_index_view,
    direct_inverted_index_error>
validate_compact_hash32_direct_index(
        std::span<const std::byte> image,
        const direct_index_identity& expected_identity,
        std::uint32_t expected_max_occ_cap,
        compact_hash32_geometry expected_geometry = {}) {
    using namespace compact_hash32_detail;
    if (image.size() < k_compact_hash32_direct_index_header_bytes) {
        return std::unexpected(direct_inverted_index_error::image_size);
    }
    if (read_le<std::uint64_t>(image, 0u)
            != k_compact_hash32_direct_index_magic) {
        return std::unexpected(direct_inverted_index_error::bad_magic);
    }
    const std::uint32_t layout_revision = read_le<std::uint32_t>(image, 8u);
    if (layout_revision != k_compact_hash32_direct_index_layout_revision) {
        return std::unexpected(direct_inverted_index_error::bad_layout_revision);
    }
    const std::uint16_t descriptor_width_bytes =
        read_le<std::uint16_t>(image, 22u);
    constexpr std::uint16_t expected_descriptor_width =
        k_compact_hash32_direct_index_descriptor_bytes;
    if (read_le<std::uint32_t>(image, 12u)
            != k_compact_hash32_direct_index_header_bytes
        || read_le<std::uint32_t>(image, 16u) != 0u
        || descriptor_width_bytes != expected_descriptor_width
        || read_le<std::uint32_t>(image, 60u)
            != k_compact_hash32_direct_index_rank_group_bits) {
        return std::unexpected(direct_inverted_index_error::bad_header);
    }
    compact_hash32_header header{};
    header.layout_revision = layout_revision;
    header.descriptor_width_bytes = descriptor_width_bytes;
    header.identity = direct_index_identity{
        .generation_id = read_le<std::uint64_t>(image, 24u),
        .source_identity = read_le<std::uint64_t>(image, 32u),
        .storage_contract_checksum = read_le<std::uint64_t>(image, 40u),
        .target_length = read_le<std::uint64_t>(image, 48u),
    };
    header.max_occ_cap = read_le<std::uint32_t>(image, 56u);
    header.posting_width_bytes = std::to_integer<std::uint8_t>(image[20u]);
    header.geometry = compact_hash32_geometry{
        .hash_domain_bits = std::to_integer<std::uint8_t>(image[21u]),
        .max_hash = read_le<std::uint32_t>(image, 232u),
    };
    header.source_hash_count = read_le<std::uint64_t>(image, 64u);
    header.source_posting_count = read_le<std::uint64_t>(image, 72u);
    header.accepted_hash_count = read_le<std::uint64_t>(image, 80u);
    header.accepted_posting_count = read_le<std::uint64_t>(image, 88u);
    header.inline_singleton_count = read_le<std::uint64_t>(
        image, k_compact_hash32_inline_singleton_count_offset);
    header.rejected_hash_count = read_le<std::uint64_t>(image, 96u);
    header.rejected_posting_count = read_le<std::uint64_t>(image, 104u);
    header.membership_offset = read_le<std::uint64_t>(image, 112u);
    header.membership_bytes = read_le<std::uint64_t>(image, 120u);
    header.rank_offset = read_le<std::uint64_t>(image, 128u);
    header.rank_bytes = read_le<std::uint64_t>(image, 136u);
    header.descriptors_offset = read_le<std::uint64_t>(image, 144u);
    header.descriptors_bytes = read_le<std::uint64_t>(image, 152u);
    header.postings_offset = read_le<std::uint64_t>(image, 160u);
    header.postings_bytes = read_le<std::uint64_t>(image, 168u);
    header.membership_crc64 = read_le<std::uint64_t>(image, 176u);
    header.rank_crc64 = read_le<std::uint64_t>(image, 184u);
    header.descriptors_crc64 = read_le<std::uint64_t>(image, 192u);
    header.postings_crc64 = read_le<std::uint64_t>(image, 200u);
    header.header_crc64 = read_le<std::uint64_t>(image, 208u);
    header.image_bytes = read_le<std::uint64_t>(image, 216u);
    header.sorted_stream_crc64 = read_le<std::uint64_t>(image, 224u);

    if (header.identity != expected_identity
        || header.max_occ_cap != expected_max_occ_cap
        || header.geometry.hash_domain_bits
            != expected_geometry.hash_domain_bits
        || header.geometry.max_hash != expected_geometry.max_hash) {
        return std::unexpected(direct_inverted_index_error::identity_mismatch);
    }
    const auto geometry = geometry_counts(header.geometry);
    if (!geometry) return std::unexpected(geometry.error());
    const std::uint8_t expected_width =
        header.identity.target_length <= (std::uint64_t{1u} << 31u)
        ? 4u : 8u;
    const bool reserved_header_bytes_zero = direct_index_detail::all_zero(
        image.subspan(
            k_compact_hash32_inline_singleton_count_offset
                + sizeof(std::uint64_t),
            12u));
    if (header.identity.target_length > (std::uint64_t{1u} << 63u)
        || header.posting_width_bytes != expected_width
        || header.max_occ_cap
            > (std::numeric_limits<std::uint16_t>::max)()
        || !reserved_header_bytes_zero) {
        return std::unexpected(direct_inverted_index_error::bad_header);
    }
    if (header_crc(image.first(k_compact_hash32_direct_index_header_bytes),
            k_compact_hash32_header_crc_offset,
            k_direct_inverted_index_checksum_bytes)
            != header.header_crc64) {
        return std::unexpected(direct_inverted_index_error::header_checksum);
    }
    if (header.image_bytes != image.size()
        || header.inline_singleton_count > header.accepted_hash_count
        || header.inline_singleton_count > header.accepted_posting_count
        || (header.posting_width_bytes == 8u
            && header.inline_singleton_count != 0u)
        || header.accepted_posting_count
            > (std::numeric_limits<std::uint32_t>::max)()) {
        return std::unexpected(direct_inverted_index_error::image_size);
    }
    std::uint64_t count_sum = 0u;
    std::uint64_t posting_sum = 0u;
    if (!checked_add(header.accepted_hash_count,
            header.rejected_hash_count, count_sum)
        || count_sum != header.source_hash_count
        || !checked_add(header.accepted_posting_count,
            header.rejected_posting_count, posting_sum)
        || posting_sum != header.source_posting_count) {
        return std::unexpected(direct_inverted_index_error::control);
    }
    header.materialized_hash_count = header.accepted_hash_count
        - header.inline_singleton_count;
    header.materialized_posting_count = header.accepted_posting_count
        - header.inline_singleton_count;
    const compact_hash32_scan_receipt receipt{
        .source_hash_count = header.source_hash_count,
        .source_posting_count = header.source_posting_count,
        .accepted_hash_count = header.accepted_hash_count,
        .accepted_posting_count = header.accepted_posting_count,
        .inline_singleton_count = header.inline_singleton_count,
        .rejected_hash_count = header.rejected_hash_count,
        .rejected_posting_count = header.rejected_posting_count,
        .sorted_stream_crc64 = header.sorted_stream_crc64,
    };
    const compact_hash32_build_options layout_options{
        .identity = header.identity,
        .max_occ_cap = header.max_occ_cap,
        .geometry = header.geometry,
    };
    const auto layout = layout_from_receipt(layout_options, receipt);
    if (!layout
        || layout->layout_revision != header.layout_revision
        || layout->descriptor_width_bytes
            != header.descriptor_width_bytes
        || layout->inline_singleton_count
            != header.inline_singleton_count
        || layout->materialized_hash_count
            != header.materialized_hash_count
        || layout->materialized_posting_count
            != header.materialized_posting_count
        || layout->membership_offset != header.membership_offset
        || layout->membership_bytes != header.membership_bytes
        || layout->rank_offset != header.rank_offset
        || layout->rank_bytes != header.rank_bytes
        || layout->descriptors_offset != header.descriptors_offset
        || layout->descriptors_bytes != header.descriptors_bytes
        || layout->postings_offset != header.postings_offset
        || layout->postings_bytes != header.postings_bytes
        || layout->image_bytes != header.image_bytes) {
        return std::unexpected(direct_inverted_index_error::section_layout);
    }
    const auto section = [&image](
            std::uint64_t offset, std::uint64_t bytes) {
        return image.subspan(
            static_cast<std::size_t>(offset),
            static_cast<std::size_t>(bytes));
    };
    if (!direct_index_detail::all_zero(image.subspan(
            k_compact_hash32_direct_index_header_bytes,
            static_cast<std::size_t>(header.membership_offset
                - k_compact_hash32_direct_index_header_bytes)))
        || !direct_index_detail::all_zero(image.subspan(
            static_cast<std::size_t>(header.membership_offset
                + header.membership_bytes),
            static_cast<std::size_t>(header.rank_offset
                - header.membership_offset - header.membership_bytes)))
        || !direct_index_detail::all_zero(image.subspan(
            static_cast<std::size_t>(header.rank_offset + header.rank_bytes),
            static_cast<std::size_t>(header.descriptors_offset
                - header.rank_offset - header.rank_bytes)))
        || !direct_index_detail::all_zero(image.subspan(
            static_cast<std::size_t>(header.descriptors_offset
                + header.descriptors_bytes),
            static_cast<std::size_t>(header.postings_offset
                - header.descriptors_offset - header.descriptors_bytes)))) {
        return std::unexpected(direct_inverted_index_error::section_layout);
    }
    if (crc64(section(header.membership_offset, header.membership_bytes))
            != header.membership_crc64
        || crc64(section(header.rank_offset, header.rank_bytes))
            != header.rank_crc64
        || crc64(section(header.descriptors_offset,
            header.descriptors_bytes)) != header.descriptors_crc64
        || crc64(section(header.postings_offset, header.postings_bytes))
            != header.postings_crc64) {
        return std::unexpected(direct_inverted_index_error::section_checksum);
    }

    std::uint64_t accepted_seen = 0u;
    std::uint64_t accepted_postings_seen = 0u;
    std::uint64_t inline_singletons_seen = 0u;
    std::uint64_t materialized_hashes_seen = 0u;
    std::uint64_t materialized_posting_cursor = 0u;
    for (std::uint64_t group = 0u;
         group < geometry->rank_checkpoint_count; ++group) {
        if (accepted_seen
                > (std::numeric_limits<std::uint32_t>::max)()
            || read_le<std::uint32_t>(image,
                static_cast<std::size_t>(header.rank_offset + group * 4u))
                != accepted_seen) {
            return std::unexpected(direct_inverted_index_error::dictionary);
        }
        for (std::uint64_t word_ordinal = 0u;
             word_ordinal < 4u; ++word_ordinal) {
            std::uint64_t word = read_le<std::uint64_t>(image,
                static_cast<std::size_t>(header.membership_offset
                    + group * 32u + word_ordinal * 8u));
            while (word != 0u) {
                const unsigned bit = std::countr_zero(word);
                if (accepted_seen >= header.accepted_hash_count) {
                    return std::unexpected(
                        direct_inverted_index_error::dictionary);
                }
                const std::uint64_t descriptor_offset =
                    header.descriptors_offset
                    + accepted_seen * header.descriptor_width_bytes;
                const compact_hash32_decoded_descriptor descriptor = decode_descriptor(
                    image,
                    descriptor_offset,
                    header.posting_width_bytes);
                if (descriptor.global_count == 0u
                    || descriptor.global_count > header.max_occ_cap
                    || descriptor.strand_zero_count
                        > descriptor.global_count
                    || accepted_postings_seen
                        > header.accepted_posting_count
                    || descriptor.global_count
                        > header.accepted_posting_count
                            - accepted_postings_seen) {
                    return std::unexpected(
                        direct_inverted_index_error::control);
                }
                std::array<std::uint64_t, 2> previous_bp{};
                std::array<bool, 2> have_previous_bp{};
                const auto validate_encoded = [&](std::uint64_t encoded,
                        std::uint64_t ordinal) noexcept {
                    const bool strand_one = (encoded & 1u) != 0u;
                    const bool expected_strand =
                        ordinal >= descriptor.strand_zero_count;
                    const std::uint64_t bp = encoded >> 1u;
                    if (strand_one != expected_strand
                        || bp >= header.identity.target_length
                        || (have_previous_bp[strand_one]
                            && bp < previous_bp[strand_one])) {
                        return false;
                    }
                    previous_bp[strand_one] = bp;
                    have_previous_bp[strand_one] = true;
                    return true;
                };
                if (descriptor.inline_singleton) {
                    if (header.posting_width_bytes != 4u
                        || descriptor.global_count != 1u
                        || !validate_encoded(
                            descriptor.inline_encoded_position, 0u)) {
                        return std::unexpected(
                            direct_inverted_index_error::posting);
                    }
                    ++inline_singletons_seen;
                } else {
                    if (materialized_posting_cursor
                            > header.materialized_posting_count
                        || descriptor.posting_ordinal
                            != materialized_posting_cursor
                        || descriptor.global_count
                            > header.materialized_posting_count
                                - materialized_posting_cursor) {
                        return std::unexpected(
                            direct_inverted_index_error::control);
                    }
                    for (std::uint64_t ordinal = 0u;
                         ordinal < descriptor.global_count; ++ordinal) {
                        const std::uint64_t encoded = decode_posting(
                            image,
                            header.postings_offset
                                + (materialized_posting_cursor + ordinal)
                                    * header.posting_width_bytes,
                            header.posting_width_bytes);
                        if (!validate_encoded(encoded, ordinal)) {
                            return std::unexpected(
                                direct_inverted_index_error::posting);
                        }
                    }
                    materialized_posting_cursor += descriptor.global_count;
                    ++materialized_hashes_seen;
                }
                accepted_postings_seen += descriptor.global_count;
                ++accepted_seen;
                word &= word - 1u;
                static_cast<void>(bit); // bit identifies the authoritative hash32.
            }
        }
    }
    if (accepted_seen != header.accepted_hash_count
        || accepted_postings_seen != header.accepted_posting_count
        || inline_singletons_seen != header.inline_singleton_count
        || materialized_hashes_seen != header.materialized_hash_count
        || materialized_posting_cursor
            != header.materialized_posting_count) {
        return std::unexpected(direct_inverted_index_error::dictionary);
    }
    return compact_hash32_direct_index_view{.image = image, .header = header};
}

[[nodiscard]] inline std::expected<compact_hash32_query_plan,
    direct_inverted_index_error>
query_compact_hash32_direct_index(
        const compact_hash32_direct_index_view& view,
        std::span<const compact_hash32_query_occurrence> queries,
        std::uint32_t runtime_max_occ) {
    using namespace compact_hash32_detail;
    if (runtime_max_occ == 0u
        || runtime_max_occ > view.header.max_occ_cap
        || queries.size()
            > static_cast<std::size_t>(
                (std::numeric_limits<std::uint32_t>::max)())) {
        return std::unexpected(direct_inverted_index_error::storage_contract_mismatch);
    }
    try {
        compact_hash32_query_plan plan{
            .image_header_crc64 = view.header.header_crc64,
            .max_occ_cap = view.header.max_occ_cap,
            .runtime_max_occ = runtime_max_occ,
            .posting_width_bytes = view.header.posting_width_bytes,
        };
        plan.results.resize(queries.size());
        plan.stats.query_occurrence_count = queries.size();
        bool have_previous = false;
        std::uint32_t previous_hash = 0u;
        bool previous_present = false;
        std::uint64_t previous_dense = 0u;
        compact_hash32_decoded_descriptor previous_descriptor{};
        std::uint64_t cached_rank_group =
            (std::numeric_limits<std::uint64_t>::max)();
        std::uint64_t cached_rank_base = 0u;
        std::uint32_t previous_selection = k_direct_inverted_index_no_selection;
        for (std::size_t ordinal = 0u; ordinal < queries.size(); ++ordinal) {
            const std::uint32_t hash = queries[ordinal].hash;
            if (have_previous && hash < previous_hash) {
                return std::unexpected(direct_inverted_index_error::order);
            }
            bool present = previous_present;
            std::uint64_t dense = previous_dense;
            compact_hash32_decoded_descriptor descriptor = previous_descriptor;
            if (!have_previous || hash != previous_hash) {
                ++plan.stats.distinct_query_hash_count;
                const auto address = resolve_compact_hash32_address(
                    hash, view.header.geometry);
                if (!address) return std::unexpected(address.error());
                if (cached_rank_group
                        != address->rank_checkpoint_ordinal) {
                    cached_rank_group = address->rank_checkpoint_ordinal;
                    cached_rank_base = read_le<std::uint32_t>(
                        view.image,
                        static_cast<std::size_t>(view.header.rank_offset
                            + cached_rank_group * 4u));
                    ++plan.stats.rank_checkpoints_examined;
                }
                const std::uint64_t word_in_group =
                    address->bit_within_rank_group / 64u;
                const unsigned bit_in_word =
                    address->bit_within_rank_group % 64u;
                std::uint64_t before = 0u;
                std::uint64_t selected_word = 0u;
                for (std::uint64_t word = 0u;
                     word <= word_in_group; ++word) {
                    const std::uint64_t value = read_le<std::uint64_t>(
                        view.image,
                        static_cast<std::size_t>(
                            view.header.membership_offset
                            + cached_rank_group * 32u + word * 8u));
                    ++plan.stats.membership_words_examined;
                    if (word < word_in_group) {
                        before += std::popcount(value);
                    } else {
                        selected_word = value;
                        const std::uint64_t lower_mask = bit_in_word == 0u
                            ? 0u
                            : (std::uint64_t{1u} << bit_in_word) - 1u;
                        before += std::popcount(value & lower_mask);
                    }
                }
                present = (selected_word
                    & (std::uint64_t{1u} << bit_in_word)) != 0u;
                dense = cached_rank_base + before;
                descriptor = {};
                if (present) {
                    if (dense >= view.header.accepted_hash_count) {
                        return std::unexpected(
                            direct_inverted_index_error::dictionary);
                    }
                    descriptor = decode_descriptor(
                        view.image,
                        view.header.descriptors_offset
                            + dense
                                * view.header.descriptor_width_bytes,
                        view.header.posting_width_bytes);
                    ++plan.stats.descriptors_examined;
                    if (descriptor.global_count > runtime_max_occ) {
                        ++plan.stats.runtime_rejected_hash_count;
                        if (!checked_add(
                                plan.stats.runtime_rejected_posting_count,
                                descriptor.global_count,
                                plan.stats.runtime_rejected_posting_count)) {
                            return std::unexpected(
                                direct_inverted_index_error::arithmetic_overflow);
                        }
                    }
                }
                previous_present = present;
                previous_dense = dense;
                previous_descriptor = descriptor;
                previous_selection = k_direct_inverted_index_no_selection;
            }
            compact_hash32_query_result& result = plan.results[ordinal];
            if (present && descriptor.global_count <= runtime_max_occ) {
                result.accepted = true;
                result.dense_ordinal = dense;
                result.global_count = descriptor.global_count;
                result.forward_count = queries[ordinal].query_strand_one
                    ? descriptor.global_count
                        - descriptor.strand_zero_count
                    : descriptor.strand_zero_count;
                result.reverse_count =
                    descriptor.global_count - result.forward_count;
                if (previous_selection == k_direct_inverted_index_no_selection) {
                    if (plan.selections.size()
                        >= k_direct_inverted_index_no_selection) {
                        return std::unexpected(
                            direct_inverted_index_error::query_size);
                    }
                    const std::uint32_t selection =
                        static_cast<std::uint32_t>(plan.selections.size());
                    const std::uint64_t first_span = plan.spans.size();
                    const std::uint64_t posting_byte_offset =
                        descriptor.inline_singleton
                        ? view.header.descriptors_offset
                            + dense
                                * view.header.descriptor_width_bytes
                        : view.header.postings_offset
                            + static_cast<std::uint64_t>(
                                descriptor.posting_ordinal)
                                * view.header.posting_width_bytes;
                    if (descriptor.strand_zero_count != 0u) {
                        plan.spans.push_back(direct_index_posting_span{
                            .byte_offset = posting_byte_offset,
                            .count = descriptor.strand_zero_count,
                            .selection_ordinal = selection,
                            .target_strand_one = false,
                        });
                    }
                    const std::uint64_t strand_one =
                        descriptor.global_count
                        - descriptor.strand_zero_count;
                    if (strand_one != 0u) {
                        plan.spans.push_back(direct_index_posting_span{
                            .byte_offset = posting_byte_offset
                                + static_cast<std::uint64_t>(
                                    descriptor.strand_zero_count)
                                    * view.header.posting_width_bytes,
                            .count = strand_one,
                            .selection_ordinal = selection,
                            .target_strand_one = true,
                        });
                    }
                    plan.selections.push_back(compact_hash32_selection{
                        .hash = hash,
                        .dense_ordinal = dense,
                        .global_count = descriptor.global_count,
                        .target_strand_zero_count =
                            descriptor.strand_zero_count,
                        .first_span = first_span,
                        .span_count = plan.spans.size() - first_span,
                    });
                    previous_selection = selection;
                    ++plan.stats.unique_selected_hash_count;
                    if (descriptor.inline_singleton) {
                        ++plan.stats.selected_inline_singleton_count;
                    } else {
                        ++plan.stats.selected_materialized_hash_count;
                        if (!checked_add(
                                plan.stats
                                    .selected_materialized_posting_count,
                                descriptor.global_count,
                                plan.stats
                                    .selected_materialized_posting_count)) {
                            return std::unexpected(
                                direct_inverted_index_error::arithmetic_overflow);
                        }
                    }
                    if (!checked_add(plan.stats.selected_posting_count,
                            descriptor.global_count,
                            plan.stats.selected_posting_count)) {
                        return std::unexpected(
                            direct_inverted_index_error::arithmetic_overflow);
                    }
                }
                result.selection_ordinal = previous_selection;
            }
            previous_hash = hash;
            have_previous = true;
        }
        return plan;
    } catch (const std::bad_alloc&) {
        return std::unexpected(direct_inverted_index_error::allocation);
    } catch (const std::length_error&) {
        return std::unexpected(direct_inverted_index_error::allocation);
    }
}

/// Projected admission-safe lookup for already-deduplicated monotonic query
/// hashes. `hash_at` is deliberately constrained to a noexcept exact-u32
/// projection so callers can expose hashes already embedded in a wider query
/// plan without allocating and filling a duplicate contiguous hash owner.
/// Construction reserves U mappings exactly once, fills without growth, and
/// then replaces that owner with an exact accepted-only c-element owner. Thus
/// peak logical capacity is U+c (at most 2U), while the returned plan retains
/// exactly c 8-byte mappings. The visitor re-derives descriptors and strand
/// spans from the image.
template <class p_hash_at>
requires std::is_nothrow_invocable_v<p_hash_at&, std::size_t>
    && std::is_same_v<
        std::invoke_result_t<p_hash_at&, std::size_t>, std::uint32_t>
[[nodiscard]] inline std::expected<compact_hash32_unique_query_plan,
    direct_inverted_index_error>
query_compact_hash32_direct_index_unique_projected(
        const compact_hash32_direct_index_view& view,
        std::size_t strictly_increasing_unique_hash_count,
        p_hash_at hash_at,
        std::uint32_t runtime_max_occ) {
    using namespace compact_hash32_detail;
    if (runtime_max_occ == 0u
        || runtime_max_occ > view.header.max_occ_cap
        || strictly_increasing_unique_hash_count
            >= static_cast<std::size_t>(
                k_direct_inverted_index_no_selection)) {
        return std::unexpected(direct_inverted_index_error::storage_contract_mismatch);
    }
    try {
        compact_hash32_unique_query_plan plan{};
        plan.image_header_crc64 = view.header.header_crc64;
        plan.max_occ_cap = view.header.max_occ_cap;
        plan.runtime_max_occ = runtime_max_occ;
        plan.posting_width_bytes = view.header.posting_width_bytes;
        plan.stats.query_occurrence_count =
            strictly_increasing_unique_hash_count;
        const std::size_t reserve_request =
            strictly_increasing_unique_hash_count;
        if (reserve_request > plan.selections.max_size()) {
            return std::unexpected(direct_inverted_index_error::query_size);
        }
        plan.stats.selection_reserve_call_count = 1u;
        plan.stats.selection_reserve_requested_elements = reserve_request;
        plan.selections.reserve(reserve_request);
        if (plan.selections.capacity() != reserve_request) {
            return std::unexpected(direct_inverted_index_error::admission);
        }
        plan.stats.selection_initial_capacity_elements =
            plan.selections.capacity();
        plan.stats.selection_storage_allocation_count =
            plan.selections.capacity() == 0u ? 0u : 1u;
        plan.stats.selection_peak_capacity_bytes =
            plan.retained_selection_capacity_bytes();
        bool have_previous = false;
        std::uint32_t previous_hash = 0u;
        std::uint64_t cached_rank_group =
            (std::numeric_limits<std::uint64_t>::max)();
        std::uint64_t cached_rank_base = 0u;
        std::array<std::uint64_t, 4u> cached_membership_words{};
        std::array<std::uint64_t, 5u>
            cached_membership_prefix_popcounts{};
        for (std::size_t query_ordinal = 0u;
             query_ordinal < strictly_increasing_unique_hash_count;
             ++query_ordinal) {
            const std::uint32_t hash = hash_at(query_ordinal);
            if (have_previous && hash <= previous_hash) {
                return std::unexpected(direct_inverted_index_error::order);
            }
            ++plan.stats.distinct_query_hash_count;
            const auto address = resolve_compact_hash32_address(
                hash, view.header.geometry);
            if (!address) return std::unexpected(address.error());
            if (cached_rank_group
                    != address->rank_checkpoint_ordinal) {
                cached_rank_group = address->rank_checkpoint_ordinal;
                cached_rank_base = read_le<std::uint32_t>(
                    view.image,
                    static_cast<std::size_t>(view.header.rank_offset
                        + cached_rank_group * 4u));
                ++plan.stats.rank_checkpoints_examined;
                cached_membership_prefix_popcounts[0u] = 0u;
                for (std::size_t word = 0u;
                     word < cached_membership_words.size(); ++word) {
                    cached_membership_words[word] =
                        read_le<std::uint64_t>(
                            view.image,
                            static_cast<std::size_t>(
                                view.header.membership_offset
                                + cached_rank_group * 32u
                                + word * 8u));
                    ++plan.stats.membership_words_examined;
                    cached_membership_prefix_popcounts[word + 1u] =
                        cached_membership_prefix_popcounts[word]
                        + std::popcount(cached_membership_words[word]);
                }
            }
            const std::size_t word_in_group =
                static_cast<std::size_t>(
                    address->bit_within_rank_group / 64u);
            const unsigned bit_in_word =
                address->bit_within_rank_group % 64u;
            const std::uint64_t selected_word =
                cached_membership_words[word_in_group];
            const std::uint64_t lower_mask = bit_in_word == 0u
                ? 0u
                : (std::uint64_t{1u} << bit_in_word) - 1u;
            const std::uint64_t before =
                cached_membership_prefix_popcounts[word_in_group]
                + std::popcount(selected_word & lower_mask);
            const bool present = (selected_word
                & (std::uint64_t{1u} << bit_in_word)) != 0u;
            const std::uint64_t dense = cached_rank_base + before;
            if (!present) {
                previous_hash = hash;
                have_previous = true;
                continue;
            }
            if (dense >= view.header.accepted_hash_count) {
                return std::unexpected(
                    direct_inverted_index_error::dictionary);
            }
            const compact_hash32_decoded_descriptor descriptor = decode_descriptor(
                view.image,
                view.header.descriptors_offset
                    + dense * view.header.descriptor_width_bytes,
                view.header.posting_width_bytes);
            ++plan.stats.descriptors_examined;
            if (descriptor.global_count > runtime_max_occ) {
                ++plan.stats.runtime_rejected_hash_count;
                if (!checked_add(
                        plan.stats.runtime_rejected_posting_count,
                        descriptor.global_count,
                        plan.stats.runtime_rejected_posting_count)) {
                    return std::unexpected(
                        direct_inverted_index_error::arithmetic_overflow);
                }
                previous_hash = hash;
                have_previous = true;
                continue;
            }
            if (plan.selections.size()
                    >= k_direct_inverted_index_no_selection) {
                return std::unexpected(
                    direct_inverted_index_error::query_size);
            }
            const compact_hash32_unique_selection* const storage_before_push =
                plan.selections.data();
            const std::size_t capacity_before_push =
                plan.selections.capacity();
            plan.selections.push_back(compact_hash32_unique_selection{
                .dense_ordinal = static_cast<std::uint32_t>(dense),
                .query_ordinal = static_cast<std::uint32_t>(query_ordinal),
            });
            if (plan.selections.data() != storage_before_push
                || plan.selections.capacity() != capacity_before_push) {
                ++plan.stats.selection_fill_reallocation_count;
            }
            ++plan.stats.unique_selected_hash_count;
            if (descriptor.inline_singleton) {
                ++plan.stats.selected_inline_singleton_count;
            } else {
                ++plan.stats.selected_materialized_hash_count;
                if (!checked_add(
                        plan.stats.selected_materialized_posting_count,
                        descriptor.global_count,
                        plan.stats.selected_materialized_posting_count)) {
                    return std::unexpected(
                        direct_inverted_index_error::arithmetic_overflow);
                }
            }
            if (!checked_add(plan.stats.selected_posting_count,
                    descriptor.global_count,
                    plan.stats.selected_posting_count)) {
                return std::unexpected(
                    direct_inverted_index_error::arithmetic_overflow);
            }
            previous_hash = hash;
            have_previous = true;
        }
        if (plan.stats.selection_fill_reallocation_count != 0u) {
            return std::unexpected(direct_inverted_index_error::admission);
        }
        const std::size_t accepted_count = plan.selections.size();
        if (accepted_count < reserve_request) {
            if (accepted_count == 0u) {
                std::vector<compact_hash32_unique_selection> empty;
                plan.selections.swap(empty);
            } else {
                std::vector<compact_hash32_unique_selection> compacted;
                if (accepted_count > compacted.max_size()) {
                    return std::unexpected(
                        direct_inverted_index_error::query_size);
                }
                compacted.reserve(accepted_count);
                if (compacted.capacity() != accepted_count) {
                    return std::unexpected(
                        direct_inverted_index_error::admission);
                }
                ++plan.stats.selection_compaction_allocation_count;
                ++plan.stats.selection_storage_allocation_count;
                const std::uint64_t compacted_capacity_bytes =
                    accepted_count
                            > (std::numeric_limits<std::uint64_t>::max)()
                                / sizeof(compact_hash32_unique_selection)
                        ? (std::numeric_limits<std::uint64_t>::max)()
                        : static_cast<std::uint64_t>(accepted_count)
                            * sizeof(compact_hash32_unique_selection);
                if (!checked_add(
                        plan.stats.selection_peak_capacity_bytes,
                        compacted_capacity_bytes,
                        plan.stats.selection_peak_capacity_bytes)) {
                    return std::unexpected(
                        direct_inverted_index_error::arithmetic_overflow);
                }
                const compact_hash32_unique_selection* const compacted_storage =
                    compacted.data();
                for (const compact_hash32_unique_selection selection
                        : plan.selections) {
                    compacted.push_back(selection);
                }
                if (compacted.size() != accepted_count
                    || compacted.capacity() != accepted_count
                    || compacted.data() != compacted_storage) {
                    return std::unexpected(
                        direct_inverted_index_error::admission);
                }
                plan.selections.swap(compacted);
            }
        }
        if (plan.selections.size() != accepted_count
            || plan.selections.capacity() != accepted_count) {
            return std::unexpected(direct_inverted_index_error::admission);
        }
        plan.stats.selection_retained_capacity_elements =
            plan.selections.capacity();
        plan.stats.selection_retained_capacity_bytes =
            plan.retained_selection_capacity_bytes();
        compact_hash32_unique_query_plan_access::seal(plan);
        return plan;
    } catch (const std::bad_alloc&) {
        return std::unexpected(direct_inverted_index_error::allocation);
    } catch (const std::length_error&) {
        return std::unexpected(direct_inverted_index_error::allocation);
    }
}

/// Contiguous-u32 adapter over the projected query authority. It preserves
/// accepted-plan accounting and authentication.
[[nodiscard]] inline std::expected<compact_hash32_unique_query_plan,
    direct_inverted_index_error>
query_compact_hash32_direct_index_unique(
        const compact_hash32_direct_index_view& view,
        std::span<const std::uint32_t> strictly_increasing_unique_hashes,
        std::uint32_t runtime_max_occ) {
    return query_compact_hash32_direct_index_unique_projected(
        view,
        strictly_increasing_unique_hashes.size(),
        [strictly_increasing_unique_hashes](std::size_t ordinal) noexcept {
            return strictly_increasing_unique_hashes[ordinal];
        },
        runtime_max_occ);
}

[[nodiscard]] inline std::expected<direct_index_posting_visit_stats,
    direct_inverted_index_error>
visit_compact_hash32_direct_index_postings(
        const compact_hash32_direct_index_view& view,
        const compact_hash32_query_plan& plan,
        void* context,
        direct_index_posting_visitor visitor) {
    using namespace compact_hash32_detail;
    if (visitor == nullptr
        || plan.image_header_crc64 != view.header.header_crc64
        || plan.max_occ_cap != view.header.max_occ_cap
        || plan.runtime_max_occ == 0u
        || plan.runtime_max_occ > view.header.max_occ_cap
        || plan.posting_width_bytes != view.header.posting_width_bytes
        || plan.stats.rejected_posting_reads != 0u
        || (view.header.posting_width_bytes != 4u
            && view.header.posting_width_bytes != 8u)) {
        return std::unexpected(direct_inverted_index_error::query_plan);
    }
    std::uint64_t postings_end = 0u;
    std::uint64_t descriptors_end = 0u;
    if (!checked_add(view.header.postings_offset,
            view.header.postings_bytes, postings_end)
        || postings_end > view.header.image_bytes
        || !checked_add(view.header.descriptors_offset,
            view.header.descriptors_bytes, descriptors_end)
        || descriptors_end > view.header.image_bytes
        || plan.stats.unique_selected_hash_count
            != plan.selections.size()) {
        return std::unexpected(direct_inverted_index_error::query_plan);
    }
    direct_index_posting_visit_stats stats{};
    std::uint64_t expected_postings = 0u;
    std::uint64_t expected_first_span = 0u;
    bool have_previous_hash = false;
    std::uint32_t previous_hash = 0u;
    for (std::size_t selection_ordinal = 0u;
         selection_ordinal < plan.selections.size(); ++selection_ordinal) {
        const compact_hash32_selection& selection =
            plan.selections[selection_ordinal];
        const auto address = resolve_compact_hash32_address(
            selection.hash, view.header.geometry);
        if (!address
            || (have_previous_hash && selection.hash <= previous_hash)
            || selection.first_span != expected_first_span
            || selection.first_span > plan.spans.size()
            || selection.span_count
                > plan.spans.size() - selection.first_span) {
            return std::unexpected(direct_inverted_index_error::query_plan);
        }

        std::uint64_t rank_delta = 0u;
        std::uint64_t rank_offset = 0u;
        std::uint64_t membership_group_delta = 0u;
        if (!checked_multiply(address->rank_checkpoint_ordinal,
                4u, rank_delta)
            || !checked_add(view.header.rank_offset,
                rank_delta, rank_offset)
            || !checked_multiply(address->rank_checkpoint_ordinal,
                32u, membership_group_delta)) {
            return std::unexpected(direct_inverted_index_error::query_plan);
        }
        const std::uint64_t rank_base = read_le<std::uint32_t>(
            view.image, static_cast<std::size_t>(rank_offset));
        const std::uint64_t word_in_group =
            address->bit_within_rank_group / 64u;
        const unsigned bit_in_word =
            address->bit_within_rank_group % 64u;
        std::uint64_t before = 0u;
        std::uint64_t selected_word = 0u;
        for (std::uint64_t word = 0u; word <= word_in_group; ++word) {
            std::uint64_t word_delta = 0u;
            std::uint64_t membership_offset = 0u;
            if (!checked_multiply(word, 8u, word_delta)
                || !checked_add(view.header.membership_offset,
                    membership_group_delta, membership_offset)
                || !checked_add(membership_offset,
                    word_delta, membership_offset)) {
                return std::unexpected(
                    direct_inverted_index_error::query_plan);
            }
            const std::uint64_t value = read_le<std::uint64_t>(
                view.image, static_cast<std::size_t>(membership_offset));
            if (word < word_in_group) {
                before += std::popcount(value);
            } else {
                selected_word = value;
                const std::uint64_t lower_mask = bit_in_word == 0u
                    ? 0u
                    : (std::uint64_t{1u} << bit_in_word) - 1u;
                before += std::popcount(value & lower_mask);
            }
        }
        const std::uint64_t dense_ordinal = rank_base + before;
        if ((selected_word & (std::uint64_t{1u} << bit_in_word)) == 0u
            || dense_ordinal != selection.dense_ordinal
            || dense_ordinal >= view.header.accepted_hash_count) {
            return std::unexpected(direct_inverted_index_error::query_plan);
        }
        std::uint64_t descriptor_delta = 0u;
        std::uint64_t descriptor_offset = 0u;
        if (!checked_multiply(dense_ordinal,
                view.header.descriptor_width_bytes,
                descriptor_delta)
            || !checked_add(view.header.descriptors_offset,
                descriptor_delta, descriptor_offset)
            || descriptor_offset > descriptors_end
            || view.header.descriptor_width_bytes
                > descriptors_end - descriptor_offset) {
            return std::unexpected(direct_inverted_index_error::query_plan);
        }
        const compact_hash32_decoded_descriptor descriptor = decode_descriptor(
            view.image,
            descriptor_offset,
            view.header.posting_width_bytes);
        if (descriptor.strand_zero_count > descriptor.global_count) {
            return std::unexpected(direct_inverted_index_error::query_plan);
        }
        const std::uint64_t strand_one_count =
            descriptor.global_count - descriptor.strand_zero_count;
        const std::uint64_t expected_span_count =
            static_cast<std::uint64_t>(
                descriptor.strand_zero_count != 0u)
            + static_cast<std::uint64_t>(strand_one_count != 0u);
        std::uint64_t posting_delta = 0u;
        std::uint64_t posting_offset = 0u;
        if (descriptor.global_count != selection.global_count
            || descriptor.strand_zero_count
                != selection.target_strand_zero_count
            || descriptor.global_count > plan.runtime_max_occ
            || selection.span_count != expected_span_count
            || (!descriptor.inline_singleton
                && (!checked_multiply(descriptor.posting_ordinal,
                    view.header.posting_width_bytes, posting_delta)
                    || !checked_add(view.header.postings_offset,
                        posting_delta, posting_offset)))) {
            return std::unexpected(direct_inverted_index_error::query_plan);
        }
        if (descriptor.inline_singleton) {
            posting_offset = descriptor_offset;
        }
        std::uint64_t selection_postings = 0u;
        for (std::uint64_t relative = 0u;
             relative < selection.span_count; ++relative) {
            const direct_index_posting_span& span = plan.spans[
                static_cast<std::size_t>(selection.first_span + relative)];
            const bool expects_strand_zero =
                descriptor.strand_zero_count != 0u && relative == 0u;
            const bool expected_strand_one = !expects_strand_zero;
            const std::uint64_t expected_count = expects_strand_zero
                ? descriptor.strand_zero_count : strand_one_count;
            std::uint64_t expected_span_delta = 0u;
            std::uint64_t expected_span_offset = posting_offset;
            if (expected_strand_one
                && !checked_multiply(descriptor.strand_zero_count,
                    view.header.posting_width_bytes,
                    expected_span_delta)) {
                return std::unexpected(
                    direct_inverted_index_error::query_plan);
            }
            if (!checked_add(expected_span_offset,
                    expected_span_delta, expected_span_offset)) {
                return std::unexpected(
                    direct_inverted_index_error::query_plan);
            }
            std::uint64_t span_bytes = 0u;
            std::uint64_t span_end = 0u;
            if (span.selection_ordinal != selection_ordinal
                || span.count == 0u
                || !checked_multiply(span.count,
                    view.header.posting_width_bytes, span_bytes)
                || !checked_add(span.byte_offset, span_bytes, span_end)
                || (descriptor.inline_singleton
                    ? (span.byte_offset != descriptor_offset
                        || span_bytes != 4u
                        || span_end != descriptor_offset + 4u)
                    : (span.byte_offset < view.header.postings_offset
                        || (span.byte_offset
                                - view.header.postings_offset)
                            % view.header.posting_width_bytes != 0u
                        || span_end > postings_end))
                || span.byte_offset != expected_span_offset
                || span.count != expected_count
                || span.target_strand_one != expected_strand_one
                || span.fragment_ordinal
                    != k_direct_inverted_index_no_fragment
                || !checked_add(selection_postings, span.count,
                    selection_postings)) {
                return std::unexpected(direct_inverted_index_error::query_plan);
            }
            for (std::uint64_t ordinal = 0u;
                 ordinal < span.count; ++ordinal) {
                const std::uint64_t encoded = decode_posting(
                    view.image,
                    span.byte_offset
                        + ordinal * view.header.posting_width_bytes,
                    view.header.posting_width_bytes);
                if (((encoded & 1u) != 0u) != span.target_strand_one
                    || (encoded >> 1u)
                        >= view.header.identity.target_length) {
                    return std::unexpected(
                        direct_inverted_index_error::query_plan);
                }
                if (!visitor(context, direct_index_posting{
                        .selection_ordinal =
                            static_cast<std::uint32_t>(selection_ordinal),
                        .encoded_position = encoded,
                        .target_strand_one = span.target_strand_one,
                    })) {
                    return std::unexpected(
                        direct_inverted_index_error::visitor_rejected);
                }
                ++stats.postings_visited;
            }
            ++stats.spans_visited;
        }
        if (selection_postings != selection.global_count
            || !checked_add(expected_postings, selection_postings,
                expected_postings)) {
            return std::unexpected(direct_inverted_index_error::query_plan);
        }
        expected_first_span += selection.span_count;
        previous_hash = selection.hash;
        have_previous_hash = true;
    }
    if (expected_first_span != plan.spans.size()
        || expected_postings != plan.stats.selected_posting_count
        || stats.postings_visited != expected_postings) {
        return std::unexpected(direct_inverted_index_error::query_plan);
    }
    return stats;
}

/// Authenticate and bind an accepted-only plan to its direct-index view once.
/// The returned accessor is a constant-time receipt for subsequent range
/// visits: it snapshots the validated view and accounting while aliasing the
/// immutable selection storage.  Binding is the only operation that scans all
/// selections to recompute the plan authentication seal.
[[nodiscard]] inline std::expected<compact_hash32_selection_span_range_accessor,
    direct_inverted_index_error>
bind_compact_hash32_direct_index_selection_spans(
        const compact_hash32_direct_index_view& view,
        const compact_hash32_unique_query_plan& plan) {
    using namespace compact_hash32_detail;
    if (view.image.size() < k_compact_hash32_direct_index_header_bytes
        || view.header.image_bytes != view.image.size()
        || read_le<std::uint64_t>(view.image, 0u)
            != k_compact_hash32_direct_index_magic
        || read_le<std::uint32_t>(view.image, 8u)
            != view.header.layout_revision
        || read_le<std::uint64_t>(view.image, 208u)
            != view.header.header_crc64
        || read_le<std::uint64_t>(view.image, 216u)
            != view.header.image_bytes
        || header_crc(view.image.first(k_compact_hash32_direct_index_header_bytes),
                k_compact_hash32_header_crc_offset,
                k_direct_inverted_index_checksum_bytes)
            != view.header.header_crc64) {
        return std::unexpected(direct_inverted_index_error::query_plan);
    }
    constexpr std::uint16_t expected_descriptor_width =
        k_compact_hash32_direct_index_descriptor_bytes;
    const std::uint64_t persisted_inline_singletons =
        read_le<std::uint64_t>(view.image,
            k_compact_hash32_inline_singleton_count_offset);
    std::uint64_t expected_descriptor_bytes = 0u;
    std::uint64_t expected_posting_bytes = 0u;
    if (view.header.layout_revision
            != k_compact_hash32_direct_index_layout_revision
        || view.header.descriptor_width_bytes
            != expected_descriptor_width
        || read_le<std::uint16_t>(view.image, 22u)
            != view.header.descriptor_width_bytes
        || std::to_integer<std::uint8_t>(view.image[20u])
            != view.header.posting_width_bytes
        || read_le<std::uint64_t>(view.image, 48u)
            != view.header.identity.target_length
        || read_le<std::uint32_t>(view.image, 56u)
            != view.header.max_occ_cap
        || read_le<std::uint64_t>(view.image, 80u)
            != view.header.accepted_hash_count
        || read_le<std::uint64_t>(view.image, 88u)
            != view.header.accepted_posting_count
        || read_le<std::uint64_t>(view.image, 144u)
            != view.header.descriptors_offset
        || read_le<std::uint64_t>(view.image, 152u)
            != view.header.descriptors_bytes
        || read_le<std::uint64_t>(view.image, 160u)
            != view.header.postings_offset
        || read_le<std::uint64_t>(view.image, 168u)
            != view.header.postings_bytes
        || persisted_inline_singletons
            != view.header.inline_singleton_count
        || (view.header.posting_width_bytes != 4u
            && view.header.posting_width_bytes != 8u)
        || view.header.identity.target_length > (std::uint64_t{1u} << 63u)
        || view.header.posting_width_bytes
            != (view.header.identity.target_length
                    <= (std::uint64_t{1u} << 31u) ? 4u : 8u)
        || view.header.inline_singleton_count
            > view.header.accepted_hash_count
        || view.header.inline_singleton_count
            > view.header.accepted_posting_count
        || view.header.accepted_posting_count
            > (std::numeric_limits<std::uint32_t>::max)()
        || (view.header.posting_width_bytes == 8u
            && view.header.inline_singleton_count != 0u)
        || view.header.materialized_hash_count
            != view.header.accepted_hash_count
                - view.header.inline_singleton_count
        || view.header.materialized_posting_count
            != view.header.accepted_posting_count
                - view.header.inline_singleton_count
        || !checked_multiply(view.header.accepted_hash_count,
            view.header.descriptor_width_bytes,
            expected_descriptor_bytes)
        || expected_descriptor_bytes != view.header.descriptors_bytes
        || !checked_multiply(view.header.materialized_posting_count,
            view.header.posting_width_bytes,
            expected_posting_bytes)
        || expected_posting_bytes != view.header.postings_bytes) {
        return std::unexpected(direct_inverted_index_error::query_plan);
    }
    std::uint64_t initial_capacity_bytes = 0u;
    std::uint64_t expected_peak_capacity_bytes = 0u;
    if (!checked_multiply(
            plan.stats.selection_initial_capacity_elements,
            sizeof(compact_hash32_unique_selection),
            initial_capacity_bytes)) {
        return std::unexpected(direct_inverted_index_error::query_plan);
    }
    const bool compacted_to_nonempty =
        plan.stats.selection_retained_capacity_elements != 0u
        && plan.stats.selection_retained_capacity_elements
            < plan.stats.selection_initial_capacity_elements;
    expected_peak_capacity_bytes = initial_capacity_bytes;
    if (compacted_to_nonempty
        && !checked_add(expected_peak_capacity_bytes,
            plan.stats.selection_retained_capacity_bytes,
            expected_peak_capacity_bytes)) {
        return std::unexpected(direct_inverted_index_error::query_plan);
    }
    const std::uint64_t expected_storage_allocations =
        (plan.stats.selection_initial_capacity_elements == 0u
            ? 0u : 1u)
        + (compacted_to_nonempty ? 1u : 0u);
    if (!compact_hash32_unique_query_plan_access::authenticated(plan)
        || plan.image_header_crc64 != view.header.header_crc64
        || plan.max_occ_cap != view.header.max_occ_cap
        || plan.runtime_max_occ == 0u
        || plan.runtime_max_occ > view.header.max_occ_cap
        || plan.posting_width_bytes != view.header.posting_width_bytes
        || (view.header.posting_width_bytes != 4u
            && view.header.posting_width_bytes != 8u)
        || plan.stats.unique_selected_hash_count
            != plan.selections.size()
        || plan.stats.rejected_posting_reads != 0u
        || plan.stats.selection_reserve_call_count != 1u
        || plan.stats.selection_reserve_requested_elements
            != plan.stats.query_occurrence_count
        || plan.stats.selection_initial_capacity_elements
            != plan.stats.selection_reserve_requested_elements
        || plan.stats.selection_retained_capacity_elements
            > plan.stats.selection_initial_capacity_elements
        || plan.stats.selection_fill_reallocation_count != 0u
        || plan.stats.selection_compaction_allocation_count
            != (compacted_to_nonempty ? 1u : 0u)
        || plan.stats.selection_storage_allocation_count
            != expected_storage_allocations
        || plan.stats.selection_peak_capacity_bytes
            != expected_peak_capacity_bytes
        || plan.stats.selection_retained_capacity_elements
            != plan.selections.size()
        || plan.stats.selection_retained_capacity_elements
            != plan.selections.capacity()
        || plan.stats.selection_retained_capacity_bytes
            != plan.retained_selection_capacity_bytes()
        || plan.selections.size() >= k_direct_inverted_index_no_selection) {
        return std::unexpected(direct_inverted_index_error::query_plan);
    }
    std::uint64_t postings_end = 0u;
    std::uint64_t descriptors_end = 0u;
    if (!checked_add(view.header.postings_offset,
            view.header.postings_bytes, postings_end)
        || postings_end != view.header.image_bytes
        || !checked_add(view.header.descriptors_offset,
            view.header.descriptors_bytes, descriptors_end)
        || view.header.descriptors_offset
            < k_compact_hash32_direct_index_header_bytes
        || descriptors_end > view.header.postings_offset
        || view.header.postings_offset > postings_end) {
        return std::unexpected(direct_inverted_index_error::query_plan);
    }

    return compact_hash32_selection_span_range_accessor{
        view,
        plan,
        compact_hash32_unique_query_plan_access::authentication_receipt(plan),
        descriptors_end,
        postings_end,
    };
}

/// Expose both strand spans for the caller-specified half-open selection range
/// in one typed, inlinable callback per selection.  The accessor performs the
/// whole-plan authentication only at bind time; every range visit rechecks its
/// constant-size binding receipt and validates the selected descriptor/span
/// geometry.  Global selection ordinals are preserved so concatenating
/// adjacent visits is exactly equivalent to one full visit.
template <class p_visitor>
[[nodiscard]] inline std::expected<compact_hash32_selection_span_visit_stats,
    direct_inverted_index_error>
visit_compact_hash32_direct_index_selection_span_range(
        const compact_hash32_selection_span_range_accessor& accessor,
        std::uint64_t selection_begin,
        std::uint64_t selection_end,
        p_visitor&& visitor) {
    using namespace compact_hash32_detail;
    if (accessor.plan_ == nullptr) {
        return std::unexpected(direct_inverted_index_error::query_plan);
    }
    const compact_hash32_unique_query_plan& plan = *accessor.plan_;
    if (compact_hash32_unique_query_plan_access::authentication_receipt(plan)
            != accessor.plan_authentication_receipt_
        || plan.image_header_crc64
            != accessor.plan_image_header_crc64_
        || plan.max_occ_cap != accessor.plan_max_occ_cap_
        || plan.runtime_max_occ != accessor.runtime_max_occ_
        || plan.posting_width_bytes
            != accessor.plan_posting_width_bytes_
        || plan.stats != accessor.plan_stats_
        || plan.selections.data() != accessor.selections_.data()
        || plan.selections.size() != accessor.selections_.size()
        || plan.selections.capacity() != accessor.selection_capacity_
        || selection_begin > selection_end
        || selection_end > accessor.selections_.size()) {
        return std::unexpected(direct_inverted_index_error::query_plan);
    }

    compact_hash32_selection_span_visit_stats stats{};
    std::uint64_t expected_postings = 0u;
    bool have_previous = selection_begin != 0u;
    std::uint32_t previous_dense_ordinal = 0u;
    std::uint32_t previous_query_ordinal = 0u;
    if (have_previous) {
        const compact_hash32_unique_selection& previous =
            accessor.selections_[static_cast<std::size_t>(
                selection_begin - 1u)];
        previous_dense_ordinal = previous.dense_ordinal;
        previous_query_ordinal = previous.query_ordinal;
    }
    for (std::uint64_t selection_ordinal = selection_begin;
         selection_ordinal < selection_end;
         ++selection_ordinal) {
        const compact_hash32_unique_selection& selection =
            accessor.selections_[static_cast<std::size_t>(
                selection_ordinal)];
        if (selection.query_ordinal
                >= accessor.plan_stats_.query_occurrence_count
            || selection.dense_ordinal
                >= accessor.view_.header.accepted_hash_count
            || (have_previous
                && (selection.dense_ordinal <= previous_dense_ordinal
                    || selection.query_ordinal
                        <= previous_query_ordinal))) {
            return std::unexpected(direct_inverted_index_error::query_plan);
        }
        std::uint64_t descriptor_delta = 0u;
        std::uint64_t descriptor_offset = 0u;
        if (!checked_multiply(selection.dense_ordinal,
                accessor.view_.header.descriptor_width_bytes,
                descriptor_delta)
            || !checked_add(accessor.view_.header.descriptors_offset,
                descriptor_delta, descriptor_offset)
            || descriptor_offset > accessor.descriptors_end_
            || accessor.view_.header.descriptor_width_bytes
                > accessor.descriptors_end_ - descriptor_offset) {
            return std::unexpected(direct_inverted_index_error::query_plan);
        }
        const compact_hash32_decoded_descriptor descriptor = decode_descriptor(
            accessor.view_.image,
            descriptor_offset,
            accessor.view_.header.posting_width_bytes);
        ++stats.descriptors_examined;
        if (descriptor.global_count == 0u
            || descriptor.global_count > accessor.runtime_max_occ_
            || descriptor.strand_zero_count > descriptor.global_count) {
            return std::unexpected(direct_inverted_index_error::query_plan);
        }

        compact_hash32_selection_posting_spans selected{
            .selection_ordinal = static_cast<std::uint32_t>(
                selection_ordinal),
            .dense_ordinal = selection.dense_ordinal,
            .query_ordinal = selection.query_ordinal,
            .global_count = descriptor.global_count,
            .strand_zero_count = descriptor.strand_zero_count,
        };
        for (std::uint32_t strand = 0u; strand < 2u; ++strand) {
            const bool strand_one = strand != 0u;
            const std::uint64_t count = strand_one
                ? descriptor.global_count - descriptor.strand_zero_count
                : descriptor.strand_zero_count;
            if (count == 0u) continue;
            const std::uint64_t ordinal_delta = strand_one
                ? descriptor.strand_zero_count : 0u;
            std::uint64_t posting_ordinal = 0u;
            std::uint64_t posting_delta = 0u;
            std::uint64_t posting_offset = 0u;
            std::uint64_t posting_bytes = 0u;
            std::uint64_t span_end = 0u;
            if (descriptor.inline_singleton) {
                posting_offset = descriptor_offset;
                posting_bytes = 4u;
                if (!checked_add(
                        descriptor_offset, 4u, span_end)
                    || count != 1u
                    || descriptor.global_count != 1u
                    || accessor.view_.header.posting_width_bytes != 4u
                    || span_end > accessor.descriptors_end_) {
                    return std::unexpected(
                        direct_inverted_index_error::query_plan);
                }
            } else if (!checked_add(descriptor.posting_ordinal,
                        ordinal_delta, posting_ordinal)
                    || !checked_multiply(posting_ordinal,
                        accessor.view_.header.posting_width_bytes,
                        posting_delta)
                    || !checked_add(accessor.view_.header.postings_offset,
                        posting_delta, posting_offset)
                    || !checked_multiply(count,
                        accessor.view_.header.posting_width_bytes,
                        posting_bytes)
                    || !checked_add(posting_offset,
                        posting_bytes, span_end)
                    || posting_offset
                        < accessor.view_.header.postings_offset
                    || (posting_offset
                            - accessor.view_.header.postings_offset)
                        % accessor.view_.header.posting_width_bytes != 0u
                    || span_end > accessor.postings_end_) {
                return std::unexpected(
                    direct_inverted_index_error::query_plan);
            }
            selected.spans[selected.span_count++] =
                compact_hash32_posting_span_view{
                    .encoded_bytes = accessor.view_.image.subspan(
                        static_cast<std::size_t>(posting_offset),
                        static_cast<std::size_t>(posting_bytes)),
                    .count = count,
                    .target_length =
                        accessor.view_.header.identity.target_length,
                    .posting_width_bytes =
                        accessor.view_.header.posting_width_bytes,
                    .target_strand_one = strand_one,
                };
            ++stats.spans_exposed;
        }
        if (!checked_add(expected_postings,
                descriptor.global_count, expected_postings)) {
            return std::unexpected(
                direct_inverted_index_error::query_plan);
        }
        if (!static_cast<bool>(visitor(selected))) {
            return std::unexpected(
                direct_inverted_index_error::visitor_rejected);
        }
        ++stats.selection_callbacks;
        if (descriptor.inline_singleton) {
            ++stats.inline_selection_callbacks;
            ++stats.inline_postings_exposed;
        } else {
            ++stats.materialized_selection_callbacks;
            if (!checked_add(stats.materialized_postings_exposed,
                    descriptor.global_count,
                    stats.materialized_postings_exposed)) {
                return std::unexpected(
                    direct_inverted_index_error::arithmetic_overflow);
            }
        }
        if (!checked_add(stats.postings_exposed,
                descriptor.global_count, stats.postings_exposed)) {
            return std::unexpected(
                direct_inverted_index_error::arithmetic_overflow);
        }
        previous_dense_ordinal = selection.dense_ordinal;
        previous_query_ordinal = selection.query_ordinal;
        have_previous = true;
    }
    const std::uint64_t range_selection_count =
        selection_end - selection_begin;
    const bool full_range = selection_begin == 0u
        && selection_end == accessor.selections_.size();
    if (stats.postings_exposed != expected_postings
        || stats.selection_callbacks != range_selection_count
        || stats.descriptors_examined != range_selection_count
        || stats.inline_postings_exposed
                + stats.materialized_postings_exposed
            != stats.postings_exposed
        || stats.inline_selection_callbacks
                + stats.materialized_selection_callbacks
            != stats.selection_callbacks
        || stats.rank_checkpoints_examined != 0u
        || stats.membership_words_examined != 0u
        || (full_range
            && (expected_postings
                    != accessor.plan_stats_.selected_posting_count
                || stats.inline_selection_callbacks
                    != accessor.plan_stats_
                        .selected_inline_singleton_count
                || stats.materialized_selection_callbacks
                    != accessor.plan_stats_
                        .selected_materialized_hash_count
                || stats.inline_postings_exposed
                    != accessor.plan_stats_
                        .selected_inline_singleton_count
                || stats.materialized_postings_exposed
                    != accessor.plan_stats_
                        .selected_materialized_posting_count))) {
        return std::unexpected(direct_inverted_index_error::query_plan);
    }
    return stats;
}

/// Whole-plan traversal API. Its statistics remain
/// unchanged; internally it binds once and delegates to the ranged visitor.
template <class p_visitor>
[[nodiscard]] inline std::expected<compact_hash32_selection_span_visit_stats,
    direct_inverted_index_error>
visit_compact_hash32_direct_index_selection_spans(
        const compact_hash32_direct_index_view& view,
        const compact_hash32_unique_query_plan& plan,
        p_visitor&& visitor) {
    const auto accessor = bind_compact_hash32_direct_index_selection_spans(
        view, plan);
    if (!accessor) return std::unexpected(accessor.error());
    return visit_compact_hash32_direct_index_selection_span_range(
        *accessor,
        0u,
        accessor->selection_count(),
        std::forward<p_visitor>(visitor));
}

/// Callback adapter over the bulk span visitor. Plan authentication, dense
/// descriptor binding, and span bounds are shared with the bulk API; only this
/// adapter performs one indirect call per decoded posting.
[[nodiscard]] inline std::expected<direct_index_posting_visit_stats,
    direct_inverted_index_error>
visit_compact_hash32_direct_index_postings(
        const compact_hash32_direct_index_view& view,
        const compact_hash32_unique_query_plan& plan,
        void* context,
        direct_index_posting_visitor visitor) {
    if (visitor == nullptr) {
        return std::unexpected(direct_inverted_index_error::query_plan);
    }
    direct_index_posting_visit_stats stats{};
    std::optional<direct_inverted_index_error> callback_error{};
    const auto visited = visit_compact_hash32_direct_index_selection_spans(
        view,
        plan,
        [&](const compact_hash32_selection_posting_spans& selected) noexcept {
            for (std::uint8_t span_ordinal = 0u;
                 span_ordinal < selected.span_count;
                 ++span_ordinal) {
                const compact_hash32_posting_span_view& span =
                    selected.spans[span_ordinal];
                for (std::uint64_t ordinal = 0u;
                     ordinal < span.count;
                     ++ordinal) {
                    const auto encoded = span.encoded_position_at(ordinal);
                    if (!encoded) {
                        callback_error = encoded.error();
                        return false;
                    }
                    if (!visitor(context, direct_index_posting{
                            .selection_ordinal =
                                selected.selection_ordinal,
                            .encoded_position = *encoded,
                            .target_strand_one =
                                span.target_strand_one,
                        })) {
                        callback_error =
                            direct_inverted_index_error::visitor_rejected;
                        return false;
                    }
                    ++stats.postings_visited;
                }
                ++stats.spans_visited;
            }
            return true;
        });
    if (!visited) {
        return std::unexpected(
            callback_error.value_or(visited.error()));
    }
    if (stats.spans_visited != visited->spans_exposed
        || stats.postings_visited != visited->postings_exposed) {
        return std::unexpected(direct_inverted_index_error::query_plan);
    }
    return stats;
}

} // namespace bioseq::indexing

namespace bioseq::indexing::direct_index_detail {

inline constexpr std::uint32_t k_flag_run_summaries = 1u;

struct direct_index_prepared_occurrence {
    std::uint32_t hash = 0u;
    std::uint32_t fragment = 0u;
    std::uint64_t target_bp = 0u;
    bool strand_one = false;
};

struct direct_index_hash_group {
    std::uint32_t hash = 0u;
    std::uint64_t begin = 0u;
    std::uint64_t count = 0u;
    std::uint64_t strand_zero_count = 0u;
    std::uint32_t summary_count = 0u;
};

struct direct_index_decoded_control {
    std::uint32_t hash = 0u;
    std::uint32_t flags = 0u;
    std::uint64_t global_count = 0u;
    std::uint64_t strand_zero_count = 0u;
    std::uint64_t posting_byte_offset = 0u;
    std::uint64_t summary_byte_offset = 0u;
    std::uint32_t summary_count = 0u;
    std::uint32_t reserved_zero = 0u;
};

struct direct_index_decoded_summary {
    std::uint32_t fragment_ordinal = 0u;
    std::uint32_t reserved_zero = 0u;
    std::uint64_t posting_byte_offset = 0u;
    std::uint64_t strand_zero_count = 0u;
    std::uint64_t total_count = 0u;
};

struct direct_index_prepared_build {
    direct_index_image_plan plan{};
    std::vector<direct_index_prepared_occurrence> occurrences{};
    std::vector<direct_index_hash_group> groups{};
    std::vector<std::uint64_t> group_by_slot{};
};

[[nodiscard]] constexpr std::expected<direct_index_image_plan,
    direct_inverted_index_error>
layout_from_counts(
        std::uint64_t occurrence_count,
        std::uint64_t unique_hash_count,
        std::uint64_t bucket_count,
        std::uint64_t summary_count,
        std::uint8_t posting_width) noexcept {
    direct_index_image_plan plan{
        .posting_width_bytes = posting_width,
        .occurrence_count = occurrence_count,
        .unique_hash_count = unique_hash_count,
        .bucket_count = bucket_count,
        .run_summary_count = summary_count,
    };
    if (!power_of_two(bucket_count) || bucket_count < 8u
        || (posting_width != 4u && posting_width != 8u)) {
        return std::unexpected(direct_inverted_index_error::bucket_count);
    }
    plan.occupancy_bytes = ceil_div(bucket_count, 8u);
    const std::uint64_t rank_count =
        ceil_div(bucket_count, k_direct_inverted_index_rank_group_buckets) + 1u;
    if (!checked_multiply(rank_count, sizeof(std::uint64_t), plan.rank_bytes)
        || !checked_multiply(
            unique_hash_count,
            k_direct_inverted_index_control_bytes,
            plan.controls_bytes)
        || !checked_multiply(
            summary_count,
            k_direct_inverted_index_run_summary_bytes,
            plan.summaries_bytes)
        || !checked_multiply(
            occurrence_count, posting_width, plan.postings_bytes)) {
        return std::unexpected(
            direct_inverted_index_error::arithmetic_overflow);
    }
    std::uint64_t cursor = k_direct_inverted_index_header_bytes;
    if (!align_up(cursor, k_direct_inverted_index_section_alignment,
            plan.occupancy_offset)
        || !checked_add(plan.occupancy_offset, plan.occupancy_bytes, cursor)
        || !align_up(cursor, k_direct_inverted_index_section_alignment,
            plan.rank_offset)
        || !checked_add(plan.rank_offset, plan.rank_bytes, cursor)
        || !align_up(cursor, k_direct_inverted_index_section_alignment,
            plan.controls_offset)
        || !checked_add(plan.controls_offset, plan.controls_bytes, cursor)
        || !align_up(cursor, k_direct_inverted_index_section_alignment,
            plan.summaries_offset)
        || !checked_add(plan.summaries_offset, plan.summaries_bytes, cursor)
        || !align_up(cursor, k_direct_inverted_index_section_alignment,
            plan.postings_offset)
        || !checked_add(plan.postings_offset, plan.postings_bytes,
            plan.image_bytes)) {
        return std::unexpected(
            direct_inverted_index_error::arithmetic_overflow);
    }
    return plan;
}

[[nodiscard]] constexpr std::expected<std::uint64_t,
    direct_inverted_index_error>
choose_bucket_count(
        std::uint64_t unique_hash_count,
        std::uint64_t override_count) noexcept {
    if (override_count != 0u) {
        if (!power_of_two(override_count) || override_count < 8u) {
            return std::unexpected(direct_inverted_index_error::bucket_count);
        }
        if (unique_hash_count > override_count
            - override_count / 8u) {
            return std::unexpected(direct_inverted_index_error::bucket_count);
        }
        return override_count;
    }
    std::uint64_t numerator = 0u;
    if (!checked_multiply(unique_hash_count, 8u, numerator)) {
        return std::unexpected(
            direct_inverted_index_error::arithmetic_overflow);
    }
    const std::uint64_t minimum = std::max<std::uint64_t>(
        8u, ceil_div(numerator, 7u));
    const auto selected = next_power_of_two(minimum);
    if (!selected) {
        return std::unexpected(direct_inverted_index_error::bucket_count);
    }
    return *selected;
}

[[nodiscard]] inline std::expected<direct_index_prepared_build,
    direct_inverted_index_error>
prepare_build(
        std::span<const direct_index_occurrence> input,
        const direct_index_build_options& options) {
    if (options.identity.target_length > (std::uint64_t{1u} << 63u)
        || options.identity.generation_id == 0u
        || options.identity.source_identity == 0u
        || options.identity.storage_contract_checksum == 0u
        || input.size() > options.max_occurrences) {
        return std::unexpected(direct_inverted_index_error::invalid_options);
    }
    std::uint64_t prepared_bytes = 0u;
    std::uint64_t group_upper_bound_bytes = 0u;
    std::uint64_t pre_dictionary_scratch = 0u;
    if (!checked_multiply(
            input.size(), sizeof(direct_index_prepared_occurrence), prepared_bytes)
        || !checked_multiply(
            input.size(), sizeof(direct_index_hash_group), group_upper_bound_bytes)
        || !checked_add(prepared_bytes, group_upper_bound_bytes,
            pre_dictionary_scratch)) {
        return std::unexpected(
            direct_inverted_index_error::arithmetic_overflow);
    }
    if (pre_dictionary_scratch > options.max_build_scratch_bytes) {
        return std::unexpected(direct_inverted_index_error::admission);
    }

    try {
        direct_index_prepared_build prepared;
        prepared.occurrences.reserve(input.size());
        prepared.groups.reserve(input.size());
        for (const direct_index_occurrence& occurrence : input) {
            if (occurrence.target_bp >= options.identity.target_length) {
                return std::unexpected(
                    direct_inverted_index_error::occurrence_range);
            }
            std::uint32_t fragment = 0u;
            if (options.fragment_span_bp != 0u) {
                const std::uint64_t wide_fragment =
                    occurrence.target_bp / options.fragment_span_bp;
                if (wide_fragment
                    >= k_direct_inverted_index_no_fragment) {
                    return std::unexpected(
                        direct_inverted_index_error::occurrence_range);
                }
                fragment = static_cast<std::uint32_t>(wide_fragment);
            }
            prepared.occurrences.push_back(direct_index_prepared_occurrence{
                .hash = occurrence.hash,
                .fragment = fragment,
                .target_bp = occurrence.target_bp,
                .strand_one = occurrence.target_strand_one,
            });
        }
        const bool summaries = options.fragment_span_bp != 0u;
        std::sort(
            prepared.occurrences.begin(), prepared.occurrences.end(),
            [summaries](const direct_index_prepared_occurrence& left,
                        const direct_index_prepared_occurrence& right) noexcept {
                if (left.hash != right.hash) return left.hash < right.hash;
                if (summaries && left.fragment != right.fragment) {
                    return left.fragment < right.fragment;
                }
                if (left.strand_one != right.strand_one) {
                    return left.strand_one < right.strand_one;
                }
                return left.target_bp < right.target_bp;
            });

        std::uint64_t summary_count = 0u;
        for (std::size_t begin = 0u; begin < prepared.occurrences.size();) {
            std::size_t end = begin + 1u;
            while (end < prepared.occurrences.size()
                && prepared.occurrences[end].hash
                    == prepared.occurrences[begin].hash) {
                ++end;
            }
            std::uint64_t strand_zero_count = 0u;
            std::uint32_t group_summaries = 0u;
            std::uint32_t previous_fragment = 0u;
            bool have_fragment = false;
            for (std::size_t ordinal = begin; ordinal < end; ++ordinal) {
                strand_zero_count += !prepared.occurrences[ordinal].strand_one;
                if (summaries
                    && (!have_fragment
                        || previous_fragment
                            != prepared.occurrences[ordinal].fragment)) {
                    if (group_summaries
                        == (std::numeric_limits<std::uint32_t>::max)()) {
                        return std::unexpected(
                            direct_inverted_index_error::arithmetic_overflow);
                    }
                    ++group_summaries;
                    ++summary_count;
                    previous_fragment = prepared.occurrences[ordinal].fragment;
                    have_fragment = true;
                }
            }
            prepared.groups.push_back(direct_index_hash_group{
                .hash = prepared.occurrences[begin].hash,
                .begin = begin,
                .count = end - begin,
                .strand_zero_count = strand_zero_count,
                .summary_count = group_summaries,
            });
            begin = end;
        }
        const std::uint64_t unique_hash_count = prepared.groups.size();
        if (unique_hash_count > options.max_unique_hashes
            || summary_count > options.max_run_summaries) {
            return std::unexpected(direct_inverted_index_error::admission);
        }
        const auto bucket_count = choose_bucket_count(
            unique_hash_count, options.bucket_count_override);
        if (!bucket_count || *bucket_count > options.max_bucket_count
            || *bucket_count
                > static_cast<std::uint64_t>(
                    (std::numeric_limits<std::size_t>::max)())) {
            return std::unexpected(bucket_count
                ? direct_inverted_index_error::admission
                : bucket_count.error());
        }
        const std::uint8_t posting_width =
            options.identity.target_length <= (std::uint64_t{1u} << 31u)
            ? 4u : 8u;
        const auto layout = layout_from_counts(
            input.size(), unique_hash_count, *bucket_count,
            summary_count, posting_width);
        if (!layout) return std::unexpected(layout.error());
        prepared.plan = *layout;
        if (prepared.plan.image_bytes > options.max_image_bytes
            || prepared.plan.image_bytes
                > static_cast<std::uint64_t>(
                    (std::numeric_limits<std::size_t>::max)())) {
            return std::unexpected(direct_inverted_index_error::admission);
        }
        std::uint64_t slots_bytes = 0u;
        std::uint64_t scratch_bytes = 0u;
        if (!checked_multiply(
                *bucket_count, sizeof(std::uint64_t), slots_bytes)
            || !checked_add(prepared_bytes, group_upper_bound_bytes,
                scratch_bytes)
            || !checked_add(scratch_bytes, slots_bytes, scratch_bytes)) {
            return std::unexpected(
                direct_inverted_index_error::arithmetic_overflow);
        }
        prepared.plan.estimated_build_scratch_bytes = scratch_bytes;
        if (scratch_bytes > options.max_build_scratch_bytes) {
            return std::unexpected(direct_inverted_index_error::admission);
        }

        prepared.group_by_slot.assign(
            static_cast<std::size_t>(*bucket_count),
            (std::numeric_limits<std::uint64_t>::max)());
        const std::uint64_t mask = *bucket_count - 1u;
        for (std::uint64_t group_ordinal = 0u;
             group_ordinal < prepared.groups.size(); ++group_ordinal) {
            std::uint64_t slot =
                static_cast<std::uint64_t>(mix_hash32(
                    prepared.groups[static_cast<std::size_t>(group_ordinal)].
                        hash)) & mask;
            std::uint64_t probes = 0u;
            while (prepared.group_by_slot[static_cast<std::size_t>(slot)]
                    != (std::numeric_limits<std::uint64_t>::max)()) {
                slot = (slot + 1u) & mask;
                if (++probes == *bucket_count) {
                    return std::unexpected(
                        direct_inverted_index_error::dictionary);
                }
            }
            prepared.group_by_slot[static_cast<std::size_t>(slot)] =
                group_ordinal;
        }
        return prepared;
    } catch (const std::bad_alloc&) {
        return std::unexpected(direct_inverted_index_error::allocation);
    } catch (const std::length_error&) {
        return std::unexpected(direct_inverted_index_error::allocation);
    }
}

[[nodiscard]] constexpr direct_index_decoded_control decode_control(
        std::span<const std::byte> image,
        std::uint64_t offset) noexcept {
    const std::size_t base = static_cast<std::size_t>(offset);
    return direct_index_decoded_control{
        .hash = read_le<std::uint32_t>(image, base),
        .flags = read_le<std::uint32_t>(image, base + 4u),
        .global_count = read_le<std::uint64_t>(image, base + 8u),
        .strand_zero_count = read_le<std::uint64_t>(image, base + 16u),
        .posting_byte_offset = read_le<std::uint64_t>(image, base + 24u),
        .summary_byte_offset = read_le<std::uint64_t>(image, base + 32u),
        .summary_count = read_le<std::uint32_t>(image, base + 40u),
        .reserved_zero = read_le<std::uint32_t>(image, base + 44u),
    };
}

[[nodiscard]] constexpr direct_index_decoded_summary decode_summary(
        std::span<const std::byte> image,
        std::uint64_t offset) noexcept {
    const std::size_t base = static_cast<std::size_t>(offset);
    return direct_index_decoded_summary{
        .fragment_ordinal = read_le<std::uint32_t>(image, base),
        .reserved_zero = read_le<std::uint32_t>(image, base + 4u),
        .posting_byte_offset = read_le<std::uint64_t>(image, base + 8u),
        .strand_zero_count = read_le<std::uint64_t>(image, base + 16u),
        .total_count = read_le<std::uint64_t>(image, base + 24u),
    };
}

[[nodiscard]] inline std::uint64_t occupied_before_in_group(
        std::span<const std::byte> occupancy,
        std::uint64_t slot) noexcept {
    const std::uint64_t group_begin =
        (slot / k_direct_inverted_index_rank_group_buckets)
        * k_direct_inverted_index_rank_group_buckets;
    std::uint64_t count = 0u;
    for (std::uint64_t candidate = group_begin;
         candidate < slot;) {
        const std::uint64_t byte_ordinal = candidate / 8u;
        const unsigned within = static_cast<unsigned>(candidate % 8u);
        const unsigned take = static_cast<unsigned>(
            std::min<std::uint64_t>(8u - within, slot - candidate));
        const std::uint8_t byte = std::to_integer<std::uint8_t>(
            occupancy[static_cast<std::size_t>(byte_ordinal)]);
        const std::uint16_t mask = static_cast<std::uint16_t>(
            ((std::uint16_t{1u} << take) - 1u) << within);
        count += std::popcount(static_cast<std::uint8_t>(byte & mask));
        candidate += take;
    }
    return count;
}

[[nodiscard]] inline bool occupied(
        std::span<const std::byte> occupancy,
        std::uint64_t slot) noexcept {
    return (std::to_integer<std::uint8_t>(
        occupancy[static_cast<std::size_t>(slot / 8u)])
        & (std::uint8_t{1u} << (slot % 8u))) != 0u;
}

struct direct_index_lookup_receipt {
    std::uint64_t slot = 0u;
    std::uint64_t control_ordinal = 0u;
    direct_index_decoded_control control{};
};

[[nodiscard]] inline std::expected<std::optional<direct_index_lookup_receipt>,
    direct_inverted_index_error>
lookup_control(
        const direct_inverted_index_view& view,
        std::uint32_t hash,
        direct_index_query_stats* stats = nullptr) noexcept {
    const auto& header = view.header;
    const auto occupancy = view.image.subspan(
        static_cast<std::size_t>(header.occupancy_offset),
        static_cast<std::size_t>(header.occupancy_bytes));
    const auto ranks = view.image.subspan(
        static_cast<std::size_t>(header.rank_offset),
        static_cast<std::size_t>(header.rank_bytes));
    const std::uint64_t mask = header.bucket_count - 1u;
    std::uint64_t slot = static_cast<std::uint64_t>(mix_hash32(hash)) & mask;
    for (std::uint64_t probe = 0u; probe < header.bucket_count; ++probe) {
        if (stats != nullptr) ++stats->dictionary_slots_examined;
        if (!occupied(occupancy, slot)) return std::optional<direct_index_lookup_receipt>{};
        const std::uint64_t group =
            slot / k_direct_inverted_index_rank_group_buckets;
        const std::uint64_t rank_base = read_le<std::uint64_t>(
            ranks, static_cast<std::size_t>(group * 8u));
        const std::uint64_t ordinal =
            rank_base + occupied_before_in_group(occupancy, slot);
        std::uint64_t control_delta = 0u;
        std::uint64_t control_offset = 0u;
        if (!checked_multiply(
                ordinal, k_direct_inverted_index_control_bytes, control_delta)
            || !checked_add(
                header.controls_offset, control_delta, control_offset)
            || ordinal >= header.unique_hash_count) {
            return std::unexpected(direct_inverted_index_error::dictionary);
        }
        if (stats != nullptr) ++stats->control_records_examined;
        const direct_index_decoded_control control = decode_control(view.image, control_offset);
        if (control.hash == hash) {
            return std::optional<direct_index_lookup_receipt>{direct_index_lookup_receipt{
                .slot = slot,
                .control_ordinal = ordinal,
                .control = control,
            }};
        }
        slot = (slot + 1u) & mask;
    }
    return std::unexpected(direct_inverted_index_error::dictionary);
}

[[nodiscard]] constexpr std::uint64_t decode_position(
        std::span<const std::byte> image,
        std::uint64_t byte_offset,
        std::uint8_t width) noexcept {
    return width == 4u
        ? static_cast<std::uint64_t>(read_le<std::uint32_t>(
            image, static_cast<std::size_t>(byte_offset)))
        : read_le<std::uint64_t>(
            image, static_cast<std::size_t>(byte_offset));
}

} // namespace bioseq::indexing::direct_index_detail

export namespace bioseq::indexing {

[[nodiscard]] inline std::expected<direct_index_image_plan,
    direct_inverted_index_error>
plan_direct_inverted_index(
        std::span<const direct_index_occurrence> occurrences,
        const direct_index_build_options& options) {
    auto prepared = direct_index_detail::prepare_build(occurrences, options);
    if (!prepared) return std::unexpected(prepared.error());
    return prepared->plan;
}

[[nodiscard]] inline std::expected<std::vector<std::byte>,
    direct_inverted_index_error>
build_direct_inverted_index(
        std::span<const direct_index_occurrence> input,
        const direct_index_build_options& options) {
    using namespace direct_index_detail;
    auto prepared = prepare_build(input, options);
    if (!prepared) return std::unexpected(prepared.error());
    const direct_index_image_plan& plan = prepared->plan;
    try {
        std::vector<std::byte> image(
            static_cast<std::size_t>(plan.image_bytes), std::byte{0});
        std::span<std::byte> bytes{image};
        const bool summaries = options.fragment_span_bp != 0u;

        std::uint64_t dense_ordinal = 0u;
        std::uint64_t posting_cursor = plan.postings_offset;
        std::uint64_t summary_cursor = plan.summaries_offset;
        std::uint64_t occupied_count = 0u;
        const std::uint64_t rank_group_count =
            direct_index_detail::ceil_div(
                plan.bucket_count,
                k_direct_inverted_index_rank_group_buckets);
        for (std::uint64_t slot = 0u; slot < plan.bucket_count; ++slot) {
            if (slot % k_direct_inverted_index_rank_group_buckets == 0u) {
                write_le<std::uint64_t>(
                    bytes,
                    static_cast<std::size_t>(plan.rank_offset
                        + (slot / k_direct_inverted_index_rank_group_buckets) * 8u),
                    occupied_count);
            }
            const std::uint64_t group_ordinal =
                prepared->group_by_slot[static_cast<std::size_t>(slot)];
            if (group_ordinal
                == (std::numeric_limits<std::uint64_t>::max)()) {
                continue;
            }
            bytes[static_cast<std::size_t>(
                plan.occupancy_offset + slot / 8u)] |=
                    static_cast<std::byte>(std::uint8_t{1u} << (slot % 8u));
            ++occupied_count;
            const direct_index_hash_group& group =
                prepared->groups[static_cast<std::size_t>(group_ordinal)];
            const std::uint64_t control_offset = plan.controls_offset
                + dense_ordinal * k_direct_inverted_index_control_bytes;
            write_le<std::uint32_t>(bytes,
                static_cast<std::size_t>(control_offset), group.hash);
            write_le<std::uint32_t>(bytes,
                static_cast<std::size_t>(control_offset + 4u),
                summaries ? k_flag_run_summaries : 0u);
            write_le<std::uint64_t>(bytes,
                static_cast<std::size_t>(control_offset + 8u), group.count);
            write_le<std::uint64_t>(bytes,
                static_cast<std::size_t>(control_offset + 16u),
                group.strand_zero_count);
            write_le<std::uint64_t>(bytes,
                static_cast<std::size_t>(control_offset + 24u),
                posting_cursor);
            write_le<std::uint64_t>(bytes,
                static_cast<std::size_t>(control_offset + 32u),
                summaries ? summary_cursor : 0u);
            write_le<std::uint32_t>(bytes,
                static_cast<std::size_t>(control_offset + 40u),
                group.summary_count);

            const std::size_t group_begin =
                static_cast<std::size_t>(group.begin);
            const std::size_t group_end =
                static_cast<std::size_t>(group.begin + group.count);
            if (summaries) {
                std::uint64_t summary_posting_cursor = posting_cursor;
                std::size_t run_begin = group_begin;
                while (run_begin < group_end) {
                    std::size_t run_end = run_begin + 1u;
                    while (run_end < group_end
                        && prepared->occurrences[run_end].fragment
                            == prepared->occurrences[run_begin].fragment) {
                        ++run_end;
                    }
                    std::uint64_t run_strand_zero = 0u;
                    for (std::size_t ordinal = run_begin;
                         ordinal < run_end; ++ordinal) {
                        run_strand_zero +=
                            !prepared->occurrences[ordinal].strand_one;
                    }
                    write_le<std::uint32_t>(bytes,
                        static_cast<std::size_t>(summary_cursor),
                        prepared->occurrences[run_begin].fragment);
                    write_le<std::uint64_t>(bytes,
                        static_cast<std::size_t>(summary_cursor + 8u),
                        summary_posting_cursor);
                    write_le<std::uint64_t>(bytes,
                        static_cast<std::size_t>(summary_cursor + 16u),
                        run_strand_zero);
                    write_le<std::uint64_t>(bytes,
                        static_cast<std::size_t>(summary_cursor + 24u),
                        run_end - run_begin);
                    std::uint64_t run_posting_bytes = 0u;
                    if (!checked_multiply(
                            run_end - run_begin,
                            plan.posting_width_bytes,
                            run_posting_bytes)
                        || !checked_add(
                            summary_posting_cursor,
                            run_posting_bytes,
                            summary_posting_cursor)) {
                        return std::unexpected(
                            direct_inverted_index_error::arithmetic_overflow);
                    }
                    summary_cursor += k_direct_inverted_index_run_summary_bytes;
                    run_begin = run_end;
                }
                std::uint64_t group_posting_bytes = 0u;
                std::uint64_t group_posting_end = 0u;
                if (!checked_multiply(group.count,
                        plan.posting_width_bytes, group_posting_bytes)
                    || !checked_add(posting_cursor, group_posting_bytes,
                        group_posting_end)
                    || summary_posting_cursor != group_posting_end) {
                    return std::unexpected(
                        direct_inverted_index_error::arithmetic_overflow);
                }
            }
            for (std::size_t ordinal = group_begin;
                 ordinal < group_end; ++ordinal) {
                const direct_index_prepared_occurrence& occurrence =
                    prepared->occurrences[ordinal];
                const std::uint64_t encoded =
                    (occurrence.target_bp << 1u)
                    | static_cast<std::uint64_t>(occurrence.strand_one);
                if (plan.posting_width_bytes == 4u) {
                    write_le<std::uint32_t>(bytes,
                        static_cast<std::size_t>(posting_cursor),
                        static_cast<std::uint32_t>(encoded));
                } else {
                    write_le<std::uint64_t>(bytes,
                        static_cast<std::size_t>(posting_cursor), encoded);
                }
                posting_cursor += plan.posting_width_bytes;
            }
            ++dense_ordinal;
        }
        write_le<std::uint64_t>(
            bytes,
            static_cast<std::size_t>(plan.rank_offset
                + rank_group_count * 8u),
            occupied_count);
        if (dense_ordinal != plan.unique_hash_count
            || occupied_count != plan.unique_hash_count
            || posting_cursor != plan.image_bytes
            || summary_cursor != plan.summaries_offset + plan.summaries_bytes) {
            return std::unexpected(direct_inverted_index_error::control);
        }

        const auto section_crc = [&image](
                std::uint64_t offset, std::uint64_t count) noexcept {
            return direct_index_detail::crc64(
                std::span<const std::byte>{image}.subspan(
                    static_cast<std::size_t>(offset),
                    static_cast<std::size_t>(count)));
        };
        const std::uint64_t occupancy_crc = section_crc(
            plan.occupancy_offset, plan.occupancy_bytes);
        const std::uint64_t rank_crc = section_crc(
            plan.rank_offset, plan.rank_bytes);
        const std::uint64_t controls_crc = section_crc(
            plan.controls_offset, plan.controls_bytes);
        const std::uint64_t summaries_crc = section_crc(
            plan.summaries_offset, plan.summaries_bytes);
        const std::uint64_t postings_crc = section_crc(
            plan.postings_offset, plan.postings_bytes);

        write_le<std::uint64_t>(bytes, 0u, k_direct_inverted_index_magic);
        write_le<std::uint32_t>(bytes, 8u, k_direct_inverted_index_layout_revision);
        write_le<std::uint32_t>(bytes, 12u,
            k_direct_inverted_index_header_bytes);
        write_le<std::uint32_t>(bytes, 16u,
            summaries ? k_flag_run_summaries : 0u);
        bytes[20u] = static_cast<std::byte>(plan.posting_width_bytes);
        write_le<std::uint64_t>(bytes, 24u,
            options.identity.generation_id);
        write_le<std::uint64_t>(bytes, 32u,
            options.identity.source_identity);
        write_le<std::uint64_t>(bytes, 40u,
            options.identity.storage_contract_checksum);
        write_le<std::uint64_t>(bytes, 48u,
            options.identity.target_length);
        write_le<std::uint64_t>(bytes, 56u, plan.occurrence_count);
        write_le<std::uint64_t>(bytes, 64u, plan.unique_hash_count);
        write_le<std::uint64_t>(bytes, 72u, plan.bucket_count);
        write_le<std::uint64_t>(bytes, 80u, plan.occupancy_offset);
        write_le<std::uint64_t>(bytes, 88u, plan.occupancy_bytes);
        write_le<std::uint64_t>(bytes, 96u, plan.rank_offset);
        write_le<std::uint64_t>(bytes, 104u, plan.rank_bytes);
        write_le<std::uint64_t>(bytes, 112u, plan.controls_offset);
        write_le<std::uint64_t>(bytes, 120u, plan.controls_bytes);
        write_le<std::uint64_t>(bytes, 128u, plan.summaries_offset);
        write_le<std::uint64_t>(bytes, 136u, plan.summaries_bytes);
        write_le<std::uint64_t>(bytes, 144u, plan.postings_offset);
        write_le<std::uint64_t>(bytes, 152u, plan.postings_bytes);
        write_le<std::uint64_t>(bytes, 160u, options.fragment_span_bp);
        write_le<std::uint64_t>(bytes, 168u, plan.run_summary_count);
        write_le<std::uint64_t>(bytes,
            k_direct_inverted_index_occupancy_crc_offset, occupancy_crc);
        write_le<std::uint64_t>(bytes,
            k_direct_inverted_index_rank_crc_offset, rank_crc);
        write_le<std::uint64_t>(bytes,
            k_direct_inverted_index_controls_crc_offset, controls_crc);
        write_le<std::uint64_t>(bytes,
            k_direct_inverted_index_summaries_crc_offset, summaries_crc);
        write_le<std::uint64_t>(bytes,
            k_direct_inverted_index_postings_crc_offset, postings_crc);
        write_le<std::uint64_t>(bytes, 224u, plan.image_bytes);
        write_le<std::uint32_t>(bytes, 232u,
            k_direct_inverted_index_rank_group_buckets);
        write_le<std::uint32_t>(bytes, 236u,
            k_direct_inverted_index_control_bytes);
        write_le<std::uint32_t>(bytes, 240u,
            k_direct_inverted_index_run_summary_bytes);
        write_le<std::uint32_t>(bytes, 244u,
            k_direct_inverted_index_hash_mix_id);
        const std::uint64_t header_checksum = header_crc(
            std::span<const std::byte>{image}.first(
                k_direct_inverted_index_header_bytes),
            k_direct_inverted_index_header_crc_offset,
            k_direct_inverted_index_checksum_bytes);
        write_le<std::uint64_t>(bytes,
            k_direct_inverted_index_header_crc_offset, header_checksum);
        return image;
    } catch (const std::bad_alloc&) {
        return std::unexpected(direct_inverted_index_error::allocation);
    } catch (const std::length_error&) {
        return std::unexpected(direct_inverted_index_error::allocation);
    }
}

[[nodiscard]] inline std::expected<direct_inverted_index_view,
    direct_inverted_index_error>
validate_direct_inverted_index(
        std::span<const std::byte> image,
        const direct_index_identity& expected_identity) {
    // This is the exact/offline validator: it authenticates and semantically
    // walks every section, including postings.  The returned view still owns
    // no heap copy.  A production mapped-file open should pair publication-
    // time use of this routine with a constant-time descriptor/inode-bound
    // open and lazy extent CRCs; silently weakening this exact API would make
    // corruption tests pass without proving the persisted image.
    using namespace direct_index_detail;
    if (image.size() < k_direct_inverted_index_header_bytes) {
        return std::unexpected(direct_inverted_index_error::image_size);
    }
    if (read_le<std::uint64_t>(image, 0u) != k_direct_inverted_index_magic) {
        return std::unexpected(direct_inverted_index_error::bad_magic);
    }
    if (read_le<std::uint32_t>(image, 8u)
            != k_direct_inverted_index_layout_revision) {
        return std::unexpected(direct_inverted_index_error::bad_layout_revision);
    }
    if (read_le<std::uint32_t>(image, 12u)
            != k_direct_inverted_index_header_bytes) {
        return std::unexpected(direct_inverted_index_error::bad_header);
    }
    direct_index_header header{
        .flags = read_le<std::uint32_t>(image, 16u),
        .posting_width_bytes = std::to_integer<std::uint8_t>(image[20u]),
        .identity = direct_index_identity{
            .generation_id = read_le<std::uint64_t>(image, 24u),
            .source_identity = read_le<std::uint64_t>(image, 32u),
            .storage_contract_checksum = read_le<std::uint64_t>(image, 40u),
            .target_length = read_le<std::uint64_t>(image, 48u),
        },
        .occurrence_count = read_le<std::uint64_t>(image, 56u),
        .unique_hash_count = read_le<std::uint64_t>(image, 64u),
        .bucket_count = read_le<std::uint64_t>(image, 72u),
        .occupancy_offset = read_le<std::uint64_t>(image, 80u),
        .occupancy_bytes = read_le<std::uint64_t>(image, 88u),
        .rank_offset = read_le<std::uint64_t>(image, 96u),
        .rank_bytes = read_le<std::uint64_t>(image, 104u),
        .controls_offset = read_le<std::uint64_t>(image, 112u),
        .controls_bytes = read_le<std::uint64_t>(image, 120u),
        .summaries_offset = read_le<std::uint64_t>(image, 128u),
        .summaries_bytes = read_le<std::uint64_t>(image, 136u),
        .postings_offset = read_le<std::uint64_t>(image, 144u),
        .postings_bytes = read_le<std::uint64_t>(image, 152u),
        .fragment_span_bp = read_le<std::uint64_t>(image, 160u),
        .run_summary_count = read_le<std::uint64_t>(image, 168u),
        .occupancy_crc64 = read_le<std::uint64_t>(image, 176u),
        .rank_crc64 = read_le<std::uint64_t>(image, 184u),
        .controls_crc64 = read_le<std::uint64_t>(image, 192u),
        .summaries_crc64 = read_le<std::uint64_t>(image, 200u),
        .postings_crc64 = read_le<std::uint64_t>(image, 208u),
        .header_crc64 = read_le<std::uint64_t>(image, 216u),
        .image_bytes = read_le<std::uint64_t>(image, 224u),
    };
    if (header.identity != expected_identity) {
        return std::unexpected(direct_inverted_index_error::identity_mismatch);
    }
    if ((header.flags & ~k_flag_run_summaries) != 0u
        || ((header.flags & k_flag_run_summaries) != 0u)
            != (header.fragment_span_bp != 0u)
        || ((header.flags & k_flag_run_summaries) == 0u
            && header.run_summary_count != 0u)) {
        return std::unexpected(direct_inverted_index_error::bad_flags);
    }
    const std::uint8_t expected_width =
        header.identity.target_length <= (std::uint64_t{1u} << 31u)
        ? 4u : 8u;
    if (header.identity.target_length > (std::uint64_t{1u} << 63u)
        || header.posting_width_bytes != expected_width
        || read_le<std::uint32_t>(image, 232u)
            != k_direct_inverted_index_rank_group_buckets
        || read_le<std::uint32_t>(image, 236u)
            != k_direct_inverted_index_control_bytes
        || read_le<std::uint32_t>(image, 240u)
            != k_direct_inverted_index_run_summary_bytes
        || read_le<std::uint32_t>(image, 244u)
            != k_direct_inverted_index_hash_mix_id) {
        return std::unexpected(direct_inverted_index_error::bad_header);
    }
    if (!all_zero(image.subspan(21u, 3u))
        || !all_zero(image.subspan(248u, 8u))) {
        return std::unexpected(direct_inverted_index_error::reserved_nonzero);
    }
    const std::uint64_t computed_header_crc = header_crc(
        image.first(k_direct_inverted_index_header_bytes),
        k_direct_inverted_index_header_crc_offset,
        k_direct_inverted_index_checksum_bytes);
    if (computed_header_crc != header.header_crc64) {
        return std::unexpected(direct_inverted_index_error::header_checksum);
    }
    if (header.image_bytes != image.size()) {
        return std::unexpected(direct_inverted_index_error::image_size);
    }
    if (header.unique_hash_count
        > header.bucket_count - header.bucket_count / 8u) {
        return std::unexpected(direct_inverted_index_error::bucket_count);
    }
    const auto expected_layout = layout_from_counts(
        header.occurrence_count,
        header.unique_hash_count,
        header.bucket_count,
        header.run_summary_count,
        header.posting_width_bytes);
    if (!expected_layout
        || expected_layout->occupancy_offset != header.occupancy_offset
        || expected_layout->occupancy_bytes != header.occupancy_bytes
        || expected_layout->rank_offset != header.rank_offset
        || expected_layout->rank_bytes != header.rank_bytes
        || expected_layout->controls_offset != header.controls_offset
        || expected_layout->controls_bytes != header.controls_bytes
        || expected_layout->summaries_offset != header.summaries_offset
        || expected_layout->summaries_bytes != header.summaries_bytes
        || expected_layout->postings_offset != header.postings_offset
        || expected_layout->postings_bytes != header.postings_bytes
        || expected_layout->image_bytes != header.image_bytes) {
        return std::unexpected(direct_inverted_index_error::section_layout);
    }
    const auto exact = [&image](std::uint64_t offset, std::uint64_t count) {
        return image.subspan(
            static_cast<std::size_t>(offset),
            static_cast<std::size_t>(count));
    };
    if (!all_zero(image.subspan(k_direct_inverted_index_header_bytes,
            static_cast<std::size_t>(header.occupancy_offset
                - k_direct_inverted_index_header_bytes)))
        || !all_zero(image.subspan(
            static_cast<std::size_t>(header.occupancy_offset
                + header.occupancy_bytes),
            static_cast<std::size_t>(header.rank_offset
                - header.occupancy_offset - header.occupancy_bytes)))
        || !all_zero(image.subspan(
            static_cast<std::size_t>(header.rank_offset + header.rank_bytes),
            static_cast<std::size_t>(header.controls_offset
                - header.rank_offset - header.rank_bytes)))
        || !all_zero(image.subspan(
            static_cast<std::size_t>(header.controls_offset
                + header.controls_bytes),
            static_cast<std::size_t>(header.summaries_offset
                - header.controls_offset - header.controls_bytes)))
        || !all_zero(image.subspan(
            static_cast<std::size_t>(header.summaries_offset
                + header.summaries_bytes),
            static_cast<std::size_t>(header.postings_offset
                - header.summaries_offset - header.summaries_bytes)))) {
        return std::unexpected(direct_inverted_index_error::section_layout);
    }
    if (crc64(exact(header.occupancy_offset, header.occupancy_bytes))
            != header.occupancy_crc64
        || crc64(exact(header.rank_offset, header.rank_bytes))
            != header.rank_crc64
        || crc64(exact(header.controls_offset, header.controls_bytes))
            != header.controls_crc64
        || crc64(exact(header.summaries_offset, header.summaries_bytes))
            != header.summaries_crc64
        || crc64(exact(header.postings_offset, header.postings_bytes))
            != header.postings_crc64) {
        return std::unexpected(direct_inverted_index_error::section_checksum);
    }

    direct_inverted_index_view view{.image = image, .header = header};
    const auto occupancy = exact(header.occupancy_offset,
        header.occupancy_bytes);
    const auto ranks = exact(header.rank_offset, header.rank_bytes);
    std::uint64_t occupied_count = 0u;
    std::uint64_t posting_cursor = header.postings_offset;
    std::uint64_t summary_cursor = header.summaries_offset;
    std::uint64_t dense_ordinal = 0u;
    const std::uint64_t rank_group_count = ceil_div(
        header.bucket_count, k_direct_inverted_index_rank_group_buckets);
    for (std::uint64_t slot = 0u; slot < header.bucket_count; ++slot) {
        if (slot % k_direct_inverted_index_rank_group_buckets == 0u) {
            if (read_le<std::uint64_t>(ranks,
                    static_cast<std::size_t>(
                        slot / k_direct_inverted_index_rank_group_buckets * 8u))
                != occupied_count) {
                return std::unexpected(direct_inverted_index_error::dictionary);
            }
        }
        if (!occupied(occupancy, slot)) continue;
        ++occupied_count;
        const std::uint64_t control_offset = header.controls_offset
            + dense_ordinal * k_direct_inverted_index_control_bytes;
        const direct_index_decoded_control control = decode_control(image, control_offset);
        if (control.reserved_zero != 0u || control.global_count == 0u
            || control.strand_zero_count > control.global_count
            || control.posting_byte_offset != posting_cursor
            || control.flags
                != ((header.flags & k_flag_run_summaries) != 0u
                    ? k_flag_run_summaries : 0u)) {
            return std::unexpected(direct_inverted_index_error::control);
        }
        const auto found = lookup_control(view, control.hash);
        if (!found || !*found || (*found)->slot != slot) {
            return std::unexpected(direct_inverted_index_error::dictionary);
        }
        std::uint64_t posting_bytes = 0u;
        if (!checked_multiply(control.global_count,
                header.posting_width_bytes, posting_bytes)
            || !checked_add(posting_cursor, posting_bytes, posting_cursor)
            || posting_cursor > header.image_bytes) {
            return std::unexpected(direct_inverted_index_error::posting);
        }
        if ((header.flags & k_flag_run_summaries) != 0u) {
            if (control.summary_count == 0u
                || control.summary_byte_offset != summary_cursor) {
                return std::unexpected(
                    direct_inverted_index_error::run_summary);
            }
            std::uint64_t run_total = 0u;
            std::uint64_t run_strand_zero = 0u;
            std::uint64_t run_posting_cursor = control.posting_byte_offset;
            std::uint32_t previous_fragment = 0u;
            bool have_previous_fragment = false;
            std::uint64_t control_summary_bytes = 0u;
            std::uint64_t control_summary_end = 0u;
            if (!checked_multiply(control.summary_count,
                    k_direct_inverted_index_run_summary_bytes,
                    control_summary_bytes)
                || !checked_add(summary_cursor, control_summary_bytes,
                    control_summary_end)
                || control_summary_end
                    > header.summaries_offset + header.summaries_bytes) {
                return std::unexpected(
                    direct_inverted_index_error::run_summary);
            }
            for (std::uint32_t run = 0u; run < control.summary_count; ++run) {
                const direct_index_decoded_summary summary = decode_summary(
                    image, summary_cursor);
                if (summary.reserved_zero != 0u
                    || summary.total_count == 0u
                    || summary.strand_zero_count > summary.total_count
                    || summary.posting_byte_offset != run_posting_cursor
                    || (have_previous_fragment
                        && summary.fragment_ordinal <= previous_fragment)) {
                    return std::unexpected(
                        direct_inverted_index_error::run_summary);
                }
                std::uint64_t run_bytes = 0u;
                std::uint64_t run_end = 0u;
                if (summary.total_count > control.global_count - run_total
                    || !checked_multiply(summary.total_count,
                        header.posting_width_bytes, run_bytes)
                    || !checked_add(run_posting_cursor, run_bytes, run_end)
                    || run_end > posting_cursor) {
                    return std::unexpected(
                        direct_inverted_index_error::run_summary);
                }
                std::uint64_t previous_bp[2]{0u, 0u};
                bool have_previous_bp[2]{false, false};
                for (std::uint64_t ordinal = 0u;
                     ordinal < summary.total_count; ++ordinal) {
                    const std::uint64_t position_offset =
                        run_posting_cursor
                        + ordinal * header.posting_width_bytes;
                    const std::uint64_t encoded = decode_position(
                        image, position_offset, header.posting_width_bytes);
                    const bool strand_one = (encoded & 1u) != 0u;
                    const std::uint64_t bp = encoded >> 1u;
                    const bool expected_strand =
                        ordinal >= summary.strand_zero_count;
                    if (strand_one != expected_strand
                        || bp >= header.identity.target_length
                        || bp / header.fragment_span_bp
                            != summary.fragment_ordinal
                        || (have_previous_bp[strand_one]
                            && bp < previous_bp[strand_one])) {
                        return std::unexpected(
                            direct_inverted_index_error::posting);
                    }
                    previous_bp[strand_one] = bp;
                    have_previous_bp[strand_one] = true;
                }
                run_posting_cursor = run_end;
                if (!checked_add(run_total, summary.total_count, run_total)
                    || !checked_add(run_strand_zero,
                        summary.strand_zero_count, run_strand_zero)) {
                    return std::unexpected(
                        direct_inverted_index_error::arithmetic_overflow);
                }
                previous_fragment = summary.fragment_ordinal;
                have_previous_fragment = true;
                summary_cursor += k_direct_inverted_index_run_summary_bytes;
            }
            if (run_total != control.global_count
                || run_strand_zero != control.strand_zero_count
                || run_posting_cursor != posting_cursor) {
                return std::unexpected(
                    direct_inverted_index_error::run_summary);
            }
        } else {
            if (control.summary_byte_offset != 0u
                || control.summary_count != 0u) {
                return std::unexpected(direct_inverted_index_error::control);
            }
            std::uint64_t previous_bp[2]{0u, 0u};
            bool have_previous_bp[2]{false, false};
            for (std::uint64_t ordinal = 0u;
                 ordinal < control.global_count; ++ordinal) {
                const std::uint64_t encoded = decode_position(
                    image,
                    control.posting_byte_offset
                        + ordinal * header.posting_width_bytes,
                    header.posting_width_bytes);
                const bool strand_one = (encoded & 1u) != 0u;
                const std::uint64_t bp = encoded >> 1u;
                if (strand_one != (ordinal >= control.strand_zero_count)
                    || bp >= header.identity.target_length
                    || (have_previous_bp[strand_one]
                        && bp < previous_bp[strand_one])) {
                    return std::unexpected(direct_inverted_index_error::posting);
                }
                previous_bp[strand_one] = bp;
                have_previous_bp[strand_one] = true;
            }
        }
        ++dense_ordinal;
    }
    if (read_le<std::uint64_t>(ranks,
            static_cast<std::size_t>(rank_group_count * 8u))
            != occupied_count
        || occupied_count != header.unique_hash_count
        || dense_ordinal != header.unique_hash_count
        || posting_cursor != header.image_bytes
        || summary_cursor
            != header.summaries_offset + header.summaries_bytes) {
        return std::unexpected(direct_inverted_index_error::dictionary);
    }
    return view;
}

[[nodiscard]] inline std::expected<direct_index_query_plan,
    direct_inverted_index_error>
query_direct_inverted_index(
        const direct_inverted_index_view& view,
        std::span<const direct_index_query_occurrence> queries,
        std::uint64_t max_occ =
            (std::numeric_limits<std::uint64_t>::max)()) {
    using namespace direct_index_detail;
    if (queries.size()
        > static_cast<std::size_t>(
            (std::numeric_limits<std::uint32_t>::max)())) {
        return std::unexpected(direct_inverted_index_error::query_size);
    }
    try {
        direct_index_query_plan plan{
            .image_header_crc64 = view.header.header_crc64,
            .posting_width_bytes = view.header.posting_width_bytes,
        };
        plan.results.resize(queries.size());
        plan.stats.query_occurrence_count = queries.size();
        std::unordered_map<std::uint32_t, std::uint32_t> selected_by_hash;
        selected_by_hash.reserve(std::min<std::size_t>(
            queries.size(), static_cast<std::size_t>(
                std::min<std::uint64_t>(
                    view.header.unique_hash_count,
                    (std::numeric_limits<std::size_t>::max)()))));
        for (std::size_t query_ordinal = 0u;
             query_ordinal < queries.size(); ++query_ordinal) {
            const auto found = lookup_control(
                view, queries[query_ordinal].hash, &plan.stats);
            if (!found) return std::unexpected(found.error());
            if (!*found) continue;
            const direct_index_decoded_control& control = (*found)->control;
            direct_index_query_result& result = plan.results[query_ordinal];
            result.present = true;
            result.global_count = control.global_count;
            result.forward_count = queries[query_ordinal].query_strand_one
                ? control.global_count - control.strand_zero_count
                : control.strand_zero_count;
            result.reverse_count =
                control.global_count - result.forward_count;
            if (control.global_count > max_occ) continue;
            result.positions_selected = true;
            const auto existing = selected_by_hash.find(control.hash);
            if (existing != selected_by_hash.end()) {
                result.selection_ordinal = existing->second;
                continue;
            }
            if (plan.selections.size()
                >= k_direct_inverted_index_no_selection) {
                return std::unexpected(direct_inverted_index_error::query_size);
            }
            const std::uint32_t selection_ordinal =
                static_cast<std::uint32_t>(plan.selections.size());
            const std::uint64_t first_span = plan.spans.size();
            if ((control.flags & k_flag_run_summaries) != 0u) {
                std::uint64_t summary_offset = control.summary_byte_offset;
                for (std::uint32_t run = 0u;
                     run < control.summary_count; ++run) {
                    const direct_index_decoded_summary summary = decode_summary(
                        view.image, summary_offset);
                    ++plan.stats.run_summaries_examined;
                    if (summary.strand_zero_count != 0u) {
                        plan.spans.push_back(direct_index_posting_span{
                            .byte_offset = summary.posting_byte_offset,
                            .count = summary.strand_zero_count,
                            .selection_ordinal = selection_ordinal,
                            .fragment_ordinal = summary.fragment_ordinal,
                            .target_strand_one = false,
                        });
                    }
                    const std::uint64_t strand_one_count =
                        summary.total_count - summary.strand_zero_count;
                    if (strand_one_count != 0u) {
                        plan.spans.push_back(direct_index_posting_span{
                            .byte_offset = summary.posting_byte_offset
                                + summary.strand_zero_count
                                    * view.header.posting_width_bytes,
                            .count = strand_one_count,
                            .selection_ordinal = selection_ordinal,
                            .fragment_ordinal = summary.fragment_ordinal,
                            .target_strand_one = true,
                        });
                    }
                    summary_offset += k_direct_inverted_index_run_summary_bytes;
                }
            } else {
                if (control.strand_zero_count != 0u) {
                    plan.spans.push_back(direct_index_posting_span{
                        .byte_offset = control.posting_byte_offset,
                        .count = control.strand_zero_count,
                        .selection_ordinal = selection_ordinal,
                        .target_strand_one = false,
                    });
                }
                const std::uint64_t strand_one_count =
                    control.global_count - control.strand_zero_count;
                if (strand_one_count != 0u) {
                    plan.spans.push_back(direct_index_posting_span{
                        .byte_offset = control.posting_byte_offset
                            + control.strand_zero_count
                                * view.header.posting_width_bytes,
                        .count = strand_one_count,
                        .selection_ordinal = selection_ordinal,
                        .target_strand_one = true,
                    });
                }
            }
            plan.selections.push_back(direct_index_selection{
                .hash = control.hash,
                .global_count = control.global_count,
                .target_strand_zero_count = control.strand_zero_count,
                .first_span = first_span,
                .span_count = plan.spans.size() - first_span,
            });
            selected_by_hash.emplace(control.hash, selection_ordinal);
            result.selection_ordinal = selection_ordinal;
            ++plan.stats.unique_selected_hash_count;
            if (!checked_add(plan.stats.selected_posting_count,
                    control.global_count,
                    plan.stats.selected_posting_count)) {
                return std::unexpected(
                    direct_inverted_index_error::arithmetic_overflow);
            }
        }
        if (!checked_multiply(plan.stats.selected_posting_count,
                view.header.posting_width_bytes,
                plan.stats.selected_posting_bytes)) {
            return std::unexpected(
                direct_inverted_index_error::arithmetic_overflow);
        }
        return plan;
    } catch (const std::bad_alloc&) {
        return std::unexpected(direct_inverted_index_error::allocation);
    } catch (const std::length_error&) {
        return std::unexpected(direct_inverted_index_error::allocation);
    }
}

[[nodiscard]] inline std::expected<direct_index_posting_visit_stats,
    direct_inverted_index_error>
visit_direct_inverted_index_postings(
        const direct_inverted_index_view& view,
        const direct_index_query_plan& plan,
        void* context,
        direct_index_posting_visitor visitor) {
    using namespace direct_index_detail;
    if (visitor == nullptr
        || plan.image_header_crc64 != view.header.header_crc64
        || plan.posting_width_bytes != view.header.posting_width_bytes) {
        return std::unexpected(direct_inverted_index_error::query_plan);
    }
    direct_index_posting_visit_stats stats{};
    std::uint64_t expected_postings = 0u;
    for (std::size_t selection_ordinal = 0u;
         selection_ordinal < plan.selections.size(); ++selection_ordinal) {
        const direct_index_selection& selection =
            plan.selections[selection_ordinal];
        if (selection.first_span > plan.spans.size()
            || selection.span_count
                > plan.spans.size() - selection.first_span) {
            return std::unexpected(direct_inverted_index_error::query_plan);
        }
        std::uint64_t selection_postings = 0u;
        for (std::uint64_t relative = 0u;
             relative < selection.span_count; ++relative) {
            const direct_index_posting_span& span = plan.spans[
                static_cast<std::size_t>(selection.first_span + relative)];
            if (span.selection_ordinal != selection_ordinal
                || span.count == 0u
                || span.byte_offset < view.header.postings_offset
                || (span.byte_offset - view.header.postings_offset)
                    % view.header.posting_width_bytes != 0u) {
                return std::unexpected(direct_inverted_index_error::query_plan);
            }
            std::uint64_t span_bytes = 0u;
            std::uint64_t span_end = 0u;
            if (!checked_multiply(span.count,
                    view.header.posting_width_bytes, span_bytes)
                || !checked_add(span.byte_offset, span_bytes, span_end)
                || span_end > view.header.image_bytes
                || !checked_add(selection_postings, span.count,
                    selection_postings)) {
                return std::unexpected(direct_inverted_index_error::query_plan);
            }
            for (std::uint64_t ordinal = 0u;
                 ordinal < span.count; ++ordinal) {
                const std::uint64_t encoded = decode_position(
                    view.image,
                    span.byte_offset
                        + ordinal * view.header.posting_width_bytes,
                    view.header.posting_width_bytes);
                const bool strand_one = (encoded & 1u) != 0u;
                const std::uint64_t bp = encoded >> 1u;
                if (strand_one != span.target_strand_one
                    || bp >= view.header.identity.target_length
                    || (span.fragment_ordinal
                            != k_direct_inverted_index_no_fragment
                        && (view.header.fragment_span_bp == 0u
                            || bp / view.header.fragment_span_bp
                                != span.fragment_ordinal))) {
                    return std::unexpected(direct_inverted_index_error::query_plan);
                }
                if (!visitor(context, direct_index_posting{
                        .selection_ordinal =
                            static_cast<std::uint32_t>(selection_ordinal),
                        .fragment_ordinal = span.fragment_ordinal,
                        .encoded_position = encoded,
                        .target_strand_one = strand_one,
                    })) {
                    return std::unexpected(
                        direct_inverted_index_error::visitor_rejected);
                }
                ++stats.postings_visited;
            }
            ++stats.spans_visited;
        }
        if (selection_postings != selection.global_count
            || !checked_add(expected_postings, selection_postings,
                expected_postings)) {
            return std::unexpected(direct_inverted_index_error::query_plan);
        }
    }
    if (expected_postings != plan.stats.selected_posting_count
        || stats.postings_visited != expected_postings) {
        return std::unexpected(direct_inverted_index_error::query_plan);
    }
    return stats;
}

} // namespace bioseq::indexing
