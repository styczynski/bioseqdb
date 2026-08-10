/**
 * @file alignment_contracts.cppm
 * @brief Highway-free alignment models shared by generated targets and schedulers.
 */
module;

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <span>
#include <vector>

export module bioseq.alignment.contracts;

export import bioseq.alignment.utils;

export namespace bioseq::simd {

namespace detail_ksw2_batched_ordering {

/** Geometry and orientation key used to group batched KSW2 fills. */
struct fill_bucket_key {
    std::int32_t maximum_query_length;  ///< Maximum admitted query extent.
    std::int32_t maximum_target_length;  ///< Maximum admitted target extent.
    std::int32_t maximum_bandwidth;  ///< Maximum admitted bandwidth.
    bool is_long_fill;  ///< Whether the fill bypasses short SIMD buckets.
    bool is_reversed;  ///< Whether query ranks use reverse orientation.

    [[nodiscard]] friend constexpr bool operator==(
        const fill_bucket_key&, const fill_bucket_key&) noexcept = default;
};

/** Short-fill length boundaries used for stable SIMD bucketing. */
inline constexpr std::array<std::int32_t, 4> k_fill_length_brackets = {
    32, 64, 128, 256};
/** Short-fill bandwidth boundaries used for stable SIMD bucketing. */
inline constexpr std::array<std::int32_t, 3> k_fill_band_brackets = {
    16, 32, 64};

/** Round a positive extent up to the first configured bucket boundary. */
[[nodiscard, gnu::always_inline]] inline std::int32_t round_up_to_bracket(
    std::int32_t value,
    std::span<const std::int32_t> brackets) noexcept {
    for (auto upper_bound : brackets) {
        if (value <= upper_bound) return upper_bound;
    }
    return value;
}

/** Build the stable bucketing key for one fill descriptor. */
template <class p_fill_descriptor>
[[nodiscard, gnu::always_inline]] inline fill_bucket_key bucket_key_for(
    const p_fill_descriptor& fill,
    std::int32_t default_bandwidth) noexcept {
    const bool is_long = (fill.query_length > 256) || (fill.target_length > 256);
    fill_bucket_key key{};
    if (is_long) {
        key.maximum_query_length = fill.query_length;
        key.maximum_target_length = fill.target_length;
        key.maximum_bandwidth = fill.bandwidth > 0
            ? fill.bandwidth
            : default_bandwidth;
        key.is_long_fill = true;
    } else {
        key.maximum_query_length = round_up_to_bracket(fill.query_length,
            std::span<const std::int32_t>{k_fill_length_brackets});
        key.maximum_target_length = round_up_to_bracket(fill.target_length,
            std::span<const std::int32_t>{k_fill_length_brackets});
        key.maximum_bandwidth = round_up_to_bracket(
            fill.bandwidth > 0 ? fill.bandwidth : default_bandwidth,
            std::span<const std::int32_t>{k_fill_band_brackets});
        key.is_long_fill = false;
    }
    key.is_reversed = fill.is_reversed;
    return key;
}

/** Compare fills by bucket geometry and then by stable region order. */
struct by_bucket_then_order {
    std::int32_t default_bandwidth;  ///< Bandwidth used when a fill omits one.

    template <class p_fill_descriptor>
    [[nodiscard, gnu::always_inline]] inline bool operator()(
        const p_fill_descriptor& left,
        const p_fill_descriptor& right) const noexcept
    {
        const fill_bucket_key left_key = bucket_key_for(left, default_bandwidth);
        const fill_bucket_key right_key = bucket_key_for(right, default_bandwidth);
        if (left_key.is_long_fill != right_key.is_long_fill) {
            return !left_key.is_long_fill;
        }
        if (left_key.is_reversed != right_key.is_reversed) {
            return !left_key.is_reversed;
        }
        if (left_key.maximum_query_length
            != right_key.maximum_query_length) {
            return left_key.maximum_query_length
                < right_key.maximum_query_length;
        }
        if (left_key.maximum_target_length
            != right_key.maximum_target_length) {
            return left_key.maximum_target_length
                < right_key.maximum_target_length;
        }
        if (left_key.maximum_bandwidth != right_key.maximum_bandwidth) {
            return left_key.maximum_bandwidth
                < right_key.maximum_bandwidth;
        }
        if (left.region_index != right.region_index) {
            return left.region_index < right.region_index;
        }
        return left.fill_order < right.fill_order;
    }
};

/** Compare fills by their original region and fill order. */
struct chain_order {
    template <class p_fill_descriptor>
    [[nodiscard, gnu::always_inline]] inline bool operator()(
        const p_fill_descriptor& left,
        const p_fill_descriptor& right) const noexcept
    {
        if (left.region_index != right.region_index) {
            return left.region_index < right.region_index;
        }
        return left.fill_order < right.fill_order;
    }
};

}  // namespace detail_ksw2_batched_ordering

/** Behavioral flags shared by the single- and dual-affine KSW2 kernels. */
struct ksw_flags {
    bool score_only    : 1 = false;  ///< Suppress traceback generation.
    bool right_align   : 1 = false;  ///< Prefer rightmost equivalent traceback.
    bool generic_score : 1 = false;  ///< Materialize generic matrix scores.
    bool approx_max    : 1 = false;  ///< Use the bounded approximate maximum.
    bool approx_zdrop  : 1 = false;  ///< Use the approximate Z-drop maximum.
    bool extz_only     : 1 = false;  ///< Return extension-only coordinates.
    bool reverse_cigar : 1 = false;  ///< Emit traceback in reverse orientation.
    bool reserved      : 1 = false;  ///< Reserved layout bit; must remain false.
};
static_assert(sizeof(ksw_flags) == 1);

/** Parameters for the single-affine KSW2 recurrence. */
struct ksw_single_affine_parameters {
    int8_t    gap_open   = 0;   ///< Positive gap-open penalty.
    int8_t    gap_extend = 0;   ///< Positive gap-extension penalty.
    int       band_width = -1;  ///< Band width, or negative for unrestricted.
    int       z_drop     = -1;  ///< Z-drop threshold, or negative to disable.
    int       end_bonus  = 0;   ///< Bonus for reaching an admitted end.
    ksw_flags flags      = {};  ///< Behavioral recurrence flags.
};

/** Parameters for the dual-affine KSW2 recurrence. */
struct ksw_dual_affine_parameters {
    int8_t gap_open_short   = 0;   ///< Positive short-gap open penalty.
    int8_t gap_extend_short = 0;   ///< Positive short-gap extension penalty.
    int8_t gap_open_long    = 0;   ///< Positive long-gap open penalty.
    int8_t gap_extend_long  = 0;   ///< Positive long-gap extension penalty.
    int    band_width       = -1;  ///< Band width, or negative for unrestricted.
    int    z_drop           = -1;  ///< Z-drop threshold, or negative to disable.
    int    end_bonus        = 0;   ///< Bonus for reaching an admitted end.
    ksw_flags flags         = {};  ///< Behavioral recurrence flags.
};

/** Initial score sentinel shared by all alignment results. */
constexpr int32_t k_ksw_negative_infinity = -0x40000000;

/** Shared alignment result returned by KSW2 and Block Aligner callables. */
struct ksw_alignment_result {
    int32_t max       = 0;  ///< Maximum score observed during extension.
    int32_t score     = k_ksw_negative_infinity;  ///< Final alignment score.
    int32_t mqe       = k_ksw_negative_infinity;  ///< Best query-end score.
    int32_t mte       = k_ksw_negative_infinity;  ///< Best target-end score.
    int32_t max_q     = -1;  ///< Query coordinate of the maximum score.
    int32_t max_t     = -1;  ///< Target coordinate of the maximum score.
    int32_t mqe_t     = -1;  ///< Target coordinate for the query-end maximum.
    int32_t mte_q     = -1;  ///< Query coordinate for the target-end maximum.
    bool is_z_dropped = false;  ///< Whether Z-drop terminated the recurrence.
    bool has_reached_end = false;  ///< Whether the selected path reached an end.
    std::uint64_t edit_distance = 0;  ///< Exact unit-cost edit distance.
    bool is_edit_distance_valid = false;  ///< Whether edit_distance is populated.
    std::vector<uint32_t> cigar;  ///< Packed KSW2 CIGAR operations.
};
/** Semantic pipeline stage requesting a Block Aligner invocation. */
enum class block_alignment_stage : std::uint8_t {
    left_extension,
    gap_extension,
    right_extension,
};

/// Modes implemented by the upstream adaptive-block reference. This slice
/// admits only the modes needed by production: global gap fill and native
/// two-confirmation X-drop extension. KSW2 z-drop is deliberately absent.
enum class block_alignment_mode : std::uint8_t {
    global,
    x_drop,
};

/** Input orientation consumed by Block Aligner. */
enum class block_input_order : std::uint8_t {
    forward,
    reversed,
};

/** Saturating execution counters for Block Aligner routing and SIMD work. */
struct block_aligner_execution_stats {
    std::uint64_t align_region_invocations = 0;  ///< Region entry-point calls.
    std::uint64_t serial_align_region_dispatches = 0;  ///< Serial region calls.
    std::uint64_t parallel_align_region_dispatches = 0;  ///< Parallel region calls.
    std::uint64_t routed_region_scopes = 0;  ///< Region scopes routed to alignment.
    std::uint64_t left_route_candidates = 0;  ///< Routed left extensions.
    std::uint64_t gap_route_candidates = 0;  ///< Routed inter-seed gaps.
    std::uint64_t right_route_candidates = 0;  ///< Routed right extensions.
    std::uint64_t left_eligible = 0;  ///< Admitted left extensions.
    std::uint64_t left_executed = 0;  ///< Executed left extensions.
    std::uint64_t gap_eligible = 0;  ///< Admitted inter-seed gaps.
    std::uint64_t gap_executed = 0;  ///< Executed inter-seed gaps.
    std::uint64_t right_eligible = 0;  ///< Admitted right extensions.
    std::uint64_t right_executed = 0;  ///< Executed right extensions.
    std::uint64_t global_calls = 0;  ///< Requested global alignments.
    std::uint64_t native_x_drop_calls = 0;  ///< Requested native X-drop alignments.
    std::uint64_t native_x_drop_terminations = 0;  ///< Native X-drop terminations.
    std::uint64_t zero_work_calls = 0;  ///< Calls with an empty sequence extent.
    std::uint64_t nonempty_calls = 0;  ///< Calls with biological work.
    std::uint64_t nonempty_calls_with_simd = 0;  ///< Nonempty calls using SIMD.
    std::uint64_t trace_calls = 0;  ///< Requests with traceback enabled.
    std::uint64_t executed_trace_calls = 0;  ///< Executed traceback requests.
    std::uint64_t forward_input_calls = 0;  ///< Requests using forward inputs.
    std::uint64_t reversed_input_calls = 0;  ///< Requests using reversed inputs.
    std::uint64_t executed_global_calls = 0;  ///< Executed global alignments.
    std::uint64_t executed_native_x_drop_calls = 0;  ///< Executed X-drop alignments.
    std::uint64_t executed_forward_input_calls = 0;  ///< Executed forward inputs.
    std::uint64_t executed_reversed_input_calls = 0;  ///< Executed reversed inputs.
    std::uint64_t simd_eight_lane_calls = 0;  ///< Calls using eight score lanes.
    std::uint64_t simd_sixteen_lane_calls = 0;  ///< Calls using sixteen score lanes.
    std::uint64_t simd_dp_vector_batches = 0;  ///< DP vector batches evaluated.
    std::uint64_t simd_dp_cells = 0;  ///< DP cells evaluated.
    // Global equal-length calls may be discharged by a SIMD scan whose
    // strict score bound proves that the coordinate diagonal is the unique
    // optimum.  Keep that work separate from DP counters: simd_dp_cells is
    // always the number of cells actually evaluated by align_block_core.
    std::uint64_t simd_diagonal_certificate_calls = 0;  ///< Certified diagonal calls.
    std::uint64_t simd_diagonal_certificate_vector_batches = 0;  ///< Scan batches.
    std::uint64_t simd_diagonal_certificate_symbols = 0;  ///< Symbols scanned.
    std::uint64_t adaptive_grow_events = 0;  ///< Adaptive block growth events.
    std::uint64_t adaptive_shrink_events = 0;  ///< Adaptive block shrink events.
    std::uint64_t expected_reversed_compact_symbols = 0;  ///< Expected reversals.
    std::uint64_t reversed_compact_symbols = 0;  ///< Performed compact reversals.
    std::uint64_t configuration_identity_calls = 0;  ///< Requested identities.
    std::uint64_t configuration_identity_xor = 0;  ///< Requested identity XOR.
    std::uint64_t configuration_identity_sum = 0;  ///< Requested identity sum.
    std::uint64_t executed_configuration_identity_calls = 0;  ///< Executed identities.
    std::uint64_t executed_configuration_identity_xor = 0;  ///< Executed XOR.
    std::uint64_t executed_configuration_identity_sum = 0;  ///< Executed sum.
    std::uint64_t unsupported_contract_rejections = 0;  ///< Rejected contracts.
    std::uint64_t forbidden_ksw2_calls = 0;  ///< Forbidden KSW2 fallbacks.
    std::uint64_t forbidden_scalar_biological_fallbacks = 0;  ///< Scalar fallbacks.
    bool is_saturated = false;  ///< Whether any counter saturated.

    /** Return whether an unsigned counter increment would overflow. */
    [[nodiscard]] static constexpr bool add_would_overflow(
            std::uint64_t destination,
            std::uint64_t increment) noexcept {
        return increment > (std::numeric_limits<std::uint64_t>::max)()
            - destination;
    }

    /** Add to a counter, saturating and recording overflow. */
    static void add(
            std::uint64_t& destination,
            std::uint64_t increment,
            bool& saturation) noexcept {
        if (add_would_overflow(destination, increment)) {
            destination = (std::numeric_limits<std::uint64_t>::max)();
            saturation = true;
            return;
        }
        destination += increment;
    }

    /** Add to a member counter using this record's saturation flag. */
    void add(std::uint64_t& destination, std::uint64_t increment = 1) noexcept {
        add(destination, increment, is_saturated);
    }

    /** Merge another independent execution record into this one. */
    void merge(const block_aligner_execution_stats& other) noexcept {
        is_saturated = is_saturated || other.is_saturated;
        add(align_region_invocations, other.align_region_invocations);
        add(serial_align_region_dispatches,
            other.serial_align_region_dispatches);
        add(parallel_align_region_dispatches,
            other.parallel_align_region_dispatches);
        add(routed_region_scopes, other.routed_region_scopes);
        add(left_route_candidates, other.left_route_candidates);
        add(gap_route_candidates, other.gap_route_candidates);
        add(right_route_candidates, other.right_route_candidates);
        add(left_eligible, other.left_eligible);
        add(left_executed, other.left_executed);
        add(gap_eligible, other.gap_eligible);
        add(gap_executed, other.gap_executed);
        add(right_eligible, other.right_eligible);
        add(right_executed, other.right_executed);
        add(global_calls, other.global_calls);
        add(native_x_drop_calls, other.native_x_drop_calls);
        add(native_x_drop_terminations, other.native_x_drop_terminations);
        add(zero_work_calls, other.zero_work_calls);
        add(nonempty_calls, other.nonempty_calls);
        add(nonempty_calls_with_simd, other.nonempty_calls_with_simd);
        add(trace_calls, other.trace_calls);
        add(executed_trace_calls, other.executed_trace_calls);
        add(forward_input_calls, other.forward_input_calls);
        add(reversed_input_calls, other.reversed_input_calls);
        add(executed_global_calls, other.executed_global_calls);
        add(executed_native_x_drop_calls, other.executed_native_x_drop_calls);
        add(executed_forward_input_calls, other.executed_forward_input_calls);
        add(executed_reversed_input_calls, other.executed_reversed_input_calls);
        add(simd_eight_lane_calls, other.simd_eight_lane_calls);
        add(simd_sixteen_lane_calls, other.simd_sixteen_lane_calls);
        add(simd_dp_vector_batches, other.simd_dp_vector_batches);
        add(simd_dp_cells, other.simd_dp_cells);
        add(simd_diagonal_certificate_calls,
            other.simd_diagonal_certificate_calls);
        add(simd_diagonal_certificate_vector_batches,
            other.simd_diagonal_certificate_vector_batches);
        add(simd_diagonal_certificate_symbols,
            other.simd_diagonal_certificate_symbols);
        add(adaptive_grow_events, other.adaptive_grow_events);
        add(adaptive_shrink_events, other.adaptive_shrink_events);
        add(expected_reversed_compact_symbols,
            other.expected_reversed_compact_symbols);
        add(reversed_compact_symbols, other.reversed_compact_symbols);
        add(configuration_identity_calls, other.configuration_identity_calls);
        configuration_identity_xor ^= other.configuration_identity_xor;
        // Identity sums are deliberately modulo 2^64 (unsigned wrapping),
        // paired with XOR; unlike counters, wrap is part of the checksum.
        configuration_identity_sum += other.configuration_identity_sum;
        add(executed_configuration_identity_calls,
            other.executed_configuration_identity_calls);
        executed_configuration_identity_xor ^= other.executed_configuration_identity_xor;
        executed_configuration_identity_sum += other.executed_configuration_identity_sum;
        add(unsupported_contract_rejections,
            other.unsupported_contract_rejections);
        add(forbidden_ksw2_calls, other.forbidden_ksw2_calls);
        add(forbidden_scalar_biological_fallbacks,
            other.forbidden_scalar_biological_fallbacks);
    }

    /** Sum three counters when the exact result is representable. */
    [[nodiscard]] static constexpr bool sum_three_counters(
            std::uint64_t first,
            std::uint64_t second,
            std::uint64_t third,
            std::uint64_t& result) noexcept {
        if (add_would_overflow(first, second)) return false;
        const std::uint64_t partial = first + second;
        if (add_would_overflow(partial, third)) return false;
        result = partial + third;
        return true;
    }

    /** Return the number of eligible calls, or UINT64_MAX on overflow. */
    [[nodiscard]] constexpr std::uint64_t eligible_calls() const noexcept {
        std::uint64_t result = (std::numeric_limits<std::uint64_t>::max)();
        (void)sum_three_counters(left_eligible, gap_eligible, right_eligible, result);
        return result;
    }

    /** Return the number of executed calls, or UINT64_MAX on overflow. */
    [[nodiscard]] constexpr std::uint64_t executed_calls() const noexcept {
        std::uint64_t result = (std::numeric_limits<std::uint64_t>::max)();
        (void)sum_three_counters(left_executed, gap_executed, right_executed, result);
        return result;
    }

    /** Validate routing, mode, work, and forbidden-fallback invariants. */
    [[nodiscard]] constexpr bool is_internally_consistent() const noexcept {
        std::uint64_t eligible_total = 0;
        std::uint64_t executed_total = 0;
        std::uint64_t requested_xdrop_total = 0;
        std::uint64_t executed_xdrop_total = 0;
        std::uint64_t requested_forward_total = 0;
        std::uint64_t executed_forward_total = 0;
        std::uint64_t work_total = 0;
        std::uint64_t lane_total = 0;
        const bool totals_valid =
            sum_three_counters(left_eligible, gap_eligible, right_eligible, eligible_total)
            && sum_three_counters(left_executed, gap_executed, right_executed, executed_total)
            && !add_would_overflow(left_eligible, right_eligible)
            && !add_would_overflow(left_executed, right_executed)
            && !add_would_overflow(gap_eligible, right_eligible)
            && !add_would_overflow(gap_executed, right_executed)
            && !add_would_overflow(zero_work_calls, nonempty_calls)
            && !add_would_overflow(simd_eight_lane_calls, simd_sixteen_lane_calls)
            && !add_would_overflow(serial_align_region_dispatches,
                                   parallel_align_region_dispatches);
        if (!totals_valid || is_saturated) return false;
        requested_xdrop_total = left_eligible + right_eligible;
        executed_xdrop_total = left_executed + right_executed;
        requested_forward_total = gap_eligible + right_eligible;
        executed_forward_total = gap_executed + right_executed;
        work_total = zero_work_calls + nonempty_calls;
        lane_total = simd_eight_lane_calls + simd_sixteen_lane_calls;
        if (simd_diagonal_certificate_calls > nonempty_calls) return false;
        const std::uint64_t dp_nonempty_calls = nonempty_calls
            - simd_diagonal_certificate_calls;
        const bool exact_align_region_dispatch =
            serial_align_region_dispatches
                    + parallel_align_region_dispatches
                == align_region_invocations
            && (routed_region_scopes == 0 || align_region_invocations > 0);
        const bool exact_stage_coverage = left_eligible == left_executed
            && gap_eligible == gap_executed
            && right_eligible == right_executed;
        const bool exact_router_coverage = routed_region_scopes == 0
            || (left_route_candidates == left_executed
                && gap_route_candidates == gap_executed
                && right_route_candidates == right_executed);
        const bool exact_mode_coverage = global_calls == gap_eligible
            && native_x_drop_calls == requested_xdrop_total
            && executed_global_calls == gap_executed
            && executed_native_x_drop_calls == executed_xdrop_total;
        const bool exact_input_order_coverage =
            reversed_input_calls == left_eligible
            && forward_input_calls == requested_forward_total
            && executed_reversed_input_calls == left_executed
            && executed_forward_input_calls == executed_forward_total;
        const bool exact_config_coverage =
            configuration_identity_calls == eligible_total
            && executed_configuration_identity_calls == executed_total
            && configuration_identity_calls == executed_configuration_identity_calls
            && configuration_identity_xor == executed_configuration_identity_xor
            && configuration_identity_sum == executed_configuration_identity_sum;
        const bool truthful_dp_work =
            (dp_nonempty_calls == 0
                && simd_dp_vector_batches == 0 && simd_dp_cells == 0)
            || (dp_nonempty_calls > 0
                && simd_dp_vector_batches > 0 && simd_dp_cells > 0);
        const bool truthful_certificate_work =
            (simd_diagonal_certificate_calls == 0
                && simd_diagonal_certificate_vector_batches == 0
                && simd_diagonal_certificate_symbols == 0)
            || (simd_diagonal_certificate_calls > 0
                && simd_diagonal_certificate_vector_batches
                    >= simd_diagonal_certificate_calls
                && simd_diagonal_certificate_symbols
                    >= simd_diagonal_certificate_calls
                && simd_diagonal_certificate_vector_batches
                    <= simd_diagonal_certificate_symbols
                && simd_diagonal_certificate_calls
                    <= executed_global_calls);
        const bool truthful_work = work_total == executed_total
            && nonempty_calls_with_simd == nonempty_calls
            && lane_total == executed_total
            && truthful_dp_work
            && truthful_certificate_work;
        return exact_stage_coverage
            && exact_align_region_dispatch
            && exact_router_coverage
            && exact_mode_coverage
            && exact_input_order_coverage
            && exact_config_coverage
            && truthful_work
            && trace_calls == executed_trace_calls
            && expected_reversed_compact_symbols == reversed_compact_symbols
            && native_x_drop_terminations <= executed_native_x_drop_calls
            && unsupported_contract_rejections == 0
            && forbidden_ksw2_calls == 0
            && forbidden_scalar_biological_fallbacks == 0;
    }

    /** Compare every counter and saturation state. */
    [[nodiscard]] constexpr bool operator==(
        const block_aligner_execution_stats&) const noexcept = default;
};
/** Result of the exact equal-length diagonal certification fast path. */
struct block_diagonal_certificate {
    bool is_certified = false;  ///< Whether the diagonal is the unique optimum.
    int32_t score = 0;  ///< Exact diagonal score.
    std::uint64_t edit_distance = 0;  ///< Exact unit-cost edit distance.
    std::uint64_t vector_batches = 0;  ///< SIMD scan batches used.
    std::uint64_t symbols = 0;  ///< Symbols examined by the scan.
};

/** Arena-backed allocation counters for one Block Aligner invocation. */
struct block_aligner_construction_stats {
    std::uint64_t align_arena_scopes = 0;  ///< Alignment arena scopes entered.
    std::uint64_t padded_sequence_allocations = 0;  ///< Padded inputs allocated.
    std::uint64_t scratch_allocations = 0;  ///< DP scratch allocations.
    std::uint64_t trace_allocations = 0;  ///< Traceback allocations.
};
/** Validated parameters controlling one Block Aligner invocation. */
struct block_aligner_parameters {
    int8_t gap_open = 0;  ///< Positive q; reference open is -(q + e).
    int8_t gap_extend = 0;  ///< Positive e; reference extend is -e.
    size_t minimum_block_size = 32;  ///< Minimum adaptive block size.
    size_t maximum_block_size = 256;  ///< Maximum adaptive block size.
    int32_t x_drop_threshold = 0;  ///< Native Block Aligner X-drop threshold.
    block_alignment_mode mode = block_alignment_mode::global;  ///< Recurrence mode.
    block_input_order input_order = block_input_order::forward;  ///< Input order.
    block_alignment_stage stage = block_alignment_stage::gap_extension;  ///< Stage.
    bool is_trace_enabled = true;  ///< Whether traceback is requested.
    bool should_compute_edit_distance = false;  ///< Whether exact edits are required.
};

/** Outcome of attempting the packed equal-length certification path. */
enum class block_packed_certificate_decision : std::uint8_t {
    not_eligible,
    rejected,
    certified,
};

/// Immutable per-region state for the resident DNA4/u64 gap certificate.
/// Matrix/contract checks are deliberately hoisted out of the seed-pair loop;
/// exact coordinates and packed sequence contents remain per-gap inputs.
struct block_global_gap_certificate_plan {
    block_aligner_parameters parameters{};  ///< Validated global parameters.
    std::int8_t match_score = 0;  ///< Projected DNA4 match score.
    std::int8_t mismatch_score = 0;  ///< Projected DNA4 mismatch score.
    std::uint64_t scoring_identity = 0;  ///< Semantic scoring identity.
    /// Target-local int16 lane count captured by the fixed-target preparer.
    std::uint8_t simd_lane_count = 0;
    bool is_contract_supported = false;  ///< Whether the parameter contract is valid.
    bool is_generated_matrix_supported = false;  ///< Whether scoring is certifiable.
};

/** Decision and optional diagonal result from packed certification. */
struct block_packed_certificate_attempt {
    /** Certification route outcome. */
    block_packed_certificate_decision decision =
        block_packed_certificate_decision::not_eligible;
    block_diagonal_certificate certificate{};  ///< Exact result when certified.
};

/** Contract rejection reason for unsupported Block Aligner coordinates. */
enum class block_aligner_contract_error : std::uint8_t {
    none,
    gap_penalty,
    block_geometry,
    x_drop_threshold,
    stage_mode,
    lane_shape,
    coordinates,
    scoring_matrix,
    trace_accounting,
};

/** Return whether the active lane shape has a validated Block Aligner path. */
[[nodiscard]] constexpr bool is_block_aligner_lane_shape_supported(
        size_t lanes) noexcept {
    return lanes == 8 || lanes == 16;
}

/** Validate invariant-bearing parameters and the active SIMD lane shape. */
[[nodiscard]] constexpr block_aligner_contract_error validate_block_aligner_contract(
        const block_aligner_parameters& parameters,
        size_t lanes) noexcept {
    if (parameters.gap_open <= 0 || parameters.gap_extend <= 0
        || static_cast<int>(parameters.gap_open) + parameters.gap_extend
             > -static_cast<int>(std::numeric_limits<int8_t>::min())) {
        return block_aligner_contract_error::gap_penalty;
    }
    if (!is_block_aligner_lane_shape_supported(lanes)) {
        return block_aligner_contract_error::lane_shape;
    }
    const size_t minimum_block_size = std::max(parameters.minimum_block_size, lanes);
    const size_t maximum_block_size = std::max(parameters.maximum_block_size, lanes);
    if (!std::has_single_bit(minimum_block_size) || !std::has_single_bit(maximum_block_size)
        || minimum_block_size > maximum_block_size
        || maximum_block_size >= std::numeric_limits<std::uint16_t>::max()) {
        return block_aligner_contract_error::block_geometry;
    }
    if ((parameters.mode == block_alignment_mode::x_drop && parameters.x_drop_threshold < 0)
        || (parameters.mode == block_alignment_mode::global && parameters.x_drop_threshold != 0)) {
        return block_aligner_contract_error::x_drop_threshold;
    }
    const bool stage_mode_valid =
        (parameters.stage == block_alignment_stage::left_extension
            && parameters.mode == block_alignment_mode::x_drop
            && parameters.input_order == block_input_order::reversed)
        || (parameters.stage == block_alignment_stage::gap_extension
            && parameters.mode == block_alignment_mode::global
            && parameters.input_order == block_input_order::forward)
        || (parameters.stage == block_alignment_stage::right_extension
            && parameters.mode == block_alignment_mode::x_drop
            && parameters.input_order == block_input_order::forward);
    if (!stage_mode_valid) {
        return block_aligner_contract_error::stage_mode;
    }
    if (parameters.should_compute_edit_distance && !parameters.is_trace_enabled) {
        return block_aligner_contract_error::trace_accounting;
    }
    return block_aligner_contract_error::none;
}

/** Compute the stable semantic identity of an admitted alignment contract. */
[[nodiscard]] constexpr std::uint64_t block_aligner_configuration_identity(
        const block_aligner_parameters& parameters,
        std::uint64_t scoring_identity = 0,
        bool strand_reversed = false) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&hash](std::uint64_t value) constexpr {
        hash ^= value;
        hash *= 1099511628211ULL;
    };
    mix(static_cast<std::uint8_t>(parameters.gap_open));
    mix(static_cast<std::uint8_t>(parameters.gap_extend));
    mix(parameters.minimum_block_size);
    mix(parameters.maximum_block_size);
    mix(static_cast<std::uint32_t>(parameters.x_drop_threshold));
    mix(static_cast<std::uint8_t>(parameters.mode));
    mix(static_cast<std::uint8_t>(parameters.input_order));
    mix(static_cast<std::uint8_t>(parameters.stage));
    mix(parameters.is_trace_enabled);
    mix(parameters.should_compute_edit_distance);
    mix(scoring_identity);
    mix(strand_reversed);
    return hash;
}

/** Record a certified gap without re-entering a target namespace. */
inline void record_block_certified_global_gap(
        const block_global_gap_certificate_plan& plan,
        const block_diagonal_certificate& certificate,
        bool strand_reversed,
        block_aligner_execution_stats* execution_stats) noexcept {
    if (execution_stats == nullptr) return;
    if (!certificate.is_certified || certificate.symbols == 0u
        || certificate.vector_batches == 0u
        || certificate.edit_distance > certificate.symbols
        || !is_block_aligner_lane_shape_supported(plan.simd_lane_count)) {
        std::abort();
    }

    auto& stats = *execution_stats;
    const std::uint64_t identity = block_aligner_configuration_identity(
        plan.parameters, plan.scoring_identity, strand_reversed);

    stats.add(stats.gap_eligible);
    stats.add(stats.global_calls);
    if (plan.parameters.is_trace_enabled) stats.add(stats.trace_calls);
    stats.add(stats.forward_input_calls);
    stats.add(stats.configuration_identity_calls);
    stats.configuration_identity_xor ^= identity;
    stats.configuration_identity_sum += identity;

    stats.add(stats.gap_executed);
    stats.add(stats.executed_global_calls);
    if (plan.parameters.is_trace_enabled) stats.add(stats.executed_trace_calls);
    stats.add(stats.executed_forward_input_calls);
    if (plan.simd_lane_count == 8u) stats.add(stats.simd_eight_lane_calls);
    else stats.add(stats.simd_sixteen_lane_calls);
    stats.add(stats.nonempty_calls);
    stats.add(stats.nonempty_calls_with_simd);
    stats.add(stats.simd_diagonal_certificate_calls);
    stats.add(stats.simd_diagonal_certificate_vector_batches,
        certificate.vector_batches);
    stats.add(stats.simd_diagonal_certificate_symbols,
        certificate.symbols);
    stats.add(stats.executed_configuration_identity_calls);
    stats.executed_configuration_identity_xor ^= identity;
    stats.executed_configuration_identity_sum += identity;
}

/** Abort if Block Aligner would route to a forbidden KSW2 fallback. */
[[noreturn]] inline void abort_forbidden_ksw2_fallback(
        block_aligner_execution_stats& stats) noexcept {
    stats.add(stats.forbidden_ksw2_calls);
    std::abort();
}

}  // namespace bioseq::simd
