/**
 * @file target_index.cppm
 * @brief Builds resident target indexes and stable sorted minimizer pairs on Vulkan.
 */
module;

#include <cstdint>
#include <libassert/assert.hpp>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

module bioseq.gpu.vulkan.runtime:target_index;

import :frontend_types;
import :support;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.parallelism.memory_admission;
import bioseq.instrumentation;
import std;

namespace bioseq::gpu::vulkan {

/// @brief Prepares every pipeline used by the complete target-index route while locked.
void runtime::prepare_target_index_pipelines_locked(const vulkan_sketch_params& params) {
    // Keep this list identical to the complete target-index route. Pipeline
    // owners and their exact metadata permits remain process-lived in runtime;
    // later getters in count, radix, mark, and bucket stages are cache hits
    // under the operation's transient allocation permit.
    (void)get_compute_pipeline_locked(shader_kind::ordered_sketch_tile,
                                      k_sketch_local_size,
                                      std::max<std::uint32_t>(params.window_size, 1u));
    (void)get_compute_pipeline_locked(shader_kind::prefix_scan_block, 1u, 1u);
    (void)get_compute_pipeline_locked(shader_kind::prefix_scan_add, 128u, 1u);
    (void)get_compute_pipeline_locked(shader_kind::radix_sort_histogram, 1u, 1u);
    (void)get_compute_pipeline_locked(shader_kind::radix_sort_scan, 1u, 1u);
    (void)get_compute_pipeline_locked(shader_kind::radix_sort_scatter, 1u, 1u);
    (void)get_compute_pipeline_locked(shader_kind::target_index_mark_runs, 128u, 1u);
    (void)get_compute_pipeline_locked(shader_kind::target_index_emit_buckets, 128u, 1u);
}

/// @brief Validates the sketch shape and prepares target-index pipelines under queue ownership.
void runtime::prepare_target_index_pipelines(const vulkan_sketch_params& params) {
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw vulkan_runtime_error("prepare_target_index_pipelines on unavailable device");
    }
    if (params.window_size == 0u) {
        throw vulkan_runtime_error("prepare_target_index_pipelines with zero window");
    }
    prepare_target_index_pipelines_locked(params);
}

/// @brief Builds and validates a resident target index without materializing it on the host.
/// @return Move-owned index buffers, admission statistics, and a precise failure reason.
gpu_built_index runtime::build_target_index(std::span<const std::byte> packed_sequence,
                                            std::uint64_t padded_sequence_bytes,
                                            std::uint64_t owned_sequence_staging_bytes,
                                            std::function<void()> release_owned_sequence_staging,
                                            const vulkan_rank_lut& rank_lut,
                                            const vulkan_sketch_params& params,
                                            std::uint64_t max_target_minimizers,
                                            std::span<const std::uint32_t> query_gate_words,
                                            std::uint32_t query_gate_mask,
                                            gpu_index_gate_identity query_gate_identity,
                                            bioseq::parallelism::memory_permit* peak_permit) {
    std::lock_guard<std::mutex> guard(mu_);
    static const auto* _run_d = bioseq::instrumentation::register_static_region("target_index_run");
    auto _run_s = bioseq::instrumentation::region(_run_d);
    gpu_built_index out;
    out.ownership.count_submit_admissions = peak_permit != nullptr ? 1u : 0u;
    if (!features_.available)
        throw vulkan_runtime_error("build_target_index on unavailable device");
    const std::uint32_t bp = params.bp_size;
    const std::uint32_t w = params.window_size;
    const std::uint32_t k = params.kmer_size;
    out.target_len = bp;
    out.ownership.target_minimizer_capacity_estimate = max_target_minimizers;
    if (bp == 0u || w == 0u || k == 0u || bp < k || max_target_minimizers == 0u) {
        out.failure_reason = gpu_index_build_failure::invalid_shape;
        return out; // empty/invalid shape; selected GPU callers throw.
    }
    if (max_target_minimizers > std::numeric_limits<std::uint32_t>::max()) {
        out.failure_reason = gpu_index_build_failure::minimizer_capacity_exceeds_u32;
        return out;
    }
    if (packed_sequence.size() > padded_sequence_bytes ||
        padded_sequence_bytes >
            static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
        out.failure_reason = gpu_index_build_failure::memory_plan_invalid;
        return out;
    }
    if (query_gate_words.size() >
        static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
        throw vulkan_runtime_error("gpu index query gate exceeds uint32 word count");
    }
    const std::uint32_t query_gate_word_count = static_cast<std::uint32_t>(query_gate_words.size());
    const bool gate_uses_identity_owner =
        query_gate_identity.canonical_words &&
        query_gate_words.data() == query_gate_identity.canonical_words->data() &&
        query_gate_words.size() == query_gate_identity.canonical_words->size();
    const bool gate_shape_matches =
        query_gate_word_count == 0u
            ? query_gate_identity.is_well_formed() && !query_gate_identity.enabled
            : query_gate_identity.is_well_formed() && query_gate_identity.enabled &&
                  query_gate_identity.word_count == query_gate_word_count &&
                  query_gate_identity.mask == query_gate_mask &&
                  (gate_uses_identity_owner ||
                   std::equal(query_gate_words.begin(),
                              query_gate_words.end(),
                              query_gate_identity.canonical_words->begin()));
    if (!gate_shape_matches) {
        throw vulkan_runtime_error("gpu index query gate lacks matching immutable identity");
    }
    // Fail-safe for direct runtime callers. The production wrapper prepares
    // this same set before reserving target-index memory, so these are normally
    // cache hits. Keeping the invariant here prevents a future caller from
    // reintroducing a cold miss under the much larger exact execution peak below.
    prepare_target_index_pipelines_locked(params);
    out.gate_identity = std::move(query_gate_identity);
    out.ownership.query_gate_enabled = query_gate_word_count != 0u ? 1u : 0u;
    out.ownership.query_gate_word_count = query_gate_word_count;
    out.ownership.query_gate_mask = query_gate_word_count != 0u ? query_gate_mask : 0u;
    out.ownership.query_gate_digest_lo =
        query_gate_word_count != 0u ? out.gate_identity.digest_lo : 0u;
    out.ownership.query_gate_digest_hi =
        query_gate_word_count != 0u ? out.gate_identity.digest_hi : 0u;
    const std::uint64_t query_gate_bytes =
        static_cast<std::uint64_t>(query_gate_words.size()) * sizeof(std::uint32_t);
    const std::uint64_t flag_scan_alignment = std::max<std::uint64_t>(
        k_gpu_buffer_offset_alignment, features_.min_storage_buffer_offset_alignment);

    const gpu_sketch_memory_plan memory_plan =
        make_gpu_sketch_memory_plan(bp,
                                    padded_sequence_bytes,
                                    sizeof(vulkan_rank_lut),
                                    max_target_minimizers,
                                    0u,
                                    gpu_sketch_purpose::target_index_build,
                                    k,
                                    w,
                                    params.letters_per_unit,
                                    features_.max_workgroup_count_x);
    if (!memory_plan.is_valid) {
        out.failure_reason = gpu_index_build_failure::memory_plan_invalid;
        return out;
    }
    const gpu_target_index_host_scratch_plan host_scratch_plan =
        make_gpu_target_index_host_scratch_plan(memory_plan, owned_sequence_staging_bytes);
    if (!host_scratch_plan.is_valid ||
        (owned_sequence_staging_bytes != 0u && !release_owned_sequence_staging)) {
        out.failure_reason = gpu_index_build_failure::memory_plan_invalid;
        return out;
    }
    out.ownership.count_submit_host_transient_bytes = host_scratch_plan.count_submit_bytes;
    out.ownership.execution_submit_host_transient_bytes = host_scratch_plan.execution_submit_bytes;
    const std::uint32_t tile_bp = memory_plan.tiles.tile_bp;
    const std::uint32_t num_tiles = memory_plan.tiles.tile_count;
    // : grid-strided sketch — clamp the dispatch to the device limit.
    const std::uint32_t sketch_groups = std::min(
        bioseq::common::ceil_div(num_tiles, k_sketch_local_size), features_.max_workgroup_count_x);
    const std::uint32_t overlap = sketch_replay_overlap_bp(k, w);
    // Small per-call scaffolding lives in the shared arena (seq + lut + tiles +
    // tile counts/offsets). The large minimizer/sort/index arrays are sized from
    // the EXACT minimizer count (downloaded after the count pass) as standalone
    // device buffers — never a single 14 GB arena buffer that Metal cannot
    // allocate for the XXL_HIFI 500 Mbp target.
    gpu_arena& arena = reusable_gpu_arena();
    out.ownership.idle_arena_trim_attempts_before_count = 1u;
    const std::uint64_t idle_arena_bytes = bioseq::common::saturating_add(
        arena.device_buffer().allocation_bytes,
        bioseq::common::saturating_add(arena.upload_buffer().allocation_bytes,
                                       arena.download_buffer().allocation_bytes));
    const std::uint64_t pretrim_observed_peak =
        bioseq::common::saturating_add(idle_arena_bytes, host_scratch_plan.count_submit_bytes);
    out.ownership.pretrim_peak_observed_bytes = pretrim_observed_peak;
    if (!arena.release_idle_capacity()) {
        out.failure_reason = gpu_index_build_failure::index_invariant_failed;
        return out;
    }
    out.ownership.idle_arena_trim_successes_before_count = 1u;
    out.ownership.idle_arena_released_bytes_before_count = idle_arena_bytes;
    gpu_arena_trim_guard arena_trim{arena};
    const std::uint64_t arena_bytes = bioseq::common::saturating_add(
        memory_plan.arena_scaffold_bytes,
        query_gate_bytes == 0u
            ? 0u
            : bioseq::common::saturating_add(query_gate_bytes,
                                             std::uint64_t{k_gpu_buffer_offset_alignment}));
    const std::uint64_t count_arena_requirement =
        std::max(arena.device_buffer().allocation_bytes,
                 buffer_memory_requirement_locked(arena_bytes, k_gpu_arena_usage));
    const std::uint64_t count_required_peak = bioseq::common::saturating_add(
        count_arena_requirement, host_scratch_plan.count_submit_bytes);
    if (peak_permit == nullptr) {
        out.failure_reason = gpu_index_build_failure::peak_allocation_exceeds_admission;
        return out;
    }
    if (count_required_peak > peak_permit->bytes() &&
        !peak_permit->grow_to(count_required_peak, &out.execution_peak_admission_failure)) {
        out.failure_reason = gpu_index_build_failure::count_peak_memory_admission_denied;
        return out;
    }
    if (!arena.ensure_capacity(arena_bytes, 0, 0)) {
        throw vulkan_runtime_error(
            fmt::format("gpu index arena grow failed (bp={}, arena_bytes={})", bp, arena_bytes));
    }
    const gpu_arena_mark mark = arena.mark();
    const gpu_arena_slice seq_s = arena.allocate_device(memory_plan.sequence_bytes, 16u);
    const gpu_arena_slice lut_s = arena.allocate_device(memory_plan.rank_lut_bytes, 16u);
    const gpu_arena_slice tiles_s = arena.allocate_device(memory_plan.tile_bytes, 16u);
    const gpu_arena_slice tcounts_s = arena.allocate_device(memory_plan.tile_count_bytes, 16u);
    const gpu_arena_slice toffsets_s = arena.allocate_device(memory_plan.tile_offset_bytes, 16u);
    const gpu_arena_slice tblocks_s = arena.allocate_device(memory_plan.scan_block_bytes, 16u);
    const gpu_arena_slice counters_s = arena.allocate_device(memory_plan.counter_bytes, 16u);
    const gpu_arena_slice query_gate_s =
        query_gate_bytes == 0u ? gpu_arena_slice{} : arena.allocate_device(query_gate_bytes, 16u);
    if (!seq_s.is_valid() || !lut_s.is_valid() || !tiles_s.is_valid() || !tcounts_s.is_valid() ||
        !toffsets_s.is_valid() || !tblocks_s.is_valid() || !counters_s.is_valid() ||
        (query_gate_bytes != 0u && !query_gate_s.is_valid())) {
        bioseq_log<logging_level::error>(
            "[gpu index] scaffold arena oversubscribed (bp={}, num_tiles={}, "
            "arena_bytes={}); returning invalid GPU index",
            bp,
            num_tiles,
            arena_bytes);
        out.failure_reason = gpu_index_build_failure::device_arena_allocation_failed;
        arena.rewind(mark);
        return out;
    }

    const std::uint64_t count_observed_peak =
        std::max(pretrim_observed_peak,
                 bioseq::common::saturating_add(arena.device_buffer().allocation_bytes,
                                                host_scratch_plan.count_submit_bytes));
    if (count_observed_peak > peak_permit->bytes()) {
        out.failure_reason = gpu_index_build_failure::peak_allocation_exceeds_admission;
        out.observed_device_peak_bytes = arena.device_buffer().allocation_bytes;
        arena.rewind(mark);
        return out;
    }

    auto* base = static_cast<std::byte*>(arena.device_buffer().mapped);
    if (!packed_sequence.empty()) {
        std::memcpy(base + seq_s.offset, packed_sequence.data(), packed_sequence.size());
    }
    const std::uint64_t sequence_padding_bytes =
        seq_s.size - static_cast<std::uint64_t>(packed_sequence.size());
    if (sequence_padding_bytes != 0u) {
        std::memset(base + seq_s.offset + packed_sequence.size(),
                    0,
                    static_cast<std::size_t>(sequence_padding_bytes));
    }
    out.ownership.direct_sequence_arena_copies = 1u;
    out.ownership.sequence_padding_zeroed_bytes = sequence_padding_bytes;
    if (release_owned_sequence_staging) {
        release_owned_sequence_staging();
        release_owned_sequence_staging = {};
        out.ownership.host_sequence_capacity_releases_before_execution = 1u;
        out.ownership.host_sequence_capacity_released_bytes = owned_sequence_staging_bytes;
    }
    std::memcpy(base + lut_s.offset, &rank_lut, sizeof(rank_lut));
    for (std::uint32_t tile_index = 0u; tile_index < num_tiles; ++tile_index) {
        const std::uint32_t lo = tile_index * tile_bp;
        const std::uint32_t hi = std::min(bp, (tile_index + 1u) * tile_bp);
        const sketch_tile_type tile{
            params.seq_word_offset, bp, lo > overlap ? lo - overlap : 0u, lo, hi, 0u};
        std::memcpy(base + tiles_s.offset +
                        static_cast<std::uint64_t>(tile_index) * sizeof(sketch_tile_type),
                    &tile,
                    sizeof(tile));
    }
    out.ownership.direct_tile_arena_fills = 1u;
    out.ownership.host_tile_staging_elided_bytes = host_scratch_plan.elided_tile_staging_bytes;
    out.ownership.host_transient_released_before_execution_bytes =
        host_scratch_plan.released_before_execution_bytes;
    std::memset(base + tcounts_s.offset, 0, static_cast<std::size_t>(tcounts_s.size));
    if (query_gate_bytes != 0u) {
        std::memcpy(base + query_gate_s.offset,
                    query_gate_words.data(),
                    static_cast<std::size_t>(query_gate_bytes));
    }

    const compute_pipeline sketch_pipe = get_compute_pipeline_locked(
        shader_kind::ordered_sketch_tile, k_sketch_local_size, std::max<std::uint32_t>(w, 1u));
    const compute_pipeline scan_block_pipe =
        get_compute_pipeline_locked(shader_kind::prefix_scan_block, 1u, 1u);
    const compute_pipeline scan_add_pipe =
        get_compute_pipeline_locked(shader_kind::prefix_scan_add, 128u, 1u);

    // Count every target build on device first.  Query-gated segment indexes
    // usually retain only a tiny subset, while the ungated 500-Mbp row emits
    // 93,308,896 minimizers from a pessimistic capacity of 150,000,192.  Sizing
    // all four emit/sort streams from that pessimistic capacity made the main
    // peak C*40 bytes (6,000,007,680 bytes) even though the adopted resident
    // index is only exact-T/exact-D.  The count fence makes T authoritative
    // before any large standalone buffer exists.
    std::uint64_t emit_capacity = 0u;
    const gpu_arena_slice& gate_binding_s = query_gate_bytes == 0u ? counters_s : query_gate_s;
    {
        {
            const std::array<std::uint32_t, 4> cinit{0u, 0u, 0u, 0u};
            std::memcpy(base + counters_s.offset, cinit.data(), sizeof(cinit));
        }
        {
            static const auto* _rd =
                bioseq::instrumentation::register_static_region("target_sketch_count");
            auto _rs = bioseq::instrumentation::region(_rd);
            reset_dispatch_descriptor_pool_locked();
            const std::array<VkDescriptorBufferInfo, 9> sketch_infos{{
                {seq_s.buffer->buffer_handle, seq_s.offset, seq_s.size},
                {lut_s.buffer->buffer_handle, lut_s.offset, lut_s.size},
                {tiles_s.buffer->buffer_handle, tiles_s.offset, tiles_s.size},
                {tcounts_s.buffer->buffer_handle, tcounts_s.offset, tcounts_s.size},
                {counters_s.buffer->buffer_handle, counters_s.offset, counters_s.size},
                {counters_s.buffer->buffer_handle, counters_s.offset, counters_s.size},
                {toffsets_s.buffer->buffer_handle, toffsets_s.offset, toffsets_s.size},
                {counters_s.buffer->buffer_handle, counters_s.offset, counters_s.size},
                {gate_binding_s.buffer->buffer_handle, gate_binding_s.offset, gate_binding_s.size},
            }};
            const std::array<VkDescriptorBufferInfo, 3> scan_infos{{
                {tcounts_s.buffer->buffer_handle, tcounts_s.offset, tcounts_s.size},
                {toffsets_s.buffer->buffer_handle, toffsets_s.offset, toffsets_s.size},
                {tblocks_s.buffer->buffer_handle, tblocks_s.offset, tblocks_s.size},
            }};
            sketch_push_constants push{0u,
                                       w,
                                       k,
                                       params.hash_bits,
                                       params.hash_shift,
                                       params.pos_bits,
                                       params.minimizer_word_kind,
                                       params.minimizer_words32,
                                       k,
                                       params.alphabet_kind,
                                       params.unit_bits,
                                       params.bits_per_char,
                                       params.letters_per_unit,
                                       params.char_mask,
                                       num_tiles,
                                       1u,
                                       query_gate_mask,
                                       query_gate_word_count};
            const std::uint32_t num_blocks = memory_plan.tiles.scan_block_count;
            scan_push_constants scan0{num_tiles, k_prefix_scan_block_size, num_blocks, 0u};
            scan_push_constants scan1{num_tiles, k_prefix_scan_block_size, num_blocks, 1u};
            const std::uint32_t count_scan_block_groups =
                grid_stride_dispatch_group_count(num_blocks, 1u, features_.max_workgroup_count_x);
            const std::uint32_t count_scan_add_groups =
                grid_stride_dispatch_group_count(num_tiles, 128u, features_.max_workgroup_count_x);
            check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_idx_count)");
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_idx_count)");
            record_host_to_shader(command_buffer_);
            record_compute_dispatch_locked(
                command_buffer_, sketch_pipe, sketch_infos, &push, sizeof(push), sketch_groups);
            record_compute_barrier(command_buffer_);
            record_compute_dispatch_locked(command_buffer_,
                                           scan_block_pipe,
                                           scan_infos,
                                           &scan0,
                                           sizeof(scan0),
                                           count_scan_block_groups);
            record_compute_barrier(command_buffer_);
            record_compute_dispatch_locked(
                command_buffer_, scan_add_pipe, scan_infos, &scan0, sizeof(scan0), 1u);
            record_compute_barrier(command_buffer_);
            record_compute_dispatch_locked(command_buffer_,
                                           scan_add_pipe,
                                           scan_infos,
                                           &scan1,
                                           sizeof(scan1),
                                           count_scan_add_groups);
            record_shader_to_host(command_buffer_);
            submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(pc_idx_count)");
            out.ownership.exact_count_dispatches += 1u;
            out.ownership.exact_count_prefix_scan_dispatches += 3u;
            if (query_gate_bytes != 0u) {
                out.ownership.gate_count_dispatches += 1u;
            }
        }
        std::uint32_t last_offset = 0u;
        std::uint32_t last_count = 0u;
        std::memcpy(&last_offset,
                    base + toffsets_s.offset +
                        std::uint64_t{num_tiles - 1u} * sizeof(std::uint32_t),
                    sizeof(last_offset));
        std::memcpy(&last_count,
                    base + tcounts_s.offset + std::uint64_t{num_tiles - 1u} * sizeof(std::uint32_t),
                    sizeof(last_count));
        out.ownership.exact_count_control_readbacks += 1u;
        const std::uint64_t exact_count = static_cast<std::uint64_t>(last_offset) + last_count;
        const std::uint64_t hard_bound = target_minimizer_count_upper_bound(bp, k);
        if ((last_count & 0x80000000u) != 0u || exact_count > hard_bound ||
            exact_count > static_cast<std::uint64_t>(bp) ||
            exact_count > static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)())) {
            bioseq_log<logging_level::error>(
                "[gpu idx] exact target count={} violates hard bound={} "
                "(capacity estimate={})",
                exact_count,
                hard_bound,
                max_target_minimizers);
            out.failure_reason = gpu_index_build_failure::output_capacity_overflow;
            arena.rewind(mark);
            return out;
        }
        out.ownership.counted_minimizer_count = exact_count;
        out.ownership.capacity_estimate_underestimated =
            exact_count > max_target_minimizers ? 1u : 0u;
        if (exact_count == 0u) {
            const std::uint64_t empty_slot_requirement =
                buffer_memory_requirement_locked(16u, k_compute_buffer_usage);
            const std::uint64_t empty_position_requirement =
                buffer_memory_requirement_locked(8u, k_compute_buffer_usage);
            const std::uint64_t empty_peak = bioseq::common::saturating_add(
                memory_plan.execution_submit_host_transient_bytes,
                bioseq::common::saturating_add(
                    arena.device_buffer().allocation_bytes,
                    bioseq::common::saturating_add(empty_slot_requirement,
                                                   empty_position_requirement)));
            if (peak_permit == nullptr ||
                !peak_permit->grow_to(empty_peak, &out.execution_peak_admission_failure)) {
                out.failure_reason =
                    gpu_index_build_failure::execution_peak_memory_admission_denied;
                arena.rewind(mark);
                return out;
            }
            out.ownership.execution_peak_admissions = 1u;
            out.peak_admission_bytes = peak_permit->bytes();
            out.ownership.peak_admitted_bytes = peak_permit->bytes();
            out.slots = create_device_buffer(16u, k_compute_buffer_usage);
            out.positions = create_device_buffer(8u, k_compute_buffer_usage);
            if (out.slots.mapped == nullptr || out.positions.mapped == nullptr ||
                out.slots.allocation_bytes != empty_slot_requirement ||
                out.positions.allocation_bytes != empty_position_requirement) {
                out.failure_reason = gpu_index_build_failure::device_buffer_allocation_failed;
                arena.rewind(mark);
                return out;
            }
            std::memset(out.slots.mapped, 0, static_cast<std::size_t>(out.slots.allocation_bytes));
            std::memset(
                out.positions.mapped, 0, static_cast<std::size_t>(out.positions.allocation_bytes));
            out.observed_device_peak_bytes = bioseq::common::saturating_add(
                arena.device_buffer().allocation_bytes,
                bioseq::common::saturating_add(out.slots.allocation_bytes,
                                               out.positions.allocation_bytes));
            out.ownership.peak_observed_bytes = std::max(
                count_observed_peak,
                bioseq::common::saturating_add(out.observed_device_peak_bytes,
                                               memory_plan.execution_submit_host_transient_bytes));
            out.occupied_slots = 0u;
            out.position_count = 0u;
            out.slot_mask = 0u;
            check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_idx_empty-post)");
            arena.rewind(mark);
            const bool arena_released = arena_trim.release_now();
            const bool arena_invalid = !arena.device_buffer().is_valid() &&
                                       !arena.upload_buffer().is_valid() &&
                                       !arena.download_buffer().is_valid();
            if (!arena_released || !arena_invalid) {
                out.positions.reset();
                out.slots.reset();
                out.failure_reason = gpu_index_build_failure::index_invariant_failed;
                return out;
            }
            out.ownership.verified_emit_count = 0u;
            out.ownership.count_emit_parity_verified = 1u;
            out.ownership.arena_trimmed_before_permit_shrink = 1u;
            out.valid = true;
            return out;
        }
        emit_capacity = exact_count;
    }
    const std::uint64_t emit_flag_bytes = emit_capacity * 4u;
    const std::uint64_t emit_scanned_offset =
        bioseq::common::checked_align_up(emit_flag_bytes, flag_scan_alignment)
            .value_or((std::numeric_limits<std::uint64_t>::max)());
    const std::uint64_t emit_flag_scan_bytes =
        bioseq::common::saturating_add(emit_scanned_offset, emit_flag_bytes);
    const gpu_sketch_memory_plan exact_main_plan =
        make_gpu_sketch_memory_plan(bp,
                                    padded_sequence_bytes,
                                    sizeof(vulkan_rank_lut),
                                    emit_capacity,
                                    0u,
                                    gpu_sketch_purpose::target_index_build,
                                    k,
                                    w,
                                    params.letters_per_unit,
                                    features_.max_workgroup_count_x);
    const auto allocation_requirement = [this](std::uint64_t bytes) {
        return buffer_memory_requirement_locked(bytes, k_compute_buffer_usage);
    };
    const std::uint64_t initial_key_requirement =
        allocation_requirement(std::max<std::uint64_t>(emit_capacity * 8u, emit_flag_scan_bytes));
    const std::uint64_t initial_value_requirement = allocation_requirement(emit_capacity * 8u);
    const std::uint64_t exact_flag_bytes = emit_capacity * 4u;
    const std::uint64_t exact_scanned_offset =
        bioseq::common::checked_align_up(exact_flag_bytes, flag_scan_alignment)
            .value_or((std::numeric_limits<std::uint64_t>::max)());
    const std::uint64_t exact_flag_scan_bytes =
        bioseq::common::saturating_add(exact_scanned_offset, exact_flag_bytes);
    const std::uint64_t scratch_key_requirement =
        allocation_requirement(std::max<std::uint64_t>(emit_capacity * 8u, exact_flag_scan_bytes));
    const std::uint64_t scratch_value_requirement = allocation_requirement(emit_capacity * 8u);
    const workgroup_plan peak_wg = plan_workgroups(emit_capacity, features_.max_workgroup_count_x);
    const std::uint64_t histogram_requirement =
        allocation_requirement(std::uint64_t{peak_wg.num_workgroups} * 256u * 4u);
    const std::uint64_t scan_block_requirement = allocation_requirement(
        std::uint64_t{bioseq::common::ceil_div(emit_capacity, k_prefix_scan_block_size)} * 4u);
    const std::uint64_t bucket_requirement = allocation_requirement(emit_capacity * 16u);
    const std::uint32_t planned_hash_passes =
        std::clamp<std::uint32_t>((params.hash_bits + 7u) / 8u, 1u, 8u);
    const bool planned_sorted_in_scratch = (planned_hash_passes & 1u) != 0u;
    const std::uint64_t sorted_key_requirement =
        planned_sorted_in_scratch ? scratch_key_requirement : initial_key_requirement;
    const std::uint64_t flag_workspace_requirement =
        planned_sorted_in_scratch ? initial_key_requirement : scratch_key_requirement;
    const std::uint64_t arena_requirement = arena.device_buffer().allocation_bytes;
    const std::uint64_t sort_device_peak = bioseq::common::saturating_add(
        arena_requirement,
        bioseq::common::saturating_add(
            initial_key_requirement,
            bioseq::common::saturating_add(
                initial_value_requirement,
                bioseq::common::saturating_add(
                    scratch_key_requirement,
                    bioseq::common::saturating_add(scratch_value_requirement,
                                                   histogram_requirement)))));
    const std::uint64_t mark_device_peak = bioseq::common::saturating_add(
        arena_requirement,
        bioseq::common::saturating_add(
            sorted_key_requirement,
            bioseq::common::saturating_add(
                scratch_value_requirement,
                bioseq::common::saturating_add(flag_workspace_requirement,
                                               scan_block_requirement))));
    const std::uint64_t bucket_device_peak = bioseq::common::saturating_add(
        arena_requirement,
        bioseq::common::saturating_add(
            sorted_key_requirement,
            bioseq::common::saturating_add(
                scratch_value_requirement,
                bioseq::common::saturating_add(flag_workspace_requirement, bucket_requirement))));
    const std::uint64_t exact_device_peak =
        std::max(sort_device_peak, std::max(mark_device_peak, bucket_device_peak));
    const std::uint64_t exact_execution_admission = bioseq::common::saturating_add(
        exact_device_peak, exact_main_plan.execution_submit_host_transient_bytes);
    if (peak_permit == nullptr || !exact_main_plan.is_valid ||
        exact_execution_admission == (std::numeric_limits<std::uint64_t>::max)()) {
        out.failure_reason = gpu_index_build_failure::peak_allocation_exceeds_admission;
        arena.rewind(mark);
        return out;
    }
    if (!peak_permit->grow_to(exact_execution_admission, &out.execution_peak_admission_failure)) {
        out.failure_reason = gpu_index_build_failure::execution_peak_memory_admission_denied;
        arena.rewind(mark);
        return out;
    }
    out.ownership.execution_peak_admissions = 1u;
    out.peak_admission_bytes = peak_permit->bytes();
    out.ownership.peak_admitted_bytes = peak_permit->bytes();

    buffer keys = create_device_buffer(
        std::max<std::uint64_t>(emit_capacity * 8u, emit_flag_scan_bytes),
        k_compute_buffer_usage); // out_words: u64 hash/min; later flags+scan alias
    buffer vals = create_device_buffer(emit_capacity * 8u,
                                       k_compute_buffer_usage); // out_pair_vals: u64 pos_strand/min
    if (keys.mapped == nullptr || vals.mapped == nullptr ||
        keys.allocation_bytes != initial_key_requirement ||
        vals.allocation_bytes != initial_value_requirement) {
        bioseq_log<logging_level::error>("[gpu index] capacity buffer alloc failed (cap={}); "
                                         "returning invalid GPU index",
                                         emit_capacity);
        out.failure_reason = gpu_index_build_failure::device_buffer_allocation_failed;
        arena.rewind(mark);
        return out;
    }
    const std::uint64_t emit_observed_device = bioseq::common::saturating_add(
        arena.device_buffer().allocation_bytes,
        bioseq::common::saturating_add(keys.allocation_bytes, vals.allocation_bytes));
    out.observed_device_peak_bytes = std::max(out.observed_device_peak_bytes, emit_observed_device);
    if (bioseq::common::saturating_add(emit_observed_device,
                                       exact_main_plan.execution_submit_host_transient_bytes) >
        peak_permit->bytes()) {
        out.failure_reason = gpu_index_build_failure::peak_allocation_exceeds_admission;
        arena.rewind(mark);
        return out;
    }
    const gpu_arena_slice keys_s{&keys, 0, keys.bytes};
    const gpu_arena_slice vals_s{&vals, 0, vals.bytes};
    {
        const std::array<std::uint32_t, 4> cinit{
            0u, static_cast<std::uint32_t>(emit_capacity), 0u, 0u};
        std::memcpy(base + counters_s.offset, cinit.data(), sizeof(cinit));
    }
    {
        static const auto* _rd = bioseq::instrumentation::register_static_region("target_sketch");
        auto _rs = bioseq::instrumentation::region(_rd);
        reset_dispatch_descriptor_pool_locked();
        const std::array<VkDescriptorBufferInfo, 9> infos{{
            {seq_s.buffer->buffer_handle, seq_s.offset, seq_s.size},
            {lut_s.buffer->buffer_handle, lut_s.offset, lut_s.size},
            {tiles_s.buffer->buffer_handle, tiles_s.offset, tiles_s.size},
            {tcounts_s.buffer->buffer_handle, tcounts_s.offset, tcounts_s.size},
            {keys_s.buffer->buffer_handle, keys_s.offset, keys_s.size}, // out_words
            {counters_s.buffer->buffer_handle, counters_s.offset, counters_s.size},
            {toffsets_s.buffer->buffer_handle, toffsets_s.offset, toffsets_s.size},
            {vals_s.buffer->buffer_handle, vals_s.offset, vals_s.size}, // out_pair_vals
            {gate_binding_s.buffer->buffer_handle, gate_binding_s.offset, gate_binding_s.size},
        }};
        sketch_push_constants push{1u,
                                   w,
                                   k,
                                   params.hash_bits,
                                   params.hash_shift,
                                   params.pos_bits,
                                   params.minimizer_word_kind,
                                   params.minimizer_words32,
                                   k,
                                   params.alphabet_kind,
                                   params.unit_bits,
                                   params.bits_per_char,
                                   params.letters_per_unit,
                                   params.char_mask,
                                   num_tiles,
                                   1u,
                                   query_gate_mask,
                                   query_gate_word_count};
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_idx_emit)");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_idx_emit)");
        record_host_to_shader(command_buffer_);
        // Prefix offsets/counts were produced by the prior submit. Establish
        // explicit shader-write -> shader-read visibility before mode 1 reads
        // them; queue order and a host fence alone are not a memory barrier.
        record_compute_barrier(command_buffer_);
        record_compute_dispatch_locked(
            command_buffer_, sketch_pipe, infos, &push, sizeof(push), sketch_groups);
        record_shader_to_host(command_buffer_);
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(pc_idx_emit)");
        out.ownership.deterministic_tile_emit_dispatches += 1u;
        if (query_gate_bytes != 0u) {
            out.ownership.gated_emit_dispatches += 1u;
        } else {
            out.ownership.ungated_emit_dispatches += 1u;
        }
    }
    out.ownership.emit_validation_control_readbacks += 1u;
    std::uint32_t emit_status = 0u;
    std::memcpy(
        &emit_status, base + counters_s.offset + 2u * sizeof(std::uint32_t), sizeof(emit_status));
    constexpr std::uint32_t k_target_emit_complete = 0x80000000u;
    constexpr std::uint32_t k_target_emit_capacity_failure = 1u;
    const auto* tile_offsets = reinterpret_cast<const std::uint32_t*>(base + toffsets_s.offset);
    const auto* tile_markers = reinterpret_cast<const std::uint32_t*>(base + tcounts_s.offset);
    bool emit_parity_valid = emit_status == 0u && tile_offsets[0] == 0u;
    std::uint64_t verified_emit_count = 0u;
    for (std::uint32_t tile = 0u; emit_parity_valid && tile < num_tiles; ++tile) {
        const std::uint64_t offset = tile_offsets[tile];
        const std::uint64_t next_offset = tile + 1u < num_tiles
                                              ? static_cast<std::uint64_t>(tile_offsets[tile + 1u])
                                              : emit_capacity;
        const std::uint32_t marker = tile_markers[tile];
        const std::uint64_t actual = marker & ~k_target_emit_complete;
        emit_parity_valid = (marker & k_target_emit_complete) != 0u && offset <= next_offset &&
                            next_offset <= emit_capacity && actual == next_offset - offset;
        verified_emit_count = bioseq::common::saturating_add(verified_emit_count, actual);
    }
    emit_parity_valid = emit_parity_valid && verified_emit_count == emit_capacity;
    if (!emit_parity_valid) {
        bioseq_log<logging_level::error>("[gpu idx] deterministic emit parity failed "
                                         "(status={}, counted={}, verified={})",
                                         emit_status,
                                         emit_capacity,
                                         verified_emit_count);
        out.failure_reason = (emit_status & k_target_emit_capacity_failure) != 0u
                                 ? gpu_index_build_failure::output_capacity_overflow
                                 : gpu_index_build_failure::index_invariant_failed;
        arena.rewind(mark);
        return out;
    }
    const std::uint64_t tm = emit_capacity;
    out.ownership.verified_emit_count = verified_emit_count;
    out.ownership.count_emit_parity_verified = 1u;
    out.ownership.retained_minimizer_count = tm;

    const std::uint64_t tm_flag_bytes = tm * 4u;
    const std::uint64_t tm_scanned_offset =
        bioseq::common::checked_align_up(tm_flag_bytes, flag_scan_alignment)
            .value_or((std::numeric_limits<std::uint64_t>::max)());
    const std::uint64_t tm_flag_scan_bytes =
        bioseq::common::saturating_add(tm_scanned_offset, tm_flag_bytes);
    const std::uint64_t tm_scratch_key_requirement =
        allocation_requirement(std::max<std::uint64_t>(tm * 8u, tm_flag_scan_bytes));
    const std::uint64_t tm_scratch_value_requirement = allocation_requirement(tm * 8u);
    const workgroup_plan tm_expected_wg = plan_workgroups(tm, features_.max_workgroup_count_x);
    const std::uint64_t tm_histogram_requirement =
        allocation_requirement(std::uint64_t{tm_expected_wg.num_workgroups} * 256u * 4u);
    const std::uint64_t tm_scan_block_requirement = allocation_requirement(
        std::uint64_t{bioseq::common::ceil_div(tm, k_prefix_scan_block_size)} * 4u);

    // Exact-T sort scratch.  Lifetimes are split at GPU fences below so the
    // bucket stage never overlaps the histogram and the unused value buffer.
    const workgroup_plan swg = plan_workgroups(tm, features_.max_workgroup_count_x);
    const std::uint64_t flag_bytes = tm * 4u;
    const std::uint64_t scanned_offset =
        bioseq::common::checked_align_up(flag_bytes, flag_scan_alignment)
            .value_or((std::numeric_limits<std::uint64_t>::max)());
    const std::uint64_t flag_scan_bytes =
        bioseq::common::saturating_add(scanned_offset, flag_bytes);
    buffer keys_scr, vals_scr, hist;
    {
        static const auto* _alloc_d =
            bioseq::instrumentation::register_static_region("target_index_allocation");
        auto _alloc_s = bioseq::instrumentation::region(_alloc_d);
        keys_scr = create_device_buffer(std::max<std::uint64_t>(tm * 8u, flag_scan_bytes),
                                        k_compute_buffer_usage);
        vals_scr = create_device_buffer(tm * 8u, k_compute_buffer_usage);
        hist = create_device_buffer(std::uint64_t{swg.num_workgroups} * 256u * 4u,
                                    k_compute_buffer_usage);
    }
    if (keys_scr.mapped == nullptr || vals_scr.mapped == nullptr || hist.mapped == nullptr ||
        keys_scr.allocation_bytes != tm_scratch_key_requirement ||
        vals_scr.allocation_bytes != tm_scratch_value_requirement ||
        hist.allocation_bytes != tm_histogram_requirement) {
        bioseq_log<logging_level::error>(
            "[gpu index] scratch buffer alloc failed (tm={}, num_wg={}); "
            "returning invalid GPU index",
            tm,
            swg.num_workgroups);
        out.failure_reason = gpu_index_build_failure::device_buffer_allocation_failed;
        arena.rewind(mark);
        return out;
    }
    const gpu_arena_slice keys_scr_s{&keys_scr, 0, keys_scr.bytes};
    const gpu_arena_slice vals_scr_s{&vals_scr, 0, vals_scr.bytes};
    const gpu_arena_slice hist_s{&hist, 0, hist.bytes};

    out.observed_device_peak_bytes = bioseq::common::saturating_add(
        arena.device_buffer().allocation_bytes,
        bioseq::common::saturating_add(
            keys.allocation_bytes,
            bioseq::common::saturating_add(
                vals.allocation_bytes,
                bioseq::common::saturating_add(
                    keys_scr.allocation_bytes,
                    bioseq::common::saturating_add(vals_scr.allocation_bytes,
                                                   hist.allocation_bytes)))));
    if (bioseq::common::saturating_add(out.observed_device_peak_bytes,
                                       exact_main_plan.execution_submit_host_transient_bytes) >
        peak_permit->bytes()) {
        out.failure_reason = gpu_index_build_failure::peak_allocation_exceeds_admission;
        arena.rewind(mark);
        return out;
    }

    // --- Submit 2a: radix sort pairs by hash. ---
    // Only sort the significant hash bytes: for K=15 dna4 the stored hash is
    // ~30 bits → 4 passes, not 8, roughly halving the sort.
    const std::uint32_t hash_passes =
        std::clamp<std::uint32_t>((params.hash_bits + 7u) / 8u, 1u, 8u);
    gpu_sort_plan sort_plan{tm, 8u, 8u, 8u, hash_passes};
    bool sorted_in_scratch = false;
    {
        static const auto* _rd =
            bioseq::instrumentation::register_static_region("target_index_sort");
        auto _rs = bioseq::instrumentation::region(_rd);
        reset_dispatch_descriptor_pool_locked();
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_idx2)");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_idx2)");
        record_compute_barrier(command_buffer_);
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
        if (!sorted_in_scratch) {
            VkMemoryBarrier compute_to_copy{};
            compute_to_copy.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            compute_to_copy.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            compute_to_copy.dstAccessMask =
                VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(command_buffer_,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0u,
                                 1u,
                                 &compute_to_copy,
                                 0u,
                                 nullptr,
                                 0u,
                                 nullptr);
            const VkBufferCopy copy{0u, 0u, tm * 8u};
            vkCmdCopyBuffer(command_buffer_, vals.buffer_handle, vals_scr.buffer_handle, 1u, &copy);
            VkMemoryBarrier copy_to_host{};
            copy_to_host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            copy_to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            copy_to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT;
            vkCmdPipelineBarrier(command_buffer_,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT,
                                 0u,
                                 1u,
                                 &copy_to_host,
                                 0u,
                                 nullptr,
                                 0u,
                                 nullptr);
        }
        record_shader_to_host(command_buffer_);
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(pc_idx2_sort)");
    }

    // Preserve the sorted value run in its exact-T allocation. Four K=15 hash
    // passes may finish in the source value buffer; copying once here makes the
    // sorted allocation the sole resident position owner and lets the other
    // ping-pong allocation die.
    buffer positions_exact;
    buffer flag_scan;
    if (sorted_in_scratch) {
        positions_exact = std::move(vals_scr);
        vals.reset();
        flag_scan = std::move(keys);
    } else {
        positions_exact = std::move(vals_scr);
        vals.reset();
        flag_scan = std::move(keys_scr);
    }
    if (positions_exact.allocation_bytes > tm * 8u) {
        std::memset(static_cast<std::byte*>(positions_exact.mapped) + tm * 8u,
                    0,
                    static_cast<std::size_t>(positions_exact.allocation_bytes - tm * 8u));
    }
    hist.reset();

    if (flag_scan.bytes < flag_scan_bytes) {
        out.failure_reason = gpu_index_build_failure::peak_allocation_exceeds_admission;
        arena.rewind(mark);
        return out;
    }
    const gpu_arena_slice sorted_keys_s = sorted_in_scratch
                                              ? gpu_arena_slice{&keys_scr, 0u, tm * 8u}
                                              : gpu_arena_slice{&keys, 0u, tm * 8u};
    const gpu_arena_slice flags_s{&flag_scan, 0u, flag_bytes};
    const gpu_arena_slice scanned_s{&flag_scan, scanned_offset, flag_bytes};
    buffer iblocks = create_device_buffer(
        std::uint64_t{bioseq::common::ceil_div(tm, k_prefix_scan_block_size)} * 4u,
        k_compute_buffer_usage);
    if (iblocks.mapped == nullptr) {
        out.failure_reason = gpu_index_build_failure::device_buffer_allocation_failed;
        arena.rewind(mark);
        return out;
    }
    if (iblocks.allocation_bytes != tm_scan_block_requirement) {
        out.failure_reason = gpu_index_build_failure::peak_allocation_exceeds_admission;
        arena.rewind(mark);
        return out;
    }
    const gpu_arena_slice iblocks_s{&iblocks, 0u, iblocks.bytes};
    out.observed_device_peak_bytes =
        std::max(out.observed_device_peak_bytes,
                 bioseq::common::saturating_add(
                     arena.device_buffer().allocation_bytes,
                     bioseq::common::saturating_add(
                         sorted_keys_s.buffer->allocation_bytes,
                         bioseq::common::saturating_add(
                             positions_exact.allocation_bytes,
                             bioseq::common::saturating_add(flag_scan.allocation_bytes,
                                                            iblocks.allocation_bytes)))));
    if (bioseq::common::saturating_add(out.observed_device_peak_bytes,
                                       exact_main_plan.execution_submit_host_transient_bytes) >
        peak_permit->bytes()) {
        out.failure_reason = gpu_index_build_failure::peak_allocation_exceeds_admission;
        arena.rewind(mark);
        return out;
    }
    const VkDescriptorBufferInfo dummy{
        counters_s.buffer->buffer_handle, counters_s.offset, counters_s.size};

    // --- Submit 2b: mark runs and scan flags after sort scratch is released. ---
    {
        static const auto* _rd =
            bioseq::instrumentation::register_static_region("target_index_mark_scan");
        auto _rs = bioseq::instrumentation::region(_rd);
        reset_dispatch_descriptor_pool_locked();
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_idx_mark)");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_idx_mark)");
        record_host_to_shader(command_buffer_);
        record_compute_barrier(command_buffer_);
        const compute_pipeline mark_pipe =
            get_compute_pipeline_locked(shader_kind::target_index_mark_runs, 128u, 1u);
        const std::array<VkDescriptorBufferInfo, 7> idx_infos{{
            {sorted_keys_s.buffer->buffer_handle, sorted_keys_s.offset, sorted_keys_s.size},
            {flags_s.buffer->buffer_handle, flags_s.offset, flags_s.size},
            {scanned_s.buffer->buffer_handle, scanned_s.offset, scanned_s.size},
            dummy,
            dummy,
            dummy,
            dummy,
        }};
        index_push_constants ipush{static_cast<std::uint32_t>(tm), 0u, 0u, 0u, 0u, 0u};
        const std::uint32_t mark_groups =
            grid_stride_dispatch_group_count(tm, 128u, features_.max_workgroup_count_x);
        record_compute_dispatch_locked(
            command_buffer_, mark_pipe, idx_infos, &ipush, sizeof(ipush), mark_groups);
        record_compute_barrier(command_buffer_);
        const compute_pipeline scanb =
            get_compute_pipeline_locked(shader_kind::prefix_scan_block, 1u, 1u);
        const compute_pipeline scana =
            get_compute_pipeline_locked(shader_kind::prefix_scan_add, 128u, 1u);
        const std::uint32_t inb = bioseq::common::ceil_div(tm, k_prefix_scan_block_size);
        const std::array<VkDescriptorBufferInfo, 3> iscan{{
            {flags_s.buffer->buffer_handle, flags_s.offset, flags_s.size},
            {scanned_s.buffer->buffer_handle, scanned_s.offset, scanned_s.size},
            {iblocks_s.buffer->buffer_handle, iblocks_s.offset, iblocks_s.size},
        }};
        scan_push_constants is0{static_cast<std::uint32_t>(tm), k_prefix_scan_block_size, inb, 0u};
        scan_push_constants is1{static_cast<std::uint32_t>(tm), k_prefix_scan_block_size, inb, 1u};
        const std::uint32_t scan_block_groups =
            grid_stride_dispatch_group_count(inb, 1u, features_.max_workgroup_count_x);
        const std::uint32_t scan_add_groups =
            grid_stride_dispatch_group_count(tm, 128u, features_.max_workgroup_count_x);
        record_compute_dispatch_locked(
            command_buffer_, scanb, iscan, &is0, sizeof(is0), scan_block_groups);
        record_compute_barrier(command_buffer_);
        record_compute_dispatch_locked(command_buffer_, scana, iscan, &is0, sizeof(is0), 1u);
        record_compute_barrier(command_buffer_);
        record_compute_dispatch_locked(
            command_buffer_, scana, iscan, &is1, sizeof(is1), scan_add_groups);
        record_shader_to_host(command_buffer_);
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(pc_idx_mark)");
    }
    std::uint32_t scan_last = 0, flag_last = 0;
    std::memcpy(&scan_last,
                static_cast<const std::byte*>(flag_scan.mapped) + scanned_offset + (tm - 1u) * 4u,
                4u);
    std::memcpy(&flag_last, static_cast<const std::byte*>(flag_scan.mapped) + (tm - 1u) * 4u, 4u);
    const std::uint32_t num_buckets = scan_last + flag_last;
    out.ownership.distinct_minimizer_count = num_buckets;
    if (num_buckets == 0u) {
        bioseq_log<logging_level::error>("[gpu idx] num_buckets=0 (tm={})", tm);
        out.failure_reason = gpu_index_build_failure::index_invariant_failed;
        arena.rewind(mark);
        return out;
    }

    // The dense bucket array is built sorted by hash (the pairs were radix-sorted
    // by hash), so the seed lookup binary-searches it directly — no open-addressed
    // slot table. Building a 134 M-slot table with 50 M atomicCompSwap probes
    // tripped the macOS GPU watchdog (VK_ERROR_DEVICE_LOST) at XXL_HIFI; the
    // sorted-bucket form removes that whole submit. `buckets` becomes the resident
    // index ("slots"); `positions` is the hash-run-ordered pos_strand array.
    iblocks.reset();
    buffer buckets = create_device_buffer(std::uint64_t{num_buckets} * 16u, k_compute_buffer_usage);
    if (buckets.mapped == nullptr) {
        out.failure_reason = gpu_index_build_failure::device_buffer_allocation_failed;
        arena.rewind(mark);
        return out;
    }
    const std::uint64_t exact_bucket_requirement =
        allocation_requirement(std::uint64_t{num_buckets} * 16u);
    if (buckets.allocation_bytes != exact_bucket_requirement) {
        out.failure_reason = gpu_index_build_failure::peak_allocation_exceeds_admission;
        arena.rewind(mark);
        return out;
    }
    const gpu_arena_slice buckets_s{&buckets, 0u, buckets.bytes};
    {
        static const auto* _bd =
            bioseq::instrumentation::register_static_region("target_index_bucket_memory");
        auto _bs = bioseq::instrumentation::region(_bd);
        // Touch the complete allocation, including Vk padding, so resident
        // accounting never fabricates full-touch coverage.
        std::memset(buckets.mapped, 0, static_cast<std::size_t>(buckets.allocation_bytes));
    }

    out.observed_device_peak_bytes =
        std::max(out.observed_device_peak_bytes,
                 bioseq::common::saturating_add(
                     arena.device_buffer().allocation_bytes,
                     bioseq::common::saturating_add(
                         sorted_keys_s.buffer->allocation_bytes,
                         bioseq::common::saturating_add(
                             positions_exact.allocation_bytes,
                             bioseq::common::saturating_add(flag_scan.allocation_bytes,
                                                            buckets.allocation_bytes)))));
    if (bioseq::common::saturating_add(out.observed_device_peak_bytes,
                                       exact_main_plan.execution_submit_host_transient_bytes) >
        peak_permit->bytes()) {
        out.failure_reason = gpu_index_build_failure::peak_allocation_exceeds_admission;
        arena.rewind(mark);
        return out;
    }

    // --- Submit 3: emit dense sorted buckets (single light dispatch). ---
    {
        static const auto* _rd =
            bioseq::instrumentation::register_static_region("target_index_slots");
        auto _rs = bioseq::instrumentation::region(_rd);
        const gpu_arena_slice& sorted_keys = sorted_in_scratch ? keys_scr_s : keys_s;
        const std::array<VkDescriptorBufferInfo, 7> idx_infos{{
            {sorted_keys.buffer->buffer_handle, sorted_keys.offset, sorted_keys.size},
            {flags_s.buffer->buffer_handle, flags_s.offset, flags_s.size},
            {scanned_s.buffer->buffer_handle, scanned_s.offset, scanned_s.size},
            {buckets_s.buffer->buffer_handle, buckets_s.offset, buckets_s.size},
            dummy,
            dummy,
            dummy,
        }};
        index_push_constants ipush{static_cast<std::uint32_t>(tm), 0u, 0u, num_buckets, 0u, 0u};
        reset_dispatch_descriptor_pool_locked();
        const compute_pipeline emit_b =
            get_compute_pipeline_locked(shader_kind::target_index_emit_buckets, 128u, 1u);
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_idx3)");
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_idx3)");
        record_host_to_shader(command_buffer_);
        record_compute_barrier(command_buffer_);
        const std::uint32_t bucket_groups =
            grid_stride_dispatch_group_count(tm, 128u, features_.max_workgroup_count_x);
        record_compute_dispatch_locked(
            command_buffer_, emit_b, idx_infos, &ipush, sizeof(ipush), bucket_groups);
        record_shader_to_host(command_buffer_);
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(pc_idx3_buckets)");
    }

    out.slots = std::move(buckets);             // exact-D sorted dense buckets
    out.positions = std::move(positions_exact); // exact-T sorted pos_strand
    out.slot_mask = 0u;                         // 0 ⇒ seed binary-searches `occupied_slots` buckets
    out.occupied_slots = num_buckets;
    out.position_count = static_cast<std::uint32_t>(tm);
    out.ownership.peak_observed_bytes = std::max(
        count_observed_peak,
        bioseq::common::saturating_add(out.observed_device_peak_bytes,
                                       exact_main_plan.execution_submit_host_transient_bytes));
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_idx-post)");
    arena.rewind(mark);
    const bool arena_released = arena_trim.release_now();
    const bool arena_invalid = !arena.device_buffer().is_valid() &&
                               !arena.upload_buffer().is_valid() &&
                               !arena.download_buffer().is_valid();
    if (!arena_released || !arena_invalid) {
        out.positions.reset();
        out.slots.reset();
        out.failure_reason = gpu_index_build_failure::index_invariant_failed;
        return out;
    }
    out.ownership.arena_trimmed_before_permit_shrink = 1u;
    out.valid = true;
    chaining_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    return out;
}

/// @brief Sketches a target and returns stable value-then-key sorted minimizer pairs.
/// @return Resident sorted pairs and the exact host download used for persistence.
gpu_sorted_pair_result
runtime::run_sketch_sort_pairs_roundtrip(std::span<const std::byte> packed_sequence,
                                         const vulkan_rank_lut& rank_lut,
                                         const vulkan_sketch_params& params,
                                         std::uint64_t max_target_minimizers) {
    std::lock_guard<std::mutex> guard(mu_);
    static const auto* _run_d = bioseq::instrumentation::register_static_region("sorted_pair_run");
    auto _run_s = bioseq::instrumentation::region(_run_d);

    gpu_sorted_pair_result out;
    if (!features_.available) {
        throw vulkan_runtime_error("run_sketch_sort_pairs_roundtrip on unavailable device");
    }

    const std::uint32_t bp = params.bp_size;
    const std::uint32_t window_size = params.window_size;
    const std::uint32_t kmer_size = params.kmer_size;
    out.target_len = bp;
    if (bp == 0u || window_size == 0u || kmer_size == 0u || bp < kmer_size ||
        max_target_minimizers == 0u) {
        return out;
    }
    if (max_target_minimizers >
        static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)())) {
        return out;
    }

    const gpu_sketch_memory_plan memory_plan =
        make_gpu_sketch_memory_plan(bp,
                                    packed_sequence.size(),
                                    sizeof(vulkan_rank_lut),
                                    max_target_minimizers,
                                    0u,
                                    gpu_sketch_purpose::sorted_pairs,
                                    kmer_size,
                                    window_size,
                                    params.letters_per_unit,
                                    features_.max_workgroup_count_x);
    if (!memory_plan.is_valid) {
        return out;
    }
    const std::uint32_t tile_bp = memory_plan.tiles.tile_bp;
    const std::uint32_t num_tiles = memory_plan.tiles.tile_count;
    if (num_tiles == 0u) {
        return out;
    }
    // : grid-strided sketch — clamp the dispatch to the device limit.
    const std::uint32_t sketch_groups = std::min(
        bioseq::common::ceil_div(num_tiles, k_sketch_local_size), features_.max_workgroup_count_x);

    const std::uint32_t overlap = sketch_replay_overlap_bp(kmer_size, window_size);
    std::vector<sketch_tile_type> tiles(num_tiles);
    for (std::uint32_t tile = 0; tile < num_tiles; ++tile) {
        const std::uint32_t lo = tile * tile_bp;
        const std::uint32_t hi = std::min(bp, (tile + 1u) * tile_bp);
        tiles[tile] = sketch_tile_type{
            params.seq_word_offset, bp, lo > overlap ? lo - overlap : 0u, lo, hi, 0u};
    }

    gpu_arena& arena = reusable_gpu_arena();
    if (!arena.ensure_capacity(memory_plan.arena_scaffold_bytes, 0u, 0u)) {
        throw vulkan_runtime_error(
            fmt::format("gpu sorted-pair arena grow failed (bp={}, arena_bytes={})",
                        bp,
                        memory_plan.arena_scaffold_bytes));
    }

    const gpu_arena_mark mark = arena.mark();
    const gpu_arena_slice seq_s = arena.allocate_device(memory_plan.sequence_bytes, 16u);
    const gpu_arena_slice lut_s = arena.allocate_device(memory_plan.rank_lut_bytes, 16u);
    const gpu_arena_slice tiles_s = arena.allocate_device(memory_plan.tile_bytes, 16u);
    const gpu_arena_slice tcounts_s = arena.allocate_device(memory_plan.tile_count_bytes, 16u);
    const gpu_arena_slice toffsets_s = arena.allocate_device(memory_plan.tile_offset_bytes, 16u);
    const gpu_arena_slice tblocks_s = arena.allocate_device(memory_plan.scan_block_bytes, 16u);
    const gpu_arena_slice counters_s = arena.allocate_device(memory_plan.counter_bytes, 16u);
    if (!seq_s.is_valid() || !lut_s.is_valid() || !tiles_s.is_valid() || !tcounts_s.is_valid() ||
        !toffsets_s.is_valid() || !tblocks_s.is_valid() || !counters_s.is_valid()) {
        arena.rewind(mark);
        return out;
    }

    auto* base = static_cast<std::byte*>(arena.device_buffer().mapped);
    if (!packed_sequence.empty()) {
        std::memcpy(base + seq_s.offset, packed_sequence.data(), packed_sequence.size());
    }
    std::memcpy(base + lut_s.offset, &rank_lut, sizeof(rank_lut));
    std::memcpy(base + tiles_s.offset, tiles.data(), tiles.size() * sizeof(sketch_tile_type));
    std::memset(base + tcounts_s.offset, 0, static_cast<std::size_t>(tcounts_s.size));

    const compute_pipeline sketch_pipe =
        get_compute_pipeline_locked(shader_kind::ordered_sketch_tile,
                                    k_sketch_local_size,
                                    std::max<std::uint32_t>(window_size, 1u));

    const std::uint64_t capacity = max_target_minimizers;
    buffer keys = create_device_buffer(capacity * 8u, k_compute_buffer_usage);
    buffer values = create_device_buffer(capacity * 8u, k_compute_buffer_usage);
    if (keys.mapped == nullptr || values.mapped == nullptr) {
        arena.rewind(mark);
        return out;
    }

    const gpu_arena_slice keys_s{&keys, 0u, keys.bytes};
    const gpu_arena_slice values_s{&values, 0u, values.bytes};
    {
        const std::array<std::uint32_t, 4> counters_init{
            0u, static_cast<std::uint32_t>(capacity), 0u, 0u};
        std::memcpy(base + counters_s.offset, counters_init.data(), sizeof(counters_init));
    }

    {
        static const auto* _sketch_d =
            bioseq::instrumentation::register_static_region("sorted_pair_sketch");
        auto _sketch_s = bioseq::instrumentation::region(_sketch_d);
        reset_dispatch_descriptor_pool_locked();
        const std::array<VkDescriptorBufferInfo, 9> infos{{
            {seq_s.buffer->buffer_handle, seq_s.offset, seq_s.size},
            {lut_s.buffer->buffer_handle, lut_s.offset, lut_s.size},
            {tiles_s.buffer->buffer_handle, tiles_s.offset, tiles_s.size},
            {tcounts_s.buffer->buffer_handle, tcounts_s.offset, tcounts_s.size},
            {keys_s.buffer->buffer_handle, keys_s.offset, keys_s.size},
            {counters_s.buffer->buffer_handle, counters_s.offset, counters_s.size},
            {toffsets_s.buffer->buffer_handle, toffsets_s.offset, toffsets_s.size},
            {values_s.buffer->buffer_handle, values_s.offset, values_s.size},
            {counters_s.buffer->buffer_handle, counters_s.offset, counters_s.size},
        }};
        sketch_push_constants push{2u,
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
                                   1u,
                                   0u,
                                   0u};
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_sorted_pair_emit)");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_sorted_pair_emit)");
        record_host_to_shader(command_buffer_);
        record_compute_dispatch_locked(
            command_buffer_, sketch_pipe, infos, &push, sizeof(push), sketch_groups);
        record_shader_to_host(command_buffer_);
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(pc_sorted_pair_emit)");
    }

    std::uint32_t pair_count32 = 0;
    std::memcpy(&pair_count32, base + counters_s.offset, sizeof(pair_count32));
    const std::uint64_t pair_count = pair_count32;
    // A window with zero minimizers (for example, an all-N region) is a
    // LEGITIMATE empty result, not a GPU failure: return valid + empty so the
    // caller emits no pairs rather than throwing vulkan_runtime_error. Only an
    // actual capacity overshoot (count > the worst-case bound) is a real decline.
    if (pair_count == 0u) {
        out.pair_count = 0u;
        out.valid = true;
        arena.rewind(mark);
        return out;
    }
    if (pair_count > max_target_minimizers) {
        arena.rewind(mark);
        return out;
    }

    const workgroup_plan sort_wg = plan_workgroups(pair_count, features_.max_workgroup_count_x);
    buffer keys_scratch = create_device_buffer(pair_count * 8u, k_compute_buffer_usage);
    buffer values_scratch = create_device_buffer(pair_count * 8u, k_compute_buffer_usage);
    buffer histogram = create_device_buffer(std::uint64_t{sort_wg.num_workgroups} * 256u * 4u,
                                            k_compute_buffer_usage);
    if (keys_scratch.mapped == nullptr || values_scratch.mapped == nullptr ||
        histogram.mapped == nullptr) {
        arena.rewind(mark);
        return out;
    }

    sorted_pair_device_views views{
        .keys = keys_s,
        .values = values_s,
        .keys_scratch = gpu_arena_slice{&keys_scratch, 0u, keys_scratch.bytes},
        .values_scratch = gpu_arena_slice{&values_scratch, 0u, values_scratch.bytes},
        .keys_in_scratch = false,
    };
    const gpu_arena_slice histogram_s{&histogram, 0u, histogram.bytes};
    const gpu_sort_plan value_sort_plan{
        .element_count = pair_count,
        .key_bytes = 8u,
        .value_bytes = 8u,
        .radix_bits = 8u,
        .pass_count = 8u,
    };
    const std::uint32_t hash_passes =
        std::clamp<std::uint32_t>((params.hash_bits + 7u) / 8u, 1u, 8u);
    const gpu_sort_plan key_sort_plan{
        .element_count = pair_count,
        .key_bytes = 8u,
        .value_bytes = 8u,
        .radix_bits = 8u,
        .pass_count = hash_passes,
    };

    {
        static const auto* _sort_d =
            bioseq::instrumentation::register_static_region("sorted_pair_sort");
        auto _sort_s = bioseq::instrumentation::region(_sort_d);
        reset_dispatch_descriptor_pool_locked();
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_sorted_pair_sort)");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_sorted_pair_sort)");
        record_stable_pair_sort_by_value_then_key_locked(command_buffer_,
                                                         views,
                                                         histogram_s,
                                                         value_sort_plan,
                                                         key_sort_plan,
                                                         sort_wg.num_workgroups,
                                                         sort_wg.elems_per_wg);
        record_shader_to_host(command_buffer_);
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(pc_sorted_pair_sort)");
    }

    const gpu_arena_slice& sorted_keys = choose_sorted_keys(views);
    const gpu_arena_slice& sorted_values = choose_sorted_values(views);
    const std::uint64_t host_bytes = pair_count * sizeof(std::uint64_t);
    out.host_key_bytes.resize(static_cast<std::size_t>(host_bytes));
    out.host_value_bytes.resize(static_cast<std::size_t>(host_bytes));
    std::memcpy(out.host_key_bytes.data(),
                static_cast<const std::byte*>(sorted_keys.buffer->mapped) + sorted_keys.offset,
                static_cast<std::size_t>(host_bytes));
    std::memcpy(out.host_value_bytes.data(),
                static_cast<const std::byte*>(sorted_values.buffer->mapped) + sorted_values.offset,
                static_cast<std::size_t>(host_bytes));

    const bool sorted_keys_are_scratch = sorted_keys.buffer == &keys_scratch;
    const bool sorted_values_are_scratch = sorted_values.buffer == &values_scratch;
    out.keys = sorted_keys_are_scratch ? std::move(keys_scratch) : std::move(keys);
    out.values = sorted_values_are_scratch ? std::move(values_scratch) : std::move(values);
    out.pair_count = pair_count32;
    out.valid = true;
    arena.rewind(mark);
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_sorted_pair_post)");
    chaining_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    return out;
}

// — GPU sequence sort. Two submits (modelled on the index build):
//  1: upload seq+descs -> extract 64-bit canonical key per row -> radix sort.
//  host: build equal-key run ranges; seed `index` from the sorted row ids.
//  2: per-run bitonic tie-break over the full canonical compare; if any run is
//     oversized, continue in the same submit with global full-compare bitonic
//     stages over the padded permutation. Then download the permutation.

} // namespace bioseq::gpu::vulkan
