module;

#include <hwy/highway.h>
#include <hwy/aligned_allocator.h>

#include "common.h"

#include <array>
#include <ranges>
#include <type_traits>

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

// =================================================================================
// `sequence_simd_iteration_state` — Phase-4 lambda extraction (REPORT_PHASE_3_INDEPTH §7.1).
// ---------------------------------------------------------------------------------
// The Phase-3 production lambda emitted a 705-instruction (≈ 2.8 KB) standalone
// function with a ≥ 160-byte capture struct because clang's inliner treats
// large lambdas with by-reference captures as opaque aggregates and bails out
// per-call site even with `[[gnu::always_inline]]`. The DIRECT control proved
// the body inlines cleanly when expressed as a free template with named
// parameters: state lives in NEON registers across iterations, the
// capture-struct pointer-chase disappears.
//
// This struct repackages the lambda's captured locals as struct members and
// the body as a `[[gnu::always_inline]]` member function. The instance is a
// local in the caller's frame, so SROA breaks the struct apart back into
// individual NEON-register-resident locals and the inliner sees a small,
// finite parameter list (`d`, `offsets`, `live_mask`, `bp_v`) instead of a
// 20-pointer capture struct. Same machine code as DIRECT — the readability
// of the bundled state struct without the 16 % p50 lambda penalty.
//
// Invariants:
//   * The struct is stack-allocated by the impl and never heap-allocated.
//   * `step_one_bp_vec_stream` is the ONLY mutator; no other code touches
//     state members between iterations.
//   * `collector` is a reference because the sketch collector lives in the
//     caller's frame (or on its caller's stack); the state struct is
//     short-lived (one impl invocation).
// =================================================================================
template<
    size_t StaticW,
    size_t StaticK,
    size_t NumStreams,
    bool   DetectPalindrome,
    class  SeqTraits,
    template<typename H_A> typename simd_hash_t,
    class  MinimizerT,
    typename HashValT
>
struct sequence_simd_iteration_state {
    // Compile-time-derived state-struct knobs — all expressible from the
    // few "intent" parameters above. Pulling them out as `using`/`static
    // constexpr` (instead of forwarding them as separate template
    // parameters) drops the impl's instantiation site from 11 args to 8,
    // and removes the risk of name-clash between `D` and `d_t`.
    static constexpr bool   HasStaticW  = (StaticW != 0);
    static constexpr bool   HasStaticK  = (StaticK != 0);
    static constexpr size_t BufCapacity = HasStaticW ? StaticW : two_stack_max_window;
    using ST            = typename SeqTraits::storage_unit;
    using AlphabetT     = typename SeqTraits::alphabet_type;
    using d_t           = hn::ScalableTag<ST>;
    using vec_t         = hn::Vec<d_t>;
    using mask_t        = hn::Mask<d_t>;
    using vec_stream_t  = vec_stream<d_t, ST, NumStreams>;
    using mask_stream_t = mask_stream<d_t, NumStreams>;

    // ----- Per-iteration mutable state (was lambda capture-by-ref) ----
    vec_stream_t state_fwd;
    vec_stream_t state_rc;
    vec_stream_t prefix_min;
    vec_stream_t prefix_kmer;
    vec_stream_t pos_local;
    vec_stream_t last_emit_pos;
    std::array<vec_stream_t, BufCapacity> ring_buf_elem;
    std::array<vec_stream_t, BufCapacity> ring_buf_hash_strand;
    ST     pos_wrap_delta;
    size_t step;
    size_t ring_idx;
    size_t next_wrap_step;

    // ----- Loop-invariant SIMD constants (was lambda capture-by-ref) ----
    vec_t v_kmer_mask;
    vec_t v_alphabet_complement_xor;
    vec_t v_max;
    vec_t v_one;
    vec_t v_pos_mask;
    vec_t v_hash_kmer_mask;
    vec_t v_strand_clear;
    vec_t v_golden;
    vec_t v_wrap;

    // ----- Loop-invariant scalars (was lambda capture-by-ref) ----
    size_t   w;
    size_t   k_clamped;
    size_t   emit_warmup;
    size_t   wrap_period;
    size_t   lanes_per_stream;
    HashValT kmer_mask_scalar;
    ST       wrap_delta_amount;

    // ----- External (was lambda capture-by-ref) ----
    sketch_collector_t<MinimizerT>& collector;

    /// Per-BP body — the simd-minimizers paper's two-stack inner loop,
    /// expressed bundle-uniformly. NumStreams enters only via the template
    /// parameter on `vec_stream_t` / `mask_stream_t`; there is NO
    /// `if constexpr (NumStreams …)` branching for per-stream replication
    /// anywhere in this body.
    ///
    /// This was the `step_one_bp` lambda inside
    /// `collect_minimizers_two_stack_nthash_simd_impl` (Phase 3); pulled
    /// out per Phase-3-indepth §7.1 so clang's inliner sees a finite-param
    /// member function instead of a 705-instruction standalone closure.
    template<bool IsFull, typename OffsetsArray>
    [[gnu::always_inline]] [[gnu::flatten]] HWY_INLINE
    inline void step_one_bp_vec_stream(
        d_t          d,
        OffsetsArray const& offsets_in,
        mask_t       live_mask,
        vec_stream_t bp_v
    ) {
        // ---- Forward / RC state update ----
        const vec_stream_t complemented = Xor(bp_v, v_alphabet_complement_xor);
        state_fwd = And(Or(ShiftLeft<SeqTraits::bits_per_char>(state_fwd), bp_v),
                        v_kmer_mask);

        vec_stream_t new_high;
        if constexpr (HasStaticK) {
            constexpr size_t static_rev_shift =
                SeqTraits::bits_per_char * (StaticK - 1);
            // The public dispatcher widens `hash_val_t` so the lane fits
            // K's reverse-complement shift. If that contract is ever
            // broken at compile time, fail the build here instead of
            // silently clamping the shift (which silently corrupts the
            // canonical kmer — see Phase-5 §3.4).
            static_assert(static_rev_shift < sizeof(ST) * 8,
                "Static K reverse-complement shift exceeds SIMD lane "
                "width. The public dispatcher must select a wider "
                "hash_val_t (uint64_t) for K such that K*bits_per_char "
                ">= sizeof(ST)*8.");
            new_high = ShiftLeft<static_rev_shift>(complemented);
        } else {
            const size_t dynamic_rev_shift =
                SeqTraits::bits_per_char * (k_clamped - 1);
            new_high = ShiftLeftSame(complemented,
                static_cast<int>(dynamic_rev_shift));
        }
        state_rc = Or(ShiftRight<SeqTraits::bits_per_char>(state_rc), new_high);

        // ---- Lex + strand classification ----
        const vec_stream_t  lex    = Min(state_fwd, state_rc);
        const mask_stream_t strand = Lt(state_rc, state_fwd);

        // ---- Mixed-hash for ordering. See V10b BUG FIX block in
        // collect_minimizers_two_stack_nthash_simd_impl for why the post-mul
        // mask is `hash_mask_high` alone.
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
            if constexpr (HasStaticW) {
                for_each_index<StaticW - 1>([&](auto idx_const) [[gnu::always_inline]] {
                    constexpr size_t i = StaticW - 1 - decltype(idx_const)::value;
                    const auto current_slot   = ring_buf_elem[i];
                    const auto previous_slot  = ring_buf_elem[i - 1];
                    const auto current_better = Lt(current_slot, previous_slot);
                    ring_buf_elem[i - 1] =
                        IfThenElse(current_better, current_slot, previous_slot);
                    ring_buf_hash_strand[i - 1] =
                        IfThenElse(current_better,
                            ring_buf_hash_strand[i],
                            ring_buf_hash_strand[i - 1]);
                });
            } else {
                for (size_t i = w - 1; i > 0; --i) {
                    const auto current_slot   = ring_buf_elem[i];
                    const auto previous_slot  = ring_buf_elem[i - 1];
                    const auto current_better = Lt(current_slot, previous_slot);
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
            if constexpr (HasStaticW) {
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
                static_cast<ST>(pos_wrap_delta + wrap_delta_amount);
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
                HWY_ALIGN std::array<std::array<ST, max_lanes_per_stream>, NumStreams> pos_arr;
                HWY_ALIGN std::array<std::array<ST, max_lanes_per_stream>, NumStreams> hs_arr;
                HWY_ALIGN std::array<std::array<ST, max_lanes_per_stream>, NumStreams> emit_arr;
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
                                SeqTraits::letters_per_unit + local_pos;
                        const HashValT hash_strand_val =
                            static_cast<HashValT>(hs_arr[s][lane]);
                        const HashValT lexic_kmer =
                            hash_strand_val & ~static_cast<HashValT>(1);
                        const HashValT scrambled_kmer =
                            simd_hash_t<AlphabetT>::template scramble<HashValT>(
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
    }
};

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

    // ---- Phase-4 view selection (T4: bit-cast view substrate) -----
    // If the underlying storage is bit-castable to `hash_val_t` (alphabet
    // matches, both unit widths leave no leftover bits, target unit width
    // <= source — the downcast direction encoded in
    // `is_sequence_custom_alphabet_bit_castable_to`), construct a
    // `bit_casted_view<>` that re-interprets the buffer at the new unit
    // width. Otherwise hold a const reference to `seq` directly. For the
    // default `hash_val_t = R::storage_unit`, the bitcast is identity
    // (target == source); the substrate exists so future callers can opt
    // into a wider lane width by passing an explicit `hash_val_t`. See
    // `REGRESSION_NTHASH_INVESTIGATE_CRITICAL_PROBLEM_REPORT.md`'s
    // Layer A / Layer B discussion.
    using base_seq_traits   = typename compact_sequence_range<R>::traits;
    constexpr bool can_bitcast_to_hash =
        is_sequence_bit_castable_to<R, hash_val_t>;

    using bitcast_view_t = bit_casted_view<
        compact_sequence_range<R>,
        compact_sequence_range<sequence_custom_alphabet<
            typename base_seq_traits::alphabet_type, hash_val_t,
            base_seq_traits::bits_per_char>>>;
    using view_t = std::conditional_t<can_bitcast_to_hash,
        bitcast_view_t,
        compact_sequence_range<R> const&>;

    // `auto&&` deliberately — for the non-bitcast branch the IIFE returns
    // an lvalue reference to `seq`, and `auto&& seq_view` binds to that
    // reference. Using plain `auto seq_view = ...` would *value-copy* the
    // sequence, which for the `sequence_custom_alphabet` specialisation
    // owns a `std::vector` and therefore allocates+copies the entire
    // packed buffer on every call. (Forwarding-ref binding here is sound:
    // both the `seq` lvalue and the `bit_casted_view` prvalue have
    // lifetimes that span the rest of the function.)
    auto&& seq_view = ([&]() -> view_t {
        if constexpr (can_bitcast_to_hash) {
            return seq.template bit_cast_to_storage_unit<hash_val_t>();
        } else {
            return seq;
        }
    })();

    // From here on the impl works through `seq_view`'s traits — these
    // give us the SIMD lane width (`storage_unit`) actually used for the
    // rolling state, which is the right denominator for the K-clamp
    // guard below. For the default `hash_val_t = R::storage_unit` path
    // this equals `base_seq_traits` exactly, so existing K=15 code paths
    // see no change.
    using seq_traits   = typename std::remove_cvref_t<view_t>::traits;
    using storage_unit = typename seq_traits::storage_unit;
    using alphabet_t   = typename seq_traits::alphabet_type;

    // K-fits-in-lane invariant. The public dispatcher
    // (`collect_minimizers_two_stack_nthash_simd`) chooses `hash_val_t`
    // such that `K * bits_per_char` fits in the resulting SIMD lane:
    // state_fwd accumulates K letters' worth of bits, and state_rc's
    // reverse-complement shift `(K-1)*bpc < K*bpc <= lane_width` is
    // therefore in-range for `ushl`. If a future caller instantiates
    // the impl directly with an under-sized hash unit, the
    // DEBUG_ASSERT below trips before the per-BP body emits a `ushl`
    // that would saturate to zero (see Phase-5 §3 for the failure
    // mechanism).
    DEBUG_ASSERT(k * seq_traits::bits_per_char <= sizeof(storage_unit) * 8,
        fmt::format(
            "two_stack SIMD lane storage_unit too narrow for K. "
            "k={}, bits_per_char={}, lane_bits={}. The public dispatcher "
            "should have widened hash_val_t (e.g. to uint64_t) so the "
            "K-mer ({} bits) fits in the lane. See "
            "NTHASH_NARROW_FIX/REPORT_PHASE_5_INDEPTH_ANALYSIS.md.",
            k, seq_traits::bits_per_char, sizeof(storage_unit) * 8,
            k * seq_traits::bits_per_char));

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

    // `k_clamped` is now just an alias for `k` — the K-clamp guard above
    // (Layer A scalar fallback) ensures the silent-clamp branch is never
    // taken before reaching this code, so the previous
    // `(k < kmer_max_length) ? k : kmer_max_length` clamp is unreachable.
    // The alias is preserved only so the rest of the impl's existing
    // `k_clamped` identifier keeps working without renaming.
    const size_t k_clamped = k;
    const storage_unit kmer_mask =
        seq_traits::template kmer_mask<storage_unit>(k_clamped);
    const vec_t v_kmer_mask = hn::Set(d, kmer_mask);

    // ---- Alphabet complement (XOR for dna4; dna5 uses the same XOR
    // path here per ENGINEERING_REPORT_V2.md V11, with the known dna5
    // limitation tracked in FIX_TESTS_FINDINGS_AND_SOLUTIONS.md §2.2).
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

    // ---- Loop-invariant scalars derived here for the state struct ----
    constexpr storage_unit pos_max = pos_mask;
    const storage_unit wrap_delta_amount =
        static_cast<storage_unit>(pos_max - static_cast<storage_unit>(w) - 1);
    const size_t wrap_period = static_cast<size_t>(wrap_delta_amount);
    const size_t emit_warmup = k + w - 1;
    const vec_t  v_wrap      = hn::Set(d, wrap_delta_amount);

    // Scalar copy of kmer_mask passed to `simd_hash_t<…>::scramble` at
    // emit time (the SIMD per-step path doesn't need it because `hash_full`
    // already lives in the high half).
    const hash_val_t kmer_mask_scalar = static_cast<hash_val_t>(kmer_mask);

    // ---- Per-iteration state struct (Phase-4 lambda extraction) ----
    // Aggregate-init in declaration order so `step_one_bp_vec_stream` sees
    // a fully-populated stack-resident object on its first invocation.
    // Ring buffers default-init to zero here — the meaningful "splat(max)"
    // initialisation runs in the loop below (only the first `w` slots are
    // ever read; this matches the pre-Phase-4 init contract). With
    // [[clang::trivial_abi]] on `vec_stream` and the impl being
    // [[gnu::always_inline]] [[gnu::flatten]], SROA should break the struct
    // back apart into per-member NEON-register-resident locals so the
    // inliner sees a small finite-parameter member function (DIRECT-shape).
    using state_t = two_stack_internal::sequence_simd_iteration_state<
        StaticW, StaticK, NumStreams, DetectPalindrome,
        seq_traits, simd_hash_t, MinimizerT, hash_val_t>;
    state_t state{
        .state_fwd                  = vec_stream_t::splat(d, static_cast<storage_unit>(0)),
        .state_rc                   = vec_stream_t::splat(d, static_cast<storage_unit>(0)),
        .prefix_min                 = vec_stream_t::splat(d, max_val),
        .prefix_kmer                = vec_stream_t::splat(d, static_cast<storage_unit>(0)),
        .pos_local                  = vec_stream_t::splat(d, static_cast<storage_unit>(0)),
        .last_emit_pos              = vec_stream_t::splat(d, max_val),
        .ring_buf_elem              = {},
        .ring_buf_hash_strand       = {},
        .pos_wrap_delta             = static_cast<storage_unit>(0),
        .step                       = 0,
        .ring_idx                   = 0,
        .next_wrap_step             = wrap_period,
        .v_kmer_mask                = v_kmer_mask,
        .v_alphabet_complement_xor  = v_alphabet_complement_xor,
        .v_max                      = v_max,
        .v_one                      = v_one,
        .v_pos_mask                 = v_pos_mask,
        .v_hash_kmer_mask           = v_hash_kmer_mask,
        .v_strand_clear             = v_strand_clear,
        .v_golden                   = v_golden,
        .v_wrap                     = v_wrap,
        .w                          = w,
        .k_clamped                  = k_clamped,
        .emit_warmup                = emit_warmup,
        .wrap_period                = wrap_period,
        .lanes_per_stream           = lanes_per_stream,
        .kmer_mask_scalar           = kmer_mask_scalar,
        .wrap_delta_amount          = wrap_delta_amount,
        .collector                  = collector,
    };
    {
        const auto initial_elem = vec_stream_t::splat(d, max_val);
        const auto initial_hs   = vec_stream_t::splat(d, static_cast<storage_unit>(0));
        for (size_t i = 0; i < w; ++i) {
            state.ring_buf_elem[i]        = initial_elem;
            state.ring_buf_hash_strand[i] = initial_hs;
        }
    }

    // ---- Drive iteration ----
    // The handler is a thin forwarding lambda: zero captures other than
    // `state` (by reference). [[gnu::always_inline]] [[gnu::flatten]] flatten
    // the call into the v2 fast-path, which itself is [[gnu::always_inline]]
    // [[gnu::flatten]] back into the impl. The lambda body's only operation
    // is forwarding the bundled BP vec_stream into the state struct's member
    // function; clang's inliner sees a finite parameter list (no opaque
    // capture struct) and inlines DIRECT-style.
    iterate_sequence_bp_simd_v2<NumStreams>(
        seq_view, seq_begin, seq_end, k + w - 1,
        [&]<bool is_full>(
            std::bool_constant<is_full>,
            auto const&  offsets_in,
            auto const&  d_in,
            mask_t       live_mask,
            vec_stream_t bp_v
        ) [[gnu::always_inline]] [[gnu::flatten]] {
            state.template step_one_bp_vec_stream<is_full>(
                d_in, offsets_in, live_mask, bp_v);
        },
        // No-op finish_fn — emits are flushed per-step, so the partial-tail
        // exit path has nothing extra to do.
        [](auto const&, auto const&, auto&){});
}


/**
 * Public API for the two-stack nthash collector.
 *
 * Two-axis dispatch (Phase-5):
 *
 *   1. **Static (W, K) specialisation** — the minimap2 default
 *      `(w=10, k=15)` short-circuits to a fully constexpr body; other
 *      (w, k) shapes route through one of the dynamic-K fallbacks
 *      (`(W=10, K=0)` or `(W=0, K=0)`).
 *   2. **K-aware hash unit selection** — the SIMD lane storage_unit
 *      must be wide enough to hold one K-mer's worth of bits (Phase-5
 *      §3 root-cause: a `ushl` with shift `>= lane_width` saturates
 *      to zero on ARMv8, silently zeroing `state_rc` and collapsing
 *      every emitted minimizer's hash to zero). The smallest legal
 *      lane is `R::storage_unit` when `K * bits_per_char` fits, or
 *      `uint64_t` otherwise (the bit-cast view substrate from Phase-4
 *      re-interprets the source buffer at the wider unit, so no copy
 *      and no scalar fallback is needed).
 *
 * Generic principle: pick the smallest power-of-two unit that holds
 * the K-mer. Applies to every (alphabet, base storage, K) the bitcast
 * concept admits — no per-test special cases.
 *
 * Picks `NumStreams ∈ {1, 2}` based on `seq.bp_size()` against the
 * *chosen lane's* `Lanes(d) * letters_per_unit` chunk size (so a uint64
 * lane that holds 32 letters per chunk has a different dual-stream
 * threshold than the uint32 lane that holds 16).
 *
 * `MinimizerT` is deduced from the collector argument so call sites stay
 * the same shape as before — only `sketch_collector` (which is
 * `sketch_collector_t<MinimapSketchData>`) changed under the hood.
 */
template<
    template<typename H_A> typename simd_hash_t,
    class MinimizerT,
    supports_compact_sequence_range R,
    typename I = decltype(compact_sequence_range<R>().begin())
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

    using seq_traits           = compact_sequence_range<R>::traits;
    using base_storage_unit    = typename seq_traits::storage_unit;
    constexpr size_t base_bits = sizeof(base_storage_unit) * 8;
    constexpr size_t bpc       = seq_traits::bits_per_char;

    // Stage 1 — given the lane unit and (StaticW, StaticK), pick
    // NumStreams and forward to the impl. The dual-stream eligibility
    // threshold is derived from the *lane's* `Lanes(d)` so a wider lane
    // (uint64) sees a higher threshold (`Lanes` halves but
    // `letters_per_unit` doubles, so the per-chunk BPs come out the same
    // — but use the lane's own counts to avoid surprise on platforms
    // where `Lanes` differs).
    auto dispatch_with_unit =
        [&]<typename hash_unit_t, size_t StaticW, size_t StaticK>() {
            using unit_d_t = hn::ScalableTag<hash_unit_t>;
            constexpr size_t lpu_unit =
                sizeof(hash_unit_t) * 8 / bpc;
            const size_t lanes_per_stream_unit = hn::Lanes(unit_d_t{});
            const size_t context_bps           = k + w - 1;
            const size_t min_seq_for_dual_unit =
                2 * lanes_per_stream_unit * lpu_unit + context_bps;
            const bool use_dual_unit =
                (seq.bp_size() >= min_seq_for_dual_unit);

            if (use_dual_unit) {
                collect_minimizers_two_stack_nthash_simd_impl<
                    StaticW, StaticK,
                    /*NumStreams=*/2, /*DetectPalindrome=*/false,
                    simd_hash_t, MinimizerT, R, I, hash_unit_t>(
                        collector, seq, seq_begin, seq_end, w, k);
            } else {
                collect_minimizers_two_stack_nthash_simd_impl<
                    StaticW, StaticK,
                    /*NumStreams=*/1, /*DetectPalindrome=*/true,
                    simd_hash_t, MinimizerT, R, I, hash_unit_t>(
                        collector, seq, seq_begin, seq_end, w, k);
            }
        };

    // Stage 2 — given (StaticW, StaticK), pick the lane unit. The
    // K-fits-in-lane test is `K * bpc <= lane_width`: state_fwd
    // accumulates K letters into the lane (so K*bpc bits must fit),
    // and state_rc shifts the new high letter by `(K-1)*bpc` (which is
    // strictly less than `K*bpc`, so the shift is in-range whenever
    // state_fwd fits). For dynamic K (StaticK == 0) the runtime k
    // value drives the choice; uint64 covers every K up to 32 for
    // dna4 (the SIMD ceiling on AArch64; > 64-bit lanes are out of
    // NEON's reach today).
    auto dispatch_with_k_aware_unit =
        [&]<size_t StaticW, size_t StaticK>() {
            if constexpr (StaticK > 0) {
                constexpr size_t kmer_bits_static = StaticK * bpc;
                if constexpr (kmer_bits_static <= base_bits) {
                    dispatch_with_unit.template operator()<
                        base_storage_unit, StaticW, StaticK>();
                } else if constexpr (kmer_bits_static <= 64) {
                    dispatch_with_unit.template operator()<
                        uint64_t, StaticW, StaticK>();
                } else {
                    static_assert(kmer_bits_static <= 64,
                        "Static K-mer doesn't fit in any SIMD lane "
                        "we support today (uint64_t = 64 bits is the "
                        "ceiling on AArch64 NEON). Bench grid never "
                        "exercises this; either widen K or extend the "
                        "lane vocabulary.");
                }
            } else {
                const size_t kmer_bits = k * bpc;
                if (kmer_bits <= base_bits) {
                    dispatch_with_unit.template operator()<
                        base_storage_unit, StaticW, 0>();
                } else if (kmer_bits <= 64) {
                    dispatch_with_unit.template operator()<
                        uint64_t, StaticW, 0>();
                } else {
                    DEBUG_ASSERT(false, fmt::format(
                        "two_stack SIMD: kmer_bits={} exceeds uint64 "
                        "lane (k={}, bits_per_char={}). Need a wider "
                        "lane vocabulary or a scalar fallback.",
                        kmer_bits, k, bpc));
                }
            }
        };

    // Stage 3 — top-level (W, K) static dispatch. The minimap2 default
    // (w=10, k=15) short-circuits to the fully static specialisation.
    if (w == 10 && k == 15) [[likely]] {
        dispatch_with_k_aware_unit.template operator()<10, 15>();
        return;
    }
    if (w == 10) {
        dispatch_with_k_aware_unit.template operator()<10, 0>();
        return;
    }
    dispatch_with_k_aware_unit.template operator()<0, 0>();
}


} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();

}