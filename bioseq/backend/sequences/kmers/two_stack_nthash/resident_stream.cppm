/**
 * @file resident_stream.cppm
 * @brief Resident vector-stream traversal and checked collection adapters.
 */
module;

#include <hwy/highway.h>
#include <libassert/assert.hpp>

export module bioseq.highway.sources.minimizer_two_stack_nthash_resident_stream;

import std;
import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.instrumentation;
import bioseq.storage.lazy_sequence;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.highway.sources.minimizer_two_stack_nthash_state;
import bioseq.highway.sources.sequence_ops;
import bioseq.highway.sources.hasher_base;
import bioseq.highway.sources.hasher_lexic;
import bioseq.highway.sources.hasher_minimap_int;
import bioseq.highway.sources.simd_utils;
import bioseq.highway.sources.vector_stream;

export namespace bioseq::simd {
namespace [[bioseq::simd]] target {
namespace hn = hwy::HWY_NAMESPACE;

namespace two_stack_internal {

/// @brief Checks that the current Highway target exposes an admitted lane count.
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

/// @brief Builds a compile-time lane ordinal table.
template<class p_lane_type, std::size_t p_size>
[[nodiscard]] consteval std::array<p_lane_type, p_size>
make_lane_index_table() noexcept
{
    static_assert(std::is_unsigned_v<p_lane_type>);
    std::array<p_lane_type, p_size> indices{};
    for (std::size_t lane = 0; lane < p_size; ++lane) {
        indices[lane] = static_cast<p_lane_type>(lane);
    }
    return indices;
}

/**
 * Portable unsigned lane ordinals for targets such as NEON where Highway's
 * Iota overload is intentionally unavailable for 64-bit lanes. The table is
 * constant-initialized once per compiled target and enters the kernel through
 * one vector load. All offset, packed-shift, and candidate-position work stays
 * in vector registers.
 */
template<class p_simd_tag>
[[nodiscard]] HWY_INLINE hn::Vec<p_simd_tag>
lane_indices(p_simd_tag d) noexcept
{
    using lane_type = hn::TFromD<p_simd_tag>;
    static_assert(std::is_unsigned_v<lane_type>);
    constexpr std::size_t capacity = hn::MaxLanes(p_simd_tag{});
    static constexpr auto indices =
        make_lane_index_table<lane_type, capacity>();
    return hn::LoadU(d, indices.data());
}

/// @brief Constructs one undefined multi-stream vector value.
template<
    class p_simd_tag,
    typename p_storage_unit,
    std::size_t p_window_size,
    std::size_t... p_index>
[[nodiscard, gnu::always_inline]] HWY_INLINE
vector_stream<p_simd_tag, p_storage_unit, p_window_size>
make_undefined_vector_stream(
    p_simd_tag simd_tag,
    std::index_sequence<p_index...>) noexcept
{
    return pack_vector_stream<p_simd_tag, p_storage_unit, p_window_size>(
        simd_tag, ((void)p_index, hn::Undefined(simd_tag))...);
}

/// @brief Constructs an array of undefined multi-stream vector values.
template<
    std::size_t p_capacity,
    class p_simd_tag,
    typename p_storage_unit,
    std::size_t p_window_size,
    std::size_t... p_index>
[[nodiscard, gnu::always_inline]] HWY_INLINE
std::array<vector_stream<p_simd_tag, p_storage_unit, p_window_size>, p_capacity>
make_undefined_vector_stream_array(
    p_simd_tag simd_tag,
    std::index_sequence<p_index...>) noexcept
{
    return {
        ((void)p_index,
         make_undefined_vector_stream<
             p_simd_tag, p_storage_unit, p_window_size>(
             simd_tag, std::make_index_sequence<p_window_size>{}))...
    };
}

/**
 * Construct every std::array element inside target-attributed project code.
 * The ring's live prefix is overwritten with its semantic max/zero values
 * before the first read, so Undefined avoids an otherwise redundant fill.
 */
template<
    std::size_t p_capacity,
    class p_simd_tag,
    typename p_storage_unit,
    std::size_t p_window_size>
[[nodiscard, gnu::always_inline]] HWY_INLINE
std::array<vector_stream<p_simd_tag, p_storage_unit, p_window_size>, p_capacity>
make_undefined_vector_stream_array(p_simd_tag simd_tag) noexcept
{
    return make_undefined_vector_stream_array<
        p_capacity, p_simd_tag, p_storage_unit, p_window_size>(
        simd_tag, std::make_index_sequence<p_capacity>{});
}

/**
 * Register-promotable state for one two-stack SIMD traversal.
 *
 * The state is stack-owned, `step_one_bp_vector_stream` is its only mutator, and
 * the collector reference remains valid for the enclosing kernel call.
 */
template<
    size_t p_static_window_size,
    size_t p_static_kmer_size,
    size_t p_stream_count,
    bool   p_detect_palindrome,
    class  p_sequence_traits,
    template<typename p_hash_alphabet> typename p_simd_hasher_template,
    class  p_minimizer_type,
    typename p_hash_value_type,
    class p_collector_type
>
struct sequence_simd_iteration_state {
    // Compile-time-derived state-struct knobs — all expressible from the
    // few "intent" parameters above. Pulling them out as `using`/`static
    // constexpr` (instead of forwarding them as separate template
    // parameters) drops the impl's instantiation site from 11 args to 8,
    // and removes the risk of name-clash between `p_simd_tag` and `simd_tag_type`.
    static constexpr bool   k_has_static_window  = (p_static_window_size != 0);
    static constexpr bool   k_has_static_kmer  = (p_static_kmer_size != 0);
    static constexpr size_t k_buffer_capacity = k_has_static_window ? p_static_window_size : k_max_two_stack_window;
    using storage_unit = typename p_sequence_traits::storage_unit;
    using alphabet_type = typename p_sequence_traits::alphabet_type;
    using simd_tag_type = hn::ScalableTag<storage_unit>;
    using vector_type         = hn::Vec<simd_tag_type>;
    using mask_type        = hn::Mask<simd_tag_type>;
    using vector_stream_type =
        vector_stream<simd_tag_type, storage_unit, p_stream_count>;
    using mask_stream_type = mask_stream<simd_tag_type, p_stream_count>;

    // Per-iteration mutable state.
    vector_stream_type state_fwd;
    vector_stream_type state_rc;
    vector_stream_type prefix_min;
    vector_stream_type prefix_kmer;
    vector_stream_type pos_local;
    vector_stream_type last_emit_pos;
    std::array<vector_stream_type, k_buffer_capacity> ring_buf_elem;
    std::array<vector_stream_type, k_buffer_capacity> ring_buf_hash_strand;
    storage_unit pos_wrap_delta;
    size_t step;
    size_t ring_idx;
    size_t next_wrap_step;

    // Loop-invariant SIMD constants.
    vector_type v_kmer_mask;
    vector_type v_alphabet_complement_xor;
    vector_type v_max;
    vector_type v_one;
    vector_type v_pos_mask;
    vector_type v_hash_kmer_mask;
    vector_type v_strand_clear;
    vector_type v_golden;
    vector_type v_wrap;

    // Loop-invariant scalars.
    size_t   w;
    size_t   kmer_size;
    size_t   emit_warmup;
    size_t   wrap_period;
    size_t   lanes_per_stream;
    p_hash_value_type kmer_mask_scalar;
    storage_unit wrap_delta_amount;

    // Output owner.
    p_collector_type& collector;

    /// Per-base-pair body for the bounded ntHash two-stack inner loop,
    /// expressed bundle-uniformly. p_stream_count enters only via the template
    /// parameter on `vector_stream_type` / `mask_stream_type`. There is NO
    /// `if constexpr (p_stream_count …)` branching for per-stream replication
    /// anywhere in this body.
    ///
    /// The named member keeps the fixed parameter list visible to the inliner.
    // BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN two_stack_simd_iteration_resident_kernel
    template<bool p_is_full, typename p_offsets_type>
    [[gnu::always_inline]] [[gnu::flatten]] HWY_INLINE
    void step_one_bp_vector_stream(
        simd_tag_type          d,
        p_offsets_type const& offsets_in,
        mask_type       live_mask,
        vector_stream_type bp_v
    ) {
        // ---- Forward / RC state update ----
        const vector_stream_type complemented =
            bit_xor(bp_v, v_alphabet_complement_xor);
        state_fwd = bit_and(
            bit_or(
                shift_left<p_sequence_traits::bits_per_char>(state_fwd),
                bp_v),
            v_kmer_mask);

        vector_stream_type new_high;
        if constexpr (k_has_static_kmer) {
            constexpr size_t static_rev_shift =
                p_sequence_traits::bits_per_char * (p_static_kmer_size - 1);
            // The dispatcher widens the hash lane when the packed alphabet is
            // bit-castable. dna5 has leftover bits at every candidate width,
            // so unsupported wide shifts must be rejected before this leaf.
            if constexpr (static_rev_shift < sizeof(storage_unit) * 8) {
                new_high = shift_left<static_rev_shift>(complemented);
            } else {
                DEBUG_ASSERT(false,
                    fmt::format("two_stack SIMD: static k-mer reverse-complement "
                                "shift {} >= lane width {}; this alphabet/k-mer "
                                "is unsupported by this exact SIMD leaf",
                                static_rev_shift, sizeof(storage_unit) * 8));
                // Compile-time placeholder; admitted calls never observe it.
                new_high = vector_stream_type::splat(
                    d, static_cast<storage_unit>(0));
            }
        } else {
            const size_t dynamic_rev_shift =
                p_sequence_traits::bits_per_char * (kmer_size - 1);
            new_high = shift_left_same(complemented,
                static_cast<int>(dynamic_rev_shift));
        }
        state_rc = bit_or(
            shift_right<p_sequence_traits::bits_per_char>(state_rc), new_high);

        // ---- Lex + strand classification ----
        const vector_stream_type lex = minimum(state_fwd, state_rc);
        const mask_stream_type strand = less(state_rc, state_fwd);

        // Mixed hash for ordering. The post-multiplication
        // mask is `hash_mask_high` alone.
        const vector_stream_type hash_full = bit_and(
            multiply(lex, v_golden), v_hash_kmer_mask);

        // ---- Strand-encoded lex (LSB := strand bit) for emit-time recovery.
        const vector_stream_type hash_with_strand_lex = if_then_else(
            strand,
            bit_or(lex, v_one),
            bit_and(lex, v_strand_clear));

        // ---- Element pack — palindrome detection on `p_detect_palindrome` path.
        const vector_stream_type elem_valid = bit_or(hash_full, pos_local);
        vector_stream_type packed_element;
        if constexpr (p_detect_palindrome) {
            const mask_stream_type palindrome = equal(state_fwd, state_rc);
            const mask_stream_type valid_mask =
                bit_and_not(palindrome, live_mask);
            packed_element = if_then_else(valid_mask, elem_valid, v_max);
        } else {
            packed_element = if_then_else(live_mask, elem_valid, v_max);
        }

        ring_buf_elem[ring_idx]        = packed_element;
        ring_buf_hash_strand[ring_idx] = hash_with_strand_lex;

        // ---- Prefix-min running update ----
        const mask_stream_type prefix_better = less(packed_element, prefix_min);
        prefix_min = if_then_else(prefix_better, packed_element, prefix_min);
        prefix_kmer = if_then_else(
            prefix_better, hash_with_strand_lex, prefix_kmer);

        ++ring_idx;
        ++step;
        pos_local = add(pos_local, v_one);

        // ---- Suffix-min rebuild every w iterations ----
        if (ring_idx == w) [[unlikely]] {
            // The rebuild reads ring[i] BEFORE writing ring[i-1], so we
            // iterate i from w-1 down to 1 inclusive. With static-w we
            // unroll via index_sequence; with runtime-w we use a normal
            // descending for-loop.
            if constexpr (k_has_static_window) {
                for_each_index<p_static_window_size - 1>(
                    [this](auto idx_const) [[gnu::always_inline]] {
                        constexpr size_t i = p_static_window_size - 1 - decltype(idx_const)::value;
                        const auto current_slot = ring_buf_elem[i];
                        const auto previous_slot = ring_buf_elem[i - 1];
                        const auto current_better = less(current_slot, previous_slot);
                        ring_buf_elem[i - 1] =
                            if_then_else(current_better, current_slot, previous_slot);
                        ring_buf_hash_strand[i - 1] = if_then_else(
                            current_better, ring_buf_hash_strand[i], ring_buf_hash_strand[i - 1]);
                    });
            } else {
                for (size_t i = w - 1; i > 0; --i) {
                    const auto current_slot   = ring_buf_elem[i];
                    const auto previous_slot  = ring_buf_elem[i - 1];
                    const auto current_better = less(current_slot, previous_slot);
                    ring_buf_elem[i - 1] =
                        if_then_else(current_better, current_slot, previous_slot);
                    ring_buf_hash_strand[i - 1] =
                        if_then_else(current_better,
                            ring_buf_hash_strand[i],
                            ring_buf_hash_strand[i - 1]);
                }
            }
            prefix_min  = packed_element;
            prefix_kmer = hash_with_strand_lex;
            ring_idx = 0;
        }

        // ---- Window-min = min(prefix_min, ring_buf[ring_idx]) ----
        const vector_stream_type  suffix_min  = ring_buf_elem[ring_idx];
        const vector_stream_type  suffix_kmer = ring_buf_hash_strand[ring_idx];
        const mask_stream_type prefix_wins = less(prefix_min, suffix_min);
        const vector_stream_type  window_min  =
            if_then_else(prefix_wins, prefix_min, suffix_min);
        const vector_stream_type  window_hs   =
            if_then_else(prefix_wins, prefix_kmer, suffix_kmer);

        // ---- Position wrap (storage-half-bit pos rolls every wrap_period) ----
        if (step == next_wrap_step) [[unlikely]] {
            pos_local = subtract(pos_local, v_wrap);
            prefix_min = subtract(prefix_min, v_wrap);
            const mask_stream_type emit_seen = not_equal(last_emit_pos, v_max);
            last_emit_pos = if_then_else(
                emit_seen, subtract(last_emit_pos, v_wrap), last_emit_pos);
            const auto wrap_one = [this](auto idx_const) [[gnu::always_inline]] {
                constexpr size_t i = decltype(idx_const)::value;
                const mask_stream_type slot_valid =
                    not_equal(ring_buf_elem[i], v_max);
                ring_buf_elem[i] = if_then_else(slot_valid,
                    subtract(ring_buf_elem[i], v_wrap),
                    ring_buf_elem[i]);
            };
            if constexpr (k_has_static_window) {
                for_each_index<p_static_window_size>(wrap_one);
            } else {
                for (size_t i = 0; i < w; ++i) {
                    const mask_stream_type slot_valid =
                        not_equal(ring_buf_elem[i], v_max);
                    ring_buf_elem[i] = if_then_else(slot_valid,
                        subtract(ring_buf_elem[i], v_wrap),
                        ring_buf_elem[i]);
                }
            }
            pos_wrap_delta =
                static_cast<storage_unit>(
                    pos_wrap_delta + wrap_delta_amount);
            next_wrap_step += wrap_period;
        }

        // ---- Emit ----
        if (step >= emit_warmup) {
            const vector_stream_type window_pos = bit_and(window_min, v_pos_mask);
            const mask_stream_type pos_changed =
                not_equal(window_pos, last_emit_pos);
            const mask_stream_type valid_window = not_equal(window_min, v_max);
            const mask_stream_type real_emit = bit_and(pos_changed, valid_window);
            // collect_all_false_per_stream returns true when ALL lanes are false (no
            // emit candidate this stream). We keep the inverted bool below
            // (`per_stream_emits`) explicit so the predicate reads naturally.
            const auto no_emit_per_stream =
                collect_all_false_per_stream(d, real_emit);

            std::array<bool, p_stream_count> per_stream_emits{};
            bool any_stream_emits = false;
            for_each_index<p_stream_count>(
                [&per_stream_emits, &no_emit_per_stream, &any_stream_emits](auto stream_idx_const)
                    [[gnu::always_inline]] {
                        constexpr size_t s = decltype(stream_idx_const)::value;
                        per_stream_emits[s] = !no_emit_per_stream[s];
                        any_stream_emits = any_stream_emits || per_stream_emits[s];
                    });

            if (any_stream_emits) [[likely]] {
                constexpr size_t max_lanes_per_stream = hn::MaxLanes(simd_tag_type{});
                HWY_ALIGN std::array<
                    std::array<storage_unit, max_lanes_per_stream>,
                    p_stream_count> pos_arr;
                HWY_ALIGN std::array<
                    std::array<storage_unit, max_lanes_per_stream>,
                    p_stream_count> hs_arr;
                HWY_ALIGN std::array<
                    std::array<storage_unit, max_lanes_per_stream>,
                    p_stream_count> emit_arr;
                for_each_index<p_stream_count>(
                    [&window_pos, &d, &pos_arr, &window_hs, &hs_arr, &real_emit, &emit_arr](
                        auto stream_idx_const) [[gnu::always_inline]] {
                        constexpr size_t s = decltype(stream_idx_const)::value;
                        hn::Store(window_pos[s], d, pos_arr[s].data());
                        hn::Store(window_hs[s], d, hs_arr[s].data());
                        hn::Store(hn::VecFromMask(d, real_emit[s]), d, emit_arr[s].data());
                    });

                const size_t lc = lanes_per_stream;
                for_each_index<p_stream_count>([this,
                                                &per_stream_emits,
                                                &lc,
                                                &emit_arr,
                                                &pos_arr,
                                                &offsets_in,
                                                &hs_arr](auto stream_idx_const) {
                    constexpr size_t s = decltype(stream_idx_const)::value;
                    if (!per_stream_emits[s])
                        return;
                    for (size_t lane = 0; lane < lc; ++lane) {
                        if (!emit_arr[s][lane])
                            continue;
                        const size_t local_pos =
                            static_cast<size_t>(pos_arr[s][lane]) +
                            static_cast<size_t>(pos_wrap_delta);
                        const size_t abs_pos =
                            offsets_in[s * lc + lane] *
                                p_sequence_traits::letters_per_unit + local_pos;
                        const p_hash_value_type hash_strand_val =
                            static_cast<p_hash_value_type>(hs_arr[s][lane]);
                        const p_hash_value_type lexic_kmer =
                            hash_strand_val & ~static_cast<p_hash_value_type>(1);
                        const p_hash_value_type scrambled_kmer =
                            p_simd_hasher_template<alphabet_type>::
                                template scramble<p_hash_value_type>(
                                lexic_kmer, kmer_mask_scalar);
                        collector.collect(p_minimizer_type(
                            /*kmer=*/static_cast<uint64_t>(scrambled_kmer),
                            /*last_pos=*/abs_pos,
                            /*strand=*/static_cast<uint64_t>(hash_strand_val) & 0x1ULL));
                    }
                });
                last_emit_pos =
                    if_then_else(real_emit, window_pos, last_emit_pos);
            }
        }
    }
    // BIOSEQ_PRIVATE_PACKED_ACCESS_END two_stack_simd_iteration_resident_kernel
};

}  // namespace two_stack_internal


/**
 * Run the unified single- or dual-stream two-stack ntHash collector.
 *
 * Per-stream rolling and ring state stays in `vector_stream`, the chosen
 * canonical hash is scrambled only at emission, and query/target calls share
 * the identical ordering. Palindrome detection is a compile-time route choice.
 */

template<
    size_t p_static_window_size,
    size_t p_static_kmer_size,
    size_t p_stream_count,
    bool   p_detect_palindrome,
    template<typename p_hash_alphabet> typename p_simd_hasher_template,
    class p_minimizer_type,
    supports_compact_sequence_range p_sequence_range,
    typename p_iterator = decltype(
        compact_sequence_range<p_sequence_range>().begin()),
    typename p_hash_value = typename compact_sequence_range<
        p_sequence_range>::traits::storage_unit,
    class p_collector_type = minimizer_sink_collector<p_minimizer_type>
>
requires (
    supports_simd_hash<p_simd_hasher_template, typename compact_sequence_range<p_sequence_range>::traits::alphabet_type>
)
[[gnu::always_inline]] [[gnu::flatten]] inline void
collect_minimizers_two_stack_nthash_simd_impl(
    p_collector_type& collector,
    compact_sequence_range<p_sequence_range>& seq,
    const p_iterator& seq_begin,
    const p_iterator& seq_end,
    const size_t w_runtime,
    const size_t k_runtime
) {
    constexpr bool has_static_w = (p_static_window_size != 0);
    constexpr bool has_static_k = (p_static_kmer_size != 0);
    const size_t w = has_static_w ? p_static_window_size : w_runtime;
    const size_t k = has_static_k ? p_static_kmer_size : k_runtime;

    // Select a zero-copy bit-cast view when the packed widths permit it.
    // If the underlying storage is bit-castable to `p_hash_value` (alphabet
    // matches, both unit widths leave no leftover bits, target unit width
    // <= source — the downcast direction encoded in
    // `is_sequence_custom_alphabet_bit_castable_to`), construct a
    // `bit_casted_view<>` that re-interprets the buffer at the new unit
    // width. Otherwise hold a const reference to `seq` directly. For the
    // default `p_hash_value = p_sequence_range::storage_unit`, the bitcast is identity
    // (target == source); the substrate exists so future callers can opt
    // into a wider lane width by passing an explicit `p_hash_value`.
    using base_seq_traits   = typename compact_sequence_range<p_sequence_range>::traits;
    constexpr bool can_bitcast_to_hash =
        is_sequence_bit_castable_to<p_sequence_range, p_hash_value>;

    using bitcast_view_type = bit_casted_view<
        compact_sequence_range<p_sequence_range>,
        compact_sequence_range<sequence_custom_alphabet<
            typename base_seq_traits::alphabet_type, p_hash_value,
            base_seq_traits::bits_per_char>>>;
    using view_type = std::conditional_t<can_bitcast_to_hash,
        bitcast_view_type,
        compact_sequence_range<p_sequence_range> const&>;

    // `auto&&` deliberately — for the non-bitcast branch the IIFE returns
    // an lvalue reference to `seq`, and `auto&& seq_view` binds to that
    // reference. Using plain `auto seq_view = ...` would *value-copy* the
    // sequence, which for the `sequence_custom_alphabet` specialisation
    // owns a `std::vector` and therefore allocates+copies the entire
    // packed buffer on every call. (Forwarding-ref binding here is sound:
    // both the `seq` lvalue and the `bit_casted_view` prvalue have
    // lifetimes that span the rest of the function.)
    auto&& seq_view = ([&seq]() -> view_type {
        if constexpr (can_bitcast_to_hash) {
            return seq.template bit_cast_to_storage_unit<p_hash_value>();
        } else {
            return seq;
        }
    })();

    // The selected view determines the SIMD lane width used by rolling state.
    using seq_traits   = typename std::remove_cvref_t<view_type>::traits;
    using storage_unit = typename seq_traits::storage_unit;
    using alphabet_type   = typename seq_traits::alphabet_type;

    // A complete k-mer and its reverse-complement shift must fit in one lane.
    DEBUG_ASSERT(k * seq_traits::bits_per_char <= sizeof(storage_unit) * 8,
        fmt::format(
            "two_stack SIMD lane storage unit is too narrow for the k-mer. "
            "k={}, bits_per_char={}, lane_bits={}. The public dispatcher "
            "should have widened p_hash_value (e.g. to uint64_t) so the "
            "k-mer ({} bits) fits in the lane.",
            k, seq_traits::bits_per_char, sizeof(storage_unit) * 8,
            k * seq_traits::bits_per_char));

    using simd_tag_type    = hn::ScalableTag<storage_unit>;
    const simd_tag_type d;
    using vector_type  = hn::Vec<simd_tag_type>;
    using mask_type = hn::Mask<simd_tag_type>;

    // p_stream_count-wide bundle types. With aggressive inlining (the impl is
    // [[gnu::always_inline]] [[gnu::flatten]] and every vector_stream operation
    // is HWY_INLINE), the bundle's std::array<vector_type, p_window_size> storage gets
    // scalar-replaced back to per-slot NEON registers — same machine code
    // an equivalent named-scalar layout.
    using vector_stream_type =
        vector_stream<simd_tag_type, storage_unit, p_stream_count>;
    using mask_stream_type = mask_stream<simd_tag_type, p_stream_count>;

    const size_t lanes_per_stream = hn::Lanes(d);

    const size_t kmer_size = k;
    const storage_unit kmer_mask =
        seq_traits::template kmer_mask<storage_unit>(kmer_size);
    const vector_type v_kmer_mask = hn::Set(d, kmer_mask);

    // Alphabet complement mask. dna5 is rejected before this exact route.
    const storage_unit alphabet_complement_mask =
        static_cast<storage_unit>(seq_traits::alphabet_size - 1);
    const vector_type v_alphabet_complement_xor = hn::Set(d, alphabet_complement_mask);

    // ---- Element-packing constants (hash high half | pos low half) --
    constexpr size_t k_storage_bits   = sizeof(storage_unit) * 8;
    constexpr size_t k_hash_bits      = k_storage_bits / 2;
    constexpr storage_unit pos_mask = (k_hash_bits >= k_storage_bits)
        ? static_cast<storage_unit>(-1)
        : (static_cast<storage_unit>(1) << k_hash_bits) - 1;
    constexpr storage_unit hash_mask_high = static_cast<storage_unit>(~pos_mask);
    constexpr storage_unit max_val        = static_cast<storage_unit>(-1);

    DEBUG_ASSERT(w <= k_max_two_stack_window,
        "k_max_two_stack_window must hold the requested window size");
    DEBUG_ASSERT(w >= 1, "Window size must be at least 1");

    const vector_type v_max       = hn::Set(d, max_val);
    const vector_type v_one       = hn::Set(d, static_cast<storage_unit>(1));
    const vector_type v_pos_mask  = hn::Set(d, pos_mask);
    // `hash_mask_high` zeros the low `k_hash_bits` of the storage
    // word so we can OR in `pos_local`.
    //
    // Folding `kmer_mask & hash_mask_high` into one mask is invalid for any
    // storage_unit where `kmer_mask` lives entirely in the *low* bits and
    // `hash_mask_high` in the *high* bits — concretely, every uint64
    // configuration on the bench grid:
    //   * dna4 p_kmer_size=15 → kmer_mask = (1<<30)-1 = 0x3FFFFFFF (low 30 bits)
    //   * uint64    → hash_mask_high = 0xFFFFFFFF00000000 (top 32 bits)
    //   * intersect → 0   ← every hash collapses to zero
    // After the bug, `elem = hash_full | pos_local` reduced to just
    // `pos_local`, so the sliding-window-min trivially picked the leftmost
    // *position* every step (instead of the leftmost smallest *hash*),
    // and the per-step `pos_changed` check fired on every iteration —
    // producing ~N emits on dna4-longer instead of the ~N/p_window_size the
    // simd-minimizers bounded_nthash guarantees.
    //
    // The right mask is `hash_mask_high` *alone*: the multiplication
    // `lex × golden` produces a ~62-bit-wide product, and the high bits
    // of that product are what we want to keep as the hash. The kmer-mask
    // intersection was protecting against an ill-formed lex (which the
    // hasher itself already guarantees never happens — `lexic_hash`
    // is masked at construction).
    const storage_unit hash_kmer_mask = hash_mask_high;
    const vector_type v_hash_kmer_mask = hn::Set(d, hash_kmer_mask);
    const vector_type v_strand_clear =
        hn::Set(d, static_cast<storage_unit>(~static_cast<storage_unit>(1)));
    constexpr storage_unit golden_ratio_const =
        (sizeof(storage_unit) >= 4)
            ? static_cast<storage_unit>(0x9E3779B9)
            : static_cast<storage_unit>(0x9E37);
    const vector_type v_golden = hn::Set(d, golden_ratio_const);

    constexpr size_t buf_capacity = has_static_w ? p_static_window_size : k_max_two_stack_window;

    // ---- Loop-invariant scalars derived here for the state struct ----
    constexpr storage_unit pos_max = pos_mask;
    const storage_unit wrap_delta_amount =
        static_cast<storage_unit>(pos_max - static_cast<storage_unit>(w) - 1);
    const size_t wrap_period = static_cast<size_t>(wrap_delta_amount);
    const size_t emit_warmup = k + w - 1;
    const vector_type  v_wrap      = hn::Set(d, wrap_delta_amount);

    // scalar copy of kmer_mask passed to `p_simd_hasher_template<…>::scramble` at
    // emit time (the SIMD per-step path doesn't need it because `hash_full`
    // already lives in the high half).
    const p_hash_value kmer_mask_scalar =
        static_cast<p_hash_value>(kmer_mask);

    // Build the per-iteration state in declaration order.
    // Aggregate-init in declaration order so `step_one_bp_vector_stream` sees
    // a fully-populated stack-resident object on its first invocation.
    // Ring-buffer elements are explicitly constructed with Undefined inside
    // target-attributed project code; the meaningful max/zero initialization
    // runs in the loop below before any read (only the first `w` slots are
    // live). With
    // [[clang::trivial_abi]] on `vector_stream` and the impl being
    // [[gnu::always_inline]] [[gnu::flatten]], SROA should break the struct
    // back apart into per-member NEON-register-resident locals so the
    // inliner sees a small finite-parameter member function.
    using state_type = two_stack_internal::sequence_simd_iteration_state<
        p_static_window_size, p_static_kmer_size, p_stream_count, p_detect_palindrome,
        seq_traits,
        p_simd_hasher_template,
        p_minimizer_type,
        p_hash_value,
        p_collector_type>;
    state_type state{
        .state_fwd                  = vector_stream_type::splat(d, static_cast<storage_unit>(0)),
        .state_rc                   = vector_stream_type::splat(d, static_cast<storage_unit>(0)),
        .prefix_min                 = vector_stream_type::splat(d, max_val),
        .prefix_kmer                = vector_stream_type::splat(d, static_cast<storage_unit>(0)),
        .pos_local                  = vector_stream_type::splat(d, static_cast<storage_unit>(0)),
        .last_emit_pos              = vector_stream_type::splat(d, max_val),
        .ring_buf_elem              = two_stack_internal::
            make_undefined_vector_stream_array<
                buf_capacity, simd_tag_type, storage_unit, p_stream_count>(d),
        .ring_buf_hash_strand       = two_stack_internal::
            make_undefined_vector_stream_array<
                buf_capacity, simd_tag_type, storage_unit, p_stream_count>(d),
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
        .kmer_size                  = kmer_size,
        .emit_warmup                = emit_warmup,
        .wrap_period                = wrap_period,
        .lanes_per_stream           = lanes_per_stream,
        .kmer_mask_scalar           = kmer_mask_scalar,
        .wrap_delta_amount          = wrap_delta_amount,
        .collector                  = collector,
    };
    {
        const auto initial_elem = vector_stream_type::splat(d, max_val);
        const auto initial_hs   = vector_stream_type::splat(d, static_cast<storage_unit>(0));
        for (size_t i = 0; i < w; ++i) {
            state.ring_buf_elem[i]        = initial_elem;
            state.ring_buf_hash_strand[i] = initial_hs;
        }
    }

    // ---- Drive iteration ----
    // The handler is a thin forwarding lambda: zero captures other than
    // `state` (by reference). [[gnu::always_inline]] [[gnu::flatten]] flatten
    // the call into the fixed-target fast path, which is [[gnu::always_inline]]
    // [[gnu::flatten]] back into the impl. The lambda body's only operation
    // forwards the bundled logical-rank stream into the state member
    // function; clang's inliner sees a finite parameter list (no opaque
    // capture struct) and remains directly inlineable.
    iterate_sequence_bp_simd<p_stream_count>(
        seq_view,
        seq_begin,
        seq_end,
        k + w - 1,
        [&state]<bool p_is_full>(std::bool_constant<p_is_full>,
                                 auto const& offsets_in,
                                 auto const& d_in,
                                 mask_type live_mask,
                                 vector_stream_type bp_v) [[gnu::always_inline]] [[gnu::flatten]] {
            state.template step_one_bp_vector_stream<p_is_full>(
                d_in, offsets_in, live_mask, bp_v);
        },
        // No-op finish_fn — emits are flushed per-step, so the partial-tail
        // exit path has nothing extra to do.
        [](auto const&, auto const&, auto&) {});
}


/**
 * The resident-vector-stream kernel runs two independent vector streams once the resident
 * sequence is long enough to keep both streams productive.  Keep this one
 * calculation shared by the dispatch and the stats wrapper so the attested
 * stream count cannot drift from the branch that actually ran.
 */
template<
    typename p_hash_unit,
    supports_compact_sequence_range p_sequence_range>
[[nodiscard]] inline bool two_stack_resident_uses_dual_streams(
    const compact_sequence_range<p_sequence_range>& seq,
    std::size_t w,
    std::size_t k) noexcept
{
    using seq_traits = typename compact_sequence_range<p_sequence_range>::traits;
    using storage_simd_tag_type = hn::ScalableTag<p_hash_unit>;
    constexpr std::size_t letters_per_hash_unit =
        sizeof(p_hash_unit) * 8u / seq_traits::bits_per_char;
    const std::size_t lanes_per_stream = hn::Lanes(storage_simd_tag_type{});
    const std::size_t context_bps = k + w - 1u;
    const std::size_t min_seq_for_dual =
        2u * lanes_per_stream * letters_per_hash_unit + context_bps;
    return seq.bp_size() >= min_seq_for_dual;
}


/**
 * Collects two-stack ntHash minimizers through a fixed-target SIMD route.
 *
 * The dispatcher selects compile-time window/k-mer specializations, the
 * narrowest lane that holds the complete k-mer, and one or two streams from
 * the selected lane geometry. Unsupported widths are rejected explicitly.
 */
template<
    template<typename p_hash_alphabet> typename p_simd_hasher_template,
    class p_minimizer_type,
    supports_compact_sequence_range p_sequence_range,
    typename p_iterator = decltype(
        compact_sequence_range<p_sequence_range>().begin()),
    // The default collector writes directly into the neutral batch sink.
    class p_collector_type = minimizer_sink_collector<p_minimizer_type>
>
requires (
    supports_simd_hash<p_simd_hasher_template, typename compact_sequence_range<p_sequence_range>::traits::alphabet_type>
)
inline void collect_minimizers_two_stack_nthash_simd(
    p_collector_type& collector,
    compact_sequence_range<p_sequence_range>& seq,
    const p_iterator& seq_begin,
    const p_iterator& seq_end,
    const size_t w,
    const size_t k
) {
    static const auto* region_description =
        bioseq::instrumentation::register_static_region("mm_sketch_simd");
    auto region_scope = bioseq::instrumentation::region(region_description);

    using seq_traits           = compact_sequence_range<p_sequence_range>::traits;
    using base_storage_unit    = typename seq_traits::storage_unit;
    constexpr size_t base_bits = sizeof(base_storage_unit) * 8;
    constexpr size_t bpc       = seq_traits::bits_per_char;

    // Select the stream count for the admitted lane and shape. Eligibility
    // threshold is derived from the *lane's* `Lanes(d)` so a wider lane
    // (uint64) sees a higher threshold (`Lanes` halves but
    // `letters_per_unit` doubles, so the per-chunk BPs come out the same
    // — but use the lane's own counts to avoid surprise on platforms
    // where `Lanes` differs).
    auto dispatch_with_unit =
        [&seq, &w, &k, &collector, &seq_begin, &seq_end]<typename p_hash_unit_type,
                                                         size_t p_static_window_size,
                                                         size_t p_static_kmer_size>() {
            constexpr size_t lpu_unit =
                sizeof(p_hash_unit_type) * 8 / bpc;
            static_assert(lpu_unit > 0u);
            const bool use_dual_unit =
                two_stack_resident_uses_dual_streams<p_hash_unit_type>(
                    seq, w, k);

            if (use_dual_unit) {
                collect_minimizers_two_stack_nthash_simd_impl<
                    p_static_window_size, p_static_kmer_size,
                    /*p_stream_count=*/2, /*p_detect_palindrome=*/false,
                    p_simd_hasher_template, p_minimizer_type, p_sequence_range, p_iterator, p_hash_unit_type, p_collector_type>(
                        collector, seq, seq_begin, seq_end, w, k);
            } else {
                collect_minimizers_two_stack_nthash_simd_impl<
                    p_static_window_size, p_static_kmer_size,
                    /*p_stream_count=*/1, /*p_detect_palindrome=*/true,
                    p_simd_hasher_template, p_minimizer_type, p_sequence_range, p_iterator, p_hash_unit_type, p_collector_type>(
                        collector, seq, seq_begin, seq_end, w, k);
            }
        };

    // Select the smallest lane that contains the complete forward and reverse
    // k-mer state. Runtime shapes use the supplied k-mer size.
    auto dispatch_with_k_aware_unit =
        [&k, &w, &dispatch_with_unit]<size_t p_static_window_size, size_t p_static_kmer_size>() {
            if constexpr (p_static_kmer_size > 0) {
                constexpr size_t kmer_bits_static = p_static_kmer_size * bpc;
                if constexpr (kmer_bits_static <= base_bits) {
                    dispatch_with_unit.template operator()<
                        base_storage_unit, p_static_window_size, p_static_kmer_size>();
                } else if constexpr (kmer_bits_static <= 64) {
                    dispatch_with_unit.template operator()<
                        uint64_t, p_static_window_size, p_static_kmer_size>();
                } else {
                    static_assert(kmer_bits_static <= 64,
                        "Static k-mer does not fit in any SIMD lane "
                        "we support today (uint64_t = 64 bits is the "
                        "ceiling on AArch64 NEON). Bench grid never "
                        "supports this; either widen the k-mer contract or extend the "
                        "lane vocabulary.");
                }
            } else {
                const size_t kmer_bits = k * bpc;
                if (kmer_bits <= base_bits) {
                    dispatch_with_unit.template operator()<
                        base_storage_unit, p_static_window_size, 0>();
                } else if (kmer_bits <= 64) {
                    dispatch_with_unit.template operator()<
                        uint64_t, p_static_window_size, 0>();
                } else {
                    DEBUG_ASSERT(false, fmt::format(
                        "two_stack SIMD: kmer_bits={} exceeds uint64 "
                        "lane (k={}, bits_per_char={}). Need a wider "
                        "lane vocabulary; the caller must reject this shape.",
                        kmer_bits, k, bpc));
                }
            }
        };

    // Select the compile-time window/k-mer specialization.
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

/** Compile-time window/k-mer bridge for the two-stack SIMD kernel. */

template <
    std::size_t p_window_size, std::size_t p_kmer_size,
    template<typename p_hash_alphabet> typename p_simd_hasher_template,
    class p_minimizer_type,
    supports_compact_sequence_range p_sequence_range,
    typename p_iterator = decltype(
        compact_sequence_range<p_sequence_range>().begin()),
    class p_collector_type  = minimizer_sink_collector<p_minimizer_type>>
requires (
    supports_simd_hash<p_simd_hasher_template, typename compact_sequence_range<p_sequence_range>::traits::alphabet_type>
)
[[gnu::always_inline]] inline void
collect_minimizers_two_stack_nthash_simd_constexpr(
    p_collector_type&                 collector,
    compact_sequence_range<p_sequence_range>& seq,
    const p_iterator& seq_begin,
    const p_iterator& seq_end)
{
    using seq_traits        = typename compact_sequence_range<p_sequence_range>::traits;
    using base_storage_unit = typename seq_traits::storage_unit;
    constexpr std::size_t base_bits = sizeof(base_storage_unit) * 8;
    constexpr std::size_t bpc       = seq_traits::bits_per_char;
    constexpr std::size_t kmer_bits = p_kmer_size * bpc;

    if constexpr (kmer_bits <= base_bits) {
        constexpr std::size_t lpu_unit = sizeof(base_storage_unit) * 8 / bpc;
        static_assert(lpu_unit > 0u);
        const bool use_dual =
            two_stack_resident_uses_dual_streams<base_storage_unit>(
                seq, p_window_size, p_kmer_size);
        if (use_dual) {
            collect_minimizers_two_stack_nthash_simd_impl<
                p_window_size, p_kmer_size, /*p_stream_count=*/2, /*p_detect_palindrome=*/false,
                p_simd_hasher_template, p_minimizer_type, p_sequence_range, p_iterator, base_storage_unit, p_collector_type>(
                    collector, seq, seq_begin, seq_end, p_window_size, p_kmer_size);
        } else {
            collect_minimizers_two_stack_nthash_simd_impl<
                p_window_size, p_kmer_size, /*p_stream_count=*/1, /*p_detect_palindrome=*/true,
                p_simd_hasher_template, p_minimizer_type, p_sequence_range, p_iterator, base_storage_unit, p_collector_type>(
                    collector, seq, seq_begin, seq_end, p_window_size, p_kmer_size);
        }
    } else {
        static_assert(p_kmer_size * bpc <= 64u,
            "Static p_kmer_size-mer does not fit in any supported SIMD lane "
            "(uint64_t is the AArch64-NEON ceiling).");
        constexpr std::size_t lpu_unit = 64u / bpc;
        static_assert(lpu_unit > 0u);
        const bool use_dual =
            two_stack_resident_uses_dual_streams<std::uint64_t>(
                seq, p_window_size, p_kmer_size);
        if (use_dual) {
            collect_minimizers_two_stack_nthash_simd_impl<
                p_window_size, p_kmer_size, /*p_stream_count=*/2, /*p_detect_palindrome=*/false,
                p_simd_hasher_template, p_minimizer_type, p_sequence_range, p_iterator, std::uint64_t, p_collector_type>(
                    collector, seq, seq_begin, seq_end, p_window_size, p_kmer_size);
        } else {
            collect_minimizers_two_stack_nthash_simd_impl<
                p_window_size, p_kmer_size, /*p_stream_count=*/1, /*p_detect_palindrome=*/true,
                p_simd_hasher_template, p_minimizer_type, p_sequence_range, p_iterator, std::uint64_t, p_collector_type>(
                    collector, seq, seq_begin, seq_end, p_window_size, p_kmer_size);
        }
    }
}

namespace two_stack_internal {

template<class p_collector_type>
[[nodiscard]] inline std::size_t resident_collector_size(
        const p_collector_type& collector) noexcept {
    if constexpr (requires { collector.size(); }) {
        return static_cast<std::size_t>(collector.size());
    } else {
        return static_cast<std::size_t>(
            collector.extract_sketches_data().size());
    }
}

template<class p_sequence_range, class p_collector_type>
[[nodiscard]] inline two_stack_execution_stats resident_vector_stream_stats(
    const compact_sequence_range<p_sequence_range>& seq,
    const p_collector_type& collector,
    std::size_t records_before,
    bool used_dual_streams,
    std::size_t w,
    std::size_t k)
{
    const std::size_t records_after = resident_collector_size(collector);
    if (records_after < records_before) {
        throw exact_simd_runtime_error(
            two_stack_continuation_status::invalid_state,
            "resident vector_stream minimizer sketch");
    }
    const bool performed_nonempty_work =
        seq.bp_size() >= k + w - 1u;
    if (performed_nonempty_work && records_after == records_before) {
        throw exact_simd_runtime_error(
            two_stack_continuation_status::invalid_state,
            "resident vector_stream minimizer sketch emitted no records");
    }
    two_stack_execution_stats stats{};
    stats.bp_consumed = static_cast<std::uint64_t>(seq.bp_size());
    stats.minimizers_emitted = static_cast<std::uint64_t>(
        records_after - records_before);
    stats.resident_vector_stream_kernel_calls = 1u;
    stats.resident_vector_stream_single_calls = used_dual_streams ? 0u : 1u;
    stats.resident_vector_stream_dual_calls = used_dual_streams ? 1u : 0u;
    stats.resident_vector_stream_nonempty_work_calls =
        performed_nonempty_work ? 1u : 0u;
    stats.resident_vector_stream_input_bp = stats.bp_consumed;
    stats.resident_vector_stream_minimizers_emitted =
        stats.minimizers_emitted;
    return stats;
}

} // namespace two_stack_internal

/**
 * Stats-returning full-resident entry point for the resident-vector-stream fast kernel.
 * It is limited to the admitted production resident shape. Other shapes use
 * the checked chronological continuation. There is no scalar fallback here.
 */
template<
    class p_minimizer_type,
    supports_compact_sequence_range p_sequence_range,
    class p_collector_type = minimizer_sink_collector<p_minimizer_type>>
requires (
    exact_simd_minimizer_representation<p_minimizer_type>
    && std::same_as<
        typename compact_sequence_range<p_sequence_range>::traits::alphabet_type,
        seq::dna4>
    && std::same_as<
        typename compact_sequence_range<p_sequence_range>::traits::storage_unit,
        std::uint32_t>)
[[nodiscard]] inline two_stack_execution_stats
collect_minimizers_two_stack_nthash_resident_or_throw(
    p_collector_type& collector,
    compact_sequence_range<p_sequence_range>& seq,
    std::size_t w,
    std::size_t k)
{
    if (w != 10u || k != 15u) {
        throw exact_simd_runtime_error(
            two_stack_continuation_status::invalid_static_configuration,
            "resident vector_stream minimizer sketch");
    }
#if HWY_TARGET == HWY_SCALAR
    (void)collector;
    (void)seq;
    throw exact_simd_runtime_error(
        two_stack_continuation_status::unsupported_vector_shape,
        "resident vector_stream minimizer sketch");
#else
    if (!two_stack_internal::resident_vector_shape_supported()) {
        throw exact_simd_runtime_error(
            two_stack_continuation_status::unsupported_vector_shape,
            "resident vector_stream minimizer sketch");
    }
    const std::size_t records_before =
        two_stack_internal::resident_collector_size(collector);
    const bool used_dual_streams =
        two_stack_resident_uses_dual_streams<std::uint32_t>(seq, w, k);
    collect_minimizers_two_stack_nthash_simd<
        minimap_int_hasher, p_minimizer_type, p_sequence_range>(
            collector, seq, seq.begin(), seq.end(), w, k);
    return two_stack_internal::resident_vector_stream_stats(
        seq, collector, records_before, used_dual_streams, w, k);
#endif
}

template<
    class p_minimizer_type,
    sequence_alphabet p_alphabet,
    class p_collector_type = minimizer_sink_collector<p_minimizer_type>>
requires (
    exact_simd_minimizer_representation<p_minimizer_type>
    && std::same_as<p_alphabet, seq::dna4>)
[[nodiscard]] inline two_stack_execution_stats
collect_minimizers_two_stack_nthash_resident_descriptor_or_throw(
    p_collector_type& collector,
    const typename bioseq::storage::lazy_sequence_view<p_alphabet>::
        resident_packed_sequence& descriptor,
    std::size_t w,
    std::size_t k)
{
    if (descriptor.storage_unit_width != sizeof(std::uint32_t)
        || descriptor.bits_per_symbol != 2u
        || descriptor.leading_symbol_offset != 0u) {
        throw exact_simd_runtime_error(
            two_stack_continuation_status::non_contiguous_range,
            "resident vector_stream minimizer descriptor");
    }
    return resident_descriptor_detail::with_borrowed_compact_range<
        p_alphabet, std::uint32_t>(
            descriptor,
            [&collector, w, k]<class p_range>(p_range& range) {
                return collect_minimizers_two_stack_nthash_resident_or_throw<
                    p_minimizer_type>(collector, range, w, k);
            });
}

template<
    std::size_t p_window_size,
    std::size_t p_kmer_size,
    class p_minimizer_type,
    supports_compact_sequence_range p_sequence_range,
    class p_collector_type = minimizer_sink_collector<p_minimizer_type>>
requires (
    p_window_size == 10u && p_kmer_size == 15u
    && exact_simd_minimizer_representation<p_minimizer_type>
    && std::same_as<
        typename compact_sequence_range<p_sequence_range>::traits::alphabet_type,
        seq::dna4>
    && std::same_as<
        typename compact_sequence_range<p_sequence_range>::traits::storage_unit,
        std::uint32_t>)
[[nodiscard]] inline two_stack_execution_stats
collect_minimizers_two_stack_nthash_resident_constexpr_or_throw(
    p_collector_type& collector,
    compact_sequence_range<p_sequence_range>& seq,
    std::size_t w,
    std::size_t k)
{
    if (w != p_window_size || k != p_kmer_size) {
        throw exact_simd_runtime_error(
            two_stack_continuation_status::invalid_static_configuration,
            "constexpr resident vector_stream minimizer sketch");
    }
#if HWY_TARGET == HWY_SCALAR
    (void)collector;
    (void)seq;
    throw exact_simd_runtime_error(
        two_stack_continuation_status::unsupported_vector_shape,
        "constexpr resident vector_stream minimizer sketch");
#else
    if (!two_stack_internal::resident_vector_shape_supported()) {
        throw exact_simd_runtime_error(
            two_stack_continuation_status::unsupported_vector_shape,
            "constexpr resident vector_stream minimizer sketch");
    }
    const std::size_t records_before =
        two_stack_internal::resident_collector_size(collector);
    const bool used_dual_streams =
        two_stack_resident_uses_dual_streams<std::uint32_t>(seq, p_window_size, p_kmer_size);
    collect_minimizers_two_stack_nthash_simd_constexpr<
        p_window_size, p_kmer_size, minimap_int_hasher, p_minimizer_type, p_sequence_range>(
            collector, seq, seq.begin(), seq.end());
    return two_stack_internal::resident_vector_stream_stats(
        seq, collector, records_before, used_dual_streams, p_window_size, p_kmer_size);
#endif
}



}  // namespace target
}  // namespace bioseq::simd
