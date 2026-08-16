/**
 * @file continuation.cppm
 * @brief Fragment continuation reset, consume, and finalize operations.
 */
module;

#include <hwy/highway.h>
#include <libassert/assert.hpp>

export module bioseq.highway.sources.minimizer_two_stack_nthash_continuation;

import std;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.highway.sources.minimizer_two_stack_nthash_state;
import bioseq.highway.sources.minimizer_two_stack_nthash_exact_vectors;
import bioseq.highway.sources.minimizer_two_stack_nthash_exact_decode;
import bioseq.highway.sources.minimizer_two_stack_nthash_exact_k15_w10;
import bioseq.highway.sources.minimizer_two_stack_nthash_exact_chronological;

export namespace bioseq::simd {
namespace [[bioseq::simd]] target {
namespace hn = hwy::HWY_NAMESPACE;

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



}  // namespace target
}  // namespace bioseq::simd
