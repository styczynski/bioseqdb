/**
 * @file
 * @brief Copies Highway vector lanes into ordinary C++ vectors.
 */
module;

#include <array>
#include <bit>
#include <type_traits>
#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <hwy/aligned_allocator.h>
#include <hwy/highway.h>
#include <limits>
#include <optional>
#include <span>
#include <vector>

export module bioseq.simd.utils;

import bioseq.common.templating;
import bioseq.common.seqan_adapter;
import bioseq.common.checked_arithmetic;

export namespace bioseq::simd {

struct sequence_rank_summary {
    std::uint64_t sum{};
    std::uint8_t minimum{255}, maximum{};
    bool valid{true};
};
/** Exact widened reduction of a bounded native-rank tile. */
// Keep the Highway bodies in their owning module. Importing these helpers and
// the codec module must not emit two copies of Highway's internal-linkage APIs.
[[nodiscard]] sequence_rank_summary summarize_native_rank_chunk(
        std::span<const std::uint8_t> input, std::uint8_t rank_limit) noexcept {
    namespace hn = hwy::HWY_NAMESPACE;
    const hn::ScalableTag<std::uint8_t> tag;
    const hn::Repartition<std::uint64_t, decltype(tag)> sums_tag;
    const auto lanes = hn::Lanes(tag);
    const auto limit = hn::Set(tag, rank_limit);
    sequence_rank_summary result{};
    std::size_t offset = 0;
    for (; input.size() - offset >= lanes; offset += lanes) {
        const auto ranks = hn::LoadU(tag, input.data() + offset);
        if (!hn::AllTrue(tag, hn::Lt(ranks, limit))) { result.valid = false; return result; }
        result.sum += hn::ReduceSum(sums_tag, hn::SumsOf8(ranks));
        result.minimum = std::min(result.minimum, hn::ReduceMin(tag, ranks));
        result.maximum = std::max(result.maximum, hn::ReduceMax(tag, ranks));
    }
    for (; offset < input.size(); ++offset) {
        const auto rank = input[offset];
        if (rank >= rank_limit) { result.valid = false; return result; }
        result.sum += rank;
        result.minimum = std::min(result.minimum, rank); result.maximum = std::max(result.maximum, rank);
    }
    return result;
}

/** First unequal native rank in a bounded tile; returns length for equal tiles.
 * Full Highway vectors never cross the admitted end. Short tails are scalar. */
[[nodiscard]] std::size_t first_unequal_sequence_rank(
        const std::uint8_t* left, const std::uint8_t* right, std::size_t length) noexcept {
    namespace hn = hwy::HWY_NAMESPACE;
    const hn::ScalableTag<std::uint8_t> descriptor;
    const auto lanes = hn::Lanes(descriptor);
    std::size_t offset = 0;
    for (; length - offset >= lanes; offset += lanes) {
        const auto difference = hn::Ne(hn::LoadU(descriptor, left + offset),
                                      hn::LoadU(descriptor, right + offset));
        const auto first = hn::FindFirstTrue(descriptor, difference);
        if (first >= 0) return offset + static_cast<std::size_t>(first);
    }
    for (; offset < length; ++offset) if (left[offset] != right[offset]) return offset;
    return length;
}

/** Derive a packed XOR complement only when it represents every valid rank. */
template<class Traits, bool QueryReversed>
[[nodiscard]] constexpr std::optional<typename Traits::storage_unit>
packed_symbol_complement_mask() {
    using unit_type = typename Traits::storage_unit;
    using alphabet_type = typename Traits::alphabet_type;
    if constexpr (!QueryReversed) {
        return unit_type{0};
    } else if constexpr (requires(alphabet_type symbol) { seqan3::complement(symbol); }) {
        const auto rank_xor = seqan3::to_rank(seqan3::complement(alphabet_type{}));
        for (std::size_t rank = 0u; rank < seqan3::alphabet_size<alphabet_type>; ++rank) {
            if (seqan3::to_rank(seqan3::complement(seqan3::assign_rank_to(rank, alphabet_type{})))
                != (rank ^ rank_xor)) return std::nullopt;
        }
        unit_type mask{};
        for (std::size_t offset = 0u; offset < Traits::unit_used_bits; offset += Traits::bits_per_char)
            mask |= static_cast<unit_type>(rank_xor) << offset;
        return mask;
    } else {
        return std::nullopt;
    }
}

/** One low bit per logical symbol in complete native units of Word. */
template<class Traits, std::unsigned_integral Word = typename Traits::storage_unit>
    requires (std::numeric_limits<Word>::digits % Traits::unit_width == 0u)
[[nodiscard]] constexpr Word packed_symbol_low_bits() noexcept {
    Word result{};
    for (std::size_t unit_offset = 0u; unit_offset < std::numeric_limits<Word>::digits;
         unit_offset += Traits::unit_width) {
        for (std::size_t shift = 0u; shift < Traits::unit_used_bits; shift += Traits::bits_per_char)
            result |= Word{1} << (unit_offset + shift);
    }
    return result;
}

/** Fold each nonzero packed XOR field onto its low bit, preserving native padding.
 * Word defaults to the original native unit; explicit wider words contain only
 * complete native units and do not change the sequence's packing geometry.
 */
template<class Traits, std::unsigned_integral Word = typename Traits::storage_unit>
    requires (std::numeric_limits<Word>::digits % Traits::unit_width == 0u)
[[nodiscard]] constexpr Word packed_symbol_mismatch_bits(
    std::type_identity_t<Word> difference) noexcept {
    auto folded = difference;
    bioseq::common::templating::for_each_index<Traits::bits_per_char - 1u>(
        [difference, &folded]<std::size_t Shift>(std::integral_constant<std::size_t, Shift>) {
            folded |= difference >> (Shift + 1u);
        });
    return folded & packed_symbol_low_bits<Traits, Word>();
}

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

/** Load one forward machine-word group while preserving each native unit's padding.
 * The caller admits a complete group of logical symbols on a little-endian host.
 * A nonzero phase therefore has one additional native unit available; both memcpy
 * spans stay inside that admitted interval's physical units. No wider typed alias
 * or new packed representation is created.
 */
template<class Traits>
[[nodiscard]] inline std::uint64_t load_packed_forward_machine_word(
    const typename Traits::storage_unit* units, std::size_t phase) noexcept {
    constexpr auto machine_bits = std::numeric_limits<std::uint64_t>::digits;
    static_assert(Traits::unit_width <= machine_bits);
    static_assert(machine_bits % Traits::unit_width == 0u);
    constexpr auto lane_ones = [] {
        std::uint64_t result{};
        for (std::size_t shift = 0u; shift < machine_bits; shift += Traits::unit_width)
            result |= std::uint64_t{1} << shift;
        return result;
    }();
    constexpr auto used = lane_ones * static_cast<std::uint64_t>(Traits::used_storage_mask);
    std::uint64_t first;
    std::memcpy(&first, units, sizeof(first));
    if (phase == 0u) return first & used;
    std::uint64_t following;
    std::memcpy(&following, units + 1u, sizeof(following));
    const auto shift = phase * Traits::bits_per_char;
    const auto low = lane_ones * static_cast<std::uint64_t>(
        bioseq::simd::packed_symbol_mask<Traits>(Traits::letters_per_unit - phase));
    return ((first >> shift) & low)
        | ((following << (Traits::unit_used_bits - shift)) & (used ^ low));
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

/** Load one bounded native word in the requested logical orientation.
 * The resident owner validates coordinates and 0 < length <= letters_per_unit.
 * Reverse readers provide a
 * verified XOR complement mask and the physical total including leading slots.
 */
template<class Traits, bool QueryReversed>
[[nodiscard]] inline typename Traits::storage_unit load_oriented_packed_symbol_window(
    const typename Traits::storage_unit* units, std::size_t physical_total,
    std::size_t logical_start, std::size_t length,
    typename Traits::storage_unit complement_mask = 0) noexcept {
    if constexpr (QueryReversed) {
        const auto forward = load_packed_symbol_window<Traits>(
            units, physical_total - logical_start - length, length);
        return (reverse_packed_symbol_groups<Traits>(forward)
            >> ((Traits::letters_per_unit - length) * Traits::bits_per_char))
            ^ (complement_mask & packed_symbol_mask<Traits>(length));
    } else {
        return load_packed_symbol_window<Traits>(units, logical_start, length);
    }
}

/** Validate borrowed native packed bytes with runtime geometry and no retained state.
 * Arbitrary byte alignment and nonminimal rank widths are supported. False means
 * malformed geometry or an active rank outside the supplied alphabet cardinality.
 */
using sequence_packed_rank_validation_kernel = bool (*)(
    std::span<const std::byte>, std::size_t, std::size_t,
    std::size_t, std::size_t, std::size_t) noexcept;

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

/** Scalar ABI for one resolved packed layout and compile-time query orientation.
 * Source owners retain aligned native words. Starts and length are bounded by
 * their validated resident descriptors; query_physical_total includes leading
 * symbols so the reverse entry can recover the original physical window. */
using sequence_packed_equality_kernel = sequence_symbol_equality (*)(
    const void*, const void*, std::size_t, std::size_t, std::size_t, std::size_t);

/** Resolve one target-owned packed comparator; unsupported layouts return null. */
using sequence_packed_equality_resolver = sequence_packed_equality_kernel (*)(
    std::uint8_t, std::uint8_t, bool);

/** Bounded reader I/O capacity, independent of a target's SIMD vector width. */
inline constexpr std::size_t sequence_rank_io_chunk_capacity = 1024u;

/** Native rank-table operations supplied by the selected Highway target. */
enum class sequence_rank_transform : std::uint8_t {
    translate_triplets, synthesize_triplets, synthesize_paired_nibbles, collapse_equal_triplets, map_bytes, pack_nibbles,
    unpack_nibbles, reverse_bytes, validate_phred, validate_fastq, validate_missing_quality
};

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
