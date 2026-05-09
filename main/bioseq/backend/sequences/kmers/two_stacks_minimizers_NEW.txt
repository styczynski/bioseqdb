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
import bioseq_simd.utils;            // for_each_index<W>
import bioseq_simd.vector_stream;    // vec_stream<D, ST, W> + lifted ops
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
// Phase-3 layout: per-stream rolling state and the ring buffers are bundled
// into `vec_stream<D, ST, NumStreams>` from `bioseq_simd.vector_stream`. The
// pre-Phase-3 file used named scalars (`state_fwd_0`, `state_fwd_1`, …) under
// `if constexpr (NumStreams > 1)` guards so the Phase-1 register-safety
// contract held; Phase-3 hides every per-stream replication site behind the
// vec_stream abstraction. Compile-time unrolling at W ∈ {1, 2} produces the
// same scalar-replaced code the named layout did, but the source reads as
// the simd-minimizers paper's pseudocode (one statement per logical op).
//
// The lambda boundary still receives BPs as a parameter PACK
// (`BpVecs... bps`) — Phase-1 demonstrated that passing `std::array<vec_t, N>`
// across this boundary forces the compiler to lose register state. The pack
// is converted into a `vec_stream::pack(bps...)` bundle inside the lambda
// where `[[gnu::always_inline]]` lets SROA promote the bundle's storage
// back to the same NEON registers V0/V1 that the ABI used for the pack.
//
// Hash function: lexic ordering with a cheap multiplicative mix
// (`(lex × golden_ratio) & hash_kmer_mask`) inside the inner loop; the full
// `simd_hash_t::scramble` (e.g. minimap_int's seven-step integer mix) is
// applied at emit time on the chosen lane's lex value (recovered from the
// parallel `ring_buf_hash_strand` slot, with strand bit packed in LSB).
// Both query and target apply the identical scheme so target-index lookups
// match.
//
// Palindrome detection is gated by the new `bool DetectPalindrome` template
// parameter (Phase-3 decoupling). The dispatch shell sets
// `DetectPalindrome = (NumStreams == 1)` to preserve Phase-1's byte-identity
// — the dual-stream production path drops palindrome detection (V12+
// optimisation: 1 cmeq + 1 bic per stream per iter saved at the cost of
// ≈ 4^-(k/2) ≈ 0.001 % selection drift on random k=15 sequences); the
// single-stream fallback keeps detection so short-k inputs (k=4 dna4 → 4-mer
// palindromes like "ACGT", "AGCT", "TGCA") match the historical
// `_single_stream_simd_impl` behaviour bit-for-bit. Decoupling lets future
// callers opt out independently.
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
    bool   DetectPalindrome,
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

    // NumStreams-wide bundle types. With aggressive inlining (the impl is
    // [[gnu::always_inline]] [[gnu::flatten]] and every vec_stream operation
    // is HWY_INLINE), the bundle's std::array<vec_t, W> storage gets
    // scalar-replaced back to per-slot NEON registers — same machine code
    // the named-scalar layout produced in Phase 1.
    using vec_stream_t  = vec_stream<d_t, storage_unit, NumStreams>;
    using mask_stream_t = mask_stream<d_t, NumStreams>;

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

    // ---- Ring buffers — slot-major bundles. Each slot holds NumStreams
    //      vec_t's; with W = 1 or 2 and aggressive inlining the bundle's
    //      array storage scalar-replaces into per-slot register state the
    //      same way Phase-1's named-scalar layout did. (No HWY_ALIGN needed:
    //      vec_stream's vec_t members carry the SIMD ABI alignment; an extra
    //      alignas can inhibit SROA — see vector_stream.cppm comment.)
    std::array<vec_stream_t, buf_capacity> ring_buf_elem;
    std::array<vec_stream_t, buf_capacity> ring_buf_hash_strand;
    {
        const auto initial_elem = vec_stream_t::splat(d, max_val);
        const auto initial_hs   = vec_stream_t::splat(d, static_cast<storage_unit>(0));
        for (size_t i = 0; i < w; ++i) {
            ring_buf_elem[i]        = initial_elem;
            ring_buf_hash_strand[i] = initial_hs;
        }
    }

    // ---- Per-stream rolling state — bundled. ----
    vec_stream_t state_fwd      = vec_stream_t::splat(d, static_cast<storage_unit>(0));
    vec_stream_t state_rc       = vec_stream_t::splat(d, static_cast<storage_unit>(0));
    vec_stream_t prefix_min     = vec_stream_t::splat(d, max_val);
    vec_stream_t prefix_kmer    = vec_stream_t::splat(d, static_cast<storage_unit>(0));
    vec_stream_t pos_local      = vec_stream_t::splat(d, static_cast<storage_unit>(0));
    vec_stream_t last_emit_pos  = vec_stream_t::splat(d, max_val);

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
    //      expressed bundle-uniformly. NumStreams enters only via the
    //      template parameter on `vec_stream_t` / `mask_stream_t`; there
    //      is NO `if constexpr (NumStreams …)` branching for per-stream
    //      replication anywhere in this body.
    //
    // BPs arrive as a parameter PACK (Phase-1 ABI for register safety) and
    // are immediately bundled via `vec_stream_t::pack(bps...)`. With
    // [[gnu::always_inline]] on this lambda + [[gnu::flatten]] on the
    // impl, the bundle's storage scalar-replaces back to the same V0/V1
    // NEON registers the ABI used for the pack.
    auto step_one_bp = [&]<bool is_full, typename... BpVecs>(
        std::bool_constant<is_full>,
        auto const& offsets_in,
        auto& /*d unused — captured */,
        mask_t live_mask,
        BpVecs... bps
    ) [[gnu::always_inline]] [[gnu::flatten]] {
        static_assert(sizeof...(bps) == NumStreams,
            "step_one_bp lambda expects exactly NumStreams BP vectors");
        const vec_stream_t bp_v = vec_stream_t::pack(bps...);

        // ---- Forward / RC state update ----
        const vec_stream_t complemented = Xor(bp_v, v_alphabet_complement_xor);
        state_fwd = And(Or(ShiftLeft<seq_traits::bits_per_char>(state_fwd), bp_v),
                        v_kmer_mask);

        vec_stream_t new_high;
        if constexpr (has_static_k) {
            constexpr size_t static_rev_shift =
                seq_traits::bits_per_char * (StaticK - 1);
            constexpr size_t storage_bits_max = sizeof(storage_unit) * 8;
            constexpr size_t clamped_rev_shift =
                (static_rev_shift >= storage_bits_max)
                    ? (storage_bits_max - 1)
                    : static_rev_shift;
            new_high = ShiftLeft<clamped_rev_shift>(complemented);
        } else {
            const size_t dynamic_rev_shift =
                seq_traits::bits_per_char * (k_clamped - 1);
            new_high = ShiftLeftSame(complemented,
                static_cast<int>(dynamic_rev_shift));
        }
        state_rc = Or(ShiftRight<seq_traits::bits_per_char>(state_rc), new_high);

        // ---- Lex + strand classification ----
        const vec_stream_t  lex    = Min(state_fwd, state_rc);
        const mask_stream_t strand = Lt(state_rc, state_fwd);

        // ---- Mixed-hash for ordering. See V10b BUG FIX block above for why
        //      the post-mul mask is `hash_mask_high` alone.
        const vec_stream_t hash_full = And(Mul(lex, v_golden), v_hash_kmer_mask);

        // ---- Strand-encoded lex (LSB := strand bit) for emit-time recovery.
        const vec_stream_t hash_with_strand_lex =
            IfThenElse(strand, Or(lex, v_one), And(lex, v_strand_clear));

        // ---- Element pack — palindrome detection on `DetectPalindrome` path.
        const vec_stream_t elem_valid = Or(hash_full, pos_local);
        vec_stream_t packed_element;
        if constexpr (DetectPalindrome) {
            const mask_stream_t palindrome = Eq(state_fwd, state_rc);
            const mask_stream_t valid_mask = AndNot(palindrome, live_mask);
            packed_element = IfThenElse(valid_mask, elem_valid, v_max);
        } else {
            packed_element = IfThenElse(live_mask, elem_valid, v_max);
        }

        ring_buf_elem[ring_idx]        = packed_element;
        ring_buf_hash_strand[ring_idx] = hash_with_strand_lex;

        // ---- Prefix-min running update ----
        const mask_stream_t prefix_better = Lt(packed_element, prefix_min);
        prefix_min  = IfThenElse(prefix_better, packed_element, prefix_min);
        prefix_kmer = IfThenElse(prefix_better, hash_with_strand_lex, prefix_kmer);

        ++ring_idx;
        ++step;
        pos_local = Add(pos_local, v_one);

        // ---- Suffix-min rebuild every w iterations ----
        if (ring_idx == w) [[unlikely]] {
            // The rebuild reads ring[i] BEFORE writing ring[i-1], so we
            // iterate i from w-1 down to 1 inclusive. With static-w we
            // unroll via index_sequence; with runtime-w we use a normal
            // descending for-loop.
            if constexpr (has_static_w) {
                for_each_index<StaticW - 1>([&](auto idx_const) [[gnu::always_inline]] {
                    constexpr size_t i = StaticW - 1 - decltype(idx_const)::value;
                    const auto current_slot      = ring_buf_elem[i];
                    const auto previous_slot     = ring_buf_elem[i - 1];
                    const auto current_better    = Lt(current_slot, previous_slot);
                    ring_buf_elem[i - 1] =
                        IfThenElse(current_better, current_slot, previous_slot);
                    ring_buf_hash_strand[i - 1] =
                        IfThenElse(current_better,
                            ring_buf_hash_strand[i],
                            ring_buf_hash_strand[i - 1]);
                });
            } else {
                for (size_t i = w - 1; i > 0; --i) {
                    const auto current_slot      = ring_buf_elem[i];
                    const auto previous_slot     = ring_buf_elem[i - 1];
                    const auto current_better    = Lt(current_slot, previous_slot);
                    ring_buf_elem[i - 1] =
                        IfThenElse(current_better, current_slot, previous_slot);
                    ring_buf_hash_strand[i - 1] =
                        IfThenElse(current_better,
                            ring_buf_hash_strand[i],
                            ring_buf_hash_strand[i - 1]);
                }
            }
            prefix_min  = packed_element;
            prefix_kmer = hash_with_strand_lex;
            ring_idx = 0;
        }

        // ---- Window-min = min(prefix_min, ring_buf[ring_idx]) ----
        const vec_stream_t  suffix_min  = ring_buf_elem[ring_idx];
        const vec_stream_t  suffix_kmer = ring_buf_hash_strand[ring_idx];
        const mask_stream_t prefix_wins = Lt(prefix_min, suffix_min);
        const vec_stream_t  window_min  =
            IfThenElse(prefix_wins, prefix_min, suffix_min);
        const vec_stream_t  window_hs   =
            IfThenElse(prefix_wins, prefix_kmer, suffix_kmer);

        // ---- Position wrap (storage-half-bit pos rolls every wrap_period) ----
        if (step == next_wrap_step) [[unlikely]] {
            pos_local  = Sub(pos_local, v_wrap);
            prefix_min = Sub(prefix_min, v_wrap);
            const mask_stream_t emit_seen = Ne(last_emit_pos, v_max);
            last_emit_pos = IfThenElse(emit_seen,
                Sub(last_emit_pos, v_wrap), last_emit_pos);
            const auto wrap_one = [&](auto idx_const) [[gnu::always_inline]] {
                constexpr size_t i = decltype(idx_const)::value;
                const mask_stream_t slot_valid = Ne(ring_buf_elem[i], v_max);
                ring_buf_elem[i] = IfThenElse(slot_valid,
                    Sub(ring_buf_elem[i], v_wrap),
                    ring_buf_elem[i]);
            };
            if constexpr (has_static_w) {
                for_each_index<StaticW>(wrap_one);
            } else {
                for (size_t i = 0; i < w; ++i) {
                    const mask_stream_t slot_valid = Ne(ring_buf_elem[i], v_max);
                    ring_buf_elem[i] = IfThenElse(slot_valid,
                        Sub(ring_buf_elem[i], v_wrap),
                        ring_buf_elem[i]);
                }
            }
            pos_wrap_delta =
                static_cast<storage_unit>(pos_wrap_delta + wrap_delta_amount);
            next_wrap_step += wrap_period;
        }

        // ---- Emit ----
        if (step >= emit_warmup) {
            const vec_stream_t  window_pos    = And(window_min, v_pos_mask);
            const mask_stream_t pos_changed   = Ne(window_pos, last_emit_pos);
            const mask_stream_t valid_window  = Ne(window_min, v_max);
            const mask_stream_t real_emit     = And(pos_changed, valid_window);
            // AllFalsePerStream returns true when ALL lanes are false (no
            // emit candidate this stream). We keep the inverted bool below
            // (`per_stream_emits`) explicit so the predicate reads naturally.
            const auto no_emit_per_stream     = AllFalsePerStream(d, real_emit);

            std::array<bool, NumStreams> per_stream_emits{};
            bool any_stream_emits = false;
            for_each_index<NumStreams>([&](auto stream_idx_const) [[gnu::always_inline]] {
                constexpr size_t s = decltype(stream_idx_const)::value;
                per_stream_emits[s] = !no_emit_per_stream[s];
                any_stream_emits = any_stream_emits || per_stream_emits[s];
            });

            if (any_stream_emits) [[likely]] {
                constexpr size_t max_lanes_per_stream = hn::MaxLanes(d_t{});
                HWY_ALIGN std::array<std::array<storage_unit, max_lanes_per_stream>, NumStreams> pos_arr;
                HWY_ALIGN std::array<std::array<storage_unit, max_lanes_per_stream>, NumStreams> hs_arr;
                HWY_ALIGN std::array<std::array<storage_unit, max_lanes_per_stream>, NumStreams> emit_arr;
                for_each_index<NumStreams>([&](auto stream_idx_const) [[gnu::always_inline]] {
                    constexpr size_t s = decltype(stream_idx_const)::value;
                    hn::Store(window_pos[s], d, pos_arr[s].data());
                    hn::Store(window_hs[s],  d, hs_arr[s].data());
                    hn::Store(hn::VecFromMask(d, real_emit[s]), d, emit_arr[s].data());
                });

                const size_t lc = lanes_per_stream;
                for_each_index<NumStreams>([&](auto stream_idx_const) {
                    constexpr size_t s = decltype(stream_idx_const)::value;
                    if (!per_stream_emits[s]) return;
                    for (size_t lane = 0; lane < lc; ++lane) {
                        if (!emit_arr[s][lane]) continue;
                        const size_t local_pos =
                            static_cast<size_t>(pos_arr[s][lane]) +
                            static_cast<size_t>(pos_wrap_delta);
                        const size_t abs_pos =
                            offsets_in[s * lc + lane] *
                                seq_traits::letters_per_unit + local_pos;
                        const hash_val_t hash_strand_val =
                            static_cast<hash_val_t>(hs_arr[s][lane]);
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
                });
                last_emit_pos = IfThenElse(real_emit, window_pos, last_emit_pos);
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
 * `collect_minimizers_two_stack_nthash_simd_impl` with
 * `DetectPalindrome = (NumStreams == 1)` (Phase-1 byte-identity contract)
 * and the static-(w, k) specialization for the minimap2 default
 * `w == 10, k == 15` plus the two standard runtime-w/k fallbacks.
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
                StaticW, StaticK,
                /*NumStreams=*/2, /*DetectPalindrome=*/false,
                simd_hash_t, MinimizerT, R, I, hash_val_t>(
                    collector, seq, seq_begin, seq_end, w, k);
        } else {
            collect_minimizers_two_stack_nthash_simd_impl<
                StaticW, StaticK,
                /*NumStreams=*/1, /*DetectPalindrome=*/true,
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