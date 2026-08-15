/**
 * @file batch.cppm
 * @brief Resident and batched Vulkan minimizer sketch routes.
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

export module bioseq.sequences.kmers.vulkan_minimizers:batch;

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

export namespace bioseq::gpu::vulkan {

/**
 * @brief Sketches one resident packed sequence into host and device storage.
 * @tparam p_minimizer_type Packed minimizer record.
 * @tparam p_sequence Contiguous packed sequence type.
 * @param seq Source sequence.
 * @param window_size Window length in k-mers.
 * @param kmer_size K-mer length in biological symbols.
 * @param decline_out Optional pre-dispatch rejection record.
 * @param host_ownership_route Host allocation ownership contract.
 * @return Sketch result, or no value when the shape or resources are rejected.
 * @throws vulkan_runtime_error If runtime execution fails.
 */
template <class p_minimizer_type, class p_sequence>
[[nodiscard]] std::optional<vulkan_sketch_result<p_minimizer_type>>
collect_minimizers_vulkan_result_resident_impl(
    const p_sequence& seq,
    std::size_t window_size,
    std::size_t kmer_size,
    vulkan_sketch_decline* decline_out = nullptr,
    vulkan_sketch_host_ownership_route host_ownership_route =
        vulkan_sketch_host_ownership_route::collector_compatible)
{
    using bare = std::remove_cvref_t<p_sequence>;
    if (decline_out != nullptr) {
        *decline_out = vulkan_sketch_decline{
            .bp_size = vulkan_decline_bp_size(seq),
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
        if constexpr (traits::alphabet_size > 256 || traits::bits_per_char == 0
                      || traits::bits_per_char > 32) {
            decline(vulkan_sketch_decline_reason::alphabet_unsupported);
            return std::nullopt;
        } else {
            constexpr std::size_t step_bits =
                choose_alphabet_kernel<bare>() == alphabet_kernel_kind::generic
                    ? traits::bits_per_char
                    : std::size_t{2};
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
            return sketch_uploaded_window_result<p_minimizer_type>(
                upload,
                rank_lut,
                window_size,
                kmer_size,
                0u,
                nullptr,
                {},
                decline_out,
                nullptr,
                0u,
                host_ownership_route);
        }
    }
}

/**
 * @brief Prevents direct use of the generic sequence-batch entry point.
 * @tparam p_minimizer_type Packed minimizer record.
 * @tparam p_sequence Sequence element type.
 */
template <class p_minimizer_type, class p_sequence>
[[nodiscard]] std::optional<vulkan_batch_sketch_result<p_minimizer_type>>
collect_minimizers_vulkan_batch_result(
    std::span<const p_sequence> seqs,
    std::size_t window_size,
    std::size_t kmer_size) = delete;

/**
 * @brief Sketches a batch of prepared uploads into shared host and device output.
 * @param uploads Ordered query upload views.
 * @param rank_lut Shared alphabet rank mapping.
 * @param window_size Window length in k-mers.
 * @param kmer_size K-mer length in biological symbols.
 * @param materialization Requested host materialization mode.
 * @return Batch result, or no value when shape or admission fails.
 * @throws vulkan_runtime_error If runtime execution or ownership checks fail.
 */
[[nodiscard]] std::optional<
    vulkan_batch_sketch_result<::bioseq::minimizers::minimap_sketch_data>>
collect_minimap_sketch_data_vulkan_upload_batch_result(
    std::span<const sequence_upload_view> uploads,
    const vulkan_rank_lut& rank_lut,
    std::size_t window_size,
    std::size_t kmer_size,
    vulkan_sketch_host_materialization materialization)
{
    using minimizer_type = ::bioseq::minimizers::minimap_sketch_data;
    using bioseq::parallelism::memory_admission_controller;
    using bioseq::common::saturating_mul;

    if (materialization != vulkan_sketch_host_materialization::host_and_device
        && materialization != vulkan_sketch_host_materialization::device_only) {
        throw vulkan_runtime_error{
            "Vulkan batch sketch has invalid host materialization mode"};
    }
    if (uploads.empty()) {
        return vulkan_batch_sketch_result<minimizer_type>{
            .materialization = materialization,
        };
    }
    if (kmer_size == 0 || kmer_size > 32 || window_size == 0 || window_size > 256) {
        return std::nullopt;
    }
    if (uploads.size()
        > static_cast<std::size_t>(
            (std::numeric_limits<std::uint32_t>::max)())) {
        return std::nullopt;
    }

    auto& runtime = bioseq::gpu::vulkan::runtime::instance();
    if (!hardware_device_available(runtime) || runtime.features().max_workgroup_count_x == 0u) {
        return std::nullopt;
    }

    using query_tile_range = gpu_sketch_query_layout_wire;

    // Pass 1 is scalar-only: validate every upload and compute exact vector
    // sizes without constructing any owned vector or Vulkan buffer.
    std::uint64_t packed_sequence_bytes = 0u;
    std::uint64_t total_tile_count = 0u;
    std::uint64_t max_minimizers = 0u;
    std::uint64_t max_bp = 0u;
    const sequence_upload_view& first = uploads.front();
    const std::uint32_t letters_per_unit = first.letters_per_unit;
    const std::uint32_t unit_bits = first.unit_bits;
    const std::uint32_t bits_per_char = first.bits_per_char;
    const std::uint32_t char_mask = first.char_mask;
    const std::uint32_t alphabet_size = first.alphabet_size;
    const std::uint32_t padding_letters = first.padding_letters;
    const std::uint32_t alphabet_kind = static_cast<std::uint32_t>(first.alphabet_kind);

    for (std::size_t query_index = 0; query_index < uploads.size(); ++query_index) {
        const sequence_upload_view& upload = uploads[query_index];
        if (upload.bp_size > std::numeric_limits<std::uint32_t>::max()
            || upload.padded_byte_size == std::numeric_limits<std::uint64_t>::max()
            || upload.padded_byte_size
                > static_cast<std::uint64_t>(
                    (std::numeric_limits<std::size_t>::max)())
            || upload.byte_size > upload.padded_byte_size
            || (upload.byte_size != 0u && upload.bytes == nullptr)
            || upload.letters_per_unit != letters_per_unit
            || upload.unit_bits != unit_bits
            || upload.bits_per_char != bits_per_char
            || upload.char_mask != char_mask
            || upload.alphabet_size != alphabet_size
            || static_cast<std::uint32_t>(upload.alphabet_kind) != alphabet_kind) {
            return std::nullopt;
        }
        max_bp = std::max(max_bp, upload.bp_size);
        max_minimizers = bioseq::common::saturating_add(
            max_minimizers,
            estimate_vulkan_minimizer_capacity(upload.bp_size, window_size));
        if (max_minimizers
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::uint32_t>::max)())) {
            return std::nullopt;
        }

        const std::uint64_t aligned_begin =
            bioseq::common::checked_align_up(
                packed_sequence_bytes,
                sizeof(std::uint32_t))
                .value_or((std::numeric_limits<std::uint64_t>::max)());
        if (aligned_begin
                == (std::numeric_limits<std::uint64_t>::max)()
            || aligned_begin / sizeof(std::uint32_t)
                > static_cast<std::uint64_t>(
                    (std::numeric_limits<std::uint32_t>::max)())) {
            return std::nullopt;
        }
        packed_sequence_bytes = bioseq::common::saturating_add(
            aligned_begin,
            upload.padded_byte_size);
        if (packed_sequence_bytes
                == (std::numeric_limits<std::uint64_t>::max)()
            || packed_sequence_bytes
                > static_cast<std::uint64_t>(
                    (std::numeric_limits<std::size_t>::max)())) {
            return std::nullopt;
        }

        if (upload.bp_size >= kmer_size) {
            const gpu_sketch_tile_plan tile_plan = make_sketch_tile_plan(
                upload.bp_size,
                static_cast<std::uint32_t>(kmer_size),
                static_cast<std::uint32_t>(window_size),
                upload.letters_per_unit,
                runtime.features().max_workgroup_count_x);
            if (!tile_plan.is_dispatchable || tile_plan.tile_count == 0u) {
                return std::nullopt;
            }
            total_tile_count = bioseq::common::saturating_add(
                total_tile_count,
                tile_plan.tile_count);
            if (total_tile_count
                > static_cast<std::uint64_t>(
                    (std::numeric_limits<std::uint32_t>::max)())) {
                return std::nullopt;
            }
        }
    }

    if (total_tile_count == 0u || max_minimizers == 0u) {
        // A nonempty caller requested GPU work, but no dispatchable query
        // exists.  Do not return a result that could be mistaken for is_internally_consistent
        // zero-emission Vulkan execution.
        return std::nullopt;
    }

    const gpu_sketch_batch_allocation_requirements initial_requirements =
        runtime.sketch_batch_allocation_requirements(
            packed_sequence_bytes,
            total_tile_count,
            static_cast<std::uint32_t>(
                minimizer_wire_traits<minimizer_type>::k_word_kind),
            minimizer_wire_traits<minimizer_type>::k_words32_per_minimizer,
            max_minimizers);
    if (!initial_requirements.valid) {
        return std::nullopt;
    }
    const std::uint64_t pessimistic_host_minimizer_bytes =
        materialization == vulkan_sketch_host_materialization::host_and_device
        ? saturating_mul(max_minimizers, sizeof(minimizer_type))
        : 0u;
    const std::uint64_t peak_host_minimizer_bytes = saturating_mul(
        pessimistic_host_minimizer_bytes,
        materialization == vulkan_sketch_host_materialization::host_and_device
            ? 2u
            : 0u);
    const auto vector_capacity_bytes = []<class p_value_type>(
        const std::vector<p_value_type>& values) noexcept -> std::uint64_t {
        if (values.capacity()
            > (std::numeric_limits<std::uint64_t>::max)()) {
            return (std::numeric_limits<std::uint64_t>::max)();
        }
        return bioseq::common::saturating_mul(
            static_cast<std::uint64_t>(values.capacity()),
            sizeof(p_value_type));
    };
    const auto release_vector_storage = []<class p_value_type>(
        std::vector<p_value_type>& values) noexcept {
        std::vector<p_value_type>{}.swap(values);
    };
    const std::uint64_t exact_scaffold_bytes = bioseq::common::saturating_add(
        bioseq::common::saturating_add(
            packed_sequence_bytes,
            saturating_mul(
                total_tile_count,
                sizeof(gpu_sketch_tile_wire))),
        bioseq::common::saturating_add(
            bioseq::common::saturating_add(
                saturating_mul(uploads.size(), sizeof(query_tile_range)),
                saturating_mul(
                    uploads.size(),
                    2u * sizeof(gpu_seed_query_record_wire)
                        + sizeof(std::uint32_t))),
            saturating_mul(
                total_tile_count,
                2u * sizeof(std::uint32_t))));
    const std::uint64_t initial_peak_bytes = bioseq::common::saturating_add(
        bioseq::common::saturating_add(
            initial_requirements.arena_allocation_bytes,
            initial_requirements.pessimistic_output_allocation_bytes),
        bioseq::common::saturating_add(peak_host_minimizer_bytes, exact_scaffold_bytes));
    if (initial_peak_bytes
        == (std::numeric_limits<std::uint64_t>::max)()) {
        return std::nullopt;
    }
    auto initial_permit = memory_admission_controller::instance().try_reserve(
        initial_peak_bytes);
    if (!initial_permit) return std::nullopt;

    // Declaration order is a correctness invariant: the shared permit holder
    // precedes result/scaffold/raw ownership, so reverse destruction always
    // releases vectors and Vulkan Buffers before admission on every exit.
    auto memory_retention =
        std::make_shared<vulkan_batch_sketch_memory_retention>();
    memory_retention->permit = std::move(*initial_permit);
    vulkan_batch_sketch_result<minimizer_type> result{
        .memory_retention = memory_retention,
        .materialization = materialization,
    };
    std::vector<std::byte> packed_sequence;
    std::vector<gpu_sketch_tile_wire> tiles;
    std::vector<query_tile_range> tile_ranges;

    result.query_records.resize(uploads.size());
    packed_sequence.reserve(
        static_cast<std::size_t>(packed_sequence_bytes));
    tiles.reserve(static_cast<std::size_t>(total_tile_count));
    tile_ranges.resize(uploads.size());

    // Pass 2 fills the exact preflight shapes without capacity growth under
    // normal allocators.  Actual capacities are reconciled before dispatch.
    for (std::size_t query_index = 0; query_index < uploads.size(); ++query_index) {
        const sequence_upload_view& upload = uploads[query_index];
        const std::size_t aligned_begin = static_cast<std::size_t>(
            bioseq::common::checked_align_up(
                packed_sequence.size(),
                sizeof(std::uint32_t))
                .value_or((std::numeric_limits<std::size_t>::max)()));
        packed_sequence.resize(aligned_begin);
        const std::uint32_t seq_word_offset =
            static_cast<std::uint32_t>(
                aligned_begin / sizeof(std::uint32_t));
        const std::size_t append_begin = packed_sequence.size();
        const std::size_t padded_bytes =
            static_cast<std::size_t>(upload.padded_byte_size);
        packed_sequence.resize(append_begin + padded_bytes);
        if (padded_bytes != 0u) {
            std::memset(
                packed_sequence.data() + append_begin,
                0,
                padded_bytes);
        }
        if (upload.byte_size != 0u) {
            std::memcpy(
                packed_sequence.data() + append_begin,
                upload.bytes,
                static_cast<std::size_t>(upload.byte_size));
        }

        const std::uint32_t tile_begin =
            static_cast<std::uint32_t>(tiles.size());
        if (upload.bp_size >= kmer_size) {
            const gpu_sketch_tile_plan tile_plan = make_sketch_tile_plan(
                upload.bp_size,
                static_cast<std::uint32_t>(kmer_size),
                static_cast<std::uint32_t>(window_size),
                upload.letters_per_unit,
                runtime.features().max_workgroup_count_x);
            const std::uint32_t bp =
                static_cast<std::uint32_t>(upload.bp_size);
            const std::uint32_t overlap = sketch_replay_overlap_bp(
                static_cast<std::uint32_t>(kmer_size),
                static_cast<std::uint32_t>(window_size));
            for (std::uint32_t tile = 0u; tile < tile_plan.tile_count; ++tile) {
                const std::uint32_t lo = tile * tile_plan.tile_bp;
                const std::uint32_t hi =
                    std::min(bp, (tile + 1u) * tile_plan.tile_bp);
                tiles.push_back(gpu_sketch_tile_wire{
                    .seq_word_offset = seq_word_offset,
                    .bp_count = bp,
                    .replay_lo = lo > overlap ? lo - overlap : 0u,
                    .tile_lo = lo,
                    .tile_hi = hi,
                    .out_base = 0u,
                });
            }
        }
        tile_ranges[query_index] = query_tile_range{
            .begin = tile_begin,
            .count = static_cast<std::uint32_t>(
                tiles.size() - tile_begin),
            .query_len = static_cast<std::uint32_t>(upload.bp_size),
        };
    }
    if (packed_sequence.size() != packed_sequence_bytes
        || tiles.size() != total_tile_count) {
        throw vulkan_runtime_error{
            "Vulkan batch sketch preflight/fill shape mismatch"};
    }

    const gpu_sketch_batch_allocation_requirements pre_dispatch_requirements =
        runtime.sketch_batch_allocation_requirements(
            packed_sequence_bytes,
            total_tile_count,
            static_cast<std::uint32_t>(
                minimizer_wire_traits<minimizer_type>::k_word_kind),
            minimizer_wire_traits<minimizer_type>::k_words32_per_minimizer,
            max_minimizers);
    if (!pre_dispatch_requirements.valid) return std::nullopt;
    const std::uint64_t actual_scaffold_capacity_bytes = bioseq::common::saturating_add(
        bioseq::common::saturating_add(
            vector_capacity_bytes(packed_sequence),
            vector_capacity_bytes(tiles)),
        bioseq::common::saturating_add(
            vector_capacity_bytes(tile_ranges),
            bioseq::common::saturating_add(
                vector_capacity_bytes(result.query_records),
                vector_capacity_bytes(result.host_minimizers))));
    const std::uint64_t anticipated_raw_count_bytes = saturating_mul(
        total_tile_count,
        2u * sizeof(std::uint32_t));
    const std::uint64_t authority_query_record_headroom = saturating_mul(
        saturating_mul(
            uploads.size(),
            sizeof(gpu_seed_query_record_wire) + sizeof(std::uint32_t)),
        bioseq::parallelism::k_vector_preallocation_headroom_factor);
    const std::uint64_t pre_dispatch_peak_bytes = bioseq::common::saturating_add(
        bioseq::common::saturating_add(
            pre_dispatch_requirements.arena_allocation_bytes,
            pre_dispatch_requirements.pessimistic_output_allocation_bytes),
        bioseq::common::saturating_add(
            peak_host_minimizer_bytes,
            bioseq::common::saturating_add(
                actual_scaffold_capacity_bytes,
                bioseq::common::saturating_add(
                    anticipated_raw_count_bytes,
                    authority_query_record_headroom))));
    bioseq::parallelism::memory_admission_failure growth_failure{};
    if (pre_dispatch_peak_bytes
            == (std::numeric_limits<std::uint64_t>::max)()
        || !memory_retention->permit.grow_to(
            pre_dispatch_peak_bytes,
            &growth_failure)) {
        return std::nullopt;
    }

    const vulkan_sketch_params sketch_params{
        .bp_size = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(
                max_bp,
                static_cast<std::uint64_t>(
                    (std::numeric_limits<std::uint32_t>::max)()))),
        .window_size = static_cast<std::uint32_t>(window_size),
        .kmer_size = static_cast<std::uint32_t>(kmer_size),
        .hash_bits = minimizer_wire_traits<minimizer_type>::k_hash_bits,
        .hash_shift = minimizer_wire_traits<minimizer_type>::k_hash_shift,
        .pos_bits = minimizer_wire_traits<minimizer_type>::k_position_bits,
        .minimizer_word_kind = static_cast<std::uint32_t>(
            minimizer_wire_traits<minimizer_type>::k_word_kind),
        .minimizer_words32 =
            minimizer_wire_traits<minimizer_type>::k_words32_per_minimizer,
        .alphabet_kind = alphabet_kind,
        .unit_bits = unit_bits,
        .bits_per_char = bits_per_char,
        .letters_per_unit = letters_per_unit,
        .alphabet_size = alphabet_size,
        .seq_word_offset = 0u,
        .char_mask = char_mask,
        .padding_letters = padding_letters,
    };

    // Declared after every other owner: any later failure destroys this raw
    // buffer/vector aggregate first, then scaffolds/result, then the permit.
    runtime_sketch_batch_result raw =
        runtime.run_sketch_tile_batch_roundtrip(
            std::span<const std::byte>{
                packed_sequence.data(),
                packed_sequence.size()},
            rank_lut,
            sketch_params,
            std::span<const gpu_sketch_tile_wire>{tiles.data(), tiles.size()},
            std::span<const gpu_sketch_query_layout_wire>{
                tile_ranges.data(), tile_ranges.size()},
            max_minimizers,
            materialization);
    const gpu_sketch_batch_allocation_requirements post_dispatch_requirements =
        runtime.sketch_batch_allocation_requirements(
            packed_sequence_bytes,
            total_tile_count,
            static_cast<std::uint32_t>(
                minimizer_wire_traits<minimizer_type>::k_word_kind),
            minimizer_wire_traits<minimizer_type>::k_words32_per_minimizer,
            max_minimizers);
    if (!post_dispatch_requirements.valid) return std::nullopt;
    const std::uint64_t post_dispatch_live_vector_bytes = bioseq::common::saturating_add(
        bioseq::common::saturating_add(
            vector_capacity_bytes(packed_sequence),
            vector_capacity_bytes(tiles)),
        bioseq::common::saturating_add(
            vector_capacity_bytes(tile_ranges),
            bioseq::common::saturating_add(
                bioseq::common::saturating_add(
                    vector_capacity_bytes(result.query_records),
                    vector_capacity_bytes(result.host_minimizers)),
                bioseq::common::saturating_add(
                    vector_capacity_bytes(raw.host_minimizer_bytes),
                    bioseq::common::saturating_add(
                        vector_capacity_bytes(raw.tile_counts),
                        bioseq::common::saturating_add(
                            vector_capacity_bytes(raw.tile_offsets),
                            raw.query_authority.owned_capacity_bytes()))))));
    const std::uint64_t post_dispatch_live_bytes = bioseq::common::saturating_add(
        raw.minimizer_words.allocation_bytes,
        bioseq::common::saturating_add(
            post_dispatch_requirements.arena_allocation_bytes,
            post_dispatch_live_vector_bytes));
    if (post_dispatch_live_bytes
            == (std::numeric_limits<std::uint64_t>::max)()
        || !memory_retention->permit.grow_to(
            post_dispatch_live_bytes,
            &growth_failure)) {
        return std::nullopt;
    }
    if (raw.overflow) {
        return std::nullopt;
    }
    if (raw.dispatch_stats.materialization() != materialization
        || !raw.dispatch_stats.is_internally_consistent()) {
        throw vulkan_runtime_error{
            "Vulkan batch sketch returned without matching is_internally_consistent "
            "count/scan/emit stats"};
    }
    if (materialization == vulkan_sketch_host_materialization::device_only
        && !raw.query_authority.does_cover_device_output()) {
        throw vulkan_runtime_error{
            "device-only Vulkan batch sketch returned without runtime-issued "
            "device-output coverage authority"};
    }

    if (materialization == vulkan_sketch_host_materialization::host_and_device) {
        result.host_minimizers = decode_host_minimizer_bytes<minimizer_type>(
            std::span<const std::byte>{
                raw.host_minimizer_bytes.data(),
                raw.host_minimizer_bytes.size()},
            raw.count);
    } else if (!raw.host_minimizer_bytes.empty()) {
        throw vulkan_runtime_error{
            "device-only Vulkan batch sketch unexpectedly materialized host hashes"};
    }
    std::uint64_t query_minimizer_cursor = 0u;
    for (std::size_t query_index = 0; query_index < uploads.size(); ++query_index) {
        const query_tile_range range = tile_ranges[query_index];
        std::uint64_t minimizer_count = 0u;
        for (std::uint32_t tile = 0u; tile < range.count; ++tile) {
            minimizer_count += raw.tile_counts[range.begin + tile];
        }
        const std::uint64_t minimizer_offset = range.count == 0u
            ? query_minimizer_cursor
            : static_cast<std::uint64_t>(raw.tile_offsets[range.begin]);
        if (minimizer_offset != query_minimizer_cursor) {
            throw vulkan_runtime_error{
                "Vulkan batch query minimizer slices are not contiguous"};
        }
        if (minimizer_count
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::uint32_t>::max)())) {
            throw vulkan_runtime_error{
                "Vulkan batch query minimizer count exceeds ABI"};
        }
        if (uploads[query_index].bp_size
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::uint32_t>::max)())) {
            throw vulkan_runtime_error{"Vulkan batch query length exceeds ABI"};
        }
        result.query_records[query_index] = gpu_seed_query_record_wire{
            .minimizer_offset = static_cast<std::uint32_t>(minimizer_offset),
            .minimizer_count = static_cast<std::uint32_t>(minimizer_count),
            .query_len = static_cast<std::uint32_t>(uploads[query_index].bp_size),
            .flags = 0u,
        };
        query_minimizer_cursor = bioseq::common::saturating_add(
            query_minimizer_cursor,
            minimizer_count);
    }
    if (query_minimizer_cursor != raw.count) {
        throw vulkan_runtime_error{
            "Vulkan batch query minimizer slices do not cover device output"};
    }

    const std::uint64_t retained_host_capacity_bytes =
        vector_capacity_bytes(result.host_minimizers);
    const std::uint64_t retained_query_record_capacity_bytes =
        vector_capacity_bytes(result.query_records);
    const std::uint64_t retained_bytes = bioseq::common::saturating_add(
        raw.minimizer_words.allocation_bytes,
        bioseq::common::saturating_add(
            retained_host_capacity_bytes,
            bioseq::common::saturating_add(
                retained_query_record_capacity_bytes,
                raw.query_authority.owned_capacity_bytes())));
    const std::uint64_t transient_vector_bytes = bioseq::common::saturating_add(
        bioseq::common::saturating_add(
            vector_capacity_bytes(raw.host_minimizer_bytes),
            vector_capacity_bytes(raw.tile_counts)),
        bioseq::common::saturating_add(
            vector_capacity_bytes(raw.tile_offsets),
            bioseq::common::saturating_add(
                vector_capacity_bytes(packed_sequence),
                bioseq::common::saturating_add(
                    vector_capacity_bytes(tiles),
                    vector_capacity_bytes(tile_ranges)))));
    const std::uint64_t final_live_peak_bytes = bioseq::common::saturating_add(
        post_dispatch_requirements.arena_allocation_bytes,
        bioseq::common::saturating_add(retained_bytes, transient_vector_bytes));
    if (final_live_peak_bytes
            == (std::numeric_limits<std::uint64_t>::max)()
        || !memory_retention->permit.grow_to(
            final_live_peak_bytes,
            &growth_failure)) {
        return std::nullopt;
    }

    release_vector_storage(raw.host_minimizer_bytes);
    release_vector_storage(raw.tile_counts);
    release_vector_storage(raw.tile_offsets);
    release_vector_storage(packed_sequence);
    release_vector_storage(tiles);
    release_vector_storage(tile_ranges);
    memory_retention->permit.shrink_to(retained_bytes);

    auto storage =
        std::make_shared<vulkan_device_minimizer_storage<minimizer_type>>();
    storage->words = std::move(raw.minimizer_words);
    storage->batch_memory_retention = memory_retention;
    storage->count = raw.count;
    storage->capacity =
        materialization == vulkan_sketch_host_materialization::host_and_device
        ? max_minimizers
        : raw.count;
    result.device = std::move(storage);
    result.dispatch_stats = raw.dispatch_stats;
    result.query_authority = std::move(raw.query_authority);
    result.device_minimizer_count = raw.count;
    return result;
}

} // namespace bioseq::gpu::vulkan
