/**
 * @file vulkan/abi/alignment.cppm
 * @brief Defines Vulkan chaining, coordinate, extension, and CIGAR ABI records.
 *
 * Wire structs and push layouts are size- and offset-pinned by static assertions
 * so host validation and shader reflection share one layout contract.
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
/** @brief Maximum band width admitted by the extension kernel. */
inline constexpr std::uint32_t k_gpu_alignment_extension_max_band_width = 512u;
/** @brief Shader status indicating successful alignment extension. */
inline constexpr std::uint32_t k_gpu_alignment_extension_status_ok = 0u;
/** @brief Shader status indicating unsupported alignment geometry. */
inline constexpr std::uint32_t k_gpu_alignment_extension_status_unsupported_geometry = 1u;
/** @brief Shader status indicating insufficient CIGAR output capacity. */
inline constexpr std::uint32_t k_gpu_alignment_extension_status_cigar_overflow = 2u;
/** @brief Job payload contains native packed sequence units, not expanded rank words. */
inline constexpr std::uint32_t k_gpu_alignment_cigar_job_flag_packed_inputs = 1u << 0u;
/** @brief Device must reverse-complement the target packed window before alignment. */
inline constexpr std::uint32_t k_gpu_alignment_cigar_job_flag_reverse_target = 1u << 1u;
/** @brief Complete set of accepted CIGAR job flags. */
inline constexpr std::uint32_t k_gpu_alignment_cigar_job_flags =
    k_gpu_alignment_cigar_job_flag_packed_inputs | k_gpu_alignment_cigar_job_flag_reverse_target;
/** @brief Transient candidate/MAPQ ABI revision; persisted grouped-index ABI is unchanged. */
inline constexpr std::uint32_t k_gpu_persisted_mapq_abi_revision = 1u;
/** @brief Fail-closed candidate capacity compiled into the first full-Vulkan MAPQ slice. */
inline constexpr std::uint32_t k_gpu_persisted_mapq_candidate_capacity = 64u;
/** @brief Candidate/result status indicating a successful physical GPU operation. */
inline constexpr std::uint32_t k_gpu_mapq_status_ok = 0u;
/** @brief More accepted chains existed than the admitted candidate capacity. */
inline constexpr std::uint32_t k_gpu_mapq_status_candidate_overflow = 1u;
/** @brief A candidate shader input or configuration was malformed. */
inline constexpr std::uint32_t k_gpu_mapq_status_invalid_input = 2u;
/** @brief A predecessor trace was cyclic or otherwise invalid. */
inline constexpr std::uint32_t k_gpu_mapq_status_invalid_chain = 3u;
/** @brief Query minimizers were not in the order required for repetitive-span union. */
inline constexpr std::uint32_t k_gpu_mapq_status_repetitive_order = 4u;
/** @brief Retained candidate and physical GPU alignment evidence disagreed. */
inline constexpr std::uint32_t k_gpu_mapq_status_alignment_mismatch = 5u;
/** @brief Primary/secondary classification produced an invalid relationship. */
inline constexpr std::uint32_t k_gpu_mapq_status_classification = 6u;
/** @brief The selected Vulkan MAPQ expression produced an invalid numeric value. */
inline constexpr std::uint32_t k_gpu_mapq_status_numeric = 7u;
/** @brief Device output was not in the authenticated problem/candidate order. */
inline constexpr std::uint32_t k_gpu_mapq_status_output_order = 8u;
/** @brief Publication ordinal used by candidates omitted from the final result set. */
inline constexpr std::uint32_t k_gpu_mapq_unpublished_ordinal = 0xffff'ffffu;
/** @brief Canonicalizes a raw MAPQ digest as a nonzero SQL bigint value. */
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
/** @brief Candidate has a causally matched physical GPU alignment result. */
inline constexpr std::uint32_t k_gpu_chain_candidate_flag_aligned = 1u << 3u;
/** @brief Candidate maps to the reverse strand. */
inline constexpr std::uint32_t k_gpu_chain_candidate_flag_reverse = 1u << 4u;
/** @brief Complete accepted candidate-flag mask. */
inline constexpr std::uint32_t k_gpu_chain_candidate_flags =
    k_gpu_chain_candidate_flag_valid | k_gpu_chain_candidate_flag_retained |
    k_gpu_chain_candidate_flag_primary | k_gpu_chain_candidate_flag_aligned |
    k_gpu_chain_candidate_flag_reverse;
/**
 * @brief Computes traceback words for a gridded alignment job.
 * @param[in] query_len Query length in symbols.
 * @param[in] target_len Target length in symbols.
 * @param[in] tile_query Query symbols covered by one traceback tile.
 * @param[in] tile_target Target symbols covered by one traceback tile.
 * @param[in] band_width Maximum absolute query/target coordinate delta.
 * @return Required boundary and scratch words, or zero for invalid geometry.
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
/** @brief Distinct 24-byte MPS anchor wire emitted and sorted on the device. */
struct alignas(8) gpu_anchor_wire {
    std::uint64_t ref_position;   ///< Absolute target reference coordinate in base pairs.
    std::uint64_t query_position; ///< Query coordinate in base pairs.
    std::uint32_t is_reverse;     ///< Nonzero when the anchor lies on the reverse strand.
    std::uint8_t query_span;      ///< Query span covered by this anchor in symbols.
    std::uint8_t flags;           ///< Anchor routing flags consumed by chain kernels.
    std::uint16_t segment_id;     ///< Target segment identifier for rebasing.
};
/** @brief Encodes the MPS anchor strand and reference position for radix order. */
[[nodiscard, gnu::always_inline]] inline constexpr std::uint64_t
gpu_anchor_reference_bits(const gpu_anchor_wire& anchor) noexcept {
    constexpr std::uint64_t position_mask = (std::uint64_t{1} << 63u) - 1u;
    return (static_cast<std::uint64_t>(anchor.is_reverse) << 63u) |
           (anchor.ref_position & position_mask);
}

/** @brief Compares distinct MPS wires using the canonical chaining order. */
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

/** @brief Heap-sorts one equal-reference MPS tie run without object-layout conversion. */
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

/** @brief Normalizes equal-reference ties in completed MPS wire storage. */
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

/** @brief Validates and digests explicit fields of completed MPS anchor wires. */
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

/** @brief Stores one query span consumed by the device hard-range builder. */
struct gpu_chain_range_build_span_wire {
    std::uint32_t anchor_lo; ///< Inclusive lower anchor index for the build span.
    std::uint32_t anchor_hi; ///< Exclusive upper anchor index for the build span.
    std::uint32_t range_lo;  ///< Input chain-range index selected for compaction.
    std::uint32_t range_out; ///< Output chain-range slot written by the builder.
};

/**
 * @brief Stores a compact coordinate-only seed-and-chain result.
 *
 * Coordinates are half-open `[start, end)` intervals in query and target base
 * pairs. `is_valid` is one for a populated result and zero for the canonical
 * empty wire value.
 */
struct gpu_coordinate_result_wire {
    std::uint64_t query_start;  ///< Query interval start in base pairs.
    std::uint64_t query_end;    ///< Query interval end in base pairs.
    std::uint64_t target_start; ///< Target interval start in base pairs.
    std::uint64_t target_end;   ///< Target interval end in base pairs.
    std::uint64_t match_length; ///< Matched span length in base pairs.
    std::uint64_t block_length; ///< Chained block span length in base pairs.
    std::int32_t chain_score;   ///< Aggregate chain score for the result.
    std::int32_t seed_count;    ///< Number of seeds contributing to the chain.
    std::uint32_t is_reverse;   ///< Nonzero when the alignment is on the reverse strand.
    std::uint32_t is_valid;     ///< One when the result is populated, zero when empty.
};

/** @brief Carries coordinate-only chain-reduction parameters. */
struct gpu_coordinate_reduce_push {
    std::uint32_t anchor_count;   ///< Number of anchors considered by the reducer.
    std::uint32_t query_len_lo;   ///< Low 32 bits of the query length bound in base pairs.
    std::uint32_t query_len_hi;   ///< High 32 bits of the query length bound in base pairs.
    std::int32_t min_chain_score; ///< Minimum chain score required to keep a candidate.
    std::int32_t min_seed_count;  ///< Minimum seed count required to keep a candidate.
    std::uint32_t probe_limit;    ///< Upper bound on reducer probe steps per dispatch.
    std::uint32_t reserved;       ///< Reserved for ABI growth, must be zero on upload.
};

/** @brief Transient full-Vulkan MAPQ configuration bound exactly into receipts. */
struct gpu_persisted_mapq_config {
    std::uint32_t abi_revision = 0u;       ///< `k_gpu_persisted_mapq_abi_revision`.
    std::uint32_t candidate_capacity = 0u; ///< Fail-closed accepted-chain capacity.
    std::uint32_t mask_level_bits = 0u;    ///< Primary mask level as IEEE-754 bits.
    std::int32_t mask_len = 0;             ///< Maximum uncovered query span.
    std::uint32_t pri_ratio_bits = 0u;     ///< Secondary ratio as IEEE-754 bits.
    std::int32_t min_diff = 0;             ///< Minimum primary/secondary score difference.
    std::int32_t sub_diff = 0;             ///< DP score difference counted as a close secondary.
    std::int32_t best_n = 0;               ///< Maximum retained secondaries across the query.
    std::int32_t match_score = 0;          ///< Positive match score in the MAPQ expression.
    std::uint32_t is_short_read = 0u;      ///< Short-read mode; unsupported by this ABI revision.
    std::uint32_t is_splice = 0u;          ///< Splice mode; unsupported by this ABI revision.
    std::uint32_t reserved = 0u;           ///< Must be zero.

    /// @brief Validates every dispatch-affecting field without normalizing float bits.
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

    /// @brief Compares the complete bit-exact MAPQ configuration identity.
    [[nodiscard]] constexpr bool operator==(const gpu_persisted_mapq_config&) const = default;
};

/** @brief Per-query bounds and status for retained chain candidates. */
struct gpu_chain_candidate_header_wire {
    std::uint32_t candidate_offset = 0u;   ///< First record in the operation candidate array.
    std::uint32_t candidate_count = 0u;    ///< Accepted chains represented by records.
    std::uint32_t retained_count = 0u;     ///< Candidates selected for physical alignment.
    std::uint32_t primary_count = 0u;      ///< Primary candidates after current classification.
    std::uint32_t alignment_offset = 0u;   ///< First physical alignment binding for this query.
    std::uint32_t alignment_count = 0u;    ///< Physical alignment bindings for this query.
    std::uint32_t repetitive_span = 0u;    ///< Exact union length of filtered query intervals.
    std::uint32_t status = 0u;             ///< `k_gpu_mapq_status_*`.
    std::uint32_t candidate_capacity = 0u; ///< Capacity authenticated by the dispatch.
    std::uint32_t reserved = 0u;           ///< Must be zero.
};

/** @brief One accepted chain plus pre/post-alignment MAPQ evidence. */
struct gpu_chain_candidate_wire {
    std::uint64_t query_start = 0u;         ///< Half-open query start.
    std::uint64_t query_end = 0u;           ///< Half-open query end.
    std::uint64_t target_start = 0u;        ///< Half-open target start.
    std::uint64_t target_end = 0u;          ///< Half-open target end.
    std::uint64_t match_length = 0u;        ///< Chained or aligned match length.
    std::uint64_t block_length = 0u;        ///< Chained or aligned block length.
    std::int32_t chain_score = 0;           ///< Current chain score.
    std::int32_t original_chain_score = 0;  ///< Immutable preclassification score (`score0` role).
    std::int32_t seed_count = 0;            ///< Anchors contributing to this chain.
    std::int32_t parent_index = -1;         ///< Stable query-local parent candidate index.
    std::int32_t secondary_chain_score = 0; ///< Best secondary score (`subsc`).
    std::int32_t secondary_count = 0;       ///< Close-secondary count (`n_sub`).
    std::int32_t dp_score = 0;              ///< Physical GPU alignment score.
    std::int32_t dp_max_score = 0;          ///< Physical GPU maximum DP score.
    std::int32_t secondary_dp_max = 0;      ///< Best qualifying secondary DP maximum.
    std::uint32_t candidate_index = 0u;     ///< Stable query-local candidate index.
    std::uint32_t problem_index = 0u;       ///< Input query/problem ordinal.
    std::uint32_t flags = 0u;               ///< `k_gpu_chain_candidate_flag_*`.
};

/** @brief Exact binding between one CIGAR job and its retained chain candidate. */
struct gpu_alignment_candidate_binding_wire {
    std::uint32_t problem_index = 0u;   ///< Input query/problem ordinal.
    std::uint32_t candidate_index = 0u; ///< Stable query-local candidate ordinal.
};

/** @brief Ordered MAPQ publication decision for one chain candidate. */
struct gpu_mapq_result_wire {
    std::uint32_t problem_index = 0u;   ///< Input query/problem ordinal.
    std::uint32_t candidate_index = 0u; ///< Stable query-local candidate ordinal.
    std::int32_t mapq = 0;              ///< Deterministic Vulkan mapping quality in `[0,60]`.
    std::uint32_t status = 0u;          ///< `k_gpu_mapq_status_*`.
    std::uint32_t is_primary = 0u;      ///< One only for a post-alignment primary.
    std::uint32_t publish = 0u;         ///< One when the production result must be published.
    std::uint32_t publication_ordinal = ///< DP-ranked query-local publication order.
        k_gpu_mapq_unpublished_ordinal;
    std::uint32_t reserved1 = 0u; ///< Must be zero.
};

/** @brief Push block for complete accepted-chain enumeration and preclassification. */
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

/** @brief Push block for post-alignment classification and physical Vulkan MAPQ. */
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
 * @brief Describes one score-and-endpoint-only affine extension job.
 *
 * Offsets address caller-normalized alphabet-rank streams.
 */
struct gpu_alignment_extension_job_wire {
    std::uint32_t query_offset;  ///< Query rank stream offset in 32-bit symbols.
    std::uint32_t target_offset; ///< Target rank stream offset in 32-bit symbols.
    std::uint32_t query_len;     ///< Query span length in symbols.
    std::uint32_t target_len;    ///< Target span length in symbols.
    std::uint32_t band_width;    ///< Band width admitted for this job.
    std::uint32_t flags;         ///< Reserved routing flags, must be zero on upload.
    std::uint32_t reserved0;     ///< Reserved for ABI growth, must be zero on upload.
    std::uint32_t reserved1;     ///< Reserved for ABI growth, must be zero on upload.
};

/** @brief Stores the score and best endpoint of one extension job. */
struct gpu_alignment_extension_result_wire {
    std::int32_t score;       ///< Best extension score produced by the kernel.
    std::uint32_t query_end;  ///< Best query endpoint in local symbols.
    std::uint32_t target_end; ///< Best target endpoint in local symbols.
    std::uint32_t status;     ///< Extension status code (`k_gpu_alignment_extension_status_*`).
};

/** @brief Defines geometry and scoring limits for an extension batch. */
struct gpu_alignment_extension_params {
    std::uint32_t max_query_len = 0;     ///< Maximum query length in symbols for the batch.
    std::uint32_t max_target_len = 0;    ///< Maximum target length in symbols for the batch.
    std::uint32_t max_band_width = 0;    ///< Maximum band width admitted for any job.
    std::uint32_t alphabet_size = 0;     ///< Rank alphabet size used to validate symbols.
    std::int32_t match_score = 0;        ///< Match score applied by the extension kernel.
    std::int32_t mismatch_penalty = 0;   ///< Mismatch penalty applied by the extension kernel.
    std::int32_t gap_open_penalty = 0;   ///< Gap-open penalty applied by the extension kernel.
    std::int32_t gap_extend_penalty = 0; ///< Gap-extend penalty applied by the extension kernel.
    std::uint32_t flags = 0;             ///< Reserved routing flags, must be zero on upload.
    std::uint32_t reserved0 = 0;         ///< Reserved for ABI growth, must be zero on upload.
};

/** @brief Describes one CIGAR-capable extension and traceback job. */
struct gpu_alignment_cigar_job_wire {
    std::uint32_t query_symbol_offset = 0;     ///< Rank offset, or packed byte offset when flagged.
    std::uint32_t target_symbol_offset = 0;    ///< Rank offset, or packed byte offset when flagged.
    std::uint32_t query_len = 0;               ///< Query span length in symbols.
    std::uint32_t target_len = 0;              ///< Target span length in symbols.
    std::uint32_t query_global_start = 0;      ///< Query global start added to local coordinates.
    std::uint32_t target_global_start = 0;     ///< Target global start added to local coordinates.
    std::uint32_t band_width = 0;              ///< Band width admitted for this job.
    std::uint32_t flags = 0;                   ///< Reserved routing flags, must be zero on upload.
    std::uint32_t cigar_offset = 0;            ///< Output CIGAR array offset in operations.
    std::uint32_t cigar_capacity = 0;          ///< Maximum CIGAR operations writable for this job.
    std::uint32_t traceback_tile_offset = 0;   ///< Traceback tile array offset for this job.
    std::uint32_t traceback_tile_count = 0;    ///< Number of traceback tiles allocated.
    std::uint32_t query_packed_byte_count = 0; ///< Packed query bytes available to the shader.
    std::uint32_t target_packed_byte_count = 0;    ///< Packed target bytes available to the shader.
    std::uint32_t query_leading_symbol_offset = 0; ///< Symbols skipped in first query unit.
    std::uint32_t target_leading_symbol_offset = 0; ///< Symbols skipped in first target unit.
    std::uint32_t query_storage_unit_width = 0;     ///< Query packed-unit width in bytes.
    std::uint32_t target_storage_unit_width = 0;    ///< Target packed-unit width in bytes.
    std::uint32_t bits_per_symbol = 0;              ///< Logical rank width in packed storage.
    std::uint32_t problem_index = 0;                ///< Coordinate-receipt job bound to this work.
};

/** @brief Stores score, coordinate, status, and CIGAR output for one job. */
struct gpu_alignment_cigar_result_wire {
    std::int32_t score = 0;         ///< Reported alignment score for the job.
    std::int32_t max_score = 0;     ///< Maximum score observed during extension.
    std::uint32_t query_start = 0;  ///< Query interval start in local symbols.
    std::uint32_t query_end = 0;    ///< Query interval end in local symbols.
    std::uint32_t target_start = 0; ///< Target interval start in local symbols.
    std::uint32_t target_end = 0;   ///< Target interval end in local symbols.
    std::uint32_t cigar_offset = 0; ///< CIGAR array offset written for this job.
    std::uint32_t cigar_length = 0; ///< Number of CIGAR operations written.
    std::uint32_t status = 0;       ///< CIGAR status code (`k_gpu_alignment_extension_status_*`).
    std::uint32_t reserved = 0;     ///< Reserved for ABI growth, must be zero on readback.
    std::uint32_t num_matches = 0;  ///< Exact equal-rank count computed during GPU traceback.
    std::uint32_t block_length = 0; ///< Total aligned columns represented by the GPU CIGAR.
};

/** @brief Defines scoring and traceback geometry for CIGAR alignment. */
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
    std::int32_t z_drop = 0;           ///< Z-drop threshold terminating extension early.
    std::int32_t end_bonus = 0;        ///< End bonus applied at alignment endpoints.
    std::uint32_t flags = 0;           ///< Reserved routing flags, must be zero on upload.
    std::uint32_t reserved = 0;        ///< Reserved for ABI growth, must be zero on upload.
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
