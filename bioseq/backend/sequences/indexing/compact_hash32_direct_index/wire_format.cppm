/**
 * @file wire_format.cppm
 * @brief Stored layouts and byte readers for direct minimizer indexes.
 *
 * Integer fields use little-endian encoding. The header records section
 * offsets, sizes, and CRCs so readers can check the image before using it.
 */
export module bioseq.indexing.compact_hash32_direct_index:wire_format;

import std;
import bioseq.common.checked_arithmetic;
import bioseq.indexing.storage;

namespace bioseq::indexing::direct_index_detail {

/**
 * @brief Write an unsigned integer in little-endian byte order.
 * @tparam p_unsigned_integer Integer type determining the field width.
 * @param[in,out] output Destination with room for the complete field at offset.
 * @param[in] offset First byte to write.
 * @param[in] value Integer to store.
 */
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

/**
 * @brief Read an unsigned integer stored in little-endian byte order.
 * @tparam p_unsigned_integer Integer type determining the field width.
 * @param[in] input Source containing the complete field at offset.
 * @param[in] offset First byte to read.
 * @return Integer in the machine's native byte order.
 */
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

/**
 * @brief Adds two image-size values without unsigned overflow.
 * @param[in] left Left operand
 * @param[in] right Right operand
 * @param[out] result Sum when the operation succeeds
 * @return `true` when the sum is representable
 */
[[nodiscard]] constexpr bool
checked_add(std::uint64_t left, std::uint64_t right, std::uint64_t& result) noexcept {
    const auto sum = bioseq::common::checked_add(left, right);
    if (!sum.has_value()) {
        return false;
    }
    result = *sum;
    return true;
}

/**
 * @brief Multiplies two image-size values without unsigned overflow.
 * @param[in] left Left operand
 * @param[in] right Right operand
 * @param[out] result Product when the operation succeeds
 * @return `true` when the product is representable
 */
[[nodiscard]] constexpr bool
checked_multiply(std::uint64_t left, std::uint64_t right, std::uint64_t& result) noexcept {
    const auto product = bioseq::common::checked_mul(left, right);
    if (!product.has_value()) {
        return false;
    }
    result = *product;
    return true;
}

/**
 * @brief Round an offset up to the requested byte alignment.
 * @param[in] value Offset to align.
 * @param[in] alignment Nonzero power-of-two alignment.
 * @param[out] result Aligned offset on success, unchanged on failure.
 * @return False for invalid alignment or an overflowing result.
 */
[[nodiscard]] constexpr bool
align_up(std::uint64_t value, std::uint64_t alignment, std::uint64_t& result) noexcept {
    const auto aligned = bioseq::common::checked_align_up(value, alignment);
    if (!aligned.has_value()) {
        return false;
    }
    result = *aligned;
    return true;
}

/**
 * @brief Divide and round up to the next whole number.
 * @param[in] value Value to divide.
 * @param[in] divisor Nonzero divisor.
 * @return Ceiling of value / divisor.
 */
[[nodiscard]] constexpr std::uint64_t ceil_div(std::uint64_t value,
                                               std::uint64_t divisor) noexcept {
    return bioseq::common::ceil_div(value, divisor);
}

/**
 * @brief Tests whether a nonzero value has exactly one set bit.
 * @param[in] value Value to inspect
 * @return `true` for a power of two
 */
[[nodiscard]] constexpr bool power_of_two(std::uint64_t value) noexcept {
    return value != 0u && (value & (value - 1u)) == 0u;
}

/**
 * @brief Find the smallest power of two at least as large as value.
 * @param[in] value Value to round, with zero rounded to one.
 * @return Power of two, or nullopt if it would exceed uint64_t.
 */
[[nodiscard]] constexpr std::optional<std::uint64_t>
next_power_of_two(std::uint64_t value) noexcept {
    if (value <= 1u)
        return 1u;
    if (value > (std::uint64_t{1u} << 63u))
        return std::nullopt;
    return std::uint64_t{1u} << (64u - std::countl_zero(value - 1u));
}

/**
 * @brief Mix a minimizer hash before choosing a hash-table slot.
 * @param[in] value Original 32-bit hash.
 * @return Mixed hash used for dictionary placement and lookup.
 */
[[nodiscard]] constexpr std::uint32_t mix_hash32(std::uint32_t value) noexcept {
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}

/// @brief Incremental checksum state used by direct-index codecs.
using crc64_state = bioseq::indexing::crc64_ecma182_accumulator;

/**
 * @brief Compute a checksum of stored bytes.
 * @param[in] bytes Bytes in their stored order.
 * @return CRC-64/ECMA-182 checksum.
 */
[[nodiscard]] inline std::uint64_t crc64(std::span<const std::byte> bytes) noexcept {
    return bioseq::indexing::crc64_ecma182(bytes);
}

/**
 * @brief Checksum the header with its own checksum field treated as zero.
 * @param[in] header Complete header bytes.
 * @param[in] checksum_offset First byte of the checksum field.
 * @param[in] checksum_bytes Field width, at most eight bytes.
 * @return CRC-64/ECMA-182 checksum of that header.
 */
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

/**
 * @brief Tests whether every byte in a reserved wire range is zero.
 * @param[in] bytes Reserved bytes to inspect
 * @return `true` when all bytes are zero
 */
[[nodiscard]] inline bool all_zero(std::span<const std::byte> bytes) noexcept {
    for (const std::byte byte : bytes) {
        if (byte != std::byte{0})
            return false;
    }
    return true;
}

} // namespace bioseq::indexing::direct_index_detail

export namespace bioseq::indexing {

/// @brief Numeric tag at the start of a direct inverted-index header.
inline constexpr std::uint64_t k_direct_inverted_index_magic =
    0x3132494432334842ULL; // Little-endian bytes spell "BH32DI21".
/// @brief Persisted direct inverted-index layout revision accepted by this reader.
inline constexpr std::uint32_t k_direct_inverted_index_layout_revision = 1u;
/// @brief Fixed direct inverted-index header width in bytes.
inline constexpr std::uint32_t k_direct_inverted_index_header_bytes = 256u;
/// @brief Fixed hash-control record width in bytes.
inline constexpr std::uint32_t k_direct_inverted_index_control_bytes = 48u;
/// @brief Fixed fragment-run summary width in bytes.
inline constexpr std::uint32_t k_direct_inverted_index_run_summary_bytes = 32u;
/// @brief Number of dictionary buckets covered by one rank checkpoint.
inline constexpr std::uint32_t k_direct_inverted_index_rank_group_buckets = 256u;
/// @brief Persisted identifier of the dictionary hash mixer.
inline constexpr std::uint32_t k_direct_inverted_index_hash_mix_id = 1u;
/// @brief Required byte alignment of every direct-index section.
inline constexpr std::uint64_t k_direct_inverted_index_section_alignment = 64u;
/// @brief Header offset of the occupancy-section checksum.
inline constexpr std::size_t k_direct_inverted_index_occupancy_crc_offset = 176u;
/// @brief Header offset of the rank-section checksum.
inline constexpr std::size_t k_direct_inverted_index_rank_crc_offset = 184u;
/// @brief Header offset of the control-section checksum.
inline constexpr std::size_t k_direct_inverted_index_controls_crc_offset = 192u;
/// @brief Header offset of the run-summary checksum.
inline constexpr std::size_t k_direct_inverted_index_summaries_crc_offset = 200u;
/// @brief Header offset of the posting-section checksum.
inline constexpr std::size_t k_direct_inverted_index_postings_crc_offset = 208u;
/// @brief Header offset of the checksum calculated with its own field zeroed.
inline constexpr std::size_t k_direct_inverted_index_header_crc_offset = 216u;
/// @brief Width in bytes of each direct-index checksum field.
inline constexpr std::size_t k_direct_inverted_index_checksum_bytes = 8u;
/// @brief Sentinel used when a query occurrence has no accepted selection.
inline constexpr std::uint32_t k_direct_inverted_index_no_selection =
    (std::numeric_limits<std::uint32_t>::max)();
/// @brief Sentinel used when a posting is not restricted to a fragment.
inline constexpr std::uint32_t k_direct_inverted_index_no_fragment =
    (std::numeric_limits<std::uint32_t>::max)();

/** @brief Errors reported while building, opening, or querying a direct index. */
enum class direct_inverted_index_error : std::uint8_t {
    /// @brief A size or offset calculation overflowed.
    arithmetic_overflow,
    /// @brief The requested size or vector capacity does not meet the operation's limits.
    admission,
    /// @brief Required storage could not be allocated.
    allocation,
    /// @brief Build or open settings are unsupported or inconsistent.
    invalid_options,
    /// @brief A hash, target position, or occurrence count is outside the allowed range.
    occurrence_range,
    /// @brief The hash table has an invalid size or cannot hold the required hashes.
    bucket_count,
    /// @brief The image size or stored counts do not match the required layout.
    image_size,
    /// @brief The header tag does not identify this index format.
    bad_magic,
    /// @brief This reader does not support the stored format version.
    bad_layout_revision,
    /// @brief Fixed header fields are internally inconsistent.
    bad_header,
    /// @brief Header flags are unsupported or disagree with other fields.
    bad_flags,
    /// @brief A field reserved for future use contains a nonzero byte.
    reserved_nonzero,
    /// @brief The stored source identity or index settings differ from those expected.
    identity_mismatch,
    /// @brief The fixed-header checksum does not match.
    header_checksum,
    /// @brief A stored section does not match its checksum.
    section_checksum,
    /// @brief Section offsets, sizes, alignment, or padding are invalid.
    section_layout,
    /// @brief The hash dictionary is malformed.
    dictionary,
    /// @brief A posting-list descriptor has invalid fields or counts.
    control,
    /// @brief A fragment summary has invalid counts, ordering, or offsets.
    run_summary,
    /// @brief A stored target position or posting-list range is invalid.
    posting,
    /// @brief The query has too many hashes or selections for this format.
    query_size,
    /// @brief A query plan or selected position fails validation against the index.
    query_plan,
    /// @brief A caller-provided visitor stopped traversal.
    visitor_rejected,
    /// @brief The replayable sorted-run source could not be read completely.
    source_read,
    /// @brief Hashes or positions are out of their required sorted order.
    order,
    /// @brief A count or coordinate cannot fit its persisted width.
    fixed_width,
    /// @brief A replayed source differs from its planning scan.
    replay_mismatch,
    /// @brief Index settings, query limits, or layout metadata disagree.
    storage_contract_mismatch,
    /// @brief The supplied checksum or record does not match the stored image.
    receipt_mismatch,
};

/** @brief A minimizer hash, target position, and strand to add to the index. */
struct direct_index_occurrence {
    std::uint32_t hash = 0u;
    std::uint64_t target_bp = 0u;
    bool target_strand_one = false;

    /// @brief Compares the hash, coordinate, and strand value.
    [[nodiscard]] constexpr bool
    operator==(const direct_index_occurrence&) const noexcept = default;
};

/** @brief Identify the index generation and the target sequence it belongs to. */
struct direct_index_identity {
    std::uint64_t generation_id = 0u;
    std::uint64_t source_identity = 0u;
    std::uint64_t storage_contract_checksum = 0u;
    std::uint64_t target_length = 0u;

    /// @brief Compares every persisted identity component.
    [[nodiscard]] constexpr bool operator==(const direct_index_identity&) const noexcept = default;
};

/** @brief Source identity, optional fragment summaries, and limits for building an index. */
struct direct_index_build_options {
    direct_index_identity identity{};
    /// Target fragment length in symbols. Zero disables summaries. Otherwise
    /// store a summary for each hash occurring in each target fragment.
    std::uint64_t fragment_span_bp = 0u;
    /// Optional power-of-two table size. Zero chooses enough buckets to keep
    /// the load at or below 7/8.
    std::uint64_t bucket_count_override = 0u;
    std::uint64_t max_occurrences = (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t max_unique_hashes = (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t max_bucket_count = (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t max_run_summaries = (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t max_image_bytes = (std::numeric_limits<std::uint64_t>::max)();
    /// Excludes the caller-owned input and final returned image.
    std::uint64_t max_build_scratch_bytes = (std::numeric_limits<std::uint64_t>::max)();
};

/** @brief Section offsets, sizes, and position widths for the planned index. */
struct direct_index_image_plan {
    /**
     * Each section begins on a 64-byte boundary. The image contains a bitmap
     * of occupied buckets, rank checkpoints to find packed control records,
     * optional fragment summaries, and encoded target positions.
     *
     * estimated_build_scratch_bytes covers the sorted occurrence copy, space
     * for hash groups, and the bucket-to-group table. The caller's input and
     * the final image have separate lifetimes and are excluded.
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

    /// @brief Compares every planned count, width, offset, and size.
    [[nodiscard]] constexpr bool
    operator==(const direct_index_image_plan&) const noexcept = default;
};

/** @brief Values decoded from the direct index's fixed-size header. */
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

/** @brief Borrow the validated index bytes and keep a copy of their header. */
struct direct_inverted_index_view {
    std::span<const std::byte> image{};
    direct_index_header header{};

    /** @brief Reports whether the image contains fragment-run summaries. */
    [[nodiscard]] constexpr bool has_run_summaries() const noexcept {
        return (header.flags & 1u) != 0u;
    }
};

/** @brief Supplies one query hash and its query-strand orientation. */
struct direct_index_query_occurrence {
    std::uint32_t hash = 0u;
    bool query_strand_one = false;
};

/** @brief A contiguous range of target positions selected for one query hash. */
struct direct_index_posting_span {
    std::uint64_t byte_offset = 0u;
    std::uint64_t count = 0u;
    std::uint32_t selection_ordinal = 0u;
    std::uint32_t fragment_ordinal = k_direct_inverted_index_no_fragment;
    bool target_strand_one = false;
};

/** @brief The selected posting ranges and strand counts for one query hash. */
struct direct_index_selection {
    std::uint32_t hash = 0u;
    std::uint64_t global_count = 0u;
    std::uint64_t target_strand_zero_count = 0u;
    std::uint64_t first_span = 0u;
    std::uint64_t span_count = 0u;
};

/** @brief Reports lookup acceptance and strand counts for one query occurrence. */
struct direct_index_query_result {
    bool present = false;
    bool positions_selected = false;
    std::uint32_t selection_ordinal = k_direct_inverted_index_no_selection;
    std::uint64_t global_count = 0u;
    std::uint64_t forward_count = 0u;
    std::uint64_t reverse_count = 0u;
};

/** @brief Lookup work and selected-position counts recorded during planning. */
struct direct_index_query_stats {
    std::uint64_t query_occurrence_count = 0u;
    std::uint64_t dictionary_slots_examined = 0u;
    std::uint64_t control_records_examined = 0u;
    std::uint64_t run_summaries_examined = 0u;
    std::uint64_t unique_selected_hash_count = 0u;
    std::uint64_t selected_posting_count = 0u;
    std::uint64_t selected_posting_bytes = 0u;
};

/** @brief Own the selected lists and the result for each query occurrence. */
struct direct_index_query_plan {
    std::uint64_t image_header_crc64 = 0u;
    std::uint8_t posting_width_bytes = 0u;
    std::vector<direct_index_selection> selections{};
    std::vector<direct_index_posting_span> spans{};
    std::vector<direct_index_query_result> results{};
    direct_index_query_stats stats{};
};

/** @brief A target position and the query selection that requested it. */
struct direct_index_posting {
    std::uint32_t selection_ordinal = 0u;
    std::uint32_t fragment_ordinal = k_direct_inverted_index_no_fragment;
    std::uint64_t encoded_position = 0u;
    bool target_strand_one = false;

    /** @brief Returns the zero-based target coordinate encoded above the strand bit. */
    [[nodiscard]] constexpr std::uint64_t target_bp() const noexcept {
        return encoded_position >> 1u;
    }
};

/**
 * @brief Callback for one decoded target position.
 *
 * Return false to stop traversal with a visitor_rejected error. The posting
 * reference is valid only during the call.
 */
using direct_index_posting_visitor = bool (*)(void*, const direct_index_posting&);

/** @brief Counts spans and postings delivered by one visitor traversal. */
struct direct_index_posting_visit_stats {
    std::uint64_t spans_visited = 0u;
    std::uint64_t postings_visited = 0u;
};

/**
 * @brief Compute a direct-index byte range's checksum.
 * @param[in] bytes Bytes in their stored order.
 * @return CRC-64/ECMA-182 checksum.
 */
[[nodiscard]] inline std::uint64_t
direct_inverted_index_crc64(std::span<const std::byte> bytes) noexcept {
    return direct_index_detail::crc64(bytes);
}

} // namespace bioseq::indexing

// Compact hash32 layout with one membership bit per possible hash.

namespace bioseq::indexing::compact_hash32_detail {
/// @brief Allow the planner to save and check a query plan's private checksum.
struct compact_hash32_unique_query_plan_access;
}

export namespace bioseq::indexing {

/// @brief Numeric tag at the start of a compact hash32 image header.
inline constexpr std::uint64_t k_compact_hash32_direct_index_magic = 0x3143443233485342ULL;
/// Version of the stored compact hash32 format.
inline constexpr std::uint32_t k_compact_hash32_direct_index_layout_revision = 2u;
/// @brief Fixed compact hash32 header width in bytes.
inline constexpr std::uint32_t k_compact_hash32_direct_index_header_bytes = 256u;
/// Descriptor bytes: a uint32_t followed by total and strand-zero uint16_t counts.
/// The first field holds the encoded position for a single 32-bit occurrence.
/// Other lists use it as an index into the posting section.
inline constexpr std::uint16_t k_compact_hash32_direct_index_descriptor_bytes = 8u;
/// @brief Number of membership bits covered by one rank checkpoint.
inline constexpr std::uint32_t k_compact_hash32_direct_index_rank_group_bits = 256u;
/// @brief Width of the unrestricted hash32 domain.
inline constexpr std::uint8_t k_compact_hash32_full_hash_bits = 32u;
/// @brief Width of the canonical K=15 minimizer hash domain.
inline constexpr std::uint8_t k_compact_hash32_canonical_k15_hash_bits = 30u;
/// @brief Membership bitmap width in bytes for the full 32-bit hash domain.
inline constexpr std::uint64_t k_compact_hash32_full_membership_bytes =
    std::uint64_t{1u} << 29u; // 2^32 exact membership bits.
/// @brief Total bytes for rank checkpoints over the full 32-bit hash domain.
inline constexpr std::uint64_t k_compact_hash32_full_rank_bytes =
    std::uint64_t{1u} << 26u; // 2^24 uint32 checkpoints.
/// @brief Membership bitmap width in bytes for the canonical K=15 domain.
inline constexpr std::uint64_t k_compact_hash32_canonical_k15_membership_bytes =
    std::uint64_t{1u} << 27u; // 2^30 exact membership bits.
/// @brief Total bytes for rank checkpoints over the canonical K=15 domain.
inline constexpr std::uint64_t k_compact_hash32_canonical_k15_rank_bytes =
    std::uint64_t{1u} << 24u; // 2^22 uint32 checkpoints.
/// @brief Header offset of the compact membership-section checksum.
inline constexpr std::size_t k_compact_hash32_membership_crc_offset = 176u;
/// @brief Header offset of the compact rank-section checksum.
inline constexpr std::size_t k_compact_hash32_rank_crc_offset = 184u;
/// @brief Header offset of the compact descriptor-section checksum.
inline constexpr std::size_t k_compact_hash32_descriptors_crc_offset = 192u;
/// @brief Header offset of the compact posting-section checksum.
inline constexpr std::size_t k_compact_hash32_postings_crc_offset = 200u;
/// @brief Header offset of the checksum calculated with its own field zeroed.
inline constexpr std::size_t k_compact_hash32_header_crc_offset = 208u;
/// @brief Header offset of the replayed sorted-stream checksum.
inline constexpr std::size_t k_compact_hash32_stream_crc_offset = 224u;
/// @brief Header offset of the count of postings stored inline in descriptors.
inline constexpr std::size_t k_compact_hash32_inline_singleton_count_offset = 236u;

/** @brief Bitmap and rank-checkpoint positions for one hash. */
struct compact_hash32_address {
    std::uint64_t membership_byte_ordinal = 0u;
    std::uint8_t membership_bit = 0u;
    std::uint64_t rank_checkpoint_ordinal = 0u;
    std::uint16_t bit_within_rank_group = 0u;

    /// @brief Compares every resolved address component.
    [[nodiscard]] constexpr bool operator==(const compact_hash32_address&) const noexcept = default;
};

/// Size of the hash domain represented by the membership bitmap.
/// Widths from 9 through 32 bits are supported. Canonical K=15 hashes use 30
/// bits, and full hash32 uses 32. Tests can use smaller domains to avoid large
/// bitmaps. The header stores both the width and its largest hash value.
struct compact_hash32_geometry {
    std::uint8_t hash_domain_bits = k_compact_hash32_canonical_k15_hash_bits;
    std::uint32_t max_hash = (std::uint32_t{1u} << k_compact_hash32_canonical_k15_hash_bits) - 1u;

    /** @brief Reports whether the geometry is the canonical 30-bit K=15 domain. */
    [[nodiscard]] constexpr bool canonical_k15() const noexcept {
        return hash_domain_bits == k_compact_hash32_canonical_k15_hash_bits &&
               max_hash == 0x3fffffffu;
    }

    /** @brief Reports whether the geometry covers the complete 32-bit hash domain. */
    [[nodiscard]] constexpr bool full_h32() const noexcept {
        return hash_domain_bits == k_compact_hash32_full_hash_bits &&
               max_hash == (std::numeric_limits<std::uint32_t>::max)();
    }

    /// @brief Compares the hash-domain width and maximum hash.
    [[nodiscard]] constexpr bool
    operator==(const compact_hash32_geometry&) const noexcept = default;
};

/**
 * @brief Find a hash's membership bit and rank checkpoint.
 * @param[in] hash Hash to locate.
 * @param[in] geometry Domain width and largest allowed hash.
 * @return Bitmap address, or a range error if the geometry or hash is invalid.
 */
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

/** @brief Hash and strand counts for one list supplied by a replayable source. */
struct compact_hash32_sorted_run_header {
    std::uint32_t hash = 0u;
    std::uint64_t global_count = 0u;
    std::uint64_t target_strand_zero_count = 0u;
};

/** @brief Callback that begins one sorted hash run. */
using compact_hash32_begin_run = std::expected<void, direct_inverted_index_error> (*)(
    void*, const compact_hash32_sorted_run_header&);
/** @brief Callback that supplies the next ordered position batch for the active run. */
using compact_hash32_consume_positions =
    std::expected<void, direct_inverted_index_error> (*)(void*, std::span<const std::uint64_t>);

/** @brief Callbacks that receive hash headers and temporary batches of positions. */
struct compact_hash32_sorted_run_receiver {
    void* context = nullptr;
    compact_hash32_begin_run begin_run_fn = nullptr;
    compact_hash32_consume_positions consume_positions_fn = nullptr;

    /**
     * @brief Send the next hash header to the receiver.
     * @param[in] header Hash, total count, and strand-zero count for the list.
     * @return Callback result, or source_read if the callback or context is missing.
     */
    [[nodiscard]] std::expected<void, direct_inverted_index_error>
    begin_run(const compact_hash32_sorted_run_header& header) const {
        if (context == nullptr || begin_run_fn == nullptr) {
            return std::unexpected(direct_inverted_index_error::source_read);
        }
        return begin_run_fn(context, header);
    }

    /**
     * @brief Send the next batch of positions for the current hash.
     * @param[in] encoded_positions Positions with strand in the low bit, valid during the call.
     * @return Callback result, or source_read if the callback or context is missing.
     */
    [[nodiscard]] std::expected<void, direct_inverted_index_error>
    consume_positions(std::span<const std::uint64_t> encoded_positions) const {
        if (context == nullptr || consume_positions_fn == nullptr) {
            return std::unexpected(direct_inverted_index_error::source_read);
        }
        return consume_positions_fn(context, encoded_positions);
    }
};

/** @brief Callback that sends every sorted hash list to a receiver. */
using compact_hash32_scan_sorted_runs = std::expected<void, direct_inverted_index_error> (*)(
    void*, const compact_hash32_sorted_run_receiver&);

/// A source that can replay hash lists in strictly increasing hash order.
/// Each list supplies exactly global_count positions, in batches of any size.
/// Send strand-zero positions first, then strand-one positions, with coordinates
/// in nondecreasing order within each strand.
struct compact_hash32_sorted_run_source {
    void* context = nullptr;
    compact_hash32_scan_sorted_runs scan_sorted_runs_fn = nullptr;

    /**
     * @brief Replay every hash list into the receiver.
     * @param[in] receiver Callbacks that consume each header and position batch immediately.
     * @return Completion status, or source_read if this source has no callback or context.
     */
    [[nodiscard]] std::expected<void, direct_inverted_index_error>
    scan_sorted_runs(const compact_hash32_sorted_run_receiver& receiver) const {
        if (context == nullptr || scan_sorted_runs_fn == nullptr) {
            return std::unexpected(direct_inverted_index_error::source_read);
        }
        return scan_sorted_runs_fn(context, receiver);
    }
};

/** @brief Source identity, occurrence cap, hash domain, and maximum image size. */
struct compact_hash32_build_options {
    direct_index_identity identity{};
    std::uint32_t max_occ_cap = 0u;
    compact_hash32_geometry geometry{};
    std::uint64_t max_image_bytes = (std::numeric_limits<std::uint64_t>::max)();
};

/**
 * @brief Section sizes and counts for a compact hash32 index.
 *
 * A membership bit says whether a hash was accepted during the build. Rank
 * checkpoints count earlier set bits to find its eight-byte descriptor. A
 * single 32-bit position fits inside that descriptor. Other lists store their
 * positions in the posting section. All integer fields use little-endian encoding.
 */
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

    /// @brief Compares every identity, count, width, offset, size, and stream checksum.
    [[nodiscard]] constexpr bool
    operator==(const compact_hash32_image_plan&) const noexcept = default;
};

/** @brief Decoded compact header, including the layout and stored checksums. */
struct compact_hash32_header : compact_hash32_image_plan {
    std::uint64_t membership_crc64 = 0u;
    std::uint64_t rank_crc64 = 0u;
    std::uint64_t descriptors_crc64 = 0u;
    std::uint64_t postings_crc64 = 0u;
    std::uint64_t header_crc64 = 0u;
};

/** @brief Borrowed compact-index bytes with a copy of their decoded header. */
struct compact_hash32_direct_index_view {
    std::span<const std::byte> image{};
    compact_hash32_header header{};
};

/// Expected index identity and layout supplied by the surrounding file format.
/// The bounded opener compares these values while reading only the fixed header.
struct compact_hash32_bounded_open_options {
    direct_index_identity expected_identity{};
    std::uint32_t expected_max_occ_cap = 0u;
    compact_hash32_geometry expected_geometry{};
    /// Optional CRC-64/ECMA-182 of all 256 header bytes, including the header's
    /// own stored checksum field.
    std::optional<std::uint64_t> expected_direct_header_crc64{};
};

/** @brief Counts bytes inspected by a bounded compact-header open. */
struct compact_hash32_bounded_open_stats {
    std::uint64_t direct_section_bytes = 0u;
    std::uint64_t header_bytes_examined = 0u;
    std::uint64_t payload_bytes_examined = 0u;
    bool expected_direct_header_crc64_checked = false;
};

/// A decoded index view with a shared pointer that keeps its owner alive.
/// The bounded opener makes this pointer an alias to the index bytes inside
/// the mapped file. Direct construction may use a pointer different from
/// image.data(). If the pointer does not own the mapping, the external mapping
/// lease must outlive this view.
class compact_hash32_bounded_open_view {
  public:
    /**
     * @brief Keep the decoded view together with its mapping lifetime token.
     * @param[in] image_owner Shared owner, or token backed by an external mapping lease.
     * @param[in] view Index bytes covered by that owner or lease.
     * @param[in] stats Header checks performed while opening the index.
     */
    compact_hash32_bounded_open_view(std::shared_ptr<const std::byte> image_owner,
                                     compact_hash32_direct_index_view view,
                                     compact_hash32_bounded_open_stats stats) noexcept
        : image_owner_{std::move(image_owner)}, view_{view}, stats_{stats} {}

    /** @brief Return the borrowed index bytes and decoded header. */
    [[nodiscard]] const compact_hash32_direct_index_view& view() const noexcept {
        return view_;
    }

    /** @brief Return the saved mapping lifetime token. */
    [[nodiscard]] const std::shared_ptr<const std::byte>& image_owner() const noexcept {
        return image_owner_;
    }

    /** @brief Return the byte counts recorded while opening the header. */
    [[nodiscard]] const compact_hash32_bounded_open_stats& stats() const noexcept {
        return stats_;
    }

  private:
    std::shared_ptr<const std::byte> image_owner_{};
    compact_hash32_direct_index_view view_{};
    compact_hash32_bounded_open_stats stats_{};
};

/** @brief Supplies one compact query hash and its query-strand orientation. */
struct compact_hash32_query_occurrence {
    std::uint32_t hash = 0u;
    bool query_strand_one = false;
};

/** @brief Acceptance, descriptor index, and strand counts for one query occurrence. */
struct compact_hash32_query_result {
    bool accepted = false;
    std::uint32_t selection_ordinal = k_direct_inverted_index_no_selection;
    std::uint64_t dense_ordinal = 0u;
    std::uint64_t global_count = 0u;
    std::uint64_t forward_count = 0u;
    std::uint64_t reverse_count = 0u;
};

/** @brief Describes an accepted distinct hash and its posting spans. */
struct compact_hash32_selection {
    std::uint32_t hash = 0u;
    std::uint64_t dense_ordinal = 0u;
    std::uint64_t global_count = 0u;
    std::uint64_t target_strand_zero_count = 0u;
    std::uint64_t first_span = 0u;
    std::uint64_t span_count = 0u;
};

/** @brief Count lookup work, selected positions, and query-plan allocations. */
struct compact_hash32_query_stats {
    std::uint64_t query_occurrence_count = 0u;
    std::uint64_t distinct_query_hash_count = 0u;
    std::uint64_t rank_checkpoints_examined = 0u;
    /// Membership words loaded from the image. The unique-query planner caches
    /// all four words in a 256-hash group and reuses them for later hashes.
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
    /// Selection-storage counters. The unique-query planner reserves for all
    /// input hashes, fills without growing, then reduces capacity to exactly the
    /// number of accepted mappings.
    std::uint64_t selection_reserve_call_count = 0u;
    std::uint64_t selection_reserve_requested_elements = 0u;
    std::uint64_t selection_initial_capacity_elements = 0u;
    std::uint64_t selection_fill_reallocation_count = 0u;
    std::uint64_t selection_compaction_allocation_count = 0u;
    std::uint64_t selection_storage_allocation_count = 0u;
    std::uint64_t selection_peak_capacity_bytes = 0u;
    std::uint64_t selection_retained_capacity_elements = 0u;
    std::uint64_t selection_retained_capacity_bytes = 0u;

    /// @brief Compares every query-work and allocation counter.
    [[nodiscard]] constexpr bool
    operator==(const compact_hash32_query_stats&) const noexcept = default;
};

/** @brief Selected compact-index lists and a result for every query occurrence. */
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

/** @brief Connect an accepted query hash's input index to its descriptor index. */
struct compact_hash32_unique_selection {
    std::uint32_t dense_ordinal = 0u;
    std::uint32_t query_ordinal = 0u;
};
static_assert(sizeof(compact_hash32_unique_selection) == 8u);

/// Selected descriptor indices for a sorted set of unique query hashes.
/// Traversal can read these descriptors directly, without repeating bitmap and
/// rank lookups. A private checksum covers the public fields and mappings so
/// binding can detect changes made after the planner built the result.
struct compact_hash32_unique_query_plan {
    std::uint64_t image_header_crc64 = 0u;
    std::uint32_t max_occ_cap = 0u;
    std::uint32_t runtime_max_occ = 0u;
    std::uint8_t posting_width_bytes = 0u;
    std::vector<compact_hash32_unique_selection> selections{};
    compact_hash32_query_stats stats{};

    /**
     * @brief Measure the selection vector's allocated capacity in bytes.
     * @return Byte count, capped at uint64_t maximum on overflow.
     */
    [[nodiscard]] std::uint64_t retained_selection_capacity_bytes() const noexcept {
        const std::uint64_t capacity = selections.capacity();
        return capacity > (std::numeric_limits<std::uint64_t>::max)() /
                              sizeof(compact_hash32_unique_selection)
                   ? (std::numeric_limits<std::uint64_t>::max)()
                   : capacity * sizeof(compact_hash32_unique_selection);
    }

  private:
    std::uint64_t authentication_crc64_ = 0u;

    /** @brief Include one plan field in the checksum used to detect changes. */
    [[nodiscard]] static constexpr std::uint64_t mix_authentication_(std::uint64_t state,
                                                                     std::uint64_t value) noexcept {
        value += 0x9e3779b97f4a7c15ULL;
        value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27u)) * 0x94d049bb133111ebULL;
        value ^= value >> 31u;
        return std::rotl(state ^ value, 27u) * 0x3c79ac492ba7b653ULL + 0x1c69b3f74ac4ae35ULL;
    }

    /** @brief Recalculate the checksum over the plan fields, counters, and mappings. */
    [[nodiscard]] std::uint64_t computed_authentication_() const noexcept {
        std::uint64_t state = 0x43483332504c414eULL;
        // Include the next field in the plan checksum.
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

/// Borrowed encoded positions for one target strand.
/// The bulk reader checks the descriptor and byte range before creating this
/// view. Callers can then read or visit positions directly, with coordinate
/// and strand checks performed as each position is decoded.
struct compact_hash32_posting_span_view {
    std::span<const std::byte> encoded_bytes{};
    std::uint64_t count = 0u;
    std::uint64_t target_length = 0u;
    std::uint8_t posting_width_bytes = 0u;
    bool target_strand_one = false;

    /**
     * @brief Read one position and check its strand and target bounds.
     * @param[in] ordinal Position index within this span.
     * @return Position with strand in its low bit, or a query-plan error.
     */
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

    /**
     * @brief Decode every position in this span and pass it to visitor.
     *
     * Choose the 32- or 64-bit reader once before the loop. Check each position's
     * strand and target bounds before calling visitor. A later error can follow
     * earlier successful callbacks.
     *
     * @tparam p_visitor Callback accepting a position with its strand in the low bit.
     * @param[in] visitor Synchronous callback. Return true to continue or false
     * to stop with an error. Do not keep references into the image.
     * @return Number of visited positions, or a validation or visitor error.
     */
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
    /**
     * @brief Decode consecutive positions using a fixed integer width.
     * @tparam p_encoded_integer Unsigned 32- or 64-bit stored position type.
     * @tparam p_visitor Callback accepting an encoded position.
     * @param[in] visitor Return true to continue or false to stop with an error.
     * @return Number of visited positions, or a validation or visitor error.
     */
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

/// The nonempty target-strand spans for one accepted query hash.
/// The bulk reader passes this value once per selected hash.
struct compact_hash32_selection_posting_spans {
    std::uint32_t selection_ordinal = 0u;
    std::uint32_t dense_ordinal = 0u;
    std::uint32_t query_ordinal = 0u;
    std::uint32_t global_count = 0u;
    std::uint32_t strand_zero_count = 0u;
    std::array<compact_hash32_posting_span_view, 2u> spans{};
    std::uint8_t span_count = 0u;
};

/** @brief Count selection callbacks and positions exposed through their spans. */
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

/// Borrow a checked query plan and index for repeated visits to selection ranges.
/// Binding scans the complete plan once. Each range visit checks the saved
/// binding and reads only its selected descriptors. The plan, selection storage,
/// and mapped bytes must remain alive and unchanged for the accessor's lifetime.
/// Independent visitors may read separate ranges concurrently.
class compact_hash32_selection_span_range_accessor {
  public:
    /// @brief Copy the borrowed view and plan references.
    compact_hash32_selection_span_range_accessor(
        const compact_hash32_selection_span_range_accessor&) noexcept = default;
    /// @brief Refer to the same borrowed image and plan as another accessor.
    compact_hash32_selection_span_range_accessor&
    operator=(const compact_hash32_selection_span_range_accessor&) noexcept = default;
    /// @brief Move the borrowed binding without taking ownership of the image.
    compact_hash32_selection_span_range_accessor(
        compact_hash32_selection_span_range_accessor&&) noexcept = default;
    /// @brief Replace this accessor with another borrowed binding.
    compact_hash32_selection_span_range_accessor&
    operator=(compact_hash32_selection_span_range_accessor&&) noexcept = default;
    /// @brief Destroy the accessor while leaving the image and plan with their owners.
    ~compact_hash32_selection_span_range_accessor() = default;

    /** @brief Return the number of accepted selections in the bound plan. */
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

    /** @brief Save borrowed image and plan references after the binding checks. */
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

/** @brief Upper bounds for metadata bytes read while looking up query hashes. */
struct compact_hash32_query_touch_estimate {
    std::uint64_t query_hashes = 0u;
    std::uint64_t distinct_rank_groups = 0u;
    std::uint64_t accepted_query_hashes = 0u;
    std::uint64_t maximum_membership_bytes = 0u;
    std::uint64_t maximum_rank_bytes = 0u;
    std::uint64_t maximum_descriptor_bytes = 0u;
    std::uint64_t maximum_metadata_bytes = 0u;
};

/** @brief Section-size estimates based on accepted hash and occurrence totals. */
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

/** @brief Save and check the private checksum of an accepted-hash query plan. */
struct compact_hash32_unique_query_plan_access {
    /** @brief Save a checksum of the plan's current fields and selections. */
    static void seal(compact_hash32_unique_query_plan& plan) noexcept {
        plan.authentication_crc64_ = plan.computed_authentication_();
    }

    /** @brief Check that the plan still matches its saved checksum. */
    [[nodiscard]] static bool authenticated(const compact_hash32_unique_query_plan& plan) noexcept {
        return plan.authentication_crc64_ != 0u &&
               plan.authentication_crc64_ == plan.computed_authentication_();
    }

    /// Read the saved checksum without scanning selections again.
    /// First check authenticated(plan), then keep the plan unchanged while
    /// using this value for later binding checks.
    [[nodiscard]] static std::uint64_t
    authentication_receipt(const compact_hash32_unique_query_plan& plan) noexcept {
        return plan.authentication_crc64_;
    }
};

/** @brief Hash-domain size and the resulting bitmap and rank-table sizes. */
struct compact_hash32_geometry_counts {
    std::uint64_t hash_count = 0u;
    std::uint64_t membership_bytes = 0u;
    std::uint64_t rank_checkpoint_count = 0u;
    std::uint64_t rank_bytes = 0u;
};

/**
 * @brief Calculate bitmap and rank-table sizes for a hash domain.
 * @param[in] geometry Domain bit width and corresponding largest hash value.
 * @return Hash and section counts, or an error for inconsistent geometry.
 */
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

/** @brief Records accepted and rejected counts plus the checksum of one planning scan. */
struct compact_hash32_scan_receipt {
    std::uint64_t source_hash_count = 0u;
    std::uint64_t source_posting_count = 0u;
    std::uint64_t accepted_hash_count = 0u;
    std::uint64_t accepted_posting_count = 0u;
    std::uint64_t inline_singleton_count = 0u;
    std::uint64_t rejected_hash_count = 0u;
    std::uint64_t rejected_posting_count = 0u;
    std::uint64_t sorted_stream_crc64 = 0u;

    /// @brief Compares every scan count and the sorted-stream checksum.
    [[nodiscard]] constexpr bool
    operator==(const compact_hash32_scan_receipt&) const noexcept = default;
};

/**
 * @brief Calculate compact section offsets from the source scan's counts.
 * @param[in] options Source identity, occurrence cap, and hash domain.
 * @param[in] receipt Counts and checksum recorded by the source scan.
 * @return Image layout, or an error for invalid settings, counts, or overflow.
 */
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

/** @brief Values read from an eight-byte compact posting-list descriptor. */
struct compact_hash32_decoded_descriptor {
    std::uint32_t posting_ordinal = 0u;
    std::uint32_t inline_encoded_position = 0u;
    std::uint32_t global_count = 0u;
    std::uint32_t strand_zero_count = 0u;
    bool inline_singleton = false;
};

/**
 * @brief Read a compact list's counts and posting location.
 * @param[in] image Image containing the complete eight-byte descriptor.
 * @param[in] offset First byte of the descriptor.
 * @param[in] posting_width_bytes Position width, either 4 or 8 bytes.
 * @return List counts with a posting index or an inline singleton position.
 */
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

/**
 * @brief Read a 32- or 64-bit encoded target position.
 * @param[in] image Image containing the complete position field.
 * @param[in] offset First byte of the field.
 * @param[in] width Field width, either 4 or 8 bytes.
 * @return Encoded position widened to uint64_t, with strand in the low bit.
 */
[[nodiscard]] constexpr std::uint64_t decode_posting(std::span<const std::byte> image,
                                                     std::uint64_t offset,
                                                     std::uint8_t width) noexcept {
    return width == 4u ? static_cast<std::uint64_t>(
                             read_le<std::uint32_t>(image, static_cast<std::size_t>(offset)))
                       : read_le<std::uint64_t>(image, static_cast<std::size_t>(offset));
}

} // namespace bioseq::indexing::compact_hash32_detail

namespace bioseq::indexing::direct_index_detail {

/// @brief Header flag indicating that fragment-run summaries are present.
inline constexpr std::uint32_t k_flag_run_summaries = 1u;

/** @brief Counts and byte offsets read from a 48-byte hash-table control record. */
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

/** @brief Counts and posting offset for one hash within one target fragment. */
struct direct_index_decoded_summary {
    std::uint32_t fragment_ordinal = 0u;
    std::uint32_t reserved_zero = 0u;
    std::uint64_t posting_byte_offset = 0u;
    std::uint64_t strand_zero_count = 0u;
    std::uint64_t total_count = 0u;
};

/**
 * @brief Calculate direct-index section sizes and aligned byte offsets.
 * @param[in] occurrence_count Total target positions.
 * @param[in] unique_hash_count Number of hash control records.
 * @param[in] bucket_count Power-of-two table size, at least eight.
 * @param[in] summary_count Number of fragment summaries.
 * @param[in] posting_width Bytes per encoded position, either 4 or 8.
 * @return Image layout, or an invalid-size or overflow error.
 */
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

/**
 * @brief Read a hash-table control record in little-endian byte order.
 * @param[in] image Image containing the complete 48-byte record.
 * @param[in] offset First byte of the record.
 * @return Decoded hash, counts, offsets, flags, and reserved field.
 */
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

/**
 * @brief Read a fragment summary in little-endian byte order.
 * @param[in] image Image containing the complete 32-byte record.
 * @param[in] offset First byte of the record.
 * @return Fragment number, position offset, counts, and reserved field.
 */
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

/**
 * @brief Count occupied slots before slot within its 256-bucket group.
 * @param[in] occupancy Bitmap containing the requested group and slot.
 * @param[in] slot Bucket index.
 * @return Set-bit count to add to the group's rank checkpoint.
 */
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

/**
 * @brief Check whether a hash-table bucket is occupied.
 * @param[in] occupancy Bitmap containing the requested slot.
 * @param[in] slot Bucket index.
 * @return True when the bucket has a stored control record.
 */
[[nodiscard]] inline bool occupied(std::span<const std::byte> occupancy,
                                   std::uint64_t slot) noexcept {
    return (std::to_integer<std::uint8_t>(occupancy[static_cast<std::size_t>(slot / 8u)]) &
            (std::uint8_t{1u} << (slot % 8u))) != 0u;
}

/** @brief A found hash-table bucket and its packed control-record index. */
struct direct_index_lookup_receipt {
    std::uint64_t slot = 0u;
    std::uint64_t control_ordinal = 0u;
    direct_index_decoded_control control{};
};

/**
 * @brief Find a hash in the dictionary by linear probing.
 *
 * Start at the mixed hash's bucket and check successive occupied buckets.
 * Bitmap rank counts locate their packed control records. An empty bucket
 * ends an unsuccessful lookup.
 *
 * @param[in] view Checked direct-index image.
 * @param[in] hash Hash to find.
 * @param[in,out] stats Optional counters for bucket and control-record reads.
 * @return Empty optional if absent, the matching record if found, or an error.
 */
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

/**
 * @brief Read a 32- or 64-bit encoded target position.
 * @param[in] image Image containing the complete position field.
 * @param[in] byte_offset First byte of the field.
 * @param[in] width Field width, either 4 or 8 bytes.
 * @return Encoded target position with strand in the low bit.
 */
[[nodiscard]] constexpr std::uint64_t decode_position(std::span<const std::byte> image,
                                                      std::uint64_t byte_offset,
                                                      std::uint8_t width) noexcept {
    return width == 4u ? static_cast<std::uint64_t>(
                             read_le<std::uint32_t>(image, static_cast<std::size_t>(byte_offset)))
                       : read_le<std::uint64_t>(image, static_cast<std::size_t>(byte_offset));
}

} // namespace bioseq::indexing::direct_index_detail
