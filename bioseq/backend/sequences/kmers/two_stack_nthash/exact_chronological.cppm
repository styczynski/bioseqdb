/**
 * @file exact_chronological.cppm
 * @brief Builds minimizer candidates and emits them in sequence order.
 */
module;

#include <hwy/highway.h>
#include <libassert/assert.hpp>

export module bioseq.highway.sources.minimizer_two_stack_nthash_exact_chronological;

import std;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.highway.sources.minimizer_two_stack_nthash_state;
import bioseq.highway.sources.simd_utils;
import bioseq.highway.sources.minimizer_two_stack_nthash_resident_stream;
import bioseq.highway.sources.hasher_minimap_int;
import bioseq.highway.sources.minimizer_two_stack_nthash_exact_vectors;
import bioseq.highway.sources.minimizer_two_stack_nthash_exact_decode;

export namespace bioseq::simd {
namespace [[bioseq::simd]] target {
namespace hn = hwy::HWY_NAMESPACE;

namespace two_stack_internal {

/** Forward and reverse-complement k-mer codes for each SIMD lane. */
template<class p_simd_tag>
struct canonical_kmer_vectors {
    hn::Vec<p_simd_tag> forward;
    hn::Vec<p_simd_tag> reverse;
};

/** Adds the next base to each forward and reverse-complement  k-mer code. */
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

/** Hash, encoded output, and sequence-order index for each candidate lane. */
template<class p_simd_tag>
struct minimizer_candidate_vectors {
    hn::Vec<p_simd_tag> hash;
    hn::Vec<p_simd_tag> raw;
    hn::Vec<p_simd_tag> ordinal;
};

/** Updates each window minimum, choosing the later candidate on equal hashes. */
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

/**
 * Builds wide minimizer records from the selected hash and position streams.
 * Each record receives its position, strand, and runtime k-mer length.
 * Returns the collector status when the collector provides one.
 */
template<class p_minimizer_type, class p_collector_type>
requires wide_simd_minimizer_representation<p_minimizer_type>
[[nodiscard]] inline bool append_wide_minimizer_records(
    p_collector_type& collector,
    const std::uint64_t* HWY_RESTRICT hashes,
    const std::uint64_t* HWY_RESTRICT position_strands,
    std::size_t count,
    std::size_t runtime_k)
{
    static_assert(requires (
        p_collector_type& output,
        p_minimizer_type record) {
            output.collect(std::move(record));
        });
    for (std::size_t index = 0; index < count; ++index) {
        const std::uint64_t position_strand = position_strands[index];
        collector.collect(p_minimizer_type{
            hashes[index],
            position_strand >> 1u,
            position_strand & std::uint64_t{1u},
            static_cast<std::uint64_t>(runtime_k)});
    }
    if constexpr (requires (const p_collector_type& output) {
                      { output.is_ok() } -> std::convertible_to<bool>;
                  }) {
        return collector.is_ok();
    }
    return true;
}

/**
 * Processes consecutlve candidates in SIMD lanes and emits selected minimizers
 * in sequence order. Each lane finds its window minimum, with later equal
 * hashes taking  precedence. Separate tie events preserve the minimap2 output
 * order when a window first fills or its current minimum expires.
 *
 * The invalid-only mode advances the window with sentinel candidates.
 * Successful processing saves the tails needed by the next batch
 */
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
    constexpr bool k_native_output =
        exact_simd_minimizer_representation<p_minimizer_type>;
    constexpr bool k_wide_output =
        wide_simd_minimizer_representation<p_minimizer_type>;
    static_assert(two_stack_simd_minimizer_representation<p_minimizer_type>);
    static_assert(std::is_trivially_copyable_v<p_minimizer_type>);
    if constexpr (k_native_output) {
        using storage_type = typename p_minimizer_type::storage_type;
        static_assert(std::is_same_v<storage_type, std::uint64_t>);
        static_assert(sizeof(p_minimizer_type) == sizeof(storage_type));
        static_assert(requires (
            p_collector_type& output,
            const storage_type* raw,
            std::size_t count) {
                { output.append_raw_storage(raw, count) } -> std::same_as<bool>;
            });
    } else {
        static_assert(k_wide_output);
        static_assert(requires (
            p_collector_type& output,
            p_minimizer_type record) {
                output.collect(std::move(record));
            });
    }

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
    const std::uint64_t max_raw = []() constexpr {
        if constexpr (exact_simd_minimizer_representation<p_minimizer_type>) {
            return static_cast<std::uint64_t>(
                p_minimizer_type::maximum().raw_value());
        } else {
            return static_cast<std::uint64_t>(
                p_minimizer_type::maximum().pos_strand_encoded());
        }
    }();
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
        if constexpr (k_native_output) {
            candidate_raw = hn::Or(
                hn::ShiftLeftSame(
                    candidate_hash,
                    static_cast<int>(p_minimizer_type::k_hash_shift)),
                hn::Or(
                    hn::ShiftLeftSame(
                        hn::And(
                            output_position,
                            hn::Set(d, static_cast<std::uint64_t>(
                                p_minimizer_type::k_position_field_mask))),
                        static_cast<int>(p_minimizer_type::k_position_shift)),
                    strand));
        } else {
            candidate_raw = hn::Or(
                hn::ShiftLeft<1>(
                    hn::And(
                        output_position,
                        hn::Set(d, static_cast<std::uint64_t>(
                            p_minimizer_type::k_position_field_mask)))),
                strand);
        }
        candidate_raw = hn::IfThenElse(
            live_non_palindrome, candidate_raw, hn::Set(d, max_raw));
    }

    // Tail copies and vector stores initialize every element read below, so
    // these scratch arrays need no separate zero-fill.
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

    // Storing and reloading one lane earlier carries the previous minimum
    // across the whole, vector, including AVX2 and AVX-512 128-bit block boundaries.
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
    // Tie processing fills every matrix  slot it reads, so zero-fill is unnecessary.
    HWY_ALIGN std::array<std::uint64_t, event_capacity> event_raw;
    HWY_ALIGN std::array<vector_type, max_lanes> event_columns{};
    std::uint64_t first_window_tie_events = 0;
    std::uint64_t expiry_tie_events = 0;
    const vector_type v_max_raw = hn::Set(d, max_raw);

    std::size_t compressed_count = 0;

    // Check for tied records before building the event matrix. With no ties,
    // compress the previous minima directly in sequence order.
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

    if constexpr (k_native_output) {
        HWY_ALIGN std::array<std::uint64_t, event_capacity + max_lanes>
            compressed_raw;
        const vector_type v_raw_hash_mask = hn::Set(
            d, static_cast<std::uint64_t>(p_minimizer_type::k_hash_mask));
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
            // Transpose tiles of tie events so output is ordered by candidate first,
            // then by event within that candidate.
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
                        const vector_type slot_hash = hn::LoadU(
                            d, combined_hash.data() + candidate_slot);
                        const vector_type slot_raw = hn::LoadU(
                            d, combined_raw.data() + candidate_slot);
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
                const vector_type events = hn::LoadU(
                    d, event_raw.data() + offset);
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
    } else {
        // Wide records keep hash and position/strand in separae vectors. Compress
        // both with the same mask, then construct records from the selected lanes.
        HWY_ALIGN std::array<std::uint64_t, event_capacity + max_lanes>
            compressed_hash;
        HWY_ALIGN std::array<std::uint64_t, event_capacity + max_lanes>
            compressed_position_strand;
        if (hn::CountTrue(d, actual_tie_lanes) == 0) [[likely]] {
            const mask_type valid_old = hn::And(
                emit_old, hn::Ne(previous_hash, v_max_hash));
            const std::size_t hash_count = hn::CompressStore(
                previous_hash, valid_old, d, compressed_hash.data());
            compressed_count = hn::CompressStore(
                previous_raw,
                valid_old,
                d,
                compressed_position_strand.data());
            if (hash_count != compressed_count) {
                return reject_continuation(
                    state, two_stack_continuation_status::invalid_state);
            }
            add_stats_counter(state.stats.stable_compress_batches, 1);
            add_stats_counter(state.stats.direct_event_compress_batches, 1);
            add_stats_counter(state.stats.scalar_orchestration_steps, 1);
        } else {
            add_stats_counter(state.stats.tie_matrix_event_batches, 1);
            HWY_ALIGN std::array<std::uint64_t, event_capacity> event_hash;
            HWY_ALIGN std::array<vector_type, max_lanes> event_hash_columns{};
            for (std::size_t column_block = 0;
                 column_block < row_stride;
                 column_block += lanes) {
                for (std::size_t column = 0; column < lanes; ++column) {
                    const std::size_t event_slot = column_block + column;
                    if (event_slot == 0) {
                        event_columns[column] = hn::IfThenElse(
                            emit_old, previous_raw, v_max_raw);
                        event_hash_columns[column] = hn::IfThenElse(
                            emit_old, previous_hash, v_max_hash);
                    } else if (event_slot <= w) {
                        const std::size_t candidate_slot = event_slot - 1;
                        const vector_type slot_hash = hn::LoadU(
                            d, combined_hash.data() + candidate_slot);
                        const vector_type slot_raw = hn::LoadU(
                            d, combined_raw.data() + candidate_slot);
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
                        const mask_type emit_tie = hn::Or(
                            special_match, expiry_match);
                        first_window_tie_events +=
                            hn::CountTrue(d, special_match);
                        expiry_tie_events += hn::CountTrue(d, expiry_match);
                        event_columns[column] = hn::IfThenElse(
                            emit_tie, slot_raw, v_max_raw);
                        event_hash_columns[column] = hn::IfThenElse(
                            emit_tie, slot_hash, v_max_hash);
                    } else {
                        event_columns[column] = v_max_raw;
                        event_hash_columns[column] = v_max_hash;
                    }
                }
                if (!transpose_u64_lane_matrix(d, event_columns.data())
                    || !transpose_u64_lane_matrix(
                        d, event_hash_columns.data())) {
                    return reject_continuation(
                        state,
                        two_stack_continuation_status::unsupported_vector_shape);
                }
                for (std::size_t row = 0; row < lanes; ++row) {
                    const std::size_t event_offset =
                        row * row_stride + column_block;
                    hn::StoreU(
                        event_columns[row], d, event_raw.data() + event_offset);
                    hn::StoreU(
                        event_hash_columns[row],
                        d,
                        event_hash.data() + event_offset);
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
                const vector_type event_hashes = hn::LoadU(
                    d, event_hash.data() + offset);
                const vector_type event_positions = hn::LoadU(
                    d, event_raw.data() + offset);
                const mask_type valid_events = hn::Ne(
                    event_hashes, v_max_hash);
                const std::size_t hash_count = hn::CompressStore(
                    event_hashes,
                    valid_events,
                    d,
                    compressed_hash.data() + compressed_count);
                const std::size_t position_count = hn::CompressStore(
                    event_positions,
                    valid_events,
                    d,
                    compressed_position_strand.data() + compressed_count);
                if (hash_count != position_count) {
                    return reject_continuation(
                        state, two_stack_continuation_status::invalid_state);
                }
                compressed_count += hash_count;
                add_stats_counter(state.stats.stable_compress_batches, 1);
                add_stats_counter(state.stats.scalar_orchestration_steps, 1);
            }
        }
        if (compressed_count != 0
            && !append_wide_minimizer_records<p_minimizer_type>(
                collector,
                compressed_hash.data(),
                compressed_position_strand.data(),
                compressed_count,
                k)) {
            return reject_continuation(
                state, two_stack_continuation_status::collector_overflow);
        }
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

/** Advances the minimizer window with invalid candidates and their valid-run lengths. */
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

/**
 * Adds an  ambiguity-free run to the saved k-mer tail and pending SIMD batch.
 * Tracks original input positions and the length of the valid run so windows
 * after an ambiguous base follow the same emission rules as minimap2.
 */
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

/**
 * Emits the current minimum if it is valid, keeping the continuation open.
 * Updates output counters and reports collector failure
 */
template<class p_minimizer_type, class p_continuation_state, class p_collector_type>
[[nodiscard]] inline two_stack_continuation_status flush_current_minimum(
    p_continuation_state& state,
    p_collector_type& collector)
{
    using simd_tag_type = hn::ScalableTag<std::uint64_t>;
    simd_tag_type d;
    std::size_t final_count = 0u;
    if constexpr (exact_simd_minimizer_representation<p_minimizer_type>) {
        using storage_type = typename p_minimizer_type::storage_type;
        const auto current_raw = hn::Set(
            d, static_cast<std::uint64_t>(state.current_min.raw));
        const auto raw_hash_mask = hn::Set(
            d, static_cast<std::uint64_t>(p_minimizer_type::k_hash_mask));
        const auto current_valid = hn::Ne(
            hn::And(current_raw, raw_hash_mask), raw_hash_mask);
        HWY_ALIGN std::array<storage_type, p_continuation_state::max_vector_lanes>
            final_raw{};
        final_count = hn::CompressStore(
            current_raw,
            hn::And(current_valid, hn::FirstN(d, 1)),
            d,
            final_raw.data());
        if (final_count != 0
            && !collector.append_raw_storage(final_raw.data(), final_count)) {
            return reject_continuation(
                state, two_stack_continuation_status::collector_overflow);
        }
    } else {
        static_assert(wide_simd_minimizer_representation<p_minimizer_type>);
        const auto current_hash = hn::Set(
            d, static_cast<std::uint64_t>(state.current_min.hash));
        const auto current_position_strand = hn::Set(
            d, static_cast<std::uint64_t>(state.current_min.raw));
        const auto maximum_hash = hn::Set(
            d, static_cast<std::uint64_t>(p_minimizer_type::maximum().kmer()));
        const auto current_valid = hn::Ne(current_hash, maximum_hash);
        const auto final_mask = hn::And(current_valid, hn::FirstN(d, 1));
        HWY_ALIGN std::array<std::uint64_t, p_continuation_state::max_vector_lanes>
            final_hash{};
        HWY_ALIGN std::array<std::uint64_t, p_continuation_state::max_vector_lanes>
            final_position_strand{};
        const std::size_t hash_count = hn::CompressStore(
            current_hash, final_mask, d, final_hash.data());
        final_count = hn::CompressStore(
            current_position_strand,
            final_mask,
            d,
            final_position_strand.data());
        if (hash_count != final_count) {
            return reject_continuation(
                state, two_stack_continuation_status::invalid_state);
        }
        if (final_count != 0
            && !append_wide_minimizer_records<p_minimizer_type>(
                collector,
                final_hash.data(),
                final_position_strand.data(),
                final_count,
                state.kmer_size())) {
            return reject_continuation(
                state, two_stack_continuation_status::collector_overflow);
        }
    }
    add_stats_counter(state.stats.minimizers_emitted, final_count);
    add_stats_counter(state.stats.final_flush_records, final_count);
    add_stats_counter(state.stats.event_generation_batches, 1);
    add_stats_counter(state.stats.stable_compress_batches, 1);
    add_stats_counter(state.stats.scalar_orchestration_steps, 1);
    return two_stack_continuation_status::ok;
}

/**
 * Finishes pending candidates before an ambiguous base, then resets the
 * valid run and k-mer tail. One invalid candidate advances the window while
 * its history  and sequence-order count carry on.
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
