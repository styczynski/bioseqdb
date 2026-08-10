/**
 * @file
 * @brief Target-neutral scheduler and alignment contracts for Highway dispatch.
 */
module;

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <vector>

export module bioseq.highway.dispatch_contract:pipeline;

export import bioseq.pipeline.runtime_contract;
export import bioseq.alignment.contracts;
export import bioseq.sequences.cigar;
export import bioseq.chaining.core;
export import bioseq.storage.lazy_sequence;
export import bioseq.storage.size;
import bioseq.sequences;

export namespace bioseq::simd_dispatch {

struct highway_kernel_table;

} // namespace bioseq::simd_dispatch

export namespace bioseq::simd {

using bioseq::chaining::chaining_mode;
using bioseq::chaining::chaining_parameters;
using bioseq::chaining::simd_chaining_policy;
using bioseq::storage::sequence_size_t;

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
    [[nodiscard]] chaining_parameters make_chaining_parameters() const {
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

/**
 * @brief Mutable chaining region and optional base-alignment result.
 *
 * Sequence coordinates retain the full sequence_size_t domain. Callers clamp
 * potentially negative intermediate coordinates before assigning them here.
 */
struct alignment_region {
    /** Region identifier. */
    std::int32_t region_id = 0;
    /** Parent identifier, or -1 when unset; self denotes a primary region. */
    std::int32_t parent = -1;
    /** Current chain score. */
    std::int32_t score = 0;
    /** Chain score before merging. */
    std::int32_t original_score = 0;
    /** Number of seeds in the chain. */
    std::int32_t seed_count = 0;
    /** Offset of the first region anchor in the anchor array. */
    std::int32_t anchor_start = 0;

    /** Query start coordinate. */
    sequence_size_t query_start = 0;
    /** Query end coordinate. */
    sequence_size_t query_end = 0;
    /** Reference start coordinate. */
    sequence_size_t ref_start = 0;
    /** Reference end coordinate. */
    sequence_size_t ref_end = 0;
    /** Reference sequence identifier. */
    std::int32_t reference_id = 0;
    /** Whether the region maps to the reverse strand. */
    bool is_reverse = false;

    /** Seeded exact-match length. */
    sequence_size_t match_length = 0;
    /** Seeded alignment-block length. */
    sequence_size_t block_length = 0;
    /** Best secondary score competing with this primary region. */
    std::int32_t secondary_score = 0;
    /** Number of retained suboptimal mappings. */
    std::int32_t secondary_count = 0;

    /** Dynamic-programming alignment score. */
    std::int32_t dp_score = 0;
    /** Maximum scoring dynamic-programming segment. */
    std::int32_t dp_max_score = 0;
    /** Best secondary dynamic-programming score. */
    std::int32_t secondary_dp_max = 0;
    /** Whether dynamic-programming alignment was performed. */
    bool has_alignment = false;
    /** Divergence estimate, or a negative value when unavailable. */
    float divergence = -1.0f;

    /** Mapping quality. */
    std::int32_t mapq = 0;

    /** Traceback operations for the aligned region. */
    std::vector<cigar_operation> cigar;
    /** Exact unit-cost edit distance represented by cigar. */
    sequence_size_t edit_distance = 0;
};

/** @brief Saturating counters describing scalar minimizer execution. */
struct scalar_minimizer_execution_stats {
    /** Number of scalar minimizer calls. */
    std::uint64_t calls = 0;
    /** Total input symbols processed by scalar minimizer calls. */
    std::uint64_t input_symbols = 0;
    /** Whether any counter saturated. */
    bool saturated = false;

    /**
     * @brief Add a counter increment with saturation.
     * @param destination Counter to update.
     * @param increment Value to add.
     * @param is_saturated Saturation state updated on overflow.
     */
    static constexpr void add(
            std::uint64_t& destination,
            const std::uint64_t increment,
            bool& is_saturated) noexcept {
        if (increment > (std::numeric_limits<std::uint64_t>::max)()
                            - destination) {
            destination = (std::numeric_limits<std::uint64_t>::max)();
            is_saturated = true;
            return;
        }
        destination += increment;
    }

    /**
     * @brief Record one scalar minimizer call and its input size.
     * @param symbols Number of input symbols processed by the call.
     */
    constexpr void record_input(const std::uint64_t symbols) noexcept {
        add(calls, std::uint64_t{1}, saturated);
        add(input_symbols, symbols, saturated);
    }

    /**
     * @brief Merge another execution sample into this aggregate.
     * @param other Execution sample to merge.
     */
    constexpr void merge(
            const scalar_minimizer_execution_stats& other) noexcept {
        bool is_saturated = saturated || other.saturated;
        add(calls, other.calls, is_saturated);
        add(input_symbols, other.input_symbols, is_saturated);
        saturated = is_saturated;
    }

    /**
     * @brief Test whether the counters describe coherent non-saturated work.
     * @param is_nonempty_work_required Whether at least one input symbol is
     * required.
     * @return `true` when the counter relationships are coherent.
     */
    [[nodiscard]] constexpr bool is_coherent(
            const bool is_nonempty_work_required = true) const noexcept {
        return !saturated
            && calls > 0
            && (!is_nonempty_work_required || input_symbols > 0);
    }

    /** @brief Compare all counters and saturation state. */
    constexpr bool operator==(
        const scalar_minimizer_execution_stats&) const noexcept = default;
};

static_assert(std::is_standard_layout_v<scalar_minimizer_execution_stats>);
static_assert(std::is_trivially_copyable_v<scalar_minimizer_execution_stats>);

/** @brief Saturating counters for batched KSW2 region execution. */
struct ksw2_batched_execution_stats {
    /** Number of alignment-region entry-point invocations. */
    std::uint64_t alignment_region_invocations = 0;
    /** Number of regions observed by all invocations. */
    std::uint64_t regions_seen = 0;
    /** Number of deferred dynamic-programming fills collected. */
    std::uint64_t fills_collected = 0;
    /** Number of non-empty fill calls executed. */
    std::uint64_t nonempty_fill_calls = 0;
    /** Number of short-fill SIMD bucket dispatches. */
    std::uint64_t short_bucket_dispatches = 0;
    /** Number of active lanes across short-fill bucket dispatches. */
    std::uint64_t short_bucket_lanes = 0;
    /** Number of long-fill calls executed outside short buckets. */
    std::uint64_t long_fill_calls = 0;
    /** Sum of query-by-target cell areas across non-empty fills. */
    std::uint64_t input_cell_area = 0;
    /** Whether any counter or area calculation saturated. */
    bool saturated = false;

    /**
     * @brief Record one alignment-region invocation.
     * @param region_count Number of regions presented to the invocation.
     */
    constexpr void record_invocation(
            const std::uint64_t region_count) noexcept {
        scalar_minimizer_execution_stats::add(
            alignment_region_invocations, std::uint64_t{1}, saturated);
        scalar_minimizer_execution_stats::add(
            regions_seen, region_count, saturated);
    }

    /**
     * @brief Record deferred fills collected by one invocation.
     * @param fill_count Number of collected fills.
     */
    constexpr void record_collected_fills(
            const std::uint64_t fill_count) noexcept {
        scalar_minimizer_execution_stats::add(
            fills_collected, fill_count, saturated);
    }

    /**
     * @brief Record one short-fill bucket dispatch.
     * @param lane_count Number of active lanes in the bucket.
     */
    constexpr void record_short_bucket_dispatch(
            const std::uint64_t lane_count) noexcept {
        scalar_minimizer_execution_stats::add(
            short_bucket_dispatches, std::uint64_t{1}, saturated);
        scalar_minimizer_execution_stats::add(
            short_bucket_lanes, lane_count, saturated);
    }

    /**
     * @brief Record one non-empty dynamic-programming fill.
     * @param query_symbols Query length in symbols.
     * @param target_symbols Target length in symbols.
     * @param is_long_fill Whether the fill used the long-fill route.
     */
    constexpr void record_nonempty_fill(
            const std::uint64_t query_symbols,
            const std::uint64_t target_symbols,
            const bool is_long_fill) noexcept {
        scalar_minimizer_execution_stats::add(
            nonempty_fill_calls, std::uint64_t{1}, saturated);
        if (is_long_fill) {
            scalar_minimizer_execution_stats::add(
                long_fill_calls, std::uint64_t{1}, saturated);
        }
        if (query_symbols != 0u
            && target_symbols
                > (std::numeric_limits<std::uint64_t>::max)()
                    / query_symbols) {
            input_cell_area =
                (std::numeric_limits<std::uint64_t>::max)();
            saturated = true;
            return;
        }
        scalar_minimizer_execution_stats::add(
            input_cell_area, query_symbols * target_symbols, saturated);
    }

    /**
     * @brief Merge another execution sample into this aggregate.
     * @param other Execution sample to merge.
     */
    constexpr void merge(
            const ksw2_batched_execution_stats& other) noexcept {
        bool is_saturated = saturated || other.saturated;
        scalar_minimizer_execution_stats::add(
            alignment_region_invocations,
            other.alignment_region_invocations, is_saturated);
        scalar_minimizer_execution_stats::add(
            regions_seen, other.regions_seen, is_saturated);
        scalar_minimizer_execution_stats::add(
            fills_collected, other.fills_collected, is_saturated);
        scalar_minimizer_execution_stats::add(
            nonempty_fill_calls, other.nonempty_fill_calls, is_saturated);
        scalar_minimizer_execution_stats::add(
            short_bucket_dispatches,
            other.short_bucket_dispatches, is_saturated);
        scalar_minimizer_execution_stats::add(
            short_bucket_lanes, other.short_bucket_lanes, is_saturated);
        scalar_minimizer_execution_stats::add(
            long_fill_calls, other.long_fill_calls, is_saturated);
        scalar_minimizer_execution_stats::add(
            input_cell_area, other.input_cell_area, is_saturated);
        saturated = is_saturated;
    }

    /**
     * @brief Test whether all counters form a coherent execution sample.
     * @param is_nonempty_work_required Whether the sample must include a
     * non-empty region and fill.
     * @return `true` when every counter relationship is coherent.
     */
    [[nodiscard]] constexpr bool is_coherent(
            const bool is_nonempty_work_required) const noexcept {
        const bool is_short_route_coherent =
            (short_bucket_dispatches == 0u)
                == (short_bucket_lanes == 0u);
        const bool is_call_accounting_coherent = short_bucket_lanes
                <= (std::numeric_limits<std::uint64_t>::max)()
                    - long_fill_calls
            && nonempty_fill_calls
                == short_bucket_lanes + long_fill_calls
            && short_bucket_dispatches <= short_bucket_lanes
            && nonempty_fill_calls == fills_collected
            && (nonempty_fill_calls == 0u)
                == (input_cell_area == 0u)
            && input_cell_area >= nonempty_fill_calls;
        return !saturated
            && alignment_region_invocations > 0u
            && is_short_route_coherent
            && is_call_accounting_coherent
            && (!is_nonempty_work_required
                || (regions_seen > 0u
                    && fills_collected > 0u
                    && nonempty_fill_calls > 0u));
    }

    /** @brief Compare all counters and saturation state. */
    constexpr bool operator==(
        const ksw2_batched_execution_stats&) const noexcept = default;
};

static_assert(std::is_standard_layout_v<ksw2_batched_execution_stats>);
static_assert(std::is_trivially_copyable_v<ksw2_batched_execution_stats>);

/**
 * @brief Fixed-target alignment functions for one logical alphabet.
 * @tparam p_alphabet Logical sequence alphabet accepted by every callable.
 */
template <sequence_alphabet p_alphabet>
struct alignment_kernels final {
    /** Lazy sequence view accepted by the target-neutral callables. */
    using view_type =
        bioseq::storage::lazy_sequence_view<p_alphabet>;
    /** Scoring matrix normalized for p_alphabet. */
    using matrix_type = scoring_matrix<p_alphabet>;

    /** Single-affine KSW2 callable. */
    using align_ksw2_single_affine_kernel = ksw_alignment_result (*)(
        bool, view_type, view_type,
        std::size_t, std::size_t, std::size_t, std::size_t,
        const matrix_type&, const ksw_single_affine_parameters&);
    /** Dual-affine KSW2 callable. */
    using align_ksw2_dual_affine_kernel = ksw_alignment_result (*)(
        bool, view_type, view_type,
        std::size_t, std::size_t, std::size_t, std::size_t,
        const matrix_type&, ksw_dual_affine_parameters);
    /** Cross-region batched KSW2 callable. */
    using align_ksw2_batched_kernel = void (*)(
        std::span<alignment_region>,
        std::span<const bioseq::chaining::anchor>,
        view_type, view_type, const matrix_type&, const minimap2_parameters&,
        ksw2_batched_execution_stats*);
    /** Block-aligner certificate preparation callable. */
    using prepare_block_global_gap_certificate_kernel =
        block_global_gap_certificate_plan (*)(
        const matrix_type&, const block_aligner_parameters&) noexcept;
    /** Certified block-gap attempt callable. */
    using try_block_certified_gap_kernel = block_packed_certificate_attempt (*)(
        bool, view_type, view_type,
        std::size_t, std::size_t, std::size_t, std::size_t,
        const block_global_gap_certificate_plan&) noexcept;
    /** Full block-alignment callable. */
    using align_block_kernel = ksw_alignment_result (*)(
        bool, bool, view_type, view_type,
        std::size_t, std::size_t, std::size_t, std::size_t,
        const matrix_type&, const block_aligner_parameters&,
        block_aligner_execution_stats*, block_aligner_construction_stats*);

    /** Single-affine KSW2 implementation for p_alphabet. */
    align_ksw2_single_affine_kernel align_ksw2_single_affine = nullptr;
    /** Dual-affine KSW2 implementation for p_alphabet. */
    align_ksw2_dual_affine_kernel align_ksw2_dual_affine = nullptr;
    /** Batched KSW2 implementation for p_alphabet. */
    align_ksw2_batched_kernel align_ksw2_batched = nullptr;
    /** Block-aligner certificate preparation for p_alphabet. */
    prepare_block_global_gap_certificate_kernel
        prepare_block_global_gap_certificate = nullptr;
    /** Certified block-gap implementation for p_alphabet. */
    try_block_certified_gap_kernel try_block_certified_gap = nullptr;
    /** Full block-alignment implementation for p_alphabet. */
    align_block_kernel align_block = nullptr;
};

/**
 * @brief Whether an alphabet table can cross the neutral dispatch boundary.
 * @tparam p_alphabet Logical sequence alphabet represented by the table.
 */
template <sequence_alphabet p_alphabet>
inline constexpr bool is_alignment_kernel_table_neutral =
    std::is_standard_layout_v<alignment_kernels<p_alphabet>>
    && std::is_trivially_copyable_v<alignment_kernels<p_alphabet>>;

/**
 * @brief Test whether every callable in a typed alignment table is bound.
 * @tparam p_alphabet Logical sequence alphabet represented by the table.
 * @param kernels Typed fixed-target alignment table to validate.
 * @return `true` when every callable is non-null.
 */
template <sequence_alphabet p_alphabet>
[[nodiscard]] constexpr bool is_alignment_kernel_table_complete(
    const alignment_kernels<p_alphabet>& kernels) noexcept
{
    return kernels.align_ksw2_single_affine != nullptr
        && kernels.align_ksw2_dual_affine != nullptr
        && kernels.align_ksw2_batched != nullptr
        && kernels.prepare_block_global_gap_certificate != nullptr
        && kernels.try_block_certified_gap != nullptr
        && kernels.align_block != nullptr;
}

/** @brief Type-erased entry for one alphabet-specific alignment table. */
struct alignment_table_entry final {
    /** Scheduler-owned packed alphabet code. */
    std::uint8_t alphabet_code = 0;
    /** Completeness result computed while the table retains its static type. */
    bool typed_table_complete = false;
    /** Pointer to alignment_kernels for alphabet_code. */
    const void* typed_table = nullptr;
};

static_assert(std::is_standard_layout_v<alignment_table_entry>);
static_assert(std::is_trivially_copyable_v<alignment_table_entry>);

} // namespace bioseq::simd
