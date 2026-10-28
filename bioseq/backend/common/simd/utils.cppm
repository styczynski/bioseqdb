/**
 * @file
 * @brief Copies Highway vector lanes into ordinary C++ vectors.
 */
module;

#include <array>
#include <bit>
#include <type_traits>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <hwy/aligned_allocator.h>
#include <hwy/highway.h>
#include <limits>
#include <span>
#include <vector>

export module bioseq.simd.utils;

import bioseq.common.templating;
import bioseq.common.checked_arithmetic;

export namespace bioseq::simd {

/** Return the used low bits of a compact word without a full-width shift. */
template<class Traits>
[[nodiscard]] constexpr typename Traits::storage_unit packed_symbol_mask(std::size_t symbols) noexcept {
    return *bioseq::common::checked_low_bits_mask<typename Traits::storage_unit>(symbols * Traits::bits_per_char);
}

/** Read only the one or two packed units intersecting a bounded symbol window. */
template<class Traits>
[[nodiscard]] inline typename Traits::storage_unit load_packed_symbol_window(
    const typename Traits::storage_unit* units, std::size_t start, std::size_t length) noexcept {
    using unit_type = typename Traits::storage_unit;
    constexpr std::size_t symbols_per_unit = Traits::letters_per_unit;
    const std::size_t unit_index = start / symbols_per_unit;
    const std::size_t symbol_offset = start % symbols_per_unit;
    const std::size_t bit_offset = symbol_offset * Traits::bits_per_char;
    unit_type packed = (units[unit_index] & Traits::used_storage_mask) >> bit_offset;
    if (symbol_offset != 0 && symbol_offset + length > symbols_per_unit) {
        packed |= (units[unit_index + 1] & Traits::used_storage_mask)
            << (Traits::unit_used_bits - bit_offset);
    }
    return packed & packed_symbol_mask<Traits>(length);
}

/** Reverse symbol groups using a derived bit permutation where geometry permits it. */
template<class Traits>
[[nodiscard]] constexpr typename Traits::storage_unit reverse_packed_symbol_groups(
    typename Traits::storage_unit packed) noexcept {
    using unit_type = typename Traits::storage_unit;
    if constexpr (std::has_single_bit(Traits::bits_per_char) && Traits::unit_leftover_bits == 0) {
        for (std::size_t step = Traits::bits_per_char; step < Traits::unit_width; step *= 2) {
            unit_type low_groups = 0;
            for (std::size_t offset = 0; offset < Traits::unit_width; offset += step * 2)
                low_groups |= *bioseq::common::checked_low_bits_mask<unit_type>(step) << offset;
            packed = ((packed & low_groups) << step) | ((packed >> step) & low_groups);
        }
        return packed;
    } else {
        unit_type reversed = 0;
        for (std::size_t symbol = 0; symbol < Traits::letters_per_unit; ++symbol) {
            reversed = (reversed << Traits::bits_per_char) | (packed & Traits::char_mask);
            packed >>= Traits::bits_per_char;
        }
        return reversed;
    }
}

/** Whether a bounded equality scan must reject ranks outside a supplied alphabet. */
enum class sequence_rank_validation : std::uint8_t { none, upper_bound };

/** Counts from a bounded SIMD equality scan, without allocating full sequences. */
struct sequence_symbol_equality {
    std::uint64_t equal_symbols{};
    std::size_t vector_batches{};
    bool valid_ranks{true};
};

/** Scalar-only ABI for a caller-selected Highway rank comparison kernel. */
using sequence_rank_equality_kernel = sequence_symbol_equality (*)(
    const std::uint8_t*, const std::uint8_t*, std::size_t,
    sequence_rank_validation, std::size_t);

/** Bounded reader I/O capacity, independent of a target's SIMD vector width. */
inline constexpr std::size_t sequence_rank_io_chunk_capacity = 1024u;

/** Native rank-table operations supplied by the selected Highway target. */
enum class sequence_rank_transform : std::uint8_t { translate_triplets, synthesize_triplets };

/** Scalar-only ABI; input/output are disjoint bounded native-rank tiles.
 * Forward lookup uses one entry per source triplet; inverse entries pack three
 * destination rank bytes. False reports a malformed shape or source rank. */
using sequence_rank_transform_kernel = bool (*)(sequence_rank_transform,
    std::span<const std::uint8_t>, std::size_t,
    std::span<const std::uint32_t>, std::span<std::uint8_t>);

/**
 * Count selected sequence windows through bounded byte readers and a selected
 * target-owned numeric kernel. No Highway tag/vector crosses this neutral API.
 * The fixed I/O chunk size is independent of vector width; target kernels own
 * their own MaxLanes-sized final-vector scratch.
 */
template<bool p_query_reversed,
         sequence_rank_validation p_validation = sequence_rank_validation::none,
         class QueryReader, class TargetReader>
[[nodiscard]] inline sequence_symbol_equality count_equal_sequence_symbols(
        const QueryReader& query, const TargetReader& target,
        std::size_t query_start, std::size_t target_start, std::size_t length,
        sequence_rank_equality_kernel counter, std::size_t rank_limit = 0u) {
    constexpr std::size_t chunk_capacity = sequence_rank_io_chunk_capacity;
    std::array<std::uint8_t, chunk_capacity> query_chunk;
    std::array<std::uint8_t, chunk_capacity> target_chunk;
    sequence_symbol_equality result;
    if (counter == nullptr) {
        result.valid_ranks = false;
        return result;
    }
    for (std::size_t consumed = 0u; consumed < length;) {
        const std::size_t chunk_size = std::min(chunk_capacity, length - consumed);
        const auto query_source = query.template borrow_rank_source<p_query_reversed>();
        const auto target_source = target.template borrow_rank_source<false>();
        const auto query_read = query_source.read_checked(query_start + consumed, chunk_size, query_chunk);
        if (!query_read) { result.valid_ranks = false; return result; }
        // Preserve the bounded query window before an aliased target reader may evict it.
        if (query_read->data() != query_chunk.data())
            std::copy(query_read->begin(), query_read->end(), query_chunk.begin());
        const auto target_read = target_source.read_checked(target_start + consumed, chunk_size, target_chunk);
        if (!target_read) { result.valid_ranks = false; return result; }
        if (target_read->data() != target_chunk.data())
            std::copy(target_read->begin(), target_read->end(), target_chunk.begin());
        const auto compared = counter(query_chunk.data(), target_chunk.data(),
            chunk_size, p_validation, rank_limit);
        result.equal_symbols += compared.equal_symbols;
        result.vector_batches += compared.vector_batches;
        if (!compared.valid_ranks) {
            result.valid_ranks = false;
            return result;
        }
        consumed += chunk_size;
    }
    return result;
}

} // namespace bioseq::simd

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

/// @brief Makes the shared compile-time index, loop available in this SIMD namespace.
using ::bioseq::common::templating::for_each_index;

/**
 * @brief Copies the active SIMD lanes into a C++ vector in lane order
 * @param[in] simd_tag Highway descriptor giving the active lne count.
 * @param[in] vector Vector to copy.
 * @tparam p_value_type Scalar lane type.
 */
template <typename p_value_type, typename p_simd_tag>
HWY_INLINE auto extract_lanes(const p_simd_tag& simd_tag, const hn::Vec<p_simd_tag>& vector)
    -> std::vector<p_value_type> {
#if HWY_TARGET == HWY_RVV
    auto storage = hwy::AllocateAligned<p_value_type>(hn::Lanes(simd_tag));
    p_value_type* HWY_RESTRICT storage_buffer = storage.get();
#else
    HWY_ALIGN std::array<p_value_type, hn::MaxLanes(p_simd_tag{})> storage;
    p_value_type* storage_buffer = storage.data();
#endif
    hn::StoreU(vector, simd_tag, storage_buffer);
    return std::vector<p_value_type>(
        storage_buffer,
        storage_buffer + hn::Lanes(simd_tag));
}

/**
 * @brief Copies a SIMD mask into a C++ vector.
 *
 * Each false lane has all bits clear. Each true lane has all bits set, using
 * Highway's vector representation of the mask. Lane order is preserved.
 */
template <typename p_value_type, typename p_simd_tag>
HWY_INLINE auto extract_lanes(const p_simd_tag& simd_tag, const hn::Mask<p_simd_tag>& mask)
    -> std::vector<p_value_type> {
    return extract_lanes<p_value_type>(
        simd_tag,
        hn::VecFromMask(simd_tag, mask));
}

} // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();

}
