/**
 * @file drivers.cppm
 * @brief Checked exact drivers and coarse route selection.
 */
module;

#include <hwy/highway.h>
#include <libassert/assert.hpp>

export module bioseq.highway.sources.minimizer_two_stack_nthash_drivers;

import std;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.instrumentation;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.highway.sources.minimizer_two_stack_nthash_state;
import bioseq.highway.sources.minimizer_two_stack_nthash_resident_stream;
import bioseq.highway.sources.minimizer_two_stack_nthash_exact_vectors;
import bioseq.highway.sources.minimizer_two_stack_nthash_continuation;

export namespace bioseq::simd {
namespace [[bioseq::simd]] target {
namespace hn = hwy::HWY_NAMESPACE;

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


}  // namespace target
}  // namespace bioseq::simd
