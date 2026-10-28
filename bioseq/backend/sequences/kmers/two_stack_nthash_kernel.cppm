/**
 * @file two_stack_nthash_kernel.cppm
 * @brief Generated entry points for the exact two-stack minimizer kernels.
 */
module;

#include <hwy/highway.h>
#include <libassert/assert.hpp>

export module bioseq.highway.sources.minimizer_two_stack_nthash;

import std;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.sequences.kmers.minimizer_physical_registry;
import bioseq.highway.sources.minimizer_two_stack_nthash_state;
import bioseq.highway.sources.minimizer_two_stack_nthash_drivers;

export namespace bioseq::simd {
namespace [[bioseq::simd]] target {
namespace hn = hwy::HWY_NAMESPACE;

/**
 * Runs a two-stack minimizer request on this Highway target. Checks the
 * window, k-mer  length, and output position range, before selecting a typed
 * output record and driver. The driver keeps all SIMD state inside the call
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

    // The selected driver checks that both requested masks match the output
    // record. The caller must choose compatible layouts for the index and query.
    bool is_internally_consistent = false;
    bool admitted_record = false;
    const bool found = ::bioseq::minimizers::visit_minimizer_output_layout(
        invocation.output_layout,
        [&invocation, &result, &is_internally_consistent, &admitted_record]<class p_record>() {
            if constexpr (::bioseq::minimizers::two_stack_simd_minimizer_representation<p_record>) {
                admitted_record = true;
                is_internally_consistent = coarse_two_stack_detail::run_selected_output_layout<
                    p_record, p_alphabet, p_storage_unit>(invocation, result);
            }
        });
    if (!found || !admitted_record) return reject_output_layout();
    if (!is_internally_consistent && result.status == minimizer_kernel_status::ok) {
        result.status = minimizer_kernel_status::invalid_sequence_view;
    }
    return result;
}

/** Checks packed input dimensions before running a biological minimizer case. */
template<sequence_alphabet p_alphabet, class p_storage_unit>
requires (
    (seqan3::nucleotide_alphabet<p_alphabet> || seqan3::aminoacid_alphabet<p_alphabet>)
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

}  // namespace target
}  // namespace bioseq::simd
