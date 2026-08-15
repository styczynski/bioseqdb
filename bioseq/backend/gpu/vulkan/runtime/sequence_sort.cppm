/**
 * @file sequence_sort.cppm
 * @brief Executes dense and global Vulkan sequence-sort routes.
 */
module;

#include <cstdint>
#include <libassert/assert.hpp>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

module bioseq.gpu.vulkan.runtime:sequence_sort;

import :frontend_types;
import :support;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.parallelism.memory_admission;
import bioseq.instrumentation;
import std;

namespace bioseq::gpu::vulkan {

/// @brief Sorts a contiguous packed-sequence image through the chunk-based overload.
std::vector<std::uint32_t>
runtime::run_sequence_sort_roundtrip(std::span<const std::byte> packed_units,
                                     std::span<const sequence_descriptor_wire> descs,
                                     const sequence_sort_kernel_parameters& params) {
    const sequence_sort_upload_chunk chunk{packed_units.data(),
                                           static_cast<std::uint64_t>(packed_units.size()),
                                           static_cast<std::uint64_t>(packed_units.size())};
    return run_sequence_sort_roundtrip(
        std::span<const sequence_sort_upload_chunk>(&chunk, 1u), descs, params);
}

/// @brief Sorts borrowed packed chunks by shader keys and exact lexical tie breaks.
std::vector<std::uint32_t>
runtime::run_sequence_sort_roundtrip(std::span<const sequence_sort_upload_chunk> chunks,
                                     std::span<const sequence_descriptor_wire> descs,
                                     const sequence_sort_kernel_parameters& params) {
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw vulkan_runtime_error("run_sequence_sort_roundtrip on unavailable device");
    }
    const std::uint32_t n = static_cast<std::uint32_t>(descs.size());
    if (n == 0)
        return {};
    if (n == 1)
        return std::vector<std::uint32_t>{0u};

    std::uint64_t packed_unit_bytes = 0;
    for (const sequence_sort_upload_chunk& chunk : chunks) {
        if (chunk.byte_size > chunk.padded_byte_size) {
            throw vulkan_runtime_error("run_sequence_sort_roundtrip: invalid upload chunk padding");
        }
        if (chunk.byte_size > 0u && chunk.bytes == nullptr) {
            throw vulkan_runtime_error(
                "run_sequence_sort_roundtrip: non-empty upload chunk has null data");
        }
        if (packed_unit_bytes >
            (std::numeric_limits<std::uint64_t>::max)() - chunk.padded_byte_size) {
            throw vulkan_runtime_error("run_sequence_sort_roundtrip: upload chunks exceed uint64");
        }
        packed_unit_bytes += chunk.padded_byte_size;
    }
    seq_sort_uploaded_bytes_.fetch_add(packed_unit_bytes, std::memory_order_relaxed);

    static constexpr std::uint32_t k_sequence_sort_local_size = 256u;
    const bool dna4 =
        params.alphabet_kind == static_cast<std::uint32_t>(alphabet_kernel_kind::dna4_nt4);
    const shader_kind key_kind =
        dna4 ? shader_kind::seq_sort_extract_key_dna4 : shader_kind::seq_sort_extract_key_generic;
    const shader_kind tb_kind =
        dna4 ? shader_kind::seq_sort_tiebreak_dna4 : shader_kind::seq_sort_tiebreak_generic;
    const shader_kind gc_kind = dna4 ? shader_kind::seq_sort_global_compare_dna4
                                     : shader_kind::seq_sort_global_compare_generic;
    const shader_kind dense_kind = dna4 ? shader_kind::seq_sort_dense_scatter_dna4
                                        : shader_kind::seq_sort_dense_scatter_generic;
    const shader_kind prefix_kind =
        dna4 ? shader_kind::seq_sort_prefix_check_dna4 : shader_kind::seq_sort_prefix_check_generic;
    const compute_pipeline key_pipe =
        get_compute_pipeline_locked(key_kind, k_sequence_sort_local_size, 0u);
    const compute_pipeline tb_pipe =
        get_compute_pipeline_locked(tb_kind, k_sequence_sort_local_size, 0u);
    const compute_pipeline gc_pipe =
        get_compute_pipeline_locked(gc_kind, k_sequence_sort_local_size, 0u);
    const compute_pipeline dense_pipe =
        get_compute_pipeline_locked(dense_kind, k_sequence_sort_local_size, 0u);
    const compute_pipeline prefix_pipe =
        get_compute_pipeline_locked(prefix_kind, k_sequence_sort_local_size, 0u);
    const workgroup_plan swg = plan_workgroups(n, features_.max_workgroup_count_x);
    auto next_power_of_two = [](std::uint32_t value) -> std::uint32_t {
        if (value <= 1u)
            return 1u;
        if (value > 0x80000000u) {
            throw vulkan_runtime_error(
                "run_sequence_sort_roundtrip: batch too large for padded bitonic index");
        }
        value -= 1u;
        value |= value >> 1u;
        value |= value >> 2u;
        value |= value >> 4u;
        value |= value >> 8u;
        value |= value >> 16u;
        return value + 1u;
    };
    const std::uint32_t padded_n = next_power_of_two(n);

    // Device buffers (unified/host-coherent on Apple). seq is the concatenated,
    // 4-byte-aligned per-row unit blocks viewed as `uint seq_words[]`.
    const std::uint64_t seq_bytes = std::max<std::uint64_t>(packed_unit_bytes, 4u);
    buffer seq = create_device_buffer(seq_bytes, k_compute_buffer_usage);
    buffer descs_buf = create_device_buffer(std::uint64_t{n} * 16u, k_compute_buffer_usage);
    buffer keys = create_device_buffer(std::uint64_t{n} * 8u, k_compute_buffer_usage);
    buffer vals = create_device_buffer(std::uint64_t{n} * 8u, k_compute_buffer_usage);
    buffer keys_scr = create_device_buffer(std::uint64_t{n} * 8u, k_compute_buffer_usage);
    buffer vals_scr = create_device_buffer(std::uint64_t{n} * 8u, k_compute_buffer_usage);
    buffer hist =
        create_device_buffer(std::uint64_t{swg.num_workgroups} * 256u * 4u, k_compute_buffer_usage);
    buffer index_buf = create_device_buffer(std::uint64_t{padded_n} * 4u, k_compute_buffer_usage);
    buffer dense_status = create_device_buffer(4u, k_compute_buffer_usage);
    if (seq.mapped == nullptr || descs_buf.mapped == nullptr || keys.mapped == nullptr ||
        vals.mapped == nullptr || keys_scr.mapped == nullptr || vals_scr.mapped == nullptr ||
        hist.mapped == nullptr || index_buf.mapped == nullptr || dense_status.mapped == nullptr) {
        throw vulkan_runtime_error("run_sequence_sort_roundtrip: device buffer alloc failed");
    }
    auto* seq_base = static_cast<std::byte*>(seq.mapped);
    std::uint64_t write_offset = 0;
    for (const sequence_sort_upload_chunk& chunk : chunks) {
        if (chunk.byte_size >
            static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
            throw vulkan_runtime_error("run_sequence_sort_roundtrip: upload chunk exceeds size_t");
        }
        if (chunk.padded_byte_size - chunk.byte_size >
            static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
            throw vulkan_runtime_error(
                "run_sequence_sort_roundtrip: upload padding exceeds size_t");
        }
        const auto* bytes = static_cast<const std::byte*>(chunk.bytes);
        const std::size_t byte_count = static_cast<std::size_t>(chunk.byte_size);
        if (byte_count > 0u)
            std::memcpy(seq_base + write_offset, bytes, byte_count);
        write_offset += chunk.byte_size;
        const std::size_t pad_count =
            static_cast<std::size_t>(chunk.padded_byte_size - chunk.byte_size);
        if (pad_count > 0u)
            std::memset(seq_base + write_offset, 0, pad_count);
        write_offset += chunk.padded_byte_size - chunk.byte_size;
    }
    if (write_offset < seq_bytes)
        std::memset(seq_base + write_offset, 0, static_cast<std::size_t>(seq_bytes - write_offset));
    std::memcpy(descs_buf.mapped, descs.data(), descs.size() * sizeof(sequence_descriptor_wire));

    const gpu_arena_slice seq_s{&seq, 0, seq.bytes};
    const gpu_arena_slice descs_s{&descs_buf, 0, descs_buf.bytes};
    const gpu_arena_slice keys_s{&keys, 0, keys.bytes};
    const gpu_arena_slice vals_s{&vals, 0, vals.bytes};
    const gpu_arena_slice keys_scr_s{&keys_scr, 0, keys_scr.bytes};
    const gpu_arena_slice vals_scr_s{&vals_scr, 0, vals_scr.bytes};
    const gpu_arena_slice hist_s{&hist, 0, hist.bytes};
    const gpu_arena_slice index_s{&index_buf, 0, index_buf.bytes};
    const gpu_arena_slice dense_status_s{&dense_status, 0, dense_status.bytes};

    auto bump = [this]() { seq_sort_dispatch_count_.fetch_add(1, std::memory_order_relaxed); };

    // Dense-rank GPU fast path. Offset 0 handles short/front-key batches. The
    // tail offset handles long fixed-length common-prefix batches only when the
    // shader validates the prefix identity and the selected key is [0,n)-dense.
    auto* index_ptr = static_cast<std::uint32_t*>(index_buf.mapped);
    auto* dense_status_ptr = static_cast<std::uint32_t*>(dense_status.mapped);
    const std::uint32_t units_for_64 =
        std::max<std::uint32_t>(1u, (64u + params.unit_bits - 1u) / params.unit_bits);
    std::array<std::uint32_t, 2> dense_offsets{0u, 0u};
    std::uint32_t dense_offset_count = 1u;
    if (descs.front().unit_count > units_for_64) {
        const std::uint32_t tail_offset = descs.front().unit_count - units_for_64;
        if (tail_offset != 0u)
            dense_offsets[dense_offset_count++] = tail_offset;
    }
    for (std::uint32_t attempt = 0; attempt < dense_offset_count; ++attempt) {
        const std::uint32_t key_offset = dense_offsets[attempt];
        const std::uint64_t prefix_work =
            static_cast<std::uint64_t>(n) * static_cast<std::uint64_t>(key_offset);
        if (prefix_work > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
            continue;
        }
        std::fill(index_ptr, index_ptr + padded_n, 0xffffffffu);
        *dense_status_ptr = 0u;
        const sequence_sort_push_constants dense_push{
            n,
            params.unit_bits,
            params.bits_per_char,
            params.letters_per_unit,
            params.char_mask,
            static_cast<std::uint32_t>(params.used_storage_mask & 0xffffffffu),
            static_cast<std::uint32_t>(params.used_storage_mask >> 32),
            key_offset,
            key_offset == 0u ? 0u : 1u,
            0u};
        reset_dispatch_descriptor_pool_locked();
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(seqsort-dense)");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(seqsort-dense)");
        record_host_to_shader(command_buffer_);
        const std::array<VkDescriptorBufferInfo, 4> dense_infos{{
            {seq_s.buffer->buffer_handle, seq_s.offset, seq_s.size},
            {descs_s.buffer->buffer_handle, descs_s.offset, descs_s.size},
            {index_s.buffer->buffer_handle, index_s.offset, index_s.size},
            {dense_status_s.buffer->buffer_handle, dense_status_s.offset, dense_status_s.size},
        }};
        if (key_offset != 0u) {
            record_compute_dispatch_locked(
                command_buffer_,
                prefix_pipe,
                dense_infos,
                &dense_push,
                sizeof(dense_push),
                bioseq::common::ceil_div(static_cast<std::uint32_t>(prefix_work),
                                         k_sequence_sort_local_size));
            record_compute_barrier(command_buffer_);
        }
        record_compute_dispatch_locked(command_buffer_,
                                       dense_pipe,
                                       dense_infos,
                                       &dense_push,
                                       sizeof(dense_push),
                                       bioseq::common::ceil_div(n, k_sequence_sort_local_size));
        record_shader_to_host(command_buffer_);
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(seqsort-dense)");
        if (*dense_status_ptr == 0u &&
            std::find(index_ptr, index_ptr + n, 0xffffffffu) == index_ptr + n) {
            std::vector<std::uint32_t> perm(index_ptr, index_ptr + n);
            seq_sort_dense_scatter_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
            bump();
            return perm;
        }
    }

    const sequence_sort_push_constants key_push{
        n,
        params.unit_bits,
        params.bits_per_char,
        params.letters_per_unit,
        params.char_mask,
        static_cast<std::uint32_t>(params.used_storage_mask & 0xffffffffu),
        static_cast<std::uint32_t>(params.used_storage_mask >> 32),
        0u,
        0u,
        0u};

    // --- Submit 1: extract keys, then radix sort (key,value=row index) ---
    bool sorted_in_scratch = false;
    {
        reset_dispatch_descriptor_pool_locked();
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(seqsort1)");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(seqsort1)");
        record_host_to_shader(command_buffer_);
        const std::array<VkDescriptorBufferInfo, 4> key_infos{{
            {seq_s.buffer->buffer_handle, seq_s.offset, seq_s.size},
            {descs_s.buffer->buffer_handle, descs_s.offset, descs_s.size},
            {keys_s.buffer->buffer_handle, keys_s.offset, keys_s.size},
            {vals_s.buffer->buffer_handle, vals_s.offset, vals_s.size},
        }};
        record_compute_dispatch_locked(command_buffer_,
                                       key_pipe,
                                       key_infos,
                                       &key_push,
                                       sizeof(key_push),
                                       bioseq::common::ceil_div(n, k_sequence_sort_local_size));
        record_compute_barrier(command_buffer_);
        const gpu_sort_plan sort_plan{n, 8u, 8u, 8u, 8u}; // full 64-bit key, 8 LSD passes
        record_radix_sort_locked(command_buffer_,
                                 keys_s,
                                 vals_s,
                                 keys_scr_s,
                                 vals_scr_s,
                                 hist_s,
                                 sort_plan,
                                 swg.num_workgroups,
                                 swg.elems_per_wg,
                                 1u,
                                 sorted_in_scratch);
        record_shader_to_host(command_buffer_);
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(seqsort1)");
    }

    // Sorted keys/vals live in keys/vals or their scratch, per the ping-pong parity.
    const std::uint64_t* sorted_keys =
        static_cast<const std::uint64_t*>((sorted_in_scratch ? keys_scr : keys).mapped);
    const std::uint64_t* sorted_vals =
        static_cast<const std::uint64_t*>((sorted_in_scratch ? vals_scr : vals).mapped);

    // Seed `index` from the sorted row ids (ascending by key, radix-stable on ties).
    for (std::uint32_t i = 0; i < n; ++i)
        index_ptr[i] = static_cast<std::uint32_t>(sorted_vals[i] & 0xffffffffu);
    for (std::uint32_t i = n; i < padded_n; ++i)
        index_ptr[i] = 0xffffffffu;

    // Build equal-key run ranges (only runs of length > 1 need a tie-break).
    struct run_range {
        std::uint32_t begin;
        std::uint32_t len;
    };
    std::vector<run_range> ranges;
    bool has_oversized_run = false;
    for (std::uint32_t i = 0; i < n;) {
        std::uint32_t j = i + 1u;
        while (j < n && sorted_keys[j] == sorted_keys[i])
            ++j;
        if (j - i > 1u) {
            const std::uint32_t len = j - i;
            ranges.push_back(run_range{i, len});
            has_oversized_run = has_oversized_run || len > k_sequence_sort_local_size;
        }
        i = j;
    }

    if (ranges.empty()) {
        // No equal-key collisions: the 64-bit key fully ordered the batch.
        std::vector<std::uint32_t> perm(index_ptr, index_ptr + n);
        bump();
        return perm;
    }

    // --- Submit 2: GPU full-compare tie-break. Oversized collisions continue
    // through a device-side global bitonic network before the one host readback.
    buffer ranges_buf =
        create_device_buffer(std::uint64_t{ranges.size()} * 8u, k_compute_buffer_usage);
    if (ranges_buf.mapped == nullptr)
        throw vulkan_runtime_error("run_sequence_sort_roundtrip: ranges buffer alloc failed");
    std::memcpy(ranges_buf.mapped, ranges.data(), ranges.size() * sizeof(run_range));
    const gpu_arena_slice ranges_s{&ranges_buf, 0, ranges_buf.bytes};
    const sequence_sort_push_constants tb_push{
        static_cast<std::uint32_t>(ranges.size()),
        params.unit_bits,
        params.bits_per_char,
        params.letters_per_unit,
        params.char_mask,
        static_cast<std::uint32_t>(params.used_storage_mask & 0xffffffffu),
        static_cast<std::uint32_t>(params.used_storage_mask >> 32),
        0u,
        0u,
        0u};
    std::uint64_t global_compare_dispatches = 0;
    {
        reset_dispatch_descriptor_pool_locked();
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(seqsort2)");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(seqsort2)");
        record_host_to_shader(command_buffer_);
        const std::array<VkDescriptorBufferInfo, 4> tb_infos{{
            {seq_s.buffer->buffer_handle, seq_s.offset, seq_s.size},
            {descs_s.buffer->buffer_handle, descs_s.offset, descs_s.size},
            {index_s.buffer->buffer_handle, index_s.offset, index_s.size},
            {ranges_s.buffer->buffer_handle, ranges_s.offset, ranges_s.size},
        }};
        record_compute_dispatch_locked(command_buffer_,
                                       tb_pipe,
                                       tb_infos,
                                       &tb_push,
                                       sizeof(tb_push),
                                       static_cast<std::uint32_t>(ranges.size()));
        if (has_oversized_run) {
            const std::array<VkDescriptorBufferInfo, 4> gc_infos{{
                {seq_s.buffer->buffer_handle, seq_s.offset, seq_s.size},
                {descs_s.buffer->buffer_handle, descs_s.offset, descs_s.size},
                {index_s.buffer->buffer_handle, index_s.offset, index_s.size},
                {ranges_s.buffer->buffer_handle, ranges_s.offset, ranges_s.size},
            }};
            for (std::uint32_t sort_size = 2u; sort_size <= padded_n;) {
                for (std::uint32_t stride = sort_size >> 1u; stride > 0u; stride >>= 1u) {
                    record_compute_barrier(command_buffer_);
                    sequence_sort_push_constants gc_push{
                        padded_n,
                        params.unit_bits,
                        params.bits_per_char,
                        params.letters_per_unit,
                        params.char_mask,
                        static_cast<std::uint32_t>(params.used_storage_mask & 0xffffffffu),
                        static_cast<std::uint32_t>(params.used_storage_mask >> 32),
                        0u,
                        sort_size,
                        stride};
                    record_compute_dispatch_locked(
                        command_buffer_,
                        gc_pipe,
                        gc_infos,
                        &gc_push,
                        sizeof(gc_push),
                        bioseq::common::ceil_div(padded_n, k_sequence_sort_local_size));
                    ++global_compare_dispatches;
                }
                if (sort_size == padded_n)
                    break;
                sort_size <<= 1u;
            }
        }
        record_shader_to_host(command_buffer_);
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(seqsort2)");
    }

    if (global_compare_dispatches != 0) {
        seq_sort_global_compare_dispatch_count_.fetch_add(global_compare_dispatches,
                                                          std::memory_order_relaxed);
    }
    index_ptr =
        static_cast<std::uint32_t*>(index_buf.mapped); // mapping is stable; re-fetch defensively
    std::vector<std::uint32_t> perm(index_ptr, index_ptr + n);
    bump();
    return perm;
}

} // namespace bioseq::gpu::vulkan
