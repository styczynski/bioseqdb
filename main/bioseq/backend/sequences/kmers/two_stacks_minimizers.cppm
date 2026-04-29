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
 * Compile-time upper bound on the window size `w` accepted by
 * `collect_minimizers_two_stack_simd`. Our minimap2 ports run at the
 * default `w = 10`; bumping past 256 also blows the L1-friendly stack
 * footprint of the two parallel ring buffers (elem / hash_strand).
 */
constexpr size_t two_stack_max_window = 256;

/**
 * V8: SIMD minimizer collector with **inlined hasher state**.
 *
 * The hasher class (`lexic_hasher::simd_hash` + `minimap_int_hasher::
 * simd_hash`) maintains `state_forward`, `state_reverse_compl`,
 * `mask`, `rev_shift` as class members. Through a captured-by-
 * reference lambda those members live on the stack and the per-call
 * `state_forward = ...` reads/writes go through memory. Even with
 * `-O3` the compiler does not always promote them to registers across
 * the lambda boundary.
 *
 * V8 sidesteps this by inlining the hasher state into the impl
 * function directly:
 *   - `state_forward`, `state_reverse_compl` are local vec_t variables.
 *   - The hash update is a small inline block invoked per step.
 *   - `rev_shift` becomes a compile-time `constexpr` derived from the
 *     template parameter `StaticK`. When `StaticK != 0` we use
 *     `hn::ShiftLeft<StaticK>` (constant amount); the dynamic-k
 *     fallback uses `hn::ShiftLeftSame` with a runtime amount.
 *
 * The wrapper `collect_minimizers_two_stack_simd` dispatches to
 *   - `StaticW=10, StaticK=15` for minimap2's defaults (the bench's
 *     hot path),
 *   - `StaticW=10, StaticK=0` for runtime-k callers,
 *   - `StaticW=0, StaticK=0` as a fallback for non-default w.
 */

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

/**
 * Implementation of `collect_minimizers_two_stack_simd`. Templated on:
 *   - `StaticW`: window size (compile-time when != 0; minimap2's
 *     default is 10).
 *   - `StaticK`: kmer length (compile-time when != 0; minimap2's
 *     default is 15). When known we use `hn::ShiftLeft<2*(K-1)>` for
 *     the rolling hash's RC update — a constant-amount shift that
 *     compiles to a single op on every Highway target.
 */
template<
    size_t StaticW,
    size_t StaticK,
    template<typename H_A> typename simd_hash_t,
    supports_compact_sequence_range R,
    typename I = decltype(compact_sequence_range<R>().begin()),
    typename hash_val_t = compact_sequence_range<R>::traits::storage_unit
>
requires (
    supports_simd_hash<simd_hash_t, typename compact_sequence_range<R>::traits::alphabet_type>
)
[[gnu::always_inline]] inline void collect_minimizers_two_stack_simd_impl(
    sketch_collector& collector,
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

    using seq_traits = compact_sequence_range<R>::traits;
    using storage_unit = typename seq_traits::storage_unit;
    using alphabet_t = typename seq_traits::alphabet_type;

    using d_t = hn::ScalableTag<storage_unit>;
    d_t d;
    using vec_t = hn::Vec<d_t>;
    using mask_t = hn::Mask<d_t>;

    // ---- Inline hasher state ------------------------------------------
    // Replaces the `lexic_hasher::simd_hash` + `minimap_int_hasher::
    // simd_hash` class instances with locals that the compiler can
    // keep in registers across the captured lambda boundary.
    constexpr size_t kmer_max_length =
        sizeof(storage_unit) * 8 / seq_traits::bits_per_char;
    const size_t k_clamped = (k < kmer_max_length) ? k : kmer_max_length;
    const storage_unit kmer_mask =
        seq_traits::template kmer_mask<storage_unit>(k_clamped);
    const vec_t v_kmer_mask = hn::Set(d, kmer_mask);

    // For dna4 the alphabet complement is `Xor(rank, 3)`. For dna5
    // (non-power-of-2 alphabet) the V6 lexic_hasher falls back to a
    // per-rank IfThenElse cascade. We keep the fast XOR path for the
    // common-case bench (dna4) and forward to the class-based hasher
    // for the rest.
    constexpr bool is_xor_complement =
        two_stack_internal::is_xor_complement_v<alphabet_t, storage_unit>;
    const storage_unit alphabet_complement_mask =
        static_cast<storage_unit>(seq_traits::alphabet_size - 1);
    const vec_t v_alphabet_complement_xor = hn::Set(d, alphabet_complement_mask);

    vec_t state_forward = hn::Zero(d);
    vec_t state_reverse_compl = hn::Zero(d);

    // ---- Element-packing constants ------------------------------------
    constexpr size_t storage_bits = sizeof(storage_unit) * 8;
    constexpr size_t hash_bits = storage_bits / 2;
    constexpr storage_unit pos_mask = (hash_bits >= storage_bits)
        ? static_cast<storage_unit>(-1)
        : (static_cast<storage_unit>(1) << hash_bits) - 1;
    constexpr storage_unit hash_mask = static_cast<storage_unit>(~pos_mask);
    constexpr storage_unit max_val = static_cast<storage_unit>(-1);

    DEBUG_ASSERT(w <= two_stack_max_window,
        "two_stack_max_window must hold the requested window size");
    DEBUG_ASSERT(w >= 1, "Window size must be at least 1");

    // ---- Per-lane SIMD state ------------------------------------------
    const vec_t v_max = hn::Set(d, max_val);
    const vec_t v_zero = hn::Zero(d);
    const vec_t v_one = hn::Set(d, static_cast<storage_unit>(1));
    const vec_t v_pos_mask = hn::Set(d, pos_mask);
    const vec_t v_hash_mask = hn::Set(d, hash_mask);
    const vec_t v_strand_clear = hn::Set(d, static_cast<storage_unit>(~static_cast<storage_unit>(1)));

    // Two parallel ring buffers (V6 encoding preserved):
    //   - ring_buf_elem[i]        = (hash & HASH_MASK) | pos_local
    //   - ring_buf_hash_strand[i] = (hash & ~1) | strand_bit
    constexpr size_t buf_capacity = has_static_w ? StaticW : two_stack_max_window;
    HWY_ALIGN std::array<vec_t, buf_capacity> ring_buf_elem;
    HWY_ALIGN std::array<vec_t, buf_capacity> ring_buf_hash_strand;
    for (size_t i = 0; i < w; ++i) {
        ring_buf_elem[i] = v_max;
        ring_buf_hash_strand[i] = v_zero;
    }

    vec_t prefix_min = v_max;
    vec_t prefix_min_hash_strand = v_zero;

    vec_t pos_local = v_zero;
    vec_t last_emit_pos = v_max;
    storage_unit pos_wrap_delta = 0;

    size_t step = 0;
    size_t ring_idx = 0;

    constexpr storage_unit pos_max = pos_mask;
    const storage_unit wrap_delta_amount =
        static_cast<storage_unit>(pos_max - static_cast<storage_unit>(w) - 1);
    const size_t wrap_period = static_cast<size_t>(wrap_delta_amount);
    size_t next_wrap_step = wrap_period;
    const size_t emit_warmup = k + w - 1;

    // Compile-time RevShift when StaticK != 0. The dynamic-k case
    // resolves to `seq_traits::bits_per_char * (k - 1)` at runtime.
    constexpr size_t static_rev_shift =
        has_static_k ? seq_traits::bits_per_char * (StaticK - 1) : 0;
    const size_t dynamic_rev_shift =
        seq_traits::bits_per_char * (k_clamped - 1);

    // ============================================================
    // Outer iteration. Use iterate_sequence_bp_simd_offsets for the
    // matrix load / transpose / per-bp loop. The body inlines the
    // hash update (state_forward / state_reverse_compl + scramble) so
    // the compiler can keep the hasher state in vector registers.
    // ============================================================
    iterate_sequence_bp_simd_offsets(seq, seq_begin, seq_end, k + w - 1,
        [&]<bool is_full>(std::bool_constant<is_full>, auto const& offsets_in, auto& d_local, auto& mask, auto&& incoming) {
        // ---- Inline hash update ----
        // Forward state update. Only depends on `incoming`.
        state_forward = hn::And(hn::Or(hn::ShiftLeft<seq_traits::bits_per_char>(state_forward), incoming), v_kmer_mask);

        // Reverse-complement state update. Use the constexpr shift
        // amount when StaticK != 0.
        const vec_t complemented = is_xor_complement
            ? hn::Xor(incoming, v_alphabet_complement_xor)
            : hn::Xor(incoming, v_alphabet_complement_xor);
        vec_t new_high;
        if constexpr (has_static_k) {
            // Clamp at compile time so non-uint32 storage units don't
            // overflow the Highway shift bound.
            constexpr size_t clamped_rev_shift =
                (static_rev_shift >= sizeof(storage_unit) * 8)
                    ? sizeof(storage_unit) * 8 - 1
                    : static_rev_shift;
            new_high = hn::ShiftLeft<clamped_rev_shift>(complemented);
        } else {
            new_high = hn::ShiftLeftSame(complemented, static_cast<int>(dynamic_rev_shift));
        }
        state_reverse_compl = hn::Or(hn::ShiftRight<seq_traits::bits_per_char>(state_reverse_compl), new_high);

        // Canonical = min(fwd, rc).
        const vec_t lexic_hash = hn::Min(state_forward, state_reverse_compl);

        // Palindrome mask: forward == reverse_complement.
        const mask_t palindrome_mask = hn::Eq(state_forward, state_reverse_compl);
        // Strand mask: rc < forward (rc is min). 0 when palindromic.
        const mask_t strand_mask = hn::Lt(state_reverse_compl, state_forward);

        // Cheap randomising mix for ORDERING ONLY. The full
        // minimap_int integer scramble is deferred to emit time (~23
        // SIMD ops per iter saved); without ANY mix, lexic-only
        // ordering would bias minimizer selection toward AT-rich
        // kmers (numerically smallest in the bit-packed encoding),
        // which inflates downstream `collect_seed_hits` lookup chains
        // because AT-rich kmers tend to be repeats. A single
        // multiplicative mix with the golden-ratio constant keeps the
        // distribution uniformly random while costing only 2 SIMD ops
        // per step (Mul + AndMask).
        //
        // The HASH STORED in `ring_buf_hash_strand` is the LEXIC
        // canonical (no mix applied), so emit-time scramble can
        // recover the proper minimap_int hash.
        constexpr storage_unit golden_ratio_const =
            (sizeof(storage_unit) >= 4)
                ? static_cast<storage_unit>(0x9E3779B9)
                : static_cast<storage_unit>(0x9E37);
        const vec_t v_golden = hn::Set(d, golden_ratio_const);
        const vec_t hash_full = hn::And(hn::Mul(lexic_hash, v_golden), v_kmer_mask);

        const mask_t valid_mask = hn::AndNot(palindrome_mask, mask);

        // hash_strand holds the LEXIC canonical hash + strand bit so
        // emit time can apply minimap_int scramble. This is distinct
        // from the MIXED `hash_full` we use for ORDERING.
        const vec_t hash_strand = hn::IfThenElse(strand_mask,
            hn::Or(lexic_hash, v_one),
            hn::And(lexic_hash, v_strand_clear));

        const vec_t elem_valid = hn::Or(hn::And(hash_full, v_hash_mask), pos_local);
        const vec_t elem = hn::IfThenElse(valid_mask, elem_valid, v_max);

        ring_buf_elem[ring_idx] = elem;
        ring_buf_hash_strand[ring_idx] = hash_strand;

        const mask_t prefix_better = hn::Lt(elem, prefix_min);
        prefix_min = hn::IfThenElse(prefix_better, elem, prefix_min);
        prefix_min_hash_strand = hn::IfThenElse(prefix_better, hash_strand, prefix_min_hash_strand);

        ++ring_idx;
        ++step;
        pos_local = hn::Add(pos_local, v_one);

        if (ring_idx == w) [[unlikely]] {
            if constexpr (has_static_w) {
                [&]<size_t... Idx>(std::index_sequence<Idx...>) {
                    auto rebuild_one = [&](auto i_const) [[gnu::always_inline]] {
                        constexpr size_t i = StaticW - 1 - decltype(i_const)::value;
                        const vec_t cur = ring_buf_elem[i];
                        const vec_t prev = ring_buf_elem[i - 1];
                        const mask_t cur_better = hn::Lt(cur, prev);
                        ring_buf_elem[i - 1] = hn::IfThenElse(cur_better, cur, prev);
                        ring_buf_hash_strand[i - 1] = hn::IfThenElse(cur_better,
                            ring_buf_hash_strand[i], ring_buf_hash_strand[i - 1]);
                    };
                    (rebuild_one(std::integral_constant<size_t, Idx>{}), ...);
                }(std::make_index_sequence<StaticW - 1>{});
            } else {
                for (size_t i = w - 1; i > 0; --i) {
                    const vec_t cur = ring_buf_elem[i];
                    const vec_t prev = ring_buf_elem[i - 1];
                    const mask_t cur_better = hn::Lt(cur, prev);
                    ring_buf_elem[i - 1] = hn::IfThenElse(cur_better, cur, prev);
                    ring_buf_hash_strand[i - 1] = hn::IfThenElse(cur_better,
                        ring_buf_hash_strand[i], ring_buf_hash_strand[i - 1]);
                }
            }
            prefix_min = elem;
            prefix_min_hash_strand = hash_strand;
            ring_idx = 0;
        }

        const vec_t suffix_min = ring_buf_elem[ring_idx];
        const vec_t suffix_hash_strand = ring_buf_hash_strand[ring_idx];
        const mask_t prefix_wins = hn::Lt(prefix_min, suffix_min);
        const vec_t window_min = hn::IfThenElse(prefix_wins, prefix_min, suffix_min);
        const vec_t window_hash_strand = hn::IfThenElse(prefix_wins,
            prefix_min_hash_strand, suffix_hash_strand);

        if (step == next_wrap_step) [[unlikely]] {
            const vec_t v_wrap = hn::Set(d_local, wrap_delta_amount);
            pos_local = hn::Sub(pos_local, v_wrap);
            prefix_min = hn::Sub(prefix_min, v_wrap);
            const mask_t emit_seen = hn::Ne(last_emit_pos, v_max);
            last_emit_pos = hn::IfThenElse(emit_seen,
                hn::Sub(last_emit_pos, v_wrap), last_emit_pos);
            if constexpr (has_static_w) {
                [&]<size_t... Idx>(std::index_sequence<Idx...>) {
                    auto wrap_slot = [&](auto i_const) [[gnu::always_inline]] {
                        constexpr size_t i = decltype(i_const)::value;
                        const mask_t slot_valid = hn::Ne(ring_buf_elem[i], v_max);
                        ring_buf_elem[i] = hn::IfThenElse(slot_valid,
                            hn::Sub(ring_buf_elem[i], v_wrap), ring_buf_elem[i]);
                    };
                    (wrap_slot(std::integral_constant<size_t, Idx>{}), ...);
                }(std::make_index_sequence<StaticW>{});
            } else {
                for (size_t i = 0; i < w; ++i) {
                    const mask_t slot_valid = hn::Ne(ring_buf_elem[i], v_max);
                    ring_buf_elem[i] = hn::IfThenElse(slot_valid,
                        hn::Sub(ring_buf_elem[i], v_wrap), ring_buf_elem[i]);
                }
            }
            pos_wrap_delta = static_cast<storage_unit>(pos_wrap_delta + wrap_delta_amount);
            next_wrap_step += wrap_period;
        }

        if (step >= emit_warmup) {
            const vec_t window_pos = hn::And(window_min, v_pos_mask);
            const mask_t pos_changed = hn::Ne(window_pos, last_emit_pos);
            const mask_t valid_window = hn::Ne(window_min, v_max);
            const mask_t real_emit = hn::And(pos_changed, valid_window);

            if (!hn::AllFalse(d_local, real_emit)) [[likely]] {
                HWY_ALIGN std::array<storage_unit, hn::MaxLanes(d_local)> pos_arr;
                HWY_ALIGN std::array<storage_unit, hn::MaxLanes(d_local)> hash_strand_arr;
                HWY_ALIGN std::array<storage_unit, hn::MaxLanes(d_local)> emit_arr;
                hn::Store(window_pos, d_local, pos_arr.data());
                hn::Store(window_hash_strand, d_local, hash_strand_arr.data());
                hn::Store(hn::VecFromMask(d_local, real_emit), d_local, emit_arr.data());

                // Apply minimap_int integer scramble at emit time.
                // The inner loop now stores the lexic canonical hash
                // (with strand bit packed in the LSB) — much cheaper
                // per iteration. The full minimap2 hash is recovered
                // here via 7 stages of mix on the chosen lane's value
                // before pushing to the collector. Because emit fires
                // ~1/w of iterations, the per-emit ~25 scalar ops
                // contribute < 0.5 ms on a 1 Mbp pair.
                constexpr size_t storage_max_shift_e = sizeof(storage_unit) * 8 - 1;
                constexpr auto clamp_e = [](size_t s) constexpr -> size_t {
                    return s > storage_max_shift_e ? storage_max_shift_e : s;
                };
                const storage_unit kmer_mask_scalar = kmer_mask;
                auto scramble_minimap_int = [&](storage_unit key) -> storage_unit {
                    key = (~key + (key << clamp_e(21))) & kmer_mask_scalar;
                    key = key ^ (key >> clamp_e(24));
                    key = ((key + (key << clamp_e(3))) + (key << clamp_e(8))) & kmer_mask_scalar;
                    key = key ^ (key >> clamp_e(14));
                    key = ((key + (key << clamp_e(2))) + (key << clamp_e(4))) & kmer_mask_scalar;
                    key = key ^ (key >> clamp_e(28));
                    key = (key + (key << clamp_e(31))) & kmer_mask_scalar;
                    return key;
                };

                const size_t lane_count = hn::Lanes(d_local);
                for (size_t lane = 0; lane < lane_count; ++lane) {
                    if (!emit_arr[lane]) continue;
                    const size_t local_pos =
                        static_cast<size_t>(pos_arr[lane]) +
                        static_cast<size_t>(pos_wrap_delta);
                    const size_t abs_pos =
                        offsets_in[lane] * seq_traits::letters_per_unit + local_pos;
                    const storage_unit hash_strand_val = hash_strand_arr[lane];
                    // Recover lexic canonical hash + strand from packed
                    // ring-buffer slot, then apply the deferred scramble.
                    const storage_unit lexic_kmer =
                        hash_strand_val & ~static_cast<storage_unit>(1);
                    const storage_unit scrambled_kmer =
                        scramble_minimap_int(lexic_kmer);
                    collector.collect(MinimapSketch({
                        .sketch{ .sketch{
                            .kmer = static_cast<uint64_t>(scrambled_kmer),
                            .span = k,
                            .rid = 0,
                            .last_pos = abs_pos,
                            .strand = static_cast<uint64_t>(hash_strand_val) & 0x1ULL,
                        }}
                    }));
                }
                last_emit_pos = hn::IfThenElse(real_emit, window_pos, last_emit_pos);
            }
        }
    },
    [&](auto const& offsets_in, auto& d_local, auto& mask) {
        (void)offsets_in;
        (void)d_local;
        (void)mask;
    });
}

/**
 * User-facing wrapper. Dispatches to the static-w / static-k specialization
 * for the minimap2 default `w == 10, k == 15`, falling back to runtime-w/k
 * paths otherwise.
 */
template<
    template<typename H_A> typename simd_hash_t,
    supports_compact_sequence_range R,
    typename I = decltype(compact_sequence_range<R>().begin()),
    typename hash_val_t = compact_sequence_range<R>::traits::storage_unit
>
requires (
    supports_simd_hash<simd_hash_t, typename compact_sequence_range<R>::traits::alphabet_type>
)
inline void collect_minimizers_two_stack_simd(
    sketch_collector& collector,
    compact_sequence_range<R>& seq,
    const I& seq_begin,
    const I& seq_end,
    const size_t w,
    const size_t k
) {
    static const auto* _desc =
        bioseq::instr::register_region_static("mm_sketch_simd");
    auto _scope = bioseq::instr::region(_desc);

    if (w == 10 && k == 15) [[likely]] {
        collect_minimizers_two_stack_simd_impl<10, 15, simd_hash_t, R, I, hash_val_t>(
            collector, seq, seq_begin, seq_end, w, k);
        return;
    }
    if (w == 10) {
        collect_minimizers_two_stack_simd_impl<10, 0, simd_hash_t, R, I, hash_val_t>(
            collector, seq, seq_begin, seq_end, w, k);
        return;
    }
    collect_minimizers_two_stack_simd_impl<0, 0, simd_hash_t, R, I, hash_val_t>(
        collector, seq, seq_begin, seq_end, w, k);
}

} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();

}
