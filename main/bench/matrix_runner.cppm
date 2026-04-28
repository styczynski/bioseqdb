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
#include <cstdio>
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
import bioseq_instr;
import bioseq_instr.config;
import bioseq_instr.descriptor;
import bioseq_instr.aggregate;
import bioseq_instr.thread_state;

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
    VERY_LONG = 3,  ///< [1000000, 3M)    — assembly-to-assembly
    XL        = 4,  ///< [5M, 15M)        — chromosome-scale assemblies
                    ///<                    Used by the long-only iteration
                    ///<                    bench to verify the user's stated
                    ///<                    range (>1M, <20M); not part of the
                    ///<                    default sweep.
};

[[nodiscard]] constexpr const char* sequence_length_bucket_name(SequenceLengthBucket bucket) {
    switch (bucket) {
        case SequenceLengthBucket::SHORT:     return "short (<1kb)";
        case SequenceLengthBucket::MEDIUM:    return "medium (1k-10k)";
        case SequenceLengthBucket::LONG:      return "long (10k-1M)";
        case SequenceLengthBucket::VERY_LONG: return "very long (1M-3M)";
        case SequenceLengthBucket::XL:        return "xl (5M-15M)";
    }
    return "unknown";
}

/**
 * @brief Buckets for the error-rate axis.
 *
 * Replaces the original 3-bucket low/medium/high split (which was
 * pinned to 0.6%, 5.6%, 26% effective error). The original HIGH at
 * 26% was harsh enough that even minimap2 lost the alignment for
 * everything past the SHORT bucket — see
 * `RESEARCH/BENCHMARKING_SPEEDUP/POOR_RESULTS_REPORT.md` for the full
 * cause analysis.
 *
 * The new buckets are calibrated against three published benchmarking
 * studies that explicitly characterise long-read sequencing error
 * profiles:
 *
 *  * Prodanov & Bansal (Nucleic Acids Res. 2020 — DuploMap) use SimLoRD's
 *    default PacBio CLR profile of 0.11 ins / 0.04 del / 0.01 sub
 *    (~16% total) and a "high error" profile of 0.15/0.09/0.04 (~28%).
 *    Insertions dominate by a factor of ~10 over substitutions in raw
 *    PacBio CLR — the inverse of what NGS data exhibits.
 *
 *  * Rohmer, Touzet & Limasset (PeerJ 2024 — MSA_Limit) sweep error
 *    rates from 1% to 30% in 1, 2, 5, 10, 15, 20, 25, 30 steps and
 *    document an ONT MIX model of 23% subs / 31% ins / 46% dels (so
 *    deletions are the largest error type for ONT R9). They also
 *    benchmark four real datasets ranging from 6.6% (Human / T2T) to
 *    17.28% (E. coli HiFi reference + ONT reads).
 *
 *  * Ruffalo & LaFramboise (SEAL — Bioinformatics 2011) sweep
 *    NGS-style error rates from 0% to 10% with separate indel-frequency
 *    sweeps from 1e-5 to 1e-2; their canonical NGS profile is
 *    sub-dominated (~1% sub, indels rare).
 *
 * Buckets in display order:
 *   HIFI                — modern PacBio HiFi / NGS Illumina (~0.2% effective,
 *                         substitutions slightly dominate); the floor of
 *                         what real-world data exhibits today.
 *   CCS                 — PacBio CCS (multi-pass) / typical Illumina mix
 *                         (~1.5%, balanced sub/ins/del).
 *   ONT_MODERN          — ONT R10 / late R9.4 (~5%, indels ~9× subs);
 *                         matches the Human dataset in PeerJ Table 1.
 *   ONT_LEGACY          — ONT R9 / PacBio CLR (~10%, indels ~9× subs);
 *                         straddles SimLoRD's default 16% case but a
 *                         touch tamer so minimap2 still recovers.
 *   HIGH_INDEL_LOW_SUB  — exotic / adversarial: ~8% effective, but ALL
 *                         from indels with a near-zero substitution
 *                         component. Models pathological homopolymer-
 *                         heavy ONT runs and tests how aligners handle
 *                         pure gap structure (PeerJ "INS"+"DEL"-only
 *                         datasets at 5-10% — Fig. 7 of that paper).
 *   HIGH_SUB_LOW_INDEL  — exotic / adversarial: ~5% but ALL from
 *                         substitutions (the inverse of the above).
 *                         Reproduces the SEAL substitution-only sweeps
 *                         and probes how chaining handles dense SNV
 *                         clusters with no gaps.
 */
enum class ErrorRateBucket : uint8_t {
    HIFI                = 0,
    CCS                 = 1,
    ONT_MODERN          = 2,
    ONT_LEGACY          = 3,
    HIGH_INDEL_LOW_SUB  = 4,
    HIGH_SUB_LOW_INDEL  = 5,
};

[[nodiscard]] constexpr const char* error_rate_bucket_name(ErrorRateBucket bucket) {
    switch (bucket) {
        case ErrorRateBucket::HIFI:                return "hifi (~0.2%)";
        case ErrorRateBucket::CCS:                 return "ccs (~1.5%)";
        case ErrorRateBucket::ONT_MODERN:          return "ont_modern (~5%)";
        case ErrorRateBucket::ONT_LEGACY:          return "ont_legacy (~10%)";
        case ErrorRateBucket::HIGH_INDEL_LOW_SUB:  return "high_indel_low_sub (~8%)";
        case ErrorRateBucket::HIGH_SUB_LOW_INDEL:  return "high_sub_low_indel (~5%)";
    }
    return "unknown";
}

/// Short label without the parenthetical effective error rate — used
/// where chart real estate is tight (heatmap column headers, etc.).
[[nodiscard]] constexpr const char* error_rate_bucket_short_label(ErrorRateBucket bucket) {
    switch (bucket) {
        case ErrorRateBucket::HIFI:                return "hifi";
        case ErrorRateBucket::CCS:                 return "ccs";
        case ErrorRateBucket::ONT_MODERN:          return "ont_modern";
        case ErrorRateBucket::ONT_LEGACY:          return "ont_legacy";
        case ErrorRateBucket::HIGH_INDEL_LOW_SUB:  return "high_indel";
        case ErrorRateBucket::HIGH_SUB_LOW_INDEL:  return "high_sub";
    }
    return "?";
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
    ErrorRateBucket      error_bucket  = ErrorRateBucket::HIFI;
    std::string length_bucket_label;
    std::string error_bucket_label;

    std::size_t requested_query_length = 0;
    std::size_t effective_query_length = 0;
    std::size_t target_length          = 0;

    double effective_error_rate = 0.0;

    /// Per-sample minimizer parameters. Recorded so the chart layer
    /// can pivot on (k, w) for the parameter-sweep charts. For the
    /// default algorithm grid these are pinned to (15, 10) — the
    /// minimap2 `map-ont` preset values; the dedicated k/w-sweep
    /// configurations vary them across the canonical short-read,
    /// long-read and assembly presets.
    int32_t kmer_size   = 15;
    int32_t window_size = 10;

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

    /// Per-pipeline-phase mean nanoseconds per timed sample. Populated by
    /// snapshotting the persistent profiler aggregates (`mm_sketch`,
    /// `mg_lchain_*`, `mm_align_skeleton`, etc.) before and after the timed
    /// sample loop and dividing the delta by `sample_latencies_ns.size()`.
    /// First element is the phase descriptor name (matches the
    /// `register_region_static` literal in the source); second is the mean
    /// ns per sample for that phase. Empty when no timed samples were
    /// captured (e.g. the per-pair budget tripped or an exception fired).
    std::vector<std::pair<std::string, double>> phase_mean_ns_per_sample;

    /// Per-phase mean PMU counters per timed sample. Same key as
    /// `phase_mean_ns_per_sample` (the descriptor name); fields are mean
    /// cycles / instructions / branches / branch_misses *per call*. The
    /// values are zero on platforms without PMU access (Linux without
    /// CAP_PERFMON, macOS without root). The chart layer treats all-zero
    /// columns as "PMU not available — hide the chart".
    struct PhaseCounters {
        std::string name;
        double mean_cycles_per_sample        = 0.0;
        double mean_instructions_per_sample  = 0.0;
        double mean_branches_per_sample      = 0.0;
        double mean_branch_misses_per_sample = 0.0;
    };
    std::vector<PhaseCounters> phase_counters_per_sample;
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
    /// Minimizer parameters. Default (15, 10) matches minimap2's
    /// `-x map-ont` preset and the existing reference output documented
    /// in CODE_GUIDELINES.md §9. The k/w-sweep grid varies these to
    /// answer the "how do k and w affect quality?" question that the
    /// default grid cannot answer (since it pins both).
    int32_t kmer_size   = 15;
    int32_t window_size = 10;
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
    //
    // VERY_LONG was reduced from 2 Mbp to 1 Mbp (and LONG from 100 kbp
    // to 50 kbp). Reason: the previous configuration consistently
    // exceeded the bench wall-clock budget on the LONG/VERY_LONG
    // buckets — the runner never reached them and the report was
    // missing those cells entirely. The PeerJ benchmark in
    // SOME_BENCHMARKING_PAPERS.md covers up to 10 kbp regions and the
    // DuploMap study uses 8.5-50 kbp PacBio reads, so 50 kbp / 1 Mbp
    // still spans the published long-read territory.
    switch (length_bucket) {
        case SequenceLengthBucket::SHORT:
            params.fragment_length_mean   = 300;
            params.fragment_length_stddev = 50.0;
            params.reference_length       = 3'000;
            params.fragment_count         = 12;
            break;
        case SequenceLengthBucket::MEDIUM:
            params.fragment_length_mean   = 5'000;
            params.fragment_length_stddev = 500.0;
            params.reference_length       = 30'000;
            params.fragment_count         = 6;
            break;
        case SequenceLengthBucket::LONG:
            params.fragment_length_mean   = 50'000;
            params.fragment_length_stddev = 3'000.0;
            params.reference_length       = 250'000;
            params.fragment_count         = 3;
            break;
        case SequenceLengthBucket::VERY_LONG:
            params.fragment_length_mean   = 1'000'000;
            params.fragment_length_stddev = 50'000.0;
            params.reference_length       = 1'500'000;
            params.fragment_count         = 1;
            break;
        case SequenceLengthBucket::XL:
            // ~5 Mbp fragments out of a 7 Mbp reference — squarely in the
            // user's stated >1 Mbp / <20 Mbp range. One pair per seed so the
            // long-only iteration bench finishes in a few minutes per pass.
            params.fragment_length_mean   = 5'000'000;
            params.fragment_length_stddev = 100'000.0;
            params.reference_length       = 7'000'000;
            params.fragment_count         = 1;
            break;
    }

    // Error rates and split between substitutions / insertions / deletions
    // are calibrated against the published long-read sequencing
    // benchmarks documented next to the ErrorRateBucket enum above.
    //
    // The indel:substitution ratio matters more than the absolute total.
    // Real long-read data has indels dominating subs by ~10× (DuploMap,
    // PeerJ MIX model = 23% sub / 31% ins / 46% del); the original
    // benchmark inverted this ratio, which is why even minimap2 lost
    // alignments past 5% effective error.
    //
    // The indel-length means are kept conservative (1.5-3 bp) to stay
    // within ksw2's z-drop default of 400, mirroring what minimap2's
    // -x map-ont preset assumes.
    switch (error_bucket) {
        case ErrorRateBucket::HIFI:
            // ~0.2% total. Modern PacBio HiFi (Wenger 2019) and
            // Illumina-typical NGS. SEAL's NGS profile (Bioinformatics
            // 2011) is also in this range.
            params.substitution_rate       = 0.001;
            params.insertion_rate          = 0.0005;
            params.deletion_rate           = 0.0005;
            params.insertion_length_mean   = 1.0;
            params.deletion_length_mean    = 1.0;
            break;
        case ErrorRateBucket::CCS:
            // ~1.5% total, balanced sub/indel mix. PacBio CCS multi-pass
            // and the Human dataset of PeerJ Table 1 (6.6%) reduced to
            // a more comfortable middle ground that still has every
            // error type represented.
            params.substitution_rate       = 0.005;
            params.insertion_rate          = 0.005;
            params.deletion_rate           = 0.005;
            params.insertion_length_mean   = 1.5;
            params.deletion_length_mean    = 1.5;
            break;
        case ErrorRateBucket::ONT_MODERN:
            // ~5% total. ONT R10 / late R9.4. Indels dominate by ~9×
            // matching the PeerJ MIX model proportions (23% sub /
            // 77% indel) at ~5% absolute. Closest published reference:
            // the 6.6% Human dataset in PeerJ Table 1.
            params.substitution_rate       = 0.005;
            params.insertion_rate          = 0.025;
            params.deletion_rate           = 0.020;
            params.insertion_length_mean   = 2.0;
            params.deletion_length_mean    = 2.0;
            break;
        case ErrorRateBucket::ONT_LEGACY:
            // ~10% total. ONT R9 / PacBio CLR. SimLoRD's default profile
            // (DuploMap paper) is 11% ins / 4% del / 1% sub = 16%; this
            // is a tamer cousin (5% ins / 4% del / 1% sub = 10%) so
            // minimap2 still produces a primary at the 50 kbp LONG
            // bucket, while keeping the indel:sub bias intact.
            params.substitution_rate       = 0.01;
            params.insertion_rate          = 0.05;
            params.deletion_rate           = 0.04;
            params.insertion_length_mean   = 2.5;
            params.deletion_length_mean    = 2.5;
            break;
        case ErrorRateBucket::HIGH_INDEL_LOW_SUB:
            // ~8% total. ALL the error budget is in indels (4% ins +
            // 4% del); substitution rate is effectively zero. Mirrors
            // the PeerJ "INS-only" + "DEL-only" sweeps (Fig. 7) at the
            // mid-rate point. Probes whether the chaining DP and
            // base-aligner can handle pure gap structure with no
            // mismatch noise.
            params.substitution_rate       = 0.001;
            params.insertion_rate          = 0.04;
            params.deletion_rate           = 0.04;
            params.insertion_length_mean   = 2.5;
            params.deletion_length_mean    = 2.5;
            break;
        case ErrorRateBucket::HIGH_SUB_LOW_INDEL:
            // ~5% total. ALL the budget is in substitutions; indels are
            // effectively zero. Reproduces the SEAL substitution-rate
            // sweep at the upper end. Tests whether minimizer hits
            // survive dense SNV clusters when no gaps perturb the
            // anchor structure.
            params.substitution_rate       = 0.05;
            params.insertion_rate          = 0.0005;
            params.deletion_rate           = 0.0005;
            params.insertion_length_mean   = 1.0;
            params.deletion_length_mean    = 1.0;
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
        // VERY_LONG / XL — bumped from 1 to multiple samples so the
        // per-pair mean has lower noise. With the per-seed-pair gap-fill
        // optimisation in `align_regions`, both VERY_LONG and XL fit
        // comfortably under the 600 s wall-clock budget at these sample
        // counts.
        case SequenceLengthBucket::VERY_LONG: return 3;
        case SequenceLengthBucket::XL:        return 2;
    }
    return 1;
}

/// Apply the algorithm config onto a fresh `Minimap2Params` instance.
[[nodiscard]] inline Minimap2Params build_aligner_params(const AlgorithmConfig& config) {
    Minimap2Params params;
    params.chaining_algorithm        = config.chaining_algorithm;
    params.base_alignment_algorithm  = config.base_alignment_algorithm;
    params.minimizer_algorithm       = config.minimizer_algorithm;
    params.kmer_size                 = config.kmer_size;
    params.window_size               = config.window_size;
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

    // ---- Snapshot per-phase aggregates before the timed loop. ----
    // The persistent profiler block accumulates region totals across the
    // entire bench run, so per-pair phase totals are computed as the
    // (after - before) delta around the timed loop. We deliberately
    // exclude the warm-up above (its allocator-cold sample is what
    // pollutes the percentile estimates and we treat it the same way for
    // phase accounting). The snapshot type matches the bench's
    // `args_t = context_args_profiling_timing_only` so the tls_profiler
    // pointer the bench installed at TEST_CASE start lines up.
    using phase_args_t = bioseq::instr::context_args_profiling_timing_only;
    auto& phase_prof = bioseq::instr::persistent_profiler_block<phase_args_t>();
    const std::uint32_t phase_descriptor_count =
        bioseq::instr::descriptor_count();
    // We capture timing AND PMU counters (cycles / instructions /
    // branches / branch_misses) for every region — the bench's args
    // has enable_counters=true now, so the RegionAggregate fields are
    // populated when the kernel grants PMU access. We snapshot four
    // separate baselines per id alongside total_ns so the post-loop
    // delta is a clean (after - before) for each metric.
    struct PhaseBaseline {
        std::uint64_t total_ns          = 0;
        std::uint64_t cycles            = 0;
        std::uint64_t instructions      = 0;
        std::uint64_t branches          = 0;
        std::uint64_t branch_misses     = 0;
    };
    std::vector<PhaseBaseline> phase_baseline(phase_descriptor_count + 1);
    for (std::uint32_t id = 1; id <= phase_descriptor_count; ++id) {
        auto& base = phase_baseline[id];
        if (auto* la = phase_prof.loop_aggregates.try_get(id)) {
            base.total_ns          += la->total_ns;
            base.cycles            += la->total_cycles;
            base.instructions      += la->total_instructions;
            base.branches          += la->total_branches;
            base.branch_misses     += la->total_branch_misses;
        }
        if (auto* ra = phase_prof.aggregates.try_get(id)) {
            base.total_ns          += ra->total_ns;
            base.cycles            += ra->total_cycles;
            base.instructions      += ra->total_instructions;
            base.branches          += ra->total_branches;
            base.branch_misses     += ra->total_branch_misses;
        }
    }

    // ---- Timed sample loop ----
    // Memory-tracking syscalls (`current_resident_bytes()` → macOS
    // `task_info` / Linux `/proc/self/statm`) are kept *outside* the
    // per-sample t0..t1 window so they don't pollute latency
    // measurements. Per-sample RSS deltas are coarse-grained: we
    // record only the sample-mean delta across the whole loop, since
    // per-sample syscalls were previously costing ~few µs each on
    // macOS and stacking up between iterations (n_samples × 2
    // syscalls). The chart layer reads `sample_peak_rss_delta_bytes`
    // for both per-sample distributions and aggregates; with the
    // mean-only model we fill every sample slot with the same delta
    // so existing chart code keeps working without changes.
    const auto rss_baseline = current_resident_bytes();
    for (std::size_t sample_index = 0; sample_index < n_samples; ++sample_index) {
        const auto t0 = clock::now();
        bool _ignored_pp = false;
        int  _ignored_mapq = 0;
        int  _ignored_ts = 0;
        bool _ignored_rev = false;
        run_aligner_once(_ignored_pp, _ignored_mapq, _ignored_ts, _ignored_rev);
        const auto t1 = clock::now();

        const double elapsed_ns =
            std::chrono::duration<double, std::nano>(t1 - t0).count();
        record.sample_latencies_ns.push_back(elapsed_ns);
    }
    const auto rss_after = current_resident_bytes();
    const std::int64_t rss_delta_bytes =
        static_cast<std::int64_t>(rss_after) -
        static_cast<std::int64_t>(rss_baseline);
    // Negative deltas (allocator returned memory between calls) are
    // clamped to zero so the chart axis stays positive — the
    // underlying current-RSS reading is meaningful, but a per-call
    // delta is still subject to allocator round-trips. Per-sample
    // delta is the loop-total divided by sample count.
    const double mean_rss_delta = (rss_delta_bytes > 0 && n_samples > 0)
        ? static_cast<double>(rss_delta_bytes) / static_cast<double>(n_samples)
        : 0.0;
    record.sample_peak_rss_delta_bytes.assign(n_samples, mean_rss_delta);

    // ---- Snapshot per-phase aggregates after the timed loop. ----
    // The mean phase ns per sample is the delta divided by sample count.
    // We only record phases that registered any time during this loop
    // (delta > 0); idle phases stay out of the JSON so the chart layer
    // doesn't render zero-height bars. Phase names are taken from the
    // descriptor's `name` field exactly as registered in
    // `register_region_static("...")` so the chart legend matches the
    // existing instrumentation summary tables. PMU counters (cycles,
    // instructions, branches, branch_misses) are captured alongside
    // timing so the chart layer can render counter-comparison charts
    // beside the wall-clock chart.
    if (!record.sample_latencies_ns.empty()) {
        const double samples_d =
            static_cast<double>(record.sample_latencies_ns.size());
        record.phase_mean_ns_per_sample.reserve(16);
        record.phase_counters_per_sample.reserve(16);
        for (std::uint32_t id = 1; id <= phase_descriptor_count; ++id) {
            std::uint64_t after_ns          = 0;
            std::uint64_t after_cycles      = 0;
            std::uint64_t after_instructions = 0;
            std::uint64_t after_branches    = 0;
            std::uint64_t after_branch_miss = 0;
            if (auto* la = phase_prof.loop_aggregates.try_get(id)) {
                after_ns          += la->total_ns;
                after_cycles      += la->total_cycles;
                after_instructions += la->total_instructions;
                after_branches    += la->total_branches;
                after_branch_miss += la->total_branch_misses;
            }
            if (auto* ra = phase_prof.aggregates.try_get(id)) {
                after_ns          += ra->total_ns;
                after_cycles      += ra->total_cycles;
                after_instructions += ra->total_instructions;
                after_branches    += ra->total_branches;
                after_branch_miss += ra->total_branch_misses;
            }
            if (after_ns <= phase_baseline[id].total_ns) {
                continue;
            }
            const auto& base = phase_baseline[id];
            const std::uint64_t delta_ns      = after_ns          - base.total_ns;
            const std::uint64_t d_cycles      = after_cycles      >= base.cycles
                ? after_cycles - base.cycles : 0ull;
            const std::uint64_t d_instr       = after_instructions >= base.instructions
                ? after_instructions - base.instructions : 0ull;
            const std::uint64_t d_branches    = after_branches    >= base.branches
                ? after_branches - base.branches : 0ull;
            const std::uint64_t d_branch_miss = after_branch_miss >= base.branch_misses
                ? after_branch_miss - base.branch_misses : 0ull;
            const auto* desc = bioseq::instr::descriptor_lookup(id);
            if (desc == nullptr || desc->name == nullptr) continue;
            std::string phase_name{desc->name};
            record.phase_mean_ns_per_sample.emplace_back(
                phase_name,
                static_cast<double>(delta_ns) / samples_d);
            BenchSample::PhaseCounters pc;
            pc.name                          = std::move(phase_name);
            pc.mean_cycles_per_sample        = static_cast<double>(d_cycles)      / samples_d;
            pc.mean_instructions_per_sample  = static_cast<double>(d_instr)       / samples_d;
            pc.mean_branches_per_sample      = static_cast<double>(d_branches)    / samples_d;
            pc.mean_branch_misses_per_sample = static_cast<double>(d_branch_miss) / samples_d;
            record.phase_counters_per_sample.push_back(std::move(pc));
        }
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
    // The "fastest" bioseq config (KSW2 + SIMD-DP + two-stack-mm) is the
    // V4 candidate currently being tuned to beat scalar-mm by ≥10 %. Add
    // it once at the end so it appears in every chart that pivots on
    // algorithm without bloating the grid into the full 18-row Cartesian.
    // The two-stack collector is implemented in
    // `kmers/two_stacks_minimizers.cppm` and dispatched in
    // `minimap_port.cppm` via `MinimizerAlgorithmType::TWO_STACK`.
    {
        AlgorithmConfig cfg;
        cfg.label = "bioseq KSW2 + SIMD-DP + two-stack-mm";
        cfg.is_minimap2_reference        = false;
        cfg.chaining_algorithm           = ChainingAlgorithmType::SIMD_MM2;
        cfg.base_alignment_algorithm     = BaseAlignmentAlgorithmType::KSW2;
        cfg.minimizer_algorithm          = MinimizerAlgorithmType::TWO_STACK;
        grid.push_back(std::move(cfg));
    }
    return grid;
}

/// Decide whether to skip a (config, length_bucket, error_bucket) cell.
///
/// One remaining skip rule:
///
///  (LONG | VERY_LONG) × ONT_LEGACY/HIGH_INDEL_LOW_SUB: at 8-10%
///  indel rate on a 50 kbp+ pair, the alignment block expands far
///  beyond ksw2's z-drop threshold and the chaining DP fails to
///  find a long-enough chain. Even minimap2 returns no primary in
///  that regime (the original 26%-error HIGH bucket reproduced this
///  for *every* length past SHORT, which is why the prior report
///  was empty for LONG/VERY_LONG). The error-noisy buckets stay on
///  SHORT/MEDIUM where the chain length is comfortably within
///  range, while the lower-error HIFI / CCS / ONT_MODERN buckets
///  run on every length so the length-vs-accuracy charts still have
///  data. Override via `BIOSEQ_BENCH_INCLUDE_NOISY_LONG=1`.
///
/// Previous versions also skipped bioseq RMQ chaining for LONG /
/// VERY_LONG inputs because of an AVL-tree balance corruption that
/// segfaulted on ~50 kbp queries. That bug was tracked down to
/// swapped (p, q) balance assignments in the double-rotation case
/// of `RmqTree::insert` and `RmqTree::erase` (and to ancestor
/// size/min updates that were skipped after the early-break in the
/// rebalance loop) — see chaining_rmq.cppm. The skip rule is gone;
/// RMQ chaining now runs to completion on every length bucket.
[[nodiscard]] inline bool should_skip_cell(
    const AlgorithmConfig& config,
    SequenceLengthBucket length_bucket,
    ErrorRateBucket error_bucket) noexcept
{
    const bool is_long_or_longer =
        length_bucket == SequenceLengthBucket::LONG ||
        length_bucket == SequenceLengthBucket::VERY_LONG;

    // Noisy × long: chain too short for ksw2 to recover on 50 kbp+ pairs.
    const bool is_noisy_bucket =
        error_bucket == ErrorRateBucket::ONT_LEGACY ||
        error_bucket == ErrorRateBucket::HIGH_INDEL_LOW_SUB;
    if (is_noisy_bucket && is_long_or_longer) {
        const char* override_env = std::getenv("BIOSEQ_BENCH_INCLUDE_NOISY_LONG");
        if (override_env == nullptr || override_env[0] != '1') return true;
    }

    // Suppress the now-unused `config` parameter warning.
    (void)config;
    return false;
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
    // Crash-diagnostic logging gated behind `BIOSEQ_BENCH_VERBOSE=1`.
    // JustLog routes through fmtlog (async, buffered) so a SIGSEGV in
    // this function loses the buffered messages — we used these
    // unbuffered fprintf-to-stderr calls to track down the
    // bioseq-RMQ-on-LONG segfault that's now skipped via
    // `should_skip_cell`. Leave the hooks in place behind the env-var
    // toggle so the next time something hangs / crashes we can
    // reproduce the trace without having to re-edit the source.
    const bool verbose = []{
        const char* env = std::getenv("BIOSEQ_BENCH_VERBOSE");
        return env != nullptr && env[0] == '1';
    }();
    if (verbose) {
        std::fprintf(stderr, "[matrix_runner.entry] bucket=(%s, %s) seed=%llu grid_size=%zu\n",
                     sequence_length_bucket_name(length_bucket),
                     error_rate_bucket_name(error_bucket),
                     static_cast<unsigned long long>(seed),
                     algorithm_grid.size());
        std::fflush(stderr);
    }
    const auto generator_params = detail::make_generator_params(
        length_bucket, error_bucket, seed);
    auto pairs = generate_test_sequences(generator_params);
    if (verbose) {
        std::fprintf(stderr, "[matrix_runner.gen_done] bucket=(%s, %s) %zu pairs\n",
                     sequence_length_bucket_name(length_bucket),
                     error_rate_bucket_name(error_bucket),
                     pairs.size());
        std::fflush(stderr);
    }
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
        if (should_skip_cell(config, length_bucket, error_bucket)) {
            JustLog("[bench.matrix_runner] SKIP {} for bucket {}/{} (RMQ+very_long or noisy+long; set BIOSEQ_BENCH_INCLUDE_RMQ_VERY_LONG / BIOSEQ_BENCH_INCLUDE_NOISY_LONG to override)",
                    config.label,
                    sequence_length_bucket_name(length_bucket),
                    error_rate_bucket_name(error_bucket));
            continue;
        }
        if (verbose) {
            std::fprintf(stderr, "[matrix_runner.config_start] %s for bucket %s/%s\n",
                         config.label.c_str(),
                         sequence_length_bucket_name(length_bucket),
                         error_rate_bucket_name(error_bucket));
            std::fflush(stderr);
        }
        const auto aligner_params = detail::build_aligner_params(config);
        bool config_aborted = false;
        std::size_t pair_index = 0;
        for (auto& pair : pairs) {
            if (config_aborted) break;
            if (verbose) {
                std::fprintf(stderr, "[matrix_runner.pair_start] %s pair %zu/%zu (qlen=%zu, tlen=%zu)\n",
                             config.label.c_str(), pair_index + 1, pairs.size(),
                             pair.query.bp_size(), pair.target.bp_size());
                std::fflush(stderr);
            }
            ++pair_index;
            BenchSample record;
            record.algorithm_label   = config.label;
            record.algorithm_kind    = config.is_minimap2_reference ? "minimap2" : "bioseq";
            record.kmer_size         = config.kmer_size;
            record.window_size       = config.window_size;
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
 *
 * The order is "fast cells first" so that if the bench is interrupted
 * midway (wall-clock budget hit, OS kill, etc.) the SHORT/MEDIUM data
 * for every (algorithm, error_bucket) is still preserved in the
 * accumulator. Earlier the order was length-major, so an interrupt
 * during LONG/HIGH would lose every HIGH-error sample for SHORT and
 * MEDIUM too.
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
        for (auto error_bucket : {ErrorRateBucket::HIFI,
                                  ErrorRateBucket::CCS,
                                  ErrorRateBucket::ONT_MODERN,
                                  ErrorRateBucket::ONT_LEGACY,
                                  ErrorRateBucket::HIGH_INDEL_LOW_SUB,
                                  ErrorRateBucket::HIGH_SUB_LOW_INDEL}) {
            run_matrix_for_bucket(length_bucket, error_bucket, seed, algorithm_grid);
            // Mix the seed by a prime so each bucket sees a distinct
            // RNG state but the run is still deterministic for a given
            // base_seed.
            seed = seed * 1099511628211ull + 14695981039346656037ull;
        }
    }
}

// =====================================================================
// k/w sweep — varies kmer_size and window_size while pinning the
// algorithm choice to (minimap2, bioseq KSW2 + scalar-DP + scalar-mm).
// Only one length bucket and a single error bucket are exercised so
// the chart can isolate the parameter-impact signal from algorithmic
// confounds.
// =====================================================================

/**
 * @brief Build the k/w sweep grid: a small Cartesian of (k, w) ×
 *        (minimap2 reference, bioseq KSW2 + scalar-DP + scalar-mm).
 *
 * The sweep is the **Cartesian product** of:
 *   - k in {11, 15, 19, 21, 25}  — odd values only, see below
 *   - w in {5, 10, 15}            — spans the published preset range
 *                                   (5 for `-x map-pb`, 10 for `-x map-ont`,
 *                                   ≥10 for `-x asm*` presets)
 *
 * k must be **odd**: the C minimap2 reference's `mm_sketch`
 * (sketch.c line 140) asserts `l == i + 1`, an invariant that breaks
 * whenever a palindromic k-mer triggers `kmer[0] == kmer[1]` and the
 * loop `continue`s without incrementing `l`. Palindromes over
 * {A,C,G,T} require an even k (the middle base would need to be its
 * own complement) — so even-k values would crash the C reference and
 * abort the bench. We sweep only odd k values to avoid that.
 *
 * 5 × 3 = 15 (k, w) cells × 2 algorithms = 30 configurations. At the
 * MEDIUM/CCS bucket (6 pairs, 5 samples per pair) that's 900 timed
 * iterations, still fast.
 *
 * Only two algorithms participate so the heatmap stays at one row
 * per algorithm rather than fanning across every bioseq variant —
 * the question we're answering is "how do k and w affect quality?",
 * not "how do bioseq variants compare to each other under k/w
 * variation".
 */
[[nodiscard]] inline std::vector<AlgorithmConfig> default_kw_sweep_grid() {
    constexpr std::array<int32_t, 5> kmer_axis = {11, 15, 19, 21, 25};
    constexpr std::array<int32_t, 3> window_axis = {5, 10, 15};

    std::vector<AlgorithmConfig> grid;
    grid.reserve(kmer_axis.size() * window_axis.size() * 2);

    for (const auto k : kmer_axis) {
        for (const auto w : window_axis) {
            AlgorithmConfig mm2_cfg;
            mm2_cfg.label = fmt::format("minimap2 (k={},w={})", k, w);
            mm2_cfg.is_minimap2_reference = true;
            mm2_cfg.chaining_algorithm = ChainingAlgorithmType::SCALAR_MM2;
            mm2_cfg.base_alignment_algorithm = BaseAlignmentAlgorithmType::KSW2;
            mm2_cfg.minimizer_algorithm = MinimizerAlgorithmType::SCALAR;
            mm2_cfg.kmer_size = k;
            mm2_cfg.window_size = w;
            grid.push_back(std::move(mm2_cfg));

            AlgorithmConfig bioseq_cfg;
            bioseq_cfg.label = fmt::format("bioseq KSW2 + scalar-DP + scalar-mm (k={},w={})", k, w);
            bioseq_cfg.is_minimap2_reference = false;
            bioseq_cfg.chaining_algorithm = ChainingAlgorithmType::SCALAR_MM2;
            bioseq_cfg.base_alignment_algorithm = BaseAlignmentAlgorithmType::KSW2;
            bioseq_cfg.minimizer_algorithm = MinimizerAlgorithmType::SCALAR;
            bioseq_cfg.kmer_size = k;
            bioseq_cfg.window_size = w;
            grid.push_back(std::move(bioseq_cfg));
        }
    }
    return grid;
}

/**
 * @brief Drive the k/w sweep: run `default_kw_sweep_grid()` on a
 *        single MEDIUM (5 kbp) / CCS (~1.5%) cell so the resulting
 *        accuracy depends only on k and w, not on length / error.
 *
 * The MEDIUM bucket is chosen because (a) it is small enough that
 * every (k, w) point completes within seconds even with the longer
 * configurations, and (b) at 5 kbp the chain has enough room to
 * differentiate "good" vs "bad" k/w choices (SHORT at 300 bp would
 * see chain lengths so small that any reasonable k/w succeeds).
 *
 * The CCS error bucket is the upper end of where every (k, w) point
 * is expected to succeed — pushing to ONT_MODERN would mask the k/w
 * signal behind error-rate failures.
 */
inline void run_kw_sweep(std::uint64_t base_seed = 9876543ull) {
    constexpr SequenceLengthBucket sweep_length = SequenceLengthBucket::MEDIUM;
    constexpr ErrorRateBucket      sweep_error  = ErrorRateBucket::CCS;
    const auto grid = default_kw_sweep_grid();
    JustLog("[bench.matrix_runner] k/w sweep: {} configs × MEDIUM/{} bucket",
            grid.size(), error_rate_bucket_name(sweep_error));
    run_matrix_for_bucket(sweep_length, sweep_error, base_seed, grid);
}

}  // namespace bioseq::bench::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();

}  // export
