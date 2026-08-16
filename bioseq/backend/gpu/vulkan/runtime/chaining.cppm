/**
 * @file chaining.cppm
 * @brief Records tiled dynamic-programming chaining and coordinate-trace dispatches.
 */
module;

#include <cstdint>
#include <libassert/assert.hpp>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

module bioseq.gpu.vulkan.runtime:chaining;

import :frontend_types;
import :support;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.parallelism.memory_admission;
import bioseq.instrumentation;
import std;

namespace bioseq::gpu::vulkan {

/// @brief Runs tiled dynamic-programming chaining over caller-owned buffers.
void runtime::run_chaining_tiled_dp_roundtrip(const chaining_pipeline_key& key,
                                              buffer& transfer_scratch,
                                              buffer& anchors,
                                              buffer& ranges,
                                              buffer& best_scores,
                                              buffer& predecessors,
                                              std::span<const std::byte> anchors_bytes,
                                              std::span<const std::byte> ranges_bytes,
                                              std::span<std::byte> best_scores_bytes,
                                              std::span<std::byte> predecessors_bytes,
                                              std::uint32_t anchor_count,
                                              std::uint32_t range_count,
                                              std::int32_t max_dist_x,
                                              std::int32_t max_dist_y,
                                              std::int32_t bandwidth,
                                              std::int32_t max_iter,
                                              float gap_penalty,
                                              float skip_penalty,
                                              std::int32_t max_chain_skip) {
    std::lock_guard<std::mutex> guard(mu_);

    if (!features_.available) {
        throw vulkan_runtime_error("run_chaining_tiled_dp_roundtrip on unavailable device");
    }
    // ONE WORKGROUP PER RANGE; the kernel grid-strides
    // workgroups over ranges, so the dispatch is min(range_count, device limit)
    // workgroups — range_count is just data, no workgroup-count throw.
    const std::uint32_t chain_groups = std::min(range_count, features_.max_workgroup_count_x);
    const VkPipeline pipeline = create_pipeline_locked(key).pipeline;

    // H2D: write the anchors/ranges and the caller-initialised best-scores /
    // predecessors straight into the host-visible device buffers.
    upload_locked(transfer_scratch, anchors, anchors_bytes, VK_ACCESS_SHADER_READ_BIT);
    upload_locked(transfer_scratch, ranges, ranges_bytes, VK_ACCESS_SHADER_READ_BIT);
    upload_locked(transfer_scratch,
                  best_scores,
                  best_scores_bytes,
                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    upload_locked(transfer_scratch,
                  predecessors,
                  predecessors_bytes,
                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

    // Point the reusable descriptor set at this call's four buffers.
    const std::array<VkDescriptorBufferInfo, 4> buffer_infos{{
        {anchors.buffer_handle, 0, VK_WHOLE_SIZE},
        {ranges.buffer_handle, 0, VK_WHOLE_SIZE},
        {best_scores.buffer_handle, 0, VK_WHOLE_SIZE},
        {predecessors.buffer_handle, 0, VK_WHOLE_SIZE},
    }};
    std::array<VkWriteDescriptorSet, 4> writes{};
    for (std::uint32_t binding_index = 0; binding_index < 4; ++binding_index) {
        writes[binding_index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[binding_index].dstSet = descriptor_set_;
        writes[binding_index].dstBinding = binding_index;
        writes[binding_index].descriptorCount = 1;
        writes[binding_index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[binding_index].pBufferInfo = &buffer_infos[binding_index];
    }
    vkUpdateDescriptorSets(
        device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);

    chaining_push_constants push{};
    push.anchor_count = anchor_count;
    push.range_count = range_count;
    push.max_dist_x = max_dist_x;
    push.max_dist_y = max_dist_y;
    push.bandwidth = bandwidth;
    push.max_iter = max_iter;
    push.gap_penalty = gap_penalty;
    push.skip_penalty = skip_penalty;
    push.max_chain_skip = max_chain_skip;

    // record: host-write barrier -> dispatch -> shader-write->host-read barrier.
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "vkBeginCommandBuffer");

    VkMemoryBarrier host_to_shader{};
    host_to_shader.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    host_to_shader.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    host_to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(command_buffer_,
                         VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0,
                         1,
                         &host_to_shader,
                         0,
                         nullptr,
                         0,
                         nullptr);

    vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command_buffer_,
                            VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline_layout_,
                            0,
                            1,
                            &descriptor_set_,
                            0,
                            nullptr);
    vkCmdPushConstants(
        command_buffer_, pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(command_buffer_, chain_groups, 1, 1);

    VkMemoryBarrier shader_to_host{};
    shader_to_host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    shader_to_host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    shader_to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command_buffer_,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT,
                         0,
                         1,
                         &shader_to_host,
                         0,
                         nullptr,
                         0,
                         nullptr);

    check_vk(vkEndCommandBuffer(command_buffer_), "vkEndCommandBuffer");

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffer_;
    check_vk(vkQueueSubmit(queue_, 1, &submit, fence_), "vkQueueSubmit");
    check_vk(vkWaitForFences(device_,
                             1,
                             &fence_,
                             VK_TRUE,
                             std::numeric_limits<std::uint64_t>::max()),
             "vkWaitForFences");
    require_immutable_metal_policy_live("chaining operation completion");
    check_vk(vkResetFences(device_, 1, &fence_), "vkResetFences");

    // D2H: read the propagated scores / predecessors back.
    download_locked(transfer_scratch, best_scores, best_scores_bytes);
    download_locked(transfer_scratch, predecessors, predecessors_bytes);

    check_vk(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer(post)");
    chaining_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
}

/// @brief Runs coordinate chaining and downloads the selected predecessor trace.
gpu_coordinate_chain_trace_result
runtime::run_chaining_coordinate_trace_roundtrip(const chaining_pipeline_key& key,
                                                 buffer& transfer_scratch,
                                                 buffer& anchors,
                                                 buffer& ranges,
                                                 buffer& best_scores,
                                                 buffer& predecessors,
                                                 std::span<const std::byte> anchors_bytes,
                                                 std::span<const std::byte> ranges_bytes,
                                                 std::span<const std::byte> best_scores_bytes,
                                                 std::span<const std::byte> predecessors_bytes,
                                                 std::uint32_t anchor_count,
                                                 std::uint32_t range_count,
                                                 std::uint64_t query_len,
                                                 std::int32_t max_dist_x,
                                                 std::int32_t max_dist_y,
                                                 std::int32_t bandwidth,
                                                 std::int32_t max_iter,
                                                 float gap_penalty,
                                                 float skip_penalty,
                                                 std::int32_t max_chain_skip,
                                                 std::int32_t min_chain_score,
                                                 std::int32_t min_seed_count,
                                                 std::uint32_t probe_limit) {
    std::lock_guard<std::mutex> guard(mu_);

    gpu_coordinate_chain_trace_result out{};
    if (!features_.available) {
        throw vulkan_runtime_error("run_chaining_coordinate_trace_roundtrip on unavailable device");
    }

    const std::uint32_t chain_groups = std::min(range_count, features_.max_workgroup_count_x);
    const VkPipeline pipeline = create_pipeline_locked(key).pipeline;
    const compute_pipeline coord_pipe =
        get_compute_pipeline_locked(shader_kind::coordinate_reduce, 128u, 1u);

    upload_locked(transfer_scratch, anchors, anchors_bytes, VK_ACCESS_SHADER_READ_BIT);
    upload_locked(transfer_scratch, ranges, ranges_bytes, VK_ACCESS_SHADER_READ_BIT);
    upload_locked(transfer_scratch,
                  best_scores,
                  best_scores_bytes,
                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    upload_locked(transfer_scratch,
                  predecessors,
                  predecessors_bytes,
                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

    constexpr VkBufferUsageFlags coordinate_trace_buffer_usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer visited = create_device_buffer(std::uint64_t{anchor_count} * sizeof(std::uint32_t),
                                          coordinate_trace_buffer_usage);
    buffer trace = create_device_buffer(std::uint64_t{anchor_count} * sizeof(std::uint32_t),
                                        coordinate_trace_buffer_usage);
    buffer result_buf =
        create_device_buffer(sizeof(gpu_coordinate_result_wire), coordinate_trace_buffer_usage);
    if (visited.mapped == nullptr || trace.mapped == nullptr || result_buf.mapped == nullptr) {
        throw vulkan_runtime_error("coordinate-only chaining requires host-visible trace buffers");
    }
    std::memset(visited.mapped, 0, static_cast<std::size_t>(visited.bytes));
    std::memset(result_buf.mapped, 0, static_cast<std::size_t>(result_buf.bytes));

    const std::array<VkDescriptorBufferInfo, 4> chain_infos{{
        {anchors.buffer_handle, 0, VK_WHOLE_SIZE},
        {ranges.buffer_handle, 0, VK_WHOLE_SIZE},
        {best_scores.buffer_handle, 0, VK_WHOLE_SIZE},
        {predecessors.buffer_handle, 0, VK_WHOLE_SIZE},
    }};
    std::array<VkWriteDescriptorSet, 4> writes{};
    for (std::uint32_t binding_index = 0; binding_index < 4; ++binding_index) {
        writes[binding_index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[binding_index].dstSet = descriptor_set_;
        writes[binding_index].dstBinding = binding_index;
        writes[binding_index].descriptorCount = 1;
        writes[binding_index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[binding_index].pBufferInfo = &chain_infos[binding_index];
    }
    vkUpdateDescriptorSets(
        device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);

    chaining_push_constants chain_push{};
    chain_push.anchor_count = anchor_count;
    chain_push.range_count = range_count;
    chain_push.max_dist_x = max_dist_x;
    chain_push.max_dist_y = max_dist_y;
    chain_push.bandwidth = bandwidth;
    chain_push.max_iter = max_iter;
    chain_push.gap_penalty = gap_penalty;
    chain_push.skip_penalty = skip_penalty;
    chain_push.max_chain_skip = max_chain_skip;

    reset_dispatch_descriptor_pool_locked();
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(chain_coord)");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(chain_coord)");

    VkMemoryBarrier host_to_shader{};
    host_to_shader.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    host_to_shader.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    host_to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(command_buffer_,
                         VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0,
                         1,
                         &host_to_shader,
                         0,
                         nullptr,
                         0,
                         nullptr);

    vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command_buffer_,
                            VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline_layout_,
                            0,
                            1,
                            &descriptor_set_,
                            0,
                            nullptr);
    vkCmdPushConstants(command_buffer_,
                       pipeline_layout_,
                       VK_SHADER_STAGE_COMPUTE_BIT,
                       0,
                       sizeof(chain_push),
                       &chain_push);
    vkCmdDispatch(command_buffer_, chain_groups, 1, 1);
    VkMemoryBarrier shader_to_shader{};
    shader_to_shader.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    shader_to_shader.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    shader_to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(command_buffer_,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0,
                         1,
                         &shader_to_shader,
                         0,
                         nullptr,
                         0,
                         nullptr);

    const std::array<VkDescriptorBufferInfo, 6> coord_infos{{
        {anchors.buffer_handle, 0, VK_WHOLE_SIZE},
        {best_scores.buffer_handle, 0, VK_WHOLE_SIZE},
        {predecessors.buffer_handle, 0, VK_WHOLE_SIZE},
        {visited.buffer_handle, 0, visited.bytes},
        {trace.buffer_handle, 0, trace.bytes},
        {result_buf.buffer_handle, 0, result_buf.bytes},
    }};
    gpu_coordinate_reduce_push coord_push{
        .anchor_count = anchor_count,
        .query_len_lo = static_cast<std::uint32_t>(query_len & 0xffffffffull),
        .query_len_hi = static_cast<std::uint32_t>(query_len >> 32),
        .min_chain_score = min_chain_score,
        .min_seed_count = min_seed_count,
        .probe_limit = probe_limit,
        .reserved = 0u,
    };
    record_compute_dispatch_locked(
        command_buffer_, coord_pipe, coord_infos, &coord_push, sizeof(coord_push), 1u);
    VkMemoryBarrier shader_to_host{};
    shader_to_host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    shader_to_host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    shader_to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command_buffer_,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT,
                         0,
                         1,
                         &shader_to_host,
                         0,
                         nullptr,
                         0,
                         nullptr);

    check_vk(vkEndCommandBuffer(command_buffer_), "end(chain_coord)");

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffer_;
    check_vk(vkQueueSubmit(queue_, 1, &submit, fence_), "submit(chain_coord)");
    check_vk(vkWaitForFences(device_,
                             1,
                             &fence_,
                             VK_TRUE,
                             std::numeric_limits<std::uint64_t>::max()),
             "wait(chain_coord)");
    require_immutable_metal_policy_live("chaining coordinate operation completion");
    check_vk(vkResetFences(device_, 1, &fence_), "reset_fence(chain_coord)");

    std::memcpy(&out.result, result_buf.mapped, sizeof(out.result));
    if (out.result.is_valid != 0u) {
        if (out.result.seed_count < 0 ||
            static_cast<std::uint64_t>(out.result.seed_count) > anchor_count) {
            throw vulkan_runtime_error("coordinate-only chaining produced invalid trace length");
        }
        const auto trace_count = static_cast<std::size_t>(out.result.seed_count);
        out.trace.resize(trace_count);
        if (trace_count > 0) {
            std::memcpy(out.trace.data(), trace.mapped, trace_count * sizeof(std::uint32_t));
        }
    }

    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(chain_coord_post)");
    chaining_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    return out;
}

// ---------------------------------------------------------------------------
// Seed lookup — seeding roundtrips
// ---------------------------------------------------------------------------

} // namespace bioseq::gpu::vulkan
