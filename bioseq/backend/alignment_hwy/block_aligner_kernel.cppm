/**
 * @file block_aligner_kernel.cppm
 * @brief Run BlockAligner for one CPU instruction set and collect its execution statistics.
 */
module;

#include <hwy/highway.h>
#include <hwy/aligned_allocator.h>

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <array>
#include <algorithm>
#include <bit>
#include <limits>
#include <type_traits>
#include <concepts>
#include <tuple>
#include <string>
#include <string_view>
#include <utility>

export module bioseq.highway.sources.block_aligner;

import std;
import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.cigar;
import bioseq.alignment.contracts;
import bioseq.highway.sources.alignment_utils;
import bioseq.highway.sources.block_aligner_dp;
import bioseq.highway.sources.block_aligner_traceback;
import bioseq.highway.sources.block_aligner_certificates;
import bioseq.common.context;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;

export namespace bioseq::simd {

namespace [[bioseq::simd]] target {
namespace hn = hwy::HWY_NAMESPACE;

/**
 * Count a validated alignment request before choosing how to execute it.
 *
 * Record the extension stage, alignment mode, input order and scoring identity.
 * For reversed nonempty inputs, also count the symbols expected to be encoded.
 * These totals can be compared with the execution counters.
 */
inline void record_block_aligner_eligibility(
        block_aligner_execution_stats& stats,
        const block_aligner_parameters& parameters,
        std::size_t query_length,
        std::size_t target_length,
        std::uint64_t configuration_identity) noexcept {
    switch (parameters.stage) {
    case block_alignment_stage::left_extension: stats.add(stats.left_eligible); break;
    case block_alignment_stage::gap_extension: stats.add(stats.gap_eligible); break;
    case block_alignment_stage::right_extension: stats.add(stats.right_eligible); break;
    }
    if (parameters.mode == block_alignment_mode::global) stats.add(stats.global_calls);
    else stats.add(stats.native_x_drop_calls);
    if (parameters.is_trace_enabled) stats.add(stats.trace_calls);
    if (parameters.input_order == block_input_order::reversed) {
        stats.add(stats.reversed_input_calls);
        if (query_length != 0 && target_length != 0) {
            stats.add(stats.expected_reversed_compact_symbols, query_length);
            stats.add(stats.expected_reversed_compact_symbols, target_length);
        }
    } else {
        stats.add(stats.forward_input_calls);
    }
    stats.add(stats.configuration_identity_calls);
    stats.configuration_identity_xor ^= configuration_identity;
    stats.configuration_identity_sum += configuration_identity;
}

/**
 * Add the completed call to the execution statistics.
 *
 * Count empty inputs, SIMD alignment work and successful diagonal shortcuts
 * separately. Copy the block growth, shrink and X-drop counters, and mark the
 * aggregate as saturated when the core counters or lane count are invalid.
 */
inline void record_block_aligner_execution(
        block_aligner_execution_stats& stats,
        const block_aligner_parameters& parameters,
        std::size_t lane_count,
        bool is_zero_work,
        const block_core_stats& core_stats,
        std::uint64_t configuration_identity) noexcept {
    switch (parameters.stage) {
    case block_alignment_stage::left_extension: stats.add(stats.left_executed); break;
    case block_alignment_stage::gap_extension: stats.add(stats.gap_executed); break;
    case block_alignment_stage::right_extension: stats.add(stats.right_executed); break;
    }
    if (parameters.mode == block_alignment_mode::global) {
        stats.add(stats.executed_global_calls);
    } else {
        stats.add(stats.executed_native_x_drop_calls);
    }
    if (parameters.is_trace_enabled) stats.add(stats.executed_trace_calls);
    if (parameters.input_order == block_input_order::reversed) {
        stats.add(stats.executed_reversed_input_calls);
    } else {
        stats.add(stats.executed_forward_input_calls);
    }
    if (lane_count == 8) stats.add(stats.simd_eight_lane_calls);
    else if (lane_count == 16) stats.add(stats.simd_sixteen_lane_calls);
    else stats.is_saturated = true;
    if (is_zero_work) {
        stats.add(stats.zero_work_calls);
    } else {
        stats.add(stats.nonempty_calls);
        const bool was_simd_dp_executed = core_stats.vector_batches != 0
            && core_stats.cells != 0;
        const bool was_simd_certificate_executed = core_stats.is_diagonal_certified
            && core_stats.diagonal_certificate_vector_batches != 0
            && core_stats.diagonal_certificate_symbols != 0;
        if ((was_simd_dp_executed || was_simd_certificate_executed)
            && !core_stats.is_saturated) {
            stats.add(stats.nonempty_calls_with_simd);
        }
    }
    stats.add(stats.simd_dp_vector_batches, core_stats.vector_batches);
    stats.add(stats.simd_dp_cells, core_stats.cells);
    if (core_stats.is_diagonal_certified) {
        stats.add(stats.simd_diagonal_certificate_calls);
    }
    stats.add(stats.simd_diagonal_certificate_vector_batches,
        core_stats.diagonal_certificate_vector_batches);
    stats.add(stats.simd_diagonal_certificate_symbols,
        core_stats.diagonal_certificate_symbols);
    stats.add(stats.adaptive_grow_events, core_stats.grow_events);
    stats.add(stats.adaptive_shrink_events, core_stats.shrink_events);
    if (core_stats.is_x_drop_terminated) {
        stats.add(stats.native_x_drop_terminations);
    }
    stats.is_saturated = stats.is_saturated || core_stats.is_saturated;
    stats.add(stats.executed_configuration_identity_calls);
    stats.executed_configuration_identity_xor ^= configuration_identity;
    stats.executed_configuration_identity_sum += configuration_identity;
}

/**
 * Count and abort an attempt to run BlockAligner with an unsupported SIMD lane count.
 * @param[in,out] stats Execution counters for the alignment batch.
 */
[[noreturn]] inline void abort_forbidden_scalar_biological_fallback(
        block_aligner_execution_stats& stats) {
    stats.add(stats.forbidden_scalar_biological_fallbacks);
    bioseq_log<logging_level::error>(
        "Block Aligner attempted forbidden scalar biological fallback");
    std::abort();
}

// BlockAligner for the selected CPU instruction set.

/** BlockAligner entry points compiled for one CPU instruction set. */
class highway_block_aligner {
public:
    /**
     * Check the scoring matrix and BlockAligner settings once for repeated gap checks.
     *
     * The returned plan stores the scoring identity, SIMD lane count and match and
     * mismatch scores needed to test whether a gap can be aligned without insertions
     * or deletions. Unsupported settings are recorded in the plan.
     *
     * @tparam p_alphabet Sequence alphabet.
     * @tparam p_alphabet_dimension Matrix size, including the wildcard rank.
     * @param scoring_matrix Scores for symbol pairs.
     * @param parameters Alignment settings to save in the plan.
     */
    template<
        sequence_alphabet p_alphabet,
        size_t p_alphabet_dimension = seqan3::alphabet_size<p_alphabet> + 1u>
    [[nodiscard]] static block_global_gap_certificate_plan
    prepare_global_gap_certificate(
            const scoring_matrix<p_alphabet>& scoring_matrix,
            const block_aligner_parameters& parameters) noexcept {
        block_global_gap_certificate_plan plan{
            .parameters = parameters,
            .scoring_identity = scoring_matrix.block_semantic_identity(),
        };
        const size_t lane_count = hn::Lanes(block_aligner_simd_tag{});
        plan.simd_lane_count = static_cast<std::uint8_t>(lane_count);
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
        using ksw2_byte_tag = hn::ScalableTag<ksw2_partial_score_type>;
        using ksw2_full_tag = hn::ScalableTag<ksw2_full_score_type>;
        static_assert(std::same_as<hn::TFromD<ksw2_byte_tag>, ksw2_partial_score_type>);
        static_assert(std::same_as<hn::TFromD<ksw2_full_tag>, ksw2_full_score_type>);
        plan.ksw2_byte_lane_count =
            hn::Lanes(ksw2_byte_tag{});
        plan.ksw2_full_score_bytes = sizeof(hn::TFromD<ksw2_full_tag>);
#endif
        plan.is_contract_supported = scoring_matrix.is_block_layout_valid()
            && validate_block_aligner_contract(parameters, lane_count)
                == block_aligner_contract_error::none;
        plan.is_generated_matrix_supported =
            has_generated_diagonal_score_layout<p_alphabet, p_alphabet_dimension>(
                scoring_matrix, plan.match_score, plan.mismatch_score);
        return plan;
    }

    /**
     * Try to prove that this gap needs no insertions or deletions.
     *
     * Read resident packed DNA4 directly. The result says whether the check could
     * run, ran without proving the diagonal best, or proved it and supplies the
     * score and edit count. The caller can use dynamic programming when it fails.
     *
     * The query and target intervals are half-open. p_is_reversed selects the
     * query orientation, and plan contains the checked scoring settings.
     */
    template<
        bool p_is_reversed,
        sequence_alphabet p_alphabet>
    [[nodiscard]] static HWY_INLINE block_packed_certificate_attempt
    try_packed_global_gap(
            bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
            bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
            size_t query_start,
            size_t query_end,
            size_t target_start,
            size_t target_end,
            const block_global_gap_certificate_plan& plan) noexcept {
        return try_packed_global_gap_certificate<p_is_reversed>(
            query_seq, target_seq,
            query_start, query_end, target_start, target_end, plan);
    }

    /**
     * Align the requested query and target intervals with BlockAligner.
     *
     * Global mode joins both ends of the intervals, while X-drop mode extends
     * from the start until the score falls too far below the best score.
     * An optional ungapped check can skip DP.
     *
     * Intervals are half-open. p_is_reversed selects the query orientation, while
     * parameters.input_order controls the direction in which both inputs are read.
     * Gap penalties are positive. A gap of length n costs gap_open + n * gap_extend.
     *
     * The result owns its CIGAR, and parameters select trace and edit-count output
     * while the optional counters record execution work and scratch allocation counts.
     * Invalid coordinates, scoring layouts and BlockAligner settings abort execution.
     */
    template<
        bool p_is_reversed,
        bool p_is_diagonal_certificate_enabled = true,
        sequence_alphabet p_alphabet,
        size_t p_alphabet_dimension = seqan3::alphabet_size<p_alphabet> + 1u>
    [[nodiscard]] static ksw_alignment_result align(
        bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
        bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
        size_t query_start,
        size_t query_end,
        size_t target_start,
        size_t target_end,
        const scoring_matrix<p_alphabet>& scoring_matrix,
        const block_aligner_parameters& parameters,
        block_aligner_execution_stats* execution_stats = nullptr,
        block_aligner_construction_stats* construction_stats = nullptr)
    {
        static_assert(
            p_alphabet_dimension
                == seqan3::alphabet_size<p_alphabet> + 1u,
            "Block Aligner scoring/padding alphabet dimension must match input");

        ksw_alignment_result result;

        const block_aligner_simd_tag simd_tag;
        const size_t lane_count = hn::Lanes(simd_tag);
        if (execution_stats != nullptr
            && !is_block_aligner_lane_shape_supported(lane_count)) {
            // This BlockAligner requires SIMD. Stop before allocating or aligning.
            abort_forbidden_scalar_biological_fallback(
                *execution_stats);
        }
        const auto contract_error = validate_block_aligner_contract(parameters, lane_count);
        const bool is_scoring_matrix_valid = scoring_matrix.is_block_layout_valid();
        const bool are_coordinates_ordered = query_end >= query_start
            && target_end >= target_start;
        const size_t query_length = are_coordinates_ordered
            ? query_end - query_start : 0;
        const size_t target_length = are_coordinates_ordered
            ? target_end - target_start : 0;
        constexpr size_t max_result_coordinate = static_cast<size_t>(
            std::numeric_limits<int32_t>::max());
        constexpr size_t max_packed_cigar_run = static_cast<size_t>(
            std::numeric_limits<std::uint32_t>::max() >> 4u);
        const bool are_coordinates_valid = are_coordinates_ordered
            && query_end <= query_seq.bp_size()
            && target_end <= target_seq.bp_size()
            && query_length <= max_result_coordinate
            && target_length <= max_result_coordinate
            && (!parameters.is_trace_enabled
                || (query_length <= max_packed_cigar_run
                    && target_length <= max_packed_cigar_run));
        const auto effective_contract_error = !are_coordinates_valid
            ? block_aligner_contract_error::coordinates
            : !is_scoring_matrix_valid
                ? block_aligner_contract_error::scoring_matrix
                : contract_error;
        if (effective_contract_error != block_aligner_contract_error::none) {
            if (execution_stats != nullptr) {
                execution_stats->add(
                    execution_stats->unsupported_contract_rejections);
            }
            bioseq_log<logging_level::error>(
                "Block Aligner rejected contract (error={}, are_coordinates_valid={})",
                static_cast<unsigned>(effective_contract_error),
                are_coordinates_valid);
            std::abort();
        }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        if (query_length > (std::numeric_limits<size_t>::max)() - target_length) {
            abort_block_aligner_invariant("Block result CIGAR bound overflow");
        }
        result.cigar.set_logical_bounds(
            query_length + target_length, query_length + target_length);
#endif
        const std::uint64_t configuration_identity =
            ::bioseq::simd::block_aligner_configuration_identity(
                parameters, scoring_matrix.block_semantic_identity(), p_is_reversed);
        if (execution_stats != nullptr) {
            record_block_aligner_eligibility(
                *execution_stats, parameters, query_length, target_length,
                configuration_identity);
        }

        if (query_length == 0 || target_length == 0) {
            if (parameters.mode == block_alignment_mode::x_drop) {
                result.score = 0;
                result.max = 0;
            } else {
                const size_t gap_len = std::max(query_length, target_length);
                result.score = gap_len == 0 ? 0 : -(
                    static_cast<int32_t>(parameters.gap_open)
                    + static_cast<int32_t>(parameters.gap_extend) * gap_len);
                result.max = result.score;
                result.max_q = static_cast<int32_t>(query_length) - 1;
                result.max_t = static_cast<int32_t>(target_length) - 1;
                if (parameters.is_trace_enabled && gap_len != 0) {
                    const std::uint32_t opcode = query_length == 0
                        ? cigar_deletion_opcode : cigar_insertion_opcode;
                    result.cigar.push_back(
                        (static_cast<std::uint32_t>(gap_len) << 4u) | opcode);
                }
                result.has_reached_end = true;
                result.mqe = result.score;
                result.mqe_t = result.max_t;
                result.mte = result.score;
                result.mte_q = result.max_q;
            }
            if (execution_stats != nullptr) {
                record_block_aligner_execution(
                    *execution_stats, parameters, lane_count, true, block_core_stats{},
                    configuration_identity);
            }
            if (parameters.should_compute_edit_distance) {
                result.edit_distance = std::max(query_length, target_length);
                result.is_edit_distance_valid = true;
            }
            return result;
        }

        block_core_stats core_stats;
        if constexpr (p_is_diagonal_certificate_enabled) {
            const block_diagonal_certificate certificate =
                try_global_diagonal_certificate<
                    p_is_reversed, p_alphabet, p_alphabet_dimension>(
                        query_seq, target_seq,
                        query_start, query_end, target_start, target_end,
                        scoring_matrix, parameters);
            if (certificate.is_certified) {
                result.score = certificate.score;
                result.max = certificate.score;
                result.max_q = static_cast<int32_t>(query_length) - 1;
                result.max_t = static_cast<int32_t>(target_length) - 1;
                result.mqe = certificate.score;
                result.mqe_t = result.max_t;
                result.mte = certificate.score;
                result.mte_q = result.max_q;
                result.has_reached_end = true;
                if (parameters.is_trace_enabled) {
                    result.cigar.push_back(
                        (static_cast<std::uint32_t>(query_length) << 4u)
                        | cigar_match_opcode);
                }
                if (parameters.should_compute_edit_distance) {
                    result.edit_distance = certificate.edit_distance;
                    result.is_edit_distance_valid = true;
                }
                core_stats.is_diagonal_certified = true;
                core_stats.diagonal_certificate_vector_batches =
                    certificate.vector_batches;
                core_stats.diagonal_certificate_symbols = certificate.symbols;
                if (execution_stats != nullptr) {
                    record_block_aligner_execution(
                        *execution_stats, parameters, lane_count, false, core_stats,
                        configuration_identity);
                }
                return result;
            }
        }

        // Arena-backed padded, border, scratch, and trace storage is scoped to
        // this alignment. The result owns its CIGAR independently.
        if (construction_stats != nullptr) {
            ++construction_stats->align_arena_scopes;
        }
        auto align_scope = context::current().push<context::arena::alignment>();

        const size_t minimum_block_size = std::max(parameters.minimum_block_size, lane_count);
        const size_t maximum_block_size = std::max(parameters.maximum_block_size, lane_count);
        const auto storage_plan = require_block_storage_plan(
            query_length, target_length, maximum_block_size, parameters.is_trace_enabled);

        // The block recurrence uses negative costs for opening and extending gaps.
        const int8_t negative_gap_open = static_cast<int8_t>(
            -(static_cast<int>(parameters.gap_open) + parameters.gap_extend));
        const int8_t negative_gap_extension = static_cast<int8_t>(-parameters.gap_extend);

        block_padded_bytes query_padded, target_padded;
        if (construction_stats != nullptr) {
            construction_stats->padded_sequence_allocations += 2u;
        }
        if (parameters.input_order == block_input_order::reversed) {
            // Decode both inputs in reverse order directly into the DP buffers.
            query_padded.template encode_region<p_is_reversed, true, p_alphabet_dimension>(
                query_seq, query_start, query_end, maximum_block_size);
            target_padded.template encode_region<false, true, p_alphabet_dimension>(
                target_seq, target_start, target_end, maximum_block_size);
            if (execution_stats != nullptr) {
                execution_stats->add(
                    execution_stats->reversed_compact_symbols, query_length);
                execution_stats->add(
                    execution_stats->reversed_compact_symbols, target_length);
            }
        } else {
            // Encode the sub-region directly into its padded DP buffer.
            query_padded.template encode_region<p_is_reversed, false, p_alphabet_dimension>(
                query_seq, query_start, query_end, maximum_block_size);
            target_padded.template encode_region<false, false, p_alphabet_dimension>(
                target_seq, target_start, target_end, maximum_block_size);
        }

        const bool is_trace_enabled = parameters.is_trace_enabled;
        if (construction_stats != nullptr) {
            ++construction_stats->scratch_allocations;
            construction_stats->trace_allocations += is_trace_enabled ? 1u : 0u;
        }
        if (query_padded.storage_elements() != storage_plan.query_padded_bytes ||
            target_padded.storage_elements() != storage_plan.reference_padded_bytes) {
            abort_block_aligner_invariant("padded allocation disagrees with Block storage plan");
        }
        block_aligner_scratch scratch(query_length, target_length, storage_plan);
        scratch.clear(simd_tag, query_length, target_length, maximum_block_size, is_trace_enabled);

        int32_t score = 0;
        size_t output_query_position = 0, output_reference_position = 0;

        const bool is_x_drop_enabled = parameters.mode == block_alignment_mode::x_drop;
        const auto run_core =
            [&parameters,
             &execution_stats,
             &simd_tag,
             &score,
             &output_query_position,
             &output_reference_position,
             &scratch,
             &scoring_matrix,
             &negative_gap_open,
             &negative_gap_extension,
             &query_padded,
             &target_padded,
             &minimum_block_size,
             &maximum_block_size,
             &core_stats]<
                bool p_is_trace_enabled,
                bool p_is_x_drop_enabled>(
                std::bool_constant<p_is_trace_enabled>,
                std::bool_constant<p_is_x_drop_enabled>) {
            constexpr bool k_should_collect_stats = true;
            const int32_t effective_x_drop_threshold = p_is_x_drop_enabled
                ? parameters.x_drop_threshold
                : 0;
            if (execution_stats != nullptr) {
                align_block_core<
                    p_is_trace_enabled,
                    p_is_x_drop_enabled,
                    k_should_collect_stats,
                    p_alphabet>(
                    simd_tag, score, output_query_position, output_reference_position, scratch,
                    scoring_matrix, negative_gap_open, negative_gap_extension,
                    query_padded, target_padded, minimum_block_size, maximum_block_size,
                    effective_x_drop_threshold, core_stats);
            } else {
                align_block_core<
                    p_is_trace_enabled,
                    p_is_x_drop_enabled,
                    !k_should_collect_stats,
                    p_alphabet>(
                    simd_tag, score, output_query_position, output_reference_position, scratch,
                    scoring_matrix, negative_gap_open, negative_gap_extension,
                    query_padded, target_padded, minimum_block_size, maximum_block_size,
                    effective_x_drop_threshold, core_stats);
            }
        };

        if (is_trace_enabled && is_x_drop_enabled) {
            run_core(std::true_type{}, std::true_type{});
        } else if (is_trace_enabled) {
            run_core(std::true_type{}, std::false_type{});
        } else if (is_x_drop_enabled) {
            run_core(std::false_type{}, std::true_type{});
        } else {
            run_core(std::false_type{}, std::false_type{});
        }

        if (is_trace_enabled) {
            block_traceback_result traceback_result =
                parameters.should_compute_edit_distance
                ? build_block_cigar<true>(
                    scratch.trace, output_query_position, output_reference_position,
                    query_padded, target_padded)
                : build_block_cigar<false>(
                    scratch.trace, output_query_position, output_reference_position,
                    query_padded, target_padded);
            result.cigar = std::move(traceback_result.cigar);
            if (parameters.input_order == block_input_order::reversed) {
                std::reverse(result.cigar.begin(), result.cigar.end());
            }
            if (parameters.should_compute_edit_distance) {
                result.edit_distance = traceback_result.edit_distance;
                result.is_edit_distance_valid = true;
            }
        }

        result.score = score;
        result.max = score;
        result.max_q = static_cast<int32_t>(output_query_position) - 1;
        result.max_t = static_cast<int32_t>(output_reference_position) - 1;
        result.is_z_dropped = core_stats.is_x_drop_terminated;

        if (output_query_position >= query_length) {
            result.mqe = score;
            result.mqe_t = static_cast<int32_t>(output_reference_position) - 1;
            // Reaching the query end is enough to set has_reached_end.
            // The target may still have unread symbols.
            result.has_reached_end = true;
        }
        if (output_reference_position >= target_length) {
            result.mte = score;
            result.mte_q = static_cast<int32_t>(output_query_position) - 1;
        }

        if (execution_stats != nullptr) {
            record_block_aligner_execution(
                *execution_stats, parameters, lane_count, false, core_stats,
                configuration_identity);
        }

        return result;
    }
};

/**
 * Prepare the scoring checks reused by packed gap shortcuts on this CPU target.
 *
 * The plan stores the alignment settings, scoring identity and SIMD lane count,
 * and records whether the matrix supports the ungapped-alignment check.
 */
template<sequence_alphabet p_alphabet>
[[nodiscard]] block_global_gap_certificate_plan prepare_block_global_gap_certificate(
    const scoring_matrix<p_alphabet>& scoring_matrix,
    const block_aligner_parameters& parameters) noexcept
{
    return highway_block_aligner::prepare_global_gap_certificate<p_alphabet>(
        scoring_matrix, parameters);
}

/**
 * Check whether an ungapped alignment is best for the requested gap.
 *
 * Select the query orientation, then try the packed DNA4 check with the prepared
 * plan. Both intervals are half-open. The result distinguishes an unavailable
 * check, an inconclusive check and a proven alignment with its score and edit count.
 */
template<sequence_alphabet p_alphabet>
[[nodiscard]] block_packed_certificate_attempt try_block_certified_gap(
    bool is_reversed,
    bioseq::storage::lazy_sequence_view<p_alphabet> query_sequence,
    bioseq::storage::lazy_sequence_view<p_alphabet> target_sequence,
    size_t query_start,
    size_t query_end,
    size_t target_start,
    size_t target_end,
    const block_global_gap_certificate_plan& plan) noexcept
{
    return is_reversed
        ? highway_block_aligner::try_packed_global_gap<true>(
            query_sequence, target_sequence, query_start, query_end,
            target_start, target_end, plan)
        : highway_block_aligner::try_packed_global_gap<false>(
            query_sequence, target_sequence, query_start, query_end,
            target_start, target_end, plan);
}

/**
 * Run BlockAligner with the requested query orientation and shortcut setting.
 *
 * Both intervals are half-open. The scoring matrix and parameters control gap
 * penalties, global or X-drop alignment, and optional trace output. The result
 * contains the score, endpoints and an owning CIGAR when tracing is enabled.
 * Optional counters record execution work and scratch allocation.
 */
template<sequence_alphabet p_alphabet>
[[nodiscard]] ksw_alignment_result align_block(
    bool is_reversed,
    bool is_diagonal_certificate_enabled,
    bioseq::storage::lazy_sequence_view<p_alphabet> query_sequence,
    bioseq::storage::lazy_sequence_view<p_alphabet> target_sequence,
    size_t query_start,
    size_t query_end,
    size_t target_start,
    size_t target_end,
    const scoring_matrix<p_alphabet>& scoring_matrix,
    const block_aligner_parameters& parameters,
    block_aligner_execution_stats* execution_stats,
    block_aligner_construction_stats* construction_stats)
{
    if (is_reversed) {
        return is_diagonal_certificate_enabled
            ? highway_block_aligner::align<true, true>(
                query_sequence, target_sequence, query_start, query_end,
                target_start, target_end, scoring_matrix, parameters,
                execution_stats, construction_stats)
            : highway_block_aligner::align<true, false>(
                query_sequence, target_sequence, query_start, query_end,
                target_start, target_end, scoring_matrix, parameters,
                execution_stats, construction_stats);
    }
    return is_diagonal_certificate_enabled
        ? highway_block_aligner::align<false, true>(
            query_sequence, target_sequence, query_start, query_end,
            target_start, target_end, scoring_matrix, parameters,
            execution_stats, construction_stats)
        : highway_block_aligner::align<false, false>(
            query_sequence, target_sequence, query_start, query_end,
            target_start, target_end, scoring_matrix, parameters,
            execution_stats, construction_stats);
}

}  // namespace target
}  // namespace bioseq::simd
