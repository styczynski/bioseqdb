/** Canonical generator input for the exact two-stack minimizer kernels. */
module;
#include <hwy/highway.h>
#include <libassert/assert.hpp>

export module bioseq.highway.sources.minimizer_two_stack_nthash;

import std;
import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.instrumentation;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.sequences.kmers.highway_minimizer_contract;
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

using ::bioseq::minimizers::minimap_sketch_data;
using ::bioseq::minimizers::minimizer_representation;
using ::bioseq::minimizers::minimizer_sink_collector;
using ::bioseq::minimizers::minimizer_kernel_algorithm;
using ::bioseq::minimizers::minimizer_kernel_invocation;
using ::bioseq::minimizers::minimizer_kernel_result;
using ::bioseq::minimizers::minimizer_kernel_status;
using ::bioseq::minimizers::minimizer_output_layout;
using ::bioseq::minimizers::minimizer_output_position_mask;
using ::bioseq::minimizers::two_stack_route;

/**
 * Compile-time upper bound on the window size `w` is_internally_consistent by the
 * two-stack collectors in this file. Our minimap2 ports run at the
 * default `w = 10`; bumping past 256 also blows the L1-friendly stack
 * footprint of the parallel ring buffers.
 */
using ::bioseq::minimizers::k_max_two_stack_window;

/** Maximum nt4 k-mer size represented by one native Highway integer lane. */
using ::bioseq::minimizers::k_max_two_stack_kmer;

/** Largest k-mer accepted by the exact scalar-oracle arithmetic. */
using ::bioseq::minimizers::k_max_exact_two_stack_kmer;

using ::bioseq::minimizers::two_stack_continuation_status;
using ::bioseq::minimizers::exact_simd_kernel;
using ::bioseq::minimizers::two_stack_continuation_status_name;
using ::bioseq::minimizers::two_stack_execution_stats;
using ::bioseq::minimizers::two_stack_finalize_result;
using ::bioseq::minimizers::bounded_nthash_status;
using ::bioseq::minimizers::k_bounded_nthash_scheme_id;
using ::bioseq::minimizers::k_bounded_nthash_max_window;
using ::bioseq::minimizers::bounded_nthash_execution_stats;
using ::bioseq::minimizers::bounded_nthash_result;
using ::bioseq::minimizers::exact_simd_runtime_error;
using ::bioseq::minimizers::exact_simd_minimizer_representation;

namespace resident_descriptor_detail {

/// @brief Non-owning header for a resident packed sequence descriptor.
struct borrowed_packed_header {
    const void* data = nullptr;
    bioseq::storage::sequence_size_t bp_size = 0;
    std::uint8_t storage_unit_byte_width = 0;
    std::uint16_t padding_letters = 0;
};

/// @brief Adapts a borrowed packed header to the compact-sequence protocol.
template <sequence_alphabet p_alphabet, class p_storage_unit>
struct borrowed_packed_instance {
    using alphabet_type = p_alphabet;
    using storage_unit = p_storage_unit;
    using compact_sequence_stdc_type = borrowed_packed_header;

    static constexpr std::size_t letter_bit_width =
        bioseq::storage::lazy_sequence_view<p_alphabet>::traits::
            k_logical_bits_per_symbol;
    static constexpr std::size_t byte_width = sizeof(p_storage_unit);
    static constexpr bool is_valid_compact_sequence_stdc_instance = true;
};

/// @brief Invokes a function with a typed non-owning compact range.
template <sequence_alphabet p_alphabet, class p_storage_unit, class p_function>
decltype(auto) with_borrowed_compact_range(
        const typename bioseq::storage::lazy_sequence_view<p_alphabet>::
            resident_packed_sequence& descriptor,
        p_function&& fn) {
    using instance = borrowed_packed_instance<p_alphabet, p_storage_unit>;
    using range_type = compact_sequence_range<instance>;
    borrowed_packed_header header{
        .data = descriptor.units,
        .bp_size = descriptor.logical_length,
        .storage_unit_byte_width = descriptor.storage_unit_width,
        .padding_letters = descriptor.trailing_padding,
    };
    range_type range{header};
    return std::forward<p_function>(fn)(range);
}

}  // namespace resident_descriptor_detail

/**
 * Fragment continuation for the nt4-normalized nucleotide two-stack kernel.
 * Pending capacity follows the generated target's Highway lane count.
 */
template<std::size_t p_window_size, std::size_t p_kmer_size, class p_alphabet_traits>
struct two_stack_continuation_state {
    static constexpr std::size_t max_vector_lanes =
        hn::MaxLanes(hn::ScalableTag<std::uint64_t>{});
    static constexpr std::size_t fast_block_candidates = 8;
    static constexpr std::size_t pending_capacity =
        std::max(max_vector_lanes, fast_block_candidates);

    using traits = p_alphabet_traits;
    using alphabet_type = typename traits::alphabet_type;

    // Static routes retain their own bounded window and k-mer capacities.
    // scratch footprint.  This state is instantiated inside the per-sketch
    // driver, and its arrays participate in the hot candidate-block frame.
    static constexpr std::size_t code_tail_capacity =
        p_kmer_size != 0 && p_kmer_size <= k_max_exact_two_stack_kmer
            ? std::max<std::size_t>(p_kmer_size - 1, 1)
            : k_max_exact_two_stack_kmer - 1;
    static constexpr std::size_t candidate_tail_capacity =
        p_window_size != 0 && p_window_size <= k_max_two_stack_window
            ? std::max<std::size_t>(p_window_size - 1, 1)
            : k_max_two_stack_window - 1;

    /// @brief Stores one chronological minimizer candidate.
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
    exact_simd_kernel kernel_kind = exact_simd_kernel::generic_u64;
    two_stack_continuation_status status = two_stack_continuation_status::ok;
    two_stack_execution_stats stats{};

    /// @brief Returns the compile-time or runtime window size.
    [[nodiscard]] constexpr std::size_t window_size() const noexcept {
        if constexpr (p_window_size != 0) return p_window_size;
        return runtime_w;
    }

    /// @brief Returns the compile-time or runtime k-mer size.
    [[nodiscard]] constexpr std::size_t kmer_size() const noexcept {
        if constexpr (p_kmer_size != 0) return p_kmer_size;
        return runtime_k;
    }

};

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
 * one vector load; all offset, packed-shift, and candidate-position work stays
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
    /// parameter on `vector_stream_type` / `mask_stream_type`; there is NO
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
 * the checked chronological continuation; there is no scalar fallback here.
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
 *  * `minimizer_representation<33, 15>` for target postings (30 hash bits); and
 *  * `minimap_sketch_data == minimizer_representation<28, 15>` for query sketches
 *    (35 hash bits, of which minimap-int uses the low 30).
 *
 * The original gate admitted only the target layout even though every raw
 * construction below is already parameterized by `k_hash_shift`,
 * `k_position_field_mask`, and `k_position_shift`.  Keep the admission deliberately narrow
 * to these two persisted-compatible layouts rather than silently widening the
 * kernel to arbitrary `p_minimizer_type` encodings.
 */
template <class p_minimizer_type>
inline constexpr bool exact_k15_w10_b8_representation_v =
    exact_simd_minimizer_representation<p_minimizer_type> &&
    std::same_as<typename p_minimizer_type::storage_type, std::uint64_t> &&
    p_minimizer_type::k_position_shift == 1u &&
    ((static_cast<std::uint64_t>(p_minimizer_type::k_hash_field_mask) == 0x3fffffffull &&
      static_cast<std::uint64_t>(p_minimizer_type::k_position_field_mask) == 0x1ffffffffull &&
      p_minimizer_type::k_hash_shift == 34u) ||
     (static_cast<std::uint64_t>(p_minimizer_type::k_hash_field_mask) == 0x7ffffffffull &&
      static_cast<std::uint64_t>(p_minimizer_type::k_position_field_mask) == 0x0fffffffull &&
      p_minimizer_type::k_hash_shift == 29u));

template<class p_minimizer_type, class p_alphabet_traits>
[[nodiscard]] inline exact_simd_kernel select_exact_simd_kernel_kind(
    std::size_t consumed_bp) noexcept
{
    if constexpr (std::same_as<typename p_alphabet_traits::alphabet_type, seq::dna4> &&
                  std::same_as<typename p_alphabet_traits::storage_unit, std::uint32_t> &&
                  exact_k15_w10_b8_representation_v<p_minimizer_type>) {
        const hn::ScalableTag<std::uint32_t> d32;
        const hn::ScalableTag<std::uint64_t> d64;
        if (!exact_compiled_target_is_scalar && hn::Lanes(d32) == 4 && hn::Lanes(d64) == 2 &&
            consumed_bp <= static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
            return exact_simd_kernel::kmer_15_window_10_byte_8_uint32_hash;
        }
    }
    return exact_simd_kernel::generic_u64;
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
 * therefore deliberately leaves every execution-stats counter at zero.
 */
template<class p_state_type>
[[nodiscard]] constexpr two_stack_continuation_status
validate_exact_vector_shape(
    p_state_type& state,
    std::size_t lanes,
    std::size_t lane_capacity,
    bool compiled_target_is_scalar,
    bool transpose_shape_supported) noexcept
{
    if (compiled_target_is_scalar
        || !exact_vector_lane_count_supported(lanes, lane_capacity)
        || !transpose_shape_supported) {
        state.status = two_stack_continuation_status::unsupported_vector_shape;
        return state.status;
    }
    return two_stack_continuation_status::ok;
}

#if HWY_TARGET == HWY_SCALAR

// Keep the scalar translation entirely free of multi-lane Highway operations.
// These stubs let the common reset/consume templates compile, but the target
// gate rejects them before any biological work or stats accounting.
template<class p_simd_tag>
[[nodiscard]] HWY_INLINE bool u64_lane_transpose_shape_supported(p_simd_tag) noexcept
{
    static_assert(std::is_same_v<hn::TFromD<p_simd_tag>, std::uint64_t>);
    return false;
}

template<class p_simd_tag>
[[nodiscard]] HWY_INLINE bool transpose_u64_lane_matrix(
    p_simd_tag,
    hn::Vec<p_simd_tag>* HWY_RESTRICT) noexcept
{
    static_assert(std::is_same_v<hn::TFromD<p_simd_tag>, std::uint64_t>);
    return false;
}

#else

/**
 * Whether repeated Half<p_simd_tag> operations exactly describe a power-of-two square
 * for this target. This is checked before any biological work so an unusual
 * scalable-vector shape cannot reach a late transpose failure.
 */
template<class p_simd_tag>
[[nodiscard]] HWY_INLINE bool u64_lane_transpose_shape_supported(p_simd_tag d) noexcept
{
    static_assert(std::is_same_v<hn::TFromD<p_simd_tag>, std::uint64_t>);
    const std::size_t lanes = hn::Lanes(d);
    if (lanes < 2 || lanes > 16) return false;
    if (lanes == 2) return true;
    if ((lanes & (lanes - 1)) != 0) return false;

    if constexpr (hn::MaxLanes(p_simd_tag{}) < 4) {
        return false;
    } else {
        using half_simd_tag_type = hn::Half<p_simd_tag>;
        const half_simd_tag_type dh;
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
template<class p_simd_tag>
[[nodiscard]] HWY_INLINE bool transpose_u64_lane_matrix(
    p_simd_tag d,
    hn::Vec<p_simd_tag>* HWY_RESTRICT matrix) noexcept
{
    static_assert(std::is_same_v<hn::TFromD<p_simd_tag>, std::uint64_t>);
    const std::size_t lanes = hn::Lanes(d);
    if (lanes == 2) {
        const hn::Vec<p_simd_tag> column0 = matrix[0];
        const hn::Vec<p_simd_tag> column1 = matrix[1];
        matrix[0] = hn::InterleaveLower(d, column0, column1);
        matrix[1] = hn::InterleaveUpper(d, column0, column1);
        return true;
    }
    if (lanes < 4 || lanes > 16 || (lanes & (lanes - 1)) != 0) {
        return false;
    }

    if constexpr (hn::MaxLanes(p_simd_tag{}) < 4) {
        return false;
    } else {
        using half_simd_tag_type = hn::Half<p_simd_tag>;
        using half_vector_type = hn::Vec<half_simd_tag_type>;
        constexpr std::size_t half_capacity = hn::MaxLanes(half_simd_tag_type{});
        const half_simd_tag_type dh;
        const std::size_t half_lanes = lanes / 2;
        if (hn::Lanes(dh) != half_lanes) return false;

        HWY_ALIGN std::array<half_vector_type, half_capacity> lower_left{};
        HWY_ALIGN std::array<half_vector_type, half_capacity> lower_right{};
        HWY_ALIGN std::array<half_vector_type, half_capacity> upper_left{};
        HWY_ALIGN std::array<half_vector_type, half_capacity> upper_right{};
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

template<class p_state_type>
[[nodiscard]] inline two_stack_continuation_status reject_continuation(
    p_state_type& state,
    two_stack_continuation_status status) noexcept
{
    state.status = status;
    if (state.stats.fail_closed_rejections
        != std::numeric_limits<std::uint64_t>::max()) {
        ++state.stats.fail_closed_rejections;
    }
    return status;
}

inline void add_stats_counter(
    std::uint64_t& counter,
    std::uint64_t increment) noexcept
{
    const std::uint64_t maximum =
        std::numeric_limits<std::uint64_t>::max();
    counter = increment > maximum - counter
        ? maximum
        : counter + increment;
}

template<class p_simd_tag>
struct decoded_dna4_rank_vector {
    using byte_simd_tag_type = hn::Rebind<std::uint8_t, p_simd_tag>;
    hn::Vec<byte_simd_tag_type> ranks;
    bool crossed_word = false;
};

template<class p_simd_tag>
struct decoded_nt4_rank_vector {
    using byte_simd_tag_type = hn::Rebind<std::uint8_t, p_simd_tag>;
    hn::Vec<byte_simd_tag_type> codes;
    std::uint64_t ambiguous_lanes = 0;
    bool crossed_word = false;
};

/** Select one lane-sized chunk from the two-word packed decode staging pair. */
template<class p_lane_type>
[[nodiscard, gnu::always_inline]] HWY_INLINE p_lane_type
extract_packed_dna4_lane_chunk(
    std::uint64_t packed_low,
    std::uint64_t packed_high,
    std::size_t origin) noexcept
{
    static_assert(std::is_unsigned_v<p_lane_type>);
    static_assert(sizeof(p_lane_type) == 4 || sizeof(p_lane_type) == 8);
    if (origin < 64) {
        std::uint64_t chunk = packed_low >> origin;
        if (origin != 0) {
            chunk |= packed_high << (64 - origin);
        }
        return static_cast<p_lane_type>(chunk);
    }
    return static_cast<p_lane_type>(packed_high >> (origin - 64));
}

/**
 * Decode up to Lanes(p_simd_tag) consecutive packed DNA4 ranks in real SIMD lanes.
 *
 * The packed source may use 8/16/32/64-bit storage units.  We assemble at
 * most the 128 source bits covering this lane block, then perform rank
 * extraction with vector variable shifts.  This keeps the biological decode
 * vectorized while allowing scanner fragments to forward their packed span
 * without constructing or re-encoding a compact sequence object.
 */
// BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN decode_packed_dna4_rank_vector
template<class p_simd_tag, class p_alphabet_traits>
[[nodiscard]] HWY_INLINE decoded_dna4_rank_vector<p_simd_tag>
decode_packed_dna4_rank_vector(
    p_simd_tag d,
    std::span<const typename p_alphabet_traits::storage_unit> packed,
    std::size_t local_bp,
    std::size_t rank_count) noexcept
{
    using lane_type = hn::TFromD<p_simd_tag>;
    using storage_unit = typename p_alphabet_traits::storage_unit;
    using unsigned_storage = std::make_unsigned_t<storage_unit>;
    static_assert(std::is_unsigned_v<lane_type>);
    static_assert(sizeof(lane_type) == 4 || sizeof(lane_type) == 8);
    static_assert(p_alphabet_traits::bits_per_char == 2);
    static_assert(std::is_integral_v<storage_unit>);
    static_assert(sizeof(storage_unit) == 1 || sizeof(storage_unit) == 2
        || sizeof(storage_unit) == 4 || sizeof(storage_unit) == 8);
    constexpr std::size_t unit_bits = sizeof(storage_unit) * 8;
    constexpr std::size_t letters_per_unit = unit_bits / 2;
    static_assert(p_alphabet_traits::letters_per_unit == letters_per_unit);

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
    const hn::Rebind<std::uint16_t, p_simd_tag> d_u16;
    const hn::Rebind<std::uint8_t, p_simd_tag> d_u8;
    if constexpr (sizeof(lane_type) == 8) {
        const hn::Rebind<std::uint32_t, p_simd_tag> d_u32;
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
// BIOSEQ_PRIVATE_PACKED_ACCESS_END decode_packed_dna4_rank_vector

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
// BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN decode_packed_nt4_rank_vector
template<class p_simd_tag, class p_alphabet_traits>
[[nodiscard]] HWY_INLINE decoded_nt4_rank_vector<p_simd_tag>
decode_packed_nt4_rank_vector(
    p_simd_tag d,
    std::span<const typename p_alphabet_traits::storage_unit> packed,
    std::size_t local_bp,
    std::size_t rank_count) noexcept
{
    using alphabet_type = typename p_alphabet_traits::alphabet_type;
    using storage_unit = typename p_alphabet_traits::storage_unit;
    using unsigned_storage = std::make_unsigned_t<storage_unit>;
    static_assert(seqan3::nucleotide_alphabet<alphabet_type>);
    static_assert(std::is_integral_v<storage_unit>);
    static_assert(sizeof(storage_unit) == 1 || sizeof(storage_unit) == 2
        || sizeof(storage_unit) == 4 || sizeof(storage_unit) == 8);
    static_assert(p_alphabet_traits::bits_per_char > 0
        && p_alphabet_traits::bits_per_char <= 8);
    constexpr std::size_t unit_bits = sizeof(storage_unit) * 8;
    constexpr std::size_t bits_per_char = p_alphabet_traits::bits_per_char;
    constexpr std::size_t letters_per_unit = unit_bits / bits_per_char;
    static_assert(p_alphabet_traits::letters_per_unit == letters_per_unit);
    constexpr std::size_t max_lanes = hn::MaxLanes(p_simd_tag{});

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
        d, static_cast<std::uint64_t>(k_nt4_sentinel));
    for (std::size_t rank = 0;
         rank < seqan3::alphabet_size<alphabet_type>;
         ++rank) {
        nt4_codes = hn::IfThenElse(
            hn::Eq(ranks, hn::Set(d, static_cast<std::uint64_t>(rank))),
            hn::Set(d, static_cast<std::uint64_t>(
                k_nt4_lut<alphabet_type>[rank])),
            nt4_codes);
    }
    const auto active = hn::FirstN(d, rank_count);
    const auto ambiguous = hn::And(
        active,
        hn::Ge(
            nt4_codes,
            hn::Set(d, static_cast<std::uint64_t>(k_nt4_sentinel))));

    const hn::Rebind<std::uint32_t, p_simd_tag> d_u32;
    const hn::Rebind<std::uint16_t, p_simd_tag> d_u16;
    const hn::Rebind<std::uint8_t, p_simd_tag> d_u8;
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
// BIOSEQ_PRIVATE_PACKED_ACCESS_END decode_packed_nt4_rank_vector

template<class p_simd_tag>
[[nodiscard]] HWY_INLINE hn::Vec<p_simd_tag> load_promoted_dna4_codes(
    p_simd_tag d,
    const std::uint8_t* HWY_RESTRICT codes) noexcept
{
    const hn::Rebind<std::uint8_t, p_simd_tag> d_u8;
    const hn::Rebind<std::uint16_t, p_simd_tag> d_u16;
    const hn::Rebind<std::uint32_t, p_simd_tag> d_u32;
    const auto codes_u8 = hn::LoadU(d_u8, codes);
    const auto codes_u16 = hn::PromoteTo(d_u16, codes_u8);
    const auto codes_u32 = hn::PromoteTo(d_u32, codes_u16);
    return hn::PromoteTo(d, codes_u32);
}

template<class p_simd_tag>
[[nodiscard]] HWY_INLINE hn::Vec<p_simd_tag> load_promoted_dna4_codes_u32(
    p_simd_tag d,
    const std::uint8_t* HWY_RESTRICT codes) noexcept
{
    static_assert(std::is_same_v<hn::TFromD<p_simd_tag>, std::uint32_t>);
    const hn::Rebind<std::uint8_t, p_simd_tag> d_u8;
    const hn::Rebind<std::uint16_t, p_simd_tag> d_u16;
    const auto codes_u8 = hn::LoadU(d_u8, codes);
    return hn::PromoteTo(d, hn::PromoteTo(d_u16, codes_u8));
}

template<class p_uint32_simd_tag, class p_uint64_simd_tag>
struct split_u32_mask_result {
    hn::Mask<p_uint64_simd_tag> lower;
    hn::Mask<p_uint64_simd_tag> upper;
};

/** Split one u32x4 decision mask into its two chronological u64x2 halves. */
template<class p_uint32_simd_tag, class p_uint64_simd_tag>
[[nodiscard]] HWY_INLINE split_u32_mask_result<p_uint32_simd_tag, p_uint64_simd_tag> split_u32_mask(
    p_uint32_simd_tag d32,
    p_uint64_simd_tag d64,
    hn::Mask<p_uint32_simd_tag> mask) noexcept
{
    const auto bits = hn::VecFromMask(d32, mask);
    const auto zero = hn::Zero(d64);
    return {
        .lower = hn::Ne(hn::PromoteLowerTo(d64, bits), zero),
        .upper = hn::Ne(hn::PromoteUpperTo(d64, bits), zero),
    };
}

template<class p_uint32_simd_tag, class p_uint64_simd_tag>
[[nodiscard]] HWY_INLINE hn::Mask<p_uint32_simd_tag> join_u64_masks(
    p_uint32_simd_tag d32,
    p_uint64_simd_tag d64,
    hn::Mask<p_uint64_simd_tag> lower,
    hn::Mask<p_uint64_simd_tag> upper) noexcept
{
    const hn::Half<p_uint32_simd_tag> d32_half;
    return hn::CombineMasks(
        d32,
        hn::DemoteMaskTo(d32_half, d64, upper),
        hn::DemoteMaskTo(d32_half, d64, lower));
}

template<class p_uint32_simd_tag, class p_uint64_simd_tag>
struct exact_fast_minimum_vectors {
    hn::Vec<p_uint32_simd_tag> hash;
    hn::Vec<p_uint64_simd_tag> raw_lower;
    hn::Vec<p_uint64_simd_tag> raw_upper;
    hn::Vec<p_uint64_simd_tag> ordinal_lower;
    hn::Vec<p_uint64_simd_tag> ordinal_upper;
};

/**
 * Reduce four overlapping K15/W10 hash windows.  The sequential <= scan
 * deliberately preserves the exact rightmost-minimum rule used by the
 * scalar oracle; metadata follows the same mask at every chronological slot.
 */
template<class p_uint32_simd_tag, class p_uint64_simd_tag>
[[nodiscard]] HWY_INLINE exact_fast_minimum_vectors<p_uint32_simd_tag, p_uint64_simd_tag>
reduce_exact_k15_w10_minimum_metadata(
    p_uint32_simd_tag d32,
    p_uint64_simd_tag d64,
    const std::uint32_t* HWY_RESTRICT candidate_hash,
    const std::uint64_t* HWY_RESTRICT candidate_raw,
    const std::uint64_t* HWY_RESTRICT candidate_ordinal,
    std::uint32_t maximum_hash,
    std::uint64_t maximum_raw,
    std::uint64_t ordinal_sentinel) noexcept
{
    static_assert(std::is_same_v<hn::TFromD<p_uint32_simd_tag>, std::uint32_t>);
    static_assert(std::is_same_v<hn::TFromD<p_uint64_simd_tag>, std::uint64_t>);
    constexpr std::size_t window = 10;
    exact_fast_minimum_vectors<p_uint32_simd_tag, p_uint64_simd_tag> minimum{
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
    class p_minimizer_type,
    class p_continuation_state,
    class p_collector_type>
[[nodiscard]] inline two_stack_continuation_status
process_exact_k15_w10_u32_group(
    p_continuation_state& state,
    p_collector_type& collector,
    const std::uint32_t* HWY_RESTRICT candidate_hash,
    const std::uint64_t* HWY_RESTRICT candidate_raw,
    const std::uint64_t* HWY_RESTRICT candidate_ordinal,
    std::size_t active_lanes)
{
    static_assert(exact_k15_w10_b8_representation_v<p_minimizer_type>);

    using uint32_simd_tag_type = hn::ScalableTag<std::uint32_t>;
    using uint64_simd_tag_type = hn::ScalableTag<std::uint64_t>;
    using uint32_vector_type = hn::Vec<uint32_simd_tag_type>;
    using uint64_vector_type = hn::Vec<uint64_simd_tag_type>;
    using uint32_mask_type = hn::Mask<uint32_simd_tag_type>;
    const uint32_simd_tag_type d32;
    const uint64_simd_tag_type d64;
    constexpr std::size_t window = 10;
    constexpr std::size_t tail_size = window - 1;
    constexpr std::size_t lanes32 = 4;
    constexpr std::size_t row_stride = 12;
    constexpr std::uint64_t ordinal_sentinel =
        (std::numeric_limits<std::uint64_t>::max)();
    const std::uint32_t maximum_hash = static_cast<std::uint32_t>(
        p_minimizer_type::maximum().kmer());
    const std::uint64_t maximum_raw = p_minimizer_type::maximum().raw_value();

    if (state.status != two_stack_continuation_status::ok) return state.status;
    if (hn::Lanes(d32) != lanes32 || hn::Lanes(d64) != 2
        || active_lanes == 0 || active_lanes > lanes32) {
        return reject_continuation(
            state, two_stack_continuation_status::unsupported_vector_shape);
    }
    if (state.candidate_count
            > (std::numeric_limits<std::uint64_t>::max)() - active_lanes) {
        return reject_continuation(
            state, two_stack_continuation_status::arithmetic_overflow);
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
        add_stats_counter(state.stats.scalar_orchestration_steps, 1);
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
                state, two_stack_continuation_status::invalid_state);
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

    const exact_fast_minimum_vectors<uint32_simd_tag_type, uint64_simd_tag_type> minimum =
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

    const uint32_vector_type previous_hash = hn::LoadU(d32,
        previous_hash_storage.data());
    const uint64_vector_type previous_raw_lower = hn::LoadU(d64,
        previous_raw_storage.data());
    const uint64_vector_type previous_raw_upper = hn::LoadU(d64,
        previous_raw_storage.data() + 2);
    const uint64_vector_type previous_ordinal_lower = hn::LoadU(d64,
        previous_ordinal_storage.data());
    const uint64_vector_type previous_ordinal_upper = hn::LoadU(d64,
        previous_ordinal_storage.data() + 2);
    const uint32_vector_type entering_hash = hn::LoadU(
        d32, combined_hash.data() + tail_size);
    const uint32_mask_type active = hn::FirstN(d32, active_lanes);
    const uint32_mask_type previous_valid = hn::Ne(
        previous_hash, hn::Set(d32, maximum_hash));

    HWY_ALIGN std::array<std::uint64_t, lanes32> ordinal_storage;
    std::memcpy(ordinal_storage.data(), candidate_ordinal,
        lanes32 * sizeof(std::uint64_t));
    const uint64_vector_type ordinal_lower = hn::LoadU(d64, ordinal_storage.data());
    const uint64_vector_type ordinal_upper = hn::LoadU(d64, ordinal_storage.data() + 2);
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
    const uint32_mask_type first_full_window = join_u64_masks(
        d32, d64, first_lower, first_upper);
    const uint32_mask_type steady_window = join_u64_masks(
        d32, d64, steady_lower, steady_upper);

    const uint32_mask_type entering_wins = hn::Le(entering_hash, previous_hash);
    const auto entering_halves = split_u32_mask(d32, d64, entering_wins);
    const uint64_vector_type expired_ordinal_lower = hn::Sub(
        ordinal_lower, hn::Set(d64, std::uint64_t{window}));
    const uint64_vector_type expired_ordinal_upper = hn::Sub(
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
    const uint32_mask_type expired = join_u64_masks(
        d32, d64, expired_lower, expired_upper);
    const uint32_mask_type emit_old = hn::And(
        steady_window,
        hn::And(previous_valid, hn::Or(entering_wins, expired)));
    const uint32_mask_type special_ties = hn::And(first_full_window, previous_valid);
    const uint32_mask_type expiry_ties = hn::And(
        expired,
        hn::Ne(minimum.hash, hn::Set(d32, maximum_hash)));

    const uint32_mask_type possible_ties = hn::Or(special_ties, expiry_ties);
    uint32_mask_type actual_ties = hn::MaskFalse(d32);
    if (hn::CountTrue(d32, possible_ties) != 0) [[unlikely]] {
        uint32_mask_type seen_special_hash = hn::MaskFalse(d32);
        uint32_mask_type seen_expiry_hash = hn::MaskFalse(d32);
        uint32_mask_type duplicate_special_hash = hn::MaskFalse(d32);
        uint32_mask_type duplicate_expiry_hash = hn::MaskFalse(d32);
        for (std::size_t slot = 0; slot < window; ++slot) {
            const uint32_vector_type slot_hash = hn::LoadU(
                d32, combined_hash.data() + slot);
            const uint32_mask_type special_hash_match = hn::And(
                special_ties, hn::Eq(slot_hash, previous_hash));
            const uint32_mask_type expiry_hash_match = hn::And(
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
        const uint32_mask_type duplicate_hashes = hn::Or(
            duplicate_special_hash, duplicate_expiry_hash);
        add_stats_counter(state.stats.tie_hash_prefilter_batches, 1);
        add_stats_counter(
            state.stats.tie_hash_prefilter_rejected_lanes,
            hn::CountTrue(
                d32, hn::AndNot(duplicate_hashes, possible_ties)));
        if (hn::CountTrue(d32, duplicate_hashes) != 0) [[unlikely]] {
            add_stats_counter(state.stats.tie_raw_verification_batches, 1);
            for (std::size_t slot = 0; slot < window; ++slot) {
                const uint32_vector_type slot_hash = hn::LoadU(
                    d32, combined_hash.data() + slot);
                const uint64_vector_type slot_raw_lower = hn::LoadU(
                    d64, combined_raw.data() + slot);
                const uint64_vector_type slot_raw_upper = hn::LoadU(
                    d64, combined_raw.data() + slot + 2);
                const uint32_mask_type differs_previous = join_u64_masks(
                    d32,
                    d64,
                    hn::Ne(slot_raw_lower, previous_raw_lower),
                    hn::Ne(slot_raw_upper, previous_raw_upper));
                const uint32_mask_type differs_minimum = join_u64_masks(
                    d32,
                    d64,
                    hn::Ne(slot_raw_lower, minimum.raw_lower),
                    hn::Ne(slot_raw_upper, minimum.raw_upper));
                const uint32_mask_type special_match = hn::And(
                    duplicate_special_hash,
                    hn::And(
                        hn::Eq(slot_hash, previous_hash), differs_previous));
                const uint32_mask_type expiry_match = hn::And(
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
        add_stats_counter(state.stats.stable_compress_batches, 2);
        add_stats_counter(state.stats.direct_event_compress_batches, 1);
    } else {
        std::array<uint64_vector_type, row_stride> event_lower{};
        std::array<uint64_vector_type, row_stride> event_upper{};
        const uint64_vector_type max_raw_v = hn::Set(d64, maximum_raw);
        const auto emit_halves = split_u32_mask(d32, d64, emit_old);
        event_lower[0] = hn::IfThenElse(
            emit_halves.lower, previous_raw_lower, max_raw_v);
        event_upper[0] = hn::IfThenElse(
            emit_halves.upper, previous_raw_upper, max_raw_v);
        std::uint64_t first_tie_records = 0;
        std::uint64_t expiry_tie_records = 0;
        for (std::size_t slot = 0; slot < window; ++slot) {
            const uint32_vector_type slot_hash = hn::LoadU(
                d32, combined_hash.data() + slot);
            const uint64_vector_type slot_raw_lower = hn::LoadU(
                d64, combined_raw.data() + slot);
            const uint64_vector_type slot_raw_upper = hn::LoadU(
                d64, combined_raw.data() + slot + 2);
            const uint32_mask_type differs_previous = join_u64_masks(
                d32,
                d64,
                hn::Ne(slot_raw_lower, previous_raw_lower),
                hn::Ne(slot_raw_upper, previous_raw_upper));
            const uint32_mask_type differs_minimum = join_u64_masks(
                d32,
                d64,
                hn::Ne(slot_raw_lower, minimum.raw_lower),
                hn::Ne(slot_raw_upper, minimum.raw_upper));
            const uint32_mask_type special_match = hn::And(
                special_ties,
                hn::And(
                    hn::Eq(slot_hash, previous_hash), differs_previous));
            const uint32_mask_type expiry_match = hn::And(
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
            add_stats_counter(state.stats.row_transpose_batches, 1);
            add_stats_counter(state.stats.row_vector_stores, 4);
        }
        const uint64_vector_type raw_hash_mask = hn::Set(
            d64, static_cast<std::uint64_t>(p_minimizer_type::k_hash_mask));
        for (std::size_t offset = 0;
             offset < lanes32 * row_stride;
             offset += 2) {
            const uint64_vector_type events = hn::LoadU(
                d64, compressed_source.data() + offset);
            const auto valid = hn::Ne(
                hn::And(events, raw_hash_mask), raw_hash_mask);
            compressed_count += hn::CompressStore(
                events,
                valid,
                d64,
                compressed_raw.data() + compressed_count);
            add_stats_counter(state.stats.stable_compress_batches, 1);
        }
        add_stats_counter(state.stats.tie_matrix_event_batches, 1);
        add_stats_counter(
            state.stats.first_window_tie_events, first_tie_records);
        add_stats_counter(
            state.stats.expiry_tie_events, expiry_tie_records);
    }

    if (compressed_count != 0
        && !collector.append_raw_storage(
            compressed_raw.data(), compressed_count)) {
        return reject_continuation(
            state, two_stack_continuation_status::collector_overflow);
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
    add_stats_counter(state.stats.minimizers_emitted, compressed_count);
    add_stats_counter(state.stats.vector_batches, 1);
    add_stats_counter(state.stats.vector_lanes_visited, active_lanes);
    add_stats_counter(
        state.stats.kmer_15_window_10_byte_8_uint32_hash_candidate_blocks, 1);
    add_stats_counter(
        state.stats.kmer_15_window_10_byte_8_uint32_hash_lane_visits, active_lanes);
    add_stats_counter(state.stats.masked_tail_lanes, lanes32 - active_lanes);
    add_stats_counter(state.stats.consecutive_kmer_batches, 1);
    add_stats_counter(state.stats.canonical_scramble_batches, 1);
    add_stats_counter(state.stats.window_reduction_batches, 1);
    add_stats_counter(state.stats.event_generation_batches, 1);
    add_stats_counter(
        state.stats.entering_min_transitions,
        hn::CountTrue(d32, hn::And(steady_window, entering_wins)));
    add_stats_counter(
        state.stats.expiry_rescan_transitions,
        hn::CountTrue(d32, expired));
    add_stats_counter(state.stats.scalar_orchestration_steps, 8);
    return two_stack_continuation_status::ok;
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
template<class p_simd_tag>
[[nodiscard, gnu::always_inline]] HWY_INLINE hn::Vec<p_simd_tag>
reverse_two_bit_groups(p_simd_tag d, hn::Vec<p_simd_tag> value) noexcept
{
    static_assert(std::is_same_v<hn::TFromD<p_simd_tag>, std::uint32_t>);
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

template<class p_uint32_simd_tag>
struct exact_k15_b8_windows {
    hn::Vec<p_uint32_simd_tag> lower;
    hn::Vec<p_uint32_simd_tag> upper;
};

/**
 * Extract eight consecutive K15 words from one packed 22-base block.
 *
 * p_input_type assembly is scalar SWAR staging, but all eight biological p_kmer_size-mer
 * extractions are variable-shift SIMD work in four u64x2 pairs.  Demoting and
 * combining those pairs yields the two chronological u32x4 candidate groups
 * consumed by the existing canonical/hash/window/event pipeline.
 */
template<class p_uint32_simd_tag, class p_uint64_simd_tag>
[[nodiscard, gnu::always_inline]] HWY_INLINE exact_k15_b8_windows<p_uint32_simd_tag>
extract_exact_k15_b8_windows(
    p_uint32_simd_tag d32,
    p_uint64_simd_tag d64,
    std::uint64_t packed22) noexcept
{
    static_assert(std::is_same_v<hn::TFromD<p_uint32_simd_tag>, std::uint32_t>);
    static_assert(std::is_same_v<hn::TFromD<p_uint64_simd_tag>, std::uint64_t>);
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
    const hn::Half<p_uint32_simd_tag> d32_half;
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
template<class p_minimizer_type, class p_continuation_state, class p_collector_type>
[[nodiscard]] inline two_stack_continuation_status
process_exact_k15_w10_b8_candidate_block(
    p_continuation_state& state,
    p_collector_type& collector,
    std::size_t active_candidates)
{
    static_assert(std::is_same_v<typename p_continuation_state::alphabet_type,
        seq::dna4>);
    static_assert(exact_k15_w10_b8_representation_v<p_minimizer_type>);
    using uint32_simd_tag_type = hn::ScalableTag<std::uint32_t>;
    using uint64_simd_tag_type = hn::ScalableTag<std::uint64_t>;
    const uint32_simd_tag_type d32;
    const uint64_simd_tag_type d64;
    constexpr std::size_t kmer = 15;
    constexpr std::size_t block = 8;
    constexpr std::size_t lanes = 4;
    constexpr std::uint32_t kmer_mask = (std::uint32_t{1} << 30) - 1;
    const std::uint32_t maximum_hash = static_cast<std::uint32_t>(
        p_minimizer_type::maximum().kmer());
    const std::uint64_t maximum_raw = p_minimizer_type::maximum().raw_value();
    const std::uint64_t ordinal_sentinel =
        (std::numeric_limits<std::uint64_t>::max)();

    if (active_candidates == 0 || active_candidates > block
        || state.code_tail_size != kmer - 1
        || state.pending_count < active_candidates
        || hn::Lanes(d32) != lanes || hn::Lanes(d64) != 2) {
        return reject_continuation(
            state, two_stack_continuation_status::invalid_state);
    }
    if (state.candidate_count
            > (std::numeric_limits<std::uint64_t>::max)() - block
        || state.emitted_position_base
            > (std::numeric_limits<std::uint64_t>::max)() - (kmer - 1)
        || state.candidate_count + (block - 1)
            > (std::numeric_limits<std::uint64_t>::max)()
                - state.emitted_position_base - (kmer - 1)) {
        return reject_continuation(
            state, two_stack_continuation_status::arithmetic_overflow);
    }

    // Two zero padding bytes make all three SWAR staging loads in-bounds.
    HWY_ALIGN std::array<std::uint8_t, 24> code_scratch{};
    std::memcpy(code_scratch.data(), state.code_tail.data(), kmer - 1);
    std::memcpy(code_scratch.data() + kmer - 1,
        state.pending_codes.data(), active_candidates);

    const auto natural = extract_exact_k15_b8_windows(
        d32, d64, pack_k15_b8_dna4_codes_swar(code_scratch.data()));
    std::array<hn::Vec<uint32_simd_tag_type>, 2> forward{
        reverse_two_bit_groups(d32, natural.lower),
        reverse_two_bit_groups(d32, natural.upper)};
    std::array<hn::Vec<uint32_simd_tag_type>, 2> reverse{
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
            uint32_simd_tag_type>(d32, canonical, hn::Set(d32, kmer_mask));
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
            hn::ShiftLeft<p_minimizer_type::k_hash_shift>(hash_lower),
            hn::Or(
                hn::ShiftLeft<p_minimizer_type::k_position_shift>(hn::And(
                    position_lower,
                    hn::Set(d64, static_cast<std::uint64_t>(
                        p_minimizer_type::k_position_field_mask)))),
                hn::IfThenElse(strand_halves.lower, one, zero)));
        auto raw_upper = hn::Or(
            hn::ShiftLeft<p_minimizer_type::k_hash_shift>(hash_upper),
            hn::Or(
                hn::ShiftLeft<p_minimizer_type::k_position_shift>(hn::And(
                    position_upper,
                    hn::Set(d64, static_cast<std::uint64_t>(
                        p_minimizer_type::k_position_field_mask)))),
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
        add_stats_counter(
            state.stats.palindrome_lanes,
            hn::CountTrue(d32, hn::And(active, palindrome)));
    }

    const std::size_t first_group = std::min(lanes, active_candidates);
    auto status = process_exact_k15_w10_u32_group<p_minimizer_type>(
        state,
        collector,
        candidate_hash.data(),
        candidate_raw.data(),
        candidate_ordinal.data(),
        first_group);
    if (status != two_stack_continuation_status::ok) return status;
    if (active_candidates > lanes) {
        status = process_exact_k15_w10_u32_group<p_minimizer_type>(
            state,
            collector,
            candidate_hash.data() + lanes,
            candidate_raw.data() + lanes,
            candidate_ordinal.data() + lanes,
            active_candidates - lanes);
        if (status != two_stack_continuation_status::ok) return status;
    }

    std::memcpy(state.code_tail.data(),
        code_scratch.data() + active_candidates, kmer - 1);
    const std::uint64_t packed_groups = active_candidates > lanes ? 2u : 1u;
    add_stats_counter(
        state.stats.kmer_15_window_10_byte_8_packed_22_groups, packed_groups);
    add_stats_counter(
        state.stats.kmer_15_window_10_byte_8_packed_22_lane_visits, active_candidates);
    add_stats_counter(state.stats.scalar_orchestration_steps, 7);
    return two_stack_continuation_status::ok;
}

template<class p_simd_tag>
struct canonical_kmer_vectors {
    hn::Vec<p_simd_tag> forward;
    hn::Vec<p_simd_tag> reverse;
};

template<class p_simd_tag>
[[nodiscard]] HWY_INLINE canonical_kmer_vectors<p_simd_tag> incorporate_kmer_codes(
    p_simd_tag d,
    canonical_kmer_vectors<p_simd_tag> current,
    hn::Vec<p_simd_tag> codes,
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

template<class p_simd_tag>
struct minimizer_candidate_vectors {
    hn::Vec<p_simd_tag> hash;
    hn::Vec<p_simd_tag> raw;
    hn::Vec<p_simd_tag> ordinal;
};

template<class p_simd_tag>
[[nodiscard]] HWY_INLINE minimizer_candidate_vectors<p_simd_tag> reduce_window_slot(
    p_simd_tag d,
    minimizer_candidate_vectors<p_simd_tag> minimum,
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
    bool p_invalid_only,
    std::size_t p_window_size,
    std::size_t p_kmer_size,
    class p_minimizer_type,
    class p_continuation_state,
    class p_collector_type>
[[nodiscard, gnu::always_inline]] inline two_stack_continuation_status
process_chronological_candidate_block(
    p_continuation_state& state,
    p_collector_type& collector,
    std::size_t active_lanes)
{
    using storage_type = typename p_minimizer_type::storage_type;
    static_assert(std::is_same_v<storage_type, std::uint64_t>);
    static_assert(std::is_trivially_copyable_v<p_minimizer_type>);
    static_assert(sizeof(p_minimizer_type) == sizeof(storage_type));
    static_assert(requires (
        p_collector_type& output,
        const storage_type* raw,
        std::size_t count) {
            { output.append_raw_storage(raw, count) } -> std::same_as<bool>;
        });

    if (state.status != two_stack_continuation_status::ok) return state.status;

    using simd_tag_type = hn::ScalableTag<std::uint64_t>;
    using vector_type = hn::Vec<simd_tag_type>;
    using mask_type = hn::Mask<simd_tag_type>;
    simd_tag_type d;
    const std::size_t lanes = hn::Lanes(d);
    const auto target_status = validate_exact_vector_shape(state,
                                                           lanes,
                                                           p_continuation_state::max_vector_lanes,
                                                           exact_compiled_target_is_scalar,
                                                           u64_lane_transpose_shape_supported(d));
    if (target_status != two_stack_continuation_status::ok) {
        return target_status;
    }

    const std::size_t w = state.window_size();
    const std::size_t k = state.kmer_size();
    if (w == 0 || w > k_max_two_stack_window) {
        return reject_continuation(
            state, two_stack_continuation_status::invalid_window_size);
    }
    if (k == 0 || k > k_max_exact_two_stack_kmer) {
        return reject_continuation(
            state, two_stack_continuation_status::invalid_kmer_size);
    }
    if (active_lanes == 0 || active_lanes > lanes
        || (!p_invalid_only && (state.code_tail_size != k - 1
            || state.pending_count < active_lanes))) {
        return reject_continuation(
            state, two_stack_continuation_status::invalid_range);
    }

    constexpr std::size_t max_lanes = p_continuation_state::max_vector_lanes;
    constexpr std::size_t window_capacity =
        p_window_size != 0 && p_window_size <= k_max_two_stack_window ? p_window_size : k_max_two_stack_window;
    constexpr std::size_t combined_capacity =
        window_capacity - 1 + max_lanes;
    constexpr std::size_t event_capacity =
        max_lanes * (window_capacity + max_lanes);
    constexpr std::uint64_t ordinal_sentinel =
        std::numeric_limits<std::uint64_t>::max();

    const std::uint64_t max_hash = p_minimizer_type::maximum().kmer();
    const std::uint64_t max_raw = p_minimizer_type::maximum().raw_value();
    const std::uint64_t raw_hash_mask = p_minimizer_type::k_hash_mask;
    const std::uint64_t kmer_mask =
        (std::uint64_t{1} << (2 * k)) - 1;

    if (state.candidate_count
            > std::numeric_limits<std::uint64_t>::max()
                - static_cast<std::uint64_t>(active_lanes)) {
        return reject_continuation(
            state, two_stack_continuation_status::arithmetic_overflow);
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
        add_stats_counter(state.stats.scalar_orchestration_steps, 1);
    }

    const mask_type active_mask = hn::FirstN(d, active_lanes);
    const vector_type v_max_hash = hn::Set(d, max_hash);
    const vector_type ordinal_unmasked = hn::Add(
        hn::Set(d, static_cast<std::uint64_t>(state.candidate_count)),
        lane_indices(d));
    const vector_type candidate_ordinal = hn::IfThenElse(
        active_mask, ordinal_unmasked, hn::Set(d, ordinal_sentinel));
    vector_type candidate_hash = v_max_hash;
    vector_type candidate_raw = hn::Set(d, max_raw);
    mask_type palindrome_mask = hn::MaskFalse(d);
    HWY_ALIGN std::array<std::uint8_t,
        k_max_exact_two_stack_kmer - 1 + max_lanes> code_scratch{};
    if constexpr (!p_invalid_only) {
        std::memcpy(
            code_scratch.data(), state.code_tail.data(), k - 1);
        std::memcpy(
            code_scratch.data() + k - 1,
            state.pending_codes.data(), active_lanes);
        add_stats_counter(state.stats.scalar_orchestration_steps, 2);

        canonical_kmer_vectors<simd_tag_type> kmer_vectors{
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
        add_stats_counter(state.stats.scalar_orchestration_steps, k);

        const vector_type v_kmer_mask = hn::Set(d, kmer_mask);
        kmer_vectors.forward = hn::And(kmer_vectors.forward, v_kmer_mask);
        kmer_vectors.reverse = hn::And(kmer_vectors.reverse, v_kmer_mask);
        palindrome_mask = hn::Eq(
            kmer_vectors.forward, kmer_vectors.reverse);
        const mask_type live_non_palindrome =
            hn::AndNot(palindrome_mask, active_mask);
        const vector_type scrambled =
            minimap_int_hasher<seq::dna4>::template scramble_simd<simd_tag_type>(
                d,
                hn::Min(kmer_vectors.forward, kmer_vectors.reverse),
                v_kmer_mask);
        candidate_hash = hn::And(
            scrambled,
            hn::Set(d, static_cast<std::uint64_t>(
                p_minimizer_type::k_hash_field_mask)));
        candidate_hash = hn::IfThenElse(
            live_non_palindrome, candidate_hash, v_max_hash);

        vector_type output_position = hn::Zero(d);
        if constexpr (std::same_as<
                typename p_continuation_state::alphabet_type, seq::dna4>) {
            if (state.emitted_position_base
                > (std::numeric_limits<std::uint64_t>::max)() - (k - 1)) {
                return reject_continuation(
                    state, two_stack_continuation_status::arithmetic_overflow);
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
                    state, two_stack_continuation_status::arithmetic_overflow);
            }
            output_position = hn::Add(
                    ordinal_unmasked,
                    hn::Set(d, output_position_base));
        } else {
            output_position = hn::LoadU(
                d, state.pending_positions.data());
        }
        const vector_type strand = hn::IfThenElse(
            hn::Lt(kmer_vectors.reverse, kmer_vectors.forward),
            hn::Set(d, std::uint64_t{1}),
            hn::Zero(d));
        candidate_raw = hn::Or(
            hn::ShiftLeftSame(
                candidate_hash, static_cast<int>(p_minimizer_type::k_hash_shift)),
            hn::Or(
                hn::ShiftLeftSame(
                    hn::And(
                        output_position,
                        hn::Set(d, static_cast<std::uint64_t>(
                            p_minimizer_type::k_position_field_mask))),
                    static_cast<int>(p_minimizer_type::k_position_shift)),
                strand));
        candidate_raw = hn::IfThenElse(
            live_non_palindrome, candidate_raw, hn::Set(d, max_raw));
    }

    // Every element subsequently read is covered by the tail copies or the
    // full-width vector stores.  p_value_type-initializing these arrays generated
    // three hot-loop bzero calls (3 * 2056 bytes on the runtime p_window_size=256 route).
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
    add_stats_counter(state.stats.scalar_orchestration_steps, 3);

    minimizer_candidate_vectors<simd_tag_type> window_minimum{
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
    add_stats_counter(state.stats.scalar_orchestration_steps, w);

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
    const vector_type previous_hash = hn::LoadU(d, previous_hash_storage.data());
    const vector_type previous_raw = hn::LoadU(d, previous_raw_storage.data());
    const vector_type previous_ordinal =
        hn::LoadU(d, previous_ordinal_storage.data());
    add_stats_counter(state.stats.scalar_orchestration_steps, 3);

    const mask_type previous_valid = hn::Ne(previous_hash, v_max_hash);
    mask_type first_full_window = hn::MaskFalse(d);
    mask_type steady_window = hn::MaskFalse(d);
    if constexpr (std::same_as<
            typename p_continuation_state::alphabet_type, seq::dna4>) {
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
        const vector_type valid_run_lengths = hn::LoadU(
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
    const mask_type entering_wins = hn::Le(candidate_hash, previous_hash);
    const mask_type expired = hn::And(
        steady_window,
        hn::AndNot(
            entering_wins,
            hn::Eq(
                previous_ordinal,
                hn::Sub(
                    ordinal_unmasked,
                    hn::Set(d, static_cast<std::uint64_t>(w))))));
    const mask_type emit_old = hn::And(
        steady_window,
        hn::And(previous_valid, hn::Or(entering_wins, expired)));
    const mask_type special_ties = hn::And(first_full_window, previous_valid);
    const mask_type expiry_ties = hn::And(
        expired, hn::Ne(window_minimum.hash, v_max_hash));

    const std::size_t event_stride = w + 1;
    const std::size_t row_stride =
        ((event_stride + lanes - 1) / lanes) * lanes;
    // The slow tie path overwrites every row/column slot it reads.  Avoid a
    // fourth per-block zero-fill of the maximum-capacity matrix.
    HWY_ALIGN std::array<std::uint64_t, event_capacity> event_raw;
    HWY_ALIGN std::array<vector_type, max_lanes> event_columns{};
    std::uint64_t first_window_tie_events = 0;
    std::uint64_t expiry_tie_events = 0;
    const vector_type v_max_raw = hn::Set(d, max_raw);

    HWY_ALIGN std::array<std::uint64_t, event_capacity + max_lanes>
        compressed_raw;
    std::size_t compressed_count = 0;
    const vector_type v_raw_hash_mask = hn::Set(d, raw_hash_mask);

    // Equal packed minimizer hashes are rare (the XXL production layout has a
    // 30-bit hash field), so prove their absence before constructing the
    // padded row-major tie-event matrix.  The
    // direct CompressStore is already in chronological lane order and emits
    // exactly the event-0 records the general path would retain.
    const mask_type possible_tie_lanes = hn::Or(special_ties, expiry_ties);
    mask_type actual_tie_lanes = hn::MaskFalse(d);
    if (hn::CountTrue(d, possible_tie_lanes) != 0) [[unlikely]] {
        for (std::size_t candidate_slot = 0;
             candidate_slot < w;
             ++candidate_slot) {
            const vector_type slot_hash =
                hn::LoadU(d, combined_hash.data() + candidate_slot);
            const vector_type slot_raw =
                hn::LoadU(d, combined_raw.data() + candidate_slot);
            const mask_type special_match = hn::And(
                special_ties,
                hn::And(
                    hn::Eq(slot_hash, previous_hash),
                    hn::Ne(slot_raw, previous_raw)));
            const mask_type expiry_match = hn::And(
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
        add_stats_counter(state.stats.stable_compress_batches, 1);
        add_stats_counter(state.stats.direct_event_compress_batches, 1);
        add_stats_counter(state.stats.scalar_orchestration_steps, 1);
    } else {
        add_stats_counter(state.stats.tie_matrix_event_batches, 1);
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
                    const vector_type slot_hash =
                        hn::LoadU(d, combined_hash.data() + candidate_slot);
                    const vector_type slot_raw =
                        hn::LoadU(d, combined_raw.data() + candidate_slot);
                    const mask_type special_match = hn::And(
                        special_ties,
                        hn::And(
                            hn::Eq(slot_hash, previous_hash),
                            hn::Ne(slot_raw, previous_raw)));
                    const mask_type expiry_match = hn::And(
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
                    two_stack_continuation_status::unsupported_vector_shape);
            }
            for (std::size_t row = 0; row < lanes; ++row) {
                hn::StoreU(
                    event_columns[row],
                    d,
                    event_raw.data() + row * row_stride + column_block);
            }
            add_stats_counter(state.stats.row_transpose_batches, 1);
            add_stats_counter(state.stats.row_vector_stores, lanes);
            add_stats_counter(
                state.stats.scalar_orchestration_steps, 2 * lanes);
        }

        const std::size_t active_event_slots = active_lanes * row_stride;
        for (std::size_t offset = 0;
             offset < active_event_slots;
             offset += lanes) {
            const vector_type events = hn::LoadU(d, event_raw.data() + offset);
            const mask_type valid_events = hn::Ne(
                hn::And(events, v_raw_hash_mask), v_raw_hash_mask);
            compressed_count += hn::CompressStore(
                events,
                valid_events,
                d,
                compressed_raw.data() + compressed_count);
            add_stats_counter(state.stats.stable_compress_batches, 1);
            add_stats_counter(state.stats.scalar_orchestration_steps, 1);
        }
    }
    if (compressed_count != 0
        && !collector.append_raw_storage(
            compressed_raw.data(), compressed_count)) {
        return reject_continuation(
            state, two_stack_continuation_status::collector_overflow);
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
    if constexpr (!p_invalid_only) {
        std::memcpy(
            state.code_tail.data(),
            code_scratch.data() + active_lanes,
            k - 1);
        add_stats_counter(state.stats.scalar_orchestration_steps, 4);
    } else {
        add_stats_counter(state.stats.scalar_orchestration_steps, 3);
    }

    state.candidate_count += active_lanes;
    add_stats_counter(state.stats.minimizers_emitted, compressed_count);
    add_stats_counter(state.stats.vector_batches, 1);
    add_stats_counter(state.stats.vector_lanes_visited, active_lanes);
    add_stats_counter(state.stats.generic_u64_candidate_blocks, 1);
    add_stats_counter(state.stats.generic_u64_lane_visits, active_lanes);
    add_stats_counter(state.stats.masked_tail_lanes, lanes - active_lanes);
    if constexpr (!p_invalid_only) {
        add_stats_counter(state.stats.consecutive_kmer_batches, 1);
        add_stats_counter(state.stats.canonical_scramble_batches, 1);
    }
    add_stats_counter(state.stats.window_reduction_batches, 1);
    add_stats_counter(state.stats.event_generation_batches, 1);
    add_stats_counter(
        state.stats.palindrome_lanes,
        hn::CountTrue(d, hn::And(palindrome_mask, active_mask)));
    add_stats_counter(
        state.stats.first_window_tie_events, first_window_tie_events);
    add_stats_counter(state.stats.expiry_tie_events, expiry_tie_events);
    add_stats_counter(
        state.stats.entering_min_transitions,
        hn::CountTrue(d, hn::And(steady_window, entering_wins)));
    add_stats_counter(
        state.stats.expiry_rescan_transitions,
        hn::CountTrue(d, expired));
    return two_stack_continuation_status::ok;
}

/** Advance one or more max/invalid entries through the SIMD minimizer ring. */
template<
    std::size_t p_window_size,
    std::size_t p_kmer_size,
    class p_minimizer_type,
    class p_continuation_state,
    class p_collector_type>
[[nodiscard]] inline two_stack_continuation_status advance_invalid_slots(
    p_continuation_state& state,
    p_collector_type& collector,
    std::span<const std::uint64_t> valid_run_lengths)
{
    if (valid_run_lengths.empty()
        || valid_run_lengths.size() > state.pending_valid_run_lengths.size()) {
        return reject_continuation(
            state, two_stack_continuation_status::invalid_range);
    }
    std::memmove(
        state.pending_valid_run_lengths.data(),
        valid_run_lengths.data(),
        valid_run_lengths.size() * sizeof(std::uint64_t));
    return process_chronological_candidate_block<true, p_window_size, p_kmer_size, p_minimizer_type>(
        state, collector, valid_run_lengths.size());
}

/** Append one ambiguity-free nt4 run to the persistent SIMD k-mer stream. */
template<
    std::size_t p_window_size,
    std::size_t p_kmer_size,
    class p_minimizer_type,
    class p_continuation_state,
    class p_collector_type>
[[nodiscard]] inline two_stack_continuation_status append_valid_nt4_codes(
    p_continuation_state& state,
    p_collector_type& collector,
    std::span<const std::uint8_t> codes,
    std::size_t first_input_position)
{
    using simd_tag_type = hn::ScalableTag<std::uint64_t>;
    const std::size_t lanes = hn::Lanes(simd_tag_type{});
    const std::size_t k = state.kmer_size();
    if (!codes.empty()
        && first_input_position
            > (std::numeric_limits<std::size_t>::max)()
                - (codes.size() - 1)) {
        return reject_continuation(
            state, two_stack_continuation_status::arithmetic_overflow);
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
                state, two_stack_continuation_status::arithmetic_overflow);
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
                        state, two_stack_continuation_status::arithmetic_overflow);
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
            const auto status = advance_invalid_slots<p_window_size, p_kmer_size, p_minimizer_type>(
                state,
                collector,
                std::span<const std::uint64_t>{
                    state.pending_valid_run_lengths.data(), take});
            if (status != two_stack_continuation_status::ok) return status;
        } else {
            state.pending_count += take;
        }
        consumed += take;
        add_stats_counter(state.stats.scalar_orchestration_steps, 1);
        if (state.pending_count == lanes) {
            const auto status =
                process_chronological_candidate_block<false, p_window_size, p_kmer_size, p_minimizer_type>(
                    state, collector, lanes);
            if (status != two_stack_continuation_status::ok) return status;
            state.pending_count = 0;
        }
    }
    return two_stack_continuation_status::ok;
}

/** Emit the final live window minimum without finalizing the continuation. */
template<class p_minimizer_type, class p_continuation_state, class p_collector_type>
[[nodiscard]] inline two_stack_continuation_status flush_current_minimum(
    p_continuation_state& state,
    p_collector_type& collector)
{
    using storage_type = typename p_minimizer_type::storage_type;
    using simd_tag_type = hn::ScalableTag<std::uint64_t>;
    simd_tag_type d;
    const auto current_raw = hn::Set(
        d, static_cast<std::uint64_t>(state.current_min.raw));
    const auto raw_hash_mask = hn::Set(
        d, static_cast<std::uint64_t>(p_minimizer_type::k_hash_mask));
    const auto current_valid = hn::Ne(
        hn::And(current_raw, raw_hash_mask), raw_hash_mask);
    HWY_ALIGN std::array<storage_type, p_continuation_state::max_vector_lanes> final_raw{};
    const std::size_t final_count = hn::CompressStore(
        current_raw,
        hn::And(current_valid, hn::FirstN(d, 1)),
        d,
        final_raw.data());
    if (final_count != 0
        && !collector.append_raw_storage(final_raw.data(), final_count)) {
        return reject_continuation(
            state, two_stack_continuation_status::collector_overflow);
    }
    add_stats_counter(state.stats.minimizers_emitted, final_count);
    add_stats_counter(state.stats.final_flush_records, final_count);
    add_stats_counter(state.stats.event_generation_batches, 1);
    add_stats_counter(state.stats.stable_compress_batches, 1);
    add_stats_counter(state.stats.scalar_orchestration_steps, 1);
    return two_stack_continuation_status::ok;
}

/**
 * Apply minimap2's ambiguous-base transition: l/rolling-kmer reset, one max
 * ring entry, but no window flush, ring clear, ordinal reset, or emit.
 */
template<
    std::size_t p_window_size,
    std::size_t p_kmer_size,
    class p_minimizer_type,
    class p_continuation_state,
    class p_collector_type>
[[nodiscard]] inline two_stack_continuation_status advance_ambiguity(
    p_continuation_state& state,
    p_collector_type& collector)
{
    if (state.code_tail_size == state.kmer_size() - 1
        && state.pending_count != 0) {
        const auto status =
            process_chronological_candidate_block<false, p_window_size, p_kmer_size, p_minimizer_type>(
                state, collector, state.pending_count);
        if (status != two_stack_continuation_status::ok) return status;
        state.pending_count = 0;
    }
    state.valid_run_length = 0;
    state.code_tail_size = 0;
    state.pending_count = 0;
    state.pending_valid_run_lengths[0] = 0;
    const auto advance_status = advance_invalid_slots<p_window_size, p_kmer_size, p_minimizer_type>(
        state,
        collector,
        std::span<const std::uint64_t>{
            state.pending_valid_run_lengths.data(), 1});
    if (advance_status != two_stack_continuation_status::ok) {
        return advance_status;
    }
    add_stats_counter(state.stats.ambiguous_symbols, 1);
    add_stats_counter(state.stats.scalar_orchestration_steps, 1);
    return two_stack_continuation_status::ok;
}

}  // namespace two_stack_internal


// BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN reset_two_stack_nthash
template<std::size_t p_window_size, std::size_t p_kmer_size, class p_alphabet_traits>
[[nodiscard]] inline two_stack_continuation_status reset_two_stack_nthash(
    two_stack_continuation_state<p_window_size, p_kmer_size, p_alphabet_traits>& state,
    std::size_t runtime_w,
    std::size_t runtime_k,
    std::size_t emitted_position_base)
{
    state = {};
    using alphabet_type = typename p_alphabet_traits::alphabet_type;
    using storage_unit = typename p_alphabet_traits::storage_unit;
    if constexpr (!seqan3::nucleotide_alphabet<alphabet_type>) {
        return two_stack_internal::reject_continuation(
            state, two_stack_continuation_status::unsupported_alphabet);
    } else if constexpr (!std::is_integral_v<storage_unit>
        || !(sizeof(storage_unit) == 1 || sizeof(storage_unit) == 2
             || sizeof(storage_unit) == 4 || sizeof(storage_unit) == 8)
        || p_alphabet_traits::bits_per_char == 0
        || p_alphabet_traits::bits_per_char > 8
        || p_alphabet_traits::letters_per_unit
            != sizeof(storage_unit) * 8 / p_alphabet_traits::bits_per_char) {
        return two_stack_internal::reject_continuation(
            state, two_stack_continuation_status::unsupported_storage);
    }
    using simd_tag_type = hn::ScalableTag<std::uint64_t>;
    const simd_tag_type d;
    const std::size_t lanes = hn::Lanes(d);
    const auto target_status = two_stack_internal::validate_exact_vector_shape(
        state,
        lanes,
        two_stack_continuation_state<p_window_size, p_kmer_size, p_alphabet_traits>::
            max_vector_lanes,
        two_stack_internal::exact_compiled_target_is_scalar,
        two_stack_internal::u64_lane_transpose_shape_supported(d));
    if (target_status != two_stack_continuation_status::ok) {
        return target_status;
    }
    if constexpr (p_window_size > k_max_two_stack_window
        || p_kmer_size > k_max_exact_two_stack_kmer) {
        return two_stack_internal::reject_continuation(
            state,
            two_stack_continuation_status::invalid_static_configuration);
    }
    if constexpr (p_window_size != 0) {
        if (runtime_w != p_window_size) {
            return two_stack_internal::reject_continuation(
                state,
                two_stack_continuation_status::invalid_static_configuration);
        }
    }
    if constexpr (p_kmer_size != 0) {
        if (runtime_k != p_kmer_size) {
            return two_stack_internal::reject_continuation(
                state,
                two_stack_continuation_status::invalid_static_configuration);
        }
    }
    if (runtime_w == 0 || runtime_w > k_max_two_stack_window) {
        return two_stack_internal::reject_continuation(
            state, two_stack_continuation_status::invalid_window_size);
    }
    if (runtime_k == 0 || runtime_k > k_max_exact_two_stack_kmer) {
        return two_stack_internal::reject_continuation(
            state, two_stack_continuation_status::invalid_kmer_size);
    }
    state.runtime_w = runtime_w;
    state.runtime_k = runtime_k;
    state.emitted_position_base = emitted_position_base;
    state.kernel_kind = exact_simd_kernel::generic_u64;
    state.stats.generic_u64_kernel_calls = 1;
    state.initialized = true;
    state.status = two_stack_continuation_status::ok;
    return state.status;
}
// BIOSEQ_PRIVATE_PACKED_ACCESS_END reset_two_stack_nthash


// BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN consume_two_stack_nthash_packed_fragment
template<
    std::size_t p_window_size,
    std::size_t p_kmer_size,
    class p_minimizer_type,
    class p_alphabet_traits,
    class p_collector_type>
[[nodiscard]] inline two_stack_continuation_status
consume_two_stack_nthash_packed_fragment(
    two_stack_continuation_state<p_window_size, p_kmer_size, p_alphabet_traits>& state,
    p_collector_type& collector,
    std::span<const typename p_alphabet_traits::storage_unit> packed_fragment,
    std::size_t fragment_bp_size,
    std::size_t fragment_begin_bp,
    std::size_t consume_begin_bp,
    std::size_t consume_end_bp)
{
    using storage_unit = typename p_alphabet_traits::storage_unit;

    if (state.status != two_stack_continuation_status::ok) return state.status;
    if (!state.initialized) {
        return two_stack_internal::reject_continuation(
            state, two_stack_continuation_status::not_initialized);
    }
    if (state.finalized) {
        return two_stack_internal::reject_continuation(
            state, two_stack_continuation_status::already_finalized);
    }
    if constexpr (!seqan3::nucleotide_alphabet<
        typename p_alphabet_traits::alphabet_type>) {
        return two_stack_internal::reject_continuation(
            state, two_stack_continuation_status::unsupported_alphabet);
    } else if constexpr (!std::is_integral_v<storage_unit>
        || !(sizeof(storage_unit) == 1 || sizeof(storage_unit) == 2
             || sizeof(storage_unit) == 4 || sizeof(storage_unit) == 8)
        || p_alphabet_traits::bits_per_char == 0
        || p_alphabet_traits::bits_per_char > 8
        || p_alphabet_traits::letters_per_unit
            != sizeof(storage_unit) * 8 / p_alphabet_traits::bits_per_char) {
        return two_stack_internal::reject_continuation(
            state, two_stack_continuation_status::unsupported_storage);
    } else {
    if (consume_begin_bp < fragment_begin_bp
        || consume_end_bp < consume_begin_bp
        || consume_end_bp - fragment_begin_bp > fragment_bp_size
        || fragment_bp_size
            > packed_fragment.size() * p_alphabet_traits::letters_per_unit) {
        return two_stack_internal::reject_continuation(
            state, two_stack_continuation_status::invalid_range);
    }

    if (!state.saw_first_fragment) {
        state.input_position_base = consume_begin_bp;
        state.next_input_position = consume_begin_bp;
        state.saw_first_fragment = true;
    }
    if (consume_begin_bp != state.next_input_position) {
        return two_stack_internal::reject_continuation(
            state, two_stack_continuation_status::non_contiguous_range);
    }

    const std::size_t k = state.kmer_size();
    if (state.window_size() == 0
        || state.window_size() > k_max_two_stack_window) {
        return two_stack_internal::reject_continuation(
            state, two_stack_continuation_status::invalid_window_size);
    }
    if (k == 0 || k > k_max_exact_two_stack_kmer) {
        return two_stack_internal::reject_continuation(
            state, two_stack_continuation_status::invalid_kmer_size);
    }
    using simd_tag_type = hn::ScalableTag<std::uint64_t>;
    simd_tag_type d;
    const std::size_t lanes = hn::Lanes(d);
    constexpr bool fast_type_eligible =
        !two_stack_internal::exact_compiled_target_is_scalar && p_window_size == 10 &&
        p_kmer_size == 15 && std::same_as<typename p_alphabet_traits::alphabet_type, seq::dna4> &&
        std::same_as<storage_unit, std::uint32_t> &&
        two_stack_internal::exact_k15_w10_b8_representation_v<p_minimizer_type>;
    const std::size_t pending_limit =
        fast_type_eligible &&
                state.kernel_kind == exact_simd_kernel::kmer_15_window_10_byte_8_uint32_hash
            ? two_stack_continuation_state<p_window_size, p_kmer_size, p_alphabet_traits>::
                  fast_block_candidates
            : lanes;
    if (state.code_tail_size > k - 1
        || state.pending_count >= pending_limit) {
        return two_stack_internal::reject_continuation(
            state, two_stack_continuation_status::invalid_state);
    }

    const std::size_t consume_count = consume_end_bp - consume_begin_bp;
    if (consume_count
            > std::numeric_limits<std::uint64_t>::max()
                - state.stats.bp_consumed
        || consume_count
            > std::numeric_limits<std::uint64_t>::max()
                - state.stats.fragment_symbols_staged) {
        return two_stack_internal::reject_continuation(
            state, two_stack_continuation_status::arithmetic_overflow);
    }
    if (consume_count == 0) return two_stack_continuation_status::ok;

    const std::size_t local_begin = consume_begin_bp - fragment_begin_bp;
    const std::size_t local_end = consume_end_bp - fragment_begin_bp;
    const hn::Rebind<std::uint8_t, simd_tag_type> d_u8;

    std::size_t local = local_begin;
    if constexpr (std::same_as<
            typename p_alphabet_traits::alphabet_type, seq::dna4>) {
        if constexpr (fast_type_eligible) {
            if (state.kernel_kind
                == exact_simd_kernel::kmer_15_window_10_byte_8_uint32_hash) {
                using fast_simd_tag_type = hn::ScalableTag<std::uint32_t>;
                const fast_simd_tag_type fast_d;
                const hn::Rebind<std::uint8_t, fast_simd_tag_type> fast_d_u8;
                const std::size_t fast_lanes = hn::Lanes(fast_d);
                constexpr std::size_t fast_block =
                    two_stack_continuation_state<p_window_size, p_kmer_size, p_alphabet_traits>::
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
                            fast_simd_tag_type, p_alphabet_traits>(
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
                    two_stack_internal::add_stats_counter(
                        state.stats.packed_decode_batches, 1);
                    two_stack_internal::add_stats_counter(
                        state.stats.packed_decode_lanes, take);
                    two_stack_internal::add_stats_counter(
                        state.stats.packed_word_boundary_merges,
                        decoded.crossed_word ? 1 : 0);
                    two_stack_internal::add_stats_counter(
                        state.stats.scalar_orchestration_steps, 1);
                    if (state.pending_count == fast_block) {
                        const auto block_status =
                            two_stack_internal::
                                process_exact_k15_w10_b8_candidate_block<
                                    p_minimizer_type>(
                                        state, collector, fast_block);
                        if (block_status != two_stack_continuation_status::ok) {
                            return block_status;
                        }
                        state.pending_count = 0;
                    }
                }
                state.next_input_position = consume_end_bp;
                two_stack_internal::add_stats_counter(
                    state.stats.bp_consumed, consume_count);
                two_stack_internal::add_stats_counter(
                    state.stats.fragment_symbols_staged, consume_count);
                return two_stack_continuation_status::ok;
            }
        }
        // Keep the established dna4 decode/staging loop byte-for-byte hot:
        // dna4 has no ambiguity sentinel, so no run-boundary orchestration is
        // needed and its resident_vector_stream-relative performance must not regress.
        while (local < local_end) {
            const bool filling_code_tail = state.code_tail_size < k - 1;
            const std::size_t available_capacity = filling_code_tail
                ? (k - 1 - state.code_tail_size)
                : (lanes - state.pending_count);
            const std::size_t take = std::min(
                lanes, std::min(available_capacity, local_end - local));
            const auto decoded =
                two_stack_internal::decode_packed_dna4_rank_vector<
                    simd_tag_type, p_alphabet_traits>(
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
            two_stack_internal::add_stats_counter(
                state.stats.packed_decode_batches, 1);
            two_stack_internal::add_stats_counter(
                state.stats.packed_decode_lanes, take);
            two_stack_internal::add_stats_counter(
                state.stats.packed_word_boundary_merges,
                decoded.crossed_word ? 1 : 0);
            two_stack_internal::add_stats_counter(
                state.stats.scalar_orchestration_steps, 1);
            if (state.pending_count == lanes) {
                const auto block_status =
                    two_stack_internal::process_chronological_candidate_block<
                    false, p_window_size, p_kmer_size, p_minimizer_type>(state, collector, lanes);
                if (block_status != two_stack_continuation_status::ok) {
                    return block_status;
                }
                state.pending_count = 0;
            }
        }
    } else {
        constexpr std::size_t max_lanes =
            two_stack_continuation_state<p_window_size, p_kmer_size, p_alphabet_traits>::
                max_vector_lanes;
        HWY_ALIGN std::array<std::uint8_t, max_lanes> decoded_codes{};
        while (local < local_end) {
            const std::size_t take = std::min(lanes, local_end - local);
            const auto decoded =
                two_stack_internal::decode_packed_nt4_rank_vector<
                    simd_tag_type, p_alphabet_traits>(
                        d, packed_fragment, local, take);
            hn::StoreN(
                decoded.codes, d_u8, decoded_codes.data(), take);
            two_stack_internal::add_stats_counter(
                state.stats.packed_decode_batches, 1);
            two_stack_internal::add_stats_counter(
                state.stats.packed_decode_lanes, take);
            two_stack_internal::add_stats_counter(
                state.stats.packed_word_boundary_merges,
                decoded.crossed_word ? 1 : 0);
            two_stack_internal::add_stats_counter(
                state.stats.scalar_orchestration_steps, 1);

            std::uint64_t ambiguous_lanes = decoded.ambiguous_lanes;
            std::size_t run_begin = 0;
            if (ambiguous_lanes != 0) {
                two_stack_internal::add_stats_counter(
                    state.stats.ambiguous_reset_batches, 1);
            }
            while (ambiguous_lanes != 0) {
                const std::size_t ambiguous_lane = static_cast<std::size_t>(
                    std::countr_zero(ambiguous_lanes));
                const auto append_status =
                    two_stack_internal::append_valid_nt4_codes<
                        p_window_size, p_kmer_size, p_minimizer_type>(
                            state,
                            collector,
                            std::span<const std::uint8_t>{
                                decoded_codes.data() + run_begin,
                                ambiguous_lane - run_begin},
                            fragment_begin_bp + local + run_begin);
                if (append_status != two_stack_continuation_status::ok) {
                    return append_status;
                }
                const auto reset_status =
                    two_stack_internal::advance_ambiguity<
                        p_window_size, p_kmer_size, p_minimizer_type>(
                            state, collector);
                if (reset_status != two_stack_continuation_status::ok) {
                    return reset_status;
                }
                run_begin = ambiguous_lane + 1;
                ambiguous_lanes &= ambiguous_lanes - 1;
            }
            const auto append_status =
                two_stack_internal::append_valid_nt4_codes<
                    p_window_size, p_kmer_size, p_minimizer_type>(
                        state,
                        collector,
                        std::span<const std::uint8_t>{
                            decoded_codes.data() + run_begin,
                            take - run_begin},
                        fragment_begin_bp + local + run_begin);
            if (append_status != two_stack_continuation_status::ok) {
                return append_status;
            }
            local += take;
        }
    }

    state.next_input_position = consume_end_bp;
    two_stack_internal::add_stats_counter(
        state.stats.bp_consumed, consume_count);
    two_stack_internal::add_stats_counter(
        state.stats.fragment_symbols_staged, consume_count);
    return two_stack_continuation_status::ok;
    }
}
// BIOSEQ_PRIVATE_PACKED_ACCESS_END consume_two_stack_nthash_packed_fragment


template<
    std::size_t p_window_size,
    std::size_t p_kmer_size,
    class p_minimizer_type,
    supports_compact_sequence_range p_sequence_range,
    class p_collector_type>
[[nodiscard]] inline two_stack_continuation_status
// BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN consume_two_stack_nthash_fragment
consume_two_stack_nthash_fragment(
    two_stack_continuation_state<
        p_window_size, p_kmer_size, typename compact_sequence_range<p_sequence_range>::traits>& state,
    p_collector_type& collector,
    compact_sequence_range<p_sequence_range>& fragment,
    std::size_t fragment_begin_bp,
    std::size_t consume_begin_bp,
    std::size_t consume_end_bp)
{
    using fragment_type = compact_sequence_range<p_sequence_range>;
    using fragment_traits = typename fragment_type::traits;
    using storage_unit = typename fragment_traits::storage_unit;
    const std::size_t unit_count = static_cast<std::size_t>(fragment.size());
    const storage_unit* packed = unit_count == 0
        ? nullptr
        : fragment.read_cptr(fragment.cbegin());
    return consume_two_stack_nthash_packed_fragment<p_window_size, p_kmer_size, p_minimizer_type>(
        state,
        collector,
        std::span<const storage_unit>{packed, unit_count},
        static_cast<std::size_t>(fragment.bp_size()),
        fragment_begin_bp,
        consume_begin_bp,
        consume_end_bp);
}
// BIOSEQ_PRIVATE_PACKED_ACCESS_END consume_two_stack_nthash_fragment


template<std::size_t p_window_size, std::size_t p_kmer_size, class p_alphabet_traits, class p_collector_type>
[[nodiscard]] inline two_stack_finalize_result finalize_two_stack_nthash(
    two_stack_continuation_state<p_window_size, p_kmer_size, p_alphabet_traits>& state,
    p_collector_type& collector)
{
    using minimizer_type = typename p_collector_type::minimizer_type;
    if (state.status != two_stack_continuation_status::ok) {
        return {.status = state.status, .stats = state.stats};
    }
    if (!state.initialized) {
        const auto status = two_stack_internal::reject_continuation(
            state, two_stack_continuation_status::not_initialized);
        return {.status = status, .stats = state.stats};
    }
    if (state.finalized) {
        const auto status = two_stack_internal::reject_continuation(
            state, two_stack_continuation_status::already_finalized);
        return {.status = status, .stats = state.stats};
    }

    if (state.window_size() == 0
        || state.window_size() > k_max_two_stack_window) {
        const auto status = two_stack_internal::reject_continuation(
            state, two_stack_continuation_status::invalid_window_size);
        return {.status = status, .stats = state.stats};
    }
    if (state.kmer_size() == 0
        || state.kmer_size() > k_max_exact_two_stack_kmer) {
        const auto status = two_stack_internal::reject_continuation(
            state, two_stack_continuation_status::invalid_kmer_size);
        return {.status = status, .stats = state.stats};
    }
    using simd_tag_type = hn::ScalableTag<std::uint64_t>;
    const std::size_t lanes = hn::Lanes(simd_tag_type{});
    constexpr bool fast_type_eligible =
        !two_stack_internal::exact_compiled_target_is_scalar && p_window_size == 10 &&
        p_kmer_size == 15 && std::same_as<typename p_alphabet_traits::alphabet_type, seq::dna4> &&
        std::same_as<typename p_alphabet_traits::storage_unit, std::uint32_t> &&
        two_stack_internal::exact_k15_w10_b8_representation_v<minimizer_type>;
    const std::size_t pending_limit =
        fast_type_eligible &&
                state.kernel_kind == exact_simd_kernel::kmer_15_window_10_byte_8_uint32_hash
            ? two_stack_continuation_state<p_window_size, p_kmer_size, p_alphabet_traits>::
                  fast_block_candidates
            : lanes;
    if (state.code_tail_size > state.kmer_size() - 1
        || state.pending_count >= pending_limit) {
        const auto status = two_stack_internal::reject_continuation(
            state, two_stack_continuation_status::invalid_state);
        return {.status = status, .stats = state.stats};
    }

    if (state.code_tail_size == state.kmer_size() - 1
        && state.pending_count != 0) {
        two_stack_continuation_status block_status;
        if constexpr (fast_type_eligible) {
            if (state.kernel_kind
                == exact_simd_kernel::kmer_15_window_10_byte_8_uint32_hash) {
                block_status = two_stack_internal::
                    process_exact_k15_w10_b8_candidate_block<minimizer_type>(
                        state, collector, state.pending_count);
            } else {
                block_status = two_stack_internal::
                    process_chronological_candidate_block<
                        false, p_window_size, p_kmer_size, minimizer_type>(
                            state, collector, state.pending_count);
            }
        } else {
            block_status = two_stack_internal::
                process_chronological_candidate_block<
                    false, p_window_size, p_kmer_size, minimizer_type>(
                        state, collector, state.pending_count);
        }
        if (block_status != two_stack_continuation_status::ok) {
            return {.status = block_status, .stats = state.stats};
        }
        state.pending_count = 0;
    }

    // End-of-input follows mm_sketch's unconditional final-current flush, even
    // when fewer than p_window_size candidates were seen. Ambiguity boundaries deliberately
    // do not call this helper: they advance a max slot and retain the ring.
    const auto flush_status =
        two_stack_internal::flush_current_minimum<minimizer_type>(
            state, collector);
    if (flush_status != two_stack_continuation_status::ok) {
        return {.status = flush_status, .stats = state.stats};
    }

    state.finalized = true;
    return {
        .status = two_stack_continuation_status::ok,
        .stats = state.stats,
    };
}


/**
 * One checked two_stack_nthash driver for resident, scanner-fragmented, and bounded
 * unit ranges. vulkan_runtime (`p_window_size=p_kmer_size=0`) and constexpr routes share the same
 * reset/packed-consume/finalize implementation. Positions are relative to
 * `emitted_position_base`, matching scalar range collection.
 */
template<
    std::size_t p_window_size,
    std::size_t p_kmer_size,
    class p_minimizer_type,
    class p_sequence,
    class p_collector_type>
[[nodiscard]] inline two_stack_finalize_result
// BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN collect_minimizers_two_stack_nthash_exact_range_resident_impl_checked
collect_minimizers_two_stack_nthash_exact_range_resident_impl_checked(
    p_collector_type& collector,
    p_sequence& sequence,
    std::size_t lo_unit,
    std::size_t hi_unit,
    std::size_t runtime_w,
    std::size_t runtime_k,
    std::size_t emitted_position_base = 0)
{
    using sequence_type = std::remove_cvref_t<p_sequence>;
    using traits = typename sequence_type::traits;
    using storage_unit = typename traits::storage_unit;

    auto rejected = [](two_stack_continuation_status status) {
        two_stack_execution_stats stats{};
        stats.fail_closed_rejections = 1;
        return two_stack_finalize_result{.status = status, .stats = stats};
    };

    if constexpr (!exact_simd_minimizer_representation<p_minimizer_type>) {
        return rejected(
            two_stack_continuation_status::unsupported_representation);
    } else if constexpr (!seqan3::nucleotide_alphabet<
            typename traits::alphabet_type>) {
        return rejected(two_stack_continuation_status::unsupported_alphabet);
    } else if constexpr (!std::is_integral_v<storage_unit>
        || !(sizeof(storage_unit) == 1 || sizeof(storage_unit) == 2
             || sizeof(storage_unit) == 4 || sizeof(storage_unit) == 8)
        || traits::bits_per_char == 0
        || traits::bits_per_char > 8
        || traits::letters_per_unit
            != sizeof(storage_unit) * 8 / traits::bits_per_char) {
        return rejected(two_stack_continuation_status::unsupported_storage);
    } else {
        const std::size_t total_units = static_cast<std::size_t>(sequence.size());
        const std::size_t total_bp = static_cast<std::size_t>(sequence.bp_size());
        constexpr std::size_t lpu = traits::letters_per_unit;
        if (lo_unit > hi_unit || hi_unit > total_units
            || lo_unit > (std::numeric_limits<std::size_t>::max)() / lpu
            || hi_unit > (std::numeric_limits<std::size_t>::max)() / lpu) {
            return rejected(two_stack_continuation_status::invalid_range);
        }
        const std::size_t consume_begin_bp = lo_unit * lpu;
        const std::size_t consume_end_bp = std::min(hi_unit * lpu, total_bp);
        if (consume_begin_bp > consume_end_bp) {
            return rejected(two_stack_continuation_status::invalid_range);
        }

        // Time the selected two_stack_nthash kernel itself. This scope deliberately
        // lives inside the admitted execution branch: rejected shapes do not
        // manufacture a visit, and the pipeline does not wrap this call in a
        // detached/label-only `mm_sketch_simd` scope.
        static const auto* exact_simd_region_desc =
            bioseq::instrumentation::register_static_region("mm_sketch_simd");
        auto exact_simd_region_scope =
            bioseq::instrumentation::region(exact_simd_region_desc);

        two_stack_continuation_state<p_window_size, p_kmer_size, traits> state;
        const auto reset_status = reset_two_stack_nthash(
            state, runtime_w, runtime_k, emitted_position_base);
        if (reset_status != two_stack_continuation_status::ok) {
            return {.status = reset_status, .stats = state.stats};
        }
        if constexpr (p_window_size == 10 && p_kmer_size == 15) {
            state.kernel_kind =
                two_stack_internal::select_exact_simd_kernel_kind<
                    p_minimizer_type, traits>(consume_end_bp - consume_begin_bp);
            if (state.kernel_kind
                == exact_simd_kernel::kmer_15_window_10_byte_8_uint32_hash) {
                state.stats.generic_u64_kernel_calls = 0;
                state.stats.kmer_15_window_10_byte_8_uint32_hash_kernel_calls = 1;
            }
        }

        auto consume_resident = [&state,
                                 &collector,
                                 &consume_begin_bp,
                                 &consume_end_bp]<class p_resident_range>(
                                    const p_resident_range& resident) {
            const std::size_t unit_count =
                static_cast<std::size_t>(resident.size());
            const storage_unit* packed = unit_count == 0
                ? nullptr
                : resident.read_cptr(resident.cbegin());
            return consume_two_stack_nthash_packed_fragment<
                p_window_size, p_kmer_size, p_minimizer_type>(
                    state,
                    collector,
                    std::span<const storage_unit>{packed, unit_count},
                    static_cast<std::size_t>(resident.bp_size()),
                    0,
                    consume_begin_bp,
                    consume_end_bp);
        };

        const two_stack_continuation_status consume_status =
            consume_resident(sequence);
        if (consume_status != two_stack_continuation_status::ok) {
            return {.status = consume_status, .stats = state.stats};
        }
        return finalize_two_stack_nthash(state, collector);
    }
}
// BIOSEQ_PRIVATE_PACKED_ACCESS_END collect_minimizers_two_stack_nthash_exact_range_resident_impl_checked

template<
    std::size_t p_window_size,
    std::size_t p_kmer_size,
    class p_minimizer_type,
    class p_sequence,
    class p_collector_type>
[[nodiscard]] inline two_stack_execution_stats
collect_minimizers_two_stack_nthash_exact_range_resident_impl_or_throw(
    p_collector_type& collector,
    p_sequence& sequence,
    std::size_t lo_unit,
    std::size_t hi_unit,
    std::size_t runtime_w,
    std::size_t runtime_k,
    std::size_t emitted_position_base = 0,
    std::string_view operation = "minimizer sketch")
{
    // The public pipeline accepts runtime w/k, but the overwhelmingly common
    // minimap-compatible shape must enter a genuinely static implementation.
    // Besides enabling loop folding, this keeps p_window_size=10 scratch arrays out of the
    // dynamic p_window_size=256 frame.  Other shapes retain the checked generic route.
    two_stack_finalize_result result;
    if constexpr (p_window_size == 0 && p_kmer_size == 0) {
        if (runtime_w == 10 && runtime_k == 15) [[likely]] {
            result = collect_minimizers_two_stack_nthash_exact_range_resident_impl_checked<
                10, 15, p_minimizer_type>(
                    collector,
                    sequence,
                    lo_unit,
                    hi_unit,
                    runtime_w,
                    runtime_k,
                    emitted_position_base);
        } else {
            result = collect_minimizers_two_stack_nthash_exact_range_resident_impl_checked<
                0, 0, p_minimizer_type>(
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
            p_window_size, p_kmer_size, p_minimizer_type>(
                collector,
                sequence,
                lo_unit,
                hi_unit,
                runtime_w,
                runtime_k,
                emitted_position_base);
    }
    if (!result.is_ok()) {
        throw exact_simd_runtime_error{result.status, operation};
    }
    return result.stats;
}

/**
 * Canonical two_stack_nthash owner. Coordinates are biological positions in the
 * facade, never packed-unit offsets. A compatible resident is consumed
 * directly. Nonresident input fails closed: the caller must retain an
 * admitted_packed_sequence through this synchronous kernel invocation.
 */
template<
    std::size_t p_window_size,
    std::size_t p_kmer_size,
    class p_minimizer_type,
    sequence_alphabet p_alphabet,
    class p_collector_type>
[[nodiscard]] inline two_stack_finalize_result
collect_minimizers_two_stack_nthash_exact_range_checked(
    p_collector_type& collector,
    bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
    std::size_t lo_bp,
    std::size_t hi_bp,
    std::size_t runtime_w,
    std::size_t runtime_k,
    std::size_t emitted_position_base = 0)
{
    using packed_instance = sequence_custom_alphabet<
        p_alphabet, std::uint32_t>;
    using packed_range = compact_sequence_range<packed_instance>;
    using traits = typename packed_range::traits;
    using storage_unit = typename traits::storage_unit;

    auto rejected = [](two_stack_continuation_status status) {
        two_stack_execution_stats stats{};
        stats.fail_closed_rejections = 1;
        return two_stack_finalize_result{.status = status, .stats = stats};
    };

    if constexpr (!exact_simd_minimizer_representation<p_minimizer_type>) {
        return rejected(
            two_stack_continuation_status::unsupported_representation);
    } else if constexpr (!seqan3::nucleotide_alphabet<p_alphabet>) {
        return rejected(two_stack_continuation_status::unsupported_alphabet);
    } else {
        const bioseq::storage::sequence_size_t total_bp_64 =
            sequence.bp_size();
        if (total_bp_64
                > static_cast<bioseq::storage::sequence_size_t>(
                    (std::numeric_limits<std::size_t>::max)())
            || lo_bp > hi_bp
            || hi_bp > static_cast<std::size_t>(total_bp_64)) {
            return rejected(two_stack_continuation_status::invalid_range);
        }

        static const auto* exact_simd_region_desc =
            bioseq::instrumentation::register_static_region("mm_sketch_simd");
        auto exact_simd_region_scope =
            bioseq::instrumentation::region(exact_simd_region_desc);

        two_stack_continuation_state<p_window_size, p_kmer_size, traits> state;
        const auto reset_status = reset_two_stack_nthash(
            state, runtime_w, runtime_k, emitted_position_base);
        if (reset_status != two_stack_continuation_status::ok) {
            return {.status = reset_status, .stats = state.stats};
        }
        if constexpr (p_window_size == 10 && p_kmer_size == 15) {
            state.kernel_kind =
                two_stack_internal::select_exact_simd_kernel_kind<
                    p_minimizer_type, traits>(hi_bp - lo_bp);
            if (state.kernel_kind
                    == exact_simd_kernel::kmer_15_window_10_byte_8_uint32_hash) {
                state.stats.generic_u64_kernel_calls = 0;
                state.stats.kmer_15_window_10_byte_8_uint32_hash_kernel_calls = 1;
            }
        }

        two_stack_continuation_status consume_status =
            two_stack_continuation_status::ok;
        if (auto resident = sequence.try_resident_packed(
                static_cast<bioseq::storage::sequence_size_t>(lo_bp),
                static_cast<bioseq::storage::sequence_size_t>(hi_bp),
                static_cast<std::uint8_t>(sizeof(storage_unit)));
            resident.has_value()) {
            const std::size_t leading =
                resident->leading_symbol_offset;
            const std::size_t logical_length = hi_bp - lo_bp;
            consume_status = consume_two_stack_nthash_packed_fragment<
                p_window_size, p_kmer_size, p_minimizer_type>(
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
                two_stack_continuation_status::non_contiguous_range);
        }
        if (consume_status != two_stack_continuation_status::ok) {
            return {.status = consume_status, .stats = state.stats};
        }
        return finalize_two_stack_nthash(state, collector);
    }
}

template<
    std::size_t p_window_size,
    std::size_t p_kmer_size,
    class p_minimizer_type,
    sequence_alphabet p_alphabet,
    class p_collector_type>
[[nodiscard]] inline two_stack_execution_stats
collect_minimizers_two_stack_nthash_exact_range_or_throw(
    p_collector_type& collector,
    bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
    std::size_t lo_bp,
    std::size_t hi_bp,
    std::size_t runtime_w,
    std::size_t runtime_k,
    std::size_t emitted_position_base = 0,
    std::string_view operation = "minimizer sketch")
{
    two_stack_finalize_result result;
    if constexpr (p_window_size == 0 && p_kmer_size == 0) {
        if (runtime_w == 10u && runtime_k == 15u) [[likely]] {
            result = collect_minimizers_two_stack_nthash_exact_range_checked<
                10, 15, p_minimizer_type>(
                    collector, sequence, lo_bp, hi_bp,
                    runtime_w, runtime_k, emitted_position_base);
        } else {
            result = collect_minimizers_two_stack_nthash_exact_range_checked<
                0, 0, p_minimizer_type>(
                    collector, sequence, lo_bp, hi_bp,
                    runtime_w, runtime_k, emitted_position_base);
        }
    } else {
        result = collect_minimizers_two_stack_nthash_exact_range_checked<
            p_window_size, p_kmer_size, p_minimizer_type>(
                collector, sequence, lo_bp, hi_bp,
                runtime_w, runtime_k, emitted_position_base);
    }
    if (!result.is_ok()) {
        throw exact_simd_runtime_error{result.status, operation};
    }
    return result.stats;
}

namespace coarse_two_stack_detail {

[[nodiscard]] constexpr minimizer_kernel_status transport_status_for(
    const two_stack_continuation_status status) noexcept
{
    switch (status) {
        case two_stack_continuation_status::ok:
            return minimizer_kernel_status::ok;
        case two_stack_continuation_status::collector_overflow:
            return minimizer_kernel_status::sink_rejected;
        case two_stack_continuation_status::invalid_range:
        case two_stack_continuation_status::non_contiguous_range:
            return minimizer_kernel_status::invalid_sequence_view;
        case two_stack_continuation_status::not_initialized:
        case two_stack_continuation_status::already_finalized:
        case two_stack_continuation_status::invalid_state:
            return minimizer_kernel_status::invalid_continuation;
        default:
            return minimizer_kernel_status::invalid_configuration;
    }
}

template<class p_minimizer_type,
         sequence_alphabet p_alphabet, class p_storage_unit>
[[nodiscard]] bool run(
    minimizer_kernel_invocation& invocation,
    minimizer_kernel_result& result)
{
    return ::bioseq::minimizers::kernel_detail::
        with_borrowed_compact_range<p_alphabet, p_storage_unit>(
            invocation.sequence,
            [&invocation, &result]<class p_range>(p_range& range) {
                minimizer_sink_collector<p_minimizer_type> collector{
                    invocation.sink};
                two_stack_finalize_result native{};
                if (invocation.window == 10u
                    && invocation.kmer == 15u) {
                    native =
                        collect_minimizers_two_stack_nthash_exact_range_resident_impl_checked<
                            10u, 15u, p_minimizer_type>(
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
                            0u, 0u, p_minimizer_type>(
                                collector,
                                range,
                                0u,
                                static_cast<std::size_t>(range.size()),
                                invocation.window,
                                invocation.kmer,
                                invocation.emitted_position_base);
                }

                result.stats.two_stack = native.stats;
                result.family_status =
                    static_cast<std::uint32_t>(native.status);
                result.status = transport_status_for(native.status);

                const bool flushed = collector.flush();
                result.emitted = collector.committed_size();
                if (!flushed || !collector.is_ok()) {
                    result.status = minimizer_kernel_status::sink_rejected;
                    if (native.status == two_stack_continuation_status::ok) {
                        result.family_status = static_cast<std::uint32_t>(
                            two_stack_continuation_status::collector_overflow);
                    }
                }
            });
}

// BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN run_fragmented_impl
template<std::size_t p_window_size, std::size_t p_kmer_size, class p_minimizer_type,
         sequence_alphabet p_alphabet, class p_storage_unit>
void run_fragmented_impl(
    minimizer_kernel_invocation& invocation,
    minimizer_kernel_result& result)
{
    using sequence_config = sequence_custom_alphabet<p_alphabet, p_storage_unit>;
    using range_type = compact_sequence_range<sequence_config>;
    using traits = typename range_type::traits;
    using state_type = two_stack_continuation_state<p_window_size, p_kmer_size, traits>;
    constexpr std::size_t letters_per_unit = traits::letters_per_unit;

    minimizer_sink_collector<p_minimizer_type> collector{invocation.sink};
    state_type state{};
    two_stack_continuation_status status = reset_two_stack_nthash(
        state,
        invocation.window,
        invocation.kmer,
        invocation.emitted_position_base);

    const auto& source = invocation.fragments;
    if (status == two_stack_continuation_status::ok
        && (source.read == nullptr
            || source.release == nullptr
            || source.fragment_end < source.first_fragment)) {
        status = two_stack_continuation_status::invalid_state;
    }

    for (std::size_t fragment_no = source.first_fragment;
         status == two_stack_continuation_status::ok
            && fragment_no < source.fragment_end;
         ++fragment_no) {
        const auto fragment = source.read(source.state, fragment_no);
        const auto release = [&source, fragment_no]() noexcept {
            source.release(source.state, fragment_no);
        };
        const bool pointer_valid = fragment.storage_units == 0u
            || (fragment.storage != nullptr
                && reinterpret_cast<std::uintptr_t>(fragment.storage)
                    % alignof(p_storage_unit) == 0u);
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
            status = two_stack_continuation_status::invalid_range;
            break;
        }

        try {
            status = consume_two_stack_nthash_packed_fragment<
                p_window_size, p_kmer_size, p_minimizer_type>(
                    state,
                    collector,
                    std::span<const p_storage_unit>{
                        reinterpret_cast<const p_storage_unit*>(fragment.storage),
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

    two_stack_finalize_result native{};
    if (status == two_stack_continuation_status::ok) {
        native = finalize_two_stack_nthash(state, collector);
    } else {
        native.status = status;
        native.stats = state.stats;
    }
    result.stats.two_stack = native.stats;
    result.family_status = static_cast<std::uint32_t>(native.status);
    result.status = transport_status_for(native.status);

    const bool flushed = collector.flush();
    result.emitted = collector.committed_size();
    if (!flushed || !collector.is_ok()) {
        result.status = minimizer_kernel_status::sink_rejected;
        result.family_status = static_cast<std::uint32_t>(
            two_stack_continuation_status::collector_overflow);
    }
}
// BIOSEQ_PRIVATE_PACKED_ACCESS_END run_fragmented_impl

template<class p_minimizer_type,
         sequence_alphabet p_alphabet, class p_storage_unit>
[[nodiscard]] bool run_fragmented(
    minimizer_kernel_invocation& invocation,
    minimizer_kernel_result& result)
{
    if (invocation.window == 10u && invocation.kmer == 15u) {
        run_fragmented_impl<10u, 15u, p_minimizer_type, p_alphabet, p_storage_unit>(
            invocation, result);
    } else {
        run_fragmented_impl<0u, 0u, p_minimizer_type, p_alphabet, p_storage_unit>(
            invocation, result);
    }
    return true;
}

template<class p_minimizer_type>
[[nodiscard]] bool run_resident_vector_stream(
    minimizer_kernel_invocation& invocation,
    minimizer_kernel_result& result)
{
    return ::bioseq::minimizers::kernel_detail::
        with_borrowed_compact_range<seq::dna4, std::uint32_t>(
            invocation.sequence,
            [&invocation, &result]<class p_range>(p_range& range) {
                minimizer_sink_collector<p_minimizer_type> collector{
                    invocation.sink};
                try {
                    result.stats.two_stack =
                        collect_minimizers_two_stack_nthash_resident_or_throw<
                            p_minimizer_type>(
                                collector,
                                range,
                                invocation.window,
                                invocation.kmer);
                    result.family_status = static_cast<std::uint32_t>(
                        two_stack_continuation_status::ok);
                    result.status = minimizer_kernel_status::ok;
                } catch (const exact_simd_runtime_error& error) {
                    result.family_status = static_cast<std::uint32_t>(
                        error.status());
                    result.status = transport_status_for(error.status());
                }

                const bool flushed = collector.flush();
                result.emitted = collector.committed_size();
                if (!flushed || !collector.is_ok()) {
                    result.status = minimizer_kernel_status::sink_rejected;
                    result.family_status = static_cast<std::uint32_t>(
                        two_stack_continuation_status::collector_overflow);
                }
            });
}

template<class p_minimizer_type,
         sequence_alphabet p_alphabet, class p_storage_unit>
[[nodiscard]] bool run_selected_output_layout(
    minimizer_kernel_invocation& invocation,
    minimizer_kernel_result& result)
{
    static_assert(exact_simd_minimizer_representation<p_minimizer_type>);

    if (invocation.output_hash_mask
            != static_cast<std::uint64_t>(p_minimizer_type::k_hash_field_mask)
        || invocation.output_position_mask
            != static_cast<std::uint64_t>(p_minimizer_type::k_position_field_mask)) {
        result.family_status = static_cast<std::uint32_t>(
            two_stack_continuation_status::unsupported_representation);
        result.status = minimizer_kernel_status::invalid_configuration;
        result.stats.two_stack.fail_closed_rejections = 1u;
        return false;
    }

    switch (invocation.two_stack_route) {
        case two_stack_route::chronological_continuation:
            return invocation.fragments.is_present()
                ? run_fragmented<p_minimizer_type, p_alphabet, p_storage_unit>(
                    invocation, result)
                : run<p_minimizer_type, p_alphabet, p_storage_unit>(
                    invocation, result);
        case two_stack_route::resident_vector_stream:
            if constexpr (!std::same_as<p_alphabet, seq::dna4>
                          || !std::same_as<p_storage_unit, std::uint32_t>) {
                result.family_status = static_cast<std::uint32_t>(
                    two_stack_continuation_status::invalid_static_configuration);
                result.status = minimizer_kernel_status::invalid_configuration;
                return false;
            }
            if (invocation.window != 10u
                || invocation.kmer != 15u
                || invocation.emitted_position_base != 0u
                || invocation.fragments.is_present()) {
                result.family_status = static_cast<std::uint32_t>(
                    two_stack_continuation_status::invalid_static_configuration);
                result.status = minimizer_kernel_status::invalid_configuration;
                return false;
            }
            return run_resident_vector_stream<p_minimizer_type>(invocation, result);
    }
    return false;
}

} // namespace coarse_two_stack_detail

/**
 * One fixed-target, one-shot resident two_stack_nthash call. The exact target-local
 * continuation is created, consumed and finalized inside the checked driver;
 * no target-dependent state crosses the neutral ABI.
 */
template<sequence_alphabet p_alphabet, class p_storage_unit>
[[nodiscard]] inline minimizer_kernel_result two_stack_nthash_fixed_case(
    minimizer_kernel_invocation& invocation)
{
    minimizer_kernel_result result{};
    result.stats.algorithm = minimizer_kernel_algorithm::two_stack_nthash;
    result.stats.target = static_cast<std::int64_t>(HWY_TARGET);

    const auto reject_output_layout = [&result]() {
        result.family_status = static_cast<std::uint32_t>(
            two_stack_continuation_status::unsupported_representation);
        result.status = minimizer_kernel_status::invalid_configuration;
        return result;
    };

    if (!invocation.sequence.final_segment
        || invocation.sequence.begin_symbol != 0u
        || invocation.window == 0u
        || invocation.window > k_max_two_stack_window
        || invocation.kmer == 0u
        || invocation.kmer > k_max_exact_two_stack_kmer) {
        result.status = minimizer_kernel_status::invalid_configuration;
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

    bool is_internally_consistent = false;
    switch (invocation.output_layout) {
        case minimizer_output_layout::sequence_log2_15_kmer_15_uint64:
            is_internally_consistent = coarse_two_stack_detail::run_selected_output_layout<
                minimizer_representation<15u, 15u>, p_alphabet, p_storage_unit>(
                    invocation, result);
            break;
        case minimizer_output_layout::sequence_log2_28_kmer_15_uint64:
            is_internally_consistent = coarse_two_stack_detail::run_selected_output_layout<
                minimizer_representation<28u, 15u>, p_alphabet, p_storage_unit>(
                    invocation, result);
            break;
        case minimizer_output_layout::sequence_log2_33_kmer_15_uint64:
            is_internally_consistent = coarse_two_stack_detail::run_selected_output_layout<
                minimizer_representation<33u, 15u>, p_alphabet, p_storage_unit>(
                    invocation, result);
            break;
#if defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_EXTENDED_KMER_COORDINATES)
        case minimizer_output_layout::sequence_log2_49_kmer_7_uint64:
            is_internally_consistent = coarse_two_stack_detail::run_selected_output_layout<
                minimizer_representation<49u, 7u>, p_alphabet, p_storage_unit>(
                    invocation, result);
            break;
#endif
        case minimizer_output_layout::unsupported:
        default:
            return reject_output_layout();
    }
    if (!is_internally_consistent && result.status == minimizer_kernel_status::ok) {
        result.status = minimizer_kernel_status::invalid_sequence_view;
    }
    return result;
}

/** Source-topology-owned exact two-stack case. */
template<sequence_alphabet p_alphabet, class p_storage_unit>
requires (
    (std::same_as<p_alphabet, seq::dna4>
        || std::same_as<p_alphabet, seq::dna5>)
    && (std::same_as<p_storage_unit, std::uint8_t>
        || std::same_as<p_storage_unit, std::uint16_t>
        || std::same_as<p_storage_unit, std::uint32_t>
        || std::same_as<p_storage_unit, std::uint64_t>))
[[nodiscard]] inline minimizer_kernel_result two_stack_nthash_case(
    minimizer_kernel_invocation& invocation)
{
    using config = sequence_custom_alphabet<p_alphabet, p_storage_unit>;
    using traits = typename compact_sequence_range<config>::traits;
    if (invocation.sequence.bits_per_symbol != traits::bits_per_char
        || invocation.sequence.storage_unit_bytes != sizeof(p_storage_unit)) {
        minimizer_kernel_result rejected{};
        rejected.stats.algorithm =
            minimizer_kernel_algorithm::two_stack_nthash;
        rejected.stats.target = static_cast<std::int64_t>(HWY_TARGET);
        rejected.status = minimizer_kernel_status::invalid_configuration;
        rejected.family_status = static_cast<std::uint32_t>(
            two_stack_continuation_status::invalid_static_configuration);
        return rejected;
    }
    return two_stack_nthash_fixed_case<p_alphabet, p_storage_unit>(invocation);
}



} // namespace target
} // namespace bioseq::simd
