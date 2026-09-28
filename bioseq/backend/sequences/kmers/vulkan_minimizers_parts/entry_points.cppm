/**
 * @file entry_points.cppm
 * @brief Vulkan sketching and index building for lazy  sequence views.
 */
module;

#include <libassert/assert.hpp>

#include <cstring>
#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.sequences.kmers.vulkan_minimizers:entry_points;

import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.sequences.kmers.minimizer_physical_registry;
import bioseq.sequences.kmers.hashers.base;
import bioseq.common.checked_arithmetic;
import bioseq.gpu.vulkan.abi;
import bioseq.gpu.vulkan.runtime;
import bioseq.parallelism.memory_admission;
import bioseq.pipeline.runtime_contract;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.instrumentation;
import :contract;
import :upload;
import :memory;
import :sketch_dispatch;
import :index_materialization;
import :batch;
import :index_routes;
import :segmented;

export namespace bioseq::gpu::vulkan {

/**
 * @brief Chooses the GPU record layout for a segment-pair sketch
 *
 * Uses minimap_sketch_data when its registered profile and field widths fit.
 * Otherwise, asks the generated registry for a layozt and checks that the
 * selected algorithm remains Vulkan sketching.
 * @param window_size K-mers per window.
 * @param kmer_size Symbols per k-mer.
 * @param maximum_window_bp Maximum upload length in symbols.
 * @param callable Called with the chosen record type as a template argument.
 * @return Callback result.
 * @throws vulkan_runtime_error If no supported record layout fits.
 */
template <sequence_alphabet p_alphabet, class p_callable>
[[nodiscard]] auto with_vulkan_segment_pair_record(
    std::size_t window_size,
    std::size_t kmer_size,
    std::uint64_t maximum_window_bp,
    p_callable&& callable)
    -> decltype(std::forward<p_callable>(callable).template operator()<
        bioseq::minimizers::minimap_sketch_data>())
{
    namespace minimizers = bioseq::minimizers;
    using legacy_record = minimizers::minimap_sketch_data;
    constexpr auto algorithm =
        ::bioseq::simd::minimizer_algorithm::vulkan_minimap_integer;
    if (kmer_size == 0u || kmer_size > 32u
        || !vulkan_minimizer_window_size_supported(window_size)
        || window_size > (std::numeric_limits<std::uint16_t>::max)()) {
        throw vulkan_runtime_error{
            "Vulkan segment pair shape exceeds its runtime kernel capacity"};
    }
    if constexpr (!minimizers::minimizer_profile_has_alphabet<p_alphabet>()) {
        throw vulkan_runtime_error{
            "Vulkan segment pair alphabet has no generated record profile"};
    } else {
        const auto runtime_k = static_cast<std::uint16_t>(kmer_size);
        const auto runtime_w = static_cast<std::uint16_t>(window_size);
        constexpr auto alphabet_id =
            minimizers::minimizer_profile_alphabet_id<p_alphabet>();
        constexpr auto legacy_id =
            minimizers::minimizer_physical_record_detail::record_id<legacy_record>();
        const std::uint64_t required_hash_mask = kmer_size == 32u
            ? (std::numeric_limits<std::uint64_t>::max)()
            : (std::uint64_t{1u} << (2u * kmer_size)) - 1u;
        const auto* legacy_descriptor =
            minimizers::find_minimizer_physical_record(legacy_id);
        if (legacy_descriptor != nullptr
            && maximum_window_bp <= legacy_record::k_position_field_mask
            && required_hash_mask <= legacy_record::k_hash_field_mask) {
            const auto* profile = minimizers::find_minimizer_runtime_profile(
                algorithm, alphabet_id, runtime_k, runtime_w,
                legacy_descriptor->position_log2, 32u);
            if (profile != nullptr && profile->record_id == legacy_id
                && profile->effective_algorithm == algorithm) {
                return std::forward<p_callable>(callable).
                    template operator()<legacy_record>();
            }
        }
        try {
            const auto layout = minimizers::select_minimizer_physical_layout<
                p_alphabet, std::uint32_t>(
                    algorithm, maximum_window_bp, runtime_k, runtime_w);
            if (layout.effective_algorithm != algorithm) {
                throw vulkan_runtime_error{
                    "Vulkan segment pair record selection changed the algorithm"};
            }
            return minimizers::visit_minimizer_physical_layout(
                layout, std::forward<p_callable>(callable));
        } catch (const std::invalid_argument& error) {
            throw vulkan_runtime_error{
                std::string{"Vulkan segment pair record selection failed: "}
                    + error.what()};
        }
    }
}

/**
 * @brief Sketches a lazy sequence and returns host and device minimizer buffers.
 *
 * Borrows compatible packed input or reserves memory for a packed copy.
 * @param sequence Source sequence.
 * @param window_size K-mers per  window.
 * @param kmer_size Symbols per k-mer.
 * @param decline_out Optional rejection details.
 * @param host_ownership_route How the caller will keep the host buffer alive.
 * @return Sketch result, or no value when input or memory checks reject it.
 */
template <class p_minimizer_type, sequence_alphabet p_alphabet>
[[nodiscard]] std::optional<vulkan_sketch_result<p_minimizer_type>>
collect_minimizers_vulkan_result(
    bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
    std::size_t window_size,
    std::size_t kmer_size,
    vulkan_sketch_decline* decline_out = nullptr,
    vulkan_sketch_host_ownership_route host_ownership_route =
        vulkan_sketch_host_ownership_route::collector_compatible)
{
    if (!admitted_sequence_detail::validate_facade_sketch_shape(
            sequence, window_size, kmer_size, decline_out)) {
        return std::nullopt;
    }
    auto window = admitted_sequence_detail::prepare_canonical_u32_window(
        sequence, 0u, sequence.bp_size(), decline_out);
    if (!window) {
        return std::nullopt;
    }
    return admitted_sequence_detail::with_canonical_u32_upload_source<p_alphabet>(
        *window,
        [window_size, kmer_size, decline_out, host_ownership_route]
        <class p_range>(p_range& resident) {
            return collect_minimizers_vulkan_result_resident_impl<p_minimizer_type>(
                resident,
                window_size,
                kmer_size,
                decline_out,
                host_ownership_route);
        });
}

/**
 * @brief Sketches a lazy sequence and sends downloaded records to a collector.
 * @param collector Receives each minimizer.
 * @param sequence Source sequence.
 * @param window_size K-mers per window.
 * @param kmer_size Symbols per k-mer.
 * @param decline_out Optional rejection details.
 * @return Completion token, or no value when sketching is rejected.
 */
template <class p_minimizer_type, class p_collector_type, sequence_alphabet p_alphabet>
[[nodiscard]] std::optional<vulkan_sketch_dispatch_token>
collect_minimizers_vulkan(
    p_collector_type& collector,
    bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
    std::size_t window_size,
    std::size_t kmer_size,
    vulkan_sketch_decline* decline_out = nullptr)
{
    auto result = collect_minimizers_vulkan_result<p_minimizer_type>(
        sequence, window_size, kmer_size, decline_out);
    if (!result) {
        return std::nullopt;
    }
    for (const p_minimizer_type& minimizer : result->host_minimizers) {
        collector.collect(minimizer);
    }
    if (!result->dispatch_token.is_internally_consistent()) {
        throw vulkan_runtime_error{
            "Vulkan minimizer sketch returned without a completed dispatch token"};
    }
    return result->dispatch_token;
}

/**
 * @brief Builds a target minimizer index in device memory from a lazy sequence.
 *
 * Borrows compatible packed input or reserves memory for a packed copy.
 * @param sequence Target sequence.
 * @param window_size K-mers per window.
 * @param kmer_size Symbols per k-mer.
 * @param query_gate Optional bitmap filtering  target hashes.
 * @param decline_out Optional, rejection details.
 * @return Device index, or no value when construction is rejected.
 */
template <class p_minimizer_type, sequence_alphabet p_alphabet>
[[nodiscard]] std::optional<gpu_built_index> build_gpu_index_on_device(
    bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
    std::size_t window_size,
    std::size_t kmer_size,
    vulkan_sketch_query_gate query_gate = {},
    vulkan_sketch_decline* decline_out = nullptr)
{
    if (!admitted_sequence_detail::validate_facade_sketch_shape(
            sequence, window_size, kmer_size, decline_out)) {
        return std::nullopt;
    }
    auto window = admitted_sequence_detail::prepare_canonical_u32_window(
        sequence, 0u, sequence.bp_size(), decline_out);
    if (!window) {
        return std::nullopt;
    }
    return admitted_sequence_detail::with_canonical_u32_upload_source<p_alphabet>(
        *window,
        [window_size, kmer_size, query_gate, decline_out]
        <class p_range>(p_range& resident) {
            return build_gpu_index_on_device_resident_impl<p_minimizer_type>(
                resident,
                window_size,
                kmer_size,
                query_gate,
                decline_out);
        });
}

/**
 * @brief Builds a GPU target index for a lazy sequence ingerval
 *
 * Index positions are relative to lo_bp. The interval is packed or borrowed
 * as contiguous input before the synchronous build.
 * @param sequence Target sequence.
 * @param lo_bp First symbol, inclusive.
 * @param hi_bp Last symbol, exclusive.
 * @param window_size K-mers per window.
 * @param kmer_size Symbols per k-mer.
 * @param query_gate Optional bitmap filtering target hashes.
 * @param decline_out Optional rejection details.
 * @return Device index, or no value when the interval or build is rejected.
 */
template <class p_minimizer_type, sequence_alphabet p_alphabet>
[[nodiscard]] std::optional<gpu_built_index> build_gpu_segment_index_on_device(
    bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
    bioseq::storage::sequence_size_t lo_bp,
    bioseq::storage::sequence_size_t hi_bp,
    std::size_t window_size,
    std::size_t kmer_size,
    vulkan_sketch_query_gate query_gate = {},
    vulkan_sketch_decline* decline_out = nullptr)
{
    if (!admitted_sequence_detail::validate_facade_sketch_shape(
            sequence, window_size, kmer_size, decline_out)) {
        return std::nullopt;
    }
    if (lo_bp > hi_bp || hi_bp > sequence.bp_size()) {
        if (decline_out != nullptr) {
            decline_out->reason =
                vulkan_sketch_decline_reason::sequence_type_unsupported;
        }
        return std::nullopt;
    }
    auto window = admitted_sequence_detail::prepare_canonical_u32_window(
        sequence, lo_bp, hi_bp, decline_out);
    if (!window) {
        return std::nullopt;
    }
    return admitted_sequence_detail::with_canonical_u32_upload_source<p_alphabet>(
        *window,
        [window_size, kmer_size, query_gate, decline_out]
        <class p_range>(p_range& resident) {
            return build_gpu_segment_index_on_device_resident_impl<p_minimizer_type>(
                resident,
                0u,
                static_cast<std::size_t>(resident.size()),
                window_size,
                kmer_size,
                query_gate,
                decline_out);
        });
}

/**
 * @brief Sketches a lazy sequence interval on the GPU and sends records to a collector.
 *
 * Positions are relative to lo_bp. Input is borrowed or packed before  dispatch,
 * and large intervals can be split into overlapping uploads. A query gate requires
 * a single upload.
 * @param collector Receives minimizers.
 * @param sequence Source sequence.
 * @param lo_bp First symbol, inclusive.
 * @param hi_bp Last symbol, exclusive.
 * @param window_size K-mers per window.
 * @param kmer_size Symbols per k-mer.
 * @param query_gate Optional bitmap filtering hashes.
 * @throws vulkan_runtime_error If input checks or Vulkan execution fail.
 */
template <class p_minimizer_type, class p_collector_type, sequence_alphabet p_alphabet>
void collect_minimizers_vulkan_segment_or_throw(
    p_collector_type& collector,
    bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
    bioseq::storage::sequence_size_t lo_bp,
    bioseq::storage::sequence_size_t hi_bp,
    std::size_t window_size,
    std::size_t kmer_size,
    vulkan_sketch_query_gate query_gate = {})
{
    vulkan_sketch_decline decline{};
    if (!admitted_sequence_detail::validate_facade_sketch_shape(
            sequence, window_size, kmer_size, &decline)
        || lo_bp > hi_bp || hi_bp > sequence.bp_size()) {
        if (!decline.declined()) {
            decline.reason = vulkan_sketch_decline_reason::sequence_type_unsupported;
        }
        throw_vulkan_strict_decline(
            "Vulkan segment minimizer sketch", decline);
    }
    auto window = admitted_sequence_detail::prepare_canonical_u32_window(
        sequence, lo_bp, hi_bp, &decline);
    if (!window) {
        throw_vulkan_strict_decline(
            "Vulkan segment minimizer sketch", decline);
    }
    admitted_sequence_detail::with_canonical_u32_upload_source<p_alphabet>(
        *window,
        [&collector, window_size, kmer_size, query_gate]
        <class p_range>(p_range& resident) {
            collect_minimizers_vulkan_segment_resident_impl_or_throw<p_minimizer_type>(
                collector,
                resident,
                0u,
                static_cast<std::size_t>(resident.size()),
                window_size,
                kmer_size,
                query_gate);
        });
}

/**
 * @brief Returns sorted minimizer hash-position pairs for a lazy sequence interval.
 *
 * Chooses a record layout that fits the k-mer and upload length, then sketches
 * overlapping GPU windows and keeps unique pairs in the requested global  core.
 * @param sequence Source sequence.
 * @param lo_bp First uploaded symbol, inclusive.
 * @param hi_bp Last uploaded symbol, exclusive.
 * @param emit_base_bp Global symbl offset assigned to the upload start.
 * @param core_lo_bp First global output position, inclusive.
 * @param core_hi_bp Last global output position, exclusive.
 * @param window_size K-mers per window.
 * @param kmer_size Symbols per k-mer
 * @param execution_stats_out Optional completed-dispatch and split counts.
 * @return Pairs of hash and (global position << 1 | strand), sorted in that order,
 * with their memory reservation.
 * @throws vulkan_runtime_error If input checks or Vulkan execution fail.
 */
template <sequence_alphabet p_alphabet>
[[nodiscard]] vulkan_owned_segment_pairs
collect_sorted_minimizer_pairs_vulkan_segment_or_throw(
    bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
    bioseq::storage::sequence_size_t lo_bp,
    bioseq::storage::sequence_size_t hi_bp,
    bioseq::storage::sequence_size_t emit_base_bp,
    bioseq::storage::sequence_size_t core_lo_bp,
    bioseq::storage::sequence_size_t core_hi_bp,
    std::size_t window_size,
    std::size_t kmer_size,
    vulkan_segment_pair_execution_stats* execution_stats_out = nullptr)
{
    vulkan_sketch_decline decline{};
    if (!admitted_sequence_detail::validate_facade_sketch_shape(
            sequence, window_size, kmer_size, &decline)
        || lo_bp > hi_bp || hi_bp > sequence.bp_size()) {
        if (!decline.declined()) {
            decline.reason = vulkan_sketch_decline_reason::sequence_type_unsupported;
        }
        throw_vulkan_strict_decline(
            "Vulkan segment minimizer pair path", decline);
    }
    auto window = admitted_sequence_detail::prepare_canonical_u32_window(
        sequence, lo_bp, hi_bp, &decline);
    if (!window) {
        throw_vulkan_strict_decline(
            "Vulkan segment minimizer pair path", decline);
    }
    return admitted_sequence_detail::with_canonical_u32_upload_source<p_alphabet>(
        *window,
        [emit_base_bp, core_lo_bp, core_hi_bp, window_size, kmer_size,
         execution_stats_out]
        <class p_range>(p_range& resident) {
            return with_vulkan_segment_pair_record<p_alphabet>(
                window_size, kmer_size,
                static_cast<std::uint64_t>(resident.bp_size()),
                [&]<class p_minimizer_type>() {
                    return collect_sorted_minimizer_pairs_vulkan_segment_resident_impl_or_throw<
                        p_minimizer_type>(
                            resident,
                            0u,
                            static_cast<std::size_t>(resident.size()),
                            emit_base_bp,
                            core_lo_bp,
                            core_hi_bp,
                            window_size,
                            kmer_size,
                            execution_stats_out);
                });
        });
}

} // namespace bioseq::gpu::vulkan
