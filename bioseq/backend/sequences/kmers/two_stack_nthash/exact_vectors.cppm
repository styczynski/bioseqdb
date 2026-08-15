/**
 * @file exact_vectors.cppm
 * @brief Exact packed-rank decode and chronological SIMD minimizer kernels.
 */
module;

#include <hwy/highway.h>
#include <libassert/assert.hpp>

export module bioseq.highway.sources.minimizer_two_stack_nthash_exact_vectors;

import std;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.highway.sources.minimizer_two_stack_nthash_state;
import bioseq.highway.sources.hasher_minimap_int;

export namespace bioseq::simd {
namespace [[bioseq::simd]] target {
namespace hn = hwy::HWY_NAMESPACE;

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



}  // namespace target
}  // namespace bioseq::simd
