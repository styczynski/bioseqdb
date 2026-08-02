/**
 * @file alignment_contracts.cppm
 * @brief Highway-free alignment models shared by generated targets and schedulers.
 */
module;

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <vector>

export module bioseq.alignment.contracts;

export import bioseq.alignment.utils;

export namespace bioseq::simd {

struct KswFlags {
    bool score_only    : 1 = false;
    bool right_align   : 1 = false;
    bool generic_score : 1 = false;
    bool approx_max    : 1 = false;
    bool approx_zdrop  : 1 = false;
    bool extz_only     : 1 = false;
    bool reverse_cigar : 1 = false;
    bool reserved      : 1 = false;
};
static_assert(sizeof(KswFlags) == 1);

struct KswZ2Params {
    int8_t    gap_open   = 0;
    int8_t    gap_extend = 0;
    int       band_width = -1;
    int       z_drop     = -1;
    int       end_bonus  = 0;
    KswFlags  flags      = {};
};

struct KswD2Params {
    int8_t    gap_open_short   = 0;
    int8_t    gap_extend_short = 0;
    int8_t    gap_open_long    = 0;
    int8_t    gap_extend_long  = 0;
    int       band_width       = -1;
    int       z_drop           = -1;
    int       end_bonus        = 0;
    KswFlags  flags            = {};
};

// =============================================================================
// KswAlignResult
// =============================================================================

constexpr int32_t kKswNegInf = -0x40000000;

struct KswAlignResult {
    int32_t max       = 0;
    int32_t score     = kKswNegInf;
    int32_t mqe       = kKswNegInf;
    int32_t mte       = kKswNegInf;
    int32_t max_q     = -1;
    int32_t max_t     = -1;
    int32_t mqe_t     = -1;
    int32_t mte_q     = -1;
    bool    zdropped  = false;
    bool    reach_end = false;
    std::uint64_t edit_distance = 0;
    bool edit_distance_valid = false;
    std::vector<uint32_t> cigar;
};
enum class BlockAlignerPhase : std::uint8_t {
    LeftExtension,
    GapExtension,
    RightExtension,
};

/// Modes implemented by the upstream adaptive-block reference. This slice
/// admits only the modes needed by production: global gap fill and native
/// two-confirmation X-drop extension. KSW2 z-drop is deliberately absent.
enum class BlockAlignmentMode : std::uint8_t {
    Global,
    XDrop,
};

enum class BlockInputOrder : std::uint8_t {
    Forward,
    Reversed,
};

struct BlockAlignerExecutionProof {
    std::uint64_t align_region_invocations = 0;
    std::uint64_t serial_align_region_dispatches = 0;
    std::uint64_t parallel_align_region_dispatches = 0;
    std::uint64_t routed_region_scopes = 0;
    std::uint64_t left_route_candidates = 0;
    std::uint64_t gap_route_candidates = 0;
    std::uint64_t right_route_candidates = 0;
    std::uint64_t left_eligible = 0;
    std::uint64_t left_executed = 0;
    std::uint64_t gap_eligible = 0;
    std::uint64_t gap_executed = 0;
    std::uint64_t right_eligible = 0;
    std::uint64_t right_executed = 0;
    std::uint64_t global_calls = 0;
    std::uint64_t native_xdrop_calls = 0;
    std::uint64_t native_xdrop_terminations = 0;
    std::uint64_t zero_work_calls = 0;
    std::uint64_t nonempty_calls = 0;
    std::uint64_t nonempty_calls_with_simd = 0;
    std::uint64_t trace_calls = 0;
    std::uint64_t executed_trace_calls = 0;
    std::uint64_t forward_input_calls = 0;
    std::uint64_t reversed_input_calls = 0;
    std::uint64_t executed_global_calls = 0;
    std::uint64_t executed_native_xdrop_calls = 0;
    std::uint64_t executed_forward_input_calls = 0;
    std::uint64_t executed_reversed_input_calls = 0;
    std::uint64_t simd_lane8_calls = 0;
    std::uint64_t simd_lane16_calls = 0;
    std::uint64_t simd_dp_vector_batches = 0;
    std::uint64_t simd_dp_cells = 0;
    // Global equal-length calls may be discharged by a SIMD scan whose
    // strict score bound proves that the coordinate diagonal is the unique
    // optimum.  Keep that work separate from DP counters: simd_dp_cells is
    // always the number of cells actually evaluated by ba_align_core.
    std::uint64_t simd_diagonal_certificate_calls = 0;
    std::uint64_t simd_diagonal_certificate_vector_batches = 0;
    std::uint64_t simd_diagonal_certificate_symbols = 0;
    std::uint64_t adaptive_grow_events = 0;
    std::uint64_t adaptive_shrink_events = 0;
    std::uint64_t expected_reversed_compact_symbols = 0;
    std::uint64_t reversed_compact_symbols = 0;
    std::uint64_t config_identity_calls = 0;
    std::uint64_t config_identity_xor = 0;
    std::uint64_t config_identity_sum = 0;
    std::uint64_t executed_config_identity_calls = 0;
    std::uint64_t executed_config_identity_xor = 0;
    std::uint64_t executed_config_identity_sum = 0;
    std::uint64_t unsupported_contract_rejections = 0;
    std::uint64_t forbidden_ksw2_calls = 0;
    std::uint64_t forbidden_scalar_biological_fallbacks = 0;
    bool saturated = false;

    [[nodiscard]] static constexpr bool add_would_overflow(
            std::uint64_t destination,
            std::uint64_t increment) noexcept {
        return increment > (std::numeric_limits<std::uint64_t>::max)()
            - destination;
    }

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

    void add(std::uint64_t& destination, std::uint64_t increment = 1) noexcept {
        add(destination, increment, saturated);
    }

    void combine(const BlockAlignerExecutionProof& other) noexcept {
        saturated = saturated || other.saturated;
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
        add(native_xdrop_calls, other.native_xdrop_calls);
        add(native_xdrop_terminations, other.native_xdrop_terminations);
        add(zero_work_calls, other.zero_work_calls);
        add(nonempty_calls, other.nonempty_calls);
        add(nonempty_calls_with_simd, other.nonempty_calls_with_simd);
        add(trace_calls, other.trace_calls);
        add(executed_trace_calls, other.executed_trace_calls);
        add(forward_input_calls, other.forward_input_calls);
        add(reversed_input_calls, other.reversed_input_calls);
        add(executed_global_calls, other.executed_global_calls);
        add(executed_native_xdrop_calls, other.executed_native_xdrop_calls);
        add(executed_forward_input_calls, other.executed_forward_input_calls);
        add(executed_reversed_input_calls, other.executed_reversed_input_calls);
        add(simd_lane8_calls, other.simd_lane8_calls);
        add(simd_lane16_calls, other.simd_lane16_calls);
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
        add(config_identity_calls, other.config_identity_calls);
        config_identity_xor ^= other.config_identity_xor;
        // Identity sums are deliberately modulo 2^64 (unsigned wrapping),
        // paired with XOR; unlike counters, wrap is part of the checksum.
        config_identity_sum += other.config_identity_sum;
        add(executed_config_identity_calls,
            other.executed_config_identity_calls);
        executed_config_identity_xor ^= other.executed_config_identity_xor;
        executed_config_identity_sum += other.executed_config_identity_sum;
        add(unsupported_contract_rejections,
            other.unsupported_contract_rejections);
        add(forbidden_ksw2_calls, other.forbidden_ksw2_calls);
        add(forbidden_scalar_biological_fallbacks,
            other.forbidden_scalar_biological_fallbacks);
    }

    [[nodiscard]] static constexpr bool sum3(
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

    [[nodiscard]] constexpr std::uint64_t eligible_calls() const noexcept {
        std::uint64_t result = (std::numeric_limits<std::uint64_t>::max)();
        (void)sum3(left_eligible, gap_eligible, right_eligible, result);
        return result;
    }

    [[nodiscard]] constexpr std::uint64_t executed_calls() const noexcept {
        std::uint64_t result = (std::numeric_limits<std::uint64_t>::max)();
        (void)sum3(left_executed, gap_executed, right_executed, result);
        return result;
    }

    [[nodiscard]] constexpr bool accepted() const noexcept {
        std::uint64_t eligible_total = 0;
        std::uint64_t executed_total = 0;
        std::uint64_t requested_xdrop_total = 0;
        std::uint64_t executed_xdrop_total = 0;
        std::uint64_t requested_forward_total = 0;
        std::uint64_t executed_forward_total = 0;
        std::uint64_t work_total = 0;
        std::uint64_t lane_total = 0;
        const bool totals_valid =
            sum3(left_eligible, gap_eligible, right_eligible, eligible_total)
            && sum3(left_executed, gap_executed, right_executed, executed_total)
            && !add_would_overflow(left_eligible, right_eligible)
            && !add_would_overflow(left_executed, right_executed)
            && !add_would_overflow(gap_eligible, right_eligible)
            && !add_would_overflow(gap_executed, right_executed)
            && !add_would_overflow(zero_work_calls, nonempty_calls)
            && !add_would_overflow(simd_lane8_calls, simd_lane16_calls)
            && !add_would_overflow(serial_align_region_dispatches,
                                   parallel_align_region_dispatches);
        if (!totals_valid || saturated) return false;
        requested_xdrop_total = left_eligible + right_eligible;
        executed_xdrop_total = left_executed + right_executed;
        requested_forward_total = gap_eligible + right_eligible;
        executed_forward_total = gap_executed + right_executed;
        work_total = zero_work_calls + nonempty_calls;
        lane_total = simd_lane8_calls + simd_lane16_calls;
        if (simd_diagonal_certificate_calls > nonempty_calls) return false;
        const std::uint64_t dp_nonempty_calls = nonempty_calls
            - simd_diagonal_certificate_calls;
        const bool exact_align_region_dispatch =
            serial_align_region_dispatches
                    + parallel_align_region_dispatches
                == align_region_invocations
            && (routed_region_scopes == 0 || align_region_invocations > 0);
        const bool exact_phase_coverage = left_eligible == left_executed
            && gap_eligible == gap_executed
            && right_eligible == right_executed;
        const bool exact_router_coverage = routed_region_scopes == 0
            || (left_route_candidates == left_executed
                && gap_route_candidates == gap_executed
                && right_route_candidates == right_executed);
        const bool exact_mode_coverage = global_calls == gap_eligible
            && native_xdrop_calls == requested_xdrop_total
            && executed_global_calls == gap_executed
            && executed_native_xdrop_calls == executed_xdrop_total;
        const bool exact_input_order_coverage =
            reversed_input_calls == left_eligible
            && forward_input_calls == requested_forward_total
            && executed_reversed_input_calls == left_executed
            && executed_forward_input_calls == executed_forward_total;
        const bool exact_config_coverage =
            config_identity_calls == eligible_total
            && executed_config_identity_calls == executed_total
            && config_identity_calls == executed_config_identity_calls
            && config_identity_xor == executed_config_identity_xor
            && config_identity_sum == executed_config_identity_sum;
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
        return exact_phase_coverage
            && exact_align_region_dispatch
            && exact_router_coverage
            && exact_mode_coverage
            && exact_input_order_coverage
            && exact_config_coverage
            && truthful_work
            && trace_calls == executed_trace_calls
            && expected_reversed_compact_symbols == reversed_compact_symbols
            && native_xdrop_terminations <= executed_native_xdrop_calls
            && unsupported_contract_rejections == 0
            && forbidden_ksw2_calls == 0
            && forbidden_scalar_biological_fallbacks == 0;
    }

    [[nodiscard]] constexpr bool operator==(
        const BlockAlignerExecutionProof&) const noexcept = default;
};
struct BlockDiagonalCertificate {
    bool certified = false;
    int32_t score = 0;
    std::uint64_t edit_distance = 0;
    std::uint64_t vector_batches = 0;
    std::uint64_t symbols = 0;
};

/// Exact construction counters used by the forced-old differential oracle.
/// They describe arena-backed aligner construction only; the early
/// certificate uses fixed stack chunks and therefore leaves all fields zero.
struct BlockAlignerConstructionEvidence {
    std::uint64_t align_arena_scopes = 0;
    std::uint64_t padded_sequence_allocations = 0;
    std::uint64_t scratch_allocations = 0;
    std::uint64_t trace_allocations = 0;
};
struct BlockAlignerParams {
    int8_t gap_open = 0;        ///< Positive q; reference open is -(q + e)
    int8_t gap_extend = 0;      ///< Positive e; reference extend is -e
    size_t min_size = 32;       ///< Reference minimum adaptive block size
    size_t max_size = 256;      ///< Reference maximum adaptive block size
    int32_t x_drop = 0;         ///< Native Block Aligner X-drop threshold
    BlockAlignmentMode mode = BlockAlignmentMode::Global;
    BlockInputOrder input_order = BlockInputOrder::Forward;
    BlockAlignerPhase phase = BlockAlignerPhase::GapExtension;
    bool trace = true;
    bool compute_edit_distance = false;
};

enum class BlockPackedCertificateDecision : std::uint8_t {
    NotEligible,
    Rejected,
    Certified,
};

/// Immutable per-region state for the resident DNA4/u64 gap certificate.
/// Matrix/contract checks are deliberately hoisted out of the seed-pair loop;
/// exact coordinates and packed sequence contents remain per-gap inputs.
struct BlockGlobalGapCertificatePlan {
    BlockAlignerParams params{};
    std::int8_t match_score = 0;
    std::int8_t mismatch_score = 0;
    std::uint64_t scoring_identity = 0;
    /// Target-local int16 lane count captured by the fixed-target preparer.
    std::uint8_t simd_lanes = 0;
    bool contract_supported = false;
    bool generated_matrix_supported = false;
};

struct BlockPackedCertificateAttempt {
    BlockPackedCertificateDecision decision =
        BlockPackedCertificateDecision::NotEligible;
    BlockDiagonalCertificate certificate{};
};

enum class BlockAlignerContractError : std::uint8_t {
    None,
    GapPenalty,
    BlockGeometry,
    XDropThreshold,
    PhaseMode,
    LaneShape,
    Coordinates,
    ScoringMatrix,
    TraceAccounting,
};

[[nodiscard]] constexpr bool block_aligner_lane_shape_supported(
        size_t lanes) noexcept {
    return lanes == 8 || lanes == 16;
}

[[nodiscard]] constexpr BlockAlignerContractError validate_block_aligner_contract(
        const BlockAlignerParams& params,
        size_t lanes) noexcept {
    if (params.gap_open <= 0 || params.gap_extend <= 0
        || static_cast<int>(params.gap_open) + params.gap_extend
             > -static_cast<int>(std::numeric_limits<int8_t>::min())) {
        return BlockAlignerContractError::GapPenalty;
    }
    if (!block_aligner_lane_shape_supported(lanes)) {
        return BlockAlignerContractError::LaneShape;
    }
    const size_t min_size = std::max(params.min_size, lanes);
    const size_t max_size = std::max(params.max_size, lanes);
    if (!std::has_single_bit(min_size) || !std::has_single_bit(max_size)
        || min_size > max_size
        || max_size >= std::numeric_limits<std::uint16_t>::max()) {
        return BlockAlignerContractError::BlockGeometry;
    }
    if ((params.mode == BlockAlignmentMode::XDrop && params.x_drop < 0)
        || (params.mode == BlockAlignmentMode::Global && params.x_drop != 0)) {
        return BlockAlignerContractError::XDropThreshold;
    }
    const bool phase_mode_valid =
        (params.phase == BlockAlignerPhase::LeftExtension
            && params.mode == BlockAlignmentMode::XDrop
            && params.input_order == BlockInputOrder::Reversed)
        || (params.phase == BlockAlignerPhase::GapExtension
            && params.mode == BlockAlignmentMode::Global
            && params.input_order == BlockInputOrder::Forward)
        || (params.phase == BlockAlignerPhase::RightExtension
            && params.mode == BlockAlignmentMode::XDrop
            && params.input_order == BlockInputOrder::Forward);
    if (!phase_mode_valid) {
        return BlockAlignerContractError::PhaseMode;
    }
    if (params.compute_edit_distance && !params.trace) {
        return BlockAlignerContractError::TraceAccounting;
    }
    return BlockAlignerContractError::None;
}

[[nodiscard]] constexpr std::uint64_t block_aligner_config_identity(
        const BlockAlignerParams& params,
        std::uint64_t scoring_identity = 0,
        bool strand_reversed = false) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&hash](std::uint64_t value) constexpr {
        hash ^= value;
        hash *= 1099511628211ULL;
    };
    mix(static_cast<std::uint8_t>(params.gap_open));
    mix(static_cast<std::uint8_t>(params.gap_extend));
    mix(params.min_size);
    mix(params.max_size);
    mix(static_cast<std::uint32_t>(params.x_drop));
    mix(static_cast<std::uint8_t>(params.mode));
    mix(static_cast<std::uint8_t>(params.input_order));
    mix(static_cast<std::uint8_t>(params.phase));
    mix(params.trace);
    mix(params.compute_edit_distance);
    mix(scoring_identity);
    mix(strand_reversed);
    return hash;
}

/** Record a certified gap without re-entering a target namespace. */
inline void record_block_certified_global_gap(
        const BlockGlobalGapCertificatePlan& plan,
        const BlockDiagonalCertificate& certificate,
        BlockAlignerExecutionProof* execution_proof) noexcept {
    if (execution_proof == nullptr) return;
    if (!certificate.certified || certificate.symbols == 0u
        || certificate.vector_batches == 0u
        || certificate.edit_distance > certificate.symbols
        || !block_aligner_lane_shape_supported(plan.simd_lanes)) {
        std::abort();
    }

    auto& proof = *execution_proof;
    const std::uint64_t identity = block_aligner_config_identity(
        plan.params, plan.scoring_identity, false);

    proof.add(proof.gap_eligible);
    proof.add(proof.global_calls);
    if (plan.params.trace) proof.add(proof.trace_calls);
    proof.add(proof.forward_input_calls);
    proof.add(proof.config_identity_calls);
    proof.config_identity_xor ^= identity;
    proof.config_identity_sum += identity;

    proof.add(proof.gap_executed);
    proof.add(proof.executed_global_calls);
    if (plan.params.trace) proof.add(proof.executed_trace_calls);
    proof.add(proof.executed_forward_input_calls);
    if (plan.simd_lanes == 8u) proof.add(proof.simd_lane8_calls);
    else proof.add(proof.simd_lane16_calls);
    proof.add(proof.nonempty_calls);
    proof.add(proof.nonempty_calls_with_simd);
    proof.add(proof.simd_diagonal_certificate_calls);
    proof.add(proof.simd_diagonal_certificate_vector_batches,
        certificate.vector_batches);
    proof.add(proof.simd_diagonal_certificate_symbols,
        certificate.symbols);
    proof.add(proof.executed_config_identity_calls);
    proof.executed_config_identity_xor ^= identity;
    proof.executed_config_identity_sum += identity;
}

[[noreturn]] inline void block_aligner_forbidden_ksw2_fallback(
        BlockAlignerExecutionProof& proof) noexcept {
    proof.add(proof.forbidden_ksw2_calls);
    std::abort();
}

}  // namespace bioseq::simd
