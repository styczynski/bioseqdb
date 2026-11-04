/**
 * @file prefilter.cppm
 * @brief Runs SneakySnake and GateKeeper filters using reusable GPU commands.
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
import bioseq.simd.utils;

namespace bioseq::gpu::vulkan {

/// @brief Run SneakySnake on sequences encoded as symbol ranks.
/// Use teams of GPU threads when a supported job has a large edit limit.
std::vector<bioseq::prefilter::prefilter_result> runtime::run_sneaky_snake_rank_batch_roundtrip(
    std::span<const gpu_packed_sequence_upload> queries,
    std::span<const gpu_packed_sequence_upload> targets,
    const vulkan_rank_lut& rank_lut,
    std::span<const bioseq::prefilter::prefilter_job> jobs,
    bioseq::simd::sequence_packed_rank_validation_kernel validate_ranks,
    std::uint32_t rank_bits) {
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
                                                  queries,
                                                  targets,
                                                  rank_lut,
                                                  jobs, validate_ranks, rank_bits);
    if (!jobs.empty()) {
        sneaky_snake_dispatch_count_.fetch_add(1u, std::memory_order_relaxed);
    }
    return results;
}

/// @brief Runs GateKeeper, choosing a specialized shader when all jobs fit its fixed shape.
std::vector<bioseq::prefilter::prefilter_result> runtime::run_gatekeeper_rank_batch_roundtrip(
    std::span<const gpu_packed_sequence_upload> queries,
    std::span<const gpu_packed_sequence_upload> targets,
    const vulkan_rank_lut& rank_lut,
    std::span<const bioseq::prefilter::prefilter_job> jobs,
    bioseq::simd::sequence_packed_rank_validation_kernel validate_ranks) {
    std::lock_guard<std::mutex> guard(mu_);
    const gatekeeper_rank_batch_route route = resolve_gatekeeper_rank_batch_route(jobs);
    std::vector<bioseq::prefilter::prefilter_result> results =
        run_rank_prefilter_batch_roundtrip_locked(
            route.kind, 0u, route.replay_index, queries, targets, rank_lut, jobs, validate_ranks, 0u);
    if (!jobs.empty()) {
        gatekeeper_dispatch_count_.fetch_add(1u, std::memory_order_relaxed);
    }
    return results;
}

/// @brief Uploads prefilter jobs, runs their GPU commands, and checks the returned results.
/// Reuses recorded commands when the pipeline and batch shape match.
/// The caller holds the runtime lock.
std::vector<bioseq::prefilter::prefilter_result> runtime::run_rank_prefilter_batch_roundtrip_locked(
    shader_kind kind,
    std::uint32_t push_mode,
    std::size_t replay_index,
    std::span<const gpu_packed_sequence_upload> queries,
    std::span<const gpu_packed_sequence_upload> targets,
    const vulkan_rank_lut& rank_lut,
    std::span<const bioseq::prefilter::prefilter_job> jobs,
    bioseq::simd::sequence_packed_rank_validation_kernel validate_ranks,
    std::uint32_t rank_bits) {
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
    constexpr std::uint32_t k_prefilter_local_size = 64u;
    const shader_variant_key key = make_shader_key(
        kind, 384u, k_prefilter_local_size,
        sneaky ? rank_bits : k_gpu_gatekeeper_max_length);
    if ((!sneaky && rank_bits != 0u) || !is_shader_variant_key_valid(key)) {
        throw vulkan_runtime_error("invalid rank-prefilter width selector");
    }
    validate_prefilter_inputs(algorithm, queries, targets, rank_lut, jobs, validate_ranks, rank_bits);
    if (features_.max_workgroup_count_x == 0u) {
        throw vulkan_runtime_error(
            fmt::format("{} device reports zero max workgroup count", algorithm));
    }

    constexpr std::uint64_t k_retained_workspace_bytes = 64u * 1024u * 1024u;
    const compute_pipeline pipe = get_compute_pipeline_locked(
        kind, key.local_size, key.max_span);

    std::uint64_t packed_bytes = 0u;
    const auto count_packed_bytes = [&packed_bytes](auto uploads) {
        for (const auto& upload : uploads) {
            packed_bytes = bioseq::common::saturating_add(packed_bytes, upload.sequence.padded_byte_count);
        }
    };
    count_packed_bytes(queries);
    count_packed_bytes(targets);
    const std::uint64_t packed_sequence_bytes = std::max<std::uint64_t>(packed_bytes, sizeof(std::uint32_t));
    const std::uint64_t query_record_bytes = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(queries.size()) * sizeof(gpu_sequence_view_wire), sizeof(gpu_sequence_view_wire));
    const std::uint64_t target_record_bytes = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(targets.size()) * sizeof(gpu_sequence_view_wire), sizeof(gpu_sequence_view_wire));
    const std::uint64_t job_bytes = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(jobs.size()) * sizeof(bioseq::prefilter::prefilter_job),
        sizeof(std::uint32_t));
    const std::uint64_t result_bytes =
        static_cast<std::uint64_t>(jobs.size()) * sizeof(bioseq::prefilter::prefilter_result);
    constexpr std::uint64_t rank_policy_bytes = sizeof(vulkan_rank_lut);

    for (const auto [label, bytes] : {std::pair{"packed sequences", packed_sequence_bytes},
                                      std::pair{"query descriptors", query_record_bytes},
                                      std::pair{"target descriptors", target_record_bytes},
                                      std::pair{"rank policy", rank_policy_bytes},
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
            prefilter_query_buffer_, packed_sequence_bytes, k_compute_buffer_usage);
        ensure_prefilter_workspace_buffer_locked(
            prefilter_target_buffer_, query_record_bytes, k_compute_buffer_usage);
        ensure_prefilter_workspace_buffer_locked(
            prefilter_jobs_buffer_, job_bytes, k_compute_buffer_usage);
        ensure_prefilter_workspace_buffer_locked(
            prefilter_results_buffer_, result_bytes, k_compute_buffer_usage);
        ensure_prefilter_workspace_buffer_locked(
            prefilter_target_records_buffer_, target_record_bytes, k_compute_buffer_usage);
        ensure_prefilter_workspace_buffer_locked(
            prefilter_rank_lut_buffer_, rank_policy_bytes, k_compute_buffer_usage);
        if (prefilter_target_records_buffer_.mapped == nullptr ||
            prefilter_rank_lut_buffer_.mapped == nullptr ||
            prefilter_query_buffer_.mapped == nullptr ||
            prefilter_target_buffer_.mapped == nullptr ||
            prefilter_jobs_buffer_.mapped == nullptr ||
            prefilter_results_buffer_.mapped == nullptr) {
            throw vulkan_runtime_error(
                fmt::format("run_{}_rank_batch_roundtrip: device buffer alloc failed",
                            sneaky ? "sneaky_snake" : "gatekeeper"));
        }

        // Copy each packed window and its returned descriptor directly into admitted storage.
        std::size_t packed_byte_offset = 0u;
        const auto upload_records = [this, packed_sequence_bytes, &packed_byte_offset](
            auto uploads, std::span<std::byte> record_destination) {
            const std::span<std::byte> destination{
                static_cast<std::byte*>(prefilter_query_buffer_.mapped),
                static_cast<std::size_t>(packed_sequence_bytes)};
            for (std::size_t index = 0u; index < uploads.size(); ++index) {
                const auto descriptor = place_packed_sequence_upload(uploads[index],
                    packed_byte_offset, destination, packed_sequence_upload_geometry_is_valid);
                if (!descriptor)
                    throw vulkan_runtime_error("prefilter packed arena placement failed");
                // The destination was sized from this upload count before workspace admission.
                std::memcpy(record_destination.data() + index * sizeof(gpu_sequence_view_wire),
                            &*descriptor, sizeof(gpu_sequence_view_wire));
                packed_byte_offset += static_cast<std::size_t>(descriptor->padded_byte_count);
            }
        };
        upload_records(queries, {static_cast<std::byte*>(prefilter_target_buffer_.mapped),
                                static_cast<std::size_t>(query_record_bytes)});
        upload_records(targets, {static_cast<std::byte*>(prefilter_target_records_buffer_.mapped),
                                static_cast<std::size_t>(target_record_bytes)});
        std::memcpy(prefilter_rank_lut_buffer_.mapped, &rank_lut, sizeof(rank_lut));
        std::memcpy(prefilter_jobs_buffer_.mapped,
                    jobs.data(),
                    jobs.size() * sizeof(bioseq::prefilter::prefilter_job));

        const std::array<VkBuffer, 6> descriptor_buffers{{
            prefilter_query_buffer_.buffer_handle,
            prefilter_target_buffer_.buffer_handle,
            prefilter_jobs_buffer_.buffer_handle,
            prefilter_results_buffer_.buffer_handle,
            prefilter_target_records_buffer_.buffer_handle,
            prefilter_rank_lut_buffer_.buffer_handle,
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
            .query_record_count = static_cast<std::uint32_t>(queries.size()),
            .target_record_count = static_cast<std::uint32_t>(targets.size()),
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
            recorded.push.query_record_count == push.query_record_count &&
            recorded.push.target_record_count == push.target_record_count && recorded.groups == groups;
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
            validate_sneaky_snake_results(jobs, results);
        } else {
            validate_gatekeeper_results(jobs, results);
        }
    } catch (...) {
        if (prefilter_workspace_allocation_bytes_locked() > k_retained_workspace_bytes) {
            // A failed wait can leave submitted work  running. Keep its buffers until
            // the device is idle or reports that it is lost.
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
