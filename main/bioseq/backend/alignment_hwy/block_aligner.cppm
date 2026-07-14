/**
 * @file block_aligner.cppm
 * @brief Highway SIMD port of the Block Aligner algorithm.
 *
 * Adaptive-block Smith-Waterman-Gotoh with single-affine gap penalties.
 * Maintains a sliding/growing/shrinking square block instead of computing
 * the full O(N*M) DP matrix.
 *
 * Based on the Rust block-aligner reference (scan_block.rs).
 *
 * The public result reuses the pipeline's KswAlignResult storage type, but the
 * recurrence, termination, traceback, and parameter contract are the native
 * Block Aligner semantics from scan_block.rs.
 */
module;

#include <hwy/highway.h>
#include <hwy/aligned_allocator.h>

#include "common.h"

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

export module bioseq.alignment.block_aligner;

import bioseq.common.mlogging;
import bioseq.common.sequan_compat;
import bioseq.sequences;
import bioseq.simd.utils;
import bioseq.sequences.cigar;
import bioseq.alignment;
import bioseq.alignment.utils;
import bioseq.common.context;
import bioseq.storage.scanner;   // DOC0263 — sequence_scanner_none default
import bioseq.storage.lazy;      // DOC0263 — lazy_compact_sequence<R, Scanner>

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

// DOC0263 §9.1 — the block-aligner encode path runs through the lazy
// wrapper's `encoded_iter<…>().ptr_at(...)` slot ring (§7), not
// `to_encoded_into`; the shared helper lives in bioseq_storage_lazy so
// block_aligner and ksw2 use the same one definition.
using bioseq::storage::encode_region_via_ptr_at;

// =============================================================================
// Constants
// =============================================================================

/// Block shift step size. Fixed at 8 for algorithmic equivalence with the
/// Rust AVX2 reference. When L (SIMD lanes) == 8 (SSE2/NEON), STEP == L.
/// When L == 16 (AVX2), STEP == L/2.
inline constexpr size_t BA_STEP = 8;

/// Consecutive x-drop confirmations before termination.
inline constexpr size_t BA_X_DROP_ITER_COUNT = 2;

/// Whether block shrinking is enabled.
inline constexpr bool BA_SHRINK_ENABLED = true;

/// Elements inspected at the end of each border for shrink decision.
inline constexpr size_t BA_SHRINK_SUFFIX_LEN = BA_STEP / 4;

/// Baseline bias for all i16 DP cells. True delta = cell - BA_ZERO.
inline constexpr int16_t BA_ZERO = static_cast<int16_t>(1 << 14);

/// Minimum representable relative score (saturating floor).
inline constexpr int16_t BA_MIN = 0;

/// The upstream implementations are 128-bit (8 x i16) and AVX2
/// (16 x i16).  Cap wider Highway targets at the proven AVX2 geometry rather
/// than rejecting an AVX-512 host or accidentally changing STEP semantics.
using BlockAlignerSimdTag = hn::CappedTag<int16_t, 16>;

/// The three production phases that may invoke Block Aligner. Keeping the
/// phase in the algorithm-local proof makes a missing left/right call visible
/// without coupling this module to the canonical pipeline attestation ABI.
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
        const bool truthful_work = work_total == executed_total
            && nonempty_calls_with_simd == nonempty_calls
            && lane_total == executed_total
            && ((nonempty_calls == 0
                    && simd_dp_vector_batches == 0 && simd_dp_cells == 0)
                || (nonempty_calls > 0
                    && simd_dp_vector_batches > 0 && simd_dp_cells > 0));
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

struct BlockCoreMetrics {
    std::uint64_t vector_batches = 0;
    std::uint64_t cells = 0;
    std::uint64_t grow_events = 0;
    std::uint64_t shrink_events = 0;
    bool saturated = false;
    bool xdrop_terminated = false;

    void add(std::uint64_t& destination, std::uint64_t increment = 1) noexcept {
        BlockAlignerExecutionProof::add(destination, increment, saturated);
    }

    void add_product(
            std::uint64_t& destination,
            std::uint64_t first,
            std::uint64_t second) noexcept {
        if (first != 0
            && second > (std::numeric_limits<std::uint64_t>::max)() / first) {
            destination = (std::numeric_limits<std::uint64_t>::max)();
            saturated = true;
            return;
        }
        add(destination, first * second);
    }
};

[[noreturn]] inline void block_aligner_invariant_failure(
        std::string_view detail) {
    bioseq_log<ERR>("Block Aligner invariant failure: {}", detail);
    std::abort();
}

// =============================================================================
// Scalar helpers
// =============================================================================

constexpr int16_t ba_clamp_i16(int32_t x) {
    return static_cast<int16_t>(
        std::min<int32_t>(std::max<int32_t>(x, INT16_MIN), INT16_MAX));
}

constexpr size_t ba_div_ceil(size_t n, size_t d) {
    return (n + d - 1) / d;
}

// =============================================================================
// SIMD helpers — all templated on tag D
// =============================================================================

/// ConcatLowerUpper: result = [b_hi | a_lo].
/// On AVX2 shifts by L/2 = STEP lanes.
template<class D>
HWY_INLINE hn::Vec<D> ba_simd_step(D d, hn::Vec<D> a, hn::Vec<D> b) {
    return hn::ConcatLowerUpper(d, a, b);
}

// =============================================================================
// BlockPaddedBytes — internal padded byte buffer for SIMD-safe access
// =============================================================================

/// Padded sequence buffer: [NULL | byte_0..byte_{n-1} | NULL * padding].
/// The 1-byte prefix prevents underread in shift_left_1_borrow.
/// The suffix prevents SIMD overread.
/// Bytes are stored as alphabet rank values (0-based).
///
/// Storage is non-owning: it points into the current align arena (SPEC §5.2,
/// §6.23). Instances must not outlive the enclosing align-scope.
class BlockPaddedBytes {
public:
    BlockPaddedBytes() = default;

    /// Build from compact_sequence_range sub-region.
    template<bool is_reversed, bool write_reversed, int AlphabetDim,
             supports_compact_sequence_range R, class Scanner>
    void fill(const bioseq::storage::lazy_compact_sequence<R, Scanner>& seq,
              size_t bp_from, size_t bp_to, size_t block_size) {
        len_ = bp_to - bp_from;
        const size_t total = 1 + len_ + block_size;
        data_ = context::current().alloc_align_array<uint8_t, 64>(total + 64);
        const uint8_t null_byte = static_cast<uint8_t>(AlphabetDim);
        data_[0] = null_byte;
        encode_region_via_ptr_at<is_reversed, write_reversed>(
            seq, data_.subspan(1, len_), bp_from, bp_to);
        std::memset(data_.data() + 1 + len_, null_byte, block_size);
    }

    /// Get byte at 1-based padded index. get(0) = prefix NULL.
    [[nodiscard]] uint8_t get(size_t i) const { return data_[i]; }

    /// Pointer for SIMD loads at position i in padded buffer.
    [[nodiscard]] const uint8_t* ptr_at(size_t i) const {
        return data_.data() + i;
    }

    /// Original (unpadded) sequence length.
    [[nodiscard]] size_t len() const { return len_; }

private:
    std::span<uint8_t> data_;
    size_t len_ = 0;
};

// BlockSIMDMatrix is replaced by ScoringMatrix from alignment_hwy_utils.

// =============================================================================
// BlockAlignedBuf — SIMD-aligned i16 buffer
// =============================================================================

/// SIMD-aligned i16 scratch buffer. Storage lives in the current align
/// arena (SPEC §5.2) — the buffer must not outlive the enclosing
/// align-scope. The ctor skips the former explicit zero-fill: every
/// caller invokes clear(d, count) before use.
class BlockAlignedBuf {
public:
    BlockAlignedBuf() = default;

    explicit BlockAlignedBuf(size_t capacity)
        : storage_(context::current().alloc_align_array<int16_t, 64>(capacity))
        , capacity_(capacity) {}

    template<class D>
    void clear(D d, size_t count) {
        const auto min_vec = hn::Set(d, BA_MIN);
        const size_t L = hn::Lanes(d);
        for (size_t i = 0; i < count; i += L) {
            hn::Store(min_vec, d, storage_.data() + i);
        }
    }

    template<class D>
    void set_vec(D d, const BlockAlignedBuf& other, size_t idx) {
        hn::Store(hn::Load(d, other.data() + idx), d, data() + idx);
    }

    template<class D>
    void copy_vec(D d, size_t dst, size_t src) {
        hn::Store(hn::Load(d, data() + src), d, data() + dst);
    }

    [[nodiscard]] int16_t get(size_t i) const { return storage_[i]; }
    void set(size_t i, int16_t v) { storage_[i] = v; }
    [[nodiscard]] int16_t* data() { return storage_.data(); }
    [[nodiscard]] const int16_t* data() const { return storage_.data(); }

private:
    std::span<int16_t> storage_;
    size_t capacity_ = 0;
};

// BlockCigarOp, BlockCigarEntry, BlockCigar are now in bioseq_cigar module.

// =============================================================================
// BlockTrace — trace data for CIGAR generation
// =============================================================================

/// CIGAR trace buffer. Five parallel arrays — the `trace_` / `trace2_`
/// pair can reach multi-MiB for long fill alignments.
///
/// Storage is non-owning: it points into the current align arena
/// (SPEC §6.69 — the former std::vector::resize(n, 0) was multi-MiB of
/// wasted zero-fill because the DP writes every cell before traceback).
/// The ctor only zeros `right_`; the other four arrays are always
/// written-before-read by the DP.
class BlockTrace {
public:
    using TraceType = uint32_t;

    BlockTrace() = default;

    BlockTrace(size_t query_len, size_t ref_len, size_t max_size)
        : query_len_(query_len), ref_len_(ref_len) {
        const size_t len = query_len + ref_len + 2;
        const size_t L = hn::Lanes(BlockAlignerSimdTag{});
        if (max_size > (std::numeric_limits<size_t>::max)() / 2
            || len > (std::numeric_limits<size_t>::max)() - max_size * 2) {
            block_aligner_invariant_failure("trace capacity arithmetic overflow");
        }
        const size_t trace_columns = len + max_size * 2;
        const size_t trace_rows = max_size / L;
        if (trace_rows != 0
            && trace_columns > (std::numeric_limits<size_t>::max)() / trace_rows) {
            block_aligner_invariant_failure("trace capacity arithmetic overflow");
        }
        prevalidated_trace_capacity_ = trace_rows * trace_columns;
        if (prevalidated_trace_capacity_
                > (std::numeric_limits<size_t>::max)() - 64) {
            block_aligner_invariant_failure("trace capacity guard overflow");
        }
        const size_t trace_cap = prevalidated_trace_capacity_ + 64;
        const size_t right_cap = ba_div_ceil(len * 2, 64);
        const size_t block_cap = len * 4;
        auto& ctx = context::current();
        trace_       = ctx.alloc_align_array<TraceType>(trace_cap);
        trace2_      = ctx.alloc_align_array<TraceType>(trace_cap);
        right_       = ctx.alloc_align_array<uint64_t>(right_cap);
        block_start_ = ctx.alloc_align_array<uint32_t>(block_cap);
        block_size_  = ctx.alloc_align_array<uint16_t>(block_cap);
        std::memset(right_.data(), 0, right_cap * sizeof(uint64_t));
    }

    void clear(size_t qlen, size_t rlen) {
        std::memset(right_.data(), 0, right_.size() * sizeof(uint64_t));
        trace_idx_ = 0;
        block_idx_ = 0;
        ckpt_trace_idx_ = 0;
        ckpt_block_idx_ = 0;
        query_len_ = qlen;
        ref_len_ = rlen;
    }

    void add_trace(TraceType t, TraceType t2) {
        // Constructor-time capacity proof covers every reference-admitted
        // trace word. Keep only a debug assertion in the hottest DP loop;
        // fail-closed runtime checks remain at capacity construction and at
        // the rectangle-level skip boundary below.
        DEBUG_ASSERT(trace_idx_ < prevalidated_trace_capacity_,
                     "prevalidated Block trace capacity exceeded");
        trace_[trace_idx_] = t;
        trace2_[trace_idx_] = t2;
        ++trace_idx_;
    }

    void add_block(size_t i, size_t j, size_t width, size_t height, bool right) {
        const bool block_slot_valid = block_start_.size() >= 2
            && block_size_.size() >= 2
            && block_idx_ <= (block_start_.size() - 2) / 2
            && block_idx_ <= (block_size_.size() - 2) / 2;
        const size_t word = block_idx_ / 64;
        if (!block_slot_valid || word >= right_.size()
            || i > std::numeric_limits<std::uint32_t>::max()
            || j > std::numeric_limits<std::uint32_t>::max()
            || width > std::numeric_limits<std::uint16_t>::max()
            || height > std::numeric_limits<std::uint16_t>::max()) {
            block_aligner_invariant_failure("trace block capacity/encoding exceeded");
        }
        block_start_[block_idx_ * 2]     = static_cast<uint32_t>(i);
        block_start_[block_idx_ * 2 + 1] = static_cast<uint32_t>(j);
        block_size_[block_idx_ * 2]      = static_cast<uint16_t>(height);
        block_size_[block_idx_ * 2 + 1]  = static_cast<uint16_t>(width);
        const size_t bit  = block_idx_ % 64;
        right_[word] = (right_[word] & ~(1ULL << bit))
                     | (static_cast<uint64_t>(right) << bit);
        ++block_idx_;
    }

    void add_trace_idx(size_t add) {
        if (trace_idx_ > prevalidated_trace_capacity_
            || add > prevalidated_trace_capacity_ - trace_idx_) {
            block_aligner_invariant_failure("trace skip exceeded capacity");
        }
        trace_idx_ += add;
    }
    void save_ckpt() { ckpt_trace_idx_ = trace_idx_; ckpt_block_idx_ = block_idx_; }
    void restore_ckpt() { trace_idx_ = ckpt_trace_idx_; block_idx_ = ckpt_block_idx_; }

    [[nodiscard]] std::span<const TraceType> trace_data() const { return trace_; }
    [[nodiscard]] std::span<const TraceType> trace2_data() const { return trace2_; }
    [[nodiscard]] std::span<const uint64_t>  right_data() const { return right_; }
    [[nodiscard]] std::span<const uint32_t>  block_start_data() const { return block_start_; }
    [[nodiscard]] std::span<const uint16_t>  block_size_data() const { return block_size_; }
    [[nodiscard]] size_t trace_idx() const { return trace_idx_; }
    [[nodiscard]] size_t block_idx() const { return block_idx_; }
    [[nodiscard]] size_t query_len() const { return query_len_; }
    [[nodiscard]] size_t ref_len() const { return ref_len_; }

private:
    std::span<TraceType> trace_;
    std::span<TraceType> trace2_;
    std::span<uint64_t>  right_;
    std::span<uint32_t>  block_start_;
    std::span<uint16_t>  block_size_;
    size_t trace_idx_ = 0, block_idx_ = 0;
    size_t ckpt_trace_idx_ = 0, ckpt_block_idx_ = 0;
    size_t query_len_ = 0, ref_len_ = 0;
    size_t prevalidated_trace_capacity_ = 0;
};

// =============================================================================
// Trace LUT — compile-time lookup table for traceback
// =============================================================================

enum class BlockTraceTable : uint8_t { D = 0, C = 1, R = 2 };

struct BlockTraceLutEntry {
    BlockCigarOp   op;
    size_t         di;
    size_t         dj;
    BlockTraceTable next_table;
};

consteval std::array<std::array<BlockTraceLutEntry, 64>, 2> ba_build_trace_lut() {
    std::array<std::array<BlockTraceLutEntry, 64>, 2> lut{};
    for (size_t r = 0; r < 2; ++r)
        for (size_t i = 0; i < 64; ++i)
            lut[r][i] = {BlockCigarOp::D, 0, 1, BlockTraceTable::D};

    for (size_t right = 0; right < 2; ++right) {
        for (size_t trace = 0; trace < 4; ++trace) {
            for (size_t trace2 = 0; trace2 < 4; ++trace2) {
                for (size_t table_idx = 0; table_idx < 3; ++table_idx) {
                    const size_t idx = (trace << 4) | (trace2 << 2) | table_idx;
                    BlockTraceLutEntry entry{};
                    if (right == 1) {
                        switch (table_idx) {
                        case 0: // D table
                            if (trace == 0b00) {
                                entry = {BlockCigarOp::M, 1, 1, BlockTraceTable::D};
                            } else if (trace & 0b01) {
                                entry = (trace2 & 0b01)
                                    ? BlockTraceLutEntry{BlockCigarOp::D, 0, 1, BlockTraceTable::D}
                                    : BlockTraceLutEntry{BlockCigarOp::D, 0, 1, BlockTraceTable::C};
                            } else {
                                entry = (trace2 & 0b10)
                                    ? BlockTraceLutEntry{BlockCigarOp::I, 1, 0, BlockTraceTable::D}
                                    : BlockTraceLutEntry{BlockCigarOp::I, 1, 0, BlockTraceTable::R};
                            }
                            break;
                        case 1: // C table
                            entry = (trace2 & 0b01)
                                ? BlockTraceLutEntry{BlockCigarOp::D, 0, 1, BlockTraceTable::D}
                                : BlockTraceLutEntry{BlockCigarOp::D, 0, 1, BlockTraceTable::C};
                            break;
                        case 2: // R table
                            entry = (trace2 & 0b10)
                                ? BlockTraceLutEntry{BlockCigarOp::I, 1, 0, BlockTraceTable::D}
                                : BlockTraceLutEntry{BlockCigarOp::I, 1, 0, BlockTraceTable::R};
                            break;
                        }
                    } else {
                        // Down block: C<->R swapped, I<->D swapped
                        switch (table_idx) {
                        case 0: // D table
                            if (trace == 0b00) {
                                entry = {BlockCigarOp::M, 1, 1, BlockTraceTable::D};
                            } else if (trace & 0b01) {
                                entry = (trace2 & 0b01)
                                    ? BlockTraceLutEntry{BlockCigarOp::I, 1, 0, BlockTraceTable::D}
                                    : BlockTraceLutEntry{BlockCigarOp::I, 1, 0, BlockTraceTable::R};
                            } else {
                                entry = (trace2 & 0b10)
                                    ? BlockTraceLutEntry{BlockCigarOp::D, 0, 1, BlockTraceTable::D}
                                    : BlockTraceLutEntry{BlockCigarOp::D, 0, 1, BlockTraceTable::C};
                            }
                            break;
                        case 1: // C table (acts as R in down)
                            entry = (trace2 & 0b10)
                                ? BlockTraceLutEntry{BlockCigarOp::D, 0, 1, BlockTraceTable::D}
                                : BlockTraceLutEntry{BlockCigarOp::D, 0, 1, BlockTraceTable::C};
                            break;
                        case 2: // R table (acts as C in down)
                            entry = (trace2 & 0b01)
                                ? BlockTraceLutEntry{BlockCigarOp::I, 1, 0, BlockTraceTable::D}
                                : BlockTraceLutEntry{BlockCigarOp::I, 1, 0, BlockTraceTable::R};
                            break;
                        }
                    }
                    lut[right][idx] = entry;
                }
            }
        }
    }
    return lut;
}

inline constexpr auto BA_TRACE_LUT = ba_build_trace_lut();

// =============================================================================
// BlockAlignerScratch — pre-allocated working memory
// =============================================================================

struct BlockAlignerScratch {
    BlockTrace trace;
    BlockAlignedBuf D_col, C_col, D_row, R_row;
    BlockAlignedBuf D_col_ckpt, C_col_ckpt, D_row_ckpt, R_row_ckpt;
    BlockAlignedBuf temp_buf1, temp_buf2;

    BlockAlignerScratch(size_t qlen, size_t rlen, size_t max_sz, bool do_trace)
        : trace(do_trace ? BlockTrace(qlen, rlen, max_sz) : BlockTrace())
        , D_col(max_sz), C_col(max_sz), D_row(max_sz), R_row(max_sz)
        , D_col_ckpt(max_sz), C_col_ckpt(max_sz)
        , D_row_ckpt(max_sz), R_row_ckpt(max_sz)
        , temp_buf1(64), temp_buf2(64) {}

    template<class D>
    void clear(D d, size_t qlen, size_t rlen, size_t max_block_sz, bool do_trace) {
        // Match block-aligner-research Allocated::clear exactly. Clearing the
        // complete admitted border geometry is required when a checkpoint is
        // restored after growth; relying on "eventually overwritten" tails
        // made score parity depend on prior arena contents.
        if (do_trace) trace.clear(qlen, rlen);
        D_col.clear(d, max_block_sz); C_col.clear(d, max_block_sz);
        D_row.clear(d, max_block_sz); R_row.clear(d, max_block_sz);
        D_col_ckpt.clear(d, max_block_sz); C_col_ckpt.clear(d, max_block_sz);
        D_row_ckpt.clear(d, max_block_sz); R_row_ckpt.clear(d, max_block_sz);
        const size_t L = hn::Lanes(d);
        temp_buf1.clear(d, L); temp_buf2.clear(d, L);
    }
};

// =============================================================================
// Prefix scan — O(log L) parallel prefix max with cumulative gap cost
// =============================================================================

/// Pre-compute constants for the prefix scan.
/// Returns (gap_extend_all, prefix_scan_lane_consts).
template<class D>
auto ba_get_prefix_scan_consts(D d, hn::Vec<D> gap_extend)
    -> std::pair<hn::Vec<D>, hn::Vec<D>>
{
    const size_t L = hn::Lanes(d);
    auto shift1 = lanes_shift_left_zero<1>(d, gap_extend);
    shift1 = hn::SaturatedAdd(shift1, gap_extend);
    auto shift2 = lanes_shift_left_zero<2>(d, shift1);
    shift2 = hn::SaturatedAdd(shift2, shift1);
    auto shift4 = lanes_shift_left_zero<4>(d, shift2);
    shift4 = hn::SaturatedAdd(shift4, shift2);

    if (L > 8) {
        HWY_ALIGN int16_t tmp[hn::MaxLanes(d)];
        hn::Store(shift4, d, tmp);
        const int16_t half_last = tmp[L / 2 - 1];
        HWY_ALIGN int16_t correct[hn::MaxLanes(d)];
        for (size_t i = 0; i < L / 2; ++i) correct[i] = tmp[i];
        for (size_t i = L / 2; i < L; ++i) {
            correct[i] = ba_clamp_i16(
                static_cast<int32_t>(half_last) + static_cast<int32_t>(tmp[i]));
        }
        return {hn::Load(d, correct), shift4};
    }
    return {shift4, shift4};
}

/// Parallel prefix scan: result[i] = max_{k=0..i} (input[k] + gap * (i-k)).
/// Uses lane-independent shifts (ShiftLeftBytes), NOT SlideUpLanes.
template<class D>
HWY_INLINE hn::Vec<D> ba_prefix_scan(
    D d, hn::Vec<D> input, hn::Vec<D> gap_cost, hn::Vec<D> lane_consts)
{
    const size_t L = hn::Lanes(d);
    auto shift1 = lanes_shift_left_zero<1>(d, input);
    shift1 = hn::SaturatedAdd(shift1, gap_cost);
    shift1 = hn::Max(input, shift1);

    auto gap2 = hn::ShiftLeft<1>(gap_cost);
    auto shift2 = lanes_shift_left_zero<2>(d, shift1);
    shift2 = hn::SaturatedAdd(shift2, gap2);
    shift2 = hn::Max(shift1, shift2);

    auto gap4 = hn::ShiftLeft<2>(gap_cost);
    auto shift4 = lanes_shift_left_zero<4>(d, shift2);
    shift4 = hn::SaturatedAdd(shift4, gap4);
    shift4 = hn::Max(shift2, shift4);

    if (L > 8) {
        HWY_ALIGN int16_t tmp[hn::MaxLanes(d)];
        hn::Store(shift4, d, tmp);
        const int16_t lower_last = tmp[L / 2 - 1];
        HWY_ALIGN int16_t lc[hn::MaxLanes(d)];
        hn::Store(lane_consts, d, lc);
        HWY_ALIGN int16_t correct_arr[hn::MaxLanes(d)];
        for (size_t i = 0; i < L / 2; ++i) correct_arr[i] = BA_MIN;
        for (size_t i = L / 2; i < L; ++i) {
            correct_arr[i] = ba_clamp_i16(
                static_cast<int32_t>(lower_last) + static_cast<int32_t>(lc[i]));
        }
        return hn::Max(shift4, hn::Load(d, correct_arr));
    }
    return shift4;
}

// =============================================================================
// Trace recording — compress DP cell decisions into bitmasks
// =============================================================================

/// Spread 16 low bits of `lo` into the even bit positions of a 32-bit
/// result (bits 0,2,4,...,30) and the 16 low bits of `hi` into the odd
/// bit positions (bits 1,3,5,...,31). Used by the trace recorder to
/// pack two parallel mask vectors into a single TraceType word.
HWY_INLINE constexpr uint32_t ba_interleave_lo_bits_16(uint32_t lo, uint32_t hi) {
    auto spread = [](uint32_t x) -> uint32_t {
        x &= 0xFFFFu;
        x = (x | (x << 8)) & 0x00FF00FFu;
        x = (x | (x << 4)) & 0x0F0F0F0Fu;
        x = (x | (x << 2)) & 0x33333333u;
        x = (x | (x << 1)) & 0x55555555u;
        return x;
    };
    return spread(lo) | (spread(hi) << 1);
}

template<class D>
HWY_INLINE void ba_record_trace(
    D d, BlockTrace& trace,
    hn::Vec<D> D11, hn::Vec<D> C11, hn::Vec<D> R11,
    hn::Vec<D> C11_open, hn::Vec<D> D11_open,
    hn::Vec<D>& prev_trace_R,
    [[maybe_unused]] int16_t relative_zero)
{
    [[maybe_unused]] const size_t L = hn::Lanes(d);
    // TraceType is uint32_t (32 bits). With 2 bits per lane, max L = 16.
    // AVX-512 (L=32) would need uint64_t traces.
    DEBUG_ASSERT(L <= 16, "Trace bitmask overflow: L > 16 not supported");

    // Per-lane "score came from" decisions: BitsFromMask compresses one
    // mask bit per lane into a uint64_t, lowering to PMOVMSKB on x86 and
    // a 1-cycle reduce on NEON. Avoids the previous 4 stack stores +
    // 32 scalar shifts per cell of the production TRACE=true path.
    const auto mask_DC = hn::Eq(D11, C11);
    const auto mask_DR = hn::Eq(D11, R11);
    const uint32_t bits_DC = static_cast<uint32_t>(hn::BitsFromMask(d, mask_DC));
    const uint32_t bits_DR = static_cast<uint32_t>(hn::BitsFromMask(d, mask_DR));
    const BlockTrace::TraceType trace_data =
        ba_interleave_lo_bits_16(bits_DC, bits_DR);

    auto temp_trace_R = hn::VecFromMask(d, hn::Eq(R11, D11_open));
    auto trace_R_shifted = lanes_shift_left_1_borrow(d, temp_trace_R, prev_trace_R);
    const auto mask_C_open = hn::Eq(C11, C11_open);
    const auto mask_R_shifted = hn::MaskFromVec(trace_R_shifted);
    const uint32_t bits_C_open = static_cast<uint32_t>(hn::BitsFromMask(d, mask_C_open));
    const uint32_t bits_R_shifted = static_cast<uint32_t>(hn::BitsFromMask(d, mask_R_shifted));
    const BlockTrace::TraceType trace_data2 =
        ba_interleave_lo_bits_16(bits_C_open, bits_R_shifted);

    prev_trace_R = temp_trace_R;
    trace.add_trace(trace_data, trace_data2);
}

// =============================================================================
// place_block — core DP computation for a rectangular region
// =============================================================================

/// Compute a width x height rectangle of the DP matrix.
/// For DOWN shifts, the caller swaps query<->ref and the border buffers.
/// Returns (D_max, D_argmax_i, D_argmax_j) per lane.
template<bool TRACE, bool X_DROP, bool COLLECT_METRICS,
         supports_compact_sequence_range R, class D,
         size_t AlphabetDim = compact_sequence_range<R>::traits::alphabet_size_n>
HWY_INLINE auto ba_place_block(
    D d,
    const ScoringMatrix<R>& matrix,
    int8_t gap_open_neg,
    int8_t gap_extend_neg,
    const BlockPaddedBytes& query,
    const BlockPaddedBytes& reference,
    BlockTrace& trace,
    size_t start_i, size_t start_j,
    size_t width, size_t height,
    int16_t* HWY_RESTRICT D_col,
    int16_t* HWY_RESTRICT C_col,
    int16_t* HWY_RESTRICT D_row,
    int16_t* HWY_RESTRICT R_row,
    hn::Vec<D> D_corner,
    int16_t relative_zero,
    bool right,
    BlockCoreMetrics& metrics
) -> std::tuple<hn::Vec<D>, hn::Vec<D>, hn::Vec<D>>
{
    const size_t L = hn::Lanes(d);

    const auto gap_open_vec = hn::Set(d, static_cast<int16_t>(gap_open_neg));
    const auto gap_extend_vec = hn::Set(d, static_cast<int16_t>(gap_extend_neg));
    auto [gap_extend_all, prefix_scan_consts] =
        ba_get_prefix_scan_consts(d, gap_extend_vec);
    auto D_max = hn::Set(d, BA_MIN);
    auto D_argmax_i = hn::Set(d, static_cast<int16_t>(0));
    auto D_argmax_j = hn::Set(d, static_cast<int16_t>(0));

    if (width == 0 || height == 0) {
        return {D_max, D_argmax_i, D_argmax_j};
    }

    // gap_open - gap_extend for the prefix scan trick
    const auto gap_open_minus_extend = hn::SaturatedSub(gap_open_vec, gap_extend_vec);
    const auto min_vec = hn::Set(d, BA_MIN);

    hn::Vec<D> D11{}, R11{};

    // The origin (start_i+i==0, start_j+j==0) cell can only fire on the
    // very first place_block call of the very first block. Hoist the
    // condition out of the per-cell branch.
    const bool can_have_origin = (start_i == 0 && start_j == 0);

    size_t processed_columns = 0;
    for (size_t j = 0; j < width; ++j) {
        auto R01 = min_vec;
        D11 = min_vec;
        R11 = min_vec;
        [[maybe_unused]] auto prev_trace_R = min_vec;

        const uint8_t c = reference.get(start_j + j);
        // Per-column-invariant scoring LUT: load the 16-byte row for `c`
        // once, reuse for every `i` in this column. Without this hoist
        // the LoadDup128 fires on every cell (height/L times per column).
        const auto score_lut = matrix.load_score_lut(d, c);

        // Origin can only land on j == 0 when can_have_origin is true.
        const bool col_can_have_origin = can_have_origin && (j == 0);

        for (size_t i = 0; i < height; i += L) {
            auto D10 = hn::Load(d, D_col + i);
            auto C10 = hn::Load(d, C_col + i);

            // Diagonal: shift D10 left by 1, borrow from D_corner
            auto D00 = lanes_shift_left_1_borrow(d, D10, D_corner);
            D_corner = D10;

            // Score lookup (LUT pre-loaded above; no per-cell row load)
            auto scores = matrix.get_scores_with_lut(
                d, score_lut, query.ptr_at(start_i + i));

            // Match/mismatch
            D11 = hn::SaturatedAdd(D00, scores);

            // Origin cell: insert relative_zero at lane 0 — the inner
            // condition is only reachable when `i == 0` (since
            // start_i + i == 0 requires both start_i == 0 and i == 0).
            if (col_can_have_origin && i == 0) {
                D11 = insert_first(d, D11, relative_zero);
            }

            // Column gap: C[i][j] = max(C[i][j-1] + ext, D[i][j-1] + open)
            auto C11_open = hn::SaturatedAdd(D10, gap_open_vec);
            auto C11 = hn::Max(hn::SaturatedAdd(C10, gap_extend_vec), C11_open);
            D11 = hn::Max(D11, C11);

            // Row gap via prefix scan
            auto D11_open = hn::SaturatedAdd(D11, gap_open_minus_extend);
            R11 = ba_prefix_scan(d, D11_open, gap_extend_vec, prefix_scan_consts);

            // Cross-vector R dependency
            R11 = hn::Max(R11, hn::SaturatedAdd(
                broadcast_last(d, R01), gap_extend_all));
            D11 = hn::Max(D11, R11);
            R01 = R11;

            // Trace recording
            if constexpr (TRACE) {
                ba_record_trace(d, trace, D11, C11, R11, C11_open, D11_open,
                                prev_trace_R, relative_zero);
            }

            // Track per-lane maximum
            D_max = hn::Max(D_max, D11);
            if constexpr (X_DROP) {
                auto update_mask = hn::MaskFromVec(
                    hn::VecFromMask(d, hn::Eq(D_max, D11)));
                D_argmax_i = hn::IfThenElse(update_mask,
                    hn::Set(d, static_cast<int16_t>(i)), D_argmax_i);
                D_argmax_j = hn::IfThenElse(update_mask,
                    hn::Set(d, static_cast<int16_t>(j)), D_argmax_j);
            }

            // Store updated column borders
            hn::Store(D11, d, D_col + i);
            hn::Store(C11, d, C_col + i);
        }

        // End of column — update bottom border
        D_corner = hn::Set(d, BA_MIN);
        D_row[j] = extract_last(d, D11);
        R_row[j] = extract_last(d, R11);
        processed_columns = j + 1;

        // Early exit past both sequence ends (global alignment only)
        if constexpr (!X_DROP) {
            if (start_i + height > query.len() && start_j + j >= reference.len()) {
                if constexpr (TRACE) {
                    trace.add_trace_idx((width - 1 - j) * (height / L));
                }
                break;
            }
        }
    }

    if constexpr (COLLECT_METRICS) {
        // Account only columns whose SIMD loop actually completed.  Keeping
        // this outside the per-vector hot loop makes proof collection a pair
        // of checked additions per rectangle rather than two additions per DP
        // vector, while retaining exact early-exit semantics.
        metrics.add_product(
            metrics.vector_batches,
            processed_columns,
            height / L);
        metrics.add_product(metrics.cells, processed_columns, height);
    }

    return {D_max, D_argmax_i, D_argmax_j};
}

// =============================================================================
// Align core helpers
// =============================================================================

/// Shift border arrays by STEP positions. Returns D_corner for next step.
/// Handles both L == STEP (SSE2/NEON) and L > STEP (AVX2) cases.
template<class D>
HWY_INLINE hn::Vec<D> ba_shift_and_offset(
    D d, size_t block_size,
    BlockAlignedBuf& buf1, BlockAlignedBuf& buf2,
    BlockAlignedBuf& temp1, BlockAlignedBuf& temp2,
    hn::Vec<D> off_add)
{
    const size_t L = hn::Lanes(d);
    auto curr1 = hn::SaturatedAdd(hn::Load(d, buf1.data()), off_add);
    auto curr2 = hn::SaturatedAdd(hn::Load(d, buf2.data()), off_add);

    HWY_ALIGN int16_t tmp[hn::MaxLanes(d)];
    hn::Store(curr1, d, tmp);
    const auto D_corner = hn::Set(d, tmp[BA_STEP - 1]);

    if (L == BA_STEP) {
        // L == STEP: shift by one full SIMD vector
        for (size_t i = 0; i + L < block_size; i += L) {
            auto next1 = hn::SaturatedAdd(hn::Load(d, buf1.data() + i + L), off_add);
            auto next2 = hn::SaturatedAdd(hn::Load(d, buf2.data() + i + L), off_add);
            hn::Store(next1, d, buf1.data() + i);
            hn::Store(next2, d, buf2.data() + i);
            curr1 = next1; curr2 = next2;
        }
        hn::Store(hn::Load(d, temp1.data()), d, buf1.data() + block_size - L);
        hn::Store(hn::Load(d, temp2.data()), d, buf2.data() + block_size - L);
    } else {
        // L > STEP: use ConcatLowerUpper (shifts by L/2 = STEP)
        for (size_t i = 0; i + L < block_size; i += L) {
            auto next1 = hn::SaturatedAdd(hn::Load(d, buf1.data() + i + L), off_add);
            auto next2 = hn::SaturatedAdd(hn::Load(d, buf2.data() + i + L), off_add);
            hn::Store(ba_simd_step(d, next1, curr1), d, buf1.data() + i);
            hn::Store(ba_simd_step(d, next2, curr2), d, buf2.data() + i);
            curr1 = next1; curr2 = next2;
        }
        hn::Store(ba_simd_step(d, hn::Load(d, temp1.data()), curr1),
                  d, buf1.data() + block_size - L);
        hn::Store(ba_simd_step(d, hn::Load(d, temp2.data()), curr2),
                  d, buf2.data() + block_size - L);
    }
    return D_corner;
}

/// Offset-adjust two border buffers.
template<class D>
HWY_INLINE void ba_just_offset(D d, size_t block_size,
    BlockAlignedBuf& buf1, BlockAlignedBuf& buf2, hn::Vec<D> off_add)
{
    const size_t L = hn::Lanes(d);
    for (size_t i = 0; i < block_size; i += L) {
        hn::Store(hn::SaturatedAdd(hn::Load(d, buf1.data() + i), off_add),
                  d, buf1.data() + i);
        hn::Store(hn::SaturatedAdd(hn::Load(d, buf2.data() + i), off_add),
                  d, buf2.data() + i);
    }
}

/// Maximum of the first STEP elements in the first SIMD vector.
template<class D>
HWY_INLINE int16_t ba_prefix_max(D d, const int16_t* buf) {
    HWY_ALIGN int16_t tmp[hn::MaxLanes(d)];
    hn::Store(hn::Load(d, buf), d, tmp);
    int16_t result = BA_MIN;
    for (size_t i = 0; i < BA_STEP; ++i) result = std::max(result, tmp[i]);
    return result;
}

/// Maximum of the last SHRINK_SUFFIX_LEN elements in the last vector.
template<class D>
HWY_INLINE int16_t ba_suffix_max(D d, const int16_t* buf, size_t buf_len) {
    HWY_ALIGN int16_t tmp[hn::MaxLanes(d)];
    const size_t L = hn::Lanes(d);
    hn::Store(hn::Load(d, buf + buf_len - L), d, tmp);
    int16_t result = BA_MIN;
    for (size_t i = L - BA_SHRINK_SUFFIX_LEN; i < L; ++i)
        result = std::max(result, tmp[i]);
    return result;
}

// =============================================================================
// align_core — main adaptive-block alignment loop
// =============================================================================

enum class BlockDirection : uint8_t { Right, Down, Grow };

template<bool TRACE, bool X_DROP, bool COLLECT_METRICS,
         supports_compact_sequence_range R, class D,
         size_t AlphabetDim = compact_sequence_range<R>::traits::alphabet_size_n>
void ba_align_core(
    D d,
    int32_t& out_score,
    size_t& out_query_idx,
    size_t& out_ref_idx,
    BlockAlignerScratch& alloc,
    const ScoringMatrix<R>& matrix,
    int8_t gap_open_neg,
    int8_t gap_extend_neg,
    const BlockPaddedBytes& query,
    const BlockPaddedBytes& reference,
    size_t min_size, size_t max_size,
    int32_t x_drop_threshold,
    BlockCoreMetrics& metrics)
{
    const size_t L = hn::Lanes(d);

    int32_t best_max = 0;
    size_t best_argmax_i = 0;
    size_t best_argmax_j = 0;

    auto prev_dir = BlockDirection::Grow;
    auto dir = BlockDirection::Grow;
    size_t prev_size = 0;
    size_t block_size = min_size;

    int32_t off = 0, off_max = 0, prev_off = 0;
    size_t y_drop_iter = 0;
    [[maybe_unused]] size_t x_drop_iter = 0;

    size_t state_i = 0, state_j = 0;
    size_t i_ckpt = 0, j_ckpt = 0;
    int32_t off_ckpt = 0;

    auto D_corner = hn::Set(d, BA_MIN);

    int16_t right_max = BA_MIN;
    int16_t down_max = BA_MIN;

    for (;;) {
        prev_off = off;

        auto grow_D_max = hn::Set(d, BA_MIN);
        [[maybe_unused]] auto grow_D_argmax_i = hn::Set(d, static_cast<int16_t>(0));
        [[maybe_unused]] auto grow_D_argmax_j = hn::Set(d, static_cast<int16_t>(0));

        hn::Vec<D> D_max{}, D_argmax_i{}, D_argmax_j{};

        switch (dir) {
        case BlockDirection::Right: {
            off = off_max;
            auto off_add = hn::Set(d, ba_clamp_i16(prev_off - off));

            if constexpr (TRACE) {
                alloc.trace.add_block(state_i,
                    state_j + block_size - BA_STEP, BA_STEP, block_size, true);
            }

            ba_just_offset(d, block_size, alloc.D_col, alloc.C_col, off_add);

            auto d_corner_arg = (prev_dir == BlockDirection::Down)
                ? hn::SaturatedAdd(D_corner, off_add) : hn::Set(d, BA_MIN);

            std::tie(D_max, D_argmax_i, D_argmax_j) =
                ba_place_block<TRACE, X_DROP, COLLECT_METRICS, R>(
                    d, matrix, gap_open_neg, gap_extend_neg,
                    query, reference, alloc.trace,
                    state_i, state_j + block_size - BA_STEP,
                    BA_STEP, block_size,
                    alloc.D_col.data(), alloc.C_col.data(),
                    alloc.temp_buf1.data(), alloc.temp_buf2.data(),
                    d_corner_arg, ba_clamp_i16(-off + BA_ZERO), true,
                    metrics);

            right_max = ba_prefix_max(d, alloc.D_col.data());
            D_corner = ba_shift_and_offset(d, block_size,
                alloc.D_row, alloc.R_row,
                alloc.temp_buf1, alloc.temp_buf2, off_add);
            down_max = ba_prefix_max(d, alloc.D_row.data());
            break;
        }

        case BlockDirection::Down: {
            off = off_max;
            auto off_add = hn::Set(d, ba_clamp_i16(prev_off - off));

            if constexpr (TRACE) {
                alloc.trace.add_block(state_i + block_size - BA_STEP,
                    state_j, block_size, BA_STEP, false);
            }

            ba_just_offset(d, block_size, alloc.D_row, alloc.R_row, off_add);

            auto d_corner_arg = (prev_dir == BlockDirection::Right)
                ? hn::SaturatedAdd(D_corner, off_add) : hn::Set(d, BA_MIN);

            // Query/ref SWAPPED, D_row<->D_col, R_row<->C_col
            std::tie(D_max, D_argmax_i, D_argmax_j) =
                ba_place_block<TRACE, X_DROP, COLLECT_METRICS, R>(
                    d, matrix, gap_open_neg, gap_extend_neg,
                    reference, query, alloc.trace,
                    state_j, state_i + block_size - BA_STEP,
                    BA_STEP, block_size,
                    alloc.D_row.data(), alloc.R_row.data(),
                    alloc.temp_buf1.data(), alloc.temp_buf2.data(),
                    d_corner_arg, ba_clamp_i16(-off + BA_ZERO), false,
                    metrics);

            down_max = ba_prefix_max(d, alloc.D_row.data());
            D_corner = ba_shift_and_offset(d, block_size,
                alloc.D_col, alloc.C_col,
                alloc.temp_buf1, alloc.temp_buf2, off_add);
            right_max = ba_prefix_max(d, alloc.D_col.data());
            break;
        }

        case BlockDirection::Grow: {
            D_corner = hn::Set(d, BA_MIN);
            const size_t grow_step = block_size - prev_size;

            if constexpr (TRACE) {
                alloc.trace.add_block(state_i + prev_size, state_j,
                    prev_size, grow_step, false);
            }

            // Grow DOWN: prev_size cols x grow_step rows
            hn::Vec<D> D_max1, ai1, aj1;
            std::tie(D_max1, ai1, aj1) =
                ba_place_block<TRACE, X_DROP, COLLECT_METRICS, R>(
                    d, matrix, gap_open_neg, gap_extend_neg,
                    reference, query, alloc.trace,
                    state_j, state_i + prev_size,
                    grow_step, prev_size,
                    alloc.D_row.data(), alloc.R_row.data(),
                    alloc.D_col.data() + prev_size,
                    alloc.C_col.data() + prev_size,
                    hn::Set(d, BA_MIN), ba_clamp_i16(-off + BA_ZERO), false,
                    metrics);
            grow_D_argmax_i = ai1;
            grow_D_argmax_j = aj1;

            if constexpr (TRACE) {
                alloc.trace.add_block(state_i, state_j + prev_size,
                    grow_step, block_size, true);
            }

            // Grow RIGHT: grow_step cols x block_size rows
            hn::Vec<D> D_max2, ai2, aj2;
            std::tie(D_max2, ai2, aj2) =
                ba_place_block<TRACE, X_DROP, COLLECT_METRICS, R>(
                    d, matrix, gap_open_neg, gap_extend_neg,
                    query, reference, alloc.trace,
                    state_i, state_j + prev_size,
                    grow_step, block_size,
                    alloc.D_col.data(), alloc.C_col.data(),
                    alloc.D_row.data() + prev_size,
                    alloc.R_row.data() + prev_size,
                    hn::Set(d, BA_MIN), ba_clamp_i16(-off + BA_ZERO), true,
                    metrics);

            right_max = ba_prefix_max(d, alloc.D_col.data());
            down_max  = ba_prefix_max(d, alloc.D_row.data());
            grow_D_max = D_max1;

            // Save checkpoint after grow
            for (size_t i = 0; i < block_size; i += L) {
                alloc.D_col_ckpt.set_vec(d, alloc.D_col, i);
                alloc.C_col_ckpt.set_vec(d, alloc.C_col, i);
                alloc.D_row_ckpt.set_vec(d, alloc.D_row, i);
                alloc.R_row_ckpt.set_vec(d, alloc.R_row, i);
            }
            if constexpr (TRACE) { alloc.trace.save_ckpt(); }

            D_max = D_max2; D_argmax_i = ai2; D_argmax_j = aj2;
            break;
        }
        } // switch

        // --- Score update ---
        prev_dir = dir;
        int16_t D_max_max = hn::ReduceMax(d, D_max);
        int16_t grow_max_val = hn::ReduceMax(d, grow_D_max);
        int16_t max_val = std::max(D_max_max, grow_max_val);
        off_max = off + static_cast<int32_t>(max_val) - static_cast<int32_t>(BA_ZERO);

        y_drop_iter += 1;
        bool grow_no_max = (dir == BlockDirection::Grow);

        if (off_max > best_max) {
            if constexpr (X_DROP) {
                // Track argmax position
                int16_t lane_max = D_max_max;
                size_t arg_lane = lane_hargmax(d, D_max, lane_max);
                int16_t arg_i = extract_lane(d, D_argmax_i, arg_lane);
                int16_t arg_j = extract_lane(d, D_argmax_j, arg_lane);
                switch (dir) {
                case BlockDirection::Right:
                    best_argmax_i = state_i + static_cast<size_t>(arg_i) + arg_lane;
                    best_argmax_j = state_j + block_size - BA_STEP + static_cast<size_t>(arg_j);
                    break;
                case BlockDirection::Down:
                    best_argmax_i = state_i + block_size - BA_STEP + static_cast<size_t>(arg_j);
                    best_argmax_j = state_j + static_cast<size_t>(arg_i) + arg_lane;
                    break;
                case BlockDirection::Grow:
                    if (D_max_max >= grow_max_val) {
                        best_argmax_i = state_i
                            + static_cast<size_t>(arg_i) + arg_lane;
                        best_argmax_j = state_j + prev_size
                            + static_cast<size_t>(arg_j);
                    } else {
                        const size_t grow_lane = lane_hargmax(
                            d, grow_D_max, grow_max_val);
                        const int16_t grow_i = extract_lane(
                            d, grow_D_argmax_i, grow_lane);
                        const int16_t grow_j = extract_lane(
                            d, grow_D_argmax_j, grow_lane);
                        best_argmax_i = state_i + prev_size
                            + static_cast<size_t>(grow_j);
                        best_argmax_j = state_j
                            + static_cast<size_t>(grow_i) + grow_lane;
                    }
                    break;
                }
            }

            if (block_size < max_size) {
                i_ckpt = state_i; j_ckpt = state_j; off_ckpt = off;
                for (size_t i = 0; i < block_size; i += L) {
                    alloc.D_col_ckpt.set_vec(d, alloc.D_col, i);
                    alloc.C_col_ckpt.set_vec(d, alloc.C_col, i);
                    alloc.D_row_ckpt.set_vec(d, alloc.D_row, i);
                    alloc.R_row_ckpt.set_vec(d, alloc.R_row, i);
                }
                if constexpr (TRACE) { alloc.trace.save_ckpt(); }
                grow_no_max = false;
            }
            best_max = off_max;
            y_drop_iter = 0;
        }

        // --- X-drop check ---
        if constexpr (X_DROP) {
            if (off_max < best_max - x_drop_threshold) {
                x_drop_iter += 1;
                if (x_drop_iter >= BA_X_DROP_ITER_COUNT) {
                    metrics.xdrop_terminated = true;
                    break;
                }
            } else {
                x_drop_iter = 0;
            }
        }

        // --- End check ---
        if (state_i + block_size > query.len()
            && state_j + block_size > reference.len()) {
            break;
        }

        // --- Forced direction ---
        if (state_j + block_size > reference.len()) {
            state_i += BA_STEP; dir = BlockDirection::Down; continue;
        }
        if (state_i + block_size > query.len()) {
            state_j += BA_STEP; dir = BlockDirection::Right; continue;
        }

        // --- Y-drop grow ---
        size_t next_size = block_size * 2;
        if (next_size <= max_size) {
            if (y_drop_iter > (block_size / BA_STEP) - 1 || grow_no_max) {
                prev_size = block_size;
                block_size = next_size;
                dir = BlockDirection::Grow;
                if constexpr (COLLECT_METRICS) {
                    metrics.add(metrics.grow_events);
                }
                state_i = i_ckpt; state_j = j_ckpt; off = off_ckpt;
                for (size_t i = 0; i < prev_size; i += L) {
                    alloc.D_col.set_vec(d, alloc.D_col_ckpt, i);
                    alloc.C_col.set_vec(d, alloc.C_col_ckpt, i);
                    alloc.D_row.set_vec(d, alloc.D_row_ckpt, i);
                    alloc.R_row.set_vec(d, alloc.R_row_ckpt, i);
                }
                if constexpr (TRACE) { alloc.trace.restore_ckpt(); }
                y_drop_iter = 0;
                continue;
            }
        }

        // --- Shrink ---
        if constexpr (BA_SHRINK_ENABLED) {
            if (block_size > min_size && y_drop_iter == 0) {
                int16_t shrink_max = std::max(
                    ba_suffix_max(d, alloc.D_row.data(), block_size),
                    ba_suffix_max(d, alloc.D_col.data(), block_size));
                if (shrink_max >= max_val) {
                    prev_dir = BlockDirection::Grow; // prevent D_corner use
                    block_size /= 2;
                    if constexpr (COLLECT_METRICS) {
                        metrics.add(metrics.shrink_events);
                    }
                    for (size_t i = 0; i < block_size; i += L) {
                        alloc.D_col.copy_vec(d, i, i + block_size);
                        alloc.C_col.copy_vec(d, i, i + block_size);
                        alloc.D_row.copy_vec(d, i, i + block_size);
                        alloc.R_row.copy_vec(d, i, i + block_size);
                    }
                    state_i += block_size; state_j += block_size;
                    i_ckpt = state_i; j_ckpt = state_j; off_ckpt = off;
                    for (size_t i = 0; i < block_size; i += L) {
                        alloc.D_col_ckpt.set_vec(d, alloc.D_col, i);
                        alloc.C_col_ckpt.set_vec(d, alloc.C_col, i);
                        alloc.D_row_ckpt.set_vec(d, alloc.D_row, i);
                        alloc.R_row_ckpt.set_vec(d, alloc.R_row, i);
                    }
                    right_max = ba_prefix_max(d, alloc.D_col.data());
                    down_max = ba_prefix_max(d, alloc.D_row.data());
                    if constexpr (TRACE) { alloc.trace.save_ckpt(); }
                    y_drop_iter = 0;
                }
            }
        }

        // --- Direction decision ---
        if (down_max > right_max) {
            state_i += BA_STEP; dir = BlockDirection::Down;
        } else {
            state_j += BA_STEP; dir = BlockDirection::Right;
        }
    } // main loop

    // --- Extract result ---
    if constexpr (X_DROP) {
        out_score = best_max;
        out_query_idx = best_argmax_i;
        out_ref_idx = best_argmax_j;
    } else {
        int16_t final_cell = BA_ZERO;
        if (dir == BlockDirection::Right || dir == BlockDirection::Grow) {
            if (state_i > query.len()) {
                block_aligner_invariant_failure(
                    "global result query origin exceeded sequence length");
            }
            const size_t idx = query.len() - state_i;
            if (idx >= block_size) {
                block_aligner_invariant_failure(
                    "global result query border index exceeded block");
            }
            final_cell = alloc.D_col.get(idx);
        } else {
            if (state_j > reference.len()) {
                block_aligner_invariant_failure(
                    "global result reference origin exceeded sequence length");
            }
            const size_t idx = reference.len() - state_j;
            if (idx >= block_size) {
                block_aligner_invariant_failure(
                    "global result reference border index exceeded block");
            }
            final_cell = alloc.D_row.get(idx);
        }
        out_score = off + static_cast<int32_t>(final_cell)
                       - static_cast<int32_t>(BA_ZERO);
        out_query_idx = query.len();
        out_ref_idx = reference.len();
    }
}

// =============================================================================
// Traceback — CIGAR generation from recorded trace data
// =============================================================================

void ba_cigar_core(
    const BlockTrace& trace,
    size_t end_i, size_t end_j,
    std::vector<uint32_t>& cigar)
{
    cigar.clear();
    if (end_i > (std::numeric_limits<size_t>::max)() - end_j) {
        block_aligner_invariant_failure("traceback CIGAR capacity overflow");
    }
    // A traceback step consumes at least one query/reference symbol, so this
    // is an upper bound on the number of packed runs. Reserving it keeps
    // the hot traceback allocation-free after the one result-vector growth,
    // while using one quarter of the old BlockCigar entry storage.
    cigar.reserve(end_i + end_j);

    // scan_block.rs exposes two traceback projections: cigar(), which emits
    // M for every diagonal, and cigar_eq(), which reloads both sequences to
    // distinguish = from X. The pipeline ABI intentionally projects M, =,
    // and X to the same minimap packed-M opcode, so doing cigar_eq() work was
    // both unobservable and expensive. Accumulate the reference cigar()
    // operations locally and materialize only one packed entry per run.
    BlockCigarOp pending_op = BlockCigarOp::Sentinel;
    size_t pending_len = 0;
    const auto flush_pending = [&]() {
        if (pending_len == 0) return;

        uint32_t opcode = CIGAR_MATCH;
        switch (pending_op) {
            case BlockCigarOp::M:
            case BlockCigarOp::Eq:
            case BlockCigarOp::X: opcode = CIGAR_MATCH; break;
            case BlockCigarOp::I: opcode = CIGAR_INS; break;
            case BlockCigarOp::D: opcode = CIGAR_DEL; break;
            case BlockCigarOp::Sentinel: return;
        }
        // Keep projection-time coalescing so a future trace entry that uses
        // Eq/X still has exactly the canonical packed-M behavior of the old
        // BlockCigar::to_packed_cigar path.
        push_packed_cigar(
            cigar, opcode, static_cast<uint32_t>(pending_len));
    };
    const auto add_op = [&](BlockCigarOp op) {
        if (op == pending_op) {
            ++pending_len;
            return;
        }
        flush_pending();
        pending_op = op;
        pending_len = 1;
    };

    const size_t L = hn::Lanes(BlockAlignerSimdTag{});

    size_t block_idx = trace.block_idx();
    size_t trace_idx = trace.trace_idx();
    auto table = BlockTraceTable::D;

    size_t i = end_i, j = end_j;

    while (i > 0 || j > 0) {
        // Find the block containing (i, j)
        size_t block_i, block_j, block_height, block_width;
        bool right;
        for (;;) {
            if (block_idx == 0) {
                block_aligner_invariant_failure(
                    "traceback exhausted blocks before reaching origin");
            }
            block_idx -= 1;
            block_i      = trace.block_start_data()[block_idx * 2];
            block_j      = trace.block_start_data()[block_idx * 2 + 1];
            block_height = trace.block_size_data()[block_idx * 2];
            block_width  = trace.block_size_data()[block_idx * 2 + 1];
            const size_t block_trace_words = block_width * block_height / L;
            if (block_trace_words > trace_idx) {
                block_aligner_invariant_failure("traceback trace index underflow");
            }
            trace_idx -= block_trace_words;

            if (i >= block_i && j >= block_j) {
                const size_t word = block_idx / 64;
                const size_t bit  = block_idx % 64;
                if (word >= trace.right_data().size()) {
                    block_aligner_invariant_failure(
                        "traceback direction index exceeded capacity");
                }
                right = (trace.right_data()[word] >> bit) & 1;
                break;
            }
        }

        const auto& lut = BA_TRACE_LUT[right ? 1 : 0];

        if (right) {
            // Right block: vectors vertical (height/L per column)
            while (i >= block_i && j >= block_j && (i > 0 || j > 0)) {
                size_t curr_i = i - block_i;
                size_t curr_j = j - block_j;
                size_t idx = trace_idx + curr_i / L + curr_j * (block_height / L);

                if (idx >= trace.trace_idx()
                    || idx >= trace.trace_data().size()
                    || idx >= trace.trace2_data().size()) {
                    block_aligner_invariant_failure(
                        "traceback cell index exceeded written trace");
                }

                uint32_t t  = (trace.trace_data()[idx]  >> ((curr_i % L) * 2)) & 0b11;
                uint32_t t2 = (trace.trace2_data()[idx] >> ((curr_i % L) * 2)) & 0b11;

                size_t lut_idx = (t << 4) | (t2 << 2)
                               | static_cast<size_t>(table);
                const auto& entry = lut[lut_idx];

                i -= entry.di;
                j -= entry.dj;
                table = entry.next_table;
                add_op(entry.op);
            }
        } else {
            // Down block: vectors horizontal (width/L per row)
            while (i >= block_i && j >= block_j && (i > 0 || j > 0)) {
                size_t curr_i = i - block_i;
                size_t curr_j = j - block_j;
                size_t idx = trace_idx + curr_j / L + curr_i * (block_width / L);

                if (idx >= trace.trace_idx()
                    || idx >= trace.trace_data().size()
                    || idx >= trace.trace2_data().size()) {
                    block_aligner_invariant_failure(
                        "traceback cell index exceeded written trace");
                }

                uint32_t t  = (trace.trace_data()[idx]  >> ((curr_j % L) * 2)) & 0b11;
                uint32_t t2 = (trace.trace2_data()[idx] >> ((curr_j % L) * 2)) & 0b11;

                size_t lut_idx = (t << 4) | (t2 << 2)
                               | static_cast<size_t>(table);
                const auto& entry = lut[lut_idx];

                i -= entry.di;
                j -= entry.dj;
                table = entry.next_table;
                add_op(entry.op);
            }
        }
    }
    flush_pending();
    std::reverse(cigar.begin(), cigar.end());
}

// =============================================================================
// Native Block Aligner parameter contract
// =============================================================================

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
    mix(scoring_identity);
    mix(strand_reversed);
    return hash;
}

inline void block_aligner_record_eligible(
        BlockAlignerExecutionProof& proof,
        const BlockAlignerParams& params,
        std::size_t query_len,
        std::size_t target_len,
        std::uint64_t config_identity) noexcept {
    switch (params.phase) {
    case BlockAlignerPhase::LeftExtension: proof.add(proof.left_eligible); break;
    case BlockAlignerPhase::GapExtension: proof.add(proof.gap_eligible); break;
    case BlockAlignerPhase::RightExtension: proof.add(proof.right_eligible); break;
    }
    if (params.mode == BlockAlignmentMode::Global) proof.add(proof.global_calls);
    else proof.add(proof.native_xdrop_calls);
    if (params.trace) proof.add(proof.trace_calls);
    if (params.input_order == BlockInputOrder::Reversed) {
        proof.add(proof.reversed_input_calls);
        if (query_len != 0 && target_len != 0) {
            proof.add(proof.expected_reversed_compact_symbols, query_len);
            proof.add(proof.expected_reversed_compact_symbols, target_len);
        }
    } else {
        proof.add(proof.forward_input_calls);
    }
    proof.add(proof.config_identity_calls);
    proof.config_identity_xor ^= config_identity;
    proof.config_identity_sum += config_identity;
}

inline void block_aligner_record_executed(
        BlockAlignerExecutionProof& proof,
        const BlockAlignerParams& params,
        std::size_t lanes,
        bool zero_work,
        const BlockCoreMetrics& metrics,
        std::uint64_t config_identity) noexcept {
    switch (params.phase) {
    case BlockAlignerPhase::LeftExtension: proof.add(proof.left_executed); break;
    case BlockAlignerPhase::GapExtension: proof.add(proof.gap_executed); break;
    case BlockAlignerPhase::RightExtension: proof.add(proof.right_executed); break;
    }
    if (params.mode == BlockAlignmentMode::Global) {
        proof.add(proof.executed_global_calls);
    } else {
        proof.add(proof.executed_native_xdrop_calls);
    }
    if (params.trace) proof.add(proof.executed_trace_calls);
    if (params.input_order == BlockInputOrder::Reversed) {
        proof.add(proof.executed_reversed_input_calls);
    } else {
        proof.add(proof.executed_forward_input_calls);
    }
    if (lanes == 8) proof.add(proof.simd_lane8_calls);
    else if (lanes == 16) proof.add(proof.simd_lane16_calls);
    else proof.saturated = true;
    if (zero_work) {
        proof.add(proof.zero_work_calls);
    } else {
        proof.add(proof.nonempty_calls);
        if (metrics.vector_batches != 0 && metrics.cells != 0
            && !metrics.saturated) {
            proof.add(proof.nonempty_calls_with_simd);
        }
    }
    proof.add(proof.simd_dp_vector_batches, metrics.vector_batches);
    proof.add(proof.simd_dp_cells, metrics.cells);
    proof.add(proof.adaptive_grow_events, metrics.grow_events);
    proof.add(proof.adaptive_shrink_events, metrics.shrink_events);
    if (metrics.xdrop_terminated) {
        proof.add(proof.native_xdrop_terminations);
    }
    proof.saturated = proof.saturated || metrics.saturated;
    proof.add(proof.executed_config_identity_calls);
    proof.executed_config_identity_xor ^= config_identity;
    proof.executed_config_identity_sum += config_identity;
}

[[noreturn]] inline void block_aligner_forbidden_ksw2_fallback(
        BlockAlignerExecutionProof& proof) {
    proof.add(proof.forbidden_ksw2_calls);
    bioseq_log<ERR>("Block Aligner attempted forbidden KSW2 fallback");
    std::abort();
}

[[noreturn]] inline void block_aligner_forbidden_scalar_biological_fallback(
        BlockAlignerExecutionProof& proof) {
    proof.add(proof.forbidden_scalar_biological_fallbacks);
    bioseq_log<ERR>("Block Aligner attempted forbidden scalar biological fallback");
    std::abort();
}

// =============================================================================
// HwyBlockAligner — public API
// =============================================================================

class HwyBlockAligner {
public:
    /// Run block aligner on a sub-region of the given sequences.
    /// Returns KswAlignResult as the pipeline's shared result storage ABI.
    ///
    /// Gap convention: gap_open/gap_extend are the positive minimap q/e
    /// encoding only; this does not request KSW recurrence semantics. They are
    /// converted to the reference's negative penalties:
    ///   internal_open   = -(gap_open + gap_extend)
    ///   internal_extend = -gap_extend
    template<
        bool is_reversed,
        supports_compact_sequence_range QueryRange,
        supports_compact_sequence_range TargetRange,
        size_t AlphabetDim = compact_sequence_range<QueryRange>::traits::alphabet_size_n,
        class Scanner = bioseq::storage::sequence_scanner_none<
            typename compact_sequence_range<QueryRange>::traits::storage_unit>>
    [[nodiscard]] static KswAlignResult align(
        const bioseq::storage::lazy_compact_sequence<QueryRange, Scanner>& query_seq,
        const bioseq::storage::lazy_compact_sequence<TargetRange, Scanner>& target_seq,
        size_t query_start,
        size_t query_end,
        size_t target_start,
        size_t target_end,
        const ScoringMatrix<QueryRange>& scoring_matrix,
        const BlockAlignerParams& params,
        BlockAlignerExecutionProof* execution_proof = nullptr)
    {
        using query_alphabet_t = typename compact_sequence_range<
            QueryRange>::traits::alphabet_type;
        using target_alphabet_t = typename compact_sequence_range<
            TargetRange>::traits::alphabet_type;
        static_assert(std::same_as<query_alphabet_t, target_alphabet_t>,
            "Block Aligner query and target alphabets must be identical");
        static_assert(
            compact_sequence_range<QueryRange>::traits::alphabet_size_n
                == compact_sequence_range<TargetRange>::traits::alphabet_size_n,
            "Block Aligner query and target alphabet ranks must be compatible");
        static_assert(
            AlphabetDim
                == compact_sequence_range<QueryRange>::traits::alphabet_size_n,
            "Block Aligner scoring/padding alphabet dimension must match input");

        KswAlignResult result;

        const BlockAlignerSimdTag d;
        const size_t L = hn::Lanes(d);
        if (execution_proof != nullptr
            && !block_aligner_lane_shape_supported(L)) {
            // The native path has no scalar biological implementation. Make
            // the forbidden boundary operational and fail before any work.
            block_aligner_forbidden_scalar_biological_fallback(
                *execution_proof);
        }
        const auto contract_error = validate_block_aligner_contract(params, L);
        const bool scoring_matrix_valid = scoring_matrix.block_layout_valid();
        const bool ordered_coordinates = query_end >= query_start
            && target_end >= target_start;
        const size_t query_len = ordered_coordinates
            ? query_end - query_start : 0;
        const size_t target_len = ordered_coordinates
            ? target_end - target_start : 0;
        constexpr size_t max_result_coordinate = static_cast<size_t>(
            std::numeric_limits<int32_t>::max());
        constexpr size_t max_packed_cigar_run = static_cast<size_t>(
            std::numeric_limits<std::uint32_t>::max() >> 4u);
        const bool coordinates_valid = ordered_coordinates
            && query_end <= query_seq.bp_size()
            && target_end <= target_seq.bp_size()
            && query_len <= max_result_coordinate
            && target_len <= max_result_coordinate
            && (!params.trace
                || (query_len <= max_packed_cigar_run
                    && target_len <= max_packed_cigar_run));
        const auto effective_contract_error = !coordinates_valid
            ? BlockAlignerContractError::Coordinates
            : !scoring_matrix_valid
                ? BlockAlignerContractError::ScoringMatrix
                : contract_error;
        if (effective_contract_error != BlockAlignerContractError::None) {
            if (execution_proof != nullptr) {
                execution_proof->add(
                    execution_proof->unsupported_contract_rejections);
            }
            bioseq_log<ERR>(
                "Block Aligner rejected contract (error={}, coordinates_valid={})",
                static_cast<unsigned>(effective_contract_error),
                coordinates_valid);
            std::abort();
        }
        const std::uint64_t config_identity = block_aligner_config_identity(
            params, scoring_matrix.block_semantic_identity(), is_reversed);
        if (execution_proof != nullptr) {
            block_aligner_record_eligible(
                *execution_proof, params, query_len, target_len,
                config_identity);
        }

        if (query_len == 0 || target_len == 0) {
            if (params.mode == BlockAlignmentMode::XDrop) {
                result.score = 0;
                result.max = 0;
            } else {
                const size_t gap_len = std::max(query_len, target_len);
                result.score = gap_len == 0 ? 0 : -(
                    static_cast<int32_t>(params.gap_open)
                    + static_cast<int32_t>(params.gap_extend) * gap_len);
                result.max = result.score;
                result.max_q = static_cast<int32_t>(query_len) - 1;
                result.max_t = static_cast<int32_t>(target_len) - 1;
                if (params.trace && gap_len != 0) {
                    const std::uint32_t opcode = query_len == 0
                        ? CIGAR_DEL : CIGAR_INS;
                    result.cigar.push_back(
                        (static_cast<std::uint32_t>(gap_len) << 4u) | opcode);
                }
                result.reach_end = true;
                result.mqe = result.score;
                result.mqe_t = result.max_t;
                result.mte = result.score;
                result.mte_q = result.max_q;
            }
            if (execution_proof != nullptr) {
                block_aligner_record_executed(
                    *execution_proof, params, L, true, BlockCoreMetrics{},
                    config_identity);
            }
            return result;
        }

        // Every arena-backed buffer below (BlockPaddedBytes, BlockAlignedBuf,
        // BlockTrace, scratch encoded-sequence spans) dies here. The CIGAR
        // output lives on result's own vector so it survives the unwind.
        auto align_scope = context::current().push<context::Arena::Align>();

        const size_t min_size = std::max(params.min_size, L);
        const size_t max_size = std::max(params.max_size, L);

        // Convert positive q/e into the reference's negative gap costs.
        const int8_t gap_open_neg = static_cast<int8_t>(
            -(static_cast<int>(params.gap_open) + params.gap_extend));
        const int8_t gap_extend_neg = static_cast<int8_t>(-params.gap_extend);

        BlockPaddedBytes query_padded, ref_padded;
        if (params.input_order == BlockInputOrder::Reversed) {
            // Emit the reversed alignment orientation directly from compact
            // storage. This is the same orientation matrix used by the prior
            // KSW encoder, without a scalar whole-buffer reverse pass.
            query_padded.template fill<is_reversed, true, AlphabetDim>(
                query_seq, query_start, query_end, max_size);
            ref_padded.template fill<false, true, AlphabetDim>(
                target_seq, target_start, target_end, max_size);
            if (execution_proof != nullptr) {
                execution_proof->add(
                    execution_proof->reversed_compact_symbols, query_len);
                execution_proof->add(
                    execution_proof->reversed_compact_symbols, target_len);
            }
        } else {
            // Hot path (gap-fill): encode the sub-region directly into
            // the padded buffer. Saves one arena allocation and one
            // memcpy per query/target compared to the previous
            // encode-then-pad pipeline.
            query_padded.template fill<is_reversed, false, AlphabetDim>(
                query_seq, query_start, query_end, max_size);
            ref_padded.template fill<false, false, AlphabetDim>(
                target_seq, target_start, target_end, max_size);
        }

        // Allocate scratch space
        const bool do_trace = params.trace;
        BlockAlignerScratch scratch(query_len, target_len, max_size, do_trace);
        // The reference clears the full maximum border/checkpoint geometry.
        scratch.clear(d, query_len, target_len, max_size, do_trace);

        int32_t score = 0;
        size_t out_qi = 0, out_ri = 0;

        const bool use_xdrop = params.mode == BlockAlignmentMode::XDrop;
        BlockCoreMetrics metrics;

        // Dispatch based on TRACE and X_DROP combinations
        if (do_trace && use_xdrop) {
            if (execution_proof != nullptr) {
                ba_align_core<true, true, true, QueryRange>(
                    d, score, out_qi, out_ri, scratch,
                    scoring_matrix, gap_open_neg, gap_extend_neg,
                    query_padded, ref_padded, min_size, max_size,
                    params.x_drop, metrics);
            } else {
                ba_align_core<true, true, false, QueryRange>(
                    d, score, out_qi, out_ri, scratch,
                    scoring_matrix, gap_open_neg, gap_extend_neg,
                    query_padded, ref_padded, min_size, max_size,
                    params.x_drop, metrics);
            }
        } else if (do_trace && !use_xdrop) {
            if (execution_proof != nullptr) {
                ba_align_core<true, false, true, QueryRange>(
                    d, score, out_qi, out_ri, scratch,
                    scoring_matrix, gap_open_neg, gap_extend_neg,
                    query_padded, ref_padded, min_size, max_size, 0, metrics);
            } else {
                ba_align_core<true, false, false, QueryRange>(
                    d, score, out_qi, out_ri, scratch,
                    scoring_matrix, gap_open_neg, gap_extend_neg,
                    query_padded, ref_padded, min_size, max_size, 0, metrics);
            }
        } else if (!do_trace && use_xdrop) {
            if (execution_proof != nullptr) {
                ba_align_core<false, true, true, QueryRange>(
                    d, score, out_qi, out_ri, scratch,
                    scoring_matrix, gap_open_neg, gap_extend_neg,
                    query_padded, ref_padded, min_size, max_size,
                    params.x_drop, metrics);
            } else {
                ba_align_core<false, true, false, QueryRange>(
                    d, score, out_qi, out_ri, scratch,
                    scoring_matrix, gap_open_neg, gap_extend_neg,
                    query_padded, ref_padded, min_size, max_size,
                    params.x_drop, metrics);
            }
        } else {
            if (execution_proof != nullptr) {
                ba_align_core<false, false, true, QueryRange>(
                    d, score, out_qi, out_ri, scratch,
                    scoring_matrix, gap_open_neg, gap_extend_neg,
                    query_padded, ref_padded, min_size, max_size, 0, metrics);
            } else {
                ba_align_core<false, false, false, QueryRange>(
                    d, score, out_qi, out_ri, scratch,
                    scoring_matrix, gap_open_neg, gap_extend_neg,
                    query_padded, ref_padded, min_size, max_size, 0, metrics);
            }
        }

        // Build CIGAR via traceback
        if (do_trace) {
            ba_cigar_core(
                scratch.trace, out_qi, out_ri, result.cigar);
            if (params.input_order == BlockInputOrder::Reversed) {
                std::reverse(result.cigar.begin(), result.cigar.end());
            }
        }

        // Fill KswAlignResult fields
        result.score = score;
        result.max = score;
        result.max_q = static_cast<int32_t>(out_qi) - 1;
        result.max_t = static_cast<int32_t>(out_ri) - 1;
        result.zdropped = metrics.xdrop_terminated;

        if (out_qi >= query_len) {
            result.mqe = score;
            result.mqe_t = static_cast<int32_t>(out_ri) - 1;
            // KSW's reach_end consumer asks whether the query extension
            // reached its admitted end. The reference result uses exclusive
            // coordinates, so out_qi == query_len is the exact condition;
            // the reference axis may legitimately stop earlier.
            result.reach_end = true;
        }
        if (out_ri >= target_len) {
            result.mte = score;
            result.mte_q = static_cast<int32_t>(out_qi) - 1;
        }

        if (execution_proof != nullptr) {
            block_aligner_record_executed(
                *execution_proof, params, L, false, metrics,
                config_identity);
        }

        return result;
    }
};

}  // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();

}  // export
