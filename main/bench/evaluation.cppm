/**
 * @file evaluation.cppm
 * @brief Benchmark evaluator that compares bioseq minimap_port vs the
 *        reference minimap2 C pipeline.
 *
 * Given ground-truth (query, target, truth_start/end, is_reverse)
 * metadata, this module invokes both aligners and produces the same
 * statistics SEAL reports on its aligners:
 *
 *   - per-pair "correct vs incorrect" classification
 *     (SEAL's SamReader.java:109 — exact position match, generalised
 *     here to a tolerance window for real-world indel slop)
 *   - per-MAPQ-threshold confusion matrices (TP/FP/FN counts) with
 *     precision and recall, mirroring SEAL's FilteredAlignmentResults
 *   - summary stats comparing our port's output to minimap2's output
 *   - position-offset histogram so divergence patterns are visible
 *
 * Entry points:
 *   compare_alignments()  — compare two pre-computed MatchResultCollections
 *   evaluate_and_benchmark_results(single pair) — run both aligners + compare
 *   evaluate_and_benchmark_results(batch)       — same, aggregated over pairs
 */
module;

#include <hwy/highway.h>

#include "common.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

export module bench.evaluation;

import mlogging;
import sequan_compat;
import bioseq_sequences;
import bioseq_sequences.minimap_port;
import bioseq_cigar;
import bench.tool_minimap2;
import bench.generator;

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::bench::HWY_NAMESPACE {

using bioseq::simd::HWY_NAMESPACE::MatchResult;
using bioseq::simd::HWY_NAMESPACE::MatchResultCollection;
using bioseq::simd::HWY_NAMESPACE::Minimap2Params;

/**
 * @brief Knobs controlling what counts as a "correct" alignment and
 *        which MAPQ thresholds the confusion matrix is computed at.
 *
 * SEAL matches the aligned position to ground truth exactly (SamReader
 * line 109). We default to a small tolerance window because indel-heavy
 * reads move the start position by a few bp even when the rest of the
 * alignment is semantically identical; set `position_tolerance = 0` to
 * reproduce SEAL's strict check.
 */
struct EvaluationParams {
    /// Allowed |aligned_start - truth_start| (bp) for a hit to count as correct.
    int32_t position_tolerance = 10;

    /// MAPQ thresholds used for the per-threshold confusion matrix
    /// (mirrors SEAL's PHRED_THRESHOLDS in AlignmentToolService).
    std::vector<int32_t> mapq_thresholds = {0, 10, 20, 30, 40, 60};

    /// Parameters passed to both aligners. The same struct is sent to
    /// bioseq's minimap_port and to our tool_minimap2 wrapper so they
    /// operate under identical k/w/scoring settings.
    Minimap2Params aligner_params = Minimap2Params{};
};

/**
 * @brief Summary of one aligner's output for a single (query, target) pair.
 *
 * Only the primary alignment is surfaced into these fields (secondary
 * alignments affect result_count and the comparison check but are not
 * individually reported, matching SEAL's "first record wins" rule when
 * penalize_duplicate_mappings=false — see SamReader.java:113-117).
 */
struct AlignmentEvalSnapshot {
    size_t   result_count       = 0;
    bool     produced_primary   = false;
    int32_t  primary_query_start= 0;
    int32_t  primary_query_end  = 0;
    int32_t  primary_target_start= 0;
    int32_t  primary_target_end = 0;
    bool     primary_is_reverse = false;
    int32_t  primary_mapq       = 0;
    int32_t  primary_dp_score   = 0;
    int32_t  primary_num_matches= 0;
    int32_t  primary_block_length= 0;
    size_t   primary_cigar_length= 0;

    /// Wall-clock time spent inside the aligner for this pair.
    double   elapsed_ns = 0.0;
};

/**
 * @brief Per-pair diagnostic record, built from both aligners' snapshots.
 *
 * Carries enough metadata (truth position, truth strand, nominal error
 * rate, pre-error query length) that a downstream CSV writer can
 * reproduce SEAL's ROC-plus-parameter data layout — each row contains
 * the simulation parameter that produced it, the quality score the
 * aligner assigned, and the correctness label.
 */
struct SingleEvalResult {
    AlignmentEvalSnapshot bioseq;
    AlignmentEvalSnapshot minimap2;

    size_t truth_target_start = 0;
    size_t truth_target_end   = 0;
    bool   truth_is_reverse   = false;

    /// Nominal per-base error rate the fragment was generated under
    /// (substitution + insertion + deletion rate). Left at 0 when the
    /// evaluator is invoked without generator metadata.
    double effective_error_rate = 0.0;
    size_t original_query_length = 0;

    bool    bioseq_position_correct    = false;
    bool    minimap2_position_correct  = false;
    int32_t bioseq_position_offset     = 0;   ///< aligned - truth (signed)
    int32_t minimap2_position_offset   = 0;
    bool    bioseq_strand_correct      = false;
    bool    minimap2_strand_correct    = false;

    /// When both aligners produced a primary, do their CIGARs agree
    /// byte-for-byte? Useful for catching silent divergences even when
    /// both are "correct" per the position-tolerance rule.
    bool    cigars_identical           = false;
};

/**
 * @brief Confusion-matrix row for one MAPQ threshold.
 *
 * Mirrors SEAL's FilteredAlignmentResults classification:
 *   TP = produced_primary AND correct AND mapq >= threshold
 *   FP = produced_primary AND incorrect AND mapq >= threshold
 *   FN = produced_primary AND correct AND mapq <  threshold
 *        OR no primary produced (unmapped fragment — SEAL's
 *        `missingFragments`, which is added to each per-threshold FN
 *        row unconditionally, so one unmapped fragment contributes one
 *        FN to every threshold row)
 *   TN = produced_primary AND incorrect AND mapq < threshold
 *
 * precision = TP / (TP + FP), recall = TP / (TP + FN),
 * used_read_ratio = (TP + FP) / (TP + FP + FN + TN).
 */
struct ThresholdMetrics {
    int32_t threshold         = 0;
    size_t  true_positives    = 0;
    size_t  false_positives   = 0;
    size_t  false_negatives   = 0;
    size_t  true_negatives    = 0;
    double  precision         = 0.0;
    double  recall            = 0.0;
    double  used_read_ratio   = 0.0;
};

/**
 * @brief Aggregated statistics for one aligner across a batch of pairs.
 *
 * `position_offset_histogram` has 21 bins: index 0 -> offset <= -10 bp,
 * index 20 -> offset >= +10 bp, indices 1..19 -> -9..+9 bp. This is
 * intentionally bp-scale because most aligner disagreements are <5 bp.
 */
struct AlignerAggregatedStats {
    size_t total_pairs  = 0;
    size_t mapped       = 0;
    size_t unmapped     = 0;
    size_t correct      = 0;     ///< position within tolerance AND primary produced
    size_t incorrect    = 0;     ///< primary produced but position wrong
    size_t strand_correct = 0;

    double correct_fraction      = 0.0;
    double mapped_fraction       = 0.0;
    double mean_absolute_offset  = 0.0;
    double total_alignment_ns    = 0.0;
    double mean_alignment_ns     = 0.0;

    std::vector<ThresholdMetrics> per_threshold;

    static constexpr size_t kHistogramBins = 21;
    std::array<size_t, kHistogramBins> position_offset_histogram{};
};

/**
 * @brief Full result container for a batch evaluation.
 */
struct EvaluationResults {
    std::vector<SingleEvalResult> per_pair;
    AlignerAggregatedStats bioseq;
    AlignerAggregatedStats minimap2;
};

namespace detail {

/// Pick the primary MatchResult from a collection, or return nullptr if
/// no primary was produced.
///
/// This deliberately does NOT fall back to the first result when the
/// primary flag is unset — SEAL's SamReader.java:113 treats only the
/// first *correctly placed* mapping as authoritative, and silently
/// promoting a secondary to primary would mask real bugs in either
/// pipeline's primary/secondary classification.
[[nodiscard]] inline const MatchResult* primary_result(
    const MatchResultCollection& collection)
{
    for (const auto& result : collection.view()) {
        if (result.is_primary) {
            return &result;
        }
    }
    return nullptr;
}

/// Build an AlignmentEvalSnapshot from a MatchResultCollection.
[[nodiscard]] inline AlignmentEvalSnapshot snapshot_from_collection(
    const MatchResultCollection& collection, double elapsed_ns)
{
    AlignmentEvalSnapshot snapshot;
    snapshot.result_count = collection.view().size();
    snapshot.elapsed_ns   = elapsed_ns;
    const MatchResult* primary = primary_result(collection);
    if (primary != nullptr) {
        snapshot.produced_primary    = true;
        snapshot.primary_query_start = primary->query_start;
        snapshot.primary_query_end   = primary->query_end;
        snapshot.primary_target_start= primary->target_start;
        snapshot.primary_target_end  = primary->target_end;
        snapshot.primary_is_reverse  = primary->is_reverse;
        snapshot.primary_mapq        = primary->mapq;
        snapshot.primary_dp_score    = primary->dp_score;
        snapshot.primary_num_matches = primary->num_matches;
        snapshot.primary_block_length= primary->block_length;
        snapshot.primary_cigar_length= primary->cigar.size();
    }
    return snapshot;
}

/// Clamp a signed offset into the 21-bin histogram ([-10, +10] clipped).
inline void accumulate_offset_histogram(
    std::array<size_t, AlignerAggregatedStats::kHistogramBins>& histogram,
    int32_t signed_offset)
{
    constexpr int32_t half_span = 10;
    const int32_t clamped = std::clamp(signed_offset, -half_span, half_span);
    const size_t bin_index = static_cast<size_t>(clamped + half_span);
    ++histogram[bin_index];
}

/// Fill the per-threshold confusion matrix (SEAL's
/// FilteredAlignmentResults.filter(threshold), applied for each
/// configured threshold).
inline void populate_threshold_metrics(
    AlignerAggregatedStats& stats,
    const std::vector<SingleEvalResult>& per_pair,
    bool use_bioseq,
    const std::vector<int32_t>& thresholds)
{
    stats.per_threshold.reserve(thresholds.size());
    for (const int32_t threshold : thresholds) {
        ThresholdMetrics metrics;
        metrics.threshold = threshold;
        for (const auto& result : per_pair) {
            const auto& snapshot = use_bioseq ? result.bioseq : result.minimap2;
            const bool position_correct = use_bioseq ? result.bioseq_position_correct
                                                     : result.minimap2_position_correct;
            if (!snapshot.produced_primary) {
                ++metrics.false_negatives;
                continue;
            }
            const bool passes_threshold = snapshot.primary_mapq >= threshold;
            if (position_correct && passes_threshold) {
                ++metrics.true_positives;
            } else if (position_correct) {
                ++metrics.false_negatives;
            } else if (passes_threshold) {
                ++metrics.false_positives;
            } else {
                ++metrics.true_negatives;
            }
        }
        const size_t tp_fp = metrics.true_positives + metrics.false_positives;
        const size_t tp_fn = metrics.true_positives + metrics.false_negatives;
        const size_t total = metrics.true_positives + metrics.false_positives
                           + metrics.false_negatives + metrics.true_negatives;
        metrics.precision       = tp_fp > 0
            ? static_cast<double>(metrics.true_positives) / static_cast<double>(tp_fp) : 0.0;
        metrics.recall          = tp_fn > 0
            ? static_cast<double>(metrics.true_positives) / static_cast<double>(tp_fn) : 0.0;
        metrics.used_read_ratio = total > 0
            ? static_cast<double>(tp_fp) / static_cast<double>(total) : 0.0;
        stats.per_threshold.push_back(metrics);
    }
}

/// Roll summary counters over a batch of SingleEvalResult rows.
inline void finalize_aggregated_stats(
    AlignerAggregatedStats& stats,
    const std::vector<SingleEvalResult>& per_pair,
    bool use_bioseq,
    const std::vector<int32_t>& thresholds)
{
    stats.total_pairs = per_pair.size();
    size_t absolute_offset_sum = 0;
    for (const auto& result : per_pair) {
        const auto& snapshot = use_bioseq ? result.bioseq : result.minimap2;
        const bool position_correct = use_bioseq ? result.bioseq_position_correct
                                                 : result.minimap2_position_correct;
        const bool strand_correct = use_bioseq ? result.bioseq_strand_correct
                                               : result.minimap2_strand_correct;
        const int32_t position_offset = use_bioseq ? result.bioseq_position_offset
                                                    : result.minimap2_position_offset;
        stats.total_alignment_ns += snapshot.elapsed_ns;
        if (snapshot.produced_primary) {
            ++stats.mapped;
            if (position_correct) ++stats.correct;
            else                  ++stats.incorrect;
            if (strand_correct)   ++stats.strand_correct;
            absolute_offset_sum += static_cast<size_t>(std::abs(position_offset));
            // Only count this row in the offset histogram when a primary
            // was produced — otherwise the default-zero position_offset
            // would pollute the "0 bp" bin and make unmapped fragments
            // look like perfect placements.
            accumulate_offset_histogram(stats.position_offset_histogram, position_offset);
        } else {
            ++stats.unmapped;
        }
    }
    stats.correct_fraction    = stats.total_pairs > 0
        ? static_cast<double>(stats.correct) / static_cast<double>(stats.total_pairs) : 0.0;
    stats.mapped_fraction     = stats.total_pairs > 0
        ? static_cast<double>(stats.mapped)  / static_cast<double>(stats.total_pairs) : 0.0;
    stats.mean_absolute_offset= stats.mapped > 0
        ? static_cast<double>(absolute_offset_sum) / static_cast<double>(stats.mapped) : 0.0;
    stats.mean_alignment_ns   = stats.total_pairs > 0
        ? stats.total_alignment_ns / static_cast<double>(stats.total_pairs) : 0.0;
    populate_threshold_metrics(stats, per_pair, use_bioseq, thresholds);
}

}  // namespace detail

/**
 * @brief Compare two precomputed MatchResultCollections against ground truth.
 *
 * Does not invoke either aligner — useful when the caller already has
 * results in hand and only wants the classification logic.
 */
[[nodiscard]] inline SingleEvalResult compare_alignments(
    const MatchResultCollection& bioseq_results,
    const MatchResultCollection& minimap2_results,
    size_t truth_target_start,
    size_t truth_target_end,
    bool   truth_is_reverse,
    const EvaluationParams& params = EvaluationParams{})
{
    SingleEvalResult output;
    output.truth_target_start = truth_target_start;
    output.truth_target_end   = truth_target_end;
    output.truth_is_reverse   = truth_is_reverse;

    output.bioseq   = detail::snapshot_from_collection(bioseq_results,   0.0);
    output.minimap2 = detail::snapshot_from_collection(minimap2_results, 0.0);

    const int32_t tolerance = params.position_tolerance;
    const int32_t truth_start_i32 = static_cast<int32_t>(truth_target_start);

    if (output.bioseq.produced_primary) {
        output.bioseq_position_offset = output.bioseq.primary_target_start - truth_start_i32;
        output.bioseq_position_correct =
            std::abs(output.bioseq_position_offset) <= tolerance;
        output.bioseq_strand_correct = (output.bioseq.primary_is_reverse == truth_is_reverse);
    }
    if (output.minimap2.produced_primary) {
        output.minimap2_position_offset = output.minimap2.primary_target_start - truth_start_i32;
        output.minimap2_position_correct =
            std::abs(output.minimap2_position_offset) <= tolerance;
        output.minimap2_strand_correct = (output.minimap2.primary_is_reverse == truth_is_reverse);
    }

    if (output.bioseq.produced_primary && output.minimap2.produced_primary) {
        const MatchResult* bioseq_primary   = detail::primary_result(bioseq_results);
        const MatchResult* minimap2_primary = detail::primary_result(minimap2_results);
        if (bioseq_primary != nullptr && minimap2_primary != nullptr) {
            output.cigars_identical =
                bioseq::simd::HWY_NAMESPACE::format_cigar(bioseq_primary->cigar)
                == bioseq::simd::HWY_NAMESPACE::format_cigar(minimap2_primary->cigar);
        }
    }

    return output;
}

/**
 * @brief Run bioseq minimap_port and tool_minimap2 on a single pair and
 *        compare their outputs against ground truth.
 */
template<
    supports_compact_sequence_range QueryRange,
    supports_compact_sequence_range TargetRange>
[[nodiscard]] SingleEvalResult evaluate_and_benchmark_results(
    compact_sequence_range<QueryRange>& query_seq,
    compact_sequence_range<TargetRange>& target_seq,
    size_t truth_target_start,
    size_t truth_target_end,
    bool   truth_is_reverse,
    const EvaluationParams& params = EvaluationParams{})
{
    using clock = std::chrono::steady_clock;

    const auto bioseq_start = clock::now();
    auto bioseq_results = bioseq::simd::HWY_NAMESPACE::minimap_port(
        query_seq, target_seq, params.aligner_params);
    const auto bioseq_end = clock::now();

    const auto minimap2_start = clock::now();
    auto minimap2_results = tool_minimap2(
        query_seq, target_seq, params.aligner_params);
    const auto minimap2_end = clock::now();

    SingleEvalResult result = compare_alignments(
        bioseq_results, minimap2_results,
        truth_target_start, truth_target_end, truth_is_reverse, params);

    result.bioseq.elapsed_ns =
        std::chrono::duration<double, std::nano>(bioseq_end - bioseq_start).count();
    result.minimap2.elapsed_ns =
        std::chrono::duration<double, std::nano>(minimap2_end - minimap2_start).count();

    JustLog("[bench.evaluation] pair: bioseq correct={} offset={} mapq={} | "
            "minimap2 correct={} offset={} mapq={} | truth=[{},{}) rev={}",
            result.bioseq_position_correct, result.bioseq_position_offset, result.bioseq.primary_mapq,
            result.minimap2_position_correct, result.minimap2_position_offset, result.minimap2.primary_mapq,
            truth_target_start, truth_target_end, truth_is_reverse);

    return result;
}

/**
 * @brief Run both aligners over a whole batch and return aggregated
 *        statistics in SEAL's shape.
 *
 * Each pair is evaluated by the single-pair entry point above; the
 * results are summarised into AlignerAggregatedStats separately for
 * bioseq and minimap2 so the two can be compared side-by-side.
 */
[[nodiscard]] inline EvaluationResults evaluate_and_benchmark_results(
    std::vector<GeneratedBenchSequencePair>& pairs,
    const EvaluationParams& params = EvaluationParams{})
{
    EvaluationResults output;
    output.per_pair.reserve(pairs.size());
    for (auto& pair : pairs) {
        SingleEvalResult per_pair = evaluate_and_benchmark_results(
            pair.query, pair.target,
            pair.truth_target_start, pair.truth_target_end, pair.is_reverse,
            params);
        // Forward the generator metadata into the per-pair row so
        // downstream consumers (CSV writers, ROC plotters) can join
        // correctness with the nominal simulation parameter without
        // re-running the generator.
        per_pair.effective_error_rate  = pair.effective_error_rate;
        per_pair.original_query_length = pair.original_query_length;
        output.per_pair.push_back(std::move(per_pair));
    }
    detail::finalize_aggregated_stats(
        output.bioseq,   output.per_pair, /*use_bioseq=*/true,  params.mapq_thresholds);
    detail::finalize_aggregated_stats(
        output.minimap2, output.per_pair, /*use_bioseq=*/false, params.mapq_thresholds);

    JustLog("[bench.evaluation] Batch complete: {} pairs | bioseq correct={}/{}, minimap2 correct={}/{}",
            output.per_pair.size(),
            output.bioseq.correct,   output.bioseq.total_pairs,
            output.minimap2.correct, output.minimap2.total_pairs);
    return output;
}

}  // namespace bioseq::bench::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();

}  // export
