/**
 * @file index_routes.cppm
 * @brief Resident whole-target and segment index routes.
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

export module bioseq.sequences.kmers.vulkan_minimizers:index_routes;

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

export namespace bioseq::gpu::vulkan {

/**
 * @brief Builds a device-resident minimizer index from a contiguous sequence.
 * @tparam p_minimizer_type Packed minimizer record emitted by the sketch kernel.
 * @tparam p_sequence Contiguous packed sequence type.
 * @param seq Target sequence.
 * @param window_size Window length in k-mers.
 * @param kmer_size K-mer length in biological symbols.
 * @param query_gate Optional canonical query filter.
 * @param decline_out Optional rejection record.
 * @return Resident index, or no value when shape or resources are rejected.
 * @throws vulkan_runtime_error If Vulkan execution fails.
 */
template <class p_minimizer_type, class p_sequence>
[[nodiscard]] std::optional<gpu_built_index> build_gpu_index_on_device_resident_impl(
    const p_sequence& seq,
    std::size_t window_size,
    std::size_t kmer_size,
    vulkan_sketch_query_gate query_gate = {},
    vulkan_sketch_decline* decline_out = nullptr)
{
    static const auto* index_build_region_description =
        bioseq::instrumentation::register_static_region("gpu_index_build");
    auto index_build_region_scope =
        bioseq::instrumentation::region(index_build_region_description);
    using bare = std::remove_cvref_t<p_sequence>;
    if (decline_out != nullptr) {
        *decline_out = vulkan_sketch_decline{
            .bp_size = vulkan_decline_bp_size(seq),
            .query_gate_words = static_cast<std::uint64_t>(
                query_gate.words.size()),
            .window_size = window_size,
            .kmer_size = kmer_size,
        };
    }
    const auto decline = [decline_out](
            vulkan_sketch_decline_reason reason) noexcept {
        if (decline_out != nullptr) {
            decline_out->reason = reason;
        }
    };
    if constexpr (!vulkan_sketchable_sequence<bare>) {
        decline(vulkan_sketch_decline_reason::sequence_type_unsupported);
        return std::nullopt;
    } else {
        using traits = typename bare::traits;
        if constexpr (traits::alphabet_size > 256 || traits::bits_per_char == 0 ||
                      traits::bits_per_char > 32) {
            decline(vulkan_sketch_decline_reason::alphabet_unsupported);
            return std::nullopt;
        } else {
            constexpr std::size_t step_bits =
                choose_alphabet_kernel<bare>() == alphabet_kernel_kind::generic
                    ? traits::bits_per_char : std::size_t{2};
            if (kmer_size == 0 || kmer_size > 64 / step_bits || kmer_size > 32) {
                decline(vulkan_sketch_decline_reason::kmer_size_unsupported);
                return std::nullopt;
            }
            if (window_size == 0 || window_size > 256) {
                decline(vulkan_sketch_decline_reason::window_size_unsupported);
                return std::nullopt;
            }

            const sequence_upload_view upload = make_sequence_upload_view(seq);
            const vulkan_rank_lut rank_lut = make_vulkan_rank_lut<bare>();
            return build_gpu_index_from_upload_on_device<p_minimizer_type>(
                upload,
                rank_lut,
                window_size,
                kmer_size,
                query_gate,
                decline_out);
        }
    }
}

/**
 * @brief Builds a device-resident minimizer index for one packed segment.
 * @tparam p_minimizer_type Packed minimizer record emitted by the sketch kernel.
 * @tparam p_sequence Contiguous packed sequence type.
 * @param seq Target sequence.
 * @param lo_unit Inclusive packed unit index.
 * @param hi_unit Exclusive packed unit index.
 * @param window_size Window length in k-mers.
 * @param kmer_size K-mer length in biological symbols.
 * @param query_gate Optional canonical query filter.
 * @param decline_out Optional rejection record.
 * @return Resident index with positions local to `lo_unit`, or no value.
 * @throws vulkan_runtime_error If Vulkan execution fails.
 */
// BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN build_gpu_segment_index_on_device_resident_impl
template <class p_minimizer_type, class p_sequence>
[[nodiscard]] std::optional<gpu_built_index> build_gpu_segment_index_on_device_resident_impl(
    const p_sequence& seq,
    std::size_t lo_unit,
    std::size_t hi_unit,
    std::size_t window_size,
    std::size_t kmer_size,
    vulkan_sketch_query_gate query_gate = {},
    vulkan_sketch_decline* decline_out = nullptr)
{
    static const auto* segment_index_build_region_description =
        bioseq::instrumentation::register_static_region(
            "gpu_segment_index_build");
    auto segment_index_build_region_scope = bioseq::instrumentation::region(
        segment_index_build_region_description);
    using bare = std::remove_cvref_t<p_sequence>;
    if (decline_out != nullptr) {
        *decline_out = vulkan_sketch_decline{
            .bp_size = vulkan_decline_bp_size(seq),
            .query_gate_words = static_cast<std::uint64_t>(
                query_gate.words.size()),
            .window_size = window_size,
            .kmer_size = kmer_size,
        };
    }
    const auto decline = [decline_out](
            vulkan_sketch_decline_reason reason) noexcept {
        if (decline_out != nullptr) {
            decline_out->reason = reason;
        }
    };
    if constexpr (!vulkan_sketchable_sequence<bare>) {
        decline(vulkan_sketch_decline_reason::sequence_type_unsupported);
        return std::nullopt;
    } else {
        using traits = typename bare::traits;
        if constexpr (traits::alphabet_size > 256 || traits::bits_per_char == 0 ||
                      traits::bits_per_char > 32) {
            decline(vulkan_sketch_decline_reason::alphabet_unsupported);
            return std::nullopt;
        } else {
            constexpr std::size_t step_bits =
                alphabet_kernel_kind_of_traits<traits>() == alphabet_kernel_kind::generic
                    ? traits::bits_per_char : std::size_t{2};
            if (kmer_size == 0 || kmer_size > 64 / step_bits || kmer_size > 32) {
                decline(vulkan_sketch_decline_reason::kmer_size_unsupported);
                return std::nullopt;
            }
            if (window_size == 0 || window_size > 256) {
                decline(vulkan_sketch_decline_reason::window_size_unsupported);
                return std::nullopt;
            }

            const segment_upload_descriptor descriptor =
                compute_segment_descriptor(seq, lo_unit, hi_unit);
            if (decline_out != nullptr) {
                decline_out->bp_size = descriptor.window_bp;
            }
            if (descriptor.window_bp == 0u) {
                decline(vulkan_sketch_decline_reason::empty_sequence);
                return std::nullopt;
            }
            if (descriptor.window_bp
                < static_cast<std::uint64_t>(window_size + kmer_size)) {
                decline(vulkan_sketch_decline_reason::segment_window_too_short);
                return std::nullopt;
            }

            auto begin_iter = seq.cbegin();
            begin_iter += static_cast<bioseq::storage::sequence_size_t>(
                descriptor.lo_unit);
            const auto* base = seq.read_cptr(begin_iter);
            const sequence_upload_view upload =
                make_upload_view_from_window<bare>(seq, base, descriptor);
            const vulkan_rank_lut rank_lut = make_vulkan_rank_lut<bare>();
            return build_gpu_index_from_upload_on_device<p_minimizer_type>(
                upload,
                rank_lut,
                window_size,
                kmer_size,
                query_gate,
                decline_out);
        }
    }
}
// BIOSEQ_PRIVATE_PACKED_ACCESS_END build_gpu_segment_index_on_device_resident_impl

} // namespace bioseq::gpu::vulkan
