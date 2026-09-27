/**
 * @file sketch.cppm
 * @brief Runs GPU minimizer sketches and checks query-buffer readback.
 */
module;

#include <cstdint>
#include <libassert/assert.hpp>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

module bioseq.gpu.vulkan.runtime:sketch;

import :frontend_types;
import :support;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.parallelism.memory_admission;
import bioseq.instrumentation;
import std;

namespace bioseq::gpu::vulkan {

namespace {

/** @brief Records the buffers and completed steps of a checked minimizer download. */
struct authorized_readback_receipt {
    runtime_buffer_identity source{};
    runtime_buffer_identity destination{};
    std::uint64_t authorization_identity = 0u;
    std::uint64_t destination_offset = 0u;
    std::uint64_t copied_bytes = 0u;
    bool submitted = false;
    bool completed = false;
    bool command_reset = false;
};

/** @brief Checks that a completed download used the expected buffers and byte range. */
[[nodiscard]] inline bool authorized_readback_receipt_matches(
    const authorized_readback_receipt& receipt,
    const runtime_buffer_identity& expected_source,
    const runtime_buffer_identity& expected_destination,
    std::uint64_t expected_authorization_identity,
    std::uint64_t expected_destination_offset,
    std::uint64_t expected_bytes) noexcept {
    return receipt.source.is_valid() && receipt.destination.is_valid() &&
           receipt.source == expected_source && receipt.destination == expected_destination &&
           receipt.authorization_identity == expected_authorization_identity &&
           receipt.destination_offset == expected_destination_offset &&
           receipt.copied_bytes == expected_bytes && receipt.submitted && receipt.completed &&
           receipt.command_reset;
}

} // namespace

/// @brief Counts and writes one sequence's minimizers in one GPU submission.
/// Temporary  arena storage is freed before return.
/// @param[in] packed_sequence Packed bytes copied before submission.
/// @param[in] rank_lut Packed-rank, translation table
/// @param[in] params Sequence encoding and minimizer settings.
/// @param[in] max_minimizers Output capacity in minimizers.
/// @param[in] query_gate_words Optional query-hash bitmap.
/// @param[in] query_gate_mask Low-bit hash mask, or zero without a bitmap.
/// @return Exact count and owned device buffer, or overflow.
/// @throws vulkan_runtime_error If validation or execution fails.
runtime_sketch_result runtime::run_sketch_roundtrip(std::span<const std::byte> packed_sequence,
                                                    const vulkan_rank_lut& rank_lut,
                                                    const vulkan_sketch_params& params,
                                                    std::uint64_t max_minimizers,
                                                    std::span<const std::uint32_t> query_gate_words,
                                                    std::uint32_t query_gate_mask) {
    std::lock_guard<std::mutex> guard(mu_);
    runtime_sketch_result result;
    if (!features_.available) {
        throw vulkan_runtime_error("run_sketch_roundtrip on unavailable device");
    }
    gpu_arena& arena = reusable_gpu_arena();
    // The result owns a separate output buffer, so the shared arena can be
    // freed before return. Drop  memory left by earlier calls, then arrange
    // the same cleanup for this call, including exception paths.
    if (!arena.release_idle_capacity()) {
        throw vulkan_runtime_error("gpu sketch entered with live arena slices");
    }
    gpu_arena_trim_guard arena_trim{arena};
    const std::uint32_t bp = params.bp_size;
    const std::uint32_t window_size = params.window_size;
    const std::uint32_t kmer_size = params.kmer_size;
    if (bp == 0u || window_size == 0u || kmer_size == 0u || bp < kmer_size) {
        result.count = 0;
        if (!arena_trim.release_now()) {
            throw vulkan_runtime_error("gpu sketch empty roundtrip left live arena slices");
        }
        result.arena_trimmed_before_return = true;
        return result;
    }
    if (query_gate_words.size() >
        static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
        throw vulkan_runtime_error("gpu sketch query gate exceeds uint32 word count");
    }
    const std::uint32_t query_gate_word_count = static_cast<std::uint32_t>(query_gate_words.size());
    if ((query_gate_word_count == 0u && query_gate_mask != 0u) ||
        (query_gate_word_count != 0u && (query_gate_word_count != k_gpu_query_gate_word_count ||
                                         query_gate_mask != k_gpu_query_gate_mask))) {
        throw vulkan_runtime_error("gpu sketch query gate has truncated/noncanonical coverage");
    }
    const std::uint64_t query_gate_bytes =
        static_cast<std::uint64_t>(query_gate_words.size()) * sizeof(std::uint32_t);
    const std::uint64_t out_bytes = std::max<std::uint64_t>(
        minimizer_device_bytes(
            params.minimizer_word_kind, params.minimizer_words32, max_minimizers),
        4u);
    const gpu_sketch_memory_plan memory_plan =
        make_gpu_sketch_memory_plan(bp,
                                    packed_sequence.size(),
                                    sizeof(vulkan_rank_lut),
                                    max_minimizers,
                                    out_bytes,
                                    gpu_sketch_purpose::minimizer_words,
                                    kmer_size,
                                    window_size,
                                    params.letters_per_unit,
                                    features_.max_workgroup_count_x);
    if (!memory_plan.is_valid) {
        throw vulkan_runtime_error(
            fmt::format("gpu sketch memory plan invalid (bp={}, max_min={}, lpu={})",
                        bp,
                        max_minimizers,
                        params.letters_per_unit));
    }
    const std::uint32_t tile_bp = memory_plan.tiles.tile_bp;
    const std::uint32_t num_tiles = memory_plan.tiles.tile_count;
    // Cap workgroups at the device limit. The shader loops over tile indices,
    // so the same diqpatch still processes every tile.
    const std::uint32_t sketch_groups = std::min(
        bioseq::common::ceil_div(num_tiles, k_sketch_local_size), features_.max_workgroup_count_x);
    const std::uint32_t overlap = sketch_replay_overlap_bp(kmer_size, window_size);
    std::vector<sketch_tile_type> tiles(num_tiles);
    for (std::uint32_t tile_index = 0; tile_index < num_tiles; ++tile_index) {
        const std::uint32_t lo = tile_index * tile_bp;
        const std::uint32_t hi = std::min(bp, (tile_index + 1u) * tile_bp);
        const std::uint32_t rlo = lo > overlap ? lo - overlap : 0u;
        tiles[tile_index] = sketch_tile_type{params.seq_word_offset, bp, rlo, lo, hi, 0u};
    }
    const std::uint32_t num_blocks = memory_plan.tiles.scan_block_count;
    buffer out_words = create_device_buffer(out_bytes, k_compute_buffer_usage);
    if (out_words.mapped == nullptr)
        throw vulkan_runtime_error("gpu sketch non-unified memory");

    const std::uint64_t arena_bytes = bioseq::common::saturating_add(
        memory_plan.arena_scaffold_bytes,
        query_gate_bytes == 0u
            ? 0u
            : bioseq::common::saturating_add(query_gate_bytes,
                                             std::uint64_t{k_gpu_buffer_offset_alignment}));
    if (!arena.ensure_capacity(arena_bytes, 0, 0)) {
        throw vulkan_runtime_error(
            fmt::format("gpu arena grow failed (bp={}, arena_bytes={})", bp, arena_bytes));
    }
    const gpu_arena_mark mark = arena.mark();

    const gpu_arena_slice seq_s = arena.allocate_device(memory_plan.sequence_bytes, 16u);
    const gpu_arena_slice lut_s = arena.allocate_device(memory_plan.rank_lut_bytes, 16u);
    const gpu_arena_slice tiles_s = arena.allocate_device(memory_plan.tile_bytes, 16u);
    const gpu_arena_slice counts_s = arena.allocate_device(memory_plan.tile_count_bytes, 16u);
    const gpu_arena_slice offsets_s = arena.allocate_device(memory_plan.tile_offset_bytes, 16u);
    const gpu_arena_slice blocks_s = arena.allocate_device(memory_plan.scan_block_bytes, 16u);
    const gpu_arena_slice counters_s = arena.allocate_device(memory_plan.counter_bytes, 16u);
    const gpu_arena_slice pairvals_s =
        arena.allocate_device(memory_plan.arena_pair_value_bytes, 16u);
    const gpu_arena_slice query_gate_s =
        query_gate_bytes == 0u ? gpu_arena_slice{} : arena.allocate_device(query_gate_bytes, 16u);
    if (!seq_s.is_valid() || !lut_s.is_valid() || !tiles_s.is_valid() || !counts_s.is_valid() ||
        !offsets_s.is_valid() || !blocks_s.is_valid() || !counters_s.is_valid() ||
        !pairvals_s.is_valid() || (query_gate_bytes != 0u && !query_gate_s.is_valid())) {
        arena.rewind(mark);
        throw vulkan_runtime_error(
            fmt::format("gpu sketch arena oversub (bp={} arena_bytes={} num_tiles={})",
                        bp,
                        arena_bytes,
                        num_tiles));
    }

    auto* base = static_cast<std::byte*>(arena.device_buffer().mapped);
    if (!packed_sequence.empty()) {
#ifdef BIOSEQ_TEST_INDEXES
        note_vulkan_submission_test_upload();
#endif
        std::memcpy(base + seq_s.offset, packed_sequence.data(), packed_sequence.size());
    }
    std::memcpy(base + lut_s.offset, &rank_lut, sizeof(rank_lut));
    std::memcpy(base + tiles_s.offset, tiles.data(), tiles.size() * sizeof(sketch_tile_type));
    std::memset(base + counts_s.offset, 0, static_cast<std::size_t>(counts_s.size));
    std::memset(base + counters_s.offset, 0, 8);
    if (query_gate_bytes != 0u) {
        std::memcpy(base + query_gate_s.offset,
                    query_gate_words.data(),
                    static_cast<std::size_t>(query_gate_bytes));
    }

    if (params.minimizer_word_kind == 0u)
        std::memset(out_words.mapped, 0, static_cast<std::size_t>(out_words.bytes));

    const compute_pipeline sketch_pipe = get_compute_pipeline_locked(
        shader_kind::ordered_sketch_tile,
        k_sketch_local_size,
        choose_ordered_sketch_window_capacity(window_size));
    const compute_pipeline scan_block_pipe =
        get_compute_pipeline_locked(shader_kind::prefix_scan_block, 1u, 1u);
    const compute_pipeline scan_add_pipe =
        get_compute_pipeline_locked(shader_kind::prefix_scan_add, 128u, 1u);
    reset_dispatch_descriptor_pool_locked();

    const gpu_arena_slice& gate_binding_s = query_gate_bytes == 0u ? counters_s : query_gate_s;
    const std::array<VkDescriptorBufferInfo, 9> sketch_infos{{
        {seq_s.buffer->buffer_handle, seq_s.offset, seq_s.size},
        {lut_s.buffer->buffer_handle, lut_s.offset, lut_s.size},
        {tiles_s.buffer->buffer_handle, tiles_s.offset, tiles_s.size},
        {counts_s.buffer->buffer_handle, counts_s.offset, counts_s.size},
        {out_words.buffer_handle, 0, VK_WHOLE_SIZE},
        {counters_s.buffer->buffer_handle, counters_s.offset, counters_s.size},
        {offsets_s.buffer->buffer_handle, offsets_s.offset, offsets_s.size},
        {pairvals_s.buffer->buffer_handle, pairvals_s.offset, pairvals_s.size},
        {gate_binding_s.buffer->buffer_handle, gate_binding_s.offset, gate_binding_s.size},
    }};
    const std::array<VkDescriptorBufferInfo, 3> scan_infos{{
        {counts_s.buffer->buffer_handle, counts_s.offset, counts_s.size},
        {offsets_s.buffer->buffer_handle, offsets_s.offset, offsets_s.size},
        {blocks_s.buffer->buffer_handle, blocks_s.offset, blocks_s.size},
    }};

    sketch_push_constants push{0u,
                               window_size,
                               kmer_size,
                               params.hash_bits,
                               params.hash_shift,
                               params.pos_bits,
                               params.minimizer_word_kind,
                               params.minimizer_words32,
                               kmer_size,
                               params.alphabet_kind,
                               params.unit_bits,
                               params.bits_per_char,
                               params.letters_per_unit,
                               params.char_mask,
                               num_tiles,
                               0u,
                               query_gate_mask,
                               query_gate_word_count};
    scan_push_constants scan0{num_tiles, k_prefix_scan_block_size, num_blocks, 0u};
    scan_push_constants scan1{num_tiles, k_prefix_scan_block_size, num_blocks, 1u};

    static const auto* _qsketch_d = bioseq::instrumentation::register_static_region("query_sketch");
    auto _qsketch_s = bioseq::instrumentation::region(_qsketch_d);
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer(pc_sketch)");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "vkBeginCommandBuffer(pc_sketch)");
    record_host_to_shader(command_buffer_);
    record_compute_dispatch_locked(
        command_buffer_, sketch_pipe, sketch_infos, &push, sizeof(push), sketch_groups);
    record_compute_barrier(command_buffer_);
    record_compute_dispatch_locked(
        command_buffer_, scan_block_pipe, scan_infos, &scan0, sizeof(scan0), num_blocks);
    record_compute_barrier(command_buffer_);
    record_compute_dispatch_locked(
        command_buffer_, scan_add_pipe, scan_infos, &scan0, sizeof(scan0), 1u);
    record_compute_barrier(command_buffer_);
    record_compute_dispatch_locked(command_buffer_,
                                   scan_add_pipe,
                                   scan_infos,
                                   &scan1,
                                   sizeof(scan1),
                                   bioseq::common::ceil_div(num_tiles, 128u));
    record_compute_barrier(command_buffer_);
    push.emit_mode = 1u;
    record_compute_dispatch_locked(
        command_buffer_, sketch_pipe, sketch_infos, &push, sizeof(push), sketch_groups);
    record_shader_to_host(command_buffer_);
    check_vk(vkEndCommandBuffer(command_buffer_), "vkEndCommandBuffer(pc_sketch)");
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffer_;
    check_vk(vkQueueSubmit(queue_, 1, &submit, fence_), "vkQueueSubmit(pc_sketch)");
#ifdef BIOSEQ_TEST_INDEXES
    const vulkan_submission_test_fault submission_test_fault =
        delay_vulkan_submission_test_after_submit();
#endif
    check_vk(vkWaitForFences(device_,
                             1,
                             &fence_,
                             VK_TRUE,
                             std::numeric_limits<std::uint64_t>::max()),
             "vkWaitForFences(pc_sketch)");
    check_vk(vkResetFences(device_, 1, &fence_), "vkResetFences(pc_sketch)");
    require_immutable_metal_policy_live("sketch operation completion");
#ifdef BIOSEQ_TEST_INDEXES
    delay_vulkan_submission_test_after_drain(submission_test_fault);
    deliver_vulkan_submission_test_fault_after_drain(submission_test_fault);
#endif

    std::uint32_t last_off = 0, last_cnt = 0;
    std::memcpy(&last_off, base + offsets_s.offset + std::uint64_t{num_tiles - 1u} * 4u, 4);
    std::memcpy(&last_cnt, base + counts_s.offset + std::uint64_t{num_tiles - 1u} * 4u, 4);
    const std::uint64_t total = static_cast<std::uint64_t>(last_off) + last_cnt;

    if (total > max_minimizers) {
        result.overflow = true;
    } else {
        result.count = total;
        result.minimizer_words = std::move(out_words);
    }
    arena.rewind(mark);
    if (!arena_trim.release_now()) {
        result.minimizer_words.reset();
        throw vulkan_runtime_error("gpu sketch completed with live arena slices");
    }
    result.arena_trimmed_before_return = true;
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer(pc_sketch-post)");
    chaining_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    return result;
}

/// @brief Sketches query tiles and records each query's minimizer range.
/// Host output uses one submission. Device-only output counts first, allocates
/// exact output, then emits in a second submission and saves a query record
/// for later seed lookup checks. Overflow returns without device-only emission.
/// `query_layouts` must consecutively cover all tiles. Inputs are  copied before
/// submission.
/// @return Owned device output, tile counts, offsets, and dispatch measurements.
/// Host mode also returns a host copy.
/// @throws vulkan_runtime_error If validation or execution fails.
runtime_sketch_batch_result runtime::run_sketch_tile_batch_roundtrip(
    std::span<const std::byte> packed_sequence,
    const vulkan_rank_lut& rank_lut,
    const vulkan_sketch_params& params,
    std::span<const gpu_sketch_tile_wire> tiles,
    std::span<const gpu_sketch_query_layout_wire> query_layouts,
    std::uint64_t max_minimizers,
    vulkan_sketch_host_materialization materialization) {
    std::lock_guard<std::mutex> guard(mu_);
    runtime_sketch_batch_result result;
    if (!features_.available) {
        throw vulkan_runtime_error("run_sketch_tile_batch_roundtrip on unavailable device");
    }
    if (materialization != vulkan_sketch_host_materialization::host_and_device &&
        materialization != vulkan_sketch_host_materialization::device_only) {
        throw vulkan_runtime_error("gpu sketch tile batch has invalid host materialization mode");
    }
    result.dispatch_stats.materialization_ = materialization;
    const std::uint32_t window_size = params.window_size;
    const std::uint32_t kmer_size = params.kmer_size;
    if (tiles.empty() || query_layouts.empty() || window_size == 0u || kmer_size == 0u ||
        max_minimizers == 0u) {
        return result;
    }
    if (tiles.size() > static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
        throw vulkan_runtime_error("gpu sketch tile batch exceeds uint32 tile count");
    }
    if (max_minimizers > static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)())) {
        throw vulkan_runtime_error("gpu sketch tile batch capacity exceeds ABI");
    }
    std::uint64_t layout_tile_cursor = 0u;
    for (const gpu_sketch_query_layout_wire& layout : query_layouts) {
        if (layout.begin != layout_tile_cursor ||
            layout.count > static_cast<std::uint64_t>(tiles.size()) - layout_tile_cursor) {
            throw vulkan_runtime_error("gpu sketch query layout does not densely partition tiles");
        }
        layout_tile_cursor += layout.count;
    }
    if (layout_tile_cursor != tiles.size()) {
        throw vulkan_runtime_error("gpu sketch query layout does not cover all tiles");
    }

    const std::uint32_t tile_count = static_cast<std::uint32_t>(tiles.size());
    const gpu_sketch_batch_arena_request arena_request =
        make_sketch_batch_arena_request(packed_sequence.size(), tile_count);
    if (!arena_request.valid) {
        throw vulkan_runtime_error("gpu sketch tile batch arena request is invalid");
    }
    const std::uint32_t num_blocks = arena_request.num_blocks;
    const std::uint32_t sketch_groups = std::min(
        bioseq::common::ceil_div(tile_count, k_sketch_local_size), features_.max_workgroup_count_x);
    if (sketch_groups == 0u) {
        return result;
    }

    // Host output reserves the maximum size and counts, scans, and emits in
    // one submission Device-only output waits for the count before allocating.
    buffer host_materialized_out_words;
    if (materialization == vulkan_sketch_host_materialization::host_and_device) {
        const std::uint64_t pessimistic_output_bytes = std::max<std::uint64_t>(
            minimizer_device_bytes(
                params.minimizer_word_kind, params.minimizer_words32, max_minimizers),
            4u);
        host_materialized_out_words =
            create_device_buffer(pessimistic_output_bytes, k_compute_buffer_usage);
        if (host_materialized_out_words.mapped == nullptr) {
            throw vulkan_runtime_error("gpu sketch tile batch non-unified host output memory");
        }
    }

    const std::uint64_t arena_bytes = arena_request.requested_bytes;
    gpu_arena& arena = reusable_gpu_arena();
    if (!arena.ensure_capacity(arena_bytes, 0u, 0u)) {
        throw vulkan_runtime_error(
            fmt::format("gpu sketch tile batch arena grow failed (tiles={}, arena_bytes={})",
                        tile_count,
                        arena_bytes));
    }
    const gpu_arena_mark mark = arena.mark();
    // Restore the arena allocation positions even if the batch throws.
    struct arena_rewind_guard {
        gpu_arena* arena = nullptr;
        gpu_arena_mark mark{};

        /// @brief Restores the arena mark when the batch ends.
        ~arena_rewind_guard() {
            if (arena != nullptr)
                arena->rewind(mark);
        }
    } arena_rewind_on_exit{&arena, mark};

    const gpu_arena_slice seq_s =
        arena.allocate_device(std::max<std::uint64_t>(packed_sequence.size(), 4u), 16u);
    const gpu_arena_slice lut_s = arena.allocate_device(sizeof(vulkan_rank_lut), 16u);
    const gpu_arena_slice tiles_s = arena.allocate_device(
        static_cast<std::uint64_t>(tiles.size()) * sizeof(gpu_sketch_tile_wire), 16u);
    const gpu_arena_slice counts_s =
        arena.allocate_device(static_cast<std::uint64_t>(tile_count) * 4u, 16u);
    const gpu_arena_slice offsets_s =
        arena.allocate_device(static_cast<std::uint64_t>(tile_count) * 4u, 16u);
    const gpu_arena_slice blocks_s =
        arena.allocate_device(static_cast<std::uint64_t>(num_blocks) * 4u, 16u);
    const gpu_arena_slice counters_s = arena.allocate_device(8u, 16u);
    const gpu_arena_slice pairvals_s = arena.allocate_device(16u, 16u);
    if (!seq_s.is_valid() || !lut_s.is_valid() || !tiles_s.is_valid() || !counts_s.is_valid() ||
        !offsets_s.is_valid() || !blocks_s.is_valid() || !counters_s.is_valid() ||
        !pairvals_s.is_valid()) {
        throw vulkan_runtime_error(
            fmt::format("gpu sketch tile batch arena oversub (tiles={} arena_bytes={})",
                        tile_count,
                        arena_bytes));
    }

    auto* base = static_cast<std::byte*>(arena.device_buffer().mapped);
    std::memset(base + seq_s.offset, 0, static_cast<std::size_t>(seq_s.size));
    if (!packed_sequence.empty()) {
        std::memcpy(base + seq_s.offset, packed_sequence.data(), packed_sequence.size());
    }
    std::memcpy(base + lut_s.offset, &rank_lut, sizeof(rank_lut));
    std::memcpy(base + tiles_s.offset, tiles.data(), tiles.size() * sizeof(gpu_sketch_tile_wire));
    std::memset(base + counts_s.offset, 0, static_cast<std::size_t>(counts_s.size));
    std::memset(base + offsets_s.offset, 0, static_cast<std::size_t>(offsets_s.size));
    std::memset(base + blocks_s.offset, 0, static_cast<std::size_t>(blocks_s.size));
    std::memset(base + counters_s.offset, 0, static_cast<std::size_t>(counters_s.size));
    if (materialization == vulkan_sketch_host_materialization::host_and_device &&
        params.minimizer_word_kind == 0u) {
        std::memset(host_materialized_out_words.mapped,
                    0,
                    static_cast<std::size_t>(host_materialized_out_words.bytes));
    }
    std::unique_ptr<runtime_pipeline_use_collector> query_pipeline_collector;
    const bool causal_pipeline_token_required =
        materialization == vulkan_sketch_host_materialization::device_only &&
        (k_native_vulkan_pipeline_receipts || should_use_shader_provider_locked() ||
         is_runtime_metal_archive_configuration_installed());
    if (causal_pipeline_token_required) {
        query_pipeline_collector = runtime_pipeline_use_collector::create(
            runtime_pipeline_operation_kind::query_sketch, 1u, next_operation_identity_locked());
    }
    const compute_pipeline sketch_pipe = get_compute_pipeline_locked(
        shader_kind::ordered_sketch_tile,
        k_sketch_local_size,
        choose_ordered_sketch_window_capacity(window_size));
    const compute_pipeline scan_block_pipe =
        get_compute_pipeline_locked(shader_kind::prefix_scan_block, 1u, 1u);
    const compute_pipeline scan_add_pipe =
        get_compute_pipeline_locked(shader_kind::prefix_scan_add, 128u, 1u);
    if (query_pipeline_collector != nullptr) {
        // Record each pipeline used to produce the query buffer.
        const auto prepare = [&query_pipeline_collector](const compute_pipeline& pipe) {
            query_pipeline_collector->prepare(
                make_shader_key(pipe.kind, 384u, pipe.local_size, pipe.max_span),
                pipe.metal_archive_receipt);
        };
        prepare(sketch_pipe);
        prepare(scan_block_pipe);
        prepare(scan_add_pipe);
    }
    reset_dispatch_descriptor_pool_locked();

    // Counting does not write minimizer output. Device-only  mode can bind the
    // counters buffer here until the count gives the required output size.
    // Host mode binds the output, buffer ued by its later emit dispatch.
    const VkDescriptorBufferInfo initial_output_info =
        materialization == vulkan_sketch_host_materialization::host_and_device
            ? VkDescriptorBufferInfo{host_materialized_out_words.buffer_handle, 0, VK_WHOLE_SIZE}
            : VkDescriptorBufferInfo{
                  counters_s.buffer->buffer_handle, counters_s.offset, counters_s.size};
    const std::array<VkDescriptorBufferInfo, 9> count_sketch_infos{{
        {seq_s.buffer->buffer_handle, seq_s.offset, seq_s.size},
        {lut_s.buffer->buffer_handle, lut_s.offset, lut_s.size},
        {tiles_s.buffer->buffer_handle, tiles_s.offset, tiles_s.size},
        {counts_s.buffer->buffer_handle, counts_s.offset, counts_s.size},
        initial_output_info,
        {counters_s.buffer->buffer_handle, counters_s.offset, counters_s.size},
        {offsets_s.buffer->buffer_handle, offsets_s.offset, offsets_s.size},
        {pairvals_s.buffer->buffer_handle, pairvals_s.offset, pairvals_s.size},
        {counters_s.buffer->buffer_handle, counters_s.offset, counters_s.size},
    }};
    const std::array<VkDescriptorBufferInfo, 3> scan_infos{{
        {counts_s.buffer->buffer_handle, counts_s.offset, counts_s.size},
        {offsets_s.buffer->buffer_handle, offsets_s.offset, offsets_s.size},
        {blocks_s.buffer->buffer_handle, blocks_s.offset, blocks_s.size},
    }};

    sketch_push_constants push{0u,
                               window_size,
                               kmer_size,
                               params.hash_bits,
                               params.hash_shift,
                               params.pos_bits,
                               params.minimizer_word_kind,
                               params.minimizer_words32,
                               kmer_size,
                               params.alphabet_kind,
                               params.unit_bits,
                               params.bits_per_char,
                               params.letters_per_unit,
                               params.char_mask,
                               tile_count,
                               0u,
                               0u,
                               0u};
    scan_push_constants scan0{tile_count, k_prefix_scan_block_size, num_blocks, 0u};
    scan_push_constants scan1{tile_count, k_prefix_scan_block_size, num_blocks, 1u};

    static const auto* _qsketch_d =
        bioseq::instrumentation::register_static_region("query_sketch_batch");
    auto _qsketch_s = bioseq::instrumentation::region(_qsketch_d);
    runtime_sketch_dispatch_stats dispatch_stats{};
    dispatch_stats.materialization_ = materialization;
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer(pc_sketch_batch)");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(command_buffer_, &begin),
             "vkBeginCommandBuffer(pc_sketch_batch_count_scan)");
    if (query_pipeline_collector != nullptr) {
        query_pipeline_collector->begin(command_buffer_);
    }
    record_host_to_shader(command_buffer_);
    record_compute_dispatch_locked(command_buffer_,
                                   sketch_pipe,
                                   count_sketch_infos,
                                   &push,
                                   sizeof(push),
                                   sketch_groups,
                                   query_pipeline_collector.get(),
                                   0u);
    ++dispatch_stats.count_dispatches_;
    record_compute_barrier(command_buffer_);
    record_compute_dispatch_locked(command_buffer_,
                                   scan_block_pipe,
                                   scan_infos,
                                   &scan0,
                                   sizeof(scan0),
                                   num_blocks,
                                   query_pipeline_collector.get(),
                                   0u);
    ++dispatch_stats.prefix_scan_dispatches_;
    record_compute_barrier(command_buffer_);
    record_compute_dispatch_locked(command_buffer_,
                                   scan_add_pipe,
                                   scan_infos,
                                   &scan0,
                                   sizeof(scan0),
                                   1u,
                                   query_pipeline_collector.get(),
                                   0u);
    ++dispatch_stats.prefix_scan_dispatches_;
    record_compute_barrier(command_buffer_);
    record_compute_dispatch_locked(command_buffer_,
                                   scan_add_pipe,
                                   scan_infos,
                                   &scan1,
                                   sizeof(scan1),
                                   bioseq::common::ceil_div(tile_count, 128u),
                                   query_pipeline_collector.get(),
                                   0u);
    ++dispatch_stats.prefix_scan_dispatches_;
    ++dispatch_stats.prefix_scan_sequences_;
    if (materialization == vulkan_sketch_host_materialization::host_and_device) {
        record_compute_barrier(command_buffer_);
        push.emit_mode = 1u;
        record_compute_dispatch_locked(
            command_buffer_, sketch_pipe, count_sketch_infos, &push, sizeof(push), sketch_groups);
        ++dispatch_stats.emit_dispatches_;
    }
    record_shader_to_host(command_buffer_);
    check_vk(vkEndCommandBuffer(command_buffer_), "vkEndCommandBuffer(pc_sketch_batch_count_scan)");
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffer_;
    check_vk(vkQueueSubmit(queue_, 1, &submit, fence_),
             "vkQueueSubmit(pc_sketch_batch_count_scan)");
    check_vk(vkWaitForFences(device_,
                             1,
                             &fence_,
                             VK_TRUE,
                             std::numeric_limits<std::uint64_t>::max()),
             "vkWaitForFences(pc_sketch_batch_count_scan)");
    require_immutable_metal_policy_live("batched sketch count operation completion");
    check_vk(vkResetFences(device_, 1, &fence_), "vkResetFences(pc_sketch_batch_count_scan)");
    if (query_pipeline_collector != nullptr) {
        query_pipeline_collector->commit(command_buffer_);
    }
    ++dispatch_stats.completed_roundtrips_;
    result.dispatch_stats = dispatch_stats;

    result.tile_counts.resize(tiles.size());
    result.tile_offsets.resize(tiles.size());
    std::memcpy(result.tile_counts.data(),
                base + counts_s.offset,
                result.tile_counts.size() * sizeof(std::uint32_t));
    std::memcpy(result.tile_offsets.data(),
                base + offsets_s.offset,
                result.tile_offsets.size() * sizeof(std::uint32_t));
    const std::uint64_t total = static_cast<std::uint64_t>(result.tile_offsets.back()) +
                                static_cast<std::uint64_t>(result.tile_counts.back());

    if (total > max_minimizers) {
        result.overflow = true;
    } else if (materialization == vulkan_sketch_host_materialization::host_and_device) {
        result.count = total;
        const std::uint64_t host_bytes = minimizer_host_bytes(params.minimizer_word_kind, total);
        result.host_minimizer_bytes.resize(static_cast<std::size_t>(host_bytes));
        if (host_bytes > 0u) {
            std::memcpy(result.host_minimizer_bytes.data(),
                        host_materialized_out_words.mapped,
                        static_cast<std::size_t>(host_bytes));
        }
        result.minimizer_words = std::move(host_materialized_out_words);
    } else {
        // Allocate from the completed count, keeping a 4-byte buffer for an empty
        // result because Vulkan requires a nonzero size. Reuse the uploaded
        // sequence and tile counts for the submission that writes minimizers.
        const std::uint64_t exact_device_bytes = std::max<std::uint64_t>(
            minimizer_device_bytes(params.minimizer_word_kind, params.minimizer_words32, total),
            4u);
        buffer out_words = create_device_buffer(exact_device_bytes, k_compute_buffer_usage);
        if (out_words.mapped == nullptr) {
            throw vulkan_runtime_error("gpu sketch tile batch non-unified exact output memory");
        }
        if (params.minimizer_word_kind == 0u) {
            std::memset(out_words.mapped, 0, static_cast<std::size_t>(out_words.bytes));
        }

        const std::array<VkDescriptorBufferInfo, 9> emit_sketch_infos{{
            {seq_s.buffer->buffer_handle, seq_s.offset, seq_s.size},
            {lut_s.buffer->buffer_handle, lut_s.offset, lut_s.size},
            {tiles_s.buffer->buffer_handle, tiles_s.offset, tiles_s.size},
            {counts_s.buffer->buffer_handle, counts_s.offset, counts_s.size},
            {out_words.buffer_handle, 0, VK_WHOLE_SIZE},
            {counters_s.buffer->buffer_handle, counters_s.offset, counters_s.size},
            {offsets_s.buffer->buffer_handle, offsets_s.offset, offsets_s.size},
            {pairvals_s.buffer->buffer_handle, pairvals_s.offset, pairvals_s.size},
            {counters_s.buffer->buffer_handle, counters_s.offset, counters_s.size},
        }};

        // The first submission has finished, so its descriptor sets can be reused.
        // Keep the arena slices until the emit submission finishes.
        reset_dispatch_descriptor_pool_locked();
        if (query_pipeline_collector != nullptr) {
            query_pipeline_collector->burn_before_reset(command_buffer_);
        }
        check_vk(vkResetCommandBuffer(command_buffer_, 0),
                 "vkResetCommandBuffer(pc_sketch_batch_emit)");
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin),
                 "vkBeginCommandBuffer(pc_sketch_batch_emit)");
        if (query_pipeline_collector != nullptr) {
            query_pipeline_collector->begin(command_buffer_);
        }
        // Make the packed-u16 zero fill and the earlier GPU counts visible to
        // the emit  shader.
        record_host_to_shader(command_buffer_);
        record_compute_barrier(command_buffer_);
        push.emit_mode = 1u;
        record_compute_dispatch_locked(command_buffer_,
                                       sketch_pipe,
                                       emit_sketch_infos,
                                       &push,
                                       sizeof(push),
                                       sketch_groups,
                                       query_pipeline_collector.get(),
                                       0u);
        ++dispatch_stats.emit_dispatches_;
        record_shader_to_host(command_buffer_);
        check_vk(vkEndCommandBuffer(command_buffer_), "vkEndCommandBuffer(pc_sketch_batch_emit)");
        check_vk(vkQueueSubmit(queue_, 1, &submit, fence_), "vkQueueSubmit(pc_sketch_batch_emit)");
        check_vk(vkWaitForFences(device_,
                                 1,
                                 &fence_,
                                 VK_TRUE,
                                 std::numeric_limits<std::uint64_t>::max()),
                 "vkWaitForFences(pc_sketch_batch_emit)");
        require_immutable_metal_policy_live("batched sketch emit operation completion");
        check_vk(vkResetFences(device_, 1, &fence_), "vkResetFences(pc_sketch_batch_emit)");
        if (query_pipeline_collector != nullptr) {
            query_pipeline_collector->commit(command_buffer_);
        }
        ++dispatch_stats.completed_roundtrips_;
        result.dispatch_stats = dispatch_stats;

        result.count = total;
        result.minimizer_words = std::move(out_words);
    }
    if (!result.overflow && materialization == vulkan_sketch_host_materialization::device_only &&
        result.minimizer_words.is_valid() && dispatch_stats.is_internally_consistent()) {
        if (query_pipeline_collector != nullptr) {
            result.query_authority.pipeline_token_ = query_pipeline_collector->seal();
        }
        result.query_authority.identity_ = next_operation_identity_locked();
        result.query_authority.runtime_ = this;
        result.query_authority.buffer_ = result.minimizer_words.buffer_handle;
        result.query_authority.memory_identity_ =
            vulkan_handle_identity(result.minimizer_words.memory);
        result.query_authority.buffer_bytes_ = result.minimizer_words.bytes;
        result.query_authority.allocation_bytes_ = result.minimizer_words.allocation_bytes;
        result.query_authority.logical_minimizer_bytes_ = minimizer_device_bytes(
            params.minimizer_word_kind, params.minimizer_words32, result.count);
        result.query_authority.minimizer_count_ = result.count;
        result.query_authority.window_size_ = params.window_size;
        result.query_authority.kmer_size_ = params.kmer_size;
        result.query_authority.minimizer_word_kind_ = params.minimizer_word_kind;
        result.query_authority.minimizer_words32_per_record_ = params.minimizer_words32;
        result.query_authority.hash_shift_ = params.hash_shift;
        result.query_authority.pos_bits_ = params.pos_bits;
        result.query_authority.materialization_ = materialization;
        result.query_authority.dispatch_stats_ = dispatch_stats;
        result.query_authority.device_uuid_ = features_.device_uuid;
        result.query_authority.query_records_.reserve(query_layouts.size());
        result.query_authority.query_tile_counts_.reserve(query_layouts.size());
        std::uint64_t query_minimizer_cursor = 0u;
        for (const gpu_sketch_query_layout_wire& layout : query_layouts) {
            std::uint64_t query_minimizer_count = 0u;
            for (std::uint32_t tile = 0u; tile < layout.count; ++tile) {
                query_minimizer_count += result.tile_counts[layout.begin + tile];
            }
            const std::uint64_t query_minimizer_offset =
                layout.count == 0u ? query_minimizer_cursor : result.tile_offsets[layout.begin];
            if (query_minimizer_offset != query_minimizer_cursor ||
                query_minimizer_count > (std::numeric_limits<std::uint32_t>::max)()) {
                throw vulkan_runtime_error("gpu sketch query authority partition is invalid");
            }
            result.query_authority.query_records_.push_back(gpu_seed_query_record_wire{
                .minimizer_offset = static_cast<std::uint32_t>(query_minimizer_offset),
                .minimizer_count = static_cast<std::uint32_t>(query_minimizer_count),
                .query_len = layout.query_len,
                .flags = 0u,
            });
            result.query_authority.query_tile_counts_.push_back(layout.count);
            query_minimizer_cursor += query_minimizer_count;
        }
        if (query_minimizer_cursor != result.count ||
            !result.query_authority.does_cover_device_output()) {
            throw vulkan_runtime_error("gpu sketch query authority does not cover device output");
        }
    }
    if (query_pipeline_collector != nullptr) {
        query_pipeline_collector->burn_before_reset(command_buffer_);
    }
    check_vk(vkResetCommandBuffer(command_buffer_, 0),
             "vkResetCommandBuffer(pc_sketch_batch-post)");
    chaining_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    return result;
}

/// @brief Checks that a query buffer and its requested ranges match the completed sketch record.
/// @return Number of minimizer bytes covered by the record.
std::uint64_t runtime::validate_query_sketch_authorization_locked(
    const device_query_sketch_authorization& authorization,
    const buffer& device_words,
    std::uint64_t expected_minimizer_count,
    std::uint32_t expected_word_kind,
    std::uint32_t expected_words32_per_record,
    std::uint32_t expected_hash_shift,
    std::uint32_t expected_pos_bits,
    std::span<const gpu_seed_query_record_wire> expected_query_records) const {
    const bool typed_word_shape =
        (expected_word_kind == 1u && expected_words32_per_record == 1u) ||
        (expected_word_kind == 2u && expected_words32_per_record == 2u) ||
        (expected_word_kind == 3u && expected_words32_per_record == 4u);
    const auto exact_bytes = bioseq::common::checked_mul(
        expected_minimizer_count,
        static_cast<std::uint64_t>(expected_words32_per_record) * sizeof(std::uint32_t));
    if (!typed_word_shape || !exact_bytes.has_value() ||
        *exact_bytes > (std::numeric_limits<std::size_t>::max)() ||
        !authorization.is_valid() || authorization.runtime_ != this ||
        authorization.identity_ == 0u || !device_words.is_valid() ||
        device_words.owner != this ||
        authorization.buffer_ != device_words.buffer_handle ||
        authorization.memory_identity_ != vulkan_handle_identity(device_words.memory) ||
        authorization.buffer_bytes_ != device_words.bytes ||
        authorization.allocation_bytes_ != device_words.allocation_bytes ||
        authorization.logical_minimizer_bytes_ != *exact_bytes ||
        *exact_bytes > device_words.bytes ||
        authorization.minimizer_count_ != expected_minimizer_count ||
        authorization.minimizer_word_kind_ != expected_word_kind ||
        authorization.minimizer_words32_per_record_ != expected_words32_per_record ||
        authorization.hash_shift_ != expected_hash_shift ||
        authorization.pos_bits_ != expected_pos_bits ||
        authorization.device_uuid_ != features_.device_uuid ||
        authorization.query_records_.size() != expected_query_records.size()) {
        throw vulkan_runtime_error(
            "authorized query-minimizer readback identity or typed extent mismatch");
    }
    for (std::size_t index = 0u; index < expected_query_records.size(); ++index) {
        const gpu_seed_query_record_wire& sealed = authorization.query_records_[index];
        const gpu_seed_query_record_wire& expected = expected_query_records[index];
        if (sealed.minimizer_offset != expected.minimizer_offset ||
            sealed.minimizer_count != expected.minimizer_count ||
            sealed.query_len != expected.query_len || sealed.flags != expected.flags) {
            throw vulkan_runtime_error(
                "authorized query-minimizer readback partition mismatch");
        }
    }
    return *exact_bytes;
}

/// @brief Copies query minimizers to the host after checking their sketch record.
/// The destination size must equal the payload size. The query record remains
/// available for seed lookup after this copy
void runtime::readback_authorized_query_minimizers(
    const device_query_sketch_authorization& authorization,
    const buffer& device_words,
    std::uint64_t expected_minimizer_count,
    std::uint32_t expected_word_kind,
    std::uint32_t expected_words32_per_record,
    std::uint32_t expected_hash_shift,
    std::uint32_t expected_pos_bits,
    std::span<const gpu_seed_query_record_wire> expected_query_records,
    std::span<std::byte> destination) {
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw vulkan_runtime_error(
            "readback_authorized_query_minimizers on unavailable device");
    }
    const std::uint64_t exact_bytes = validate_query_sketch_authorization_locked(
        authorization,
        device_words,
        expected_minimizer_count,
        expected_word_kind,
        expected_words32_per_record,
        expected_hash_shift,
        expected_pos_bits,
        expected_query_records);
    if (destination.size() != static_cast<std::size_t>(exact_bytes)) {
        throw vulkan_runtime_error(
            "authorized query-minimizer readback destination is not exact");
    }
    if (exact_bytes == 0u)
        return;

    gpu_arena& arena = reusable_gpu_arena();
    gpu_arena_trim_guard trim{arena};
    if (!arena.ensure_admitted_capacity(0u, 0u, exact_bytes)) {
        throw vulkan_runtime_error(
            "authorized query-minimizer readback backing admission failed");
    }
    const gpu_arena_slice download = arena.allocate_download(exact_bytes, 16u);
    if (!download.is_valid() || download.buffer->mapped == nullptr) {
        throw vulkan_runtime_error(
            "authorized query-minimizer readback arena is unavailable");
    }

    authorized_readback_receipt receipt{
        .source = make_runtime_buffer_identity(&device_words),
        .destination = make_runtime_buffer_identity(download.buffer),
        .authorization_identity = authorization.identity_,
        .destination_offset = download.offset,
        .copied_bytes = exact_bytes,
    };

    check_vk(vkResetCommandBuffer(command_buffer_, 0),
             "reset(authorized_query_minimizer_readback)");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(command_buffer_, &begin),
             "begin(authorized_query_minimizer_readback)");
    VkMemoryBarrier compute_to_transfer{};
    compute_to_transfer.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    compute_to_transfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    compute_to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command_buffer_,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0u,
                         1u,
                         &compute_to_transfer,
                         0u,
                         nullptr,
                         0u,
                         nullptr);
    const VkBufferCopy copy{0u, download.offset, exact_bytes};
    vkCmdCopyBuffer(command_buffer_,
                    device_words.buffer_handle,
                    download.buffer->buffer_handle,
                    1u,
                    &copy);
    VkMemoryBarrier transfer_to_host{};
    transfer_to_host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    transfer_to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    transfer_to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command_buffer_,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT,
                         0u,
                         1u,
                         &transfer_to_host,
                         0u,
                         nullptr,
                         0u,
                         nullptr);
    receipt.submitted = true;
    submit_and_wait(device_,
                    queue_,
                    command_buffer_,
                    fence_,
                    "submit(authorized_query_minimizer_readback)");
    receipt.completed = true;
    check_vk(vkResetCommandBuffer(command_buffer_, 0),
             "reset(authorized_query_minimizer_readback-post)");
    receipt.command_reset = true;

    if (!authorized_readback_receipt_matches(
            receipt,
            make_runtime_buffer_identity(&device_words),
            make_runtime_buffer_identity(download.buffer),
            authorization.identity_,
            download.offset,
            exact_bytes)) {
        throw vulkan_runtime_error(
            "authorized query-minimizer readback completion receipt mismatch");
    }
    std::memcpy(destination.data(),
                static_cast<const std::byte*>(download.buffer->mapped) + download.offset,
                destination.size());
    if (!trim.release_now()) {
        throw vulkan_runtime_error(
            "authorized query-minimizer readback arena did not become idle");
    }
}

#if defined(BIOSEQ_TEST_INDEXES)
bool runtime::self_test_authorized_readback_receipt_contract() {
    std::byte source_object{};
    std::byte destination_object{};
    std::byte owner_object{};
    const auto* owner = reinterpret_cast<const runtime*>(std::addressof(owner_object));
    const runtime_buffer_identity source{
        .object = reinterpret_cast<const buffer*>(std::addressof(source_object)),
        .owner = owner,
        .buffer_handle = 11u,
        .memory_handle = 12u,
        .bytes = 128u,
        .allocation_bytes = 160u,
    };
    const runtime_buffer_identity destination{
        .object = reinterpret_cast<const buffer*>(std::addressof(destination_object)),
        .owner = owner,
        .buffer_handle = 21u,
        .memory_handle = 22u,
        .bytes = 256u,
        .allocation_bytes = 320u,
    };
    constexpr std::uint64_t authorization_identity = 31u;
    constexpr std::uint64_t destination_offset = 32u;
    constexpr std::uint64_t exact_bytes = 64u;
    const authorized_readback_receipt exact{
        .source = source,
        .destination = destination,
        .authorization_identity = authorization_identity,
        .destination_offset = destination_offset,
        .copied_bytes = exact_bytes,
        .submitted = true,
        .completed = true,
        .command_reset = true,
    };
    if (!authorized_readback_receipt_matches(exact,
                                             source,
                                             destination,
                                             authorization_identity,
                                             destination_offset,
                                             exact_bytes)) {
        return false;
    }

    auto short_receipt = exact;
    short_receipt.copied_bytes = exact_bytes - 1u;
    auto long_receipt = exact;
    long_receipt.copied_bytes = exact_bytes + 1u;
    auto unsubmitted_receipt = exact;
    unsubmitted_receipt.submitted = false;
    auto incomplete_receipt = exact;
    incomplete_receipt.completed = false;
    auto unreset_receipt = exact;
    unreset_receipt.command_reset = false;
    return !authorized_readback_receipt_matches(short_receipt,
                                                source,
                                                destination,
                                                authorization_identity,
                                                destination_offset,
                                                exact_bytes) &&
           !authorized_readback_receipt_matches(long_receipt,
                                                source,
                                                destination,
                                                authorization_identity,
                                                destination_offset,
                                                exact_bytes) &&
           !authorized_readback_receipt_matches(unsubmitted_receipt,
                                                source,
                                                destination,
                                                authorization_identity,
                                                destination_offset,
                                                exact_bytes) &&
           !authorized_readback_receipt_matches(incomplete_receipt,
                                                source,
                                                destination,
                                                authorization_identity,
                                                destination_offset,
                                                exact_bytes) &&
           !authorized_readback_receipt_matches(unreset_receipt,
                                                source,
                                                destination,
                                                authorization_identity,
                                                destination_offset,
                                                exact_bytes);
}
#endif

} // namespace bioseq::gpu::vulkan
