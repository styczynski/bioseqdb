/**
 * @file samples.cppm
 * @brief Domain types for the aligner bench's measurement record.
 *
 * Pulled out of `matrix_runner.cppm` so the chart builder and the
 * regression-glue layer can `import` just the data shapes without
 * dragging the HWY-bound runner template into their translation unit.
 *
 * Everything here is intentionally aligner-specific:
 *
 *   * `SequenceLengthBucket` and `ErrorRateBucket` describe the bench's
 *     2D matrix axes — they're calibrated against published long-read
 *     benchmarks (DuploMap, PeerJ MSA_Limit, SEAL).
 *   * `BenchSample` carries the per-(algorithm, pair) measurement row
 *     the chart layer pivots on — sample latencies, RSS deltas, the
 *     phase-counter snapshot, and the SEAL-style position-correctness
 *     flags.
 *   * `benchmark_samples()` is the process-wide accumulator the matrix
 *     runner appends into and the chart builder reads from.
 *
 * The HWY runner adds nothing to this surface; it lives in a separate
 * `bench.aligners.matrix_runner` module that imports this one.
 */
module;

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

export module bench.aligners.samples;

export {

namespace bioseq::bench {

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
    XXL       = 5,  ///< [50M, 150M)      — full chromosome-grade pairs
                    ///<                    (10× XL by user request); single
                    ///<                    pair per error bucket, sample
                    ///<                    count clamped low. Skipped for
                    ///<                    every "noisy" error bucket.
};

[[nodiscard]] constexpr const char* sequence_length_bucket_name(SequenceLengthBucket bucket) {
    switch (bucket) {
        case SequenceLengthBucket::SHORT:     return "short (<1kb)";
        case SequenceLengthBucket::MEDIUM:    return "medium (1k-10k)";
        case SequenceLengthBucket::LONG:      return "long (10k-1M)";
        case SequenceLengthBucket::VERY_LONG: return "very long (1M-3M)";
        case SequenceLengthBucket::XL:        return "xl (5M-15M)";
        case SequenceLengthBucket::XXL:       return "xxl (50M-150M)";
    }
    return "unknown";
}

/**
 * @brief Buckets for the error-rate axis. Calibrated against three
 *        published benchmarks (DuploMap, PeerJ MSA_Limit, SEAL); see
 *        the original comment block in this enum's git history for the
 *        full citation list. Display order is HIFI → CCS → ONT_MODERN
 *        → ONT_LEGACY → HIGH_INDEL_LOW_SUB → HIGH_SUB_LOW_INDEL.
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
 * @brief One measurement row. Captures every axis the chart builder
 *        pivots on plus the per-sample latency vector so the builder
 *        can compute mean / P50 / P99 without re-running the workload.
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

}  // export
