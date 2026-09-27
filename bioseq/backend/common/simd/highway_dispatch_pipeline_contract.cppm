/**
 * @file
 * @brief Target-neutral scheduler and alignment contracts for Highway dispatch.
 */
module;

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

export module bioseq.highway.dispatch_contract:pipeline;

export import bioseq.pipeline.minimap_parameters_contract;
export import bioseq.pipeline.runtime_contract;
export import bioseq.alignment.contracts;
export import bioseq.sequences.cigar;
export import bioseq.chaining.core;
export import bioseq.storage.lazy_sequence;
export import bioseq.storage.size;
import bioseq.sequences;

export namespace bioseq::simd {

using bioseq::chaining::chaining_mode;
using bioseq::chaining::chaining_parameters;
using bioseq::chaining::simd_chaining_policy;
using bioseq::storage::sequence_size_t;

/**
 * @brief Mutable chaining region and optional base-alignment result.
 *
 * Sequence coordinates retain the full sequence_size_t domain. Callers clamp
 * potentially negative intermediate coordinates before assigning them here.
 */
struct alignment_region {
    /** Region identifier. */
    std::int32_t region_id = 0;
    /** Parent identifier, or -1 when unset. Self denotes a primary region. */
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
    alignment_result_vector<cigar_operation> cigar;
    /** Exact unit-cost edit distance represented by cigar. */
    sequence_size_t edit_distance = 0;
};

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
static_assert(!std::is_copy_constructible_v<alignment_region>);
static_assert(std::is_nothrow_move_constructible_v<alignment_region>);
#endif

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
/** Source-derived logical element bounds for the region-to-result tail. */
struct alignment_tail_storage_plan {
    std::size_t region_count = 0u;
    std::size_t primary_index_capacity = 0u;
    std::size_t coverage_interval_capacity = 0u;
    std::size_t filter_marker_count = 0u;
    std::size_t mapq_region_count = 0u;
    std::size_t result_count = 0u;
    std::size_t block_stats_count = 0u;
    std::size_t scalar_stats_count = 0u;
};

/**
 * @brief Bound the unchanged one-region-per-chain tail without allocating.
 *
 * Region identifiers and parents are signed 32-bit values in the existing
 * algorithms, so a larger count is not representable even when size_t is.
 */
[[nodiscard]] constexpr std::optional<alignment_tail_storage_plan>
derive_alignment_tail_storage_plan(
    const std::size_t region_count,
    const bool collect_block_stats = false,
    const bool collect_scalar_stats = false) noexcept {
    constexpr std::size_t maximum_region_count =
        static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)());
    if (region_count > maximum_region_count) {
        return std::nullopt;
    }
    return alignment_tail_storage_plan{
        .region_count = region_count,
        .primary_index_capacity = region_count,
        .coverage_interval_capacity = region_count,
        .filter_marker_count = region_count,
        .mapq_region_count = region_count,
        .result_count = region_count,
        .block_stats_count = collect_block_stats ? region_count : 0u,
        .scalar_stats_count = collect_scalar_stats ? region_count : 0u,
    };
}
#endif

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
    static constexpr auto add(std::uint64_t& destination,
                              const std::uint64_t increment,
                              bool& is_saturated) noexcept -> void {
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
    constexpr auto record_input(const std::uint64_t symbols) noexcept -> void {
        add(calls, std::uint64_t{1}, saturated);
        add(input_symbols, symbols, saturated);
    }

    /**
     * @brief Merge another execution sample into this aggregate.
     * @param other Execution sample to merge.
     */
    constexpr auto merge(const scalar_minimizer_execution_stats& other) noexcept -> void {
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
    [[nodiscard]] constexpr auto
    is_coherent(const bool is_nonempty_work_required = true) const noexcept -> bool {
        return !saturated
            && calls > 0
            && (!is_nonempty_work_required || input_symbols > 0);
    }

    /** @brief Compare all counters and saturation state. */
    constexpr auto operator==(const scalar_minimizer_execution_stats&) const noexcept
        -> bool = default;
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
    constexpr auto record_invocation(const std::uint64_t region_count) noexcept -> void {
        scalar_minimizer_execution_stats::add(
            alignment_region_invocations, std::uint64_t{1}, saturated);
        scalar_minimizer_execution_stats::add(
            regions_seen, region_count, saturated);
    }

    /**
     * @brief Record deferred fills collected by one invocation.
     * @param fill_count Number of collected fills.
     */
    constexpr auto record_collected_fills(const std::uint64_t fill_count) noexcept -> void {
        scalar_minimizer_execution_stats::add(
            fills_collected, fill_count, saturated);
    }

    /**
     * @brief Record one short-fill bucket dispatch.
     * @param lane_count Number of active lanes in the bucket.
     */
    constexpr auto record_short_bucket_dispatch(const std::uint64_t lane_count) noexcept -> void {
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
    constexpr auto record_nonempty_fill(const std::uint64_t query_symbols,
                                        const std::uint64_t target_symbols,
                                        const bool is_long_fill) noexcept -> void {
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
    constexpr auto merge(const ksw2_batched_execution_stats& other) noexcept -> void {
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
    [[nodiscard]] constexpr auto is_coherent(const bool is_nonempty_work_required) const noexcept
        -> bool {
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
    constexpr auto operator==(const ksw2_batched_execution_stats&) const noexcept -> bool = default;
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
// Imported templates can otherwise produce one weak symbol shared by
// different ISA callers. Keep this predicate in the caller's target context.
[[nodiscard, gnu::always_inline]] inline constexpr auto
is_alignment_kernel_table_complete(const alignment_kernels<p_alphabet>& kernels) noexcept -> bool {
    return kernels.align_ksw2_single_affine != nullptr
        && kernels.align_ksw2_dual_affine != nullptr
        && kernels.align_ksw2_batched != nullptr
        && kernels.prepare_block_global_gap_certificate != nullptr
        && kernels.try_block_certified_gap != nullptr
        && kernels.align_block != nullptr;
}

} // namespace bioseq::simd
