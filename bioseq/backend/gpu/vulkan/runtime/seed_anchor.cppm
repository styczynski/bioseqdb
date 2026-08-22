/**
 * @file seed_anchor.cppm
 * @brief Executes resident seed lookup, anchor emission, and canonical GPU sorting.
 */
module;

#include <cstdint>
#include <libassert/assert.hpp>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

module bioseq.gpu.vulkan.runtime:seed_anchor;

import :frontend_types;
import :support;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.parallelism.memory_admission;
import bioseq.instrumentation;
import std;

namespace bioseq::gpu::vulkan {

/// @brief Counts and emits seed hits against an already resident target index.
/// @param[in] inputs Borrowed resident index buffers and query minimizer storage.
/// @param[in] prefix_counts Exclusive per-minimizer anchor offsets plus the final total.
/// @param[in] query_len Query length used to encode anchor coordinates.
/// @param[in] kmer_size K-mer span encoded into emitted anchors.
/// @param[in] exact_anchor_count Exact output count established by the count pass.
/// @param[in] sort_plan Device-specific bitonic or radix allocation plan.
/// @param[in] expected_vulkan_allocation_bytes Exact allocation total admitted by the caller.
/// @return Canonically ordered anchors in owned host storage with dispatch statistics.
/// @throws vulkan_runtime_error When inputs, allocation accounting, dispatch, or order checks fail.
seed_lookup_result
runtime::run_seed_lookup_roundtrip(runtime_seed_lookup_inputs inputs,
                                   std::span<const std::uint32_t> prefix_counts,
                                   std::uint64_t query_len,
                                   std::uint32_t kmer_size,
                                   std::uint64_t exact_anchor_count,
                                   const seed_anchor_sort_plan& sort_plan,
                                   std::uint64_t expected_vulkan_allocation_bytes) {
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw vulkan_runtime_error("run_seed_lookup_roundtrip on unavailable device");
    }
    if (prefix_counts.size() < 2 || exact_anchor_count == 0) {
        if (expected_vulkan_allocation_bytes != 0u) {
            throw vulkan_runtime_error("zero-anchor seed lookup carried a Vulkan allocation plan");
        }
        seed_lookup_result empty_result{};
        empty_result.anchor_order_audit = audit_seed_anchor_canonical_order({}, 0u);
        return empty_result;
    }
    if (inputs.slots == nullptr || inputs.positions == nullptr) {
        throw vulkan_runtime_error("seed lookup missing resident index buffers");
    }
    if (static_cast<std::uint64_t>(inputs.position_count) * 8u > inputs.positions->bytes) {
        throw vulkan_runtime_error("seed lookup resident position range is malformed");
    }
    const std::uint64_t slot_count_from_bytes = inputs.slots->bytes / 16u;
    if (inputs.bucket_count != 0u) {
        if (inputs.slot_mask != 0u || inputs.bucket_count > slot_count_from_bytes) {
            throw vulkan_runtime_error(
                "seed lookup sorted-dense index representation is malformed");
        }
    } else {
        const std::uint64_t open_capacity = static_cast<std::uint64_t>(inputs.slot_mask) + 1u;
        if (inputs.slot_mask == 0u || !std::has_single_bit(open_capacity) ||
            open_capacity > slot_count_from_bytes) {
            throw vulkan_runtime_error(
                "seed lookup open-addressed index representation is malformed");
        }
    }
    const seed_anchor_sort_plan expected_sort_plan =
        make_seed_anchor_sort_plan(exact_anchor_count,
                                   inputs.target_len,
                                   inputs.target_position_offset,
                                   gpu_anchor_sort_capacity_,
                                   features_.max_workgroup_count_x,
                                   features_.max_workgroup_invocations,
                                   features_.max_workgroup_size_x,
                                   features_.max_storage_buffer_range);
    if (!sort_plan.is_valid() || sort_plan != expected_sort_plan ||
        sort_plan.backend == seed_anchor_sort_backend::none) {
        throw vulkan_runtime_error(
            "seed lookup sort plan does not match the runtime/device contract");
    }
    if (prefix_counts.back() != exact_anchor_count) {
        throw vulkan_runtime_error("seed lookup prefix total disagrees with anchor count");
    }
    const std::uint64_t minimizer_count = prefix_counts.size() - 1u;
    constexpr std::uint32_t seed_local = 128u;
    const std::uint64_t seed_groups = (minimizer_count + seed_local - 1u) / seed_local;
    if (seed_groups > features_.max_workgroup_count_x) {
        throw vulkan_runtime_error("seed lookup dispatch exceeds maxComputeWorkGroupCount[0]");
    }

    const compute_pipeline seed_pipe =
        get_compute_pipeline_locked(shader_kind::seed_lookup_anchors, seed_local, 256u);

    buffer scratch = create_host_buffer(4u, k_compute_buffer_usage);

    buffer uploaded_min_words;
    const buffer* min_words = inputs.device_minimizer_words;
    if (min_words == nullptr) {
        const std::uint64_t min_bytes = std::max<std::uint64_t>(
            minimizer_device_bytes(
                inputs.minimizer_word_kind, inputs.minimizer_words32_per_record, minimizer_count),
            4u);
        uploaded_min_words = create_device_buffer(min_bytes, k_compute_buffer_usage);
        if (uploaded_min_words.mapped != nullptr) {
            std::memset(
                uploaded_min_words.mapped, 0, static_cast<std::size_t>(uploaded_min_words.bytes));
        }
        if (!inputs.host_minimizer_bytes.empty()) {
            upload_locked(scratch,
                          uploaded_min_words,
                          inputs.host_minimizer_bytes,
                          VK_ACCESS_SHADER_READ_BIT);
        }
        min_words = &uploaded_min_words;
    }

    buffer prefix_buf = create_device_buffer(
        std::max<std::uint64_t>(prefix_counts.size() * sizeof(std::uint32_t), 4u),
        k_compute_buffer_usage);
    upload_locked(scratch, prefix_buf, std::as_bytes(prefix_counts), VK_ACCESS_SHADER_READ_BIT);

    const std::uint64_t anchor_bytes = sort_plan.anchor_bytes;
    buffer anchors_buf = create_device_buffer(anchor_bytes, k_compute_buffer_usage);
    buffer anchor_keys_buf = create_device_buffer(sort_plan.key_bytes, k_compute_buffer_usage);
    buffer ranges_buf;
    std::optional<compute_pipeline> sort_pipe;
    buffer anchors_scratch_buf;
    buffer anchor_keys_scratch_buf;
    buffer histogram_buf;
    if (sort_plan.backend == seed_anchor_sort_backend::bitonic) {
        ranges_buf = create_device_buffer(sizeof(sort_range_wire), k_compute_buffer_usage);
        const sort_range_wire range{0u, static_cast<std::uint32_t>(exact_anchor_count)};
        upload_locked(
            scratch,
            ranges_buf,
            std::span<const std::byte>{reinterpret_cast<const std::byte*>(&range), sizeof(range)},
            VK_ACCESS_SHADER_READ_BIT);
        sort_pipe = get_compute_pipeline_locked(
            shader_kind::sort_anchors_bitonic, gpu_anchor_sort_capacity_, 256u);
    } else {
        anchors_scratch_buf = create_device_buffer(sort_plan.anchor_bytes, k_compute_buffer_usage);
        anchor_keys_scratch_buf = create_device_buffer(sort_plan.key_bytes, k_compute_buffer_usage);
        histogram_buf = create_device_buffer(sort_plan.histogram_bytes, k_compute_buffer_usage);
    }

    std::uint64_t actual_vulkan_allocation_bytes = 0u;
    // Accumulates the exact physical allocations retained for this operation.
    const auto add_allocation = [&actual_vulkan_allocation_bytes](const buffer& buffer) noexcept {
        actual_vulkan_allocation_bytes =
            bioseq::common::saturating_add(actual_vulkan_allocation_bytes, buffer.allocation_bytes);
    };
    add_allocation(scratch);
    add_allocation(uploaded_min_words);
    add_allocation(prefix_buf);
    add_allocation(anchors_buf);
    add_allocation(anchor_keys_buf);
    add_allocation(ranges_buf);
    add_allocation(anchors_scratch_buf);
    add_allocation(anchor_keys_scratch_buf);
    add_allocation(histogram_buf);
    if (actual_vulkan_allocation_bytes != expected_vulkan_allocation_bytes) {
        throw vulkan_runtime_error(fmt::format("seed lookup Vulkan allocation requirement mismatch "
                                               "(planned={} actual={})",
                                               expected_vulkan_allocation_bytes,
                                               actual_vulkan_allocation_bytes));
    }

    {
        const std::array<VkBuffer, 6> seed_bufs{min_words->buffer_handle,
                                                inputs.slots->buffer_handle,
                                                inputs.positions->buffer_handle,
                                                prefix_buf.buffer_handle,
                                                anchors_buf.buffer_handle,
                                                anchor_keys_buf.buffer_handle};
        bind_compute_pipeline_descriptors_locked(seed_pipe, seed_bufs);
        if (sort_plan.backend == seed_anchor_sort_backend::bitonic) {
            const std::array<VkBuffer, 2> sort_bufs{anchors_buf.buffer_handle,
                                                    ranges_buf.buffer_handle};
            bind_compute_pipeline_descriptors_locked(*sort_pipe, sort_bufs);
        }
    }

    if (sort_plan.backend == seed_anchor_sort_backend::radix) {
        reset_dispatch_descriptor_pool_locked();
    }

    check_vk(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer(seed)");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "vkBeginCommandBuffer(seed)");

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

    const seed_lookup_push_constants seed_push{static_cast<std::uint32_t>(minimizer_count),
                                               inputs.minimizer_words32_per_record,
                                               inputs.minimizer_word_kind,
                                               inputs.hash_shift,
                                               inputs.pos_bits,
                                               static_cast<std::uint32_t>(query_len),
                                               kmer_size,
                                               inputs.slot_mask,
                                               inputs.bucket_count,
                                               static_cast<std::uint32_t>(exact_anchor_count),
                                               sort_plan.target_ref_bits};
    vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, seed_pipe.pipeline);
    vkCmdBindDescriptorSets(command_buffer_,
                            VK_PIPELINE_BIND_POINT_COMPUTE,
                            seed_pipe.pipeline_layout,
                            0,
                            1,
                            &seed_pipe.descriptor_set,
                            0,
                            nullptr);
    vkCmdPushConstants(command_buffer_,
                       seed_pipe.pipeline_layout,
                       VK_SHADER_STAGE_COMPUTE_BIT,
                       0,
                       sizeof(seed_push),
                       &seed_push);
    vkCmdDispatch(command_buffer_, static_cast<std::uint32_t>(seed_groups), 1, 1);

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

    bool radix_result_in_scratch = false;
    if (sort_plan.backend == seed_anchor_sort_backend::bitonic) {
        vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, sort_pipe->pipeline);
        vkCmdBindDescriptorSets(command_buffer_,
                                VK_PIPELINE_BIND_POINT_COMPUTE,
                                sort_pipe->pipeline_layout,
                                0,
                                1,
                                &sort_pipe->descriptor_set,
                                0,
                                nullptr);
        vkCmdDispatch(command_buffer_, 1, 1, 1);
    } else {
        const gpu_arena_slice anchors{&anchors_buf, 0u, sort_plan.anchor_bytes};
        const gpu_arena_slice keys{&anchor_keys_buf, 0u, sort_plan.key_bytes};
        const gpu_arena_slice anchors_scratch{&anchors_scratch_buf, 0u, sort_plan.anchor_bytes};
        const gpu_arena_slice keys_scratch{&anchor_keys_scratch_buf, 0u, sort_plan.key_bytes};
        const gpu_arena_slice histogram{&histogram_buf, 0u, sort_plan.histogram_bytes};
        const gpu_sort_plan radix_plan{exact_anchor_count, 8u, 24u, 8u, sort_plan.radix_pass_count};
        record_radix_sort_locked(command_buffer_,
                                 keys,
                                 anchors,
                                 keys_scratch,
                                 anchors_scratch,
                                 histogram,
                                 radix_plan,
                                 sort_plan.num_workgroups,
                                 sort_plan.elements_per_workgroup,
                                 3u,
                                 radix_result_in_scratch);
        if (radix_result_in_scratch !=
            is_seed_radix_result_in_scratch(sort_plan.radix_pass_count)) {
            throw vulkan_runtime_error("seed radix sort result parity mismatch");
        }
    }

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

    check_vk(vkEndCommandBuffer(command_buffer_), "vkEndCommandBuffer(seed)");
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffer_;
    check_vk(vkQueueSubmit(queue_, 1, &submit, fence_), "vkQueueSubmit(seed)");
    check_vk(vkWaitForFences(device_,
                             1,
                             &fence_,
                             VK_TRUE,
                             std::numeric_limits<std::uint64_t>::max()),
             "vkWaitForFences(seed)");
    require_immutable_metal_policy_live("seed operation completion");
    check_vk(vkResetFences(device_, 1, &fence_), "vkResetFences(seed)");

    seed_lookup_result result;
    result.count = exact_anchor_count;
    result.actual_vulkan_allocation_bytes = actual_vulkan_allocation_bytes;
    result.seed_lookup_dispatches = 1u;
    result.open_addressed_lookup_dispatches = inputs.bucket_count == 0u ? 1u : 0u;
    result.sorted_dense_lookup_dispatches = inputs.bucket_count != 0u ? 1u : 0u;
    result.host_bytes.resize(static_cast<std::size_t>(anchor_bytes));
    const buffer& sorted_anchors = radix_result_in_scratch ? anchors_scratch_buf : anchors_buf;
    download_locked(scratch,
                    sorted_anchors,
                    std::span<std::byte>{result.host_bytes.data(), result.host_bytes.size()});
    const auto tie_normalization = normalize_seed_anchor_equal_ref_ties(
        std::span<std::byte>{result.host_bytes.data(), result.host_bytes.size()}, result.count);
    if (!tie_normalization.gpu_ref_order_checked || !tie_normalization.gpu_ref_order_accepted) {
        throw vulkan_runtime_error("seed lookup GPU result is not ordered by Anchor ref_bits");
    }
    result.canonical_tie_normalization_runs = tie_normalization.canonical_tie_run_count;
    result.canonical_tie_normalization_anchors = tie_normalization.canonical_tie_anchor_count;
    result.anchor_order_audit = audit_seed_anchor_canonical_order(
        std::span<const std::byte>{result.host_bytes.data(), result.host_bytes.size()},
        result.count);
    if (!result.anchor_order_audit.canonical_order_checked ||
        !result.anchor_order_audit.canonical_order_accepted ||
        result.anchor_order_audit.anchor_count != result.count) {
        throw vulkan_runtime_error("seed lookup result is not in canonical Anchor order");
    }

    if (sort_plan.backend == seed_anchor_sort_backend::bitonic) {
        result.sort_stats.bitonic_sort_dispatches = 1u;
    } else {
        result.sort_stats.radix_sort_operations = 1u;
        result.sort_stats.radix_anchor_count = exact_anchor_count;
        result.sort_stats.radix_passes = sort_plan.radix_pass_count;
        result.sort_stats.radix_histogram_dispatches = sort_plan.radix_pass_count;
        result.sort_stats.radix_scan_dispatches = sort_plan.radix_pass_count;
        result.sort_stats.radix_scatter_dispatches = sort_plan.radix_pass_count;
    }

    check_vk(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer(seed-post)");
    chaining_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    return result;
}

// ===========================================================================
// GPU frontend — arena + frontend implementation
// ===========================================================================

/// @brief Runs seed lookup and canonical anchor sorting for one query.
/// @param[in] inputs Borrowed resident index and query minimizer buffers.
/// @param[in] query_len Query length used to encode emitted anchors.
/// @param[in] kmer_size K-mer span represented by each seed.
/// @param[in] max_occ Maximum target occurrences emitted per minimizer.
/// @return Canonically ordered anchors in owned host storage with dispatch statistics.
/// @throws vulkan_runtime_error When validation, admission, allocation, dispatch, or order checks fail.
seed_lookup_result runtime::run_seed_anchor_roundtrip(const gpu_seed_inputs& inputs,
                                                      std::uint64_t query_len,
                                                      std::uint32_t kmer_size,
                                                      std::int32_t max_occ) {
    std::lock_guard<std::mutex> guard(mu_);
    seed_lookup_result result;
    result.anchor_order_audit = audit_seed_anchor_canonical_order({}, 0u);
    if (!features_.available)
        throw vulkan_runtime_error("run_seed_anchor_roundtrip on unavailable device");
    if (inputs.slots == nullptr || inputs.positions == nullptr) {
        throw vulkan_runtime_error("gpu seed missing resident index buffers");
    }
    const std::uint32_t mc = inputs.minimizer_count;
    gpu_arena& arena = reusable_gpu_arena();
    if (mc == 0u || max_occ < 0) {
        if (!arena.release_idle_capacity()) {
            throw vulkan_runtime_error("gpu seed idle arena release found live slices");
        }
        return result;
    }
    const std::uint32_t num_blocks = bioseq::common::ceil_div(mc, k_prefix_scan_block_size);
    const std::uint64_t min_words_bytes =
        inputs.device_minimizer_words == nullptr
            ? std::max<std::uint64_t>(minimizer_device_bytes(inputs.minimizer_word_kind,
                                                             inputs.minimizer_words32_per_record,
                                                             mc),
                                      4u)
            : 0u;
    // Each allocate_device call rounds the offset up to k_gpu_buffer_offset_alignment
    // (256), so the five sub-allocations below can need up to 5*256 bytes of
    // alignment padding on top of their byte footprint. Reserve one alignment
    // unit per boundary plus initial cursor slack so cold and reused arenas obey
    // the same capacity invariant.
    std::uint64_t dt = std::uint64_t{mc} * 4u * 2u + std::uint64_t{num_blocks} * 4u + 16u +
                       min_words_bytes + 6u * std::uint64_t{k_gpu_buffer_offset_alignment};
    if (!arena.ensure_capacity(dt, 0, 0)) {
        // ensure_capacity never advances a cursor.  Drop any partial/high-water
        // backing before reporting the failure so a retry starts cold and does
        // not inherit memory from a failed request.
        if (!arena.release_idle_capacity()) {
            throw vulkan_runtime_error("gpu seed failed grow left live arena slices");
        }
        throw vulkan_runtime_error("gpu seed arena grow failed");
    }
    const gpu_arena_mark mark = arena.mark();
    // Rewinds all per-query slices and releases the arena on every exit path.
    struct seed_arena_lifetime {
        gpu_arena& arena;
        gpu_arena_mark mark;
        bool released = false;

        /// @brief Rewinds and trims the arena on exceptional exits.
        ~seed_arena_lifetime() noexcept {
            if (!released) {
                // Exception cleanup must leave a retryable arena.  Destructors
                // cannot report the invariant failure, but every explicit
                // return below uses release_or_throw() and does report it.
                arena.rewind(mark);
                (void)arena.release_idle_capacity();
            }
        }

        /// @brief Rewinds, trims, and reports any live-slice invariant failure.
        void release_or_throw() {
            arena.rewind(mark);
            if (!arena.release_idle_capacity()) {
                throw vulkan_runtime_error("gpu seed idle arena release found live slices");
            }
            released = true;
        }
    } seed_arena_lifetime{arena, mark};

    const gpu_arena_slice counts_s = arena.allocate_device(std::uint64_t{mc} * 4u, 16u);
    const gpu_arena_slice prefix_s = arena.allocate_device(std::uint64_t{mc} * 4u, 16u);
    const gpu_arena_slice blocks_s =
        arena.allocate_device(std::max<std::uint64_t>(num_blocks, 1u) * 4u, 16u);
    const gpu_arena_slice qrec_s = arena.allocate_device(16u, 16u);
    gpu_arena_slice minw_s{};
    if (inputs.device_minimizer_words == nullptr)
        minw_s = arena.allocate_device(min_words_bytes, 16u);
    if (!counts_s.is_valid() || !prefix_s.is_valid() || !blocks_s.is_valid() ||
        !qrec_s.is_valid() || (inputs.device_minimizer_words == nullptr && !minw_s.is_valid())) {
        seed_arena_lifetime.release_or_throw();
        throw vulkan_runtime_error("gpu seed arena oversubscribed");
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

    const std::uint32_t target_offset_lo =
        static_cast<std::uint32_t>(inputs.target_position_offset & 0xffffffffull);
    const std::uint32_t target_offset_hi =
        static_cast<std::uint32_t>(inputs.target_position_offset >> 32);
    const gpu_anchor_sort_key_plan anchor_sort_plan =
        make_anchor_sort_key_plan(inputs.target_len, inputs.target_position_offset);
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

    // --- Submit 1: seed count + prefix scan ---
    {
        static const auto* _rd = bioseq::instrumentation::register_static_region("seed_count");
        auto _rs = bioseq::instrumentation::region(_rd);
        reset_dispatch_descriptor_pool_locked();
        const std::array<VkDescriptorBufferInfo, 8> seed_infos{{
            {min_words_buffer, min_words_offset, min_words_range},
            {inputs.slots->buffer_handle, 0, VK_WHOLE_SIZE},
            {inputs.positions->buffer_handle, 0, VK_WHOLE_SIZE},
            {qrec_s.buffer->buffer_handle, qrec_s.offset, qrec_s.size},
            {counts_s.buffer->buffer_handle, counts_s.offset, counts_s.size},
            {prefix_s.buffer->buffer_handle, prefix_s.offset, prefix_s.size},
            {counts_s.buffer->buffer_handle,
             counts_s.offset,
             counts_s.size}, // anchors (unused in count)
            {counts_s.buffer->buffer_handle,
             counts_s.offset,
             counts_s.size}, // anchor_keys (unused in count)
        }};
        const std::array<VkDescriptorBufferInfo, 3> scan_infos{{
            {counts_s.buffer->buffer_handle, counts_s.offset, counts_s.size},
            {prefix_s.buffer->buffer_handle, prefix_s.offset, prefix_s.size},
            {blocks_s.buffer->buffer_handle, blocks_s.offset, blocks_s.size},
        }};
        scan_push_constants s0{mc, k_prefix_scan_block_size, num_blocks, 0u};
        scan_push_constants s1{mc, k_prefix_scan_block_size, num_blocks, 1u};
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_seed1)");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_seed1)");
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
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(pc_seed1)");
    }
    result.seed_lookup_dispatches = 1u;
    result.open_addressed_lookup_dispatches = inputs.bucket_count == 0u ? 1u : 0u;
    result.sorted_dense_lookup_dispatches = inputs.bucket_count != 0u ? 1u : 0u;
    std::uint32_t plast = 0, clast = 0;
    std::memcpy(&plast, base + prefix_s.offset + (mc - 1u) * 4u, 4);
    std::memcpy(&clast, base + counts_s.offset + (mc - 1u) * 4u, 4);
    const std::uint64_t total = static_cast<std::uint64_t>(plast) + clast;
    if (total == 0u) {
        seed_arena_lifetime.release_or_throw();
        return result;
    }
    if (total > std::numeric_limits<std::uint32_t>::max()) {
        seed_arena_lifetime.release_or_throw();
        throw vulkan_runtime_error("gpu seed anchor count overflow");
    }

    // --- Submit 2: seed emit + device anchor sort by ref_bits ---
    // The exact anchor count is only known after the count submit, so the anchor
    // and sort-scratch buffers are sized exactly here as standalone device
    // buffers (the count/prefix arena slices from submit 1 must stay live, so the
    // arena is never grown mid-op). They are wrapped as offset-0 gpu_arena_slice
    // for the shared dispatch/sort recorders.
    const std::uint64_t anchor_u64 = total * 3u; // 24-byte anchors as 3 u64
    const workgroup_plan awg = plan_workgroups(total, features_.max_workgroup_count_x);
    const std::uint64_t exact_reserve = seed_anchor_reserved_bytes(total, awg.num_workgroups);
    auto exact_permit =
        bioseq::parallelism::memory_admission_controller::instance().try_reserve(exact_reserve);
    if (!exact_permit) {
        auto& admission = bioseq::parallelism::memory_admission_controller::instance();
        seed_arena_lifetime.release_or_throw();
        throw vulkan_runtime_error(
            fmt::format("gpu seed exact anchor memory admission failed for {} bytes "
                        "(anchors={} workgroups={} reserved={} allocatable={})",
                        exact_reserve,
                        total,
                        awg.num_workgroups,
                        admission.reserved_bytes(),
                        admission.allocatable_after_reservations()));
    }
    buffer anchors_buf = create_device_buffer(anchor_u64 * 8u, k_compute_buffer_usage);
    buffer akeys_buf = create_device_buffer(total * 8u, k_compute_buffer_usage);
    buffer anchors_scr_buf = create_device_buffer(anchor_u64 * 8u, k_compute_buffer_usage);
    buffer akeys_scr_buf = create_device_buffer(total * 8u, k_compute_buffer_usage);
    buffer ahist_buf =
        create_device_buffer(std::uint64_t{awg.num_workgroups} * 256u * 4u, k_compute_buffer_usage);
    if (anchors_buf.mapped == nullptr) {
        seed_arena_lifetime.release_or_throw();
        throw vulkan_runtime_error("gpu seed non-unified memory");
    }
    const gpu_arena_slice anchors_s{&anchors_buf, 0, anchors_buf.bytes};
    const gpu_arena_slice akeys_s{&akeys_buf, 0, akeys_buf.bytes};
    const gpu_arena_slice anchors_scr_s{&anchors_scr_buf, 0, anchors_scr_buf.bytes};
    const gpu_arena_slice akeys_scr_s{&akeys_scr_buf, 0, akeys_scr_buf.bytes};
    const gpu_arena_slice ahist_s{&ahist_buf, 0, ahist_buf.bytes};

    bool sorted_in_scratch = false;
    {
        static const auto* _rd = bioseq::instrumentation::register_static_region("seed_emit");
        auto _rs = bioseq::instrumentation::region(_rd);
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
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_seed2)");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_seed2)");
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
        record_shader_to_host(command_buffer_);
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(pc_seed2)");
    }
    const gpu_arena_slice& sorted_anchors = sorted_in_scratch ? anchors_scr_s : anchors_s;
    result.count = total;
    result.host_bytes.resize(static_cast<std::size_t>(total * 24u));
    std::memcpy(result.host_bytes.data(),
                static_cast<const std::byte*>(sorted_anchors.buffer->mapped) +
                    sorted_anchors.offset,
                static_cast<std::size_t>(total * 24u));
    const auto tie_normalization = normalize_seed_anchor_equal_ref_ties(
        std::span<std::byte>{result.host_bytes.data(), result.host_bytes.size()}, result.count);
    if (!tie_normalization.gpu_ref_order_checked || !tie_normalization.gpu_ref_order_accepted) {
        seed_arena_lifetime.release_or_throw();
        throw vulkan_runtime_error("gpu seed GPU result is not ordered by Anchor ref_bits");
    }
    result.canonical_tie_normalization_runs = tie_normalization.canonical_tie_run_count;
    result.canonical_tie_normalization_anchors = tie_normalization.canonical_tie_anchor_count;
    result.anchor_order_audit = audit_seed_anchor_canonical_order(
        std::span<const std::byte>{result.host_bytes.data(), result.host_bytes.size()},
        result.count);
    if (!result.anchor_order_audit.canonical_order_checked ||
        !result.anchor_order_audit.canonical_order_accepted ||
        result.anchor_order_audit.anchor_count != result.count) {
        seed_arena_lifetime.release_or_throw();
        throw vulkan_runtime_error("gpu seed result is not in canonical Anchor order");
    }
    result.sort_stats.radix_sort_operations = 1u;
    result.sort_stats.radix_anchor_count = total;
    result.sort_stats.radix_passes = anchor_sort_plan.pass_count;
    result.sort_stats.radix_histogram_dispatches = anchor_sort_plan.pass_count;
    result.sort_stats.radix_scan_dispatches = anchor_sort_plan.pass_count;
    result.sort_stats.radix_scatter_dispatches = anchor_sort_plan.pass_count;

    seed_arena_lifetime.release_or_throw();
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_seed-post)");
    chaining_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    return result;
}

} // namespace bioseq::gpu::vulkan
