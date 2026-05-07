module;

#include <hwy/highway.h>
#include <hwy/aligned_allocator.h>

#include "common.h"

#include <array>
#include <ranges>

export module bioseq_sequences.kmers.two_stacks;

import mlogging;
import context;
import bioseq_sequences;
import bioseq_sequences.simd;
import bioseq_sequences.kmers;
import bioseq_sequences.kmers.hashers;
import algo.circular_append_buffer;
import bioseq_instr;

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

/**
 * Compile-time upper bound on the window size `w` accepted by the
 * two-stack collectors in this file. Our minimap2 ports run at the
 * default `w = 10`; bumping past 256 also blows the L1-friendly stack
 * footprint of the two parallel ring buffers (elem / hash_strand).
 */
constexpr size_t two_stack_max_window = 256;


namespace two_stack_internal {

/// Compile-time XOR-complement detection. Identical to the predicate
/// inside `lexic_hasher::simd_hash` but pulled out so it can be used
/// at the impl-function site.
template<sequence_alphabet alphabet_t, typename storage_unit>
constexpr bool is_xor_complement_v = []() constexpr {
    using traits = compact_sequence_range<alphabet_t>::compact_sequence_range_base::traits;
    constexpr storage_unit xor_mask =
        static_cast<storage_unit>(traits::alphabet_size - 1);
    for (size_t r = 0; r < traits::alphabet_size; ++r) {
        const storage_unit expected = static_cast<storage_unit>(r) ^ xor_mask;
        const storage_unit actual = static_cast<storage_unit>(
            traits::letter_complement(static_cast<storage_unit>(r)));
        if (expected != actual) return false;
    }
    return true;
}();

}  // namespace two_stack_internal


// =================================================================================
// Two-stack nthash collector — UNIFIED single-stream + dual-stream impl.
// ---------------------------------------------------------------------------------
// One templated function (NumStreams ∈ {1, 2}) replaces what used to be two
// sibling implementations (`…_single_stream_simd_impl` for short inputs and
// `…_simd_impl` for the dual-stream production path). Driven by the generic
// `iterate_sequence_bp_simd_v2` from `simd.cppm`, which owns all of the
// matrix-load / transpose / partial-chunk-mask plumbing; the per-BP body
// here is one `[&]` lambda mirroring the simd-minimizers paper's
// description of the two-stack queue.
//
// Hash function: lexic ordering with a cheap multiplicative mix
// (`(lex × golden_ratio) & hash_kmer_mask`) inside the inner loop; the full
// `simd_hash_t::scramble` (e.g. minimap_int's seven-step integer mix) is
// applied at emit time on the chosen lane's lex value (recovered from the
// parallel `ring_buf_hash_strand_*` slot, with strand bit packed in LSB).
// Both query and target apply the identical scheme so target-index lookups
// match.
//
// Palindrome detection is `if constexpr (NumStreams == 1)`-gated: dropped
// on the dual-stream production path (saves 1 cmeq + 1 bic per stream per
// iteration; palindromic kmers that win their window are exceedingly rare
// on random sequences ≈ 4^-(k/2) ≈ 0.001 % at k=15 and the resulting
// selection drift sits well within the density tolerance the bench uses),
// preserved on the single-stream fallback to match the V9 single-stream
// behaviour byte-for-byte. The dual-stream path is V12+'s deliberate
// performance choice; the single-stream path's palindrome detection
// matters at short k (e.g. k=4 dna4 → 4-mer palindromes like "ACGT",
// "AGCT", "TGCA" are real and common). Keeping the gating consistent
// with the OLD `_single_stream_simd_impl` / `_simd_impl` split means the
// behaviour pre/post refactor is identical at every NumStreams.
//
// For inputs too short to fill 2 × Lanes(d) chunks of stride the public
// dispatch picks NumStreams=1, which still works against the same
// templated impl.
//
// State layout: per-stream forward / RC rolling state, prefix-min, ring
// buffers — all kept as flat NAMED scalars (`state_fwd_0`, `state_fwd_1`,
// `ring_buf_elem_0`, `ring_buf_elem_1`) under `if constexpr (NumStreams > 1)`
// guards rather than `std::array<vec_t, NumStreams>`. `std::array` would
// force the compiler to materialise the array's address through `[&]`
// lambda capture and tends to spill state to memory; the named-scalar
// layout below is the same shape today's dual-stream production path uses
// and is empirically register-safe on M1 NEON. See ENGINEERING_REPORT_V2.md
// §V11–V13 for the historical evolution.
//
// V10b "kmer_mask & hash_mask_high" BUG FIX comment block is preserved
// below verbatim — the regression it documents (every uint64 storage
// configuration silently collapsing to position-only ordering when the
// fold bug was present) is the same on the unified impl, hence the same
// `hash_kmer_mask = hash_mask_high` choice.
// =================================================================================

template<
    size_t StaticW,
    size_t StaticK,
    size_t NumStreams,
    template<typename H_A> typename simd_hash_t,
    class MinimizerT,
    supports_compact_sequence_range R,
    typename I = decltype(compact_sequence_range<R>().begin()),
    typename hash_val_t = compact_sequence_range<R>::traits::storage_unit
>
requires (
    supports_simd_hash<simd_hash_t, typename compact_sequence_range<R>::traits::alphabet_type>
)
[[gnu::always_inline]] [[gnu::flatten]] inline void
collect_minimizers_two_stack_nthash_simd_impl(
    sketch_collector_t<MinimizerT>& collector,
    compact_sequence_range<R>& seq,
    const I& seq_begin,
    const I& seq_end,
    const size_t w_runtime,
    const size_t k_runtime
) {
    constexpr bool has_static_w = (StaticW != 0);
    constexpr bool has_static_k = (StaticK != 0);
    const size_t w = has_static_w ? StaticW : w_runtime;
    const size_t k = has_static_k ? StaticK : k_runtime;

    using seq_traits   = compact_sequence_range<R>::traits;
    using storage_unit = typename seq_traits::storage_unit;
    using alphabet_t   = typename seq_traits::alphabet_type;

    using d_t    = hn::ScalableTag<storage_unit>;
    const d_t d;
    using vec_t  = hn::Vec<d_t>;
    using mask_t = hn::Mask<d_t>;

    const size_t lanes_per_stream = hn::Lanes(d);

    // ---- K-clamp (REGRESSION_NTHASH known caveat — see report) -------
    constexpr size_t kmer_max_length =
        sizeof(storage_unit) * 8 / seq_traits::bits_per_char;
    const size_t k_clamped = (k < kmer_max_length) ? k : kmer_max_length;
    const storage_unit kmer_mask =
        seq_traits::template kmer_mask<storage_unit>(k_clamped);
    const vec_t v_kmer_mask = hn::Set(d, kmer_mask);

    // ---- Alphabet complement (XOR for dna4; dna5 uses the same XOR
    // path here per ENGINEERING_REPORT_V2.md V11, with the known dna5
    // limitation tracked in FIX_TESTS_FINDINGS_AND_SOLUTIONS.md §2.2).
    constexpr bool is_xor_complement =
        two_stack_internal::is_xor_complement_v<alphabet_t, storage_unit>;
    (void)is_xor_complement;
    const storage_unit alphabet_complement_mask =
        static_cast<storage_unit>(seq_traits::alphabet_size - 1);
    const vec_t v_alphabet_complement_xor = hn::Set(d, alphabet_complement_mask);

    // ---- Element-packing constants (hash high half | pos low half) --
    constexpr size_t storage_bits   = sizeof(storage_unit) * 8;
    constexpr size_t hash_bits      = storage_bits / 2;
    constexpr storage_unit pos_mask = (hash_bits >= storage_bits)
        ? static_cast<storage_unit>(-1)
        : (static_cast<storage_unit>(1) << hash_bits) - 1;
    constexpr storage_unit hash_mask_high = static_cast<storage_unit>(~pos_mask);
    constexpr storage_unit max_val        = static_cast<storage_unit>(-1);

    DEBUG_ASSERT(w <= two_stack_max_window,
        "two_stack_max_window must hold the requested window size");
    DEBUG_ASSERT(w >= 1, "Window size must be at least 1");

    const vec_t v_max       = hn::Set(d, max_val);
    const vec_t v_zero      = hn::Zero(d);
    const vec_t v_one       = hn::Set(d, static_cast<storage_unit>(1));
    const vec_t v_pos_mask  = hn::Set(d, pos_mask);
    // V10b mask: `hash_mask_high` zeros the low `hash_bits` of the storage
    // word so we can OR in `pos_local`.
    //
    // BUG FIX (FIX_MINIMIZER_TERRIBLE_REGRESSION_PROBLEMS_REPORT.md):
    // the previous form folded `kmer_mask & hash_mask_high` into one mask
    // as a "save-1-SIMD-op" optimisation. That fold is broken for any
    // storage_unit where `kmer_mask` lives entirely in the *low* bits and
    // `hash_mask_high` in the *high* bits — concretely, every uint64
    // configuration on the bench grid:
    //   * dna4 K=15 → kmer_mask = (1<<30)-1 = 0x3FFFFFFF (low 30 bits)
    //   * uint64    → hash_mask_high = 0xFFFFFFFF00000000 (top 32 bits)
    //   * intersect → 0   ← every hash collapses to zero
    // After the bug, `elem = hash_full | pos_local` reduced to just
    // `pos_local`, so the sliding-window-min trivially picked the leftmost
    // *position* every step (instead of the leftmost smallest *hash*),
    // and the per-step `pos_changed` check fired on every iteration —
    // producing ~N emits on dna4-longer instead of the ~N/W the
    // simd-minimizers paper guarantees.
    //
    // The right mask is `hash_mask_high` *alone*: the multiplication
    // `lex × golden` produces a ~62-bit-wide product, and the high bits
    // of that product are what we want to keep as the hash. The kmer-mask
    // intersection was protecting against an ill-formed lex (which the
    // hasher itself already guarantees never happens — `lexic_hash`
    // is masked at construction).
    const storage_unit hash_kmer_mask = hash_mask_high;
    const vec_t v_hash_kmer_mask = hn::Set(d, hash_kmer_mask);
    const vec_t v_strand_clear =
        hn::Set(d, static_cast<storage_unit>(~static_cast<storage_unit>(1)));
    constexpr storage_unit golden_ratio_const =
        (sizeof(storage_unit) >= 4)
            ? static_cast<storage_unit>(0x9E3779B9)
            : static_cast<storage_unit>(0x9E37);
    const vec_t v_golden = hn::Set(d, golden_ratio_const);

    constexpr size_t buf_capacity = has_static_w ? StaticW : two_stack_max_window;

    // ---- Per-stream ring buffers — flat NAMED arrays under if constexpr.
    // (See impl docstring above for the std::array-vs-named rationale.)
    HWY_ALIGN std::array<vec_t, buf_capacity> ring_buf_elem_0;
    HWY_ALIGN std::array<vec_t, buf_capacity> ring_buf_hash_strand_0;
    [[maybe_unused]] HWY_ALIGN std::array<vec_t, buf_capacity> ring_buf_elem_1;
    [[maybe_unused]] HWY_ALIGN std::array<vec_t, buf_capacity> ring_buf_hash_strand_1;
    for (size_t i = 0; i < w; ++i) {
        ring_buf_elem_0[i]        = v_max;
        ring_buf_hash_strand_0[i] = v_zero;
        if constexpr (NumStreams > 1) {
            ring_buf_elem_1[i]        = v_max;
            ring_buf_hash_strand_1[i] = v_zero;
        }
    }

    // ---- Per-stream rolling state ----
    vec_t state_fwd_0      = v_zero;
    vec_t state_rc_0       = v_zero;
    vec_t prefix_min_0     = v_max;
    vec_t prefix_kmer_0    = v_zero;
    vec_t pos_local_0      = v_zero;
    vec_t last_emit_pos_0  = v_max;
    [[maybe_unused]] vec_t state_fwd_1, state_rc_1, prefix_min_1,
                           prefix_kmer_1, pos_local_1, last_emit_pos_1;
    if constexpr (NumStreams > 1) {
        state_fwd_1     = v_zero;
        state_rc_1      = v_zero;
        prefix_min_1    = v_max;
        prefix_kmer_1   = v_zero;
        pos_local_1     = v_zero;
        last_emit_pos_1 = v_max;
    }

    // ---- Shared scalar bookkeeping ----
    storage_unit pos_wrap_delta = 0;
    size_t step     = 0;
    size_t ring_idx = 0;

    constexpr storage_unit pos_max = pos_mask;
    const storage_unit wrap_delta_amount =
        static_cast<storage_unit>(pos_max - static_cast<storage_unit>(w) - 1);
    const size_t wrap_period   = static_cast<size_t>(wrap_delta_amount);
    size_t next_wrap_step      = wrap_period;
    const size_t emit_warmup   = k + w - 1;
    const vec_t v_wrap         = hn::Set(d, wrap_delta_amount);

    // Scalar copy of kmer_mask passed to `simd_hash_t<…>::scramble` at
    // emit time (the SIMD per-step path doesn't need it because `hash_full`
    // already lives in the high half).
    const hash_val_t kmer_mask_scalar = static_cast<hash_val_t>(kmer_mask);

    // ---- Per-BP body — the simd-minimizers paper's two-stack inner loop,
    // generalised over NumStreams via `if constexpr` on per-stream state.
    //
    // BP vectors arrive as a parameter PACK (`BpVecs... bps`) that
    // expands to `vec_t bp_v_0 [, vec_t bp_v_1]` — i.e. separate
    // by-value `vec_t` parameters, NOT a `std::array<vec_t, NumStreams>`.
    // On the M1 NEON ABI vec_t parameters land in NEON registers V0/V1,
    // so the lambda's body sees BPs as register values from the start
    // (matches the original `step_one_bp_pair(d, bp_lo, bp_hi, …)`
    // shape that V11/V12 verified to compile to good code).
    auto step_one_bp = [&]<bool is_full, typename... BpVecs>(
        std::bool_constant<is_full>,
        auto const& offsets_in,
        auto& /*d unused — captured */,
        auto& live_mask,
        BpVecs... bps
    ) [[gnu::always_inline]] {
        static_assert(sizeof...(bps) == NumStreams,
            "step_one_bp lambda expects exactly NumStreams BP vectors");
        // Pack-of-references → named locals via std::tuple. Compiler
        // promotes these to NEON register copies because the bps... pack
        // is itself by-value and already register-resident.
        const auto bps_tuple = std::forward_as_tuple(bps...);
        const vec_t bp_v_0 = std::get<0>(bps_tuple);
        [[maybe_unused]] vec_t bp_v_1;
        if constexpr (NumStreams > 1) {
            bp_v_1 = std::get<1>(bps_tuple);
        }

        // ---- Forward / RC state update ----
        // Hand-interleaved across the two streams via flat scalars: the
        // OoO scheduler reorders independent ops on the M1 NEON pipes
        // exactly as the original `step_one_bp_pair` did.
        const vec_t shift_left_fwd_0 = hn::ShiftLeft<seq_traits::bits_per_char>(state_fwd_0);
        const vec_t complemented_0   = hn::Xor(bp_v_0, v_alphabet_complement_xor);
        [[maybe_unused]] vec_t shift_left_fwd_1, complemented_1;
        if constexpr (NumStreams > 1) {
            shift_left_fwd_1 = hn::ShiftLeft<seq_traits::bits_per_char>(state_fwd_1);
            complemented_1   = hn::Xor(bp_v_1, v_alphabet_complement_xor);
        }
        state_fwd_0 = hn::And(hn::Or(shift_left_fwd_0, bp_v_0), v_kmer_mask);
        if constexpr (NumStreams > 1) {
            state_fwd_1 = hn::And(hn::Or(shift_left_fwd_1, bp_v_1), v_kmer_mask);
        }

        vec_t new_high_0;
        [[maybe_unused]] vec_t new_high_1;
        if constexpr (has_static_k) {
            constexpr size_t static_rev_shift =
                seq_traits::bits_per_char * (StaticK - 1);
            constexpr size_t storage_bits_max = sizeof(storage_unit) * 8;
            constexpr size_t clamped_rev_shift =
                (static_rev_shift >= storage_bits_max)
                    ? (storage_bits_max - 1)
                    : static_rev_shift;
            new_high_0 = hn::ShiftLeft<clamped_rev_shift>(complemented_0);
            if constexpr (NumStreams > 1) {
                new_high_1 = hn::ShiftLeft<clamped_rev_shift>(complemented_1);
            }
        } else {
            const size_t dynamic_rev_shift =
                seq_traits::bits_per_char * (k_clamped - 1);
            new_high_0 = hn::ShiftLeftSame(complemented_0,
                static_cast<int>(dynamic_rev_shift));
            if constexpr (NumStreams > 1) {
                new_high_1 = hn::ShiftLeftSame(complemented_1,
                    static_cast<int>(dynamic_rev_shift));
            }
        }
        const vec_t shift_right_rc_0 =
            hn::ShiftRight<seq_traits::bits_per_char>(state_rc_0);
        [[maybe_unused]] vec_t shift_right_rc_1;
        if constexpr (NumStreams > 1) {
            shift_right_rc_1 =
                hn::ShiftRight<seq_traits::bits_per_char>(state_rc_1);
        }
        state_rc_0 = hn::Or(shift_right_rc_0, new_high_0);
        if constexpr (NumStreams > 1) {
            state_rc_1 = hn::Or(shift_right_rc_1, new_high_1);
        }

        // ---- Canonical lex + strand classification ----
        // Palindrome gating is NumStreams=1 only (see file-level docstring
        // for the rationale). Dual-stream drops it to save 1 cmeq + 1 bic
        // per stream per iter at the price of a ≈ 4^-(k/2) selection-drift
        // tolerance on the dense-bench grid; single-stream keeps it so that
        // short-k inputs (k=4 / dna4 has real 4-mer palindromes like
        // "ACGT", "AGCT") match the historical _single_stream_simd_impl
        // behaviour bit-for-bit.
        const vec_t lex_0    = hn::Min(state_fwd_0, state_rc_0);
        const mask_t strand_0 = hn::Lt(state_rc_0, state_fwd_0);
        [[maybe_unused]] mask_t palindrome_mask_0;
        if constexpr (NumStreams == 1) {
            palindrome_mask_0 = hn::Eq(state_fwd_0, state_rc_0);
        }
        [[maybe_unused]] vec_t lex_1;
        [[maybe_unused]] mask_t strand_1;
        if constexpr (NumStreams > 1) {
            lex_1    = hn::Min(state_fwd_1, state_rc_1);
            strand_1 = hn::Lt(state_rc_1, state_fwd_1);
        }

        // Mixed-hash for ordering. See V10b BUG FIX block above for why
        // the post-mul mask is `hash_mask_high` alone.
        const vec_t hash_full_0 =
            hn::And(hn::Mul(lex_0, v_golden), v_hash_kmer_mask);
        [[maybe_unused]] vec_t hash_full_1;
        if constexpr (NumStreams > 1) {
            hash_full_1 = hn::And(hn::Mul(lex_1, v_golden), v_hash_kmer_mask);
        }

        // Strand-encoded lex (LSB := strand bit) for emit-time recovery.
        const vec_t hs_0 = hn::IfThenElse(strand_0,
            hn::Or(lex_0, v_one),
            hn::And(lex_0, v_strand_clear));
        [[maybe_unused]] vec_t hs_1;
        if constexpr (NumStreams > 1) {
            hs_1 = hn::IfThenElse(strand_1,
                hn::Or(lex_1, v_one),
                hn::And(lex_1, v_strand_clear));
        }

        // (hash | pos) packed element. Live-mask gates non-padding lanes;
        // single-stream additionally `AndNot(palindrome_mask)`-gates the
        // live mask so palindromic lanes get the v_max sentinel.
        const vec_t elem_valid_0 = hn::Or(hash_full_0, pos_local_0);
        mask_t valid_mask_0;
        if constexpr (NumStreams == 1) {
            valid_mask_0 = hn::AndNot(palindrome_mask_0, live_mask);
        } else {
            valid_mask_0 = live_mask;
        }
        const vec_t elem_0 = hn::IfThenElse(valid_mask_0, elem_valid_0, v_max);
        [[maybe_unused]] vec_t elem_valid_1, elem_1;
        if constexpr (NumStreams > 1) {
            elem_valid_1 = hn::Or(hash_full_1, pos_local_1);
            elem_1       = hn::IfThenElse(live_mask, elem_valid_1, v_max);
        }

        ring_buf_elem_0[ring_idx]        = elem_0;
        ring_buf_hash_strand_0[ring_idx] = hs_0;
        if constexpr (NumStreams > 1) {
            ring_buf_elem_1[ring_idx]        = elem_1;
            ring_buf_hash_strand_1[ring_idx] = hs_1;
        }

        // ---- Prefix-min running update ----
        const mask_t prefix_better_0 = hn::Lt(elem_0, prefix_min_0);
        prefix_min_0  = hn::IfThenElse(prefix_better_0, elem_0, prefix_min_0);
        prefix_kmer_0 = hn::IfThenElse(prefix_better_0, hs_0, prefix_kmer_0);
        if constexpr (NumStreams > 1) {
            const mask_t prefix_better_1 = hn::Lt(elem_1, prefix_min_1);
            prefix_min_1  = hn::IfThenElse(prefix_better_1, elem_1, prefix_min_1);
            prefix_kmer_1 = hn::IfThenElse(prefix_better_1, hs_1, prefix_kmer_1);
        }

        ++ring_idx;
        ++step;
        pos_local_0 = hn::Add(pos_local_0, v_one);
        if constexpr (NumStreams > 1) {
            pos_local_1 = hn::Add(pos_local_1, v_one);
        }

        // ---- Suffix-min rebuild every w iterations ----
        if (ring_idx == w) [[unlikely]] {
            if constexpr (has_static_w) {
                [&]<size_t... Idx>(std::index_sequence<Idx...>) {
                    auto rebuild_one = [&](auto i_const) [[gnu::always_inline]] {
                        constexpr size_t i = StaticW - 1 - decltype(i_const)::value;
                        const vec_t cur_0  = ring_buf_elem_0[i];
                        const vec_t prev_0 = ring_buf_elem_0[i - 1];
                        const mask_t cur_better_0 = hn::Lt(cur_0, prev_0);
                        ring_buf_elem_0[i - 1] =
                            hn::IfThenElse(cur_better_0, cur_0, prev_0);
                        ring_buf_hash_strand_0[i - 1] =
                            hn::IfThenElse(cur_better_0,
                                ring_buf_hash_strand_0[i],
                                ring_buf_hash_strand_0[i - 1]);
                        if constexpr (NumStreams > 1) {
                            const vec_t cur_1  = ring_buf_elem_1[i];
                            const vec_t prev_1 = ring_buf_elem_1[i - 1];
                            const mask_t cur_better_1 = hn::Lt(cur_1, prev_1);
                            ring_buf_elem_1[i - 1] =
                                hn::IfThenElse(cur_better_1, cur_1, prev_1);
                            ring_buf_hash_strand_1[i - 1] =
                                hn::IfThenElse(cur_better_1,
                                    ring_buf_hash_strand_1[i],
                                    ring_buf_hash_strand_1[i - 1]);
                        }
                    };
                    (rebuild_one(std::integral_constant<size_t, Idx>{}), ...);
                }(std::make_index_sequence<StaticW - 1>{});
            } else {
                for (size_t i = w - 1; i > 0; --i) {
                    const vec_t cur_0  = ring_buf_elem_0[i];
                    const vec_t prev_0 = ring_buf_elem_0[i - 1];
                    const mask_t cur_better_0 = hn::Lt(cur_0, prev_0);
                    ring_buf_elem_0[i - 1] =
                        hn::IfThenElse(cur_better_0, cur_0, prev_0);
                    ring_buf_hash_strand_0[i - 1] =
                        hn::IfThenElse(cur_better_0,
                            ring_buf_hash_strand_0[i],
                            ring_buf_hash_strand_0[i - 1]);
                    if constexpr (NumStreams > 1) {
                        const vec_t cur_1  = ring_buf_elem_1[i];
                        const vec_t prev_1 = ring_buf_elem_1[i - 1];
                        const mask_t cur_better_1 = hn::Lt(cur_1, prev_1);
                        ring_buf_elem_1[i - 1] =
                            hn::IfThenElse(cur_better_1, cur_1, prev_1);
                        ring_buf_hash_strand_1[i - 1] =
                            hn::IfThenElse(cur_better_1,
                                ring_buf_hash_strand_1[i],
                                ring_buf_hash_strand_1[i - 1]);
                    }
                }
            }
            prefix_min_0  = elem_0;
            prefix_kmer_0 = hs_0;
            if constexpr (NumStreams > 1) {
                prefix_min_1  = elem_1;
                prefix_kmer_1 = hs_1;
            }
            ring_idx = 0;
        }

        // ---- Window-min = min(prefix_min, ring_buf[ring_idx]) ----
        const vec_t suffix_min_0  = ring_buf_elem_0[ring_idx];
        const vec_t suffix_kmer_0 = ring_buf_hash_strand_0[ring_idx];
        const mask_t prefix_wins_0 = hn::Lt(prefix_min_0, suffix_min_0);
        const vec_t window_min_0   =
            hn::IfThenElse(prefix_wins_0, prefix_min_0, suffix_min_0);
        const vec_t window_hs_0    =
            hn::IfThenElse(prefix_wins_0, prefix_kmer_0, suffix_kmer_0);
        [[maybe_unused]] vec_t window_min_1, window_hs_1;
        if constexpr (NumStreams > 1) {
            const vec_t suffix_min_1  = ring_buf_elem_1[ring_idx];
            const vec_t suffix_kmer_1 = ring_buf_hash_strand_1[ring_idx];
            const mask_t prefix_wins_1 = hn::Lt(prefix_min_1, suffix_min_1);
            window_min_1 =
                hn::IfThenElse(prefix_wins_1, prefix_min_1, suffix_min_1);
            window_hs_1  =
                hn::IfThenElse(prefix_wins_1, prefix_kmer_1, suffix_kmer_1);
        }

        // ---- Position wrap (storage-half-bit pos rolls every wrap_period) ----
        if (step == next_wrap_step) [[unlikely]] {
            pos_local_0  = hn::Sub(pos_local_0, v_wrap);
            prefix_min_0 = hn::Sub(prefix_min_0, v_wrap);
            const mask_t emit_seen_0 = hn::Ne(last_emit_pos_0, v_max);
            last_emit_pos_0 = hn::IfThenElse(emit_seen_0,
                hn::Sub(last_emit_pos_0, v_wrap), last_emit_pos_0);
            if constexpr (NumStreams > 1) {
                pos_local_1  = hn::Sub(pos_local_1, v_wrap);
                prefix_min_1 = hn::Sub(prefix_min_1, v_wrap);
                const mask_t emit_seen_1 = hn::Ne(last_emit_pos_1, v_max);
                last_emit_pos_1 = hn::IfThenElse(emit_seen_1,
                    hn::Sub(last_emit_pos_1, v_wrap), last_emit_pos_1);
            }
            if constexpr (has_static_w) {
                [&]<size_t... Idx>(std::index_sequence<Idx...>) {
                    auto wrap_slot = [&](auto i_const) [[gnu::always_inline]] {
                        constexpr size_t i = decltype(i_const)::value;
                        const mask_t slot_valid_0 =
                            hn::Ne(ring_buf_elem_0[i], v_max);
                        ring_buf_elem_0[i] = hn::IfThenElse(slot_valid_0,
                            hn::Sub(ring_buf_elem_0[i], v_wrap),
                            ring_buf_elem_0[i]);
                        if constexpr (NumStreams > 1) {
                            const mask_t slot_valid_1 =
                                hn::Ne(ring_buf_elem_1[i], v_max);
                            ring_buf_elem_1[i] = hn::IfThenElse(slot_valid_1,
                                hn::Sub(ring_buf_elem_1[i], v_wrap),
                                ring_buf_elem_1[i]);
                        }
                    };
                    (wrap_slot(std::integral_constant<size_t, Idx>{}), ...);
                }(std::make_index_sequence<StaticW>{});
            } else {
                for (size_t i = 0; i < w; ++i) {
                    const mask_t slot_valid_0 =
                        hn::Ne(ring_buf_elem_0[i], v_max);
                    ring_buf_elem_0[i] = hn::IfThenElse(slot_valid_0,
                        hn::Sub(ring_buf_elem_0[i], v_wrap),
                        ring_buf_elem_0[i]);
                    if constexpr (NumStreams > 1) {
                        const mask_t slot_valid_1 =
                            hn::Ne(ring_buf_elem_1[i], v_max);
                        ring_buf_elem_1[i] = hn::IfThenElse(slot_valid_1,
                            hn::Sub(ring_buf_elem_1[i], v_wrap),
                            ring_buf_elem_1[i]);
                    }
                }
            }
            pos_wrap_delta =
                static_cast<storage_unit>(pos_wrap_delta + wrap_delta_amount);
            next_wrap_step += wrap_period;
        }

        // ---- Emit ----
        if (step >= emit_warmup) {
            const vec_t window_pos_0  = hn::And(window_min_0, v_pos_mask);
            const mask_t pos_changed_0 = hn::Ne(window_pos_0, last_emit_pos_0);
            const mask_t valid_window_0 = hn::Ne(window_min_0, v_max);
            const mask_t real_emit_0    = hn::And(pos_changed_0, valid_window_0);
            const bool any_emit_0       = !hn::AllFalse(d, real_emit_0);

            [[maybe_unused]] vec_t window_pos_1;
            [[maybe_unused]] mask_t real_emit_1 = hn::MaskFalse(d);
            [[maybe_unused]] bool any_emit_1    = false;
            if constexpr (NumStreams > 1) {
                window_pos_1 = hn::And(window_min_1, v_pos_mask);
                const mask_t pos_changed_1 =
                    hn::Ne(window_pos_1, last_emit_pos_1);
                const mask_t valid_window_1 =
                    hn::Ne(window_min_1, v_max);
                real_emit_1 = hn::And(pos_changed_1, valid_window_1);
                any_emit_1  = !hn::AllFalse(d, real_emit_1);
            }

            if (any_emit_0 || any_emit_1) [[likely]] {
                constexpr size_t max_lanes_per_stream = hn::MaxLanes(d_t{});
                HWY_ALIGN std::array<storage_unit, max_lanes_per_stream> pos_arr_0;
                HWY_ALIGN std::array<storage_unit, max_lanes_per_stream> hs_arr_0;
                HWY_ALIGN std::array<storage_unit, max_lanes_per_stream> emit_arr_0;
                hn::Store(window_pos_0, d, pos_arr_0.data());
                hn::Store(window_hs_0,  d, hs_arr_0.data());
                hn::Store(hn::VecFromMask(d, real_emit_0), d, emit_arr_0.data());
                [[maybe_unused]] HWY_ALIGN std::array<storage_unit, max_lanes_per_stream>
                    pos_arr_1, hs_arr_1, emit_arr_1;
                if constexpr (NumStreams > 1) {
                    hn::Store(window_pos_1, d, pos_arr_1.data());
                    hn::Store(window_hs_1,  d, hs_arr_1.data());
                    hn::Store(hn::VecFromMask(d, real_emit_1), d, emit_arr_1.data());
                }

                const size_t lc = lanes_per_stream;
                if (any_emit_0) {
                    for (size_t lane = 0; lane < lc; ++lane) {
                        if (!emit_arr_0[lane]) continue;
                        const size_t local_pos =
                            static_cast<size_t>(pos_arr_0[lane]) +
                            static_cast<size_t>(pos_wrap_delta);
                        const size_t abs_pos =
                            offsets_in[lane] * seq_traits::letters_per_unit +
                            local_pos;
                        const hash_val_t hash_strand_val =
                            static_cast<hash_val_t>(hs_arr_0[lane]);
                        const hash_val_t lexic_kmer =
                            hash_strand_val & ~static_cast<hash_val_t>(1);
                        const hash_val_t scrambled_kmer =
                            simd_hash_t<alphabet_t>::template scramble<hash_val_t>(
                                lexic_kmer, kmer_mask_scalar);
                        collector.collect(MinimizerT(
                            /*kmer=*/static_cast<uint64_t>(scrambled_kmer),
                            /*last_pos=*/abs_pos,
                            /*strand=*/static_cast<uint64_t>(hash_strand_val) & 0x1ULL));
                    }
                    last_emit_pos_0 = hn::IfThenElse(real_emit_0,
                        window_pos_0, last_emit_pos_0);
                }
                if constexpr (NumStreams > 1) {
                    if (any_emit_1) {
                        for (size_t lane = 0; lane < lc; ++lane) {
                            if (!emit_arr_1[lane]) continue;
                            const size_t local_pos =
                                static_cast<size_t>(pos_arr_1[lane]) +
                                static_cast<size_t>(pos_wrap_delta);
                            const size_t abs_pos =
                                offsets_in[lc + lane] *
                                    seq_traits::letters_per_unit + local_pos;
                            const hash_val_t hash_strand_val =
                                static_cast<hash_val_t>(hs_arr_1[lane]);
                            const hash_val_t lexic_kmer =
                                hash_strand_val & ~static_cast<hash_val_t>(1);
                            const hash_val_t scrambled_kmer =
                                simd_hash_t<alphabet_t>::template scramble<hash_val_t>(
                                    lexic_kmer, kmer_mask_scalar);
                            collector.collect(MinimizerT(
                                /*kmer=*/static_cast<uint64_t>(scrambled_kmer),
                                /*last_pos=*/abs_pos,
                                /*strand=*/static_cast<uint64_t>(hash_strand_val) & 0x1ULL));
                        }
                        last_emit_pos_1 = hn::IfThenElse(real_emit_1,
                            window_pos_1, last_emit_pos_1);
                    }
                }
            }
        }
    };

    // ---- Drive iteration ----
    iterate_sequence_bp_simd_v2<NumStreams>(
        seq, seq_begin, seq_end, k + w - 1,
        step_one_bp,
        // No-op finish_fn — emits are flushed per-step, so the partial-tail
        // exit path has nothing extra to do.
        [](auto const&, auto&, auto&){});
}


/**
 * Public API for the two-stack nthash collector.
 *
 * Picks NumStreams ∈ {1, 2} based on `seq.bp_size()` (dual-stream needs
 * room for two parallel slices of `Lanes(d) * letters_per_unit` BPs each
 * plus the `k + w - 1` context overlap), then dispatches to
 * `collect_minimizers_two_stack_nthash_simd_impl` with the static-(w, k)
 * specialization for the minimap2 default `w == 10, k == 15` and the two
 * standard runtime-w/k fallbacks.
 *
 * `MinimizerT` is deduced from the collector argument so call sites stay
 * the same shape as before — only `sketch_collector` (which is
 * `sketch_collector_t<MinimapSketchData>`) changed under the hood.
 */
template<
    template<typename H_A> typename simd_hash_t,
    class MinimizerT,
    supports_compact_sequence_range R,
    typename I = decltype(compact_sequence_range<R>().begin()),
    typename hash_val_t = compact_sequence_range<R>::traits::storage_unit
>
requires (
    supports_simd_hash<simd_hash_t, typename compact_sequence_range<R>::traits::alphabet_type>
)
inline void collect_minimizers_two_stack_nthash_simd(
    sketch_collector_t<MinimizerT>& collector,
    compact_sequence_range<R>& seq,
    const I& seq_begin,
    const I& seq_end,
    const size_t w,
    const size_t k
) {
    static const auto* _desc =
        bioseq::instr::register_region_static("mm_sketch_simd");
    auto _scope = bioseq::instr::region(_desc);

    using seq_traits   = compact_sequence_range<R>::traits;
    using storage_unit = typename seq_traits::storage_unit;
    using d_t          = hn::ScalableTag<storage_unit>;
    const d_t d;
    constexpr size_t lpu        = seq_traits::letters_per_unit;
    const size_t lanes_per_stream = hn::Lanes(d);
    const size_t context_bps    = k + w - 1;
    const size_t min_seq_for_dual =
        2 * lanes_per_stream * lpu + context_bps;
    const bool use_dual = (seq.bp_size() >= min_seq_for_dual);

    auto dispatch = [&]<size_t StaticW, size_t StaticK>() {
        if (use_dual) {
            collect_minimizers_two_stack_nthash_simd_impl<
                StaticW, StaticK, /*NumStreams=*/2,
                simd_hash_t, MinimizerT, R, I, hash_val_t>(
                    collector, seq, seq_begin, seq_end, w, k);
        } else {
            collect_minimizers_two_stack_nthash_simd_impl<
                StaticW, StaticK, /*NumStreams=*/1,
                simd_hash_t, MinimizerT, R, I, hash_val_t>(
                    collector, seq, seq_begin, seq_end, w, k);
        }
    };

    if (w == 10 && k == 15) [[likely]] {
        dispatch.template operator()<10, 15>();
        return;
    }
    if (w == 10) {
        dispatch.template operator()<10, 0>();
        return;
    }
    dispatch.template operator()<0, 0>();
}


} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();

}
