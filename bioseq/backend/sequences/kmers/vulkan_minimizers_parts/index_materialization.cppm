/**
 * @file index_materialization.cppm
 * @brief Builds a target minimizer, index on the GPU from prepared input.
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

export module bioseq.sequences.kmers.vulkan_minimizers:index_materialization;

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

export namespace bioseq::gpu::vulkan {

/**
 * @brief Builds a target minimizer index that  remains in device memory.
 *
 * Reserves memory for counting first. The runtime then uses the exact count to
 * size the index and checks its allocation against the memory budget.
 * @param upload Packed input kept alive until the runtime finishes reading it.
 * @param rank_lut Alphabet decoding table.
 * @param window_size K-mers per window.
 * @param kmer_size Symbols per k-mer.
 * @param query_gate Optional bitmap filtering target hashes.
 * @param decline_out Optional rejection details.
 * @param owned_sequence_staging_bytes Bytes in caller-owned temporary input.
 * @param release_owned_sequence_staging Releases that input once upload is complete.
 * @return Device index, or no value when input, memory, or build checks fail.
 * @throws vulkan_runtime_error If  execuion fails.
 */
template <class p_minimizer_type>
[[nodiscard]] std::optional<gpu_built_index>
build_gpu_index_from_upload_on_device(
    const sequence_upload_view& upload,
    const vulkan_rank_lut& rank_lut,
    std::size_t window_size,
    std::size_t kmer_size,
    vulkan_sketch_query_gate query_gate = {},
    vulkan_sketch_decline* decline_out = nullptr,
    std::uint64_t owned_sequence_staging_bytes = 0u,
    std::function<void()> release_owned_sequence_staging = {})
{
    using bioseq::parallelism::memory_admission_controller;

    if (decline_out != nullptr) {
        *decline_out = vulkan_sketch_decline{
            .bp_size = upload.bp_size,
            .padded_sequence_bytes = upload.padded_byte_size,
            .query_gate_words = static_cast<std::uint64_t>(
                query_gate.words.size()),
            .window_size = window_size,
            .kmer_size = kmer_size,
        };
    }
    const auto decline = [decline_out](
            vulkan_sketch_decline_reason reason,
            std::uint64_t max_minimizers = 0u) noexcept {
        if (decline_out != nullptr) {
            decline_out->reason = reason;
            decline_out->max_minimizers = max_minimizers;
        }
    };

    if (upload.bp_size == 0u) {
        decline(vulkan_sketch_decline_reason::empty_sequence);
        return std::nullopt;
    }
    if (upload.bp_size > std::numeric_limits<std::uint32_t>::max()) {
        decline(vulkan_sketch_decline_reason::sequence_size_exceeds_u32);
        return std::nullopt;
    }
    if (upload.padded_byte_size
        == std::numeric_limits<std::uint64_t>::max()) {
        decline(vulkan_sketch_decline_reason::padded_sequence_size_overflow);
        return std::nullopt;
    }
    if (query_gate.words.size()
        > static_cast<std::size_t>(
            (std::numeric_limits<std::uint32_t>::max)())) {
        decline(vulkan_sketch_decline_reason::query_gate_exceeds_u32);
        return std::nullopt;
    }
    auto& runtime = bioseq::gpu::vulkan::runtime::instance();
    if (!hardware_device_available(runtime)) {
        decline(vulkan_sketch_decline_reason::hardware_device_unavailable);
        return std::nullopt;
    }

    static std::mutex target_index_build_mutex;
    std::lock_guard<std::mutex> target_index_build_guard{
        target_index_build_mutex};

    const std::uint64_t max_minimizers = std::min(
        estimate_vulkan_minimizer_capacity(upload.bp_size, window_size),
        target_minimizer_count_upper_bound(
            upload.bp_size, static_cast<std::uint32_t>(kmer_size)));
    if (decline_out != nullptr) {
        decline_out->max_minimizers = max_minimizers;
    }

    const gpu_sketch_memory_plan memory_plan = make_gpu_sketch_memory_plan(
        upload.bp_size,
        upload.padded_byte_size,
        sizeof(vulkan_rank_lut),
        max_minimizers,
        0u,
        gpu_sketch_purpose::target_index_build,
        static_cast<std::uint32_t>(kmer_size),
        static_cast<std::uint32_t>(window_size),
        upload.letters_per_unit,
        runtime.features().max_workgroup_count_x);
    if (!memory_plan.is_valid) {
        decline(vulkan_sketch_decline_reason::memory_plan_invalid, max_minimizers);
        return std::nullopt;
    }
    const gpu_target_index_host_scratch_plan host_scratch_plan =
        make_gpu_target_index_host_scratch_plan(
            memory_plan, owned_sequence_staging_bytes);
    if (!host_scratch_plan.is_valid
        || (owned_sequence_staging_bytes != 0u
            && !release_owned_sequence_staging)) {
        decline(vulkan_sketch_decline_reason::memory_plan_invalid, max_minimizers);
        return std::nullopt;
    }
    const std::uint64_t query_gate_bytes =
        bioseq::common::saturating_mul(
            static_cast<std::uint64_t>(query_gate.words.size()),
            sizeof(std::uint32_t));
    const std::uint64_t count_arena_request = bioseq::common::saturating_add(
        memory_plan.arena_scaffold_bytes,
        query_gate_bytes == 0u
            ? 0u
            : bioseq::common::saturating_add(query_gate_bytes,
                      std::uint64_t{k_gpu_buffer_offset_alignment}));
    const std::uint64_t count_arena_bytes =
        runtime.gpu_arena_allocation_requirement(count_arena_request);
    const std::uint64_t admission_bytes = bioseq::common::saturating_add(
        count_arena_bytes, host_scratch_plan.count_submit_bytes);
    if (query_gate_bytes == (std::numeric_limits<std::uint64_t>::max)()
        || admission_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
        decline(vulkan_sketch_decline_reason::memory_plan_invalid, max_minimizers);
        return std::nullopt;
    }

    const auto query_gate_identity = query_gate.canonical_identity();
    const vulkan_sketch_params sketch_params{
        .bp_size = static_cast<std::uint32_t>(upload.bp_size),
        .window_size = static_cast<std::uint32_t>(window_size),
        .kmer_size = static_cast<std::uint32_t>(kmer_size),
        .hash_bits = minimizer_wire_traits<p_minimizer_type>::k_hash_bits,
        .hash_shift = minimizer_wire_traits<p_minimizer_type>::k_hash_shift,
        .pos_bits = minimizer_wire_traits<p_minimizer_type>::k_position_bits,
        .minimizer_word_kind = static_cast<std::uint32_t>(
            minimizer_wire_traits<p_minimizer_type>::k_word_kind),
        .minimizer_words32 =
            minimizer_wire_traits<p_minimizer_type>::k_words32_per_minimizer,
        .alphabet_kind = static_cast<std::uint32_t>(upload.alphabet_kind),
        .unit_bits = upload.unit_bits,
        .bits_per_char = upload.bits_per_char,
        .letters_per_unit = upload.letters_per_unit,
        .alphabet_size = upload.alphabet_size,
        .seq_word_offset = 0u,
        .char_mask = upload.char_mask,
        .padding_letters = upload.padding_letters,
    };
    // Prepare the target-index pipelines before reserving dispatch memory.
    // Their cached metadata lives for the process lifetime and needs its own
    // reservation, which is harder to obtain after large GPU buffers are mapped.
    runtime.prepare_target_index_pipelines(sketch_params);

    auto& admission = memory_admission_controller::instance();
    bioseq::parallelism::memory_admission_failure admission_failure{};
    auto permit = admission.try_reserve(admission_bytes, &admission_failure);
    if (!permit) {
        if (decline_out != nullptr) {
            decline_out->reason =
                vulkan_sketch_decline_reason::memory_admission_denied;
            decline_out->max_minimizers = max_minimizers;
            decline_out->admission = admission_failure;
        }
        bioseq_log<logging_level::error>(
            "[gpu sketch index] target-index build admission failed "
            "({})",
            format_vulkan_sketch_decline(
                decline_out != nullptr
                    ? *decline_out
                    : vulkan_sketch_decline{
                        .reason = vulkan_sketch_decline_reason::
                            memory_admission_denied,
                        .bp_size = upload.bp_size,
                        .padded_sequence_bytes = upload.padded_byte_size,
                        .max_minimizers = max_minimizers,
                        .query_gate_words = static_cast<std::uint64_t>(
                            query_gate.words.size()),
                        .window_size = window_size,
                        .kmer_size = kmer_size,
                        .admission = admission_failure,
                    }));
        return std::nullopt;
    }

    try {
        gpu_built_index built{};
        const auto materialization = admission.materialize(
            *permit,
            [&built,
             &runtime,
             &upload,
             &owned_sequence_staging_bytes,
             &release_owned_sequence_staging,
             &rank_lut,
             &sketch_params,
             &max_minimizers,
             &query_gate,
             &query_gate_identity,
             &permit]() -> bioseq::parallelism::memory_materialization_snapshot {
                built = runtime.build_target_index(
                    std::span<const std::byte>{
                        static_cast<const std::byte*>(upload.bytes),
                        static_cast<std::size_t>(upload.byte_size)},
                    upload.padded_byte_size,
                    owned_sequence_staging_bytes,
                    std::move(release_owned_sequence_staging),
                    rank_lut,
                    sketch_params,
                    max_minimizers,
                    query_gate.words,
                    query_gate.mask,
                    query_gate_identity,
                    &*permit);

                if (!built.valid) {
                    return {};
                }
                // The runtime records arena cleanup while holding its mutex. Check that
                // result here, since another request may already be using the shared arena.
                if (built.ownership.arena_trimmed_before_permit_shrink != 1u) {
                    built.valid = false;
                    built.failure_reason = gpu_index_build_failure::
                        index_invariant_failed;
                    built.positions.reset();
                    built.slots.reset();
                    return {};
                }
                if (built.ownership.peak_observed_bytes > permit->bytes()) {
                    built.valid = false;
                    built.failure_reason = gpu_index_build_failure::
                        peak_allocation_exceeds_admission;
                    built.positions.reset();
                    built.slots.reset();
                    return {};
                }
                const std::uint64_t resident_bytes = bioseq::common::saturating_add(
                    built.slots.allocation_bytes,
                    built.positions.allocation_bytes);
                if (resident_bytes > permit->bytes()) {
                    built.valid = false;
                    built.failure_reason = gpu_index_build_failure::
                        peak_allocation_exceeds_admission;
                    built.positions.reset();
                    built.slots.reset();
                    return {};
                }
                permit->shrink_to(resident_bytes);
                built.ownership.resident_bytes = resident_bytes;
                const bool host_visible_coherent =
                    built.slots.is_host_visible_coherent()
                    && built.positions.is_host_visible_coherent();
                return bioseq::parallelism::memory_materialization_snapshot{
                    .actual_allocation_bytes = resident_bytes,
                    .fully_touched_bytes = resident_bytes,
                    .is_host_visible = host_visible_coherent,
                    .is_host_coherent = host_visible_coherent,
                    .is_process_footprint_reflected =
                        built.slots.process_footprint_reflected
                        && built.positions.process_footprint_reflected,
                };
            });
        if (!built.valid) {
            if (decline_out != nullptr
                && built.execution_peak_admission_failure.has_failure()) {
                decline_out->admission = built.execution_peak_admission_failure;
            }
            decline(
                vulkan_target_index_failure_decline_reason(
                    built.failure_reason),
                max_minimizers);
            return std::nullopt;
        }
        built.materialization = materialization;
        built.ownership.materialization_admitted_bytes =
            materialization.admitted_bytes;
        built.ownership.materialization_actual_allocation_bytes =
            materialization.allocation_snapshot.actual_allocation_bytes;
        built.ownership.materialization_fully_touched_bytes =
            materialization.allocation_snapshot.fully_touched_bytes;
        built.ownership.materialization_reflected_delta_bytes =
            materialization.reflected_delta_bytes;
        built.ownership.materialization_permit_released =
            materialization.is_permit_released ? 1u : 0u;
        built.ownership.materialization_host_visible =
            materialization.allocation_snapshot.is_host_visible ? 1u : 0u;
        built.ownership.materialization_host_coherent =
            materialization.allocation_snapshot.is_host_coherent ? 1u : 0u;
        built.ownership.materialization_process_footprint_reflected =
            materialization.allocation_snapshot.is_process_footprint_reflected ? 1u : 0u;
        built.permit = std::move(*permit);
        return built;
    } catch (const vulkan_runtime_error&) {
        throw;
    }
}

} // namespace bioseq::gpu::vulkan
