/**
 * @file batch.cppm
 * @brief Records one batched seed-to-coordinate descriptor and submission sequence.
 */
module;

#include <cstdint>
#include <libassert/assert.hpp>
#include <new>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

export module bioseq.gpu.vulkan.runtime:coordinate_pipeline.batch;

import :coordinate_pipeline.batch_preparation;
import :frontend_types;
import :support;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.parallelism.memory_admission;
import bioseq.instrumentation;
import std;

namespace bioseq::gpu::vulkan {

using coordinate_pipeline_detail::chain_range_push_constants;
using coordinate_pipeline_detail::gpu_coordinate_vote_job_wire;
using coordinate_pipeline_detail::gpu_seed_coordinate_wave_state;

/// @brief Runs the batched seed, sort, chain, and coordinate submission sequence.
/// @param[in] query_inputs Shared query minimizer storage and per-query records.
/// @param[in] jobs Ordered coordinate jobs referring to `query_inputs`.
/// @param[in] kmer_size K-mer span encoded into anchors.
/// @param[in] max_occ Maximum target occurrences emitted per minimizer.
/// @param[in] coordinate_algorithm Tiled-DP or diagonal-vote reduction.
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
/// @param[in] query_authority Device-query authorization consumed into the final receipt.
/// @return Per-job results, dispatch statistics, and causal operation receipt.
/// @throws vulkan_runtime_error When identity, allocation, submission, or policy checks fail.
gpu_seed_coordinate_batch_result
runtime::run_seed_to_coordinate_batch_roundtrip(const gpu_seed_query_inputs& query_inputs,
                                                std::span<const gpu_seed_coordinate_job> jobs,
                                                std::uint32_t kmer_size,
                                                std::int32_t max_occ,
                                                gpu_seed_coordinate_algorithm coordinate_algorithm,
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
                                                std::uint32_t probe_limit,
                                                device_query_sketch_authorization query_authority) {
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw vulkan_runtime_error(
            "run_seed_to_coordinate_results_roundtrip on unavailable device");
    }
    if (features_.max_workgroup_count_x == 0u) {
        throw vulkan_runtime_error("gpu seed-to-coordinate batch has zero workgroup limit");
    }
    if (coordinate_algorithm != gpu_seed_coordinate_algorithm::vulkan_tiled_dp &&
        coordinate_algorithm != gpu_seed_coordinate_algorithm::coordinate_vote) {
        throw vulkan_runtime_error("gpu seed-to-coordinate algorithm identity is invalid");
    }
    const bool coordinate_vote_selected =
        coordinate_algorithm == gpu_seed_coordinate_algorithm::coordinate_vote;
    if (query_inputs.device_query_records != nullptr) {
        throw vulkan_runtime_error(
            "gpu seed-to-coordinate device query records are not implemented");
    }

    const coordinate_pipeline_detail::coordinate_batch_shape batch_shape =
        coordinate_pipeline_detail::inspect_coordinate_batch_shape(jobs,
                                                                   query_inputs,
                                                                   coordinate_vote_selected,
                                                                   features_,
                                                                   kmer_size,
                                                                   max_occ,
                                                                   coordinate_algorithm,
                                                                   chain_key,
                                                                   max_dist_x,
                                                                   max_dist_y,
                                                                   bandwidth,
                                                                   max_iter,
                                                                   gap_penalty,
                                                                   skip_penalty,
                                                                   max_chain_skip,
                                                                   min_chain_score,
                                                                   min_seed_count,
                                                                   probe_limit);
    const bool grouped_soa_selected = batch_shape.grouped_soa_selected;
    const auto& shape_operation_configuration = batch_shape.operation_configuration;
    const gpu_grouped_soa_resident_view* common_shape_view = batch_shape.common_view;

    gpu_seed_coordinate_batch_result run{};
    run.results.resize(jobs.size());
    run.stats.problem_count = static_cast<std::uint64_t>(jobs.size());
    run.grouped_soa_direct.selected = grouped_soa_selected;
    run.grouped_soa_direct.problem_count = static_cast<std::uint64_t>(jobs.size());

    const coordinate_pipeline_detail::grouped_soa_resident_layout resident_layout =
        coordinate_pipeline_detail::validate_grouped_soa_resident(
            this, batch_shape, query_inputs, features_, kmer_size, max_occ, run);
    const std::uint64_t shape_control_padded_bytes = resident_layout.control_padded_bytes;
    const std::uint64_t shape_position_padded_bytes = resident_layout.position_padded_bytes;
    const std::uint32_t total_mc = query_inputs.minimizer_count;
    if (jobs.empty() || max_occ < 0 || (total_mc == 0u && !grouped_soa_selected)) {
        run.grouped_soa_direct.direct_count_scan_emit_completed = grouped_soa_selected;
        return run;
    }
    const coordinate_pipeline_detail::coordinate_query_layout query_layout =
        coordinate_pipeline_detail::validate_coordinate_query_layout(
            this, query_inputs, features_, grouped_soa_selected, kmer_size);
    const std::uint64_t required_min_words_bytes = query_layout.required_min_words_bytes;
    std::unique_ptr<runtime_pipeline_use_collector> shape_pipeline_collector;
    if (grouped_soa_selected) {
        bool authority_records_match =
            query_authority.query_records_.size() == query_inputs.host_query_records.size();
        for (std::size_t record_index = 0u;
             authority_records_match && record_index < query_authority.query_records_.size();
             ++record_index) {
            const auto& sealed = query_authority.query_records_[record_index];
            const auto& supplied = query_inputs.host_query_records[record_index];
            authority_records_match = sealed.minimizer_offset == supplied.minimizer_offset &&
                                      sealed.minimizer_count == supplied.minimizer_count &&
                                      sealed.query_len == supplied.query_len &&
                                      sealed.flags == supplied.flags;
        }
        if (!query_authority.is_valid() || query_authority.runtime_ != this ||
            query_authority.buffer_ != query_inputs.device_minimizer_words->buffer_handle ||
            query_authority.memory_identity_ !=
                vulkan_handle_identity(query_inputs.device_minimizer_words->memory) ||
            query_authority.buffer_bytes_ != query_inputs.device_minimizer_words->bytes ||
            query_authority.allocation_bytes_ !=
                query_inputs.device_minimizer_words->allocation_bytes ||
            query_authority.minimizer_count_ != total_mc ||
            query_authority.kmer_size_ != kmer_size ||
            query_authority.window_size_ != query_inputs.window_size ||
            query_authority.minimizer_word_kind_ != query_inputs.minimizer_word_kind ||
            query_authority.minimizer_words32_per_record_ !=
                query_inputs.minimizer_words32_per_record ||
            query_authority.hash_shift_ != query_inputs.hash_shift ||
            query_authority.pos_bits_ != query_inputs.pos_bits || !authority_records_match ||
            query_authority.device_uuid_ != features_.device_uuid) {
            throw vulkan_runtime_error(
                "gpu grouped-SoA direct route lacks matching one-shot query authority");
        }
        if (query_authority.buffer_ == common_shape_view->descriptors->buffer_handle ||
            query_authority.buffer_ == common_shape_view->controls->buffer_handle ||
            query_authority.buffer_ == common_shape_view->positions->buffer_handle) {
            throw vulkan_runtime_error(
                "gpu grouped-SoA query image aliases resident target storage");
        }
        run.operation_receipt.operation_identity_ = next_operation_identity_locked();
        run.operation_receipt.admit_records(jobs.size());
        shape_pipeline_collector = runtime_pipeline_use_collector::create(
            runtime_pipeline_operation_kind::grouped_soa_coordinate,
            jobs.size(),
            run.operation_receipt.operation_identity_);
    } else if (query_authority.is_valid()) {
        throw vulkan_runtime_error(
            "gpu query authority cannot be consumed by a source-direct index route");
    }
    const std::uint64_t min_words_bytes =
        query_inputs.device_minimizer_words == nullptr ? required_min_words_bytes : 0u;
    if (query_inputs.device_minimizer_words == nullptr &&
        query_inputs.host_minimizer_bytes.size() > static_cast<std::size_t>(min_words_bytes)) {
        throw vulkan_runtime_error("gpu seed-to-coordinate minimizer upload exceeds buffer");
    }

    const compute_pipeline count_pipe = get_compute_pipeline_locked(
        grouped_soa_selected ? shader_kind::grouped_soa_seed_count : shader_kind::seed_count,
        128u,
        grouped_soa_selected ? 3u : 1u);
    const compute_pipeline emit_pipe = get_compute_pipeline_locked(
        grouped_soa_selected ? shader_kind::grouped_soa_seed_emit : shader_kind::seed_emit,
        128u,
        grouped_soa_selected ? 3u : 1u);
    const compute_pipeline scanb =
        get_compute_pipeline_locked(shader_kind::prefix_scan_block, 1u, 1u);
    const compute_pipeline scana =
        get_compute_pipeline_locked(shader_kind::prefix_scan_add, 128u, 1u);
    const compute_pipeline range_pipe =
        get_compute_pipeline_locked(shader_kind::chain_range_build, 1u, 1u);
    const compute_pipeline coord_pipe =
        get_compute_pipeline_locked(shader_kind::coordinate_reduce, 128u, 1u);
    compute_pipeline vote_coord_pipe{};
    compute_pipeline vote_coord_batch_pipe{};
    if (coordinate_vote_selected) {
        vote_coord_pipe =
            get_compute_pipeline_locked(shader_kind::coordinate_vote_reduce, 128u, 1u);
        vote_coord_batch_pipe =
            get_compute_pipeline_locked(shader_kind::coordinate_vote_reduce_batch, 128u, 1u);
    }
    const chaining_pipeline chain_pipeline = create_pipeline_locked(chain_key);
    if (shape_pipeline_collector != nullptr) {
        // Registers each pipeline whose receipt contributes to the shape token.
        const auto prepare = [&shape_pipeline_collector](const compute_pipeline& pipe) {
            shape_pipeline_collector->prepare(
                make_shader_key(pipe.kind, 384u, pipe.local_size, pipe.max_span),
                pipe.metal_archive_receipt);
        };
        prepare(count_pipe);
        prepare(emit_pipe);
        prepare(scanb);
        prepare(scana);
        prepare(range_pipe);
        prepare(coord_pipe);
        shape_pipeline_collector->prepare(chain_pipeline.shader_key,
                                          chain_pipeline.metal_archive_receipt);
    }

    constexpr std::uint64_t qrec_stride = k_gpu_buffer_offset_alignment;
    const std::uint64_t query_record_bytes = std::max<std::uint64_t>(
        bioseq::common::saturating_mul(
            static_cast<std::uint64_t>(query_inputs.host_query_records.size()), qrec_stride),
        qrec_stride);

    gpu_arena& arena = reusable_gpu_arena();
    std::uint64_t submitted_waves = 0;
    for (std::size_t wave_begin = 0; wave_begin < jobs.size();) {
        const coordinate_pipeline_detail::coordinate_wave_plan wave =
            coordinate_pipeline_detail::prepare_coordinate_wave(jobs,
                                                                  query_inputs.host_query_records,
                                                                  wave_begin,
                                                                  coordinate_vote_selected,
                                                                  grouped_soa_selected,
                                                                  min_words_bytes,
                                                                  query_record_bytes);
        const std::size_t wave_end = wave.end;
        const std::size_t next_wave_begin = wave_end;
        const std::size_t wave_count = wave.count;
        if (!arena.ensure_capacity(wave.arena_bytes, 0u, 0u)) {
            throw vulkan_runtime_error("gpu seed-to-coordinate batch arena grow failed");
        }
        const gpu_arena_mark mark = arena.mark();
        try {
            auto* base = static_cast<std::byte*>(arena.device_buffer().mapped);
            const gpu_arena_slice qrec_s = arena.allocate_device(query_record_bytes, 16u);
            gpu_arena_slice minw_s{};
            if (query_inputs.device_minimizer_words == nullptr) {
                minw_s = arena.allocate_device(min_words_bytes, 16u);
            }
            if (!qrec_s.is_valid() ||
                (query_inputs.device_minimizer_words == nullptr && !minw_s.is_valid())) {
                throw vulkan_runtime_error("gpu seed-to-coordinate batch arena oversubscribed");
            }

            const coordinate_pipeline_detail::coordinate_minimizer_binding minimizer_binding =
                coordinate_pipeline_detail::prepare_coordinate_query_upload(
                    base, qrec_s, minw_s, query_inputs, qrec_stride);
            const VkBuffer min_words_buffer = minimizer_binding.buffer;
            const std::uint64_t min_words_offset = minimizer_binding.offset;
            const std::uint64_t min_words_range = minimizer_binding.range;

            std::vector<gpu_seed_coordinate_wave_state> states;
            states.reserve(wave_count);
            std::uint32_t seed_active_count = 0;
            for (std::size_t job_index = wave_begin; job_index < wave_end; ++job_index) {
                gpu_seed_coordinate_wave_state& state = states.emplace_back();
                state.result_index = job_index;
                state.job = jobs[job_index];
                state.query_record = query_inputs.host_query_records[state.job.query_index];
                if (grouped_soa_selected) {
                    coordinate_pipeline_detail::initialize_grouped_soa_job_audit(
                        state,
                        *common_shape_view,
                        query_inputs.device_minimizer_words,
                        shape_operation_configuration,
                        run.operation_receipt.operation_identity_,
                        query_authority.identity_,
                        query_authority.query_tile_counts_[state.job.query_index],
                        query_authority.dispatch_stats_);
                }
                state.minimizer_count = state.query_record.minimizer_count;
                state.query_len = state.query_record.query_len;
                if (shape_pipeline_collector != nullptr) {
                    shape_pipeline_collector->set_shape_input(state.result_index,
                                                              state.minimizer_count,
                                                              state.job.target_len,
                                                              state.job.target_position_offset);
                }
                state.num_blocks = std::max<std::uint32_t>(
                    bioseq::common::ceil_div(state.minimizer_count, k_prefix_scan_block_size), 1u);
                state.seed_groups = std::max<std::uint32_t>(
                    1u,
                    grouped_soa_selected
                        ? 1u
                        : std::min(bioseq::common::ceil_div(state.minimizer_count, 128u),
                                   features_.max_workgroup_count_x));
                state.skip_seed = state.minimizer_count == 0u ||
                                  (!grouped_soa_selected && state.job.position_count == 0u);
                if (grouped_soa_selected && state.minimizer_count == 0u) {
                    run.operation_receipt.records_[state.result_index].audit =
                        state.operation_audit;
                }
                const std::uint64_t count_word_count =
                    grouped_soa_selected ? static_cast<std::uint64_t>(state.minimizer_count) + 1u
                                         : std::max<std::uint64_t>(state.minimizer_count, 1u);
                state.counts_s = arena.allocate_device(count_word_count * 4u, 16u);
                state.prefix_s = arena.allocate_device(
                    std::max<std::uint64_t>(state.minimizer_count, 1u) * 4u, 16u);
                state.blocks_s = arena.allocate_device(std::uint64_t{state.num_blocks} * 4u, 16u);
                if (!state.counts_s.is_valid() || !state.prefix_s.is_valid() ||
                    !state.blocks_s.is_valid()) {
                    throw vulkan_runtime_error("gpu seed-to-coordinate batch arena oversubscribed");
                }
                if (grouped_soa_selected) {
                    const std::uint32_t zero_status = 0u;
                    std::memcpy(base + state.counts_s.offset +
                                    static_cast<std::uint64_t>(state.minimizer_count) *
                                        sizeof(std::uint32_t),
                                &zero_status,
                                sizeof(zero_status));
                }
                coordinate_pipeline_detail::prepare_seed_push_constants(state,
                                                                           query_inputs,
                                                                           grouped_soa_selected,
                                                                           kmer_size,
                                                                           max_occ,
                                                                           total_mc,
                                                                           resident_layout);
                if (!state.skip_seed) {
                    seed_active_count += 1u;
                }
            }

            if (seed_active_count == 0u) {
                arena.rewind(mark);
                wave_begin = next_wave_begin;
                continue;
            }

            if (coordinate_vote_selected) {
                std::vector<gpu_seed_coordinate_wave_state*> active_states;
                active_states.reserve(seed_active_count);
                for (gpu_seed_coordinate_wave_state& state : states) {
                    if (!state.skip_seed) {
                        active_states.push_back(&state);
                    }
                }

                const gpu_seed_coordinate_wave_state* first_active =
                    active_states.empty() ? nullptr : active_states.front();
                const bool same_target_wave =
                    coordinate_pipeline_detail::has_same_active_coordinate_target(active_states);

                if (same_target_wave) {
                    const std::uint32_t total_blocks = std::max<std::uint32_t>(
                        bioseq::common::ceil_div(total_mc, k_prefix_scan_block_size), 1u);
                    const std::uint64_t counts_bytes =
                        std::max<std::uint64_t>(std::uint64_t{total_mc} * 4u, 4u);
                    const std::uint64_t blocks_bytes =
                        std::max<std::uint64_t>(std::uint64_t{total_blocks} * 4u, 4u);
                    const std::uint64_t dense_qrec_bytes =
                        std::max<std::uint64_t>(static_cast<std::uint64_t>(active_states.size()) *
                                                    sizeof(gpu_seed_query_record_wire),
                                                sizeof(gpu_seed_query_record_wire));

                    buffer batch_counts_buf =
                        create_device_buffer(counts_bytes, k_compute_buffer_usage);
                    buffer batch_prefix_buf =
                        create_device_buffer(counts_bytes, k_compute_buffer_usage);
                    buffer batch_blocks_buf =
                        create_device_buffer(blocks_bytes, k_compute_buffer_usage);
                    buffer batch_qrec_buf =
                        create_device_buffer(dense_qrec_bytes, k_compute_buffer_usage);
                    if (batch_counts_buf.mapped == nullptr || batch_prefix_buf.mapped == nullptr ||
                        batch_blocks_buf.mapped == nullptr || batch_qrec_buf.mapped == nullptr) {
                        throw vulkan_runtime_error(
                            "gpu seed-to-coordinate multi-record batch non-unified memory");
                    }
                    std::memset(batch_counts_buf.mapped,
                                0,
                                static_cast<std::size_t>(batch_counts_buf.bytes));
                    std::memset(batch_prefix_buf.mapped,
                                0,
                                static_cast<std::size_t>(batch_prefix_buf.bytes));
                    std::memset(batch_blocks_buf.mapped,
                                0,
                                static_cast<std::size_t>(batch_blocks_buf.bytes));
                    std::memset(
                        batch_qrec_buf.mapped, 0, static_cast<std::size_t>(batch_qrec_buf.bytes));
                    for (std::size_t active_index = 0; active_index < active_states.size();
                         ++active_index) {
                        std::memcpy(static_cast<std::byte*>(batch_qrec_buf.mapped) +
                                        active_index * sizeof(gpu_seed_query_record_wire),
                                    &active_states[active_index]->query_record,
                                    sizeof(gpu_seed_query_record_wire));
                    }

                    seed_push_constants batch_seed_push = first_active->seed_push;
                    batch_seed_push.record_count = static_cast<std::uint32_t>(active_states.size());
                    batch_seed_push.emit_mode = 0u;
                    const std::array<VkDescriptorBufferInfo, 8> count_infos{{
                        {min_words_buffer, min_words_offset, min_words_range},
                        {first_active->job.slots->buffer_handle, 0, VK_WHOLE_SIZE},
                        {first_active->job.positions->buffer_handle, 0, VK_WHOLE_SIZE},
                        {batch_qrec_buf.buffer_handle, 0, batch_qrec_buf.bytes},
                        {batch_counts_buf.buffer_handle, 0, batch_counts_buf.bytes},
                        {batch_prefix_buf.buffer_handle, 0, batch_prefix_buf.bytes},
                        {batch_counts_buf.buffer_handle, 0, batch_counts_buf.bytes},
                        {batch_counts_buf.buffer_handle, 0, batch_counts_buf.bytes},
                    }};
                    const std::array<VkDescriptorBufferInfo, 3> scan_infos{{
                        {batch_counts_buf.buffer_handle, 0, batch_counts_buf.bytes},
                        {batch_prefix_buf.buffer_handle, 0, batch_prefix_buf.bytes},
                        {batch_blocks_buf.buffer_handle, 0, batch_blocks_buf.bytes},
                    }};
                    scan_push_constants scan{total_mc, k_prefix_scan_block_size, total_blocks, 0u};

                    reset_dispatch_descriptor_pool_locked();
                    check_vk(vkResetCommandBuffer(command_buffer_, 0),
                             "reset(pc_seed_coord_mrec1)");
                    VkCommandBufferBeginInfo begin{};
                    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                    check_vk(vkBeginCommandBuffer(command_buffer_, &begin),
                             "begin(pc_seed_coord_mrec1)");
                    record_host_to_shader(command_buffer_);
                    record_compute_dispatch_locked(
                        command_buffer_,
                        count_pipe,
                        count_infos,
                        &batch_seed_push,
                        sizeof(batch_seed_push),
                        static_cast<std::uint32_t>(active_states.size()));
                    run.stats.seed_count_dispatches += 1u;
                    record_compute_barrier(command_buffer_);
                    record_compute_dispatch_locked(
                        command_buffer_, scanb, scan_infos, &scan, sizeof(scan), total_blocks);
                    record_compute_barrier(command_buffer_);
                    record_compute_dispatch_locked(
                        command_buffer_, scana, scan_infos, &scan, sizeof(scan), 1u);
                    record_compute_barrier(command_buffer_);
                    scan.pass = 1u;
                    record_compute_dispatch_locked(command_buffer_,
                                                   scana,
                                                   scan_infos,
                                                   &scan,
                                                   sizeof(scan),
                                                   bioseq::common::ceil_div(total_mc, 128u));
                    record_shader_to_host(command_buffer_);
                    submit_and_wait(
                        device_, queue_, command_buffer_, fence_, "submit(pc_seed_coord_mrec1)");

                    std::uint32_t wave_total_prefix = 0u;
                    std::uint32_t wave_total_count = 0u;
                    std::memcpy(&wave_total_prefix,
                                static_cast<std::byte*>(batch_prefix_buf.mapped) +
                                    std::uint64_t{total_mc - 1u} * 4u,
                                4u);
                    std::memcpy(&wave_total_count,
                                static_cast<std::byte*>(batch_counts_buf.mapped) +
                                    std::uint64_t{total_mc - 1u} * 4u,
                                4u);
                    const std::uint64_t wave_total_anchors =
                        static_cast<std::uint64_t>(wave_total_prefix) + wave_total_count;
                    if (wave_total_anchors > std::numeric_limits<std::uint32_t>::max()) {
                        throw vulkan_runtime_error(
                            "gpu seed-to-coordinate multi-record anchor overflow");
                    }

                    std::uint32_t anchor_active_count = 0;
                    for (gpu_seed_coordinate_wave_state* state : active_states) {
                        const std::uint32_t begin_min = state->query_record.minimizer_offset;
                        const std::uint32_t end_min =
                            begin_min + state->query_record.minimizer_count - 1u;
                        std::uint32_t anchor_begin = 0u;
                        std::uint32_t anchor_end_prefix = 0u;
                        std::uint32_t anchor_end_count = 0u;
                        std::memcpy(&anchor_begin,
                                    static_cast<std::byte*>(batch_prefix_buf.mapped) +
                                        std::uint64_t{begin_min} * 4u,
                                    4u);
                        std::memcpy(&anchor_end_prefix,
                                    static_cast<std::byte*>(batch_prefix_buf.mapped) +
                                        std::uint64_t{end_min} * 4u,
                                    4u);
                        std::memcpy(&anchor_end_count,
                                    static_cast<std::byte*>(batch_counts_buf.mapped) +
                                        std::uint64_t{end_min} * 4u,
                                    4u);
                        const std::uint64_t anchor_end =
                            static_cast<std::uint64_t>(anchor_end_prefix) + anchor_end_count;
                        if (anchor_end < anchor_begin) {
                            throw vulkan_runtime_error(
                                "gpu seed-to-coordinate multi-record invalid prefix");
                        }
                        state->total_anchors =
                            anchor_end - static_cast<std::uint64_t>(anchor_begin);
                        if (state->total_anchors == 0u) {
                            continue;
                        }
                        if (state->total_anchors > std::numeric_limits<std::uint32_t>::max()) {
                            throw vulkan_runtime_error(
                                "gpu seed-to-coordinate multi-record per-query overflow");
                        }
                        state->total_anchors_u32 = static_cast<std::uint32_t>(state->total_anchors);
                        state->anchor_offset = anchor_begin;
                        state->has_anchors = true;
                        anchor_active_count += 1u;
                    }

                    if (anchor_active_count == 0u || wave_total_anchors == 0u) {
                        arena.rewind(mark);
                        submitted_waves += 1u;
                        run.stats.wave_count += 1u;
                        wave_begin = next_wave_begin;
                        continue;
                    }

                    const std::uint64_t anchor_bytes =
                        wave_total_anchors * 3u * sizeof(std::uint64_t);
                    buffer batch_anchors_buf =
                        create_device_buffer(anchor_bytes, k_compute_buffer_usage);
                    buffer batch_anchor_keys_buf = create_device_buffer(
                        wave_total_anchors * sizeof(std::uint64_t), k_compute_buffer_usage);
                    if (batch_anchors_buf.mapped == nullptr ||
                        batch_anchor_keys_buf.mapped == nullptr) {
                        throw vulkan_runtime_error(
                            "gpu seed-to-coordinate multi-record anchor memory");
                    }
                    std::memset(batch_anchors_buf.mapped,
                                0,
                                static_cast<std::size_t>(batch_anchors_buf.bytes));
                    std::memset(batch_anchor_keys_buf.mapped,
                                0,
                                static_cast<std::size_t>(batch_anchor_keys_buf.bytes));

                    std::vector<gpu_coordinate_vote_job_wire> vote_jobs;
                    vote_jobs.reserve(anchor_active_count);
                    for (gpu_seed_coordinate_wave_state* state : active_states) {
                        if (!state->has_anchors) {
                            continue;
                        }
                        state->vote_result_index = static_cast<std::uint32_t>(vote_jobs.size());
                        vote_jobs.push_back(gpu_coordinate_vote_job_wire{
                            .anchor_offset = state->anchor_offset,
                            .anchor_count = state->total_anchors_u32,
                            .query_len_lo =
                                static_cast<std::uint32_t>(state->query_len & 0xffffffffull),
                            .query_len_hi = static_cast<std::uint32_t>(state->query_len >> 32),
                        });
                    }
                    const std::uint64_t vote_job_bytes =
                        std::max<std::uint64_t>(static_cast<std::uint64_t>(vote_jobs.size()) *
                                                    sizeof(gpu_coordinate_vote_job_wire),
                                                sizeof(gpu_coordinate_vote_job_wire));
                    const std::uint64_t vote_result_bytes =
                        std::max<std::uint64_t>(static_cast<std::uint64_t>(vote_jobs.size()) *
                                                    sizeof(gpu_coordinate_result_wire),
                                                sizeof(gpu_coordinate_result_wire));
                    buffer vote_jobs_buf =
                        create_device_buffer(vote_job_bytes, k_compute_buffer_usage);
                    buffer vote_results_buf =
                        create_device_buffer(vote_result_bytes, k_compute_buffer_usage);
                    if (vote_jobs_buf.mapped == nullptr || vote_results_buf.mapped == nullptr) {
                        throw vulkan_runtime_error(
                            "gpu seed-to-coordinate multi-record vote memory");
                    }
                    std::memset(
                        vote_jobs_buf.mapped, 0, static_cast<std::size_t>(vote_jobs_buf.bytes));
                    std::memset(vote_results_buf.mapped,
                                0,
                                static_cast<std::size_t>(vote_results_buf.bytes));
                    std::memcpy(vote_jobs_buf.mapped,
                                vote_jobs.data(),
                                vote_jobs.size() * sizeof(gpu_coordinate_vote_job_wire));

                    reset_dispatch_descriptor_pool_locked();
                    check_vk(vkResetCommandBuffer(command_buffer_, 0),
                             "reset(pc_seed_coord_mrec2)");
                    begin = VkCommandBufferBeginInfo{};
                    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                    check_vk(vkBeginCommandBuffer(command_buffer_, &begin),
                             "begin(pc_seed_coord_mrec2)");
                    record_host_to_shader(command_buffer_);
                    batch_seed_push.emit_mode = 1u;
                    const std::array<VkDescriptorBufferInfo, 8> emit_infos{{
                        {min_words_buffer, min_words_offset, min_words_range},
                        {first_active->job.slots->buffer_handle, 0, VK_WHOLE_SIZE},
                        {first_active->job.positions->buffer_handle, 0, VK_WHOLE_SIZE},
                        {batch_qrec_buf.buffer_handle, 0, batch_qrec_buf.bytes},
                        {batch_counts_buf.buffer_handle, 0, batch_counts_buf.bytes},
                        {batch_prefix_buf.buffer_handle, 0, batch_prefix_buf.bytes},
                        {batch_anchors_buf.buffer_handle, 0, batch_anchors_buf.bytes},
                        {batch_anchor_keys_buf.buffer_handle, 0, batch_anchor_keys_buf.bytes},
                    }};
                    record_compute_dispatch_locked(
                        command_buffer_,
                        emit_pipe,
                        emit_infos,
                        &batch_seed_push,
                        sizeof(batch_seed_push),
                        static_cast<std::uint32_t>(active_states.size()));
                    run.stats.seed_emit_dispatches += 1u;
                    record_compute_barrier(command_buffer_);
                    const std::array<VkDescriptorBufferInfo, 6> coord_infos{{
                        {batch_anchors_buf.buffer_handle, 0, batch_anchors_buf.bytes},
                        {vote_jobs_buf.buffer_handle, 0, vote_jobs_buf.bytes},
                        {batch_anchor_keys_buf.buffer_handle, 0, batch_anchor_keys_buf.bytes},
                        {vote_results_buf.buffer_handle, 0, vote_results_buf.bytes},
                        {vote_results_buf.buffer_handle, 0, vote_results_buf.bytes},
                        {vote_results_buf.buffer_handle, 0, vote_results_buf.bytes},
                    }};
                    gpu_coordinate_reduce_push coord_push{
                        .anchor_count = static_cast<std::uint32_t>(vote_jobs.size()),
                        .query_len_lo = 0u,
                        .query_len_hi = 0u,
                        .min_chain_score = min_chain_score,
                        .min_seed_count = min_seed_count,
                        .probe_limit = probe_limit,
                        .reserved = 0u,
                    };
                    record_compute_dispatch_locked(command_buffer_,
                                                   vote_coord_batch_pipe,
                                                   coord_infos,
                                                   &coord_push,
                                                   sizeof(coord_push),
                                                   static_cast<std::uint32_t>(vote_jobs.size()));
                    run.stats.coordinate_vote_dispatches += 1u;
                    record_shader_to_host(command_buffer_);
                    submit_and_wait(
                        device_, queue_, command_buffer_, fence_, "submit(pc_seed_coord_mrec2)");

                    for (gpu_seed_coordinate_wave_state* state : active_states) {
                        if (state->has_anchors) {
                            std::memcpy(&run.results[state->result_index],
                                        static_cast<std::byte*>(vote_results_buf.mapped) +
                                            static_cast<std::uint64_t>(state->vote_result_index) *
                                                sizeof(gpu_coordinate_result_wire),
                                        sizeof(gpu_coordinate_result_wire));
                        }
                    }

                    arena.rewind(mark);
                    submitted_waves += 1u;
                    run.stats.wave_count += 1u;
                    wave_begin = next_wave_begin;
                    continue;
                }
            }

            reset_dispatch_descriptor_pool_locked();
            if (shape_pipeline_collector != nullptr) {
                shape_pipeline_collector->burn_before_reset(command_buffer_);
            }
            check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_seed_coord_batch1)");
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_seed_coord_batch1)");
            if (shape_pipeline_collector != nullptr) {
                shape_pipeline_collector->begin(command_buffer_);
            }
            record_host_to_shader(command_buffer_);
            for (gpu_seed_coordinate_wave_state& state : states) {
                if (state.skip_seed) {
                    continue;
                }
                if (grouped_soa_selected) {
                    const gpu_grouped_soa_resident_view& view = *state.job.grouped_soa;
                    const std::array<VkDescriptorBufferInfo, 9> seed_infos{{
                        {min_words_buffer, min_words_offset, min_words_range},
                        {view.descriptors->buffer_handle, 0, view.descriptor_bytes},
                        {view.controls->buffer_handle, 0, shape_control_padded_bytes},
                        {view.positions->buffer_handle, 0, shape_position_padded_bytes},
                        {qrec_s.buffer->buffer_handle,
                         qrec_s.offset + std::uint64_t{state.job.query_index} * qrec_stride,
                         sizeof(gpu_seed_query_record_wire)},
                        {state.counts_s.buffer->buffer_handle,
                         state.counts_s.offset,
                         state.counts_s.size},
                        {state.prefix_s.buffer->buffer_handle,
                         state.prefix_s.offset,
                         state.prefix_s.size},
                        {state.counts_s.buffer->buffer_handle,
                         state.counts_s.offset,
                         state.counts_s.size},
                        {state.counts_s.buffer->buffer_handle,
                         state.counts_s.offset,
                         state.counts_s.size},
                    }};
                    record_compute_dispatch_locked(command_buffer_,
                                                   count_pipe,
                                                   seed_infos,
                                                   &state.grouped_soa_seed_push,
                                                   sizeof(state.grouped_soa_seed_push),
                                                   1u,
                                                   shape_pipeline_collector.get(),
                                                   state.result_index);
                    run.grouped_soa_direct.count_dispatches += 1u;
                    state.operation_audit.direct_count_dispatches += 1u;
                } else {
                    const std::array<VkDescriptorBufferInfo, 8> seed_infos{{
                        {min_words_buffer, min_words_offset, min_words_range},
                        {state.job.slots->buffer_handle, 0, VK_WHOLE_SIZE},
                        {state.job.positions->buffer_handle, 0, VK_WHOLE_SIZE},
                        {qrec_s.buffer->buffer_handle,
                         qrec_s.offset + std::uint64_t{state.job.query_index} * qrec_stride,
                         sizeof(gpu_seed_query_record_wire)},
                        {state.counts_s.buffer->buffer_handle,
                         state.counts_s.offset,
                         state.counts_s.size},
                        {state.prefix_s.buffer->buffer_handle,
                         state.prefix_s.offset,
                         state.prefix_s.size},
                        {state.counts_s.buffer->buffer_handle,
                         state.counts_s.offset,
                         state.counts_s.size},
                        {state.counts_s.buffer->buffer_handle,
                         state.counts_s.offset,
                         state.counts_s.size},
                    }};
                    record_compute_dispatch_locked(command_buffer_,
                                                   count_pipe,
                                                   seed_infos,
                                                   &state.seed_push,
                                                   sizeof(state.seed_push),
                                                   state.seed_groups);
                }
                run.stats.seed_count_dispatches += 1u;
            }
            record_compute_barrier(command_buffer_);
            for (gpu_seed_coordinate_wave_state& state : states) {
                if (state.skip_seed) {
                    continue;
                }
                const std::array<VkDescriptorBufferInfo, 3> scan_infos{{
                    {state.counts_s.buffer->buffer_handle,
                     state.counts_s.offset,
                     state.counts_s.size},
                    {state.prefix_s.buffer->buffer_handle,
                     state.prefix_s.offset,
                     state.prefix_s.size},
                    {state.blocks_s.buffer->buffer_handle,
                     state.blocks_s.offset,
                     state.blocks_s.size},
                }};
                scan_push_constants scan{
                    state.minimizer_count, k_prefix_scan_block_size, state.num_blocks, 0u};
                record_compute_dispatch_locked(command_buffer_,
                                               scanb,
                                               scan_infos,
                                               &scan,
                                               sizeof(scan),
                                               state.num_blocks,
                                               shape_pipeline_collector.get(),
                                               state.result_index);
                if (grouped_soa_selected) {
                    state.operation_audit.direct_scan_dispatches += 1u;
                }
            }
            record_compute_barrier(command_buffer_);
            for (gpu_seed_coordinate_wave_state& state : states) {
                if (state.skip_seed) {
                    continue;
                }
                const std::array<VkDescriptorBufferInfo, 3> scan_infos{{
                    {state.counts_s.buffer->buffer_handle,
                     state.counts_s.offset,
                     state.counts_s.size},
                    {state.prefix_s.buffer->buffer_handle,
                     state.prefix_s.offset,
                     state.prefix_s.size},
                    {state.blocks_s.buffer->buffer_handle,
                     state.blocks_s.offset,
                     state.blocks_s.size},
                }};
                scan_push_constants scan{
                    state.minimizer_count, k_prefix_scan_block_size, state.num_blocks, 0u};
                record_compute_dispatch_locked(command_buffer_,
                                               scana,
                                               scan_infos,
                                               &scan,
                                               sizeof(scan),
                                               1u,
                                               shape_pipeline_collector.get(),
                                               state.result_index);
                if (grouped_soa_selected) {
                    state.operation_audit.direct_scan_dispatches += 1u;
                }
            }
            record_compute_barrier(command_buffer_);
            for (gpu_seed_coordinate_wave_state& state : states) {
                if (state.skip_seed) {
                    continue;
                }
                const std::array<VkDescriptorBufferInfo, 3> scan_infos{{
                    {state.counts_s.buffer->buffer_handle,
                     state.counts_s.offset,
                     state.counts_s.size},
                    {state.prefix_s.buffer->buffer_handle,
                     state.prefix_s.offset,
                     state.prefix_s.size},
                    {state.blocks_s.buffer->buffer_handle,
                     state.blocks_s.offset,
                     state.blocks_s.size},
                }};
                scan_push_constants scan{
                    state.minimizer_count, k_prefix_scan_block_size, state.num_blocks, 1u};
                record_compute_dispatch_locked(
                    command_buffer_,
                    scana,
                    scan_infos,
                    &scan,
                    sizeof(scan),
                    bioseq::common::ceil_div(state.minimizer_count, 128u),
                    shape_pipeline_collector.get(),
                    state.result_index);
                if (grouped_soa_selected) {
                    state.operation_audit.direct_scan_dispatches += 1u;
                }
            }
            record_shader_to_host(command_buffer_);
            submit_and_wait(device_,
                            queue_,
                            command_buffer_,
                            fence_,
                            "submit(pc_seed_coord_batch1)",
                            shape_pipeline_collector.get());
            if (grouped_soa_selected) {
                const std::uint64_t receipt_identity = next_operation_identity_locked();
                for (gpu_seed_coordinate_wave_state& state : states) {
                    if (!state.skip_seed) {
                        state.operation_audit.count_submit_wait_identity = receipt_identity;
                    }
                }
            }

            std::uint32_t anchor_active_count = 0;
            for (gpu_seed_coordinate_wave_state& state : states) {
                if (state.skip_seed) {
                    continue;
                }
                if (grouped_soa_selected) {
                    std::uint32_t status = 0u;
                    std::memcpy(&status,
                                base + state.counts_s.offset +
                                    static_cast<std::uint64_t>(state.minimizer_count) *
                                        sizeof(std::uint32_t),
                                sizeof(status));
                    run.grouped_soa_direct.count_status_checks += 1u;
                    run.grouped_soa_direct.status_or |= status;
                    state.operation_audit.count_status_checks += 1u;
                    state.operation_audit.status_or |= status;
                    if (status != 0u) {
                        throw vulkan_runtime_error(fmt::format(
                            "gpu grouped-SoA count rejected job {} with status 0x{:08x}",
                            state.result_index,
                            status));
                    }
                }
                std::uint32_t plast = 0;
                std::uint32_t clast = 0;
                std::memcpy(&plast,
                            base + state.prefix_s.offset +
                                std::uint64_t{state.minimizer_count - 1u} * 4u,
                            4u);
                std::memcpy(&clast,
                            base + state.counts_s.offset +
                                std::uint64_t{state.minimizer_count - 1u} * 4u,
                            4u);
                state.total_anchors = static_cast<std::uint64_t>(plast) + clast;
                if (grouped_soa_selected) {
                    state.operation_audit.anchor_count = state.total_anchors;
                    shape_pipeline_collector->set_shape_outcome(
                        state.result_index, state.total_anchors, 0u);
                }
                if (state.total_anchors == 0u) {
                    if (grouped_soa_selected) {
                        run.operation_receipt.records_[state.result_index].audit =
                            state.operation_audit;
                    }
                    continue;
                }
                if (state.total_anchors > std::numeric_limits<std::uint32_t>::max()) {
                    throw vulkan_runtime_error(
                        "gpu seed-to-coordinate batch anchor count overflow");
                }
                state.total_anchors_u32 = static_cast<std::uint32_t>(state.total_anchors);
                state.anchor_wg =
                    plan_workgroups(state.total_anchors, features_.max_workgroup_count_x);
                const std::uint64_t anchor_u64 = state.total_anchors * 3u;
                state.anchors_buf = create_device_buffer(anchor_u64 * 8u, k_compute_buffer_usage);
                state.anchor_keys_buf =
                    create_device_buffer(state.total_anchors * 8u, k_compute_buffer_usage);
                if (coordinate_vote_selected) {
                    state.result_buf = create_device_buffer(sizeof(gpu_coordinate_result_wire),
                                                            k_compute_buffer_usage);
                    if (state.anchors_buf.mapped == nullptr ||
                        state.anchor_keys_buf.mapped == nullptr ||
                        state.result_buf.mapped == nullptr) {
                        throw vulkan_runtime_error(
                            "gpu seed-to-coordinate vote batch non-unified memory");
                    }
                    std::memset(state.result_buf.mapped,
                                0,
                                static_cast<std::size_t>(state.result_buf.bytes));
                    state.has_anchors = true;
                    anchor_active_count += 1u;
                    continue;
                }
                state.anchors_scratch_buf =
                    create_device_buffer(anchor_u64 * 8u, k_compute_buffer_usage);
                state.anchor_keys_scratch_buf =
                    create_device_buffer(state.total_anchors * 8u, k_compute_buffer_usage);
                state.anchor_histogram_buf =
                    create_device_buffer(std::uint64_t{state.anchor_wg.num_workgroups} * 256u * 4u,
                                         k_compute_buffer_usage);
                state.ranges_buf = create_device_buffer(
                    std::max<std::uint64_t>(state.total_anchors * sizeof(gpu_chain_range_wire), 8u),
                    k_compute_buffer_usage);
                state.range_count_buf = create_device_buffer(8u, k_compute_buffer_usage);
                state.compacted_anchors_buf =
                    create_device_buffer(anchor_u64 * 8u, k_compute_buffer_usage);
                state.span_buf = create_device_buffer(sizeof(gpu_chain_range_build_span_wire),
                                                      k_compute_buffer_usage);
                if (state.anchors_buf.mapped == nullptr ||
                    state.anchor_keys_buf.mapped == nullptr ||
                    state.anchors_scratch_buf.mapped == nullptr ||
                    state.anchor_keys_scratch_buf.mapped == nullptr ||
                    state.anchor_histogram_buf.mapped == nullptr ||
                    state.ranges_buf.mapped == nullptr || state.range_count_buf.mapped == nullptr ||
                    state.compacted_anchors_buf.mapped == nullptr ||
                    state.span_buf.mapped == nullptr) {
                    throw vulkan_runtime_error(
                        "gpu seed-to-coordinate batch non-unified anchor memory");
                }
                const gpu_chain_range_build_span_wire span{
                    .anchor_lo = 0u,
                    .anchor_hi = state.total_anchors_u32,
                    .range_lo = 0u,
                    .range_out = 0u,
                };
                std::memcpy(state.span_buf.mapped, &span, sizeof(span));
                std::memset(state.range_count_buf.mapped, 0, 8u);
                state.has_anchors = true;
                anchor_active_count += 1u;
            }

            if (anchor_active_count == 0u) {
                arena.rewind(mark);
                submitted_waves += 1u;
                run.stats.wave_count += 1u;
                wave_begin = next_wave_begin;
                continue;
            }

            reset_dispatch_descriptor_pool_locked();
            if (shape_pipeline_collector != nullptr) {
                shape_pipeline_collector->burn_before_reset(command_buffer_);
            }
            check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_seed_coord_batch2)");
            begin = VkCommandBufferBeginInfo{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_seed_coord_batch2)");
            if (shape_pipeline_collector != nullptr) {
                shape_pipeline_collector->begin(command_buffer_);
            }
            record_host_to_shader(command_buffer_);
            for (gpu_seed_coordinate_wave_state& state : states) {
                if (!state.has_anchors) {
                    continue;
                }
                gpu_arena_slice anchors_s = state.anchors_slice();
                gpu_arena_slice anchor_keys_s = state.anchor_keys_slice();
                if (grouped_soa_selected) {
                    gpu_grouped_soa_seed_push emit_push = state.grouped_soa_seed_push;
                    emit_push.emit_mode = 1u;
                    emit_push.anchor_capacity = state.total_anchors_u32;
                    const gpu_grouped_soa_resident_view& view = *state.job.grouped_soa;
                    const std::array<VkDescriptorBufferInfo, 9> seed_infos{{
                        {min_words_buffer, min_words_offset, min_words_range},
                        {view.descriptors->buffer_handle, 0, view.descriptor_bytes},
                        {view.controls->buffer_handle, 0, shape_control_padded_bytes},
                        {view.positions->buffer_handle, 0, shape_position_padded_bytes},
                        {qrec_s.buffer->buffer_handle,
                         qrec_s.offset + std::uint64_t{state.job.query_index} * qrec_stride,
                         sizeof(gpu_seed_query_record_wire)},
                        {state.counts_s.buffer->buffer_handle,
                         state.counts_s.offset,
                         state.counts_s.size},
                        {state.prefix_s.buffer->buffer_handle,
                         state.prefix_s.offset,
                         state.prefix_s.size},
                        {anchors_s.buffer->buffer_handle, anchors_s.offset, anchors_s.size},
                        {anchor_keys_s.buffer->buffer_handle,
                         anchor_keys_s.offset,
                         anchor_keys_s.size},
                    }};
                    record_compute_dispatch_locked(command_buffer_,
                                                   emit_pipe,
                                                   seed_infos,
                                                   &emit_push,
                                                   sizeof(emit_push),
                                                   1u,
                                                   shape_pipeline_collector.get(),
                                                   state.result_index);
                    run.grouped_soa_direct.emit_dispatches += 1u;
                    state.operation_audit.direct_emit_dispatches += 1u;
                } else {
                    seed_push_constants emit_push = state.seed_push;
                    emit_push.emit_mode = 1u;
                    const std::array<VkDescriptorBufferInfo, 8> seed_infos{{
                        {min_words_buffer, min_words_offset, min_words_range},
                        {state.job.slots->buffer_handle, 0, VK_WHOLE_SIZE},
                        {state.job.positions->buffer_handle, 0, VK_WHOLE_SIZE},
                        {qrec_s.buffer->buffer_handle,
                         qrec_s.offset + std::uint64_t{state.job.query_index} * qrec_stride,
                         sizeof(gpu_seed_query_record_wire)},
                        {state.counts_s.buffer->buffer_handle,
                         state.counts_s.offset,
                         state.counts_s.size},
                        {state.prefix_s.buffer->buffer_handle,
                         state.prefix_s.offset,
                         state.prefix_s.size},
                        {anchors_s.buffer->buffer_handle, anchors_s.offset, anchors_s.size},
                        {anchor_keys_s.buffer->buffer_handle,
                         anchor_keys_s.offset,
                         anchor_keys_s.size},
                    }};
                    record_compute_dispatch_locked(command_buffer_,
                                                   emit_pipe,
                                                   seed_infos,
                                                   &emit_push,
                                                   sizeof(emit_push),
                                                   state.seed_groups);
                }
                run.stats.seed_emit_dispatches += 1u;
            }
            record_compute_barrier(command_buffer_);
            if (grouped_soa_selected) {
                record_shader_to_host(command_buffer_);
                submit_and_wait(device_,
                                queue_,
                                command_buffer_,
                                fence_,
                                "submit(pc_grouped_soa_seed_coord_emit)",
                                shape_pipeline_collector.get());
                const std::uint64_t receipt_identity = next_operation_identity_locked();
                for (gpu_seed_coordinate_wave_state& state : states) {
                    if (!state.has_anchors) {
                        continue;
                    }
                    state.operation_audit.emit_submit_wait_identity = receipt_identity;
                    std::uint32_t status = 0u;
                    std::memcpy(&status,
                                base + state.counts_s.offset +
                                    static_cast<std::uint64_t>(state.minimizer_count) *
                                        sizeof(std::uint32_t),
                                sizeof(status));
                    run.grouped_soa_direct.emit_status_checks += 1u;
                    run.grouped_soa_direct.status_or |= status;
                    state.operation_audit.emit_status_checks += 1u;
                    state.operation_audit.status_or |= status;
                    if (status != 0u) {
                        throw vulkan_runtime_error(
                            fmt::format("gpu grouped-SoA emit rejected job {} with status 0x{:08x}",
                                        state.result_index,
                                        status));
                    }
                }

                reset_dispatch_descriptor_pool_locked();
                shape_pipeline_collector->burn_before_reset(command_buffer_);
                check_vk(vkResetCommandBuffer(command_buffer_, 0),
                         "reset(pc_grouped_soa_seed_coord_post_emit)");
                begin = VkCommandBufferBeginInfo{};
                begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                check_vk(vkBeginCommandBuffer(command_buffer_, &begin),
                         "begin(pc_grouped_soa_seed_coord_post_emit)");
                shape_pipeline_collector->begin(command_buffer_);
            }
            if (coordinate_vote_selected) {
                for (gpu_seed_coordinate_wave_state& state : states) {
                    if (!state.has_anchors) {
                        continue;
                    }
                    gpu_arena_slice anchors_s = state.anchors_slice();
                    gpu_arena_slice anchor_keys_s = state.anchor_keys_slice();
                    const std::array<VkDescriptorBufferInfo, 6> coord_infos{{
                        {anchors_s.buffer->buffer_handle, anchors_s.offset, anchors_s.size},
                        {anchor_keys_s.buffer->buffer_handle,
                         anchor_keys_s.offset,
                         anchor_keys_s.size},
                        {anchor_keys_s.buffer->buffer_handle,
                         anchor_keys_s.offset,
                         anchor_keys_s.size},
                        {state.result_buf.buffer_handle, 0, state.result_buf.bytes},
                        {state.result_buf.buffer_handle, 0, state.result_buf.bytes},
                        {state.result_buf.buffer_handle, 0, state.result_buf.bytes},
                    }};
                    gpu_coordinate_reduce_push coord_push{
                        .anchor_count = state.total_anchors_u32,
                        .query_len_lo = static_cast<std::uint32_t>(state.query_len & 0xffffffffull),
                        .query_len_hi = static_cast<std::uint32_t>(state.query_len >> 32),
                        .min_chain_score = min_chain_score,
                        .min_seed_count = min_seed_count,
                        .probe_limit = probe_limit,
                        .reserved = 0u,
                    };
                    record_compute_dispatch_locked(command_buffer_,
                                                   vote_coord_pipe,
                                                   coord_infos,
                                                   &coord_push,
                                                   sizeof(coord_push),
                                                   1u);
                    run.stats.coordinate_vote_dispatches += 1u;
                }
                record_shader_to_host(command_buffer_);
                submit_and_wait(
                    device_, queue_, command_buffer_, fence_, "submit(pc_seed_coord_vote_batch2)");

                for (gpu_seed_coordinate_wave_state& state : states) {
                    if (state.has_anchors) {
                        std::memcpy(&run.results[state.result_index],
                                    state.result_buf.mapped,
                                    sizeof(gpu_coordinate_result_wire));
                    }
                }

                arena.rewind(mark);
                submitted_waves += 1u;
                run.stats.wave_count += 1u;
                wave_begin = next_wave_begin;
                continue;
            }
            for (gpu_seed_coordinate_wave_state& state : states) {
                if (!state.has_anchors) {
                    continue;
                }
                gpu_arena_slice anchors_s = state.anchors_slice();
                gpu_arena_slice anchor_keys_s = state.anchor_keys_slice();
                gpu_arena_slice anchors_scratch_s = state.anchors_scratch_slice();
                gpu_arena_slice anchor_keys_scratch_s = state.anchor_keys_scratch_slice();
                gpu_arena_slice anchor_histogram_s = state.anchor_histogram_slice();
                const gpu_anchor_sort_key_plan anchor_sort_plan = make_anchor_sort_key_plan(
                    state.job.target_len, state.job.target_position_offset);
                gpu_sort_plan sort_plan{
                    state.total_anchors, 8u, 24u, 8u, anchor_sort_plan.pass_count};
                record_radix_sort_locked(command_buffer_,
                                         anchor_keys_s,
                                         anchors_s,
                                         anchor_keys_scratch_s,
                                         anchors_scratch_s,
                                         anchor_histogram_s,
                                         sort_plan,
                                         state.anchor_wg.num_workgroups,
                                         state.anchor_wg.elems_per_wg,
                                         3u,
                                         state.sorted_in_scratch,
                                         shape_pipeline_collector.get(),
                                         state.result_index);
                run.stats.sort_dispatches += 1u;
                if (grouped_soa_selected) {
                    state.operation_audit.sort_operations += 1u;
                }
                const gpu_arena_slice sorted_anchors =
                    state.sorted_in_scratch ? anchors_scratch_s : anchors_s;
                const gpu_arena_slice compacted_anchors_s = state.compacted_anchors_slice();
                const std::array<VkDescriptorBufferInfo, 6> range_infos{{
                    {sorted_anchors.buffer->buffer_handle,
                     sorted_anchors.offset,
                     sorted_anchors.size},
                    {state.range_count_buf.buffer_handle, 0, state.range_count_buf.bytes},
                    {state.ranges_buf.buffer_handle, 0, state.ranges_buf.bytes},
                    {state.range_count_buf.buffer_handle, 0, state.range_count_buf.bytes},
                    {compacted_anchors_s.buffer->buffer_handle,
                     compacted_anchors_s.offset,
                     compacted_anchors_s.size},
                    {state.span_buf.buffer_handle, 0, state.span_buf.bytes},
                }};
                chain_range_push_constants range_push{
                    .query_count = 1u,
                    .max_ref_gap =
                        static_cast<std::uint32_t>(std::max<std::int32_t>(max_dist_x, 0)),
                };
                record_compute_dispatch_locked(command_buffer_,
                                               range_pipe,
                                               range_infos,
                                               &range_push,
                                               sizeof(range_push),
                                               1u,
                                               shape_pipeline_collector.get(),
                                               state.result_index);
                run.stats.range_build_dispatches += 1u;
                if (grouped_soa_selected) {
                    state.operation_audit.range_build_dispatches += 1u;
                }
            }
            record_shader_to_host(command_buffer_);
            submit_and_wait(device_,
                            queue_,
                            command_buffer_,
                            fence_,
                            "submit(pc_seed_coord_batch2)",
                            shape_pipeline_collector.get());
            if (grouped_soa_selected) {
                const std::uint64_t receipt_identity = next_operation_identity_locked();
                for (gpu_seed_coordinate_wave_state& state : states) {
                    if (state.has_anchors) {
                        state.operation_audit.sort_range_submit_wait_identity = receipt_identity;
                    }
                }
            }

            std::uint32_t chain_active_count = 0;
            for (gpu_seed_coordinate_wave_state& state : states) {
                if (!state.has_anchors) {
                    continue;
                }
                std::memcpy(&state.range_count, state.range_count_buf.mapped, 4u);
                std::memcpy(&state.compacted_anchor_count,
                            static_cast<std::byte*>(state.range_count_buf.mapped) + 4u,
                            4u);
                if (grouped_soa_selected) {
                    state.operation_audit.range_count = state.range_count;
                    shape_pipeline_collector->set_shape_outcome(
                        state.result_index, state.total_anchors, state.range_count);
                }
                if (state.range_count == 0u) {
                    if (grouped_soa_selected) {
                        run.operation_receipt.records_[state.result_index].audit =
                            state.operation_audit;
                    }
                    continue;
                }
                if (state.compacted_anchor_count > state.total_anchors_u32) {
                    throw vulkan_runtime_error(
                        "gpu seed-to-coordinate batch compacted anchor overflow");
                }
                const bool use_compacted_anchors =
                    state.compacted_anchor_count != 0u &&
                    state.compacted_anchor_count < state.total_anchors_u32;
                state.active_anchor_count =
                    use_compacted_anchors ? state.compacted_anchor_count : state.total_anchors_u32;
                state.score_buf = create_device_buffer(std::uint64_t{state.active_anchor_count} *
                                                           sizeof(std::int32_t),
                                                       k_compute_buffer_usage);
                state.predecessor_buf = create_device_buffer(
                    std::uint64_t{state.active_anchor_count} * sizeof(std::int64_t),
                    k_compute_buffer_usage);
                state.visited_buf = create_device_buffer(std::uint64_t{state.active_anchor_count} *
                                                             sizeof(std::uint32_t),
                                                         k_compute_buffer_usage);
                state.trace_buf = create_device_buffer(std::uint64_t{state.active_anchor_count} *
                                                           sizeof(std::uint32_t),
                                                       k_compute_buffer_usage);
                state.result_buf = create_device_buffer(sizeof(gpu_coordinate_result_wire),
                                                        k_compute_buffer_usage);
                if (state.score_buf.mapped == nullptr || state.predecessor_buf.mapped == nullptr ||
                    state.visited_buf.mapped == nullptr || state.trace_buf.mapped == nullptr ||
                    state.result_buf.mapped == nullptr) {
                    throw vulkan_runtime_error(
                        "gpu seed-to-coordinate batch non-unified chain memory");
                }
                state.chain_active = true;
                chain_active_count += 1u;
            }

            if (chain_active_count == 0u) {
                arena.rewind(mark);
                submitted_waves += 1u;
                run.stats.wave_count += 1u;
                wave_begin = next_wave_begin;
                continue;
            }

            reset_dispatch_descriptor_pool_locked();
            if (shape_pipeline_collector != nullptr) {
                shape_pipeline_collector->burn_before_reset(command_buffer_);
            }
            check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_seed_coord_batch3)");
            begin = VkCommandBufferBeginInfo{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_seed_coord_batch3)");
            if (shape_pipeline_collector != nullptr) {
                shape_pipeline_collector->begin(command_buffer_);
            }
            for (gpu_seed_coordinate_wave_state& state : states) {
                if (!state.chain_active) {
                    continue;
                }
                std::memset(
                    state.score_buf.mapped, 0, static_cast<std::size_t>(state.score_buf.bytes));
                std::memset(state.predecessor_buf.mapped,
                            0xff,
                            static_cast<std::size_t>(state.predecessor_buf.bytes));
                std::memset(
                    state.visited_buf.mapped, 0, static_cast<std::size_t>(state.visited_buf.bytes));
                std::memset(
                    state.result_buf.mapped, 0, static_cast<std::size_t>(state.result_buf.bytes));
            }
            record_host_to_shader(command_buffer_);
            for (gpu_seed_coordinate_wave_state& state : states) {
                if (!state.chain_active) {
                    continue;
                }
                gpu_arena_slice anchors_s = state.anchors_slice();
                gpu_arena_slice anchors_scratch_s = state.anchors_scratch_slice();
                gpu_arena_slice compacted_anchors_s = state.compacted_anchors_slice();
                const bool use_compacted_anchors =
                    state.compacted_anchor_count != 0u &&
                    state.compacted_anchor_count < state.total_anchors_u32;
                const gpu_arena_slice active_anchors =
                    use_compacted_anchors
                        ? compacted_anchors_s
                        : (state.sorted_in_scratch ? anchors_scratch_s : anchors_s);
                const std::array<VkDescriptorBufferInfo, 4> chain_infos{{
                    {active_anchors.buffer->buffer_handle,
                     active_anchors.offset,
                     active_anchors.size},
                    {state.ranges_buf.buffer_handle, 0, state.ranges_buf.bytes},
                    {state.score_buf.buffer_handle, 0, state.score_buf.bytes},
                    {state.predecessor_buf.buffer_handle, 0, state.predecessor_buf.bytes},
                }};
                VkDescriptorSet chain_set =
                    allocate_dispatch_descriptor_set_locked(descriptor_set_layout_);
                std::array<VkWriteDescriptorSet, 4> writes{};
                for (std::uint32_t binding = 0; binding < writes.size(); ++binding) {
                    writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    writes[binding].dstSet = chain_set;
                    writes[binding].dstBinding = binding;
                    writes[binding].descriptorCount = 1;
                    writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    writes[binding].pBufferInfo = &chain_infos[binding];
                }
                vkUpdateDescriptorSets(
                    device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);

                chaining_push_constants chain_push{};
                chain_push.anchor_count = state.active_anchor_count;
                chain_push.range_count = state.range_count;
                chain_push.max_dist_x = max_dist_x;
                chain_push.max_dist_y = max_dist_y;
                chain_push.bandwidth = bandwidth;
                chain_push.max_iter = max_iter;
                chain_push.gap_penalty = gap_penalty;
                chain_push.skip_penalty = skip_penalty;
                chain_push.max_chain_skip = max_chain_skip;
                const std::uint32_t chain_groups =
                    std::min(state.range_count, features_.max_workgroup_count_x);
                vkCmdBindPipeline(
                    command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, chain_pipeline.pipeline);
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
                if (shape_pipeline_collector != nullptr) {
                    shape_pipeline_collector->use(command_buffer_,
                                                  state.result_index,
                                                  chain_pipeline.shader_key,
                                                  chain_pipeline.metal_archive_receipt,
                                                  chain_groups);
                }
                vkCmdDispatch(command_buffer_, chain_groups, 1, 1);
                run.stats.chain_dispatches += 1u;
                if (grouped_soa_selected) {
                    state.operation_audit.chain_dispatches += 1u;
                }
            }
            record_compute_barrier(command_buffer_);
            for (gpu_seed_coordinate_wave_state& state : states) {
                if (!state.chain_active) {
                    continue;
                }
                gpu_arena_slice anchors_s = state.anchors_slice();
                gpu_arena_slice anchors_scratch_s = state.anchors_scratch_slice();
                gpu_arena_slice compacted_anchors_s = state.compacted_anchors_slice();
                const bool use_compacted_anchors =
                    state.compacted_anchor_count != 0u &&
                    state.compacted_anchor_count < state.total_anchors_u32;
                const gpu_arena_slice active_anchors =
                    use_compacted_anchors
                        ? compacted_anchors_s
                        : (state.sorted_in_scratch ? anchors_scratch_s : anchors_s);
                const std::array<VkDescriptorBufferInfo, 6> coord_infos{{
                    {active_anchors.buffer->buffer_handle,
                     active_anchors.offset,
                     active_anchors.size},
                    {state.score_buf.buffer_handle, 0, state.score_buf.bytes},
                    {state.predecessor_buf.buffer_handle, 0, state.predecessor_buf.bytes},
                    {state.visited_buf.buffer_handle, 0, state.visited_buf.bytes},
                    {state.trace_buf.buffer_handle, 0, state.trace_buf.bytes},
                    {state.result_buf.buffer_handle, 0, state.result_buf.bytes},
                }};
                gpu_coordinate_reduce_push coord_push{
                    .anchor_count = state.active_anchor_count,
                    .query_len_lo = static_cast<std::uint32_t>(state.query_len & 0xffffffffull),
                    .query_len_hi = static_cast<std::uint32_t>(state.query_len >> 32),
                    .min_chain_score = min_chain_score,
                    .min_seed_count = min_seed_count,
                    .probe_limit = probe_limit,
                    .reserved = 0u,
                };
                record_compute_dispatch_locked(command_buffer_,
                                               coord_pipe,
                                               coord_infos,
                                               &coord_push,
                                               sizeof(coord_push),
                                               1u,
                                               shape_pipeline_collector.get(),
                                               state.result_index);
                run.stats.chain_backtrack_dispatches += 1u;
                if (grouped_soa_selected) {
                    state.operation_audit.chain_backtrack_dispatches += 1u;
                    state.operation_audit.coordinate_dispatches += 1u;
                }
            }
            record_shader_to_host(command_buffer_);
            submit_and_wait(device_,
                            queue_,
                            command_buffer_,
                            fence_,
                            "submit(pc_seed_coord_batch3)",
                            shape_pipeline_collector.get());
            if (grouped_soa_selected) {
                const std::uint64_t receipt_identity = next_operation_identity_locked();
                for (gpu_seed_coordinate_wave_state& state : states) {
                    if (state.chain_active) {
                        state.operation_audit.chain_coordinate_submit_wait_identity =
                            receipt_identity;
                    }
                }
            }

            for (gpu_seed_coordinate_wave_state& state : states) {
                if (state.chain_active) {
                    std::memcpy(&run.results[state.result_index],
                                state.result_buf.mapped,
                                sizeof(gpu_coordinate_result_wire));
                    if (grouped_soa_selected) {
                        state.operation_audit.result = run.results[state.result_index];
                        state.operation_audit.result_readbacks += 1u;
                        run.operation_receipt.records_[state.result_index].audit =
                            state.operation_audit;
                    }
                }
            }

            arena.rewind(mark);
            submitted_waves += 1u;
            run.stats.wave_count += 1u;
            wave_begin = next_wave_begin;
        } catch (...) {
            arena.rewind(mark);
            throw;
        }
    }

    if (shape_pipeline_collector != nullptr) {
        shape_pipeline_collector->burn_before_reset(command_buffer_);
    }
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_seed_coord_batch_post)");
    if (submitted_waves != 0u) {
        chaining_dispatch_count_.fetch_add(submitted_waves, std::memory_order_relaxed);
    }
    if (grouped_soa_selected) {
        run.grouped_soa_direct.direct_count_scan_emit_completed = true;
        run.operation_receipt.query_pipeline_token_ = std::move(query_authority.pipeline_token_);
        run.operation_receipt.grouped_soa_pipeline_token_ = shape_pipeline_collector->seal();
        run.operation_receipt.sealed_ =
            run.operation_receipt.records_.size() == jobs.size() &&
            !run.operation_receipt.records_.empty() &&
            run.operation_receipt.has_valid_causal_tokens() &&
            std::all_of(run.operation_receipt.records_.begin(),
                        run.operation_receipt.records_.end(),
                        [](const auto& record) {
                            return record.state ==
                                       persisted_grouped_soa_coordinate_operation_receipt::
                                           record_state::fresh &&
                                   record.audit.is_internally_consistent();
                        });
    }
    return run;
}

} // namespace bioseq::gpu::vulkan
