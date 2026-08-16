/**
 * @file arena.cppm
 * @brief Owns reusable GPU arena allocation, descriptor sets, and recorded dispatch helpers.
 */
module;

#include <cstdint>
#include <libassert/assert.hpp>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

module bioseq.gpu.vulkan.runtime:arena;

import :frontend_types;
import :support;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.parallelism.memory_admission;
import bioseq.instrumentation;
import std;

namespace bioseq::gpu::vulkan {

/// @brief Computes count-submit and pessimistic output admission for a sketch batch.
gpu_sketch_batch_allocation_requirements
runtime::sketch_batch_allocation_requirements(std::uint64_t packed_sequence_bytes,
                                              std::uint64_t tile_count,
                                              std::uint32_t minimizer_word_kind,
                                              std::uint32_t minimizer_words32,
                                              std::uint64_t max_minimizers) {
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw vulkan_runtime_error("sketch_batch_allocation_requirements on unavailable device");
    }
    const bool minimizer_shape_valid = (minimizer_word_kind == 0u && minimizer_words32 == 0u) ||
                                       (minimizer_word_kind == 1u && minimizer_words32 == 1u) ||
                                       (minimizer_word_kind == 2u && minimizer_words32 == 2u) ||
                                       (minimizer_word_kind == 3u && minimizer_words32 == 4u);
    const gpu_sketch_batch_arena_request arena_request =
        make_sketch_batch_arena_request(packed_sequence_bytes, tile_count);
    if (!minimizer_shape_valid || !arena_request.valid || max_minimizers == 0u ||
        max_minimizers > static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)())) {
        return {};
    }
    const std::uint64_t output_requested_bytes = std::max<std::uint64_t>(
        minimizer_device_bytes(minimizer_word_kind, minimizer_words32, max_minimizers), 4u);
    const std::uint64_t arena_requested_allocation =
        buffer_memory_requirement_locked(arena_request.requested_bytes, k_gpu_arena_usage);
    const std::uint64_t current_arena_allocation =
        gpu_arena_ ? gpu_arena_->device_buffer().allocation_bytes : 0u;
    return {
        .valid = true,
        .arena_requested_bytes = arena_request.requested_bytes,
        .arena_allocation_bytes = std::max(arena_requested_allocation, current_arena_allocation),
        .pessimistic_output_requested_bytes = output_requested_bytes,
        .pessimistic_output_allocation_bytes =
            buffer_memory_requirement_locked(output_requested_bytes, k_compute_buffer_usage),
    };
}

/// @brief Generates packed sequence units through the dedicated Vulkan kernel.
buffer runtime::run_generate_units_roundtrip(std::uint32_t word_bits,
                                             std::span<const std::uint32_t> cdf,
                                             generate_units_push push,
                                             std::uint64_t out_block_bytes) {
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw vulkan_runtime_error("run_generate_units_roundtrip on unavailable device");
    }

    constexpr std::uint32_t local = 256u;
    const compute_pipeline pipe =
        get_compute_pipeline_locked(shader_kind::generate_units, local, word_bits);

    buffer out =
        create_device_buffer(std::max<std::uint64_t>(out_block_bytes, 4u), k_compute_buffer_usage);
    buffer cdf_buf = create_device_buffer(
        std::max<std::uint64_t>(static_cast<std::uint64_t>(cdf.size()) * sizeof(std::uint32_t), 4u),
        k_compute_buffer_usage);
    buffer scratch = create_host_buffer(4u, k_compute_buffer_usage);
    upload_locked(scratch, cdf_buf, std::as_bytes(cdf), VK_ACCESS_SHADER_READ_BIT);

    const std::array<VkBuffer, 2> buffers{out.buffer_handle, cdf_buf.buffer_handle};
    bind_compute_pipeline_descriptors_locked(pipe, buffers);

    const std::uint64_t max_groups = features_.max_workgroup_count_x;
    const std::uint64_t tile_chunks = std::max<std::uint64_t>(1, max_groups * local);

    check_vk(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer(gen)");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "vkBeginCommandBuffer(gen)");
    record_host_to_shader(command_buffer_);
    vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pipe.pipeline);
    vkCmdBindDescriptorSets(command_buffer_,
                            VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipe.pipeline_layout,
                            0,
                            1,
                            &pipe.descriptor_set,
                            0,
                            nullptr);
    for (std::uint64_t base = 0; base < push.total_chunks; base += tile_chunks) {
        push.chunk_base = static_cast<std::uint32_t>(base);
        const std::uint64_t chunk_count =
            std::min<std::uint64_t>(tile_chunks, push.total_chunks - base);
        const std::uint32_t groups =
            static_cast<std::uint32_t>((chunk_count + local - 1) / local);
        vkCmdPushConstants(command_buffer_,
                           pipe.pipeline_layout,
                           VK_SHADER_STAGE_COMPUTE_BIT,
                           0,
                           sizeof(push),
                           &push);
        vkCmdDispatch(command_buffer_, groups, 1, 1);
    }
    record_shader_to_host(command_buffer_);
    check_vk(vkEndCommandBuffer(command_buffer_), "vkEndCommandBuffer(gen)");

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffer_;
    check_vk(vkQueueSubmit(queue_, 1, &submit, fence_), "vkQueueSubmit(gen)");
    check_vk(vkWaitForFences(device_,
                             1,
                             &fence_,
                             VK_TRUE,
                             std::numeric_limits<std::uint64_t>::max()),
             "vkWaitForFences(gen)");
    require_immutable_metal_policy_live("generate operation completion");
    check_vk(vkResetFences(device_, 1, &fence_), "vkResetFences(gen)");
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer(gen-post)");
    if (push.total_chunks != 0) {
        generate_units_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    }
    return out;
}

/// @brief Copies one validated allocation slice into caller-owned host storage.
void runtime::download_slice(const buffer& src,
                             std::uint64_t src_byte_offset,
                             std::span<std::byte> dst) {
    if (dst.empty()) {
        return;
    }
    if (src.mapped == nullptr) {
        throw vulkan_runtime_error("download_slice: device buffer not host-visible");
    }
    if (src_byte_offset > src.bytes ||
        dst.size() > static_cast<std::size_t>(src.bytes - src_byte_offset)) {
        throw vulkan_runtime_error("download_slice: out of range");
    }
    DEBUG_ASSERT(src_byte_offset <= src.bytes && dst.size() <= src.bytes - src_byte_offset,
                 "download_slice: out of range");
    std::memcpy(
        dst.data(), static_cast<const std::byte*>(src.mapped) + src_byte_offset, dst.size());
}

// --- Arena -----------------------------------------------------------------

/// @brief Returns the lazily constructed arena owned by this runtime.
gpu_arena& runtime::reusable_gpu_arena() {
    if (!gpu_arena_) {
        gpu_arena_.emplace(*this);
    }
    return *gpu_arena_;
}

/// @brief Grows all arena classes before a paired submission begins.
bool gpu_arena::ensure_capacity(std::uint64_t device_bytes,
                                std::uint64_t upload_bytes,
                                std::uint64_t download_bytes) {
    // Only called at op start (cursors already at their rewound baseline). Grows
    // a buffer geometrically when needed and resets just that buffer's cursor;
    // never grows mid-operation while slices are live.
    try {
        if (device_bytes > device_capacity_) {
            // Operation starts only from a rewound mark. Release the obsolete
            // arena before allocating its exact replacement so growth never
            // creates an unadmitted overlap between prior and replacement
            // allocations or a geometric 2x tail.
            device_.reset();
            device_capacity_ = 0u;
            device_ = runtime_->create_device_buffer(device_bytes, k_gpu_arena_usage);
            if (device_.mapped == nullptr)
                return false; // GPU frontend needs unified memory.
            device_capacity_ = device_.bytes;
            device_cursor_ = 0;
        }
        if (upload_bytes > upload_capacity_) {
            upload_.reset();
            upload_capacity_ = 0u;
            upload_ = runtime_->create_host_buffer(upload_bytes,
                                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            upload_capacity_ = upload_.bytes;
            upload_cursor_ = 0;
        }
        if (download_bytes > download_capacity_) {
            download_.reset();
            download_capacity_ = 0u;
            download_ = runtime_->create_host_buffer(download_bytes,
                                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            download_capacity_ = download_.bytes;
            download_cursor_ = 0;
        }
    } catch (const vulkan_runtime_error&) {
        return false;
    }
    return true;
}

/// @brief Releases every backing buffer after all live slices have rewound.
bool gpu_arena::release_idle_capacity() noexcept {
    if (device_cursor_ != 0u || upload_cursor_ != 0u || download_cursor_ != 0u) {
        return false;
    }
    device_.reset();
    upload_.reset();
    download_.reset();
    device_capacity_ = 0u;
    upload_capacity_ = 0u;
    download_capacity_ = 0u;
    return true;
}

/// @brief Bump-allocates one aligned device slice.
gpu_arena_slice gpu_arena::allocate_device(std::uint64_t bytes, std::uint64_t alignment) {
    const std::uint64_t effective_alignment =
        std::max<std::uint64_t>(alignment, k_gpu_buffer_offset_alignment);
    const std::uint64_t offset = align_up(device_cursor_, effective_alignment);
    if (offset + bytes > device_capacity_) {
        return gpu_arena_slice{}; // caller checks .is_valid()
    }
    device_cursor_ = offset + bytes;
    return gpu_arena_slice{&device_, offset, bytes};
}

/// @brief Bump-allocates one aligned upload slice.
gpu_arena_slice gpu_arena::allocate_upload(std::uint64_t bytes, std::uint64_t alignment) {
    const std::uint64_t offset = align_up(upload_cursor_, std::max<std::uint64_t>(alignment, 16u));
    if (offset + bytes > upload_capacity_)
        return gpu_arena_slice{};
    upload_cursor_ = offset + bytes;
    return gpu_arena_slice{&upload_, offset, bytes};
}

/// @brief Bump-allocates one aligned download slice.
gpu_arena_slice gpu_arena::allocate_download(std::uint64_t bytes, std::uint64_t alignment) {
    const std::uint64_t offset =
        align_up(download_cursor_, std::max<std::uint64_t>(alignment, 16u));
    if (offset + bytes > download_capacity_)
        return gpu_arena_slice{};
    download_cursor_ = offset + bytes;
    return gpu_arena_slice{&download_, offset, bytes};
}

/// @brief Binds exactly one arena slice without widening its descriptor range.
void gpu_arena::bind_storage_slice(VkWriteDescriptorSet& write,
                                   VkDescriptorBufferInfo& info,
                                   std::uint32_t binding,
                                   VkDescriptorSet dst_set,
                                   const gpu_arena_slice& slice) const noexcept {
    info = VkDescriptorBufferInfo{slice.buffer->buffer_handle, slice.offset, slice.size};
    write = VkWriteDescriptorSet{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = dst_set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &info;
}

// --- GPU frontend descriptor pool + dispatch recorder ---------------------------

/// @brief Creates the fresh-set descriptor pool on first use.
void runtime::ensure_dispatch_descriptor_pool_locked() {
    if (dispatch_descriptor_pool_ != VK_NULL_HANDLE)
        return;
    VkDescriptorPoolSize size{};
    size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    size.descriptorCount = 4096u * 9u;
    VkDescriptorPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.maxSets = 4096u;
    info.poolSizeCount = 1;
    info.pPoolSizes = &size;
    check_vk(vkCreateDescriptorPool(device_, &info, nullptr, &dispatch_descriptor_pool_),
             "vkCreateDescriptorPool(gpu)");
}

/// @brief Resets the fresh-set pool between complete frontend operations.
void runtime::reset_dispatch_descriptor_pool_locked() {
    ensure_dispatch_descriptor_pool_locked();
    check_vk(vkResetDescriptorPool(device_, dispatch_descriptor_pool_, 0),
             "vkResetDescriptorPool(gpu)");
}

/// @brief Allocates one descriptor set whose bindings remain immutable through submission.
VkDescriptorSet runtime::allocate_dispatch_descriptor_set_locked(VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    info.descriptorPool = dispatch_descriptor_pool_;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    check_vk(vkAllocateDescriptorSets(device_, &info, &set), "vkAllocateDescriptorSets(gpu)");
    return set;
}

/// @brief Updates a reusable descriptor set from explicit buffer-offset-range triples.
void runtime::bind_descriptors_offsets_locked(const compute_pipeline& entry,
                                              std::span<const VkDescriptorBufferInfo> infos) {
    std::vector<VkWriteDescriptorSet> writes(infos.size());
    for (std::size_t binding_index = 0; binding_index < infos.size(); ++binding_index) {
        writes[binding_index] = VkWriteDescriptorSet{};
        writes[binding_index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[binding_index].dstSet = entry.descriptor_set;
        writes[binding_index].dstBinding = static_cast<std::uint32_t>(binding_index);
        writes[binding_index].descriptorCount = 1;
        writes[binding_index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[binding_index].pBufferInfo = &infos[binding_index];
    }
    vkUpdateDescriptorSets(
        device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

/// @brief Records one compute dispatch and its causal receipt use.
void runtime::record_compute_dispatch_locked(VkCommandBuffer cmd,
                                             const compute_pipeline& pipe,
                                             std::span<const VkDescriptorBufferInfo> infos,
                                             const void* push,
                                             std::uint32_t push_size,
                                             std::uint32_t group_count,
                                             runtime_pipeline_use_collector* collector,
                                             std::size_t collector_job_index) {
    if (group_count == 0)
        return;
    VkDescriptorSet set = allocate_dispatch_descriptor_set_locked(pipe.set_layout);
    std::vector<VkWriteDescriptorSet> writes(infos.size());
    for (std::size_t binding_index = 0; binding_index < infos.size(); ++binding_index) {
        writes[binding_index] = VkWriteDescriptorSet{};
        writes[binding_index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[binding_index].dstSet = set;
        writes[binding_index].dstBinding = static_cast<std::uint32_t>(binding_index);
        writes[binding_index].descriptorCount = 1;
        writes[binding_index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[binding_index].pBufferInfo = &infos[binding_index];
    }
    vkUpdateDescriptorSets(
        device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe.pipeline);
    vkCmdBindDescriptorSets(
        cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe.pipeline_layout, 0, 1, &set, 0, nullptr);
    if (push_size > 0 && push != nullptr) {
        vkCmdPushConstants(
            cmd, pipe.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, push_size, push);
    }
    if (collector != nullptr) {
        collector->use(cmd,
                       collector_job_index,
                       make_shader_key(pipe.kind, 384u, pipe.local_size, pipe.max_span),
                       pipe.metal_archive_receipt,
                       group_count);
    }
    vkCmdDispatch(cmd, group_count, 1, 1);
}

/// @brief Records every histogram, scan, and scatter pass for one stable radix sort.
void runtime::record_radix_sort_locked(VkCommandBuffer cmd,
                                       const gpu_arena_slice& keys,
                                       const gpu_arena_slice& values,
                                       const gpu_arena_slice& keys_scratch,
                                       const gpu_arena_slice& values_scratch,
                                       const gpu_arena_slice& histogram,
                                       const gpu_sort_plan& plan,
                                       std::uint32_t num_workgroups,
                                       std::uint32_t elems_per_wg,
                                       std::uint32_t value_words,
                                       bool& result_in_scratch,
                                       runtime_pipeline_use_collector* collector,
                                       std::size_t collector_job_index) {
    const compute_pipeline hist_pipe =
        get_compute_pipeline_locked(shader_kind::radix_sort_histogram, 1, 1);
    const compute_pipeline scan_pipe =
        get_compute_pipeline_locked(shader_kind::radix_sort_scan, 1, 1);
    const compute_pipeline scatter_pipe =
        get_compute_pipeline_locked(shader_kind::radix_sort_scatter, 1, 1);
    if (collector != nullptr) {
        const auto prepare = [collector](const compute_pipeline& pipe) {
            collector->prepare(make_shader_key(pipe.kind, 384u, pipe.local_size, pipe.max_span),
                               pipe.metal_archive_receipt);
        };
        prepare(hist_pipe);
        prepare(scan_pipe);
        prepare(scatter_pipe);
    }

    bool in_scratch = false;
    for (std::uint32_t pass = 0; pass < plan.pass_count; ++pass) {
        const gpu_arena_slice& cur_keys = in_scratch ? keys_scratch : keys;
        const gpu_arena_slice& cur_vals = in_scratch ? values_scratch : values;
        const gpu_arena_slice& out_keys = in_scratch ? keys : keys_scratch;
        const gpu_arena_slice& out_vals = in_scratch ? values : values_scratch;

        sort_push_constants push{static_cast<std::uint32_t>(plan.element_count),
                                 pass * plan.radix_bits,
                                 num_workgroups,
                                 elems_per_wg,
                                 value_words};

        const std::array<VkDescriptorBufferInfo, 5> infos{{
            {cur_keys.buffer->buffer_handle, cur_keys.offset, cur_keys.size},
            {cur_vals.buffer->buffer_handle, cur_vals.offset, cur_vals.size},
            {out_keys.buffer->buffer_handle, out_keys.offset, out_keys.size},
            {out_vals.buffer->buffer_handle, out_vals.offset, out_vals.size},
            {histogram.buffer->buffer_handle, histogram.offset, histogram.size},
        }};
        record_compute_dispatch_locked(cmd,
                                       hist_pipe,
                                       infos,
                                       &push,
                                       sizeof(push),
                                       num_workgroups,
                                       collector,
                                       collector_job_index);
        record_compute_barrier(cmd);
        record_compute_dispatch_locked(
            cmd, scan_pipe, infos, &push, sizeof(push), 1, collector, collector_job_index);
        record_compute_barrier(cmd);
        record_compute_dispatch_locked(cmd,
                                       scatter_pipe,
                                       infos,
                                       &push,
                                       sizeof(push),
                                       num_workgroups,
                                       collector,
                                       collector_job_index);
        record_compute_barrier(cmd);
        in_scratch = !in_scratch;
    }
    result_in_scratch = in_scratch;
}

/// @brief Records stable value-first then key sorting over parallel pair buffers.
void runtime::record_stable_pair_sort_by_value_then_key_locked(VkCommandBuffer command_buffer,
                                                               sorted_pair_device_views& views,
                                                               gpu_arena_slice hist,
                                                               gpu_sort_plan value_sort_plan,
                                                               gpu_sort_plan key_sort_plan,
                                                               std::uint32_t num_workgroups,
                                                               std::uint32_t elems_per_workgroup) {
    bool value_sorted_in_scratch = false;
    record_radix_sort_locked(command_buffer,
                             views.values,
                             views.keys,
                             views.values_scratch,
                             views.keys_scratch,
                             hist,
                             value_sort_plan,
                             num_workgroups,
                             elems_per_workgroup,
                             1u,
                             value_sorted_in_scratch);

    record_compute_barrier(command_buffer);

    const gpu_arena_slice key_after_value_sort =
        value_sorted_in_scratch ? views.keys_scratch : views.keys;
    const gpu_arena_slice value_after_value_sort =
        value_sorted_in_scratch ? views.values_scratch : views.values;
    const gpu_arena_slice key_scratch_after_value_sort =
        value_sorted_in_scratch ? views.keys : views.keys_scratch;
    const gpu_arena_slice value_scratch_after_value_sort =
        value_sorted_in_scratch ? views.values : views.values_scratch;

    bool key_sorted_in_scratch = false;
    record_radix_sort_locked(command_buffer,
                             key_after_value_sort,
                             value_after_value_sort,
                             key_scratch_after_value_sort,
                             value_scratch_after_value_sort,
                             hist,
                             key_sort_plan,
                             num_workgroups,
                             elems_per_workgroup,
                             1u,
                             key_sorted_in_scratch);

    views.keys = key_after_value_sort;
    views.values = value_after_value_sort;
    views.keys_scratch = key_scratch_after_value_sort;
    views.values_scratch = value_scratch_after_value_sort;
    views.keys_in_scratch = key_sorted_in_scratch;
}

} // namespace bioseq::gpu::vulkan
