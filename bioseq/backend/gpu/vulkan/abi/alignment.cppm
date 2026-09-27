/**
 * @file vulkan/abi/alignment.cppm
 * @brief Data shared by the CPU and Vulkan alignment shaders.
 *
 * These records carry anchors, chains, alignment results, and CIGAR output.
 * Static assertions check  field offsets and sizes so both sides use the same
 * memory layout.
 */
module;

#include <cstddef>

export module bioseq.gpu.vulkan.abi:alignment;

import :seed_anchor_sort;
import std;
import bioseq.common.checked_arithmetic;

export namespace bioseq::gpu::vulkan {

/** @brief Maximum sequence length handled by the extension kernel. */
inline constexpr std::uint32_t k_gpu_alignment_extension_max_len = 512u;
/** @brief Sequence length represented by one CIGAR traceback tile. */
inline constexpr std::uint32_t k_gpu_alignment_cigar_traceback_tile_len = 128u;
/** @brief Largest per-operation symbol count representable by the packed CIGAR word. */
inline constexpr std::uint32_t k_gpu_alignment_cigar_operation_max_len =
    (std::numeric_limits<std::uint32_t>::max)() >> 4u;
/** @brief Maximum gap length represented by the simple CIGAR kernel. */
inline constexpr std::uint32_t k_gpu_alignment_simple_cigar_gap_max = 256u;
/** @brief Maximum simple-CIGAR gap operations emitted by one alignment. */
inline constexpr std::uint32_t k_gpu_alignment_simple_cigar_gap_op_max = 64u;
/** @brief Largest band width accepted by the extension kernel. */
inline constexpr std::uint32_t k_gpu_alignment_extension_max_band_width = 512u;
/** @brief Shader status indicating successful alignment extension. */
inline constexpr std::uint32_t k_gpu_alignment_extension_status_ok = 0u;
/** @brief The alignment inputs or dimensions are unsupported. */
inline constexpr std::uint32_t k_gpu_alignment_extension_status_unsupported_geometry = 1u;
/** @brief Shader status indicating insufficient CIGAR output capacity. */
inline constexpr std::uint32_t k_gpu_alignment_extension_status_cigar_overflow = 2u;
/** @brief Input buffers contain packed sequence units. */
inline constexpr std::uint32_t k_gpu_alignment_cigar_job_flag_packed_inputs = 1u << 0u;
/** @brief Device must reverse-complement the target packed window before alignment. */
inline constexpr std::uint32_t k_gpu_alignment_cigar_job_flag_reverse_target = 1u << 1u;
/** @brief Complete set of accepted CIGAR job flags. */
inline constexpr std::uint32_t k_gpu_alignment_cigar_job_flags =
    k_gpu_alignment_cigar_job_flag_packed_inputs | k_gpu_alignment_cigar_job_flag_reverse_target;
/** @brief Version of the temporary chain-candidate and MAPQ record layouts. */
inline constexpr std::uint32_t k_gpu_persisted_mapq_abi_revision = 1u;
/** @brief Maximum accepted chains per query. Exceeding it reports an error. */
inline constexpr std::uint32_t k_gpu_persisted_mapq_candidate_capacity = 64u;
/** @brief Candidate or MAPQ computation completed successfully. */
inline constexpr std::uint32_t k_gpu_mapq_status_ok = 0u;
/** @brief Accepted chains exceeded the reserved candidate capacity. */
inline constexpr std::uint32_t k_gpu_mapq_status_candidate_overflow = 1u;
/** @brief A candidate shader input or configuration was malformed. */
inline constexpr std::uint32_t k_gpu_mapq_status_invalid_input = 2u;
/** @brief A predecessor trace was cyclic or otherwise invalid. */
inline constexpr std::uint32_t k_gpu_mapq_status_invalid_chain = 3u;
/** @brief Query minimizers are out of oder for merging repetitive intervals. */
inline constexpr std::uint32_t k_gpu_mapq_status_repetitive_order = 4u;
/** @brief An alignment result does  not match its saved chain candidate. */
inline constexpr std::uint32_t k_gpu_mapq_status_alignment_mismatch = 5u;
/** @brief Primary/secondary classification produced an invalid relationship. */
inline constexpr std::uint32_t k_gpu_mapq_status_classification = 6u;
/** @brief The MAPQ calculation produced an invalid number. */
inline constexpr std::uint32_t k_gpu_mapq_status_numeric = 7u;
/** @brief Results are out of query or, candidate order. */
inline constexpr std::uint32_t k_gpu_mapq_status_output_order = 8u;
/** @brief Sentinel result position for candidates omitted from the output. */
inline constexpr std::uint32_t k_gpu_mapq_unpublished_ordinal = 0xffff'ffffu;
/** @brief Converts a MAPQ hash to a positive SQL bigint value */
[[nodiscard]] inline constexpr std::uint64_t
canonicalize_gpu_mapq_result_digest(const std::uint64_t raw_digest) noexcept {
    const std::uint64_t sql_safe_digest = raw_digest & ((std::uint64_t{1} << 63u) - 1u);
    return sql_safe_digest == 0u ? 1u : sql_safe_digest;
}
/** @brief Candidate record contains a valid accepted chain. */
inline constexpr std::uint32_t k_gpu_chain_candidate_flag_valid = 1u << 0u;
/** @brief Candidate survived the current primary/secondary selection pass. */
inline constexpr std::uint32_t k_gpu_chain_candidate_flag_retained = 1u << 1u;
/** @brief Candidate is a primary mapping after the current classification pass. */
inline constexpr std::uint32_t k_gpu_chain_candidate_flag_primary = 1u << 2u;
/** @brief A GPU alignment result has been matched to this candidate. */
inline constexpr std::uint32_t k_gpu_chain_candidate_flag_aligned = 1u << 3u;
/** @brief Candidate maps to the reverse strand. */
inline constexpr std::uint32_t k_gpu_chain_candidate_flag_reverse = 1u << 4u;
/** @brief Bit mask of all allowed candidate flags. */
inline constexpr std::uint32_t k_gpu_chain_candidate_flags =
    k_gpu_chain_candidate_flag_valid | k_gpu_chain_candidate_flag_retained |
    k_gpu_chain_candidate_flag_primary | k_gpu_chain_candidate_flag_aligned |
    k_gpu_chain_candidate_flag_reverse;
/**
 * @brief Calculates traceback storage for a tiled, banded alignment.
 * @param[in] query_len Query length in symbols.
 * @param[in] target_len Target length in symbols.
 * @param[in] tile_query Query symbols covered by one tile.
 * @param[in] tile_target Target symbols covered by one tile.
 * @param[in] band_width Maximum query/target coordinate difference.
 * @return Required 32-bit boundary and  scratch words, or zero for invalid
 *         dimensions or arithmetic overflow.
 *
 * Only tiles touched by the band need saved boundaries. One reusable tile
 * buffer holds the detailed traceback work.
 */
[[nodiscard]] inline constexpr std::uint64_t
gpu_alignment_cigar_gridded_traceback_words(const std::uint32_t query_len,
                                            const std::uint32_t target_len,
                                            const std::uint32_t tile_query,
                                            const std::uint32_t tile_target,
                                            const std::uint32_t band_width) noexcept {
    const std::uint32_t length_delta =
        query_len > target_len ? query_len - target_len : target_len - query_len;
    if (query_len == 0 || target_len == 0 || tile_query == 0 || tile_target == 0 ||
        length_delta > band_width) {
        return 0;
    }
    const std::uint64_t query_tiles = bioseq::common::ceil_div(query_len, tile_query);
    const std::uint64_t target_tiles = bioseq::common::ceil_div(target_len, tile_target);
    // A row spans at most tile_query + 2*band_width target cells. One extra
    // slot covers arbitrary target-tile alignment at the interval edges.
    const std::uint64_t band_cells =
        static_cast<std::uint64_t>(tile_query) + 2u * static_cast<std::uint64_t>(band_width);
    const std::uint64_t tile_slots = std::min<std::uint64_t>(
        target_tiles, bioseq::common::ceil_div(band_cells, tile_target) + 1u);
    const std::uint64_t boundary_words_per_tile =
        5u * (static_cast<std::uint64_t>(tile_query) + 1u +
              static_cast<std::uint64_t>(tile_target) + 1u);
    const auto scratch_words = bioseq::common::checked_mul(
        static_cast<std::uint64_t>(tile_query) + 1u, static_cast<std::uint64_t>(tile_target) + 1u);
    const auto occupied_tiles = bioseq::common::checked_mul(query_tiles, tile_slots);
    if (!scratch_words.has_value() || !occupied_tiles.has_value()) {
        return 0;
    }
    const auto boundary_words =
        bioseq::common::checked_mul(*occupied_tiles, boundary_words_per_tile);
    if (!boundary_words.has_value()) {
        return 0;
    }
    return bioseq::common::checked_add(*boundary_words, *scratch_words).value_or(0);
}
/** @brief A 24-byte MPS anchor record used by GPU sorting and chaining. */
struct alignas(8) gpu_anchor_wire {
    std::uint64_t ref_position;   ///< Absolute target reference coordinate in base pairs.
    std::uint64_t query_position; ///< Query coordinate in base pairs.
    std::uint32_t is_reverse;     ///< One for the reverse strand, zero for the forward strand.
    std::uint8_t query_span;      ///< Query span covered by this anchor in symbols.
    std::uint8_t flags;           ///< Flags used by the chaining kernels.
    std::uint16_t segment_id;     ///< Target segment identifier for rebasing.
};
/** @brief Packs strand and target position into the anchor sort key. */
[[nodiscard, gnu::always_inline]] inline constexpr std::uint64_t
gpu_anchor_reference_bits(const gpu_anchor_wire& anchor) noexcept {
    constexpr std::uint64_t position_mask = (std::uint64_t{1} << 63u) - 1u;
    return (static_cast<std::uint64_t>(anchor.is_reverse) << 63u) |
           (anchor.ref_position & position_mask);
}

/**
 * @brief Compares MPS anchors in the order required for chaining.
 *
 * Strand and target positxon come first. Ties use the packed segment, flags,
 * span, and low query-position bits, then the complete query position.
 */
[[nodiscard, gnu::always_inline]] inline constexpr bool
is_gpu_anchor_wire_less(const gpu_anchor_wire& left, const gpu_anchor_wire& right) noexcept {
    const std::uint64_t left_ref = gpu_anchor_reference_bits(left);
    const std::uint64_t right_ref = gpu_anchor_reference_bits(right);
    if (left_ref != right_ref)
        return left_ref < right_ref;

    const std::uint64_t left_query = (static_cast<std::uint64_t>(left.segment_id) << 48u) |
                                     (static_cast<std::uint64_t>(left.flags) << 40u) |
                                     (static_cast<std::uint64_t>(left.query_span) << 32u) |
                                     static_cast<std::uint32_t>(left.query_position);
    const std::uint64_t right_query = (static_cast<std::uint64_t>(right.segment_id) << 48u) |
                                      (static_cast<std::uint64_t>(right.flags) << 40u) |
                                      (static_cast<std::uint64_t>(right.query_span) << 32u) |
                                      static_cast<std::uint32_t>(right.query_position);
    if (left_query != right_query)
        return left_query < right_query;
    return left.query_position < right.query_position;
}

/**
 * @brief Sorts anchors with  the same target position using the chaining tie order.
 *
 * Heap sort rearranges these records in place without another anchor buffer.
 */
inline void normalize_gpu_anchor_tie_run(std::span<gpu_anchor_wire> anchors,
                                         std::uint64_t begin,
                                         std::uint64_t count) noexcept {
    const auto sift_down = [anchors, begin](std::uint64_t heap_count,
                                            std::uint64_t first_root) noexcept {
        std::uint64_t root = first_root;
        while (root < heap_count / 2u) {
            std::uint64_t child = root * 2u + 1u;
            if (child + 1u < heap_count &&
                is_gpu_anchor_wire_less(anchors[begin + child], anchors[begin + child + 1u])) {
                ++child;
            }
            if (!is_gpu_anchor_wire_less(anchors[begin + root], anchors[begin + child]))
                return;
            std::swap(anchors[begin + root], anchors[begin + child]);
            root = child;
        }
    };
    for (std::uint64_t root = count / 2u; root != 0u; --root)
        sift_down(count, root - 1u);
    for (std::uint64_t end = count; end > 1u; --end) {
        std::swap(anchors[begin], anchors[begin + end - 1u]);
        sift_down(end - 1u, 0u);
    }
}

/**
 * @brief Checks target order and sorts ties within each equal-position group.
 *
 * Returns a failed order check for invalid strand flags or decreasing target
 * keys. Ties already processed may have been reordered when a later check
 * fails.
 */
[[nodiscard]] inline seed_anchor_tie_normalization_result
normalize_gpu_anchor_equal_ref_ties(std::span<gpu_anchor_wire> anchors) noexcept {
    seed_anchor_tie_normalization_result result{
        .gpu_ref_order_checked = true,
        .gpu_ref_order_accepted = true,
    };
    std::uint64_t run_begin = 0u;
    while (run_begin < anchors.size()) {
        const gpu_anchor_wire& first = anchors[run_begin];
        if (first.is_reverse > 1u) {
            result.gpu_ref_order_accepted = false;
            return result;
        }
        const std::uint64_t run_ref = gpu_anchor_reference_bits(first);
        if (run_begin != 0u && gpu_anchor_reference_bits(anchors[run_begin - 1u]) > run_ref) {
            result.gpu_ref_order_accepted = false;
            return result;
        }
        std::uint64_t run_end = run_begin + 1u;
        while (run_end < anchors.size()) {
            const gpu_anchor_wire& next = anchors[run_end];
            if (next.is_reverse > 1u) {
                result.gpu_ref_order_accepted = false;
                return result;
            }
            const std::uint64_t next_ref = gpu_anchor_reference_bits(next);
            if (next_ref < run_ref) {
                result.gpu_ref_order_accepted = false;
                return result;
            }
            if (next_ref != run_ref)
                break;
            ++run_end;
        }
        const std::uint64_t run_count = run_end - run_begin;
        if (run_count > 1u) {
            ++result.canonical_tie_run_count;
            result.canonical_tie_anchor_count += run_count;
            normalize_gpu_anchor_tie_run(anchors, run_begin, run_count);
        }
        run_begin = run_end;
    }
    return result;
}

/**
 * @brief Checks anchor order and hashes the fields of every record
 *
 * The hash includes record positions and field values, excluding object
 * padding. Invalid strand flags or decreasing keys fail the  order check.
 */
[[nodiscard]] inline seed_anchor_canonical_order_audit
audit_gpu_anchor_canonical_order(std::span<const gpu_anchor_wire> anchors) noexcept {
    seed_anchor_canonical_order_audit audit{
        .anchor_count = anchors.size(),
        .canonical_order_checked = true,
        .canonical_order_accepted = true,
    };
    gpu_anchor_wire previous{};
    bool have_previous = false;
    for (std::uint64_t index = 0u; index < anchors.size(); ++index) {
        const gpu_anchor_wire& anchor = anchors[index];
        if (anchor.is_reverse > 1u ||
            (have_previous && is_gpu_anchor_wire_less(anchor, previous))) {
            audit.canonical_order_accepted = false;
        }
        const std::array<std::uint64_t, 6u> fields{
            anchor.ref_position,
            anchor.query_position,
            anchor.is_reverse,
            anchor.query_span,
            anchor.flags,
            anchor.segment_id,
        };
        for (std::size_t field_index = 0u; field_index < fields.size(); ++field_index) {
            const std::uint64_t tagged = seed_anchor_digest_mix(
                fields[field_index] ^ seed_anchor_digest_mix((index << 3u) | field_index));
            audit.digest_lo =
                seed_anchor_digest_mix(audit.digest_lo ^ tagged ^ 0xa4093822299f31d0ULL);
            audit.digest_hi = std::rotl(audit.digest_hi + tagged + 0x082efa98ec4e6c89ULL, 23);
            audit.digest_hi ^= seed_anchor_digest_mix(tagged ^ 0x452821e638d01377ULL);
        }
        previous = anchor;
        have_previous = true;
    }
    return audit;
}

/** @brief Stores one 8-byte chain range as `{begin, length}`. */
struct gpu_chain_range_wire {
    std::uint32_t begin;  ///< First anchor index included in the range.
    std::uint32_t length; ///< Number of consecutive anchors in the range.
};

/** @brief Query anchor bounds and output offsets for ranges and, compacted anchors. */
struct gpu_chain_range_build_span_wire {
    std::uint32_t anchor_lo; ///< Inclusive lower anchor index for the build span.
    std::uint32_t anchor_hi; ///< Exclusive upper anchor index for the build span.
    std::uint32_t range_lo;  ///< First output slot in the chain-range array.
    std::uint32_t range_out; ///< First output slot in the compacted-anchor array.
};

/**
 * @brief Coordinates and score obtained from seed chaining.
 *
 * Query and target intervals are half-open `[start, end)` base-pair ranges.
 * `is_valid` is one for a populated result and zero for an empty result.
 */
struct gpu_coordinate_result_wire {
    std::uint64_t query_start;  ///< Query interval start in base pairs.
    std::uint64_t query_end;    ///< Query interval end in base pairs.
    std::uint64_t target_start; ///< Target interval start in base pairs.
    std::uint64_t target_end;   ///< Target interval end in base pairs.
    std::uint64_t match_length; ///< Matched span estimated from the chain, in base pairs.
    std::uint64_t block_length; ///< Chained block span length in base pairs.
    std::int32_t chain_score;   ///< Score of the selected chain.
    std::int32_t seed_count;    ///< Number of seeds contributing to the chain.
    std::uint32_t is_reverse;   ///< Nonzero when the alignment is on the reverse strand.
    std::uint32_t is_valid;     ///< One when the result is populated, zero when empty.
};

/** @brief Parameters for choosing a chann and returning its coordinates. */
struct gpu_coordinate_reduce_push {
    std::uint32_t anchor_count;   ///< Number of anchors considered by the reducer.
    std::uint32_t query_len_lo;   ///< Low 32 bits of the query length bound in base pairs.
    std::uint32_t query_len_hi;   ///< High 32 bits of the query length bound in base pairs.
    std::int32_t min_chain_score; ///< Minimum chain score required to keep a candidate.
    std::int32_t min_seed_count;  ///< Minimum seed count required to keep a candidate.
    std::uint32_t probe_limit;    ///< Maximum candidate chains examined by this dispatch.
    std::uint32_t reserved;       ///< Reserved. Set to zero on upload.
};

/** @brief GPU MAPQ settings recorded with the dispatch for later  validation. */
struct gpu_persisted_mapq_config {
    std::uint32_t abi_revision = 0u;       ///< `k_gpu_persisted_mapq_abi_revision`.
    std::uint32_t candidate_capacity = 0u; ///< Maximum accepted chains. Overflow is an error.
    std::uint32_t mask_level_bits = 0u;    ///< Primary mask level as IEEE-754 bits.
    std::int32_t mask_len = 0;             ///< Maximum uncovered query span.
    std::uint32_t pri_ratio_bits = 0u;     ///< Secondary ratio as IEEE-754 bits.
    std::int32_t min_diff = 0;             ///< Allowed score deficit when keeping a secondary.
    std::int32_t sub_diff = 0;             ///< DP score difference counted as a close secondary.
    std::int32_t best_n = 0;               ///< Maximum secondaries kept across the query.
    std::int32_t match_score = 0;          ///< Positive match score in the MAPQ expression.
    std::uint32_t is_short_read = 0u;      ///< Short-read mode. This version requires zero.
    std::uint32_t is_splice = 0u;          ///< Splice mode. This version requires zero.
    std::uint32_t reserved = 0u;           ///< Must be zero.

    /// @brief Checks supported settings and finite thresholds, preserving the original float bits.
    [[nodiscard]] constexpr bool is_valid() const noexcept {
        const std::uint32_t mask_exponent = mask_level_bits & 0x7f80'0000u;
        const std::uint32_t ratio_exponent = pri_ratio_bits & 0x7f80'0000u;
        return abi_revision == k_gpu_persisted_mapq_abi_revision &&
               candidate_capacity == k_gpu_persisted_mapq_candidate_capacity &&
               mask_exponent != 0x7f80'0000u && ratio_exponent != 0x7f80'0000u &&
               std::bit_cast<float>(mask_level_bits) >= 0.0f &&
               std::bit_cast<float>(pri_ratio_bits) > 0.0f && mask_len >= 0 && min_diff >= 0 &&
               sub_diff >= 0 && best_n >= 0 && match_score > 0 && is_short_read == 0u &&
               is_splice == 0u && reserved == 0u;
    }

    /// @brief Compares every field, including the exact float bit patterns.
    [[nodiscard]] constexpr bool operator==(const gpu_persisted_mapq_config&) const = default;
};

/** @brief Per-query bounds and status for saved chain candidates. */
struct gpu_chain_candidate_header_wire {
    std::uint32_t candidate_offset = 0u;   ///< First record in the operation candidate array.
    std::uint32_t candidate_count = 0u;    ///< Accepted chains represented by records.
    std::uint32_t retained_count = 0u;     ///< Candidates kept by the current classification pass
    std::uint32_t primary_count = 0u;      ///< Primary candidates after current classification.
    std::uint32_t alignment_offset = 0u;   ///< First  entry linking this query to its alignment results.
    std::uint32_t alignment_count = 0u;    ///< Number of alignment-result bindings for this query.
    std::uint32_t repetitive_span = 0u;    ///< Exact union length of filtered query intervals.
    std::uint32_t status = 0u;             ///< `k_gpu_mapq_status_*`.
    std::uint32_t candidate_capacity = 0u; ///< Candidate limit recorded for this dispatch.
    std::uint32_t reserved = 0u;           ///< Must be zero.
};

/** @brief An accepted chain and the scores used to classify it and calculate MAPQ. */
struct gpu_chain_candidate_wire {
    std::uint64_t query_start = 0u;         ///< Query intekval start, inclusive.
    std::uint64_t query_end = 0u;           ///< Query interval end, exclusive.
    std::uint64_t target_start = 0u;        ///< Target interval start, inclusive.
    std::uint64_t target_end = 0u;          ///< Target interval end, exclusive.
    std::uint64_t match_length = 0u;        ///< Chained or aligned match length.
    std::uint64_t block_length = 0u;        ///< Chained or aligned block length.
    std::int32_t chain_score = 0;           ///< Current chain score.
    std::int32_t original_chain_score = 0;  ///< Chain score saved before classification (`score0`).
    std::int32_t seed_count = 0;            ///< Anchors contributing to this chain.
    std::int32_t parent_index = -1;         ///< Parent candidate index within this query. Primaries point  to themselves.
    std::int32_t secondary_chain_score = 0; ///< Best secondary score (`subsc`).
    std::int32_t secondary_count = 0;       ///< Close-secondary count (`n_sub`).
    std::int32_t dp_score = 0;              ///< Alignment score returned by the GPU.
    std::int32_t dp_max_score = 0;          ///< GPU alignment, score used for MAPQ ranking and calculation.
    std::int32_t secondary_dp_max = 0;      ///< Best qualifying secondary DP maximum.
    std::uint32_t candidate_index = 0u;     ///< Stable query-local candidate index.
    std::uint32_t problem_index = 0u;       ///< Input query index.
    std::uint32_t flags = 0u;               ///< `k_gpu_chain_candidate_flag_*`.
};

/** @brief Links a CIGAR job to its saved chain candidate. */
struct gpu_alignment_candidate_binding_wire {
    std::uint32_t problem_index = 0u;   ///< Input query index.
    std::uint32_t candidate_index = 0u; ///< Candidate index within the query.
};

/** @brief Mapping quality and output position for one chain candidate. */
struct gpu_mapq_result_wire {
    std::uint32_t problem_index = 0u;   ///< Input query index.
    std::uint32_t candidate_index = 0u; ///< Candidate index within the query.
    std::int32_t mapq = 0;              ///< GPU mapping quality in `[0, 60]`.
    std::uint32_t status = 0u;          ///< `k_gpu_mapq_status_*`.
    std::uint32_t is_primary = 0u;      ///< One only for a post-alignment primary.
    std::uint32_t publish = 0u;         ///< One when this  candidate belongs in the returned results.
    std::uint32_t publication_ordinal = ///< Position among this query's results, ordered by alignment score.
        k_gpu_mapq_unpublished_ordinal;
    std::uint32_t reserved1 = 0u; ///< Must be zero.
};

/** @brief Shader parameters for collecting chains and classifying them before alignment. */
struct gpu_chain_candidate_reduce_push {
    std::uint32_t anchor_count = 0u;
    std::uint32_t query_len_lo = 0u;
    std::uint32_t query_len_hi = 0u;
    std::int32_t min_chain_score = 0;
    std::int32_t min_seed_count = 0;
    std::uint32_t candidate_capacity = 0u;
    std::uint32_t problem_index = 0u;
    std::uint32_t minimizer_offset = 0u;
    std::uint32_t minimizer_count = 0u;
    std::uint32_t minimizer_words32_per_record = 0u;
    std::uint32_t minimizer_word_kind = 0u;
    std::uint32_t pos_bits = 0u;
    std::uint32_t kmer_size = 0u;
    std::uint32_t mask_level_bits = 0u;
    std::int32_t mask_len = 0;
    std::uint32_t pri_ratio_bits = 0u;
    std::int32_t min_diff = 0;
    std::int32_t best_n = 0;
    std::uint32_t repetitive_flag_extent = 0u;
    std::uint32_t reserved = 0u;
};

/** @brief Shader parameters for classifying aligned candidates and calculating MAPQ. */
struct gpu_mapq_reduce_push {
    std::uint32_t problem_count = 0u;
    std::uint32_t candidate_count = 0u;
    std::uint32_t alignment_count = 0u;
    std::int32_t min_chain_score = 0;
    std::int32_t match_score = 0;
    std::uint32_t mask_level_bits = 0u;
    std::int32_t mask_len = 0;
    std::int32_t sub_diff = 0;
    std::uint32_t pri_ratio_bits = 0u;
    std::int32_t min_diff = 0;
    std::int32_t best_n = 0;
    std::uint32_t candidate_capacity = 0u;
    std::uint32_t is_short_read = 0u;
    std::uint32_t is_splice = 0u;
    std::uint32_t reserved = 0u;
};

/**
 * @brief An affine-gap alignment job that returns a score and endpoint.
 *
 * Input buffers contain alphabet ranks, one 32-bit wrd per symbol
 */
struct gpu_alignment_extension_job_wire {
    std::uint32_t query_offset;  ///< Query rank stream offset in 32-bit symbols.
    std::uint32_t target_offset; ///< Target rank stream offset in 32-bit symbols.
    std::uint32_t query_len;     ///< Query span length in symbols.
    std::uint32_t target_len;    ///< Target span length in symbols.
    std::uint32_t band_width;    ///< Maximum distance from the diagonal for this job.
    std::uint32_t flags;         ///< Reserved flags. Set to zero on upload.
    std::uint32_t reserved0;     ///< Reserved. Set to zero  on upload.
    std::uint32_t reserved1;     ///< Reserved. Set to zero on upload.
};

/** @brief Stores the score and best endpoint of one extension job. */
struct gpu_alignment_extension_result_wire {
    std::int32_t score;       ///< Best extension score produced by the kernel.
    std::uint32_t query_end;  ///< Best query endpoint in local symbols.
    std::uint32_t target_end; ///< Best target endpoint in local symbols.
    std::uint32_t status;     ///< Extension status code (`k_gpu_alignment_extension_status_*`).
};

/** @brief Length, band, and scoring limits shared by an extension batch. */
struct gpu_alignment_extension_params {
    std::uint32_t max_query_len = 0;     ///< Maximum query length in symbols for the batch.
    std::uint32_t max_target_len = 0;    ///< Maximum target length in symbols for the batch.
    std::uint32_t max_band_width = 0;    ///< Maximum band width admitted for any job.
    std::uint32_t alphabet_size = 0;     ///< Rank alphabet size used to validate symbols.
    std::int32_t match_score = 0;        ///< Match score applied by the extension kernel.
    std::int32_t mismatch_penalty = 0;   ///< Mismatch penalty applied by the extension kernel.
    std::int32_t gap_open_penalty = 0;   ///< Gap-open penalty applied by the extension kernel.
    std::int32_t gap_extend_penalty = 0; ///< Gap-extend penalty applied by the extension kernel.
    std::uint32_t flags = 0;             ///< Reserved flags. Set to zero on upload.
    std::uint32_t reserved0 = 0;         ///< Reserved. Set to zero on upload.
};

/** @brief An alignment job with space reserved for CIGAR output and traceback. */
struct gpu_alignment_cigar_job_wire {
    std::uint32_t query_symbol_offset = 0;     ///< Rank offset, or packed byte offset when flagged.
    std::uint32_t target_symbol_offset = 0;    ///< Rank offset, or packed byte offset when flagged.
    std::uint32_t query_len = 0;               ///< Query span length in symbols.
    std::uint32_t target_len = 0;              ///< Target span length in symbols.
    std::uint32_t query_global_start = 0;      ///< Query global start added to local coordinates.
    std::uint32_t target_global_start = 0;     ///< Target global start added to local coordinates.
    std::uint32_t band_width = 0;              ///< Maximum distance from the diagonal for this job.
    std::uint32_t flags = 0;                   ///< `k_gpu_alignment_cigar_job_flags` bits for packed input and target orientation.
    std::uint32_t cigar_offset = 0;            ///< Output CIGAR array offset in operations.
    std::uint32_t cigar_capacity = 0;          ///< Maximum CIGAR operations writable for this job.
    std::uint32_t traceback_tile_offset = 0;   ///< Offset in the traceback buffer, in 32-bit words.
    std::uint32_t traceback_tile_count = 0;    ///< Number of 32-bit traceback words reserved for this job.
    std::uint32_t query_packed_byte_count = 0; ///< Packed query bytes available to the shader.
    std::uint32_t target_packed_byte_count = 0;    ///< Packed target bytes available to the shader.
    std::uint32_t query_leading_symbol_offset = 0; ///< Symbols skipped in first query unit.
    std::uint32_t target_leading_symbol_offset = 0; ///< Symbols skipped in first target unit.
    std::uint32_t query_storage_unit_width = 0;     ///< Query packed-unit width in bytes.
    std::uint32_t target_storage_unit_width = 0;    ///< Target packed-unit width in bytes.
    std::uint32_t bits_per_symbol = 0;              ///< Number of bits used for each packed symbol  rank.
    std::uint32_t problem_index = 0;                ///< Query job index used, to check the matching coordinate result.
};

/** @brief Alignment coordinates and score, with the location of its CIGAR output. */
struct gpu_alignment_cigar_result_wire {
    std::int32_t score = 0;         ///< Reported alignment score for the job.
    std::int32_t max_score = 0;     ///< Equal to `score` in the current CIGAR shader.
    std::uint32_t query_start = 0;  ///< Global query interval start, inclusive, in symbols.
    std::uint32_t query_end = 0;    ///< Global query interval end, exclusive, in symbols.
    std::uint32_t target_start = 0; ///< Global target interval start, inclusive, in symbols.
    std::uint32_t target_end = 0;   ///< Global target interval end, exclusive, in symbols.
    std::uint32_t cigar_offset = 0; ///< CIGAR array offset written for this job.
    std::uint32_t cigar_length = 0; ///< Number of CIGAR operations written.
    std::uint32_t status = 0;       ///< CIGAR status code (`k_gpu_alignment_extension_status_*`).
    std::uint32_t reserved = 0;     ///< Reserved. Must be zero in the returned result
    std::uint32_t num_matches = 0;  ///< Number of aligned positions witc equal  query and target ranks.
    std::uint32_t block_length = 0; ///< Total aligned columns represented by the GPU CIGAR.
};

/** @brief Scoring settings and tile sizes for GPU CIGAR alignment. */
struct gpu_alignment_cigar_params {
    std::uint32_t alphabet_size = 0;   ///< Rank alphabet size used to validate symbols.
    std::uint32_t max_band_width = 0;  ///< Maximum band width admitted for any job.
    std::uint32_t tile_query = 0;      ///< Query symbols covered by one traceback tile.
    std::uint32_t tile_target = 0;     ///< Target symbols covered by one traceback tile.
    std::int32_t match_score = 0;      ///< Match score applied by the CIGAR kernel.
    std::int32_t mismatch_penalty = 0; ///< Mismatch penalty applied by the CIGAR kernel.
    std::int32_t gap_open_short = 0;   ///< Short gap-open penalty.
    std::int32_t gap_extend_short = 0; ///< Short gap-extend penalty.
    std::int32_t gap_open_long = 0;    ///< Long gap-open penalty.
    std::int32_t gap_extend_long = 0;  ///< Long gap-extend penalty.
    std::int32_t z_drop = 0;           ///< Z-drop setting. The current CIGAR shader does not use it.
    std::int32_t end_bonus = 0;        ///< End-bonus setting. The current CIGAR shader does not use it.
    std::uint32_t flags = 0;           ///< Reserved flags. Set to zero on upload.
    std::uint32_t reserved = 0;        ///< Reserved. Set to zero on upload.
    /** @brief Eight little-endian words mapping up to 32 alphabet ranks to complement ranks. */
    std::array<std::uint32_t, 8> complement_rank_words{};
};
static_assert(std::is_standard_layout_v<gpu_anchor_wire>);
static_assert(std::is_trivially_copyable_v<gpu_anchor_wire>);
static_assert(std::endian::native == std::endian::little);
static_assert(sizeof(gpu_anchor_wire) == 24);
static_assert(alignof(gpu_anchor_wire) == 8);
static_assert(offsetof(gpu_anchor_wire, ref_position) == 0);
static_assert(offsetof(gpu_anchor_wire, query_position) == 8);
static_assert(offsetof(gpu_anchor_wire, is_reverse) == 16);
static_assert(offsetof(gpu_anchor_wire, query_span) == 20);
static_assert(offsetof(gpu_anchor_wire, flags) == 21);
static_assert(offsetof(gpu_anchor_wire, segment_id) == 22);
static_assert(sizeof(gpu_chain_range_wire) == 8);
static_assert(std::is_standard_layout_v<gpu_chain_range_build_span_wire>);
static_assert(sizeof(gpu_chain_range_build_span_wire) == 16);
static_assert(std::is_standard_layout_v<gpu_coordinate_result_wire>);
static_assert(sizeof(gpu_coordinate_result_wire) == 64);
static_assert(offsetof(gpu_coordinate_result_wire, query_start) == 0);
static_assert(offsetof(gpu_coordinate_result_wire, target_start) == 16);
static_assert(offsetof(gpu_coordinate_result_wire, chain_score) == 48);
static_assert(offsetof(gpu_coordinate_result_wire, is_valid) == 60);
static_assert(std::is_standard_layout_v<gpu_coordinate_reduce_push>);
static_assert(sizeof(gpu_coordinate_reduce_push) == 28);
static_assert(std::is_standard_layout_v<gpu_persisted_mapq_config>);
static_assert(sizeof(gpu_persisted_mapq_config) == 48u);
static_assert(!gpu_persisted_mapq_config{}.is_valid());
static_assert(std::is_standard_layout_v<gpu_chain_candidate_header_wire>);
static_assert(sizeof(gpu_chain_candidate_header_wire) == 40u);
static_assert(offsetof(gpu_chain_candidate_header_wire, alignment_offset) == 16u);
static_assert(offsetof(gpu_chain_candidate_header_wire, repetitive_span) == 24u);
static_assert(std::is_standard_layout_v<gpu_chain_candidate_wire>);
static_assert(sizeof(gpu_chain_candidate_wire) == 96u);
static_assert(offsetof(gpu_chain_candidate_wire, chain_score) == 48u);
static_assert(offsetof(gpu_chain_candidate_wire, secondary_dp_max) == 80u);
static_assert(offsetof(gpu_chain_candidate_wire, flags) == 92u);
static_assert(std::is_standard_layout_v<gpu_alignment_candidate_binding_wire>);
static_assert(sizeof(gpu_alignment_candidate_binding_wire) == 8u);
static_assert(std::is_standard_layout_v<gpu_mapq_result_wire>);
static_assert(sizeof(gpu_mapq_result_wire) == 32u);
static_assert(offsetof(gpu_mapq_result_wire, mapq) == 8u);
static_assert(offsetof(gpu_mapq_result_wire, publish) == 20u);
static_assert(offsetof(gpu_mapq_result_wire, publication_ordinal) == 24u);
static_assert(std::is_standard_layout_v<gpu_chain_candidate_reduce_push>);
static_assert(sizeof(gpu_chain_candidate_reduce_push) == 80u);
static_assert(offsetof(gpu_chain_candidate_reduce_push, mask_level_bits) == 52u);
static_assert(offsetof(gpu_chain_candidate_reduce_push, repetitive_flag_extent) == 72u);
static_assert(std::is_standard_layout_v<gpu_mapq_reduce_push>);
static_assert(sizeof(gpu_mapq_reduce_push) == 60u);
static_assert(offsetof(gpu_mapq_reduce_push, mask_level_bits) == 20u);
static_assert(offsetof(gpu_mapq_reduce_push, reserved) == 56u);
static_assert(std::is_standard_layout_v<gpu_alignment_extension_job_wire>);
static_assert(sizeof(gpu_alignment_extension_job_wire) == 32);
static_assert(offsetof(gpu_alignment_extension_job_wire, query_offset) == 0);
static_assert(offsetof(gpu_alignment_extension_job_wire, target_offset) == 4);
static_assert(offsetof(gpu_alignment_extension_job_wire, query_len) == 8);
static_assert(offsetof(gpu_alignment_extension_job_wire, target_len) == 12);
static_assert(offsetof(gpu_alignment_extension_job_wire, band_width) == 16);
static_assert(offsetof(gpu_alignment_extension_job_wire, flags) == 20);
static_assert(offsetof(gpu_alignment_extension_job_wire, reserved0) == 24);
static_assert(offsetof(gpu_alignment_extension_job_wire, reserved1) == 28);
static_assert(std::is_standard_layout_v<gpu_alignment_extension_result_wire>);
static_assert(sizeof(gpu_alignment_extension_result_wire) == 16);
static_assert(offsetof(gpu_alignment_extension_result_wire, score) == 0);
static_assert(offsetof(gpu_alignment_extension_result_wire, query_end) == 4);
static_assert(offsetof(gpu_alignment_extension_result_wire, target_end) == 8);
static_assert(offsetof(gpu_alignment_extension_result_wire, status) == 12);
static_assert(std::is_standard_layout_v<gpu_alignment_extension_params>);
static_assert(sizeof(gpu_alignment_extension_params) == 40);
static_assert(std::is_standard_layout_v<gpu_alignment_cigar_job_wire>);
static_assert(sizeof(gpu_alignment_cigar_job_wire) == 80);
static_assert(offsetof(gpu_alignment_cigar_job_wire, query_symbol_offset) == 0);
static_assert(offsetof(gpu_alignment_cigar_job_wire, target_symbol_offset) == 4);
static_assert(offsetof(gpu_alignment_cigar_job_wire, query_len) == 8);
static_assert(offsetof(gpu_alignment_cigar_job_wire, target_len) == 12);
static_assert(offsetof(gpu_alignment_cigar_job_wire, query_global_start) == 16);
static_assert(offsetof(gpu_alignment_cigar_job_wire, target_global_start) == 20);
static_assert(offsetof(gpu_alignment_cigar_job_wire, band_width) == 24);
static_assert(offsetof(gpu_alignment_cigar_job_wire, flags) == 28);
static_assert(offsetof(gpu_alignment_cigar_job_wire, cigar_offset) == 32);
static_assert(offsetof(gpu_alignment_cigar_job_wire, cigar_capacity) == 36);
static_assert(offsetof(gpu_alignment_cigar_job_wire, traceback_tile_offset) == 40);
static_assert(offsetof(gpu_alignment_cigar_job_wire, traceback_tile_count) == 44);
static_assert(offsetof(gpu_alignment_cigar_job_wire, query_packed_byte_count) == 48);
static_assert(offsetof(gpu_alignment_cigar_job_wire, target_packed_byte_count) == 52);
static_assert(offsetof(gpu_alignment_cigar_job_wire, query_leading_symbol_offset) == 56);
static_assert(offsetof(gpu_alignment_cigar_job_wire, target_leading_symbol_offset) == 60);
static_assert(offsetof(gpu_alignment_cigar_job_wire, query_storage_unit_width) == 64);
static_assert(offsetof(gpu_alignment_cigar_job_wire, target_storage_unit_width) == 68);
static_assert(offsetof(gpu_alignment_cigar_job_wire, bits_per_symbol) == 72);
static_assert(offsetof(gpu_alignment_cigar_job_wire, problem_index) == 76);
static_assert(std::is_standard_layout_v<gpu_alignment_cigar_result_wire>);
static_assert(sizeof(gpu_alignment_cigar_result_wire) == 48);
static_assert(offsetof(gpu_alignment_cigar_result_wire, score) == 0);
static_assert(offsetof(gpu_alignment_cigar_result_wire, max_score) == 4);
static_assert(offsetof(gpu_alignment_cigar_result_wire, query_start) == 8);
static_assert(offsetof(gpu_alignment_cigar_result_wire, query_end) == 12);
static_assert(offsetof(gpu_alignment_cigar_result_wire, target_start) == 16);
static_assert(offsetof(gpu_alignment_cigar_result_wire, target_end) == 20);
static_assert(offsetof(gpu_alignment_cigar_result_wire, cigar_offset) == 24);
static_assert(offsetof(gpu_alignment_cigar_result_wire, cigar_length) == 28);
static_assert(offsetof(gpu_alignment_cigar_result_wire, status) == 32);
static_assert(offsetof(gpu_alignment_cigar_result_wire, reserved) == 36);
static_assert(offsetof(gpu_alignment_cigar_result_wire, num_matches) == 40);
static_assert(offsetof(gpu_alignment_cigar_result_wire, block_length) == 44);
static_assert(std::is_standard_layout_v<gpu_alignment_cigar_params>);
static_assert(sizeof(gpu_alignment_cigar_params) == 88);
static_assert(offsetof(gpu_alignment_cigar_params, complement_rank_words) == 56);

} // namespace bioseq::gpu::vulkan
