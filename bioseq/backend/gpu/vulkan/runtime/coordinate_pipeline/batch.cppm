/**
 * @file batch.cppm
 * @brief Records one batched seed-to-coordinate descriptor and submission sequence.
 */
module;

#include <cstdint>
#include <libassert/assert.hpp>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

export module bioseq.gpu.vulkan.runtime:coordinate_pipeline.batch;

import :frontend_types;
import :support;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.parallelism.memory_admission;
import bioseq.instrumentation;
import std;

namespace bioseq::gpu::vulkan {

namespace {

/// Maximum logical jobs retained in one bounded submission wave.
constexpr std::uint32_t k_gpu_seed_coordinate_batch_wave_size = 4096u;

/// @brief Push constants for constructing one chaining range per query.
struct chain_range_push_constants {
    std::uint32_t query_count;
    std::uint32_t max_ref_gap;
};
static_assert(sizeof(chain_range_push_constants) == 8);

/// @brief Per-job coordinate-vote input consumed by the batched reduction shader.
struct gpu_coordinate_vote_job_wire {
    std::uint32_t anchor_offset;
    std::uint32_t anchor_count;
    std::uint32_t query_len_lo;
    std::uint32_t query_len_hi;
};
static_assert(sizeof(gpu_coordinate_vote_job_wire) == 16);

/// @brief Host ownership and arena slices retained through one submission wave.
struct gpu_seed_coordinate_wave_state {
    std::size_t result_index = 0;
    gpu_seed_coordinate_job job{};
    gpu_seed_query_record_wire query_record{};
    persisted_grouped_soa_coordinate_job_audit operation_audit{};
    std::uint32_t minimizer_count = 0;
    std::uint32_t num_blocks = 0;
    std::uint32_t seed_groups = 0;
    std::uint64_t query_len = 0;
    gpu_arena_slice counts_s{};
    gpu_arena_slice prefix_s{};
    gpu_arena_slice blocks_s{};
    seed_push_constants seed_push{};
    gpu_grouped_soa_seed_push grouped_soa_seed_push{};
    std::uint64_t total_anchors = 0;
    std::uint32_t total_anchors_u32 = 0;
    std::uint32_t anchor_offset = 0;
    std::uint32_t vote_result_index = 0;
    workgroup_plan anchor_wg{};
    buffer anchors_buf;
    buffer anchor_keys_buf;
    buffer anchors_scratch_buf;
    buffer anchor_keys_scratch_buf;
    buffer anchor_histogram_buf;
    buffer ranges_buf;
    buffer range_count_buf;
    buffer compacted_anchors_buf;
    buffer span_buf;
    bool sorted_in_scratch = false;
    std::uint32_t range_count = 0;
    std::uint32_t compacted_anchor_count = 0;
    std::uint32_t active_anchor_count = 0;
    buffer score_buf;
    buffer predecessor_buf;
    buffer visited_buf;
    buffer trace_buf;
    buffer result_buf;
    bool skip_seed = false;
    bool has_anchors = false;
    bool has_ranges = false;
    bool chain_active = false;

    /// @brief Views the complete primary anchor allocation.
    /// @return Arena slice spanning `anchors_buf`.
    [[nodiscard]] gpu_arena_slice anchors_slice() noexcept {
        return gpu_arena_slice{&anchors_buf, 0, anchors_buf.bytes};
    }

    /// @brief Views the complete primary anchor-key allocation.
    /// @return Arena slice spanning `anchor_keys_buf`.
    [[nodiscard]] gpu_arena_slice anchor_keys_slice() noexcept {
        return gpu_arena_slice{&anchor_keys_buf, 0, anchor_keys_buf.bytes};
    }

    /// @brief Views the complete scratch anchor allocation.
    /// @return Arena slice spanning `anchors_scratch_buf`.
    [[nodiscard]] gpu_arena_slice anchors_scratch_slice() noexcept {
        return gpu_arena_slice{&anchors_scratch_buf, 0, anchors_scratch_buf.bytes};
    }

    /// @brief Views the complete scratch anchor-key allocation.
    /// @return Arena slice spanning `anchor_keys_scratch_buf`.
    [[nodiscard]] gpu_arena_slice anchor_keys_scratch_slice() noexcept {
        return gpu_arena_slice{&anchor_keys_scratch_buf, 0, anchor_keys_scratch_buf.bytes};
    }

    /// @brief Views the complete radix histogram allocation.
    /// @return Arena slice spanning `anchor_histogram_buf`.
    [[nodiscard]] gpu_arena_slice anchor_histogram_slice() noexcept {
        return gpu_arena_slice{&anchor_histogram_buf, 0, anchor_histogram_buf.bytes};
    }

    /// @brief Views the complete compacted-anchor allocation.
    /// @return Arena slice spanning `compacted_anchors_buf`.
    [[nodiscard]] gpu_arena_slice compacted_anchors_slice() noexcept {
        return gpu_arena_slice{&compacted_anchors_buf, 0, compacted_anchors_buf.bytes};
    }
};

/// @brief Checks whether two jobs may share one resident target submission.
[[nodiscard]] bool has_same_coordinate_target(const gpu_seed_coordinate_job& lhs,
                                              const gpu_seed_coordinate_job& rhs) noexcept {
    return lhs.grouped_soa == rhs.grouped_soa && lhs.slots == rhs.slots &&
           lhs.positions == rhs.positions && lhs.target_len == rhs.target_len &&
           lhs.slot_mask == rhs.slot_mask && lhs.position_count == rhs.position_count &&
           lhs.bucket_count == rhs.bucket_count &&
           lhs.target_position_offset == rhs.target_position_offset;
}

} // namespace

/// @brief Runs the batched seed, sort, chain, and coordinate submission sequence.
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

    const gpu_seed_index_representation representation =
        jobs.empty() ? gpu_seed_index_representation::invalid
                     : classify_gpu_seed_index_representation(jobs.front());
    const bool grouped_soa_selected = representation == gpu_seed_index_representation::grouped_soa;
    if (grouped_soa_selected && coordinate_vote_selected) {
        throw vulkan_runtime_error(
            "gpu grouped-SoA direct route requires Vulkan tiled-DP coordinate output");
    }
    if (grouped_soa_selected && features_.software_device) {
        throw vulkan_runtime_error("gpu grouped-SoA direct route rejects software Vulkan devices");
    }
    const auto shape_operation_configuration =
        make_persisted_grouped_soa_coordinate_config_identity(kmer_size,
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
    if (grouped_soa_selected && !shape_operation_configuration.is_valid()) {
        throw vulkan_runtime_error("gpu grouped-SoA coordinate configuration identity is invalid");
    }

    const gpu_grouped_soa_resident_view* common_shape_view = nullptr;
    std::uint64_t common_shape_target_len = 0u;
    for (const gpu_seed_coordinate_job& job : jobs) {
        const gpu_seed_index_representation job_representation =
            classify_gpu_seed_index_representation(job);
        if (job_representation == gpu_seed_index_representation::invalid ||
            job_representation != representation) {
            throw vulkan_runtime_error("gpu seed-to-coordinate resident index representation must "
                                       "be strict XOR and uniform");
        }
        if (job.query_index >= query_inputs.host_query_records.size()) {
            throw vulkan_runtime_error("gpu seed-to-coordinate job query_index out of range");
        }
        if (job.target_len == 0u) {
            throw vulkan_runtime_error("gpu seed-to-coordinate target length must be nonzero");
        }
        if (grouped_soa_selected) {
            if (job.target_len > (std::uint64_t{1} << 63u)) {
                throw vulkan_runtime_error("gpu grouped-SoA target length exceeds anchor-key ABI");
            }
            if (common_shape_view == nullptr) {
                common_shape_view = job.grouped_soa;
                common_shape_target_len = job.target_len;
            } else if (job.grouped_soa != common_shape_view) {
                throw vulkan_runtime_error(
                    "gpu grouped-SoA batch must bind one resident generation");
            } else if (job.target_len != common_shape_target_len) {
                throw vulkan_runtime_error("gpu grouped-SoA batch target identity is inconsistent");
            }
        }
    }

    gpu_seed_coordinate_batch_result run{};
    run.results.resize(jobs.size());
    run.stats.problem_count = static_cast<std::uint64_t>(jobs.size());
    run.grouped_soa_direct.selected = grouped_soa_selected;
    run.grouped_soa_direct.problem_count = static_cast<std::uint64_t>(jobs.size());

    std::uint32_t shape_control_word_count = 0u;
    std::uint32_t shape_position_word_count = 0u;
    std::uint64_t shape_control_padded_bytes = 0u;
    std::uint64_t shape_position_padded_bytes = 0u;
    if (grouped_soa_selected) {
        if (common_shape_view == nullptr) {
            throw vulkan_runtime_error("gpu grouped-SoA route has no resident generation");
        }
        if (kmer_size == 0u || kmer_size > 255u || query_inputs.hash_shift >= 64u ||
            query_inputs.pos_bits >= 64u) {
            throw vulkan_runtime_error(
                "gpu grouped-SoA query sketch traits exceed the grouped-SoA ABI");
        }
        const auto valid_resident_buffer = [this](const buffer* buffer) {
            return buffer != nullptr && buffer->owner == this && buffer->is_valid() &&
                   buffer->is_host_visible_coherent() && buffer->mapped != nullptr;
        };
        if (!valid_resident_buffer(common_shape_view->descriptors) ||
            !valid_resident_buffer(common_shape_view->controls) ||
            !valid_resident_buffer(common_shape_view->positions)) {
            throw vulkan_runtime_error(
                "gpu grouped-SoA resident buffers are invalid, foreign, or not coherent");
        }
        if (common_shape_view->descriptors->buffer_handle ==
                common_shape_view->controls->buffer_handle ||
            common_shape_view->descriptors->buffer_handle ==
                common_shape_view->positions->buffer_handle ||
            common_shape_view->controls->buffer_handle ==
                common_shape_view->positions->buffer_handle) {
            throw vulkan_runtime_error("gpu grouped-SoA resident buffers must not alias");
        }
        if (common_shape_view->abi_revision != k_gpu_grouped_soa_abi_revision ||
            common_shape_view->partition_count == 0u || common_shape_view->control_bytes == 0u ||
            common_shape_view->position_bytes == 0u || common_shape_view->position_count == 0u ||
            common_shape_view->max_occ_cap == (std::numeric_limits<std::uint64_t>::max)() ||
            common_shape_view->generation_id == 0u || common_shape_view->source_identity == 0u ||
            common_shape_view->cold_load_identity == 0u) {
            throw vulkan_runtime_error(
                "gpu grouped-SoA resident identity or generation contract is invalid");
        }
        const std::uint64_t expected_descriptor_bytes =
            static_cast<std::uint64_t>(common_shape_view->partition_count) *
            sizeof(gpu_grouped_soa_resident_partition_descriptor);
        if (common_shape_view->descriptor_bytes != expected_descriptor_bytes ||
            common_shape_view->descriptor_bytes > common_shape_view->descriptors->bytes) {
            throw vulkan_runtime_error("gpu grouped-SoA descriptor byte extent is invalid");
        }
        const auto checked_word_count = [](std::uint64_t bytes, const char* label) {
            constexpr std::uint64_t maximum_word_bytes =
                static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)()) * 4u;
            if (bytes == 0u || bytes > maximum_word_bytes) {
                throw vulkan_runtime_error(
                    fmt::format("gpu grouped-SoA {} byte extent exceeds GLSL word ABI", label));
            }
            return static_cast<std::uint32_t>((bytes + 3u) / 4u);
        };
        shape_control_word_count = checked_word_count(common_shape_view->control_bytes, "control");
        shape_position_word_count =
            checked_word_count(common_shape_view->position_bytes, "position");
        shape_control_padded_bytes = static_cast<std::uint64_t>(shape_control_word_count) * 4u;
        shape_position_padded_bytes = static_cast<std::uint64_t>(shape_position_word_count) * 4u;
        if (shape_control_padded_bytes > common_shape_view->controls->bytes ||
            shape_position_padded_bytes > common_shape_view->positions->bytes ||
            common_shape_view->descriptor_bytes > features_.max_storage_buffer_range ||
            shape_control_padded_bytes > features_.max_storage_buffer_range ||
            shape_position_padded_bytes > features_.max_storage_buffer_range) {
            throw vulkan_runtime_error(
                "gpu grouped-SoA resident byte extent exceeds its buffer or Vulkan range");
        }
        const bool nil_view_uuid = std::all_of(common_shape_view->device_uuid.begin(),
                                               common_shape_view->device_uuid.end(),
                                               [](std::uint8_t byte) { return byte == 0u; });
        if (nil_view_uuid || common_shape_view->device_uuid != features_.device_uuid) {
            throw vulkan_runtime_error(
                "gpu grouped-SoA resident generation belongs to a different Vulkan device");
        }
        const auto* descriptor_bytes =
            static_cast<const std::byte*>(common_shape_view->descriptors->mapped);
        for (std::uint32_t ordinal = 0u; ordinal < common_shape_view->partition_count; ++ordinal) {
            gpu_grouped_soa_resident_partition_descriptor descriptor{};
            std::memcpy(&descriptor,
                        descriptor_bytes +
                            static_cast<std::uint64_t>(ordinal) *
                                sizeof(gpu_grouped_soa_resident_partition_descriptor),
                        sizeof(descriptor));
            if (!is_gpu_grouped_soa_partition_descriptor_valid(descriptor,
                                                               common_shape_view->control_bytes,
                                                               common_shape_view->position_bytes) ||
                descriptor.segment_end_bp > common_shape_target_len) {
                throw vulkan_runtime_error("gpu grouped-SoA resident descriptor validation failed");
            }
        }
        if (max_occ >= 0 && static_cast<std::uint64_t>(max_occ) > common_shape_view->max_occ_cap) {
            throw vulkan_runtime_error(
                "gpu grouped-SoA query max_occ exceeds persisted generation cap");
        }
        run.grouped_soa_direct.generation_id = common_shape_view->generation_id;
        run.grouped_soa_direct.source_identity = common_shape_view->source_identity;
        run.grouped_soa_direct.cold_load_identity = common_shape_view->cold_load_identity;
    }
    const std::uint32_t total_mc = query_inputs.minimizer_count;
    if (jobs.empty() || max_occ < 0 || (total_mc == 0u && !grouped_soa_selected)) {
        run.grouped_soa_direct.direct_count_scan_emit_completed = grouped_soa_selected;
        return run;
    }
    if (query_inputs.host_query_records.empty()) {
        throw vulkan_runtime_error("gpu seed-to-coordinate batch missing query records");
    }
    for (const gpu_seed_query_record_wire& qrec : query_inputs.host_query_records) {
        const std::uint64_t end =
            static_cast<std::uint64_t>(qrec.minimizer_offset) + qrec.minimizer_count;
        if (qrec.flags != 0u || qrec.minimizer_offset > total_mc || end > total_mc) {
            throw vulkan_runtime_error(
                "gpu seed-to-coordinate query minimizer slice or flags are invalid");
        }
        if (qrec.minimizer_count == 0u) {
            continue;
        }
        if (grouped_soa_selected &&
            (qrec.query_len < kmer_size ||
             qrec.minimizer_count == (std::numeric_limits<std::uint32_t>::max)() ||
             (static_cast<std::uint64_t>(qrec.minimizer_count) + 1u) * sizeof(std::uint32_t) >
                 features_.max_storage_buffer_range)) {
            throw vulkan_runtime_error(
                "gpu grouped-SoA query record violates local count/status ABI");
        }
    }

    const std::uint32_t required_words_per_minimizer =
        query_inputs.minimizer_word_kind == 0u || query_inputs.minimizer_word_kind == 1u ? 1u
        : query_inputs.minimizer_word_kind == 2u                                         ? 2u
                                                                                         : 4u;
    if (query_inputs.minimizer_word_kind > 3u ||
        query_inputs.minimizer_words32_per_record < required_words_per_minimizer ||
        (query_inputs.minimizer_word_kind != 0u &&
         static_cast<std::uint64_t>(total_mc) >
             (std::numeric_limits<std::uint64_t>::max)() /
                 (static_cast<std::uint64_t>(query_inputs.minimizer_words32_per_record) *
                  sizeof(std::uint32_t)))) {
        throw vulkan_runtime_error(
            "gpu seed-to-coordinate minimizer word shape is invalid or overflows");
    }
    const std::uint64_t required_min_words_bytes = std::max<std::uint64_t>(
        minimizer_device_bytes(
            query_inputs.minimizer_word_kind, query_inputs.minimizer_words32_per_record, total_mc),
        4u);
    if (query_inputs.device_minimizer_words != nullptr &&
        (!query_inputs.device_minimizer_words->is_valid() ||
         query_inputs.device_minimizer_words->owner != this ||
         required_min_words_bytes > query_inputs.device_minimizer_words->bytes)) {
        throw vulkan_runtime_error(
            "gpu seed-to-coordinate device minimizer buffer is invalid or undersized");
    }
    if (grouped_soa_selected && (query_inputs.device_minimizer_words == nullptr ||
                                 !query_inputs.host_minimizer_bytes.empty())) {
        throw vulkan_runtime_error(
            "gpu grouped-SoA direct route requires an unambiguous GPU-resident query sketch");
    }
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
        std::size_t wave_end =
            std::min<std::size_t>(jobs.size(), wave_begin + k_gpu_seed_coordinate_batch_wave_size);
        if (coordinate_vote_selected && wave_begin + 1u < wave_end) {
            std::size_t same_target_end = wave_begin + 1u;
            while (same_target_end < wave_end &&
                   has_same_coordinate_target(jobs[wave_begin], jobs[same_target_end])) {
                ++same_target_end;
            }
            if (same_target_end > wave_begin + 1u) {
                wave_end = same_target_end;
            }
        }
        const std::size_t next_wave_begin = wave_end;
        const std::size_t wave_count = wave_end - wave_begin;
        std::uint64_t per_wave_problem_bytes = 0u;
        for (std::size_t job_index = wave_begin; job_index < wave_end; ++job_index) {
            const gpu_seed_query_record_wire& qrec =
                query_inputs.host_query_records[jobs[job_index].query_index];
            const std::uint32_t qblocks = std::max<std::uint32_t>(
                bioseq::common::ceil_div(qrec.minimizer_count, k_prefix_scan_block_size), 1u);
            per_wave_problem_bytes = bioseq::common::saturating_add(
                per_wave_problem_bytes,
                bioseq::common::saturating_add(
                    bioseq::common::saturating_add(std::uint64_t{qrec.minimizer_count} * 8u,
                                                   grouped_soa_selected ? 4u : 0u),
                    std::uint64_t{qblocks} * 4u));
        }
        const std::uint64_t arena_bytes = bioseq::common::saturating_add(
            bioseq::common::saturating_add(min_words_bytes, query_record_bytes),
            bioseq::common::saturating_add(
                per_wave_problem_bytes,
                bioseq::common::saturating_mul(static_cast<std::uint64_t>(3u * wave_count + 2u),
                                               std::uint64_t{k_gpu_buffer_offset_alignment})));
        if (!arena.ensure_capacity(arena_bytes, 0u, 0u)) {
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

            const VkBuffer min_words_buffer =
                query_inputs.device_minimizer_words != nullptr
                    ? query_inputs.device_minimizer_words->buffer_handle
                    : minw_s.buffer->buffer_handle;
            const std::uint64_t min_words_offset =
                query_inputs.device_minimizer_words != nullptr ? 0u : minw_s.offset;
            const std::uint64_t min_words_range =
                query_inputs.device_minimizer_words != nullptr ? VK_WHOLE_SIZE : minw_s.size;
            if (query_inputs.device_minimizer_words == nullptr) {
                std::memset(base + minw_s.offset, 0, static_cast<std::size_t>(minw_s.size));
                if (!query_inputs.host_minimizer_bytes.empty()) {
                    std::memcpy(base + minw_s.offset,
                                query_inputs.host_minimizer_bytes.data(),
                                query_inputs.host_minimizer_bytes.size());
                }
            }
            std::memset(base + qrec_s.offset, 0, static_cast<std::size_t>(qrec_s.size));
            for (std::size_t qrec_index = 0; qrec_index < query_inputs.host_query_records.size();
                 ++qrec_index) {
                std::memcpy(base + qrec_s.offset +
                                static_cast<std::uint64_t>(qrec_index) * qrec_stride,
                            &query_inputs.host_query_records[qrec_index],
                            sizeof(gpu_seed_query_record_wire));
            }

            std::vector<gpu_seed_coordinate_wave_state> states;
            states.reserve(wave_count);
            std::uint32_t seed_active_count = 0;
            for (std::size_t job_index = wave_begin; job_index < wave_end; ++job_index) {
                gpu_seed_coordinate_wave_state& state = states.emplace_back();
                state.result_index = job_index;
                state.job = jobs[job_index];
                state.query_record = query_inputs.host_query_records[state.job.query_index];
                if (grouped_soa_selected) {
                    auto& audit = state.operation_audit;
                    audit.operation_identity = run.operation_receipt.operation_identity_;
                    audit.query_authority_identity = query_authority.identity_;
                    audit.resident_descriptors =
                        make_runtime_buffer_identity(common_shape_view->descriptors);
                    audit.resident_controls =
                        make_runtime_buffer_identity(common_shape_view->controls);
                    audit.resident_positions =
                        make_runtime_buffer_identity(common_shape_view->positions);
                    audit.query_minimizer_buffer =
                        make_runtime_buffer_identity(query_inputs.device_minimizer_words);
                    audit.device_uuid = common_shape_view->device_uuid;
                    audit.descriptor_bytes = common_shape_view->descriptor_bytes;
                    audit.control_bytes = common_shape_view->control_bytes;
                    audit.position_bytes = common_shape_view->position_bytes;
                    audit.position_count = common_shape_view->position_count;
                    audit.max_occ_cap = common_shape_view->max_occ_cap;
                    audit.partition_count = common_shape_view->partition_count;
                    audit.abi_revision = common_shape_view->abi_revision;
                    audit.generation_id = common_shape_view->generation_id;
                    audit.source_identity = common_shape_view->source_identity;
                    audit.cold_load_identity = common_shape_view->cold_load_identity;
                    audit.target_len = state.job.target_len;
                    audit.problem_index = state.result_index;
                    audit.query_index = state.job.query_index;
                    audit.logical_target_row = state.job.logical_target_row;
                    audit.logical_query_ordinal = state.job.logical_query_ordinal;
                    audit.query_record = state.query_record;
                    audit.configuration = shape_operation_configuration;
                    audit.query_tile_count =
                        query_authority.query_tile_counts_[state.job.query_index];
                    audit.query_count_dispatches =
                        query_authority.dispatch_stats_.count_dispatches();
                    audit.query_scan_dispatches =
                        query_authority.dispatch_stats_.prefix_scan_dispatches();
                    audit.query_emit_dispatches = query_authority.dispatch_stats_.emit_dispatches();
                    audit.query_completed_roundtrips =
                        query_authority.dispatch_stats_.completed_roundtrips();
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
                const std::uint32_t target_offset_lo =
                    static_cast<std::uint32_t>(state.job.target_position_offset & 0xffffffffull);
                const std::uint32_t target_offset_hi =
                    static_cast<std::uint32_t>(state.job.target_position_offset >> 32);
                const gpu_anchor_sort_key_plan anchor_sort_plan = make_anchor_sort_key_plan(
                    state.job.target_len, state.job.target_position_offset);
                state.seed_push = seed_push_constants{query_inputs.minimizer_words32_per_record,
                                                      query_inputs.minimizer_word_kind,
                                                      query_inputs.hash_shift,
                                                      query_inputs.pos_bits,
                                                      kmer_size,
                                                      state.job.slot_mask,
                                                      static_cast<std::uint32_t>(max_occ),
                                                      0u,
                                                      1u,
                                                      state.job.bucket_count,
                                                      target_offset_lo,
                                                      target_offset_hi,
                                                      anchor_sort_plan.target_ref_bits};
                if (grouped_soa_selected) {
                    const gpu_grouped_soa_resident_view& view = *state.job.grouped_soa;
                    state.grouped_soa_seed_push = gpu_grouped_soa_seed_push{
                        .abi_revision = k_gpu_grouped_soa_abi_revision,
                        .words32_per_minimizer = query_inputs.minimizer_words32_per_record,
                        .word_kind = query_inputs.minimizer_word_kind,
                        .hash_shift = query_inputs.hash_shift,
                        .pos_bits = query_inputs.pos_bits,
                        .kmer_size = kmer_size,
                        .partition_count = view.partition_count,
                        .max_occ = static_cast<std::uint32_t>(max_occ),
                        .emit_mode = 0u,
                        .record_count = 1u,
                        .target_ref_bits = anchor_sort_plan.target_ref_bits,
                        .query_minimizer_count = total_mc,
                        .control_word_count = shape_control_word_count,
                        .position_word_count = shape_position_word_count,
                        .anchor_capacity = 0u,
                        .max_occ_cap_lo =
                            static_cast<std::uint32_t>(view.max_occ_cap & 0xffffffffull),
                        .max_occ_cap_hi = static_cast<std::uint32_t>(view.max_occ_cap >> 32u),
                        .status_index = state.minimizer_count,
                    };
                }
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
                bool same_target_wave = active_states.size() >= 2u;
                if (same_target_wave) {
                    for (const gpu_seed_coordinate_wave_state* state : active_states) {
                        if (state->job.slots != first_active->job.slots ||
                            state->job.positions != first_active->job.positions ||
                            state->job.target_len != first_active->job.target_len ||
                            state->job.slot_mask != first_active->job.slot_mask ||
                            state->job.position_count != first_active->job.position_count ||
                            state->job.bucket_count != first_active->job.bucket_count ||
                            state->job.target_position_offset !=
                                first_active->job.target_position_offset) {
                            same_target_wave = false;
                            break;
                        }
                    }
                }

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
