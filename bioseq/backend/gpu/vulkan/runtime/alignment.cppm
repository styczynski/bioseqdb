/**
 * @file alignment.cppm
 * @brief Executes Vulkan alignment extension and bounded CIGAR reconstruction.
 */
module;

#include <cstdint>
#include <libassert/assert.hpp>
#include <new>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

module bioseq.gpu.vulkan.runtime:alignment;

import :frontend_types;
import :support;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.common.hashing;
import bioseq.parallelism.memory_admission;
import bioseq.instrumentation;
import std;

namespace bioseq::gpu::vulkan {

/// @brief Admit host result-vector growth before allocating compact CIGAR readback.
void reserve_alignment_cigar_result_capacity(alignment_cigar_batch_result& result,
                                             const std::size_t result_count,
                                             const std::size_t cigar_word_count) {
    using bioseq::common::saturating_add;
    using bioseq::common::saturating_mul;
    if (result_count < result.results.size() || cigar_word_count < result.packed_cigar.size() ||
        result_count > result.results.max_size() ||
        cigar_word_count > result.packed_cigar.max_size()) {
        throw vulkan_runtime_error("alignment CIGAR compact result capacity is invalid");
    }
    const std::uint64_t requested =
        saturating_add(saturating_mul(saturating_mul(static_cast<std::uint64_t>(result_count),
                                                     sizeof(gpu_alignment_cigar_result_wire)),
                                      bioseq::parallelism::k_vector_preallocation_headroom_factor),
                       saturating_mul(saturating_mul(static_cast<std::uint64_t>(cigar_word_count),
                                                     sizeof(std::uint32_t)),
                                      bioseq::parallelism::k_vector_preallocation_headroom_factor));
    if (requested == 0u || requested == (std::numeric_limits<std::uint64_t>::max)()) {
        throw vulkan_runtime_error("alignment CIGAR compact result admission overflow");
    }
    bioseq::parallelism::memory_admission_failure failure{};
    if (!result.capacity_permit) {
        auto permit = bioseq::parallelism::memory_admission_controller::instance().try_reserve(
            requested, &failure);
        if (!permit) {
            throw vulkan_runtime_error(fmt::format(
                "alignment CIGAR compact result admission denied: reason={} requested={}",
                bioseq::parallelism::memory_admission_failure_reason_name(failure.reason),
                failure.requested_bytes));
        }
        result.capacity_permit = std::move(*permit);
    } else if (requested > result.capacity_permit.bytes() &&
               !result.capacity_permit.grow_to(requested, &failure)) {
        throw vulkan_runtime_error(
            fmt::format("alignment CIGAR compact result growth denied: reason={} requested={}",
                        bioseq::parallelism::memory_admission_failure_reason_name(failure.reason),
                        failure.requested_bytes));
    }
    try {
        result.results.reserve(result_count);
        result.packed_cigar.reserve(cigar_word_count);
    } catch (const std::bad_alloc&) {
        throw vulkan_runtime_error(
            "alignment CIGAR compact result allocation failed after admission");
    }
    const std::uint64_t retained =
        saturating_add(saturating_mul(static_cast<std::uint64_t>(result.results.capacity()),
                                      sizeof(gpu_alignment_cigar_result_wire)),
                       saturating_mul(static_cast<std::uint64_t>(result.packed_cigar.capacity()),
                                      sizeof(std::uint32_t)));
    if (retained == (std::numeric_limits<std::uint64_t>::max)() ||
        retained > result.capacity_permit.bytes()) {
        throw vulkan_runtime_error("alignment CIGAR compact result exceeded its admission");
    }
    result.capacity_permit.shrink_to(retained);
}

/// @brief Admits every host vector retained by one physical MAPQ readback.
void reserve_mapq_result_capacity(gpu_mapq_batch_result& result,
                                  const std::size_t problem_count,
                                  const std::size_t candidate_count) {
    using bioseq::common::saturating_add;
    using bioseq::common::saturating_mul;
    if (problem_count == 0u || problem_count > result.headers.max_size() ||
        candidate_count > result.candidates.max_size() ||
        candidate_count > result.results.max_size()) {
        throw vulkan_runtime_error("persisted MAPQ host result capacity is invalid");
    }
    std::uint64_t retained = saturating_mul(static_cast<std::uint64_t>(problem_count),
                                            sizeof(gpu_chain_candidate_header_wire));
    retained = saturating_add(retained,
                              saturating_mul(static_cast<std::uint64_t>(candidate_count),
                                             sizeof(gpu_chain_candidate_wire)));
    retained = saturating_add(
        retained,
        saturating_mul(static_cast<std::uint64_t>(candidate_count), sizeof(gpu_mapq_result_wire)));
    const std::uint64_t peak =
        saturating_mul(retained, bioseq::parallelism::k_vector_preallocation_headroom_factor);
    if (retained == 0u || retained == (std::numeric_limits<std::uint64_t>::max)() ||
        peak == (std::numeric_limits<std::uint64_t>::max)()) {
        throw vulkan_runtime_error("persisted MAPQ host result admission overflow");
    }
    bioseq::parallelism::memory_admission_failure failure{};
    auto permit =
        bioseq::parallelism::memory_admission_controller::instance().try_reserve(peak, &failure);
    if (!permit) {
        throw vulkan_runtime_error(
            fmt::format("persisted MAPQ host result admission denied: reason={} requested={}",
                        bioseq::parallelism::memory_admission_failure_reason_name(failure.reason),
                        failure.requested_bytes));
    }
    try {
        result.headers.reserve(problem_count);
        result.candidates.reserve(candidate_count);
        result.results.reserve(candidate_count);
    } catch (const std::bad_alloc&) {
        throw vulkan_runtime_error("persisted MAPQ host result allocation failed after admission");
    }
    const std::uint64_t actual = saturating_add(
        saturating_mul(static_cast<std::uint64_t>(result.headers.capacity()),
                       sizeof(gpu_chain_candidate_header_wire)),
        saturating_add(saturating_mul(static_cast<std::uint64_t>(result.candidates.capacity()),
                                      sizeof(gpu_chain_candidate_wire)),
                       saturating_mul(static_cast<std::uint64_t>(result.results.capacity()),
                                      sizeof(gpu_mapq_result_wire))));
    if (actual == (std::numeric_limits<std::uint64_t>::max)() || actual > permit->bytes()) {
        throw vulkan_runtime_error("persisted MAPQ host vectors exceeded admitted capacity");
    }
    permit->shrink_to(actual);
    result.capacity_permit = std::move(*permit);
}

/// @brief Hashes physical post-alignment MAPQ evidence without padding or addresses.
[[nodiscard]] std::uint64_t
mapq_result_digest(const std::span<const gpu_chain_candidate_header_wire> headers,
                   const std::span<const gpu_chain_candidate_wire> candidates,
                   const std::span<const gpu_mapq_result_wire> results) noexcept {
    std::uint64_t digest = bioseq::common::k_fnv1a_64_standard_offset_basis;
    const auto mix = [&digest](const auto& value) noexcept {
        digest = bioseq::common::extend_fnv1a_64(
            digest, std::as_bytes(std::span{std::addressof(value), std::size_t{1}}));
    };
    for (const gpu_chain_candidate_header_wire& header : headers) {
        mix(header.candidate_offset);
        mix(header.candidate_count);
        mix(header.retained_count);
        mix(header.primary_count);
        mix(header.alignment_offset);
        mix(header.alignment_count);
        mix(header.repetitive_span);
        mix(header.status);
        mix(header.candidate_capacity);
        mix(header.reserved);
    }
    for (const gpu_chain_candidate_wire& candidate : candidates) {
        mix(candidate.query_start);
        mix(candidate.query_end);
        mix(candidate.target_start);
        mix(candidate.target_end);
        mix(candidate.match_length);
        mix(candidate.block_length);
        mix(candidate.chain_score);
        mix(candidate.original_chain_score);
        mix(candidate.seed_count);
        mix(candidate.parent_index);
        mix(candidate.secondary_chain_score);
        mix(candidate.secondary_count);
        mix(candidate.dp_score);
        mix(candidate.dp_max_score);
        mix(candidate.secondary_dp_max);
        mix(candidate.candidate_index);
        mix(candidate.problem_index);
        mix(candidate.flags);
    }
    for (const gpu_mapq_result_wire& result : results) {
        mix(result.problem_index);
        mix(result.candidate_index);
        mix(result.mapq);
        mix(result.status);
        mix(result.is_primary);
        mix(result.publish);
        mix(result.publication_ordinal);
        mix(result.reserved1);
    }
    return canonicalize_gpu_mapq_result_digest(digest);
}

/// @brief Executes bounded alignment-extension jobs and downloads their result wires.
std::vector<gpu_alignment_extension_result_wire>
runtime::run_alignment_extension_roundtrip(std::span<const std::uint32_t> query_symbols,
                                           std::span<const std::uint32_t> target_symbols,
                                           std::span<const gpu_alignment_extension_job_wire> jobs,
                                           const gpu_alignment_extension_params& params) {
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw vulkan_runtime_error("run_alignment_extension_roundtrip on unavailable device");
    }
    if (jobs.empty()) {
        return {};
    }
    validate_alignment_extension_inputs(query_symbols, target_symbols, jobs, params);
    if (features_.max_workgroup_count_x == 0) {
        throw vulkan_runtime_error("alignment extension device reports zero max workgroup count");
    }

    constexpr std::uint32_t k_alignment_extension_local_size = 1u;
    const compute_pipeline pipe =
        get_compute_pipeline_locked(shader_kind::align_extend_banded_affine,
                                    k_alignment_extension_local_size,
                                    k_gpu_alignment_extension_max_len);

    const std::uint64_t query_bytes = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(query_symbols.size()) * sizeof(std::uint32_t), 4u);
    const std::uint64_t target_bytes = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(target_symbols.size()) * sizeof(std::uint32_t), 4u);
    const std::uint64_t job_bytes = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(jobs.size()) * sizeof(gpu_alignment_extension_job_wire), 4u);
    const std::uint64_t result_bytes =
        static_cast<std::uint64_t>(jobs.size()) * sizeof(gpu_alignment_extension_result_wire);

    buffer query_buf = create_device_buffer(query_bytes, k_compute_buffer_usage);
    buffer target_buf = create_device_buffer(target_bytes, k_compute_buffer_usage);
    buffer jobs_buf = create_device_buffer(job_bytes, k_compute_buffer_usage);
    buffer results_buf = create_device_buffer(result_bytes, k_compute_buffer_usage);
    if (query_buf.mapped == nullptr || target_buf.mapped == nullptr || jobs_buf.mapped == nullptr ||
        results_buf.mapped == nullptr) {
        throw vulkan_runtime_error("run_alignment_extension_roundtrip: device buffer alloc failed");
    }
    if (!query_symbols.empty()) {
        std::memcpy(
            query_buf.mapped, query_symbols.data(), query_symbols.size() * sizeof(std::uint32_t));
    }
    if (!target_symbols.empty()) {
        std::memcpy(target_buf.mapped,
                    target_symbols.data(),
                    target_symbols.size() * sizeof(std::uint32_t));
    }
    std::memcpy(
        jobs_buf.mapped, jobs.data(), jobs.size() * sizeof(gpu_alignment_extension_job_wire));
    std::memset(results_buf.mapped, 0, static_cast<std::size_t>(result_bytes));

    const std::array<VkDescriptorBufferInfo, 4> infos{{
        {query_buf.buffer_handle, 0, query_buf.bytes},
        {target_buf.buffer_handle, 0, target_buf.bytes},
        {jobs_buf.buffer_handle, 0, jobs_buf.bytes},
        {results_buf.buffer_handle, 0, results_buf.bytes},
    }};
    const alignment_extension_push_constants push =
        alignment_extension_push(static_cast<std::uint32_t>(jobs.size()), params);
    const std::uint32_t groups =
        std::min(static_cast<std::uint32_t>(jobs.size()), features_.max_workgroup_count_x);

    reset_dispatch_descriptor_pool_locked();
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(align_ext)");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(align_ext)");
    record_host_to_shader(command_buffer_);
    record_compute_dispatch_locked(command_buffer_, pipe, infos, &push, sizeof(push), groups);
    record_shader_to_host(command_buffer_);
    submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(align_ext)");
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(align_ext_post)");

    std::vector<gpu_alignment_extension_result_wire> results(jobs.size());
    std::memcpy(results.data(), results_buf.mapped, static_cast<std::size_t>(result_bytes));
    for (std::size_t i = 0; i < results.size(); ++i) {
        if (results[i].status != k_gpu_alignment_extension_status_ok) {
            throw vulkan_runtime_error(fmt::format(
                "alignment extension shader rejected job {} with status {}", i, results[i].status));
        }
    }
    alignment_extension_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    return results;
}

/// @brief Uploads, executes, and reads one already-validated CIGAR payload under `mu_`.
alignment_cigar_batch_result runtime::run_alignment_cigar_payload_roundtrip_locked(
    std::span<const std::byte> query_payload,
    std::span<const std::byte> target_payload,
    std::span<const gpu_alignment_cigar_job_wire> jobs,
    const gpu_alignment_cigar_params& params,
    std::uint32_t packed_cigar_capacity,
    std::span<const std::size_t> collector_job_indices,
    bool allow_unsupported_geometry,
    bioseq::parallelism::memory_permit transient_permit,
    runtime_pipeline_use_collector* collector) {
    if ((collector != nullptr && collector_job_indices.size() != jobs.size()) ||
        (collector == nullptr && !collector_job_indices.empty())) {
        throw vulkan_runtime_error("alignment CIGAR collector mapping is invalid");
    }
    constexpr std::uint32_t k_alignment_cigar_local_size = 64u;
    const compute_pipeline pipe =
        get_compute_pipeline_locked(shader_kind::align_extend_simple_cigar,
                                    k_alignment_cigar_local_size,
                                    k_gpu_alignment_extension_max_len);
    const shader_variant_key pipeline_key =
        make_shader_key(pipe.kind, 384u, pipe.local_size, pipe.max_span);
    if (collector != nullptr) {
        collector->prepare(pipeline_key, pipe.metal_archive_receipt);
    }

    const std::uint64_t query_bytes =
        std::max<std::uint64_t>(static_cast<std::uint64_t>(query_payload.size()), 4u);
    const std::uint64_t target_bytes =
        std::max<std::uint64_t>(static_cast<std::uint64_t>(target_payload.size()), 4u);
    const std::uint64_t job_bytes = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(jobs.size()) * sizeof(gpu_alignment_cigar_job_wire), 4u);
    const std::uint64_t result_bytes =
        static_cast<std::uint64_t>(jobs.size()) * sizeof(gpu_alignment_cigar_result_wire);
    const std::uint64_t cigar_bytes =
        static_cast<std::uint64_t>(packed_cigar_capacity) * sizeof(std::uint32_t);
    std::uint64_t traceback_words = 0;
    for (const gpu_alignment_cigar_job_wire& job : jobs) {
        const std::uint64_t end = static_cast<std::uint64_t>(job.traceback_tile_offset) +
                                  static_cast<std::uint64_t>(job.traceback_tile_count);
        traceback_words = std::max(traceback_words, end);
    }
    const std::uint64_t traceback_bytes =
        std::max<std::uint64_t>(traceback_words * sizeof(std::uint32_t), 4u);

    std::uint64_t admitted_allocation_bytes = 0u;
    const std::array logical_buffer_bytes{
        query_bytes, target_bytes, job_bytes, result_bytes, cigar_bytes, traceback_bytes};
    for (const std::uint64_t logical_bytes : logical_buffer_bytes) {
        admitted_allocation_bytes = bioseq::common::saturating_add(
            admitted_allocation_bytes,
            buffer_memory_requirement_locked(logical_bytes, k_compute_buffer_usage));
    }
    if (admitted_allocation_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
        transient_permit.bytes() != admitted_allocation_bytes) {
        throw vulkan_runtime_error("alignment CIGAR transient admission changed before allocation");
    }

    buffer query_buf = create_device_buffer(query_bytes, k_compute_buffer_usage);
    buffer target_buf = create_device_buffer(target_bytes, k_compute_buffer_usage);
    buffer jobs_buf = create_device_buffer(job_bytes, k_compute_buffer_usage);
    buffer results_buf = create_device_buffer(result_bytes, k_compute_buffer_usage);
    buffer cigar_buf = create_device_buffer(cigar_bytes, k_compute_buffer_usage);
    buffer traceback_buf = create_device_buffer(traceback_bytes, k_compute_buffer_usage);
    if (query_buf.mapped == nullptr || target_buf.mapped == nullptr || jobs_buf.mapped == nullptr ||
        results_buf.mapped == nullptr || cigar_buf.mapped == nullptr ||
        traceback_buf.mapped == nullptr) {
        throw vulkan_runtime_error(
            "alignment CIGAR payload roundtrip device buffer allocation failed");
    }
    const std::uint64_t actual_allocation_bytes = bioseq::common::saturating_add(
        query_buf.allocation_bytes,
        bioseq::common::saturating_add(
            target_buf.allocation_bytes,
            bioseq::common::saturating_add(
                jobs_buf.allocation_bytes,
                bioseq::common::saturating_add(
                    results_buf.allocation_bytes,
                    bioseq::common::saturating_add(cigar_buf.allocation_bytes,
                                                   traceback_buf.allocation_bytes)))));
    if (actual_allocation_bytes != admitted_allocation_bytes) {
        throw vulkan_runtime_error("alignment CIGAR allocation exceeded its exact admission");
    }
    if (!query_payload.empty()) {
        std::memcpy(query_buf.mapped, query_payload.data(), query_payload.size());
    }
    if (!target_payload.empty()) {
        std::memcpy(target_buf.mapped, target_payload.data(), target_payload.size());
    }
    std::memcpy(jobs_buf.mapped, jobs.data(), jobs.size() * sizeof(gpu_alignment_cigar_job_wire));
    std::memset(results_buf.mapped, 0, static_cast<std::size_t>(result_bytes));
    std::memset(cigar_buf.mapped, 0, static_cast<std::size_t>(cigar_bytes));

    const std::array<VkDescriptorBufferInfo, 6> infos{{
        {query_buf.buffer_handle, 0, query_buf.bytes},
        {target_buf.buffer_handle, 0, target_buf.bytes},
        {jobs_buf.buffer_handle, 0, jobs_buf.bytes},
        {results_buf.buffer_handle, 0, results_buf.bytes},
        {cigar_buf.buffer_handle, 0, cigar_buf.bytes},
        {traceback_buf.buffer_handle, 0, traceback_buf.bytes},
    }};
    const alignment_cigar_push_constants push =
        alignment_cigar_push(static_cast<std::uint32_t>(jobs.size()), params);
    const auto job_count_u64 = static_cast<std::uint64_t>(jobs.size());
    const std::uint64_t groups_u64 =
        (job_count_u64 + k_alignment_cigar_local_size - 1u) / k_alignment_cigar_local_size;
    const std::uint32_t groups = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(groups_u64, features_.max_workgroup_count_x));

    reset_dispatch_descriptor_pool_locked();
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(align_cigar)");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(align_cigar)");
    if (collector != nullptr) {
        collector->begin(command_buffer_);
    }
    try {
        record_host_to_shader(command_buffer_);
        record_compute_dispatch_locked(command_buffer_, pipe, infos, &push, sizeof(push), groups);
        if (collector != nullptr) {
            for (std::size_t job_index = 0u; job_index < jobs.size(); ++job_index) {
                collector->use(command_buffer_,
                               collector_job_indices[job_index],
                               pipeline_key,
                               pipe.metal_archive_receipt,
                               groups);
            }
        }
        record_shader_to_host(command_buffer_);
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(align_cigar)", collector);
    } catch (...) {
        if (collector != nullptr) {
            collector->burn_before_reset(command_buffer_);
        }
        (void)vkResetCommandBuffer(command_buffer_, 0);
        throw;
    }
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(align_cigar_post)");

    const auto* const device_results =
        static_cast<const gpu_alignment_cigar_result_wire*>(results_buf.mapped);
    std::uint64_t actual_cigar_words = 0u;
    for (std::size_t i = 0u; i < jobs.size(); ++i) {
        const gpu_alignment_cigar_result_wire& device_result = device_results[i];
        if (device_result.status != k_gpu_alignment_extension_status_ok &&
            !(allow_unsupported_geometry &&
              device_result.status == k_gpu_alignment_extension_status_unsupported_geometry)) {
            throw vulkan_runtime_error(
                fmt::format("alignment CIGAR extension shader rejected job {} with status {}",
                            i,
                            device_result.status));
        }
        if (device_result.status == k_gpu_alignment_extension_status_ok) {
            const std::uint64_t source_end =
                static_cast<std::uint64_t>(device_result.cigar_offset) + device_result.cigar_length;
            if (device_result.cigar_length == 0u ||
                device_result.cigar_length > jobs[i].cigar_capacity ||
                source_end > packed_cigar_capacity ||
                device_result.cigar_offset != jobs[i].cigar_offset ||
                device_result.cigar_length >
                    (std::numeric_limits<std::uint64_t>::max)() - actual_cigar_words) {
                throw vulkan_runtime_error(
                    "alignment CIGAR device result cannot be compacted safely");
            }
            actual_cigar_words += device_result.cigar_length;
        } else if (device_result.cigar_length != 0u) {
            throw vulkan_runtime_error(
                "alignment CIGAR unsupported result retained a nonempty CIGAR");
        }
    }
    if (actual_cigar_words > packed_cigar_capacity ||
        actual_cigar_words > (std::numeric_limits<std::size_t>::max)()) {
        throw vulkan_runtime_error("alignment CIGAR compact result size exceeds its ABI capacity");
    }

    alignment_cigar_batch_result out{};
    reserve_alignment_cigar_result_capacity(
        out, jobs.size(), static_cast<std::size_t>(actual_cigar_words));
    out.results.resize(jobs.size());
    out.packed_cigar.resize(static_cast<std::size_t>(actual_cigar_words));
    const auto* const device_cigar = static_cast<const std::uint32_t*>(cigar_buf.mapped);
    std::size_t compact_offset = 0u;
    for (std::size_t i = 0u; i < jobs.size(); ++i) {
        out.results[i] = device_results[i];
        if (out.results[i].status != k_gpu_alignment_extension_status_ok) {
            out.results[i].cigar_offset = 0u;
            continue;
        }
        const std::size_t cigar_length = out.results[i].cigar_length;
        if (compact_offset > (std::numeric_limits<std::uint32_t>::max)() ||
            cigar_length > out.packed_cigar.size() - compact_offset) {
            throw vulkan_runtime_error("alignment CIGAR compact offset exceeds uint32");
        }
        std::copy_n(device_cigar + out.results[i].cigar_offset,
                    cigar_length,
                    out.packed_cigar.data() + compact_offset);
        out.results[i].cigar_offset = static_cast<std::uint32_t>(compact_offset);
        compact_offset += cigar_length;
    }
    if (compact_offset != out.packed_cigar.size()) {
        throw vulkan_runtime_error("alignment CIGAR compact result partition is incomplete");
    }
    out.committed_submission_count = 1u;
    alignment_extension_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    return out;
}

/// @brief Executes one logical CIGAR batch without ever materializing an aggregate trace arena.
alignment_cigar_batch_result runtime::run_alignment_cigar_payload_waved_locked(
    std::span<const std::byte> query_payload,
    std::span<const std::byte> target_payload,
    std::span<const gpu_alignment_cigar_job_wire> jobs,
    const gpu_alignment_cigar_params& params,
    std::uint32_t packed_cigar_capacity,
    const std::uint64_t preferred_traceback_wave_bytes,
    runtime_pipeline_use_collector* collector) {
    if (preferred_traceback_wave_bytes < sizeof(std::uint32_t)) {
        throw vulkan_runtime_error("alignment CIGAR traceback wave limit is invalid");
    }
    const std::uint64_t preferred_traceback_wave_words =
        preferred_traceback_wave_bytes / sizeof(std::uint32_t);

    enum class traceback_mode : std::uint8_t { none, required };
    struct wave_plan {
        std::vector<gpu_alignment_cigar_job_wire> jobs{};
        std::uint32_t cigar_capacity = 0u;
        std::uint64_t traceback_words = 0u;
        bool valid = false;
    };

    const auto required_traceback_words = [&params](
                                              const gpu_alignment_cigar_job_wire& job) noexcept {
        if (job.query_len <= k_gpu_alignment_extension_max_len &&
            job.target_len <= k_gpu_alignment_extension_max_len) {
            return (static_cast<std::uint64_t>(job.query_len) + 1u) *
                   (static_cast<std::uint64_t>(job.target_len) + 1u);
        }
        return gpu_alignment_cigar_gridded_traceback_words(
            job.query_len, job.target_len, params.tile_query, params.tile_target, job.band_width);
    };
    const auto plan_wave =
        [&jobs, &required_traceback_words](const std::span<const std::size_t> original_indices,
                                           const traceback_mode mode) {
            wave_plan plan{};
            plan.jobs.reserve(original_indices.size());
            std::uint64_t cigar_words = 0u;
            std::uint64_t traceback_words = 0u;
            for (const std::size_t original_index : original_indices) {
                if (original_index >= jobs.size()) {
                    return plan;
                }
                gpu_alignment_cigar_job_wire local = jobs[original_index];
                const std::uint64_t trace_count =
                    mode == traceback_mode::none ? 0u : required_traceback_words(local);
                if (trace_count > (std::numeric_limits<std::uint32_t>::max)() ||
                    traceback_words > (std::numeric_limits<std::uint32_t>::max)() - trace_count ||
                    cigar_words > (std::numeric_limits<std::uint32_t>::max)() -
                                      static_cast<std::uint64_t>(local.cigar_capacity)) {
                    return plan;
                }
                local.traceback_tile_offset = static_cast<std::uint32_t>(traceback_words);
                local.traceback_tile_count = static_cast<std::uint32_t>(trace_count);
                local.cigar_offset = static_cast<std::uint32_t>(cigar_words);
                traceback_words += trace_count;
                cigar_words += local.cigar_capacity;
                plan.jobs.push_back(local);
            }
            plan.traceback_words = traceback_words;
            plan.cigar_capacity = static_cast<std::uint32_t>(cigar_words);
            plan.valid = !plan.jobs.empty() && plan.cigar_capacity != 0u;
            return plan;
        };

    const auto reserve_wave = [this, query_payload, target_payload](
                                  const wave_plan& plan,
                                  bioseq::parallelism::memory_admission_failure* failure)
        -> std::optional<bioseq::parallelism::memory_permit> {
        if (!plan.valid ||
            plan.jobs.size() > static_cast<std::uint64_t>(features_.max_workgroup_count_x) * 64u) {
            return std::nullopt;
        }
        const std::array logical_bytes{
            std::max<std::uint64_t>(query_payload.size(), 4u),
            std::max<std::uint64_t>(target_payload.size(), 4u),
            std::max<std::uint64_t>(plan.jobs.size() * sizeof(gpu_alignment_cigar_job_wire), 4u),
            static_cast<std::uint64_t>(plan.jobs.size()) * sizeof(gpu_alignment_cigar_result_wire),
            static_cast<std::uint64_t>(plan.cigar_capacity) * sizeof(std::uint32_t),
            std::max<std::uint64_t>(plan.traceback_words * sizeof(std::uint32_t), 4u),
        };
        std::uint64_t physical_bytes = 0u;
        for (const std::uint64_t bytes : logical_bytes) {
            if (bytes == 0u || bytes > features_.max_storage_buffer_range) {
                return std::nullopt;
            }
            physical_bytes = bioseq::common::saturating_add(
                physical_bytes, buffer_memory_requirement_locked(bytes, k_compute_buffer_usage));
        }
        if (physical_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::nullopt;
        }
        return bioseq::parallelism::memory_admission_controller::instance().try_reserve(
            physical_bytes, failure);
    };

    std::vector<std::size_t> all_indices(jobs.size());
    std::iota(all_indices.begin(), all_indices.end(), std::size_t{0});

    // Preserve the one-dispatch normal path whenever the caller's complete arena is bounded and
    // admitted. A caller that deliberately supplied zero trace capacity retains the existing
    // trace-free behavior and enters the adaptive path, which is still one dispatch when every
    // job completes in the shader's exact/simple front end.
    const bool caller_requested_traceback =
        std::ranges::all_of(jobs, [](const gpu_alignment_cigar_job_wire& job) {
            return job.traceback_tile_count != 0u;
        });
    wave_plan full_plan = plan_wave(all_indices, traceback_mode::required);
    if (caller_requested_traceback && full_plan.valid &&
        full_plan.traceback_words <= preferred_traceback_wave_words) {
        bioseq::parallelism::memory_admission_failure full_failure{};
        auto full_permit = reserve_wave(full_plan, &full_failure);
        if (full_permit) {
            // Job ordinals are collector metadata, not part of the shader input.
            const std::span<const std::size_t> collector_indices =
                collector == nullptr ? std::span<const std::size_t>{} : all_indices;
            return run_alignment_cigar_payload_roundtrip_locked(query_payload,
                                                                target_payload,
                                                                full_plan.jobs,
                                                                params,
                                                                full_plan.cigar_capacity,
                                                                collector_indices,
                                                                false,
                                                                std::move(*full_permit),
                                                                collector);
        }
    }

    alignment_cigar_batch_result final{};
    reserve_alignment_cigar_result_capacity(final, jobs.size(), 0u);
    final.results.resize(jobs.size());
    std::vector<bool> completed(jobs.size(), false);
    std::vector<std::size_t> retry_indices;
    retry_indices.reserve(jobs.size());

    const auto merge_wave = [&jobs, &final, &completed, &retry_indices, packed_cigar_capacity](
                                const alignment_cigar_batch_result& wave_result,
                                const wave_plan& plan,
                                const std::span<const std::size_t> original_indices,
                                const bool collect_unsupported) {
        if (wave_result.results.size() != plan.jobs.size() ||
            wave_result.committed_submission_count != 1u) {
            throw vulkan_runtime_error("alignment CIGAR wave result shape changed");
        }
        std::uint64_t appended_words = 0u;
        for (std::size_t local_index = 0u; local_index < original_indices.size(); ++local_index) {
            const auto& result = wave_result.results[local_index];
            if (collect_unsupported &&
                result.status == k_gpu_alignment_extension_status_unsupported_geometry) {
                continue;
            }
            const std::uint64_t source_end =
                static_cast<std::uint64_t>(result.cigar_offset) + result.cigar_length;
            if (result.status != k_gpu_alignment_extension_status_ok || result.cigar_length == 0u ||
                source_end > wave_result.packed_cigar.size() ||
                result.cigar_length >
                    (std::numeric_limits<std::uint64_t>::max)() - appended_words) {
                throw vulkan_runtime_error("alignment CIGAR wave compact partition is invalid");
            }
            appended_words += result.cigar_length;
        }
        const std::uint64_t desired_words =
            static_cast<std::uint64_t>(final.packed_cigar.size()) + appended_words;
        if (desired_words > packed_cigar_capacity ||
            desired_words > (std::numeric_limits<std::size_t>::max)()) {
            throw vulkan_runtime_error("alignment CIGAR compact merge exceeds global capacity");
        }
        reserve_alignment_cigar_result_capacity(
            final, jobs.size(), static_cast<std::size_t>(desired_words));
        for (std::size_t local_index = 0u; local_index < original_indices.size(); ++local_index) {
            const std::size_t original_index = original_indices[local_index];
            const auto& local_job = plan.jobs[local_index];
            auto result = wave_result.results[local_index];
            if (collect_unsupported &&
                result.status == k_gpu_alignment_extension_status_unsupported_geometry) {
                retry_indices.push_back(original_index);
                continue;
            }
            if (result.status != k_gpu_alignment_extension_status_ok || completed[original_index] ||
                result.cigar_length > local_job.cigar_capacity ||
                static_cast<std::uint64_t>(result.cigar_offset) + result.cigar_length >
                    wave_result.packed_cigar.size()) {
                throw vulkan_runtime_error(fmt::format(
                    "alignment CIGAR wave job {} failed merge validation with status {}",
                    original_index,
                    result.status));
            }
            const std::size_t destination_offset = final.packed_cigar.size();
            if (destination_offset > (std::numeric_limits<std::uint32_t>::max)()) {
                throw vulkan_runtime_error("alignment CIGAR compact destination exceeds uint32");
            }
            final.packed_cigar.insert(final.packed_cigar.end(),
                                      wave_result.packed_cigar.begin() + result.cigar_offset,
                                      wave_result.packed_cigar.begin() + result.cigar_offset +
                                          result.cigar_length);
            result.cigar_offset = static_cast<std::uint32_t>(destination_offset);
            final.results[original_index] = result;
            completed[original_index] = true;
        }
        if (final.committed_submission_count == (std::numeric_limits<std::uint64_t>::max)()) {
            throw vulkan_runtime_error("alignment CIGAR physical submission count overflow");
        }
        ++final.committed_submission_count;
    };

    // A zero-trace GPU pass cheaply completes exact/simple jobs and identifies only the jobs that
    // need gridded traceback. It never reads traceback for a declined job.
    for (std::size_t cursor = 0u; cursor < all_indices.size();) {
        std::size_t wave_size = all_indices.size() - cursor;
        wave_plan plan{};
        std::optional<bioseq::parallelism::memory_permit> permit;
        bioseq::parallelism::memory_admission_failure failure{};
        while (wave_size != 0u) {
            const std::span indices{all_indices.data() + cursor, wave_size};
            plan = plan_wave(indices, traceback_mode::none);
            permit = reserve_wave(plan, &failure);
            if (permit) {
                break;
            }
            wave_size /= 2u;
        }
        if (!permit || wave_size == 0u) {
            throw vulkan_runtime_error(fmt::format(
                "alignment CIGAR zero-trace wave admission denied: reason={} requested={}",
                bioseq::parallelism::memory_admission_failure_reason_name(failure.reason),
                failure.requested_bytes));
        }
        const std::span indices{all_indices.data() + cursor, wave_size};
        const std::span<const std::size_t> collector_indices =
            collector == nullptr ? std::span<const std::size_t>{} : indices;
        const auto wave_result = run_alignment_cigar_payload_roundtrip_locked(query_payload,
                                                                              target_payload,
                                                                              plan.jobs,
                                                                              params,
                                                                              plan.cigar_capacity,
                                                                              collector_indices,
                                                                              true,
                                                                              std::move(*permit),
                                                                              collector);
        merge_wave(wave_result, plan, indices, true);
        cursor += wave_size;
    }

    // Retry only shader-declined jobs. Each wave rebases both 32-bit output arenas to zero and is
    // fully admitted before allocation; a single unfit job is a terminal GPU-only failure.
    for (std::size_t cursor = 0u; cursor < retry_indices.size();) {
        std::size_t wave_size = 0u;
        std::uint64_t preferred_words = 0u;
        while (cursor + wave_size < retry_indices.size()) {
            const std::uint64_t job_words =
                required_traceback_words(jobs[retry_indices[cursor + wave_size]]);
            if (wave_size != 0u &&
                (job_words > preferred_traceback_wave_words -
                                 std::min(preferred_words, preferred_traceback_wave_words))) {
                break;
            }
            preferred_words = bioseq::common::saturating_add(preferred_words, job_words);
            ++wave_size;
            if (preferred_words >= preferred_traceback_wave_words) {
                break;
            }
        }
        wave_size = std::max<std::size_t>(wave_size, 1u);
        wave_plan plan{};
        std::optional<bioseq::parallelism::memory_permit> permit;
        bioseq::parallelism::memory_admission_failure failure{};
        while (wave_size != 0u) {
            const std::span indices{retry_indices.data() + cursor, wave_size};
            plan = plan_wave(indices, traceback_mode::required);
            permit = reserve_wave(plan, &failure);
            if (permit) {
                break;
            }
            wave_size /= 2u;
        }
        if (!permit || wave_size == 0u) {
            throw vulkan_runtime_error(fmt::format(
                "alignment CIGAR traceback wave admission denied: reason={} requested={}",
                bioseq::parallelism::memory_admission_failure_reason_name(failure.reason),
                failure.requested_bytes));
        }
        const std::span indices{retry_indices.data() + cursor, wave_size};
        const std::span<const std::size_t> collector_indices =
            collector == nullptr ? std::span<const std::size_t>{} : indices;
        const auto wave_result = run_alignment_cigar_payload_roundtrip_locked(query_payload,
                                                                              target_payload,
                                                                              plan.jobs,
                                                                              params,
                                                                              plan.cigar_capacity,
                                                                              collector_indices,
                                                                              false,
                                                                              std::move(*permit),
                                                                              collector);
        merge_wave(wave_result, plan, indices, false);
        cursor += wave_size;
    }

    if (std::ranges::any_of(completed, [](const bool value) { return !value; })) {
        throw vulkan_runtime_error("alignment CIGAR waved execution left an incomplete job");
    }
    return final;
}

/// @brief Executes bounded alignment over expanded rank streams for legacy callers.
alignment_cigar_batch_result
runtime::run_alignment_simple_cigar_roundtrip(std::span<const std::uint32_t> query_symbols,
                                              std::span<const std::uint32_t> target_symbols,
                                              std::span<const gpu_alignment_cigar_job_wire> jobs,
                                              const gpu_alignment_cigar_params& params,
                                              std::uint32_t packed_cigar_capacity) {
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw vulkan_runtime_error("run_alignment_simple_cigar_roundtrip on unavailable device");
    }
    if (jobs.empty()) {
        return {};
    }
    validate_alignment_cigar_inputs(
        query_symbols, target_symbols, jobs, params, packed_cigar_capacity);
    if (features_.max_workgroup_count_x == 0u) {
        throw vulkan_runtime_error(
            "alignment CIGAR extension device reports zero max workgroup count");
    }
    return run_alignment_cigar_payload_waved_locked(std::as_bytes(query_symbols),
                                                    std::as_bytes(target_symbols),
                                                    jobs,
                                                    params,
                                                    packed_cigar_capacity,
                                                    std::uint64_t{512} << 20u,
                                                    nullptr);
}

#ifdef BIOSEQ_TEST_INDEXES
/// @brief Exercises the production packed-input wave planner without issuing receipt authority.
alignment_cigar_batch_result runtime::run_alignment_packed_cigar_roundtrip_for_test(
    std::span<const std::byte> query_packed,
    std::span<const std::byte> target_packed,
    std::span<const gpu_alignment_cigar_job_wire> jobs,
    const gpu_alignment_cigar_params& params,
    std::uint32_t packed_cigar_capacity,
    const std::uint64_t preferred_traceback_wave_bytes) {
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw vulkan_runtime_error(
            "run_alignment_packed_cigar_roundtrip_for_test on unavailable device");
    }
    validate_alignment_packed_cigar_inputs(
        query_packed, target_packed, jobs, params, packed_cigar_capacity);
    if (features_.max_workgroup_count_x == 0u) {
        throw vulkan_runtime_error(
            "packed alignment CIGAR test device reports zero max workgroup count");
    }
    return run_alignment_cigar_payload_waved_locked(query_packed,
                                                    target_packed,
                                                    jobs,
                                                    params,
                                                    packed_cigar_capacity,
                                                    preferred_traceback_wave_bytes,
                                                    nullptr);
}
#endif

/// @brief Computes a nonzero identity for one exact packed-window GPU job.
[[nodiscard]] std::uint64_t
packed_alignment_job_identity(const std::span<const std::byte> query_packed,
                              const std::span<const std::byte> target_packed,
                              const gpu_alignment_cigar_job_wire& job,
                              const gpu_alignment_candidate_binding_wire& binding,
                              const gpu_alignment_cigar_params& params) noexcept {
    std::uint64_t identity = bioseq::common::k_fnv1a_64_standard_offset_basis;
    const auto mix = [&identity](const auto& value) noexcept {
        identity = bioseq::common::extend_fnv1a_64(
            identity, std::as_bytes(std::span{std::addressof(value), std::size_t{1}}));
    };
    mix(job.query_len);
    mix(job.target_len);
    mix(job.query_global_start);
    mix(job.target_global_start);
    mix(job.band_width);
    mix(job.flags);
    mix(job.cigar_offset);
    mix(job.cigar_capacity);
    mix(job.traceback_tile_offset);
    mix(job.traceback_tile_count);
    mix(job.query_packed_byte_count);
    mix(job.target_packed_byte_count);
    mix(job.query_leading_symbol_offset);
    mix(job.target_leading_symbol_offset);
    mix(job.query_storage_unit_width);
    mix(job.target_storage_unit_width);
    mix(job.bits_per_symbol);
    mix(job.problem_index);
    mix(binding.problem_index);
    mix(binding.candidate_index);
    mix(params.alphabet_size);
    mix(params.max_band_width);
    mix(params.tile_query);
    mix(params.tile_target);
    mix(params.match_score);
    mix(params.mismatch_penalty);
    mix(params.gap_open_short);
    mix(params.gap_extend_short);
    mix(params.gap_open_long);
    mix(params.gap_extend_long);
    mix(params.z_drop);
    mix(params.end_bonus);
    mix(params.flags);
    mix(params.reserved);
    for (const std::uint32_t word : params.complement_rank_words) {
        mix(word);
    }
    identity = bioseq::common::extend_fnv1a_64(
        identity, query_packed.subspan(job.query_symbol_offset, job.query_packed_byte_count));
    identity = bioseq::common::extend_fnv1a_64(
        identity, target_packed.subspan(job.target_symbol_offset, job.target_packed_byte_count));
    identity &= (std::uint64_t{1} << 63u) - 1u;
    return identity == 0u ? bioseq::common::k_fnv1a_64_alternate_offset_basis : identity;
}

/// @brief Executes native packed-window CIGAR work and attaches its causal token.
alignment_cigar_batch_result runtime::run_persisted_alignment_cigar_roundtrip(
    std::span<const std::byte> query_packed,
    std::span<const std::byte> target_packed,
    std::span<const gpu_alignment_cigar_job_wire> jobs,
    std::span<const gpu_alignment_candidate_binding_wire> candidate_bindings,
    std::span<const gpu_chain_candidate_header_wire> candidate_headers,
    std::span<const gpu_chain_candidate_wire> candidates,
    const gpu_alignment_cigar_params& params,
    std::uint32_t packed_cigar_capacity,
    persisted_grouped_soa_coordinate_operation_receipt& coordinate_receipt) {
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw vulkan_runtime_error("run_persisted_alignment_cigar_roundtrip on unavailable device");
    }
    const bool candidate_mode = !candidate_headers.empty();
    if ((candidate_mode && (jobs.size() != candidate_bindings.size() ||
                            candidate_headers.size() != coordinate_receipt.records_.size())) ||
        (!candidate_mode &&
         (!candidate_bindings.empty() || !candidate_headers.empty() || !candidates.empty()))) {
        throw vulkan_runtime_error("persisted packed alignment candidate binding count changed");
    }
    validate_alignment_packed_cigar_inputs(
        query_packed, target_packed, jobs, params, packed_cigar_capacity);
    if (features_.max_workgroup_count_x == 0u || !coordinate_receipt.is_sealed() ||
        !coordinate_receipt.are_all_jobs_accepted() ||
        coordinate_receipt.has_alignment_cigar_receipt() ||
        coordinate_receipt.alignment_pipeline_token_.audit().kind !=
            runtime_pipeline_operation_kind::invalid) {
        throw vulkan_runtime_error("persisted packed alignment receipt precondition is invalid");
    }

    std::size_t expected_binding_count = 0u;
    std::size_t expected_candidate_offset = 0u;
    for (std::size_t problem_index = 0u; candidate_mode && problem_index < candidate_headers.size();
         ++problem_index) {
        const auto& record = coordinate_receipt.records_[problem_index];
        const gpu_chain_candidate_header_wire& header = candidate_headers[problem_index];
        const bool empty_undispatched =
            are_chain_candidate_headers_equal(header, gpu_chain_candidate_header_wire{}) &&
            record.audit.candidate_digest == 0u && record.audit.result.is_valid == 0u;
        if (record.state !=
                persisted_grouped_soa_coordinate_operation_receipt::record_state::accepted ||
            !record.audit.configuration.mapq.is_valid() ||
            !are_chain_candidate_headers_equal(record.audit.candidate_header, header) ||
            header.status != k_gpu_mapq_status_ok || header.reserved != 0u ||
            (!empty_undispatched && header.candidate_offset != expected_candidate_offset) ||
            header.candidate_offset > candidates.size() ||
            header.candidate_count > candidates.size() - header.candidate_offset ||
            header.retained_count > header.candidate_count ||
            header.candidate_count >
                (std::numeric_limits<std::size_t>::max)() - expected_candidate_offset ||
            expected_binding_count >
                (std::numeric_limits<std::size_t>::max)() - header.retained_count) {
            throw vulkan_runtime_error("persisted packed alignment candidate receipt is invalid");
        }
        const std::span<const gpu_chain_candidate_wire> problem_candidates =
            header.candidate_count == 0u
                ? std::span<const gpu_chain_candidate_wire>{}
                : candidates.subspan(header.candidate_offset, header.candidate_count);
        if (!empty_undispatched &&
            chain_candidate_digest(header, problem_candidates) != record.audit.candidate_digest) {
            throw vulkan_runtime_error("persisted packed alignment candidate digest changed");
        }
        expected_candidate_offset += header.candidate_count;
        expected_binding_count += header.retained_count;
    }
    if (candidate_mode &&
        (expected_binding_count != jobs.size() || expected_candidate_offset != candidates.size())) {
        throw vulkan_runtime_error("persisted packed alignment retained-candidate count changed");
    }
    if (!candidate_mode) {
        expected_binding_count =
            std::ranges::count_if(coordinate_receipt.records_, [](const auto& record) {
                return record.state == persisted_grouped_soa_coordinate_operation_receipt::
                                           record_state::accepted &&
                       record.audit.result.is_valid == 1u;
            });
        if (coordinate_receipt.requires_mapq_receipt() || expected_binding_count != jobs.size()) {
            throw vulkan_runtime_error("persisted packed alignment legacy receipt shape changed");
        }
    }

    auto collector = runtime_pipeline_use_collector::create(
        runtime_pipeline_operation_kind::persisted_alignment_cigar,
        jobs.size(),
        coordinate_receipt.operation_identity_);
    for (std::size_t job_index = 0u; job_index < jobs.size(); ++job_index) {
        const gpu_alignment_cigar_job_wire& job = jobs[job_index];
        const gpu_alignment_candidate_binding_wire binding =
            candidate_mode ? candidate_bindings[job_index]
                           : gpu_alignment_candidate_binding_wire{
                                 .problem_index = job.problem_index, .candidate_index = 0u};
        if (job.problem_index >= coordinate_receipt.records_.size() ||
            binding.problem_index != job.problem_index) {
            throw vulkan_runtime_error("persisted packed alignment problem index is out of range");
        }
        const auto& record = coordinate_receipt.records_[job.problem_index];
        if (candidate_mode &&
            binding.candidate_index >= candidate_headers[job.problem_index].candidate_count) {
            throw vulkan_runtime_error(
                "persisted packed alignment candidate index is out of range");
        }
        const bool reverse_target =
            (job.flags & k_gpu_alignment_cigar_job_flag_reverse_target) != 0u;
        const std::uint64_t query_end =
            static_cast<std::uint64_t>(job.query_global_start) + job.query_len;
        const std::uint64_t target_end =
            static_cast<std::uint64_t>(job.target_global_start) + job.target_len;
        bool input_matches =
            record.state ==
            persisted_grouped_soa_coordinate_operation_receipt::record_state::accepted;
        if (candidate_mode) {
            const gpu_chain_candidate_header_wire& header = candidate_headers[job.problem_index];
            const gpu_chain_candidate_wire& candidate =
                candidates[header.candidate_offset + binding.candidate_index];
            input_matches =
                input_matches &&
                (candidate.flags &
                 (k_gpu_chain_candidate_flag_valid | k_gpu_chain_candidate_flag_retained)) ==
                    (k_gpu_chain_candidate_flag_valid | k_gpu_chain_candidate_flag_retained) &&
                candidate.problem_index == binding.problem_index &&
                candidate.candidate_index == binding.candidate_index &&
                candidate.query_start == job.query_global_start &&
                candidate.query_end == query_end &&
                candidate.target_start == job.target_global_start &&
                candidate.target_end == target_end &&
                ((candidate.flags & k_gpu_chain_candidate_flag_reverse) != 0u) == reverse_target;
        } else {
            const gpu_coordinate_result_wire& coordinate = record.audit.result;
            input_matches = input_matches && coordinate.is_valid == 1u &&
                            coordinate.query_start == job.query_global_start &&
                            coordinate.query_end == query_end &&
                            coordinate.target_start == job.target_global_start &&
                            coordinate.target_end == target_end &&
                            (coordinate.is_reverse != 0u) == reverse_target;
        }
        const gpu_alignment_candidate_binding_wire prior_binding =
            job_index == 0u
                ? gpu_alignment_candidate_binding_wire{}
                : (candidate_mode ? candidate_bindings[job_index - 1u]
                                  : gpu_alignment_candidate_binding_wire{
                                        .problem_index = jobs[job_index - 1u].problem_index,
                                        .candidate_index = 0u});
        if (!input_matches ||
            (job_index != 0u && (prior_binding.problem_index > binding.problem_index ||
                                 (prior_binding.problem_index == binding.problem_index &&
                                  prior_binding.candidate_index >= binding.candidate_index)))) {
            throw vulkan_runtime_error(
                "persisted packed alignment job does not match its coordinate receipt");
        }
        const std::uint64_t input_identity =
            packed_alignment_job_identity(query_packed, target_packed, job, binding, params);
        collector->set_shape_input(job_index, job.query_len, job.target_len, input_identity);
        collector->set_shape_outcome(job_index,
                                     static_cast<std::uint64_t>(binding.problem_index) + 1u,
                                     static_cast<std::uint64_t>(binding.candidate_index) + 1u);
    }

    alignment_cigar_batch_result out =
        run_alignment_cigar_payload_waved_locked(query_packed,
                                                 target_packed,
                                                 jobs,
                                                 params,
                                                 packed_cigar_capacity,
                                                 std::uint64_t{512} << 20u,
                                                 collector.get());
    runtime_pipeline_operation_token token = collector->seal();
    if (!token.is_valid_for_persisted_alignment_cigar() ||
        token.audit().operation_identity != coordinate_receipt.operation_identity_ ||
        token.audit().jobs.size() != jobs.size() ||
        out.committed_submission_count != token.audit().committed_submissions) {
        throw vulkan_runtime_error("persisted packed alignment causal receipt is invalid");
    }
    for (std::size_t job_index = 0u; job_index < jobs.size(); ++job_index) {
        const gpu_alignment_cigar_job_wire& job = jobs[job_index];
        const gpu_alignment_candidate_binding_wire binding =
            candidate_mode ? candidate_bindings[job_index]
                           : gpu_alignment_candidate_binding_wire{
                                 .problem_index = job.problem_index, .candidate_index = 0u};
        const runtime_pipeline_job_use_audit& audit = token.audit().jobs[job_index];
        if (audit.minimizer_count != job.query_len || audit.target_len != job.target_len ||
            audit.target_position_offset !=
                packed_alignment_job_identity(query_packed, target_packed, job, binding, params) ||
            audit.anchor_count != static_cast<std::uint64_t>(binding.problem_index) + 1u ||
            audit.range_count != static_cast<std::uint64_t>(binding.candidate_index) + 1u) {
            throw vulkan_runtime_error("persisted packed alignment input receipt changed");
        }
    }
    coordinate_receipt.alignment_pipeline_token_ = std::move(token);
    return out;
}

/// @brief Executes physical post-alignment classification/MAPQ and attaches its causal token.
gpu_mapq_batch_result runtime::run_persisted_mapq_roundtrip(
    const std::span<const gpu_chain_candidate_header_wire> headers,
    const std::span<const gpu_chain_candidate_wire> candidates,
    const std::span<const gpu_alignment_candidate_binding_wire> candidate_bindings,
    const std::span<const gpu_alignment_cigar_result_wire> alignment_results,
    const gpu_persisted_mapq_config& config,
    const std::int32_t min_chain_score,
    persisted_grouped_soa_coordinate_operation_receipt& coordinate_receipt) {
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw vulkan_runtime_error("run_persisted_mapq_roundtrip on unavailable device");
    }
    if (headers.empty() || !config.is_valid() || min_chain_score <= 0 ||
        headers.size() > (std::numeric_limits<std::uint32_t>::max)() ||
        headers.size() > features_.max_workgroup_count_x ||
        candidates.size() > (std::numeric_limits<std::uint32_t>::max)() ||
        candidate_bindings.size() != alignment_results.size() ||
        candidate_bindings.size() > (std::numeric_limits<std::uint32_t>::max)() ||
        !coordinate_receipt.is_sealed() || !coordinate_receipt.are_all_jobs_accepted() ||
        coordinate_receipt.records_.size() != headers.size() ||
        coordinate_receipt.mapq_pipeline_token_.audit().kind !=
            runtime_pipeline_operation_kind::invalid ||
        coordinate_receipt.mapq_result_count_ != 0u ||
        coordinate_receipt.mapq_published_count_ != 0u ||
        coordinate_receipt.mapq_result_digest_ != 0u) {
        throw vulkan_runtime_error("persisted MAPQ receipt precondition is invalid");
    }

    std::size_t candidate_cursor = 0u;
    std::size_t alignment_cursor = 0u;
    for (std::size_t problem_index = 0u; problem_index < headers.size(); ++problem_index) {
        const auto& record = coordinate_receipt.records_[problem_index];
        const gpu_chain_candidate_header_wire& header = headers[problem_index];
        const bool empty_undispatched =
            are_chain_candidate_headers_equal(header, gpu_chain_candidate_header_wire{}) &&
            record.audit.candidate_digest == 0u && record.audit.result.is_valid == 0u;
        if (record.state !=
                persisted_grouped_soa_coordinate_operation_receipt::record_state::accepted ||
            record.audit.configuration.mapq != config ||
            record.audit.configuration.min_chain_score != min_chain_score ||
            !are_chain_candidate_headers_equal(record.audit.candidate_header, header) ||
            (!empty_undispatched && header.candidate_offset != candidate_cursor) ||
            header.status != k_gpu_mapq_status_ok || header.reserved != 0u ||
            (!empty_undispatched &&
             header.candidate_capacity != k_gpu_persisted_mapq_candidate_capacity) ||
            header.candidate_count > config.candidate_capacity ||
            header.retained_count > header.candidate_count ||
            header.primary_count > header.retained_count || header.alignment_offset != 0u ||
            header.alignment_count != 0u ||
            header.candidate_count > candidates.size() - candidate_cursor) {
            throw vulkan_runtime_error("persisted MAPQ candidate header receipt changed");
        }
        const std::span<const gpu_chain_candidate_wire> problem_candidates =
            candidates.subspan(candidate_cursor, header.candidate_count);
        if (!empty_undispatched &&
            chain_candidate_digest(header, problem_candidates) != record.audit.candidate_digest) {
            throw vulkan_runtime_error("persisted MAPQ candidate digest changed");
        }
        std::uint32_t retained_count = 0u;
        for (std::uint32_t candidate_index = 0u; candidate_index < header.candidate_count;
             ++candidate_index) {
            const gpu_chain_candidate_wire& candidate = problem_candidates[candidate_index];
            if (!is_chain_candidate_wire_well_formed(candidate,
                                                     static_cast<std::uint32_t>(problem_index),
                                                     candidate_index,
                                                     record.audit.query_record.query_len,
                                                     record.audit.target_len)) {
                throw vulkan_runtime_error("persisted MAPQ candidate wire is malformed");
            }
            const bool retained = (candidate.flags & k_gpu_chain_candidate_flag_retained) != 0u;
            if (!retained) {
                continue;
            }
            if (alignment_cursor >= candidate_bindings.size()) {
                throw vulkan_runtime_error("persisted MAPQ alignment bindings are incomplete");
            }
            const gpu_alignment_candidate_binding_wire& binding =
                candidate_bindings[alignment_cursor];
            const gpu_alignment_cigar_result_wire& aligned = alignment_results[alignment_cursor];
            if (binding.problem_index != problem_index ||
                binding.candidate_index != candidate_index ||
                aligned.status != k_gpu_alignment_extension_status_ok || aligned.reserved != 0u ||
                aligned.cigar_length == 0u || aligned.block_length == 0u ||
                aligned.num_matches > aligned.block_length || aligned.max_score <= 0 ||
                aligned.query_start != candidate.query_start ||
                aligned.query_end != candidate.query_end ||
                aligned.target_start != candidate.target_start ||
                aligned.target_end != candidate.target_end) {
                throw vulkan_runtime_error(
                    "persisted MAPQ alignment result does not match its candidate");
            }
            ++retained_count;
            ++alignment_cursor;
        }
        if (retained_count != header.retained_count) {
            throw vulkan_runtime_error("persisted MAPQ retained-candidate count changed");
        }
        candidate_cursor += header.candidate_count;
    }
    if (candidate_cursor != candidates.size() || alignment_cursor != candidate_bindings.size() ||
        ((alignment_cursor == 0u) != (coordinate_receipt.alignment_pipeline_token_.audit().kind ==
                                      runtime_pipeline_operation_kind::invalid)) ||
        (alignment_cursor != 0u && !coordinate_receipt.has_alignment_cigar_receipt())) {
        throw vulkan_runtime_error("persisted MAPQ candidate/alignment partition is incomplete");
    }

    gpu_mapq_batch_result out{};
    reserve_mapq_result_capacity(out, headers.size(), candidates.size());
    out.headers.resize(headers.size());
    out.candidates.resize(candidates.size());
    out.results.resize(candidates.size());
    std::copy(headers.begin(), headers.end(), out.headers.begin());
    std::copy(candidates.begin(), candidates.end(), out.candidates.begin());
    alignment_cursor = 0u;
    std::size_t normalized_candidate_cursor = 0u;
    for (gpu_chain_candidate_header_wire& header : out.headers) {
        if (alignment_cursor > (std::numeric_limits<std::uint32_t>::max)() ||
            normalized_candidate_cursor > (std::numeric_limits<std::uint32_t>::max)()) {
            throw vulkan_runtime_error("persisted MAPQ header offset exceeds ABI");
        }
        header.candidate_offset = static_cast<std::uint32_t>(normalized_candidate_cursor);
        header.alignment_offset = static_cast<std::uint32_t>(alignment_cursor);
        header.alignment_count = header.retained_count;
        header.candidate_capacity = config.candidate_capacity;
        normalized_candidate_cursor += header.candidate_count;
        alignment_cursor += header.retained_count;
    }
    if (normalized_candidate_cursor != candidates.size() ||
        alignment_cursor != candidate_bindings.size()) {
        throw vulkan_runtime_error("persisted MAPQ alignment header partition changed");
    }

    constexpr std::uint32_t k_mapq_local_size = 1u;
    const compute_pipeline pipe =
        get_compute_pipeline_locked(shader_kind::mapping_quality_reduce,
                                    k_mapq_local_size,
                                    k_gpu_persisted_mapq_candidate_capacity);
    const shader_variant_key pipeline_key =
        make_shader_key(pipe.kind, 384u, pipe.local_size, pipe.max_span);
    auto collector =
        runtime_pipeline_use_collector::create(runtime_pipeline_operation_kind::persisted_mapq,
                                               1u,
                                               coordinate_receipt.operation_identity_);
    collector->prepare(pipeline_key, pipe.metal_archive_receipt);
    collector->set_shape_input(0u,
                               static_cast<std::uint64_t>(headers.size()),
                               static_cast<std::uint64_t>(candidates.size()),
                               static_cast<std::uint64_t>(candidate_bindings.size()));

    const std::uint64_t header_bytes = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(out.headers.size()) * sizeof(gpu_chain_candidate_header_wire),
        4u);
    const std::uint64_t candidate_bytes = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(out.candidates.size()) * sizeof(gpu_chain_candidate_wire), 4u);
    const std::uint64_t binding_bytes =
        std::max<std::uint64_t>(static_cast<std::uint64_t>(candidate_bindings.size()) *
                                    sizeof(gpu_alignment_candidate_binding_wire),
                                4u);
    const std::uint64_t alignment_bytes =
        std::max<std::uint64_t>(static_cast<std::uint64_t>(alignment_results.size()) *
                                    sizeof(gpu_alignment_cigar_result_wire),
                                4u);
    const std::uint64_t result_bytes = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(out.results.size()) * sizeof(gpu_mapq_result_wire), 4u);
    const std::array logical_bytes{
        header_bytes, candidate_bytes, binding_bytes, alignment_bytes, result_bytes};
    std::uint64_t admitted_device_bytes = 0u;
    for (const std::uint64_t bytes : logical_bytes) {
        if (bytes > features_.max_storage_buffer_range) {
            throw vulkan_runtime_error("persisted MAPQ storage-buffer range exceeds device limit");
        }
        admitted_device_bytes = bioseq::common::saturating_add(
            admitted_device_bytes, buffer_memory_requirement_locked(bytes, k_compute_buffer_usage));
    }
    if (admitted_device_bytes == 0u ||
        admitted_device_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
        throw vulkan_runtime_error("persisted MAPQ device admission overflow");
    }
    bioseq::parallelism::memory_admission_failure device_failure{};
    auto device_permit = bioseq::parallelism::memory_admission_controller::instance().try_reserve(
        admitted_device_bytes, &device_failure);
    if (!device_permit) {
        throw vulkan_runtime_error(fmt::format(
            "persisted MAPQ device admission denied: reason={} requested={}",
            bioseq::parallelism::memory_admission_failure_reason_name(device_failure.reason),
            device_failure.requested_bytes));
    }

    buffer headers_buf = create_device_buffer(header_bytes, k_compute_buffer_usage);
    buffer candidates_buf = create_device_buffer(candidate_bytes, k_compute_buffer_usage);
    buffer bindings_buf = create_device_buffer(binding_bytes, k_compute_buffer_usage);
    buffer alignment_buf = create_device_buffer(alignment_bytes, k_compute_buffer_usage);
    buffer results_buf = create_device_buffer(result_bytes, k_compute_buffer_usage);
    if (headers_buf.mapped == nullptr || candidates_buf.mapped == nullptr ||
        bindings_buf.mapped == nullptr || alignment_buf.mapped == nullptr ||
        results_buf.mapped == nullptr) {
        throw vulkan_runtime_error("persisted MAPQ device buffer allocation failed");
    }
    std::uint64_t actual_device_bytes = 0u;
    for (const std::uint64_t bytes : std::array{headers_buf.allocation_bytes,
                                                candidates_buf.allocation_bytes,
                                                bindings_buf.allocation_bytes,
                                                alignment_buf.allocation_bytes,
                                                results_buf.allocation_bytes}) {
        actual_device_bytes = bioseq::common::saturating_add(actual_device_bytes, bytes);
    }
    if (actual_device_bytes != admitted_device_bytes) {
        throw vulkan_runtime_error("persisted MAPQ allocation changed after exact admission");
    }
    std::memcpy(
        headers_buf.mapped, out.headers.data(), out.headers.size() * sizeof(out.headers[0]));
    if (!out.candidates.empty()) {
        std::memcpy(candidates_buf.mapped,
                    out.candidates.data(),
                    out.candidates.size() * sizeof(out.candidates[0]));
    }
    if (!candidate_bindings.empty()) {
        std::memcpy(bindings_buf.mapped,
                    candidate_bindings.data(),
                    candidate_bindings.size() * sizeof(candidate_bindings[0]));
        std::memcpy(alignment_buf.mapped,
                    alignment_results.data(),
                    alignment_results.size() * sizeof(alignment_results[0]));
    }
    std::memset(results_buf.mapped, 0, static_cast<std::size_t>(result_bytes));

    const std::array<VkDescriptorBufferInfo, 5> infos{{
        {headers_buf.buffer_handle, 0u, headers_buf.bytes},
        {candidates_buf.buffer_handle, 0u, candidates_buf.bytes},
        {bindings_buf.buffer_handle, 0u, bindings_buf.bytes},
        {alignment_buf.buffer_handle, 0u, alignment_buf.bytes},
        {results_buf.buffer_handle, 0u, results_buf.bytes},
    }};
    const gpu_mapq_reduce_push push{
        .problem_count = static_cast<std::uint32_t>(headers.size()),
        .candidate_count = static_cast<std::uint32_t>(candidates.size()),
        .alignment_count = static_cast<std::uint32_t>(candidate_bindings.size()),
        .min_chain_score = min_chain_score,
        .match_score = config.match_score,
        .mask_level_bits = config.mask_level_bits,
        .mask_len = config.mask_len,
        .sub_diff = config.sub_diff,
        .pri_ratio_bits = config.pri_ratio_bits,
        .min_diff = config.min_diff,
        .best_n = config.best_n,
        .candidate_capacity = config.candidate_capacity,
        .is_short_read = config.is_short_read,
        .is_splice = config.is_splice,
        .reserved = config.reserved,
    };
    const std::uint32_t groups = static_cast<std::uint32_t>(headers.size());

    reset_dispatch_descriptor_pool_locked();
    check_vk(vkResetCommandBuffer(command_buffer_, 0u), "reset(persisted_mapq)");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(persisted_mapq)");
    collector->begin(command_buffer_);
    try {
        record_host_to_shader(command_buffer_);
        record_compute_dispatch_locked(command_buffer_, pipe, infos, &push, sizeof(push), groups);
        collector->use(command_buffer_, 0u, pipeline_key, pipe.metal_archive_receipt, groups);
        record_shader_to_host(command_buffer_);
        submit_and_wait(
            device_, queue_, command_buffer_, fence_, "submit(persisted_mapq)", collector.get());
    } catch (...) {
        collector->burn_before_reset(command_buffer_);
        (void)vkResetCommandBuffer(command_buffer_, 0u);
        throw;
    }
    check_vk(vkResetCommandBuffer(command_buffer_, 0u), "reset(persisted_mapq_post)");
    std::memcpy(
        out.headers.data(), headers_buf.mapped, out.headers.size() * sizeof(out.headers[0]));
    if (!out.candidates.empty()) {
        std::memcpy(out.candidates.data(),
                    candidates_buf.mapped,
                    out.candidates.size() * sizeof(out.candidates[0]));
        std::memcpy(
            out.results.data(), results_buf.mapped, out.results.size() * sizeof(out.results[0]));
    }

    candidate_cursor = 0u;
    alignment_cursor = 0u;
    std::uint64_t published_count = 0u;
    for (std::size_t problem_index = 0u; problem_index < out.headers.size(); ++problem_index) {
        const gpu_chain_candidate_header_wire& before = headers[problem_index];
        const gpu_chain_candidate_header_wire& after = out.headers[problem_index];
        if (after.candidate_offset != candidate_cursor ||
            after.candidate_count != before.candidate_count ||
            after.alignment_offset != alignment_cursor ||
            after.alignment_count != before.retained_count ||
            after.repetitive_span != before.repetitive_span ||
            after.status != k_gpu_mapq_status_ok ||
            after.candidate_capacity != config.candidate_capacity || after.reserved != 0u ||
            after.retained_count > after.candidate_count ||
            after.primary_count > after.retained_count) {
            throw vulkan_runtime_error("persisted MAPQ result header is inconsistent");
        }
        std::uint32_t retained_count = 0u;
        std::uint32_t primary_count = 0u;
        std::array<bool, k_gpu_persisted_mapq_candidate_capacity> publication_seen{};
        std::array<std::int32_t, k_gpu_persisted_mapq_candidate_capacity> publication_scores{};
        std::array<std::uint32_t, k_gpu_persisted_mapq_candidate_capacity>
            publication_candidate_indices{};
        for (std::uint32_t candidate_index = 0u; candidate_index < after.candidate_count;
             ++candidate_index) {
            const std::size_t global_index = candidate_cursor + candidate_index;
            const gpu_chain_candidate_wire& input = candidates[global_index];
            const gpu_chain_candidate_wire& candidate = out.candidates[global_index];
            const gpu_mapq_result_wire& result = out.results[global_index];
            const bool was_retained = (input.flags & k_gpu_chain_candidate_flag_retained) != 0u;
            const bool aligned = (candidate.flags & k_gpu_chain_candidate_flag_aligned) != 0u;
            const bool retained = (candidate.flags & k_gpu_chain_candidate_flag_retained) != 0u;
            const bool primary = (candidate.flags & k_gpu_chain_candidate_flag_primary) != 0u;
            if (candidate.query_start != input.query_start ||
                candidate.query_end != input.query_end ||
                candidate.target_start != input.target_start ||
                candidate.target_end != input.target_end ||
                candidate.chain_score != input.chain_score ||
                candidate.original_chain_score != input.original_chain_score ||
                candidate.seed_count != input.seed_count ||
                candidate.problem_index != problem_index ||
                candidate.candidate_index != candidate_index ||
                (candidate.flags & k_gpu_chain_candidate_flag_valid) == 0u ||
                (candidate.flags & ~k_gpu_chain_candidate_flags) != 0u || aligned != was_retained ||
                (retained && !aligned) || (primary && !retained) || candidate.parent_index < 0 ||
                static_cast<std::uint32_t>(candidate.parent_index) >= after.candidate_count ||
                candidate.secondary_chain_score < input.secondary_chain_score ||
                candidate.secondary_count < 0 ||
                candidate.secondary_count >
                    static_cast<std::int32_t>((config.candidate_capacity - 1u) * 2u) ||
                candidate.secondary_dp_max < 0 ||
                (primary && candidate.secondary_dp_max > candidate.dp_max_score) ||
                (!primary && candidate.secondary_dp_max != 0) ||
                result.problem_index != problem_index ||
                result.candidate_index != candidate_index ||
                result.status != k_gpu_mapq_status_ok || result.reserved1 != 0u ||
                result.publish > 1u || result.is_primary > 1u ||
                (result.publish != 0u) != retained ||
                (result.is_primary != 0u) != (retained && primary) || result.mapq < 0 ||
                result.mapq > 60 || (!primary && result.mapq != 0) ||
                (retained ? result.publication_ordinal >= after.retained_count
                          : result.publication_ordinal != k_gpu_mapq_unpublished_ordinal)) {
                throw vulkan_runtime_error("persisted MAPQ candidate result is inconsistent");
            }
            if (aligned) {
                const gpu_chain_candidate_wire& parent =
                    out.candidates[candidate_cursor +
                                   static_cast<std::uint32_t>(candidate.parent_index)];
                const bool parent_is_primary =
                    (parent.flags &
                     (k_gpu_chain_candidate_flag_aligned | k_gpu_chain_candidate_flag_retained |
                      k_gpu_chain_candidate_flag_primary)) ==
                    (k_gpu_chain_candidate_flag_aligned | k_gpu_chain_candidate_flag_retained |
                     k_gpu_chain_candidate_flag_primary);
                if ((primary &&
                     candidate.parent_index != static_cast<std::int32_t>(candidate_index)) ||
                    (!primary &&
                     (!parent_is_primary ||
                      parent.parent_index != static_cast<std::int32_t>(candidate.parent_index)))) {
                    throw vulkan_runtime_error(
                        "persisted MAPQ parent relationship is inconsistent");
                }
            }
            if (was_retained) {
                const gpu_alignment_cigar_result_wire& aligned_result =
                    alignment_results[alignment_cursor++];
                if (candidate.dp_score != aligned_result.score ||
                    candidate.dp_max_score != aligned_result.max_score ||
                    candidate.match_length != aligned_result.num_matches ||
                    candidate.block_length != aligned_result.block_length) {
                    throw vulkan_runtime_error(
                        "persisted MAPQ candidate lost its physical alignment evidence");
                }
            } else if (candidate.dp_score != 0 || candidate.dp_max_score != 0 ||
                       candidate.secondary_dp_max != 0 ||
                       candidate.match_length != input.match_length ||
                       candidate.block_length != input.block_length) {
                throw vulkan_runtime_error(
                    "persisted MAPQ unaligned candidate acquired alignment evidence");
            }
            if (retained) {
                if (publication_seen[result.publication_ordinal]) {
                    throw vulkan_runtime_error("persisted MAPQ publication ordinal is duplicated");
                }
                publication_seen[result.publication_ordinal] = true;
                publication_scores[result.publication_ordinal] = candidate.dp_max_score;
                publication_candidate_indices[result.publication_ordinal] = candidate_index;
                ++retained_count;
                ++published_count;
            }
            if (primary) {
                ++primary_count;
            }
        }
        if (retained_count != after.retained_count || primary_count != after.primary_count) {
            throw vulkan_runtime_error("persisted MAPQ candidate classification count changed");
        }
        for (std::uint32_t publication_ordinal = 0u; publication_ordinal < after.retained_count;
             ++publication_ordinal) {
            if (!publication_seen[publication_ordinal] ||
                (publication_ordinal != 0u &&
                 (publication_scores[publication_ordinal - 1u] <
                      publication_scores[publication_ordinal] ||
                  (publication_scores[publication_ordinal - 1u] ==
                       publication_scores[publication_ordinal] &&
                   publication_candidate_indices[publication_ordinal - 1u] >
                       publication_candidate_indices[publication_ordinal])))) {
                throw vulkan_runtime_error(
                    "persisted MAPQ publication order is not the authenticated DP ranking");
            }
        }
        candidate_cursor += after.candidate_count;
    }
    if (candidate_cursor != out.candidates.size() || alignment_cursor != alignment_results.size()) {
        throw vulkan_runtime_error("persisted MAPQ output partition is incomplete");
    }
    const std::uint64_t result_digest =
        mapq_result_digest(out.headers, out.candidates, out.results);
    collector->set_shape_outcome(0u, published_count, result_digest);
    runtime_pipeline_operation_token token = collector->seal();
    if (!token.is_valid_for_persisted_mapq() ||
        token.audit().operation_identity != coordinate_receipt.operation_identity_ ||
        token.audit().committed_submissions != 1u) {
        throw vulkan_runtime_error("persisted MAPQ causal receipt is invalid");
    }
    out.committed_submission_count = token.audit().committed_submissions;
    coordinate_receipt.mapq_result_count_ = static_cast<std::uint64_t>(out.results.size());
    coordinate_receipt.mapq_published_count_ = published_count;
    coordinate_receipt.mapq_result_digest_ = result_digest;
    coordinate_receipt.mapq_pipeline_token_ = std::move(token);
    if (!coordinate_receipt.has_mapq_receipt()) {
        throw vulkan_runtime_error("persisted MAPQ receipt did not seal");
    }
    return out;
}

} // namespace bioseq::gpu::vulkan
