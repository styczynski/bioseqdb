/**
 * @file wire_format.cppm
 * @brief Defines direct hash32 wire contracts and fixed-width codecs.
 *
 * Persisted scalars use explicit little-endian encoding. CRC fields, offsets,
 * revisions, and layout equations remain part of the authenticated image.
 */
export module bioseq.indexing.compact_hash32_direct_index:wire_format;

import std;
import bioseq.common.checked_arithmetic;
import bioseq.indexing.storage;

namespace bioseq::indexing::direct_index_detail {

template <class p_unsigned_integer>
    requires std::is_unsigned_v<p_unsigned_integer>
constexpr void
write_le(std::span<std::byte> output, std::size_t offset, p_unsigned_integer value) noexcept {
    if consteval {
        for (std::size_t byte = 0u; byte < sizeof(p_unsigned_integer); ++byte) {
            output[offset + byte] = static_cast<std::byte>(value & p_unsigned_integer{0xffu});
            value >>= 8u;
        }
    } else {
        if constexpr (std::endian::native == std::endian::big) {
            value = std::byteswap(value);
        }
        std::memcpy(output.data() + offset, &value, sizeof(p_unsigned_integer));
    }
}

template <class p_unsigned_integer>
    requires std::is_unsigned_v<p_unsigned_integer>
[[nodiscard]] constexpr p_unsigned_integer read_le(std::span<const std::byte> input,
                                                   std::size_t offset) noexcept {
    if consteval {
        p_unsigned_integer value = 0u;
        for (std::size_t byte = 0u; byte < sizeof(p_unsigned_integer); ++byte) {
            value |=
                static_cast<p_unsigned_integer>(std::to_integer<std::uint8_t>(input[offset + byte]))
                << (byte * 8u);
        }
        return value;
    } else {
        p_unsigned_integer value = 0u;
        std::memcpy(&value, input.data() + offset, sizeof(p_unsigned_integer));
        if constexpr (std::endian::native == std::endian::big) {
            value = std::byteswap(value);
        }
        return value;
    }
}

[[nodiscard]] constexpr bool
checked_add(std::uint64_t left, std::uint64_t right, std::uint64_t& result) noexcept {
    const auto sum = bioseq::common::checked_add(left, right);
    if (!sum.has_value()) {
        return false;
    }
    result = *sum;
    return true;
}

[[nodiscard]] constexpr bool
checked_multiply(std::uint64_t left, std::uint64_t right, std::uint64_t& result) noexcept {
    const auto product = bioseq::common::checked_mul(left, right);
    if (!product.has_value()) {
        return false;
    }
    result = *product;
    return true;
}

[[nodiscard]] constexpr bool
align_up(std::uint64_t value, std::uint64_t alignment, std::uint64_t& result) noexcept {
    const auto aligned = bioseq::common::checked_align_up(value, alignment);
    if (!aligned.has_value()) {
        return false;
    }
    result = *aligned;
    return true;
}

[[nodiscard]] constexpr std::uint64_t ceil_div(std::uint64_t value,
                                               std::uint64_t divisor) noexcept {
    return bioseq::common::ceil_div(value, divisor);
}

[[nodiscard]] constexpr bool power_of_two(std::uint64_t value) noexcept {
    return value != 0u && (value & (value - 1u)) == 0u;
}

[[nodiscard]] constexpr std::optional<std::uint64_t>
next_power_of_two(std::uint64_t value) noexcept {
    if (value <= 1u)
        return 1u;
    if (value > (std::uint64_t{1u} << 63u))
        return std::nullopt;
    return std::uint64_t{1u} << (64u - std::countl_zero(value - 1u));
}

[[nodiscard]] constexpr std::uint32_t mix_hash32(std::uint32_t value) noexcept {
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}

using crc64_state = bioseq::indexing::crc64_ecma182_accumulator;

[[nodiscard]] inline std::uint64_t crc64(std::span<const std::byte> bytes) noexcept {
    return bioseq::indexing::crc64_ecma182(bytes);
}

[[nodiscard]] inline std::uint64_t header_crc(std::span<const std::byte> header,
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
        if (byte != std::byte{0})
            return false;
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

    [[nodiscard]] constexpr bool
    operator==(const direct_index_occurrence&) const noexcept = default;
};

struct direct_index_identity {
    std::uint64_t generation_id = 0u;
    std::uint64_t source_identity = 0u;
    std::uint64_t storage_contract_checksum = 0u;
    std::uint64_t target_length = 0u;

    [[nodiscard]] constexpr bool operator==(const direct_index_identity&) const noexcept = default;
};

struct direct_index_build_options {
    direct_index_identity identity{};
    /// Zero disables summaries.  Otherwise one summary is emitted for every
    /// nonempty (hash, target-fragment) pair.
    std::uint64_t fragment_span_bp = 0u;
    /// Tests may pin a small power-of-two table.  Zero selects the production
    /// full-u32 dictionary at a maximum 7/8 load factor.
    std::uint64_t bucket_count_override = 0u;
    std::uint64_t max_occurrences = (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t max_unique_hashes = (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t max_bucket_count = (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t max_run_summaries = (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t max_image_bytes = (std::numeric_limits<std::uint64_t>::max)();
    /// Excludes the caller-owned input and final returned image.
    std::uint64_t max_build_scratch_bytes = (std::numeric_limits<std::uint64_t>::max)();
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
     * N*sizeof(direct_index_prepared_occurrence)
     * +N*sizeof(direct_index_hash_group)+B*8 in this implementation.
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

    [[nodiscard]] constexpr bool
    operator==(const direct_index_image_plan&) const noexcept = default;
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

using direct_index_posting_visitor = bool (*)(void*, const direct_index_posting&);

struct direct_index_posting_visit_stats {
    std::uint64_t spans_visited = 0u;
    std::uint64_t postings_visited = 0u;
};

[[nodiscard]] inline std::uint64_t
direct_inverted_index_crc64(std::span<const std::byte> bytes) noexcept {
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

inline constexpr std::uint64_t k_compact_hash32_direct_index_magic = 0x3143443233485342ULL;
/// Persisted compact-hash image revision. Current images retain byte value 2.
inline constexpr std::uint32_t k_compact_hash32_direct_index_layout_revision = 2u;
inline constexpr std::uint32_t k_compact_hash32_direct_index_header_bytes = 256u;
/// `{u32 posting_or_inline,u16 count,u16 strand0}`. A P32 singleton is
/// encoded in the first word. Every other run names its first posting ordinal.
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

    [[nodiscard]] constexpr bool operator==(const compact_hash32_address&) const noexcept = default;
};

/// The canonical K=15 profile binds d=30/max=2^30-1. Full-width hash32
/// binds d=32/max=UINT32_MAX.  Values [9,29] and 31 exist solely so unit tests
/// can exercise the same codec/rank algorithms without allocating either
/// production prefix.  Every image authenticates both d and max_hash.
struct compact_hash32_geometry {
    std::uint8_t hash_domain_bits = k_compact_hash32_canonical_k15_hash_bits;
    std::uint32_t max_hash = (std::uint32_t{1u} << k_compact_hash32_canonical_k15_hash_bits) - 1u;

    [[nodiscard]] constexpr bool canonical_k15() const noexcept {
        return hash_domain_bits == k_compact_hash32_canonical_k15_hash_bits &&
               max_hash == 0x3fffffffu;
    }

    [[nodiscard]] constexpr bool full_h32() const noexcept {
        return hash_domain_bits == k_compact_hash32_full_hash_bits &&
               max_hash == (std::numeric_limits<std::uint32_t>::max)();
    }

    [[nodiscard]] constexpr bool
    operator==(const compact_hash32_geometry&) const noexcept = default;
};

[[nodiscard]] constexpr std::expected<compact_hash32_address, direct_inverted_index_error>
resolve_compact_hash32_address(std::uint32_t hash, compact_hash32_geometry geometry = {}) noexcept {
    if (geometry.hash_domain_bits < 9u ||
        geometry.hash_domain_bits > k_compact_hash32_full_hash_bits) {
        return std::unexpected(direct_inverted_index_error::occurrence_range);
    }
    const std::uint32_t expected_max =
        geometry.hash_domain_bits == 32u
            ? (std::numeric_limits<std::uint32_t>::max)()
            : static_cast<std::uint32_t>((std::uint64_t{1u} << geometry.hash_domain_bits) - 1u);
    if (geometry.max_hash != expected_max || hash > geometry.max_hash) {
        return std::unexpected(direct_inverted_index_error::occurrence_range);
    }
    return compact_hash32_address{
        .membership_byte_ordinal = hash / 8u,
        .membership_bit = static_cast<std::uint8_t>(hash % 8u),
        .rank_checkpoint_ordinal = hash / k_compact_hash32_direct_index_rank_group_bits,
        .bit_within_rank_group =
            static_cast<std::uint16_t>(hash % k_compact_hash32_direct_index_rank_group_bits),
    };
}

struct compact_hash32_sorted_run_header {
    std::uint32_t hash = 0u;
    std::uint64_t global_count = 0u;
    std::uint64_t target_strand_zero_count = 0u;
};

using compact_hash32_begin_run = std::expected<void, direct_inverted_index_error> (*)(
    void*, const compact_hash32_sorted_run_header&);
using compact_hash32_consume_positions =
    std::expected<void, direct_inverted_index_error> (*)(void*, std::span<const std::uint64_t>);

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
    consume_positions(std::span<const std::uint64_t> encoded_positions) const {
        if (context == nullptr || consume_positions_fn == nullptr) {
            return std::unexpected(direct_inverted_index_error::source_read);
        }
        return consume_positions_fn(context, encoded_positions);
    }
};

using compact_hash32_scan_sorted_runs = std::expected<void, direct_inverted_index_error> (*)(
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
    std::uint64_t max_image_bytes = (std::numeric_limits<std::uint64_t>::max)();
};

struct compact_hash32_image_plan {
    direct_index_identity identity{};
    std::uint32_t layout_revision = k_compact_hash32_direct_index_layout_revision;
    std::uint16_t descriptor_width_bytes = k_compact_hash32_direct_index_descriptor_bytes;
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

    [[nodiscard]] constexpr bool
    operator==(const compact_hash32_image_plan&) const noexcept = default;
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
/// header. It deliberately does not fault in or checksum any payload page.
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
/// token. A genuinely owning token can retain the enclosing mmap. A
/// transaction-bound or otherwise non-owning token still requires its external
/// mapping lease to outlive this view.  The direct section may begin at any
/// nonzero offset. The token's stored pointer need not equal image.data().
class compact_hash32_bounded_open_view {
  public:
    compact_hash32_bounded_open_view(std::shared_ptr<const std::byte> image_owner,
                                     compact_hash32_direct_index_view view,
                                     compact_hash32_bounded_open_stats stats) noexcept
        : image_owner_{std::move(image_owner)}, view_{view}, stats_{stats} {}

    [[nodiscard]] const compact_hash32_direct_index_view& view() const noexcept {
        return view_;
    }

    [[nodiscard]] const std::shared_ptr<const std::byte>& image_owner() const noexcept {
        return image_owner_;
    }

    [[nodiscard]] const compact_hash32_bounded_open_stats& stats() const noexcept {
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

    [[nodiscard]] constexpr bool
    operator==(const compact_hash32_query_stats&) const noexcept = default;
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
/// the public accounting and mappings to the factory-built plan. A copied
/// plan whose public fields are mutated is rejected before traversal.
struct compact_hash32_unique_query_plan {
    std::uint64_t image_header_crc64 = 0u;
    std::uint32_t max_occ_cap = 0u;
    std::uint32_t runtime_max_occ = 0u;
    std::uint8_t posting_width_bytes = 0u;
    std::vector<compact_hash32_unique_selection> selections{};
    compact_hash32_query_stats stats{};

    [[nodiscard]] std::uint64_t retained_selection_capacity_bytes() const noexcept {
        const std::uint64_t capacity = selections.capacity();
        return capacity > (std::numeric_limits<std::uint64_t>::max)() /
                              sizeof(compact_hash32_unique_selection)
                   ? (std::numeric_limits<std::uint64_t>::max)()
                   : capacity * sizeof(compact_hash32_unique_selection);
    }

  private:
    std::uint64_t authentication_crc64_ = 0u;

    [[nodiscard]] static constexpr std::uint64_t mix_authentication_(std::uint64_t state,
                                                                     std::uint64_t value) noexcept {
        value += 0x9e3779b97f4a7c15ULL;
        value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27u)) * 0x94d049bb133111ebULL;
        value ^= value >> 31u;
        return std::rotl(state ^ value, 27u) * 0x3c79ac492ba7b653ULL + 0x1c69b3f74ac4ae35ULL;
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
            mix((static_cast<std::uint64_t>(selection.dense_ordinal) << 32u) |
                selection.query_ordinal);
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

    [[nodiscard]] std::expected<std::uint64_t, direct_inverted_index_error>
    encoded_position_at(std::uint64_t ordinal) const noexcept {
        if (ordinal >= count || (posting_width_bytes != 4u && posting_width_bytes != 8u)) {
            return std::unexpected(direct_inverted_index_error::query_plan);
        }
        std::uint64_t byte_offset = 0u;
        if (!direct_index_detail::checked_multiply(ordinal, posting_width_bytes, byte_offset) ||
            byte_offset > encoded_bytes.size() ||
            posting_width_bytes > encoded_bytes.size() - byte_offset) {
            return std::unexpected(direct_inverted_index_error::query_plan);
        }
        const std::uint64_t encoded =
            posting_width_bytes == 4u
                ? static_cast<std::uint64_t>(direct_index_detail::read_le<std::uint32_t>(
                      encoded_bytes, static_cast<std::size_t>(byte_offset)))
                : direct_index_detail::read_le<std::uint64_t>(
                      encoded_bytes, static_cast<std::size_t>(byte_offset));
        if (((encoded & 1u) != 0u) != target_strand_one || (encoded >> 1u) >= target_length) {
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
    [[nodiscard,
      gnu::always_inline]] inline std::expected<std::uint64_t, direct_inverted_index_error>
    visit_encoded_positions(p_visitor&& visitor) const {
        std::uint64_t expected_bytes = 0u;
        if ((posting_width_bytes != 4u && posting_width_bytes != 8u) ||
            !direct_index_detail::checked_multiply(count, posting_width_bytes, expected_bytes) ||
            expected_bytes != encoded_bytes.size() ||
            (expected_bytes != 0u && encoded_bytes.data() == nullptr)) {
            return std::unexpected(direct_inverted_index_error::query_plan);
        }
        if (expected_bytes == 0u)
            return std::uint64_t{0u};
        if (posting_width_bytes == 4u) {
            return visit_encoded_positions_width_<std::uint32_t>(std::forward<p_visitor>(visitor));
        }
        return visit_encoded_positions_width_<std::uint64_t>(std::forward<p_visitor>(visitor));
    }

  private:
    template <class p_encoded_integer, class p_visitor>
    [[nodiscard,
      gnu::always_inline]] inline std::expected<std::uint64_t, direct_inverted_index_error>
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
            if (((encoded & 1u) != 0u) != target_strand_one || (encoded >> 1u) >= target_length) {
                return std::unexpected(direct_inverted_index_error::query_plan);
            }
            if (!static_cast<bool>(visitor(encoded))) {
                return std::unexpected(direct_inverted_index_error::visitor_rejected);
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
/// once. Range visits thereafter validate only constant-size binding state and
/// the descriptors in their requested half-open range.  The bound plan,
/// selection storage, and image mapping must remain alive and unmodified for
/// the accessor lifetime.  Const range visits have no shared mutable state and
/// may therefore run concurrently with independent visitors.
class compact_hash32_selection_span_range_accessor {
  public:
    compact_hash32_selection_span_range_accessor(
        const compact_hash32_selection_span_range_accessor&) noexcept = default;
    compact_hash32_selection_span_range_accessor&
    operator=(const compact_hash32_selection_span_range_accessor&) noexcept = default;
    compact_hash32_selection_span_range_accessor(
        compact_hash32_selection_span_range_accessor&&) noexcept = default;
    compact_hash32_selection_span_range_accessor&
    operator=(compact_hash32_selection_span_range_accessor&&) noexcept = default;
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

    compact_hash32_selection_span_range_accessor(const compact_hash32_direct_index_view& view,
                                                 const compact_hash32_unique_query_plan& plan,
                                                 std::uint64_t plan_authentication_receipt,
                                                 std::uint64_t descriptors_end,
                                                 std::uint64_t postings_end) noexcept
        : view_{view}, plan_{std::addressof(plan)},
          selections_{plan.selections.data(), plan.selections.size()}, plan_stats_{plan.stats},
          plan_authentication_receipt_{plan_authentication_receipt},
          plan_image_header_crc64_{plan.image_header_crc64}, descriptors_end_{descriptors_end},
          postings_end_{postings_end}, selection_capacity_{plan.selections.capacity()},
          plan_max_occ_cap_{plan.max_occ_cap}, runtime_max_occ_{plan.runtime_max_occ},
          plan_posting_width_bytes_{plan.posting_width_bytes} {}

    friend std::expected<compact_hash32_selection_span_range_accessor, direct_inverted_index_error>
    bind_compact_hash32_direct_index_selection_spans(const compact_hash32_direct_index_view&,
                                                     const compact_hash32_unique_query_plan&);

    template <class p_visitor>
    friend std::expected<compact_hash32_selection_span_visit_stats, direct_inverted_index_error>
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

} // namespace bioseq::indexing

namespace bioseq::indexing::compact_hash32_detail {

using direct_index_detail::align_up;
using direct_index_detail::checked_add;
using direct_index_detail::checked_multiply;
using direct_index_detail::crc64;
using direct_index_detail::crc64_state;
using direct_index_detail::header_crc;
using direct_index_detail::read_le;
using direct_index_detail::write_le;

struct compact_hash32_unique_query_plan_access {
    static void seal(compact_hash32_unique_query_plan& plan) noexcept {
        plan.authentication_crc64_ = plan.computed_authentication_();
    }

    [[nodiscard]] static bool authenticated(const compact_hash32_unique_query_plan& plan) noexcept {
        return plan.authentication_crc64_ != 0u &&
               plan.authentication_crc64_ == plan.computed_authentication_();
    }

    /// Return the already-authenticated seal without rescanning selections.
    /// Callers must first establish `authenticated(plan)` and retain the plan
    /// immutably while using this constant-time receipt.
    [[nodiscard]] static std::uint64_t
    authentication_receipt(const compact_hash32_unique_query_plan& plan) noexcept {
        return plan.authentication_crc64_;
    }
};

struct compact_hash32_geometry_counts {
    std::uint64_t hash_count = 0u;
    std::uint64_t membership_bytes = 0u;
    std::uint64_t rank_checkpoint_count = 0u;
    std::uint64_t rank_bytes = 0u;
};

[[nodiscard]] constexpr std::expected<compact_hash32_geometry_counts, direct_inverted_index_error>
geometry_counts(compact_hash32_geometry geometry) noexcept {
    if (geometry.hash_domain_bits < 9u ||
        geometry.hash_domain_bits > k_compact_hash32_full_hash_bits) {
        return std::unexpected(direct_inverted_index_error::invalid_options);
    }
    const std::uint32_t expected_max =
        geometry.hash_domain_bits == 32u
            ? (std::numeric_limits<std::uint32_t>::max)()
            : static_cast<std::uint32_t>((std::uint64_t{1u} << geometry.hash_domain_bits) - 1u);
    if (geometry.max_hash != expected_max) {
        return std::unexpected(direct_inverted_index_error::storage_contract_mismatch);
    }
    const std::uint64_t hash_count = std::uint64_t{1u} << geometry.hash_domain_bits;
    return compact_hash32_geometry_counts{
        .hash_count = hash_count,
        .membership_bytes = hash_count / 8u,
        .rank_checkpoint_count = hash_count / k_compact_hash32_direct_index_rank_group_bits,
        .rank_bytes = hash_count / k_compact_hash32_direct_index_rank_group_bits * 4u,
    };
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

    [[nodiscard]] constexpr bool
    operator==(const compact_hash32_scan_receipt&) const noexcept = default;
};

[[nodiscard]] constexpr std::expected<compact_hash32_image_plan, direct_inverted_index_error>
layout_from_receipt(const compact_hash32_build_options& options,
                    const compact_hash32_scan_receipt& receipt) noexcept {
    const auto geometry = geometry_counts(options.geometry);
    if (!geometry)
        return std::unexpected(geometry.error());
    const std::uint8_t posting_width_bytes =
        options.identity.target_length <= (std::uint64_t{1u} << 31u) ? 4u : 8u;
    if (options.identity.target_length > (std::uint64_t{1u} << 63u) ||
        options.identity.generation_id == 0u || options.identity.source_identity == 0u ||
        options.identity.storage_contract_checksum == 0u ||
        (options.max_occ_cap > (std::numeric_limits<std::uint16_t>::max)())) {
        return std::unexpected(direct_inverted_index_error::invalid_options);
    }
    if (receipt.source_hash_count > geometry->hash_count ||
        receipt.accepted_hash_count > receipt.accepted_posting_count ||
        receipt.inline_singleton_count > receipt.accepted_hash_count ||
        receipt.inline_singleton_count > receipt.accepted_posting_count ||
        (posting_width_bytes == 8u && receipt.inline_singleton_count != 0u) ||
        receipt.accepted_posting_count > (std::numeric_limits<std::uint32_t>::max)()) {
        return std::unexpected(direct_inverted_index_error::fixed_width);
    }
    std::uint64_t source_hash_count = 0u;
    std::uint64_t source_posting_count = 0u;
    if (!checked_add(receipt.accepted_hash_count, receipt.rejected_hash_count, source_hash_count) ||
        source_hash_count != receipt.source_hash_count ||
        !checked_add(
            receipt.accepted_posting_count, receipt.rejected_posting_count, source_posting_count) ||
        source_posting_count != receipt.source_posting_count) {
        return std::unexpected(direct_inverted_index_error::storage_contract_mismatch);
    }
    compact_hash32_image_plan plan{
        .identity = options.identity,
        .layout_revision = k_compact_hash32_direct_index_layout_revision,
        .descriptor_width_bytes = k_compact_hash32_direct_index_descriptor_bytes,
        .max_occ_cap = options.max_occ_cap,
        .posting_width_bytes = posting_width_bytes,
        .geometry = options.geometry,
        .source_hash_count = receipt.source_hash_count,
        .source_posting_count = receipt.source_posting_count,
        .accepted_hash_count = receipt.accepted_hash_count,
        .accepted_posting_count = receipt.accepted_posting_count,
        .inline_singleton_count = receipt.inline_singleton_count,
        .materialized_hash_count = receipt.accepted_hash_count - receipt.inline_singleton_count,
        .materialized_posting_count =
            receipt.accepted_posting_count - receipt.inline_singleton_count,
        .rejected_hash_count = receipt.rejected_hash_count,
        .rejected_posting_count = receipt.rejected_posting_count,
        .membership_bytes = geometry->membership_bytes,
        .rank_bytes = geometry->rank_bytes,
        .sorted_stream_crc64 = receipt.sorted_stream_crc64,
    };
    if (!checked_multiply(
            plan.accepted_hash_count, plan.descriptor_width_bytes, plan.descriptors_bytes) ||
        !checked_multiply(
            plan.materialized_posting_count, plan.posting_width_bytes, plan.postings_bytes)) {
        return std::unexpected(direct_inverted_index_error::arithmetic_overflow);
    }
    std::uint64_t cursor = k_compact_hash32_direct_index_header_bytes;
    if (!align_up(cursor, k_direct_inverted_index_section_alignment, plan.membership_offset) ||
        !checked_add(plan.membership_offset, plan.membership_bytes, cursor) ||
        !align_up(cursor, k_direct_inverted_index_section_alignment, plan.rank_offset) ||
        !checked_add(plan.rank_offset, plan.rank_bytes, cursor) ||
        !align_up(cursor, k_direct_inverted_index_section_alignment, plan.descriptors_offset) ||
        !checked_add(plan.descriptors_offset, plan.descriptors_bytes, cursor) ||
        !align_up(cursor, k_direct_inverted_index_section_alignment, plan.postings_offset) ||
        !checked_add(plan.postings_offset, plan.postings_bytes, plan.image_bytes)) {
        return std::unexpected(direct_inverted_index_error::arithmetic_overflow);
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

[[nodiscard]] constexpr compact_hash32_decoded_descriptor
decode_descriptor(std::span<const std::byte> image,
                  std::uint64_t offset,
                  std::uint8_t posting_width_bytes) noexcept {
    const std::uint32_t posting_or_inline =
        read_le<std::uint32_t>(image, static_cast<std::size_t>(offset));
    const std::uint32_t global_count =
        read_le<std::uint16_t>(image, static_cast<std::size_t>(offset + 4u));
    const bool inline_singleton = posting_width_bytes == 4u && global_count == 1u;
    return compact_hash32_decoded_descriptor{
        .posting_ordinal = inline_singleton ? 0u : posting_or_inline,
        .inline_encoded_position = inline_singleton ? posting_or_inline : 0u,
        .global_count = global_count,
        .strand_zero_count = read_le<std::uint16_t>(image, static_cast<std::size_t>(offset + 6u)),
        .inline_singleton = inline_singleton,
    };
}

[[nodiscard]] constexpr std::uint64_t decode_posting(std::span<const std::byte> image,
                                                     std::uint64_t offset,
                                                     std::uint8_t width) noexcept {
    return width == 4u ? static_cast<std::uint64_t>(
                             read_le<std::uint32_t>(image, static_cast<std::size_t>(offset)))
                       : read_le<std::uint64_t>(image, static_cast<std::size_t>(offset));
}

} // namespace bioseq::indexing::compact_hash32_detail

namespace bioseq::indexing::direct_index_detail {

inline constexpr std::uint32_t k_flag_run_summaries = 1u;

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

[[nodiscard]] constexpr std::expected<direct_index_image_plan, direct_inverted_index_error>
layout_from_counts(std::uint64_t occurrence_count,
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
    if (!power_of_two(bucket_count) || bucket_count < 8u ||
        (posting_width != 4u && posting_width != 8u)) {
        return std::unexpected(direct_inverted_index_error::bucket_count);
    }
    plan.occupancy_bytes = ceil_div(bucket_count, 8u);
    const std::uint64_t rank_count =
        ceil_div(bucket_count, k_direct_inverted_index_rank_group_buckets) + 1u;
    if (!checked_multiply(rank_count, sizeof(std::uint64_t), plan.rank_bytes) ||
        !checked_multiply(
            unique_hash_count, k_direct_inverted_index_control_bytes, plan.controls_bytes) ||
        !checked_multiply(
            summary_count, k_direct_inverted_index_run_summary_bytes, plan.summaries_bytes) ||
        !checked_multiply(occurrence_count, posting_width, plan.postings_bytes)) {
        return std::unexpected(direct_inverted_index_error::arithmetic_overflow);
    }
    std::uint64_t cursor = k_direct_inverted_index_header_bytes;
    if (!align_up(cursor, k_direct_inverted_index_section_alignment, plan.occupancy_offset) ||
        !checked_add(plan.occupancy_offset, plan.occupancy_bytes, cursor) ||
        !align_up(cursor, k_direct_inverted_index_section_alignment, plan.rank_offset) ||
        !checked_add(plan.rank_offset, plan.rank_bytes, cursor) ||
        !align_up(cursor, k_direct_inverted_index_section_alignment, plan.controls_offset) ||
        !checked_add(plan.controls_offset, plan.controls_bytes, cursor) ||
        !align_up(cursor, k_direct_inverted_index_section_alignment, plan.summaries_offset) ||
        !checked_add(plan.summaries_offset, plan.summaries_bytes, cursor) ||
        !align_up(cursor, k_direct_inverted_index_section_alignment, plan.postings_offset) ||
        !checked_add(plan.postings_offset, plan.postings_bytes, plan.image_bytes)) {
        return std::unexpected(direct_inverted_index_error::arithmetic_overflow);
    }
    return plan;
}

[[nodiscard]] constexpr direct_index_decoded_control
decode_control(std::span<const std::byte> image, std::uint64_t offset) noexcept {
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

[[nodiscard]] constexpr direct_index_decoded_summary
decode_summary(std::span<const std::byte> image, std::uint64_t offset) noexcept {
    const std::size_t base = static_cast<std::size_t>(offset);
    return direct_index_decoded_summary{
        .fragment_ordinal = read_le<std::uint32_t>(image, base),
        .reserved_zero = read_le<std::uint32_t>(image, base + 4u),
        .posting_byte_offset = read_le<std::uint64_t>(image, base + 8u),
        .strand_zero_count = read_le<std::uint64_t>(image, base + 16u),
        .total_count = read_le<std::uint64_t>(image, base + 24u),
    };
}

[[nodiscard]] inline std::uint64_t occupied_before_in_group(std::span<const std::byte> occupancy,
                                                            std::uint64_t slot) noexcept {
    const std::uint64_t group_begin = (slot / k_direct_inverted_index_rank_group_buckets) *
                                      k_direct_inverted_index_rank_group_buckets;
    std::uint64_t count = 0u;
    for (std::uint64_t candidate = group_begin; candidate < slot;) {
        const std::uint64_t byte_ordinal = candidate / 8u;
        const unsigned within = static_cast<unsigned>(candidate % 8u);
        const unsigned take =
            static_cast<unsigned>(std::min<std::uint64_t>(8u - within, slot - candidate));
        const std::uint8_t byte =
            std::to_integer<std::uint8_t>(occupancy[static_cast<std::size_t>(byte_ordinal)]);
        const std::uint16_t mask =
            static_cast<std::uint16_t>(((std::uint16_t{1u} << take) - 1u) << within);
        count += std::popcount(static_cast<std::uint8_t>(byte & mask));
        candidate += take;
    }
    return count;
}

[[nodiscard]] inline bool occupied(std::span<const std::byte> occupancy,
                                   std::uint64_t slot) noexcept {
    return (std::to_integer<std::uint8_t>(occupancy[static_cast<std::size_t>(slot / 8u)]) &
            (std::uint8_t{1u} << (slot % 8u))) != 0u;
}

struct direct_index_lookup_receipt {
    std::uint64_t slot = 0u;
    std::uint64_t control_ordinal = 0u;
    direct_index_decoded_control control{};
};

[[nodiscard]] inline std::expected<std::optional<direct_index_lookup_receipt>,
                                   direct_inverted_index_error>
lookup_control(const direct_inverted_index_view& view,
               std::uint32_t hash,
               direct_index_query_stats* stats = nullptr) noexcept {
    const auto& header = view.header;
    const auto occupancy = view.image.subspan(static_cast<std::size_t>(header.occupancy_offset),
                                              static_cast<std::size_t>(header.occupancy_bytes));
    const auto ranks = view.image.subspan(static_cast<std::size_t>(header.rank_offset),
                                          static_cast<std::size_t>(header.rank_bytes));
    const std::uint64_t mask = header.bucket_count - 1u;
    std::uint64_t slot = static_cast<std::uint64_t>(mix_hash32(hash)) & mask;
    for (std::uint64_t probe = 0u; probe < header.bucket_count; ++probe) {
        if (stats != nullptr)
            ++stats->dictionary_slots_examined;
        if (!occupied(occupancy, slot))
            return std::optional<direct_index_lookup_receipt>{};
        const std::uint64_t group = slot / k_direct_inverted_index_rank_group_buckets;
        const std::uint64_t rank_base =
            read_le<std::uint64_t>(ranks, static_cast<std::size_t>(group * 8u));
        const std::uint64_t ordinal = rank_base + occupied_before_in_group(occupancy, slot);
        std::uint64_t control_delta = 0u;
        std::uint64_t control_offset = 0u;
        if (!checked_multiply(ordinal, k_direct_inverted_index_control_bytes, control_delta) ||
            !checked_add(header.controls_offset, control_delta, control_offset) ||
            ordinal >= header.unique_hash_count) {
            return std::unexpected(direct_inverted_index_error::dictionary);
        }
        if (stats != nullptr)
            ++stats->control_records_examined;
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

[[nodiscard]] constexpr std::uint64_t decode_position(std::span<const std::byte> image,
                                                      std::uint64_t byte_offset,
                                                      std::uint8_t width) noexcept {
    return width == 4u ? static_cast<std::uint64_t>(
                             read_le<std::uint32_t>(image, static_cast<std::size_t>(byte_offset)))
                       : read_le<std::uint64_t>(image, static_cast<std::size_t>(byte_offset));
}

} // namespace bioseq::indexing::direct_index_detail
