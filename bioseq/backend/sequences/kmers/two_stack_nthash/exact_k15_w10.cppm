/**
 * @file exact_k15_w10.cppm
 * @brief SIMD minimizers for 15-base k-mers and 10-k-mer windows.
 */
module;

#include <hwy/highway.h>
#include <libassert/assert.hpp>

export module bioseq.highway.sources.minimizer_two_stack_nthash_exact_k15_w10;

import std;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.highway.sources.minimizer_two_stack_nthash_state;
import bioseq.highway.sources.minimizer_two_stack_nthash_resident_stream;
import bioseq.highway.sources.hasher_minimap_int;
import bioseq.highway.sources.minimizer_two_stack_nthash_exact_vectors;
import bioseq.highway.sources.minimizer_two_stack_nthash_exact_decode;

export namespace bioseq::simd {
namespace [[bioseq::simd]] target {
namespace hn = hwy::HWY_NAMESPACE;

namespace two_stack_internal {

template<class p_uint32_simd_tag, class p_uint64_simd_tag>
struct exact_fast_minimum_vectors {
    hn::Vec<p_uint32_simd_tag> hash;
    hn::Vec<p_uint64_simd_tag> raw_lower;
    hn::Vec<p_uint64_simd_tag> raw_upper;
    hn::Vec<p_uint64_simd_tag> ordinal_lower;
    hn::Vec<p_uint64_simd_tag> ordinal_upper;
};

/**
 * Finds the minima of four overlapping 10-k-mer windows. Scanning candidates
 * from left to right with <= chooses the rightmost equal hash. The encoded
 * record and sequence-order index follow the same selection mask.
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
 * Processes four consecutive minimizer windows, including tied hashes and
 * expired minima. Hash comparisons use u32 lanes. Encoded records and
 * sequence-order indices use u64 lanes. Selected records are emitted in
 * sequence order, and state keeps the last window for the next group
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

/** Packs eight DNA4 byte codes into 16 bits, with the first code in the low bits. */
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

/**
 * Packs 22 DNA4 codes into the low 44 bits. The input must have 24 readable
 * bytes because  the last eight-byte load includes two padding bytes.
 */
[[nodiscard, gnu::always_inline]] inline std::uint64_t
pack_k15_b8_dna4_codes_swar(
    const std::uint8_t* HWY_RESTRICT codes) noexcept
{
    const std::uint64_t lower = pack_eight_dna4_codes_swar(codes);
    const std::uint64_t middle = pack_eight_dna4_codes_swar(codes + 8);
    const std::uint64_t upper = pack_eight_dna4_codes_swar(codes + 16);
    return lower | (middle << 16u) | ((upper & 0x0fffull) << 32u);
}

/** Reverses the fifteen two-bit DNA4 codes in the low 30 bits of each u32 lane. */
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
 * Extracts eight consecutive 15-base k-mers from a packed 22-base block.
 * SIMD shifts select overlapping windows in pairs, then combine them into
 * two u32 vectors in input order.
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

/**
 * Builds up to eight K15 candidates and updates their W10 minimizer windows.
 * The, savej 14-base tail joins the new bases into one packed block. Two
 * groups of four compute canonical hashes and encoded output records, then
 * pass them to the window  and tie-handling code.
 */
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


}  // namespace two_stack_internal

}  // namespace target
}  // namespace bioseq::simd
