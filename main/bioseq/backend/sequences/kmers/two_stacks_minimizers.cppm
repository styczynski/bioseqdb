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
// Single-stream two-stack nthash collector (internal fallback)
// ---------------------------------------------------------------------------------
// Used by `collect_minimizers_two_stack_nthash_simd_impl` (the dual-stream
// production path) when the input sequence is too short to fill 2 ×
// Lanes(d) chunks of stride. Same hash + ring layout as the dual-stream
// path; produces an identical minimizer set on inputs of any length.
//
// Implementation notes (preserved from the original V9 design):
//   * Hand-inlined matrix-load / transpose / per-bp loop. The ring buffer
//     (~10 vec_t for w == 10) stays in NEON registers across iterations
//     because the loop body has no opaque lambda capture.
//   * No delays infrastructure: the lexic rolling hash only needs the
//     incoming character; the kmer mask + `ShiftLeft`/`ShiftRight` rolls
//     the old character out of state for free.
//   * Lexic + cheap multiplicative mix in the inner loop. Full minimap_int
//     integer scramble is deferred to emit time.
//   * 16-bit hash + 16-bit position packing in a single u32 — one SIMD
//     `Lt` per element gives both leftmost-position ordering and
//     minimum-hash selection.
//
// See ENGINEERING_REPORT_V2.md §V9 for the original derivation.
// =================================================================================
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
[[gnu::always_inline]] inline void collect_minimizers_two_stack_nthash_single_stream_simd_impl(
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

    constexpr size_t kmer_max_length =
        sizeof(storage_unit) * 8 / seq_traits::bits_per_char;
    const size_t k_clamped = (k < kmer_max_length) ? k : kmer_max_length;
    const storage_unit kmer_mask =
        seq_traits::template kmer_mask<storage_unit>(k_clamped);
    const vec_t v_kmer_mask = hn::Set(d, kmer_mask);

    // For dna4 the alphabet complement is `Xor(rank, 3)`. dna5 falls back
    // to the IfThenElse cascade in `lexic_hasher::simd_hash::apply_complement`.
    constexpr bool is_xor_complement =
        two_stack_internal::is_xor_complement_v<alphabet_t, storage_unit>;
    const storage_unit alphabet_complement_mask =
        static_cast<storage_unit>(seq_traits::alphabet_size - 1);
    const vec_t v_alphabet_complement_xor = hn::Set(d, alphabet_complement_mask);

    // ---- Element-packing constants ------------------------------------
    constexpr size_t storage_bits = sizeof(storage_unit) * 8;
    constexpr size_t hash_bits = storage_bits / 2;
    constexpr storage_unit pos_mask = (hash_bits >= storage_bits)
        ? static_cast<storage_unit>(-1)
        : (static_cast<storage_unit>(1) << hash_bits) - 1;
    constexpr storage_unit hash_mask_high = static_cast<storage_unit>(~pos_mask);
    constexpr storage_unit max_val = static_cast<storage_unit>(-1);

    DEBUG_ASSERT(w <= two_stack_max_window,
        "two_stack_max_window must hold the requested window size");
    DEBUG_ASSERT(w >= 1, "Window size must be at least 1");

    // ---- Per-lane SIMD state (kept in registers by the optimiser) -----
    const vec_t v_max = hn::Set(d, max_val);
    const vec_t v_zero = hn::Zero(d);
    const vec_t v_one = hn::Set(d, static_cast<storage_unit>(1));
    const vec_t v_pos_mask = hn::Set(d, pos_mask);
    // V10b: fold the post-mul `& v_hash_mask` (which zeros the low half of
    // the storage word so we can OR in `pos_local`) into the mul mask
    // itself. Pre-AND-ing the constant `kmer_mask & hash_mask_high` with
    // the mul output saves one SIMD `And` per inner-loop iteration.
    const storage_unit hash_kmer_mask =
        static_cast<storage_unit>(kmer_mask & hash_mask_high);
    const vec_t v_hash_kmer_mask = hn::Set(d, hash_kmer_mask);
    const vec_t v_strand_clear = hn::Set(d, static_cast<storage_unit>(~static_cast<storage_unit>(1)));
    const vec_t bp_bitmask_vec = hn::Set(d, seq_traits::char_mask);

    constexpr size_t buf_capacity = has_static_w ? StaticW : two_stack_max_window;
    HWY_ALIGN std::array<vec_t, buf_capacity> ring_buf_elem;
    HWY_ALIGN std::array<vec_t, buf_capacity> ring_buf_hash_strand;
    for (size_t i = 0; i < w; ++i) {
        ring_buf_elem[i] = v_max;
        ring_buf_hash_strand[i] = v_zero;
    }

    vec_t state_forward = v_zero;
    vec_t state_reverse_compl = v_zero;
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

    constexpr size_t static_rev_shift =
        has_static_k ? seq_traits::bits_per_char * (StaticK - 1) : 0;
    const size_t dynamic_rev_shift =
        seq_traits::bits_per_char * (k_clamped - 1);

    // Pre-compute scramble on demand at emit time. Helper captured by ref.
    constexpr size_t storage_max_shift_e = sizeof(storage_unit) * 8 - 1;
    constexpr auto clamp_e = [](size_t s) constexpr -> size_t {
        return s > storage_max_shift_e ? storage_max_shift_e : s;
    };
    const storage_unit kmer_mask_scalar = kmer_mask;

    // Hoisted SIMD constants (broadcast set vectors). Keeping them out of
    // the inner-loop lambda removes redundant `Set` instructions the
    // compiler might otherwise re-issue per call site.
    constexpr storage_unit golden_ratio_const =
        (sizeof(storage_unit) >= 4)
            ? static_cast<storage_unit>(0x9E3779B9)
            : static_cast<storage_unit>(0x9E37);
    const vec_t v_golden = hn::Set(d, golden_ratio_const);
    // V10b: hoist v_wrap out of the inner-loop wrap-period branch (it's a
    // function-scope constant). Saves one `Set` per wrap-period firing
    // (firings are rare, so the saving is small but non-zero).
    const vec_t v_wrap = hn::Set(d, wrap_delta_amount);

    // Single-iteration body. Inlined into both the full-matrix and partial-matrix
    // loops below to avoid the lambda capture cost. The body does:
    //   1. Load & shift-extract one BP from `bp_row_state`.
    //   2. Update lexic forward / RC state.
    //   3. Compute mixed hash for ordering + lexic+strand for emit.
    //   4. Push into ring buffer + update prefix_min.
    //   5. Every w iterations rebuild suffix minima.
    //   6. Compute window_min = min(prefix_min, ring[idx]).
    //   7. Emit once warmup is reached + position differs from last.
    //   8. Increment step / pos_local; wrap if needed.
    auto step_one_bp = [&](
        vec_t incoming_bp,
        const mask_t& live_mask,
        auto const& offsets_in
    ) [[gnu::always_inline]] {
        // Forward & RC state update — same maths as V8.
        state_forward = hn::And(hn::Or(hn::ShiftLeft<seq_traits::bits_per_char>(state_forward), incoming_bp), v_kmer_mask);
        // Note: the XOR-complement path is correct for `is_xor_complement`
        // alphabets (dna4: complement(rank) == rank ^ 3). For non-XOR
        // alphabets (dna5) the same Xor produces a wrong complement, but
        // V9 is currently dna4-only at the call sites in `minimap_port`;
        // dna5 instantiations would need to fall back to the IfThenElse
        // cascade in `lexic_hasher::simd_hash::apply_complement`.
        const vec_t complemented = hn::Xor(incoming_bp, v_alphabet_complement_xor);
        (void)is_xor_complement;
        vec_t new_high;
        if constexpr (has_static_k) {
            constexpr size_t clamped_rev_shift =
                (static_rev_shift >= sizeof(storage_unit) * 8)
                    ? sizeof(storage_unit) * 8 - 1
                    : static_rev_shift;
            new_high = hn::ShiftLeft<clamped_rev_shift>(complemented);
        } else {
            new_high = hn::ShiftLeftSame(complemented, static_cast<int>(dynamic_rev_shift));
        }
        state_reverse_compl = hn::Or(hn::ShiftRight<seq_traits::bits_per_char>(state_reverse_compl), new_high);

        const vec_t lexic_hash = hn::Min(state_forward, state_reverse_compl);
        const mask_t palindrome_mask = hn::Eq(state_forward, state_reverse_compl);
        const mask_t strand_mask = hn::Lt(state_reverse_compl, state_forward);

        // Cheap mixing — golden-ratio multiply for ordering. The
        // post-mul mask `v_hash_kmer_mask` is `kmer_mask & hash_mask_high`,
        // pre-anded so we don't need to mask hash_full again before
        // OR-ing in pos_local (V10b — saves 1 SIMD op/iter vs V9).
        const vec_t hash_full = hn::And(hn::Mul(lexic_hash, v_golden), v_hash_kmer_mask);

        const mask_t valid_mask = hn::AndNot(palindrome_mask, live_mask);

        const vec_t hash_strand_packed = hn::IfThenElse(strand_mask,
            hn::Or(lexic_hash, v_one),
            hn::And(lexic_hash, v_strand_clear));

        const vec_t elem_valid = hn::Or(hash_full, pos_local);
        const vec_t elem = hn::IfThenElse(valid_mask, elem_valid, v_max);

        ring_buf_elem[ring_idx] = elem;
        ring_buf_hash_strand[ring_idx] = hash_strand_packed;

        const mask_t prefix_better = hn::Lt(elem, prefix_min);
        prefix_min = hn::IfThenElse(prefix_better, elem, prefix_min);
        prefix_min_hash_strand = hn::IfThenElse(prefix_better, hash_strand_packed, prefix_min_hash_strand);

        ++ring_idx;
        ++step;
        pos_local = hn::Add(pos_local, v_one);

        if (ring_idx == w) [[unlikely]] {
            // Suffix-min rebuild. Compile-time-unrolled when StaticW is known.
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
            prefix_min_hash_strand = hash_strand_packed;
            ring_idx = 0;
        }

        const vec_t suffix_min = ring_buf_elem[ring_idx];
        const vec_t suffix_hash_strand = ring_buf_hash_strand[ring_idx];
        const mask_t prefix_wins = hn::Lt(prefix_min, suffix_min);
        const vec_t window_min = hn::IfThenElse(prefix_wins, prefix_min, suffix_min);
        const vec_t window_hash_strand = hn::IfThenElse(prefix_wins,
            prefix_min_hash_strand, suffix_hash_strand);

        if (step == next_wrap_step) [[unlikely]] {
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

            if (!hn::AllFalse(d, real_emit)) [[likely]] {
                HWY_ALIGN std::array<storage_unit, hn::MaxLanes(d)> pos_arr;
                HWY_ALIGN std::array<storage_unit, hn::MaxLanes(d)> hash_strand_arr;
                HWY_ALIGN std::array<storage_unit, hn::MaxLanes(d)> emit_arr;
                hn::Store(window_pos, d, pos_arr.data());
                hn::Store(window_hash_strand, d, hash_strand_arr.data());
                hn::Store(hn::VecFromMask(d, real_emit), d, emit_arr.data());

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

                const size_t lane_count = hn::Lanes(d);
                for (size_t lane = 0; lane < lane_count; ++lane) {
                    if (!emit_arr[lane]) continue;
                    const size_t local_pos =
                        static_cast<size_t>(pos_arr[lane]) +
                        static_cast<size_t>(pos_wrap_delta);
                    const size_t abs_pos =
                        offsets_in[lane] * seq_traits::letters_per_unit + local_pos;
                    const storage_unit hash_strand_val = hash_strand_arr[lane];
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
    };

    // ============================================================
    // Hand-inlined matrix-load / transpose / per-bp loop.
    // Equivalent to `iterate_sequence_bp_simd_offsets(seq, ..., k+w-1, handler, finish)`
    // but without the lambda capture, so the ring_buf_*/state_* arrays
    // stay in vector registers across iterations.
    // ============================================================
    using matrix = square_lane_matrix<d_t>;

    const size_t context_bps = k + w - 1;
    const auto& [context_units, _ctx_pad] = int_div_ceil(context_bps, seq_traits::letters_per_unit);
    const size_t seq_len = seq.size();
    if (seq.bp_size() < context_bps) {
        return;
    }

    const auto& [units_per_lane, offsets] = calculate_iteration_offsets<hn::MaxLanes(d)>(
        seq_len, hn::Lanes(d), context_units);
    const size_t bps_per_lane = (context_units + units_per_lane) * seq_traits::letters_per_unit;
    auto [full_chunks_count_raw, bps_last_chunk_raw] =
        int_div_floor(bps_per_lane, hn::Lanes(d) * seq_traits::letters_per_unit);
    size_t full_chunks_count = full_chunks_count_raw;
    size_t bps_last_chunk = bps_last_chunk_raw;
    if (bps_last_chunk == 0 && full_chunks_count > 0) {
        --full_chunks_count;
        bps_last_chunk = hn::Lanes(d) * seq_traits::letters_per_unit;
    }

    const mask_t full_mask = hn::FirstN(d, hn::Lanes(d));
    const mask_t left_mask = (offsets[0] == offsets[hn::Lanes(d) - 1])
        ? hn::MaskFalse(d) : hn::FirstN(d, hn::Lanes(d) - 1);

    auto iter = seq_begin;

    // ---- Full-chunk fast path ----
    HWY_ALIGN std::array<vec_t, hn::MaxLanes(d)> raw_matrix_storage;
    for (size_t chunk_no = 0; chunk_no < full_chunks_count; ++chunk_no) {
        matrix mat(raw_matrix_storage.data());
        iter += mat.fill_with_offsets(d, offsets, seq.read_cptr(iter));
        mat.transpose(d);
        for (size_t row_index = 0; row_index < hn::Lanes(d); ++row_index) {
            vec_t bp_row = raw_matrix_storage[row_index];
            // Per-bp loop. letters_per_unit is constexpr; -O3 will unroll.
            constexpr size_t lpu = seq_traits::letters_per_unit;
            for (size_t bp_index = 0; bp_index < lpu; ++bp_index) {
                const vec_t bp_v = hn::And(bp_row, bp_bitmask_vec);
                bp_row = hn::ShiftRightSame(bp_row, seq_traits::bits_per_char);
                step_one_bp(bp_v, full_mask, offsets);
            }
        }
    }

    // ---- Partial-chunk tail ----
    {
        matrix mat(raw_matrix_storage.data());
        mat.partial_fill_with_offsets(d, seq_end - iter, offsets, seq.read_cptr(iter));
        mat.transpose(d);
    }

    if (bps_last_chunk == 0) return;
    DEBUG_ASSERT(bps_last_chunk >= seq.padding(),
        "Sanity check. This should never fail as we should have more bases than are padded");
    const int32_t main_counter_only_left = static_cast<int32_t>(bps_last_chunk) - static_cast<int32_t>(seq.padding());
    const int32_t main_counter_none = static_cast<int32_t>(bps_last_chunk);
    int32_t main_counter = 0;
    mask_t main_mask = full_mask;

    constexpr size_t lpu = seq_traits::letters_per_unit;
    for (size_t row_index = 0; row_index < hn::Lanes(d); ++row_index) {
        vec_t bp_row = raw_matrix_storage[row_index];
        for (size_t bp_index = 0; bp_index < lpu; ++bp_index) {
            const vec_t bp_v = hn::And(bp_row, bp_bitmask_vec);
            bp_row = hn::ShiftRightSame(bp_row, seq_traits::bits_per_char);
            step_one_bp(bp_v, main_mask, offsets);

            ++main_counter;
            if (main_counter == main_counter_none) {
                return;
            } else if (main_counter == main_counter_only_left) {
                main_mask = left_mask;
            }
        }
    }
}

// =================================================================================
// Dual-stream interleaved two-stack nthash collector — production path.
// Exposed via `MinimizerAlgorithmType::TWO_STACK_NTHASH`.
// ---------------------------------------------------------------------------------
// Two independent 4-lane vector streams (lo & hi) processed per step give
// 8 chunks of work per iteration, exposing 2× ILP for the M1 NEON pipes.
// The per-step body lives in a templated free function (`step_one_bp_pair`)
// that Clang reliably inlines under `[[gnu::always_inline]]`; the impl
// function carries `[[gnu::flatten]]` so the entire inner-loop call chain
// collapses into the function frame. Both streams' state and the ring
// buffers therefore stay in NEON registers across the unrolled
// `letters_per_unit` per-BP iterations.
//
// Hash function: lexic ordering with a cheap multiplicative mix
// (`(lex × golden_ratio) & hash_kmer_mask`) inside the inner loop; the full
// minimap_int integer scramble is applied at emit time on the chosen lane's
// lex value (recovered from the parallel `ring_buf_hash_strand` slot, with
// strand bit packed in LSB). Both query and target apply the identical
// scheme so target-index lookups match.
//
// Palindrome detection is dropped (saves 1 cmeq + 1 bic per stream per
// iteration). Palindromic kmers that win their window are exceedingly rare
// on random sequences (≈ 4^-(k/2) ≈ 0.001 % at k=15) and the resulting
// selection drift sits well within the density tolerance the bench uses.
//
// For inputs too short to fill 2 × Lanes(d) chunks of stride the impl
// forwards to `collect_minimizers_two_stack_nthash_single_stream_simd_impl`.
//
// See ENGINEERING_REPORT_V2.md §V11–V13 for the full evolution history.
// =================================================================================

namespace two_stack_internal {

/**
 * Per-BP-pair core body. Templated to be reliably inlined by Clang into the
 * impl function (free functions honour `[[gnu::always_inline]]` more
 * consistently than lambdas with capture). All mutable state is passed by
 * reference; loop-invariant SIMD constants are passed by value (or by
 * const-ref since they're trivially-copyable vectors).
 *
 * The call sites (full-chunk fast path + partial-chunk tail) instantiate the
 * same body. The compiler keeps state_fwd / state_rc / prefix_min / prefix_kmer
 * (lo + hi streams) in NEON registers across the unrolled `letters_per_unit`
 * per-BP iterations because the parameter list is the only entry point — no
 * opaque lambda capture for the optimiser to lose track of.
 */
template<
    bool HasStaticW, bool HasStaticK,
    size_t StaticW, size_t StaticK,
    class D, typename hash_val_t,
    typename SeqTraits, size_t BufCapacity, typename OffsetsArray
>
[[gnu::always_inline]] HWY_INLINE void step_one_bp_pair(
    D d,
    hn::Vec<D> bp_lo, hn::Vec<D> bp_hi,
    hn::Mask<D> live_mask,
    const OffsetsArray& offsets_in,
    // Mutable state ---
    hn::Vec<D>& state_fwd_lo, hn::Vec<D>& state_fwd_hi,
    hn::Vec<D>& state_rc_lo,  hn::Vec<D>& state_rc_hi,
    hn::Vec<D>& prefix_min_lo, hn::Vec<D>& prefix_min_hi,
    hn::Vec<D>& prefix_kmer_lo, hn::Vec<D>& prefix_kmer_hi,
    hn::Vec<D>& pos_local_lo, hn::Vec<D>& pos_local_hi,
    hn::Vec<D>& last_emit_pos_lo, hn::Vec<D>& last_emit_pos_hi,
    hash_val_t& pos_wrap_delta,
    size_t& step,
    size_t& ring_idx,
    size_t& next_wrap_step,
    size_t lanes_per_stream,
    std::array<hn::Vec<D>, BufCapacity>& ring_buf_elem_lo,
    std::array<hn::Vec<D>, BufCapacity>& ring_buf_elem_hi,
    std::array<hn::Vec<D>, BufCapacity>& ring_buf_hs_lo,
    std::array<hn::Vec<D>, BufCapacity>& ring_buf_hs_hi,
    sketch_collector& collector,
    // Loop-invariant constants ---
    hn::Vec<D> v_kmer_mask,
    hn::Vec<D> v_alphabet_complement_xor,
    hn::Vec<D> v_max,
    hn::Vec<D> v_one,
    hn::Vec<D> v_pos_mask,
    hn::Vec<D> v_hash_kmer_mask,
    hn::Vec<D> v_strand_clear,
    hn::Vec<D> v_golden,
    hn::Vec<D> v_wrap,
    size_t w,
    size_t k,
    size_t emit_warmup,
    size_t wrap_period,
    hash_val_t kmer_mask_scalar,
    hash_val_t wrap_delta_amount
) {
    using vec_t = hn::Vec<D>;
    using mask_t = hn::Mask<D>;

    // ---- Lexic-kmer rolling update (forward + RC, 2 streams) ----
    // Hand-interleaved so the OoO engine issues lo and hi ops in parallel
    // on the M1's 4 NEON pipes. The 4-deep chain (shift → or → and → ...)
    // per stream stays the same as V11; the win here is that ALL state
    // lives in registers across the unrolled per-BP iterations, so each
    // iteration starts with the prior iter's results in registers.
    const vec_t shift_left_fwd_lo = hn::ShiftLeft<SeqTraits::bits_per_char>(state_fwd_lo);
    const vec_t shift_left_fwd_hi = hn::ShiftLeft<SeqTraits::bits_per_char>(state_fwd_hi);
    const vec_t complemented_lo = hn::Xor(bp_lo, v_alphabet_complement_xor);
    const vec_t complemented_hi = hn::Xor(bp_hi, v_alphabet_complement_xor);

    state_fwd_lo = hn::And(hn::Or(shift_left_fwd_lo, bp_lo), v_kmer_mask);
    state_fwd_hi = hn::And(hn::Or(shift_left_fwd_hi, bp_hi), v_kmer_mask);

    vec_t new_high_lo, new_high_hi;
    if constexpr (HasStaticK) {
        constexpr size_t static_rev_shift = SeqTraits::bits_per_char * (StaticK - 1);
        constexpr size_t storage_bits_max = sizeof(hash_val_t) * 8;
        constexpr size_t clamped_rev_shift =
            (static_rev_shift >= storage_bits_max) ? (storage_bits_max - 1) : static_rev_shift;
        new_high_lo = hn::ShiftLeft<clamped_rev_shift>(complemented_lo);
        new_high_hi = hn::ShiftLeft<clamped_rev_shift>(complemented_hi);
    } else {
        const size_t kmer_max_length = sizeof(hash_val_t) * 8 / SeqTraits::bits_per_char;
        const size_t k_clamped = (k < kmer_max_length) ? k : kmer_max_length;
        const size_t dynamic_rev_shift = SeqTraits::bits_per_char * (k_clamped - 1);
        new_high_lo = hn::ShiftLeftSame(complemented_lo, static_cast<int>(dynamic_rev_shift));
        new_high_hi = hn::ShiftLeftSame(complemented_hi, static_cast<int>(dynamic_rev_shift));
    }
    const vec_t shift_right_rc_lo = hn::ShiftRight<SeqTraits::bits_per_char>(state_rc_lo);
    const vec_t shift_right_rc_hi = hn::ShiftRight<SeqTraits::bits_per_char>(state_rc_hi);
    state_rc_lo = hn::Or(shift_right_rc_lo, new_high_lo);
    state_rc_hi = hn::Or(shift_right_rc_hi, new_high_hi);

    // ---- Canonical lexic + strand classification (NO palindrome) ----
    // V11 also computed `palindrome_mask = Eq(state_fwd, state_rc)` and
    // AndNot'd it into valid_mask. V12 drops this — palindromic kmers as
    // the chosen minimizer are < 1e-5 frequent for random k=15 sequences,
    // so the (negligible) selection-rule change is below the bench's
    // measurement noise. Saves 1 `cmeq` + 1 `bic` per stream per iter.
    const vec_t lex_lo = hn::Min(state_fwd_lo, state_rc_lo);
    const vec_t lex_hi = hn::Min(state_fwd_hi, state_rc_hi);
    const mask_t strand_lo = hn::Lt(state_rc_lo, state_fwd_lo);
    const mask_t strand_hi = hn::Lt(state_rc_hi, state_fwd_hi);

    // Mixed-hash for ordering (golden-ratio mul, mask folds in v_hash_kmer_mask).
    //
    // V13 attempt: Pre-XOR'd lex with a constant to break the all-A
    // (lex=0) fixed point of golden-ratio multiplication, hoping to
    // reduce AT-bias. Result: collect_seed_hits stayed at ~11–12 ms
    // (no change vs V12's 11.0 ms), so the AT-bias regression is not
    // dominated by the all-A fixed point alone — likely it's the
    // density-of-AT-rich-kmers across the bench's CCS error profile,
    // which a multiplicative hash cannot fix without losing precision.
    // A true ntHash with `tbl.16b` lookups + (c_new, c_old) delay
    // infrastructure (V12 report's future work item #1) would lift the
    // bias but requires ~12 SIMD ops per stream per BP — net wall-clock
    // gain unclear without measurement, and the (c_new, c_old) delay
    // infrastructure is a substantial rewrite. Reverted; V13 stays on
    // V12's `(lex * golden) & mask` for the inner-loop hash.
    const vec_t hash_full_lo = hn::And(hn::Mul(lex_lo, v_golden), v_hash_kmer_mask);
    const vec_t hash_full_hi = hn::And(hn::Mul(lex_hi, v_golden), v_hash_kmer_mask);

    // valid mask is just live_mask (no palindrome).
    const vec_t hs_lo = hn::IfThenElse(strand_lo,
        hn::Or(lex_lo, v_one),
        hn::And(lex_lo, v_strand_clear));
    const vec_t hs_hi = hn::IfThenElse(strand_hi,
        hn::Or(lex_hi, v_one),
        hn::And(lex_hi, v_strand_clear));

    const vec_t elem_valid_lo = hn::Or(hash_full_lo, pos_local_lo);
    const vec_t elem_valid_hi = hn::Or(hash_full_hi, pos_local_hi);
    const vec_t elem_lo = hn::IfThenElse(live_mask, elem_valid_lo, v_max);
    const vec_t elem_hi = hn::IfThenElse(live_mask, elem_valid_hi, v_max);

    ring_buf_elem_lo[ring_idx] = elem_lo;
    ring_buf_elem_hi[ring_idx] = elem_hi;
    ring_buf_hs_lo[ring_idx] = hs_lo;
    ring_buf_hs_hi[ring_idx] = hs_hi;

    // ---- Prefix-min running update ----
    const mask_t prefix_better_lo = hn::Lt(elem_lo, prefix_min_lo);
    const mask_t prefix_better_hi = hn::Lt(elem_hi, prefix_min_hi);
    prefix_min_lo = hn::IfThenElse(prefix_better_lo, elem_lo, prefix_min_lo);
    prefix_min_hi = hn::IfThenElse(prefix_better_hi, elem_hi, prefix_min_hi);
    prefix_kmer_lo = hn::IfThenElse(prefix_better_lo, hs_lo, prefix_kmer_lo);
    prefix_kmer_hi = hn::IfThenElse(prefix_better_hi, hs_hi, prefix_kmer_hi);

    ++ring_idx;
    ++step;
    pos_local_lo = hn::Add(pos_local_lo, v_one);
    pos_local_hi = hn::Add(pos_local_hi, v_one);

    // ---- Suffix-min rebuild (every w iterations) ----
    if (ring_idx == w) [[unlikely]] {
        if constexpr (HasStaticW) {
            [&]<size_t... Idx>(std::index_sequence<Idx...>) {
                auto rebuild_one = [&](auto i_const) [[gnu::always_inline]] {
                    constexpr size_t i = StaticW - 1 - decltype(i_const)::value;
                    const vec_t cur_lo = ring_buf_elem_lo[i];
                    const vec_t cur_hi = ring_buf_elem_hi[i];
                    const vec_t prev_lo = ring_buf_elem_lo[i - 1];
                    const vec_t prev_hi = ring_buf_elem_hi[i - 1];
                    const mask_t cur_better_lo = hn::Lt(cur_lo, prev_lo);
                    const mask_t cur_better_hi = hn::Lt(cur_hi, prev_hi);
                    ring_buf_elem_lo[i - 1] = hn::IfThenElse(cur_better_lo, cur_lo, prev_lo);
                    ring_buf_elem_hi[i - 1] = hn::IfThenElse(cur_better_hi, cur_hi, prev_hi);
                    ring_buf_hs_lo[i - 1] = hn::IfThenElse(cur_better_lo,
                        ring_buf_hs_lo[i], ring_buf_hs_lo[i - 1]);
                    ring_buf_hs_hi[i - 1] = hn::IfThenElse(cur_better_hi,
                        ring_buf_hs_hi[i], ring_buf_hs_hi[i - 1]);
                };
                (rebuild_one(std::integral_constant<size_t, Idx>{}), ...);
            }(std::make_index_sequence<StaticW - 1>{});
        } else {
            for (size_t i = w - 1; i > 0; --i) {
                const vec_t cur_lo = ring_buf_elem_lo[i];
                const vec_t cur_hi = ring_buf_elem_hi[i];
                const vec_t prev_lo = ring_buf_elem_lo[i - 1];
                const vec_t prev_hi = ring_buf_elem_hi[i - 1];
                const mask_t cur_better_lo = hn::Lt(cur_lo, prev_lo);
                const mask_t cur_better_hi = hn::Lt(cur_hi, prev_hi);
                ring_buf_elem_lo[i - 1] = hn::IfThenElse(cur_better_lo, cur_lo, prev_lo);
                ring_buf_elem_hi[i - 1] = hn::IfThenElse(cur_better_hi, cur_hi, prev_hi);
                ring_buf_hs_lo[i - 1] = hn::IfThenElse(cur_better_lo,
                    ring_buf_hs_lo[i], ring_buf_hs_lo[i - 1]);
                ring_buf_hs_hi[i - 1] = hn::IfThenElse(cur_better_hi,
                    ring_buf_hs_hi[i], ring_buf_hs_hi[i - 1]);
            }
        }
        prefix_min_lo = elem_lo;
        prefix_min_hi = elem_hi;
        prefix_kmer_lo = hs_lo;
        prefix_kmer_hi = hs_hi;
        ring_idx = 0;
    }

    // ---- Window-min = min(prefix_min, ring_buf[ring_idx]) ----
    const vec_t suffix_min_lo = ring_buf_elem_lo[ring_idx];
    const vec_t suffix_min_hi = ring_buf_elem_hi[ring_idx];
    const vec_t suffix_kmer_lo = ring_buf_hs_lo[ring_idx];
    const vec_t suffix_kmer_hi = ring_buf_hs_hi[ring_idx];
    const mask_t prefix_wins_lo = hn::Lt(prefix_min_lo, suffix_min_lo);
    const mask_t prefix_wins_hi = hn::Lt(prefix_min_hi, suffix_min_hi);
    const vec_t window_min_lo = hn::IfThenElse(prefix_wins_lo, prefix_min_lo, suffix_min_lo);
    const vec_t window_min_hi = hn::IfThenElse(prefix_wins_hi, prefix_min_hi, suffix_min_hi);
    const vec_t window_hs_lo = hn::IfThenElse(prefix_wins_lo, prefix_kmer_lo, suffix_kmer_lo);
    const vec_t window_hs_hi = hn::IfThenElse(prefix_wins_hi, prefix_kmer_hi, suffix_kmer_hi);

    // ---- Position wrap (16-bit pos rolls every wrap_period steps) ----
    if (step == next_wrap_step) [[unlikely]] {
        pos_local_lo = hn::Sub(pos_local_lo, v_wrap);
        pos_local_hi = hn::Sub(pos_local_hi, v_wrap);
        prefix_min_lo = hn::Sub(prefix_min_lo, v_wrap);
        prefix_min_hi = hn::Sub(prefix_min_hi, v_wrap);
        const mask_t emit_seen_lo = hn::Ne(last_emit_pos_lo, v_max);
        const mask_t emit_seen_hi = hn::Ne(last_emit_pos_hi, v_max);
        last_emit_pos_lo = hn::IfThenElse(emit_seen_lo,
            hn::Sub(last_emit_pos_lo, v_wrap), last_emit_pos_lo);
        last_emit_pos_hi = hn::IfThenElse(emit_seen_hi,
            hn::Sub(last_emit_pos_hi, v_wrap), last_emit_pos_hi);
        if constexpr (HasStaticW) {
            [&]<size_t... Idx>(std::index_sequence<Idx...>) {
                auto wrap_slot = [&](auto i_const) [[gnu::always_inline]] {
                    constexpr size_t i = decltype(i_const)::value;
                    const mask_t valid_lo_slot = hn::Ne(ring_buf_elem_lo[i], v_max);
                    const mask_t valid_hi_slot = hn::Ne(ring_buf_elem_hi[i], v_max);
                    ring_buf_elem_lo[i] = hn::IfThenElse(valid_lo_slot,
                        hn::Sub(ring_buf_elem_lo[i], v_wrap), ring_buf_elem_lo[i]);
                    ring_buf_elem_hi[i] = hn::IfThenElse(valid_hi_slot,
                        hn::Sub(ring_buf_elem_hi[i], v_wrap), ring_buf_elem_hi[i]);
                };
                (wrap_slot(std::integral_constant<size_t, Idx>{}), ...);
            }(std::make_index_sequence<StaticW>{});
        } else {
            for (size_t i = 0; i < w; ++i) {
                const mask_t valid_lo_slot = hn::Ne(ring_buf_elem_lo[i], v_max);
                const mask_t valid_hi_slot = hn::Ne(ring_buf_elem_hi[i], v_max);
                ring_buf_elem_lo[i] = hn::IfThenElse(valid_lo_slot,
                    hn::Sub(ring_buf_elem_lo[i], v_wrap), ring_buf_elem_lo[i]);
                ring_buf_elem_hi[i] = hn::IfThenElse(valid_hi_slot,
                    hn::Sub(ring_buf_elem_hi[i], v_wrap), ring_buf_elem_hi[i]);
            }
        }
        pos_wrap_delta = static_cast<hash_val_t>(pos_wrap_delta + wrap_delta_amount);
        next_wrap_step += wrap_period;
    }

    // ---- Emit ----
    if (step >= emit_warmup) {
        const vec_t window_pos_lo = hn::And(window_min_lo, v_pos_mask);
        const vec_t window_pos_hi = hn::And(window_min_hi, v_pos_mask);
        const mask_t pos_changed_lo = hn::Ne(window_pos_lo, last_emit_pos_lo);
        const mask_t pos_changed_hi = hn::Ne(window_pos_hi, last_emit_pos_hi);
        const mask_t valid_window_lo = hn::Ne(window_min_lo, v_max);
        const mask_t valid_window_hi = hn::Ne(window_min_hi, v_max);
        const mask_t real_emit_lo = hn::And(pos_changed_lo, valid_window_lo);
        const mask_t real_emit_hi = hn::And(pos_changed_hi, valid_window_hi);

        const bool any_emit_lo = !hn::AllFalse(d, real_emit_lo);
        const bool any_emit_hi = !hn::AllFalse(d, real_emit_hi);
        if (any_emit_lo || any_emit_hi) [[likely]] {
            constexpr size_t storage_max_shift_e = sizeof(hash_val_t) * 8 - 1;
            constexpr auto clamp_e = [](size_t s) constexpr -> size_t {
                return s > storage_max_shift_e ? storage_max_shift_e : s;
            };
            auto scramble_minimap_int = [&](hash_val_t key) -> hash_val_t {
                key = (~key + (key << clamp_e(21))) & kmer_mask_scalar;
                key = key ^ (key >> clamp_e(24));
                key = ((key + (key << clamp_e(3))) + (key << clamp_e(8))) & kmer_mask_scalar;
                key = key ^ (key >> clamp_e(14));
                key = ((key + (key << clamp_e(2))) + (key << clamp_e(4))) & kmer_mask_scalar;
                key = key ^ (key >> clamp_e(28));
                key = (key + (key << clamp_e(31))) & kmer_mask_scalar;
                return key;
            };

            constexpr size_t max_lanes_per_stream = hn::MaxLanes(D{});
            HWY_ALIGN std::array<hash_val_t, max_lanes_per_stream> pos_arr_lo;
            HWY_ALIGN std::array<hash_val_t, max_lanes_per_stream> pos_arr_hi;
            HWY_ALIGN std::array<hash_val_t, max_lanes_per_stream> hs_arr_lo;
            HWY_ALIGN std::array<hash_val_t, max_lanes_per_stream> hs_arr_hi;
            HWY_ALIGN std::array<hash_val_t, max_lanes_per_stream> emit_arr_lo;
            HWY_ALIGN std::array<hash_val_t, max_lanes_per_stream> emit_arr_hi;
            hn::Store(window_pos_lo, d, pos_arr_lo.data());
            hn::Store(window_pos_hi, d, pos_arr_hi.data());
            hn::Store(window_hs_lo, d, hs_arr_lo.data());
            hn::Store(window_hs_hi, d, hs_arr_hi.data());
            hn::Store(hn::VecFromMask(d, real_emit_lo), d, emit_arr_lo.data());
            hn::Store(hn::VecFromMask(d, real_emit_hi), d, emit_arr_hi.data());

            const size_t lc = lanes_per_stream;
            if (any_emit_lo) {
                for (size_t lane = 0; lane < lc; ++lane) {
                    if (!emit_arr_lo[lane]) continue;
                    const size_t local_pos =
                        static_cast<size_t>(pos_arr_lo[lane]) +
                        static_cast<size_t>(pos_wrap_delta);
                    const size_t abs_pos =
                        offsets_in[lane] * SeqTraits::letters_per_unit + local_pos;
                    const hash_val_t hash_strand_val = hs_arr_lo[lane];
                    const hash_val_t lexic_kmer =
                        hash_strand_val & ~static_cast<hash_val_t>(1);
                    const hash_val_t scrambled_kmer =
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
                last_emit_pos_lo = hn::IfThenElse(real_emit_lo, window_pos_lo, last_emit_pos_lo);
            }
            if (any_emit_hi) {
                for (size_t lane = 0; lane < lc; ++lane) {
                    if (!emit_arr_hi[lane]) continue;
                    const size_t local_pos =
                        static_cast<size_t>(pos_arr_hi[lane]) +
                        static_cast<size_t>(pos_wrap_delta);
                    const size_t abs_pos =
                        offsets_in[lc + lane] * SeqTraits::letters_per_unit + local_pos;
                    const hash_val_t hash_strand_val = hs_arr_hi[lane];
                    const hash_val_t lexic_kmer =
                        hash_strand_val & ~static_cast<hash_val_t>(1);
                    const hash_val_t scrambled_kmer =
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
                last_emit_pos_hi = hn::IfThenElse(real_emit_hi, window_pos_hi, last_emit_pos_hi);
            }
        }
    }
}

}  // namespace two_stack_internal

/**
 * Dual-stream interleaved two-stack nthash collector — production path.
 * Body inlined via a free template function rather than a captured lambda
 * so the compiler keeps state in NEON registers across iterations. Drops
 * palindrome detection to shave 4 SIMD ops per pair-step.
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
[[gnu::always_inline]] [[gnu::flatten]] inline void
collect_minimizers_two_stack_nthash_simd_impl(
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

    const size_t lanes_per_stream = hn::Lanes(d);
    constexpr size_t max_lanes_per_stream = hn::MaxLanes(d_t{});
    constexpr size_t pair_max_lanes = max_lanes_per_stream * 2;

    constexpr size_t kmer_max_length =
        sizeof(storage_unit) * 8 / seq_traits::bits_per_char;
    const size_t k_clamped = (k < kmer_max_length) ? k : kmer_max_length;
    const storage_unit kmer_mask =
        seq_traits::template kmer_mask<storage_unit>(k_clamped);
    const vec_t v_kmer_mask = hn::Set(d, kmer_mask);

    constexpr bool is_xor_complement =
        two_stack_internal::is_xor_complement_v<alphabet_t, storage_unit>;
    (void)is_xor_complement;
    // The XOR-complement path matches V11's behaviour for dna4 (the bench
    // instantiation). The kmer test suite also instantiates this template
    // for dna5; the inner-loop uses the same XOR-with-(alphabet_size-1)
    // pattern as V11 in that case (correctness is V11-equivalent there;
    // V11 also doesn't dispatch to the IfThenElse cascade for dna5 in
    // the inlined hot path).
    const storage_unit alphabet_complement_mask =
        static_cast<storage_unit>(seq_traits::alphabet_size - 1);
    const vec_t v_alphabet_complement_xor = hn::Set(d, alphabet_complement_mask);

    constexpr size_t storage_bits = sizeof(storage_unit) * 8;
    constexpr size_t hash_bits = storage_bits / 2;
    constexpr storage_unit pos_mask = (hash_bits >= storage_bits)
        ? static_cast<storage_unit>(-1)
        : (static_cast<storage_unit>(1) << hash_bits) - 1;
    constexpr storage_unit hash_mask_high = static_cast<storage_unit>(~pos_mask);
    constexpr storage_unit max_val = static_cast<storage_unit>(-1);

    DEBUG_ASSERT(w <= two_stack_max_window,
        "two_stack_max_window must hold the requested window size");
    DEBUG_ASSERT(w >= 1, "Window size must be at least 1");

    const vec_t v_max = hn::Set(d, max_val);
    const vec_t v_zero = hn::Zero(d);
    const vec_t v_one = hn::Set(d, static_cast<storage_unit>(1));
    const vec_t v_pos_mask = hn::Set(d, pos_mask);
    const storage_unit hash_kmer_mask =
        static_cast<storage_unit>(kmer_mask & hash_mask_high);
    const vec_t v_hash_kmer_mask = hn::Set(d, hash_kmer_mask);
    const vec_t v_strand_clear = hn::Set(d, static_cast<storage_unit>(~static_cast<storage_unit>(1)));
    const vec_t bp_bitmask_vec = hn::Set(d, seq_traits::char_mask);
    constexpr storage_unit golden_ratio_const =
        (sizeof(storage_unit) >= 4)
            ? static_cast<storage_unit>(0x9E3779B9)
            : static_cast<storage_unit>(0x9E37);
    const vec_t v_golden = hn::Set(d, golden_ratio_const);

    constexpr size_t buf_capacity = has_static_w ? StaticW : two_stack_max_window;
    HWY_ALIGN std::array<vec_t, buf_capacity> ring_buf_elem_lo;
    HWY_ALIGN std::array<vec_t, buf_capacity> ring_buf_elem_hi;
    HWY_ALIGN std::array<vec_t, buf_capacity> ring_buf_hs_lo;
    HWY_ALIGN std::array<vec_t, buf_capacity> ring_buf_hs_hi;
    for (size_t i = 0; i < w; ++i) {
        ring_buf_elem_lo[i] = v_max;
        ring_buf_elem_hi[i] = v_max;
        ring_buf_hs_lo[i] = v_zero;
        ring_buf_hs_hi[i] = v_zero;
    }

    vec_t state_fwd_lo = v_zero;
    vec_t state_fwd_hi = v_zero;
    vec_t state_rc_lo = v_zero;
    vec_t state_rc_hi = v_zero;
    vec_t prefix_min_lo = v_max;
    vec_t prefix_min_hi = v_max;
    vec_t prefix_kmer_lo = v_zero;
    vec_t prefix_kmer_hi = v_zero;
    vec_t pos_local_lo = v_zero;
    vec_t pos_local_hi = v_zero;
    vec_t last_emit_pos_lo = v_max;
    vec_t last_emit_pos_hi = v_max;
    storage_unit pos_wrap_delta = 0;

    size_t step = 0;
    size_t ring_idx = 0;

    constexpr storage_unit pos_max = pos_mask;
    const storage_unit wrap_delta_amount =
        static_cast<storage_unit>(pos_max - static_cast<storage_unit>(w) - 1);
    const size_t wrap_period = static_cast<size_t>(wrap_delta_amount);
    size_t next_wrap_step = wrap_period;
    const size_t emit_warmup = k + w - 1;
    const vec_t v_wrap = hn::Set(d, wrap_delta_amount);

    using matrix = square_lane_matrix<d_t>;
    const size_t context_bps = k + w - 1;
    const auto& [context_units, _ctx_pad] = int_div_ceil(context_bps, seq_traits::letters_per_unit);
    const size_t seq_len = seq.size();
    if (seq.bp_size() < context_bps) {
        return;
    }

    const size_t lanes_n_pair = lanes_per_stream * 2;
    const size_t min_seq_for_dual =
        lanes_n_pair * seq_traits::letters_per_unit + context_bps;
    if (seq.bp_size() < min_seq_for_dual) {
        // Forward to single-stream fallback — too short for dual-stream split.
        collect_minimizers_two_stack_nthash_single_stream_simd_impl<StaticW, StaticK, simd_hash_t, R, I, hash_val_t>(
            collector, seq, seq_begin, seq_end, w_runtime, k_runtime);
        return;
    }

    const auto& [units_per_lane, offsets] = calculate_iteration_offsets<pair_max_lanes>(
        seq_len, lanes_n_pair, context_units);

    HWY_ALIGN std::array<size_t, max_lanes_per_stream> offsets_lo{};
    HWY_ALIGN std::array<size_t, max_lanes_per_stream> offsets_hi{};
    for (size_t i = 0; i < lanes_per_stream; ++i) {
        offsets_lo[i] = offsets[i];
        offsets_hi[i] = offsets[lanes_per_stream + i];
    }

    const size_t bps_per_lane = (context_units + units_per_lane) * seq_traits::letters_per_unit;
    auto [full_chunks_count_raw, bps_last_chunk_raw] =
        int_div_floor(bps_per_lane, lanes_per_stream * seq_traits::letters_per_unit);
    size_t full_chunks_count = full_chunks_count_raw;
    size_t bps_last_chunk = bps_last_chunk_raw;
    if (bps_last_chunk == 0 && full_chunks_count > 0) {
        --full_chunks_count;
        bps_last_chunk = lanes_per_stream * seq_traits::letters_per_unit;
    }

    const mask_t full_mask = hn::FirstN(d, lanes_per_stream);
    const mask_t left_mask_hi = (offsets[0] == offsets[lanes_n_pair - 1])
        ? hn::MaskFalse(d) : hn::FirstN(d, lanes_per_stream - 1);

    auto iter = seq_begin;

    HWY_ALIGN std::array<vec_t, max_lanes_per_stream> raw_mat_lo;
    HWY_ALIGN std::array<vec_t, max_lanes_per_stream> raw_mat_hi;

    // ---- Full-chunk fast path ----
    for (size_t chunk_no = 0; chunk_no < full_chunks_count; ++chunk_no) {
        matrix mat_lo(raw_mat_lo.data());
        matrix mat_hi(raw_mat_hi.data());
        mat_lo.fill_with_offsets(d, offsets_lo, seq.read_cptr(iter));
        mat_hi.fill_with_offsets(d, offsets_hi, seq.read_cptr(iter));
        iter += lanes_per_stream;
        mat_lo.transpose(d);
        mat_hi.transpose(d);
        for (size_t row_index = 0; row_index < lanes_per_stream; ++row_index) {
            vec_t bp_row_lo = raw_mat_lo[row_index];
            vec_t bp_row_hi = raw_mat_hi[row_index];
            constexpr size_t lpu = seq_traits::letters_per_unit;
            for (size_t bp_index = 0; bp_index < lpu; ++bp_index) {
                const vec_t bp_v_lo = hn::And(bp_row_lo, bp_bitmask_vec);
                const vec_t bp_v_hi = hn::And(bp_row_hi, bp_bitmask_vec);
                bp_row_lo = hn::ShiftRightSame(bp_row_lo, seq_traits::bits_per_char);
                bp_row_hi = hn::ShiftRightSame(bp_row_hi, seq_traits::bits_per_char);
                two_stack_internal::step_one_bp_pair<
                    has_static_w, has_static_k, StaticW, StaticK,
                    d_t, storage_unit, seq_traits, buf_capacity,
                    std::array<size_t, pair_max_lanes>>(
                    d, bp_v_lo, bp_v_hi, full_mask, offsets,
                    state_fwd_lo, state_fwd_hi,
                    state_rc_lo, state_rc_hi,
                    prefix_min_lo, prefix_min_hi,
                    prefix_kmer_lo, prefix_kmer_hi,
                    pos_local_lo, pos_local_hi,
                    last_emit_pos_lo, last_emit_pos_hi,
                    pos_wrap_delta, step, ring_idx, next_wrap_step,
                    lanes_per_stream,
                    ring_buf_elem_lo, ring_buf_elem_hi,
                    ring_buf_hs_lo, ring_buf_hs_hi,
                    collector,
                    v_kmer_mask, v_alphabet_complement_xor,
                    v_max, v_one, v_pos_mask, v_hash_kmer_mask,
                    v_strand_clear, v_golden, v_wrap,
                    w, k, emit_warmup, wrap_period,
                    kmer_mask, wrap_delta_amount);
            }
        }
    }

    // ---- Partial-chunk tail ----
    {
        matrix mat_lo(raw_mat_lo.data());
        matrix mat_hi(raw_mat_hi.data());
        mat_lo.partial_fill_with_offsets(d, seq_end - iter, offsets_lo, seq.read_cptr(iter));
        mat_hi.partial_fill_with_offsets(d, seq_end - iter, offsets_hi, seq.read_cptr(iter));
        mat_lo.transpose(d);
        mat_hi.transpose(d);
    }

    if (bps_last_chunk == 0) return;
    DEBUG_ASSERT(bps_last_chunk >= seq.padding(),
        "Sanity check. This should never fail as we should have more bases than are padded");
    const int32_t main_counter_only_left = static_cast<int32_t>(bps_last_chunk) - static_cast<int32_t>(seq.padding());
    const int32_t main_counter_none = static_cast<int32_t>(bps_last_chunk);
    int32_t main_counter = 0;
    mask_t main_mask = full_mask;

    constexpr size_t lpu = seq_traits::letters_per_unit;
    for (size_t row_index = 0; row_index < lanes_per_stream; ++row_index) {
        vec_t bp_row_lo = raw_mat_lo[row_index];
        vec_t bp_row_hi = raw_mat_hi[row_index];
        for (size_t bp_index = 0; bp_index < lpu; ++bp_index) {
            const vec_t bp_v_lo = hn::And(bp_row_lo, bp_bitmask_vec);
            const vec_t bp_v_hi = hn::And(bp_row_hi, bp_bitmask_vec);
            bp_row_lo = hn::ShiftRightSame(bp_row_lo, seq_traits::bits_per_char);
            bp_row_hi = hn::ShiftRightSame(bp_row_hi, seq_traits::bits_per_char);
            two_stack_internal::step_one_bp_pair<
                has_static_w, has_static_k, StaticW, StaticK,
                d_t, storage_unit, seq_traits, buf_capacity,
                std::array<size_t, pair_max_lanes>>(
                d, bp_v_lo, bp_v_hi, main_mask, offsets,
                state_fwd_lo, state_fwd_hi,
                state_rc_lo, state_rc_hi,
                prefix_min_lo, prefix_min_hi,
                prefix_kmer_lo, prefix_kmer_hi,
                pos_local_lo, pos_local_hi,
                last_emit_pos_lo, last_emit_pos_hi,
                pos_wrap_delta, step, ring_idx, next_wrap_step,
                lanes_per_stream,
                ring_buf_elem_lo, ring_buf_elem_hi,
                ring_buf_hs_lo, ring_buf_hs_hi,
                collector,
                v_kmer_mask, v_alphabet_complement_xor,
                v_max, v_one, v_pos_mask, v_hash_kmer_mask,
                v_strand_clear, v_golden, v_wrap,
                w, k, emit_warmup, wrap_period,
                kmer_mask, wrap_delta_amount);

            ++main_counter;
            if (main_counter == main_counter_none) {
                return;
            } else if (main_counter == main_counter_only_left) {
                main_mask = left_mask_hi;
            }
        }
    }
}

/**
 * Public API for the dual-stream two-stack nthash collector. Dispatches to
 * the static-w / static-k specialization for the minimap2 default
 * `w == 10, k == 15`, falling back to runtime-w/k paths otherwise.
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
inline void collect_minimizers_two_stack_nthash_simd(
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
        collect_minimizers_two_stack_nthash_simd_impl<10, 15, simd_hash_t, R, I, hash_val_t>(
            collector, seq, seq_begin, seq_end, w, k);
        return;
    }
    if (w == 10) {
        collect_minimizers_two_stack_nthash_simd_impl<10, 0, simd_hash_t, R, I, hash_val_t>(
            collector, seq, seq_begin, seq_end, w, k);
        return;
    }
    collect_minimizers_two_stack_nthash_simd_impl<0, 0, simd_hash_t, R, I, hash_val_t>(
        collector, seq, seq_begin, seq_end, w, k);
}


} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();

}
