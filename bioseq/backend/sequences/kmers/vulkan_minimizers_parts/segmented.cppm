/**
 * @file segmented.cppm
 * @brief Strict segmented Vulkan minimizer collectors.
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

export module bioseq.sequences.kmers.vulkan_minimizers:segmented;

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

export namespace bioseq::gpu::vulkan {

/**
 * @brief Collects a resident Vulkan sketch through an existing collector surface.
 * @tparam p_minimizer_type Packed minimizer record.
 * @tparam p_collector_type Collector accepting individual records.
 * @tparam p_sequence Contiguous packed sequence type.
 * @param collector Destination collector.
 * @param seq Source sequence.
 * @param window_size Window length in k-mers.
 * @param kmer_size K-mer length in biological symbols.
 * @param decline_out Optional rejection record.
 * @return Completed dispatch token, or no value when dispatch is rejected.
 * @throws vulkan_runtime_error If runtime execution or token validation fails.
 */
template <class p_minimizer_type, class p_collector_type, class p_sequence>
[[nodiscard]] std::optional<vulkan_sketch_dispatch_token>
collect_minimizers_vulkan_resident_impl(
    p_collector_type& collector, const p_sequence& seq,
    std::size_t window_size, std::size_t kmer_size,
    vulkan_sketch_decline* decline_out = nullptr)
{
    auto result = collect_minimizers_vulkan_result_resident_impl<p_minimizer_type>(
        seq, window_size, kmer_size, decline_out);
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
 * @brief Decodes parallel device key and position arrays into sorted pairs.
 * @param key_bytes Downloaded 64-bit minimizer hashes.
 * @param value_bytes Downloaded 64-bit encoded positions.
 * @param pair_count Number of complete pairs.
 * @return Pairs in runtime-provided hash and position order.
 * @throws vulkan_runtime_error If the count or byte spans are invalid.
 */
[[nodiscard]] inline std::vector<std::pair<std::uint64_t, std::uint64_t>>
decode_sorted_pair_bytes(
    std::span<const std::byte> key_bytes,
    std::span<const std::byte> value_bytes,
    std::uint64_t pair_count)
{
    if (pair_count
        > static_cast<std::uint64_t>(
            (std::numeric_limits<std::size_t>::max)()
            / sizeof(std::pair<std::uint64_t, std::uint64_t>))) {
        throw vulkan_runtime_error{"Vulkan sorted-pair count exceeds host address space"};
    }
    const std::size_t count = static_cast<std::size_t>(pair_count);
    const std::size_t expected = count * sizeof(std::uint64_t);
    if (key_bytes.size() < expected || value_bytes.size() < expected) {
        throw vulkan_runtime_error{"Vulkan sorted-pair download was truncated"};
    }

    std::vector<std::pair<std::uint64_t, std::uint64_t>> out;
    out.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        std::uint64_t key = 0;
        std::uint64_t value = 0;
        std::memcpy(&key, key_bytes.data() + i * sizeof(std::uint64_t), sizeof(key));
        std::memcpy(&value, value_bytes.data() + i * sizeof(std::uint64_t), sizeof(value));
        out[i] = std::pair<std::uint64_t, std::uint64_t>{key, value};
    }
    return out;
}

/**
 * @brief Collects sorted minimizer pairs for one stitched packed segment.
 * @tparam p_sequence Contiguous packed sequence type.
 * @param seq Source sequence.
 * @param lo_unit Inclusive upload unit index.
 * @param hi_unit Exclusive upload unit index.
 * @param emit_base_bp Global biological offset of the upload window.
 * @param core_lo_bp Inclusive global core boundary.
 * @param core_hi_bp Exclusive global core boundary.
 * @param window_size Window length in k-mers.
 * @param kmer_size K-mer length in biological symbols.
 * @param execution_stats_out Optional segment execution counters.
 * @return Sorted global position and hash pairs owned with their permit.
 * @throws vulkan_runtime_error For every selected-Vulkan rejection.
 */
// BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN collect_sorted_minimizer_pairs_vulkan_segment_resident_impl_or_throw
template <class p_sequence>
[[nodiscard]] vulkan_owned_segment_pairs
collect_sorted_minimizer_pairs_vulkan_segment_resident_impl_or_throw(
    const p_sequence& seq,
    std::size_t lo_unit,
    std::size_t hi_unit,
    bioseq::storage::sequence_size_t emit_base_bp,
    bioseq::storage::sequence_size_t core_lo_bp,
    bioseq::storage::sequence_size_t core_hi_bp,
    std::size_t window_size,
    std::size_t kmer_size,
    vulkan_segment_pair_execution_stats* execution_stats_out = nullptr)
{
    using bare = std::remove_cvref_t<p_sequence>;
    using minimizer_type = bioseq::minimizers::minimap_sketch_data;
    vulkan_segment_pair_execution_stats execution_stats{};
    if (execution_stats_out != nullptr) {
        *execution_stats_out = {};
    }
    vulkan_sketch_decline path_decline{
        .bp_size = vulkan_decline_bp_size(seq),
        .window_size = window_size,
        .kmer_size = kmer_size,
    };
    if constexpr (!vulkan_sketchable_sequence<bare>) {
        path_decline.reason =
            vulkan_sketch_decline_reason::fragmented_sequence_unsupported;
        throw_vulkan_strict_decline(
            "Vulkan segment minimizer pair path", path_decline);
    } else {
        using traits = typename bare::traits;
        using storage_unit = typename traits::storage_unit;

        if constexpr (!vulkan_minimizer_alphabet_shape_supported<traits>()) {
            path_decline.reason = vulkan_sketch_decline_reason::alphabet_unsupported;
            throw_vulkan_strict_decline(
                "Vulkan segment minimizer pair path", path_decline);
        } else {
            if (!vulkan_minimizer_kmer_size_supported<traits>(kmer_size)) {
                path_decline.reason =
                    vulkan_sketch_decline_reason::kmer_size_unsupported;
                throw_vulkan_strict_decline(
                    "Vulkan segment minimizer pair path", path_decline);
            }
            if (!vulkan_minimizer_window_size_supported(window_size)) {
                path_decline.reason =
                    vulkan_sketch_decline_reason::window_size_unsupported;
                throw_vulkan_strict_decline(
                    "Vulkan segment minimizer pair path", path_decline);
            }
            const std::uint64_t minimum_bases =
                static_cast<std::uint64_t>(window_size) + kmer_size - 1u;

            const segment_upload_descriptor descriptor =
                compute_segment_descriptor(seq, lo_unit, hi_unit);
            path_decline.bp_size = descriptor.window_bp;
            if (descriptor.window_bp == 0u) {
                path_decline.reason = vulkan_sketch_decline_reason::empty_sequence;
                throw_vulkan_strict_decline(
                    "Vulkan segment minimizer pair path", path_decline);
            }
            if (descriptor.window_bp
                > static_cast<std::uint64_t>(
                    minimizer_type::k_position_field_mask)) {
                path_decline.reason =
                    vulkan_sketch_decline_reason::segment_position_field_overflow;
                throw_vulkan_strict_decline(
                    "Vulkan segment minimizer pair path", path_decline);
            }
            if (descriptor.window_bp
                < minimum_bases) {
                if (execution_stats_out != nullptr) {
                    *execution_stats_out = execution_stats;
                }
                return {};
            }

            auto& runtime = bioseq::gpu::vulkan::runtime::instance();
            if (!hardware_device_available(runtime)) {
                path_decline.reason =
                    vulkan_sketch_decline_reason::hardware_device_unavailable;
                throw_vulkan_strict_decline(
                    "Vulkan segment minimizer pair path", path_decline);
            }

            const vulkan_rank_lut rank_lut = make_vulkan_rank_lut<bare>();
            const std::size_t lpu =
                std::max<std::size_t>(1u, traits::letters_per_unit);
            const std::size_t context_units =
                sketch_segment_context_units<traits>(window_size, kmer_size);
            const std::uint64_t collected_limit =
                admitted_sketch_collection_limit_bp<minimizer_type, traits>(
                    window_size,
                    kmer_size,
                    gpu_sketch_purpose::sorted_pairs);
            const std::uint64_t context_bp =
                sketch_segment_context_bp<traits>(window_size, kmer_size);
            constexpr std::uint64_t tile_safety_bp = 4096u;
            const std::uint64_t core_bp =
                collected_limit > context_bp * 2u + tile_safety_bp + 1u
                    ? collected_limit - context_bp * 2u - tile_safety_bp
                    : std::max<std::uint64_t>(1u, collected_limit / 2u);
            const std::size_t core_units = std::max<std::size_t>(
                1u, static_cast<std::size_t>(core_bp / lpu));
            const std::size_t descriptor_units =
                descriptor.hi_unit - descriptor.lo_unit;
            const std::size_t initial_window_count =
                descriptor.window_bp <= collected_limit
                    ? 1u
                    : static_cast<std::size_t>(bioseq::common::ceil_div(
                        static_cast<std::uint64_t>(descriptor_units),
                        static_cast<std::uint64_t>(core_units)));
            const std::size_t maximum_sub_units = std::min(
                descriptor_units,
                core_units + std::min(
                    descriptor_units,
                    context_units * 2u));
            const std::uint64_t maximum_sub_bp = std::min(
                descriptor.window_bp,
                bioseq::common::saturating_mul(
                    static_cast<std::uint64_t>(maximum_sub_units),
                    static_cast<std::uint64_t>(lpu)));
            const std::uint64_t maximum_dispatch_minimizers =
                estimate_vulkan_minimizer_capacity(
                    maximum_sub_bp, window_size);
            const std::uint64_t maximum_sequence_bytes = aligned_upload_bytes(
                bioseq::common::saturating_mul(
                    static_cast<std::uint64_t>(maximum_sub_units),
                    static_cast<std::uint64_t>(sizeof(storage_unit))));
            const std::uint64_t maximum_resident_requested_bytes =
                vulkan_sketch_resident_bytes<minimizer_type>(
                    maximum_dispatch_minimizers);
            const std::uint64_t maximum_host_bytes =
                bioseq::common::saturating_mul(
                    bioseq::common::saturating_mul(
                        maximum_dispatch_minimizers,
                        static_cast<std::uint64_t>(sizeof(minimizer_type))),
                    bioseq::parallelism::k_vector_preallocation_headroom_factor);
            const gpu_sketch_memory_plan maximum_dispatch_plan =
                make_gpu_sketch_memory_plan(
                    maximum_sub_bp,
                    maximum_sequence_bytes,
                    sizeof(vulkan_rank_lut),
                    maximum_dispatch_minimizers,
                    maximum_resident_requested_bytes,
                    gpu_sketch_purpose::minimizer_words,
                    static_cast<std::uint32_t>(kmer_size),
                    static_cast<std::uint32_t>(window_size),
                    static_cast<std::uint32_t>(lpu),
                    runtime.features().max_workgroup_count_x);
            if (!maximum_dispatch_plan.is_valid) {
                path_decline.reason =
                    vulkan_sketch_decline_reason::memory_plan_invalid;
                throw_vulkan_strict_decline(
                    "Vulkan segment minimizer pair path", path_decline);
            }
            const gpu_sketch_batch_allocation_requirements
                maximum_allocation_requirements =
                    runtime.sketch_batch_allocation_requirements(
                        maximum_sequence_bytes,
                        maximum_dispatch_plan.tiles.tile_count,
                            static_cast<std::uint32_t>(
                            minimizer_wire_traits<minimizer_type>::k_word_kind),
                        minimizer_wire_traits<
                            minimizer_type>::k_words32_per_minimizer,
                        maximum_dispatch_minimizers);
            if (!maximum_allocation_requirements.valid) {
                path_decline.reason =
                    vulkan_sketch_decline_reason::memory_plan_invalid;
                throw_vulkan_strict_decline(
                    "Vulkan segment minimizer pair path", path_decline);
            }
            const std::uint64_t maximum_physical_arena_bytes =
                runtime.gpu_arena_allocation_requirement(
                    maximum_dispatch_plan.arena_scaffold_bytes);
            const std::uint64_t maximum_host_transient_headroom_bytes =
                vulkan_exact_host_allocation_admission_bytes(
                    maximum_dispatch_plan.count_submit_host_transient_bytes);
            const std::uint64_t maximum_dispatch_bytes =
                vulkan_sketch_dispatch_admission_bytes(
                    maximum_physical_arena_bytes,
                    maximum_host_transient_headroom_bytes,
                    0u,
                    maximum_allocation_requirements
                        .pessimistic_output_allocation_bytes);
            const std::uint64_t maximum_host_materialization_bytes =
                vulkan_sketch_host_materialization_admission_bytes(
                    maximum_allocation_requirements
                        .pessimistic_output_allocation_bytes,
                    maximum_host_bytes);
            // The borrowed slice covers the larger of the mutually exclusive
            // dispatch and exact-host materialization. It must use the same physical
            // arena/output requirements as `sketch_uploaded_window_result`,
            // including a reusable arena's high-water allocation.
            const std::uint64_t dispatch_budget = std::max(
                maximum_dispatch_bytes,
                maximum_host_materialization_bytes);
            const std::uint64_t initial_filtered_count =
                estimate_vulkan_minimizer_capacity(
                    descriptor.window_bp, window_size);
            const auto headroom_bytes =
                []<class p_value>(std::uint64_t count) {
                return bioseq::common::saturating_mul(
                    bioseq::common::saturating_mul(
                        count, static_cast<std::uint64_t>(sizeof(p_value))),
                    bioseq::parallelism::k_vector_preallocation_headroom_factor);
            };
            const std::uint64_t initial_owner_bytes = bioseq::common::saturating_add(
                dispatch_budget,
                bioseq::common::saturating_add(
                    headroom_bytes.template operator()<gpu_sketch_segment_window>(
                        initial_window_count),
                    bioseq::common::saturating_add(
                        headroom_bytes.template operator()<storage_unit>(
                            maximum_sub_units),
                        headroom_bytes.template operator()<
                            std::pair<std::uint64_t, std::uint64_t>>(
                                initial_filtered_count))));
            if (dispatch_budget == (std::numeric_limits<std::uint64_t>::max)()
                || initial_owner_bytes
                    == (std::numeric_limits<std::uint64_t>::max)()) {
                path_decline.reason =
                    vulkan_sketch_decline_reason::memory_plan_invalid;
                throw_vulkan_strict_decline(
                    "Vulkan segment minimizer pair path", path_decline);
            }
            auto permit =
                bioseq::parallelism::memory_admission_controller::instance()
                    .try_reserve(initial_owner_bytes);
            if (!permit) {
                path_decline.reason =
                    vulkan_sketch_decline_reason::memory_admission_denied;
                throw_vulkan_strict_decline(
                    "Vulkan segment minimizer pair path", path_decline);
            }
            /** @brief Owns admitted work vectors for one segment-pair call. */
            struct segment_pair_owner {
                bioseq::parallelism::memory_permit permit;
                std::vector<gpu_sketch_segment_window> windows;
                std::vector<storage_unit> scratch;
                std::vector<std::pair<std::uint64_t, std::uint64_t>> filtered;
            } owner{
                .permit = std::move(*permit),
                .windows = {},
                .scratch = {},
                .filtered = {},
            };
            owner.scratch.reserve(maximum_sub_units);
            owner.filtered.reserve(static_cast<std::size_t>(
                initial_filtered_count));
            auto windows = sketch_segment_windows<minimizer_type>(
                seq,
                descriptor,
                window_size,
                kmer_size,
                gpu_sketch_purpose::sorted_pairs,
                collected_limit);
            owner.windows.swap(windows);

            const auto retained_owner_bytes = [&owner]() noexcept {
                std::uint64_t bytes = bioseq::common::saturating_mul(
                    static_cast<std::uint64_t>(owner.windows.capacity()),
                    static_cast<std::uint64_t>(sizeof(gpu_sketch_segment_window)));
                bytes = bioseq::common::saturating_add(bytes, bioseq::common::saturating_mul(
                    static_cast<std::uint64_t>(owner.scratch.capacity()),
                    static_cast<std::uint64_t>(sizeof(storage_unit))));
                return bioseq::common::saturating_add(bytes, bioseq::common::saturating_mul(
                    static_cast<std::uint64_t>(owner.filtered.capacity()),
                    static_cast<std::uint64_t>(
                        sizeof(std::pair<std::uint64_t, std::uint64_t>))));
            };
            const auto reconcile_owner = [&retained_owner_bytes, &dispatch_budget, &owner]() {
                const std::uint64_t exact = retained_owner_bytes();
                const std::uint64_t live = bioseq::common::saturating_add(exact, dispatch_budget);
                if (exact == (std::numeric_limits<std::uint64_t>::max)()
                    || live == (std::numeric_limits<std::uint64_t>::max)()
                    || live > owner.permit.bytes()) {
                    throw vulkan_runtime_error{
                        "Vulkan segment pair owner exceeded admitted capacity"};
                }
                owner.permit.shrink_to(live);
            };
            const auto ensure_owner_capacity = [&retained_owner_bytes,
                                                &dispatch_budget,
                                                &headroom_bytes,
                                                &owner,
                                                &reconcile_owner]<class p_value>(
                                                   std::vector<p_value>& values,
                                                   std::size_t required) {
                if (values.capacity() >= required) {
                    return;
                }
                const std::uint64_t peak = bioseq::common::saturating_add(
                    bioseq::common::saturating_add(retained_owner_bytes(), dispatch_budget),
                    headroom_bytes.template operator()<p_value>(required));
                if (peak == (std::numeric_limits<std::uint64_t>::max)()
                    || !owner.permit.grow_to(peak)) {
                    throw vulkan_runtime_error{
                        "Vulkan segment pair owner growth admission failed"};
                }
                values.reserve(required);
                reconcile_owner();
            };
            reconcile_owner();
            for (std::size_t window_index = 0u;
                 window_index < owner.windows.size();
                 ++window_index) {
                const gpu_sketch_segment_window window = owner.windows[window_index];
                const segment_upload_descriptor sub_descriptor =
                    compute_segment_descriptor(
                        seq, window.sub_lo_unit, window.sub_hi_unit);
                if (sub_descriptor.window_bp
                    < minimum_bases) {
                    continue;
                }
                const std::size_t unit_count =
                    sub_descriptor.hi_unit - sub_descriptor.lo_unit;
                if (unit_count > owner.scratch.capacity()) {
                    ensure_owner_capacity(owner.scratch, unit_count);
                }
                owner.scratch.resize(unit_count);
                auto begin_iter = seq.cbegin();
                begin_iter += static_cast<bioseq::storage::sequence_size_t>(
                    sub_descriptor.lo_unit);
                const auto* base = seq.read_cptr(begin_iter);
                const sequence_upload_view upload =
                    make_upload_view_from_window<bare>(seq, base, sub_descriptor);
                const std::uint64_t max_minimizers =
                    estimate_vulkan_minimizer_capacity(upload.bp_size, window_size);
                bool capacity_overflowed = false;
                vulkan_sketch_decline decline{};
                auto result = sketch_uploaded_window_result<minimizer_type>(
                    upload,
                    rank_lut,
                    window_size,
                    kmer_size,
                    max_minimizers,
                    &capacity_overflowed,
                    {},
                    &decline,
                    std::addressof(owner.permit),
                    dispatch_budget);
                if (!result) {
                    if (capacity_overflowed
                        != (decline.reason
                            == vulkan_sketch_decline_reason::
                                output_capacity_overflow)) {
                        throw vulkan_runtime_error{
                            "Vulkan segment minimizer pair decline/capacity "
                            "identity mismatch"};
                    }
                    gpu_sketch_segment_window left{};
                    gpu_sketch_segment_window right{};
                    if (vulkan_sketch_decline_allows_segment_split(
                            decline.reason)
                        && split_sketch_segment_window(
                            seq,
                            descriptor,
                            window,
                            context_units,
                            left,
                            right)) {
                        execution_stats.record_gpu_window_split();
                        if (owner.windows.size()
                                > (std::numeric_limits<std::size_t>::max)() - 2u) {
                            throw vulkan_runtime_error{
                                "Vulkan segment window split count overflow"};
                        }
                        ensure_owner_capacity(
                            owner.windows, owner.windows.size() + 2u);
                        const auto insert_at =
                            static_cast<std::vector<gpu_sketch_segment_window>::difference_type>(
                                window_index + 1u);
                        owner.windows.insert(
                            owner.windows.begin() + insert_at, right);
                        owner.windows.insert(
                            owner.windows.begin() + insert_at, left);
                        continue;
                    }
                    const std::string stage = fmt::format(
                        "Vulkan segment minimizer pair build "
                        "units=[{},{}) segment_units=[{},{}) "
                        "emit_base={} core=[{},{})",
                        sub_descriptor.lo_unit,
                        sub_descriptor.hi_unit,
                        descriptor.lo_unit,
                        descriptor.hi_unit,
                        static_cast<std::uint64_t>(emit_base_bp),
                        static_cast<std::uint64_t>(core_lo_bp),
                        static_cast<std::uint64_t>(core_hi_bp));
                    throw_vulkan_strict_decline(stage, decline);
                }

                if (!result->dispatch_token.is_internally_consistent()) {
                    throw vulkan_runtime_error{
                        "Vulkan segment minimizer pair path returned without "
                        "an is_internally_consistent operation-originated dispatch token"};
                }
                execution_stats.record_completed_roundtrip(
                    result->host_minimizers.size());

                const std::uint64_t sub_emit_base_bp =
                    static_cast<std::uint64_t>(emit_base_bp)
                    + static_cast<std::uint64_t>(
                        sub_descriptor.lo_unit - descriptor.lo_unit)
                        * static_cast<std::uint64_t>(lpu);
                if (result->host_minimizers.size()
                        > (std::numeric_limits<std::size_t>::max)()
                            - owner.filtered.size()) {
                    throw vulkan_runtime_error{
                        "Vulkan segment filtered pair count overflow"};
                }
                ensure_owner_capacity(
                    owner.filtered,
                    owner.filtered.size() + result->host_minimizers.size());
                for (const minimizer_type& minimizer : result->host_minimizers) {
                    const bioseq::storage::sequence_size_t global_pos =
                        static_cast<bioseq::storage::sequence_size_t>(
                            sub_emit_base_bp + minimizer.last_pos());
                    if (global_pos < core_lo_bp || global_pos >= core_hi_bp) {
                        continue;
                    }
                    owner.filtered.emplace_back(
                        minimizer.kmer(),
                        (static_cast<std::uint64_t>(global_pos) << 1)
                            | static_cast<std::uint64_t>(minimizer.strand()));
                }
                {
                    std::vector<minimizer_type> empty;
                    result->host_minimizers.swap(empty);
                }
                result->host_permit.reset();
                result->device.reset();
            }
            // std::pair's default ordering sorts by (kmer, encoded_pos), exactly
            // the order the minimizer-sequence and minimizer-fragment builders
            // expect from a "sorted pair run".
            std::sort(owner.filtered.begin(), owner.filtered.end());
            owner.filtered.erase(
                std::unique(owner.filtered.begin(), owner.filtered.end()),
                owner.filtered.end());
            if (execution_stats_out != nullptr) {
                *execution_stats_out = execution_stats;
            }
            {
                std::vector<gpu_sketch_segment_window> empty_windows;
                std::vector<storage_unit> empty_scratch;
                owner.windows.swap(empty_windows);
                owner.scratch.swap(empty_scratch);
            }
            const std::uint64_t exact_pair_bytes =
                bioseq::common::saturating_mul(
                    static_cast<std::uint64_t>(owner.filtered.capacity()),
                    static_cast<std::uint64_t>(
                        sizeof(std::pair<std::uint64_t, std::uint64_t>)));
            if (exact_pair_bytes
                    == (std::numeric_limits<std::uint64_t>::max)()
                || exact_pair_bytes > owner.permit.bytes()) {
                throw vulkan_runtime_error{
                    "Vulkan segment returned pairs exceeded admission"};
            }
            owner.permit.shrink_to(exact_pair_bytes);
            vulkan_owned_segment_pairs output;
            output.pairs.swap(owner.filtered);
            output.permit = std::move(owner.permit);
            return output;
        }
    }
}
// BIOSEQ_PRIVATE_PACKED_ACCESS_END collect_sorted_minimizer_pairs_vulkan_segment_resident_impl_or_throw

/**
 * @brief Collects minimizers for one resident segment through a collector.
 * @tparam p_minimizer_type Packed minimizer record.
 * @tparam p_collector_type Collector accepting individual records.
 * @tparam p_sequence Contiguous packed sequence type.
 * @param collector Destination collector.
 * @param seq Source sequence.
 * @param lo_unit Inclusive upload unit index.
 * @param hi_unit Exclusive upload unit index.
 * @param window_size Window length in k-mers.
 * @param kmer_size K-mer length in biological symbols.
 * @param query_gate Optional canonical query filter.
 * @throws vulkan_runtime_error For every selected-Vulkan rejection.
 */
// BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN collect_minimizers_vulkan_segment_resident_impl_or_throw
template <class p_minimizer_type, class p_collector_type, class p_sequence>
void collect_minimizers_vulkan_segment_resident_impl_or_throw(
    p_collector_type& collector,
    const p_sequence& seq,
    std::size_t lo_unit,
    std::size_t hi_unit,
    std::size_t window_size,
    std::size_t kmer_size,
    vulkan_sketch_query_gate query_gate = {})
{
    using bare = std::remove_cvref_t<p_sequence>;
    vulkan_sketch_decline path_decline{
        .bp_size = vulkan_decline_bp_size(seq),
        .query_gate_words = static_cast<std::uint64_t>(
            query_gate.words.size()),
        .window_size = window_size,
        .kmer_size = kmer_size,
    };
    if constexpr (!vulkan_sketchable_sequence<bare>) {
        path_decline.reason =
            vulkan_sketch_decline_reason::fragmented_sequence_unsupported;
        throw_vulkan_strict_decline(
            "Vulkan segment minimizer sketch", path_decline);
    } else {
        using traits = typename bare::traits;
        using minimizer_limit = bioseq::minimizers::minimap_sketch_data;

        if constexpr (!vulkan_minimizer_alphabet_shape_supported<traits>()) {
            path_decline.reason = vulkan_sketch_decline_reason::alphabet_unsupported;
            throw_vulkan_strict_decline(
                "Vulkan segment minimizer sketch", path_decline);
        } else if (!vulkan_minimizer_kmer_size_supported<traits>(kmer_size)) {
            path_decline.reason =
                vulkan_sketch_decline_reason::kmer_size_unsupported;
            throw_vulkan_strict_decline(
                "Vulkan segment minimizer sketch", path_decline);
        } else if (!vulkan_minimizer_window_size_supported(window_size)) {
            path_decline.reason =
                vulkan_sketch_decline_reason::window_size_unsupported;
            throw_vulkan_strict_decline(
                "Vulkan segment minimizer sketch", path_decline);
        }

        const segment_upload_descriptor descriptor =
            compute_segment_descriptor(seq, lo_unit, hi_unit);
        path_decline.bp_size = descriptor.window_bp;
        if (descriptor.window_bp == 0u) {
            path_decline.reason = vulkan_sketch_decline_reason::empty_sequence;
            throw_vulkan_strict_decline(
                "Vulkan segment minimizer sketch", path_decline);
        }
        if (descriptor.window_bp
            > static_cast<std::uint64_t>(minimizer_limit::k_position_field_mask)) {
            path_decline.reason =
                vulkan_sketch_decline_reason::segment_position_field_overflow;
            throw_vulkan_strict_decline(
                "Vulkan segment minimizer sketch", path_decline);
        }
        if (descriptor.window_bp
            < static_cast<std::uint64_t>(window_size + kmer_size)) {
            return;
        }

        auto& runtime = bioseq::gpu::vulkan::runtime::instance();
        if (!hardware_device_available(runtime)) {
            path_decline.reason =
                vulkan_sketch_decline_reason::hardware_device_unavailable;
            throw_vulkan_strict_decline(
                "Vulkan segment minimizer sketch", path_decline);
        }
        const vulkan_rank_lut rank_lut = make_vulkan_rank_lut<bare>();
        const std::size_t lpu = std::max<std::size_t>(1u, traits::letters_per_unit);
        const std::uint64_t outer_base_bp =
            static_cast<std::uint64_t>(descriptor.lo_unit) * lpu;
        std::vector<gpu_sketch_segment_window> windows =
            sketch_segment_windows<p_minimizer_type>(
                seq,
                descriptor,
                window_size,
                kmer_size,
                gpu_sketch_purpose::minimizer_words);
        if (query_gate.enabled() && windows.size() != 1u) {
            path_decline.reason = vulkan_sketch_decline_reason::
                query_gate_requires_single_window;
            throw_vulkan_strict_decline(
                "query-gated Vulkan segment minimizer sketch", path_decline);
        }
        const std::size_t context_units =
            sketch_segment_context_units<traits>(window_size, kmer_size);
        for (std::size_t window_index = 0u;
             window_index < windows.size();
             ++window_index) {
            const gpu_sketch_segment_window window = windows[window_index];
            const segment_upload_descriptor sub_descriptor =
                compute_segment_descriptor(seq, window.sub_lo_unit, window.sub_hi_unit);
            if (sub_descriptor.window_bp == 0u
                || sub_descriptor.window_bp
                    < static_cast<std::uint64_t>(window_size + kmer_size)) {
                continue;
            }
            auto begin_iter = seq.cbegin();
            begin_iter += static_cast<bioseq::storage::sequence_size_t>(
                sub_descriptor.lo_unit);
            const auto* base = seq.read_cptr(begin_iter);
            const sequence_upload_view upload =
                make_upload_view_from_window<bare>(seq, base, sub_descriptor);

            const std::uint64_t max_minimizers =
                estimate_vulkan_minimizer_capacity(upload.bp_size, window_size);
            bool capacity_overflowed = false;
            vulkan_sketch_decline decline{};
            auto result = sketch_uploaded_window_result<p_minimizer_type>(
                upload,
                rank_lut,
                window_size,
                kmer_size,
                max_minimizers,
                &capacity_overflowed,
                query_gate,
                &decline);
            if (!result) {
                if (capacity_overflowed
                    != (decline.reason
                        == vulkan_sketch_decline_reason::
                            output_capacity_overflow)) {
                    throw vulkan_runtime_error{
                        "Vulkan segment minimizer sketch decline/capacity "
                        "identity mismatch"};
                }
                if (query_gate.enabled()) {
                    throw_vulkan_strict_decline(
                        "query-gated Vulkan segment minimizer sketch", decline);
                }
                gpu_sketch_segment_window left{};
                gpu_sketch_segment_window right{};
                if (vulkan_sketch_decline_allows_segment_split(decline.reason)
                    && split_sketch_segment_window(
                        seq,
                        descriptor,
                        window,
                        context_units,
                        left,
                        right)) {
                    const auto insert_at =
                        static_cast<std::vector<gpu_sketch_segment_window>::difference_type>(
                            window_index + 1u);
                    windows.insert(windows.begin() + insert_at, right);
                    windows.insert(windows.begin() + insert_at, left);
                    continue;
                }
                const std::string stage = fmt::format(
                    "Vulkan segment minimizer sketch "
                    "units=[{},{}) outer_units=[{},{}) core_units=[{},{})",
                    sub_descriptor.lo_unit,
                    sub_descriptor.hi_unit,
                    descriptor.lo_unit,
                    descriptor.hi_unit,
                    window.core_lo_unit,
                    window.core_hi_unit);
                throw_vulkan_strict_decline(stage, decline);
            }

            const std::uint64_t core_lo_bp =
                static_cast<std::uint64_t>(window.core_lo_unit) * lpu;
            const std::uint64_t core_hi_bp =
                static_cast<std::uint64_t>(window.core_hi_unit) * lpu;
            const std::uint64_t sub_base_bp =
                static_cast<std::uint64_t>(sub_descriptor.lo_unit) * lpu;
            for (const p_minimizer_type& minimizer : result->host_minimizers) {
                const std::uint64_t abs_pos =
                    sub_base_bp + minimizer.last_pos();
                if (abs_pos < core_lo_bp || abs_pos >= core_hi_bp) {
                    continue;
                }
                collector.collect(p_minimizer_type{
                    minimizer.kmer(),
                    abs_pos - outer_base_bp,
                    minimizer.strand()});
            }
        }
    }
}
// BIOSEQ_PRIVATE_PACKED_ACCESS_END collect_minimizers_vulkan_segment_resident_impl_or_throw

} // namespace bioseq::gpu::vulkan
