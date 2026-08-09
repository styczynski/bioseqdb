/** Canonical generator input for the exact two-stack minimizer kernels. */
module;
#include <hwy/highway.h>
#include "common.h"

export module bioseq.highway.sources.minimizer_two_stack;

import std;
import bioseq.common.mlogging;
import bioseq.common.sequan_compat;
import bioseq.sequences;
import bioseq.instr;
import bioseq.storage.lazy;
import bioseq.storage.size;
import bioseq.sequences.kmers.highway_contract;
import bioseq.sequences.kmers.hashers.minimap_int;
import bioseq.highway.sources.sequence_ops;
import bioseq.highway.sources.hasher_base;
import bioseq.highway.sources.hasher_lexic;
import bioseq.highway.sources.hasher_minimap_int;
import bioseq.highway.sources.simd_utils;
import bioseq.highway.sources.vector_stream;

export namespace bioseq::simd {
namespace [[bioseq::simd]] target {
namespace hn = hwy::HWY_NAMESPACE;

using ::bioseq::minimizers::MinimapSketchData;
using ::bioseq::minimizers::MinimizerRepr;
using ::bioseq::minimizers::minimizer_sink_collector;
using ::bioseq::minimizers::MinimizerKernelAlgorithm;
using ::bioseq::minimizers::MinimizerKernelInvocation;
using ::bioseq::minimizers::MinimizerKernelResult;
using ::bioseq::minimizers::MinimizerKernelStatus;
using ::bioseq::minimizers::MinimizerOutputLayout;
using ::bioseq::minimizers::minimizer_output_position_mask;
using ::bioseq::minimizers::TwoStackRoute;

/**
 * Compile-time upper bound on the window size `w` accepted by the
 * two-stack collectors in this file. Our minimap2 ports run at the
 * default `w = 10`; bumping past 256 also blows the L1-friendly stack
 * footprint of the parallel ring buffers.
 */
using ::bioseq::minimizers::two_stack_max_window;

/** Maximum nt4 K represented by one native Highway integer lane. */
using ::bioseq::minimizers::two_stack_max_kmer;

/** Exact scalar-oracle path rejects K=32 because its 2*K mask shifts by 64. */
using ::bioseq::minimizers::two_stack_exact_max_kmer;

using ::bioseq::minimizers::TwoStackContinuationStatus;
using ::bioseq::minimizers::ExactSimdKernelKind;
using ::bioseq::minimizers::two_stack_status_name;
using ::bioseq::minimizers::TwoStackExecutionProof;
using ::bioseq::minimizers::TwoStackFinalizeResult;
using ::bioseq::minimizers::PackedPaperNthashStatus;
using ::bioseq::minimizers::packed_paper_nthash_scheme_id;
using ::bioseq::minimizers::packed_paper_nthash_max_window;
using ::bioseq::minimizers::PackedPaperNthashExecutionProof;
using ::bioseq::minimizers::PackedPaperNthashResult;
using ::bioseq::minimizers::ExactSimdRuntimeError;
using ::bioseq::minimizers::exact_simd_minimizer_representation;

namespace resident_descriptor_detail {

struct borrowed_packed_header {
    const void* data = nullptr;
    bioseq::storage::sequence_size_t bp_size = 0;
    std::uint8_t storage_unit_byte_width = 0;
    std::uint16_t padding_letters = 0;
};

template <sequence_alphabet Alphabet, class StorageUnit>
struct borrowed_packed_instance {
    using alphabet_type = Alphabet;
    using storage_unit = StorageUnit;
    using compact_sequence_stdc_type = borrowed_packed_header;

    static constexpr std::size_t letter_bit_width =
        bioseq::storage::basic_lazy_sequence_view<Alphabet>::traits::
            logical_bits_per_symbol;
    static constexpr std::size_t byte_width = sizeof(StorageUnit);
    static constexpr bool is_valid_compact_sequence_stdc_instance = true;
};

template <sequence_alphabet Alphabet, class StorageUnit, class Fn>
decltype(auto) with_borrowed_compact_range(
        const typename bioseq::storage::basic_lazy_sequence_view<Alphabet>::
            resident_packed_region& descriptor,
        Fn&& fn) {
    using instance = borrowed_packed_instance<Alphabet, StorageUnit>;
    using range_type = compact_sequence_range<instance>;
    borrowed_packed_header header{
        .data = descriptor.units,
        .bp_size = descriptor.bp_length,
        .storage_unit_byte_width = descriptor.storage_unit_byte_width,
        .padding_letters = descriptor.trailing_padding_letters,
    };
    range_type range{header};
    return std::forward<Fn>(fn)(range);
}

}  // namespace resident_descriptor_detail

/**
 * Fragment continuation for the nt4-normalized nucleotide two-stack kernel.
 * The proof/status models now live in the target-neutral minimizer contract;
 * the continuation remains target-local because its pending capacity depends
 * on the generated target's Highway lane count.
 */
template<std::size_t W, std::size_t K, class AlphabetTraits>
struct TwoStackContinuationState {
    static constexpr std::size_t max_vector_lanes =
        hn::MaxLanes(hn::ScalableTag<std::uint64_t>{});
    static constexpr std::size_t fast_block_candidates = 8;
    static constexpr std::size_t pending_capacity =
        std::max(max_vector_lanes, fast_block_candidates);

    using traits = AlphabetTraits;
    using alphabet_type = typename traits::alphabet_type;

    // Static W/K routes must not inherit the dynamic route's W=256/K=31
    // scratch footprint.  This state is instantiated inside the per-sketch
    // driver, and its arrays participate in the hot candidate-block frame.
    static constexpr std::size_t code_tail_capacity =
        K != 0 && K <= two_stack_exact_max_kmer
            ? std::max<std::size_t>(K - 1, 1)
            : two_stack_exact_max_kmer - 1;
    static constexpr std::size_t candidate_tail_capacity =
        W != 0 && W <= two_stack_max_window
            ? std::max<std::size_t>(W - 1, 1)
            : two_stack_max_window - 1;

    struct candidate_state {
        std::uint64_t hash = std::numeric_limits<std::uint64_t>::max();
        std::uint64_t raw = std::numeric_limits<std::uint64_t>::max();
        std::uint64_t ordinal = std::numeric_limits<std::uint64_t>::max();
    };

    std::size_t runtime_w = 0;
    std::size_t runtime_k = 0;
    std::size_t emitted_position_base = 0;
    std::size_t input_position_base = 0;
    std::size_t next_input_position = 0;
    std::uint64_t valid_run_length = 0;
    std::array<std::uint8_t, code_tail_capacity> code_tail{};
    std::size_t code_tail_size = 0;
    std::array<std::uint8_t, pending_capacity> pending_codes{};
    std::array<std::uint64_t, pending_capacity> pending_positions{};
    std::array<std::uint64_t, pending_capacity> pending_valid_run_lengths{};
    std::size_t pending_count = 0;
    std::array<std::uint64_t, candidate_tail_capacity> candidate_tail_hash{};
    std::array<std::uint64_t, candidate_tail_capacity> candidate_tail_raw{};
    std::array<std::uint64_t, candidate_tail_capacity> candidate_tail_ordinal{};
    candidate_state current_min{};
    std::uint64_t candidate_count = 0;
    bool candidate_state_initialized = false;
    bool initialized = false;
    bool saw_first_fragment = false;
    bool finalized = false;
    ExactSimdKernelKind kernel_kind = ExactSimdKernelKind::generic_u64;
    TwoStackContinuationStatus status = TwoStackContinuationStatus::ok;
    TwoStackExecutionProof proof{};

    [[nodiscard]] constexpr std::size_t window_size() const noexcept {
        if constexpr (W != 0) return W;
        return runtime_w;
    }

    [[nodiscard]] constexpr std::size_t kmer_size() const noexcept {
        if constexpr (K != 0) return K;
        return runtime_k;
    }

};

namespace two_stack_internal {

[[nodiscard]] inline bool resident_vector_shape_supported() noexcept {
#if HWY_TARGET == HWY_SCALAR
    return false;
#else
    const hn::ScalableTag<std::uint32_t> d32;
    const std::size_t lanes = hn::Lanes(d32);
    const std::size_t capacity = hn::MaxLanes(d32);
    return lanes >= 2u && lanes <= capacity
        && (lanes == 2u || lanes == 4u
            || lanes == 8u || lanes == 16u);
#endif
}

template<class Lane, std::size_t N>
[[nodiscard]] consteval std::array<Lane, N>
make_lane_index_table() noexcept
{
    static_assert(std::is_unsigned_v<Lane>);
    std::array<Lane, N> indices{};
    for (std::size_t lane = 0; lane < N; ++lane) {
        indices[lane] = static_cast<Lane>(lane);
    }
    return indices;
}

/**
 * Portable unsigned lane ordinals for targets such as NEON where Highway's
 * Iota overload is intentionally unavailable for 64-bit lanes. The table is
 * constant-initialized once per compiled target and enters the kernel through
 * one vector load; all offset, packed-shift, and candidate-position work stays
 * in vector registers.
 */
template<class D>
[[nodiscard]] HWY_INLINE hn::Vec<D>
lane_indices(D d) noexcept
{
    using lane_type = hn::TFromD<D>;
    static_assert(std::is_unsigned_v<lane_type>);
    constexpr std::size_t capacity = hn::MaxLanes(D{});
    static constexpr auto indices =
        make_lane_index_table<lane_type, capacity>();
    return hn::LoadU(d, indices.data());
}

template<class D, typename ST, std::size_t W, std::size_t... I>
[[nodiscard, gnu::always_inline]] HWY_INLINE
vec_stream<D, ST, W> make_undefined_vec_stream(
    D d,
    std::index_sequence<I...>) noexcept
{
    return pack_vec_stream<D, ST, W>(
        d, ((void)I, hn::Undefined(d))...);
}

template<
    std::size_t Capacity,
    class D,
    typename ST,
    std::size_t W,
    std::size_t... I>
[[nodiscard, gnu::always_inline]] HWY_INLINE
std::array<vec_stream<D, ST, W>, Capacity>
make_undefined_vec_stream_array(
    D d,
    std::index_sequence<I...>) noexcept
{
    return {
        ((void)I, make_undefined_vec_stream<D, ST, W>(
            d, std::make_index_sequence<W>{}))...
    };
}

/**
 * Construct every std::array element inside target-attributed project code.
 * The ring's live prefix is overwritten with its semantic max/zero values
 * before the first read, so Undefined avoids an otherwise redundant fill.
 */
template<std::size_t Capacity, class D, typename ST, std::size_t W>
[[nodiscard, gnu::always_inline]] HWY_INLINE
std::array<vec_stream<D, ST, W>, Capacity>
make_undefined_vec_stream_array(D d) noexcept
{
    return make_undefined_vec_stream_array<Capacity, D, ST, W>(
        d, std::make_index_sequence<Capacity>{});
}

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
    typename HashValT,
    class Collector
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
    Collector& collector;

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
    void step_one_bp_vec_stream(
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
            // K's reverse-complement shift. For dna5 (3-bit alphabet with
            // a 3-bit leftover at every storage width) the widen-via-
            // bitcast contract from `is_sequence_bit_castable_to` cannot
            // be honoured — leftover_bits != 0 makes the bitcast invalid,
            // so the impl ends up with `ST = base storage_unit` (e.g.
            // `unsigned char`) instead of the requested `hash_val_t =
            // uint64_t`. The previous `static_assert` here fired a hard
            // compile error whenever the dispatch_chain instantiated the
            // dna5 K=15 template (every postgresql_bioseq build pulling
            // in dispatch_buffer.cppm). DOC0260 §3.2 keeps `dna5` in the
            // `pgseq_supported_alphabets` pack, so the instantiation is
            // unavoidable. We resolve this by routing the impossible
            // configuration through a `DEBUG_ASSERT(false, …)` runtime
            // guard so the impl compiles for every (alphabet, K)
            // combination the spec advertises and only trips at run-time
            // for the configurations the public dispatcher promises to
            // skip. The bench/test grid never reaches dna5+K=15 on the
            // two-stack SIMD path (the scalar `mm_sketch_baseline`
            // handles dna5 today); see DOC0260 §1.1#9 / DOC0245.
            if constexpr (static_rev_shift < sizeof(ST) * 8) {
                new_high = ShiftLeft<static_rev_shift>(complemented);
            } else {
                DEBUG_ASSERT(false,
                    fmt::format("two_stack SIMD: static K reverse-complement "
                                "shift {} >= lane width {}; this (alphabet, K) "
                                "must be routed to the scalar fallback "
                                "(mm_sketch_baseline). See DOC0260 §3.2.",
                                static_rev_shift, sizeof(ST) * 8));
                // Placeholder that compiles. The DEBUG_ASSERT above
                // unconditionally fails in debug builds; in release the
                // resulting `new_high` is never observed because the
                // dispatcher must route every dna5+K=15 query through
                // mm_sketch_baseline before the SIMD impl ever runs.
                new_high = vec_stream_t::splat(d, static_cast<ST>(0));
            }
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
    typename hash_val_t = compact_sequence_range<R>::traits::storage_unit,
    // DOC0254 §5.3 — collector is now a template parameter, defaulting to
    // the legacy sketch_collector_t so existing call sites compile
    // unchanged.  Arena adapters substitute here.
    class Collector = minimizer_sink_collector<MinimizerT>
>
requires (
    supports_simd_hash<simd_hash_t, typename compact_sequence_range<R>::traits::alphabet_type>
)
[[gnu::always_inline]] [[gnu::flatten]] inline void
collect_minimizers_two_stack_nthash_simd_impl(
    Collector& collector,
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
    // `main/DOCS/BUGFIX/REGRESSION_NTHASH_INVESTIGATE_CRITICAL_PROBLEM_REPORT__DOC0213.md`'s
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
            "main/DOCS/BUGFIX/REPORT_PHASE_5_INDEPTH_ANALYSIS__DOC0244.md.",
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
    // limitation tracked in main/DOCS/BUGFIX/FIX_TESTS_FINDINGS_AND_SOLUTIONS__DOC0190.md §2.2).
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
    // BUG FIX (main/DOCS/BUGFIX/FIX_MINIMIZER_TERRIBLE_REGRESSION_PROBLEMS_REPORT__DOC0194.md):
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
    // Ring-buffer elements are explicitly constructed with Undefined inside
    // target-attributed project code; the meaningful max/zero initialization
    // runs in the loop below before any read (only the first `w` slots are
    // live; this matches the pre-Phase-4 init contract). With
    // [[clang::trivial_abi]] on `vec_stream` and the impl being
    // [[gnu::always_inline]] [[gnu::flatten]], SROA should break the struct
    // back apart into per-member NEON-register-resident locals so the
    // inliner sees a small finite-parameter member function (DIRECT-shape).
    using state_t = two_stack_internal::sequence_simd_iteration_state<
        StaticW, StaticK, NumStreams, DetectPalindrome,
        seq_traits, simd_hash_t, MinimizerT, hash_val_t, Collector>;
    state_t state{
        .state_fwd                  = vec_stream_t::splat(d, static_cast<storage_unit>(0)),
        .state_rc                   = vec_stream_t::splat(d, static_cast<storage_unit>(0)),
        .prefix_min                 = vec_stream_t::splat(d, max_val),
        .prefix_kmer                = vec_stream_t::splat(d, static_cast<storage_unit>(0)),
        .pos_local                  = vec_stream_t::splat(d, static_cast<storage_unit>(0)),
        .last_emit_pos              = vec_stream_t::splat(d, max_val),
        .ring_buf_elem              = two_stack_internal::
            make_undefined_vec_stream_array<
                buf_capacity, d_t, storage_unit, NumStreams>(d),
        .ring_buf_hash_strand       = two_stack_internal::
            make_undefined_vec_stream_array<
                buf_capacity, d_t, storage_unit, NumStreams>(d),
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
 * The report-76 kernel runs two independent vector streams once the resident
 * sequence is long enough to keep both streams productive.  Keep this one
 * calculation shared by the dispatch and the proof wrapper so the attested
 * stream count cannot drift from the branch that actually ran.
 */
template<
    typename HashUnit,
    supports_compact_sequence_range R>
[[nodiscard]] inline bool two_stack_resident_uses_dual_streams(
    const compact_sequence_range<R>& seq,
    std::size_t w,
    std::size_t k) noexcept
{
    using seq_traits = typename compact_sequence_range<R>::traits;
    using unit_d_t = hn::ScalableTag<HashUnit>;
    constexpr std::size_t letters_per_hash_unit =
        sizeof(HashUnit) * 8u / seq_traits::bits_per_char;
    const std::size_t lanes_per_stream = hn::Lanes(unit_d_t{});
    const std::size_t context_bps = k + w - 1u;
    const std::size_t min_seq_for_dual =
        2u * lanes_per_stream * letters_per_hash_unit + context_bps;
    return seq.bp_size() >= min_seq_for_dual;
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
    typename I = decltype(compact_sequence_range<R>().begin()),
    // DOC0254 §5.3 — Collector defaults to the legacy
    // minimizer_sink_collector<MinimizerT>.  Arena adapters substitute here.
    class Collector = minimizer_sink_collector<MinimizerT>
>
requires (
    supports_simd_hash<simd_hash_t, typename compact_sequence_range<R>::traits::alphabet_type>
)
inline void collect_minimizers_two_stack_nthash_simd(
    Collector& collector,
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
            constexpr size_t lpu_unit =
                sizeof(hash_unit_t) * 8 / bpc;
            static_assert(lpu_unit > 0u);
            const bool use_dual_unit =
                two_stack_resident_uses_dual_streams<hash_unit_t>(
                    seq, w, k);

            if (use_dual_unit) {
                collect_minimizers_two_stack_nthash_simd_impl<
                    StaticW, StaticK,
                    /*NumStreams=*/2, /*DetectPalindrome=*/false,
                    simd_hash_t, MinimizerT, R, I, hash_unit_t, Collector>(
                        collector, seq, seq_begin, seq_end, w, k);
            } else {
                collect_minimizers_two_stack_nthash_simd_impl<
                    StaticW, StaticK,
                    /*NumStreams=*/1, /*DetectPalindrome=*/true,
                    simd_hash_t, MinimizerT, R, I, hash_unit_t, Collector>(
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

// =============================================================================
// DOC0254 §8.2 — Cep-templated bridge for the two-stack nthash SIMD kernel.
//
// Short-circuits directly to the `_impl<StaticW, StaticK>` dispatcher whose
// inner code is already template-specialised on (W, K).  The dual/single
// stream eligibility threshold is recomputed inline using the static
// (W, K) constants so the dispatch remains branchless once inlined.
// =============================================================================

template <
    std::size_t W, std::size_t K,
    template<typename H_A> typename simd_hash_t,
    class MinimizerT,
    supports_compact_sequence_range R,
    typename I       = decltype(compact_sequence_range<R>().begin()),
    class Collector  = minimizer_sink_collector<MinimizerT>>
requires (
    supports_simd_hash<simd_hash_t, typename compact_sequence_range<R>::traits::alphabet_type>
)
[[gnu::always_inline]] inline void
collect_minimizers_two_stack_nthash_simd_constexpr(
    Collector&                 collector,
    compact_sequence_range<R>& seq,
    const I&                   seq_begin,
    const I&                   seq_end)
{
    using seq_traits        = typename compact_sequence_range<R>::traits;
    using base_storage_unit = typename seq_traits::storage_unit;
    constexpr std::size_t base_bits = sizeof(base_storage_unit) * 8;
    constexpr std::size_t bpc       = seq_traits::bits_per_char;
    constexpr std::size_t kmer_bits = K * bpc;

    if constexpr (kmer_bits <= base_bits) {
        constexpr std::size_t lpu_unit = sizeof(base_storage_unit) * 8 / bpc;
        static_assert(lpu_unit > 0u);
        const bool use_dual =
            two_stack_resident_uses_dual_streams<base_storage_unit>(
                seq, W, K);
        if (use_dual) {
            collect_minimizers_two_stack_nthash_simd_impl<
                W, K, /*NumStreams=*/2, /*DetectPalindrome=*/false,
                simd_hash_t, MinimizerT, R, I, base_storage_unit, Collector>(
                    collector, seq, seq_begin, seq_end, W, K);
        } else {
            collect_minimizers_two_stack_nthash_simd_impl<
                W, K, /*NumStreams=*/1, /*DetectPalindrome=*/true,
                simd_hash_t, MinimizerT, R, I, base_storage_unit, Collector>(
                    collector, seq, seq_begin, seq_end, W, K);
        }
    } else {
        static_assert(K * bpc <= 64u,
            "Static K-mer does not fit in any supported SIMD lane "
            "(uint64_t is the AArch64-NEON ceiling).");
        constexpr std::size_t lpu_unit = 64u / bpc;
        static_assert(lpu_unit > 0u);
        const bool use_dual =
            two_stack_resident_uses_dual_streams<std::uint64_t>(
                seq, W, K);
        if (use_dual) {
            collect_minimizers_two_stack_nthash_simd_impl<
                W, K, /*NumStreams=*/2, /*DetectPalindrome=*/false,
                simd_hash_t, MinimizerT, R, I, std::uint64_t, Collector>(
                    collector, seq, seq_begin, seq_end, W, K);
        } else {
            collect_minimizers_two_stack_nthash_simd_impl<
                W, K, /*NumStreams=*/1, /*DetectPalindrome=*/true,
                simd_hash_t, MinimizerT, R, I, std::uint64_t, Collector>(
                    collector, seq, seq_begin, seq_end, W, K);
        }
    }
}

namespace two_stack_internal {

template<class Collector>
[[nodiscard]] inline std::size_t resident_collector_size(
        const Collector& collector) noexcept {
    if constexpr (requires { collector.size(); }) {
        return static_cast<std::size_t>(collector.size());
    } else {
        return static_cast<std::size_t>(
            collector.extract_sketches_data().size());
    }
}

template<class R, class Collector>
[[nodiscard]] inline TwoStackExecutionProof resident_vector_stream_proof(
    const compact_sequence_range<R>& seq,
    const Collector& collector,
    std::size_t records_before,
    bool used_dual_streams,
    std::size_t w,
    std::size_t k)
{
    const std::size_t records_after = resident_collector_size(collector);
    if (records_after < records_before) {
        throw ExactSimdRuntimeError(
            TwoStackContinuationStatus::invalid_state,
            "resident vector_stream minimizer sketch");
    }
    const bool performed_nonempty_work =
        seq.bp_size() >= k + w - 1u;
    if (performed_nonempty_work && records_after == records_before) {
        throw ExactSimdRuntimeError(
            TwoStackContinuationStatus::invalid_state,
            "resident vector_stream minimizer sketch emitted no records");
    }
    TwoStackExecutionProof proof{};
    proof.bp_consumed = static_cast<std::uint64_t>(seq.bp_size());
    proof.minimizers_emitted = static_cast<std::uint64_t>(
        records_after - records_before);
    proof.resident_vector_stream_kernel_calls = 1u;
    proof.resident_vector_stream_single_calls = used_dual_streams ? 0u : 1u;
    proof.resident_vector_stream_dual_calls = used_dual_streams ? 1u : 0u;
    proof.resident_vector_stream_nonempty_work_calls =
        performed_nonempty_work ? 1u : 0u;
    proof.resident_vector_stream_input_bp = proof.bp_consumed;
    proof.resident_vector_stream_minimizers_emitted =
        proof.minimizers_emitted;
    return proof;
}

} // namespace two_stack_internal

/**
 * Proof-returning full-resident entry point for the report-76 fast kernel.
 * It is intentionally limited to the production XXL shape whose historical
 * relative speed is being restored.  Other shapes remain on the checked
 * chronological continuation; there is no scalar fallback here.
 */
template<
    class MinimizerT,
    supports_compact_sequence_range R,
    class Collector = minimizer_sink_collector<MinimizerT>>
requires (
    exact_simd_minimizer_representation<MinimizerT>
    && std::same_as<
        typename compact_sequence_range<R>::traits::alphabet_type,
        seq::dna4>
    && std::same_as<
        typename compact_sequence_range<R>::traits::storage_unit,
        std::uint32_t>)
[[nodiscard]] inline TwoStackExecutionProof
collect_minimizers_two_stack_nthash_resident_or_throw(
    Collector& collector,
    compact_sequence_range<R>& seq,
    std::size_t w,
    std::size_t k)
{
    if (w != 10u || k != 15u) {
        throw ExactSimdRuntimeError(
            TwoStackContinuationStatus::invalid_static_configuration,
            "resident vector_stream minimizer sketch");
    }
#if HWY_TARGET == HWY_SCALAR
    (void)collector;
    (void)seq;
    throw ExactSimdRuntimeError(
        TwoStackContinuationStatus::unsupported_vector_shape,
        "resident vector_stream minimizer sketch");
#else
    if (!two_stack_internal::resident_vector_shape_supported()) {
        throw ExactSimdRuntimeError(
            TwoStackContinuationStatus::unsupported_vector_shape,
            "resident vector_stream minimizer sketch");
    }
    const std::size_t records_before =
        two_stack_internal::resident_collector_size(collector);
    const bool used_dual_streams =
        two_stack_resident_uses_dual_streams<std::uint32_t>(seq, w, k);
    collect_minimizers_two_stack_nthash_simd<
        minimap_int_hasher, MinimizerT, R>(
            collector, seq, seq.begin(), seq.end(), w, k);
    return two_stack_internal::resident_vector_stream_proof(
        seq, collector, records_before, used_dual_streams, w, k);
#endif
}

template<
    class MinimizerT,
    sequence_alphabet Alphabet,
    class Collector = minimizer_sink_collector<MinimizerT>>
requires (
    exact_simd_minimizer_representation<MinimizerT>
    && std::same_as<Alphabet, seq::dna4>)
[[nodiscard]] inline TwoStackExecutionProof
collect_minimizers_two_stack_nthash_resident_descriptor_or_throw(
    Collector& collector,
    const typename bioseq::storage::basic_lazy_sequence_view<Alphabet>::
        resident_packed_region& descriptor,
    std::size_t w,
    std::size_t k)
{
    if (descriptor.storage_unit_byte_width != sizeof(std::uint32_t)
        || descriptor.bits_per_symbol != 2u
        || descriptor.leading_symbol_offset != 0u) {
        throw ExactSimdRuntimeError(
            TwoStackContinuationStatus::non_contiguous_range,
            "resident vector_stream minimizer descriptor");
    }
    return resident_descriptor_detail::with_borrowed_compact_range<
        Alphabet, std::uint32_t>(
            descriptor,
            [&collector, w, k]<class Range>(Range& range) {
                return collect_minimizers_two_stack_nthash_resident_or_throw<
                    MinimizerT>(collector, range, w, k);
            });
}

template<
    std::size_t W,
    std::size_t K,
    class MinimizerT,
    supports_compact_sequence_range R,
    class Collector = minimizer_sink_collector<MinimizerT>>
requires (
    W == 10u && K == 15u
    && exact_simd_minimizer_representation<MinimizerT>
    && std::same_as<
        typename compact_sequence_range<R>::traits::alphabet_type,
        seq::dna4>
    && std::same_as<
        typename compact_sequence_range<R>::traits::storage_unit,
        std::uint32_t>)
[[nodiscard]] inline TwoStackExecutionProof
collect_minimizers_two_stack_nthash_resident_constexpr_or_throw(
    Collector& collector,
    compact_sequence_range<R>& seq,
    std::size_t w,
    std::size_t k)
{
    if (w != W || k != K) {
        throw ExactSimdRuntimeError(
            TwoStackContinuationStatus::invalid_static_configuration,
            "constexpr resident vector_stream minimizer sketch");
    }
#if HWY_TARGET == HWY_SCALAR
    (void)collector;
    (void)seq;
    throw ExactSimdRuntimeError(
        TwoStackContinuationStatus::unsupported_vector_shape,
        "constexpr resident vector_stream minimizer sketch");
#else
    if (!two_stack_internal::resident_vector_shape_supported()) {
        throw ExactSimdRuntimeError(
            TwoStackContinuationStatus::unsupported_vector_shape,
            "constexpr resident vector_stream minimizer sketch");
    }
    const std::size_t records_before =
        two_stack_internal::resident_collector_size(collector);
    const bool used_dual_streams =
        two_stack_resident_uses_dual_streams<std::uint32_t>(seq, W, K);
    collect_minimizers_two_stack_nthash_simd_constexpr<
        W, K, minimap_int_hasher, MinimizerT, R>(
            collector, seq, seq.begin(), seq.end());
    return two_stack_internal::resident_vector_stream_proof(
        seq, collector, records_before, used_dual_streams, W, K);
#endif
}


// =============================================================================
namespace two_stack_internal {

#if HWY_TARGET == HWY_SCALAR
inline constexpr bool exact_compiled_target_is_scalar = true;
#else
inline constexpr bool exact_compiled_target_is_scalar = false;
#endif

/**
 * The packed K15/W10/B8 kernel stores candidate hashes in u32 lanes and raw
 * minimizers in u64 lanes.  Both persisted-index representations used by the
 * production pipeline satisfy that contract:
 *
 *  * `MinimizerRepr<33, 15>` for target postings (30 hash bits); and
 *  * `MinimapSketchData == MinimizerRepr<28, 15>` for query sketches
 *    (35 hash bits, of which minimap-int uses the low 30).
 *
 * The original gate admitted only the target layout even though every raw
 * construction below is already parameterized by `hash_shift`,
 * `pos_field_mask`, and `pos_shift`.  Keep the admission deliberately narrow
 * to these two persisted-compatible layouts rather than silently widening the
 * kernel to arbitrary `MinimizerT` encodings.
 */
template<class MinimizerT>
inline constexpr bool exact_k15_w10_b8_representation_v =
    exact_simd_minimizer_representation<MinimizerT>
    && std::same_as<typename MinimizerT::storage_t, std::uint64_t>
    && MinimizerT::pos_shift == 1u
    && ((static_cast<std::uint64_t>(MinimizerT::hash_field_mask)
                == 0x3fffffffull
            && static_cast<std::uint64_t>(MinimizerT::pos_field_mask)
                == 0x1ffffffffull
            && MinimizerT::hash_shift == 34u)
        || (static_cast<std::uint64_t>(MinimizerT::hash_field_mask)
                == 0x7ffffffffull
            && static_cast<std::uint64_t>(MinimizerT::pos_field_mask)
                == 0x0fffffffull
            && MinimizerT::hash_shift == 29u));

template<class MinimizerT, class AlphabetTraits>
[[nodiscard]] inline ExactSimdKernelKind select_exact_simd_kernel_kind(
    std::size_t consumed_bp) noexcept
{
    if constexpr (
        std::same_as<typename AlphabetTraits::alphabet_type, seq::dna4>
        && std::same_as<typename AlphabetTraits::storage_unit, std::uint32_t>
        && exact_k15_w10_b8_representation_v<MinimizerT>) {
        const hn::ScalableTag<std::uint32_t> d32;
        const hn::ScalableTag<std::uint64_t> d64;
        if (!exact_compiled_target_is_scalar
            && hn::Lanes(d32) == 4
            && hn::Lanes(d64) == 2
            && consumed_bp
                <= static_cast<std::size_t>(
                    (std::numeric_limits<std::uint32_t>::max)())) {
            return ExactSimdKernelKind::k15_w10_b8_u32_hash;
        }
    }
    return ExactSimdKernelKind::generic_u64;
}

[[nodiscard]] constexpr bool exact_vector_lane_count_supported(
    std::size_t lanes,
    std::size_t lane_capacity) noexcept
{
    return lanes >= 2 && lanes <= lane_capacity
        && (lanes == 2 || lanes == 4 || lanes == 8 || lanes == 16);
}

/**
 * The exact continuation is a SIMD-only contract. Target rejection differs
 * from an invalid biological request: it happens before reset/consume work and
 * therefore deliberately leaves every execution-proof counter at zero.
 */
template<class State>
[[nodiscard]] constexpr TwoStackContinuationStatus
validate_exact_vector_shape(
    State& state,
    std::size_t lanes,
    std::size_t lane_capacity,
    bool compiled_target_is_scalar,
    bool transpose_shape_supported) noexcept
{
    if (compiled_target_is_scalar
        || !exact_vector_lane_count_supported(lanes, lane_capacity)
        || !transpose_shape_supported) {
        state.status = TwoStackContinuationStatus::unsupported_vector_shape;
        return state.status;
    }
    return TwoStackContinuationStatus::ok;
}

#if HWY_TARGET == HWY_SCALAR

// Keep the scalar translation entirely free of multi-lane Highway operations.
// These stubs let the common reset/consume templates compile, but the target
// gate rejects them before any biological work or proof accounting.
template<class D>
[[nodiscard]] HWY_INLINE bool u64_lane_transpose_shape_supported(D) noexcept
{
    static_assert(std::is_same_v<hn::TFromD<D>, std::uint64_t>);
    return false;
}

template<class D>
[[nodiscard]] HWY_INLINE bool transpose_u64_lane_matrix(
    D,
    hn::Vec<D>* HWY_RESTRICT) noexcept
{
    static_assert(std::is_same_v<hn::TFromD<D>, std::uint64_t>);
    return false;
}

#else

/**
 * Whether repeated Half<D> operations exactly describe a power-of-two square
 * for this target. This is checked before any biological work so an unusual
 * scalable-vector shape cannot reach a late transpose failure.
 */
template<class D>
[[nodiscard]] HWY_INLINE bool u64_lane_transpose_shape_supported(D d) noexcept
{
    static_assert(std::is_same_v<hn::TFromD<D>, std::uint64_t>);
    const std::size_t lanes = hn::Lanes(d);
    if (lanes < 2 || lanes > 16) return false;
    if (lanes == 2) return true;
    if ((lanes & (lanes - 1)) != 0) return false;

    if constexpr (hn::MaxLanes(D{}) < 4) {
        return false;
    } else {
        using half_d_t = hn::Half<D>;
        const half_d_t dh;
        return hn::Lanes(dh) * 2 == lanes
            && u64_lane_transpose_shape_supported(dh);
    }
}

/**
 * In-register u64 square transpose. The generic square_lane_matrix widens
 * lane types between stages, which reaches a nonexistent u128 lane for u64
 * matrices. This network instead recursively transposes four half-width
 * quadrants and joins them with Highway Combine. The only base operation is a
 * two-lane Interleave; there are no scalar lane or event-record stores.
 */
template<class D>
[[nodiscard]] HWY_INLINE bool transpose_u64_lane_matrix(
    D d,
    hn::Vec<D>* HWY_RESTRICT matrix) noexcept
{
    static_assert(std::is_same_v<hn::TFromD<D>, std::uint64_t>);
    const std::size_t lanes = hn::Lanes(d);
    if (lanes == 2) {
        const hn::Vec<D> column0 = matrix[0];
        const hn::Vec<D> column1 = matrix[1];
        matrix[0] = hn::InterleaveLower(d, column0, column1);
        matrix[1] = hn::InterleaveUpper(d, column0, column1);
        return true;
    }
    if (lanes < 4 || lanes > 16 || (lanes & (lanes - 1)) != 0) {
        return false;
    }

    if constexpr (hn::MaxLanes(D{}) < 4) {
        return false;
    } else {
        using half_d_t = hn::Half<D>;
        using half_vec_t = hn::Vec<half_d_t>;
        constexpr std::size_t half_capacity = hn::MaxLanes(half_d_t{});
        const half_d_t dh;
        const std::size_t half_lanes = lanes / 2;
        if (hn::Lanes(dh) != half_lanes) return false;

        HWY_ALIGN std::array<half_vec_t, half_capacity> lower_left{};
        HWY_ALIGN std::array<half_vec_t, half_capacity> lower_right{};
        HWY_ALIGN std::array<half_vec_t, half_capacity> upper_left{};
        HWY_ALIGN std::array<half_vec_t, half_capacity> upper_right{};
        for (std::size_t column = 0; column < half_lanes; ++column) {
            lower_left[column] = hn::LowerHalf(dh, matrix[column]);
            upper_left[column] = hn::UpperHalf(dh, matrix[column]);
            lower_right[column] =
                hn::LowerHalf(dh, matrix[column + half_lanes]);
            upper_right[column] =
                hn::UpperHalf(dh, matrix[column + half_lanes]);
        }

        if (!transpose_u64_lane_matrix(dh, lower_left.data())
            || !transpose_u64_lane_matrix(dh, lower_right.data())
            || !transpose_u64_lane_matrix(dh, upper_left.data())
            || !transpose_u64_lane_matrix(dh, upper_right.data())) {
            return false;
        }
        for (std::size_t row = 0; row < half_lanes; ++row) {
            matrix[row] = hn::Combine(
                d, lower_right[row], lower_left[row]);
            matrix[row + half_lanes] = hn::Combine(
                d, upper_right[row], upper_left[row]);
        }
        return true;
    }
}

#endif

template<class State>
[[nodiscard]] inline TwoStackContinuationStatus reject_continuation(
    State& state,
    TwoStackContinuationStatus status) noexcept
{
    state.status = status;
    if (state.proof.fail_closed_rejections
        != std::numeric_limits<std::uint64_t>::max()) {
        ++state.proof.fail_closed_rejections;
    }
    return status;
}

inline void add_proof_counter(
    std::uint64_t& counter,
    std::uint64_t increment) noexcept
{
    const std::uint64_t maximum =
        std::numeric_limits<std::uint64_t>::max();
    counter = increment > maximum - counter
        ? maximum
        : counter + increment;
}

template<class D>
struct decoded_dna4_rank_vector {
    using byte_d_t = hn::Rebind<std::uint8_t, D>;
    hn::Vec<byte_d_t> ranks;
    bool crossed_word = false;
};

template<class D>
struct decoded_nt4_rank_vector {
    using byte_d_t = hn::Rebind<std::uint8_t, D>;
    hn::Vec<byte_d_t> codes;
    std::uint64_t ambiguous_lanes = 0;
    bool crossed_word = false;
};

/** Select one lane-sized chunk from the two-word packed decode staging pair. */
template<class Lane>
[[nodiscard, gnu::always_inline]] HWY_INLINE Lane
extract_packed_dna4_lane_chunk(
    std::uint64_t packed_low,
    std::uint64_t packed_high,
    std::size_t origin) noexcept
{
    static_assert(std::is_unsigned_v<Lane>);
    static_assert(sizeof(Lane) == 4 || sizeof(Lane) == 8);
    if (origin < 64) {
        std::uint64_t chunk = packed_low >> origin;
        if (origin != 0) {
            chunk |= packed_high << (64 - origin);
        }
        return static_cast<Lane>(chunk);
    }
    return static_cast<Lane>(packed_high >> (origin - 64));
}

/**
 * Decode up to Lanes(D) consecutive packed DNA4 ranks in real SIMD lanes.
 *
 * The packed source may use 8/16/32/64-bit storage units.  We assemble at
 * most the 128 source bits covering this lane block, then perform rank
 * extraction with vector variable shifts.  This keeps the biological decode
 * vectorized while allowing scanner fragments to forward their packed span
 * without constructing or re-encoding a compact sequence object.
 */
template<class D, class AlphabetTraits>
[[nodiscard]] HWY_INLINE decoded_dna4_rank_vector<D>
decode_packed_dna4_rank_vector(
    D d,
    std::span<const typename AlphabetTraits::storage_unit> packed,
    std::size_t local_bp,
    std::size_t rank_count) noexcept
{
    using lane_type = hn::TFromD<D>;
    using storage_unit = typename AlphabetTraits::storage_unit;
    using unsigned_storage = std::make_unsigned_t<storage_unit>;
    static_assert(std::is_unsigned_v<lane_type>);
    static_assert(sizeof(lane_type) == 4 || sizeof(lane_type) == 8);
    static_assert(AlphabetTraits::bits_per_char == 2);
    static_assert(std::is_integral_v<storage_unit>);
    static_assert(sizeof(storage_unit) == 1 || sizeof(storage_unit) == 2
        || sizeof(storage_unit) == 4 || sizeof(storage_unit) == 8);
    constexpr std::size_t unit_bits = sizeof(storage_unit) * 8;
    constexpr std::size_t letters_per_unit = unit_bits / 2;
    static_assert(AlphabetTraits::letters_per_unit == letters_per_unit);

    const std::size_t first_unit = local_bp / letters_per_unit;
    const std::size_t rank_in_unit = local_bp % letters_per_unit;
    const std::size_t bit_offset = rank_in_unit * 2;
    const std::size_t required_bits = bit_offset + rank_count * 2;
    const std::size_t required_units =
        (required_bits + unit_bits - 1) / unit_bits;

    std::uint64_t packed_low = 0;
    std::uint64_t packed_high = 0;
    // `bit_casted_view` may expose native u64 storage as logical u32 words.
    // Load through the character alias and memcpy into the declared storage
    // type so the zero-copy downcast does not depend on type-based aliasing.
    // Constant-size memcpy lowers to the same unaligned scalar load here.
    const auto* packed_bytes = reinterpret_cast<const unsigned char*>(
        packed.data());
    for (std::size_t unit_offset = 0;
         unit_offset < required_units;
         ++unit_offset) {
        unsigned_storage storage_value = 0;
        std::memcpy(
            &storage_value,
            packed_bytes
                + (first_unit + unit_offset) * sizeof(storage_unit),
            sizeof(storage_unit));
        const std::uint64_t value = static_cast<std::uint64_t>(
            storage_value);
        const std::size_t destination_bit = unit_offset * unit_bits;
        if (destination_bit < 64) {
            packed_low |= value << destination_bit;
            const std::size_t low_bits_available = 64 - destination_bit;
            if (unit_bits > low_bits_available) {
                packed_high |= value >> low_bits_available;
            }
        } else {
            packed_high |= value << (destination_bit - 64);
        }
    }

    constexpr std::size_t lane_bits = sizeof(lane_type) * 8;
    const std::size_t chunk_origin = bit_offset / lane_bits * lane_bits;
    const lane_type packed_chunk_low =
        extract_packed_dna4_lane_chunk<lane_type>(
            packed_low, packed_high, chunk_origin);
    const lane_type packed_chunk_high =
        extract_packed_dna4_lane_chunk<lane_type>(
            packed_low, packed_high, chunk_origin + lane_bits);
    const lane_type relative_bit_offset = static_cast<lane_type>(
        bit_offset - chunk_origin);

    const auto shifts = hn::Mul(
        lane_indices(d),
        hn::Set(d, lane_type{2}));
    const auto source_shifts = hn::Add(
        shifts, hn::Set(d, relative_bit_offset));
    const auto shift_mod_lane = hn::And(
        source_shifts,
        hn::Set(d, static_cast<lane_type>(lane_bits - 1)));
    const auto use_high = hn::Ge(
        source_shifts, hn::Set(d, static_cast<lane_type>(lane_bits)));
    const auto extracted = hn::IfThenElse(
        use_high,
        hn::Shr(hn::Set(d, packed_chunk_high), shift_mod_lane),
        hn::Shr(hn::Set(d, packed_chunk_low), shift_mod_lane));
    const auto ranks = hn::And(
        extracted,
        hn::Set(d, lane_type{3}));
    const hn::Rebind<std::uint16_t, D> d_u16;
    const hn::Rebind<std::uint8_t, D> d_u8;
    if constexpr (sizeof(lane_type) == 8) {
        const hn::Rebind<std::uint32_t, D> d_u32;
        const auto ranks_u32 = hn::DemoteTo(d_u32, ranks);
        const auto ranks_u16 = hn::DemoteTo(d_u16, ranks_u32);
        return {
            .ranks = hn::DemoteTo(d_u8, ranks_u16),
            .crossed_word = required_units > 1,
        };
    } else {
        const auto ranks_u16 = hn::DemoteTo(d_u16, ranks);
        return {
            .ranks = hn::DemoteTo(d_u8, ranks_u16),
            .crossed_word = required_units > 1,
        };
    }
}

/**
 * Decode packed nucleotide ranks and normalize them to minimap2 nt4 codes.
 *
 * Unlike dna4, alphabets such as dna5 leave unused high bits in every packed
 * storage unit (three bits per symbol do not divide 8/16/32/64).  Treating the
 * storage words as one contiguous bit stream would therefore shift every rank
 * after a word boundary.  The address/shift arrays below describe the source
 * word for each lane; rank extraction, rank-to-nt4 normalization, and
 * ambiguity classification all execute as Highway vector operations.  The
 * scalar loop only prepares gather-equivalent addresses and never hashes,
 * canonicalizes, reduces a minimizer window, or emits an event.
 */
template<class D, class AlphabetTraits>
[[nodiscard]] HWY_INLINE decoded_nt4_rank_vector<D>
decode_packed_nt4_rank_vector(
    D d,
    std::span<const typename AlphabetTraits::storage_unit> packed,
    std::size_t local_bp,
    std::size_t rank_count) noexcept
{
    using alphabet_type = typename AlphabetTraits::alphabet_type;
    using storage_unit = typename AlphabetTraits::storage_unit;
    using unsigned_storage = std::make_unsigned_t<storage_unit>;
    static_assert(seqan3::nucleotide_alphabet<alphabet_type>);
    static_assert(std::is_integral_v<storage_unit>);
    static_assert(sizeof(storage_unit) == 1 || sizeof(storage_unit) == 2
        || sizeof(storage_unit) == 4 || sizeof(storage_unit) == 8);
    static_assert(AlphabetTraits::bits_per_char > 0
        && AlphabetTraits::bits_per_char <= 8);
    constexpr std::size_t unit_bits = sizeof(storage_unit) * 8;
    constexpr std::size_t bits_per_char = AlphabetTraits::bits_per_char;
    constexpr std::size_t letters_per_unit = unit_bits / bits_per_char;
    static_assert(AlphabetTraits::letters_per_unit == letters_per_unit);
    constexpr std::size_t max_lanes = hn::MaxLanes(D{});

    HWY_ALIGN std::array<std::uint64_t, max_lanes> source_words{};
    HWY_ALIGN std::array<std::uint64_t, max_lanes> source_shifts{};
    for (std::size_t lane = 0; lane < rank_count; ++lane) {
        const std::size_t bp = local_bp + lane;
        const std::size_t unit = bp / letters_per_unit;
        const std::size_t rank_in_unit = bp % letters_per_unit;
        source_words[lane] = static_cast<std::uint64_t>(
            static_cast<unsigned_storage>(packed[unit]));
        source_shifts[lane] = rank_in_unit * bits_per_char;
    }

    const auto ranks = hn::And(
        hn::Shr(
            hn::LoadU(d, source_words.data()),
            hn::LoadU(d, source_shifts.data())),
        hn::Set(d, (std::uint64_t{1} << bits_per_char) - 1));
    auto nt4_codes = hn::Set(
        d, static_cast<std::uint64_t>(kNt4Sentinel));
    for (std::size_t rank = 0;
         rank < seqan3::alphabet_size<alphabet_type>;
         ++rank) {
        nt4_codes = hn::IfThenElse(
            hn::Eq(ranks, hn::Set(d, static_cast<std::uint64_t>(rank))),
            hn::Set(d, static_cast<std::uint64_t>(
                nt4_lut_v<alphabet_type>[rank])),
            nt4_codes);
    }
    const auto active = hn::FirstN(d, rank_count);
    const auto ambiguous = hn::And(
        active,
        hn::Ge(
            nt4_codes,
            hn::Set(d, static_cast<std::uint64_t>(kNt4Sentinel))));

    const hn::Rebind<std::uint32_t, D> d_u32;
    const hn::Rebind<std::uint16_t, D> d_u16;
    const hn::Rebind<std::uint8_t, D> d_u8;
    const auto codes_u32 = hn::DemoteTo(d_u32, nt4_codes);
    const auto codes_u16 = hn::DemoteTo(d_u16, codes_u32);
    const std::size_t first_unit = local_bp / letters_per_unit;
    const std::size_t last_unit = rank_count == 0
        ? first_unit
        : (local_bp + rank_count - 1) / letters_per_unit;
    return {
        .codes = hn::DemoteTo(d_u8, codes_u16),
        .ambiguous_lanes = static_cast<std::uint64_t>(
            hn::BitsFromMask(d, ambiguous)),
        .crossed_word = first_unit != last_unit,
    };
}

template<class D>
[[nodiscard]] HWY_INLINE hn::Vec<D> load_promoted_dna4_codes(
    D d,
    const std::uint8_t* HWY_RESTRICT codes) noexcept
{
    const hn::Rebind<std::uint8_t, D> d_u8;
    const hn::Rebind<std::uint16_t, D> d_u16;
    const hn::Rebind<std::uint32_t, D> d_u32;
    const auto codes_u8 = hn::LoadU(d_u8, codes);
    const auto codes_u16 = hn::PromoteTo(d_u16, codes_u8);
    const auto codes_u32 = hn::PromoteTo(d_u32, codes_u16);
    return hn::PromoteTo(d, codes_u32);
}

template<class D>
[[nodiscard]] HWY_INLINE hn::Vec<D> load_promoted_dna4_codes_u32(
    D d,
    const std::uint8_t* HWY_RESTRICT codes) noexcept
{
    static_assert(std::is_same_v<hn::TFromD<D>, std::uint32_t>);
    const hn::Rebind<std::uint8_t, D> d_u8;
    const hn::Rebind<std::uint16_t, D> d_u16;
    const auto codes_u8 = hn::LoadU(d_u8, codes);
    return hn::PromoteTo(d, hn::PromoteTo(d_u16, codes_u8));
}

template<class D32, class D64>
struct split_u32_mask_result {
    hn::Mask<D64> lower;
    hn::Mask<D64> upper;
};

/** Split one u32x4 decision mask into its two chronological u64x2 halves. */
template<class D32, class D64>
[[nodiscard]] HWY_INLINE split_u32_mask_result<D32, D64> split_u32_mask(
    D32 d32,
    D64 d64,
    hn::Mask<D32> mask) noexcept
{
    const auto bits = hn::VecFromMask(d32, mask);
    const auto zero = hn::Zero(d64);
    return {
        .lower = hn::Ne(hn::PromoteLowerTo(d64, bits), zero),
        .upper = hn::Ne(hn::PromoteUpperTo(d64, bits), zero),
    };
}

template<class D32, class D64>
[[nodiscard]] HWY_INLINE hn::Mask<D32> join_u64_masks(
    D32 d32,
    D64 d64,
    hn::Mask<D64> lower,
    hn::Mask<D64> upper) noexcept
{
    const hn::Half<D32> d32_half;
    return hn::CombineMasks(
        d32,
        hn::DemoteMaskTo(d32_half, d64, upper),
        hn::DemoteMaskTo(d32_half, d64, lower));
}

template<class D32, class D64>
struct exact_fast_minimum_vectors {
    hn::Vec<D32> hash;
    hn::Vec<D64> raw_lower;
    hn::Vec<D64> raw_upper;
    hn::Vec<D64> ordinal_lower;
    hn::Vec<D64> ordinal_upper;
};

/**
 * Reduce four overlapping K15/W10 hash windows.  The sequential <= scan
 * deliberately preserves the exact rightmost-minimum rule used by the
 * scalar oracle; metadata follows the same mask at every chronological slot.
 */
template<class D32, class D64>
[[nodiscard]] HWY_INLINE exact_fast_minimum_vectors<D32, D64>
reduce_exact_k15_w10_minimum_metadata(
    D32 d32,
    D64 d64,
    const std::uint32_t* HWY_RESTRICT candidate_hash,
    const std::uint64_t* HWY_RESTRICT candidate_raw,
    const std::uint64_t* HWY_RESTRICT candidate_ordinal,
    std::uint32_t maximum_hash,
    std::uint64_t maximum_raw,
    std::uint64_t ordinal_sentinel) noexcept
{
    static_assert(std::is_same_v<hn::TFromD<D32>, std::uint32_t>);
    static_assert(std::is_same_v<hn::TFromD<D64>, std::uint64_t>);
    constexpr std::size_t window = 10;
    exact_fast_minimum_vectors<D32, D64> minimum{
        .hash = hn::Set(d32, maximum_hash),
        .raw_lower = hn::Set(d64, maximum_raw),
        .raw_upper = hn::Set(d64, maximum_raw),
        .ordinal_lower = hn::Set(d64, ordinal_sentinel),
        .ordinal_upper = hn::Set(d64, ordinal_sentinel),
    };

    for (std::size_t slot = 0; slot < window; ++slot) {
        const auto slot_hash = hn::LoadU(d32, candidate_hash + slot);
        const auto take = hn::Le(slot_hash, minimum.hash);
        const auto take_halves = split_u32_mask(d32, d64, take);
        minimum.hash = hn::IfThenElse(take, slot_hash, minimum.hash);
        minimum.raw_lower = hn::IfThenElse(
            take_halves.lower,
            hn::LoadU(d64, candidate_raw + slot),
            minimum.raw_lower);
        minimum.raw_upper = hn::IfThenElse(
            take_halves.upper,
            hn::LoadU(d64, candidate_raw + slot + 2),
            minimum.raw_upper);
        minimum.ordinal_lower = hn::IfThenElse(
            take_halves.lower,
            hn::LoadU(d64, candidate_ordinal + slot),
            minimum.ordinal_lower);
        minimum.ordinal_upper = hn::IfThenElse(
            take_halves.upper,
            hn::LoadU(d64, candidate_ordinal + slot + 2),
            minimum.ordinal_upper);
    }
    return minimum;
}

/**
 * Four chronological K15/W10 windows in parallel. Hash decisions stay in
 * u32 lanes; raw minimizers and ordinals stay in u64 lanes throughout.
 */
template<
    class MinimizerT,
    class ContinuationState,
    class Collector>
[[nodiscard]] inline TwoStackContinuationStatus
process_exact_k15_w10_u32_group(
    ContinuationState& state,
    Collector& collector,
    const std::uint32_t* HWY_RESTRICT candidate_hash,
    const std::uint64_t* HWY_RESTRICT candidate_raw,
    const std::uint64_t* HWY_RESTRICT candidate_ordinal,
    std::size_t active_lanes)
{
    static_assert(exact_k15_w10_b8_representation_v<MinimizerT>);

    using d32_t = hn::ScalableTag<std::uint32_t>;
    using d64_t = hn::ScalableTag<std::uint64_t>;
    using v32_t = hn::Vec<d32_t>;
    using v64_t = hn::Vec<d64_t>;
    using m32_t = hn::Mask<d32_t>;
    const d32_t d32;
    const d64_t d64;
    constexpr std::size_t window = 10;
    constexpr std::size_t tail_size = window - 1;
    constexpr std::size_t lanes32 = 4;
    constexpr std::size_t row_stride = 12;
    constexpr std::uint64_t ordinal_sentinel =
        (std::numeric_limits<std::uint64_t>::max)();
    const std::uint32_t maximum_hash = static_cast<std::uint32_t>(
        MinimizerT::getMax().kmer());
    const std::uint64_t maximum_raw = MinimizerT::getMax().raw_value();

    if (state.status != TwoStackContinuationStatus::ok) return state.status;
    if (hn::Lanes(d32) != lanes32 || hn::Lanes(d64) != 2
        || active_lanes == 0 || active_lanes > lanes32) {
        return reject_continuation(
            state, TwoStackContinuationStatus::unsupported_vector_shape);
    }
    if (state.candidate_count
            > (std::numeric_limits<std::uint64_t>::max)() - active_lanes) {
        return reject_continuation(
            state, TwoStackContinuationStatus::arithmetic_overflow);
    }

    if (!state.candidate_state_initialized) {
        std::fill(state.candidate_tail_hash.begin(),
            state.candidate_tail_hash.end(), maximum_hash);
        std::fill(state.candidate_tail_raw.begin(),
            state.candidate_tail_raw.end(), maximum_raw);
        std::fill(state.candidate_tail_ordinal.begin(),
            state.candidate_tail_ordinal.end(), ordinal_sentinel);
        state.current_min = {
            .hash = maximum_hash,
            .raw = maximum_raw,
            .ordinal = ordinal_sentinel,
        };
        state.candidate_state_initialized = true;
        add_proof_counter(state.proof.scalar_orchestration_steps, 1);
    }

    HWY_ALIGN std::array<std::uint32_t, tail_size + lanes32>
        combined_hash;
    HWY_ALIGN std::array<std::uint64_t, tail_size + lanes32>
        combined_raw;
    HWY_ALIGN std::array<std::uint64_t, tail_size + lanes32>
        combined_ordinal;
    for (std::size_t slot = 0; slot < tail_size; ++slot) {
        if (state.candidate_tail_hash[slot]
                > (std::numeric_limits<std::uint32_t>::max)()) {
            return reject_continuation(
                state, TwoStackContinuationStatus::invalid_state);
        }
        combined_hash[slot] = static_cast<std::uint32_t>(
            state.candidate_tail_hash[slot]);
        combined_raw[slot] = state.candidate_tail_raw[slot];
        combined_ordinal[slot] = state.candidate_tail_ordinal[slot];
    }
    std::memcpy(combined_hash.data() + tail_size,
        candidate_hash, lanes32 * sizeof(std::uint32_t));
    std::memcpy(combined_raw.data() + tail_size,
        candidate_raw, lanes32 * sizeof(std::uint64_t));
    std::memcpy(combined_ordinal.data() + tail_size,
        candidate_ordinal, lanes32 * sizeof(std::uint64_t));

    const exact_fast_minimum_vectors<d32_t, d64_t> minimum =
        reduce_exact_k15_w10_minimum_metadata(
            d32,
            d64,
            combined_hash.data(),
            combined_raw.data(),
            combined_ordinal.data(),
            maximum_hash,
            maximum_raw,
            ordinal_sentinel);

    HWY_ALIGN std::array<std::uint32_t, lanes32 + 1> previous_hash_storage;
    HWY_ALIGN std::array<std::uint64_t, lanes32 + 1> previous_raw_storage;
    HWY_ALIGN std::array<std::uint64_t, lanes32 + 1>
        previous_ordinal_storage;
    previous_hash_storage[0] = static_cast<std::uint32_t>(
        state.current_min.hash);
    previous_raw_storage[0] = state.current_min.raw;
    previous_ordinal_storage[0] = state.current_min.ordinal;
    hn::StoreU(minimum.hash, d32, previous_hash_storage.data() + 1);
    hn::StoreU(minimum.raw_lower, d64, previous_raw_storage.data() + 1);
    hn::StoreU(minimum.raw_upper, d64, previous_raw_storage.data() + 3);
    hn::StoreU(
        minimum.ordinal_lower, d64, previous_ordinal_storage.data() + 1);
    hn::StoreU(
        minimum.ordinal_upper, d64, previous_ordinal_storage.data() + 3);

    const v32_t previous_hash = hn::LoadU(d32,
        previous_hash_storage.data());
    const v64_t previous_raw_lower = hn::LoadU(d64,
        previous_raw_storage.data());
    const v64_t previous_raw_upper = hn::LoadU(d64,
        previous_raw_storage.data() + 2);
    const v64_t previous_ordinal_lower = hn::LoadU(d64,
        previous_ordinal_storage.data());
    const v64_t previous_ordinal_upper = hn::LoadU(d64,
        previous_ordinal_storage.data() + 2);
    const v32_t entering_hash = hn::LoadU(
        d32, combined_hash.data() + tail_size);
    const m32_t active = hn::FirstN(d32, active_lanes);
    const m32_t previous_valid = hn::Ne(
        previous_hash, hn::Set(d32, maximum_hash));

    HWY_ALIGN std::array<std::uint64_t, lanes32> ordinal_storage;
    std::memcpy(ordinal_storage.data(), candidate_ordinal,
        lanes32 * sizeof(std::uint64_t));
    const v64_t ordinal_lower = hn::LoadU(d64, ordinal_storage.data());
    const v64_t ordinal_upper = hn::LoadU(d64, ordinal_storage.data() + 2);
    const auto active_halves = split_u32_mask(d32, d64, active);
    const auto first_lower = hn::And(
        active_halves.lower,
        hn::Eq(ordinal_lower, hn::Set(d64, std::uint64_t{window - 1})));
    const auto first_upper = hn::And(
        active_halves.upper,
        hn::Eq(ordinal_upper, hn::Set(d64, std::uint64_t{window - 1})));
    const auto steady_lower = hn::And(
        active_halves.lower,
        hn::Gt(ordinal_lower, hn::Set(d64, std::uint64_t{window - 1})));
    const auto steady_upper = hn::And(
        active_halves.upper,
        hn::Gt(ordinal_upper, hn::Set(d64, std::uint64_t{window - 1})));
    const m32_t first_full_window = join_u64_masks(
        d32, d64, first_lower, first_upper);
    const m32_t steady_window = join_u64_masks(
        d32, d64, steady_lower, steady_upper);

    const m32_t entering_wins = hn::Le(entering_hash, previous_hash);
    const auto entering_halves = split_u32_mask(d32, d64, entering_wins);
    const v64_t expired_ordinal_lower = hn::Sub(
        ordinal_lower, hn::Set(d64, std::uint64_t{window}));
    const v64_t expired_ordinal_upper = hn::Sub(
        ordinal_upper, hn::Set(d64, std::uint64_t{window}));
    const auto expired_lower = hn::And(
        steady_lower,
        hn::AndNot(
            entering_halves.lower,
            hn::Eq(previous_ordinal_lower, expired_ordinal_lower)));
    const auto expired_upper = hn::And(
        steady_upper,
        hn::AndNot(
            entering_halves.upper,
            hn::Eq(previous_ordinal_upper, expired_ordinal_upper)));
    const m32_t expired = join_u64_masks(
        d32, d64, expired_lower, expired_upper);
    const m32_t emit_old = hn::And(
        steady_window,
        hn::And(previous_valid, hn::Or(entering_wins, expired)));
    const m32_t special_ties = hn::And(first_full_window, previous_valid);
    const m32_t expiry_ties = hn::And(
        expired,
        hn::Ne(minimum.hash, hn::Set(d32, maximum_hash)));

    const m32_t possible_ties = hn::Or(special_ties, expiry_ties);
    m32_t actual_ties = hn::MaskFalse(d32);
    if (hn::CountTrue(d32, possible_ties) != 0) [[unlikely]] {
        m32_t seen_special_hash = hn::MaskFalse(d32);
        m32_t seen_expiry_hash = hn::MaskFalse(d32);
        m32_t duplicate_special_hash = hn::MaskFalse(d32);
        m32_t duplicate_expiry_hash = hn::MaskFalse(d32);
        for (std::size_t slot = 0; slot < window; ++slot) {
            const v32_t slot_hash = hn::LoadU(
                d32, combined_hash.data() + slot);
            const m32_t special_hash_match = hn::And(
                special_ties, hn::Eq(slot_hash, previous_hash));
            const m32_t expiry_hash_match = hn::And(
                expiry_ties, hn::Eq(slot_hash, minimum.hash));
            duplicate_special_hash = hn::Or(
                duplicate_special_hash,
                hn::And(seen_special_hash, special_hash_match));
            duplicate_expiry_hash = hn::Or(
                duplicate_expiry_hash,
                hn::And(seen_expiry_hash, expiry_hash_match));
            seen_special_hash = hn::Or(
                seen_special_hash, special_hash_match);
            seen_expiry_hash = hn::Or(
                seen_expiry_hash, expiry_hash_match);
        }
        const m32_t duplicate_hashes = hn::Or(
            duplicate_special_hash, duplicate_expiry_hash);
        add_proof_counter(state.proof.tie_hash_prefilter_batches, 1);
        add_proof_counter(
            state.proof.tie_hash_prefilter_rejected_lanes,
            hn::CountTrue(
                d32, hn::AndNot(duplicate_hashes, possible_ties)));
        if (hn::CountTrue(d32, duplicate_hashes) != 0) [[unlikely]] {
            add_proof_counter(state.proof.tie_raw_verification_batches, 1);
            for (std::size_t slot = 0; slot < window; ++slot) {
                const v32_t slot_hash = hn::LoadU(
                    d32, combined_hash.data() + slot);
                const v64_t slot_raw_lower = hn::LoadU(
                    d64, combined_raw.data() + slot);
                const v64_t slot_raw_upper = hn::LoadU(
                    d64, combined_raw.data() + slot + 2);
                const m32_t differs_previous = join_u64_masks(
                    d32,
                    d64,
                    hn::Ne(slot_raw_lower, previous_raw_lower),
                    hn::Ne(slot_raw_upper, previous_raw_upper));
                const m32_t differs_minimum = join_u64_masks(
                    d32,
                    d64,
                    hn::Ne(slot_raw_lower, minimum.raw_lower),
                    hn::Ne(slot_raw_upper, minimum.raw_upper));
                const m32_t special_match = hn::And(
                    duplicate_special_hash,
                    hn::And(
                        hn::Eq(slot_hash, previous_hash), differs_previous));
                const m32_t expiry_match = hn::And(
                    duplicate_expiry_hash,
                    hn::And(
                        hn::Eq(slot_hash, minimum.hash), differs_minimum));
                actual_ties = hn::Or(
                    actual_ties, hn::Or(special_match, expiry_match));
            }
        }
    }

    HWY_ALIGN std::array<std::uint64_t, lanes32 * row_stride>
        compressed_source;
    HWY_ALIGN std::array<std::uint64_t, lanes32 * row_stride>
        compressed_raw;
    std::size_t compressed_count = 0;
    if (hn::CountTrue(d32, actual_ties) == 0) [[likely]] {
        const auto emit_halves = split_u32_mask(d32, d64, emit_old);
        compressed_count += hn::CompressStore(
            previous_raw_lower,
            emit_halves.lower,
            d64,
            compressed_raw.data());
        compressed_count += hn::CompressStore(
            previous_raw_upper,
            emit_halves.upper,
            d64,
            compressed_raw.data() + compressed_count);
        add_proof_counter(state.proof.stable_compress_batches, 2);
        add_proof_counter(state.proof.direct_event_compress_batches, 1);
    } else {
        std::array<v64_t, row_stride> event_lower{};
        std::array<v64_t, row_stride> event_upper{};
        const v64_t max_raw_v = hn::Set(d64, maximum_raw);
        const auto emit_halves = split_u32_mask(d32, d64, emit_old);
        event_lower[0] = hn::IfThenElse(
            emit_halves.lower, previous_raw_lower, max_raw_v);
        event_upper[0] = hn::IfThenElse(
            emit_halves.upper, previous_raw_upper, max_raw_v);
        std::uint64_t first_tie_records = 0;
        std::uint64_t expiry_tie_records = 0;
        for (std::size_t slot = 0; slot < window; ++slot) {
            const v32_t slot_hash = hn::LoadU(
                d32, combined_hash.data() + slot);
            const v64_t slot_raw_lower = hn::LoadU(
                d64, combined_raw.data() + slot);
            const v64_t slot_raw_upper = hn::LoadU(
                d64, combined_raw.data() + slot + 2);
            const m32_t differs_previous = join_u64_masks(
                d32,
                d64,
                hn::Ne(slot_raw_lower, previous_raw_lower),
                hn::Ne(slot_raw_upper, previous_raw_upper));
            const m32_t differs_minimum = join_u64_masks(
                d32,
                d64,
                hn::Ne(slot_raw_lower, minimum.raw_lower),
                hn::Ne(slot_raw_upper, minimum.raw_upper));
            const m32_t special_match = hn::And(
                special_ties,
                hn::And(
                    hn::Eq(slot_hash, previous_hash), differs_previous));
            const m32_t expiry_match = hn::And(
                expiry_ties,
                hn::And(
                    hn::Eq(slot_hash, minimum.hash), differs_minimum));
            first_tie_records += hn::CountTrue(d32, special_match);
            expiry_tie_records += hn::CountTrue(d32, expiry_match);
            const auto match_halves = split_u32_mask(
                d32, d64, hn::Or(special_match, expiry_match));
            event_lower[slot + 1] = hn::IfThenElse(
                match_halves.lower, slot_raw_lower, max_raw_v);
            event_upper[slot + 1] = hn::IfThenElse(
                match_halves.upper, slot_raw_upper, max_raw_v);
        }
        event_lower[row_stride - 1] = max_raw_v;
        event_upper[row_stride - 1] = max_raw_v;

        for (std::size_t column = 0; column < row_stride; column += 2) {
            hn::StoreU(
                hn::InterleaveLower(
                    d64, event_lower[column], event_lower[column + 1]),
                d64,
                compressed_source.data() + column);
            hn::StoreU(
                hn::InterleaveUpper(
                    d64, event_lower[column], event_lower[column + 1]),
                d64,
                compressed_source.data() + row_stride + column);
            hn::StoreU(
                hn::InterleaveLower(
                    d64, event_upper[column], event_upper[column + 1]),
                d64,
                compressed_source.data() + 2 * row_stride + column);
            hn::StoreU(
                hn::InterleaveUpper(
                    d64, event_upper[column], event_upper[column + 1]),
                d64,
                compressed_source.data() + 3 * row_stride + column);
            add_proof_counter(state.proof.row_transpose_batches, 1);
            add_proof_counter(state.proof.row_vector_stores, 4);
        }
        const v64_t raw_hash_mask = hn::Set(
            d64, static_cast<std::uint64_t>(MinimizerT::hash_mask));
        for (std::size_t offset = 0;
             offset < lanes32 * row_stride;
             offset += 2) {
            const v64_t events = hn::LoadU(
                d64, compressed_source.data() + offset);
            const auto valid = hn::Ne(
                hn::And(events, raw_hash_mask), raw_hash_mask);
            compressed_count += hn::CompressStore(
                events,
                valid,
                d64,
                compressed_raw.data() + compressed_count);
            add_proof_counter(state.proof.stable_compress_batches, 1);
        }
        add_proof_counter(state.proof.tie_matrix_event_batches, 1);
        add_proof_counter(
            state.proof.first_window_tie_events, first_tie_records);
        add_proof_counter(
            state.proof.expiry_tie_events, expiry_tie_records);
    }

    if (compressed_count != 0
        && !collector.append_raw_storage(
            compressed_raw.data(), compressed_count)) {
        return reject_continuation(
            state, TwoStackContinuationStatus::collector_overflow);
    }

    state.current_min = {
        .hash = previous_hash_storage[active_lanes],
        .raw = previous_raw_storage[active_lanes],
        .ordinal = previous_ordinal_storage[active_lanes],
    };
    for (std::size_t slot = 0; slot < tail_size; ++slot) {
        state.candidate_tail_hash[slot] = combined_hash[active_lanes + slot];
        state.candidate_tail_raw[slot] = combined_raw[active_lanes + slot];
        state.candidate_tail_ordinal[slot] =
            combined_ordinal[active_lanes + slot];
    }
    state.candidate_count += active_lanes;
    add_proof_counter(state.proof.minimizers_emitted, compressed_count);
    add_proof_counter(state.proof.vector_batches, 1);
    add_proof_counter(state.proof.vector_lanes_visited, active_lanes);
    add_proof_counter(
        state.proof.k15_w10_b8_u32_hash_candidate_blocks, 1);
    add_proof_counter(
        state.proof.k15_w10_b8_u32_hash_lane_visits, active_lanes);
    add_proof_counter(state.proof.masked_tail_lanes, lanes32 - active_lanes);
    add_proof_counter(state.proof.consecutive_kmer_batches, 1);
    add_proof_counter(state.proof.canonical_scramble_batches, 1);
    add_proof_counter(state.proof.window_reduction_batches, 1);
    add_proof_counter(state.proof.event_generation_batches, 1);
    add_proof_counter(
        state.proof.entering_min_transitions,
        hn::CountTrue(d32, hn::And(steady_window, entering_wins)));
    add_proof_counter(
        state.proof.expiry_rescan_transitions,
        hn::CountTrue(d32, expired));
    add_proof_counter(state.proof.scalar_orchestration_steps, 8);
    return TwoStackContinuationStatus::ok;
}

/** Pack eight byte-staged dna4 ranks into one chronological 16-bit word. */
[[nodiscard, gnu::always_inline]] inline std::uint16_t
pack_eight_dna4_codes_swar(
    const std::uint8_t* HWY_RESTRICT codes) noexcept
{
    std::uint64_t word = 0;
    std::memcpy(&word, codes, sizeof(word));
    if constexpr (std::endian::native == std::endian::big) {
        word = std::byteswap(word);
    }
    word &= 0x0303030303030303ull;
    word = (word | (word >> 6u)) & 0x000f000f000f000full;
    word = (word | (word >> 12u)) & 0x000000ff000000ffull;
    word = (word | (word >> 24u)) & 0x000000000000ffffull;
    return static_cast<std::uint16_t>(word);
}

/** Pack the zero-padded 22-base K15/B8 source block into 44 bits. */
[[nodiscard, gnu::always_inline]] inline std::uint64_t
pack_k15_b8_dna4_codes_swar(
    const std::uint8_t* HWY_RESTRICT codes) noexcept
{
    const std::uint64_t lower = pack_eight_dna4_codes_swar(codes);
    const std::uint64_t middle = pack_eight_dna4_codes_swar(codes + 8);
    const std::uint64_t upper = pack_eight_dna4_codes_swar(codes + 16);
    return lower | (middle << 16u) | ((upper & 0x0fffull) << 32u);
}

/** Reverse the order of sixteen two-bit dna4 groups in every u32 lane. */
template<class D>
[[nodiscard, gnu::always_inline]] HWY_INLINE hn::Vec<D>
reverse_two_bit_groups(D d, hn::Vec<D> value) noexcept
{
    static_assert(std::is_same_v<hn::TFromD<D>, std::uint32_t>);
    const auto swap2 = hn::Set(d, std::uint32_t{0x33333333u});
    value = hn::Or(
        hn::And(hn::ShiftRight<2>(value), swap2),
        hn::ShiftLeft<2>(hn::And(value, swap2)));
    const auto swap4 = hn::Set(d, std::uint32_t{0x0f0f0f0fu});
    value = hn::Or(
        hn::And(hn::ShiftRight<4>(value), swap4),
        hn::ShiftLeft<4>(hn::And(value, swap4)));
    const auto swap8 = hn::Set(d, std::uint32_t{0x00ff00ffu});
    value = hn::Or(
        hn::And(hn::ShiftRight<8>(value), swap8),
        hn::ShiftLeft<8>(hn::And(value, swap8)));
    return hn::ShiftRight<2>(hn::Or(
        hn::ShiftRight<16>(value), hn::ShiftLeft<16>(value)));
}

template<class D32>
struct exact_k15_b8_windows {
    hn::Vec<D32> lower;
    hn::Vec<D32> upper;
};

/**
 * Extract eight consecutive K15 words from one packed 22-base block.
 *
 * Input assembly is scalar SWAR staging, but all eight biological K-mer
 * extractions are variable-shift SIMD work in four u64x2 pairs.  Demoting and
 * combining those pairs yields the two chronological u32x4 candidate groups
 * consumed by the existing canonical/hash/window/event pipeline.
 */
template<class D32, class D64>
[[nodiscard, gnu::always_inline]] HWY_INLINE exact_k15_b8_windows<D32>
extract_exact_k15_b8_windows(
    D32 d32,
    D64 d64,
    std::uint64_t packed22) noexcept
{
    static_assert(std::is_same_v<hn::TFromD<D32>, std::uint32_t>);
    static_assert(std::is_same_v<hn::TFromD<D64>, std::uint64_t>);
    const auto packed = hn::Set(d64, packed22);
    const auto ordinal_bits = hn::ShiftLeft<1>(lane_indices(d64));
    const auto mask = hn::Set(d64, std::uint64_t{0x3fffffffull});
    const auto pair0 = hn::And(hn::Shr(packed, ordinal_bits), mask);
    const auto pair1 = hn::And(
        hn::Shr(
            packed,
            hn::Add(ordinal_bits, hn::Set(d64, std::uint64_t{4}))),
        mask);
    const auto pair2 = hn::And(
        hn::Shr(
            packed,
            hn::Add(ordinal_bits, hn::Set(d64, std::uint64_t{8}))),
        mask);
    const auto pair3 = hn::And(
        hn::Shr(
            packed,
            hn::Add(ordinal_bits, hn::Set(d64, std::uint64_t{12}))),
        mask);
    const hn::Half<D32> d32_half;
    return {
        .lower = hn::Combine(
            d32,
            hn::DemoteTo(d32_half, pair1),
            hn::DemoteTo(d32_half, pair0)),
        .upper = hn::Combine(
            d32,
            hn::DemoteTo(d32_half, pair3),
            hn::DemoteTo(d32_half, pair2)),
    };
}

/** Fixed, exact K15/W10 block: two u32x4 hash groups, u64 raw state. */
template<class MinimizerT, class ContinuationState, class Collector>
[[nodiscard]] inline TwoStackContinuationStatus
process_exact_k15_w10_b8_candidate_block(
    ContinuationState& state,
    Collector& collector,
    std::size_t active_candidates)
{
    static_assert(std::is_same_v<typename ContinuationState::alphabet_type,
        seq::dna4>);
    static_assert(exact_k15_w10_b8_representation_v<MinimizerT>);
    using d32_t = hn::ScalableTag<std::uint32_t>;
    using d64_t = hn::ScalableTag<std::uint64_t>;
    const d32_t d32;
    const d64_t d64;
    constexpr std::size_t kmer = 15;
    constexpr std::size_t block = 8;
    constexpr std::size_t lanes = 4;
    constexpr std::uint32_t kmer_mask = (std::uint32_t{1} << 30) - 1;
    const std::uint32_t maximum_hash = static_cast<std::uint32_t>(
        MinimizerT::getMax().kmer());
    const std::uint64_t maximum_raw = MinimizerT::getMax().raw_value();
    const std::uint64_t ordinal_sentinel =
        (std::numeric_limits<std::uint64_t>::max)();

    if (active_candidates == 0 || active_candidates > block
        || state.code_tail_size != kmer - 1
        || state.pending_count < active_candidates
        || hn::Lanes(d32) != lanes || hn::Lanes(d64) != 2) {
        return reject_continuation(
            state, TwoStackContinuationStatus::invalid_state);
    }
    if (state.candidate_count
            > (std::numeric_limits<std::uint64_t>::max)() - block
        || state.emitted_position_base
            > (std::numeric_limits<std::uint64_t>::max)() - (kmer - 1)
        || state.candidate_count + (block - 1)
            > (std::numeric_limits<std::uint64_t>::max)()
                - state.emitted_position_base - (kmer - 1)) {
        return reject_continuation(
            state, TwoStackContinuationStatus::arithmetic_overflow);
    }

    // Two zero padding bytes make all three SWAR staging loads in-bounds.
    HWY_ALIGN std::array<std::uint8_t, 24> code_scratch{};
    std::memcpy(code_scratch.data(), state.code_tail.data(), kmer - 1);
    std::memcpy(code_scratch.data() + kmer - 1,
        state.pending_codes.data(), active_candidates);

    const auto natural = extract_exact_k15_b8_windows(
        d32, d64, pack_k15_b8_dna4_codes_swar(code_scratch.data()));
    std::array<hn::Vec<d32_t>, 2> forward{
        reverse_two_bit_groups(d32, natural.lower),
        reverse_two_bit_groups(d32, natural.upper)};
    std::array<hn::Vec<d32_t>, 2> reverse{
        hn::Xor(natural.lower, hn::Set(d32, kmer_mask)),
        hn::Xor(natural.upper, hn::Set(d32, kmer_mask))};

    HWY_ALIGN std::array<std::uint32_t, block> candidate_hash;
    HWY_ALIGN std::array<std::uint64_t, block> candidate_raw;
    HWY_ALIGN std::array<std::uint64_t, block> candidate_ordinal;
    for (std::size_t group = 0; group < 2; ++group) {
        const std::size_t group_offset = group * lanes;
        const std::size_t group_active = active_candidates > group_offset
            ? std::min(lanes, active_candidates - group_offset)
            : 0;
        const auto active = hn::FirstN(d32, group_active);
        const auto palindrome = hn::Eq(forward[group], reverse[group]);
        const auto canonical = hn::Min(forward[group], reverse[group]);
        auto hash = minimap_int_hasher<seq::dna4>::template scramble_simd<
            d32_t>(d32, canonical, hn::Set(d32, kmer_mask));
        hash = hn::And(hash, hn::Set(d32, kmer_mask));
        const auto valid = hn::And(
            active,
            hn::AndNot(
                palindrome,
                hn::Ne(hash, hn::Set(d32, maximum_hash))));
        hash = hn::IfThenElse(
            valid, hash, hn::Set(d32, maximum_hash));
        hn::StoreU(hash, d32, candidate_hash.data() + group_offset);

        const auto valid_halves = split_u32_mask(d32, d64, valid);
        const auto active_halves = split_u32_mask(d32, d64, active);
        const auto reverse_strand = hn::Lt(reverse[group], forward[group]);
        const auto strand_halves = split_u32_mask(
            d32, d64, hn::And(active, reverse_strand));
        const std::uint64_t ordinal_base =
            state.candidate_count + group_offset;
        const auto ordinal_lower = hn::Add(
            hn::Set(d64, ordinal_base), lane_indices(d64));
        const auto ordinal_upper = hn::Add(
            hn::Set(d64, ordinal_base + 2),
            lane_indices(d64));
        hn::StoreU(
            hn::IfThenElse(
                active_halves.lower,
                ordinal_lower,
                hn::Set(d64, ordinal_sentinel)),
            d64,
            candidate_ordinal.data() + group_offset);
        hn::StoreU(
            hn::IfThenElse(
                active_halves.upper,
                ordinal_upper,
                hn::Set(d64, ordinal_sentinel)),
            d64,
            candidate_ordinal.data() + group_offset + 2);

        const std::uint64_t position_base =
            state.emitted_position_base + (kmer - 1) + ordinal_base;
        const auto position_lower = hn::Add(
            hn::Set(d64, position_base), lane_indices(d64));
        const auto position_upper = hn::Add(
            hn::Set(d64, position_base + 2),
            lane_indices(d64));
        const auto hash_lower = hn::PromoteLowerTo(d64, hash);
        const auto hash_upper = hn::PromoteUpperTo(d64, hash);
        const auto one = hn::Set(d64, std::uint64_t{1});
        const auto zero = hn::Zero(d64);
        auto raw_lower = hn::Or(
            hn::ShiftLeft<MinimizerT::hash_shift>(hash_lower),
            hn::Or(
                hn::ShiftLeft<MinimizerT::pos_shift>(hn::And(
                    position_lower,
                    hn::Set(d64, static_cast<std::uint64_t>(
                        MinimizerT::pos_field_mask)))),
                hn::IfThenElse(strand_halves.lower, one, zero)));
        auto raw_upper = hn::Or(
            hn::ShiftLeft<MinimizerT::hash_shift>(hash_upper),
            hn::Or(
                hn::ShiftLeft<MinimizerT::pos_shift>(hn::And(
                    position_upper,
                    hn::Set(d64, static_cast<std::uint64_t>(
                        MinimizerT::pos_field_mask)))),
                hn::IfThenElse(strand_halves.upper, one, zero)));
        raw_lower = hn::IfThenElse(
            valid_halves.lower,
            raw_lower,
            hn::Set(d64, maximum_raw));
        raw_upper = hn::IfThenElse(
            valid_halves.upper,
            raw_upper,
            hn::Set(d64, maximum_raw));
        hn::StoreU(raw_lower, d64, candidate_raw.data() + group_offset);
        hn::StoreU(raw_upper, d64, candidate_raw.data() + group_offset + 2);
        add_proof_counter(
            state.proof.palindrome_lanes,
            hn::CountTrue(d32, hn::And(active, palindrome)));
    }

    const std::size_t first_group = std::min(lanes, active_candidates);
    auto status = process_exact_k15_w10_u32_group<MinimizerT>(
        state,
        collector,
        candidate_hash.data(),
        candidate_raw.data(),
        candidate_ordinal.data(),
        first_group);
    if (status != TwoStackContinuationStatus::ok) return status;
    if (active_candidates > lanes) {
        status = process_exact_k15_w10_u32_group<MinimizerT>(
            state,
            collector,
            candidate_hash.data() + lanes,
            candidate_raw.data() + lanes,
            candidate_ordinal.data() + lanes,
            active_candidates - lanes);
        if (status != TwoStackContinuationStatus::ok) return status;
    }

    std::memcpy(state.code_tail.data(),
        code_scratch.data() + active_candidates, kmer - 1);
    const std::uint64_t packed_groups = active_candidates > lanes ? 2u : 1u;
    add_proof_counter(
        state.proof.k15_w10_b8_packed22_groups, packed_groups);
    add_proof_counter(
        state.proof.k15_w10_b8_packed22_lane_visits, active_candidates);
    add_proof_counter(state.proof.scalar_orchestration_steps, 7);
    return TwoStackContinuationStatus::ok;
}

template<class D>
struct canonical_kmer_vectors {
    hn::Vec<D> forward;
    hn::Vec<D> reverse;
};

template<class D>
[[nodiscard]] HWY_INLINE canonical_kmer_vectors<D> incorporate_kmer_codes(
    D d,
    canonical_kmer_vectors<D> current,
    hn::Vec<D> codes,
    std::size_t code_index) noexcept
{
    current.forward = hn::Or(hn::ShiftLeft<2>(current.forward), codes);
    current.reverse = hn::Or(
        current.reverse,
        hn::ShiftLeftSame(
            hn::Xor(codes, hn::Set(d, std::uint64_t{3})),
            static_cast<int>(2 * code_index)));
    return current;
}

template<class D>
struct minimizer_candidate_vectors {
    hn::Vec<D> hash;
    hn::Vec<D> raw;
    hn::Vec<D> ordinal;
};

template<class D>
[[nodiscard]] HWY_INLINE minimizer_candidate_vectors<D> reduce_window_slot(
    D d,
    minimizer_candidate_vectors<D> minimum,
    const std::uint64_t* HWY_RESTRICT combined_hash,
    const std::uint64_t* HWY_RESTRICT combined_raw,
    const std::uint64_t* HWY_RESTRICT combined_ordinal,
    std::size_t slot) noexcept
{
    const auto slot_hash = hn::LoadU(d, combined_hash + slot);
    const auto take = hn::Le(slot_hash, minimum.hash);
    minimum.hash = hn::IfThenElse(take, slot_hash, minimum.hash);
    minimum.raw = hn::IfThenElse(
        take, hn::LoadU(d, combined_raw + slot), minimum.raw);
    minimum.ordinal = hn::IfThenElse(
        take, hn::LoadU(d, combined_ordinal + slot), minimum.ordinal);
    return minimum;
}

template<
    bool InvalidOnly,
    std::size_t W,
    std::size_t K,
    class MinimizerT,
    class ContinuationState,
    class Collector>
[[nodiscard, gnu::always_inline]] inline TwoStackContinuationStatus
process_chronological_candidate_block(
    ContinuationState& state,
    Collector& collector,
    std::size_t active_lanes)
{
    using storage_t = typename MinimizerT::storage_t;
    static_assert(std::is_same_v<storage_t, std::uint64_t>);
    static_assert(std::is_trivially_copyable_v<MinimizerT>);
    static_assert(sizeof(MinimizerT) == sizeof(storage_t));
    static_assert(requires (
        Collector& output,
        const storage_t* raw,
        std::size_t count) {
            { output.append_raw_storage(raw, count) } -> std::same_as<bool>;
        });

    if (state.status != TwoStackContinuationStatus::ok) return state.status;

    using d_t = hn::ScalableTag<std::uint64_t>;
    using vec_t = hn::Vec<d_t>;
    using mask_t = hn::Mask<d_t>;
    d_t d;
    const std::size_t lanes = hn::Lanes(d);
    const auto target_status = validate_exact_vector_shape(
        state,
        lanes,
        ContinuationState::max_vector_lanes,
        exact_compiled_target_is_scalar,
        u64_lane_transpose_shape_supported(d));
    if (target_status != TwoStackContinuationStatus::ok) {
        return target_status;
    }

    const std::size_t w = state.window_size();
    const std::size_t k = state.kmer_size();
    if (w == 0 || w > two_stack_max_window) {
        return reject_continuation(
            state, TwoStackContinuationStatus::invalid_window_size);
    }
    if (k == 0 || k > two_stack_exact_max_kmer) {
        return reject_continuation(
            state, TwoStackContinuationStatus::invalid_kmer_size);
    }
    if (active_lanes == 0 || active_lanes > lanes
        || (!InvalidOnly && (state.code_tail_size != k - 1
            || state.pending_count < active_lanes))) {
        return reject_continuation(
            state, TwoStackContinuationStatus::invalid_range);
    }

    constexpr std::size_t max_lanes = ContinuationState::max_vector_lanes;
    constexpr std::size_t window_capacity =
        W != 0 && W <= two_stack_max_window ? W : two_stack_max_window;
    constexpr std::size_t combined_capacity =
        window_capacity - 1 + max_lanes;
    constexpr std::size_t event_capacity =
        max_lanes * (window_capacity + max_lanes);
    constexpr std::uint64_t ordinal_sentinel =
        std::numeric_limits<std::uint64_t>::max();

    const std::uint64_t max_hash = MinimizerT::getMax().kmer();
    const std::uint64_t max_raw = MinimizerT::getMax().raw_value();
    const std::uint64_t raw_hash_mask = MinimizerT::hash_mask;
    const std::uint64_t kmer_mask =
        (std::uint64_t{1} << (2 * k)) - 1;

    if (state.candidate_count
            > std::numeric_limits<std::uint64_t>::max()
                - static_cast<std::uint64_t>(active_lanes)) {
        return reject_continuation(
            state, TwoStackContinuationStatus::arithmetic_overflow);
    }
    if (!state.candidate_state_initialized) {
        std::fill(state.candidate_tail_hash.begin(),
            state.candidate_tail_hash.end(), max_hash);
        std::fill(state.candidate_tail_raw.begin(),
            state.candidate_tail_raw.end(), max_raw);
        std::fill(state.candidate_tail_ordinal.begin(),
            state.candidate_tail_ordinal.end(), ordinal_sentinel);
        state.current_min = {
            .hash = max_hash,
            .raw = max_raw,
            .ordinal = ordinal_sentinel,
        };
        state.candidate_state_initialized = true;
        add_proof_counter(state.proof.scalar_orchestration_steps, 1);
    }

    const mask_t active_mask = hn::FirstN(d, active_lanes);
    const vec_t v_max_hash = hn::Set(d, max_hash);
    const vec_t ordinal_unmasked = hn::Add(
        hn::Set(d, static_cast<std::uint64_t>(state.candidate_count)),
        lane_indices(d));
    const vec_t candidate_ordinal = hn::IfThenElse(
        active_mask, ordinal_unmasked, hn::Set(d, ordinal_sentinel));
    vec_t candidate_hash = v_max_hash;
    vec_t candidate_raw = hn::Set(d, max_raw);
    mask_t palindrome_mask = hn::MaskFalse(d);
    HWY_ALIGN std::array<std::uint8_t,
        two_stack_exact_max_kmer - 1 + max_lanes> code_scratch{};
    if constexpr (!InvalidOnly) {
        std::memcpy(
            code_scratch.data(), state.code_tail.data(), k - 1);
        std::memcpy(
            code_scratch.data() + k - 1,
            state.pending_codes.data(), active_lanes);
        add_proof_counter(state.proof.scalar_orchestration_steps, 2);

        canonical_kmer_vectors<d_t> kmer_vectors{
            .forward = hn::Zero(d),
            .reverse = hn::Zero(d),
        };
        for (std::size_t code_index = 0; code_index < k; ++code_index) {
            kmer_vectors = incorporate_kmer_codes(
                d,
                kmer_vectors,
                load_promoted_dna4_codes(
                    d, code_scratch.data() + code_index),
                code_index);
        }
        add_proof_counter(state.proof.scalar_orchestration_steps, k);

        const vec_t v_kmer_mask = hn::Set(d, kmer_mask);
        kmer_vectors.forward = hn::And(kmer_vectors.forward, v_kmer_mask);
        kmer_vectors.reverse = hn::And(kmer_vectors.reverse, v_kmer_mask);
        palindrome_mask = hn::Eq(
            kmer_vectors.forward, kmer_vectors.reverse);
        const mask_t live_non_palindrome =
            hn::AndNot(palindrome_mask, active_mask);
        const vec_t scrambled =
            minimap_int_hasher<seq::dna4>::template scramble_simd<d_t>(
                d,
                hn::Min(kmer_vectors.forward, kmer_vectors.reverse),
                v_kmer_mask);
        candidate_hash = hn::And(
            scrambled,
            hn::Set(d, static_cast<std::uint64_t>(
                MinimizerT::hash_field_mask)));
        candidate_hash = hn::IfThenElse(
            live_non_palindrome, candidate_hash, v_max_hash);

        vec_t output_position = hn::Zero(d);
        if constexpr (std::same_as<
                typename ContinuationState::alphabet_type, seq::dna4>) {
            if (state.emitted_position_base
                > (std::numeric_limits<std::uint64_t>::max)() - (k - 1)) {
                return reject_continuation(
                    state, TwoStackContinuationStatus::arithmetic_overflow);
            }
            const std::uint64_t output_position_base =
                state.emitted_position_base + (k - 1);
            const std::uint64_t maximum_increment =
                (std::numeric_limits<std::uint64_t>::max)()
                - output_position_base;
            if (static_cast<std::uint64_t>(active_lanes - 1)
                    > maximum_increment
                || state.candidate_count > maximum_increment
                    - static_cast<std::uint64_t>(active_lanes - 1)) {
                return reject_continuation(
                    state, TwoStackContinuationStatus::arithmetic_overflow);
            }
            output_position = hn::Add(
                    ordinal_unmasked,
                    hn::Set(d, output_position_base));
        } else {
            output_position = hn::LoadU(
                d, state.pending_positions.data());
        }
        const vec_t strand = hn::IfThenElse(
            hn::Lt(kmer_vectors.reverse, kmer_vectors.forward),
            hn::Set(d, std::uint64_t{1}),
            hn::Zero(d));
        candidate_raw = hn::Or(
            hn::ShiftLeftSame(
                candidate_hash, static_cast<int>(MinimizerT::hash_shift)),
            hn::Or(
                hn::ShiftLeftSame(
                    hn::And(
                        output_position,
                        hn::Set(d, static_cast<std::uint64_t>(
                            MinimizerT::pos_field_mask))),
                    static_cast<int>(MinimizerT::pos_shift)),
                strand));
        candidate_raw = hn::IfThenElse(
            live_non_palindrome, candidate_raw, hn::Set(d, max_raw));
    }

    // Every element subsequently read is covered by the tail copies or the
    // full-width vector stores.  Value-initializing these arrays generated
    // three hot-loop bzero calls (3 * 2056 bytes on the runtime W=256 route).
    HWY_ALIGN std::array<std::uint64_t, combined_capacity> combined_hash;
    HWY_ALIGN std::array<std::uint64_t, combined_capacity> combined_raw;
    HWY_ALIGN std::array<std::uint64_t, combined_capacity> combined_ordinal;
    const std::size_t tail_size = w - 1;
    std::memcpy(
        combined_hash.data(), state.candidate_tail_hash.data(),
        tail_size * sizeof(std::uint64_t));
    std::memcpy(
        combined_raw.data(), state.candidate_tail_raw.data(),
        tail_size * sizeof(std::uint64_t));
    std::memcpy(
        combined_ordinal.data(), state.candidate_tail_ordinal.data(),
        tail_size * sizeof(std::uint64_t));
    hn::StoreU(candidate_hash, d, combined_hash.data() + tail_size);
    hn::StoreU(candidate_raw, d, combined_raw.data() + tail_size);
    hn::StoreU(candidate_ordinal, d, combined_ordinal.data() + tail_size);
    add_proof_counter(state.proof.scalar_orchestration_steps, 3);

    minimizer_candidate_vectors<d_t> window_minimum{
        .hash = v_max_hash,
        .raw = hn::Set(d, max_raw),
        .ordinal = hn::Set(d, ordinal_sentinel),
    };
    for (std::size_t slot = 0; slot < w; ++slot) {
        window_minimum = reduce_window_slot(
            d,
            window_minimum,
            combined_hash.data(),
            combined_raw.data(),
            combined_ordinal.data(),
            slot);
    }
    add_proof_counter(state.proof.scalar_orchestration_steps, w);

    // Store/offset-load is a width-independent whole-vector carry. Unlike
    // CombineShiftRightLanes it crosses AVX2/AVX-512 128-bit block boundaries.
    HWY_ALIGN std::array<std::uint64_t, max_lanes + 1>
        previous_hash_storage{};
    HWY_ALIGN std::array<std::uint64_t, max_lanes + 1>
        previous_raw_storage{};
    HWY_ALIGN std::array<std::uint64_t, max_lanes + 1>
        previous_ordinal_storage{};
    previous_hash_storage[0] = state.current_min.hash;
    previous_raw_storage[0] = state.current_min.raw;
    previous_ordinal_storage[0] = state.current_min.ordinal;
    hn::StoreU(window_minimum.hash, d, previous_hash_storage.data() + 1);
    hn::StoreU(window_minimum.raw, d, previous_raw_storage.data() + 1);
    hn::StoreU(window_minimum.ordinal, d, previous_ordinal_storage.data() + 1);
    const vec_t previous_hash = hn::LoadU(d, previous_hash_storage.data());
    const vec_t previous_raw = hn::LoadU(d, previous_raw_storage.data());
    const vec_t previous_ordinal =
        hn::LoadU(d, previous_ordinal_storage.data());
    add_proof_counter(state.proof.scalar_orchestration_steps, 3);

    const mask_t previous_valid = hn::Ne(previous_hash, v_max_hash);
    mask_t first_full_window = hn::MaskFalse(d);
    mask_t steady_window = hn::MaskFalse(d);
    if constexpr (std::same_as<
            typename ContinuationState::alphabet_type, seq::dna4>) {
        first_full_window = hn::And(
            active_mask,
            hn::Eq(
                ordinal_unmasked,
                hn::Set(d, static_cast<std::uint64_t>(w - 1))));
        steady_window = hn::And(
            active_mask,
            hn::Lt(
                hn::Set(d, static_cast<std::uint64_t>(w - 1)),
                ordinal_unmasked));
    } else {
        const vec_t valid_run_lengths = hn::LoadU(
            d, state.pending_valid_run_lengths.data());
        first_full_window = hn::And(
            active_mask,
            hn::Eq(
                valid_run_lengths,
                hn::Set(d, static_cast<std::uint64_t>(w + k - 1))));
        steady_window = hn::And(
            active_mask,
            hn::Le(
                hn::Set(d, static_cast<std::uint64_t>(w + k)),
                valid_run_lengths));
    }
    const mask_t entering_wins = hn::Le(candidate_hash, previous_hash);
    const mask_t expired = hn::And(
        steady_window,
        hn::AndNot(
            entering_wins,
            hn::Eq(
                previous_ordinal,
                hn::Sub(
                    ordinal_unmasked,
                    hn::Set(d, static_cast<std::uint64_t>(w))))));
    const mask_t emit_old = hn::And(
        steady_window,
        hn::And(previous_valid, hn::Or(entering_wins, expired)));
    const mask_t special_ties = hn::And(first_full_window, previous_valid);
    const mask_t expiry_ties = hn::And(
        expired, hn::Ne(window_minimum.hash, v_max_hash));

    const std::size_t event_stride = w + 1;
    const std::size_t row_stride =
        ((event_stride + lanes - 1) / lanes) * lanes;
    // The slow tie path overwrites every row/column slot it reads.  Avoid a
    // fourth per-block zero-fill of the maximum-capacity matrix.
    HWY_ALIGN std::array<std::uint64_t, event_capacity> event_raw;
    HWY_ALIGN std::array<vec_t, max_lanes> event_columns{};
    std::uint64_t first_window_tie_events = 0;
    std::uint64_t expiry_tie_events = 0;
    const vec_t v_max_raw = hn::Set(d, max_raw);

    HWY_ALIGN std::array<std::uint64_t, event_capacity + max_lanes>
        compressed_raw;
    std::size_t compressed_count = 0;
    const vec_t v_raw_hash_mask = hn::Set(d, raw_hash_mask);

    // Equal packed minimizer hashes are rare (the XXL production layout has a
    // 30-bit hash field), so prove their absence before constructing the
    // padded row-major tie-event matrix.  The
    // direct CompressStore is already in chronological lane order and emits
    // exactly the event-0 records the general path would retain.
    const mask_t possible_tie_lanes = hn::Or(special_ties, expiry_ties);
    mask_t actual_tie_lanes = hn::MaskFalse(d);
    if (hn::CountTrue(d, possible_tie_lanes) != 0) [[unlikely]] {
        for (std::size_t candidate_slot = 0;
             candidate_slot < w;
             ++candidate_slot) {
            const vec_t slot_hash =
                hn::LoadU(d, combined_hash.data() + candidate_slot);
            const vec_t slot_raw =
                hn::LoadU(d, combined_raw.data() + candidate_slot);
            const mask_t special_match = hn::And(
                special_ties,
                hn::And(
                    hn::Eq(slot_hash, previous_hash),
                    hn::Ne(slot_raw, previous_raw)));
            const mask_t expiry_match = hn::And(
                expiry_ties,
                hn::And(
                    hn::Eq(slot_hash, window_minimum.hash),
                    hn::Ne(slot_raw, window_minimum.raw)));
            actual_tie_lanes = hn::Or(
                actual_tie_lanes, hn::Or(special_match, expiry_match));
        }
    }

    if (hn::CountTrue(d, actual_tie_lanes) == 0) [[likely]] {
        compressed_count = hn::CompressStore(
            previous_raw,
            hn::And(
                emit_old,
                hn::Ne(
                    hn::And(previous_raw, v_raw_hash_mask),
                    v_raw_hash_mask)),
            d,
            compressed_raw.data());
        add_proof_counter(state.proof.stable_compress_batches, 1);
        add_proof_counter(state.proof.direct_event_compress_batches, 1);
        add_proof_counter(state.proof.scalar_orchestration_steps, 1);
    } else {
        add_proof_counter(state.proof.tie_matrix_event_batches, 1);
        // Build B columns, transpose the BxB tile in registers, then
        // vector-store each row chunk. This is the rare exact-tie route.
        for (std::size_t column_block = 0;
             column_block < row_stride;
             column_block += lanes) {
            for (std::size_t column = 0; column < lanes; ++column) {
                const std::size_t event_slot = column_block + column;
                if (event_slot == 0) {
                    event_columns[column] = hn::IfThenElse(
                        emit_old, previous_raw, v_max_raw);
                } else if (event_slot <= w) {
                    const std::size_t candidate_slot = event_slot - 1;
                    const vec_t slot_hash =
                        hn::LoadU(d, combined_hash.data() + candidate_slot);
                    const vec_t slot_raw =
                        hn::LoadU(d, combined_raw.data() + candidate_slot);
                    const mask_t special_match = hn::And(
                        special_ties,
                        hn::And(
                            hn::Eq(slot_hash, previous_hash),
                            hn::Ne(slot_raw, previous_raw)));
                    const mask_t expiry_match = hn::And(
                        expiry_ties,
                        hn::And(
                            hn::Eq(slot_hash, window_minimum.hash),
                            hn::Ne(slot_raw, window_minimum.raw)));
                    first_window_tie_events +=
                        hn::CountTrue(d, special_match);
                    expiry_tie_events += hn::CountTrue(d, expiry_match);
                    event_columns[column] = hn::IfThenElse(
                        hn::Or(special_match, expiry_match),
                        slot_raw,
                        v_max_raw);
                } else {
                    event_columns[column] = v_max_raw;
                }
            }
            if (!transpose_u64_lane_matrix(d, event_columns.data())) {
                return reject_continuation(
                    state,
                    TwoStackContinuationStatus::unsupported_vector_shape);
            }
            for (std::size_t row = 0; row < lanes; ++row) {
                hn::StoreU(
                    event_columns[row],
                    d,
                    event_raw.data() + row * row_stride + column_block);
            }
            add_proof_counter(state.proof.row_transpose_batches, 1);
            add_proof_counter(state.proof.row_vector_stores, lanes);
            add_proof_counter(
                state.proof.scalar_orchestration_steps, 2 * lanes);
        }

        const std::size_t active_event_slots = active_lanes * row_stride;
        for (std::size_t offset = 0;
             offset < active_event_slots;
             offset += lanes) {
            const vec_t events = hn::LoadU(d, event_raw.data() + offset);
            const mask_t valid_events = hn::Ne(
                hn::And(events, v_raw_hash_mask), v_raw_hash_mask);
            compressed_count += hn::CompressStore(
                events,
                valid_events,
                d,
                compressed_raw.data() + compressed_count);
            add_proof_counter(state.proof.stable_compress_batches, 1);
            add_proof_counter(state.proof.scalar_orchestration_steps, 1);
        }
    }
    if (compressed_count != 0
        && !collector.append_raw_storage(
            compressed_raw.data(), compressed_count)) {
        return reject_continuation(
            state, TwoStackContinuationStatus::collector_overflow);
    }

    state.current_min = {
        .hash = previous_hash_storage[active_lanes],
        .raw = previous_raw_storage[active_lanes],
        .ordinal = previous_ordinal_storage[active_lanes],
    };
    std::memcpy(
        state.candidate_tail_hash.data(),
        combined_hash.data() + active_lanes,
        tail_size * sizeof(std::uint64_t));
    std::memcpy(
        state.candidate_tail_raw.data(),
        combined_raw.data() + active_lanes,
        tail_size * sizeof(std::uint64_t));
    std::memcpy(
        state.candidate_tail_ordinal.data(),
        combined_ordinal.data() + active_lanes,
        tail_size * sizeof(std::uint64_t));
    if constexpr (!InvalidOnly) {
        std::memcpy(
            state.code_tail.data(),
            code_scratch.data() + active_lanes,
            k - 1);
        add_proof_counter(state.proof.scalar_orchestration_steps, 4);
    } else {
        add_proof_counter(state.proof.scalar_orchestration_steps, 3);
    }

    state.candidate_count += active_lanes;
    add_proof_counter(state.proof.minimizers_emitted, compressed_count);
    add_proof_counter(state.proof.vector_batches, 1);
    add_proof_counter(state.proof.vector_lanes_visited, active_lanes);
    add_proof_counter(state.proof.generic_u64_candidate_blocks, 1);
    add_proof_counter(state.proof.generic_u64_lane_visits, active_lanes);
    add_proof_counter(state.proof.masked_tail_lanes, lanes - active_lanes);
    if constexpr (!InvalidOnly) {
        add_proof_counter(state.proof.consecutive_kmer_batches, 1);
        add_proof_counter(state.proof.canonical_scramble_batches, 1);
    }
    add_proof_counter(state.proof.window_reduction_batches, 1);
    add_proof_counter(state.proof.event_generation_batches, 1);
    add_proof_counter(
        state.proof.palindrome_lanes,
        hn::CountTrue(d, hn::And(palindrome_mask, active_mask)));
    add_proof_counter(
        state.proof.first_window_tie_events, first_window_tie_events);
    add_proof_counter(state.proof.expiry_tie_events, expiry_tie_events);
    add_proof_counter(
        state.proof.entering_min_transitions,
        hn::CountTrue(d, hn::And(steady_window, entering_wins)));
    add_proof_counter(
        state.proof.expiry_rescan_transitions,
        hn::CountTrue(d, expired));
    return TwoStackContinuationStatus::ok;
}

/** Advance one or more max/invalid entries through the SIMD minimizer ring. */
template<
    std::size_t W,
    std::size_t K,
    class MinimizerT,
    class ContinuationState,
    class Collector>
[[nodiscard]] inline TwoStackContinuationStatus advance_invalid_slots(
    ContinuationState& state,
    Collector& collector,
    std::span<const std::uint64_t> valid_run_lengths)
{
    if (valid_run_lengths.empty()
        || valid_run_lengths.size() > state.pending_valid_run_lengths.size()) {
        return reject_continuation(
            state, TwoStackContinuationStatus::invalid_range);
    }
    std::memmove(
        state.pending_valid_run_lengths.data(),
        valid_run_lengths.data(),
        valid_run_lengths.size() * sizeof(std::uint64_t));
    return process_chronological_candidate_block<true, W, K, MinimizerT>(
        state, collector, valid_run_lengths.size());
}

/** Append one ambiguity-free nt4 run to the persistent SIMD k-mer stream. */
template<
    std::size_t W,
    std::size_t K,
    class MinimizerT,
    class ContinuationState,
    class Collector>
[[nodiscard]] inline TwoStackContinuationStatus append_valid_nt4_codes(
    ContinuationState& state,
    Collector& collector,
    std::span<const std::uint8_t> codes,
    std::size_t first_input_position)
{
    using d_t = hn::ScalableTag<std::uint64_t>;
    const std::size_t lanes = hn::Lanes(d_t{});
    const std::size_t k = state.kmer_size();
    if (!codes.empty()
        && first_input_position
            > (std::numeric_limits<std::size_t>::max)()
                - (codes.size() - 1)) {
        return reject_continuation(
            state, TwoStackContinuationStatus::arithmetic_overflow);
    }
    std::size_t consumed = 0;
    while (consumed < codes.size()) {
        const bool filling_code_tail = state.code_tail_size < k - 1;
        const std::size_t available_capacity = filling_code_tail
            ? (k - 1 - state.code_tail_size)
            : (lanes - state.pending_count);
        const std::size_t take = std::min(
            available_capacity, codes.size() - consumed);
        std::uint8_t* destination = filling_code_tail
            ? state.code_tail.data() + state.code_tail_size
            : state.pending_codes.data() + state.pending_count;
        std::memcpy(destination, codes.data() + consumed, take);
        if (take > (std::numeric_limits<std::uint64_t>::max)()
                - state.valid_run_length) {
            return reject_continuation(
                state, TwoStackContinuationStatus::arithmetic_overflow);
        }
        for (std::size_t lane = 0; lane < take; ++lane) {
            const std::uint64_t valid_length =
                state.valid_run_length + lane + 1;
            if (filling_code_tail) {
                state.pending_valid_run_lengths[lane] = valid_length;
            } else {
                const std::size_t input_position =
                    first_input_position + consumed + lane;
                if (input_position < state.input_position_base
                    || input_position - state.input_position_base
                        > (std::numeric_limits<std::size_t>::max)()
                            - state.emitted_position_base) {
                    return reject_continuation(
                        state, TwoStackContinuationStatus::arithmetic_overflow);
                }
                const std::size_t pending_lane = state.pending_count + lane;
                state.pending_positions[pending_lane] =
                    state.emitted_position_base
                    + (input_position - state.input_position_base);
                state.pending_valid_run_lengths[pending_lane] = valid_length;
            }
        }
        state.valid_run_length += take;
        if (filling_code_tail) {
            state.code_tail_size += take;
            const auto status = advance_invalid_slots<W, K, MinimizerT>(
                state,
                collector,
                std::span<const std::uint64_t>{
                    state.pending_valid_run_lengths.data(), take});
            if (status != TwoStackContinuationStatus::ok) return status;
        } else {
            state.pending_count += take;
        }
        consumed += take;
        add_proof_counter(state.proof.scalar_orchestration_steps, 1);
        if (state.pending_count == lanes) {
            const auto status =
                process_chronological_candidate_block<false, W, K, MinimizerT>(
                    state, collector, lanes);
            if (status != TwoStackContinuationStatus::ok) return status;
            state.pending_count = 0;
        }
    }
    return TwoStackContinuationStatus::ok;
}

/** Emit the final live window minimum without finalizing the continuation. */
template<class MinimizerT, class ContinuationState, class Collector>
[[nodiscard]] inline TwoStackContinuationStatus flush_current_minimum(
    ContinuationState& state,
    Collector& collector)
{
    using storage_t = typename MinimizerT::storage_t;
    using d_t = hn::ScalableTag<std::uint64_t>;
    d_t d;
    const auto current_raw = hn::Set(
        d, static_cast<std::uint64_t>(state.current_min.raw));
    const auto raw_hash_mask = hn::Set(
        d, static_cast<std::uint64_t>(MinimizerT::hash_mask));
    const auto current_valid = hn::Ne(
        hn::And(current_raw, raw_hash_mask), raw_hash_mask);
    HWY_ALIGN std::array<storage_t, ContinuationState::max_vector_lanes>
        final_raw{};
    const std::size_t final_count = hn::CompressStore(
        current_raw,
        hn::And(current_valid, hn::FirstN(d, 1)),
        d,
        final_raw.data());
    if (final_count != 0
        && !collector.append_raw_storage(final_raw.data(), final_count)) {
        return reject_continuation(
            state, TwoStackContinuationStatus::collector_overflow);
    }
    add_proof_counter(state.proof.minimizers_emitted, final_count);
    add_proof_counter(state.proof.final_flush_records, final_count);
    add_proof_counter(state.proof.event_generation_batches, 1);
    add_proof_counter(state.proof.stable_compress_batches, 1);
    add_proof_counter(state.proof.scalar_orchestration_steps, 1);
    return TwoStackContinuationStatus::ok;
}

/**
 * Apply minimap2's ambiguous-base transition: l/rolling-kmer reset, one max
 * ring entry, but no window flush, ring clear, ordinal reset, or emit.
 */
template<
    std::size_t W,
    std::size_t K,
    class MinimizerT,
    class ContinuationState,
    class Collector>
[[nodiscard]] inline TwoStackContinuationStatus advance_ambiguity(
    ContinuationState& state,
    Collector& collector)
{
    if (state.code_tail_size == state.kmer_size() - 1
        && state.pending_count != 0) {
        const auto status =
            process_chronological_candidate_block<false, W, K, MinimizerT>(
                state, collector, state.pending_count);
        if (status != TwoStackContinuationStatus::ok) return status;
        state.pending_count = 0;
    }
    state.valid_run_length = 0;
    state.code_tail_size = 0;
    state.pending_count = 0;
    state.pending_valid_run_lengths[0] = 0;
    const auto advance_status = advance_invalid_slots<W, K, MinimizerT>(
        state,
        collector,
        std::span<const std::uint64_t>{
            state.pending_valid_run_lengths.data(), 1});
    if (advance_status != TwoStackContinuationStatus::ok) {
        return advance_status;
    }
    add_proof_counter(state.proof.ambiguous_symbols, 1);
    add_proof_counter(state.proof.scalar_orchestration_steps, 1);
    return TwoStackContinuationStatus::ok;
}

}  // namespace two_stack_internal


template<std::size_t W, std::size_t K, class AlphabetTraits>
[[nodiscard]] inline TwoStackContinuationStatus reset_two_stack_nthash(
    TwoStackContinuationState<W, K, AlphabetTraits>& state,
    std::size_t runtime_w,
    std::size_t runtime_k,
    std::size_t emitted_position_base)
{
    state = {};
    using alphabet_type = typename AlphabetTraits::alphabet_type;
    using storage_unit = typename AlphabetTraits::storage_unit;
    if constexpr (!seqan3::nucleotide_alphabet<alphabet_type>) {
        return two_stack_internal::reject_continuation(
            state, TwoStackContinuationStatus::unsupported_alphabet);
    } else if constexpr (!std::is_integral_v<storage_unit>
        || !(sizeof(storage_unit) == 1 || sizeof(storage_unit) == 2
             || sizeof(storage_unit) == 4 || sizeof(storage_unit) == 8)
        || AlphabetTraits::bits_per_char == 0
        || AlphabetTraits::bits_per_char > 8
        || AlphabetTraits::letters_per_unit
            != sizeof(storage_unit) * 8 / AlphabetTraits::bits_per_char) {
        return two_stack_internal::reject_continuation(
            state, TwoStackContinuationStatus::unsupported_storage);
    }
    using d_t = hn::ScalableTag<std::uint64_t>;
    const d_t d;
    const std::size_t lanes = hn::Lanes(d);
    const auto target_status = two_stack_internal::validate_exact_vector_shape(
        state,
        lanes,
        TwoStackContinuationState<W, K, AlphabetTraits>::max_vector_lanes,
        two_stack_internal::exact_compiled_target_is_scalar,
        two_stack_internal::u64_lane_transpose_shape_supported(d));
    if (target_status != TwoStackContinuationStatus::ok) {
        return target_status;
    }
    if constexpr (W > two_stack_max_window
        || K > two_stack_exact_max_kmer) {
        return two_stack_internal::reject_continuation(
            state,
            TwoStackContinuationStatus::invalid_static_configuration);
    }
    if constexpr (W != 0) {
        if (runtime_w != W) {
            return two_stack_internal::reject_continuation(
                state,
                TwoStackContinuationStatus::invalid_static_configuration);
        }
    }
    if constexpr (K != 0) {
        if (runtime_k != K) {
            return two_stack_internal::reject_continuation(
                state,
                TwoStackContinuationStatus::invalid_static_configuration);
        }
    }
    if (runtime_w == 0 || runtime_w > two_stack_max_window) {
        return two_stack_internal::reject_continuation(
            state, TwoStackContinuationStatus::invalid_window_size);
    }
    if (runtime_k == 0 || runtime_k > two_stack_exact_max_kmer) {
        return two_stack_internal::reject_continuation(
            state, TwoStackContinuationStatus::invalid_kmer_size);
    }
    state.runtime_w = runtime_w;
    state.runtime_k = runtime_k;
    state.emitted_position_base = emitted_position_base;
    state.kernel_kind = ExactSimdKernelKind::generic_u64;
    state.proof.generic_u64_kernel_calls = 1;
    state.initialized = true;
    state.status = TwoStackContinuationStatus::ok;
    return state.status;
}


template<
    std::size_t W,
    std::size_t K,
    class MinimizerT,
    class AlphabetTraits,
    class Collector>
[[nodiscard]] inline TwoStackContinuationStatus
consume_two_stack_nthash_packed_fragment(
    TwoStackContinuationState<W, K, AlphabetTraits>& state,
    Collector& collector,
    std::span<const typename AlphabetTraits::storage_unit> packed_fragment,
    std::size_t fragment_bp_size,
    std::size_t fragment_begin_bp,
    std::size_t consume_begin_bp,
    std::size_t consume_end_bp)
{
    using storage_unit = typename AlphabetTraits::storage_unit;

    if (state.status != TwoStackContinuationStatus::ok) return state.status;
    if (!state.initialized) {
        return two_stack_internal::reject_continuation(
            state, TwoStackContinuationStatus::not_initialized);
    }
    if (state.finalized) {
        return two_stack_internal::reject_continuation(
            state, TwoStackContinuationStatus::already_finalized);
    }
    if constexpr (!seqan3::nucleotide_alphabet<
        typename AlphabetTraits::alphabet_type>) {
        return two_stack_internal::reject_continuation(
            state, TwoStackContinuationStatus::unsupported_alphabet);
    } else if constexpr (!std::is_integral_v<storage_unit>
        || !(sizeof(storage_unit) == 1 || sizeof(storage_unit) == 2
             || sizeof(storage_unit) == 4 || sizeof(storage_unit) == 8)
        || AlphabetTraits::bits_per_char == 0
        || AlphabetTraits::bits_per_char > 8
        || AlphabetTraits::letters_per_unit
            != sizeof(storage_unit) * 8 / AlphabetTraits::bits_per_char) {
        return two_stack_internal::reject_continuation(
            state, TwoStackContinuationStatus::unsupported_storage);
    } else {
    if (consume_begin_bp < fragment_begin_bp
        || consume_end_bp < consume_begin_bp
        || consume_end_bp - fragment_begin_bp > fragment_bp_size
        || fragment_bp_size
            > packed_fragment.size() * AlphabetTraits::letters_per_unit) {
        return two_stack_internal::reject_continuation(
            state, TwoStackContinuationStatus::invalid_range);
    }

    if (!state.saw_first_fragment) {
        state.input_position_base = consume_begin_bp;
        state.next_input_position = consume_begin_bp;
        state.saw_first_fragment = true;
    }
    if (consume_begin_bp != state.next_input_position) {
        return two_stack_internal::reject_continuation(
            state, TwoStackContinuationStatus::non_contiguous_range);
    }

    const std::size_t k = state.kmer_size();
    if (state.window_size() == 0
        || state.window_size() > two_stack_max_window) {
        return two_stack_internal::reject_continuation(
            state, TwoStackContinuationStatus::invalid_window_size);
    }
    if (k == 0 || k > two_stack_exact_max_kmer) {
        return two_stack_internal::reject_continuation(
            state, TwoStackContinuationStatus::invalid_kmer_size);
    }
    using d_t = hn::ScalableTag<std::uint64_t>;
    d_t d;
    const std::size_t lanes = hn::Lanes(d);
    constexpr bool fast_type_eligible =
        !two_stack_internal::exact_compiled_target_is_scalar
        && W == 10 && K == 15
        && std::same_as<
            typename AlphabetTraits::alphabet_type, seq::dna4>
        && std::same_as<storage_unit, std::uint32_t>
        && two_stack_internal::
            exact_k15_w10_b8_representation_v<MinimizerT>;
    const std::size_t pending_limit =
        fast_type_eligible
            && state.kernel_kind
                == ExactSimdKernelKind::k15_w10_b8_u32_hash
        ? TwoStackContinuationState<W, K, AlphabetTraits>::
            fast_block_candidates
        : lanes;
    if (state.code_tail_size > k - 1
        || state.pending_count >= pending_limit) {
        return two_stack_internal::reject_continuation(
            state, TwoStackContinuationStatus::invalid_state);
    }

    const std::size_t consume_count = consume_end_bp - consume_begin_bp;
    if (consume_count
            > std::numeric_limits<std::uint64_t>::max()
                - state.proof.bp_consumed
        || consume_count
            > std::numeric_limits<std::uint64_t>::max()
                - state.proof.fragment_symbols_staged) {
        return two_stack_internal::reject_continuation(
            state, TwoStackContinuationStatus::arithmetic_overflow);
    }
    if (consume_count == 0) return TwoStackContinuationStatus::ok;

    const std::size_t local_begin = consume_begin_bp - fragment_begin_bp;
    const std::size_t local_end = consume_end_bp - fragment_begin_bp;
    const hn::Rebind<std::uint8_t, d_t> d_u8;

    std::size_t local = local_begin;
    if constexpr (std::same_as<
            typename AlphabetTraits::alphabet_type, seq::dna4>) {
        if constexpr (fast_type_eligible) {
            if (state.kernel_kind
                == ExactSimdKernelKind::k15_w10_b8_u32_hash) {
                using fast_d_t = hn::ScalableTag<std::uint32_t>;
                const fast_d_t fast_d;
                const hn::Rebind<std::uint8_t, fast_d_t> fast_d_u8;
                const std::size_t fast_lanes = hn::Lanes(fast_d);
                constexpr std::size_t fast_block =
                    TwoStackContinuationState<W, K, AlphabetTraits>::
                        fast_block_candidates;
                while (local < local_end) {
                    const bool filling_code_tail =
                        state.code_tail_size < k - 1;
                    const std::size_t available_capacity = filling_code_tail
                        ? (k - 1 - state.code_tail_size)
                        : (fast_block - state.pending_count);
                    const std::size_t take = std::min(
                        fast_lanes,
                        std::min(available_capacity, local_end - local));
                    const auto decoded =
                        two_stack_internal::decode_packed_dna4_rank_vector<
                            fast_d_t, AlphabetTraits>(
                                fast_d, packed_fragment, local, take);
                    if (filling_code_tail) {
                        hn::StoreN(
                            decoded.ranks,
                            fast_d_u8,
                            state.code_tail.data() + state.code_tail_size,
                            take);
                        state.code_tail_size += take;
                    } else {
                        hn::StoreN(
                            decoded.ranks,
                            fast_d_u8,
                            state.pending_codes.data() + state.pending_count,
                            take);
                        state.pending_count += take;
                    }
                    local += take;
                    two_stack_internal::add_proof_counter(
                        state.proof.packed_decode_batches, 1);
                    two_stack_internal::add_proof_counter(
                        state.proof.packed_decode_lanes, take);
                    two_stack_internal::add_proof_counter(
                        state.proof.packed_word_boundary_merges,
                        decoded.crossed_word ? 1 : 0);
                    two_stack_internal::add_proof_counter(
                        state.proof.scalar_orchestration_steps, 1);
                    if (state.pending_count == fast_block) {
                        const auto block_status =
                            two_stack_internal::
                                process_exact_k15_w10_b8_candidate_block<
                                    MinimizerT>(
                                        state, collector, fast_block);
                        if (block_status != TwoStackContinuationStatus::ok) {
                            return block_status;
                        }
                        state.pending_count = 0;
                    }
                }
                state.next_input_position = consume_end_bp;
                two_stack_internal::add_proof_counter(
                    state.proof.bp_consumed, consume_count);
                two_stack_internal::add_proof_counter(
                    state.proof.fragment_symbols_staged, consume_count);
                return TwoStackContinuationStatus::ok;
            }
        }
        // Keep the established dna4 decode/staging loop byte-for-byte hot:
        // dna4 has no ambiguity sentinel, so no run-boundary orchestration is
        // needed and its report_76-relative performance must not regress.
        while (local < local_end) {
            const bool filling_code_tail = state.code_tail_size < k - 1;
            const std::size_t available_capacity = filling_code_tail
                ? (k - 1 - state.code_tail_size)
                : (lanes - state.pending_count);
            const std::size_t take = std::min(
                lanes, std::min(available_capacity, local_end - local));
            const auto decoded =
                two_stack_internal::decode_packed_dna4_rank_vector<
                    d_t, AlphabetTraits>(
                        d, packed_fragment, local, take);
            if (filling_code_tail) {
                hn::StoreN(
                    decoded.ranks,
                    d_u8,
                    state.code_tail.data() + state.code_tail_size,
                    take);
                state.code_tail_size += take;
            } else {
                hn::StoreN(
                    decoded.ranks,
                    d_u8,
                    state.pending_codes.data() + state.pending_count,
                    take);
                state.pending_count += take;
            }
            local += take;
            two_stack_internal::add_proof_counter(
                state.proof.packed_decode_batches, 1);
            two_stack_internal::add_proof_counter(
                state.proof.packed_decode_lanes, take);
            two_stack_internal::add_proof_counter(
                state.proof.packed_word_boundary_merges,
                decoded.crossed_word ? 1 : 0);
            two_stack_internal::add_proof_counter(
                state.proof.scalar_orchestration_steps, 1);
            if (state.pending_count == lanes) {
                const auto block_status =
                    two_stack_internal::process_chronological_candidate_block<
                    false, W, K, MinimizerT>(state, collector, lanes);
                if (block_status != TwoStackContinuationStatus::ok) {
                    return block_status;
                }
                state.pending_count = 0;
            }
        }
    } else {
        constexpr std::size_t max_lanes =
            TwoStackContinuationState<W, K, AlphabetTraits>::max_vector_lanes;
        HWY_ALIGN std::array<std::uint8_t, max_lanes> decoded_codes{};
        while (local < local_end) {
            const std::size_t take = std::min(lanes, local_end - local);
            const auto decoded =
                two_stack_internal::decode_packed_nt4_rank_vector<
                    d_t, AlphabetTraits>(
                        d, packed_fragment, local, take);
            hn::StoreN(
                decoded.codes, d_u8, decoded_codes.data(), take);
            two_stack_internal::add_proof_counter(
                state.proof.packed_decode_batches, 1);
            two_stack_internal::add_proof_counter(
                state.proof.packed_decode_lanes, take);
            two_stack_internal::add_proof_counter(
                state.proof.packed_word_boundary_merges,
                decoded.crossed_word ? 1 : 0);
            two_stack_internal::add_proof_counter(
                state.proof.scalar_orchestration_steps, 1);

            std::uint64_t ambiguous_lanes = decoded.ambiguous_lanes;
            std::size_t run_begin = 0;
            if (ambiguous_lanes != 0) {
                two_stack_internal::add_proof_counter(
                    state.proof.ambiguous_reset_batches, 1);
            }
            while (ambiguous_lanes != 0) {
                const std::size_t ambiguous_lane = static_cast<std::size_t>(
                    std::countr_zero(ambiguous_lanes));
                const auto append_status =
                    two_stack_internal::append_valid_nt4_codes<
                        W, K, MinimizerT>(
                            state,
                            collector,
                            std::span<const std::uint8_t>{
                                decoded_codes.data() + run_begin,
                                ambiguous_lane - run_begin},
                            fragment_begin_bp + local + run_begin);
                if (append_status != TwoStackContinuationStatus::ok) {
                    return append_status;
                }
                const auto reset_status =
                    two_stack_internal::advance_ambiguity<
                        W, K, MinimizerT>(
                            state, collector);
                if (reset_status != TwoStackContinuationStatus::ok) {
                    return reset_status;
                }
                run_begin = ambiguous_lane + 1;
                ambiguous_lanes &= ambiguous_lanes - 1;
            }
            const auto append_status =
                two_stack_internal::append_valid_nt4_codes<
                    W, K, MinimizerT>(
                        state,
                        collector,
                        std::span<const std::uint8_t>{
                            decoded_codes.data() + run_begin,
                            take - run_begin},
                        fragment_begin_bp + local + run_begin);
            if (append_status != TwoStackContinuationStatus::ok) {
                return append_status;
            }
            local += take;
        }
    }

    state.next_input_position = consume_end_bp;
    two_stack_internal::add_proof_counter(
        state.proof.bp_consumed, consume_count);
    two_stack_internal::add_proof_counter(
        state.proof.fragment_symbols_staged, consume_count);
    return TwoStackContinuationStatus::ok;
    }
}


template<
    std::size_t W,
    std::size_t K,
    class MinimizerT,
    supports_compact_sequence_range R,
    class Collector>
[[nodiscard]] inline TwoStackContinuationStatus
// BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN consume_two_stack_nthash_fragment
consume_two_stack_nthash_fragment(
    TwoStackContinuationState<
        W, K, typename compact_sequence_range<R>::traits>& state,
    Collector& collector,
    compact_sequence_range<R>& fragment,
    std::size_t fragment_begin_bp,
    std::size_t consume_begin_bp,
    std::size_t consume_end_bp)
{
    using fragment_type = compact_sequence_range<R>;
    using fragment_traits = typename fragment_type::traits;
    using storage_unit = typename fragment_traits::storage_unit;
    const std::size_t unit_count = static_cast<std::size_t>(fragment.size());
    const storage_unit* packed = unit_count == 0
        ? nullptr
        : fragment.read_cptr(fragment.cbegin());
    return consume_two_stack_nthash_packed_fragment<W, K, MinimizerT>(
        state,
        collector,
        std::span<const storage_unit>{packed, unit_count},
        static_cast<std::size_t>(fragment.bp_size()),
        fragment_begin_bp,
        consume_begin_bp,
        consume_end_bp);
}
// BIOSEQ_PRIVATE_PACKED_ACCESS_END consume_two_stack_nthash_fragment


template<std::size_t W, std::size_t K, class AlphabetTraits, class Collector>
[[nodiscard]] inline TwoStackFinalizeResult finalize_two_stack_nthash(
    TwoStackContinuationState<W, K, AlphabetTraits>& state,
    Collector& collector)
{
    using MinimizerT = typename Collector::minimizer_type;
    if (state.status != TwoStackContinuationStatus::ok) {
        return {.status = state.status, .proof = state.proof};
    }
    if (!state.initialized) {
        const auto status = two_stack_internal::reject_continuation(
            state, TwoStackContinuationStatus::not_initialized);
        return {.status = status, .proof = state.proof};
    }
    if (state.finalized) {
        const auto status = two_stack_internal::reject_continuation(
            state, TwoStackContinuationStatus::already_finalized);
        return {.status = status, .proof = state.proof};
    }

    if (state.window_size() == 0
        || state.window_size() > two_stack_max_window) {
        const auto status = two_stack_internal::reject_continuation(
            state, TwoStackContinuationStatus::invalid_window_size);
        return {.status = status, .proof = state.proof};
    }
    if (state.kmer_size() == 0
        || state.kmer_size() > two_stack_exact_max_kmer) {
        const auto status = two_stack_internal::reject_continuation(
            state, TwoStackContinuationStatus::invalid_kmer_size);
        return {.status = status, .proof = state.proof};
    }
    using d_t = hn::ScalableTag<std::uint64_t>;
    const std::size_t lanes = hn::Lanes(d_t{});
    constexpr bool fast_type_eligible =
        !two_stack_internal::exact_compiled_target_is_scalar
        && W == 10 && K == 15
        && std::same_as<typename AlphabetTraits::alphabet_type, seq::dna4>
        && std::same_as<
            typename AlphabetTraits::storage_unit, std::uint32_t>
        && two_stack_internal::
            exact_k15_w10_b8_representation_v<MinimizerT>;
    const std::size_t pending_limit =
        fast_type_eligible
            && state.kernel_kind
                == ExactSimdKernelKind::k15_w10_b8_u32_hash
        ? TwoStackContinuationState<W, K, AlphabetTraits>::
            fast_block_candidates
        : lanes;
    if (state.code_tail_size > state.kmer_size() - 1
        || state.pending_count >= pending_limit) {
        const auto status = two_stack_internal::reject_continuation(
            state, TwoStackContinuationStatus::invalid_state);
        return {.status = status, .proof = state.proof};
    }

    if (state.code_tail_size == state.kmer_size() - 1
        && state.pending_count != 0) {
        TwoStackContinuationStatus block_status;
        if constexpr (fast_type_eligible) {
            if (state.kernel_kind
                == ExactSimdKernelKind::k15_w10_b8_u32_hash) {
                block_status = two_stack_internal::
                    process_exact_k15_w10_b8_candidate_block<MinimizerT>(
                        state, collector, state.pending_count);
            } else {
                block_status = two_stack_internal::
                    process_chronological_candidate_block<
                        false, W, K, MinimizerT>(
                            state, collector, state.pending_count);
            }
        } else {
            block_status = two_stack_internal::
                process_chronological_candidate_block<
                    false, W, K, MinimizerT>(
                        state, collector, state.pending_count);
        }
        if (block_status != TwoStackContinuationStatus::ok) {
            return {.status = block_status, .proof = state.proof};
        }
        state.pending_count = 0;
    }

    // End-of-input follows mm_sketch's unconditional final-current flush, even
    // when fewer than W candidates were seen. Ambiguity boundaries deliberately
    // do not call this helper: they advance a max slot and retain the ring.
    const auto flush_status =
        two_stack_internal::flush_current_minimum<MinimizerT>(
            state, collector);
    if (flush_status != TwoStackContinuationStatus::ok) {
        return {.status = flush_status, .proof = state.proof};
    }

    state.finalized = true;
    return {
        .status = TwoStackContinuationStatus::ok,
        .proof = state.proof,
    };
}


/**
 * One checked ExactSimd driver for resident, scanner-fragmented, and bounded
 * unit ranges. Runtime (`W=K=0`) and constexpr routes share the same
 * reset/packed-consume/finalize implementation. Positions are relative to
 * `emitted_position_base`, matching scalar range collection.
 */
template<
    std::size_t W,
    std::size_t K,
    class MinimizerT,
    class Sequence,
    class Collector>
[[nodiscard]] inline TwoStackFinalizeResult
// BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN collect_minimizers_two_stack_nthash_exact_range_resident_impl_checked
collect_minimizers_two_stack_nthash_exact_range_resident_impl_checked(
    Collector& collector,
    Sequence& sequence,
    std::size_t lo_unit,
    std::size_t hi_unit,
    std::size_t runtime_w,
    std::size_t runtime_k,
    std::size_t emitted_position_base = 0)
{
    using sequence_type = std::remove_cvref_t<Sequence>;
    using traits = typename sequence_type::traits;
    using storage_unit = typename traits::storage_unit;

    auto rejected = [](TwoStackContinuationStatus status) {
        TwoStackExecutionProof proof{};
        proof.fail_closed_rejections = 1;
        return TwoStackFinalizeResult{.status = status, .proof = proof};
    };

    if constexpr (!exact_simd_minimizer_representation<MinimizerT>) {
        return rejected(
            TwoStackContinuationStatus::unsupported_representation);
    } else if constexpr (!seqan3::nucleotide_alphabet<
            typename traits::alphabet_type>) {
        return rejected(TwoStackContinuationStatus::unsupported_alphabet);
    } else if constexpr (!std::is_integral_v<storage_unit>
        || !(sizeof(storage_unit) == 1 || sizeof(storage_unit) == 2
             || sizeof(storage_unit) == 4 || sizeof(storage_unit) == 8)
        || traits::bits_per_char == 0
        || traits::bits_per_char > 8
        || traits::letters_per_unit
            != sizeof(storage_unit) * 8 / traits::bits_per_char) {
        return rejected(TwoStackContinuationStatus::unsupported_storage);
    } else {
        const std::size_t total_units = static_cast<std::size_t>(sequence.size());
        const std::size_t total_bp = static_cast<std::size_t>(sequence.bp_size());
        constexpr std::size_t lpu = traits::letters_per_unit;
        if (lo_unit > hi_unit || hi_unit > total_units
            || lo_unit > (std::numeric_limits<std::size_t>::max)() / lpu
            || hi_unit > (std::numeric_limits<std::size_t>::max)() / lpu) {
            return rejected(TwoStackContinuationStatus::invalid_range);
        }
        const std::size_t consume_begin_bp = lo_unit * lpu;
        const std::size_t consume_end_bp = std::min(hi_unit * lpu, total_bp);
        if (consume_begin_bp > consume_end_bp) {
            return rejected(TwoStackContinuationStatus::invalid_range);
        }

        // Time the selected ExactSimd kernel itself. This scope deliberately
        // lives inside the admitted execution branch: rejected shapes do not
        // manufacture a visit, and the pipeline does not wrap this call in a
        // detached/label-only `mm_sketch_simd` scope.
        static const auto* exact_simd_region_desc =
            bioseq::instr::register_region_static("mm_sketch_simd");
        auto exact_simd_region_scope =
            bioseq::instr::region(exact_simd_region_desc);

        TwoStackContinuationState<W, K, traits> state;
        const auto reset_status = reset_two_stack_nthash(
            state, runtime_w, runtime_k, emitted_position_base);
        if (reset_status != TwoStackContinuationStatus::ok) {
            return {.status = reset_status, .proof = state.proof};
        }
        if constexpr (W == 10 && K == 15) {
            state.kernel_kind =
                two_stack_internal::select_exact_simd_kernel_kind<
                    MinimizerT, traits>(consume_end_bp - consume_begin_bp);
            if (state.kernel_kind
                == ExactSimdKernelKind::k15_w10_b8_u32_hash) {
                state.proof.generic_u64_kernel_calls = 0;
                state.proof.k15_w10_b8_u32_hash_kernel_calls = 1;
            }
        }

        auto consume_resident = [&]<class ResidentRange>(
                const ResidentRange& resident) {
            const std::size_t unit_count =
                static_cast<std::size_t>(resident.size());
            const storage_unit* packed = unit_count == 0
                ? nullptr
                : resident.read_cptr(resident.cbegin());
            return consume_two_stack_nthash_packed_fragment<
                W, K, MinimizerT>(
                    state,
                    collector,
                    std::span<const storage_unit>{packed, unit_count},
                    static_cast<std::size_t>(resident.bp_size()),
                    0,
                    consume_begin_bp,
                    consume_end_bp);
        };

        const TwoStackContinuationStatus consume_status =
            consume_resident(sequence);
        if (consume_status != TwoStackContinuationStatus::ok) {
            return {.status = consume_status, .proof = state.proof};
        }
        return finalize_two_stack_nthash(state, collector);
    }
}
// BIOSEQ_PRIVATE_PACKED_ACCESS_END collect_minimizers_two_stack_nthash_exact_range_resident_impl_checked

template<
    std::size_t W,
    std::size_t K,
    class MinimizerT,
    class Sequence,
    class Collector>
[[nodiscard]] inline TwoStackExecutionProof
collect_minimizers_two_stack_nthash_exact_range_resident_impl_or_throw(
    Collector& collector,
    Sequence& sequence,
    std::size_t lo_unit,
    std::size_t hi_unit,
    std::size_t runtime_w,
    std::size_t runtime_k,
    std::size_t emitted_position_base = 0,
    std::string_view operation = "minimizer sketch")
{
    // The public pipeline accepts runtime w/k, but the overwhelmingly common
    // minimap-compatible shape must enter a genuinely static implementation.
    // Besides enabling loop folding, this keeps W=10 scratch arrays out of the
    // dynamic W=256 frame.  Other shapes retain the checked generic route.
    TwoStackFinalizeResult result;
    if constexpr (W == 0 && K == 0) {
        if (runtime_w == 10 && runtime_k == 15) [[likely]] {
            result = collect_minimizers_two_stack_nthash_exact_range_resident_impl_checked<
                10, 15, MinimizerT>(
                    collector,
                    sequence,
                    lo_unit,
                    hi_unit,
                    runtime_w,
                    runtime_k,
                    emitted_position_base);
        } else {
            result = collect_minimizers_two_stack_nthash_exact_range_resident_impl_checked<
                0, 0, MinimizerT>(
                    collector,
                    sequence,
                    lo_unit,
                    hi_unit,
                    runtime_w,
                    runtime_k,
                    emitted_position_base);
        }
    } else {
        result = collect_minimizers_two_stack_nthash_exact_range_resident_impl_checked<
            W, K, MinimizerT>(
                collector,
                sequence,
                lo_unit,
                hi_unit,
                runtime_w,
                runtime_k,
                emitted_position_base);
    }
    if (!result.ok()) {
        throw ExactSimdRuntimeError{result.status, operation};
    }
    return result.proof;
}

/**
 * Canonical ExactSimd owner. Coordinates are biological positions in the
 * facade, never packed-unit offsets. A compatible resident is consumed
 * directly. Nonresident input fails closed: the caller must retain an
 * admitted_packed_region through this synchronous kernel invocation.
 */
template<
    std::size_t W,
    std::size_t K,
    class MinimizerT,
    sequence_alphabet Alphabet,
    class Collector>
[[nodiscard]] inline TwoStackFinalizeResult
collect_minimizers_two_stack_nthash_exact_range_checked(
    Collector& collector,
    bioseq::storage::basic_lazy_sequence_view<Alphabet> sequence,
    std::size_t lo_bp,
    std::size_t hi_bp,
    std::size_t runtime_w,
    std::size_t runtime_k,
    std::size_t emitted_position_base = 0)
{
    using packed_instance = sequence_custom_alphabet<
        Alphabet, std::uint32_t>;
    using packed_range = compact_sequence_range<packed_instance>;
    using traits = typename packed_range::traits;
    using storage_unit = typename traits::storage_unit;

    auto rejected = [](TwoStackContinuationStatus status) {
        TwoStackExecutionProof proof{};
        proof.fail_closed_rejections = 1;
        return TwoStackFinalizeResult{.status = status, .proof = proof};
    };

    if constexpr (!exact_simd_minimizer_representation<MinimizerT>) {
        return rejected(
            TwoStackContinuationStatus::unsupported_representation);
    } else if constexpr (!seqan3::nucleotide_alphabet<Alphabet>) {
        return rejected(TwoStackContinuationStatus::unsupported_alphabet);
    } else {
        const bioseq::storage::sequence_size_t total_bp_64 =
            sequence.bp_size();
        if (total_bp_64
                > static_cast<bioseq::storage::sequence_size_t>(
                    (std::numeric_limits<std::size_t>::max)())
            || lo_bp > hi_bp
            || hi_bp > static_cast<std::size_t>(total_bp_64)) {
            return rejected(TwoStackContinuationStatus::invalid_range);
        }

        static const auto* exact_simd_region_desc =
            bioseq::instr::register_region_static("mm_sketch_simd");
        auto exact_simd_region_scope =
            bioseq::instr::region(exact_simd_region_desc);

        TwoStackContinuationState<W, K, traits> state;
        const auto reset_status = reset_two_stack_nthash(
            state, runtime_w, runtime_k, emitted_position_base);
        if (reset_status != TwoStackContinuationStatus::ok) {
            return {.status = reset_status, .proof = state.proof};
        }
        if constexpr (W == 10 && K == 15) {
            state.kernel_kind =
                two_stack_internal::select_exact_simd_kernel_kind<
                    MinimizerT, traits>(hi_bp - lo_bp);
            if (state.kernel_kind
                    == ExactSimdKernelKind::k15_w10_b8_u32_hash) {
                state.proof.generic_u64_kernel_calls = 0;
                state.proof.k15_w10_b8_u32_hash_kernel_calls = 1;
            }
        }

        TwoStackContinuationStatus consume_status =
            TwoStackContinuationStatus::ok;
        if (auto resident = sequence.try_resident_packed(
                static_cast<bioseq::storage::sequence_size_t>(lo_bp),
                static_cast<bioseq::storage::sequence_size_t>(hi_bp),
                static_cast<std::uint8_t>(sizeof(storage_unit)));
            resident.has_value()) {
            const std::size_t leading =
                resident->leading_symbol_offset;
            const std::size_t logical_length = hi_bp - lo_bp;
            consume_status = consume_two_stack_nthash_packed_fragment<
                W, K, MinimizerT>(
                    state,
                    collector,
                    std::span<const storage_unit>{
                        static_cast<const storage_unit*>(resident->units),
                        resident->unit_count},
                    leading + logical_length,
                    0u,
                    leading,
                    leading + logical_length);
        } else {
            return rejected(
                TwoStackContinuationStatus::non_contiguous_range);
        }
        if (consume_status != TwoStackContinuationStatus::ok) {
            return {.status = consume_status, .proof = state.proof};
        }
        return finalize_two_stack_nthash(state, collector);
    }
}

template<
    std::size_t W,
    std::size_t K,
    class MinimizerT,
    sequence_alphabet Alphabet,
    class Collector>
[[nodiscard]] inline TwoStackExecutionProof
collect_minimizers_two_stack_nthash_exact_range_or_throw(
    Collector& collector,
    bioseq::storage::basic_lazy_sequence_view<Alphabet> sequence,
    std::size_t lo_bp,
    std::size_t hi_bp,
    std::size_t runtime_w,
    std::size_t runtime_k,
    std::size_t emitted_position_base = 0,
    std::string_view operation = "minimizer sketch")
{
    TwoStackFinalizeResult result;
    if constexpr (W == 0 && K == 0) {
        if (runtime_w == 10u && runtime_k == 15u) [[likely]] {
            result = collect_minimizers_two_stack_nthash_exact_range_checked<
                10, 15, MinimizerT>(
                    collector, sequence, lo_bp, hi_bp,
                    runtime_w, runtime_k, emitted_position_base);
        } else {
            result = collect_minimizers_two_stack_nthash_exact_range_checked<
                0, 0, MinimizerT>(
                    collector, sequence, lo_bp, hi_bp,
                    runtime_w, runtime_k, emitted_position_base);
        }
    } else {
        result = collect_minimizers_two_stack_nthash_exact_range_checked<
            W, K, MinimizerT>(
                collector, sequence, lo_bp, hi_bp,
                runtime_w, runtime_k, emitted_position_base);
    }
    if (!result.ok()) {
        throw ExactSimdRuntimeError{result.status, operation};
    }
    return result.proof;
}

namespace coarse_two_stack_detail {

[[nodiscard]] constexpr MinimizerKernelStatus transport_status_for(
    const TwoStackContinuationStatus status) noexcept
{
    switch (status) {
        case TwoStackContinuationStatus::ok:
            return MinimizerKernelStatus::ok;
        case TwoStackContinuationStatus::collector_overflow:
            return MinimizerKernelStatus::sink_rejected;
        case TwoStackContinuationStatus::invalid_range:
        case TwoStackContinuationStatus::non_contiguous_range:
            return MinimizerKernelStatus::invalid_sequence_view;
        case TwoStackContinuationStatus::not_initialized:
        case TwoStackContinuationStatus::already_finalized:
        case TwoStackContinuationStatus::invalid_state:
            return MinimizerKernelStatus::invalid_continuation;
        default:
            return MinimizerKernelStatus::invalid_configuration;
    }
}

template<class MinimizerT,
         sequence_alphabet Alphabet, class StorageUnit>
[[nodiscard]] bool run(
    MinimizerKernelInvocation& invocation,
    MinimizerKernelResult& result)
{
    return ::bioseq::minimizers::kernel_detail::
        with_borrowed_compact_range<Alphabet, StorageUnit>(
            invocation.sequence,
            [&invocation, &result]<class Range>(Range& range) {
                minimizer_sink_collector<MinimizerT> collector{
                    invocation.sink};
                TwoStackFinalizeResult native{};
                if (invocation.window == 10u
                    && invocation.kmer == 15u) {
                    native =
                        collect_minimizers_two_stack_nthash_exact_range_resident_impl_checked<
                            10u, 15u, MinimizerT>(
                                collector,
                                range,
                                0u,
                                static_cast<std::size_t>(range.size()),
                                invocation.window,
                                invocation.kmer,
                                invocation.emitted_position_base);
                } else {
                    native =
                        collect_minimizers_two_stack_nthash_exact_range_resident_impl_checked<
                            0u, 0u, MinimizerT>(
                                collector,
                                range,
                                0u,
                                static_cast<std::size_t>(range.size()),
                                invocation.window,
                                invocation.kmer,
                                invocation.emitted_position_base);
                }

                result.proof.two_stack = native.proof;
                result.family_status =
                    static_cast<std::uint32_t>(native.status);
                result.status = transport_status_for(native.status);

                const bool flushed = collector.flush();
                result.emitted = collector.committed_size();
                if (!flushed || !collector.ok()) {
                    result.status = MinimizerKernelStatus::sink_rejected;
                    if (native.status == TwoStackContinuationStatus::ok) {
                        result.family_status = static_cast<std::uint32_t>(
                            TwoStackContinuationStatus::collector_overflow);
                    }
                }
            });
}

// BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN run_fragmented_impl
template<std::size_t W, std::size_t K, class MinimizerT,
         sequence_alphabet Alphabet, class StorageUnit>
void run_fragmented_impl(
    MinimizerKernelInvocation& invocation,
    MinimizerKernelResult& result)
{
    using sequence_config = sequence_custom_alphabet<Alphabet, StorageUnit>;
    using range_type = compact_sequence_range<sequence_config>;
    using traits = typename range_type::traits;
    using state_type = TwoStackContinuationState<W, K, traits>;
    constexpr std::size_t letters_per_unit = traits::letters_per_unit;

    minimizer_sink_collector<MinimizerT> collector{invocation.sink};
    state_type state{};
    TwoStackContinuationStatus status = reset_two_stack_nthash(
        state,
        invocation.window,
        invocation.kmer,
        invocation.emitted_position_base);

    const auto& source = invocation.fragments;
    if (status == TwoStackContinuationStatus::ok
        && (source.read == nullptr
            || source.release == nullptr
            || source.fragment_end < source.first_fragment)) {
        status = TwoStackContinuationStatus::invalid_state;
    }

    for (std::size_t fragment_no = source.first_fragment;
         status == TwoStackContinuationStatus::ok
            && fragment_no < source.fragment_end;
         ++fragment_no) {
        const auto fragment = source.read(source.state, fragment_no);
        const auto release = [&source, fragment_no]() noexcept {
            source.release(source.state, fragment_no);
        };
        const bool pointer_valid = fragment.storage_units == 0u
            || (fragment.storage != nullptr
                && reinterpret_cast<std::uintptr_t>(fragment.storage)
                    % alignof(StorageUnit) == 0u);
        const bool unit_capacity_valid = fragment.storage_units
            <= (std::numeric_limits<std::size_t>::max)()
                / letters_per_unit
            && fragment.fragment_bp_size
                <= fragment.storage_units * letters_per_unit;
        const bool coordinate_valid = fragment.fragment_begin_bp
                <= fragment.consume_begin_bp
            && fragment.consume_begin_bp <= fragment.consume_end_bp
            && fragment.fragment_bp_size
                <= (std::numeric_limits<std::size_t>::max)()
                    - fragment.fragment_begin_bp
            && fragment.consume_end_bp
                <= fragment.fragment_begin_bp + fragment.fragment_bp_size;
        if (!pointer_valid || !unit_capacity_valid || !coordinate_valid) {
            release();
            status = TwoStackContinuationStatus::invalid_range;
            break;
        }

        try {
            status = consume_two_stack_nthash_packed_fragment<
                W, K, MinimizerT>(
                    state,
                    collector,
                    std::span<const StorageUnit>{
                        reinterpret_cast<const StorageUnit*>(fragment.storage),
                        fragment.storage_units},
                    fragment.fragment_bp_size,
                    fragment.fragment_begin_bp,
                    fragment.consume_begin_bp,
                    fragment.consume_end_bp);
        } catch (...) {
            release();
            throw;
        }
        release();
    }

    TwoStackFinalizeResult native{};
    if (status == TwoStackContinuationStatus::ok) {
        native = finalize_two_stack_nthash(state, collector);
    } else {
        native.status = status;
        native.proof = state.proof;
    }
    result.proof.two_stack = native.proof;
    result.family_status = static_cast<std::uint32_t>(native.status);
    result.status = transport_status_for(native.status);

    const bool flushed = collector.flush();
    result.emitted = collector.committed_size();
    if (!flushed || !collector.ok()) {
        result.status = MinimizerKernelStatus::sink_rejected;
        result.family_status = static_cast<std::uint32_t>(
            TwoStackContinuationStatus::collector_overflow);
    }
}
// BIOSEQ_PRIVATE_PACKED_ACCESS_END run_fragmented_impl

template<class MinimizerT,
         sequence_alphabet Alphabet, class StorageUnit>
[[nodiscard]] bool run_fragmented(
    MinimizerKernelInvocation& invocation,
    MinimizerKernelResult& result)
{
    if (invocation.window == 10u && invocation.kmer == 15u) {
        run_fragmented_impl<10u, 15u, MinimizerT, Alphabet, StorageUnit>(
            invocation, result);
    } else {
        run_fragmented_impl<0u, 0u, MinimizerT, Alphabet, StorageUnit>(
            invocation, result);
    }
    return true;
}

template<class MinimizerT>
[[nodiscard]] bool run_resident_report76(
    MinimizerKernelInvocation& invocation,
    MinimizerKernelResult& result)
{
    return ::bioseq::minimizers::kernel_detail::
        with_borrowed_compact_range<seq::dna4, std::uint32_t>(
            invocation.sequence,
            [&invocation, &result]<class Range>(Range& range) {
                minimizer_sink_collector<MinimizerT> collector{
                    invocation.sink};
                try {
                    result.proof.two_stack =
                        collect_minimizers_two_stack_nthash_resident_or_throw<
                            MinimizerT>(
                                collector,
                                range,
                                invocation.window,
                                invocation.kmer);
                    result.family_status = static_cast<std::uint32_t>(
                        TwoStackContinuationStatus::ok);
                    result.status = MinimizerKernelStatus::ok;
                } catch (const ExactSimdRuntimeError& error) {
                    result.family_status = static_cast<std::uint32_t>(
                        error.status());
                    result.status = transport_status_for(error.status());
                }

                const bool flushed = collector.flush();
                result.emitted = collector.committed_size();
                if (!flushed || !collector.ok()) {
                    result.status = MinimizerKernelStatus::sink_rejected;
                    result.family_status = static_cast<std::uint32_t>(
                        TwoStackContinuationStatus::collector_overflow);
                }
            });
}

template<class MinimizerT,
         sequence_alphabet Alphabet, class StorageUnit>
[[nodiscard]] bool run_selected_output_layout(
    MinimizerKernelInvocation& invocation,
    MinimizerKernelResult& result)
{
    static_assert(exact_simd_minimizer_representation<MinimizerT>);

    if (invocation.output_hash_mask
            != static_cast<std::uint64_t>(MinimizerT::hash_field_mask)
        || invocation.output_position_mask
            != static_cast<std::uint64_t>(MinimizerT::pos_field_mask)) {
        result.family_status = static_cast<std::uint32_t>(
            TwoStackContinuationStatus::unsupported_representation);
        result.status = MinimizerKernelStatus::invalid_configuration;
        result.proof.two_stack.fail_closed_rejections = 1u;
        return false;
    }

    switch (invocation.two_stack_route) {
        case TwoStackRoute::chronological_continuation:
            return invocation.fragments.present()
                ? run_fragmented<MinimizerT, Alphabet, StorageUnit>(
                    invocation, result)
                : run<MinimizerT, Alphabet, StorageUnit>(
                    invocation, result);
        case TwoStackRoute::resident_report76:
            if constexpr (!std::same_as<Alphabet, seq::dna4>
                          || !std::same_as<StorageUnit, std::uint32_t>) {
                result.family_status = static_cast<std::uint32_t>(
                    TwoStackContinuationStatus::invalid_static_configuration);
                result.status = MinimizerKernelStatus::invalid_configuration;
                return false;
            }
            if (invocation.window != 10u
                || invocation.kmer != 15u
                || invocation.emitted_position_base != 0u
                || invocation.fragments.present()) {
                result.family_status = static_cast<std::uint32_t>(
                    TwoStackContinuationStatus::invalid_static_configuration);
                result.status = MinimizerKernelStatus::invalid_configuration;
                return false;
            }
            return run_resident_report76<MinimizerT>(invocation, result);
    }
    return false;
}

} // namespace coarse_two_stack_detail

/**
 * One fixed-target, one-shot resident ExactSimd call. The exact target-local
 * continuation is created, consumed and finalized inside the checked driver;
 * no target-dependent state crosses the neutral ABI.
 */
template<sequence_alphabet Alphabet, class StorageUnit>
[[nodiscard]] inline MinimizerKernelResult two_stack_nthash_fixed_case(
    MinimizerKernelInvocation& invocation)
{
    MinimizerKernelResult result{};
    result.proof.algorithm = MinimizerKernelAlgorithm::two_stack_nthash;
    result.proof.target = static_cast<std::int64_t>(HWY_TARGET);

    const auto reject_output_layout = [&result]() {
        result.family_status = static_cast<std::uint32_t>(
            TwoStackContinuationStatus::unsupported_representation);
        result.status = MinimizerKernelStatus::invalid_configuration;
        return result;
    };

    if (!invocation.sequence.final_segment
        || invocation.sequence.begin_symbol != 0u
        || invocation.window == 0u
        || invocation.window > two_stack_max_window
        || invocation.kmer == 0u
        || invocation.kmer > two_stack_exact_max_kmer) {
        result.status = MinimizerKernelStatus::invalid_configuration;
        return result;
    }

    const std::uint64_t output_position_mask =
        minimizer_output_position_mask(invocation.output_layout);
    const std::uint64_t maximum_local_position =
        invocation.sequence.end_symbol == 0u
            ? 0u
            : static_cast<std::uint64_t>(
                invocation.sequence.end_symbol - 1u);
    if (output_position_mask == 0u
        || (invocation.sequence.end_symbol != 0u
            && (invocation.emitted_position_base > output_position_mask
                || maximum_local_position
                    > output_position_mask
                        - invocation.emitted_position_base))) {
        return reject_output_layout();
    }

    bool accepted = false;
    switch (invocation.output_layout) {
        case MinimizerOutputLayout::repr_l15_k15_u64:
            accepted = coarse_two_stack_detail::run_selected_output_layout<
                MinimizerRepr<15u, 15u>, Alphabet, StorageUnit>(
                    invocation, result);
            break;
        case MinimizerOutputLayout::repr_l28_k15_u64:
            accepted = coarse_two_stack_detail::run_selected_output_layout<
                MinimizerRepr<28u, 15u>, Alphabet, StorageUnit>(
                    invocation, result);
            break;
        case MinimizerOutputLayout::repr_l33_k15_u64:
            accepted = coarse_two_stack_detail::run_selected_output_layout<
                MinimizerRepr<33u, 15u>, Alphabet, StorageUnit>(
                    invocation, result);
            break;
#if BIOSEQ_OPTTIMIZATION_LEVEL >= 4
        case MinimizerOutputLayout::repr_l49_k7_u64:
            accepted = coarse_two_stack_detail::run_selected_output_layout<
                MinimizerRepr<49u, 7u>, Alphabet, StorageUnit>(
                    invocation, result);
            break;
#endif
        case MinimizerOutputLayout::unsupported:
        default:
            return reject_output_layout();
    }
    if (!accepted && result.status == MinimizerKernelStatus::ok) {
        result.status = MinimizerKernelStatus::invalid_sequence_view;
    }
    return result;
}

/** Source-topology-owned exact two-stack case. */
template<sequence_alphabet Alphabet, class StorageUnit>
requires (
    (std::same_as<Alphabet, seq::dna4>
        || std::same_as<Alphabet, seq::dna5>)
    && (std::same_as<StorageUnit, std::uint8_t>
        || std::same_as<StorageUnit, std::uint16_t>
        || std::same_as<StorageUnit, std::uint32_t>
        || std::same_as<StorageUnit, std::uint64_t>))
[[nodiscard]] inline MinimizerKernelResult two_stack_nthash_case(
    MinimizerKernelInvocation& invocation)
{
    using config = sequence_custom_alphabet<Alphabet, StorageUnit>;
    using traits = typename compact_sequence_range<config>::traits;
    if (invocation.sequence.bits_per_symbol != traits::bits_per_char
        || invocation.sequence.storage_unit_bytes != sizeof(StorageUnit)) {
        MinimizerKernelResult rejected{};
        rejected.proof.algorithm =
            MinimizerKernelAlgorithm::two_stack_nthash;
        rejected.proof.target = static_cast<std::int64_t>(HWY_TARGET);
        rejected.status = MinimizerKernelStatus::invalid_configuration;
        rejected.family_status = static_cast<std::uint32_t>(
            TwoStackContinuationStatus::invalid_static_configuration);
        return rejected;
    }
    return two_stack_nthash_fixed_case<Alphabet, StorageUnit>(invocation);
}



} // namespace target
} // namespace bioseq::simd
