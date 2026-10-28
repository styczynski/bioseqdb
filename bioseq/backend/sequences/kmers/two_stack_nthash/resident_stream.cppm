/**
 * @file resident_stream.cppm
 * @brief Two-stack minimizer traversal over resident packed sequences.
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

/// @brief Accepts a non-scalar lane shape supported by the shared transpose.
template<class D>
[[nodiscard]] inline bool resident_vector_shape_supported() noexcept {
#if HWY_TARGET == HWY_SCALAR
    return false;
#else
    const D tag;
    const std::size_t lanes = hn::Lanes(tag);
    const std::size_t capacity = hn::MaxLanes(tag);
    return lanes >= 2u && lanes <= capacity
        && (lanes == 2u || lanes == 4u
            || lanes == 8u || lanes == 16u);
#endif
}

/// @brief Builds the constant  lane indices 0 through p_size-1.
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
 * Loads unsigned lane indices from a constant table. This avoids depending
 * on a target-specific Iota overload.
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

/// @brief Creates a vector stream whose lanes are initially undefined.
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

/// @brief Creates an array of vector streams with undefined initial lanes.
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
 * Constructs the vector array inside code compiled for the selected target.
 * The caller, must initialize each live ring slot before reading it. Starting
 * with  Undefined avoids filling slots that will immediately be overwritten.
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
 * Rolling k-mers and window minima for one SIMD traversal. Each lane tracks
 * its own sequence range. A running prefix minimum and stored suffix minima
 * provide the next window minimum witvout rescanning the whole window
 *
 * The enclosing kernel owns this state and keeps the collector alive.
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
    // Derive the buffer capacity and vector types from the template arguments.
    static constexpr bool   k_has_static_window  = (p_static_window_size != 0);
    static constexpr bool   k_has_static_kmer  = (p_static_kmer_size != 0);
    static constexpr size_t k_buffer_capacity = k_has_static_window ? p_static_window_size : k_max_two_stack_window;
    using storage_unit = p_hash_value_type;
    using alphabet_type = typename p_sequence_traits::alphabet_type;
    using hash_alphabet = std::conditional_t<seqan3::nucleotide_alphabet<alphabet_type>, seq::dna4, alphabet_type>;
    using hash_traits = typename hasher_base<hash_alphabet, storage_unit>::traits;
    static constexpr bool has_unseedable_rank = [] {
        if constexpr (seqan3::nucleotide_alphabet<alphabet_type>) {
            for (const auto rank : k_nt4_lut<alphabet_type>)
                if (rank >= seqan3::alphabet_size<hash_alphabet>) return true;
        }
        return false;
    }();
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
    vector_stream_type prefix_strand;
    mask_stream_type prefix_valid;
    vector_stream_type valid_run;
    vector_stream_type pos_local;
    vector_stream_type last_emit_pos;
    mask_stream_type last_emit_valid;
    std::array<vector_stream_type, k_buffer_capacity> ring_buf_elem;
    std::array<vector_stream_type, k_buffer_capacity> ring_buf_hash_strand;
    std::array<vector_stream_type, k_buffer_capacity> ring_buf_strand;
    std::array<mask_stream_type, k_buffer_capacity> ring_buf_valid;
    std::size_t pos_wrap_delta;
    size_t step;
    size_t ring_idx;
    size_t next_wrap_step;

    // Loop-invariant SIMD constants.
    vector_type v_kmer_mask;
    vector_type v_zero;
    vector_type v_one;
    vector_type v_pos_mask;
    vector_type v_hash_kmer_mask;
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

    // Borrowed output collector.
    p_collector_type& collector;
    two_stack_continuation_status failure{two_stack_continuation_status::ok};

    /**
     * Advances every active lane by one base and updates its k-mer and window
     * minimum. Hashes share a word with local positions, so  the earlier position
     * breaks a hash tie. A changed minimum is sent to the collector once a full
     * window is available.
     *
     * vector_stream applies the same operations to each configured stream.
     */
    // BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN two_stack_simd_iteration_resident_kernel
    template<bool p_is_full, typename p_offsets_type>
    [[gnu::always_inline]] [[gnu::flatten]] HWY_INLINE
    void step_one_bp_vector_stream(
        simd_tag_type          d,
        p_offsets_type const& offsets_in,
        mask_type       live_mask,
        vector_stream_type bp_v
    ) {
        if constexpr (seqan3::alphabet_size<alphabet_type>
                      < (std::size_t{1u} << p_sequence_traits::bits_per_char)) {
        if (failure != two_stack_continuation_status::ok) return;
        const auto native_invalid = greater_equal(bp_v,
            hn::Set(d, static_cast<storage_unit>(seqan3::alphabet_size<alphabet_type>)));
        const auto active_invalid = bit_and(native_invalid, live_mask);
        const auto invalid_streams = collect_all_false_per_stream(d, active_invalid);
        for (const bool valid : invalid_streams)
            if (!valid) { failure = two_stack_continuation_status::invalid_state; return; }
        }
        auto ranks = lift_operation([d](auto rank) {
            return exact_hash_rank<alphabet_type>(d, rank);
        }, bp_v);
        if constexpr (has_unseedable_rank) {
            const auto seedable = less(ranks,
                hn::Set(d, static_cast<storage_unit>(seqan3::alphabet_size<hash_alphabet>)));
            valid_run = if_then_else(seedable,
                minimum(add(valid_run, v_one), hn::Set(d, static_cast<storage_unit>(emit_warmup))), v_zero);
            ranks = if_then_else(seedable, ranks, v_zero);
        }
        state_fwd = bit_and(
            bit_or(
                shift_left<hash_traits::bits_per_char>(state_fwd),
                ranks),
            v_kmer_mask);
        if constexpr (hash_traits::canonical_reverse_complement) {
        const vector_stream_type complemented = lift_operation([d](auto rank) {
            return lexic_hasher<hash_alphabet>::template simd_hash<storage_unit, simd_tag_type>::apply_complement(d, rank);
        }, ranks);
        vector_stream_type new_high;
        if constexpr (k_has_static_kmer) {
            constexpr size_t static_rev_shift =
                hash_traits::bits_per_char * (p_static_kmer_size - 1);
            // The selected lane must hold the reverse-complement shift. Alphabets
            // with unused bits between packed units cannot use every bit-cast width.
            if constexpr (static_rev_shift < sizeof(storage_unit) * 8) {
                new_high = shift_left<static_rev_shift>(complemented);
            } else {
                DEBUG_ASSERT(false,
                    fmt::format("two_stack SIMD: static k-mer reverse-complement "
                                "shift {} >= lane width {}; this alphabet/k-mer "
                                "is unsupported by this exact SIMD leaf",
                                static_rev_shift, sizeof(storage_unit) * 8));
                // Placeholder for template instantiation after the unsupported shift assertion.
                new_high = vector_stream_type::splat(
                    d, static_cast<storage_unit>(0));
            }
        } else {
            const size_t dynamic_rev_shift =
                hash_traits::bits_per_char * (kmer_size - 1);
            new_high = shift_left_same(complemented,
                static_cast<int>(dynamic_rev_shift));
        }
        state_rc = bit_or(
            shift_right<hash_traits::bits_per_char>(state_rc), new_high);
        }

        // Choose the canonical k-mer  and its strand.
        auto lex = state_fwd;
        auto strand = mask_stream_type::splat_false(d);
        if constexpr (hash_traits::canonical_reverse_complement) {
            lex = minimum(state_fwd, state_rc);
            strand = less(state_rc, state_fwd);
        }

        // Use the high bits of the multiplied canonical code to order candidates.
        const vector_stream_type hash_full = bit_and(
            multiply(lex, v_golden), v_hash_kmer_mask);

        // Strand metadata never consumes a rank bit of the exact native code.
        const auto strand_value = if_then_else(strand, v_one, v_zero);

        // Combine jash and position, marking inactive or excluded palindromic lanes invalid.
        const vector_stream_type elem_valid = bit_or(hash_full, pos_local);
        auto valid_mask = equal(state_fwd, state_fwd);
        valid_mask = bit_and(valid_mask, live_mask);
        if constexpr (has_unseedable_rank)
            valid_mask = bit_and(greater_equal(valid_run,
                hn::Set(d, static_cast<storage_unit>(kmer_size))), valid_mask);
        if constexpr (p_detect_palindrome && hash_traits::canonical_reverse_complement
                      && (!k_has_static_kmer || p_static_kmer_size % 2u == 0u)) {
            const mask_stream_type palindrome = equal(state_fwd, state_rc);
            valid_mask = bit_and_not(palindrome, valid_mask);
        }
        const auto packed_element = elem_valid;

        ring_buf_elem[ring_idx]        = packed_element;
        ring_buf_hash_strand[ring_idx] = lex;
        ring_buf_strand[ring_idx] = strand_value;
        ring_buf_valid[ring_idx] = valid_mask;

        // Extend the prefix minimum with the new, candidate
        const mask_stream_type prefix_better = bit_and(valid_mask,
            bit_or(bit_and_not(prefix_valid, valid_mask), less(packed_element, prefix_min)));
        prefix_min = if_then_else(prefix_better, packed_element, prefix_min);
        prefix_kmer = if_then_else(
            prefix_better, lex, prefix_kmer);
        prefix_strand = if_then_else(prefix_better, strand_value, prefix_strand);
        prefix_valid = bit_or(prefix_valid, valid_mask);

        ++ring_idx;
        ++step;
        pos_local = add(pos_local, v_one);

        // Rebuild suffix minima after each group of w candidates.
        if (ring_idx == w) [[unlikely]] {
            // Walk backward so each slot receives the minimum  of itself and the
            // suffix already computed to its right. Fixed windows unroll this loop.
            if constexpr (k_has_static_window) {
                for_each_index<p_static_window_size - 1>(
                    [this](auto idx_const) [[gnu::always_inline]] {
                        constexpr size_t i = p_static_window_size - 1 - decltype(idx_const)::value;
                        const auto current_slot = ring_buf_elem[i];
                        const auto previous_slot = ring_buf_elem[i - 1];
                        const auto current_better = bit_and(ring_buf_valid[i],
                            bit_or(bit_and_not(ring_buf_valid[i - 1], ring_buf_valid[i]),
                                less(current_slot, previous_slot)));
                        ring_buf_elem[i - 1] =
                            if_then_else(current_better, current_slot, previous_slot);
                        ring_buf_hash_strand[i - 1] = if_then_else(
                            current_better, ring_buf_hash_strand[i], ring_buf_hash_strand[i - 1]);
                        ring_buf_strand[i - 1] = if_then_else(
                            current_better, ring_buf_strand[i], ring_buf_strand[i - 1]);
                        ring_buf_valid[i - 1] = bit_or(ring_buf_valid[i - 1], ring_buf_valid[i]);
                    });
            } else {
                for (size_t i = w - 1; i > 0; --i) {
                    const auto current_slot   = ring_buf_elem[i];
                    const auto previous_slot  = ring_buf_elem[i - 1];
                    const auto current_better = bit_and(ring_buf_valid[i],
                        bit_or(bit_and_not(ring_buf_valid[i - 1], ring_buf_valid[i]),
                            less(current_slot, previous_slot)));
                    ring_buf_elem[i - 1] =
                        if_then_else(current_better, current_slot, previous_slot);
                    ring_buf_hash_strand[i - 1] =
                        if_then_else(current_better,
                            ring_buf_hash_strand[i],
                            ring_buf_hash_strand[i - 1]);
                    ring_buf_strand[i - 1] = if_then_else(
                        current_better, ring_buf_strand[i], ring_buf_strand[i - 1]);
                    ring_buf_valid[i - 1] = bit_or(ring_buf_valid[i - 1], ring_buf_valid[i]);
                }
            }
            prefix_min  = packed_element;
            prefix_kmer = lex;
            prefix_strand = strand_value;
            prefix_valid = valid_mask;
            ring_idx = 0;
        }

        // The current window joins the new prefix and the saved suffix.
        const vector_stream_type  suffix_min  = ring_buf_elem[ring_idx];
        const vector_stream_type  suffix_kmer = ring_buf_hash_strand[ring_idx];
        const mask_stream_type prefix_wins = bit_and(prefix_valid,
            bit_or(bit_and_not(ring_buf_valid[ring_idx], prefix_valid), less(prefix_min, suffix_min)));
        const vector_stream_type  window_min  =
            if_then_else(prefix_wins, prefix_min, suffix_min);
        const vector_stream_type  window_hs   =
            if_then_else(prefix_wins, prefix_kmer, suffix_kmer);
        const auto window_strand = if_then_else(prefix_wins, prefix_strand, ring_buf_strand[ring_idx]);
        const auto window_valid = bit_or(prefix_valid, ring_buf_valid[ring_idx]);
        auto window_pos = bit_and(window_min, v_pos_mask);

        // Rebase local positions before the packed position field wraps.
        if (step == next_wrap_step) [[unlikely]] {
            pos_local = subtract(pos_local, v_wrap);
            prefix_min = if_then_else(prefix_valid, subtract(prefix_min, v_wrap), prefix_min);
            last_emit_valid = bit_and(last_emit_valid, greater_equal(last_emit_pos, v_wrap));
            last_emit_pos = if_then_else(
                last_emit_valid, subtract(last_emit_pos, v_wrap), last_emit_pos);
            window_pos = subtract(window_pos, v_wrap);
            const auto wrap_one = [this](auto idx_const) [[gnu::always_inline]] {
                constexpr size_t i = decltype(idx_const)::value;
                ring_buf_valid[i] = bit_and(ring_buf_valid[i],
                    greater_equal(bit_and(ring_buf_elem[i], v_pos_mask), v_wrap));
                ring_buf_elem[i] = if_then_else(ring_buf_valid[i],
                    subtract(ring_buf_elem[i], v_wrap),
                    ring_buf_elem[i]);
            };
            if constexpr (k_has_static_window) {
                for_each_index<p_static_window_size>(wrap_one);
            } else {
                for (size_t i = 0; i < w; ++i) {
                    ring_buf_valid[i] = bit_and(ring_buf_valid[i],
                        greater_equal(bit_and(ring_buf_elem[i], v_pos_mask), v_wrap));
                    ring_buf_elem[i] = if_then_else(ring_buf_valid[i],
                        subtract(ring_buf_elem[i], v_wrap),
                        ring_buf_elem[i]);
                }
            }
            pos_wrap_delta += wrap_delta_amount;
            next_wrap_step += wrap_period;
        }

        // Emit when the selected position changes after window warmup.
        if (step >= emit_warmup) {
            const mask_stream_type pos_changed = bit_or(
                bit_and_not(last_emit_valid, window_valid), not_equal(window_pos, last_emit_pos));
            auto real_emit = bit_and(pos_changed, window_valid);
            if constexpr (has_unseedable_rank)
                real_emit = bit_and(real_emit, greater_equal(valid_run,
                    hn::Set(d, static_cast<storage_unit>(emit_warmup))));
            // Skip streams with no selected output lanes.
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
                    p_stream_count> strand_arr;
                HWY_ALIGN std::array<
                    std::array<storage_unit, max_lanes_per_stream>,
                    p_stream_count> emit_arr;
                for_each_index<p_stream_count>(
                    [&window_pos, &d, &pos_arr, &window_hs, &hs_arr, &window_strand, &strand_arr, &real_emit, &emit_arr](
                        auto stream_idx_const) [[gnu::always_inline]] {
                        constexpr size_t s = decltype(stream_idx_const)::value;
                        hn::Store(window_pos[s], d, pos_arr[s].data());
                        hn::Store(window_hs[s], d, hs_arr[s].data());
                        hn::Store(window_strand[s], d, strand_arr[s].data());
                        hn::Store(hn::VecFromMask(d, real_emit[s]), d, emit_arr[s].data());
                    });

                const size_t lc = lanes_per_stream;
                for_each_index<p_stream_count>([this,
                                                &per_stream_emits,
                                                &lc,
                                                &emit_arr,
                                                &pos_arr,
                                                &offsets_in,
                                                &hs_arr, &strand_arr](auto stream_idx_const) {
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
                        const p_hash_value_type lexic_kmer =
                            static_cast<p_hash_value_type>(hs_arr[s][lane]);
                        const p_hash_value_type scrambled_kmer =
                            p_simd_hasher_template<hash_alphabet>::
                                template scramble<p_hash_value_type>(
                                lexic_kmer, kmer_mask_scalar);
                        collector.collect(p_minimizer_type(
                            /*kmer=*/static_cast<uint64_t>(scrambled_kmer),
                            /*last_pos=*/abs_pos,
                            /*strand=*/static_cast<uint64_t>(strand_arr[s][lane])));
                    }
                });
                last_emit_pos =
                    if_then_else(real_emit, window_pos, last_emit_pos);
                last_emit_valid = bit_or(last_emit_valid, real_emit);
            }
        }
    }
    // BIOSEQ_PRIVATE_PACKED_ACCESS_END two_stack_simd_iteration_resident_kernel
};

}  // namespace two_stack_internal


/**
 * Collects minimizers from one or two streams of packed sequence ranges.
 * The two-stack windows compare hashes made from canonical k-mers. At
 * emission, p_simd_hasher_template creates the hash stored in each output
 * record. Palindrome  filtering is selected at compile time.
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

    // Borrow a bit-cast view when the packing permits the requested unit width.
    // Otherwise use seq directly. The selected view supplies the storage type
    // used by the SIMD lanes.
    using base_seq_traits   = typename compact_sequence_range<p_sequence_range>::traits;
    constexpr bool can_bitcast_to_hash =
        sizeof(p_hash_value) < sizeof(typename base_seq_traits::storage_unit)
        && is_sequence_bit_castable_to<p_sequence_range, p_hash_value>;

    using bitcast_view_type = bit_casted_view<
        compact_sequence_range<p_sequence_range>,
        compact_sequence_range<sequence_custom_alphabet<
            typename base_seq_traits::alphabet_type, p_hash_value,
            base_seq_traits::bits_per_char>>>;
    using view_type = std::conditional_t<can_bitcast_to_hash,
        bitcast_view_type,
        compact_sequence_range<p_sequence_range> const&>;

    // Bind the borrowed sequence or temporary view by reference. This keeps
    // an owning uequence from being copied and extends the temporary view
    // lifetime through the traversal.
    auto&& seq_view = ([&seq]() -> view_type {
        if constexpr (can_bitcast_to_hash) {
            return seq.template bit_cast_to_storage_unit<p_hash_value>();
        } else {
            return seq;
        }
    })();

    // The selected view determines the SIMD lane width used by rolling state.
    using seq_traits   = typename std::remove_cvref_t<view_type>::traits;
    using storage_unit = p_hash_value;
    using alphabet_type   = typename seq_traits::alphabet_type;
    using hash_alphabet = std::conditional_t<seqan3::nucleotide_alphabet<alphabet_type>, seq::dna4, alphabet_type>;
    using hash_traits = typename hasher_base<hash_alphabet, storage_unit>::traits;

    // A complete k-mer and its reverse-complement shift must fit in one lane.
    DEBUG_ASSERT(k * hash_traits::bits_per_char <= sizeof(storage_unit) * 8,
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

    // Keep one vector and mask per stream. Inlining lets the compiler operate
    // on the  individual vectors.
    using vector_stream_type =
        vector_stream<simd_tag_type, storage_unit, p_stream_count>;
    using mask_stream_type = mask_stream<simd_tag_type, p_stream_count>;

    const size_t lanes_per_stream = hn::Lanes(d);

    const size_t kmer_size = k;
    const storage_unit kmer_mask =
        hash_traits::exact_kmer_mask(kmer_size);
    const vector_type v_kmer_mask = hn::Set(d, kmer_mask);

    // Pack the ordering hash in the high half and the local position in the low half
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

    const vector_type v_zero      = hn::Zero(d);
    const vector_type v_one       = hn::Set(d, static_cast<storage_unit>(1));
    const vector_type v_pos_mask  = hn::Set(d, pos_mask);
    // Keep the high product bits for the ordering hash, leaving the low half
    // free for the position. Applying kmer_mask here can, erase the hash: for
    // K15 in a u64 lane, the k-mer mask covers only the low 30 bits.
    const storage_unit hash_kmer_mask = hash_mask_high;
    const vector_type v_hash_kmer_mask = hn::Set(d, hash_kmer_mask);
    constexpr storage_unit golden_ratio_const =
        (sizeof(storage_unit) >= 4)
            ? static_cast<storage_unit>(0x9E3779B9)
            : static_cast<storage_unit>(0x9E37);
    const vector_type v_golden = hn::Set(d, golden_ratio_const);

    constexpr size_t buf_capacity = has_static_w ? p_static_window_size : k_max_two_stack_window;

    // Position wrapping and the number of bases needed for the first full window.
    constexpr storage_unit pos_max = pos_mask;
    const storage_unit wrap_delta_amount =
        static_cast<storage_unit>(pos_max - static_cast<storage_unit>(w) - 1);
    const size_t wrap_period = static_cast<size_t>(wrap_delta_amount);
    const size_t emit_warmup = k + w - 1;
    const vector_type  v_wrap      = hn::Set(d, wrap_delta_amount);

    // The output  hasher uses the full k-mer mask when emitting a selected record.
    const p_hash_value kmer_mask_scalar =
        static_cast<p_hash_value>(kmer_mask);

    // Initialize the traversal state, then fill the first w ring slots below.
    // The remaining array slots are unused.
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
        .prefix_strand              = vector_stream_type::splat(d, static_cast<storage_unit>(0)),
        .prefix_valid               = mask_stream_type::splat_false(d),
        .valid_run                  = vector_stream_type::splat(d, static_cast<storage_unit>(0)),
        .pos_local                  = vector_stream_type::splat(d, static_cast<storage_unit>(0)),
        .last_emit_pos              = vector_stream_type::splat(d, max_val),
        .last_emit_valid            = mask_stream_type::splat_false(d),
        .ring_buf_elem              = two_stack_internal::
            make_undefined_vector_stream_array<
                buf_capacity, simd_tag_type, storage_unit, p_stream_count>(d),
        .ring_buf_hash_strand       = two_stack_internal::
            make_undefined_vector_stream_array<
                buf_capacity, simd_tag_type, storage_unit, p_stream_count>(d),
        .ring_buf_strand            = two_stack_internal::
            make_undefined_vector_stream_array<
                buf_capacity, simd_tag_type, storage_unit, p_stream_count>(d),
        .ring_buf_valid             = {},
        .pos_wrap_delta             = static_cast<storage_unit>(0),
        .step                       = 0,
        .ring_idx                   = 0,
        .next_wrap_step             = wrap_period + w,
        .v_kmer_mask                = v_kmer_mask,
        .v_zero                     = v_zero,
        .v_one                      = v_one,
        .v_pos_mask                 = v_pos_mask,
        .v_hash_kmer_mask           = v_hash_kmer_mask,
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
            state.ring_buf_strand[i] = initial_hs;
            state.ring_buf_valid[i] = mask_stream_type::splat_false(d);
        }
    }

    // Feed each decoded vector stream to the state update.
    iterate_sequence_bp_simd<p_stream_count, simd_tag_type>(
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
        // Each stp hands selected records to the collector. This finish callback
        // has no extra minimizer to emit.
        [](auto const&, auto const&, auto&) {});
    if (state.failure != two_stack_continuation_status::ok)
        throw exact_simd_runtime_error(state.failure, "resident minimizer input contains an invalid native rank");
}


/**
 * Selects two streams when the sequence has enough bases for both streams
 * and their k-mer/window context. Dispatch and statistics use this  same
 * check so the reported stream count matches the selected branch.
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
    constexpr bool can_narrow = sizeof(p_hash_unit) < sizeof(typename seq_traits::storage_unit)
        && is_sequence_bit_castable_to<p_sequence_range, p_hash_unit>;
    // This scheduling heuristic needs only type geometry, not physical access.
    constexpr auto selected_unit_bytes = can_narrow
        ? sizeof(p_hash_unit) : sizeof(typename seq_traits::storage_unit);
    constexpr std::size_t letters_per_hash_unit =
        selected_unit_bytes * std::numeric_limits<std::uint8_t>::digits / seq_traits::bits_per_char;
    const std::size_t lanes_per_stream = hn::Lanes(storage_simd_tag_type{});
    const std::size_t context_bps = k + w - 1u;
    const std::size_t min_seq_for_dual =
        2u * lanes_per_stream * letters_per_hash_unit + context_bps;
    return seq.bp_size() >= min_seq_for_dual;
}


/**
 * Selects fixed or runtime k-mer/window sizes and runs the resident SIMD
 * collector. Uses the sequence storage width when the k-mer fits, otherwise
 * requests a 64-bit width. Sequence length decides whether to use one or
 * two streams.
 */
template<
    template<typename p_hash_alphabet> typename p_simd_hasher_template,
    class p_minimizer_type,
    supports_compact_sequence_range p_sequence_range,
    typename p_iterator = decltype(
        compact_sequence_range<p_sequence_range>().begin()),
    // The default collector writes batches to the caller-provided sink
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
    using alphabet_type = typename seq_traits::alphabet_type;
    using hash_alphabet = std::conditional_t<seqan3::nucleotide_alphabet<alphabet_type>, seq::dna4, alphabet_type>;
    constexpr size_t bpc = hasher_base<hash_alphabet, std::uint64_t>::traits::bits_per_char;
    constexpr bool narrow_layout = std::numeric_limits<std::uint32_t>::digits % seq_traits::bits_per_char == 0u;

    // Choose the stream count using the selected lane width and its number
    // of packed symbols per unit.
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
                    /*p_stream_count=*/2, /*p_detect_palindrome=*/true,
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

    // Use the storage lane when  it fits the k-mer, or request a 64-bit lane.
    auto dispatch_with_k_aware_unit =
        [&k, &w, &dispatch_with_unit]<size_t p_static_window_size, size_t p_static_kmer_size>() {
            if constexpr (p_static_kmer_size > 0) {
                constexpr size_t kmer_bits_static = p_static_kmer_size * bpc;
                if constexpr (narrow_layout && kmer_bits_static <= std::numeric_limits<std::uint32_t>::digits) {
                    dispatch_with_unit.template operator()<
                        std::uint32_t, p_static_window_size, p_static_kmer_size>();
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
                if constexpr (narrow_layout) {
                    if (kmer_bits <= std::numeric_limits<std::uint32_t>::digits) {
                        dispatch_with_unit.template operator()<
                            std::uint32_t, p_static_window_size, 0>();
                        return;
                    }
                }
                if (kmer_bits <= 64) {
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
    if constexpr (15u * bpc <= std::numeric_limits<std::uint64_t>::digits) {
        if (w == 10 && k == 15) [[likely]] {
            dispatch_with_k_aware_unit.template operator()<10, 15>();
            return;
        }
    }
    if (w == 10) {
        dispatch_with_k_aware_unit.template operator()<10, 0>();
        return;
    }
    dispatch_with_k_aware_unit.template operator()<0, 0>();
}

/** Runs the resident SIMD collector with fixed k-mer and window sizes. */

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
    // The shared dispatcher preserves fixed K15/W10 specialization where its
    // native rank width fits, and uses the same admitted geometry everywhere.
    collect_minimizers_two_stack_nthash_simd<
        p_simd_hasher_template, p_minimizer_type, p_sequence_range>(
            collector, seq, seq_begin, seq_end, p_window_size, p_kmer_size);
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
 * Collects a whole native resident sequence with admitted exact rank geometry,
 * then returns execution counters. Requires a supported SIMD target.
 *
 * Throws exact_simd_runtime_error for an unsupported configuration or an
 * inconsistent output count.
 */
template<
    class p_minimizer_type,
    supports_compact_sequence_range p_sequence_range,
    class p_collector_type = minimizer_sink_collector<p_minimizer_type>>
requires (
    two_stack_simd_minimizer_representation<p_minimizer_type>)
[[nodiscard]] inline two_stack_execution_stats
collect_minimizers_two_stack_nthash_resident_or_throw(
    p_collector_type& collector,
    compact_sequence_range<p_sequence_range>& seq,
    std::size_t w,
    std::size_t k)
{
    using traits = typename compact_sequence_range<p_sequence_range>::traits;
    using alphabet = typename traits::alphabet_type;
    using hash_alphabet = std::conditional_t<seqan3::nucleotide_alphabet<alphabet>, seq::dna4, alphabet>;
    constexpr auto bits = hasher_base<hash_alphabet, std::uint64_t>::traits::bits_per_char;
    if (w == 0u || w > k_max_two_stack_window || k == 0u
        || k > std::numeric_limits<std::uint64_t>::digits / bits) {
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
    constexpr bool narrow_layout = std::numeric_limits<std::uint32_t>::digits % traits::bits_per_char == 0u;
    const bool narrow = narrow_layout && k <= std::numeric_limits<std::uint32_t>::digits / bits;
    const bool shape_supported = narrow
        ? two_stack_internal::resident_vector_shape_supported<hn::ScalableTag<std::uint32_t>>()
        : two_stack_internal::resident_vector_shape_supported<hn::ScalableTag<std::uint64_t>>();
    if (!shape_supported) {
        throw exact_simd_runtime_error(
            two_stack_continuation_status::unsupported_vector_shape,
            "resident vector_stream minimizer sketch");
    }
    const std::size_t records_before =
        two_stack_internal::resident_collector_size(collector);
    const bool used_dual_streams = narrow
        ? two_stack_resident_uses_dual_streams<std::uint32_t>(seq, w, k)
        : two_stack_resident_uses_dual_streams<std::uint64_t>(seq, w, k);
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
    two_stack_simd_minimizer_representation<p_minimizer_type>)
[[nodiscard]] inline two_stack_execution_stats
collect_minimizers_two_stack_nthash_resident_descriptor_or_throw(
    p_collector_type& collector,
    const typename bioseq::storage::lazy_sequence_view<p_alphabet>::
        resident_packed_sequence& descriptor,
    std::size_t w,
    std::size_t k)
{
    if (!all_storage_unit_byte_widths::contains_runtime(descriptor.storage_unit_width)
        || descriptor.bits_per_symbol != compact_sequence_range<p_alphabet>::traits::bits_per_char
        || descriptor.leading_symbol_offset != 0u) {
        throw exact_simd_runtime_error(
            two_stack_continuation_status::non_contiguous_range,
            "resident vector_stream minimizer descriptor");
    }
    two_stack_execution_stats result{};
    dispatch_storage_unit_byte_width(descriptor.storage_unit_width,
        [&collector, &descriptor, &result, w, k]<std::size_t Width>() {
            using unit = std::conditional_t<Width == sizeof(std::uint64_t), std::uint64_t,
                std::conditional_t<Width == sizeof(std::uint32_t), std::uint32_t,
                std::conditional_t<Width == sizeof(std::uint16_t), std::uint16_t, std::uint8_t>>>;
            result = resident_descriptor_detail::with_borrowed_compact_range<p_alphabet, unit>(
                descriptor, [&collector, w, k]<class Range>(Range& range) {
                    return collect_minimizers_two_stack_nthash_resident_or_throw<p_minimizer_type>(
                        collector, range, w, k);
                });
        });
    return result;
}

template<
    std::size_t p_window_size,
    std::size_t p_kmer_size,
    class p_minimizer_type,
    supports_compact_sequence_range p_sequence_range,
    class p_collector_type = minimizer_sink_collector<p_minimizer_type>>
requires (
    two_stack_simd_minimizer_representation<p_minimizer_type>)
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
    return collect_minimizers_two_stack_nthash_resident_or_throw<p_minimizer_type>(
        collector, seq, p_window_size, p_kmer_size);
}



}  // namespace target
}  // namespace bioseq::simd
