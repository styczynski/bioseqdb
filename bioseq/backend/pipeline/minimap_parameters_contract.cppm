/**
 * @file
 * @brief Target-neutral parameters for one minimap-compatible alignment request.
 */
module;

#include <cstddef>
#include <cstdint>
#include <limits>

export module bioseq.pipeline.minimap_parameters_contract;

export import bioseq.pipeline.runtime_contract;
export import bioseq.chaining.core;
export import bioseq.highway.kernel_table_contract;

export namespace bioseq::simd {

using bioseq::chaining::chaining_mode;
using bioseq::chaining::chaining_parameters;
using bioseq::chaining::simd_chaining_policy;

/** @brief Runtime parameters and fixed-target binding for one alignment request. */
struct minimap2_parameters {
    /** Kernel table selected once for the request. */
    const ::bioseq::simd_dispatch::highway_kernel_table* highway_kernels = nullptr;
    /** Target inventory and planned target selected for the request. */
    ::bioseq::simd_dispatch::highway_dispatch_plan highway_dispatch{};
    /** Exact target that executed the request. */
    std::int64_t highway_executed_target = 0;
    /** Scheduler-owned alphabet code fixed by outer alphabet dispatch. */
    std::uint8_t alphabet_code =
        (std::numeric_limits<std::uint8_t>::max)();

    /** K-mer size in symbols. */
    std::int32_t kmer_size = 15;
    /** Minimizer window size in k-mers. */
    std::int32_t window_size = 10;
    /** Maximum reference gap accepted by chaining. */
    std::int32_t max_gap = 5000;
    /** Explicit reference gap, or -1 to use max_gap. */
    std::int32_t max_gap_ref = -1;
    /** Bandwidth shared by chaining and alignment. */
    std::int32_t bandwidth = 500;
    /** Maximum skipped predecessors before dynamic-programming termination. */
    std::int32_t max_chain_skip = 25;
    /** Maximum predecessors examined for one anchor. */
    std::int32_t max_chain_iter = 5000;
    /** Minimum seed count required for a chain. */
    std::int32_t min_seed_count = 3;
    /** Minimum accepted chain score. */
    std::int32_t min_chain_score = 40;
    /** Gap-penalty scale used by chaining. */
    float chain_gap_scale = 0.8f;
    /** Skip-penalty scale used by chaining. */
    float chain_skip_scale = 0.0f;
    /** Primary-to-secondary overlap threshold. */
    float mask_level = 0.5f;
    /** Maximum uncovered length used by overlap masking. */
    std::int32_t mask_len =
        (std::numeric_limits<std::int32_t>::max)();
    /** Secondary-to-primary score-ratio threshold. */
    float pri_ratio = 0.8f;
    /** Maximum number of secondary alignments to retain. */
    std::int32_t best_n = 5;

    /** Match score. */
    std::int32_t match_score = 2;
    /** Mismatch penalty. */
    std::int32_t mismatch_pen = 4;
    /** Short-gap open penalty. */
    std::int32_t gap_open1 = 4;
    /** Short-gap extension penalty. */
    std::int32_t gap_ext1 = 2;
    /** Long-gap open penalty. */
    std::int32_t gap_open2 = 24;
    /** Long-gap extension penalty. */
    std::int32_t gap_ext2 = 1;
    /** Ambiguous-symbol penalty. */
    std::int32_t sc_ambi = 1;
    /** Z-drop termination threshold. */
    std::int32_t zdrop = 400;
    /** Bonus for reaching sequence ends, or -1 when disabled. */
    std::int32_t end_bonus = -1;

    /** Maximum seed occurrence retained by filtering. */
    std::int32_t max_occ = 500;
    /** Mid-level seed-occurrence threshold. */
    std::int32_t mid_occ = 500;

    /**
     * Minimum gap length between consecutive seeds before invoking KSW2.
     * A zero value combines all seed-to-seed gaps into one KSW2 region.
     */
    std::int32_t min_ksw_len = 200;

    /** Enable base-level alignment and CIGAR generation. */
    bool do_cigar = true;
    /** Compute the divergence estimate from seeds. */
    bool compute_divergence = true;
    /** Compute mapping quality for output regions. */
    bool compute_mapq = true;
    /** Compute the exact unit-cost edit distance from traceback. */
    bool compute_edit_distance = false;

    /** Apply the exact strict edit predicate to aligned regions. */
    bool exact_edit_filter_enabled = false;
    /** Signed exclusive upper bound for the exact edit predicate. */
    std::int64_t exact_edit_filter_exclusive_upper_bound = 0;

    /** Enable the query-scoped SneakySnake lower-bound stage. */
    bool prefilter_enabled = false;
    /** Largest exact edit count admitted by the prefilter. */
    std::uint32_t prefilter_max_edits = 0;
    /** Backend selected for prefilter execution. */
    ::bioseq::prefilter::prefilter_backend prefilter_backend =
        ::bioseq::prefilter::prefilter_backend::automatic;

    /** Minimum block-aligner scratch block size. */
    std::size_t block_min_size = 16;
    /** Maximum block-aligner scratch block size. */
    std::size_t block_max_size = 32;

    /** Shared chaining mode selected by the runtime configuration. */
    ::bioseq::chaining::chaining_mode chaining_mode =
        ::bioseq::chaining::chaining_mode::scalar_dp;
    /** Long-join bandwidth, or zero when long joining is disabled. */
    std::int32_t bandwidth_long = 0;

    /** Chaining implementation selected once before the inner loop. */
    ::bioseq::simd::chaining_algorithm chaining_algorithm =
        ::bioseq::simd::chaining_algorithm::scalar_minimap2;
    /** Forward-chaining semantics used by SIMD chaining. */
    ::bioseq::chaining::simd_chaining_policy simd_chaining_policy =
        ::bioseq::chaining::simd_chaining_policy::exact_visited;
    /** Base-alignment implementation selected for the request. */
    ::bioseq::simd::base_alignment_algorithm base_alignment_algorithm =
        ::bioseq::simd::base_alignment_algorithm::block_aligner;
    /** Minimizer-extraction implementation selected for the request. */
    ::bioseq::simd::minimizer_algorithm minimizer_algorithm =
        ::bioseq::simd::minimizer_algorithm::scalar;

    /** Enable runtime execution accounting. */
    bool enable_profiling = false;
    /** Permit Vulkan-backed algorithms for this request. */
    bool vulkan_enabled = true;
    /** Select Vulkan seed-hit lookup. */
    bool vulkan_seed_hits_enabled = false;
    /** Cache GPU-resident minimizer indexes under their memory permits. */
    bool vulkan_index_cache_enabled = true;
    /** Select the resident two-submit Vulkan frontend. */
    bool vulkan_fused_frontend_enabled = false;
    /** Build the target minimizer index on the GPU. */
    bool vulkan_gpu_index_build_enabled = false;
    /** Enable GPU base-alignment extension under the block aligner. */
    bool vulkan_extend_enabled = false;
    /** Ordered-sketch tile size in base pairs. */
    std::uint32_t vulkan_ordered_sketch_tile_bp = 2048;
    /** Device radix-sort digit width in bits per pass. */
    std::uint32_t vulkan_radix_digit_bits = 8;

    /**
     * @brief Build the chaining configuration for this request.
     * @return Chaining parameters with derived values prepared and the shared
     * chaining mode synchronized with chaining_algorithm.
     */
    [[nodiscard]] auto make_chaining_parameters() const -> chaining_parameters {
        chaining_parameters parameters;
        parameters.kmer_size = kmer_size;
        parameters.max_gap = max_gap;
        parameters.bandwidth = bandwidth;
        parameters.max_chain_skip = max_chain_skip;
        parameters.max_chain_iter = max_chain_iter;
        parameters.min_seed_count = min_seed_count;
        parameters.min_chain_score = min_chain_score;
        parameters.chain_gap_scale = chain_gap_scale;
        parameters.chain_skip_scale = chain_skip_scale;
        parameters.simd_policy = simd_chaining_policy;
        switch (chaining_algorithm) {
            case ::bioseq::simd::chaining_algorithm::simd_minimap2:
                parameters.chaining_mode =
                    ::bioseq::chaining::chaining_mode::simd_dp;
                break;
            case ::bioseq::simd::chaining_algorithm::range_minimum_query:
                parameters.chaining_mode =
                    ::bioseq::chaining::chaining_mode::rmq;
                break;
            case ::bioseq::simd::chaining_algorithm::
                    vulkan_tiled_dynamic_programming:
                parameters.chaining_mode =
                    ::bioseq::chaining::chaining_mode::scalar_dp;
                break;
            case ::bioseq::simd::chaining_algorithm::scalar_minimap2:
            default:
                parameters.chaining_mode =
                    ::bioseq::chaining::chaining_mode::scalar_dp;
                break;
        }
        parameters.vulkan_enabled = vulkan_enabled;
        parameters.coordinate_only =
            !do_cigar && !compute_divergence && !compute_mapq;
        parameters.prepare();
        return parameters;
    }
};

} // namespace bioseq::simd
