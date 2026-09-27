/**
 * @file bounded_nthash_kernel.cppm
 * @brief Selects minimizers using rolling ntHash values and fixed-size window state.
 *
 * Eight sequence regions are scanned together with SIMD. Each window keeps
 * the smallest hash and both endpoint positions when that hash occurs more
 * than once. Overlap gives neighboring regions the context needed at their
 * boundaries. The caller receives records in unspecified order.
 */
module;
#include <hwy/highway.h>

export module bioseq.highway.sources.minimizer_bounded_nthash;

import std;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.kmers.bounded_nthash_seed_policy;
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
using ::bioseq::minimizers::bounded_nthash_seed_policy;
using ::bioseq::minimizers::bounded_nthash_seeded_alphabet;
using ::bioseq::minimizers::minimizer_kernel_algorithm;
using ::bioseq::minimizers::minimizer_kernel_invocation;
using ::bioseq::minimizers::minimizer_kernel_result;
using ::bioseq::minimizers::minimizer_kernel_status;

namespace bounded_nthash_internal {

/** @brief Looks up DNA4 ntHash seeds by alphabet rank in SIMD lanes. */
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

// Read packed symbols directly, keep the rolling hash and window state,
// and send selected minimizers to the collector.

namespace bounded_nthash_internal {

/**
 * @brief Borrows an aligned DNA4 subrange as packed 32-bit words.
 * Its sizes and padding describe only that subrange, so collected positions
 * start at zero within the segment.
 */
// BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN bounded_u32_dna4_view
struct bounded_u32_dna4_view {
    /** @brief DNA4 configuration exposed by this view. */
    using output_sequence = sequence_custom_alphabet<
        seq::dna4, std::uint32_t, 2>;
    /** @brief Packed DNA4 range using 32-bit words. */
    using output_range = compact_sequence_range<output_sequence>;
    /** @brief Alphabet and storage traits for the local view. */
    using traits = typename output_range::traits;
    /** @brief Iterator over borrowed u32 packed units. */
    using iterator = const std::uint32_t*;

    std::size_t local_bp_size = 0;
    std::size_t local_view_units = 0;

    /// @brief Sets the subrange length in symbols and packed 32-bit words.
    bounded_u32_dna4_view(
        std::size_t bp_size,
        std::size_t view_units)
        : local_bp_size(bp_size),
          local_view_units(view_units) {}

    /// @brief Number of symbols in the subrange.
    [[nodiscard]] HWY_INLINE std::size_t bp_size() const noexcept {
        return local_bp_size;
    }
    /// @brief Number of bytes occupied by the packed subrange.
    [[nodiscard]] HWY_INLINE std::size_t byte_size() const noexcept {
        return local_view_units * sizeof(std::uint32_t);
    }
    /// @brief Number of packed 32-bit words in the subrange.
    [[nodiscard]] HWY_INLINE std::size_t size() const noexcept {
        return local_view_units;
    }
    /// @brief Unused symbol slots at the end of the subrange.
    [[nodiscard]] HWY_INLINE std::size_t padding() const noexcept {
        return local_view_units * traits::letters_per_unit - local_bp_size;
    }
    /// @brief Returns the packed pointer represented by an iterator.
    [[nodiscard]] HWY_INLINE const std::uint32_t* read_cptr(
        const iterator& position) const noexcept {
        return position;
    }
    /// @brief Advances by a number of packed 32-bit words.
    HWY_INLINE static void advance_iter(
        iterator& position,
        std::size_t view_units) {
        position += view_units;
    }
    /// @brief Distance between iterators in packed 32-bit words.
    [[nodiscard]] HWY_INLINE static std::size_t view_distance(
        const iterator& first,
        const iterator& last) {
        return static_cast<std::size_t>(last - first);
    }
};
// BIOSEQ_PRIVATE_PACKED_ACCESS_END bounded_u32_dna4_view

/** @brief Borrows a packed subrange using the source's storage word type. */
template<class p_base_range>
struct bounded_native_view : bit_casted_view<p_base_range, p_base_range> {
    /** @brief Packed view supplying the pointer and iterator operations. */
    using parent = bit_casted_view<p_base_range, p_base_range>;
    /** @brief Alphabet and native storage traits from the source. */
    using traits = typename parent::traits;
    /** @brief Iterator from the borrowed compact range. */
    using iterator = typename parent::iterator;

    std::size_t local_bp_size = 0;
    std::size_t local_view_units = 0;
    std::size_t local_padding = 0;

    /// @brief Sets the subrange sizes while borrowing the source storage.
    bounded_native_view(
        const p_base_range& base,
        std::size_t bp_size,
        std::size_t view_units,
        std::size_t padding)
        : parent(base),
          local_bp_size(bp_size),
          local_view_units(view_units),
          local_padding(padding) {}

    /// @brief Number of symbols in the subrange.
    [[nodiscard]] HWY_INLINE std::size_t bp_size() const noexcept {
        return local_bp_size;
    }
    /// @brief Number of bytes occupied by the packed subrange.
    [[nodiscard]] HWY_INLINE std::size_t byte_size() const noexcept {
        return local_view_units * sizeof(typename traits::storage_unit);
    }
    /// @brief Number of source storage words in the subrange.
    [[nodiscard]] HWY_INLINE std::size_t size() const noexcept {
        return local_view_units;
    }
    /// @brief Unused symbol slots at the end of the subrange.
    [[nodiscard]] HWY_INLINE std::size_t padding() const noexcept {
        return local_padding;
    }
};

/// @brief Looks up ntHash seeds for every SIMD stream.
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

/// @brief Looks up alphabet-specific forward or complement ntHash seeds.
template<
    class p_alphabet_traits,
    bool p_complement,
    class p_simd_tag,
    std::size_t p_stream_count>
requires bounded_nthash_seeded_alphabet<
    typename p_alphabet_traits::alphabet_type>
[[nodiscard]] HWY_INLINE auto lookup_alphabet_table_stream(
    p_simd_tag simd_tag,
    hn::Vec<p_simd_tag> table,
    const vector_stream<p_simd_tag, std::uint32_t, p_stream_count>& ranks) noexcept
{
    using traits = p_alphabet_traits;
    using alphabet_type = typename traits::alphabet_type;
    using storage_unit = typename traits::storage_unit;
    using vector_stream_type =
        vector_stream<p_simd_tag, std::uint32_t, p_stream_count>;

    if constexpr (std::same_as<alphabet_type, seq::dna4>) {
        // DNA4 ranks fit in one vector table lookup.
        return lookup_table_stream(simd_tag, table, ranks);
    } else {
        // Look up ranks 0..3 with a vector permutation. Select seeds for any
        // remaining alphabet ranks with SIMD comparisons.
        auto table_values = lookup_table_stream(
            simd_tag,
            table,
            if_then_else(
                less(ranks, vector_stream_type::splat(simd_tag, 4u)),
                ranks,
                vector_stream_type::splat_zero(simd_tag)));
        [&]<std::size_t... p_rank>(std::index_sequence<p_rank...>) {
            ([&] {
                if constexpr (p_rank >= 4u) {
                    constexpr std::size_t k_seed_rank = p_complement
                        ? static_cast<std::size_t>(traits::letter_complement(
                              static_cast<storage_unit>(p_rank)))
                        : p_rank;
                    constexpr std::uint32_t k_seed =
                        bounded_nthash_seed_policy<alphabet_type>::
                            forward_seeds[k_seed_rank];
                    table_values = if_then_else(
                        equal(
                            ranks,
                            vector_stream_type::splat(
                                simd_tag,
                                static_cast<std::uint32_t>(p_rank))),
                        vector_stream_type::splat(simd_tag, k_seed),
                        table_values);
                }
            }(), ...);
        }(std::make_index_sequence<traits::alphabet_size>{});
        return table_values;
    }
}

/// @brief Rotates each 32-bit lane left by p_rotation modulo 32.
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

/// @brief Rotates each 32-bit lane left by the runtime count modulo 32.
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

/// @brief Rotates each 32-bit lane right by p_rotation modulo 32.
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
 * @brief Keeps each lane's minimum hash and its leftmost and rightmost occurrences.
 * Each endpoint also keeps its strand. valid marks lanes with a candidate.
 */
template<class p_simd_tag, std::size_t p_stream_count>
struct minimum_stream {
    /** @brief One or more SIMD vectors of 32-bit values. */
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

    // Explicit special members let the generator add HWY_ATTR to each one.
    // This avoids Clang generating members without the target attributes
    // required by BF16 vector types.
    /** @brief Initializes the minimum fields and validity masks. */
    HWY_INLINE minimum_stream() noexcept
        : hash{},
          left_position{},
          right_position{},
          left_strand{},
          right_strand{},
          valid{} {}

    /** @brief Sets the minimum hash, endpoint positions, strands and valid lanes. */
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

    /** @brief Copies the minimum values and endpoint information. */
    HWY_INLINE minimum_stream(const minimum_stream& other) noexcept
        : hash(other.hash),
          left_position(other.left_position),
          right_position(other.right_position),
          left_strand(other.left_strand),
          right_strand(other.right_strand),
          valid(other.valid) {}

    /** @brief Moves the minimum values and endpoint information. */
    HWY_INLINE minimum_stream(minimum_stream&& other) noexcept
        : hash(std::move(other.hash)),
          left_position(std::move(other.left_position)),
          right_position(std::move(other.right_position)),
          left_strand(std::move(other.left_strand)),
          right_strand(std::move(other.right_strand)),
          valid(std::move(other.valid)) {}

    /** @brief Copies another minimum into this object. */
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

    /** @brief Moves another minimum into this object. */
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
 * @brief Combines two minima independently in each SIMD lane.
 *
 * A smaller hash wins. Equal hashes keep the leftmost and rightmost
 * occurrences, so reversing a window preserves its selected endpoint pair.
 * Invalid candidates are ignored.
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
 * @brief Marks the live lanes in each SIMD stream.
 * Only the last stream uses the iterator's partial-input mask. Earlier
 * streams contain complete data, including their overlap with the next region.
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

/// @brief Loads eight per-region values into the active SIMD vectors.
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

/// @brief Keeps rolling hashes and window minima for eight sequence regions.
/// K-mer and window sizes can be fixed at compile time or supplied at runtime.
template<
    class p_alphabet_traits,
    std::size_t p_letters_per_unit,
    std::size_t p_kmer_size,
    std::size_t p_window_size,
    std::size_t p_stream_count,
    class p_minimizer_type,
    class p_collector_type>
struct packed_iteration_state {
    /** @brief Packed source traits shared by rank lookup and complementation. */
    using alphabet_traits = p_alphabet_traits;
    /** @brief Nucleotide alphabet of the packed input. */
    using alphabet_type = typename alphabet_traits::alphabet_type;
    /** @brief Highway descriptor for up to eight 32-bit lanes. */
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
    /** @brief Minimum hash and endpoint positions for every lane. */
    using minimum_type = minimum_stream<simd_tag_type, p_stream_count>;

    static_assert(bounded_nthash_seeded_alphabet<alphabet_type>);
    static_assert(p_kmer_size == 0u
                  || p_kmer_size <= k_max_exact_two_stack_kmer);
    static_assert(p_window_size == 0u
                  || p_window_size <= k_bounded_nthash_max_window);
    /** Number of symbol slots needed for the selected or maximum supported k-mer size. */
    static constexpr std::size_t k_kmer_capacity = p_kmer_size == 0u
        ? k_max_exact_two_stack_kmer
        : p_kmer_size;
    /** Number of window slots needed for the selected or maximum supported window size. */
    static constexpr std::size_t k_window_capacity = p_window_size == 0u
        ? k_bounded_nthash_max_window
        : p_window_size;

    /// @brief First selected record held back until the preceding region has finished.
    struct buffered_boundary_event {
        std::uint32_t hash = 0;
        std::uint64_t absolute_last_position = 0;
        std::uint32_t strand = 0;
        bool valid = false;
    };

    /// @brief Counts emitted records and boundary duplicate handling.
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
    // Fixed-size specializations need no runtime K and W fields.
    [[no_unique_address]] std::array<
        std::size_t,
        (p_kmer_size == 0u || p_window_size == 0u) ? 2u : 0u>
        runtime_shape{};
    std::array<vector_stream_type, k_kmer_capacity> symbol_ring;
    std::size_t symbols_seen = 0;
    std::size_t next_symbol_slot = 0;

    minimum_type prefix_minimum;
    std::array<minimum_type, k_window_capacity> suffix_ring;
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

    /** @brief Returns the fixed or runtime k-mer size. */
    [[nodiscard]] HWY_INLINE constexpr std::size_t kmer_size() const noexcept {
        if constexpr (p_kmer_size != 0u) {
            return p_kmer_size;
        } else {
            return runtime_shape[0];
        }
    }

    /** @brief Returns the fixed or runtime window size. */
    [[nodiscard]] HWY_INLINE constexpr std::size_t window_size() const noexcept {
        if constexpr (p_window_size != 0u) {
            return p_window_size;
        } else {
            return runtime_shape[1];
        }
    }

    /**
     * @brief Consumes one symbol per lane and selects minima for completed windows.
     *
     * Rolling forward and reverse hashes avoid hashing each k-mer from scratch.
     * Their sum is the canonical hash. Prefix and suffix minima split the
     * sliding window, allowing its minimum to be found with fixed-size state.
     */
    template<bool p_is_full, class p_offsets_type>
    [[gnu::always_inline]] [[gnu::flatten]] HWY_INLINE void step(
        simd_tag_type simd_tag,
        const p_offsets_type& offsets,
        mask_type iterator_mask,
        vector_stream_type incoming_rank)
    {
        const std::size_t active_kmer_size = kmer_size();
        const std::size_t active_window_size = window_size();
        const auto live = correct_live_stream<
            p_is_full, simd_tag_type, p_stream_count>(
                simd_tag, iterator_mask);

        if (symbols_seen < active_kmer_size) {
            symbol_ring[symbols_seen] = incoming_rank;
            const auto forward_value = lookup_alphabet_table_stream<
                alphabet_traits, false>(
                simd_tag, k_forward_table, incoming_rank);
            const auto reverse_value = lookup_alphabet_table_stream<
                alphabet_traits, true>(
                simd_tag, complement_table, incoming_rank);
            const int forward_rotation = static_cast<int>(
                ((active_kmer_size - 1 - symbols_seen) * 7) & 31u);
            const int reverse_rotation = static_cast<int>(
                (symbols_seen * 7) & 31u);
            // Build the first k-mer hash using a rotation for each symbol position.
            forward_hash = bit_xor(
                forward_hash,
                rotate_left_stream_runtime(
                    forward_value, forward_rotation));
            reverse_hash = bit_xor(
                reverse_hash,
                rotate_left_stream_runtime(
                    reverse_value, reverse_rotation));
            ++symbols_seen;
            if (symbols_seen < active_kmer_size) return;
        } else {
            const vector_stream_type outgoing_rank =
                symbol_ring[next_symbol_slot];
            symbol_ring[next_symbol_slot] = incoming_rank;
            if (++next_symbol_slot == active_kmer_size) next_symbol_slot = 0;

            const auto forward_out = lookup_alphabet_table_stream<
                alphabet_traits, false>(
                simd_tag, k_forward_table, outgoing_rank);
            const auto forward_in = lookup_alphabet_table_stream<
                alphabet_traits, false>(
                simd_tag, k_forward_table, incoming_rank);
            const auto reverse_out = lookup_alphabet_table_stream<
                alphabet_traits, true>(
                simd_tag, complement_table, outgoing_rank);
            const auto reverse_in = lookup_alphabet_table_stream<
                alphabet_traits, true>(
                simd_tag, complement_table, incoming_rank);
            if constexpr (p_kmer_size != 0u) {
                // A fixed k-mer size lets these rotation counts be compile-time constants.
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
            } else {
                forward_hash = bit_xor(
                    bit_xor(
                        rotate_left_stream<7>(forward_hash),
                        rotate_left_stream_runtime(
                            forward_out,
                            static_cast<int>((active_kmer_size * 7u) & 31u))),
                    forward_in);
                reverse_hash = bit_xor(
                    bit_xor(
                        rotate_right_stream<7>(reverse_hash),
                        rotate_right_stream<7>(reverse_out)),
                    rotate_left_stream_runtime(
                        reverse_in,
                        static_cast<int>(((active_kmer_size - 1u) * 7u) & 31u)));
            }
            ++symbols_seen;
        }

        const std::uint32_t local_kmer_position =
            static_cast<std::uint32_t>(symbols_seen - active_kmer_size);
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
        if (++next_window_slot == active_window_size) next_window_slot = 0;
        suffix_ring[written_slot] = entering;
        prefix_minimum = combine_minima(
            simd_tag, prefix_minimum, entering);
        const bool rebuild_suffixes = next_window_slot == 0;
        if (rebuild_suffixes) {
            minimum_type suffix = suffix_ring[active_window_size - 1u];
            for (std::size_t slot = active_window_size - 1u; slot > 0; --slot) {
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
        if (candidate_index + 1 < active_window_size) return;
        const std::uint32_t window_index = static_cast<std::uint32_t>(
            candidate_index + 1 - active_window_size);
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
     * @brief Emits selected records that differ from the lane's last selection.
     *
     * Hold back the first record of each later region. Finalization compares
     * it with the preceding region to remove a duplicate at the boundary.
     * Input positions are k-mer starts. Output positions are k-mer ends.
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
                    static_cast<std::uint64_t>(offsets[logical_lane])
                        * p_letters_per_unit
                    + position_lanes[stream][lane]
                    + (kmer_size() - 1u);
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
     * @brief Emits deferred region-boundary records unless they repeat a neighbor.
     *
     * Compare each region's first selection with the last selection of the
     * nearest preceding nonempty region. Only these boundary records were
     * held back. Collector order remains unspecified.
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
                static_cast<std::uint64_t>(offsets[logical_lane])
                    * p_letters_per_unit
                + position_lanes[stream][lane]
                + (kmer_size() - 1u);
            previous_strand = strand_lanes[stream][lane];
            previous_valid = true;
        }
    }

private:
    /**
     * @brief Appends a minimizer at its k-mer end position and updates output counts.
     * tie_endpoint identifies an emitted right endpoint of a tied minimum.
     */
    HWY_INLINE void append_collector_event(
        std::uint32_t hash,
        std::uint64_t absolute_last_position,
        std::uint32_t strand,
        bool tie_endpoint)
    {
        if constexpr (std::constructible_from<
                          p_minimizer_type,
                          std::uint64_t,
                          std::uint64_t,
                          std::uint64_t,
                          std::uint64_t>) {
            collector.collect(p_minimizer_type(
                static_cast<std::uint64_t>(hash),
                absolute_last_position,
                static_cast<std::uint64_t>(strand),
                static_cast<std::uint64_t>(kmer_size())));
        } else {
            collector.collect(p_minimizer_type(
                static_cast<std::uint64_t>(hash),
                absolute_last_position,
                static_cast<std::uint64_t>(strand)));
        }
        ++evidence.minimizers_emitted;
        ++evidence.scalar_collector_appends;
        if (tie_endpoint) ++evidence.tied_endpoint_records_emitted;
    }
};

/** @brief Packs each pair of 64-bit rank vectors into one 32-bit rank vector in order. */
template<
    std::size_t p_output_stream_count,
    class p_uint32_simd_tag,
    class p_uint64_simd_tag>
[[nodiscard]] HWY_INLINE auto join_u64_rank_streams(
    p_uint32_simd_tag d32,
    p_uint64_simd_tag,
    const vector_stream<
        p_uint64_simd_tag,
        std::uint64_t,
        p_output_stream_count * 2>& ranks) noexcept
{
    const hn::Half<p_uint32_simd_tag> d32_half;
    return make_vector_stream<
        p_uint32_simd_tag, std::uint32_t, p_output_stream_count>(
        [&d32, &d32_half, &ranks](auto stream_constant) [[gnu::always_inline]] {
            constexpr std::size_t k_output_stream =
                decltype(stream_constant)::value;
            return hn::Combine(
                d32,
                hn::DemoteTo(d32_half, ranks[k_output_stream * 2 + 1]),
                hn::DemoteTo(d32_half, ranks[k_output_stream * 2]));
        });
}

/**
 * @brief Converts the last pair of 64-bit lane masks to a 32-bit lane mask.
 * The first vector is full. The second carries the iterator's tail mask.
 */
template<class p_uint32_simd_tag, class p_uint64_simd_tag>
[[nodiscard]] HWY_INLINE hn::Mask<p_uint32_simd_tag>
join_u64_iterator_mask(
    p_uint32_simd_tag d32,
    p_uint64_simd_tag d64,
    hn::Mask<p_uint64_simd_tag> iterator_mask) noexcept
{
    const hn::Half<p_uint32_simd_tag> d32_half;
    const auto lower_full = hn::FirstN(d64, hn::Lanes(d64));
    return hn::CombineMasks(
        d32,
        hn::DemoteMaskTo(d32_half, d64, iterator_mask),
        hn::DemoteMaskTo(d32_half, d64, lower_full));
}

/**
 * @brief Checks every live rank against the alphabet before emitting any records.
 * context_base_pairs gives the overlap between scanned sequence regions.
 */
template<
    std::size_t p_hash_stream_count,
    supports_compact_sequence_bitcast_view p_sequence_view,
    class p_iterator>
[[nodiscard]] inline bool validate_alphabet_ranks(
    const p_sequence_view& view,
    const p_iterator& begin,
    const p_iterator& end,
    const std::size_t context_base_pairs)
{
    using traits = typename p_sequence_view::traits;
    using storage_unit = typename traits::storage_unit;
    static_assert(bounded_nthash_seeded_alphabet<
                  typename traits::alphabet_type>);
    static_assert(std::same_as<storage_unit, std::uint32_t>
                  || std::same_as<storage_unit, std::uint64_t>);
    using hash_simd_tag_type = hn::CappedTag<std::uint32_t, 8>;
    using input_simd_tag_type = std::conditional_t<
        std::same_as<storage_unit, std::uint32_t>,
        hash_simd_tag_type,
        hn::Repartition<std::uint64_t, hash_simd_tag_type>>;
    constexpr std::size_t k_input_stream_count =
        p_hash_stream_count * (sizeof(storage_unit) / sizeof(std::uint32_t));
    static_assert(k_input_stream_count >= 1 && k_input_stream_count <= 4);
    using input_stream_type = vector_stream<
        input_simd_tag_type, storage_unit, k_input_stream_count>;
    bool valid = true;
    iterate_sequence_bp_simd<k_input_stream_count, input_simd_tag_type>(
        view,
        begin,
        end,
        context_base_pairs,
        [&valid]<bool p_is_full>(
            std::bool_constant<p_is_full>,
            const auto&,
            const input_simd_tag_type& input_simd_tag,
            hn::Mask<input_simd_tag_type> iterator_mask,
            const input_stream_type& ranks) [[gnu::always_inline]] {
            const auto live = correct_live_stream<
                p_is_full, input_simd_tag_type, k_input_stream_count>(
                    input_simd_tag, iterator_mask);
            const auto maximum_rank = hn::Set(
                input_simd_tag,
                static_cast<storage_unit>(traits::alphabet_size - 1u));
            for_each_stream<k_input_stream_count>(
                [&valid, &input_simd_tag, &live, &maximum_rank, &ranks](
                    auto stream_constant) [[gnu::always_inline]] {
                    constexpr std::size_t k_stream_index =
                        decltype(stream_constant)::value;
                    const auto invalid = hn::And(
                        live[k_stream_index],
                        hn::Gt(ranks[k_stream_index], maximum_rank));
                    valid = valid
                        && hn::AllFalse(input_simd_tag, invalid);
                });
        },
        [](const auto&, const auto&, auto&) {});
    return valid;
}

/// @brief Scans eight overlapping sequence regions and collects bounded ntHash minima.
/// Each region emits windows that start within its assigned range. Finalization
/// removes duplicate selections at region boundaries. Positions are local to view.
/// Runtime sizes are used when the corresponding template size is zero.
template<
    std::size_t p_kmer_size,
    std::size_t p_window_size,
    std::size_t p_letters_per_unit,
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
    const std::size_t runtime_kmer_size,
    const std::size_t runtime_window_size,
    bounded_nthash_execution_stats& stats)
{
    using traits = typename p_sequence_view::traits;
    using alphabet_type = typename traits::alphabet_type;
    using storage_unit = typename traits::storage_unit;
    using simd_tag_type = hn::CappedTag<std::uint32_t, 8>;
    using input_simd_tag_type = std::conditional_t<
        std::same_as<storage_unit, std::uint32_t>,
        simd_tag_type,
        hn::Repartition<std::uint64_t, simd_tag_type>>;
    constexpr std::size_t k_input_stream_count =
        p_stream_count * (sizeof(storage_unit) / sizeof(std::uint32_t));
    static_assert(bounded_nthash_seeded_alphabet<alphabet_type>);
    static_assert(std::same_as<storage_unit, std::uint32_t>
                  || std::same_as<storage_unit, std::uint64_t>);
    static_assert(k_input_stream_count >= 1 && k_input_stream_count <= 4);
    using vector_stream_type =
        vector_stream<simd_tag_type, std::uint32_t, p_stream_count>;
    using input_stream_type = vector_stream<
        input_simd_tag_type, storage_unit, k_input_stream_count>;
    using mask_stream_type = mask_stream<simd_tag_type, p_stream_count>;
    using state_type = packed_iteration_state<
        traits,
        p_letters_per_unit,
        p_kmer_size,
        p_window_size,
        p_stream_count,
        p_minimizer_type,
        p_collector_type>;
    using minimum_type = typename state_type::minimum_type;
    const simd_tag_type simd_tag;
    const input_simd_tag_type input_simd_tag;
    const std::size_t lanes = hn::Lanes(simd_tag);
    const std::size_t input_lanes = hn::Lanes(input_simd_tag);
    constexpr std::size_t k_logical_streams = 8;
    constexpr std::size_t k_letters_per_unit = p_letters_per_unit;
    const std::size_t active_kmer_size = p_kmer_size == 0u
        ? runtime_kmer_size
        : p_kmer_size;
    const std::size_t active_window_size = p_window_size == 0u
        ? runtime_window_size
        : p_window_size;
    const std::size_t context_base_pairs =
        active_kmer_size + active_window_size - 1u;
    const std::size_t context_units =
        (context_base_pairs + k_letters_per_unit - 1u)
        / k_letters_per_unit;
    const auto [units_per_lane, offsets] =
        ::bioseq::simd::iteration_geometry::calculate_iteration_offsets<
            k_logical_streams>(
            view.size(), k_logical_streams, context_units);
    const std::size_t total_windows =
        view.bp_size() - active_kmer_size - active_window_size + 2u;

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
    constexpr const auto& forward_seeds =
        bounded_nthash_seed_policy<alphabet_type>::forward_seeds;
    for (std::size_t lane = 0; lane < k_max_lanes; ++lane) {
        if constexpr (std::same_as<alphabet_type, seq::dna4>) {
            // Repeat DNA4 seeds in rank order and pair them with complement seeds.
            const std::size_t rank = lane & 3u;
            table_values[lane] = forward_seeds[rank];
            complement_values[lane] = forward_seeds[3u - rank];
        } else {
            const std::size_t rank = lane % traits::alphabet_size;
            const std::size_t complement_rank = static_cast<std::size_t>(
                traits::letter_complement(static_cast<storage_unit>(rank)));
            table_values[lane] = forward_seeds[rank];
            complement_values[lane] = forward_seeds[complement_rank];
        }
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
        .runtime_shape = {},
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
    if constexpr (p_kmer_size == 0u || p_window_size == 0u) {
        state.runtime_shape = {active_kmer_size, active_window_size};
    }
    for (std::size_t slot = 0; slot < active_window_size; ++slot) {
        state.suffix_ring[slot] = empty_minimum;
    }

    iterate_sequence_bp_simd<k_input_stream_count, input_simd_tag_type>(
        view,
        begin,
        end,
        context_base_pairs,
        // Feed each decoded symbol into the rolling hash and window state.
        [&state, &simd_tag]<bool p_is_full>(
            std::bool_constant<p_is_full>,
            const auto& iterator_offsets,
            const input_simd_tag_type& iteration_simd_tag,
            hn::Mask<input_simd_tag_type> iterator_mask,
            input_stream_type ranks) [[gnu::always_inline]] [[gnu::flatten]] {
            if constexpr (std::same_as<storage_unit, std::uint32_t>) {
                state.template step<p_is_full>(
                    iteration_simd_tag,
                    iterator_offsets,
                    iterator_mask,
                    ranks);
            } else {
                const vector_stream_type ranks_u32 =
                    join_u64_rank_streams<p_stream_count>(
                        simd_tag, iteration_simd_tag, ranks);
                const auto mask_u32 = join_u64_iterator_mask(
                    simd_tag, iteration_simd_tag, iterator_mask);
                state.template step<p_is_full>(
                    simd_tag,
                    iterator_offsets,
                    mask_u32,
                    ranks_u32);
            }
        },
        // The per-symbol callback does all the work.
        [](const auto&, const auto&, auto&) {});
    state.finalize_stream_boundaries(simd_tag, offsets);

    // Derive predictable work counts from the scan sizes after traversal.
    // This keeps those counter updates out of the per-symbol loop.
    const std::uint64_t callback_steps =
        static_cast<std::uint64_t>(context_units + units_per_lane)
        * k_letters_per_unit;
    const std::uint64_t initialization_batches =
        std::min<std::uint64_t>(callback_steps, active_kmer_size);
    const std::uint64_t rolling_batches = callback_steps > active_kmer_size
        ? callback_steps - active_kmer_size
        : 0;
    const std::uint64_t candidate_batches = callback_steps >= active_kmer_size
        ? callback_steps - active_kmer_size + 1u
        : 0;
    const std::uint64_t decision_batches = candidate_batches >= active_window_size
        ? candidate_batches - active_window_size + 1u
        : 0;
    const std::uint64_t bps_per_matrix =
        static_cast<std::uint64_t>(input_lanes) * k_letters_per_unit;
    std::uint64_t partial_callbacks = callback_steps % bps_per_matrix;
    if (partial_callbacks == 0 && callback_steps != 0) {
        partial_callbacks = bps_per_matrix;
    }
    const std::uint64_t masked_tail_lanes =
        static_cast<std::uint64_t>(view.padding())
        * (offsets.front() == offsets.back() ? input_lanes : 1u);

    stats.packed_decode_batches = callback_steps;
    stats.packed_decode_lane_visits = callback_steps * k_logical_streams
        - masked_tail_lanes;
    stats.hash_initialization_batches = initialization_batches;
    stats.rolling_hash_batches = rolling_batches;
    stats.hash_table_lookup_batches =
        2 * initialization_batches + 4 * rolling_batches;
    stats.canonical_sum_batches = candidate_batches;
    stats.window_update_batches = candidate_batches;
    stats.suffix_rebuild_batches = candidate_batches / active_window_size;
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
 * @brief Collects bounded ntHash minimizers from a packed sequence segment.
 *
 * [begin, end) must lie within sequence and follow its storage-word boundaries.
 * Rejected input leaves the collector untouched and returns an error status.
 * Accepted input emits positions relative to begin, so callers combining
 * segments must add their sequence offsets. Output order is unspecified.
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
    const p_iterator& end,
    const std::size_t runtime_kmer_size = p_kmer_size,
    const std::size_t runtime_window_size = p_window_size)
{
    using range_type = compact_sequence_range<p_sequence_range>;
    using traits = typename range_type::traits;
    using alphabet_type = typename traits::alphabet_type;
    using storage_unit = typename traits::storage_unit;
    bounded_nthash_result result;
    // Return a rejected-input status before any records reach the collector.
    auto reject = [&result](bounded_nthash_status status) {
        result.status = status;
        result.stats.fail_closed_rejections = 1;
        return result;
    };

    if constexpr (p_kmer_size > k_max_exact_two_stack_kmer
                  || p_window_size > k_bounded_nthash_max_window) {
        return reject(bounded_nthash_status::invalid_configuration);
    } else if constexpr (!bounded_nthash_seeded_alphabet<alphabet_type>) {
        return reject(bounded_nthash_status::unsupported_alphabet);
    } else if constexpr (std::same_as<alphabet_type, seq::dna4>
                         && !is_sequence_bit_castable_to<
                             p_sequence_range, std::uint32_t>) {
        return reject(bounded_nthash_status::unsupported_storage);
    } else if constexpr (std::same_as<alphabet_type, seq::dna4>
                         && (sizeof(storage_unit) < sizeof(std::uint32_t)
                             || sizeof(storage_unit) % sizeof(std::uint32_t)
                                 != 0)) {
        return reject(bounded_nthash_status::unsupported_storage);
    } else if constexpr (!std::same_as<alphabet_type, seq::dna4>
                         && !std::same_as<storage_unit, std::uint32_t>
                         && !std::same_as<storage_unit, std::uint64_t>) {
        return reject(bounded_nthash_status::unsupported_storage);
    } else if constexpr (!std::same_as<alphabet_type, seq::dna4>
                         && (traits::bits_per_char == 0u
                             || traits::bits_per_char > 8u
                             || traits::alphabet_size
                                 > (std::size_t{1u}
                                     << traits::bits_per_char))) {
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
        const std::size_t active_kmer_size = p_kmer_size == 0u
            ? runtime_kmer_size
            : p_kmer_size;
        const std::size_t active_window_size = p_window_size == 0u
            ? runtime_window_size
            : p_window_size;
        if (active_kmer_size == 0u
            || active_kmer_size > k_max_exact_two_stack_kmer
            || active_window_size == 0u
            || active_window_size > k_bounded_nthash_max_window
            || (p_kmer_size != 0u && runtime_kmer_size != p_kmer_size)
            || (p_window_size != 0u && runtime_window_size != p_window_size)) {
            return reject(bounded_nthash_status::invalid_configuration);
        }
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
        const std::size_t context_base_pairs =
            active_kmer_size + active_window_size - 1u;
        if (local_bp_size < context_base_pairs) {
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
        using simd_tag_type = hn::CappedTag<std::uint32_t, 8>;
        const std::size_t lanes = hn::Lanes(simd_tag_type{});
        if (lanes != 4 && lanes != 8) {
            return reject(bounded_nthash_status::unsupported_vector_shape);
        }

        auto run_admitted_view =
            [&result,
             &collector,
             lanes,
             local_bp_size,
             begin_units,
             end_units,
             active_kmer_size,
             active_window_size]<
                std::size_t p_view_letters_per_unit,
                class p_view,
                class p_view_iterator>(
            const p_view& view,
            const p_view_iterator& local_words,
            const p_view_iterator& local_words_end) [[gnu::always_inline]] {
            result.stats.logical_streams = 8;
            result.stats.physical_vector_lanes = lanes;
            result.stats.vector_registers_per_step = 8 / lanes;
            result.stats.input_symbols = local_bp_size;
            result.stats.input_storage_units = end_units - begin_units;
            result.stats.segments_combined = 1;
            result.stats.positions_are_segment_local = true;
            result.stats.scheme_id = k_bounded_nthash_scheme_id;
            result.stats.kmer_size = static_cast<std::uint32_t>(active_kmer_size);
            result.stats.window_size = static_cast<std::uint32_t>(active_window_size);
            result.stats.nthash_rotation = 7;
            result.stats.source_storage_unit_bytes = sizeof(storage_unit);
            static const auto* region_description =
                bioseq::instrumentation::register_static_region(
                    "mm_sketch_bounded_nthash_simd");
            auto region_scope =
                bioseq::instrumentation::region(region_description);
            if (lanes == 4) {
                bounded_nthash_internal::run_packed_kernel<
                    p_kmer_size,
                    p_window_size,
                    p_view_letters_per_unit,
                    2,
                    p_minimizer_type>(
                        collector,
                        view,
                        local_words,
                        local_words_end,
                        active_kmer_size,
                        active_window_size,
                        result.stats);
            } else {
                bounded_nthash_internal::run_packed_kernel<
                    p_kmer_size,
                    p_window_size,
                    p_view_letters_per_unit,
                    1,
                    p_minimizer_type>(
                        collector,
                        view,
                        local_words,
                        local_words_end,
                        active_kmer_size,
                        active_window_size,
                        result.stats);
            }
        };

        if constexpr (std::same_as<alphabet_type, seq::dna4>) {
            constexpr std::size_t k_uint32_letters_per_unit =
                bounded_nthash_internal::bounded_u32_dna4_view::traits::
                    letters_per_unit;
            const std::size_t local_view_units =
                (local_bp_size + k_uint32_letters_per_unit - 1)
                / k_uint32_letters_per_unit;
            const auto* local_words = reinterpret_cast<const std::uint32_t*>(
                sequence.read_cptr(begin));
            const auto* local_words_end = local_words + local_view_units;
            const bounded_nthash_internal::bounded_u32_dna4_view view(
                local_bp_size, local_view_units);
            run_admitted_view.template operator()<
                k_uint32_letters_per_unit>(
                    view, local_words, local_words_end);
        } else {
            const std::size_t local_view_units = end_units - begin_units;
            const std::size_t local_padding =
                local_view_units * k_base_letters_per_unit - local_bp_size;
            const bounded_nthash_internal::bounded_native_view<range_type> view(
                sequence, local_bp_size, local_view_units, local_padding);
            const bool ranks_are_valid = lanes == 4
                ? bounded_nthash_internal::validate_alphabet_ranks<2>(
                    view, begin, end, context_base_pairs)
                : bounded_nthash_internal::validate_alphabet_ranks<1>(
                    view, begin, end, context_base_pairs);
            if (!ranks_are_valid) {
                return reject(bounded_nthash_status::invalid_range);
            }
            run_admitted_view.template operator()<
                k_base_letters_per_unit>(view, begin, end);
        }
        return result;
    }
}
// BIOSEQ_PRIVATE_PACKED_ACCESS_END collect_minimizers_bounded_nthash_simd_checked

/**
 * @brief Collects bounded ntHash minimizers from the whole packed sequence.
 * @return The checked collector's status and work counters.
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

/// @brief Maps ntHash status to the shared kernel status.
/// Invalid ranges become invalid_sequence_view. Other failures become invalid_configuration.
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

/// @brief Runs bounded ntHash on the borrowed input and flushes records to the sink.
/// Return false if the packed input or requested output format is inconsistent.
/// result holds the kernel and sink status.
template<class p_minimizer_type, sequence_alphabet p_alphabet, class p_storage_unit>
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
        with_borrowed_compact_range<p_alphabet, p_storage_unit>(
            invocation.sequence,
            // Borrow the packed range for the checked collector call.
            [&invocation, &result]<class p_range>(p_range& range) {
                minimizer_sink_collector<p_minimizer_type> collector{
                    invocation.sink};
                const bounded_nthash_result native = [&invocation, &collector, &range] {
                    if (invocation.kmer == 15u && invocation.window == 10u) {
                        return collect_minimizers_bounded_nthash_simd_checked<
                            15u, 10u, p_minimizer_type>(
                                collector, range, range.begin(), range.end());
                    }
                    return collect_minimizers_bounded_nthash_simd_checked<
                        0u, 0u, p_minimizer_type>(
                            collector,
                            range,
                            range.begin(),
                            range.end(),
                            invocation.kmer,
                            invocation.window);
                }();
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

// Generated source files compile each output format for its CPU target
// and input word size.
/// @brief Uses 15-bit positions with packed DNA4 in 32-bit words.
[[nodiscard]] HWY_ATTR bool bounded_nthash_repr_l15_k15_u32(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
/// @brief Uses 15-bit positions with packed DNA4 in 64-bit words.
[[nodiscard]] HWY_ATTR bool bounded_nthash_repr_l15_k15_u64(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
/// @brief Uses 28-bit positions with packed DNA4 in 32-bit words.
[[nodiscard]] HWY_ATTR bool bounded_nthash_repr_l28_k15_u32(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
/// @brief Uses 28-bit positions with packed DNA4 in 64-bit words.
[[nodiscard]] HWY_ATTR bool bounded_nthash_repr_l28_k15_u64(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
/// @brief Uses 63-bit positions with packed DNA4 in 32-bit words.
[[nodiscard]] HWY_ATTR bool bounded_nthash_repr_l63_k15_u32(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
/// @brief Uses 63-bit positions with packed DNA4 in 64-bit words.
[[nodiscard]] HWY_ATTR bool bounded_nthash_repr_l63_k15_u64(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
/// @brief Uses 15-bit positions with packed DNA5 in 32-bit words.
[[nodiscard]] HWY_ATTR bool bounded_nthash_repr_l15_dna5_u32(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
/// @brief Uses 15-bit positions with packed DNA5 in 64-bit words.
[[nodiscard]] HWY_ATTR bool bounded_nthash_repr_l15_dna5_u64(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
/// @brief Uses 28-bit positions with packed DNA5 in 32-bit words.
[[nodiscard]] HWY_ATTR bool bounded_nthash_repr_l28_dna5_u32(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
/// @brief Uses 28-bit positions with packed DNA5 in 64-bit words.
[[nodiscard]] HWY_ATTR bool bounded_nthash_repr_l28_dna5_u64(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
/// @brief Uses 63-bit positions with packed DNA5 in 32-bit words.
[[nodiscard]] HWY_ATTR bool bounded_nthash_repr_l63_dna5_u32(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
/// @brief Uses 63-bit positions with packed DNA5 in 64-bit words.
[[nodiscard]] HWY_ATTR bool bounded_nthash_repr_l63_dna5_u64(
    minimizer_kernel_invocation&, minimizer_kernel_result&);

/**
 * @brief Checks a bounded ntHash request and selects its output-format implementation.
 *
 * Accepts one complete packed segment starting at zero, with no fragment list
 * or output offset. Unsupported storage, sizes and output formats return an
 * error before collection. The result includes ntHash status and work counts.
 */
template<sequence_alphabet p_alphabet, class p_storage_unit>
requires (
    bounded_nthash_seeded_alphabet<p_alphabet>
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
    // Report an unsupported request through the shared kernel result.
    const auto reject = [&result](const bounded_nthash_status family) {
        result.status = minimizer_kernel_status::invalid_configuration;
        result.family_status = static_cast<std::uint32_t>(family);
        result.stats.bounded_nthash.fail_closed_rejections = 1u;
        return result;
    };

    if constexpr (!std::same_as<p_storage_unit, std::uint32_t>
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
            || invocation.kmer == 0u
            || invocation.kmer > k_max_exact_two_stack_kmer
            || invocation.window == 0u
            || invocation.window > k_bounded_nthash_max_window) {
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
                    if constexpr (std::same_as<p_alphabet, seq::dna4>) {
                        is_internally_consistent =
                            bounded_nthash_repr_l15_k15_u32(invocation, result);
                    } else if constexpr (std::same_as<p_alphabet, seq::dna5>) {
                        is_internally_consistent =
                            bounded_nthash_repr_l15_dna5_u32(invocation, result);
                    } else {
                        is_internally_consistent = bounded_nthash_kernel_detail::
                            run_bounded_nthash<
                                minimizer_representation<15u, 15u>,
                                p_alphabet,
                                p_storage_unit>(invocation, result);
                    }
                } else {
                    if constexpr (std::same_as<p_alphabet, seq::dna4>) {
                        is_internally_consistent =
                            bounded_nthash_repr_l15_k15_u64(invocation, result);
                    } else if constexpr (std::same_as<p_alphabet, seq::dna5>) {
                        is_internally_consistent =
                            bounded_nthash_repr_l15_dna5_u64(invocation, result);
                    } else {
                        is_internally_consistent = bounded_nthash_kernel_detail::
                            run_bounded_nthash<
                                minimizer_representation<15u, 15u>,
                                p_alphabet,
                                p_storage_unit>(invocation, result);
                    }
                }
                break;
            case minimizer_output_layout::sequence_log2_28_kmer_15_uint64:
                if constexpr (std::same_as<p_storage_unit, std::uint32_t>) {
                    if constexpr (std::same_as<p_alphabet, seq::dna4>) {
                        is_internally_consistent =
                            bounded_nthash_repr_l28_k15_u32(invocation, result);
                    } else if constexpr (std::same_as<p_alphabet, seq::dna5>) {
                        is_internally_consistent =
                            bounded_nthash_repr_l28_dna5_u32(invocation, result);
                    } else {
                        is_internally_consistent = bounded_nthash_kernel_detail::
                            run_bounded_nthash<
                                minimizer_representation<28u, 15u>,
                                p_alphabet,
                                p_storage_unit>(invocation, result);
                    }
                } else {
                    if constexpr (std::same_as<p_alphabet, seq::dna4>) {
                        is_internally_consistent =
                            bounded_nthash_repr_l28_k15_u64(invocation, result);
                    } else if constexpr (std::same_as<p_alphabet, seq::dna5>) {
                        is_internally_consistent =
                            bounded_nthash_repr_l28_dna5_u64(invocation, result);
                    } else {
                        is_internally_consistent = bounded_nthash_kernel_detail::
                            run_bounded_nthash<
                                minimizer_representation<28u, 15u>,
                                p_alphabet,
                                p_storage_unit>(invocation, result);
                    }
                }
                break;
            case minimizer_output_layout::sequence_log2_63_kmer_15_uint128:
                if constexpr (std::same_as<p_storage_unit, std::uint32_t>) {
                    if constexpr (std::same_as<p_alphabet, seq::dna4>) {
                        is_internally_consistent =
                            bounded_nthash_repr_l63_k15_u32(invocation, result);
                    } else if constexpr (std::same_as<p_alphabet, seq::dna5>) {
                        is_internally_consistent =
                            bounded_nthash_repr_l63_dna5_u32(invocation, result);
                    } else {
                        is_internally_consistent = bounded_nthash_kernel_detail::
                            run_bounded_nthash<
                                minimizer_representation<63u, 15u>,
                                p_alphabet,
                                p_storage_unit>(invocation, result);
                    }
                } else {
                    if constexpr (std::same_as<p_alphabet, seq::dna4>) {
                        is_internally_consistent =
                            bounded_nthash_repr_l63_k15_u64(invocation, result);
                    } else if constexpr (std::same_as<p_alphabet, seq::dna5>) {
                        is_internally_consistent =
                            bounded_nthash_repr_l63_dna5_u64(invocation, result);
                    } else {
                        is_internally_consistent = bounded_nthash_kernel_detail::
                            run_bounded_nthash<
                                minimizer_representation<63u, 15u>,
                                p_alphabet,
                                p_storage_unit>(invocation, result);
                    }
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
