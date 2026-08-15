/**
 * @file prefilter.cppm
 * @brief Executes SneakySnake and GateKeeper rank-prefilter replay routes.
 */
module;

#include <cstdint>
#include <libassert/assert.hpp>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

module bioseq.gpu.vulkan.runtime:prefilter;

import :frontend_types;
import :support;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.parallelism.memory_admission;
import bioseq.instrumentation;
import std;

namespace bioseq::gpu::vulkan {

std::vector<bioseq::prefilter::prefilter_result> runtime::run_sneaky_snake_rank_batch_roundtrip(
    std::span<const std::uint8_t> query_ranks,
    std::span<const std::uint8_t> target_ranks,
    std::span<const bioseq::prefilter::prefilter_job> jobs) {
    std::lock_guard<std::mutex> guard(mu_);
    constexpr std::uint32_t k_sneaky_snake_parallel_threshold = 16u;
    bool use_parallel_teams = false;
    for (const bioseq::prefilter::prefilter_job& job : jobs) {
        if (job.max_edits >= k_sneaky_snake_parallel_threshold &&
            job.max_edits <= k_gpu_sneaky_snake_max_edits &&
            job.query_length == job.target_length && job.query_length != 0u) {
            use_parallel_teams = true;
            break;
        }
    }
    const std::uint32_t push_mode = use_parallel_teams ? 8u : 1u;
    const std::optional<std::size_t> replay_index =
        rank_prefilter_replay_index(shader_kind::sneaky_snake_rank_batch, push_mode);
    if (!replay_index) {
        throw vulkan_runtime_error("invalid SneakySnake replay route");
    }
    std::vector<bioseq::prefilter::prefilter_result> results =
        run_rank_prefilter_batch_roundtrip_locked(shader_kind::sneaky_snake_rank_batch,
                                                  push_mode,
                                                  *replay_index,
                                                  query_ranks,
                                                  target_ranks,
                                                  jobs);
    if (!jobs.empty()) {
        sneaky_snake_dispatch_count_.fetch_add(1u, std::memory_order_relaxed);
    }
    return results;
}

std::vector<bioseq::prefilter::prefilter_result> runtime::run_gatekeeper_rank_batch_roundtrip(
    std::span<const std::uint8_t> query_ranks,
    std::span<const std::uint8_t> target_ranks,
    std::span<const bioseq::prefilter::prefilter_job> jobs) {
    std::lock_guard<std::mutex> guard(mu_);
    const gatekeeper_rank_batch_route route = resolve_gatekeeper_rank_batch_route(jobs);
    std::vector<bioseq::prefilter::prefilter_result> results =
        run_rank_prefilter_batch_roundtrip_locked(
            route.kind, 0u, route.replay_index, query_ranks, target_ranks, jobs);
    if (!jobs.empty()) {
        gatekeeper_dispatch_count_.fetch_add(1u, std::memory_order_relaxed);
    }
    return results;
}

std::vector<bioseq::prefilter::prefilter_result> runtime::run_rank_prefilter_batch_roundtrip_locked(
    shader_kind kind,
    std::uint32_t push_mode,
    std::size_t replay_index,
    std::span<const std::uint8_t> query_ranks,
    std::span<const std::uint8_t> target_ranks,
    std::span<const bioseq::prefilter::prefilter_job> jobs) {
    const bool sneaky = kind == shader_kind::sneaky_snake_rank_batch;
    const bool gatekeeper = is_gatekeeper_rank_batch_kind(kind);
    const std::optional<std::size_t> expected_replay_index =
        rank_prefilter_replay_index(kind, push_mode);
    if ((!sneaky && !gatekeeper) || replay_index >= prefilter_recorded_commands_.size() ||
        !expected_replay_index || replay_index != *expected_replay_index) {
        throw vulkan_runtime_error("invalid rank-prefilter dispatch route");
    }
    const char* algorithm = sneaky ? "SneakySnake" : "GateKeeper";
    if (!features_.available) {
        throw vulkan_runtime_error(fmt::format("run_{}_rank_batch_roundtrip on unavailable device",
                                               sneaky ? "sneaky_snake" : "gatekeeper"));
    }
    if (jobs.empty()) {
        return {};
    }
    validate_prefilter_inputs(algorithm, query_ranks, target_ranks, jobs);
    if (features_.max_workgroup_count_x == 0u) {
        throw vulkan_runtime_error(
            fmt::format("{} device reports zero max workgroup count", algorithm));
    }

    constexpr std::uint32_t k_prefilter_local_size = 64u;
    constexpr std::uint64_t k_retained_workspace_bytes = 64u * 1024u * 1024u;
    const compute_pipeline pipe = get_compute_pipeline_locked(
        kind,
        k_prefilter_local_size,
        sneaky ? k_gpu_sneaky_snake_submaze_length : k_gpu_gatekeeper_max_length);

    const std::uint64_t query_bytes = std::max<std::uint64_t>(
        ((static_cast<std::uint64_t>(query_ranks.size()) + 3u) / 4u) * 4u, sizeof(std::uint32_t));
    const std::uint64_t target_bytes = std::max<std::uint64_t>(
        ((static_cast<std::uint64_t>(target_ranks.size()) + 3u) / 4u) * 4u, sizeof(std::uint32_t));
    const std::uint64_t job_bytes = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(jobs.size()) * sizeof(bioseq::prefilter::prefilter_job),
        sizeof(std::uint32_t));
    const std::uint64_t result_bytes =
        static_cast<std::uint64_t>(jobs.size()) * sizeof(bioseq::prefilter::prefilter_result);

    for (const auto [label, bytes] : {std::pair{"query ranks", query_bytes},
                                      std::pair{"target ranks", target_bytes},
                                      std::pair{"jobs", job_bytes},
                                      std::pair{"results", result_bytes}}) {
        if (bytes > features_.max_storage_buffer_range) {
            throw vulkan_runtime_error(
                fmt::format("{} {} buffer {} exceeds maxStorageBufferRange {}",
                            algorithm,
                            label,
                            bytes,
                            features_.max_storage_buffer_range));
        }
    }

    std::vector<bioseq::prefilter::prefilter_result> results;
    try {
        ensure_prefilter_workspace_buffer_locked(
            prefilter_query_buffer_, query_bytes, k_compute_buffer_usage);
        ensure_prefilter_workspace_buffer_locked(
            prefilter_target_buffer_, target_bytes, k_compute_buffer_usage);
        ensure_prefilter_workspace_buffer_locked(
            prefilter_jobs_buffer_, job_bytes, k_compute_buffer_usage);
        ensure_prefilter_workspace_buffer_locked(
            prefilter_results_buffer_, result_bytes, k_compute_buffer_usage);
        if (prefilter_query_buffer_.mapped == nullptr ||
            prefilter_target_buffer_.mapped == nullptr ||
            prefilter_jobs_buffer_.mapped == nullptr ||
            prefilter_results_buffer_.mapped == nullptr) {
            throw vulkan_runtime_error(
                fmt::format("run_{}_rank_batch_roundtrip: device buffer alloc failed",
                            sneaky ? "sneaky_snake" : "gatekeeper"));
        }

        if constexpr (std::endian::native == std::endian::little) {
            if (!query_ranks.empty()) {
                std::memcpy(prefilter_query_buffer_.mapped, query_ranks.data(), query_ranks.size());
            }
            if (!target_ranks.empty()) {
                std::memcpy(
                    prefilter_target_buffer_.mapped, target_ranks.data(), target_ranks.size());
            }
            const std::size_t query_tail = query_ranks.size() & 3u;
            if (query_tail != 0u || query_ranks.empty()) {
                std::memset(static_cast<std::byte*>(prefilter_query_buffer_.mapped) +
                                query_ranks.size(),
                            0,
                            query_ranks.empty() ? sizeof(std::uint32_t) : 4u - query_tail);
            }
            const std::size_t target_tail = target_ranks.size() & 3u;
            if (target_tail != 0u || target_ranks.empty()) {
                std::memset(static_cast<std::byte*>(prefilter_target_buffer_.mapped) +
                                target_ranks.size(),
                            0,
                            target_ranks.empty() ? sizeof(std::uint32_t) : 4u - target_tail);
            }
        } else {
            std::memset(prefilter_query_buffer_.mapped, 0, static_cast<std::size_t>(query_bytes));
            for (std::size_t base = 0u; base < query_ranks.size(); base += 4u) {
                std::uint32_t packed = 0u;
                const std::size_t count = std::min<std::size_t>(4u, query_ranks.size() - base);
                for (std::size_t lane = 0u; lane < count; ++lane) {
                    packed |= static_cast<std::uint32_t>(query_ranks[base + lane]) << (lane * 8u);
                }
                std::memcpy(static_cast<std::byte*>(prefilter_query_buffer_.mapped) + base,
                            &packed,
                            sizeof(packed));
            }
            std::memset(prefilter_target_buffer_.mapped, 0, static_cast<std::size_t>(target_bytes));
            for (std::size_t base = 0u; base < target_ranks.size(); base += 4u) {
                std::uint32_t packed = 0u;
                const std::size_t count = std::min<std::size_t>(4u, target_ranks.size() - base);
                for (std::size_t lane = 0u; lane < count; ++lane) {
                    packed |= static_cast<std::uint32_t>(target_ranks[base + lane]) << (lane * 8u);
                }
                std::memcpy(static_cast<std::byte*>(prefilter_target_buffer_.mapped) + base,
                            &packed,
                            sizeof(packed));
            }
        }
        std::memcpy(prefilter_jobs_buffer_.mapped,
                    jobs.data(),
                    jobs.size() * sizeof(bioseq::prefilter::prefilter_job));

        const std::array<VkBuffer, 4> descriptor_buffers{{
            prefilter_query_buffer_.buffer_handle,
            prefilter_target_buffer_.buffer_handle,
            prefilter_jobs_buffer_.buffer_handle,
            prefilter_results_buffer_.buffer_handle,
        }};
        prefilter_descriptor_binding_cache& descriptor_binding_cache =
            prefilter_descriptor_binding_cache_[gatekeeper ? 1u : 0u];
        if (descriptor_buffers != descriptor_binding_cache.buffers ||
            pipe.descriptor_set != descriptor_binding_cache.descriptor_set) {
            bind_compute_pipeline_descriptors_locked(pipe, descriptor_buffers);
            descriptor_binding_cache.buffers = descriptor_buffers;
            descriptor_binding_cache.descriptor_set = pipe.descriptor_set;
        }
        if ((sneaky && push_mode != 1u && push_mode != 8u) || (gatekeeper && push_mode != 0u)) {
            throw vulkan_runtime_error(
                fmt::format("{} invalid rank-prefilter push mode {}", algorithm, push_mode));
        }
        const std::uint32_t jobs_per_workgroup =
            sneaky ? k_prefilter_local_size / push_mode : k_prefilter_local_size;
        const gpu_prefilter_push push{
            .job_count = static_cast<std::uint32_t>(jobs.size()),
            .mode = push_mode,
            .query_rank_count = static_cast<std::uint32_t>(query_ranks.size()),
            .target_rank_count = static_cast<std::uint32_t>(target_ranks.size()),
        };
        const std::uint64_t groups_needed =
            (static_cast<std::uint64_t>(jobs.size()) + jobs_per_workgroup - 1u) /
            jobs_per_workgroup;
        const std::uint32_t groups = static_cast<std::uint32_t>(
            std::min(groups_needed, static_cast<std::uint64_t>(features_.max_workgroup_count_x)));
        if (groups == 0u) {
            throw vulkan_runtime_error(
                fmt::format("{} grid-stride dispatch has zero workgroups", algorithm));
        }

        prefilter_recorded_command& recorded = prefilter_recorded_commands_[replay_index];
        const bool command_matches =
            recorded.recorded && recorded.kind == kind && recorded.pipeline == pipe.pipeline &&
            recorded.pipeline_layout == pipe.pipeline_layout &&
            recorded.descriptor_set == pipe.descriptor_set &&
            recorded.push.job_count == push.job_count && recorded.push.mode == push.mode &&
            recorded.push.query_rank_count == push.query_rank_count &&
            recorded.push.target_rank_count == push.target_rank_count && recorded.groups == groups;
        if (!command_matches) {
            recorded.recorded = false;
            check_vk(vkResetCommandBuffer(recorded.command_buffer, 0), "reset(prefilter_cached)");
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            check_vk(vkBeginCommandBuffer(recorded.command_buffer, &begin),
                     "begin(prefilter_cached)");
            record_host_to_shader(recorded.command_buffer);
            vkCmdBindPipeline(
                recorded.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipe.pipeline);
            vkCmdBindDescriptorSets(recorded.command_buffer,
                                    VK_PIPELINE_BIND_POINT_COMPUTE,
                                    pipe.pipeline_layout,
                                    0,
                                    1,
                                    &pipe.descriptor_set,
                                    0,
                                    nullptr);
            vkCmdPushConstants(recorded.command_buffer,
                               pipe.pipeline_layout,
                               VK_SHADER_STAGE_COMPUTE_BIT,
                               0,
                               sizeof(push),
                               &push);
            vkCmdDispatch(recorded.command_buffer, groups, 1u, 1u);
            record_shader_to_host(recorded.command_buffer);
            check_vk(vkEndCommandBuffer(recorded.command_buffer), "end(prefilter_cached)");
            recorded.kind = kind;
            recorded.pipeline = pipe.pipeline;
            recorded.pipeline_layout = pipe.pipeline_layout;
            recorded.descriptor_set = pipe.descriptor_set;
            recorded.push = push;
            recorded.groups = groups;
            recorded.recorded = true;
        }
        submit_executable_and_wait(
            device_, queue_, recorded.command_buffer, fence_, "submit(prefilter_cached)");

        results.resize(jobs.size());
        std::memcpy(results.data(),
                    prefilter_results_buffer_.mapped,
                    jobs.size() * sizeof(bioseq::prefilter::prefilter_result));
        if (sneaky) {
            validate_sneaky_snake_results(jobs, results, query_ranks.size(), target_ranks.size());
        } else {
            validate_gatekeeper_results(jobs, results, query_ranks.size(), target_ranks.size());
        }
    } catch (...) {
        if (prefilter_workspace_allocation_bytes_locked() > k_retained_workspace_bytes) {
            // A submit may have succeeded before a wait failed. Do not destroy
            // mapped buffers until the device is known idle (or lost, which
            // also guarantees that no work can continue using them).
            const VkResult idle_result = vkDeviceWaitIdle(device_);
            if (idle_result == VK_SUCCESS || idle_result == VK_ERROR_DEVICE_LOST) {
                release_prefilter_workspace_locked();
            }
        }
        throw;
    }
    if (prefilter_workspace_allocation_bytes_locked() > k_retained_workspace_bytes) {
        release_prefilter_workspace_locked();
    }
    return results;
}

} // namespace bioseq::gpu::vulkan
