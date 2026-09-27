/**
 * @file
 * @brief Sorts batches of lazy sequence views by returning their source indices.
 *
 * Compatible resident inputs can use Vulkan. If that  path declines, the CPU
 * compares logical ranks. Worker-sareable inputs use the parallel pool, while
 * other inputs are, sorted on the calling thread Fragmented inputs stay lazy.
 */
module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.storage.batch_sort;

import bioseq.common.checked_arithmetic;
import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.gpu.vulkan.abi;
import bioseq.gpu.vulkan.runtime;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.pool;
import bioseq.sequences;
import bioseq.storage.lazy_sequence;
import bioseq.storage.scanner;

export namespace bioseq::storage {

/**
 * Sequence sort options shared with the planner.
 *
 * The caller handles null placement. This sorter receives sequence views and
 * reads descending and vulkan_enabled.
 */
struct sequence_sort_options {
    bool descending = false;
    bool nulls_first = false;
    bool vulkan_enabled = false;
};

}  // namespace bioseq::storage

namespace bioseq::storage::batch_sort_detail {

/** @brief Reverse an ascending permutation when descending order was requested. */
[[nodiscard]] inline std::vector<std::size_t> apply_sort_direction(
        std::vector<std::size_t> order,
        bool descending) {
    if (descending) {
        std::ranges::reverse(order);
    }
    return order;
}

/** Packed byte spans and sequence descriptors for one GPU sort request. */
struct packed_sort_batch {
    std::vector<bioseq::gpu::vulkan::sequence_sort_upload_chunk> chunks;
    std::vector<bioseq::gpu::vulkan::sequence_descriptor_wire> descriptors;
    std::uint64_t packed_bytes{};
};

/** Tries GPU sorting with compatible resident storage. Returns no value if declined. */
template <class p_lazy_batch>
[[nodiscard]] std::optional<std::vector<std::size_t>> try_sort_on_gpu(
        const p_lazy_batch& batch,
        const sequence_sort_options& options) {
    using view_type = std::remove_cvref_t<
        std::ranges::range_value_t<p_lazy_batch>>;
    using alphabet_type = typename view_type::alphabet_type;
    using traits = typename view_type::traits;
    using descriptor_type = typename view_type::resident_packed_sequence;
    static_assert(resident_packed_sequence_view<view_type>);

    if (!options.vulkan_enabled) {
        return std::nullopt;
    }

    auto& runtime = bioseq::gpu::vulkan::runtime::instance();
    if (!bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        return std::nullopt;
    }

    const std::size_t sequence_count = std::ranges::size(batch);
    if (sequence_count <= 1u) {
        std::vector<std::size_t> order(sequence_count);
        std::iota(order.begin(), order.end(), std::size_t{0u});
        return apply_sort_direction(std::move(order), options.descending);
    }
    if (sequence_count
        >= static_cast<std::size_t>(
            (std::numeric_limits<std::uint32_t>::max)())) {
        return std::nullopt;
    }

    // Every input must expose the same packed width from storage that workers
    // can  share. Try the supported widths without copying sequence contents.
    std::uint8_t resident_width{};
    std::vector<descriptor_type> resident_descriptors;
    for (const std::uint8_t width : {8u, 4u, 2u, 1u}) {
        std::vector<descriptor_type> candidate;
        candidate.reserve(sequence_count);
        bool is_compatible = true;
        for (const view_type view : batch) {
            if (!is_worker_shareable(view)) {
                is_compatible = false;
                break;
            }
            auto descriptor = view.try_resident_packed(
                0u, view.bp_size(), width);
            if (!descriptor.has_value()
                || descriptor->leading_symbol_offset != 0u) {
                is_compatible = false;
                break;
            }
            candidate.push_back(*descriptor);
        }
        if (is_compatible) {
            resident_width = width;
            resident_descriptors = std::move(candidate);
            break;
        }
    }
    if (resident_width == 0u) {
        return std::nullopt;
    }

    constexpr std::uint32_t bits_per_symbol =
        static_cast<std::uint32_t>(traits::k_logical_bits_per_symbol);
    const std::uint32_t unit_bits =
        static_cast<std::uint32_t>(resident_width) * 8u;
    const std::uint32_t letters_per_unit = unit_bits / bits_per_symbol;
    const std::uint64_t symbol_mask =
        (std::uint64_t{1u} << bits_per_symbol) - 1u;
    const std::uint32_t used_bits = letters_per_unit * bits_per_symbol;
    const std::uint64_t used_storage_mask = used_bits == 64u
        ? (std::numeric_limits<std::uint64_t>::max)()
        : (std::uint64_t{1u} << used_bits) - 1u;

    constexpr auto alphabet_kind = [] {
        if constexpr (seqan3::nucleotide_alphabet<alphabet_type>
                      && traits::k_logical_bits_per_symbol == 2u
                      && traits::k_alphabet_size == 4u) {
            return bioseq::gpu::vulkan::alphabet_kernel_kind::dna4_nt4;
        } else if constexpr (seqan3::nucleotide_alphabet<alphabet_type>
                             && traits::k_logical_bits_per_symbol == 3u
                             && traits::k_alphabet_size == 5u) {
            return bioseq::gpu::vulkan::alphabet_kernel_kind::dna5_nt4;
        } else {
            return bioseq::gpu::vulkan::alphabet_kernel_kind::generic;
        }
    }();

    const bioseq::gpu::vulkan::sequence_sort_kernel_parameters parameters{
        .alphabet_kind = static_cast<std::uint32_t>(alphabet_kind),
        .unit_bits = unit_bits,
        .bits_per_char = bits_per_symbol,
        .letters_per_unit = letters_per_unit,
        .char_mask = static_cast<std::uint32_t>(symbol_mask),
        .used_storage_mask = used_storage_mask,
    };
    const std::uint32_t units_for_first_64_bits =
        std::max<std::uint32_t>(
            1u,
            bioseq::common::ceil_div(
                std::uint32_t{64u}, parameters.unit_bits));

    auto pack_batch = [sequence_count,
                       letters_per_unit,
                       resident_width,
                       &resident_descriptors](
            std::optional<std::uint32_t> maximum_units)
        -> std::optional<packed_sort_batch> {
        packed_sort_batch packed;
        packed.chunks.reserve(sequence_count);
        packed.descriptors.reserve(sequence_count);

        std::uint64_t total_padded_bytes{};
        for (const descriptor_type& descriptor : resident_descriptors) {
            const auto byte_size = bioseq::common::checked_mul(
                static_cast<std::uint64_t>(descriptor.unit_count),
                static_cast<std::uint64_t>(resident_width));
            if (!byte_size.has_value()
                || descriptor.logical_length
                    > (std::numeric_limits<std::uint32_t>::max)()
                || (*byte_size != 0u && descriptor.units == nullptr)) {
                return std::nullopt;
            }

            const auto logical_length =
                static_cast<std::uint32_t>(descriptor.logical_length);
            const std::uint32_t unit_count = bioseq::common::ceil_div(
                logical_length, letters_per_unit);
            const std::uint32_t upload_units = maximum_units.has_value()
                ? std::min(unit_count, *maximum_units)
                : unit_count;
            const auto upload_symbol_capacity = bioseq::common::checked_mul(
                static_cast<std::uint64_t>(upload_units),
                static_cast<std::uint64_t>(letters_per_unit));
            const auto upload_byte_capacity = bioseq::common::checked_mul(
                static_cast<std::uint64_t>(upload_units),
                static_cast<std::uint64_t>(resident_width));
            if (!upload_symbol_capacity.has_value()
                || !upload_byte_capacity.has_value()) {
                return std::nullopt;
            }
            const std::uint64_t upload_symbols = std::min<std::uint64_t>(
                descriptor.logical_length, *upload_symbol_capacity);
            const std::uint64_t upload_bytes = std::min(
                *byte_size, *upload_byte_capacity);
            const auto padded_bytes = bioseq::common::checked_align_up(
                upload_bytes, std::uint64_t{4u});
            if (!padded_bytes.has_value()) {
                return std::nullopt;
            }

            const std::uint64_t word_offset = total_padded_bytes / 4u;
            if (word_offset
                > (std::numeric_limits<std::uint32_t>::max)()) {
                return std::nullopt;
            }
            const auto next_total = bioseq::common::checked_add(
                total_padded_bytes, *padded_bytes);
            if (!next_total.has_value()
                || *next_total
                    > static_cast<std::uint64_t>(
                        (std::numeric_limits<std::size_t>::max)())) {
                return std::nullopt;
            }
            total_padded_bytes = *next_total;

            packed.descriptors.push_back(
                bioseq::gpu::vulkan::sequence_descriptor_wire{
                    .word_offset = static_cast<std::uint32_t>(word_offset),
                    .unit_count = upload_units,
                    .bp_size = static_cast<std::uint32_t>(upload_symbols),
                    .pad = 0u,
                });
            packed.chunks.push_back(
                bioseq::gpu::vulkan::sequence_sort_upload_chunk{
                    .bytes = descriptor.units,
                    .byte_size = upload_bytes,
                    .padded_byte_size = *padded_bytes,
                });
        }
        packed.packed_bytes = total_padded_bytes;
        return packed;
    };

    auto run_packed = [&runtime, parameters, sequence_count](
            const packed_sort_batch& packed,
            bool log_errors)
        -> std::optional<std::vector<std::uint32_t>> {
        constexpr std::uint64_t transient_bytes_per_sequence =
            8u + 8u + 16u + 4u + 256u;
        const std::uint64_t reservation = bioseq::common::saturating_add(
            packed.packed_bytes,
            bioseq::common::saturating_mul(
                static_cast<std::uint64_t>(sequence_count),
                transient_bytes_per_sequence));
        auto permit = bioseq::parallelism::memory_admission_controller::
            instance().try_reserve(reservation);
        if (!permit.has_value()) {
            return std::nullopt;
        }
        try {
            return runtime.run_sequence_sort_roundtrip(
                packed.chunks, packed.descriptors, parameters);
        } catch (const bioseq::gpu::vulkan::vulkan_runtime_error& error) {
            if (log_errors) {
                bioseq_log<logging_level::error>(
                    "[sequence_batch_sort] {}", error.what());
            }
            return std::nullopt;
        }
    };

    auto materialize_order = [&options](
            const std::vector<std::uint32_t>& device_order) {
        std::vector<std::size_t> order(device_order.size());
        std::ranges::transform(
            device_order,
            order.begin(),
            [](std::uint32_t index) {
                return static_cast<std::size_t>(index);
            });
        return apply_sort_direction(std::move(order), options.descending);
    };

    auto sampled_first_unit_varies = [resident_width,
                                      sequence_count,
                                      &resident_descriptors]() -> bool {
        const descriptor_type& first = resident_descriptors.front();
        const auto first_bytes = bioseq::common::checked_mul(
            static_cast<std::uint64_t>(first.unit_count),
            static_cast<std::uint64_t>(resident_width));
        if (!first_bytes.has_value() || *first_bytes < resident_width
            || first.units == nullptr) {
            return false;
        }
        std::array<std::byte, sizeof(std::uint64_t)> first_unit{};
        std::memcpy(first_unit.data(), first.units, resident_width);
        const std::size_t sample_count =
            std::min<std::size_t>(sequence_count, 64u);
        for (std::size_t index = 1u; index < sample_count; ++index) {
            const descriptor_type& descriptor = resident_descriptors[index];
            const auto bytes = bioseq::common::checked_mul(
                static_cast<std::uint64_t>(descriptor.unit_count),
                static_cast<std::uint64_t>(resident_width));
            if (bytes.has_value() && *bytes >= resident_width
                && descriptor.units != nullptr
                && std::memcmp(
                    first_unit.data(), descriptor.units, resident_width)
                    != 0) {
                return true;
            }
        }
        return false;
    };

    // A prefix sort is enough when the runtime used its dense scatter path.
    // Otherwise submit complete sequences so equal prefixes can be resolved.
    if (sampled_first_unit_varies()) {
        if (auto prefix = pack_batch(units_for_first_64_bits)) {
            const std::uint64_t dense_before =
                runtime.seq_sort_dense_scatter_dispatch_count();
            if (auto device_order = run_packed(*prefix, false)) {
                const std::uint64_t dense_after =
                    runtime.seq_sort_dense_scatter_dispatch_count();
                if (dense_after > dense_before) {
                    return materialize_order(*device_order);
                }
            }
        }
    }

    auto full = pack_batch(std::nullopt);
    if (!full.has_value()) {
        return std::nullopt;
    }
    auto device_order = run_packed(*full, true);
    if (!device_order.has_value()) {
        return std::nullopt;
    }
    return materialize_order(*device_order);
}

}  // namespace bioseq::storage::batch_sort_detail

export namespace bioseq::storage {

/**
 * Sorts a random-access batch by sequence ranks, returning source indices.
 *
 * Equal sequences are ordered by source index. Descending order reverses both
 * sequence order and the index tie-break, so equal entries appear in reverse
 * inut order when  descending is set.
 */
template <
    sequence_alphabet p_alphabet,
    class p_batch = std::span<const lazy_sequence_view<p_alphabet>>>
    requires std::ranges::sized_range<p_batch>
        && std::ranges::random_access_range<p_batch>
        && std::same_as<
            std::remove_cvref_t<std::ranges::range_value_t<p_batch>>,
            lazy_sequence_view<p_alphabet>>
/** Sorter returning a permutation of the input batch. */
struct sequence_batch_sorter {
    using sequence_view_type = std::ranges::range_value_t<p_batch>;
    using batch_type = p_batch;

    /** Sorts with the parallel pool when all sources are worker-shareable, or locally otherwise. */
    [[nodiscard]] static std::vector<std::size_t> sort_on_cpu(
            const batch_type& batch,
            const sequence_sort_options& options,
            bioseq::parallelism::tracked_job_observer observer = {}) {
        const std::size_t sequence_count = std::ranges::size(batch);
        std::vector<std::size_t> order(sequence_count);
        std::iota(order.begin(), order.end(), std::size_t{0u});

        const bool descending = options.descending;
        auto less = [&batch, descending](
                std::size_t left,
                std::size_t right) {
            const int comparison = compare_lazy_sequences(
                batch[left], batch[right]);
            if (comparison != 0) {
                return descending ? comparison > 0 : comparison < 0;
            }
            return descending ? left > right : left < right;
        };
        const bool worker_shareable = std::ranges::all_of(
            batch,
            [](sequence_view_type view) {
                return is_worker_shareable(view);
            });
        if (worker_shareable) {
            bioseq::parallelism::global_pool::instance().parallel_sort_tracked(
                order.begin(), order.end(), less, observer);
        } else {
            std::stable_sort(order.begin(), order.end(), less);
        }
        return order;
    }

    /** Tries Vulkan when enabled, then sorts on the CPU if the GPU path declines. */
    [[nodiscard]] static std::vector<std::size_t> sort(
            const batch_type& batch,
            const sequence_sort_options& options = {},
            bioseq::parallelism::tracked_job_observer observer = {}) {
        const std::size_t sequence_count = std::ranges::size(batch);
        if (sequence_count <= 1u) {
            std::vector<std::size_t> order(sequence_count);
            std::iota(order.begin(), order.end(), std::size_t{0u});
            return order;
        }
        if (auto device_order =
                batch_sort_detail::try_sort_on_gpu(batch, options)) {
            return std::move(*device_order);
        }
        return sort_on_cpu(batch, options, observer);
    }
};

}  // namespace bioseq::storage
