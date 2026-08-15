/**
 * @file entry_points.cppm
 * @brief Lazy-sequence Vulkan minimizer facade entry points.
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
import bioseq.sequences.kmers.hashers.base;
import bioseq.common.checked_arithmetic;
import bioseq.gpu.vulkan.abi;
import bioseq.gpu.vulkan.runtime;
import bioseq.parallelism.memory_admission;
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
 * @brief Sketches a lazy sequence through its admitted contiguous window.
 * @tparam p_minimizer_type Packed minimizer record.
 * @tparam p_alphabet Logical sequence alphabet.
 * @param sequence Lazy source sequence.
 * @param window_size Window length in k-mers.
 * @param kmer_size K-mer length in biological symbols.
 * @param decline_out Optional rejection record.
 * @param host_ownership_route Host allocation ownership contract.
 * @return Sketch result, or no value when shape or resources are rejected.
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
 * @brief Collects a lazy-sequence Vulkan sketch through an existing collector.
 * @tparam p_minimizer_type Packed minimizer record.
 * @tparam p_collector_type Collector accepting individual records.
 * @tparam p_alphabet Logical sequence alphabet.
 * @param collector Destination collector.
 * @param sequence Lazy source sequence.
 * @param window_size Window length in k-mers.
 * @param kmer_size K-mer length in biological symbols.
 * @param decline_out Optional rejection record.
 * @return Completed dispatch token, or no value when dispatch is rejected.
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
 * @brief Builds a device-resident index from one lazy target sequence.
 * @tparam p_minimizer_type Packed minimizer record.
 * @tparam p_alphabet Logical sequence alphabet.
 * @param sequence Lazy target sequence.
 * @param window_size Window length in k-mers.
 * @param kmer_size K-mer length in biological symbols.
 * @param query_gate Optional canonical query filter.
 * @param decline_out Optional rejection record.
 * @return Resident index, or no value when construction is rejected.
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
 * @brief Builds a device-resident index from one lazy target interval.
 * @tparam p_minimizer_type Packed minimizer record.
 * @tparam p_alphabet Logical sequence alphabet.
 * @param sequence Lazy target sequence.
 * @param lo_bp Inclusive biological interval boundary.
 * @param hi_bp Exclusive biological interval boundary.
 * @param window_size Window length in k-mers.
 * @param kmer_size K-mer length in biological symbols.
 * @param query_gate Optional canonical query filter.
 * @param decline_out Optional rejection record.
 * @return Resident index with local positions, or no value when rejected.
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
 * @brief Collects a strict Vulkan sketch for one lazy sequence interval.
 * @tparam p_minimizer_type Packed minimizer record.
 * @tparam p_collector_type Collector accepting individual records.
 * @tparam p_alphabet Logical sequence alphabet.
 * @param collector Destination collector.
 * @param sequence Lazy source sequence.
 * @param lo_bp Inclusive biological interval boundary.
 * @param hi_bp Exclusive biological interval boundary.
 * @param window_size Window length in k-mers.
 * @param kmer_size K-mer length in biological symbols.
 * @param query_gate Optional canonical query filter.
 * @throws vulkan_runtime_error For every selected-Vulkan rejection.
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
 * @brief Collects sorted minimizer pairs for one lazy sequence interval.
 * @tparam p_alphabet Logical sequence alphabet.
 * @param sequence Lazy source sequence.
 * @param lo_bp Inclusive uploaded interval boundary.
 * @param hi_bp Exclusive uploaded interval boundary.
 * @param emit_base_bp Global biological offset of the uploaded interval.
 * @param core_lo_bp Inclusive global core boundary.
 * @param core_hi_bp Exclusive global core boundary.
 * @param window_size Window length in k-mers.
 * @param kmer_size K-mer length in biological symbols.
 * @param execution_stats_out Optional segment execution counters.
 * @return Sorted global position and hash pairs owned with their permit.
 * @throws vulkan_runtime_error For every selected-Vulkan rejection.
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
            return collect_sorted_minimizer_pairs_vulkan_segment_resident_impl_or_throw(
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
}

} // namespace bioseq::gpu::vulkan
