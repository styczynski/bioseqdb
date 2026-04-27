/**
 * @file matrix_runner.cppm
 * @brief Cross-product benchmark sweep over the bioseq aligner config grid.
 *
 * The Catch2 BENCHMARK macro is great for one-shot timings, but it
 * doesn't compose well across a 14-algorithm × 4-length-bucket × 3-error
 * grid: each BENCHMARK gets its own table row in the console output and
 * we lose the structured metadata (algorithm config, error rate, length
 * bucket) needed to render comparative charts.
 *
 * This module replaces the BENCHMARK matrix with a single explicit
 * runner that:
 *
 *   1. Generates a fresh batch of (query, target) pairs for each
 *      (length_bucket, error_rate) combination, using the existing
 *      `bench.generator` module so the simulation parameters stay in
 *      one place.
 *   2. Iterates every algorithm-config row (minimap2-DP / minimap2-RMQ
 *      reference + the 12 bioseq combos) and runs N samples per pair
 *      via the eval helpers from `bench.evaluation`.
 *   3. Records per-sample latency (ns), peak resident-set growth
 *      (bytes, via getrusage), correctness, and the truth-relative
 *      offset into a flat `BenchSample` record so the chart builder
 *      can pivot on any axis.
 *
 * The data structures (BenchSample, enums, accumulator) live in
 * `bioseq::bench` (no HWY namespace) on purpose — the chart builder
 * runs after `session.run()` returns, outside any HWY dispatch context,
 * and needs to read the same storage the HWY-bound runner wrote into.
 * Only the runner functions themselves live in HWY_NAMESPACE, where
 * the HWY-target-bound aligner entry points (minimap_port,
 * tool_minimap2) are visible.
 */
module;

#include <hwy/highway.h>

#include "common.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_init.h>
#include <mach/task.h>
#include <mach/task_info.h>
#elif defined(__linux__)
#include <cstdio>
#include <unistd.h>
#endif

export module bench.matrix_runner;

import mlogging;
import sequan_compat;
import bioseq_sequences;
import bioseq_sequences.minimap_port;
import bench.tool_minimap2;
import bench.generator;
import bench.evaluation;

export {

namespace bioseq::bench {

// =====================================================================
// HWY-agnostic data types — visible to chart_builder.cppm.
// =====================================================================

/**
 * @brief Coarse buckets for the sequence-length axis.
 *
 * Sized to match the prompt: short < 1k, medium 1k-10k, long 10k-1M,
 * very long 1M-3M. Stored as an enum + helper so chart code can iterate
 * the buckets in display order without having to re-derive the cutoffs.
 */
enum class SequenceLengthBucket : uint8_t {
    SHORT     = 0,  ///< [0, 1000)        — typical Illumina read territory
    MEDIUM    = 1,  ///< [1000, 10000)    — moderately long reads / contigs
    LONG      = 2,  ///< [10000, 1000000) — PacBio / Nanopore
    VERY_LONG = 3,  ///< [1000000, ...]   — assembly-to-assembly
};

[[nodiscard]] constexpr const char* sequence_length_bucket_name(SequenceLengthBucket bucket) {
    switch (bucket) {
        case SequenceLengthBucket::SHORT:     return "short (<1kb)";
        case SequenceLengthBucket::MEDIUM:    return "medium (1k-10k)";
        case SequenceLengthBucket::LONG:      return "long (10k-1M)";
        case SequenceLengthBucket::VERY_LONG: return "very long (1M-3M)";
    }
    return "unknown";
}

/**
 * @brief Coarse buckets for the error-rate axis.
 *
 * Cut-offs taken straight from the prompt: low / medium / high.
 */
enum class ErrorRateBucket : uint8_t {
    LOW    = 0,
    MEDIUM = 1,
    HIGH   = 2,
};

[[nodiscard]] constexpr const char* error_rate_bucket_name(ErrorRateBucket bucket) {
    switch (bucket) {
        case ErrorRateBucket::LOW:    return "low";
        case ErrorRateBucket::MEDIUM: return "medium";
        case ErrorRateBucket::HIGH:   return "high";
    }
    return "unknown";
}

/**
 * @brief One measurement row.
 *
 * Captures every axis the chart builder pivots on plus the per-sample
 * latency vector so the builder can compute mean / P50 / P99 without
 * re-running the workload.
 */
struct BenchSample {
    std::string algorithm_label;
    std::string algorithm_kind;        ///< "bioseq" or "minimap2"
    std::string chaining_label;        ///< e.g. "Minimap2 RMQ"
    std::string aligner_label;         ///< e.g. "KSW2", "Block Aligner", or "n/a"
    std::string minimizer_label;       ///< e.g. "SIMD Minimizers", or "n/a"

    SequenceLengthBucket length_bucket = SequenceLengthBucket::SHORT;
    ErrorRateBucket      error_bucket  = ErrorRateBucket::LOW;
    std::string length_bucket_label;
    std::string error_bucket_label;

    std::size_t requested_query_length = 0;
    std::size_t effective_query_length = 0;
    std::size_t target_length          = 0;

    double effective_error_rate = 0.0;

    /// Per-sample wall-clock latencies for this (algorithm, pair)
    /// measurement. Sorted ascending after collection.
    std::vector<double> sample_latencies_ns;

    /// Per-sample peak-RSS *delta* (current - baseline at sample
    /// start), in bytes. Same length as `sample_latencies_ns`.
    std::vector<double> sample_peak_rss_delta_bytes;

    bool position_correct = false;
    bool strand_correct   = false;
    bool produced_primary = false;
    int  primary_mapq     = 0;
    int  position_offset_bp = 0;
};

/**
 * @brief Process-wide accumulator for `BenchSample` rows.
 *
 * Lives in the non-HWY namespace so the chart builder (which runs
 * outside any HWY dispatch context) and the matrix runner (HWY-bound)
 * see the same singleton.
 */
inline std::vector<BenchSample>& benchmark_samples() {
    static std::vector<BenchSample> samples;
    return samples;
}

}  // namespace bioseq::bench

// =====================================================================
// HWY-bound runner — uses minimap_port and tool_minimap2 entry points.
// =====================================================================

HWY_BEFORE_NAMESPACE();

namespace bioseq::bench::HWY_NAMESPACE {

using bioseq::simd::HWY_NAMESPACE::Minimap2Params;
using bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType;
using bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType;
using bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType;

/**
 * @brief Identifies one row in the algorithm sweep.
 *
 * The "minimap2 reference" rows reuse the bioseq enum values for
 * chaining (DP vs RMQ) but ignore the alignment / minimizer fields
 * because the C reference is monolithic — those columns get filled
 * with "n/a" labels by the chart builder.
 */
struct AlgorithmConfig {
    std::string label;
    bool is_minimap2_reference = false;
    ChainingAlgorithmType chaining_algorithm =
        ChainingAlgorithmType::SCALAR_MM2;
    BaseAlignmentAlgorithmType base_alignment_algorithm =
        BaseAlignmentAlgorithmType::KSW2;
    MinimizerAlgorithmType minimizer_algorithm =
        MinimizerAlgorithmType::SCALAR;
};

namespace detail {

/// Returns the *current* process resident-set-size in bytes — the
/// instantaneous live value, not the lifetime peak. The original
/// implementation read `getrusage(RUSAGE_SELF).ru_maxrss`, which is
/// monotonically non-decreasing; once any sample pushed the
/// high-water mark, every subsequent before/after delta read 0. The
/// chart that depended on it (`seq_length_vs_memory`) was therefore
/// rendering near-zero deltas for ~98% of the matrix.
///
/// macOS:  `task_info(TASK_BASIC_INFO).resident_size` — current RSS in bytes.
/// Linux:  `/proc/self/statm` field 2 (resident pages) × pagesize.
/// Other:  returns 0; chart layer hides traces with all-zero deltas.
[[nodiscard]] inline std::uint64_t current_resident_bytes() noexcept {
#if defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (::task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                    reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) {
        return 0;
    }
    return static_cast<std::uint64_t>(info.resident_size);
#elif defined(__linux__)
    std::FILE* f = std::fopen("/proc/self/statm", "r");
    if (f == nullptr) return 0;
    long size_pages = 0, resident_pages = 0;
    if (std::fscanf(f, "%ld %ld", &size_pages, &resident_pages) != 2) {
        std::fclose(f);
        return 0;
    }
    std::fclose(f);
    const long page_size = ::sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return 0;
    return static_cast<std::uint64_t>(resident_pages) *
           static_cast<std::uint64_t>(page_size);
#else
    return 0;
#endif
}

/// Materialise generator parameters for a (length_bucket, error_bucket).
[[nodiscard]] inline GeneratorParams make_generator_params(
    SequenceLengthBucket length_bucket,
    ErrorRateBucket error_bucket,
    std::uint64_t seed)
{
    GeneratorParams params;
    params.seed = seed;
    params.reverse_complement_rate = 0.5;

    // Cluster the fragment length distribution well inside each
    // bucket so a Gaussian draw at +/-1 sigma still lands in-bucket.
    switch (length_bucket) {
        case SequenceLengthBucket::SHORT:
            params.fragment_length_mean   = 300;
            params.fragment_length_stddev = 50.0;
            params.reference_length       = 3'000;
            params.fragment_count         = 16;
            break;
        case SequenceLengthBucket::MEDIUM:
            params.fragment_length_mean   = 5'000;
            params.fragment_length_stddev = 500.0;
            params.reference_length       = 30'000;
            params.fragment_count         = 8;
            break;
        case SequenceLengthBucket::LONG:
            params.fragment_length_mean   = 100'000;
            params.fragment_length_stddev = 5'000.0;
            params.reference_length       = 500'000;
            params.fragment_count         = 4;
            break;
        case SequenceLengthBucket::VERY_LONG:
            params.fragment_length_mean   = 2'000'000;
            params.fragment_length_stddev = 100'000.0;
            params.reference_length       = 3'000'000;
            params.fragment_count         = 1;
            break;
    }

    // Error rates straight from the prompt. The indel-length sigma
    // scales with the indel rate so high-error reads also produce
    // longer indel events, as observed in real Nanopore data.
    switch (error_bucket) {
        case ErrorRateBucket::LOW:
            params.substitution_rate       = 0.005;   // < 0.01
            params.insertion_rate          = 0.0003;  // < 0.0005
            params.deletion_rate           = 0.0003;  // < 0.0005
            params.insertion_length_mean   = 1.5;
            params.deletion_length_mean    = 1.5;
            break;
        case ErrorRateBucket::MEDIUM:
            params.substitution_rate       = 0.05;    // < 0.1
            params.insertion_rate          = 0.003;   // < 0.005
            params.deletion_rate           = 0.003;   // < 0.005
            params.insertion_length_mean   = 2.0;
            params.deletion_length_mean    = 2.0;
            break;
        case ErrorRateBucket::HIGH:
            params.substitution_rate       = 0.20;    // < 0.3
            params.insertion_rate          = 0.03;    // < 0.05
            params.deletion_rate           = 0.03;    // < 0.05
            params.insertion_length_mean   = 3.0;
            params.deletion_length_mean    = 3.0;
            break;
    }
    return params;
}

/// How many sample timings per (algorithm, pair) to collect.
[[nodiscard]] inline std::size_t samples_per_pair(SequenceLengthBucket bucket) noexcept {
    switch (bucket) {
        case SequenceLengthBucket::SHORT:     return 8;
        case SequenceLengthBucket::MEDIUM:    return 5;
        case SequenceLengthBucket::LONG:      return 3;
        case SequenceLengthBucket::VERY_LONG: return 1;
    }
    return 1;
}

/// Apply the algorithm config onto a fresh `Minimap2Params` instance.
[[nodiscard]] inline Minimap2Params build_aligner_params(const AlgorithmConfig& config) {
    Minimap2Params params;
    params.chaining_algorithm        = config.chaining_algorithm;
    params.base_alignment_algorithm  = config.base_alignment_algorithm;
    params.minimizer_algorithm       = config.minimizer_algorithm;
    return params;
}

/// Run one (algorithm, pair) measurement: a single warm-up call (whose
/// timing is discarded but whose primary-result fields drive the
/// per-record correctness columns), then `n_samples` timed iterations.
///
/// Correctness is captured ONCE — from the warm-up call — not per
/// iteration. The aligners are deterministic for a fixed (query,
/// target, params) input, so the per-iteration loop produces identical
/// answers; the previous code overwrote the same record fields on
/// every iteration, which was wasteful and silently wrong if the
/// aligner ever became nondeterministic. Capturing during the warm-up
/// also gives us first-call cache-cold timings to discard from the
/// percentile estimates.
///
/// `position_tolerance_bp` is the SEAL-style position-tolerance
/// window passed in by the caller — defaults to
/// `EvaluationParams::position_tolerance` (10 bp) for consistency
/// with the rest of the bench harness.
template<class Pair>
inline void run_one_pair_samples(
    Pair& pair,
    const AlgorithmConfig& config,
    const Minimap2Params& aligner_params,
    std::size_t n_samples,
    std::int32_t position_tolerance_bp,
    BenchSample& record)
{
    using clock = std::chrono::steady_clock;

    record.sample_latencies_ns.reserve(n_samples);
    record.sample_peak_rss_delta_bytes.reserve(n_samples);

    auto run_aligner_once = [&](
        bool& out_produced_primary,
        int&  out_primary_mapq,
        int&  out_primary_target_start,
        bool& out_primary_is_reverse)
    {
        out_produced_primary = false;
        out_primary_mapq = 0;
        out_primary_target_start = 0;
        out_primary_is_reverse = false;
        if (config.is_minimap2_reference) {
            auto results = tool_minimap2(pair.query, pair.target, aligner_params);
            for (const auto& match : results.view()) {
                if (match.is_primary) {
                    out_produced_primary       = true;
                    out_primary_mapq           = match.mapq;
                    out_primary_target_start   = match.target_start;
                    out_primary_is_reverse     = match.is_reverse;
                    break;
                }
            }
        } else {
            auto results = bioseq::simd::HWY_NAMESPACE::minimap_port(
                pair.query, pair.target, aligner_params);
            for (const auto& match : results.view()) {
                if (match.is_primary) {
                    out_produced_primary       = true;
                    out_primary_mapq           = match.mapq;
                    out_primary_target_start   = match.target_start;
                    out_primary_is_reverse     = match.is_reverse;
                    break;
                }
            }
        }
    };

    // ---- Warm-up call (timing discarded) — also fills correctness ----
    bool produced_primary = false;
    int  primary_mapq = 0;
    int  primary_target_start = 0;
    bool primary_is_reverse = false;
    run_aligner_once(produced_primary, primary_mapq,
                     primary_target_start, primary_is_reverse);
    record.produced_primary  = produced_primary;
    record.primary_mapq      = primary_mapq;
    const std::int32_t truth_start =
        static_cast<std::int32_t>(pair.truth_target_start);
    record.position_offset_bp = primary_target_start - truth_start;
    record.position_correct  = produced_primary &&
        std::abs(record.position_offset_bp) <= position_tolerance_bp;
    record.strand_correct    = produced_primary &&
        primary_is_reverse == pair.is_reverse;

    // ---- Timed sample loop ----
    for (std::size_t sample_index = 0; sample_index < n_samples; ++sample_index) {
        const auto rss_baseline = current_resident_bytes();
        const auto t0 = clock::now();
        bool _ignored_pp = false;
        int  _ignored_mapq = 0;
        int  _ignored_ts = 0;
        bool _ignored_rev = false;
        run_aligner_once(_ignored_pp, _ignored_mapq, _ignored_ts, _ignored_rev);
        const auto t1 = clock::now();
        const auto rss_after = current_resident_bytes();

        const double elapsed_ns =
            std::chrono::duration<double, std::nano>(t1 - t0).count();
        record.sample_latencies_ns.push_back(elapsed_ns);
        const std::int64_t rss_delta_bytes =
            static_cast<std::int64_t>(rss_after) -
            static_cast<std::int64_t>(rss_baseline);
        // Negative deltas (allocator returned memory between calls) are
        // clamped to zero so the chart axis stays positive — the
        // underlying current-RSS reading is meaningful, but a per-call
        // delta is still subject to allocator round-trips.
        record.sample_peak_rss_delta_bytes.push_back(
            rss_delta_bytes > 0 ? static_cast<double>(rss_delta_bytes) : 0.0);
    }
}

}  // namespace detail

/**
 * @brief Materialise the algorithm configurations covered by the sweep.
 *
 * Order matters for chart legend ordering: minimap2 references first
 * so the reader's eye lands on them as the baselines, then the bioseq
 * combinations grouped by (alignment, chaining, minimizer) so adjacent
 * lines on a multi-trace plot read as small variations of each other.
 */
[[nodiscard]] inline std::vector<AlgorithmConfig> default_algorithm_grid() {
    std::vector<AlgorithmConfig> grid;
    auto push_minimap2 = [&](const std::string& label, ChainingAlgorithmType chaining) {
        AlgorithmConfig cfg;
        cfg.label = label;
        cfg.is_minimap2_reference = true;
        cfg.chaining_algorithm = chaining;
        cfg.base_alignment_algorithm = BaseAlignmentAlgorithmType::KSW2;
        cfg.minimizer_algorithm = MinimizerAlgorithmType::SCALAR;
        grid.push_back(std::move(cfg));
    };
    push_minimap2("minimap2 (DP chaining)", ChainingAlgorithmType::SCALAR_MM2);
    push_minimap2("minimap2 (RMQ chaining)", ChainingAlgorithmType::RMQ);

    auto push_bioseq = [&](
        BaseAlignmentAlgorithmType aligner,
        ChainingAlgorithmType chaining,
        MinimizerAlgorithmType minimizer)
    {
        AlgorithmConfig cfg;
        cfg.is_minimap2_reference = false;
        cfg.chaining_algorithm = chaining;
        cfg.base_alignment_algorithm = aligner;
        cfg.minimizer_algorithm = minimizer;
        cfg.label = std::string("bioseq ")
            + (aligner == BaseAlignmentAlgorithmType::KSW2 ? "KSW2" : "Block")
            + " + "
            + (chaining == ChainingAlgorithmType::SCALAR_MM2 ? "scalar-DP"
               : chaining == ChainingAlgorithmType::SIMD_MM2 ? "SIMD-DP" : "RMQ")
            + " + "
            + (minimizer == MinimizerAlgorithmType::SIMD ? "SIMD-mm" : "scalar-mm");
        grid.push_back(std::move(cfg));
    };

    for (auto aligner : {BaseAlignmentAlgorithmType::KSW2,
                         BaseAlignmentAlgorithmType::BLOCK_ALIGNER}) {
        for (auto chaining : {ChainingAlgorithmType::SCALAR_MM2,
                              ChainingAlgorithmType::SIMD_MM2,
                              ChainingAlgorithmType::RMQ}) {
            for (auto minimizer : {MinimizerAlgorithmType::SCALAR,
                                   MinimizerAlgorithmType::SIMD}) {
                push_bioseq(aligner, chaining, minimizer);
            }
        }
    }
    return grid;
}

/// Decide whether to skip a (config, length_bucket) cell.
///
/// RMQ chaining is O(n log n) but the constant-factor on a 2 Mbp pair
/// pushes a single sample into the multi-minute range; multiplied by
/// the 2 RMQ alignment combinations × 2 minimizer combinations × 3
/// error rates this single bucket would dominate the entire bench
/// wall-clock. We skip RMQ for VERY_LONG by default; users who want
/// the data can set `BIOSEQ_BENCH_INCLUDE_RMQ_VERY_LONG=1` in the
/// environment before invoking the binary.
[[nodiscard]] inline bool should_skip_cell(
    const AlgorithmConfig& config,
    SequenceLengthBucket length_bucket) noexcept
{
    if (length_bucket != SequenceLengthBucket::VERY_LONG) return false;
    if (config.chaining_algorithm != ChainingAlgorithmType::RMQ) return false;
    const char* override_env = std::getenv("BIOSEQ_BENCH_INCLUDE_RMQ_VERY_LONG");
    if (override_env != nullptr && override_env[0] == '1') return false;
    return true;
}

/// Per-call wall-clock budget (seconds). A single sample exceeding this
/// budget aborts the (algorithm, pair) measurement — the partial
/// record is still emitted so the chart layer sees that something
/// happened, but no further pair iterations attempt the same config.
/// Set to 0 to disable the budget. Default 600 s (10 min).
[[nodiscard]] inline double per_call_budget_seconds() noexcept {
    const char* env = std::getenv("BIOSEQ_BENCH_PER_CALL_BUDGET_S");
    if (env == nullptr) return 600.0;
    char* end = nullptr;
    const double value = std::strtod(env, &end);
    return end != env ? value : 600.0;
}

/**
 * @brief Run the full algorithm grid for one (length_bucket, error_bucket).
 */
inline void run_matrix_for_bucket(
    SequenceLengthBucket length_bucket,
    ErrorRateBucket      error_bucket,
    std::uint64_t        seed,
    const std::vector<AlgorithmConfig>& algorithm_grid = default_algorithm_grid())
{
    const auto generator_params = detail::make_generator_params(
        length_bucket, error_bucket, seed);
    auto pairs = generate_test_sequences(generator_params);
    if (pairs.empty()) {
        JustLog("[bench.matrix_runner] No pairs generated for bucket=({},{})",
                static_cast<int>(length_bucket), static_cast<int>(error_bucket));
        return;
    }
    const std::size_t n_samples = detail::samples_per_pair(length_bucket);
    const double budget_s = per_call_budget_seconds();
    // Position-tolerance window used to decide "correct vs incorrect"
    // — sourced from the EvaluationParams default so the chart layer
    // and the SEAL-style `[simple-benchmark]` cases agree on the rule.
    constexpr std::int32_t position_tolerance_bp = 10;

    JustLog("[bench.matrix_runner] bucket=({}, {}): {} pairs × {} samples × {} configs (budget {}s/call)",
            sequence_length_bucket_name(length_bucket),
            error_rate_bucket_name(error_bucket),
            pairs.size(), n_samples, algorithm_grid.size(), budget_s);

    for (const auto& config : algorithm_grid) {
        if (should_skip_cell(config, length_bucket)) {
            JustLog("[bench.matrix_runner] SKIP {} for bucket {} (use BIOSEQ_BENCH_INCLUDE_RMQ_VERY_LONG=1 to override)",
                    config.label,
                    sequence_length_bucket_name(length_bucket));
            continue;
        }
        const auto aligner_params = detail::build_aligner_params(config);
        bool config_aborted = false;
        for (auto& pair : pairs) {
            if (config_aborted) break;
            BenchSample record;
            record.algorithm_label   = config.label;
            record.algorithm_kind    = config.is_minimap2_reference ? "minimap2" : "bioseq";
            // For the minimap2 reference rows the chaining choice is
            // a knob on the C library, not a "bioseq chaining
            // algorithm" — render it as DP / RMQ instead of the
            // bioseq enum name to avoid the misleading "Minimap2
            // Scalar" label on a row whose `algorithm_label` already
            // says "minimap2 (DP chaining)".
            if (config.is_minimap2_reference) {
                record.chaining_label = config.chaining_algorithm ==
                    bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType::RMQ
                    ? "RMQ" : "DP";
            } else {
                record.chaining_label =
                    bioseq::simd::HWY_NAMESPACE::chaining_algorithm_name(
                        config.chaining_algorithm);
            }
            record.aligner_label     = config.is_minimap2_reference
                ? std::string{"n/a"}
                : std::string{bioseq::simd::HWY_NAMESPACE::base_alignment_algorithm_name(
                    config.base_alignment_algorithm)};
            record.minimizer_label   = config.is_minimap2_reference
                ? std::string{"n/a"}
                : std::string{bioseq::simd::HWY_NAMESPACE::minimizer_algorithm_name(
                    config.minimizer_algorithm)};
            record.length_bucket     = length_bucket;
            record.error_bucket      = error_bucket;
            record.length_bucket_label = sequence_length_bucket_name(length_bucket);
            record.error_bucket_label  = error_rate_bucket_name(error_bucket);
            record.requested_query_length = pair.original_query_length;
            record.effective_query_length = pair.query.bp_size();
            record.target_length          = pair.target.bp_size();
            record.effective_error_rate   = pair.effective_error_rate;

            const auto pair_t0 = std::chrono::steady_clock::now();
            // The bioseq pipeline can throw allocator exceptions
            // (foonathan_memory::bad_node_size — "allocation node
            // size exceeds supported maximum of allocator") on inputs
            // it doesn't expect. Catching here means one bad
            // (config, pair) drops a single record instead of
            // aborting the whole sweep — the rest of the matrix
            // still produces meaningful chart data. The exception
            // shape is documented in
            // RESEARCH/BENCHMARKING_SPEEDUP/REPORT_PLOTLY_JSON_DATA.md.
            try {
                detail::run_one_pair_samples(
                    pair, config, aligner_params, n_samples,
                    position_tolerance_bp, record);
            } catch (const std::exception& ex) {
                JustLog("[bench.matrix_runner] EXCEPTION on {} bucket {} pair query={}bp: {}",
                        config.label,
                        sequence_length_bucket_name(length_bucket),
                        pair.query.bp_size(), ex.what());
                // Record the failure but keep iterating to the next
                // pair; the chart layer treats empty
                // sample_latencies_ns as "skip this row".
                record.sample_latencies_ns.clear();
                record.sample_peak_rss_delta_bytes.clear();
                record.produced_primary = false;
            } catch (...) {
                JustLog("[bench.matrix_runner] UNKNOWN EXCEPTION on {} bucket {} pair query={}bp",
                        config.label,
                        sequence_length_bucket_name(length_bucket),
                        pair.query.bp_size());
                record.sample_latencies_ns.clear();
                record.sample_peak_rss_delta_bytes.clear();
                record.produced_primary = false;
            }
            const auto pair_t1 = std::chrono::steady_clock::now();
            const double pair_seconds =
                std::chrono::duration<double>(pair_t1 - pair_t0).count();
            // Sort once so percentile reads on the chart side are O(1).
            std::sort(record.sample_latencies_ns.begin(),
                      record.sample_latencies_ns.end());
            std::sort(record.sample_peak_rss_delta_bytes.begin(),
                      record.sample_peak_rss_delta_bytes.end());
            benchmark_samples().push_back(std::move(record));

            if (budget_s > 0.0 && pair_seconds > budget_s) {
                JustLog("[bench.matrix_runner] ABORT {} on bucket {} after {:.1f}s (>{:.1f}s budget)",
                        config.label,
                        sequence_length_bucket_name(length_bucket),
                        pair_seconds, budget_s);
                config_aborted = true;
            }
        }
    }
}

/**
 * @brief Convenience: run the full sweep across every (length, error)
 *        bucket in display order.
 */
inline void run_full_matrix(std::uint64_t base_seed = 1234567ull,
                            const std::vector<AlgorithmConfig>& algorithm_grid =
                                default_algorithm_grid())
{
    std::uint64_t seed = base_seed;
    for (auto length_bucket : {SequenceLengthBucket::SHORT,
                               SequenceLengthBucket::MEDIUM,
                               SequenceLengthBucket::LONG,
                               SequenceLengthBucket::VERY_LONG}) {
        for (auto error_bucket : {ErrorRateBucket::LOW,
                                  ErrorRateBucket::MEDIUM,
                                  ErrorRateBucket::HIGH}) {
            run_matrix_for_bucket(length_bucket, error_bucket, seed, algorithm_grid);
            // Mix the seed by a prime so each bucket sees a distinct
            // RNG state but the run is still deterministic for a given
            // base_seed.
            seed = seed * 1099511628211ull + 14695981039346656037ull;
        }
    }
}

}  // namespace bioseq::bench::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();

}  // export
