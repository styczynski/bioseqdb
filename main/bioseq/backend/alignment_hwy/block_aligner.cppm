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
 * Naming follows existing KSW2 conventions:
 *   - scoring_matrix, gap_open, gap_extend (uniform with ksw2.cppm)
 *   - compact_sequence_range as public input (no PaddedBytes in API)
 *   - All SIMD helpers templated on tag D (no global type aliases)
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
#include <type_traits>
#include <tuple>
#include <string>

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
    template<bool is_reversed, int AlphabetDim,
             supports_compact_sequence_range R, class Scanner>
    void fill(const bioseq::storage::lazy_compact_sequence<R, Scanner>& seq,
              size_t bp_from, size_t bp_to, size_t block_size) {
        len_ = bp_to - bp_from;
        const size_t total = 1 + len_ + block_size;
        data_ = context::current().alloc_align_array<uint8_t, 64>(total + 64);
        const uint8_t null_byte = static_cast<uint8_t>(AlphabetDim);
        data_[0] = null_byte;
        encode_region_via_ptr_at<is_reversed>(
            seq, data_.subspan(1, len_), bp_from, bp_to);
        std::memset(data_.data() + 1 + len_, null_byte, block_size);
    }

    /// Build from pre-encoded bytes (already rank values).
    void fill_from_encoded(const uint8_t* bytes, size_t len,
                           size_t block_size, uint8_t null_byte) {
        len_ = len;
        const size_t total = 1 + len_ + block_size;
        data_ = context::current().alloc_align_array<uint8_t, 64>(total + 64);
        data_[0] = null_byte;
        std::memcpy(data_.data() + 1, bytes, len_);
        std::memset(data_.data() + 1 + len_, null_byte, block_size);
    }

    /// Build reversed from pre-encoded bytes.
    void fill_from_encoded_reversed(const uint8_t* bytes, size_t len,
                                    size_t block_size, uint8_t null_byte) {
        len_ = len;
        const size_t total = 1 + len_ + block_size;
        data_ = context::current().alloc_align_array<uint8_t, 64>(total + 64);
        data_[0] = null_byte;
        for (size_t i = 0; i < len_; ++i) {
            data_[1 + i] = bytes[len_ - 1 - i];
        }
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
        const size_t L = hn::Lanes(hn::ScalableTag<int16_t>{});
        const size_t trace_cap = (max_size / L) * (len + max_size * 2) + 64;
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
        if (trace_idx_ < trace_.size()) {
            trace_[trace_idx_] = t;
            trace2_[trace_idx_] = t2;
        }
        ++trace_idx_;
    }

    void add_block(size_t i, size_t j, size_t width, size_t height, bool right) {
        if (block_idx_ * 2 + 1 < block_start_.size()) {
            block_start_[block_idx_ * 2]     = static_cast<uint32_t>(i);
            block_start_[block_idx_ * 2 + 1] = static_cast<uint32_t>(j);
            block_size_[block_idx_ * 2]      = static_cast<uint16_t>(height);
            block_size_[block_idx_ * 2 + 1]  = static_cast<uint16_t>(width);
            const size_t word = block_idx_ / 64;
            const size_t bit  = block_idx_ % 64;
            if (word < right_.size()) {
                right_[word] = (right_[word] & ~(1ULL << bit))
                             | (static_cast<uint64_t>(right) << bit);
            }
        }
        ++block_idx_;
    }

    void add_trace_idx(size_t add) { trace_idx_ += add; }
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
    void clear(D d, size_t qlen, size_t rlen, size_t init_block_sz, bool do_trace) {
        // Only the first `init_block_sz` cells of each border buffer are
        // ever read before being written by the DP. Subsequent positions
        // are overwritten by `ba_place_block` on the first column /
        // direction transition. Zeroing only the initial slice instead
        // of the full `max_sz` cuts the per-call clear cost from
        // O(8 * max_sz) to O(4 * init_block_sz) — typically a 16x
        // reduction (max_sz=1024, init_block_sz=64). The ckpt buffers
        // are never read until the first checkpoint save copies into
        // them, so they don't need pre-clearing at all.
        if (do_trace) trace.clear(qlen, rlen);
        D_col.clear(d, init_block_sz); C_col.clear(d, init_block_sz);
        D_row.clear(d, init_block_sz); R_row.clear(d, init_block_sz);
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
template<bool TRACE, bool X_DROP, supports_compact_sequence_range R, class D,
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
    bool right
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

template<bool TRACE, bool X_DROP, supports_compact_sequence_range R, class D,
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
    int32_t x_drop_threshold)
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
                ba_place_block<TRACE, X_DROP, R>(
                    d, matrix, gap_open_neg, gap_extend_neg,
                    query, reference, alloc.trace,
                    state_i, state_j + block_size - BA_STEP,
                    BA_STEP, block_size,
                    alloc.D_col.data(), alloc.C_col.data(),
                    alloc.temp_buf1.data(), alloc.temp_buf2.data(),
                    d_corner_arg, ba_clamp_i16(-off + BA_ZERO), true);

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
                ba_place_block<TRACE, X_DROP, R>(
                    d, matrix, gap_open_neg, gap_extend_neg,
                    reference, query, alloc.trace,
                    state_j, state_i + block_size - BA_STEP,
                    BA_STEP, block_size,
                    alloc.D_row.data(), alloc.R_row.data(),
                    alloc.temp_buf1.data(), alloc.temp_buf2.data(),
                    d_corner_arg, ba_clamp_i16(-off + BA_ZERO), false);

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
                ba_place_block<TRACE, X_DROP, R>(
                    d, matrix, gap_open_neg, gap_extend_neg,
                    reference, query, alloc.trace,
                    state_j, state_i + prev_size,
                    grow_step, prev_size,
                    alloc.D_row.data(), alloc.R_row.data(),
                    alloc.D_col.data() + prev_size,
                    alloc.C_col.data() + prev_size,
                    hn::Set(d, BA_MIN), ba_clamp_i16(-off + BA_ZERO), false);

            if constexpr (TRACE) {
                alloc.trace.add_block(state_i, state_j + prev_size,
                    grow_step, block_size, true);
            }

            // Grow RIGHT: grow_step cols x block_size rows
            hn::Vec<D> D_max2, ai2, aj2;
            std::tie(D_max2, ai2, aj2) =
                ba_place_block<TRACE, X_DROP, R>(
                    d, matrix, gap_open_neg, gap_extend_neg,
                    query, reference, alloc.trace,
                    state_i, state_j + prev_size,
                    grow_step, block_size,
                    alloc.D_col.data(), alloc.C_col.data(),
                    alloc.D_row.data() + prev_size,
                    alloc.R_row.data() + prev_size,
                    hn::Set(d, BA_MIN), ba_clamp_i16(-off + BA_ZERO), true);

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
                    best_argmax_i = state_i + static_cast<size_t>(arg_i) + arg_lane;
                    best_argmax_j = state_j + prev_size + static_cast<size_t>(arg_j);
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
                if (x_drop_iter >= BA_X_DROP_ITER_COUNT) break;
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
            size_t idx = query.len() - state_i;
            if (idx < block_size) final_cell = alloc.D_col.get(idx);
        } else {
            size_t idx = reference.len() - state_j;
            if (idx < block_size) final_cell = alloc.D_row.get(idx);
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
    const BlockPaddedBytes& query,
    const BlockPaddedBytes& reference,
    size_t end_i, size_t end_j,
    BlockCigar& cigar)
{
    cigar.clear();
    const size_t L = hn::Lanes(hn::ScalableTag<int16_t>{});

    size_t block_idx = trace.block_idx();
    size_t trace_idx = trace.trace_idx();
    auto table = BlockTraceTable::D;

    size_t i = end_i, j = end_j;

    while (i > 0 || j > 0) {
        // Find the block containing (i, j)
        size_t block_i, block_j, block_height, block_width;
        bool right;
        for (;;) {
            if (block_idx == 0) goto done;
            block_idx -= 1;
            block_i      = trace.block_start_data()[block_idx * 2];
            block_j      = trace.block_start_data()[block_idx * 2 + 1];
            block_height = trace.block_size_data()[block_idx * 2];
            block_width  = trace.block_size_data()[block_idx * 2 + 1];
            trace_idx -= block_width * block_height / L;

            if (i >= block_i && j >= block_j) {
                const size_t word = block_idx / 64;
                const size_t bit  = block_idx % 64;
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

                if (idx >= trace.trace_data().size()) goto done;

                uint32_t t  = (trace.trace_data()[idx]  >> ((curr_i % L) * 2)) & 0b11;
                uint32_t t2 = (trace.trace2_data()[idx] >> ((curr_i % L) * 2)) & 0b11;

                size_t lut_idx = (t << 4) | (t2 << 2)
                               | static_cast<size_t>(table);
                const auto& entry = lut[lut_idx];

                auto op = entry.op;
                // Distinguish match vs mismatch
                if (op == BlockCigarOp::M) {
                    op = (query.get(i) == reference.get(j))
                       ? BlockCigarOp::Eq : BlockCigarOp::X;
                }

                i -= entry.di;
                j -= entry.dj;
                table = entry.next_table;
                cigar.add(op);
            }
        } else {
            // Down block: vectors horizontal (width/L per row)
            while (i >= block_i && j >= block_j && (i > 0 || j > 0)) {
                size_t curr_i = i - block_i;
                size_t curr_j = j - block_j;
                size_t idx = trace_idx + curr_j / L + curr_i * (block_width / L);

                if (idx >= trace.trace_data().size()) goto done;

                uint32_t t  = (trace.trace_data()[idx]  >> ((curr_j % L) * 2)) & 0b11;
                uint32_t t2 = (trace.trace2_data()[idx] >> ((curr_j % L) * 2)) & 0b11;

                size_t lut_idx = (t << 4) | (t2 << 2)
                               | static_cast<size_t>(table);
                const auto& entry = lut[lut_idx];

                auto op = entry.op;
                if (op == BlockCigarOp::M) {
                    op = (query.get(i) == reference.get(j))
                       ? BlockCigarOp::Eq : BlockCigarOp::X;
                }

                i -= entry.di;
                j -= entry.dj;
                table = entry.next_table;
                cigar.add(op);
            }
        }
    }
done:
    cigar.reverse();
}

// =============================================================================
// Parameters — uniform naming with KSW2
// =============================================================================

struct BlockAlignerParams {
    int8_t  gap_open     = 0;   ///< Gap open cost (positive, KSW2 convention)
    int8_t  gap_extend   = 0;   ///< Gap extend cost (positive, KSW2 convention)
    size_t  min_size     = 32;  ///< Minimum block size (power of 2)
    size_t  max_size     = 256; ///< Maximum block size (power of 2)
    int32_t x_drop       = -1;  ///< X-drop threshold (-1 = disabled)
    KswFlags flags       = {};
};

// =============================================================================
// HwyBlockAligner — public API
// =============================================================================

class HwyBlockAligner {
public:
    /// Run block aligner on a sub-region of the given sequences.
    /// Returns KswAlignResult for compatibility with KSW2 pipeline.
    ///
    /// Gap convention: gap_open/gap_extend in params are positive (KSW2 style).
    /// Internally converted to negative (block aligner style):
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
        const BlockAlignerParams& params)
    {
        KswAlignResult result;

        const size_t query_len  = query_end - query_start;
        const size_t target_len = target_end - target_start;
        if (query_len == 0 || target_len == 0) return result;

        // Every arena-backed buffer below (BlockPaddedBytes, BlockAlignedBuf,
        // BlockTrace, scratch encoded-sequence spans) dies here. The CIGAR
        // output lives on result's own vector so it survives the unwind.
        auto align_scope = context::current().push<context::Arena::Align>();

        const hn::ScalableTag<int16_t> d;
        const size_t L = hn::Lanes(d);

        // Round block sizes up to next power of 2, at least L
        auto next_pow2 = [](size_t v) -> size_t {
            if (v == 0) return 1;
            v--;
            v |= v >> 1; v |= v >> 2; v |= v >> 4;
            v |= v >> 8; v |= v >> 16; v |= v >> 32;
            return v + 1;
        };
        size_t min_size = next_pow2(std::max(params.min_size, L));
        size_t max_size = next_pow2(std::max(params.max_size, min_size));

        // Convert gap penalties: KSW2 (positive) -> block aligner (negative)
        const int8_t gap_open_neg = static_cast<int8_t>(
            -(static_cast<int>(params.gap_open) + params.gap_extend));
        const int8_t gap_extend_neg = static_cast<int8_t>(-params.gap_extend);

        const uint8_t null_byte = ScoringMatrix<QueryRange>::NULL_BYTE;

        BlockPaddedBytes query_padded, ref_padded;
        if (params.flags.reverse_cigar) {
            // Left-extension path. We still need the temp encoded
            // buffers because fill_from_encoded_reversed scalar-walks
            // them in reverse — there is no encode-and-reverse-in-one
            // primitive yet.
            auto& enc_ctx = context::current();
            auto query_enc  = enc_ctx.alloc_align_array<uint8_t>(query_len);
            auto target_enc = enc_ctx.alloc_align_array<uint8_t>(target_len);
            encode_region_via_ptr_at<is_reversed>(
                query_seq, query_enc, query_start, query_end);
            encode_region_via_ptr_at<false>(
                target_seq, target_enc, target_start, target_end);
            query_padded.fill_from_encoded_reversed(
                query_enc.data(), query_len, max_size, null_byte);
            ref_padded.fill_from_encoded_reversed(
                target_enc.data(), target_len, max_size, null_byte);
        } else {
            // Hot path (gap-fill): encode the sub-region directly into
            // the padded buffer. Saves one arena allocation and one
            // memcpy per query/target compared to the previous
            // encode-then-pad pipeline.
            query_padded.template fill<is_reversed, AlphabetDim>(
                query_seq, query_start, query_end, max_size);
            ref_padded.template fill<false, AlphabetDim>(
                target_seq, target_start, target_end, max_size);
        }

        // Allocate scratch space
        const bool do_trace = !params.flags.score_only;
        BlockAlignerScratch scratch(query_len, target_len, max_size, do_trace);
        // Pass min_size — only the first block_size cells of each
        // border buffer are read before being written by the DP.
        scratch.clear(d, query_len, target_len, min_size, do_trace);

        int32_t score = 0;
        size_t out_qi = 0, out_ri = 0;

        const bool use_xdrop = (params.x_drop >= 0);

        // Dispatch based on TRACE and X_DROP combinations
        if (do_trace && use_xdrop) {
            ba_align_core<true, true, QueryRange>(
                d, score, out_qi, out_ri, scratch,
                scoring_matrix, gap_open_neg, gap_extend_neg,
                query_padded, ref_padded, min_size, max_size,
                params.x_drop);
        } else if (do_trace && !use_xdrop) {
            ba_align_core<true, false, QueryRange>(
                d, score, out_qi, out_ri, scratch,
                scoring_matrix, gap_open_neg, gap_extend_neg,
                query_padded, ref_padded, min_size, max_size, 0);
        } else if (!do_trace && use_xdrop) {
            ba_align_core<false, true, QueryRange>(
                d, score, out_qi, out_ri, scratch,
                scoring_matrix, gap_open_neg, gap_extend_neg,
                query_padded, ref_padded, min_size, max_size,
                params.x_drop);
        } else {
            ba_align_core<false, false, QueryRange>(
                d, score, out_qi, out_ri, scratch,
                scoring_matrix, gap_open_neg, gap_extend_neg,
                query_padded, ref_padded, min_size, max_size, 0);
        }

        // Build CIGAR via traceback
        if (do_trace) {
            BlockCigar cigar(query_len + target_len);
            ba_cigar_core(scratch.trace, query_padded, ref_padded,
                          out_qi, out_ri, cigar);
            result.cigar = cigar.to_packed_cigar();
            if (params.flags.reverse_cigar) {
                std::reverse(result.cigar.begin(), result.cigar.end());
            }
        }

        // Fill KswAlignResult fields
        result.score = score;
        result.max = score;
        result.max_q = static_cast<int32_t>(out_qi) - 1;
        result.max_t = static_cast<int32_t>(out_ri) - 1;

        if (out_qi >= query_len) {
            result.mqe = score;
            result.mqe_t = static_cast<int32_t>(out_ri) - 1;
            result.reach_end = (out_ri >= target_len);
        }
        if (out_ri >= target_len) {
            result.mte = score;
            result.mte_q = static_cast<int32_t>(out_qi) - 1;
        }

        return result;
    }
};

}  // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();

}  // export
