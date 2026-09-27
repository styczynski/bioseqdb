/**
 * @file pipeline/minimap_port_parts/alignment.cppm
 * @brief Scalar and Block Aligner region-extension execution.
 */
module;

#include <new>

export module bioseq.pipeline.minimap_port:alignment;

import std;
import :results;
import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.common.memory_budget;
import bioseq.common.checked_arithmetic;
import bioseq.highway.dispatch_contract;
import bioseq.sequences;
import bioseq.sequences.cigar;
import bioseq.alignment;
import bioseq.alignment.utils;
import bioseq.alignment.block_aligner;
import bioseq.chaining.core;
import bioseq.instrumentation;
import bioseq.storage.lazy_sequence;
import bioseq.parallelism.mode;
import bioseq.parallelism.pool;
import bioseq.parallelism.pipeline_ops;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
import bioseq.pipeline.minimap_parameters_contract;
#endif

export namespace bioseq::simd {

// ============================================================================
// Stage 7: Base-level alignment — dual-affine gap DP (KSW2 equivalent)
// ============================================================================

// Score-matrix generation is owned by the alphabet-specialized matrix.

// ============================================================================
// KSW2 alignment wrapper
// ============================================================================

/// Position of one scalar KSW2 fill within an alignment region.
enum class scalar_ksw2_stage : std::uint8_t {
    /// Query and target prefix before the first seed.
    left,
    /// Gap between consecutive seeds.
    gap,
    /// Query and target suffix after the final seed.
    right,
};

/// Positive, operation-originated stats for scalar CPU dimensions in a route:
/// scalar minimizers, CPU seed lookup, scalar chaining/backtracking, and KSW2
/// extension. Hybrid routes may populate only the scalar dimensions they
/// execute. SIMD/Block/Vulkan stats objects remain separate.
struct scalar_pipeline_execution_stats {
    scalar_minimizer_execution_stats query_minimizers{};
    scalar_minimizer_execution_stats target_minimizers{};
    std::uint64_t seed_lookup_calls = 0;
    std::uint64_t seed_query_minimizers = 0;
    // A stage-level receipt written only after the selected scalar chaining
    // kernel returns successfully.  It is distinct from score/backtrack work:
    // a valid negative call can complete with zero anchors and execute neither.
    std::uint64_t chain_stage_completion_calls = 0;
    std::uint64_t chain_stage_completed_anchor_inputs = 0;
    std::uint64_t chain_score_calls = 0;
    std::uint64_t chain_backtrack_calls = 0;
    std::uint64_t chain_anchor_inputs = 0;
    std::uint64_t ksw2_left_calls = 0;
    std::uint64_t ksw2_gap_calls = 0;
    std::uint64_t ksw2_right_calls = 0;
    // Rectangle area admitted to real KSW2 calls. This is not a claim that a
    // banded/z-drop kernel visited every cell; it proves positive DP input.
    std::uint64_t ksw2_input_cell_area = 0;
    bool is_saturated = false;

    /// Add a counter while retaining overflow as a sticky flag.
    static constexpr void
    add(std::uint64_t& destination, const std::uint64_t increment, bool& saturated_flag) noexcept {
        scalar_minimizer_execution_stats::add(destination, increment, saturated_flag);
    }

    /// Record one completed scalar seed lookup.
    constexpr void record_seed_lookup(const std::uint64_t query_minimizer_count) noexcept {
        add(seed_lookup_calls, 1u, is_saturated);
        add(seed_query_minimizers, query_minimizer_count, is_saturated);
    }

    /// Record scalar score generation and backtracking for one anchor set.
    constexpr void record_scalar_chain(const std::uint64_t anchor_count) noexcept {
        add(chain_score_calls, 1u, is_saturated);
        add(chain_backtrack_calls, 1u, is_saturated);
        add(chain_anchor_inputs, anchor_count, is_saturated);
    }

    /// Record completion of the scalar chaining stage, including empty input.
    constexpr void record_scalar_chain_stage_completion(const std::uint64_t anchor_count) noexcept {
        add(chain_stage_completion_calls, 1u, is_saturated);
        add(chain_stage_completed_anchor_inputs, anchor_count, is_saturated);
    }

    /// Record one real KSW2 fill and its input rectangle.
    constexpr void record_ksw2_call(const scalar_ksw2_stage stage,
                                    const std::uint64_t query_symbols,
                                    const std::uint64_t target_symbols) noexcept {
        switch (stage) {
        case scalar_ksw2_stage::left:
            add(ksw2_left_calls, 1u, is_saturated);
            break;
        case scalar_ksw2_stage::gap:
            add(ksw2_gap_calls, 1u, is_saturated);
            break;
        case scalar_ksw2_stage::right:
            add(ksw2_right_calls, 1u, is_saturated);
            break;
        }
        if (query_symbols != 0 &&
            target_symbols > (std::numeric_limits<std::uint64_t>::max)() / query_symbols) {
            ksw2_input_cell_area = (std::numeric_limits<std::uint64_t>::max)();
            is_saturated = true;
            return;
        }
        add(ksw2_input_cell_area, query_symbols * target_symbols, is_saturated);
    }

    /// Merge independently completed scalar-route statistics.
    constexpr void merge(const scalar_pipeline_execution_stats& other) noexcept {
        query_minimizers.merge(other.query_minimizers);
        target_minimizers.merge(other.target_minimizers);
        bool overflow = is_saturated || other.is_saturated || query_minimizers.saturated ||
                        target_minimizers.saturated;
        add(seed_lookup_calls, other.seed_lookup_calls, overflow);
        add(seed_query_minimizers, other.seed_query_minimizers, overflow);
        add(chain_stage_completion_calls, other.chain_stage_completion_calls, overflow);
        add(chain_stage_completed_anchor_inputs,
            other.chain_stage_completed_anchor_inputs,
            overflow);
        add(chain_score_calls, other.chain_score_calls, overflow);
        add(chain_backtrack_calls, other.chain_backtrack_calls, overflow);
        add(chain_anchor_inputs, other.chain_anchor_inputs, overflow);
        add(ksw2_left_calls, other.ksw2_left_calls, overflow);
        add(ksw2_gap_calls, other.ksw2_gap_calls, overflow);
        add(ksw2_right_calls, other.ksw2_right_calls, overflow);
        add(ksw2_input_cell_area, other.ksw2_input_cell_area, overflow);
        is_saturated = overflow;
    }

    /// Validate the complete all-scalar tuple. Hybrid routes validate only the
    /// scalar dimensions they selected at their route-stats boundary.
    [[nodiscard]] constexpr bool is_route_consistent(const bool require_target_sketch,
                                                     const bool require_alignment) const noexcept {
        const bool is_target_valid = require_target_sketch
                                         ? target_minimizers.is_coherent()
                                         : target_minimizers == scalar_minimizer_execution_stats{};
        const bool is_alignment_valid =
            !require_alignment ||
            ((ksw2_left_calls != 0 || ksw2_gap_calls != 0 || ksw2_right_calls != 0) &&
             ksw2_input_cell_area > 0);
        const bool empty_chain_completed = chain_stage_completed_anchor_inputs == 0 &&
                                           chain_anchor_inputs == 0 && chain_score_calls == 0 &&
                                           chain_backtrack_calls == 0;
        const bool nonempty_chain_completed =
            chain_stage_completed_anchor_inputs > 0 &&
            chain_anchor_inputs == chain_stage_completed_anchor_inputs && chain_score_calls > 0 &&
            chain_score_calls == chain_backtrack_calls &&
            chain_score_calls <= chain_stage_completion_calls;
        return !is_saturated && query_minimizers.is_coherent() && is_target_valid &&
               seed_lookup_calls > 0 && seed_query_minimizers > 0 &&
               chain_stage_completion_calls > 0 &&
               (empty_chain_completed || nonempty_chain_completed) && is_alignment_valid;
    }

    /// Compare every scalar execution counter and state flag.
    constexpr bool operator==(const scalar_pipeline_execution_stats&) const noexcept = default;
};

/// Return whether Block Aligner can represent the selected affine penalties.
[[nodiscard]] constexpr bool
block_aligner_native_penalties_representable(const minimap2_parameters& params) noexcept {
    return params.gap_open1 > 0 && params.gap_ext1 > 0 &&
           params.gap_open1 <= std::numeric_limits<std::int8_t>::max() &&
           params.gap_ext1 <= std::numeric_limits<std::int8_t>::max() &&
           params.gap_open1 + params.gap_ext1 <=
               -static_cast<int>(std::numeric_limits<std::int8_t>::min());
}

/// Project pipeline scoring parameters into one Block Aligner stage.
[[nodiscard]] inline block_aligner_parameters
project_block_aligner_params(const minimap2_parameters& params,
                             block_alignment_stage stage) noexcept {
    const bool is_extension = stage != block_alignment_stage::gap_extension;
    return block_aligner_parameters{
        .gap_open = static_cast<std::int8_t>(params.gap_open1),
        .gap_extend = static_cast<std::int8_t>(params.gap_ext1),
        .minimum_block_size = params.block_min_size,
        .maximum_block_size = params.block_max_size,
        .x_drop_threshold = is_extension ? params.zdrop : 0,
        .mode = is_extension ? block_alignment_mode::x_drop : block_alignment_mode::global,
        .input_order = stage == block_alignment_stage::left_extension ? block_input_order::reversed
                                                                      : block_input_order::forward,
        .stage = stage,
        .is_trace_enabled = params.do_cigar || params.compute_edit_distance,
        .should_compute_edit_distance = params.compute_edit_distance,
    };
}

/**
 * Run one scalar KSW2 fill over lazy query and target subranges.
 *
 * @tparam p_is_reversed Whether the query is reverse-complement encoded.
 * @tparam p_alphabet Logical alphabet shared by both sequences.
 */
template <bool p_is_reversed, sequence_alphabet p_alphabet>
[[nodiscard]] ksw_alignment_result
ksw2_align_pair(bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
                bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
                std::size_t query_start,
                std::size_t query_end,
                std::size_t target_start,
                std::size_t target_end,
                const scoring_matrix<p_alphabet>& scoring_matrix,
                const alignment_kernels<p_alphabet>& alignment_kernels,
                const minimap2_parameters& params,
                int align_bandwidth,
                int end_bonus,
                int zdrop,
                ksw_flags ksw_flags,
                block_aligner_execution_stats* forbidden_block_stats = nullptr,
                scalar_pipeline_execution_stats* scalar_execution_stats = nullptr,
                scalar_ksw2_stage scalar_stage = scalar_ksw2_stage::gap) {
    if (forbidden_block_stats != nullptr) {
        abort_forbidden_ksw2_fallback(*forbidden_block_stats);
    }
    if (scalar_execution_stats != nullptr) {
        scalar_execution_stats->record_ksw2_call(
            scalar_stage,
            static_cast<std::uint64_t>(query_end - query_start),
            static_cast<std::uint64_t>(target_end - target_start));
    }
    if (params.gap_open1 == params.gap_open2 && params.gap_ext1 == params.gap_ext2) {
        const ksw_single_affine_parameters z2_params{
            .gap_open = static_cast<std::int8_t>(params.gap_open1),
            .gap_extend = static_cast<std::int8_t>(params.gap_ext1),
            .band_width = align_bandwidth,
            .z_drop = zdrop,
            .end_bonus = end_bonus,
            .flags = ksw_flags,
        };
        return alignment_kernels.align_ksw2_single_affine(p_is_reversed,
                                                          query_seq,
                                                          target_seq,
                                                          query_start,
                                                          query_end,
                                                          target_start,
                                                          target_end,
                                                          scoring_matrix,
                                                          z2_params);
    }
    const ksw_dual_affine_parameters d2_params{
        .gap_open_short = static_cast<std::int8_t>(params.gap_open1),
        .gap_extend_short = static_cast<std::int8_t>(params.gap_ext1),
        .gap_open_long = static_cast<std::int8_t>(params.gap_open2),
        .gap_extend_long = static_cast<std::int8_t>(params.gap_ext2),
        .band_width = align_bandwidth,
        .z_drop = zdrop,
        .end_bonus = end_bonus,
        .flags = ksw_flags,
    };
    return alignment_kernels.align_ksw2_dual_affine(p_is_reversed,
                                                    query_seq,
                                                    target_seq,
                                                    query_start,
                                                    query_end,
                                                    target_start,
                                                    target_end,
                                                    scoring_matrix,
                                                    d2_params);
}

/**
 * @brief Project the native Block Aligner contract for one minimap stage.
 *
 * This is deliberately not a KSW2-compatible adapter. The reference Block
 * Aligner has a single affine recurrence, adaptive block geometry, global
 * mode, and its own two-confirmation X-drop mode. Consequently only the
 * short affine pair and the Block tunables are projected from minimap2_parameters:
 *
 *   - gap fill: reference Global mode, forward input.
 *   - left extension: reference XDrop mode, reversed input.
 *   - right extension: reference XDrop mode, forward input.
 *
 * Dormant KSW-only defaults in the shared parameter object (bandwidth,
 * right-align, end-bonus, the second affine pair, and approximate-max) do not
 * invalidate a native Block run and are not included in its stats identity.
 * They are intentionally absent from this function's call surface, so a
 * caller that claims those semantics gets a compile-time contract error
 * instead of a KSW2 or scalar fallback.
 */
template <bool p_is_reversed,
          bool p_enable_diagonal_certificate = true,
          sequence_alphabet p_alphabet>
[[nodiscard]] ksw_alignment_result
block_aligner_align_pair(bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
                         bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
                         std::size_t query_start,
                         std::size_t query_end,
                         std::size_t target_start,
                         std::size_t target_end,
                         const scoring_matrix<p_alphabet>& scoring_matrix,
                         const alignment_kernels<p_alphabet>& alignment_kernels,
                         const minimap2_parameters& params,
                         block_alignment_stage stage,
                         block_aligner_execution_stats* execution_stats) {
    const bool is_extension = stage != block_alignment_stage::gap_extension;
    if (!block_aligner_native_penalties_representable(params) ||
        (is_extension && params.zdrop < 0)) {
        if (execution_stats != nullptr) {
            execution_stats->add(execution_stats->unsupported_contract_rejections);
        }
        bioseq_log<logging_level::error>(
            "Block Aligner native parameter projection rejected: q={}, e={}, "
            "zdrop={}, stage={}",
            params.gap_open1,
            params.gap_ext1,
            params.zdrop,
            static_cast<unsigned>(stage));
        std::abort();
    }
    const block_aligner_parameters ba_params = project_block_aligner_params(params, stage);
    return alignment_kernels.align_block(p_is_reversed,
                                         p_enable_diagonal_certificate,
                                         query_seq,
                                         target_seq,
                                         query_start,
                                         query_end,
                                         target_start,
                                         target_end,
                                         scoring_matrix,
                                         ba_params,
                                         execution_stats,
                                         /*construction_stats=*/nullptr);
}

/// Expected counts for the three native Block Aligner fill positions.
struct block_alignment_eligibility {
    /// Expected left-extension calls.
    std::uint64_t left = 0;
    /// Expected seed-to-seed gap-fill calls.
    std::uint64_t gap = 0;
    /// Expected right-extension calls.
    std::uint64_t right = 0;
};

/// Geometry of one mandatory seed-to-seed global fill.  Left/right
/// extensions are deliberately excluded: they are local/X-drop domains and
/// therefore cannot support a no-false-reject edit-threshold prefilter.
struct mandatory_alignment_gap_window {
    /// Inclusive query start.
    sequence_size_t query_start = 0;
    /// Exclusive query end.
    sequence_size_t query_end = 0;
    /// Inclusive target start.
    sequence_size_t target_start = 0;
    /// Exclusive target end.
    sequence_size_t target_end = 0;
};

/**
 * Visit every positive seed-to-seed fill required by one alignment region.
 *
 * @tparam p_callback Callable accepting a mandatory gap window.
 */
template <class p_callback>
[[gnu::always_inline]] bool for_each_mandatory_alignment_gap(const alignment_region& region,
                                                             std::span<const anchor> anchors,
                                                             const minimap2_parameters& params,
                                                             p_callback&& callback) {
    if (region.seed_count <= 1 || region.anchor_start < 0 ||
        static_cast<std::size_t>(region.anchor_start) >= anchors.size()) {
        return true;
    }
    const std::int32_t anchor_begin = region.anchor_start;
    const anchor& first_anchor = anchors[anchor_begin];
    const std::int64_t first_span = static_cast<std::int64_t>(first_anchor.query_span);
    std::int64_t query_start =
        static_cast<std::int64_t>(first_anchor.query_position) + 1 - first_span;
    std::int64_t target_start =
        static_cast<std::int64_t>(first_anchor.ref_position) + 1 - first_span;
    query_start = std::max<std::int64_t>(0, query_start);
    target_start = std::max<std::int64_t>(0, target_start);

    for (std::int32_t index = 1; index < region.seed_count; ++index) {
        const std::size_t anchor_index = static_cast<std::size_t>(anchor_begin + index);
        if (anchor_index >= anchors.size())
            return false;
        const anchor& current = anchors[anchor_index];
        const std::int64_t query_end = static_cast<std::int64_t>(current.query_position) + 1;
        const std::int64_t target_end = static_cast<std::int64_t>(current.ref_position) + 1;
        const bool is_last = index == region.seed_count - 1;
        const std::int64_t query_length = query_end - query_start;
        const std::int64_t target_length = target_end - target_start;
        if (!is_last && (query_length < params.min_ksw_len || target_length < params.min_ksw_len)) {
            continue;
        }
        if (query_length <= 0 || target_length <= 0) {
            query_start = query_end;
            target_start = target_end;
            continue;
        }
        const mandatory_alignment_gap_window window{
            .query_start = static_cast<sequence_size_t>(query_start),
            .query_end = static_cast<sequence_size_t>(query_end),
            .target_start = static_cast<sequence_size_t>(target_start),
            .target_end = static_cast<sequence_size_t>(target_end),
        };
        if (!callback(window))
            return false;
        query_start = query_end;
        target_start = target_end;
    }
    return true;
}

/// Derive the complete native-Block stage contract before dispatch from only
/// immutable region geometry. These values are deliberately recorded outside
/// the call branches they attest, so deleting or bypassing a stage cannot
/// delete both the expectation and execution statistics.
[[nodiscard]] inline block_alignment_eligibility
derive_block_stage_eligibility(const alignment_region& region,
                               std::span<const anchor> anchors,
                               std::int32_t anchor_begin,
                               std::int64_t seed_ref_start,
                               std::int64_t seed_query_start,
                               std::int64_t seed_ref_end,
                               std::int64_t seed_query_end,
                               std::int64_t outer_ref_start,
                               std::int64_t outer_query_start,
                               std::int64_t outer_ref_end,
                               std::int64_t outer_query_end,
                               const minimap2_parameters& params) noexcept {
    block_alignment_eligibility expected{
        .left = seed_query_start > outer_query_start && seed_ref_start > outer_ref_start ? 1u : 0u,
        .gap = 0,
        .right = seed_query_end < outer_query_end && seed_ref_end < outer_ref_end ? 1u : 0u,
    };
    if (region.seed_count <= 1)
        return expected;

    std::int64_t qs = seed_query_start;
    std::int64_t rs = seed_ref_start;
    for (std::int32_t index = 1; index < region.seed_count; ++index) {
        const anchor& current = anchors[anchor_begin + index];
        const std::int64_t qe = static_cast<std::int64_t>(current.query_position) + 1;
        const std::int64_t re = static_cast<std::int64_t>(current.ref_position) + 1;
        const bool is_last = index == region.seed_count - 1;
        const std::int64_t gap_query = qe - qs;
        const std::int64_t gap_ref = re - rs;
        if (!is_last && (gap_query < params.min_ksw_len || gap_ref < params.min_ksw_len)) {
            continue;
        }
        if (gap_query <= 0 || gap_ref <= 0) {
            qs = qe;
            rs = re;
            continue;
        }
        ++expected.gap;
        qs = qe;
        rs = re;
    }
    return expected;
}

/**
 * Execute left extension, mandatory gap fills, and right extension for one
 * region using the compile-time selected alignment algorithm.
 *
 * @tparam p_base_alignment_algorithm KSW2 or native Block Aligner route.
 * @tparam p_alphabet Logical alphabet shared by query and target.
 * @tparam p_enable_diagonal_certificate Enable Block Aligner certificates.
 */
template <base_alignment_algorithm p_base_alignment_algorithm,
          sequence_alphabet p_alphabet,
          bool p_enable_diagonal_certificate = true>
struct align_one_region {
    bioseq::storage::lazy_sequence_view<p_alphabet> query_seq;
    bioseq::storage::lazy_sequence_view<p_alphabet> target_seq;
    const scoring_matrix<p_alphabet>& scoring_matrix;
    const alignment_kernels<p_alphabet>& alignment_kernels;
    const minimap2_parameters& params;
    std::int32_t align_bandwidth;

    /**
     * Return exact edit accounting for one completed fill.
     *
     * Native Block traceback counts edits while its already-expanded padded
     * query/target ranks are resident. Other kernels retain the established
     * post-result expansion path and therefore keep identical semantics.
     */
    template <bool p_strand_reversed>
    [[gnu::always_inline]] sequence_size_t
    exact_edit_distance_for_fill(const ksw_alignment_result& fill_result,
                                 std::size_t query_start,
                                 std::size_t query_end,
                                 std::size_t target_start,
                                 std::size_t target_end) const {
        if constexpr (p_base_alignment_algorithm == base_alignment_algorithm::block_aligner) {
            if (!fill_result.is_edit_distance_valid) {
                bioseq_log<logging_level::error>(
                    "Block Aligner result omitted requested exact edit accounting");
                std::abort();
            }
            return static_cast<sequence_size_t>(fill_result.edit_distance);
        } else {
            return exact_edit_distance_from_packed_cigar<p_strand_reversed>(
                query_seq,
                target_seq,
                query_start,
                query_end,
                target_start,
                target_end,
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                std::span<const std::uint32_t>{
                    fill_result.cigar.data(), fill_result.cigar.size()}
#else
                std::span<const std::uint32_t>(fill_result.cigar)
#endif
            );
        }
    }

    /// Apply one gap-fill result and report whether Z-drop ended the walk.
    template <bool p_strand_reversed>
    [[gnu::always_inline]] bool apply_gap_fill_result(alignment_region& region,
                                                      const ksw_alignment_result& fill_result,
                                                      std::int64_t qe,
                                                      std::int64_t re,
                                                      std::int64_t& qs,
                                                      std::int64_t& rs,
                                                      std::int64_t& final_query_end,
                                                      std::int64_t& final_ref_end) const {
        if (!fill_result.cigar.empty()) {
            append_cigar(region.cigar, fill_result.cigar);
        }
        const std::int64_t fill_ref_end =
            fill_result.is_z_dropped ? rs + (fill_result.max_t + 1) : re;
        const std::int64_t fill_query_end =
            fill_result.is_z_dropped ? qs + (fill_result.max_q + 1) : qe;
        if (params.compute_edit_distance) {
            region.edit_distance += exact_edit_distance_for_fill<p_strand_reversed>(
                fill_result,
                static_cast<std::size_t>(qs),
                static_cast<std::size_t>(fill_query_end),
                static_cast<std::size_t>(rs),
                static_cast<std::size_t>(fill_ref_end));
        }
        if (fill_result.is_z_dropped) {
            final_ref_end = fill_ref_end;
            final_query_end = fill_query_end;
            region.dp_score += fill_result.max;
            return true;
        }
        final_ref_end = re;
        final_query_end = qe;
        region.dp_score += fill_result.score;
        qs = qe;
        rs = re;
        return false;
    }

    /// Execute the three alignment fills for one oriented region.
    template <bool p_strand_reversed>
    [[gnu::always_inline]] void run(alignment_region& region,
                                    std::span<const anchor> anchors,
                                    std::int32_t anchor_begin,
                                    std::int64_t seed_ref_start,
                                    std::int64_t seed_query_start,
                                    std::int64_t seed_ref_end,
                                    std::int64_t seed_query_end,
                                    std::int64_t outer_ref_start,
                                    std::int64_t outer_query_start,
                                    std::int64_t outer_ref_end,
                                    std::int64_t outer_query_end,
                                    std::int64_t query_length,
                                    block_aligner_execution_stats* block_execution_stats,
                                    scalar_pipeline_execution_stats* scalar_execution_stats) const {
        bool prior_stage_zdropped = false;

        // === 1. Left extension ===
        std::int64_t final_ref_start = seed_ref_start;
        std::int64_t final_query_start = seed_query_start;
        if (seed_query_start > outer_query_start && seed_ref_start > outer_ref_start) {
            ksw_alignment_result left_result;
            if constexpr (p_base_alignment_algorithm == base_alignment_algorithm::block_aligner) {
                left_result = block_aligner_align_pair<p_strand_reversed>(
                    query_seq,
                    target_seq,
                    static_cast<std::size_t>(outer_query_start),
                    static_cast<std::size_t>(seed_query_start),
                    static_cast<std::size_t>(outer_ref_start),
                    static_cast<std::size_t>(seed_ref_start),
                    scoring_matrix,
                    alignment_kernels,
                    params,
                    block_alignment_stage::left_extension,
                    block_execution_stats);
            } else {
                left_result = ksw2_align_pair<p_strand_reversed>(
                    query_seq,
                    target_seq,
                    static_cast<std::size_t>(outer_query_start),
                    static_cast<std::size_t>(seed_query_start),
                    static_cast<std::size_t>(outer_ref_start),
                    static_cast<std::size_t>(seed_ref_start),
                    scoring_matrix,
                    alignment_kernels,
                    params,
                    align_bandwidth,
                    params.end_bonus,
                    params.zdrop,
                    ksw_flags{.right_align = true, .extz_only = true, .reverse_cigar = true},
                    block_execution_stats,
                    scalar_execution_stats,
                    scalar_ksw2_stage::left);
            }
            if (!left_result.cigar.empty()) {
                append_cigar(region.cigar, left_result.cigar);
                region.dp_score += left_result.max;
            }
            const std::int32_t left_ref_offset =
                left_result.has_reached_end ? left_result.mqe_t + 1 : left_result.max_t + 1;
            final_ref_start = seed_ref_start - left_ref_offset;
            const std::int32_t left_query_offset = left_result.has_reached_end
                                                  ? seed_query_start - outer_query_start
                                                  : left_result.max_q + 1;
            final_query_start = seed_query_start - left_query_offset;
            if (params.compute_edit_distance) {
                region.edit_distance += exact_edit_distance_for_fill<p_strand_reversed>(
                    left_result,
                    static_cast<std::size_t>(final_query_start),
                    static_cast<std::size_t>(seed_query_start),
                    static_cast<std::size_t>(final_ref_start),
                    static_cast<std::size_t>(seed_ref_start));
            }
            prior_stage_zdropped = left_result.is_z_dropped;
        }

        // === 2. Gap filling — per-seed-pair walk ===
        std::int64_t final_ref_end = seed_ref_start;
        std::int64_t final_query_end = seed_query_start;
        if (region.seed_count > 1) {
            std::int64_t qs = seed_query_start;
            std::int64_t rs = seed_ref_start;
            bool zdropped_any = false;
            block_global_gap_certificate_plan packed_gap_plan{};
            bool packed_gap_plan_ready = false;
            final_ref_end = seed_ref_start;
            final_query_end = seed_query_start;
            for (std::int32_t i = 1; i < region.seed_count; ++i) {
                const anchor& cur_anchor = anchors[anchor_begin + i];
                const std::int64_t qe = static_cast<std::int64_t>(cur_anchor.query_position) + 1;
                const std::int64_t re = static_cast<std::int64_t>(cur_anchor.ref_position) + 1;
                const bool is_last = (i == region.seed_count - 1);
                const std::int64_t gap_query = qe - qs;
                const std::int64_t gap_ref = re - rs;
                if (!is_last && (gap_query < params.min_ksw_len || gap_ref < params.min_ksw_len)) {
                    continue;
                }
                if (gap_query <= 0 || gap_ref <= 0) {
                    qs = qe;
                    rs = re;
                    continue;
                }
                if constexpr (p_base_alignment_algorithm ==
                              base_alignment_algorithm::block_aligner) {
                    block_packed_certificate_attempt packed_attempt{};
                    if constexpr (p_enable_diagonal_certificate) {
                        // Prepare the immutable matrix/contract state only
                        // after the anchor walk produces its first valid gap.
                        // Invalid minimap penalty projections remain on the
                        // existing adapter path, which records and aborts.
                        if (block_aligner_native_penalties_representable(params)) {
                            if (!packed_gap_plan_ready) {
                                packed_gap_plan =
                                    alignment_kernels.prepare_block_global_gap_certificate(
                                        scoring_matrix,
                                        project_block_aligner_params(
                                            params, block_alignment_stage::gap_extension));
                                packed_gap_plan_ready = true;
                            }
                            packed_attempt =
                                alignment_kernels.try_block_certified_gap(p_strand_reversed,
                                                                          query_seq,
                                                                          target_seq,
                                                                          static_cast<std::size_t>(qs),
                                                                          static_cast<std::size_t>(qe),
                                                                          static_cast<std::size_t>(rs),
                                                                          static_cast<std::size_t>(re),
                                                                          packed_gap_plan);
                        }
                    }

                    if (packed_attempt.decision == block_packed_certificate_decision::certified) {
                        const std::uint32_t match_length =
                            static_cast<std::uint32_t>(packed_attempt.certificate.symbols);
                        if (params.do_cigar) {
                            if (!region.cigar.empty() &&
                                region.cigar.back().opcode == cigar_match_opcode) {
                                region.cigar.back().length += match_length;
                            } else {
                                region.cigar.push_back(
                                    cigar_operation{match_length, cigar_match_opcode});
                            }
                        }
                        region.dp_score += packed_attempt.certificate.score;
                        if (params.compute_edit_distance) {
                            region.edit_distance += static_cast<sequence_size_t>(
                                packed_attempt.certificate.edit_distance);
                        }
                        final_ref_end = re;
                        final_query_end = qe;
                        qs = qe;
                        rs = re;
                        record_block_certified_global_gap(packed_gap_plan,
                                                          packed_attempt.certificate,
                                                          p_strand_reversed,
                                                          block_execution_stats);
                        continue;
                    }

                    // Construct shared result storage only for a real DP
                    // fallback.  Certified gaps above therefore avoid even
                    // the default std::vector state/destructor.
                    ksw_alignment_result fill_result;
                    if (packed_attempt.decision == block_packed_certificate_decision::rejected) {
                        // Exact packed bytes have already been scanned.  Run
                        // the unchanged DP path without paying for the byte
                        // certificate a second time.
                        fill_result = block_aligner_align_pair<p_strand_reversed, false>(
                            query_seq,
                            target_seq,
                            static_cast<std::size_t>(qs),
                            static_cast<std::size_t>(qe),
                            static_cast<std::size_t>(rs),
                            static_cast<std::size_t>(re),
                            scoring_matrix,
                            alignment_kernels,
                            params,
                            block_alignment_stage::gap_extension,
                            block_execution_stats);
                    } else {
                        // Fragmented storage, DNA5, unsupported matrices and
                        // non-candidate geometry retain today's route,
                        // including the existing byte certificate.
                        fill_result = block_aligner_align_pair<p_strand_reversed,
                                                               p_enable_diagonal_certificate>(
                            query_seq,
                            target_seq,
                            static_cast<std::size_t>(qs),
                            static_cast<std::size_t>(qe),
                            static_cast<std::size_t>(rs),
                            static_cast<std::size_t>(re),
                            scoring_matrix,
                            alignment_kernels,
                            params,
                            block_alignment_stage::gap_extension,
                            block_execution_stats);
                    }
                    if (apply_gap_fill_result<p_strand_reversed>(
                            region, fill_result, qe, re, qs, rs, final_query_end, final_ref_end)) {
                        zdropped_any = true;
                        break;
                    }
                } else {
                    ksw_alignment_result fill_result = ksw2_align_pair<p_strand_reversed>(
                        query_seq,
                        target_seq,
                        static_cast<std::size_t>(qs),
                        static_cast<std::size_t>(qe),
                        static_cast<std::size_t>(rs),
                        static_cast<std::size_t>(re),
                        scoring_matrix,
                        alignment_kernels,
                        params,
                        params.compute_edit_distance ? -1 : align_bandwidth,
                        -1,
                        params.compute_edit_distance ? -1 : params.zdrop,
                        ksw_flags{.approx_max = true},
                        block_execution_stats,
                        scalar_execution_stats,
                        scalar_ksw2_stage::gap);
                    if (apply_gap_fill_result<p_strand_reversed>(
                            region, fill_result, qe, re, qs, rs, final_query_end, final_ref_end)) {
                        zdropped_any = true;
                        break;
                    }
                }
            }
            prior_stage_zdropped = zdropped_any;
        } else {
            final_ref_end = seed_ref_end;
            final_query_end = seed_query_end;
            if (params.do_cigar || params.compute_edit_distance) {
                append_implicit_seed_cigar(
                    region.cigar,
                    static_cast<std::size_t>(seed_query_end - seed_query_start),
                    static_cast<std::size_t>(seed_ref_end - seed_ref_start));
            }
            if (params.compute_edit_distance) {
                region.edit_distance += exact_edit_distance_for_implicit_seed<p_strand_reversed>(
                    query_seq,
                    target_seq,
                    static_cast<std::size_t>(seed_query_start),
                    static_cast<std::size_t>(seed_query_end),
                    static_cast<std::size_t>(seed_ref_start),
                    static_cast<std::size_t>(seed_ref_end));
            }
        }

        // === 3. Right extension ===
        if ((p_base_alignment_algorithm == base_alignment_algorithm::block_aligner ||
             !prior_stage_zdropped) &&
            seed_query_end < outer_query_end && seed_ref_end < outer_ref_end) {
            ksw_alignment_result right_result;
            if constexpr (p_base_alignment_algorithm == base_alignment_algorithm::block_aligner) {
                right_result = block_aligner_align_pair<p_strand_reversed>(
                    query_seq,
                    target_seq,
                    static_cast<std::size_t>(seed_query_end),
                    static_cast<std::size_t>(outer_query_end),
                    static_cast<std::size_t>(seed_ref_end),
                    static_cast<std::size_t>(outer_ref_end),
                    scoring_matrix,
                    alignment_kernels,
                    params,
                    block_alignment_stage::right_extension,
                    block_execution_stats);
            } else {
                right_result =
                    ksw2_align_pair<p_strand_reversed>(query_seq,
                                                       target_seq,
                                                       static_cast<std::size_t>(seed_query_end),
                                                       static_cast<std::size_t>(outer_query_end),
                                                       static_cast<std::size_t>(seed_ref_end),
                                                       static_cast<std::size_t>(outer_ref_end),
                                                       scoring_matrix,
                                                       alignment_kernels,
                                                       params,
                                                       align_bandwidth,
                                                       params.end_bonus,
                                                       params.zdrop,
                                                       ksw_flags{.extz_only = true},
                                                       block_execution_stats,
                                                       scalar_execution_stats,
                                                       scalar_ksw2_stage::right);
            }
            if (!right_result.cigar.empty()) {
                append_cigar(region.cigar, right_result.cigar);
                region.dp_score += right_result.max;
            }
            const std::int32_t right_ref_offset =
                right_result.has_reached_end ? right_result.mqe_t + 1 : right_result.max_t + 1;
            final_ref_end = seed_ref_end + right_ref_offset;
            const std::int32_t right_query_offset = right_result.has_reached_end
                                                   ? outer_query_end - seed_query_end
                                                   : right_result.max_q + 1;
            final_query_end = seed_query_end + right_query_offset;
            if (params.compute_edit_distance) {
                region.edit_distance += exact_edit_distance_for_fill<p_strand_reversed>(
                    right_result,
                    static_cast<std::size_t>(seed_query_end),
                    static_cast<std::size_t>(final_query_end),
                    static_cast<std::size_t>(seed_ref_end),
                    static_cast<std::size_t>(final_ref_end));
            }
        }

        // Set final coordinates from alignment extent (align.c:897-899).
        // Internal extent math is signed int64; the stored region fields are
        // sequence_size_t (non-negative absolute coordinates).
        region.ref_start = static_cast<sequence_size_t>(final_ref_start);
        region.ref_end = static_cast<sequence_size_t>(final_ref_end);
        if (!region.is_reverse) {
            region.query_start = static_cast<sequence_size_t>(final_query_start);
            region.query_end = static_cast<sequence_size_t>(final_query_end);
        } else {
            region.query_start = static_cast<sequence_size_t>(query_length - final_query_end);
            region.query_end = static_cast<sequence_size_t>(query_length - final_query_start);
        }
    }
};

/**
 * @brief Pure region bounds consumed by the unchanged three-part alignment fill.
 *
 * These are logical extents, not reservations. The final CIGAR bound counts
 * uncompressed alignment steps; initial_cigar_reserve is only the existing
 * allocation optimization and is not claimed to bound the completed CIGAR.
 */
struct alignment_region_storage_plan {
    bool has_work = false;
    std::int32_t anchor_begin = 0;
    std::int64_t seed_ref_start = 0;
    std::int64_t seed_query_start = 0;
    std::int64_t seed_ref_end = 0;
    std::int64_t seed_query_end = 0;
    std::int64_t outer_ref_start = 0;
    std::int64_t outer_query_start = 0;
    std::int64_t outer_ref_end = 0;
    std::int64_t outer_query_end = 0;
    std::uint64_t outer_query_extent = 0u;
    std::uint64_t outer_reference_extent = 0u;
    std::size_t final_cigar_step_bound = 0u;
    std::uint64_t final_cigar_capacity_bytes = 0u;
    std::size_t initial_cigar_reserve = 0u;
};

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
/** Selected-kernel temporary peak for one nonempty alignment region. */
struct selected_alignment_region_storage_plan {
    bool has_work = false;
    std::size_t fill_count = 0u;
    std::uint64_t maximum_kernel_workspace_bytes = 0u;
    std::uint64_t maximum_result_capacity_bytes = 0u;
    std::uint64_t temporary_workspace_bytes = 0u;
};
#endif

/**
 * @brief Derive checked storage geometry without allocating or running a kernel.
 * @throws std::length_error For unrepresentable coordinates or allocation extents.
 * @throws std::invalid_argument For malformed region ownership or gap parameters.
 */
[[nodiscard]] inline alignment_region_storage_plan derive_alignment_region_storage_plan(
    const alignment_region& region,
    std::span<const anchor> anchors,
    std::uint64_t query_size,
    std::uint64_t target_size,
    const minimap2_parameters& params) {
    if (region.seed_count <= 0) {
        return {};
    }
    constexpr auto signed_limit =
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
    if (query_size > signed_limit || target_size > signed_limit) {
        throw std::length_error{"alignment storage coordinates exceed the signed fill domain"};
    }
    if (region.anchor_start < 0 ||
        static_cast<std::size_t>(region.anchor_start) > anchors.size() ||
        static_cast<std::size_t>(region.seed_count) >
            anchors.size() - static_cast<std::size_t>(region.anchor_start) ||
        params.max_gap < 0 || params.match_score < 0 || params.gap_open1 < 0 ||
        params.gap_ext1 <= 0) {
        throw std::invalid_argument{"alignment storage region or gap parameters are invalid"};
    }
    // All score/gap inputs are signed 32-bit. With the validated nonnegative
    // domain, extension*match and extension+bonus fit signed 64-bit exactly.
    static_assert(std::is_same_v<decltype(params.max_gap), std::int32_t>);
    static_assert(std::is_same_v<decltype(params.match_score), std::int32_t>);
    static_assert(std::is_same_v<decltype(params.gap_open1), std::int32_t>);
    static_assert(std::is_same_v<decltype(params.gap_ext1), std::int32_t>);
    const std::int64_t query_length = static_cast<std::int64_t>(query_size);
    const std::int64_t target_length = static_cast<std::int64_t>(target_size);
    const auto first_index = static_cast<std::size_t>(region.anchor_start);
    const auto last_index = first_index + static_cast<std::size_t>(region.seed_count) - 1u;
    if (anchors[first_index].query_position >= query_size ||
        anchors[last_index].query_position >= query_size ||
        anchors[first_index].ref_position >= target_size ||
        anchors[last_index].ref_position >= target_size ||
        anchors[first_index].query_span == 0u) {
        throw std::invalid_argument{"alignment storage seed endpoints exceed their owning sequences"};
    }
    const std::int32_t anchor_begin = region.anchor_start;
    const anchor& first_anchor = anchors[anchor_begin];
    const anchor& last_anchor = anchors[static_cast<std::size_t>(anchor_begin) +
                                        static_cast<std::size_t>(region.seed_count) - 1u];

    // Inner seed boundaries (where seeds are)
    const std::int32_t first_span =
        static_cast<std::int32_t>(first_anchor.query_span);
    std::int64_t seed_ref_start =
        static_cast<std::int64_t>(first_anchor.ref_position) + 1 - first_span;
    std::int64_t seed_query_start =
        static_cast<std::int64_t>(first_anchor.query_position) + 1 - first_span;
    const std::int64_t seed_ref_end = static_cast<std::int64_t>(last_anchor.ref_position) + 1;
    const std::int64_t seed_query_end =
        static_cast<std::int64_t>(last_anchor.query_position) + 1;
    if (seed_ref_start < 0) {
        seed_ref_start = 0;
    }
    if (seed_query_start < 0) {
        seed_query_start = 0;
    }

    // Outer boundaries: extend beyond seeds (align.c:708-762 simplified).
    // `extension_len` is a bounded gap (<= params.max_gap) is_computed in int64
    // to match the int64 coordinates; the std::min args are matched in type.
    std::int64_t outer_ref_start = seed_ref_start;
    std::int64_t outer_query_start = seed_query_start;
    std::int64_t outer_ref_end = seed_ref_end;
    std::int64_t outer_query_end = seed_query_end;

    if (seed_query_start > 0 && seed_ref_start > 0) {
        std::int64_t extension_len = std::min<std::int64_t>(seed_query_start, params.max_gap);
        outer_query_start = seed_query_start - extension_len;
        const std::int64_t bonus_extension =
            (extension_len * params.match_score > params.gap_open1)
                ? (extension_len * params.match_score - params.gap_open1) / params.gap_ext1
                : 0;
        extension_len += bonus_extension;
        extension_len = std::min<std::int64_t>(extension_len, params.max_gap);
        extension_len = std::min(extension_len, seed_ref_start);
        outer_ref_start = seed_ref_start - extension_len;
    }
    if (seed_query_end < query_length && seed_ref_end < target_length) {
        std::int64_t extension_len =
            std::min<std::int64_t>(query_length - seed_query_end, params.max_gap);
        outer_query_end = seed_query_end + extension_len;
        const std::int64_t bonus_extension =
            (extension_len * params.match_score > params.gap_open1)
                ? (extension_len * params.match_score - params.gap_open1) / params.gap_ext1
                : 0;
        extension_len += bonus_extension;
        extension_len = std::min<std::int64_t>(extension_len, params.max_gap);
        extension_len = std::min(extension_len, target_length - seed_ref_end);
        outer_ref_end = seed_ref_end + extension_len;
    }
    if (outer_query_end > query_length) {
        outer_query_end = query_length;
    }

    if (outer_query_start < 0 || outer_ref_start < 0 ||
        outer_query_end < outer_query_start || outer_ref_end < outer_ref_start ||
        outer_query_end > query_length || outer_ref_end > target_length) {
        throw std::invalid_argument{"alignment storage has unordered or out-of-range extents"};
    }
    const auto query_extent = static_cast<std::uint64_t>(outer_query_end - outer_query_start);
    const auto reference_extent = static_cast<std::uint64_t>(outer_ref_end - outer_ref_start);
    const auto cigar_steps = bioseq::common::checked_add(query_extent, reference_extent);
    const auto raw_reserve = cigar_steps
        ? bioseq::common::checked_add(*cigar_steps, std::uint64_t{16u})
        : std::optional<std::uint64_t>{};
    const auto cap_product = bioseq::common::checked_mul(
        static_cast<std::uint64_t>(params.max_gap), std::uint64_t{4u});
    const auto reserve_cap = cap_product
        ? bioseq::common::checked_add(*cap_product, std::uint64_t{64u})
        : std::optional<std::uint64_t>{};
    const auto initial_reserve = raw_reserve && reserve_cap
        ? std::optional<std::uint64_t>{std::min(*raw_reserve, *reserve_cap)}
        : std::optional<std::uint64_t>{};
    const auto capacity_elements = cigar_steps && initial_reserve
        ? std::optional<std::uint64_t>{std::max(*cigar_steps, *initial_reserve)}
        : std::optional<std::uint64_t>{};
    const auto cigar_bytes = capacity_elements
        ? bioseq::common::checked_mul(
              *capacity_elements, std::uint64_t{sizeof(cigar_operation)})
        : std::optional<std::uint64_t>{};
    if (!cigar_steps || !raw_reserve || !reserve_cap || !initial_reserve ||
        !capacity_elements || !cigar_bytes ||
        *cigar_steps > (std::numeric_limits<std::size_t>::max)() ||
        *cigar_bytes > (std::numeric_limits<std::size_t>::max)() ||
        *initial_reserve > (std::numeric_limits<std::size_t>::max)()) {
        throw std::length_error{"alignment storage CIGAR extent is not representable"};
    }
    return {
        .has_work = true,
        .anchor_begin = anchor_begin,
        .seed_ref_start = seed_ref_start,
        .seed_query_start = seed_query_start,
        .seed_ref_end = seed_ref_end,
        .seed_query_end = seed_query_end,
        .outer_ref_start = outer_ref_start,
        .outer_query_start = outer_query_start,
        .outer_ref_end = outer_ref_end,
        .outer_query_end = outer_query_end,
        .outer_query_extent = query_extent,
        .outer_reference_extent = reference_extent,
        .final_cigar_step_bound = static_cast<std::size_t>(*cigar_steps),
        .final_cigar_capacity_bytes = *cigar_bytes,
        .initial_cigar_reserve = static_cast<std::size_t>(*initial_reserve),
    };
}

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
/**
 * @brief Derive the largest actual fill shape selected for one region.
 *
 * Left, gap, and right calls are visited with the same predicates and band/
 * exact-max choices used by align_one_region. Sequential fills contribute a
 * maximum, not a sum: every kernel alignment arena scope rewinds before the
 * next fill. Packed result capacity is kept separate from context backing and
 * then added once to the concurrent temporary peak.
 */
template <base_alignment_algorithm p_base_alignment_algorithm>
[[nodiscard]] inline std::optional<selected_alignment_region_storage_plan>
derive_selected_alignment_region_storage_plan(
    const alignment_region& region,
    std::span<const anchor> anchors,
    const alignment_region_storage_plan& region_plan,
    const minimap2_parameters& params,
    std::int32_t align_bandwidth,
    const block_global_gap_certificate_plan& target_geometry) noexcept {
    static_assert(p_base_alignment_algorithm == base_alignment_algorithm::ksw2 ||
                  p_base_alignment_algorithm == base_alignment_algorithm::block_aligner);
    if (!region_plan.has_work) {
        return selected_alignment_region_storage_plan{};
    }
    if (region.seed_count <= 0 || region_plan.anchor_begin < 0 ||
        static_cast<std::size_t>(region_plan.anchor_begin) >= anchors.size()) {
        return std::nullopt;
    }
    const std::size_t anchor_begin =
        static_cast<std::size_t>(region_plan.anchor_begin);
    if (static_cast<std::size_t>(region.seed_count) > anchors.size() - anchor_begin) {
        return std::nullopt;
    }

    selected_alignment_region_storage_plan selected{.has_work = true};
    const bool is_dual_affine = params.gap_open1 != params.gap_open2 ||
                                params.gap_ext1 != params.gap_ext2;
    const bool block_trace_enabled = params.do_cigar || params.compute_edit_distance;
    const std::size_t block_lane_count = target_geometry.simd_lane_count;
    const std::size_t ksw2_byte_lane_count = target_geometry.ksw2_byte_lane_count;
    const std::size_t ksw2_full_score_bytes = target_geometry.ksw2_full_score_bytes;

    const auto record_fill = [&selected,
                              &params,
                              is_dual_affine,
                              block_trace_enabled,
                              block_lane_count,
                              ksw2_byte_lane_count,
                              ksw2_full_score_bytes](
                                 std::int64_t query_length,
                                 std::int64_t reference_length,
                                 std::int32_t band_width,
                                 bool exact_max_tracking) noexcept -> bool {
        if (query_length <= 0 || reference_length <= 0) {
            return true;
        }
        if (static_cast<std::uint64_t>(query_length) >
                (std::numeric_limits<std::size_t>::max)() ||
            static_cast<std::uint64_t>(reference_length) >
                (std::numeric_limits<std::size_t>::max)()) {
            return false;
        }
        const auto query_extent = static_cast<std::size_t>(query_length);
        const auto reference_extent = static_cast<std::size_t>(reference_length);
        std::uint64_t kernel_workspace = 0u;
        std::uint64_t result_capacity = 0u;
        if constexpr (p_base_alignment_algorithm == base_alignment_algorithm::block_aligner) {
            if (!is_block_aligner_lane_shape_supported(block_lane_count)) {
                return false;
            }
            const std::size_t maximum_block_size =
                std::max(params.block_max_size, block_lane_count);
            const auto plan = derive_block_aligner_storage_plan(
                query_extent,
                reference_extent,
                maximum_block_size,
                block_lane_count,
                block_trace_enabled);
            if (!plan) {
                return false;
            }
            kernel_workspace = plan->logical_alignment_bytes;
            result_capacity = plan->packed_cigar_bytes;
        } else {
            const auto plan = derive_ksw2_storage_plan(
                query_extent,
                reference_extent,
                ksw2_byte_lane_count,
                is_dual_affine,
                band_width,
                exact_max_tracking,
                ksw2_full_score_bytes,
                /*compute_cigar=*/true);
            if (!plan) {
                return false;
            }
            kernel_workspace = plan->logical_alignment_bytes;
            result_capacity = plan->packed_cigar_bytes;
        }
        const auto temporary_peak =
            bioseq::common::checked_add(kernel_workspace, result_capacity);
        if (!temporary_peak) {
            return false;
        }
        selected.maximum_kernel_workspace_bytes =
            std::max(selected.maximum_kernel_workspace_bytes, kernel_workspace);
        selected.maximum_result_capacity_bytes =
            std::max(selected.maximum_result_capacity_bytes, result_capacity);
        selected.temporary_workspace_bytes =
            std::max(selected.temporary_workspace_bytes, *temporary_peak);
        ++selected.fill_count;
        return true;
    };

    if (!record_fill(
            region_plan.seed_query_start - region_plan.outer_query_start,
            region_plan.seed_ref_start - region_plan.outer_ref_start,
            align_bandwidth,
            /*exact_max_tracking=*/true)) {
        return std::nullopt;
    }

    std::int64_t query_start = region_plan.seed_query_start;
    std::int64_t reference_start = region_plan.seed_ref_start;
    for (std::int32_t index = 1; index < region.seed_count; ++index) {
        const std::size_t anchor_index = anchor_begin + static_cast<std::size_t>(index);
        const anchor& current = anchors[anchor_index];
        const std::int64_t query_end = static_cast<std::int64_t>(current.query_position) + 1;
        const std::int64_t reference_end = static_cast<std::int64_t>(current.ref_position) + 1;
        const std::int64_t query_length = query_end - query_start;
        const std::int64_t reference_length = reference_end - reference_start;
        const bool is_last = index == region.seed_count - 1;
        if (!is_last &&
            (query_length < params.min_ksw_len || reference_length < params.min_ksw_len)) {
            continue;
        }
        if (query_length <= 0 || reference_length <= 0) {
            query_start = query_end;
            reference_start = reference_end;
            continue;
        }
        const std::int32_t gap_bandwidth = params.compute_edit_distance ? -1 : align_bandwidth;
        if (!record_fill(
                query_length,
                reference_length,
                gap_bandwidth,
                /*exact_max_tracking=*/p_base_alignment_algorithm ==
                    base_alignment_algorithm::block_aligner)) {
            return std::nullopt;
        }
        query_start = query_end;
        reference_start = reference_end;
    }

    if (!record_fill(
            region_plan.outer_query_end - region_plan.seed_query_end,
            region_plan.outer_ref_end - region_plan.seed_ref_end,
            align_bandwidth,
            /*exact_max_tracking=*/true)) {
        return std::nullopt;
    }
    return selected;
}

/** Choose the largest descending workspace prefix that fits current headroom. */
[[nodiscard]] inline std::size_t maximum_alignment_concurrency_from_workspaces(
    std::span<const std::uint64_t> descending_workspace_bytes,
    std::size_t requested_concurrency,
    std::uint64_t available_bytes) {
    if (!std::is_sorted(descending_workspace_bytes.begin(),
                        descending_workspace_bytes.end(),
                        std::greater<>{})) {
        throw std::invalid_argument{"alignment workspace bounds are not descending"};
    }
    const std::size_t bounded_request =
        std::min(requested_concurrency, descending_workspace_bytes.size());
    if (bounded_request == 0u) {
        return 0u;
    }
    std::uint64_t prefix_bytes = 0u;
    std::size_t admitted = 0u;
    for (; admitted < bounded_request; ++admitted) {
        const std::uint64_t next = descending_workspace_bytes[admitted];
        if (next > (std::numeric_limits<std::uint64_t>::max)() - prefix_bytes) {
            throw std::length_error{"alignment concurrent workspace sum overflowed"};
        }
        if (prefix_bytes > available_bytes || next > available_bytes - prefix_bytes) {
            break;
        }
        prefix_bytes += next;
    }
    return admitted;
}
#endif

/**
 * @brief Bound alignment workers using the existing coarse-region thresholds.
 *
 * The stage uses this same decision to allocate admitted worker slots only when
 * the lower kernel could use them. No alignment geometry or score is changed.
 */
[[nodiscard]] inline std::size_t alignment_worker_concurrency(
    std::span<const alignment_region> regions,
    bool task_parallelism_enabled,
    bool sequences_worker_shareable,
    std::size_t pool_maximum_concurrency) noexcept {
    if (!task_parallelism_enabled || !sequences_worker_shareable ||
        pool_maximum_concurrency <= 1u ||
        regions.size() < bioseq::parallelism::task_parallel_minimums::k_region_count) {
        return 1u;
    }
    auto remaining_seeds = bioseq::parallelism::task_parallel_minimums::k_seed_count;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
    std::size_t nonempty_region_count = 0u;
    bool seed_threshold_reached = false;
    for (const alignment_region& region : regions) {
        if (region.seed_count <= 0) {
            continue;
        }
        ++nonempty_region_count;
        const auto seeds = static_cast<std::size_t>(region.seed_count);
        if (!seed_threshold_reached && seeds >= remaining_seeds) {
            seed_threshold_reached = true;
            remaining_seeds = 0u;
        } else if (!seed_threshold_reached) {
            remaining_seeds -= seeds;
        }
    }
    return seed_threshold_reached
        ? std::min(pool_maximum_concurrency, nonempty_region_count)
        : 1u;
#else
    for (const alignment_region& region : regions) {
        if (region.seed_count <= 0) {
            continue;
        }
        const auto seeds = static_cast<std::size_t>(region.seed_count);
        if (seeds >= remaining_seeds) {
            return std::min(pool_maximum_concurrency, regions.size());
        }
        remaining_seeds -= seeds;
    }
    return 1u;
#endif
}

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
/**
 * @brief Derive and sort selected-kernel workspace bounds, then cap fanout.
 *
 * This function owns no permit. The returned count is only a logical upper
 * bound; each worker context still admits its allocator-reported physical
 * blocks through its private installed hook, so context backing is never
 * charged a second time.
 */
template <base_alignment_algorithm p_base_alignment_algorithm>
[[nodiscard]] inline std::size_t admitted_alignment_worker_concurrency(
    std::span<const alignment_region> regions,
    std::span<const anchor> anchors,
    std::uint64_t query_size,
    std::uint64_t target_size,
    const minimap2_parameters& params,
    std::int32_t align_bandwidth,
    const block_global_gap_certificate_plan& target_geometry,
    bool task_parallelism_enabled,
    bool sequences_worker_shareable,
    std::size_t pool_maximum_concurrency,
    std::uint64_t available_bytes,
    std::span<std::uint64_t> workspace_scratch) {
    static_assert(p_base_alignment_algorithm == base_alignment_algorithm::ksw2 ||
                  p_base_alignment_algorithm == base_alignment_algorithm::block_aligner);
    const std::size_t requested = alignment_worker_concurrency(
        regions,
        task_parallelism_enabled,
        sequences_worker_shareable,
        pool_maximum_concurrency);
    if (requested <= 1u) {
        return std::any_of(
                   regions.begin(), regions.end(),
                   [](const alignment_region& region) noexcept {
                       return region.seed_count > 0;
                   })
            ? requested
            : 0u;
    }

    std::size_t nonempty_count = 0u;
    for (const alignment_region& region : regions) {
        if (region.seed_count <= 0) {
            continue;
        }
        if (nonempty_count >= workspace_scratch.size()) {
            throw std::length_error{"alignment workspace scratch is too small"};
        }
        const alignment_region_storage_plan region_plan =
            derive_alignment_region_storage_plan(
                region, anchors, query_size, target_size, params);
        const auto selected_plan =
            derive_selected_alignment_region_storage_plan<p_base_alignment_algorithm>(
                region,
                anchors,
                region_plan,
                params,
                align_bandwidth,
                target_geometry);
        if (!selected_plan || !selected_plan->has_work) {
            throw std::length_error{
                "selected alignment worker storage geometry is not representable"};
        }
        workspace_scratch[nonempty_count++] = selected_plan->temporary_workspace_bytes;
    }
    auto descending = workspace_scratch.first(nonempty_count);
    std::sort(descending.begin(), descending.end(), std::greater<>{});
    return maximum_alignment_concurrency_from_workspaces(
        std::span<const std::uint64_t>{descending.data(), descending.size()},
        requested,
        available_bytes);
}
#endif

/// Align every admitted region through the compile-time selected kernel.
template <base_alignment_algorithm p_base_alignment_algorithm,
          bool p_enable_diagonal_certificate = true,
          sequence_alphabet p_alphabet>
void align_regions(std::span<alignment_region> regions,
                   std::span<const anchor> anchors,
                   bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
                   bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
                   const scoring_matrix<p_alphabet>& scoring_matrix,
                   const alignment_kernels<p_alphabet>& alignment_kernels,
                   const minimap2_parameters& params,
                   block_aligner_execution_stats* block_execution_stats = nullptr,
                   scalar_pipeline_execution_stats* scalar_execution_stats = nullptr,
                   bioseq::parallelism::worker_context_factory worker_factory = {}) {
    static const auto* _desc = bioseq::instrumentation::register_static_region("mm_align_skeleton");
    auto _scope = bioseq::instrumentation::region(_desc);
    // — lengths are full 64-bit (no int32 truncation of bp_size).
    // Region coordinate math below is signed int64 so the extension/clamp logic
    // and the reverse-strand mirror keep working for sequences longer than 2^31.
    constexpr auto signed_coordinate_limit =
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
    if (query_seq.bp_size() > signed_coordinate_limit ||
        target_seq.bp_size() > signed_coordinate_limit) {
        throw std::length_error{"alignment sequence exceeds the signed fill domain"};
    }
    const std::int64_t query_length = static_cast<std::int64_t>(query_seq.bp_size());
    const std::int64_t target_length = static_cast<std::int64_t>(target_seq.bp_size());
    const std::int32_t align_bandwidth =
        static_cast<std::int32_t>(params.bandwidth * 1.5 + 1.0);
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    const auto tail_storage_plan = derive_alignment_tail_storage_plan(
        regions.size(),
        p_base_alignment_algorithm == base_alignment_algorithm::block_aligner &&
            block_execution_stats != nullptr,
        p_base_alignment_algorithm == base_alignment_algorithm::ksw2 &&
            scalar_execution_stats != nullptr);
    if (!tail_storage_plan) {
        throw std::length_error{"alignment tail region count is not representable"};
    }
#endif

    // Construct the non-capturing `align_one_region` functor once per region
    // batch with kernel-invariant references. The per-region call surface is
    // `aligner.template run<p_is_reversed>(...)`.
    const align_one_region<p_base_alignment_algorithm, p_alphabet, p_enable_diagonal_certificate>
        aligner{query_seq, target_seq, scoring_matrix, alignment_kernels, params, align_bandwidth};

    // stats collection is opt-in, so the normal benchmark path allocates no
    // diagnostic storage. When enabled, each worker writes one disjoint slot;
    // the deterministic reduction below avoids atomics in the DP hot path.
    alignment_result_vector<block_aligner_execution_stats> per_region_block_stats;
    if constexpr (p_base_alignment_algorithm == base_alignment_algorithm::block_aligner) {
        if (block_execution_stats != nullptr) {
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            per_region_block_stats.set_logical_bounds(
                tail_storage_plan->block_stats_count,
                tail_storage_plan->block_stats_count);
#endif
            per_region_block_stats.resize(regions.size());
        }
    }
    alignment_result_vector<scalar_pipeline_execution_stats> per_region_scalar_stats;
    if constexpr (p_base_alignment_algorithm == base_alignment_algorithm::ksw2) {
        if (scalar_execution_stats != nullptr) {
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            per_region_scalar_stats.set_logical_bounds(
                tail_storage_plan->scalar_stats_count,
                tail_storage_plan->scalar_stats_count);
#endif
            per_region_scalar_stats.resize(regions.size());
        }
    }

    auto align_one_region_by_index = [query_length,
                                      target_length,
                                      &regions,
                                      &anchors,
                                      &params,
                                      &aligner,
                                      &per_region_block_stats,
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                                      // Region workers do not inherit caller-thread attribution.
                                      benchmark_e2e_operation = current_benchmark_e2e_operation_binding(),
#endif
                                      &per_region_scalar_stats](std::size_t region_index) {
        alignment_region& region = regions[region_index];
        if (region.seed_count <= 0) {
            return;
        }

        const auto storage_plan = derive_alignment_region_storage_plan(
            region, anchors, static_cast<std::uint64_t>(query_length),
            static_cast<std::uint64_t>(target_length), params);
        const auto anchor_begin = storage_plan.anchor_begin;
        const auto seed_ref_start = storage_plan.seed_ref_start;
        const auto seed_query_start = storage_plan.seed_query_start;
        const auto seed_ref_end = storage_plan.seed_ref_end;
        const auto seed_query_end = storage_plan.seed_query_end;
        const auto outer_ref_start = storage_plan.outer_ref_start;
        const auto outer_query_start = storage_plan.outer_query_start;
        const auto outer_ref_end = storage_plan.outer_ref_end;
        const auto outer_query_end = storage_plan.outer_query_end;

        // Preserve the existing bounded initial reserve; the completed logical
        // extent is separately bounded by storage_plan.final_cigar_step_bound.
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        region.cigar.set_logical_bounds(
            storage_plan.final_cigar_step_bound,
            std::max(storage_plan.final_cigar_step_bound,
                     storage_plan.initial_cigar_reserve));
#endif
        region.cigar.reserve(storage_plan.initial_cigar_reserve);

        region.cigar.clear();
        region.dp_score = 0;
        region.edit_distance = 0;

        block_aligner_execution_stats* region_block_stats = nullptr;
        if constexpr (p_base_alignment_algorithm == base_alignment_algorithm::block_aligner) {
            if (!per_region_block_stats.empty()) {
                region_block_stats = &per_region_block_stats[region_index];
                region_block_stats->add(region_block_stats->routed_region_scopes);
                const block_alignment_eligibility expected =
                    derive_block_stage_eligibility(region,
                                                   anchors,
                                                   anchor_begin,
                                                   seed_ref_start,
                                                   seed_query_start,
                                                   seed_ref_end,
                                                   seed_query_end,
                                                   outer_ref_start,
                                                   outer_query_start,
                                                   outer_ref_end,
                                                   outer_query_end,
                                                   params);
                region_block_stats->add(region_block_stats->left_route_candidates, expected.left);
                region_block_stats->add(region_block_stats->gap_route_candidates, expected.gap);
                region_block_stats->add(region_block_stats->right_route_candidates, expected.right);
            }
        }
        scalar_pipeline_execution_stats* region_scalar_stats = nullptr;
        if constexpr (p_base_alignment_algorithm == base_alignment_algorithm::ksw2) {
            if (!per_region_scalar_stats.empty()) {
                region_scalar_stats = &per_region_scalar_stats[region_index];
            }
        }

        // Per-region diagnostics are derived from `region.cigar.empty()` and
        // the seed/outer spans during debugger inspection. Avoiding a formatted
        // log at every boundary keeps measurement runs free of per-region
        // logging overhead.

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        // KSW2 and Block Aligner fuse base-alignment DP, trace storage,
        // traceback, and CIGAR construction. The interval therefore begins at
        // the fused invocation and remains live through the CIGAR-derived
        // result fields below; it is deliberately not labelled traceback-only.
        constexpr bool benchmark_e2e_supported_cigar_algorithm =
            p_base_alignment_algorithm == base_alignment_algorithm::ksw2 ||
            p_base_alignment_algorithm == base_alignment_algorithm::block_aligner;
        constexpr benchmark_e2e_interval_algorithm benchmark_e2e_algorithm =
            p_base_alignment_algorithm == base_alignment_algorithm::block_aligner
                ? benchmark_e2e_interval_algorithm::block_aligner
                : benchmark_e2e_interval_algorithm::ksw2;
        std::optional<benchmark_e2e_interval_scope> benchmark_e2e_cigar_scope;
#endif

        // Dispatch the three alignment stages through the non-capturing
        // aligner functor selected by strand orientation.
        if (region.is_reverse) {
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            if constexpr (benchmark_e2e_supported_cigar_algorithm) {
                if (params.do_cigar) {
                    benchmark_e2e_cigar_scope.emplace(
                        params.downstream_active_wall,
                        true,
                        benchmark_e2e_interval_kind::cigar,
                        benchmark_e2e_algorithm,
                        benchmark_e2e_operation.origin,
                        benchmark_e2e_operation.is_available);
                }
            }
#endif
            aligner.template run<true>(region,
                                       anchors,
                                       anchor_begin,
                                       seed_ref_start,
                                       seed_query_start,
                                       seed_ref_end,
                                       seed_query_end,
                                       outer_ref_start,
                                       outer_query_start,
                                       outer_ref_end,
                                       outer_query_end,
                                       query_length,
                                       region_block_stats,
                                       region_scalar_stats);
        } else {
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            if constexpr (benchmark_e2e_supported_cigar_algorithm) {
                if (params.do_cigar) {
                    benchmark_e2e_cigar_scope.emplace(
                        params.downstream_active_wall,
                        true,
                        benchmark_e2e_interval_kind::cigar,
                        benchmark_e2e_algorithm,
                        benchmark_e2e_operation.origin,
                        benchmark_e2e_operation.is_available);
                }
            }
#endif
            aligner.template run<false>(region,
                                        anchors,
                                        anchor_begin,
                                        seed_ref_start,
                                        seed_query_start,
                                        seed_ref_end,
                                        seed_query_end,
                                        outer_ref_start,
                                        outer_query_start,
                                        outer_ref_end,
                                        outer_query_end,
                                        query_length,
                                        region_block_stats,
                                        region_scalar_stats);
        }

        // This is a geometry assertion, not an exact CIGAR or algorithm-parity
        // constraint. Every emitted operation consumes at least one sequence step.
        if (region.cigar.size() > storage_plan.final_cigar_step_bound) {
            throw std::length_error{"alignment CIGAR exceeded its sequence-step storage bound"};
        }

        // Compute dp_max_score from CIGAR (simplified mm_update_extra)
        region.dp_max_score = region.dp_score;
        region.has_alignment = true;

        // Compute match_length/block_length from CIGAR
        std::int32_t cigar_match_count = 0;
        std::int32_t cigar_block_count = 0;
        for (const auto& cigar_op : region.cigar) {
            const std::int32_t op_len = static_cast<std::int32_t>(cigar_op.length);
            if (cigar_op.opcode == cigar_match_opcode || cigar_op.opcode == cigar_equal_opcode) {
                cigar_match_count += op_len;
                cigar_block_count += op_len;
            } else if (cigar_op.opcode == cigar_mismatch_opcode) {
                cigar_block_count += op_len;
            } else if (cigar_op.opcode == cigar_insertion_opcode ||
                       cigar_op.opcode == cigar_deletion_opcode) {
                cigar_block_count += op_len;
            }
        }
        if (cigar_match_count > 0) {
            region.match_length = cigar_match_count;
            const std::int32_t effective_block =
                (cigar_block_count > 0) ? cigar_block_count : region.block_length;
            region.block_length = effective_block;
        }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        // Sample the exclusive endpoint immediately after all required
        // CIGAR-derived result fields have been materialized.
        benchmark_e2e_cigar_scope.reset();
#endif
    };

    const auto& pool = bioseq::parallelism::global_pool::instance();
    const std::size_t coarse_worker_count = alignment_worker_concurrency(
        regions,
        bioseq::parallelism::has_parallel_tasks(pool.get_mode()) &&
            p_base_alignment_algorithm != base_alignment_algorithm::ksw2_batched,
        bioseq::storage::is_worker_shareable(query_seq) &&
            bioseq::storage::is_worker_shareable(target_seq),
        static_cast<std::size_t>(pool.maximum_concurrency()));
    const std::size_t effective_worker_count = worker_factory.maximum_concurrency == 0u
        ? coarse_worker_count
        : std::min(coarse_worker_count, worker_factory.maximum_concurrency);
    const bool may_parallel = effective_worker_count > 1u;

    if (may_parallel) {
        const auto align_range = [&align_one_region_by_index](std::size_t lo, std::size_t hi) {
            for (std::size_t region_index = lo; region_index < hi; ++region_index) {
                align_one_region_by_index(region_index);
            }
        };
        if (worker_factory.state != nullptr) {
            bioseq::parallelism::parallel_for_pipeline(
                0u, regions.size(), 1u,
                bioseq::parallelism::task_parallel_minimums::k_region_count,
                bioseq::parallelism::task_parallel_region::align_regions,
                align_range, worker_factory);
        } else {
            bioseq::parallelism::parallel_for_pipeline(
                0u, regions.size(), 1u,
                bioseq::parallelism::task_parallel_minimums::k_region_count,
                bioseq::parallelism::task_parallel_region::align_regions, align_range);
        }
    } else {
        //  — liveness heartbeat. A single big region (many gap-fills over a
        // multi-Mbp seed span) can dominate wall-clock; the per-stage milestones
        // alone would show a long silence with no sign of progress. Emit (and flush)
        // a heartbeat at most ~once/second so `tail -f` of the log shows which
        // region index is in flight and the live footprint. The logging facade
        // reads PostgreSQL's active flag when installed and the standalone
        // fallback otherwise, preserving this module's domain-neutral closure.
        const bool debug_logging_enabled = is_bioseq_debug_logging_enabled();
        std::int64_t hb_last_ns =
            debug_logging_enabled ? bioseq::instrumentation::clock::now_ns() : 0;
        std::size_t hb_region_idx = 0;
        for (std::size_t region_index = 0; region_index < regions.size(); ++region_index) {
            ++hb_region_idx;
            if (debug_logging_enabled) {
                const std::int64_t hb_now_ns = bioseq::instrumentation::clock::now_ns();
                if (hb_now_ns - hb_last_ns > 1'000'000'000LL) {
                    hb_last_ns = hb_now_ns;
                    bioseq_log_lazy<logging_level::debug>([hb_region_idx, &regions, region_index] {
                        bioseq_log_progress<logging_level::debug>(
                            "[minimap_port] aligning region {}/{} seed_count={}"
                            " | footprint={}MB avail={}MB",
                            hb_region_idx,
                            regions.size(),
                            regions[region_index].seed_count,
                            bioseq::common::memory::process_footprint_bytes() >> 20,
                            bioseq::common::memory::system_available_bytes() >> 20);
                    });
                }
            }
            align_one_region_by_index(region_index);
        }
    }

    if constexpr (p_base_alignment_algorithm == base_alignment_algorithm::block_aligner) {
        if (block_execution_stats != nullptr) {
            *block_execution_stats = {};
            for (const block_aligner_execution_stats& region_stats : per_region_block_stats) {
                block_execution_stats->merge(region_stats);
            }
            block_execution_stats->add(block_execution_stats->align_region_invocations);
            if (may_parallel) {
                block_execution_stats->add(block_execution_stats->parallel_align_region_dispatches);
            } else {
                block_execution_stats->add(block_execution_stats->serial_align_region_dispatches);
            }
            if (!block_execution_stats->is_internally_consistent()) {
                bioseq_log<logging_level::error>(
                    "Block Aligner execution stats rejected: eligible={}, "
                    "executed={}, ksw2={}, scalar_fallbacks={}",
                    block_execution_stats->eligible_calls(),
                    block_execution_stats->executed_calls(),
                    block_execution_stats->forbidden_ksw2_calls,
                    block_execution_stats->forbidden_scalar_biological_fallbacks);
                std::abort();
            }
        }
    }
    if constexpr (p_base_alignment_algorithm == base_alignment_algorithm::ksw2) {
        if (scalar_execution_stats != nullptr) {
            *scalar_execution_stats = {};
            for (const scalar_pipeline_execution_stats& region_stats : per_region_scalar_stats) {
                scalar_execution_stats->merge(region_stats);
            }
        }
    }

    bioseq_log_lazy<logging_level::debug>([&regions] {
        bioseq_log_progress<logging_level::debug>(
            "[minimap_port] Aligned {} regions | footprint={}MB avail={}MB",
            regions.size(),
            bioseq::common::memory::process_footprint_bytes() >> 20,
            bioseq::common::memory::system_available_bytes() >> 20);
    });
}

} // namespace bioseq::simd
