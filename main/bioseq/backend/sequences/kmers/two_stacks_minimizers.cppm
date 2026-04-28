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
 * footprint of the three parallel ring buffers (elem / full-hash /
 * strand) that the algorithm maintains. If you need a larger window,
 * raise this number AND verify that the caller still fits inside the
 * stack budget for `bioseq_bench` / the Postgres extension.
 */
constexpr size_t two_stack_max_window = 256;

/**
 * SIMD minimizer collector that mirrors the SIMD-Minimizers paper
 * (Curiel & Buchin 2025) and its Rust reference implementation
 * `_simd_minimizers/src/sliding_min.rs` / `collect.rs`.
 *
 * Algorithm overview
 * ------------------
 * The original `collect_minimizers_simd()` translates minimap2's scalar
 * "current min + rescan when the saved min slides off the window" loop
 * directly to lanes. Branchlessly, that becomes "if the saved min for
 * ANY lane elapsed, run the rescan" — which fires `Lanes(d)` times more
 * often than the scalar branch and statistically defeats branch
 * prediction. The SIMD-Minimizers paper sidesteps this entirely by
 * keeping per-lane prefix-min + suffix-min state inside a ring buffer:
 *
 *   prefix_min        = running min of all elements pushed since the
 *                       last suffix-rebuild
 *   ring_buf[w]       = circular buffer; once the buffer fills, slot
 *                       i is rewritten as min(slot[i..w-1])
 *   window_min        = min(prefix_min, ring_buf[next_write_idx])
 *
 * The only conditional in the inner loop is "every w iterations,
 * rebuild the suffix" — fires uniformly across lanes, eliminating the
 * `AllFalse(rescan)` cost. The Rust paper / blog post claims this is
 * 30× faster than scalar for whole-genome sketching; the `bench` part
 * of `_simd_minimizers/bench` reports ~1-2 cycles per base in the inner
 * loop on AVX2.
 *
 * Element packing
 * ---------------
 * Each per-lane SIMD lane holds a single `storage_unit` (= the bit-pack
 * width inferred from the sequence traits — `uint32_t` on the CCS bench
 * configuration, giving 4 lanes on 128-bit NEON; `uint64_t` collapses
 * to 2 lanes and is therefore avoided in the bench).
 *
 *   elem = (hash & HASH_MASK) | local_pos
 *
 * where `local_pos` is a per-lane 16-bit (for uint32 storage) /
 * 32-bit (for uint64) counter that increments uniformly each step.
 * Tie-breaking by smaller `local_pos` is automatic via integer ordering
 * (the lower bits hold the position). The full kmer hash AND the per-
 * lane strand bit are tracked in two parallel ring buffers
 * (`ring_buf_full_hash` / `ring_buf_strand`) so that on emission we
 * can recover the original 32-bit hash and forward / RC bit without
 * re-hashing from the sequence.
 *
 * Position wraparound
 * -------------------
 * `local_pos` is bounded by `POS_MASK`. When any lane's `pos16`
 * approaches that bound we subtract a uniform delta from `pos16`,
 * `prefix_min`, every ring-buffer slot, and `last_emit_pos`, then
 * carry the delta in a per-call `pos_wrap_delta` scalar that gets
 * folded back into the absolute position at emit time. This mirrors
 * the Rust impl's `reset_positions_offsets` (sliding_min.rs:286-299).
 *
 * Lane boundary deduplication
 * ---------------------------
 * Lanes process disjoint slices of the sequence with a `k+w-1` bp
 * overlap (the `iterate_sequence_bp_simd_offsets` `context_bps`).
 * Within a lane, "adjacent equal positions" are deduplicated by
 * tracking `last_emit_pos`. Across lanes, the first lane's last emit
 * may equal the second lane's first emit; the sketch_collector and
 * its downstream `extract_pairs()` already deduplicate by
 * `(last_pos, kmer)`, so cross-lane duplicates are absorbed there.
 * The `minimap_port` consumer also tolerates duplicate seeds (it just
 * costs a few wasted hash-table lookups).
 *
 * Memory
 * ------
 * Three SIMD-vector arrays of size `two_stack_max_window` each. For
 * 16-byte vectors that's 3 × 256 × 16 = 12 KB on the stack; comfortably
 * fits in L1 alongside the ring buffer of the iteration scaffold.
 */
/**
 * Implementation of `collect_minimizers_two_stack_simd`. Templated on
 * the static window size `StaticW`: when caller knows `w` at compile
 * time (e.g. minimap2's default `w == 10`) we instantiate this with
 * `StaticW = w`, which lets the compiler:
 *   * fully unroll the suffix-rebuild loop (10 iterations of constant
 *     index arithmetic — the IfThenElse cascade folds into ~30
 *     constant-offset SIMD ops per rebuild),
 *   * keep all `StaticW` ring-buffer slots in vector registers (the
 *     small fixed-size `std::array` no longer escapes to the stack),
 *   * eliminate the `if (ring_idx == w)` branch from the steady-state
 *     part of the loop and inline the rebuild as a synchronous step
 *     every `StaticW` iterations.
 *
 * For dynamic-w callers, instantiate with `StaticW = 0` and pass `w`
 * as the runtime argument. The runtime path gives up the unrolling
 * but still gets the rest of the algorithm.
 */
template<
    size_t StaticW,
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
    const size_t k
) {
    // When StaticW != 0, w is a compile-time constant. The branch is
    // resolved at template instantiation; the runtime value is only
    // used in the StaticW == 0 path.
    constexpr bool has_static_w = (StaticW != 0);
    const size_t w = has_static_w ? StaticW : w_runtime;

    using seq_traits = compact_sequence_range<R>::traits;
    using storage_unit = typename seq_traits::storage_unit;

    using d_t = hn::ScalableTag<storage_unit>;
    const d_t d;
    using vec_t = hn::Vec<d_t>;
    using mask_t = hn::Mask<d_t>;

    using hasher_t = simd_hash_t<typename seq_traits::alphabet_type>::template simd_hash<storage_unit, d_t>;
    hasher_t hasher(d, k);

    // ---- Element-packing constants -------------------------------------
    // For storage_unit = uint32_t, HASH_BITS = 16 (16 hash + 16 pos).
    // For storage_unit = uint64_t, HASH_BITS = 32 (32 hash + 32 pos).
    // For storage_unit = uint16_t, HASH_BITS = 8  (8 hash  + 8  pos) —
    // the 8-bit position cap means the wraparound logic kicks in often
    // for long sequences, but the small-storage tests are the only
    // place where this matters.
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

    // Two parallel ring buffers indexed by `ring_idx`. We deliberately
    // keep them as `std::array` of compile-time max size so the inner
    // loop can address them with constant offsets (the inner-loop suffix
    // rebuild walks `[i = w-1; i > 0; --i]` and the compiler unrolls /
    // vectorises the address arithmetic when the index type is constexpr).
    //
    // Strand is packed into the LSB of `ring_buf_full_hash` to avoid a
    // third per-step SIMD store + IfThenElse pair. Encoding scheme:
    //   slot = (hash_full & ~1u) | strand_bit
    // Decoding (emit time):
    //   kmer = slot & ~1u                    // 31-bit hash
    //   strand = slot & 1u
    // We lose one bit of hash precision but minimizer matching is fine
    // because (a) both query and target apply the identical encoding
    // and (b) the resulting 31-bit hash space still has plenty of
    // entropy for the post-scramble minimap_int hasher. The strand bit
    // is NOT compared during the prefix/suffix-min selection (we mask
    // it off via `v_hash_mask` when building `elem`), so its presence
    // in the LSB does not perturb tie-breaking.
    // Static-w path uses small fixed arrays so the compiler keeps them
    // in registers; dynamic-w path falls back to the maximum-w buffer.
    constexpr size_t buf_capacity = has_static_w ? StaticW : two_stack_max_window;
    HWY_ALIGN std::array<vec_t, buf_capacity> ring_buf_elem;
    HWY_ALIGN std::array<vec_t, buf_capacity> ring_buf_hash_strand;
    for (size_t i = 0; i < w; ++i) {
        ring_buf_elem[i] = v_max;
        ring_buf_hash_strand[i] = v_zero;
    }

    // Running prefix-min: the elem (used for tie-broken comparison) and
    // the (hash | strand_bit) packing for the winning lane.
    vec_t prefix_min = v_max;
    vec_t prefix_min_hash_strand = v_zero;

    // Per-lane local position (bottom hash_bits bits of `elem`).
    vec_t pos_local = v_zero;
    // Per-lane "last emitted local-pos" tracking — the dedup-adjacent-
    // equals invariant means we suppress emission whenever the new
    // window minimum's local-pos matches the previous emission.
    vec_t last_emit_pos = v_max;
    // Wrap-around delta accumulated across position resets. Scalar
    // because `pos_local` increments uniformly across lanes.
    storage_unit pos_wrap_delta = 0;

    // ---- Outer loop counters (uniform across lanes) -------------------
    size_t step = 0;
    size_t ring_idx = 0;
    constexpr storage_unit pos_max = pos_mask;
    // Reset interval: subtract `wrap_delta_amount` once we approach the
    // top of the per-lane local-pos counter. Pick a single delta value
    // and use it both for the initial fire and the recurrence so the
    // first interval matches subsequent ones (off-by-one bug fixed
    // here vs the original sketch).
    const storage_unit wrap_delta_amount = static_cast<storage_unit>(pos_max - static_cast<storage_unit>(w) - 1);
    const size_t wrap_period = static_cast<size_t>(wrap_delta_amount);

    // Pre-compute the next iteration `step` at which the wraparound
    // logic must fire. Cheaper than a `step % pos_max` per step (the
    // compiler doesn't know `pos_max` is power-of-two-minus-one in all
    // build configs and emits a real divide on some targets).
    size_t next_wrap_step = wrap_period;

    // The per-step "warmup" threshold: emission must wait for the first
    // full window to have been observed. The hasher needs k chars to
    // produce a valid kmer; the window needs w more to be filled. So
    // emission is enabled for `step >= k + w - 1`.
    const size_t emit_warmup = k + w - 1;

    // Constant: clears the LSB used for the strand-bit packing so
    // we can encode `(hash & ~1) | strand_bit` cheaply.
    const vec_t v_strand_clear = hn::Set(d, static_cast<storage_unit>(~static_cast<storage_unit>(1)));

    iterate_sequence_bp_simd_offsets(seq, seq_begin, seq_end, k + w - 1,
        [&]<bool is_full>(std::bool_constant<is_full>, auto const& offsets, auto& d_local, auto& mask, auto&& incoming) {
        // Compute hash for this step (lane-parallel).
        // The `outgoing` parameter of the rolling hash represents the
        // character being added on the RC side of the kmer; that's the
        // same `incoming` character (just read from the other strand's
        // perspective). The Highway hasher's `next(d, incoming, outgoing)`
        // applies a complement on `outgoing` so passing `incoming` for
        // both arguments is *not* equivalent to the existing
        // `collect_minimizers_simd` path — but for the canonical
        // `lexic_hasher` / `minimap_int_hasher` chain the rolling-RC
        // update assumes `outgoing == incoming` because the hasher does
        // its own delay-by-k internally via the bit-shift schedule. We
        // therefore avoid the `iterate_sequence_bp_simd_offsets` delay
        // infrastructure entirely (delays_count = 0 → no per-iter
        // delay-state update, no `delays_masks` array, no per-delay
        // structured-binding read), saving ~5 SIMD ops per outer step.
        const vec_t hash_full = hasher.next(d_local, incoming, incoming);
        const mask_t palindrome_mask = hasher.get_palindrome_mask(d_local);
        const mask_t strand_mask = hasher.get_strand_mask(d_local);

        // Mask out invalid lanes (palindrome OR dead due to outer mask).
        const mask_t valid_mask = hn::AndNot(palindrome_mask, mask);

        // Pack the strand bit into the LSB of `hash_full` so we can
        // carry both the kmer (top 31 bits) and strand (bottom 1 bit)
        // through the ring buffer with a single SIMD store per step
        // (instead of two — see ring_buf_hash_strand rationale above).
        // Fused expression: `IfThenElse(strand, hash | 1, hash & ~1)`
        // collapses one IfThenElse + one And + one Or into IfThenElse +
        // Or + IfThenElse on most Highway targets, saving one SIMD op.
        const vec_t hash_strand = hn::IfThenElse(strand_mask,
            hn::Or(hash_full, v_one),
            hn::And(hash_full, v_strand_clear));

        // elem = (hash & HASH_MASK) | pos_local.
        // The high half holds the (post-scramble) hash bits used for
        // ordering; the low half holds the per-lane local position
        // for tie-break. Both halves are masked off the strand bit
        // (since the strand sits in bit 0 of `hash_full`, which is
        // outside the upper 16 bits we keep for ordering).
        // Invalid lanes: force elem to v_max so they're never chosen as
        // the window minimum and never emitted.
        const vec_t elem_valid = hn::Or(hn::And(hash_full, v_hash_mask), pos_local);
        const vec_t elem = hn::IfThenElse(valid_mask, elem_valid, v_max);

        // Push to ring buffers at the current write position. Two
        // stores per step (vs three in V0): elem for ordering,
        // hash-with-strand for emission.
        ring_buf_elem[ring_idx] = elem;
        ring_buf_hash_strand[ring_idx] = hash_strand;

        // prefix_min update — keep the (hash, strand) of the "winner".
        // Use Lt (strict) so equal lanes leave the older pos unchanged
        // (matches scalar minimap2's "first occurrence wins" tiebreak).
        const mask_t prefix_better = hn::Lt(elem, prefix_min);
        prefix_min = hn::IfThenElse(prefix_better, elem, prefix_min);
        prefix_min_hash_strand = hn::IfThenElse(prefix_better, hash_strand, prefix_min_hash_strand);

        ++ring_idx;
        ++step;
        pos_local = hn::Add(pos_local, v_one);

        // Suffix rebuild every `w` iterations. The chunk boundary fires
        // BEFORE we evaluate the window-min for this step so the new
        // suffix-min ranges overlap correctly with `prefix_min` (which
        // resets to the current elem). `[[unlikely]]` because the
        // rebuild fires once per w iterations (default w = 10 → ~10 %
        // of steps).
        if (ring_idx == w) [[unlikely]] {
            // Walk backwards: ring_buf_elem[i-1] := min(slot[i-1..w-1]).
            // Using Lt (strict <) so equal elements keep the SMALLER
            // slot index — i.e., the EARLIER pos16 wins on ties,
            // matching minimap2's tie-break.
            //
            // When StaticW is known (e.g. 10) we recurse on a
            // compile-time constant, letting the compiler unroll the
            // body into ~30 constant-offset SIMD ops with no loop
            // counter overhead. When StaticW == 0 we fall through to
            // a runtime-bounded loop.
            if constexpr (has_static_w) {
                [&]<size_t... Idx>(std::index_sequence<Idx...>) {
                    // Idx values: 0, 1, ..., StaticW - 2. We want to walk
                    // i = StaticW - 1 down to 1, writing ring_buf[i-1].
                    // That is i' = StaticW - 1 - Idx for Idx = 0..StaticW-2.
                    auto rebuild_one = [&](auto i_const) {
                        constexpr size_t i = StaticW - 1 - decltype(i_const)::value;
                        const vec_t cur = ring_buf_elem[i];
                        const vec_t prev = ring_buf_elem[i - 1];
                        const mask_t cur_better = hn::Lt(cur, prev);
                        ring_buf_elem[i - 1] = hn::IfThenElse(cur_better, cur, prev);
                        ring_buf_hash_strand[i - 1] = hn::IfThenElse(cur_better, ring_buf_hash_strand[i], ring_buf_hash_strand[i - 1]);
                    };
                    (rebuild_one(std::integral_constant<size_t, Idx>{}), ...);
                }(std::make_index_sequence<StaticW - 1>{});
            } else {
                for (size_t i = w - 1; i > 0; --i) {
                    const vec_t cur = ring_buf_elem[i];
                    const vec_t prev = ring_buf_elem[i - 1];
                    const mask_t cur_better = hn::Lt(cur, prev);
                    ring_buf_elem[i - 1] = hn::IfThenElse(cur_better, cur, prev);
                    ring_buf_hash_strand[i - 1] = hn::IfThenElse(cur_better, ring_buf_hash_strand[i], ring_buf_hash_strand[i - 1]);
                }
            }
            // Reset prefix_min to the just-pushed element. The prefix_min
            // logically tracks "the best lane since the last rebuild";
            // its scope shrinks to {elem}.
            prefix_min = elem;
            prefix_min_hash_strand = hash_strand;
            ring_idx = 0;
        }

        // Window minimum = min(prefix_min, ring_buf_elem[ring_idx]).
        // ring_buf_elem[ring_idx] is the suffix-min from slot ring_idx
        // (oldest still-live slot in the previous chunk) to w-1.
        // Together they cover exactly the last w pushed elements.
        const vec_t suffix_min = ring_buf_elem[ring_idx];
        const vec_t suffix_hash_strand = ring_buf_hash_strand[ring_idx];

        const mask_t prefix_wins = hn::Lt(prefix_min, suffix_min);
        const vec_t window_min = hn::IfThenElse(prefix_wins, prefix_min, suffix_min);
        const vec_t window_hash_strand = hn::IfThenElse(prefix_wins, prefix_min_hash_strand, suffix_hash_strand);

        // Position wraparound. Bounded by pos_mask. With uint32 storage
        // the bound is ~65 535 iterations, so for a 1 Mbp / 4-lane run
        // (~250 k iterations per lane) we expect ~3-4 wraps per call.
        // We use a precomputed `next_wrap_step` rather than `step %
        // pos_max` because the compiler doesn't always reduce the modulo
        // to a mask (pos_max is `(1 << HASH_BITS) - 1`, which is
        // power-of-two-minus-one — a single AND would suffice but only
        // when the compiler proves it).
        if (step == next_wrap_step) [[unlikely]] {
            const vec_t v_wrap = hn::Set(d_local, wrap_delta_amount);
            pos_local = hn::Sub(pos_local, v_wrap);
            prefix_min = hn::Sub(prefix_min, v_wrap);
            // last_emit_pos wraps too — but lanes that never emitted
            // (still v_max) must stay at v_max, so we mask the subtract.
            const mask_t emit_seen = hn::Ne(last_emit_pos, v_max);
            last_emit_pos = hn::IfThenElse(emit_seen,
                hn::Sub(last_emit_pos, v_wrap), last_emit_pos);
            if constexpr (has_static_w) {
                [&]<size_t... Idx>(std::index_sequence<Idx...>) {
                    auto wrap_slot = [&](auto i_const) {
                        constexpr size_t i = decltype(i_const)::value;
                        const mask_t slot_valid = hn::Ne(ring_buf_elem[i], v_max);
                        ring_buf_elem[i] = hn::IfThenElse(slot_valid,
                            hn::Sub(ring_buf_elem[i], v_wrap), ring_buf_elem[i]);
                    };
                    (wrap_slot(std::integral_constant<size_t, Idx>{}), ...);
                }(std::make_index_sequence<StaticW>{});
            } else {
                for (size_t i = 0; i < w; ++i) {
                    // Same masking: sentinel slots (v_max) must stay at
                    // v_max so the suffix-min logic keeps treating them as
                    // "no valid value here yet".
                    const mask_t slot_valid = hn::Ne(ring_buf_elem[i], v_max);
                    ring_buf_elem[i] = hn::IfThenElse(slot_valid,
                        hn::Sub(ring_buf_elem[i], v_wrap), ring_buf_elem[i]);
                }
            }
            pos_wrap_delta = static_cast<storage_unit>(pos_wrap_delta + wrap_delta_amount);
            next_wrap_step += wrap_period;
        }

        // Emit when (a) past warmup and (b) any lane's window-min pos
        // changed AND (c) window_min is real (not v_max sentinel from
        // dead / palindromic lanes).
        if (step >= emit_warmup) {
            const vec_t window_pos = hn::And(window_min, v_pos_mask);
            const mask_t pos_changed = hn::Ne(window_pos, last_emit_pos);
            const mask_t valid_window = hn::Ne(window_min, v_max);
            const mask_t real_emit = hn::And(pos_changed, valid_window);

            if (!hn::AllFalse(d_local, real_emit)) [[likely]] {
                // Scalar lane loop. With `Lanes(d) <= 16` the loop is
                // tiny; the `if` branch on `emit_arr[lane]` short-
                // circuits the empty lanes.
                HWY_ALIGN std::array<storage_unit, hn::MaxLanes(d_local)> pos_arr;
                HWY_ALIGN std::array<storage_unit, hn::MaxLanes(d_local)> hash_strand_arr;
                HWY_ALIGN std::array<storage_unit, hn::MaxLanes(d_local)> emit_arr;
                hn::Store(window_pos, d_local, pos_arr.data());
                hn::Store(window_hash_strand, d_local, hash_strand_arr.data());
                hn::Store(hn::VecFromMask(d_local, real_emit), d_local, emit_arr.data());

                const size_t lane_count = hn::Lanes(d_local);
                for (size_t lane = 0; lane < lane_count; ++lane) {
                    if (!emit_arr[lane]) continue;
                    const size_t local_pos =
                        static_cast<size_t>(pos_arr[lane]) +
                        static_cast<size_t>(pos_wrap_delta);
                    // The hasher consumes chars [lane_start, lane_start + step)
                    // by step `step`. The kmer ending at local_pos is at
                    // sequence position `offsets[lane] * letters_per_unit + local_pos`.
                    const size_t abs_pos =
                        offsets[lane] * seq_traits::letters_per_unit + local_pos;
                    // Unpack the (kmer, strand) pair from the LSB-packed
                    // hash. The downstream pipeline uses the kmer as an
                    // index key; clearing the LSB ensures forward and
                    // reverse-strand kmers of the same canonical
                    // sequence collide on lookup (the index lookup uses
                    // exact-match on the 31-bit hash).
                    const storage_unit hash_strand_val = hash_strand_arr[lane];
                    collector.collect(MinimapSketch({
                        .sketch{ .sketch{
                            .kmer = static_cast<uint64_t>(hash_strand_val & ~static_cast<storage_unit>(1)),
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
    [&](auto const& offsets, auto& d_local, auto& mask) {
        // Tail handling. Each lane's last real iteration already emitted
        // its window minimum in the main loop (the dedup-by-pos rule
        // skips the trailing sentinel-padded iterations). The Rust
        // reference impl is identical — `padded_it.advance` rolls the
        // iterator past the per-lane tail without firing emission.
        (void)offsets;
        (void)d_local;
        (void)mask;
    });
}

/**
 * User-facing wrapper. Dispatches to the static-w specialization for
 * the minimap2 default `w == 10`, falling back to the runtime-w path
 * otherwise. The dispatch happens once per call (not per step), so its
 * cost is amortised across the ~1 Mbp of input.
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

    // Hot path: minimap2's default `w == 10`. The static-w
    // specialization unrolls the suffix-rebuild and lets the compiler
    // keep the ring-buffer slots in vector registers.
    if (w == 10) [[likely]] {
        collect_minimizers_two_stack_simd_impl<10, simd_hash_t, R, I, hash_val_t>(
            collector, seq, seq_begin, seq_end, w, k);
        return;
    }
    // Cold path: runtime-w fallback. Used for unit tests / non-default
    // window sizes; the ring buffers live in `std::array` of
    // `two_stack_max_window` so the algorithm still works correctly,
    // just without the unroll wins.
    collect_minimizers_two_stack_simd_impl<0, simd_hash_t, R, I, hash_val_t>(
        collector, seq, seq_begin, seq_end, w, k);
}

} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();

}
