/**
 * @file bounded_nthash_kernel.cppm
 * @brief Fixed-target bounded ntHash minimizer kernel.
 */
module;
#include <hwy/highway.h>

export module bioseq.highway.sources.minimizer_bounded_nthash;

import std;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.simd_iteration_geometry;
import bioseq.instrumentation;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.highway.sources.sequence_ops;
import bioseq.highway.sources.hasher_base;
import bioseq.highway.sources.simd_utils;
import bioseq.highway.sources.vector_stream;

export namespace bioseq::simd {
namespace [[bioseq::simd]] target {
namespace hn = hwy::HWY_NAMESPACE;

using ::bioseq::minimizers::minimap_sketch_data;
using ::bioseq::minimizers::minimizer_output_layout;
using ::bioseq::minimizers::minimizer_representation;
using ::bioseq::minimizers::minimizer_sink_collector;
using ::bioseq::minimizers::k_minimizer_output_layout;
using ::bioseq::minimizers::k_max_two_stack_window;
using ::bioseq::minimizers::k_max_exact_two_stack_kmer;
using ::bioseq::minimizers::bounded_nthash_status;
using ::bioseq::minimizers::k_bounded_nthash_scheme_id;
using ::bioseq::minimizers::k_bounded_nthash_max_window;
using ::bioseq::minimizers::bounded_nthash_execution_stats;
using ::bioseq::minimizers::bounded_nthash_result;
using ::bioseq::minimizers::minimizer_kernel_algorithm;
using ::bioseq::minimizers::minimizer_kernel_invocation;
using ::bioseq::minimizers::minimizer_kernel_result;
using ::bioseq::minimizers::minimizer_kernel_status;

namespace bounded_nthash_internal {

/// Forward ntHash constants for A, C, T, and G ranks.
inline constexpr std::array<std::uint32_t, 4> k_forward_table{
    0x95c60474u,
    0x62a02b4cu,
    0x82572324u,
    0x4be24456u,
};

/**
 * @brief Looks up ACTG ntHash constants in one native u32 register.
 * @tparam p_simd_tag Highway descriptor type.
 * @param simd_tag Active u32 descriptor.
 * @param table Four ntHash constants arranged for lane lookup.
 * @param ranks Per-lane DNA4 ranks.
 * @return Per-lane ntHash constants.
 */
template<class p_simd_tag>
[[nodiscard]] HWY_INLINE hn::Vec<p_simd_tag> lookup_hash_table(
    p_simd_tag simd_tag,
    hn::Vec<p_simd_tag> table,
    hn::Vec<p_simd_tag> ranks) noexcept
{
    return hn::TableLookupLanes(
        table, hn::IndicesFromVec(simd_tag, ranks));
}

} // namespace bounded_nthash_internal

// The packed kernel consumes resident dna4 words, retains bounded k-mer and
// window-minimum state, and emits directly into the caller's collector.

namespace bounded_nthash_internal {

/**
 * @brief Provides a zero-copy u32 view of an aligned DNA4 subrange.
 *
 * Unlike bit_casted_view itself, size/bp_size/padding describe only the
 * selected [begin,end) interval, so iterator offset zero maps to local
 * position zero and every emitted position is segment-local.
 */
// BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN bounded_u32_dna4_view
struct bounded_u32_dna4_view {
    /** @brief DNA4 configuration exposed by this view. */
    using output_sequence = sequence_custom_alphabet<
        seq::dna4, std::uint32_t, 2>;
    /** @brief Compact range whose traits define u32 packing geometry. */
    using output_range = compact_sequence_range<output_sequence>;
    /** @brief Alphabet and storage traits for the local view. */
    using traits = typename output_range::traits;
    /** @brief Iterator over borrowed u32 packed units. */
    using iterator = const std::uint32_t*;

    std::size_t local_bp_size = 0;
    std::size_t local_view_units = 0;

    /// @brief Creates a local view with explicit logical and packed lengths.
    /// @param bp_size Logical symbols in the selected subrange.
    /// @param view_units Borrowed u32 units covering the subrange.
    bounded_u32_dna4_view(
        std::size_t bp_size,
        std::size_t view_units)
        : local_bp_size(bp_size),
          local_view_units(view_units) {}

    /// @brief Returns the local logical symbol count.
    [[nodiscard]] HWY_INLINE std::size_t bp_size() const noexcept {
        return local_bp_size;
    }
    /// @brief Returns the local packed byte count.
    [[nodiscard]] HWY_INLINE std::size_t byte_size() const noexcept {
        return local_view_units * sizeof(std::uint32_t);
    }
    /// @brief Returns the local u32 unit count.
    [[nodiscard]] HWY_INLINE std::size_t size() const noexcept {
        return local_view_units;
    }
    /// @brief Returns logical padding in the final local unit.
    [[nodiscard]] HWY_INLINE std::size_t padding() const noexcept {
        return local_view_units * traits::letters_per_unit - local_bp_size;
    }
    /// @brief Returns the packed pointer represented by an iterator.
    [[nodiscard]] HWY_INLINE const std::uint32_t* read_cptr(
        const iterator& position) const noexcept {
        return position;
    }
    /// @brief Advances a packed iterator by local u32 units.
    HWY_INLINE static void advance_iter(
        iterator& position,
        std::size_t view_units) {
        position += view_units;
    }
    /// @brief Measures a packed iterator interval in u32 units.
    [[nodiscard]] HWY_INLINE static std::size_t view_distance(
        const iterator& first,
        const iterator& last) {
        return static_cast<std::size_t>(last - first);
    }
};
// BIOSEQ_PRIVATE_PACKED_ACCESS_END bounded_u32_dna4_view

/// @brief Looks up ntHash constants for every logical stream.
/// @tparam p_simd_tag Highway descriptor type.
/// @tparam p_stream_count Number of logical vector streams.
/// @param simd_tag Active u32 descriptor.
/// @param table Four ntHash constants arranged for lane lookup.
/// @param ranks Per-stream DNA4 ranks.
/// @return Per-stream ntHash constants.
template<class p_simd_tag, std::size_t p_stream_count>
[[nodiscard]] HWY_INLINE auto lookup_table_stream(
    p_simd_tag simd_tag,
    hn::Vec<p_simd_tag> table,
    const vector_stream<p_simd_tag, std::uint32_t, p_stream_count>& ranks) noexcept
{
    return make_vector_stream<p_simd_tag, std::uint32_t, p_stream_count>(
        [&simd_tag, &table, &ranks](auto stream_constant) [[gnu::always_inline]] {
            constexpr std::size_t k_stream_index =
                decltype(stream_constant)::value;
            return bounded_nthash_internal::lookup_hash_table(
                simd_tag, table, ranks[k_stream_index]);
        });
}

/// @brief Rotates every stream lane left by a compile-time count.
/// @tparam p_rotation Compile-time bit rotation.
/// @tparam p_simd_tag Highway descriptor type.
/// @tparam p_stream_count Number of logical vector streams.
/// @param value Per-stream values to rotate.
/// @return Rotated values with the same stream geometry.
template<int p_rotation, class p_simd_tag, std::size_t p_stream_count>
[[nodiscard]] HWY_INLINE auto rotate_left_stream(
    const vector_stream<p_simd_tag, std::uint32_t, p_stream_count>& value) noexcept
{
    if constexpr ((p_rotation & 31) == 0) {
        return value;
    } else {
        return rotate_left<(p_rotation & 31)>(value);
    }
}

/// @brief Rotates every stream lane left by a runtime count.
/// @tparam p_simd_tag Highway descriptor type.
/// @tparam p_stream_count Number of logical vector streams.
/// @param value Per-stream values to rotate.
/// @param rotation Rotation modulo 32 bits.
/// @return Rotated values with the same stream geometry.
template<class p_simd_tag, std::size_t p_stream_count>
[[nodiscard]] HWY_INLINE auto rotate_left_stream_runtime(
    const vector_stream<p_simd_tag, std::uint32_t, p_stream_count>& value,
    int rotation) noexcept
{
    rotation &= 31;
    if (rotation == 0) return value;
    return bit_or(
        shift_left_same(value, rotation),
        shift_right_same(value, 32 - rotation));
}

/// @brief Rotates every stream lane right by a compile-time count.
/// @tparam p_rotation Compile-time bit rotation.
/// @tparam p_simd_tag Highway descriptor type.
/// @tparam p_stream_count Number of logical vector streams.
/// @param value Per-stream values to rotate.
/// @return Rotated values with the same stream geometry.
template<int p_rotation, class p_simd_tag, std::size_t p_stream_count>
[[nodiscard]] HWY_INLINE auto rotate_right_stream(
    const vector_stream<p_simd_tag, std::uint32_t, p_stream_count>& value) noexcept
{
    if constexpr ((p_rotation & 31) == 0) {
        return value;
    } else {
        return rotate_right<(p_rotation & 31)>(value);
    }
}

/**
 * @brief Stores one full-u32 minimum and strand at its extreme positions.
 * @tparam p_simd_tag Highway descriptor type.
 * @tparam p_stream_count Number of logical vector streams.
 */
template<class p_simd_tag, std::size_t p_stream_count>
struct minimum_stream {
    /** @brief Stream bundle containing u32 vectors. */
    using vector_stream_type =
        vector_stream<p_simd_tag, std::uint32_t, p_stream_count>;
    /** @brief Stream bundle containing per-lane masks. */
    using mask_stream_type = mask_stream<p_simd_tag, p_stream_count>;

    vector_stream_type hash;
    vector_stream_type left_position;
    vector_stream_type right_position;
    vector_stream_type left_strand;
    vector_stream_type right_strand;
    mask_stream_type valid;

    // This template is instantiated by an ordinary per-target owner unit.
    // Keep every special member explicit so the renderer can attach HWY_ATTR;
    // otherwise Clang synthesizes targetless members around BF16 streams.
    /** @brief Creates an empty target-attributed stream aggregate. */
    HWY_INLINE minimum_stream() noexcept
        : hash{},
          left_position{},
          right_position{},
          left_strand{},
          right_strand{},
          valid{} {}

    /**
     * @brief Creates a minimum aggregate from complete vector metadata.
     * @param hash_value Minimum hash in each lane.
     * @param left_position_value Leftmost position attaining each minimum.
     * @param right_position_value Rightmost position attaining each minimum.
     * @param left_strand_value Strand at each leftmost position.
     * @param right_strand_value Strand at each rightmost position.
     * @param valid_value Lanes containing a candidate.
     */
    HWY_INLINE minimum_stream(
        vector_stream_type hash_value,
        vector_stream_type left_position_value,
        vector_stream_type right_position_value,
        vector_stream_type left_strand_value,
        vector_stream_type right_strand_value,
        mask_stream_type valid_value) noexcept
        : hash(std::move(hash_value)),
          left_position(std::move(left_position_value)),
          right_position(std::move(right_position_value)),
          left_strand(std::move(left_strand_value)),
          right_strand(std::move(right_strand_value)),
          valid(std::move(valid_value)) {}

    /** @brief Copies all target-attributed minimum metadata. */
    HWY_INLINE minimum_stream(const minimum_stream& other) noexcept
        : hash(other.hash),
          left_position(other.left_position),
          right_position(other.right_position),
          left_strand(other.left_strand),
          right_strand(other.right_strand),
          valid(other.valid) {}

    /** @brief Moves all target-attributed minimum metadata. */
    HWY_INLINE minimum_stream(minimum_stream&& other) noexcept
        : hash(std::move(other.hash)),
          left_position(std::move(other.left_position)),
          right_position(std::move(other.right_position)),
          left_strand(std::move(other.left_strand)),
          right_strand(std::move(other.right_strand)),
          valid(std::move(other.valid)) {}

    /** @brief Replaces this aggregate with copied minimum metadata. */
    HWY_INLINE minimum_stream& operator=(
        const minimum_stream& other) noexcept
    {
        hash = other.hash;
        left_position = other.left_position;
        right_position = other.right_position;
        left_strand = other.left_strand;
        right_strand = other.right_strand;
        valid = other.valid;
        return *this;
    }

    /** @brief Replaces this aggregate with moved minimum metadata. */
    HWY_INLINE minimum_stream& operator=(minimum_stream&& other) noexcept
    {
        hash = std::move(other.hash);
        left_position = std::move(other.left_position);
        right_position = std::move(other.right_position);
        left_strand = std::move(other.left_strand);
        right_strand = std::move(other.right_strand);
        valid = std::move(other.valid);
        return *this;
    }
};

/**
 * Data-independent lane-wise minimum merge. Equal hashes preserve only the
 * leftmost and rightmost positions, which is the explicit RC-symmetric policy
 * for the even default window of ten candidates.
 * @tparam p_simd_tag Highway descriptor type.
 * @tparam p_stream_count Number of logical vector streams.
 * @param first Earlier minimum aggregate.
 * @param second Later minimum aggregate.
 * @return Lane-wise minimum with equal-hash endpoints merged.
 */
template<class p_simd_tag, std::size_t p_stream_count>
[[nodiscard]] HWY_INLINE minimum_stream<p_simd_tag, p_stream_count> combine_minima(
    p_simd_tag,
    const minimum_stream<p_simd_tag, p_stream_count>& first,
    const minimum_stream<p_simd_tag, p_stream_count>& second) noexcept
{
    const auto both_valid = bit_and(first.valid, second.valid);
    const auto second_smaller = bit_and(
        second.valid,
        bit_or(
            bit_and_not(first.valid, second.valid),
            less(second.hash, first.hash)));
    const auto hashes_equal = bit_and(both_valid, equal(first.hash, second.hash));
    const auto second_leftmost = bit_and(
        hashes_equal, less(second.left_position, first.left_position));
    const auto second_rightmost = bit_and(
        hashes_equal, greater(second.right_position, first.right_position));

    return minimum_stream<p_simd_tag, p_stream_count>{
        if_then_else(second_smaller, second.hash, first.hash),
        if_then_else(
            second_smaller,
            second.left_position,
            if_then_else(
                second_leftmost,
                second.left_position,
                first.left_position)),
        if_then_else(
            second_smaller,
            second.right_position,
            if_then_else(
                second_rightmost,
                second.right_position,
                first.right_position)),
        if_then_else(
            second_smaller,
            second.left_strand,
            if_then_else(
                second_leftmost,
                second.left_strand,
                first.left_strand)),
        if_then_else(
            second_smaller,
            second.right_strand,
            if_then_else(
                second_rightmost,
                second.right_strand,
                first.right_strand)),
        bit_or(first.valid, second.valid),
    };
}

/**
 * The iterator supplies one tail mask for the whole vector stream. Only the
 * final logical stream reaches sequence padding, so earlier streams retain a
 * full live mask.
 * @tparam p_is_full Whether the iterator supplied a complete matrix.
 * @tparam p_simd_tag Highway descriptor type.
 * @tparam p_stream_count Number of logical vector streams.
 * @param simd_tag Active u32 descriptor.
 * @param iterator_mask Live physical lanes in a partial matrix.
 * @return Live masks for every logical stream.
 */
template<bool p_is_full, class p_simd_tag, std::size_t p_stream_count>
[[nodiscard]] HWY_INLINE auto correct_live_stream(
    p_simd_tag simd_tag,
    hn::Mask<p_simd_tag> iterator_mask) noexcept
{
    const auto full = hn::FirstN(simd_tag, hn::Lanes(simd_tag));
    return [&full, &iterator_mask]<std::size_t... p_stream_indices>(
               std::index_sequence<p_stream_indices...>) [[gnu::always_inline]] {
        return mask_stream<p_simd_tag, p_stream_count>::pack(
            ((p_is_full || p_stream_indices + 1 < p_stream_count)
                ? full
                : iterator_mask)...);
    }(std::make_index_sequence<p_stream_count>{});
}

/// @brief Loads fixed per-stream u32 metadata into one vector bundle.
/// @tparam p_simd_tag Highway descriptor type.
/// @tparam p_stream_count Number of logical vector streams.
/// @param simd_tag Active u32 descriptor.
/// @param values Eight logical-lane values in stream-major order.
/// @return Vector-stream bundle for the active physical lane count.
template<class p_simd_tag, std::size_t p_stream_count>
[[nodiscard]] HWY_INLINE auto load_stream_metadata(
    p_simd_tag simd_tag,
    const std::array<std::uint32_t, 8>& values) noexcept
{
    const std::size_t lanes = hn::Lanes(simd_tag);
    return make_vector_stream<p_simd_tag, std::uint32_t, p_stream_count>(
        [&simd_tag, &values, &lanes](auto stream_constant) [[gnu::always_inline]] {
            constexpr std::size_t k_stream_index =
                decltype(stream_constant)::value;
            return hn::LoadU(
                simd_tag,
                values.data() + k_stream_index * lanes);
        });
}

/// @brief Holds rolling hashes, window minima, and output-boundary state.
/// @tparam p_kmer_size K-mer length in biological symbols.
/// @tparam p_window_size Minimizer window length in k-mers.
/// @tparam p_stream_count Number of logical vector streams.
/// @tparam p_minimizer_type Collector record representation.
/// @tparam p_collector_type Collector receiving selected records.
template<
    std::size_t p_kmer_size,
    std::size_t p_window_size,
    std::size_t p_stream_count,
    class p_minimizer_type,
    class p_collector_type>
struct packed_iteration_state {
    /** @brief Fixed-width u32 descriptor used by the kernel. */
    using simd_tag_type = hn::CappedTag<std::uint32_t, 8>;
    /** @brief One physical u32 vector. */
    using vector_type = hn::Vec<simd_tag_type>;
    /** @brief Mask matching one physical u32 vector. */
    using mask_type = hn::Mask<simd_tag_type>;
    /** @brief Bundle spanning all logical u32 streams. */
    using vector_stream_type =
        vector_stream<simd_tag_type, std::uint32_t, p_stream_count>;
    /** @brief Mask bundle spanning all logical streams. */
    using mask_stream_type = mask_stream<simd_tag_type, p_stream_count>;
    /** @brief Hash and endpoint metadata for a lane-wise minimum. */
    using minimum_type = minimum_stream<simd_tag_type, p_stream_count>;

    /// @brief Stores the first deferred event of a logical stream.
    struct buffered_boundary_event {
        std::uint32_t hash = 0;
        std::uint64_t absolute_last_position = 0;
        std::uint32_t strand = 0;
        bool valid = false;
    };

    /// @brief Counts biological work and collector interactions.
    struct runtime_evidence {
        std::uint64_t emission_mask_store_batches = 0;
        std::uint64_t minimizers_emitted = 0;
        std::uint64_t tied_endpoint_records_emitted = 0;
        std::uint64_t stream_boundary_records_buffered = 0;
        std::uint64_t stream_boundary_records_committed = 0;
        std::uint64_t stream_boundary_duplicates_suppressed = 0;
        std::uint64_t scalar_collector_appends = 0;
    };

    vector_stream_type forward_hash;
    vector_stream_type reverse_hash;
    std::array<vector_stream_type, p_kmer_size> symbol_ring;
    std::size_t symbols_seen = 0;
    std::size_t next_symbol_slot = 0;

    minimum_type prefix_minimum;
    std::array<minimum_type, p_window_size> suffix_ring;
    std::size_t next_window_slot = 0;

    vector_stream_type window_limits;
    vector_stream_type last_emit_hash;
    vector_stream_type last_emit_position;
    vector_stream_type last_emit_strand;
    mask_stream_type last_emit_valid;
    std::array<bool, 8> logical_lane_has_event{};
    std::array<buffered_boundary_event, 8> first_boundary_event{};

    vector_type k_forward_table;
    vector_type complement_table;
    p_collector_type& collector;
    runtime_evidence evidence{};

    /**
     * @brief Advances all streams by one decoded biological symbol.
     * @tparam p_is_full Whether every iterator lane is live.
     * @tparam p_offsets_type Stream-to-sequence offset bundle type.
     * @param simd_tag Active u32 descriptor.
     * @param offsets Packed-unit origin for each logical lane.
     * @param iterator_mask Live lanes in the final partial matrix.
     * @param incoming_rank Decoded DNA4 rank for every logical lane.
     */
    template<bool p_is_full, class p_offsets_type>
    [[gnu::always_inline]] [[gnu::flatten]] HWY_INLINE void step(
        simd_tag_type simd_tag,
        const p_offsets_type& offsets,
        mask_type iterator_mask,
        vector_stream_type incoming_rank)
    {
        const auto live = correct_live_stream<
            p_is_full, simd_tag_type, p_stream_count>(
                simd_tag, iterator_mask);

        if (symbols_seen < p_kmer_size) {
            symbol_ring[symbols_seen] = incoming_rank;
            const auto forward_value = lookup_table_stream(
                simd_tag, k_forward_table, incoming_rank);
            const auto reverse_value = lookup_table_stream(
                simd_tag, complement_table, incoming_rank);
            const int forward_rotation = static_cast<int>(
                ((p_kmer_size - 1 - symbols_seen) * 7) & 31u);
            const int reverse_rotation = static_cast<int>(
                (symbols_seen * 7) & 31u);
            // Initialization runs only p_kmer_size times. The rolling steady state below
            // uses compile-time rotations; this bounded startup uses the
            // target's vector shift-by-scalar lowering.
            forward_hash = bit_xor(
                forward_hash,
                rotate_left_stream_runtime(
                    forward_value, forward_rotation));
            reverse_hash = bit_xor(
                reverse_hash,
                rotate_left_stream_runtime(
                    reverse_value, reverse_rotation));
            ++symbols_seen;
            if (symbols_seen < p_kmer_size) return;
        } else {
            const vector_stream_type outgoing_rank =
                symbol_ring[next_symbol_slot];
            symbol_ring[next_symbol_slot] = incoming_rank;
            if (++next_symbol_slot == p_kmer_size) next_symbol_slot = 0;

            const auto forward_out = lookup_table_stream(
                simd_tag, k_forward_table, outgoing_rank);
            const auto forward_in = lookup_table_stream(
                simd_tag, k_forward_table, incoming_rank);
            const auto reverse_out = lookup_table_stream(
                simd_tag, complement_table, outgoing_rank);
            const auto reverse_in = lookup_table_stream(
                simd_tag, complement_table, incoming_rank);
            forward_hash = bit_xor(
                bit_xor(
                    rotate_left_stream<7>(forward_hash),
                    rotate_left_stream<static_cast<int>((p_kmer_size * 7) & 31u)>(
                        forward_out)),
                forward_in);
            reverse_hash = bit_xor(
                bit_xor(
                    rotate_right_stream<7>(reverse_hash),
                    rotate_right_stream<7>(reverse_out)),
                rotate_left_stream<
                    static_cast<int>(((p_kmer_size - 1) * 7) & 31u)>(reverse_in));
            ++symbols_seen;
        }

        const std::uint32_t local_kmer_position =
            static_cast<std::uint32_t>(symbols_seen - p_kmer_size);
        const vector_stream_type position = vector_stream_type::splat(
            simd_tag, local_kmer_position);
        const vector_stream_type canonical_hash = add(
            forward_hash, reverse_hash);
        const mask_stream_type reverse_strand = less(
            reverse_hash, forward_hash);
        const vector_stream_type strand = if_then_else(
            reverse_strand,
            vector_stream_type::splat(simd_tag, 1u),
            vector_stream_type::splat_zero(simd_tag));
        const minimum_type entering{
            canonical_hash,
            position,
            position,
            strand,
            strand,
            live,
        };

        const std::size_t written_slot = next_window_slot;
        if (++next_window_slot == p_window_size) next_window_slot = 0;
        suffix_ring[written_slot] = entering;
        prefix_minimum = combine_minima(
            simd_tag, prefix_minimum, entering);
        const bool rebuild_suffixes = next_window_slot == 0;
        if (rebuild_suffixes) {
            minimum_type suffix = suffix_ring[p_window_size - 1];
            for (std::size_t slot = p_window_size - 1; slot > 0; --slot) {
                suffix = combine_minima(
                    simd_tag, suffix_ring[slot - 1], suffix);
                suffix_ring[slot - 1] = suffix;
            }
            prefix_minimum = entering;
        }
        const minimum_type window_minimum = combine_minima(
            simd_tag,
            prefix_minimum,
            suffix_ring[next_window_slot]);

        const std::size_t candidate_index = local_kmer_position;
        if (candidate_index + 1 < p_window_size) return;
        const std::uint32_t window_index = static_cast<std::uint32_t>(
            candidate_index + 1 - p_window_size);
        const vector_stream_type window_index_v = vector_stream_type::splat(
            simd_tag, window_index);
        const mask_stream_type owned_window = bit_and(
            window_minimum.valid,
            less(window_index_v, window_limits));
        const mask_stream_type tied = bit_and(
            owned_window,
            not_equal(window_minimum.left_position,
               window_minimum.right_position));

        emit_selected(
            simd_tag,
            offsets,
            owned_window,
            window_minimum.hash,
            window_minimum.left_position,
            window_minimum.left_strand,
            false);
        const auto no_ties_per_stream = collect_all_false_per_stream(
            simd_tag, tied);
        bool any_tie = false;
        for (const bool no_tie : no_ties_per_stream) {
            any_tie = any_tie || !no_tie;
        }
        if (any_tie) [[unlikely]] {
            emit_selected(
                simd_tag,
                offsets,
                tied,
                window_minimum.hash,
                window_minimum.right_position,
                window_minimum.right_strand,
                true);
        }
    }

private:
    /**
     * @brief Appends newly selected lane events and defers cross-stream boundaries.
     * @tparam p_offsets_type Stream-to-sequence offset bundle type.
     * @param simd_tag Active u32 descriptor.
     * @param offsets Packed-unit origin for each logical lane.
     * @param selected Lanes whose candidate should be considered.
     * @param hash Selected canonical hashes.
     * @param position Segment-local k-mer positions.
     * @param strand Selected strand identifiers.
     * @param tie_endpoint Whether these events are tied right endpoints.
     */
    template<class p_offsets_type>
    [[gnu::always_inline]] HWY_INLINE void emit_selected(
        simd_tag_type simd_tag,
        const p_offsets_type& offsets,
        mask_stream_type selected,
        const vector_stream_type& hash,
        const vector_stream_type& position,
        const vector_stream_type& strand,
        bool tie_endpoint)
    {
        const auto same_identity = bit_and(
            last_emit_valid,
            bit_and(equal(hash, last_emit_hash),
                bit_and(equal(position, last_emit_position),
                    equal(strand, last_emit_strand))));
        const auto emit = bit_and_not(same_identity, selected);
        const auto no_emit_per_stream = collect_all_false_per_stream(
            simd_tag, emit);
        bool any_emit = false;
        for (const bool no_emit : no_emit_per_stream) {
            any_emit = any_emit || !no_emit;
        }
        if (!any_emit) return;

        constexpr std::size_t k_max_lanes = hn::MaxLanes(simd_tag_type{});
        HWY_ALIGN std::array<std::array<std::uint32_t, k_max_lanes>,
            p_stream_count> hash_lanes;
        HWY_ALIGN std::array<std::array<std::uint32_t, k_max_lanes>,
            p_stream_count> position_lanes;
        HWY_ALIGN std::array<std::array<std::uint32_t, k_max_lanes>,
            p_stream_count> strand_lanes;
        HWY_ALIGN std::array<std::array<std::uint32_t, k_max_lanes>,
            p_stream_count> emit_lanes;
        for_each_stream<p_stream_count>([&no_emit_per_stream,
                                         &hash,
                                         &simd_tag,
                                         &hash_lanes,
                                         &position,
                                         &position_lanes,
                                         &strand,
                                         &strand_lanes,
                                         &emit,
                                         &emit_lanes](auto stream_constant) {
            constexpr std::size_t k_stream_index =
                decltype(stream_constant)::value;
            if (no_emit_per_stream[k_stream_index])
                return;
            hn::StoreU(
                hash[k_stream_index],
                simd_tag,
                hash_lanes[k_stream_index].data());
            hn::StoreU(
                position[k_stream_index],
                simd_tag,
                position_lanes[k_stream_index].data());
            hn::StoreU(
                strand[k_stream_index],
                simd_tag,
                strand_lanes[k_stream_index].data());
            hn::StoreU(
                hn::VecFromMask(simd_tag, emit[k_stream_index]),
                simd_tag,
                emit_lanes[k_stream_index].data());
        });
        ++evidence.emission_mask_store_batches;

        const std::size_t lanes = hn::Lanes(simd_tag);
        for (std::size_t stream = 0; stream < p_stream_count; ++stream) {
            if (no_emit_per_stream[stream]) continue;
            for (std::size_t lane = 0; lane < lanes; ++lane) {
                if (emit_lanes[stream][lane] == 0) continue;
                const std::size_t logical_lane = stream * lanes + lane;
                const std::uint64_t absolute_last_position =
                    static_cast<std::uint64_t>(offsets[logical_lane]) * 16u
                    + position_lanes[stream][lane]
                    + (p_kmer_size - 1);
                if (!logical_lane_has_event[logical_lane]) {
                    logical_lane_has_event[logical_lane] = true;
                    if (logical_lane != 0) {
                        first_boundary_event[logical_lane] = {
                            .hash = hash_lanes[stream][lane],
                            .absolute_last_position = absolute_last_position,
                            .strand = strand_lanes[stream][lane],
                            .valid = true,
                        };
                        ++evidence.stream_boundary_records_buffered;
                        continue;
                    }
                }
                append_collector_event(
                    hash_lanes[stream][lane],
                    absolute_last_position,
                    strand_lanes[stream][lane],
                    tie_endpoint);
            }
        }

        last_emit_hash = if_then_else(emit, hash, last_emit_hash);
        last_emit_position = if_then_else(
            emit, position, last_emit_position);
        last_emit_strand = if_then_else(emit, strand, last_emit_strand);
        last_emit_valid = bit_or(last_emit_valid, emit);
    }

public:
    /**
     * @brief Completes adjacent deduplication across logical-stream joins.
     *
     * The operation uses
     * bounded metadata only. The first event of lanes 1..7 was withheld. It
     * is appended here unless it equals the nearest preceding non-empty
     * lane's final event. Collector append order is intentionally unspecified,
     * as it already is for eight lock-step SIMD streams. Record identity is
     * globally adjacent-deduplicated.
     * @tparam p_offsets_type Stream-to-sequence offset bundle type.
     * @param simd_tag Active u32 descriptor.
     * @param offsets Packed-unit origin for each logical lane.
     */
    template<class p_offsets_type>
    HWY_INLINE void finalize_stream_boundaries(
        simd_tag_type simd_tag,
        const p_offsets_type& offsets) {
        constexpr std::size_t k_max_lanes = hn::MaxLanes(simd_tag_type{});
        HWY_ALIGN std::array<std::array<std::uint32_t, k_max_lanes>,
            p_stream_count> hash_lanes;
        HWY_ALIGN std::array<std::array<std::uint32_t, k_max_lanes>,
            p_stream_count> position_lanes;
        HWY_ALIGN std::array<std::array<std::uint32_t, k_max_lanes>,
            p_stream_count> strand_lanes;
        for_each_stream<p_stream_count>([this,
                                         &simd_tag,
                                         &hash_lanes,
                                         &position_lanes,
                                         &strand_lanes](auto stream_constant) {
            constexpr std::size_t k_stream_index =
                decltype(stream_constant)::value;
            hn::StoreU(
                last_emit_hash[k_stream_index],
                simd_tag,
                hash_lanes[k_stream_index].data());
            hn::StoreU(
                last_emit_position[k_stream_index],
                simd_tag,
                position_lanes[k_stream_index].data());
            hn::StoreU(
                last_emit_strand[k_stream_index],
                simd_tag,
                strand_lanes[k_stream_index].data());
        });

        bool previous_valid = false;
        std::uint32_t previous_hash = 0;
        std::uint64_t previous_position = 0;
        std::uint32_t previous_strand = 0;
        const std::size_t lanes = hn::Lanes(simd_tag);
        for (std::size_t logical_lane = 0;
             logical_lane < 8;
             ++logical_lane) {
            if (!logical_lane_has_event[logical_lane]) continue;
            buffered_boundary_event& pending =
                first_boundary_event[logical_lane];
            if (pending.valid) {
                const bool duplicate = previous_valid
                    && pending.hash == previous_hash
                    && pending.absolute_last_position == previous_position
                    && pending.strand == previous_strand;
                if (duplicate) {
                    ++evidence.stream_boundary_duplicates_suppressed;
                } else {
                    append_collector_event(
                        pending.hash,
                        pending.absolute_last_position,
                        pending.strand,
                        false);
                    ++evidence.stream_boundary_records_committed;
                }
            }

            const std::size_t stream = logical_lane / lanes;
            const std::size_t lane = logical_lane % lanes;
            previous_hash = hash_lanes[stream][lane];
            previous_position =
                static_cast<std::uint64_t>(offsets[logical_lane]) * 16u
                + position_lanes[stream][lane]
                + (p_kmer_size - 1);
            previous_strand = strand_lanes[stream][lane];
            previous_valid = true;
        }
    }

private:
    /**
     * @brief Appends one minimizer record and updates emission evidence.
     * @param hash Canonical ntHash value.
     * @param absolute_last_position Sequence coordinate of the k-mer end.
     * @param strand Selected strand identifier.
     * @param tie_endpoint Whether the record is a tied right endpoint.
     */
    HWY_INLINE void append_collector_event(
        std::uint32_t hash,
        std::uint64_t absolute_last_position,
        std::uint32_t strand,
        bool tie_endpoint)
    {
        collector.collect(p_minimizer_type(
            static_cast<std::uint64_t>(hash),
            absolute_last_position,
            static_cast<std::uint64_t>(strand)));
        ++evidence.minimizers_emitted;
        ++evidence.scalar_collector_appends;
        if (tie_endpoint) ++evidence.tied_endpoint_records_emitted;
    }
};

/// @brief Runs the packed DNA4 bounded ntHash traversal.
/// @tparam p_kmer_size K-mer length in biological symbols.
/// @tparam p_window_size Minimizer window length in k-mers.
/// @tparam p_stream_count Number of logical vector streams.
/// @tparam p_minimizer_type Collector record representation.
/// @tparam p_collector_type Collector receiving selected records.
/// @tparam p_sequence_view Borrowed packed-view type.
/// @tparam p_iterator Packed storage iterator type.
/// @param collector Destination for selected minimizers.
/// @param view Segment-local logical and packed geometry.
/// @param begin First packed u32 unit to decode.
/// @param end One past the final packed u32 unit.
/// @param stats Evidence counters populated for the traversal.
template<
    std::size_t p_kmer_size,
    std::size_t p_window_size,
    std::size_t p_stream_count,
    class p_minimizer_type,
    class p_collector_type,
    supports_compact_sequence_bitcast_view p_sequence_view,
    class p_iterator>
// BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN run_packed_kernel
[[gnu::always_inline]] [[gnu::flatten]] inline void run_packed_kernel(
    p_collector_type& collector,
    const p_sequence_view& view,
    const p_iterator& begin,
    const p_iterator& end,
    bounded_nthash_execution_stats& stats)
{
    using simd_tag_type = hn::CappedTag<std::uint32_t, 8>;
    using vector_stream_type =
        vector_stream<simd_tag_type, std::uint32_t, p_stream_count>;
    using mask_stream_type = mask_stream<simd_tag_type, p_stream_count>;
    using state_type = packed_iteration_state<
        p_kmer_size, p_window_size, p_stream_count, p_minimizer_type, p_collector_type>;
    using minimum_type = typename state_type::minimum_type;
    const simd_tag_type simd_tag;
    const std::size_t lanes = hn::Lanes(simd_tag);
    constexpr std::size_t k_logical_streams = 8;
    constexpr std::size_t k_letters_per_unit = 16;
    constexpr std::size_t k_context_base_pairs =
        p_kmer_size + p_window_size - 1;
    constexpr std::size_t k_context_units =
        (k_context_base_pairs + k_letters_per_unit - 1)
        / k_letters_per_unit;
    const auto [units_per_lane, offsets] =
        ::bioseq::simd::iteration_geometry::calculate_iteration_offsets<
            k_logical_streams>(
            view.size(), k_logical_streams, k_context_units);
    const std::size_t total_windows =
        view.bp_size() - p_kmer_size - p_window_size + 2;

    std::array<std::uint32_t, k_logical_streams> window_limits{};
    for (std::size_t lane = 0; lane < k_logical_streams; ++lane) {
        const std::size_t start_bp = offsets[lane] * k_letters_per_unit;
        const std::size_t unclamped_end_bp = lane + 1 < k_logical_streams
            ? offsets[lane + 1] * k_letters_per_unit
            : total_windows;
        const std::size_t end_bp = std::min(
            unclamped_end_bp, total_windows);
        const std::size_t owned = start_bp < total_windows
                && end_bp > start_bp
            ? end_bp - start_bp
            : 0;
        window_limits[lane] = static_cast<std::uint32_t>(owned);
    }

    constexpr std::size_t k_max_lanes = hn::MaxLanes(simd_tag_type{});
    HWY_ALIGN std::array<std::uint32_t, k_max_lanes> table_values{};
    HWY_ALIGN std::array<std::uint32_t, k_max_lanes> complement_values{};
    for (std::size_t lane = 0; lane < k_max_lanes; ++lane) {
        const std::size_t rank = lane & 3u;
        table_values[lane] = bounded_nthash_internal::k_forward_table[rank];
        complement_values[lane] =
            bounded_nthash_internal::k_forward_table[3u - rank];
    }
    const auto zero = vector_stream_type::splat_zero(simd_tag);
    const auto false_masks = mask_stream_type::splat_false(simd_tag);
    const minimum_type empty_minimum{
        zero,
        zero,
        zero,
        zero,
        zero,
        false_masks,
    };
    state_type state{
        .forward_hash = zero,
        .reverse_hash = zero,
        .symbol_ring = {},
        .symbols_seen = 0,
        .next_symbol_slot = 0,
        .prefix_minimum = empty_minimum,
        .suffix_ring = {},
        .next_window_slot = 0,
        .window_limits = load_stream_metadata<simd_tag_type, p_stream_count>(
            simd_tag, window_limits),
        .last_emit_hash = zero,
        .last_emit_position = zero,
        .last_emit_strand = zero,
        .last_emit_valid = false_masks,
        .k_forward_table = hn::LoadU(simd_tag, table_values.data()),
        .complement_table = hn::LoadU(
            simd_tag, complement_values.data()),
        .collector = collector,
        .evidence = {},
    };
    for (std::size_t slot = 0; slot < p_window_size; ++slot) {
        state.suffix_ring[slot] = empty_minimum;
    }

    iterate_sequence_bp_simd<p_stream_count, simd_tag_type>(
        view,
        begin,
        end,
        k_context_base_pairs,
        // Advances rolling state for each decoded base matrix.
        [&state]<bool p_is_full>(std::bool_constant<p_is_full>,
                                 const auto& iterator_offsets,
                                 const auto& iteration_simd_tag,
                                 hn::Mask<simd_tag_type> iterator_mask,
                                 vector_stream_type ranks) [[gnu::always_inline]] [[gnu::flatten]] {
            state.template step<p_is_full>(
                iteration_simd_tag,
                iterator_offsets,
                iterator_mask,
                ranks);
        },
        // This kernel has no separate transposed-block consumer.
        [](const auto&, const auto&, auto&) {});
    state.finalize_stream_boundaries(simd_tag, offsets);

    // All per-base stats values are deterministic from admitted geometry.
    // Populate them once after the kernel so telemetry adds no memory traffic
    // to the biological loop.
    const std::uint64_t callback_steps =
        static_cast<std::uint64_t>(k_context_units + units_per_lane)
        * k_letters_per_unit;
    const std::uint64_t initialization_batches =
        std::min<std::uint64_t>(callback_steps, p_kmer_size);
    const std::uint64_t rolling_batches = callback_steps > p_kmer_size
        ? callback_steps - p_kmer_size
        : 0;
    const std::uint64_t candidate_batches = callback_steps >= p_kmer_size
        ? callback_steps - p_kmer_size + 1
        : 0;
    const std::uint64_t decision_batches = candidate_batches >= p_window_size
        ? candidate_batches - p_window_size + 1
        : 0;
    const std::uint64_t bps_per_matrix =
        static_cast<std::uint64_t>(lanes) * k_letters_per_unit;
    std::uint64_t partial_callbacks = callback_steps % bps_per_matrix;
    if (partial_callbacks == 0 && callback_steps != 0) {
        partial_callbacks = bps_per_matrix;
    }
    const std::uint64_t masked_tail_lanes =
        static_cast<std::uint64_t>(view.padding())
        * (offsets.front() == offsets.back() ? lanes : 1u);

    stats.packed_decode_batches = callback_steps;
    stats.packed_decode_lane_visits = callback_steps * k_logical_streams
        - masked_tail_lanes;
    stats.hash_initialization_batches = initialization_batches;
    stats.rolling_hash_batches = rolling_batches;
    stats.hash_table_lookup_batches =
        2 * initialization_batches + 4 * rolling_batches;
    stats.canonical_sum_batches = candidate_batches;
    stats.window_update_batches = candidate_batches;
    stats.suffix_rebuild_batches = candidate_batches / p_window_size;
    stats.emission_decision_batches = decision_batches;
    stats.partial_chunk_callback_batches = partial_callbacks;
    stats.masked_tail_lane_visits = masked_tail_lanes;
    stats.emission_mask_store_batches =
        state.evidence.emission_mask_store_batches;
    stats.minimizers_emitted = state.evidence.minimizers_emitted;
    stats.tied_endpoint_records_emitted =
        state.evidence.tied_endpoint_records_emitted;
    stats.stream_boundary_records_buffered =
        state.evidence.stream_boundary_records_buffered;
    stats.stream_boundary_records_committed =
        state.evidence.stream_boundary_records_committed;
    stats.stream_boundary_duplicates_suppressed =
        state.evidence.stream_boundary_duplicates_suppressed;
    stats.scalar_collector_appends =
        state.evidence.scalar_collector_appends;
}
// BIOSEQ_PRIVATE_PACKED_ACCESS_END run_packed_kernel

} // namespace bounded_nthash_internal

/**
 * @brief Collects bounded ntHash minimizers from an aligned resident segment.
 *
 * This is the checked resident entry point for the packed bounded ntHash
 * kernel. [begin,end) must be ordered and aligned to the resident range's
 * storage units. The zero-copy bounded u32 view emits segment-local positions.
 * Segmented callers add their known global base offset after collection.
 * @tparam p_kmer_size K-mer length in biological symbols.
 * @tparam p_window_size Minimizer window length in k-mers.
 * @tparam p_minimizer_type Collector record representation.
 * @tparam p_sequence_range Packed sequence configuration.
 * @tparam p_iterator Packed storage iterator type.
 * @tparam p_collector_type Collector receiving selected records.
 * @param collector Destination for selected minimizers.
 * @param sequence Borrowed resident packed sequence.
 * @param begin First aligned storage unit in the segment.
 * @param end One past the final aligned storage unit.
 * @return Status and execution evidence without throwing for rejected input.
 */
template<
    std::size_t p_kmer_size = 15,
    std::size_t p_window_size = 10,
    class p_minimizer_type = minimap_sketch_data,
    supports_compact_sequence_range p_sequence_range,
    class p_iterator = decltype(
        compact_sequence_range<p_sequence_range>().begin()),
    class p_collector_type = minimizer_sink_collector<p_minimizer_type>>
[[nodiscard]] inline bounded_nthash_result
// BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN collect_minimizers_bounded_nthash_simd_checked
collect_minimizers_bounded_nthash_simd_checked(
    p_collector_type& collector,
    compact_sequence_range<p_sequence_range>& sequence,
    const p_iterator& begin,
    const p_iterator& end)
{
    using range_type = compact_sequence_range<p_sequence_range>;
    using traits = typename range_type::traits;
    bounded_nthash_result result;
    // Records a fail-closed admission result without touching the collector.
    auto reject = [&result](bounded_nthash_status status) {
        result.status = status;
        result.stats.fail_closed_rejections = 1;
        return result;
    };

    if constexpr (p_kmer_size == 0 || p_kmer_size > k_max_exact_two_stack_kmer
                  || p_window_size == 0 || p_window_size > k_bounded_nthash_max_window) {
        return reject(bounded_nthash_status::invalid_configuration);
    } else if constexpr (!std::same_as<
            typename traits::alphabet_type, seq::dna4>) {
        return reject(bounded_nthash_status::unsupported_alphabet);
    } else if constexpr (!is_sequence_bit_castable_to<p_sequence_range, std::uint32_t>) {
        return reject(bounded_nthash_status::unsupported_storage);
    } else if constexpr (sizeof(typename traits::storage_unit)
            < sizeof(std::uint32_t)
        || sizeof(typename traits::storage_unit)
            % sizeof(std::uint32_t) != 0) {
        return reject(bounded_nthash_status::unsupported_storage);
    } else if constexpr (!std::constructible_from<
            p_minimizer_type, std::uint64_t, std::uint64_t, std::uint64_t>) {
        return reject(bounded_nthash_status::unsupported_representation);
    } else if constexpr (!requires {
            p_minimizer_type::k_hash_field_mask;
            p_minimizer_type::k_position_field_mask;
        }) {
        return reject(bounded_nthash_status::unsupported_representation);
    } else if constexpr (static_cast<std::uint64_t>(
            p_minimizer_type::k_hash_field_mask) < 0xFFFFFFFFull) {
        return reject(bounded_nthash_status::unsupported_representation);
    } else {
        const auto whole_begin = sequence.begin();
        const auto whole_end = sequence.end();
        if (begin < whole_begin || end < begin || whole_end < end) {
            return reject(bounded_nthash_status::invalid_range);
        }
        constexpr std::size_t k_base_letters_per_unit =
            traits::letters_per_unit;
        const std::size_t begin_units = static_cast<std::size_t>(
            begin - whole_begin);
        const std::size_t end_units = static_cast<std::size_t>(
            end - whole_begin);
        const std::size_t local_begin_bp =
            begin_units * k_base_letters_per_unit;
        const std::size_t local_nominal_end_bp =
            end_units * k_base_letters_per_unit;
        const std::size_t local_end_bp = std::min(
            local_nominal_end_bp,
            static_cast<std::size_t>(sequence.bp_size()));
        const std::size_t local_bp_size = local_end_bp >= local_begin_bp
            ? local_end_bp - local_begin_bp
            : 0;
        if (local_bp_size < p_kmer_size + p_window_size - 1) {
            return reject(bounded_nthash_status::input_too_short);
        }
        if (local_bp_size
            > static_cast<std::size_t>(
                (std::numeric_limits<std::uint32_t>::max)())) {
            return reject(bounded_nthash_status::input_too_large);
        }
        if (local_bp_size - 1
            > static_cast<std::size_t>(p_minimizer_type::k_position_field_mask)) {
            return reject(bounded_nthash_status::unsupported_representation);
        }
        constexpr std::size_t k_uint32_letters_per_unit = 16;
        const std::size_t local_view_units =
            (local_bp_size + k_uint32_letters_per_unit - 1)
            / k_uint32_letters_per_unit;
        const auto* local_words = reinterpret_cast<const std::uint32_t*>(
            sequence.read_cptr(begin));
        const auto* local_words_end = local_words + local_view_units;

        const bounded_nthash_internal::bounded_u32_dna4_view view(
            local_bp_size, local_view_units);
        using simd_tag_type = hn::CappedTag<std::uint32_t, 8>;
        const std::size_t lanes = hn::Lanes(simd_tag_type{});
        if (lanes != 4 && lanes != 8) {
            return reject(bounded_nthash_status::unsupported_vector_shape);
        }

        result.stats.logical_streams = 8;
        result.stats.physical_vector_lanes = lanes;
        result.stats.vector_registers_per_step = 8 / lanes;
        result.stats.input_symbols = local_bp_size;
        result.stats.input_storage_units = end_units - begin_units;
        result.stats.segments_combined = 1;
        result.stats.positions_are_segment_local = true;
        result.stats.scheme_id = k_bounded_nthash_scheme_id;
        result.stats.kmer_size = static_cast<std::uint32_t>(p_kmer_size);
        result.stats.window_size = static_cast<std::uint32_t>(p_window_size);
        result.stats.nthash_rotation = 7;
        result.stats.source_storage_unit_bytes =
            sizeof(typename traits::storage_unit);
        static const auto* region_description =
            bioseq::instrumentation::register_static_region(
                "mm_sketch_bounded_nthash_simd");
        auto region_scope =
            bioseq::instrumentation::region(region_description);
        if (lanes == 4) {
            bounded_nthash_internal::run_packed_kernel<
                p_kmer_size, p_window_size, 2, p_minimizer_type>(
                    collector,
                    view,
                    local_words,
                    local_words_end,
                    result.stats);
        } else {
            bounded_nthash_internal::run_packed_kernel<
                p_kmer_size, p_window_size, 1, p_minimizer_type>(
                    collector,
                    view,
                    local_words,
                    local_words_end,
                    result.stats);
        }
        return result;
    }
}
// BIOSEQ_PRIVATE_PACKED_ACCESS_END collect_minimizers_bounded_nthash_simd_checked

/**
 * @brief Collects bounded ntHash minimizers from a whole resident sequence.
 * @tparam p_kmer_size K-mer length in biological symbols.
 * @tparam p_window_size Minimizer window length in k-mers.
 * @tparam p_minimizer_type Collector record representation.
 * @tparam p_sequence_range Packed sequence configuration.
 * @tparam p_collector_type Collector receiving selected records.
 * @param collector Destination for selected minimizers.
 * @param sequence Borrowed resident packed sequence.
 * @return Status and execution evidence from the checked gate.
 */
template<
    std::size_t p_kmer_size = 15,
    std::size_t p_window_size = 10,
    class p_minimizer_type = minimap_sketch_data,
    supports_compact_sequence_range p_sequence_range,
    class p_collector_type = minimizer_sink_collector<p_minimizer_type>>
[[nodiscard]] inline bounded_nthash_result
collect_minimizers_bounded_nthash_simd(
    p_collector_type& collector,
    compact_sequence_range<p_sequence_range>& sequence)
{
    return collect_minimizers_bounded_nthash_simd_checked<
        p_kmer_size, p_window_size, p_minimizer_type>(
            collector, sequence, sequence.begin(), sequence.end());
}

namespace bounded_nthash_kernel_detail {

/// @brief Maps family-specific checked status to the common kernel status.
/// @param status Bounded ntHash admission result.
/// @return Equivalent common kernel status.
[[nodiscard]] constexpr minimizer_kernel_status map_bounded_nthash_status(
    const bounded_nthash_status status) noexcept
{
    switch (status) {
        case bounded_nthash_status::ok:
            return minimizer_kernel_status::ok;
        case bounded_nthash_status::invalid_range:
            return minimizer_kernel_status::invalid_sequence_view;
        default:
            return minimizer_kernel_status::invalid_configuration;
    }
}

/// @brief Adapts one typed packed descriptor to the bounded ntHash collector.
/// @tparam p_minimizer_type Selected output representation.
/// @tparam p_storage_unit Packed source storage unit.
/// @param invocation Type-erased kernel request and sink.
/// @param result Result populated with family status and evidence.
/// @return `true` when the borrowed descriptor is internally consistent.
template<class p_minimizer_type, class p_storage_unit>
[[nodiscard]] bool run_bounded_nthash(
    minimizer_kernel_invocation& invocation,
    minimizer_kernel_result& result)
{
    static_assert(
        k_minimizer_output_layout<p_minimizer_type>
            != minimizer_output_layout::unsupported);
    if (invocation.output_hash_mask
            != static_cast<std::uint64_t>(p_minimizer_type::k_hash_field_mask)
        || invocation.output_position_mask
            != static_cast<std::uint64_t>(p_minimizer_type::k_position_field_mask)) {
        result.status = minimizer_kernel_status::invalid_configuration;
        result.family_status = static_cast<std::uint32_t>(
            bounded_nthash_status::unsupported_representation);
        return false;
    }
    return ::bioseq::minimizers::kernel_detail::
        with_borrowed_compact_range<seq::dna4, p_storage_unit>(
            invocation.sequence,
            // Runs the typed checked collector against the borrowed range.
            [&invocation, &result]<class p_range>(p_range& range) {
                minimizer_sink_collector<p_minimizer_type> collector{
                    invocation.sink};
                const bounded_nthash_result native =
                    collect_minimizers_bounded_nthash_simd_checked<
                        15u, 10u, p_minimizer_type>(
                            collector,
                            range,
                            range.begin(),
                            range.end());
                result.stats.bounded_nthash = native.stats;
                result.family_status =
                    static_cast<std::uint32_t>(native.status);
                result.status = map_bounded_nthash_status(native.status);

                const bool flushed = collector.flush();
                result.emitted = collector.committed_size();
                if (!flushed || !collector.is_ok()) {
                    result.status = minimizer_kernel_status::sink_rejected;
                }
            });
}

} // namespace bounded_nthash_kernel_detail

// Definitions live in generated per-representation owners so each admitted
// algorithm body is compiled once per target/input-width identity.
/// @brief Runs the 15-bit layout with u32 packed input.
[[nodiscard]] HWY_ATTR bool bounded_nthash_repr_l15_k15_u32(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
/// @brief Runs the 15-bit layout with u64 packed input.
[[nodiscard]] HWY_ATTR bool bounded_nthash_repr_l15_k15_u64(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
/// @brief Runs the 28-bit layout with u32 packed input.
[[nodiscard]] HWY_ATTR bool bounded_nthash_repr_l28_k15_u32(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
/// @brief Runs the 28-bit layout with u64 packed input.
[[nodiscard]] HWY_ATTR bool bounded_nthash_repr_l28_k15_u64(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
/// @brief Runs the 63-bit layout with u32 packed input.
[[nodiscard]] HWY_ATTR bool bounded_nthash_repr_l63_k15_u32(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
/// @brief Runs the 63-bit layout with u64 packed input.
[[nodiscard]] HWY_ATTR bool bounded_nthash_repr_l63_k15_u64(
    minimizer_kernel_invocation&, minimizer_kernel_result&);

/**
 * @brief Dispatches one source-topology-owned bounded ntHash case.
 * @tparam p_alphabet Requested sequence alphabet.
 * @tparam p_storage_unit Packed source storage unit.
 * @param invocation Type-erased request for the exact 15/10 case.
 * @return Common kernel result with bounded ntHash family evidence.
 */
template<sequence_alphabet p_alphabet, class p_storage_unit>
requires (
    (std::same_as<p_alphabet, seq::dna4>
        || std::same_as<p_alphabet, seq::dna5>)
    && (std::same_as<p_storage_unit, std::uint8_t>
        || std::same_as<p_storage_unit, std::uint16_t>
        || std::same_as<p_storage_unit, std::uint32_t>
        || std::same_as<p_storage_unit, std::uint64_t>))
[[nodiscard]] inline minimizer_kernel_result bounded_nthash_case(
    minimizer_kernel_invocation& invocation)
{
    minimizer_kernel_result result{};
    result.stats.algorithm = minimizer_kernel_algorithm::bounded_nthash;
    result.stats.target = static_cast<std::int64_t>(HWY_TARGET);
    // Converts an admission failure into the common result representation.
    const auto reject = [&result](const bounded_nthash_status family) {
        result.status = minimizer_kernel_status::invalid_configuration;
        result.family_status = static_cast<std::uint32_t>(family);
        result.stats.bounded_nthash.fail_closed_rejections = 1u;
        return result;
    };

    if constexpr (!std::same_as<p_alphabet, seq::dna4>) {
        return reject(bounded_nthash_status::unsupported_alphabet);
    } else if constexpr (!std::same_as<p_storage_unit, std::uint32_t>
                         && !std::same_as<p_storage_unit, std::uint64_t>) {
        return reject(bounded_nthash_status::unsupported_storage);
    } else {
        if (invocation.sequence.begin_symbol
            > invocation.sequence.end_symbol) {
            result.status = minimizer_kernel_status::invalid_sequence_view;
            result.family_status = static_cast<std::uint32_t>(
                bounded_nthash_status::invalid_range);
            result.stats.bounded_nthash.fail_closed_rejections = 1u;
            return result;
        }
        if (!invocation.sequence.final_segment
            || invocation.fragments.is_present()
            || invocation.emitted_position_base != 0u
            || invocation.sequence.begin_symbol != 0u
            || invocation.window != 10u
            || invocation.kmer != 15u) {
            return reject(bounded_nthash_status::invalid_configuration);
        }
        if (invocation.output_hash_mask < 0xFFFFFFFFull
            || (invocation.sequence.end_symbol != 0u
                && invocation.sequence.end_symbol - 1u
                    > invocation.output_position_mask)) {
            return reject(bounded_nthash_status::unsupported_representation);
        }
        if (invocation.sequence.end_symbol
            < invocation.kmer + invocation.window - 1u) {
            return reject(bounded_nthash_status::input_too_short);
        }

        bool is_internally_consistent = false;
        switch (invocation.output_layout) {
            case minimizer_output_layout::sequence_log2_15_kmer_15_uint64:
                if constexpr (std::same_as<p_storage_unit, std::uint32_t>) {
                    is_internally_consistent = bounded_nthash_repr_l15_k15_u32(
                        invocation, result);
                } else {
                    is_internally_consistent = bounded_nthash_repr_l15_k15_u64(
                        invocation, result);
                }
                break;
            case minimizer_output_layout::sequence_log2_28_kmer_15_uint64:
                if constexpr (std::same_as<p_storage_unit, std::uint32_t>) {
                    is_internally_consistent = bounded_nthash_repr_l28_k15_u32(
                        invocation, result);
                } else {
                    is_internally_consistent = bounded_nthash_repr_l28_k15_u64(
                        invocation, result);
                }
                break;
            case minimizer_output_layout::sequence_log2_63_kmer_15_uint128:
                if constexpr (std::same_as<p_storage_unit, std::uint32_t>) {
                    is_internally_consistent = bounded_nthash_repr_l63_k15_u32(
                        invocation, result);
                } else {
                    is_internally_consistent = bounded_nthash_repr_l63_k15_u64(
                        invocation, result);
                }
                break;
            default:
                result.status = minimizer_kernel_status::invalid_configuration;
                result.family_status = static_cast<std::uint32_t>(
                    bounded_nthash_status::unsupported_representation);
                result.stats.bounded_nthash.fail_closed_rejections = 1u;
                break;
        }
        if (!is_internally_consistent) {
            result.stats.bounded_nthash.fail_closed_rejections = 1u;
            if (result.status == minimizer_kernel_status::ok) {
                result.status = minimizer_kernel_status::invalid_sequence_view;
                result.family_status = static_cast<std::uint32_t>(
                    bounded_nthash_status::invalid_range);
            }
        }
        return result;
    }
}

} // namespace target
} // namespace bioseq::simd
