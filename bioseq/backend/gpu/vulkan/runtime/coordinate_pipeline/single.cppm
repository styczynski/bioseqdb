/**
 * @file single.cppm
 * @brief Records the single-query seed-to-coordinate descriptor and submission sequence.
 */
module;

#include <cstdint>
#include <libassert/assert.hpp>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

export module bioseq.gpu.vulkan.runtime:coordinate_pipeline.single;

import :frontend_types;
import :support;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.parallelism.memory_admission;
import bioseq.instrumentation;
import std;

namespace bioseq::gpu::vulkan {

/// @brief Runs the single-query seed, sort, chain, and coordinate submission sequence.
/// @param[in] inputs Borrowed resident target index and query minimizer storage.
/// @param[in] query_len Query length in symbols.
/// @param[in] target_len Target length used when the resident input omits it.
/// @param[in] kmer_size K-mer span encoded into anchors.
/// @param[in] max_occ Maximum target occurrences emitted per minimizer.
/// @param[in] chain_key Chaining pipeline specialization.
/// @param[in] max_dist_x Maximum reference-coordinate chaining distance.
/// @param[in] max_dist_y Maximum query-coordinate chaining distance.
/// @param[in] bandwidth Chaining diagonal bandwidth.
/// @param[in] max_iter Maximum predecessors examined per anchor.
/// @param[in] gap_penalty Chaining gap penalty.
/// @param[in] skip_penalty Chaining skip penalty.
/// @param[in] max_chain_skip Maximum consecutive unproductive predecessors.
/// @param[in] min_chain_score Minimum accepted chain score.
/// @param[in] min_seed_count Minimum accepted seed count.
/// @param[in] probe_limit Maximum coordinate candidates examined.
/// @return Downloaded coordinate result. An all-zero wire denotes no accepted chain.
/// @throws vulkan_runtime_error When validation, allocation, submission, or policy checks fail.
gpu_coordinate_result_wire
runtime::run_seed_to_coordinate_roundtrip(const gpu_seed_inputs& inputs,
                                          std::uint64_t query_len,
                                          std::uint64_t target_len,
                                          std::uint32_t kmer_size,
                                          std::int32_t max_occ,
                                          const chaining_pipeline_key& chain_key,
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
    gpu_coordinate_result_wire result{};
    if (!features_.available) {
        throw vulkan_runtime_error("run_seed_to_coordinate_roundtrip on unavailable device");
    }
    if (inputs.slots == nullptr || inputs.positions == nullptr) {
        throw vulkan_runtime_error("gpu seed-to-coordinate missing resident index buffers");
    }
    if (query_len > std::numeric_limits<std::uint32_t>::max()) {
        throw vulkan_runtime_error("gpu seed-to-coordinate query length exceeds ABI");
    }
    const std::uint32_t mc = inputs.minimizer_count;
    if (mc == 0u || max_occ < 0)
        return result;

    // Mirrors the range-builder push block for this one-query dispatch.
    struct chain_range_push_constants {
        std::uint32_t query_count;
        std::uint32_t max_ref_gap;
    };
    static_assert(sizeof(chain_range_push_constants) == 8);

    gpu_arena& arena = reusable_gpu_arena();
    const std::uint32_t num_blocks = bioseq::common::ceil_div(mc, k_prefix_scan_block_size);
    const std::uint64_t min_words_bytes =
        inputs.device_minimizer_words == nullptr
            ? std::max<std::uint64_t>(minimizer_device_bytes(inputs.minimizer_word_kind,
                                                             inputs.minimizer_words32_per_record,
                                                             mc),
                                      4u)
            : 0u;
    std::uint64_t dt = std::uint64_t{mc} * 4u * 2u + std::uint64_t{num_blocks} * 4u + 16u +
                       min_words_bytes + 6u * std::uint64_t{k_gpu_buffer_offset_alignment};
    if (!arena.ensure_capacity(dt, 0, 0)) {
        throw vulkan_runtime_error("gpu seed-to-coordinate arena grow failed");
    }
    const gpu_arena_mark mark = arena.mark();

    const gpu_arena_slice counts_s = arena.allocate_device(std::uint64_t{mc} * 4u, 16u);
    const gpu_arena_slice prefix_s = arena.allocate_device(std::uint64_t{mc} * 4u, 16u);
    const gpu_arena_slice blocks_s =
        arena.allocate_device(std::max<std::uint64_t>(num_blocks, 1u) * 4u, 16u);
    const gpu_arena_slice qrec_s = arena.allocate_device(16u, 16u);
    gpu_arena_slice minw_s{};
    if (inputs.device_minimizer_words == nullptr) {
        minw_s = arena.allocate_device(min_words_bytes, 16u);
    }
    if (!counts_s.is_valid() || !prefix_s.is_valid() || !blocks_s.is_valid() ||
        !qrec_s.is_valid() || (inputs.device_minimizer_words == nullptr && !minw_s.is_valid())) {
        arena.rewind(mark);
        throw vulkan_runtime_error("gpu seed-to-coordinate arena oversubscribed");
    }

    auto* base = static_cast<std::byte*>(arena.device_buffer().mapped);
    const VkBuffer min_words_buffer = inputs.device_minimizer_words != nullptr
                                          ? inputs.device_minimizer_words->buffer_handle
                                          : minw_s.buffer->buffer_handle;
    const std::uint64_t min_words_offset =
        inputs.device_minimizer_words != nullptr ? 0u : minw_s.offset;
    const std::uint64_t min_words_range =
        inputs.device_minimizer_words != nullptr ? VK_WHOLE_SIZE : minw_s.size;
    if (inputs.device_minimizer_words == nullptr) {
        std::memset(base + minw_s.offset, 0, static_cast<std::size_t>(minw_s.size));
        if (!inputs.host_minimizer_bytes.empty()) {
            std::memcpy(base + minw_s.offset,
                        inputs.host_minimizer_bytes.data(),
                        inputs.host_minimizer_bytes.size());
        }
    }
    const host_query_record qrec{0u, mc, static_cast<std::uint32_t>(query_len), 0u};
    std::memcpy(base + qrec_s.offset, &qrec, sizeof(qrec));

    const compute_pipeline count_pipe =
        get_compute_pipeline_locked(shader_kind::seed_count, 128u, 1u);
    const compute_pipeline emit_pipe =
        get_compute_pipeline_locked(shader_kind::seed_emit, 128u, 1u);
    const compute_pipeline scanb =
        get_compute_pipeline_locked(shader_kind::prefix_scan_block, 1u, 1u);
    const compute_pipeline scana =
        get_compute_pipeline_locked(shader_kind::prefix_scan_add, 128u, 1u);
    const compute_pipeline range_pipe =
        get_compute_pipeline_locked(shader_kind::chain_range_build, 1u, 1u);
    const compute_pipeline coord_pipe =
        get_compute_pipeline_locked(shader_kind::coordinate_reduce, 128u, 1u);
    const VkPipeline chain_pipeline = create_pipeline_locked(chain_key).pipeline;

    const std::uint32_t target_offset_lo =
        static_cast<std::uint32_t>(inputs.target_position_offset & 0xffffffffull);
    const std::uint32_t target_offset_hi =
        static_cast<std::uint32_t>(inputs.target_position_offset >> 32);
    const gpu_anchor_sort_key_plan anchor_sort_plan = make_anchor_sort_key_plan(
        inputs.target_len != 0u ? inputs.target_len : target_len, inputs.target_position_offset);
    seed_push_constants cpush{inputs.minimizer_words32_per_record,
                              inputs.minimizer_word_kind,
                              inputs.hash_shift,
                              inputs.pos_bits,
                              kmer_size,
                              inputs.slot_mask,
                              static_cast<std::uint32_t>(max_occ),
                              0u,
                              1u,
                              inputs.bucket_count,
                              target_offset_lo,
                              target_offset_hi,
                              anchor_sort_plan.target_ref_bits};
    const std::uint32_t seed_groups = std::max<std::uint32_t>(
        1u, std::min(bioseq::common::ceil_div(mc, 128u), features_.max_workgroup_count_x));

    {
        reset_dispatch_descriptor_pool_locked();
        const std::array<VkDescriptorBufferInfo, 8> seed_infos{{
            {min_words_buffer, min_words_offset, min_words_range},
            {inputs.slots->buffer_handle, 0, VK_WHOLE_SIZE},
            {inputs.positions->buffer_handle, 0, VK_WHOLE_SIZE},
            {qrec_s.buffer->buffer_handle, qrec_s.offset, qrec_s.size},
            {counts_s.buffer->buffer_handle, counts_s.offset, counts_s.size},
            {prefix_s.buffer->buffer_handle, prefix_s.offset, prefix_s.size},
            {counts_s.buffer->buffer_handle, counts_s.offset, counts_s.size},
            {counts_s.buffer->buffer_handle, counts_s.offset, counts_s.size},
        }};
        const std::array<VkDescriptorBufferInfo, 3> scan_infos{{
            {counts_s.buffer->buffer_handle, counts_s.offset, counts_s.size},
            {prefix_s.buffer->buffer_handle, prefix_s.offset, prefix_s.size},
            {blocks_s.buffer->buffer_handle, blocks_s.offset, blocks_s.size},
        }};
        scan_push_constants s0{mc, k_prefix_scan_block_size, num_blocks, 0u};
        scan_push_constants s1{mc, k_prefix_scan_block_size, num_blocks, 1u};
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_seed_coord1)");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_seed_coord1)");
        record_host_to_shader(command_buffer_);
        record_compute_dispatch_locked(
            command_buffer_, count_pipe, seed_infos, &cpush, sizeof(cpush), seed_groups);
        record_compute_barrier(command_buffer_);
        record_compute_dispatch_locked(
            command_buffer_, scanb, scan_infos, &s0, sizeof(s0), num_blocks);
        record_compute_barrier(command_buffer_);
        record_compute_dispatch_locked(command_buffer_, scana, scan_infos, &s0, sizeof(s0), 1u);
        record_compute_barrier(command_buffer_);
        record_compute_dispatch_locked(command_buffer_,
                                       scana,
                                       scan_infos,
                                       &s1,
                                       sizeof(s1),
                                       bioseq::common::ceil_div(mc, 128u));
        record_shader_to_host(command_buffer_);
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(pc_seed_coord1)");
    }

    std::uint32_t plast = 0, clast = 0;
    std::memcpy(&plast, base + prefix_s.offset + (mc - 1u) * 4u, 4);
    std::memcpy(&clast, base + counts_s.offset + (mc - 1u) * 4u, 4);
    const std::uint64_t total = static_cast<std::uint64_t>(plast) + clast;
    if (total == 0u) {
        arena.rewind(mark);
        return result;
    }
    if (total > std::numeric_limits<std::uint32_t>::max()) {
        arena.rewind(mark);
        throw vulkan_runtime_error("gpu seed-to-coordinate anchor count overflow");
    }
    const auto total_u32 = static_cast<std::uint32_t>(total);

    const std::uint64_t anchor_u64 = total * 3u;
    const workgroup_plan awg = plan_workgroups(total, features_.max_workgroup_count_x);
    buffer anchors_buf = create_device_buffer(anchor_u64 * 8u, k_compute_buffer_usage);
    buffer akeys_buf = create_device_buffer(total * 8u, k_compute_buffer_usage);
    buffer anchors_scr_buf = create_device_buffer(anchor_u64 * 8u, k_compute_buffer_usage);
    buffer akeys_scr_buf = create_device_buffer(total * 8u, k_compute_buffer_usage);
    buffer ahist_buf =
        create_device_buffer(std::uint64_t{awg.num_workgroups} * 256u * 4u, k_compute_buffer_usage);
    buffer ranges_buf = create_device_buffer(
        std::max<std::uint64_t>(total * sizeof(gpu_chain_range_wire), 8u), k_compute_buffer_usage);
    buffer range_count_buf = create_device_buffer(8u, k_compute_buffer_usage);
    buffer compacted_anchors_buf = create_device_buffer(anchor_u64 * 8u, k_compute_buffer_usage);
    buffer span_buf =
        create_device_buffer(sizeof(gpu_chain_range_build_span_wire), k_compute_buffer_usage);
    if (anchors_buf.mapped == nullptr || ranges_buf.mapped == nullptr ||
        range_count_buf.mapped == nullptr || compacted_anchors_buf.mapped == nullptr ||
        span_buf.mapped == nullptr) {
        arena.rewind(mark);
        throw vulkan_runtime_error("gpu seed-to-coordinate non-unified memory");
    }
    const gpu_arena_slice anchors_s{&anchors_buf, 0, anchors_buf.bytes};
    const gpu_arena_slice akeys_s{&akeys_buf, 0, akeys_buf.bytes};
    const gpu_arena_slice anchors_scr_s{&anchors_scr_buf, 0, anchors_scr_buf.bytes};
    const gpu_arena_slice akeys_scr_s{&akeys_scr_buf, 0, akeys_scr_buf.bytes};
    const gpu_arena_slice ahist_s{&ahist_buf, 0, ahist_buf.bytes};
    const gpu_arena_slice compacted_anchors_s{
        &compacted_anchors_buf, 0, compacted_anchors_buf.bytes};

    const gpu_chain_range_build_span_wire span{
        .anchor_lo = 0u,
        .anchor_hi = total_u32,
        .range_lo = 0u,
        .range_out = 0u,
    };
    std::memcpy(span_buf.mapped, &span, sizeof(span));
    std::memset(range_count_buf.mapped, 0, 8u);

    bool sorted_in_scratch = false;
    {
        reset_dispatch_descriptor_pool_locked();
        seed_push_constants epush = cpush;
        epush.emit_mode = 1u;
        const std::array<VkDescriptorBufferInfo, 8> seed_infos{{
            {min_words_buffer, min_words_offset, min_words_range},
            {inputs.slots->buffer_handle, 0, VK_WHOLE_SIZE},
            {inputs.positions->buffer_handle, 0, VK_WHOLE_SIZE},
            {qrec_s.buffer->buffer_handle, qrec_s.offset, qrec_s.size},
            {counts_s.buffer->buffer_handle, counts_s.offset, counts_s.size},
            {prefix_s.buffer->buffer_handle, prefix_s.offset, prefix_s.size},
            {anchors_s.buffer->buffer_handle, anchors_s.offset, anchors_s.size},
            {akeys_s.buffer->buffer_handle, akeys_s.offset, akeys_s.size},
        }};
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_seed_coord2)");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_seed_coord2)");
        record_host_to_shader(command_buffer_);
        record_compute_dispatch_locked(
            command_buffer_, emit_pipe, seed_infos, &epush, sizeof(epush), seed_groups);
        record_compute_barrier(command_buffer_);
        gpu_sort_plan sort_plan{total, 8u, 24u, 8u, anchor_sort_plan.pass_count};
        record_radix_sort_locked(command_buffer_,
                                 akeys_s,
                                 anchors_s,
                                 akeys_scr_s,
                                 anchors_scr_s,
                                 ahist_s,
                                 sort_plan,
                                 awg.num_workgroups,
                                 awg.elems_per_wg,
                                 3u,
                                 sorted_in_scratch);
        const gpu_arena_slice& sorted_anchors = sorted_in_scratch ? anchors_scr_s : anchors_s;
        const std::array<VkDescriptorBufferInfo, 6> range_infos{{
            {sorted_anchors.buffer->buffer_handle, sorted_anchors.offset, sorted_anchors.size},
            {range_count_buf.buffer_handle, 0, range_count_buf.bytes},
            {ranges_buf.buffer_handle, 0, ranges_buf.bytes},
            {range_count_buf.buffer_handle, 0, range_count_buf.bytes},
            {compacted_anchors_s.buffer->buffer_handle,
             compacted_anchors_s.offset,
             compacted_anchors_s.size},
            {span_buf.buffer_handle, 0, span_buf.bytes},
        }};
        chain_range_push_constants rpush{
            .query_count = 1u,
            .max_ref_gap = static_cast<std::uint32_t>(std::max<std::int32_t>(max_dist_x, 0)),
        };
        record_compute_dispatch_locked(
            command_buffer_, range_pipe, range_infos, &rpush, sizeof(rpush), 1u);
        record_shader_to_host(command_buffer_);
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(pc_seed_coord2)");
    }

    std::uint32_t range_count = 0;
    std::uint32_t compacted_anchor_count = 0;
    std::memcpy(&range_count, range_count_buf.mapped, 4u);
    std::memcpy(&compacted_anchor_count, static_cast<std::byte*>(range_count_buf.mapped) + 4u, 4u);
    if (range_count == 0u) {
        arena.rewind(mark);
        return result;
    }
    if (compacted_anchor_count > total_u32) {
        arena.rewind(mark);
        throw vulkan_runtime_error("gpu seed-to-coordinate compacted anchor overflow");
    }
    const bool use_compacted_anchors =
        compacted_anchor_count != 0u && compacted_anchor_count < total_u32;
    const std::uint32_t active_anchor_count =
        use_compacted_anchors ? compacted_anchor_count : total_u32;

    buffer score_buf = create_device_buffer(
        std::uint64_t{active_anchor_count} * sizeof(std::int32_t), k_compute_buffer_usage);
    buffer pred_buf = create_device_buffer(
        std::uint64_t{active_anchor_count} * sizeof(std::int64_t), k_compute_buffer_usage);
    buffer visited_buf = create_device_buffer(
        std::uint64_t{active_anchor_count} * sizeof(std::uint32_t), k_compute_buffer_usage);
    buffer trace_buf = create_device_buffer(
        std::uint64_t{active_anchor_count} * sizeof(std::uint32_t), k_compute_buffer_usage);
    buffer result_buf =
        create_device_buffer(sizeof(gpu_coordinate_result_wire), k_compute_buffer_usage);
    const gpu_arena_slice& sorted_anchors = sorted_in_scratch ? anchors_scr_s : anchors_s;
    const gpu_arena_slice& active_anchors =
        use_compacted_anchors ? compacted_anchors_s : sorted_anchors;

    {
        reset_dispatch_descriptor_pool_locked();
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_seed_coord3)");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_seed_coord3)");

        std::memset(score_buf.mapped, 0, static_cast<std::size_t>(score_buf.bytes));
        std::memset(pred_buf.mapped, 0xff, static_cast<std::size_t>(pred_buf.bytes));
        std::memset(visited_buf.mapped, 0, static_cast<std::size_t>(visited_buf.bytes));
        std::memset(result_buf.mapped, 0, static_cast<std::size_t>(result_buf.bytes));
        record_host_to_shader(command_buffer_);

        const std::array<VkDescriptorBufferInfo, 4> chain_infos{{
            {active_anchors.buffer->buffer_handle, active_anchors.offset, active_anchors.size},
            {ranges_buf.buffer_handle, 0, ranges_buf.bytes},
            {score_buf.buffer_handle, 0, score_buf.bytes},
            {pred_buf.buffer_handle, 0, pred_buf.bytes},
        }};
        VkDescriptorSet chain_set = allocate_dispatch_descriptor_set_locked(descriptor_set_layout_);
        std::array<VkWriteDescriptorSet, 4> writes{};
        for (std::uint32_t binding_index = 0; binding_index < 4; ++binding_index) {
            writes[binding_index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[binding_index].dstSet = chain_set;
            writes[binding_index].dstBinding = binding_index;
            writes[binding_index].descriptorCount = 1;
            writes[binding_index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[binding_index].pBufferInfo = &chain_infos[binding_index];
        }
        vkUpdateDescriptorSets(
            device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);

        chaining_push_constants chain_push{};
        chain_push.anchor_count = active_anchor_count;
        chain_push.range_count = range_count;
        chain_push.max_dist_x = max_dist_x;
        chain_push.max_dist_y = max_dist_y;
        chain_push.bandwidth = bandwidth;
        chain_push.max_iter = max_iter;
        chain_push.gap_penalty = gap_penalty;
        chain_push.skip_penalty = skip_penalty;
        chain_push.max_chain_skip = max_chain_skip;
        const std::uint32_t chain_groups = std::min(range_count, features_.max_workgroup_count_x);

        vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, chain_pipeline);
        vkCmdBindDescriptorSets(command_buffer_,
                                VK_PIPELINE_BIND_POINT_COMPUTE,
                                pipeline_layout_,
                                0,
                                1,
                                &chain_set,
                                0,
                                nullptr);
        vkCmdPushConstants(command_buffer_,
                           pipeline_layout_,
                           VK_SHADER_STAGE_COMPUTE_BIT,
                           0,
                           sizeof(chain_push),
                           &chain_push);
        vkCmdDispatch(command_buffer_, chain_groups, 1, 1);
        record_compute_barrier(command_buffer_);

        const std::array<VkDescriptorBufferInfo, 6> coord_infos{{
            {active_anchors.buffer->buffer_handle, active_anchors.offset, active_anchors.size},
            {score_buf.buffer_handle, 0, score_buf.bytes},
            {pred_buf.buffer_handle, 0, pred_buf.bytes},
            {visited_buf.buffer_handle, 0, visited_buf.bytes},
            {trace_buf.buffer_handle, 0, trace_buf.bytes},
            {result_buf.buffer_handle, 0, result_buf.bytes},
        }};
        gpu_coordinate_reduce_push coord_push{
            .anchor_count = active_anchor_count,
            .query_len_lo = static_cast<std::uint32_t>(query_len & 0xffffffffull),
            .query_len_hi = static_cast<std::uint32_t>(query_len >> 32),
            .min_chain_score = min_chain_score,
            .min_seed_count = min_seed_count,
            .probe_limit = probe_limit,
            .reserved = 0u,
        };
        record_compute_dispatch_locked(
            command_buffer_, coord_pipe, coord_infos, &coord_push, sizeof(coord_push), 1u);
        record_shader_to_host(command_buffer_);
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(pc_seed_coord3)");
    }

    std::memcpy(&result, result_buf.mapped, sizeof(result));
    arena.rewind(mark);
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_seed_coord_post)");
    chaining_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    return result;
}

} // namespace bioseq::gpu::vulkan
