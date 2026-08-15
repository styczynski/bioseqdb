/**
 * @file alignment.cppm
 * @brief Executes Vulkan alignment extension and bounded CIGAR reconstruction.
 */
module;

#include <cstdint>
#include <libassert/assert.hpp>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

module bioseq.gpu.vulkan.runtime:alignment;

import :frontend_types;
import :support;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.parallelism.memory_admission;
import bioseq.instrumentation;
import std;

namespace bioseq::gpu::vulkan {

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

/// @brief Executes bounded alignment and reconstructs packed CIGAR output on the GPU.
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
    if (features_.max_workgroup_count_x == 0) {
        throw vulkan_runtime_error(
            "alignment CIGAR extension device reports zero max workgroup count");
    }

    constexpr std::uint32_t k_alignment_cigar_local_size = 64u;
    const compute_pipeline pipe =
        get_compute_pipeline_locked(shader_kind::align_extend_simple_cigar,
                                    k_alignment_cigar_local_size,
                                    k_gpu_alignment_extension_max_len);

    const std::uint64_t query_bytes = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(query_symbols.size()) * sizeof(std::uint32_t), 4u);
    const std::uint64_t target_bytes = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(target_symbols.size()) * sizeof(std::uint32_t), 4u);
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
            "run_alignment_simple_cigar_roundtrip: device buffer alloc failed");
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
    record_host_to_shader(command_buffer_);
    record_compute_dispatch_locked(command_buffer_, pipe, infos, &push, sizeof(push), groups);
    record_shader_to_host(command_buffer_);
    submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(align_cigar)");
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(align_cigar_post)");

    alignment_cigar_batch_result out{};
    out.results.resize(jobs.size());
    out.packed_cigar.resize(packed_cigar_capacity);
    std::memcpy(out.results.data(), results_buf.mapped, static_cast<std::size_t>(result_bytes));
    std::memcpy(out.packed_cigar.data(), cigar_buf.mapped, static_cast<std::size_t>(cigar_bytes));
    for (std::size_t i = 0; i < out.results.size(); ++i) {
        if (out.results[i].status != k_gpu_alignment_extension_status_ok) {
            throw vulkan_runtime_error(
                fmt::format("alignment CIGAR extension shader rejected job {} with status {}",
                            i,
                            out.results[i].status));
        }
    }
    alignment_extension_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    return out;
}

} // namespace bioseq::gpu::vulkan
