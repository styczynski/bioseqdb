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
#include <utility>

export module bioseq.highway.sources.block_aligner;

import std;
import bioseq.common.mlogging;
import bioseq.common.sequan_compat;
import bioseq.sequences;
import bioseq.highway.sources.simd_utils;
import bioseq.sequences.cigar;
import bioseq.alignment.contracts;
import bioseq.highway.sources.alignment_utils;
import bioseq.highway.sources.alignment_lanes;
import bioseq.common.context;
import bioseq.storage.scanner;   // DOC0263 — sequence_scanner_none default
import bioseq.storage.lazy;      // DOC0263 — canonical erased lazy view

export namespace bioseq::simd {

namespace [[bioseq::simd]] target {
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
// Public phase, mode, input-order, and proof models are owned by
// bioseq.alignment.contracts.

struct BlockCoreMetrics {
    std::uint64_t vector_batches = 0;
    std::uint64_t cells = 0;
    std::uint64_t diagonal_certificate_vector_batches = 0;
    std::uint64_t diagonal_certificate_symbols = 0;
    std::uint64_t grow_events = 0;
    std::uint64_t shrink_events = 0;
    bool saturated = false;
    bool xdrop_terminated = false;
    bool diagonal_certified = false;

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
        std::min<int32_t>(
            std::max<int32_t>(
                x, (std::numeric_limits<int16_t>::min)()),
            (std::numeric_limits<int16_t>::max)()));
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

    /// Build from a canonical logical-rank facade sub-region.
    template<bool is_reversed, bool write_reversed, int AlphabetDim,
             sequence_alphabet Alphabet>
    void fill(bioseq::storage::basic_lazy_sequence_view<Alphabet> seq,
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

// Certificate and construction-evidence records are target neutral.

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
         sequence_alphabet Alphabet, class D,
         size_t AlphabetDim = seqan3::alphabet_size<Alphabet> + 1u>
HWY_INLINE auto ba_place_block(
    D d,
    const ScoringMatrix<Alphabet>& matrix,
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
        const auto score_lut = load_score_lut<Alphabet>(matrix, d, c);

        // Origin can only land on j == 0 when can_have_origin is true.
        const bool col_can_have_origin = can_have_origin && (j == 0);

        for (size_t i = 0; i < height; i += L) {
            auto D10 = hn::Load(d, D_col + i);
            auto C10 = hn::Load(d, C_col + i);

            // Diagonal: shift D10 left by 1, borrow from D_corner
            auto D00 = lanes_shift_left_1_borrow(d, D10, D_corner);
            D_corner = D10;

            // Score lookup (LUT pre-loaded above; no per-cell row load)
            auto scores = get_scores_with_lut<Alphabet>(
                matrix, d, score_lut, query.ptr_at(start_i + i));

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
         sequence_alphabet Alphabet, class D,
         size_t AlphabetDim = seqan3::alphabet_size<Alphabet> + 1u>
void ba_align_core(
    D d,
    int32_t& out_score,
    size_t& out_query_idx,
    size_t& out_ref_idx,
    BlockAlignerScratch& alloc,
    const ScoringMatrix<Alphabet>& matrix,
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
                ba_place_block<TRACE, X_DROP, COLLECT_METRICS, Alphabet>(
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
                ba_place_block<TRACE, X_DROP, COLLECT_METRICS, Alphabet>(
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
                ba_place_block<TRACE, X_DROP, COLLECT_METRICS, Alphabet>(
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
                ba_place_block<TRACE, X_DROP, COLLECT_METRICS, Alphabet>(
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

struct BlockTracebackResult {
    std::vector<std::uint32_t> cigar;
    std::uint64_t edit_distance = 0;
};

/**
 * Account for the exact unit-cost edit represented by one traceback step.
 *
 * Traceback coordinates are one-based sequence positions because index zero
 * in each BlockPaddedBytes is the DP sentinel. Reversed left extensions need
 * no special handling: both padded inputs and the trace use the same reversed
 * orientation, and edit distance is invariant when both strings are reversed.
 */
template <bool compute_edit_distance>
HWY_INLINE void ba_record_traceback_edit(
    BlockTracebackResult& result,
    BlockCigarOp operation,
    size_t query_position,
    size_t reference_position,
    const BlockPaddedBytes& query,
    const BlockPaddedBytes& reference)
{
    if constexpr (!compute_edit_distance) {
        return;
    }
    switch (operation) {
        case BlockCigarOp::M:
            if (query_position == 0 || query_position > query.len()
                || reference_position == 0
                || reference_position > reference.len()) {
                block_aligner_invariant_failure(
                    "traceback diagonal edit coordinate exceeded input");
            }
            result.edit_distance +=
                query.get(query_position) != reference.get(reference_position);
            break;
        case BlockCigarOp::Eq:
            break;
        case BlockCigarOp::X:
        case BlockCigarOp::I:
        case BlockCigarOp::D:
            ++result.edit_distance;
            break;
        case BlockCigarOp::Sentinel:
            block_aligner_invariant_failure(
                "traceback emitted a sentinel operation");
    }
}

/**
 * Materialize the packed CIGAR and, when requested, derive exact edit
 * accounting during the same traceback walk over resident padded ranks.
 */
template <bool compute_edit_distance>
[[nodiscard]] BlockTracebackResult ba_cigar_core(
    const BlockTrace& trace,
    size_t end_i, size_t end_j,
    const BlockPaddedBytes& query,
    const BlockPaddedBytes& reference)
{
    BlockTracebackResult result;
    auto& cigar = result.cigar;
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
    const auto flush_pending = [&cigar, &pending_op, &pending_len]() {
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
    const auto add_op = [&flush_pending, &pending_op, &pending_len](
            BlockCigarOp op) {
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

                ba_record_traceback_edit<compute_edit_distance>(
                    result, entry.op, i, j, query, reference);
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

                ba_record_traceback_edit<compute_edit_distance>(
                    result, entry.op, i, j, query, reference);
                i -= entry.di;
                j -= entry.dj;
                table = entry.next_table;
                add_op(entry.op);
            }
        }
    }
    flush_pending();
    std::reverse(cigar.begin(), cigar.end());
    return result;
}

// =============================================================================
// Native Block Aligner parameter contract
// =============================================================================

// Parameter and validation contracts are target neutral.

template<sequence_alphabet Alphabet,
         size_t AlphabetDim = seqan3::alphabet_size<Alphabet> + 1u>
[[nodiscard]] bool ba_generated_diagonal_score_layout(
        const ScoringMatrix<Alphabet>& scoring_matrix,
        std::int8_t& match_score,
        std::int8_t& mismatch_score) noexcept {
    // AlphabetDim-1 is Block Aligner's NULL/padding sentinel.  The generated
    // matrix has one score for equal biological ranks, one for unequal ranks,
    // and one for any comparison involving the sentinel.
    if constexpr (AlphabetDim < 3) return false;
    constexpr size_t biological_ranks = AlphabetDim - 1;
    match_score = scoring_matrix.at(0, 0);
    mismatch_score = scoring_matrix.at(0, 1);
    const std::int8_t ambiguity_score = scoring_matrix.at(
        0, static_cast<int>(biological_ranks));
    if (match_score < 0) return false;
    for (size_t row = 0; row < AlphabetDim; ++row) {
        for (size_t col = 0; col < AlphabetDim; ++col) {
            const std::int8_t expected =
                row == biological_ranks || col == biological_ranks
                    ? ambiguity_score
                    : row == col ? match_score : mismatch_score;
            const std::int8_t observed = scoring_matrix.at(
                static_cast<int>(row), static_cast<int>(col));
            if (observed != expected || observed > match_score) return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr std::uint64_t ba_low_dna4_symbol_mask(
        size_t symbol_count) noexcept {
    return symbol_count == 32u
        ? (std::numeric_limits<std::uint64_t>::max)()
        : (std::uint64_t{1} << (symbol_count * 2u)) - 1u;
}

/// Load up to 32 LSB-first DNA4 ranks from an arbitrary base coordinate.
/// The second storage unit is touched only when the requested window crosses
/// the first unit boundary, so the final partial unit is never over-read.
[[nodiscard]] HWY_INLINE std::uint64_t ba_load_forward_dna4_window(
        const std::uint64_t* units,
        size_t bp_start,
        size_t symbol_count) noexcept {
    constexpr size_t symbols_per_unit = 32u;
    const size_t unit_index = bp_start / symbols_per_unit;
    const size_t symbol_offset = bp_start % symbols_per_unit;
    const size_t bit_offset = symbol_offset * 2u;
    std::uint64_t packed = units[unit_index] >> bit_offset;
    if (symbol_offset != 0u
        && symbol_offset + symbol_count > symbols_per_unit) {
        packed |= units[unit_index + 1u] << (64u - bit_offset);
    }
    return packed & ba_low_dna4_symbol_mask(symbol_count);
}

/// Reverse the order of 32 two-bit groups while retaining the bit order
/// within each rank.  DNA4 complement is a subsequent XOR with 0b11/rank.
[[nodiscard]] constexpr std::uint64_t ba_reverse_dna4_groups(
        std::uint64_t packed) noexcept {
    packed = ((packed & 0x0303030303030303ULL) << 6u)
        | ((packed & 0x0c0c0c0c0c0c0c0cULL) << 2u)
        | ((packed & 0x3030303030303030ULL) >> 2u)
        | ((packed & 0xc0c0c0c0c0c0c0c0ULL) >> 6u);
    return std::byteswap(packed);
}

template<bool is_reversed>
[[nodiscard]] HWY_INLINE std::uint64_t ba_load_oriented_dna4_window(
        const std::uint64_t* units,
        size_t total_symbols,
        size_t logical_start,
        size_t symbol_count) noexcept {
    if constexpr (!is_reversed) {
        return ba_load_forward_dna4_window(
            units, logical_start, symbol_count);
    } else {
        // Logical reverse-complement [start,start+n) maps to the descending
        // forward-domain window [total-start-n,total-start).
        const size_t source_high = total_symbols - logical_start;
        const size_t source_low = source_high - symbol_count;
        const std::uint64_t forward = ba_load_forward_dna4_window(
            units, source_low, symbol_count);
        const size_t inactive_symbols = 32u - symbol_count;
        const std::uint64_t reversed = ba_reverse_dna4_groups(forward)
            >> (inactive_symbols * 2u);
        return reversed ^ ba_low_dna4_symbol_mask(symbol_count);
    }
}

/// Prove that an equal-length Global alignment's coordinate diagonal is its
/// unique optimum.  This is deliberately a byte-and-score certificate, not a
/// seed/hash shortcut: the scan consumes the exact encoded bytes that the DP
/// would consume.
///
/// Let n be the common length, D the coordinate-diagonal score, M the largest
/// matrix entry, and q/e the public positive affine penalties.  Any different
/// global path must contain d >= 1 inserted and d deleted bases, hence at most
/// n-d diagonal pairs and at least one gap run in each direction.  Its score
/// is therefore bounded by
///
///   (n-d)M - 2q - 2ed <= (n-1)M - 2(q+e) = U.
///
/// Strict D > U proves both optimality and CIGAR uniqueness.  Equality is not
/// enough because it leaves the old traceback tie policy observable.
template<
    bool is_reversed,
    sequence_alphabet Alphabet,
    size_t AlphabetDim>
[[nodiscard]] HWY_INLINE BlockDiagonalCertificate
ba_try_global_diagonal_certificate(
        bioseq::storage::basic_lazy_sequence_view<Alphabet> query_seq,
        bioseq::storage::basic_lazy_sequence_view<Alphabet> target_seq,
        size_t query_start,
        size_t query_end,
        size_t target_start,
        size_t target_end,
        const ScoringMatrix<Alphabet>& scoring_matrix,
        const BlockAlignerParams& params) {
    BlockDiagonalCertificate result;
    const bool ordered_coordinates = query_end >= query_start
        && target_end >= target_start;
    const size_t n = ordered_coordinates ? query_end - query_start : 0u;
    const size_t target_len = ordered_coordinates
        ? target_end - target_start : 0u;
    if (params.mode != BlockAlignmentMode::Global
        || params.phase != BlockAlignerPhase::GapExtension
        || params.input_order != BlockInputOrder::Forward
        || params.gap_open <= 0 || params.gap_extend <= 0
        || !ordered_coordinates || n == 0 || target_len != n
        || query_end > query_seq.bp_size()
        || target_end > target_seq.bp_size()
        || n > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
        return result;
    }
    constexpr size_t packed_cigar_limit = static_cast<size_t>(
        std::numeric_limits<std::uint32_t>::max() >> 4u);
    if (params.trace && n > packed_cigar_limit) return result;

    // The SIMD scan below has one fast classification (equal vs unequal
    // biological ranks), so admit only the generated-matrix layout it can
    // score exactly. Logical encoded bytes must be below biological_ranks;
    // AlphabetDim-1 itself is the aligner's NULL/padding sentinel.
    if constexpr (AlphabetDim < 3) return result;
    constexpr size_t biological_ranks = AlphabetDim - 1;
    std::int8_t match_score = 0;
    std::int8_t mismatch_score = 0;
    if (!ba_generated_diagonal_score_layout<Alphabet, AlphabetDim>(
            scoring_matrix, match_score, mismatch_score)) {
        return result;
    }

    // Encode bounded chunks into stack storage in exactly the orientation the
    // DP would consume. Certified calls therefore never enter the align arena
    // or construct padded/scratch/trace owners. A rejected certificate pays
    // this small linear scan and then falls through to the unchanged DP path.
    constexpr size_t scan_chunk_symbols = 1024u;
    // Every byte subsequently inspected is overwritten by the encoder. Keep
    // these uninitialised: value-initialising 2 KiB here would replace the
    // padding memset we are deliberately removing with another per-call
    // memset of the same shape.
    alignas(64) std::array<std::uint8_t, scan_chunk_symbols> query_chunk;
    alignas(64) std::array<std::uint8_t, scan_chunk_symbols> target_chunk;
    const hn::Rebind<uint8_t, BlockAlignerSimdTag> d8;
    const size_t lanes = hn::Lanes(d8);
    const auto rank_limit = hn::Set(
        d8, static_cast<uint8_t>(biological_ranks));
    std::uint64_t equal_symbols = 0;
    for (size_t chunk_offset = 0; chunk_offset < n;
         chunk_offset += scan_chunk_symbols) {
        const size_t chunk_size = std::min(
            scan_chunk_symbols, n - chunk_offset);
        encode_region_via_ptr_at<is_reversed, false>(
            query_seq,
            std::span<std::uint8_t>{query_chunk.data(), chunk_size},
            query_start + chunk_offset,
            query_start + chunk_offset + chunk_size);
        encode_region_via_ptr_at<false, false>(
            target_seq,
            std::span<std::uint8_t>{target_chunk.data(), chunk_size},
            target_start + chunk_offset,
            target_start + chunk_offset + chunk_size);
        // Highway's masked load is permitted to issue a full hardware load
        // (notably on NEON). Initialise only the inactive bytes of the one
        // partial vector so that load never observes indeterminate storage.
        // Full 1024-symbol chunks are lane-aligned for every admitted Block
        // lane shape, so the hot bulk path performs no padding writes.
        const size_t tail_lanes = chunk_size % lanes;
        if (tail_lanes != 0) {
            const size_t tail_padding = lanes - tail_lanes;
            std::memset(
                query_chunk.data() + chunk_size, 0, tail_padding);
            std::memset(
                target_chunk.data() + chunk_size, 0, tail_padding);
        }
        for (size_t offset = 0; offset < chunk_size; offset += lanes) {
            const size_t active_lanes = std::min(
                lanes, chunk_size - offset);
            const auto active = hn::FirstN(d8, active_lanes);
            const auto query_bytes = hn::MaskedLoad(
                active, d8, query_chunk.data() + offset);
            const auto reference_bytes = hn::MaskedLoad(
                active, d8, target_chunk.data() + offset);
            const auto valid_ranks = hn::And(
                hn::Lt(query_bytes, rank_limit),
                hn::Lt(reference_bytes, rank_limit));
            if (hn::CountTrue(d8, hn::And(active, valid_ranks))
                != active_lanes) {
                return BlockDiagonalCertificate{};
            }
            equal_symbols += hn::CountTrue(
                d8, hn::And(active, hn::Eq(
                    query_bytes, reference_bytes)));
            ++result.vector_batches;
        }
    }

    const std::uint64_t unequal_symbols = static_cast<std::uint64_t>(n)
        - equal_symbols;
    const int64_t diagonal_score =
        static_cast<int64_t>(equal_symbols) * match_score
        + static_cast<int64_t>(unequal_symbols) * mismatch_score;
    const int64_t alternative_upper_bound =
        static_cast<int64_t>(n - 1) * match_score
        - 2 * (static_cast<int64_t>(params.gap_open)
            + static_cast<int64_t>(params.gap_extend));
    if (diagonal_score <= alternative_upper_bound
        || diagonal_score < std::numeric_limits<int32_t>::min()
        || diagonal_score > std::numeric_limits<int32_t>::max()) {
        return BlockDiagonalCertificate{};
    }

    result.certified = true;
    result.score = static_cast<int32_t>(diagonal_score);
    result.edit_distance = unequal_symbols;
    result.symbols = n;
    return result;
}

/// Attempt the resident packed DNA4/u64 certificate without materialising
/// byte ranks.  The three-way result is important to the caller:
///
///  * NotEligible: no packed content scan ran; preserve the current align()
///    route, including its byte certificate for fragmented/DNA5 inputs.
///  * Rejected: the exact packed content scan ran but could not prove the
///    diagonal; enter forced-old DP so the same gap is not scanned twice.
///  * Certified: caller may commit the exact score/endpoints/M run and record
///    one logical Block execution through record_certified_global_gap().
template<
    bool is_reversed,
    sequence_alphabet Alphabet>
[[nodiscard]] HWY_INLINE BlockPackedCertificateAttempt
ba_try_packed_global_gap_certificate(
        bioseq::storage::basic_lazy_sequence_view<Alphabet> query_seq,
        bioseq::storage::basic_lazy_sequence_view<Alphabet> target_seq,
        size_t query_start,
        size_t query_end,
        size_t target_start,
        size_t target_end,
        const BlockGlobalGapCertificatePlan& plan) noexcept {
    BlockPackedCertificateAttempt attempt;
    if constexpr (!std::same_as<Alphabet, seq::dna4>) {
        return attempt;
    } else {
        const bool ordered_coordinates = query_end >= query_start
            && target_end >= target_start;
        const size_t query_len = ordered_coordinates
            ? query_end - query_start : 0u;
        const size_t target_len = ordered_coordinates
            ? target_end - target_start : 0u;
        constexpr size_t packed_cigar_limit = static_cast<size_t>(
            std::numeric_limits<std::uint32_t>::max() >> 4u);
        if (!plan.contract_supported
            || !plan.generated_matrix_supported
            || plan.params.mode != BlockAlignmentMode::Global
            || plan.params.phase != BlockAlignerPhase::GapExtension
            || plan.params.input_order != BlockInputOrder::Forward
            || !ordered_coordinates || query_len == 0u
            || target_len != query_len
            || query_end > query_seq.bp_size()
            || target_end > target_seq.bp_size()
            || query_len > static_cast<size_t>(
                std::numeric_limits<std::int32_t>::max())
            || (plan.params.trace && query_len > packed_cigar_limit)) {
            return attempt;
        }

        // Borrow the complete logical facade descriptors.  Region-only probes
        // lose the logical totals required by reverse orientation and expose
        // nonzero leading offsets for ordinary unaligned slices.  The packed
        // loader already handles arbitrary symbol coordinates, so retain each
        // descriptor's physical leading offset explicitly.
        const auto query_region = query_seq.try_resident_packed(
            0u, query_seq.bp_size(), 8u);
        const auto target_region = target_seq.try_resident_packed(
            0u, target_seq.bp_size(), 8u);
        if (!query_region.has_value() || !target_region.has_value()
            || query_seq.bp_size()
                > static_cast<std::uint64_t>(
                    (std::numeric_limits<size_t>::max)())
            || target_seq.bp_size()
                > static_cast<std::uint64_t>(
                    (std::numeric_limits<size_t>::max)())) {
            return attempt;
        }
        const auto* query_units =
            static_cast<const std::uint64_t*>(query_region->units);
        const auto* target_units =
            static_cast<const std::uint64_t*>(target_region->units);
        const size_t query_logical_total =
            static_cast<size_t>(query_seq.bp_size());
        const size_t target_logical_total =
            static_cast<size_t>(target_seq.bp_size());
        const size_t query_leading = query_region->leading_symbol_offset;
        const size_t target_leading = target_region->leading_symbol_offset;
        if (query_logical_total
                > (std::numeric_limits<size_t>::max)() - query_leading
            || target_logical_total
                > (std::numeric_limits<size_t>::max)() - target_leading) {
            return attempt;
        }
        const size_t query_physical_total =
            query_leading + query_logical_total;

        // Extraction handles arbitrary base offsets one word at a time.  The
        // staging arrays are completely initialized before each masked load;
        // Highway then collapses and popcounts two-bit mismatches across the
        // native packed-word lanes.
        const hn::Repartition<std::uint64_t, BlockAlignerSimdTag> d64;
        const size_t lanes = hn::Lanes(d64);
        constexpr size_t max_packed_lanes = 4u;
        alignas(64) std::array<std::uint64_t, max_packed_lanes> query_words{};
        alignas(64) std::array<std::uint64_t, max_packed_lanes> target_words{};
        const auto pair_low_bits = hn::Set(d64, 0x5555555555555555ULL);
        const size_t word_count = ba_div_ceil(query_len, 32u);
        std::uint64_t mismatch_symbols = 0;
        for (size_t first_word = 0; first_word < word_count;
             first_word += lanes) {
            const size_t active_lanes = std::min(lanes,
                word_count - first_word);
            for (size_t lane = 0; lane < active_lanes; ++lane) {
                const size_t symbol_offset = (first_word + lane) * 32u;
                const size_t symbol_count = std::min(
                    size_t{32}, query_len - symbol_offset);
                const size_t query_logical_start =
                    query_start + symbol_offset;
                query_words[lane] = ba_load_oriented_dna4_window<is_reversed>(
                    query_units,
                    query_physical_total,
                    is_reversed
                        ? query_logical_start
                        : query_leading + query_logical_start,
                    symbol_count);
                target_words[lane] = ba_load_oriented_dna4_window<false>(
                    target_units, target_leading + target_logical_total,
                    target_leading + target_start + symbol_offset,
                    symbol_count);
            }
            const auto active = hn::FirstN(d64, active_lanes);
            const auto query_packed = hn::MaskedLoad(
                active, d64, query_words.data());
            const auto target_packed = hn::MaskedLoad(
                active, d64, target_words.data());
            const auto different_bits = hn::Xor(query_packed, target_packed);
            const auto different_symbols = hn::And(
                hn::Or(different_bits, hn::ShiftRight<1>(different_bits)),
                pair_low_bits);
            const auto per_lane_mismatches = hn::PopulationCount(
                different_symbols);
            mismatch_symbols += hn::ReduceSum(
                d64, hn::IfThenElseZero(active, per_lane_mismatches));
            ++attempt.certificate.vector_batches;
        }

        attempt.decision = BlockPackedCertificateDecision::Rejected;
        const std::uint64_t equal_symbols =
            static_cast<std::uint64_t>(query_len) - mismatch_symbols;
        const std::int64_t diagonal_score =
            static_cast<std::int64_t>(equal_symbols) * plan.match_score
            + static_cast<std::int64_t>(mismatch_symbols)
                * plan.mismatch_score;
        const std::int64_t alternative_upper_bound =
            static_cast<std::int64_t>(query_len - 1u) * plan.match_score
            - 2 * (static_cast<std::int64_t>(plan.params.gap_open)
                + static_cast<std::int64_t>(plan.params.gap_extend));
        if (diagonal_score <= alternative_upper_bound
            || diagonal_score < std::numeric_limits<std::int32_t>::min()
            || diagonal_score > std::numeric_limits<std::int32_t>::max()) {
            return attempt;
        }

        attempt.decision = BlockPackedCertificateDecision::Certified;
        attempt.certificate.certified = true;
        attempt.certificate.score = static_cast<std::int32_t>(diagonal_score);
        attempt.certificate.edit_distance = mismatch_symbols;
        attempt.certificate.symbols = query_len;
        return attempt;
    }
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
        const bool simd_dp_executed = metrics.vector_batches != 0
            && metrics.cells != 0;
        const bool simd_certificate_executed = metrics.diagonal_certified
            && metrics.diagonal_certificate_vector_batches != 0
            && metrics.diagonal_certificate_symbols != 0;
        if ((simd_dp_executed || simd_certificate_executed)
            && !metrics.saturated) {
            proof.add(proof.nonempty_calls_with_simd);
        }
    }
    proof.add(proof.simd_dp_vector_batches, metrics.vector_batches);
    proof.add(proof.simd_dp_cells, metrics.cells);
    if (metrics.diagonal_certified) {
        proof.add(proof.simd_diagonal_certificate_calls);
    }
    proof.add(proof.simd_diagonal_certificate_vector_batches,
        metrics.diagonal_certificate_vector_batches);
    proof.add(proof.simd_diagonal_certificate_symbols,
        metrics.diagonal_certificate_symbols);
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
    template<
        sequence_alphabet Alphabet,
        size_t AlphabetDim = seqan3::alphabet_size<Alphabet> + 1u>
    [[nodiscard]] static BlockGlobalGapCertificatePlan
    prepare_global_gap_certificate(
            const ScoringMatrix<Alphabet>& scoring_matrix,
            const BlockAlignerParams& params) noexcept {
        BlockGlobalGapCertificatePlan plan{
            .params = params,
            .scoring_identity = scoring_matrix.block_semantic_identity(),
        };
        const size_t lanes = hn::Lanes(BlockAlignerSimdTag{});
        plan.simd_lanes = static_cast<std::uint8_t>(lanes);
        plan.contract_supported = scoring_matrix.block_layout_valid()
            && validate_block_aligner_contract(params, lanes)
                == BlockAlignerContractError::None;
        plan.generated_matrix_supported =
            ba_generated_diagonal_score_layout<Alphabet, AlphabetDim>(
                scoring_matrix, plan.match_score, plan.mismatch_score);
        return plan;
    }

    template<
        bool is_reversed,
        sequence_alphabet Alphabet>
    [[nodiscard]] static HWY_INLINE BlockPackedCertificateAttempt
    try_packed_global_gap(
            bioseq::storage::basic_lazy_sequence_view<Alphabet> query_seq,
            bioseq::storage::basic_lazy_sequence_view<Alphabet> target_seq,
            size_t query_start,
            size_t query_end,
            size_t target_start,
            size_t target_end,
            const BlockGlobalGapCertificatePlan& plan) noexcept {
        return ba_try_packed_global_gap_certificate<is_reversed>(
            query_seq, target_seq,
            query_start, query_end, target_start, target_end, plan);
    }

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
        bool enable_diagonal_certificate = true,
        sequence_alphabet Alphabet,
        size_t AlphabetDim = seqan3::alphabet_size<Alphabet> + 1u>
    [[nodiscard]] static KswAlignResult align(
        bioseq::storage::basic_lazy_sequence_view<Alphabet> query_seq,
        bioseq::storage::basic_lazy_sequence_view<Alphabet> target_seq,
        size_t query_start,
        size_t query_end,
        size_t target_start,
        size_t target_end,
        const ScoringMatrix<Alphabet>& scoring_matrix,
        const BlockAlignerParams& params,
        BlockAlignerExecutionProof* execution_proof = nullptr,
        BlockAlignerConstructionEvidence* construction_evidence = nullptr)
    {
        static_assert(
            AlphabetDim
                == seqan3::alphabet_size<Alphabet> + 1u,
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
        const std::uint64_t config_identity =
            ::bioseq::simd::block_aligner_config_identity(
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
            if (params.compute_edit_distance) {
                result.edit_distance = std::max(query_len, target_len);
                result.edit_distance_valid = true;
            }
            return result;
        }

        BlockCoreMetrics metrics;
        if constexpr (enable_diagonal_certificate) {
            const BlockDiagonalCertificate certificate =
                ba_try_global_diagonal_certificate<
                    is_reversed, Alphabet, AlphabetDim>(
                        query_seq, target_seq,
                        query_start, query_end, target_start, target_end,
                        scoring_matrix, params);
            if (certificate.certified) {
                result.score = certificate.score;
                result.max = certificate.score;
                result.max_q = static_cast<int32_t>(query_len) - 1;
                result.max_t = static_cast<int32_t>(target_len) - 1;
                result.mqe = certificate.score;
                result.mqe_t = result.max_t;
                result.mte = certificate.score;
                result.mte_q = result.max_q;
                result.reach_end = true;
                if (params.trace) {
                    result.cigar.push_back(
                        (static_cast<std::uint32_t>(query_len) << 4u)
                        | CIGAR_MATCH);
                }
                if (params.compute_edit_distance) {
                    result.edit_distance = certificate.edit_distance;
                    result.edit_distance_valid = true;
                }
                metrics.diagonal_certified = true;
                metrics.diagonal_certificate_vector_batches =
                    certificate.vector_batches;
                metrics.diagonal_certificate_symbols = certificate.symbols;
                if (execution_proof != nullptr) {
                    block_aligner_record_executed(
                        *execution_proof, params, L, false, metrics,
                        config_identity);
                }
                return result;
            }
        }

        // Every arena-backed buffer below (BlockPaddedBytes, BlockAlignedBuf,
        // BlockTrace, scratch encoded-sequence spans) dies here. The CIGAR
        // output lives on result's own vector so it survives the unwind.
        if (construction_evidence != nullptr) {
            ++construction_evidence->align_arena_scopes;
        }
        auto align_scope = context::current().push<context::Arena::Align>();

        const size_t min_size = std::max(params.min_size, L);
        const size_t max_size = std::max(params.max_size, L);

        // Convert positive q/e into the reference's negative gap costs.
        const int8_t gap_open_neg = static_cast<int8_t>(
            -(static_cast<int>(params.gap_open) + params.gap_extend));
        const int8_t gap_extend_neg = static_cast<int8_t>(-params.gap_extend);

        BlockPaddedBytes query_padded, ref_padded;
        if (construction_evidence != nullptr) {
            construction_evidence->padded_sequence_allocations += 2u;
        }
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
        if (construction_evidence != nullptr) {
            ++construction_evidence->scratch_allocations;
            construction_evidence->trace_allocations += do_trace ? 1u : 0u;
        }
        BlockAlignerScratch scratch(query_len, target_len, max_size, do_trace);
        // The reference clears the full maximum border/checkpoint geometry.
        scratch.clear(d, query_len, target_len, max_size, do_trace);

        int32_t score = 0;
        size_t out_qi = 0, out_ri = 0;

        const bool use_xdrop = params.mode == BlockAlignmentMode::XDrop;
        // Dispatch based on TRACE and X_DROP combinations
        if (do_trace && use_xdrop) {
            if (execution_proof != nullptr) {
                ba_align_core<true, true, true, Alphabet>(
                    d, score, out_qi, out_ri, scratch,
                    scoring_matrix, gap_open_neg, gap_extend_neg,
                    query_padded, ref_padded, min_size, max_size,
                    params.x_drop, metrics);
            } else {
                ba_align_core<true, true, false, Alphabet>(
                    d, score, out_qi, out_ri, scratch,
                    scoring_matrix, gap_open_neg, gap_extend_neg,
                    query_padded, ref_padded, min_size, max_size,
                    params.x_drop, metrics);
            }
        } else if (do_trace && !use_xdrop) {
            if (execution_proof != nullptr) {
                ba_align_core<true, false, true, Alphabet>(
                    d, score, out_qi, out_ri, scratch,
                    scoring_matrix, gap_open_neg, gap_extend_neg,
                    query_padded, ref_padded, min_size, max_size, 0, metrics);
            } else {
                ba_align_core<true, false, false, Alphabet>(
                    d, score, out_qi, out_ri, scratch,
                    scoring_matrix, gap_open_neg, gap_extend_neg,
                    query_padded, ref_padded, min_size, max_size, 0, metrics);
            }
        } else if (!do_trace && use_xdrop) {
            if (execution_proof != nullptr) {
                ba_align_core<false, true, true, Alphabet>(
                    d, score, out_qi, out_ri, scratch,
                    scoring_matrix, gap_open_neg, gap_extend_neg,
                    query_padded, ref_padded, min_size, max_size,
                    params.x_drop, metrics);
            } else {
                ba_align_core<false, true, false, Alphabet>(
                    d, score, out_qi, out_ri, scratch,
                    scoring_matrix, gap_open_neg, gap_extend_neg,
                    query_padded, ref_padded, min_size, max_size,
                    params.x_drop, metrics);
            }
        } else {
            if (execution_proof != nullptr) {
                ba_align_core<false, false, true, Alphabet>(
                    d, score, out_qi, out_ri, scratch,
                    scoring_matrix, gap_open_neg, gap_extend_neg,
                    query_padded, ref_padded, min_size, max_size, 0, metrics);
            } else {
                ba_align_core<false, false, false, Alphabet>(
                    d, score, out_qi, out_ri, scratch,
                    scoring_matrix, gap_open_neg, gap_extend_neg,
                    query_padded, ref_padded, min_size, max_size, 0, metrics);
            }
        }

        // Build CIGAR via traceback
        if (do_trace) {
            BlockTracebackResult traceback_result =
                params.compute_edit_distance
                ? ba_cigar_core<true>(
                    scratch.trace, out_qi, out_ri,
                    query_padded, ref_padded)
                : ba_cigar_core<false>(
                    scratch.trace, out_qi, out_ri,
                    query_padded, ref_padded);
            result.cigar = std::move(traceback_result.cigar);
            if (params.input_order == BlockInputOrder::Reversed) {
                std::reverse(result.cigar.begin(), result.cigar.end());
            }
            if (params.compute_edit_distance) {
                result.edit_distance = traceback_result.edit_distance;
                result.edit_distance_valid = true;
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

template<sequence_alphabet Alphabet>
[[nodiscard]] BlockGlobalGapCertificatePlan block_prepare(
    const ScoringMatrix<Alphabet>& scoring_matrix,
    const BlockAlignerParams& params) noexcept
{
    return HwyBlockAligner::prepare_global_gap_certificate<Alphabet>(
        scoring_matrix, params);
}

template<sequence_alphabet Alphabet>
[[nodiscard]] BlockPackedCertificateAttempt block_try_certified_gap(
    bool reversed,
    bioseq::storage::basic_lazy_sequence_view<Alphabet> query,
    bioseq::storage::basic_lazy_sequence_view<Alphabet> target_sequence,
    size_t query_start,
    size_t query_end,
    size_t target_start,
    size_t target_end,
    const BlockGlobalGapCertificatePlan& plan) noexcept
{
    return reversed
        ? HwyBlockAligner::try_packed_global_gap<true>(
            query, target_sequence, query_start, query_end,
            target_start, target_end, plan)
        : HwyBlockAligner::try_packed_global_gap<false>(
            query, target_sequence, query_start, query_end,
            target_start, target_end, plan);
}

template<sequence_alphabet Alphabet>
[[nodiscard]] KswAlignResult block_align(
    bool reversed,
    bool enable_diagonal_certificate,
    bioseq::storage::basic_lazy_sequence_view<Alphabet> query,
    bioseq::storage::basic_lazy_sequence_view<Alphabet> target_sequence,
    size_t query_start,
    size_t query_end,
    size_t target_start,
    size_t target_end,
    const ScoringMatrix<Alphabet>& scoring_matrix,
    const BlockAlignerParams& params,
    BlockAlignerExecutionProof* execution_proof,
    BlockAlignerConstructionEvidence* construction_evidence)
{
    if (reversed) {
        return enable_diagonal_certificate
            ? HwyBlockAligner::align<true, true>(
                query, target_sequence, query_start, query_end,
                target_start, target_end, scoring_matrix, params,
                execution_proof, construction_evidence)
            : HwyBlockAligner::align<true, false>(
                query, target_sequence, query_start, query_end,
                target_start, target_end, scoring_matrix, params,
                execution_proof, construction_evidence);
    }
    return enable_diagonal_certificate
        ? HwyBlockAligner::align<false, true>(
            query, target_sequence, query_start, query_end,
            target_start, target_end, scoring_matrix, params,
            execution_proof, construction_evidence)
        : HwyBlockAligner::align<false, false>(
            query, target_sequence, query_start, query_end,
            target_start, target_end, scoring_matrix, params,
            execution_proof, construction_evidence);
}

}  // namespace target
}  // namespace bioseq::simd
