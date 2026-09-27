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

static_assert(k_grouped_soa_seed_cache_abi_revision == k_gpu_grouped_soa_seed_push_abi_revision);

namespace {

inline constexpr std::uint64_t k_mps_query_record_stride = k_gpu_buffer_offset_alignment;

struct mps_seed_wave_state {
    std::size_t job_index = 0u;
    std::size_t local_index = 0u;
    gpu_seed_query_record_wire query_record{};
    std::uint64_t anchor_bound = 0u;
    std::uint64_t total_anchors = 0u;
    std::uint32_t total_anchors_u32 = 0u;
    std::uint32_t num_blocks = 0u;
    workgroup_plan emit_workgroups{};
    gpu_grouped_soa_seed_push push{};
    gpu_arena_slice counts{};
    gpu_arena_slice prefix{};
    gpu_arena_slice blocks{};
    gpu_arena_slice repetitive{};
    gpu_arena_slice anchors{};
    gpu_arena_slice anchor_keys{};
    gpu_arena_slice anchors_scratch{};
    gpu_arena_slice anchor_keys_scratch{};
    gpu_arena_slice histogram{};
    gpu_arena_slice download{};
    bool sorted_in_scratch = false;
};

[[nodiscard]] bool append_mps_arena_extent(std::uint64_t& cursor,
                                           std::uint64_t bytes,
                                           std::uint64_t alignment) noexcept {
    const auto aligned = bioseq::common::checked_align_up(cursor, alignment);
    if (!aligned.has_value())
        return false;
    const auto end = bioseq::common::checked_add(*aligned, bytes);
    if (!end.has_value())
        return false;
    cursor = *end;
    return true;
}

[[nodiscard]] bool append_mps_device_extent(std::uint64_t& cursor,
                                            std::uint64_t bytes) noexcept {
    return append_mps_arena_extent(
        cursor, bytes, std::max<std::uint64_t>(16u, k_gpu_buffer_offset_alignment));
}

[[nodiscard]] bool append_mps_download_extent(std::uint64_t& cursor,
                                              std::uint64_t bytes) noexcept {
    return append_mps_arena_extent(cursor, bytes, 16u);
}

void ensure_mps_host_anchor_capacity(grouped_soa_seed_anchor_result& run,
                                     std::size_t required_count) {
    if (required_count <= run.anchor_wires.capacity())
        return;
    if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
        if (required_count > (std::numeric_limits<std::uint64_t>::max)()) {
            throw vulkan_runtime_error(
                "grouped-SoA seed-only host anchor count exceeds uint64");
        }
    }
    if (required_count > run.anchor_wires.max_size()) {
        throw vulkan_runtime_error(
            "grouped-SoA seed-only host anchor count is not representable");
    }
    const auto requested_bytes = bioseq::common::checked_mul(
        static_cast<std::uint64_t>(required_count), sizeof(gpu_anchor_wire));
    const auto preallocation_bound = requested_bytes.has_value()
        ? bioseq::common::checked_mul(
              *requested_bytes,
              bioseq::parallelism::k_vector_preallocation_headroom_factor)
        : std::optional<std::uint64_t>{};
    if (!requested_bytes.has_value() || !preallocation_bound.has_value()) {
        throw vulkan_runtime_error(
            "grouped-SoA seed-only host anchor capacity overflow");
    }

    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    bioseq::parallelism::memory_admission_failure failure{};
    auto reserved = admission.try_reserve(*preallocation_bound, &failure);
    if (!reserved) {
        throw vulkan_runtime_error(fmt::format(
            "grouped-SoA seed-only host anchor admission denied: reason={} requested={}",
            bioseq::parallelism::memory_admission_failure_reason_name(failure.reason),
            failure.requested_bytes));
    }

    // The prior vector and proof remain live throughout allocation, full touch,
    // and copying. The replacement vector is destroyed before its local permit
    // on every exceptional path.
    bioseq::parallelism::memory_permit replacement_permit = std::move(*reserved);
    std::vector<gpu_anchor_wire> replacement;
    bioseq::parallelism::memory_materialization_result materialization{};
    try {
        materialization = admission.materialize(
            replacement_permit,
            [&replacement, &replacement_permit, &run, required_count]()
                -> bioseq::parallelism::memory_materialization_snapshot {
                replacement.reserve(required_count);
                if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
                    if (replacement.capacity() >
                        (std::numeric_limits<std::uint64_t>::max)()) {
                        throw vulkan_runtime_error(
                            "grouped-SoA seed-only host anchor capacity exceeds uint64");
                    }
                }
                const auto actual_bytes = bioseq::common::checked_mul(
                    static_cast<std::uint64_t>(replacement.capacity()),
                    sizeof(gpu_anchor_wire));
                if (!actual_bytes.has_value() ||
                    *actual_bytes > (std::numeric_limits<std::size_t>::max)() ||
                    *actual_bytes > replacement_permit.bytes()) {
                    throw vulkan_runtime_error(
                        "grouped-SoA seed-only host anchor capacity exceeded admission");
                }
                replacement_permit.shrink_to(*actual_bytes);
                replacement.resize(replacement.capacity());
                std::memset(replacement.data(),
                            0,
                            static_cast<std::size_t>(*actual_bytes));
                std::copy(run.anchor_wires.begin(),
                          run.anchor_wires.end(),
                          replacement.begin());
                replacement.resize(run.anchor_wires.size());
                return {
                    .actual_allocation_bytes = *actual_bytes,
                    .fully_touched_bytes = *actual_bytes,
                    .is_host_visible = true,
                    .is_host_coherent = true,
                    .is_process_footprint_reflected = true,
                };
            });
    } catch (const std::bad_alloc&) {
        throw vulkan_runtime_error(
            "grouped-SoA seed-only host anchor allocation failed after admission");
    }
    const auto actual_capacity_bytes = bioseq::common::checked_mul(
        static_cast<std::uint64_t>(replacement.capacity()), sizeof(gpu_anchor_wire));
    if (!actual_capacity_bytes.has_value() ||
        materialization.allocation_snapshot.actual_allocation_bytes !=
            *actual_capacity_bytes ||
        materialization.allocation_snapshot.fully_touched_bytes !=
            *actual_capacity_bytes ||
        materialization.admitted_bytes != *actual_capacity_bytes ||
        materialization.is_permit_released ==
            static_cast<bool>(replacement_permit) ||
        (replacement_permit &&
         replacement_permit.bytes() != *actual_capacity_bytes)) {
        throw vulkan_runtime_error(
            "grouped-SoA seed-only host anchor materialization proof is inconsistent");
    }

    {
        std::vector<gpu_anchor_wire> prior;
        prior.swap(run.anchor_wires);
        run.anchor_wires.swap(replacement);
    }
    run.host_anchor_permit.release();
    run.host_anchor_permit = std::move(replacement_permit);
    run.host_anchor_capacity_reflected = materialization.is_permit_released;
}

} // namespace

/// @brief Runs the batched seed, sort, chain, and coordinate submission sequence.
/// Holds the runtime mutex until the call returns.
/// @param[in] query_inputs Shared query minimizer storage and per-query records.
///     `device_query_records` must be absent.
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
/// @param[in] query_authority One-shot query authority that must match `query_inputs`.
///     Grouped-SoA requires it; source-direct callers may supply it to receive an
///     authenticated execution receipt.
/// @return Per-job results, dispatch statistics, and causal operation receipt.
///     Empty jobs, negative `max_occ`, or zero minimizers without grouped-SoA skip GPU work.
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
                                                device_query_sketch_authorization query_authority,
                                                gpu_persisted_mapq_config mapq_config) {
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
                                                                   probe_limit,
                                                                   mapq_config);
    const bool grouped_soa_selected = batch_shape.grouped_soa_selected;
    const bool mapq_selected = mapq_config.is_valid();
    const auto& shape_operation_configuration = batch_shape.operation_configuration;
    const gpu_grouped_soa_resident_view* common_shape_view = batch_shape.common_view;

    gpu_seed_coordinate_batch_result run{};
    run.results.resize(jobs.size());
    if (mapq_selected) {
        const std::uint64_t maximum_candidate_count = bioseq::common::saturating_mul(
            static_cast<std::uint64_t>(jobs.size()),
            static_cast<std::uint64_t>(mapq_config.candidate_capacity));
        const std::uint64_t candidate_bytes = bioseq::common::saturating_add(
            bioseq::common::saturating_mul(maximum_candidate_count,
                                           sizeof(gpu_chain_candidate_wire)),
            bioseq::common::saturating_mul(static_cast<std::uint64_t>(jobs.size()),
                                           sizeof(gpu_chain_candidate_header_wire)));
        const std::uint64_t admitted_candidate_bytes = bioseq::common::saturating_mul(
            candidate_bytes, bioseq::parallelism::k_vector_preallocation_headroom_factor);
        if (maximum_candidate_count > (std::numeric_limits<std::uint32_t>::max)() ||
            maximum_candidate_count > run.candidates.max_size() ||
            jobs.size() > run.candidate_headers.max_size() ||
            candidate_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            admitted_candidate_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
            throw vulkan_runtime_error("gpu MAPQ host candidate admission overflow");
        }
        bioseq::parallelism::memory_admission_failure failure{};
        auto permit = bioseq::parallelism::memory_admission_controller::instance().try_reserve(
            admitted_candidate_bytes, &failure);
        if (!permit) {
            throw vulkan_runtime_error(fmt::format(
                "gpu MAPQ host candidate admission denied: reason={} requested={}",
                bioseq::parallelism::memory_admission_failure_reason_name(failure.reason),
                failure.requested_bytes));
        }
        try {
            run.candidate_headers.resize(jobs.size());
            run.candidates.reserve(static_cast<std::size_t>(maximum_candidate_count));
        } catch (const std::bad_alloc&) {
            throw vulkan_runtime_error("gpu MAPQ host candidate allocation failed after admission");
        }
        const std::uint64_t retained_candidate_bytes = bioseq::common::saturating_add(
            bioseq::common::saturating_mul(static_cast<std::uint64_t>(run.candidates.capacity()),
                                           sizeof(gpu_chain_candidate_wire)),
            bioseq::common::saturating_mul(
                static_cast<std::uint64_t>(run.candidate_headers.capacity()),
                sizeof(gpu_chain_candidate_header_wire)));
        if (retained_candidate_bytes > permit->bytes()) {
            throw vulkan_runtime_error("gpu MAPQ host candidate capacity exceeded admission");
        }
        permit->shrink_to(retained_candidate_bytes);
        run.candidate_capacity_permit = std::move(*permit);
    }
    run.stats.problem_count = static_cast<std::uint64_t>(jobs.size());
    run.grouped_soa_direct.selected = grouped_soa_selected;
    run.grouped_soa_direct.problem_count = static_cast<std::uint64_t>(jobs.size());

    const coordinate_pipeline_detail::grouped_soa_resident_layout resident_layout =
        coordinate_pipeline_detail::validate_grouped_soa_resident(
            this, batch_shape, query_inputs, features_, kmer_size, max_occ, run);
    const std::uint64_t shape_control_padded_bytes = resident_layout.control_padded_bytes;
    const std::uint64_t shape_position_padded_bytes = resident_layout.position_padded_bytes;
    const std::uint32_t binding_window_count = static_cast<std::uint32_t>(resident_layout.windows.size());
    const std::uint32_t maximum_window_jobs = grouped_soa_selected
        ? gpu_dispatch_job_capacity(binding_window_count + 3u, binding_window_count * 10u + 9u)
        : coordinate_pipeline_detail::k_gpu_seed_coordinate_batch_wave_size;
    if (maximum_window_jobs == 0u) {
        throw vulkan_runtime_error("grouped-SoA binding windows exceed one job's descriptor capacity");
    }
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
    const bool source_direct_authority_supplied =
        !grouped_soa_selected && query_authority.is_valid();
    if (grouped_soa_selected || source_direct_authority_supplied) {
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
        if (!query_authority.is_valid() || query_inputs.device_minimizer_words == nullptr ||
            query_authority.runtime_ != this ||
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
                "gpu seed-to-coordinate route lacks matching one-shot query authority");
        }
    }
    if (grouped_soa_selected) {
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
    } else if (source_direct_authority_supplied) {
        shape_pipeline_collector = runtime_pipeline_use_collector::create(
            runtime_pipeline_operation_kind::source_direct_coordinate,
            jobs.size(),
            query_authority.pipeline_token_.audit().operation_identity);
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
        grouped_soa_selected ? k_grouped_soa_seed_cache_abi_revision : 1u);
    const compute_pipeline emit_pipe = get_compute_pipeline_locked(
        grouped_soa_selected ? shader_kind::grouped_soa_seed_emit : shader_kind::seed_emit,
        128u,
        grouped_soa_selected ? k_grouped_soa_seed_cache_abi_revision : 1u);
    const compute_pipeline scanb =
        get_compute_pipeline_locked(shader_kind::prefix_scan_block, 1u, 1u);
    const compute_pipeline scana =
        get_compute_pipeline_locked(shader_kind::prefix_scan_add, 128u, 1u);
    const compute_pipeline range_pipe =
        get_compute_pipeline_locked(shader_kind::chain_range_build, 1u, 1u);
    const compute_pipeline coord_pipe = get_compute_pipeline_locked(
        mapq_selected ? shader_kind::chain_candidate_reduce : shader_kind::coordinate_reduce,
        128u,
        mapq_selected ? k_gpu_persisted_mapq_candidate_capacity : 1u);
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
                                                                  query_record_bytes,
                                                                  maximum_window_jobs);
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
                    base, qrec_s, minw_s, query_inputs, qrec_stride, required_min_words_bytes);
            const VkBuffer min_words_buffer = minimizer_binding.buffer;
            const std::uint64_t min_words_offset = minimizer_binding.offset;
            const std::uint64_t min_words_range = minimizer_binding.range;

            // Declared before `states` so the reservation is released only after every
            // per-state candidate buffer has been destroyed on success or unwind.
            bioseq::parallelism::memory_permit mapq_candidate_device_permit;
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
                    state.operation_audit.binding_window_count = binding_window_count;
                }
                state.minimizer_count = state.query_record.minimizer_count;
                state.query_len = state.query_record.query_len;
                if (shape_pipeline_collector != nullptr) {
                    shape_pipeline_collector->set_shape_input(state.result_index,
                                                              state.minimizer_count,
                                                              state.job.target_len,
                                                              state.job.target_position_offset,
                                                              binding_window_count);
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
                if (grouped_soa_selected) {
                    state.repetitive_s = arena.allocate_device(
                        std::max<std::uint64_t>(state.minimizer_count, 1u) * 8u, 16u);
                }
                if (!state.counts_s.is_valid() || !state.prefix_s.is_valid() ||
                    !state.blocks_s.is_valid() ||
                    (grouped_soa_selected && !state.repetitive_s.is_valid())) {
                    throw vulkan_runtime_error("gpu seed-to-coordinate batch arena oversubscribed");
                }
                if (grouped_soa_selected) {
                    std::memset(base + state.repetitive_s.offset,
                                0,
                                static_cast<std::size_t>(state.repetitive_s.size));
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
            if (grouped_soa_selected && submitted_waves == 0u) {
                // Device-only query sketching completed in an earlier queue
                // submission. Its shader writes need an explicit device memory
                // dependency before this submission's seed shader reads them;
                // the host-write barrier above covers only mapped host inputs.
                // One dependency is sufficient because this operation never
                // modifies the resident query image between later waves.
                record_compute_barrier(command_buffer_);
            }
            const std::uint32_t count_window_count = grouped_soa_selected ? binding_window_count : 1u;
            // Batch independent jobs within each window; synchronize only when
            // the next window needs their accumulated counts or emit cursors.
            for (std::uint32_t window_index = 0u; window_index < count_window_count; ++window_index) {
                for (gpu_seed_coordinate_wave_state& state : states) {
                    if (state.skip_seed) {
                        continue;
                    }
                    if (grouped_soa_selected) {
                        const auto& window = resident_layout.windows[window_index];
                        const gpu_grouped_soa_resident_view& view = *state.job.grouped_soa;
                        gpu_grouped_soa_seed_push window_push = state.grouped_soa_seed_push;
                        std::array<VkDescriptorBufferInfo, 10> seed_infos{{
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
                            {state.repetitive_s.buffer->buffer_handle,
                             state.repetitive_s.offset,
                             state.repetitive_s.size},
                        }};
                        coordinate_pipeline_detail::bind_grouped_soa_buffer_window(
                            window_push,
                            seed_infos, window,
                            features_.max_storage_buffer_range);
                        record_compute_dispatch_locked(command_buffer_,
                                                       count_pipe,
                                                       seed_infos,
                                                       &window_push,
                                                       sizeof(window_push),
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
                if (window_index + 1u < count_window_count) {
                    record_compute_barrier(command_buffer_);
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
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                if (grouped_soa_selected && state.total_anchors == 0u) {
                    // The strict E2E route will reject this biologically suspect
                    // zero-anchor job. Preserve the completed submission's exact
                    // count/scan words so the existing SQL failure distinguishes
                    // seed-count output from scan propagation/host totalization.
                    const auto read_word = [base](const gpu_arena_slice& slice,
                                                  const std::uint32_t index) noexcept {
                        std::uint32_t value = 0u;
                        std::memcpy(&value,
                                    base + slice.offset +
                                        static_cast<std::uint64_t>(index) * sizeof(value),
                                    sizeof(value));
                        return value;
                    };
                    auto& audit = state.operation_audit;
                    const std::uint32_t last_index = state.minimizer_count - 1u;
                    const std::uint32_t boundary_index =
                        std::min(k_prefix_scan_block_size, last_index);
                    audit.count_scan_failure_probe_available = true;
                    audit.count_scan_probe_first_count = read_word(state.counts_s, 0u);
                    audit.count_scan_probe_first_prefix = read_word(state.prefix_s, 0u);
                    audit.count_scan_probe_boundary_index = boundary_index;
                    audit.count_scan_probe_boundary_count =
                        read_word(state.counts_s, boundary_index);
                    audit.count_scan_probe_boundary_prefix =
                        read_word(state.prefix_s, boundary_index);
                    audit.count_scan_probe_last_index = last_index;
                    audit.count_scan_probe_last_count = clast;
                    audit.count_scan_probe_last_prefix = plast;
                    audit.count_scan_probe_block_count = state.num_blocks;
                    // The scan-add stage has converted block_sums into exclusive
                    // block offsets at this point.
                    audit.count_scan_probe_first_block_offset = read_word(state.blocks_s, 0u);
                    audit.count_scan_probe_last_block_offset =
                        read_word(state.blocks_s, state.num_blocks - 1u);

                    using trace_word = benchmark_e2e_grouped_soa_count_trace_word;
                    constexpr std::size_t trace_word_count =
                        static_cast<std::size_t>(trace_word::word_count);
                    const std::uint64_t diagnostic_word_count = std::max(
                        static_cast<std::uint64_t>(state.minimizer_count) + 1u + trace_word_count,
                        static_cast<std::uint64_t>(state.minimizer_count) * 2u);
                    if (state.result_index == 0u && state.job.query_index == 0u &&
                        diagnostic_word_count <=
                            features_.max_storage_buffer_range / sizeof(std::uint32_t) &&
                        diagnostic_word_count <= (std::numeric_limits<std::uint32_t>::max)()) {
                        // Replay only the failing problem0/query0 count on a private,
                        // host-coherent buffer. The production count/scan output and
                        // authenticated submission counters remain unchanged.
                        constexpr std::uint32_t expected_partition_ordinal = 40u;
                        const std::uint64_t trace_base_word =
                            static_cast<std::uint64_t>(state.minimizer_count) + 1u;
                        buffer diagnostic_counts = create_host_buffer(
                            diagnostic_word_count * sizeof(std::uint32_t), k_compute_buffer_usage);
                        std::memset(diagnostic_counts.mapped,
                                    0,
                                    static_cast<std::size_t>(diagnostic_counts.bytes));
                        const std::uint32_t trace_request_magic =
                            k_benchmark_e2e_grouped_soa_count_trace_request_magic;
                        std::memcpy(static_cast<std::byte*>(diagnostic_counts.mapped) +
                                        static_cast<std::uint64_t>(state.minimizer_count) *
                                            sizeof(trace_request_magic),
                                    &trace_request_magic,
                                    sizeof(trace_request_magic));
                        const auto write_diagnostic_word = [&diagnostic_counts, trace_base_word](
                                                               const trace_word word,
                                                               const std::uint32_t value) noexcept {
                            std::memcpy(static_cast<std::byte*>(diagnostic_counts.mapped) +
                                            (trace_base_word + static_cast<std::size_t>(word)) *
                                                sizeof(value),
                                        &value,
                                        sizeof(value));
                        };
                        write_diagnostic_word(
                            trace_word::request_magic,
                            k_benchmark_e2e_grouped_soa_count_trace_request_magic);
                        write_diagnostic_word(trace_word::guard_magic,
                                              k_benchmark_e2e_grouped_soa_count_trace_guard_magic);
                        write_diagnostic_word(trace_word::requested_partition_ordinal,
                                              expected_partition_ordinal);

                        const gpu_grouped_soa_resident_view& view = *state.job.grouped_soa;
                        std::array<VkDescriptorBufferInfo, 10> trace_infos{{
                            {min_words_buffer, min_words_offset, min_words_range},
                            {view.descriptors->buffer_handle, 0, view.descriptor_bytes},
                            {view.controls->buffer_handle, 0, shape_control_padded_bytes},
                            {view.positions->buffer_handle, 0, shape_position_padded_bytes},
                            {qrec_s.buffer->buffer_handle,
                             qrec_s.offset + std::uint64_t{state.job.query_index} * qrec_stride,
                             sizeof(gpu_seed_query_record_wire)},
                            {diagnostic_counts.buffer_handle, 0, diagnostic_counts.bytes},
                            {state.prefix_s.buffer->buffer_handle,
                             state.prefix_s.offset,
                             state.prefix_s.size},
                            {diagnostic_counts.buffer_handle, 0, diagnostic_counts.bytes},
                            {diagnostic_counts.buffer_handle, 0, diagnostic_counts.bytes},
                            {diagnostic_counts.buffer_handle, 0, diagnostic_counts.bytes},
                        }};
                        gpu_grouped_soa_seed_push trace_push = state.grouped_soa_seed_push;
                        const auto trace_window = std::find_if(resident_layout.windows.begin(),
                            resident_layout.windows.end(), [](const auto& window) {
                                return window.partition_begin <= expected_partition_ordinal &&
                                    expected_partition_ordinal < window.partition_end;
                            });
                        coordinate_pipeline_detail::bind_grouped_soa_buffer_window(
                            trace_push, trace_infos,
                            trace_window == resident_layout.windows.end() ? resident_layout.windows.front() : *trace_window,
                            features_.max_storage_buffer_range);

                        audit.device_count_trace_attempted = true;
                        reset_dispatch_descriptor_pool_locked();
                        shape_pipeline_collector->burn_before_reset(command_buffer_);
                        check_vk(vkResetCommandBuffer(command_buffer_, 0),
                                 "reset(pc_grouped_soa_count_trace)");
                        VkCommandBufferBeginInfo trace_begin{};
                        trace_begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                        trace_begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                        check_vk(vkBeginCommandBuffer(command_buffer_, &trace_begin),
                                 "begin(pc_grouped_soa_count_trace)");
                        record_host_to_shader(command_buffer_);
                        record_compute_barrier(command_buffer_);
                        record_compute_dispatch_locked(command_buffer_,
                                                       count_pipe,
                                                       trace_infos,
                                                       &trace_push,
                                                       sizeof(trace_push),
                                                       1u);
                        record_shader_to_host(command_buffer_);
                        submit_and_wait(device_,
                                        queue_,
                                        command_buffer_,
                                        fence_,
                                        "submit(pc_grouped_soa_count_trace)");

                        std::memcpy(audit.device_count_trace_words.data(),
                                    static_cast<const std::byte*>(diagnostic_counts.mapped) +
                                        trace_base_word * sizeof(std::uint32_t),
                                    trace_word_count * sizeof(std::uint32_t));
                        audit.device_count_trace_available =
                            audit.device_count_trace_words[static_cast<std::size_t>(
                                trace_word::completion_magic)] ==
                            k_benchmark_e2e_grouped_soa_count_trace_completion_magic;
                    }
                }
#endif
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
            const std::uint32_t emit_window_count = grouped_soa_selected ? binding_window_count : 1u;
            // Batch independent jobs within each window; synchronize only when
            // the next window needs their accumulated counts or emit cursors.
            for (std::uint32_t window_index = 0u; window_index < emit_window_count; ++window_index) {
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
                        const auto& window = resident_layout.windows[window_index];
                        const gpu_grouped_soa_resident_view& view = *state.job.grouped_soa;
                        std::array<VkDescriptorBufferInfo, 10> seed_infos{{
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
                            {state.repetitive_s.buffer->buffer_handle,
                             state.repetitive_s.offset,
                             state.repetitive_s.size},
                        }};
                        coordinate_pipeline_detail::bind_grouped_soa_buffer_window(
                            emit_push, seed_infos, window, features_.max_storage_buffer_range);
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
                if (window_index + 1u < emit_window_count) {
                    record_compute_barrier(command_buffer_);
                }
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

            if (mapq_selected) {
                const std::uint64_t candidate_header_bytes =
                    sizeof(gpu_chain_candidate_header_wire);
                const std::uint64_t candidate_array_bytes = bioseq::common::saturating_mul(
                    static_cast<std::uint64_t>(mapq_config.candidate_capacity),
                    sizeof(gpu_chain_candidate_wire));
                if (candidate_array_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
                    candidate_header_bytes > features_.max_storage_buffer_range ||
                    candidate_array_bytes > features_.max_storage_buffer_range) {
                    throw vulkan_runtime_error("gpu MAPQ candidate device-buffer range overflow");
                }
                const std::uint64_t candidate_bytes_per_state = bioseq::common::saturating_add(
                    buffer_memory_requirement_locked(candidate_header_bytes,
                                                     k_compute_buffer_usage),
                    buffer_memory_requirement_locked(candidate_array_bytes,
                                                     k_compute_buffer_usage));
                const std::uint64_t admitted_candidate_device_bytes =
                    bioseq::common::saturating_mul(static_cast<std::uint64_t>(chain_active_count),
                                                   candidate_bytes_per_state);
                if (candidate_bytes_per_state == (std::numeric_limits<std::uint64_t>::max)() ||
                    admitted_candidate_device_bytes == 0u ||
                    admitted_candidate_device_bytes ==
                        (std::numeric_limits<std::uint64_t>::max)()) {
                    throw vulkan_runtime_error("gpu MAPQ candidate device admission overflow");
                }
                bioseq::parallelism::memory_admission_failure candidate_device_failure{};
                auto candidate_device_permit =
                    bioseq::parallelism::memory_admission_controller::instance().try_reserve(
                        admitted_candidate_device_bytes, &candidate_device_failure);
                if (!candidate_device_permit) {
                    throw vulkan_runtime_error(fmt::format(
                        "gpu MAPQ candidate device admission denied: reason={} requested={}",
                        bioseq::parallelism::memory_admission_failure_reason_name(
                            candidate_device_failure.reason),
                        candidate_device_failure.requested_bytes));
                }
                mapq_candidate_device_permit = std::move(*candidate_device_permit);

                std::uint64_t actual_candidate_device_bytes = 0u;
                for (gpu_seed_coordinate_wave_state& state : states) {
                    if (!state.chain_active) {
                        continue;
                    }
                    state.candidate_header_buf =
                        create_device_buffer(candidate_header_bytes, k_compute_buffer_usage);
                    state.candidates_buf =
                        create_device_buffer(candidate_array_bytes, k_compute_buffer_usage);
                    if (state.candidate_header_buf.mapped == nullptr ||
                        state.candidates_buf.mapped == nullptr) {
                        throw vulkan_runtime_error(
                            "gpu seed-to-coordinate batch non-unified MAPQ candidate memory");
                    }
                    actual_candidate_device_bytes = bioseq::common::saturating_add(
                        actual_candidate_device_bytes,
                        bioseq::common::saturating_add(state.candidate_header_buf.allocation_bytes,
                                                       state.candidates_buf.allocation_bytes));
                }
                if (actual_candidate_device_bytes != admitted_candidate_device_bytes ||
                    mapq_candidate_device_permit.bytes() != admitted_candidate_device_bytes) {
                    throw vulkan_runtime_error(
                        "gpu MAPQ candidate allocation changed after exact admission");
                }
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
                if (mapq_selected) {
                    std::memset(state.candidate_header_buf.mapped,
                                0,
                                static_cast<std::size_t>(state.candidate_header_buf.bytes));
                    std::memset(state.candidates_buf.mapped,
                                0,
                                static_cast<std::size_t>(state.candidates_buf.bytes));
                }
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
                if (mapq_selected) {
                    const std::array<VkDescriptorBufferInfo, 10> candidate_infos{{
                        {active_anchors.buffer->buffer_handle,
                         active_anchors.offset,
                         active_anchors.size},
                        {state.score_buf.buffer_handle, 0, state.score_buf.bytes},
                        {state.predecessor_buf.buffer_handle, 0, state.predecessor_buf.bytes},
                        {state.visited_buf.buffer_handle, 0, state.visited_buf.bytes},
                        {state.trace_buf.buffer_handle, 0, state.trace_buf.bytes},
                        {state.result_buf.buffer_handle, 0, state.result_buf.bytes},
                        {state.candidate_header_buf.buffer_handle,
                         0,
                         state.candidate_header_buf.bytes},
                        {state.candidates_buf.buffer_handle, 0, state.candidates_buf.bytes},
                        {min_words_buffer, min_words_offset, min_words_range},
                        {state.repetitive_s.buffer->buffer_handle,
                         state.repetitive_s.offset,
                         state.repetitive_s.size},
                    }};
                    const gpu_chain_candidate_reduce_push candidate_push{
                        .anchor_count = state.active_anchor_count,
                        .query_len_lo = static_cast<std::uint32_t>(state.query_len & 0xffffffffull),
                        .query_len_hi = static_cast<std::uint32_t>(state.query_len >> 32),
                        .min_chain_score = min_chain_score,
                        .min_seed_count = min_seed_count,
                        .candidate_capacity = mapq_config.candidate_capacity,
                        .problem_index = static_cast<std::uint32_t>(state.result_index),
                        .minimizer_offset = state.query_record.minimizer_offset,
                        .minimizer_count = state.query_record.minimizer_count,
                        .minimizer_words32_per_record = query_inputs.minimizer_words32_per_record,
                        .minimizer_word_kind = query_inputs.minimizer_word_kind,
                        .pos_bits = query_inputs.pos_bits,
                        .kmer_size = kmer_size,
                        .mask_level_bits = mapq_config.mask_level_bits,
                        .mask_len = mapq_config.mask_len,
                        .pri_ratio_bits = mapq_config.pri_ratio_bits,
                        .min_diff = mapq_config.min_diff,
                        .best_n = mapq_config.best_n,
                        .repetitive_flag_extent = state.minimizer_count,
                        .reserved = 0u,
                    };
                    record_compute_dispatch_locked(command_buffer_,
                                                   coord_pipe,
                                                   candidate_infos,
                                                   &candidate_push,
                                                   sizeof(candidate_push),
                                                   1u,
                                                   shape_pipeline_collector.get(),
                                                   state.result_index);
                    run.stats.chain_candidate_dispatches += 1u;
                    state.operation_audit.chain_candidate_dispatches += 1u;
                } else {
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
                    const gpu_coordinate_reduce_push coord_push{
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
                }
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
                    if (mapq_selected) {
                        gpu_chain_candidate_header_wire header{};
                        std::memcpy(&header,
                                    state.candidate_header_buf.mapped,
                                    sizeof(gpu_chain_candidate_header_wire));
                        if (header.status != k_gpu_mapq_status_ok || header.reserved != 0u ||
                            header.candidate_offset != 0u ||
                            header.candidate_capacity != mapq_config.candidate_capacity ||
                            header.candidate_count > header.candidate_capacity ||
                            header.retained_count > header.candidate_count ||
                            header.primary_count > header.retained_count ||
                            header.alignment_offset != 0u || header.alignment_count != 0u ||
                            header.repetitive_span > state.query_len ||
                            run.candidates.size() > (std::numeric_limits<std::uint32_t>::max)() ||
                            header.candidate_count >
                                run.candidates.capacity() - run.candidates.size()) {
                            throw vulkan_runtime_error(fmt::format(
                                "gpu MAPQ candidate reducer rejected job {} with status {}",
                                state.result_index,
                                header.status));
                        }
                        const auto* const device_candidates =
                            static_cast<const gpu_chain_candidate_wire*>(
                                state.candidates_buf.mapped);
                        std::uint32_t retained_count = 0u;
                        std::uint32_t primary_count = 0u;
                        for (std::uint32_t candidate_index = 0u;
                             candidate_index < header.candidate_count;
                             ++candidate_index) {
                            const gpu_chain_candidate_wire& candidate =
                                device_candidates[candidate_index];
                            if (!is_chain_candidate_wire_well_formed(
                                    candidate,
                                    static_cast<std::uint32_t>(state.result_index),
                                    candidate_index,
                                    state.query_len,
                                    state.job.target_len)) {
                                throw vulkan_runtime_error(
                                    fmt::format("gpu MAPQ candidate {} for job {} is malformed",
                                                candidate_index,
                                                state.result_index));
                            }
                            retained_count +=
                                (candidate.flags & k_gpu_chain_candidate_flag_retained) != 0u ? 1u
                                                                                              : 0u;
                            primary_count +=
                                (candidate.flags & k_gpu_chain_candidate_flag_primary) != 0u ? 1u
                                                                                             : 0u;
                        }
                        const gpu_coordinate_result_wire& coordinate =
                            run.results[state.result_index];
                        const bool empty_shape =
                            header.candidate_count == 0u && header.retained_count == 0u &&
                            header.primary_count == 0u && coordinate.is_valid == 0u;
                        bool primary_shape = false;
                        if (header.candidate_count != 0u) {
                            const gpu_chain_candidate_wire& primary = device_candidates[0];
                            primary_shape =
                                (primary.flags & (k_gpu_chain_candidate_flag_retained |
                                                  k_gpu_chain_candidate_flag_primary)) ==
                                    (k_gpu_chain_candidate_flag_retained |
                                     k_gpu_chain_candidate_flag_primary) &&
                                coordinate.is_valid == 1u &&
                                coordinate.query_start == primary.query_start &&
                                coordinate.query_end == primary.query_end &&
                                coordinate.target_start == primary.target_start &&
                                coordinate.target_end == primary.target_end &&
                                coordinate.match_length == primary.match_length &&
                                coordinate.block_length == primary.block_length &&
                                coordinate.chain_score == primary.chain_score &&
                                coordinate.seed_count == primary.seed_count &&
                                (coordinate.is_reverse != 0u) ==
                                    ((primary.flags & k_gpu_chain_candidate_flag_reverse) != 0u);
                        }
                        if (retained_count != header.retained_count ||
                            primary_count != header.primary_count ||
                            (!empty_shape && !primary_shape)) {
                            throw vulkan_runtime_error(
                                "gpu MAPQ candidate summary does not match coordinate output");
                        }
                        header.candidate_offset = static_cast<std::uint32_t>(run.candidates.size());
                        try {
                            run.candidates.insert(run.candidates.end(),
                                                  device_candidates,
                                                  device_candidates + header.candidate_count);
                        } catch (const std::bad_alloc&) {
                            throw vulkan_runtime_error(
                                "gpu MAPQ candidate append failed after admission");
                        }
                        run.candidate_headers[state.result_index] = header;
                        const std::span<const gpu_chain_candidate_wire> job_candidates =
                            header.candidate_count == 0u
                                ? std::span<const gpu_chain_candidate_wire>{}
                                : std::span<const gpu_chain_candidate_wire>{
                                      run.candidates.data() + header.candidate_offset,
                                      header.candidate_count};
                        state.operation_audit.candidate_header = header;
                        state.operation_audit.candidate_digest =
                            chain_candidate_digest(header, job_candidates);
                        state.operation_audit.candidate_readbacks += 1u;
                    }
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
    } else if (source_direct_authority_supplied) {
        runtime_pipeline_operation_token coordinate_pipeline_token =
            shape_pipeline_collector->seal();
        const runtime_pipeline_operation_audit& query_pipeline =
            query_authority.pipeline_token_.audit();
        const runtime_pipeline_operation_audit& coordinate_pipeline =
            coordinate_pipeline_token.audit();
        if (!query_authority.pipeline_token_.is_valid_for_query_sketch() ||
            !coordinate_pipeline_token.is_common_state_valid() ||
            coordinate_pipeline.kind != runtime_pipeline_operation_kind::source_direct_coordinate ||
            coordinate_pipeline.operation_identity != query_pipeline.operation_identity ||
            coordinate_pipeline.jobs.size() != jobs.size() ||
            query_pipeline.device_uuid != features_.device_uuid ||
            query_pipeline.api_version != features_.api_version ||
            query_pipeline.driver_version != features_.driver_version ||
            query_pipeline.vendor_id != features_.vendor_id ||
            query_pipeline.device_id != features_.device_id ||
            query_pipeline.pipeline_cache_uuid != features_.pipeline_cache_uuid ||
            query_pipeline.device_uuid != coordinate_pipeline.device_uuid ||
            query_pipeline.api_version != coordinate_pipeline.api_version ||
            query_pipeline.driver_version != coordinate_pipeline.driver_version ||
            query_pipeline.vendor_id != coordinate_pipeline.vendor_id ||
            query_pipeline.device_id != coordinate_pipeline.device_id ||
            query_pipeline.pipeline_cache_uuid != coordinate_pipeline.pipeline_cache_uuid ||
            query_pipeline.seed_sha256 != coordinate_pipeline.seed_sha256 ||
            query_pipeline.seed_size != coordinate_pipeline.seed_size ||
            query_pipeline.manifest_checksum != coordinate_pipeline.manifest_checksum ||
            query_pipeline.seed_receipt_identity != coordinate_pipeline.seed_receipt_identity ||
            query_pipeline.archive_set != coordinate_pipeline.archive_set ||
            query_pipeline.cache_qualification != coordinate_pipeline.cache_qualification) {
            throw vulkan_runtime_error(
                "source-direct coordinate query environment identity changed");
        }

        std::array<shader_variant_key, k_runtime_pipeline_prepared_slots> union_keys{};
        std::array<const runtime_metal_archive_pipeline_receipt*, k_runtime_pipeline_prepared_slots>
            union_receipts{};
        std::size_t union_count = 0u;
        for (std::size_t index = 0u; index < query_pipeline.prepared_count; ++index) {
            union_keys[union_count] = query_pipeline.prepared[index].shader_key;
            union_receipts[union_count] = query_authority.pipeline_token_.receipts_[index];
            ++union_count;
        }
        bool receipt_union_valid = union_count != 0u;
        for (std::size_t index = 0u;
             receipt_union_valid && index < coordinate_pipeline.prepared_count;
             ++index) {
            std::size_t slot = union_count;
            for (std::size_t existing = 0u; existing < union_count; ++existing) {
                if (union_keys[existing] == coordinate_pipeline.prepared[index].shader_key) {
                    slot = existing;
                    break;
                }
            }
            if (slot == union_count) {
                if (union_count == union_receipts.size()) {
                    receipt_union_valid = false;
                    break;
                }
                union_keys[union_count] = coordinate_pipeline.prepared[index].shader_key;
                union_receipts[union_count] = coordinate_pipeline_token.receipts_[index];
                ++union_count;
            } else if (union_receipts[slot] != coordinate_pipeline_token.receipts_[index]) {
                receipt_union_valid = false;
            }
        }
        receipt_union_valid =
            receipt_union_valid && union_count == coordinate_pipeline.prepared_count + 1u;
        runtime_gpu_cache_metrics_audit maximum_cache_metrics{
            .token_identity = query_pipeline.operation_identity,
            .gpu_pipeline_cache_loads = 1u,
            .gpu_pipeline_cache_seed_bytes = query_pipeline.seed_size,
        };
        const runtime_metal_archive_pipeline_receipt* first_receipt =
            receipt_union_valid ? union_receipts.front() : nullptr;
        if (first_receipt == nullptr || !first_receipt->is_valid()) {
            throw vulkan_runtime_error("source-direct coordinate cache receipt union is invalid");
        }
        maximum_cache_metrics.gpu_pipeline_cache_load_ns = first_receipt->audit().seed_load_ns;
        for (std::size_t index = 0u; index < union_count; ++index) {
            const runtime_metal_archive_pipeline_receipt* receipt = union_receipts[index];
            if (receipt == nullptr || !receipt->is_valid() ||
                receipt->audit().seed_load_ns != maximum_cache_metrics.gpu_pipeline_cache_load_ns ||
                maximum_cache_metrics.gpu_pipeline_creates ==
                    (std::numeric_limits<std::uint64_t>::max)() ||
                receipt->audit().pipeline_create_ns >
                    (std::numeric_limits<std::uint64_t>::max)() -
                        maximum_cache_metrics.gpu_pipeline_creation_ns) {
                throw vulkan_runtime_error("source-direct coordinate cache metrics are invalid");
            }
            ++maximum_cache_metrics.gpu_pipeline_creates;
            maximum_cache_metrics.gpu_pipeline_creation_ns += receipt->audit().pipeline_create_ns;
        }
        if (!maximum_cache_metrics.is_internally_consistent()) {
            throw vulkan_runtime_error("source-direct coordinate cache metrics are inconsistent");
        }
        auto cache_claim_plan = runtime_detail::runtime_gpu_cache_metrics_claim_plan::prepare(
            query_pipeline.operation_identity,
            maximum_cache_metrics,
            seed_metrics_claimed_,
            std::span<const runtime_metal_archive_pipeline_receipt* const>{union_receipts.data(),
                                                                           union_count});
        if (!cache_claim_plan.is_valid()) {
            throw vulkan_runtime_error("source-direct coordinate cache metric claim is invalid");
        }

        const runtime_gpu_execution_environment_snapshot environment{
            .gpu_environment_present = true,
            .gpu_device_uuid = query_pipeline.device_uuid,
            .gpu_driver_identity = canonical_vulkan_driver_identity(features_),
            .cache_policy = runtime_pipeline_cache_policy::immutable_preseed_all_hit,
            .seed_sha256 = query_pipeline.seed_sha256,
            .seed_size = query_pipeline.seed_size,
            .manifest_checksum = query_pipeline.manifest_checksum,
            .pipeline_cache_all_hit = true,
        };
        if (!environment.is_internally_consistent()) {
            throw vulkan_runtime_error(
                "source-direct coordinate GPU environment receipt is invalid");
        }
        runtime_gpu_cache_metrics_delta cache_metrics{};
        cache_metrics.audit_ = cache_claim_plan.audit();
        cache_metrics.issued_ = true;
        run.source_direct_execution_receipt.seal(
            environment,
            std::move(cache_metrics),
            run.stats,
            std::span<const gpu_coordinate_result_wire>{run.results.data(), run.results.size()});
        // All allocation and validation is complete. Keep the queue/cache mutex
        // across the final live gate and the report-once claim commit.
        require_immutable_metal_policy_live("source-direct coordinate receipt sealing");
        cache_claim_plan.commit(seed_metrics_claimed_);
    }
    return run;
}

/// @brief Executes the grouped-SoA seed prefix and stops before every chain stage.
grouped_soa_seed_anchor_result runtime::run_grouped_soa_seed_anchor_roundtrip(
    const gpu_grouped_soa_resident_view& target,
    const gpu_seed_query_inputs& query_inputs,
    std::int32_t max_occ,
    std::uint32_t kmer_size,
    device_query_sketch_authorization query_authorization) {
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available || features_.software_device) {
        throw vulkan_runtime_error(
            "grouped-SoA seed-only operation requires a hardware Vulkan device");
    }
    if (features_.max_workgroup_count_x == 0u || max_occ < 0 ||
        query_inputs.device_query_records != nullptr ||
        query_inputs.device_minimizer_words == nullptr ||
        !query_inputs.host_minimizer_bytes.empty() ||
        query_inputs.host_query_records.empty() ||
        query_inputs.host_query_records.size() >
            (std::numeric_limits<std::uint32_t>::max)()) {
        throw vulkan_runtime_error("grouped-SoA seed-only input shape is invalid");
    }

    const coordinate_pipeline_detail::coordinate_batch_shape target_shape{
        .representation = gpu_seed_index_representation::grouped_soa,
        .grouped_soa_selected = true,
        .common_view = &target,
        .common_target_len = std::uint64_t{1} << 63u,
    };
    gpu_seed_coordinate_batch_result validation_run{};
    const coordinate_pipeline_detail::grouped_soa_resident_layout resident_layout =
        coordinate_pipeline_detail::validate_grouped_soa_resident(this,
                                                                   target_shape,
                                                                   query_inputs,
                                                                   features_,
                                                                   kmer_size,
                                                                   max_occ,
                                                                   validation_run);
    const coordinate_pipeline_detail::coordinate_query_layout query_layout =
        coordinate_pipeline_detail::validate_coordinate_query_layout(
            this, query_inputs, features_, true, kmer_size);
    const std::uint64_t exact_query_bytes = validate_query_sketch_authorization_locked(
        query_authorization,
        *query_inputs.device_minimizer_words,
        query_inputs.minimizer_count,
        query_inputs.minimizer_word_kind,
        query_inputs.minimizer_words32_per_record,
        query_inputs.hash_shift,
        query_inputs.pos_bits,
        query_inputs.host_query_records);
    if (exact_query_bytes > query_layout.required_min_words_bytes ||
        query_authorization.kmer_size_ != kmer_size ||
        query_authorization.window_size_ != query_inputs.window_size ||
        query_authorization.buffer_ == target.descriptors->buffer_handle ||
        query_authorization.buffer_ == target.controls->buffer_handle ||
        query_authorization.buffer_ == target.positions->buffer_handle ||
        query_authorization.memory_identity_ ==
            vulkan_handle_identity(target.descriptors->memory) ||
        query_authorization.memory_identity_ ==
            vulkan_handle_identity(target.controls->memory) ||
        query_authorization.memory_identity_ ==
            vulkan_handle_identity(target.positions->memory)) {
        throw vulkan_runtime_error(
            "grouped-SoA seed-only query authorization aliases or exceeds target input");
    }

    std::uint64_t target_position_offset =
        (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t target_end = 0u;
    const auto* descriptor_bytes = static_cast<const std::byte*>(target.descriptors->mapped);
    for (std::uint32_t ordinal = 0u; ordinal < target.partition_count; ++ordinal) {
        gpu_grouped_soa_resident_partition_descriptor descriptor{};
        std::memcpy(&descriptor,
                    descriptor_bytes +
                        static_cast<std::uint64_t>(ordinal) * sizeof(descriptor),
                    sizeof(descriptor));
        target_position_offset = std::min(target_position_offset, descriptor.segment_base_bp);
        target_end = std::max(target_end, descriptor.segment_end_bp);
    }
    if (target_position_offset >= target_end || target_end > (std::uint64_t{1} << 63u)) {
        throw vulkan_runtime_error(
            "grouped-SoA seed-only target coordinate domain is invalid");
    }
    const std::uint64_t target_len = target_end - target_position_offset;
    const gpu_anchor_sort_key_plan anchor_sort_plan =
        make_anchor_sort_key_plan(target_len, target_position_offset);
    const auto radix_pass_count =
        runtime_grouped_soa_radix_pass_count(target_position_offset, target_len);
    if (!radix_pass_count.has_value() || anchor_sort_plan.pass_count != *radix_pass_count) {
        throw vulkan_runtime_error(
            "grouped-SoA seed-only target radix geometry is invalid");
    }
    // Count uses one 10-binding seed set plus three 3-binding scan sets.
    // Emit uses one 10-binding seed set plus three 5-binding sets per radix pass.
    const std::uint32_t binding_window_count = static_cast<std::uint32_t>(resident_layout.windows.size());
    const std::uint32_t count_descriptor_sets_per_job = binding_window_count + 3u;
    const std::uint32_t count_storage_descriptors_per_job = 10u * binding_window_count + 3u * 3u;
    const std::uint32_t emit_descriptor_sets_per_job =
        binding_window_count + 3u * *radix_pass_count;
    const std::uint32_t emit_storage_descriptors_per_job =
        10u * binding_window_count + 3u * 5u * *radix_pass_count;
    const std::uint32_t maximum_jobs_per_wave = std::min(
        gpu_dispatch_job_capacity(count_descriptor_sets_per_job,
                                  count_storage_descriptors_per_job),
        gpu_dispatch_job_capacity(emit_descriptor_sets_per_job,
                                  emit_storage_descriptors_per_job));
    if (maximum_jobs_per_wave == 0u) {
        throw vulkan_runtime_error(
            "grouped-SoA seed-only descriptor-set geometry is unavailable");
    }

    const std::uint64_t max_occ_u64 = static_cast<std::uint32_t>(max_occ);
    for (const gpu_seed_query_record_wire& record : query_inputs.host_query_records) {
        const auto job_bound =
            bioseq::common::checked_mul(record.minimizer_count, max_occ_u64);
        if (!job_bound.has_value() ||
            *job_bound > (std::numeric_limits<std::uint32_t>::max)()) {
            throw vulkan_runtime_error(
                "grouped-SoA seed-only per-job anchor bound exceeds uint32");
        }
    }
    const std::size_t job_count = query_inputs.host_query_records.size();
    if (job_count >
            std::vector<grouped_soa_seed_wire_job_span>{}.max_size()) {
        throw vulkan_runtime_error(
            "grouped-SoA seed-only host job count is not representable");
    }

    const auto job_capacity_bytes = bioseq::common::checked_mul(
        static_cast<std::uint64_t>(job_count),
        sizeof(grouped_soa_seed_wire_job_span));
    const auto job_preallocation_bound = job_capacity_bytes.has_value()
        ? bioseq::common::checked_mul(
              *job_capacity_bytes,
              bioseq::parallelism::k_vector_preallocation_headroom_factor)
        : std::optional<std::uint64_t>{};
    if (!job_capacity_bytes.has_value() || !job_preallocation_bound.has_value()) {
        throw vulkan_runtime_error(
            "grouped-SoA seed-only host job capacity admission overflow");
    }

    grouped_soa_seed_anchor_result run{};
    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    bioseq::parallelism::memory_admission_failure job_failure{};
    auto job_permit = admission.try_reserve(*job_preallocation_bound, &job_failure);
    if (!job_permit) {
        throw vulkan_runtime_error(fmt::format(
            "grouped-SoA seed-only host job admission denied: reason={} requested={}",
            bioseq::parallelism::memory_admission_failure_reason_name(job_failure.reason),
            job_failure.requested_bytes));
    }
    run.host_job_permit = std::move(*job_permit);
    bioseq::parallelism::memory_materialization_result job_materialization{};
    try {
        job_materialization = admission.materialize(
            run.host_job_permit,
            [&run, job_count]()
                -> bioseq::parallelism::memory_materialization_snapshot {
                run.jobs.reserve(job_count);
                const std::uint64_t retained = run.owned_job_capacity_bytes();
                if (retained == (std::numeric_limits<std::uint64_t>::max)() ||
                    retained > run.host_job_permit.bytes()) {
                    throw vulkan_runtime_error(
                        "grouped-SoA seed-only host job capacity exceeded admission");
                }
                run.host_job_permit.shrink_to(retained);
                run.jobs.resize(run.jobs.capacity());
                std::memset(run.jobs.data(), 0, static_cast<std::size_t>(retained));
                run.jobs.resize(job_count);
                return {
                    .actual_allocation_bytes = retained,
                    .fully_touched_bytes = retained,
                    .is_host_visible = true,
                    .is_host_coherent = true,
                    .is_process_footprint_reflected = true,
                };
            });
    } catch (const std::bad_alloc&) {
        throw vulkan_runtime_error(
            "grouped-SoA seed-only host job allocation failed after admission");
    }
    const std::uint64_t retained_job_capacity = run.owned_job_capacity_bytes();
    run.host_job_capacity_reflected = job_materialization.is_permit_released;
    if (retained_job_capacity == (std::numeric_limits<std::uint64_t>::max)() ||
        job_materialization.allocation_snapshot.actual_allocation_bytes !=
            retained_job_capacity ||
        job_materialization.allocation_snapshot.fully_touched_bytes !=
            retained_job_capacity ||
        job_materialization.admitted_bytes != retained_job_capacity ||
        job_materialization.is_permit_released ==
            static_cast<bool>(run.host_job_permit) ||
        (run.host_job_permit &&
         run.host_job_permit.bytes() != retained_job_capacity)) {
        throw vulkan_runtime_error(
            "grouped-SoA seed-only host job materialization proof is inconsistent");
    }

    const std::uint64_t operation_identity = next_operation_identity_locked();
    auto collector = runtime_pipeline_use_collector::create(
        runtime_pipeline_operation_kind::grouped_soa_seed_anchor_roundtrip,
        query_inputs.host_query_records.size(),
        operation_identity);
    const compute_pipeline count_pipe = get_compute_pipeline_locked(
        shader_kind::grouped_soa_seed_count,
        128u,
        k_grouped_soa_seed_cache_abi_revision);
    const compute_pipeline emit_pipe = get_compute_pipeline_locked(
        shader_kind::grouped_soa_seed_emit,
        128u,
        k_grouped_soa_seed_cache_abi_revision);
    const compute_pipeline scan_block_pipe =
        get_compute_pipeline_locked(shader_kind::prefix_scan_block, 1u, 1u);
    const compute_pipeline scan_add_pipe =
        get_compute_pipeline_locked(shader_kind::prefix_scan_add, 128u, 1u);
    const auto prepare = [collector_ptr = collector.get()](const compute_pipeline& pipeline) {
        collector_ptr->prepare(
            make_shader_key(pipeline.kind, 384u, pipeline.local_size, pipeline.max_span),
            pipeline.metal_archive_receipt);
    };
    prepare(count_pipe);
    prepare(emit_pipe);
    prepare(scan_block_pipe);
    prepare(scan_add_pipe);

    const std::uint64_t query_authority_identity = query_authorization.identity_;
    const seed_anchor_canonical_order_audit empty_anchor_audit{
        .anchor_count = 0u,
        .canonical_order_checked = true,
        .canonical_order_accepted = true,
    };
    for (std::size_t job_index = 0u;
         job_index < query_inputs.host_query_records.size();
         ++job_index) {
        const gpu_seed_query_record_wire& record = query_inputs.host_query_records[job_index];
        collector->set_shape_input(
            job_index, record.minimizer_count, target_len, target_position_offset, binding_window_count);
        if (record.minimizer_count == 0u) {
            collector->set_shape_outcome(job_index, 0u, 0u);
            collector->set_seed_anchor_authentication(
                job_index, query_authority_identity, empty_anchor_audit);
        }
    }

    gpu_arena& count_arena = reusable_gpu_arena();
    gpu_arena_trim_guard count_arena_trim{count_arena};
    // Exact emit growth must not replace the count/prefix backing still consumed
    // by the second submission, so it has an independent physical owner.
    gpu_arena emit_arena{*this};
    gpu_arena_trim_guard emit_arena_trim{emit_arena};
    for (std::size_t wave_begin = 0u;
         wave_begin < query_inputs.host_query_records.size();) {
        const std::size_t wave_end = wave_begin + std::min<std::size_t>(
            query_inputs.host_query_records.size() - wave_begin,
            maximum_jobs_per_wave);
        const std::size_t wave_count = wave_end - wave_begin;
        const auto state_capacity_bound = bioseq::common::checked_mul(
            static_cast<std::uint64_t>(wave_count), sizeof(mps_seed_wave_state));
        const auto state_preallocation_bound = state_capacity_bound.has_value()
            ? bioseq::common::checked_mul(
                  *state_capacity_bound,
                  bioseq::parallelism::k_vector_preallocation_headroom_factor)
            : std::optional<std::uint64_t>{};
        if (!state_preallocation_bound.has_value()) {
            throw vulkan_runtime_error(
                "grouped-SoA seed-only state-vector admission overflow");
        }
        bioseq::parallelism::memory_admission_failure state_failure{};
        auto reserved_states =
            admission.try_reserve(*state_preallocation_bound, &state_failure);
        if (!reserved_states) {
            throw vulkan_runtime_error(fmt::format(
                "grouped-SoA seed-only state admission denied: reason={} requested={}",
                bioseq::parallelism::memory_admission_failure_reason_name(
                    state_failure.reason),
                state_failure.requested_bytes));
        }
        // Declared before the vector so unwind destroys capacity before permit release.
        bioseq::parallelism::memory_permit state_permit = std::move(*reserved_states);
        std::vector<mps_seed_wave_state> states;
        bioseq::parallelism::memory_materialization_result state_materialization{};
        try {
            state_materialization = admission.materialize(
                state_permit,
                [&state_permit, &states, wave_count]()
                    -> bioseq::parallelism::memory_materialization_snapshot {
                    states.reserve(wave_count);
                    const auto actual = bioseq::common::checked_mul(
                        static_cast<std::uint64_t>(states.capacity()),
                        sizeof(mps_seed_wave_state));
                    if (!actual.has_value() || *actual > state_permit.bytes()) {
                        throw vulkan_runtime_error(
                            "grouped-SoA seed-only state capacity exceeded admission");
                    }
                    state_permit.shrink_to(*actual);
                    return {
                        .actual_allocation_bytes = *actual,
                        .fully_touched_bytes = 0u,
                        .is_host_visible = true,
                        .is_host_coherent = true,
                        .is_process_footprint_reflected = true,
                    };
                });
        } catch (const std::bad_alloc&) {
            throw vulkan_runtime_error(
                "grouped-SoA seed-only state allocation failed after admission");
        }
        const auto retained_state_bytes = bioseq::common::checked_mul(
            static_cast<std::uint64_t>(states.capacity()),
            sizeof(mps_seed_wave_state));
        if (!retained_state_bytes.has_value() ||
            state_materialization.allocation_snapshot.actual_allocation_bytes !=
                *retained_state_bytes ||
            state_materialization.admitted_bytes != *retained_state_bytes ||
            state_materialization.is_permit_released || !state_permit ||
            state_permit.bytes() != *retained_state_bytes) {
            throw vulkan_runtime_error(
                "grouped-SoA seed-only state materialization proof is inconsistent");
        }

        std::uint64_t count_arena_bytes = 0u;
        const auto query_record_bytes = bioseq::common::checked_mul(
            static_cast<std::uint64_t>(wave_count), k_mps_query_record_stride);
        if (!query_record_bytes.has_value() ||
            !append_mps_device_extent(count_arena_bytes, *query_record_bytes) ||
            !append_mps_device_extent(count_arena_bytes, sizeof(gpu_anchor_wire))) {
            throw vulkan_runtime_error(
                "grouped-SoA seed-only query-record arena plan overflow");
        }

        std::size_t active_jobs = 0u;
        for (std::size_t job_index = wave_begin; job_index < wave_end; ++job_index) {
            mps_seed_wave_state& state = states.emplace_back();
            state.job_index = job_index;
            state.local_index = job_index - wave_begin;
            state.query_record = query_inputs.host_query_records[job_index];
            if (state.query_record.minimizer_count == 0u)
                continue;
            ++active_jobs;
            state.num_blocks = std::max<std::uint32_t>(
                bioseq::common::ceil_div(state.query_record.minimizer_count,
                                         k_prefix_scan_block_size),
                1u);
            state.anchor_bound = static_cast<std::uint64_t>(
                                     state.query_record.minimizer_count) *
                                 max_occ_u64;

            const std::uint64_t counts_bytes =
                (static_cast<std::uint64_t>(state.query_record.minimizer_count) + 1u) *
                sizeof(std::uint32_t);
            const std::uint64_t prefix_bytes =
                static_cast<std::uint64_t>(state.query_record.minimizer_count) *
                sizeof(std::uint32_t);
            const std::uint64_t blocks_bytes =
                static_cast<std::uint64_t>(state.num_blocks) * sizeof(std::uint32_t);
            if (counts_bytes > features_.max_storage_buffer_range ||
                prefix_bytes > features_.max_storage_buffer_range ||
                blocks_bytes > features_.max_storage_buffer_range ||
                !append_mps_device_extent(count_arena_bytes, counts_bytes) ||
                !append_mps_device_extent(count_arena_bytes, prefix_bytes) ||
                !append_mps_device_extent(count_arena_bytes, blocks_bytes) ||
                prefix_bytes * 2u > features_.max_storage_buffer_range ||
                !append_mps_device_extent(count_arena_bytes, prefix_bytes * 2u)) {
                throw vulkan_runtime_error(
                    "grouped-SoA seed-only count arena or descriptor plan is invalid");
            }
        }

        if (active_jobs == 0u) {
            for (const mps_seed_wave_state& state : states) {
                run.jobs[state.job_index] = grouped_soa_seed_wire_job_span{
                    .ordinal = static_cast<std::uint32_t>(state.job_index),
                    .offset = run.anchor_wires.size(),
                    .count = 0u,
                };
            }
            wave_begin = wave_end;
            continue;
        }

        if (!count_arena.ensure_admitted_capacity(count_arena_bytes, 0u, 0u)) {
            throw vulkan_runtime_error(
                "grouped-SoA seed-only count-arena admission failed");
        }
        const gpu_arena_mark count_mark = count_arena.mark();
        gpu_arena_mark emit_mark{};
        bool emit_marked = false;
        try {
            const gpu_arena_slice query_records =
                count_arena.allocate_device(*query_record_bytes, 16u);
            const gpu_arena_slice count_placeholder =
                count_arena.allocate_device(sizeof(gpu_anchor_wire), 16u);
            if (!query_records.is_valid() || !count_placeholder.is_valid() ||
                query_records.buffer->mapped == nullptr) {
                throw vulkan_runtime_error(
                    "grouped-SoA seed-only count arena is unavailable");
            }
            auto* device_base =
                static_cast<std::byte*>(count_arena.device_buffer().mapped);
            std::memset(device_base + query_records.offset,
                        0,
                        static_cast<std::size_t>(query_records.size));
            std::memset(device_base + count_placeholder.offset,
                        0,
                        static_cast<std::size_t>(count_placeholder.size));
            // Count mode authenticates extents but never consumes anchor/key
            // outputs. Both bindings therefore use this tiny live descriptor.
            std::size_t emitting_jobs = 0u;
            for (mps_seed_wave_state& state : states) {
                std::memcpy(device_base + query_records.offset +
                                static_cast<std::uint64_t>(state.local_index) *
                                    k_mps_query_record_stride,
                            &state.query_record,
                            sizeof(state.query_record));
                if (state.query_record.minimizer_count == 0u)
                    continue;

                const std::uint64_t counts_bytes =
                    (static_cast<std::uint64_t>(state.query_record.minimizer_count) + 1u) *
                    sizeof(std::uint32_t);
                const std::uint64_t prefix_bytes =
                    static_cast<std::uint64_t>(state.query_record.minimizer_count) *
                    sizeof(std::uint32_t);
                const std::uint64_t blocks_bytes =
                    static_cast<std::uint64_t>(state.num_blocks) * sizeof(std::uint32_t);
                state.counts = count_arena.allocate_device(counts_bytes, 16u);
                state.prefix = count_arena.allocate_device(prefix_bytes, 16u);
                state.blocks = count_arena.allocate_device(blocks_bytes, 16u);
                state.repetitive = count_arena.allocate_device(prefix_bytes * 2u, 16u);
                if (!state.counts.is_valid() || !state.prefix.is_valid() ||
                    !state.blocks.is_valid() || !state.repetitive.is_valid()) {
                    throw vulkan_runtime_error(
                        "grouped-SoA seed-only count arena was oversubscribed");
                }
                std::memset(device_base + state.counts.offset,
                            0,
                            static_cast<std::size_t>(state.counts.size));
                std::memset(device_base + state.prefix.offset,
                            0,
                            static_cast<std::size_t>(state.prefix.size));
                std::memset(device_base + state.blocks.offset,
                            0,
                            static_cast<std::size_t>(state.blocks.size));
                std::memset(device_base + state.repetitive.offset,
                            0,
                            static_cast<std::size_t>(state.repetitive.size));

                state.push = gpu_grouped_soa_seed_push{
                    .abi_revision = k_gpu_grouped_soa_seed_push_abi_revision,
                    .words32_per_minimizer =
                        query_inputs.minimizer_words32_per_record,
                    .word_kind = query_inputs.minimizer_word_kind,
                    .hash_shift = query_inputs.hash_shift,
                    .pos_bits = query_inputs.pos_bits,
                    .kmer_size = kmer_size,
                    .partition_count = target.partition_count,
                    .max_occ = static_cast<std::uint32_t>(max_occ),
                    .emit_mode = 0u,
                    .record_count = 1u,
                    .target_ref_bits = anchor_sort_plan.target_ref_bits,
                    .query_minimizer_count = query_inputs.minimizer_count,
                    .control_word_count = resident_layout.control_word_count,
                    .position_word_count = resident_layout.position_word_count,
                    .anchor_capacity = 0u,
                    .max_occ_cap_lo =
                        static_cast<std::uint32_t>(target.max_occ_cap & 0xffff'ffffu),
                    .max_occ_cap_hi =
                        static_cast<std::uint32_t>(target.max_occ_cap >> 32u),
                    .status_index = state.query_record.minimizer_count,
                };
            }

            reset_dispatch_descriptor_pool_locked();
            collector->burn_before_reset(command_buffer_);
            check_vk(vkResetCommandBuffer(command_buffer_, 0),
                     "reset(grouped_soa_seed_only_count)");
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            check_vk(vkBeginCommandBuffer(command_buffer_, &begin),
                     "begin(grouped_soa_seed_only_count)");
            collector->begin(command_buffer_);
            record_host_to_shader(command_buffer_);
            record_compute_barrier(command_buffer_);
            // Each window depends on the previous window of the same job;
            // jobs within one window have disjoint outputs and remain batched.
            for (const auto& window : resident_layout.windows) {
                for (mps_seed_wave_state& state : states) {
                    if (state.query_record.minimizer_count == 0u)
                        continue;
                    gpu_grouped_soa_seed_push window_push = state.push;
                    std::array<VkDescriptorBufferInfo, 10> infos{{
                        {query_inputs.device_minimizer_words->buffer_handle,
                         0u,
                         query_layout.required_min_words_bytes},
                        {target.descriptors->buffer_handle, 0u, target.descriptor_bytes},
                        {target.controls->buffer_handle, 0u, resident_layout.control_padded_bytes},
                        {target.positions->buffer_handle, 0u, resident_layout.position_padded_bytes},
                        {query_records.buffer->buffer_handle,
                         query_records.offset +
                             static_cast<std::uint64_t>(state.local_index) *
                                 k_mps_query_record_stride,
                         sizeof(gpu_seed_query_record_wire)},
                        {state.counts.buffer->buffer_handle,
                         state.counts.offset,
                         state.counts.size},
                        {state.prefix.buffer->buffer_handle,
                         state.prefix.offset,
                         state.prefix.size},
                        {count_placeholder.buffer->buffer_handle,
                         count_placeholder.offset,
                         count_placeholder.size},
                        {count_placeholder.buffer->buffer_handle,
                         count_placeholder.offset,
                         count_placeholder.size},
                        {state.repetitive.buffer->buffer_handle,
                         state.repetitive.offset,
                         state.repetitive.size},
                    }};
                    coordinate_pipeline_detail::bind_grouped_soa_buffer_window(
                        window_push, infos, window, features_.max_storage_buffer_range);
                    record_compute_dispatch_locked(command_buffer_,
                                                   count_pipe,
                                                   infos,
                                                   &window_push,
                                                   sizeof(window_push),
                                                   1u,
                                                   collector.get(),
                                                   state.job_index);
                }
                record_compute_barrier(command_buffer_);
            }
            for (mps_seed_wave_state& state : states) {
                if (state.query_record.minimizer_count == 0u)
                    continue;
                const std::array<VkDescriptorBufferInfo, 3> infos{{
                    {state.counts.buffer->buffer_handle,
                     state.counts.offset,
                     state.counts.size},
                    {state.prefix.buffer->buffer_handle,
                     state.prefix.offset,
                     state.prefix.size},
                    {state.blocks.buffer->buffer_handle,
                     state.blocks.offset,
                     state.blocks.size},
                }};
                const scan_push_constants push{state.query_record.minimizer_count,
                                               k_prefix_scan_block_size,
                                               state.num_blocks,
                                               0u};
                record_compute_dispatch_locked(command_buffer_,
                                               scan_block_pipe,
                                               infos,
                                               &push,
                                               sizeof(push),
                                               state.num_blocks,
                                               collector.get(),
                                               state.job_index);
            }
            record_compute_barrier(command_buffer_);
            for (mps_seed_wave_state& state : states) {
                if (state.query_record.minimizer_count == 0u)
                    continue;
                const std::array<VkDescriptorBufferInfo, 3> infos{{
                    {state.counts.buffer->buffer_handle,
                     state.counts.offset,
                     state.counts.size},
                    {state.prefix.buffer->buffer_handle,
                     state.prefix.offset,
                     state.prefix.size},
                    {state.blocks.buffer->buffer_handle,
                     state.blocks.offset,
                     state.blocks.size},
                }};
                const scan_push_constants push{state.query_record.minimizer_count,
                                               k_prefix_scan_block_size,
                                               state.num_blocks,
                                               0u};
                record_compute_dispatch_locked(command_buffer_,
                                               scan_add_pipe,
                                               infos,
                                               &push,
                                               sizeof(push),
                                               1u,
                                               collector.get(),
                                               state.job_index);
            }
            record_compute_barrier(command_buffer_);
            for (mps_seed_wave_state& state : states) {
                if (state.query_record.minimizer_count == 0u)
                    continue;
                const std::array<VkDescriptorBufferInfo, 3> infos{{
                    {state.counts.buffer->buffer_handle,
                     state.counts.offset,
                     state.counts.size},
                    {state.prefix.buffer->buffer_handle,
                     state.prefix.offset,
                     state.prefix.size},
                    {state.blocks.buffer->buffer_handle,
                     state.blocks.offset,
                     state.blocks.size},
                }};
                const scan_push_constants push{state.query_record.minimizer_count,
                                               k_prefix_scan_block_size,
                                               state.num_blocks,
                                               1u};
                record_compute_dispatch_locked(
                    command_buffer_,
                    scan_add_pipe,
                    infos,
                    &push,
                    sizeof(push),
                    bioseq::common::ceil_div(state.query_record.minimizer_count, 128u),
                    collector.get(),
                    state.job_index);
            }
            record_shader_to_host(command_buffer_);
            submit_and_wait(device_,
                            queue_,
                            command_buffer_,
                            fence_,
                            "submit(grouped_soa_seed_only_count)",
                            collector.get());

            std::uint64_t exact_emit_arena_bytes = 0u;
            std::uint64_t exact_download_arena_bytes = 0u;
            std::uint64_t wave_anchor_count = 0u;
            for (mps_seed_wave_state& state : states) {
                if (state.query_record.minimizer_count == 0u)
                    continue;
                std::uint32_t status = 0u;
                std::uint32_t last_prefix = 0u;
                std::uint32_t last_count = 0u;
                std::memcpy(&status,
                            device_base + state.counts.offset +
                                static_cast<std::uint64_t>(
                                    state.query_record.minimizer_count) *
                                    sizeof(std::uint32_t),
                            sizeof(status));
                std::memcpy(&last_prefix,
                            device_base + state.prefix.offset +
                                static_cast<std::uint64_t>(
                                    state.query_record.minimizer_count - 1u) *
                                    sizeof(std::uint32_t),
                            sizeof(last_prefix));
                std::memcpy(&last_count,
                            device_base + state.counts.offset +
                                static_cast<std::uint64_t>(
                                    state.query_record.minimizer_count - 1u) *
                                    sizeof(std::uint32_t),
                            sizeof(last_count));
                state.total_anchors =
                    static_cast<std::uint64_t>(last_prefix) + last_count;
                if (status != 0u || state.total_anchors > state.anchor_bound ||
                    state.total_anchors >
                        (std::numeric_limits<std::uint32_t>::max)()) {
                    throw vulkan_runtime_error(fmt::format(
                        "grouped-SoA seed-only count authentication failed for job {}: "
                        "status=0x{:08x} anchors={} bound={}",
                        state.job_index,
                        status,
                        state.total_anchors,
                        state.anchor_bound));
                }
                state.total_anchors_u32 =
                    static_cast<std::uint32_t>(state.total_anchors);
                collector->set_shape_outcome(
                    state.job_index, state.total_anchors, 0u);
                if (state.total_anchors != 0u) {
                    ++emitting_jobs;
                    state.emit_workgroups = plan_workgroups(
                        state.total_anchors, features_.max_workgroup_count_x);
                    const auto anchor_bytes = bioseq::common::checked_mul(
                        state.total_anchors, sizeof(gpu_anchor_wire));
                    const auto key_bytes = bioseq::common::checked_mul(
                        state.total_anchors, sizeof(std::uint64_t));
                    const auto histogram_bytes = bioseq::common::checked_mul(
                        static_cast<std::uint64_t>(
                            state.emit_workgroups.num_workgroups),
                        256u * sizeof(std::uint32_t));
                    const auto next_wave_anchor_count = bioseq::common::checked_add(
                        wave_anchor_count, state.total_anchors);
                    if (!anchor_bytes.has_value() || !key_bytes.has_value() ||
                        !histogram_bytes.has_value() ||
                        !next_wave_anchor_count.has_value() ||
                        *anchor_bytes > features_.max_storage_buffer_range ||
                        *key_bytes > features_.max_storage_buffer_range ||
                        *histogram_bytes > features_.max_storage_buffer_range ||
                        !append_mps_device_extent(
                            exact_emit_arena_bytes, *anchor_bytes) ||
                        !append_mps_device_extent(
                            exact_emit_arena_bytes, *key_bytes) ||
                        !append_mps_device_extent(
                            exact_emit_arena_bytes, *anchor_bytes) ||
                        !append_mps_device_extent(
                            exact_emit_arena_bytes, *key_bytes) ||
                        !append_mps_device_extent(
                            exact_emit_arena_bytes, *histogram_bytes) ||
                        !append_mps_download_extent(
                            exact_download_arena_bytes, *anchor_bytes)) {
                        throw vulkan_runtime_error(
                            "grouped-SoA seed-only exact emit arena plan is invalid");
                    }
                    wave_anchor_count = *next_wave_anchor_count;
                }
            }

            if (emitting_jobs == 0u) {
                for (const mps_seed_wave_state& state : states) {
                    if (state.query_record.minimizer_count != 0u) {
                        collector->set_seed_anchor_authentication(
                            state.job_index,
                            query_authority_identity,
                            empty_anchor_audit);
                    }
                    run.jobs[state.job_index] = grouped_soa_seed_wire_job_span{
                        .ordinal = static_cast<std::uint32_t>(state.job_index),
                        .offset = run.anchor_wires.size(),
                        .count = 0u,
                    };
                }
                count_arena.rewind(count_mark);
                wave_begin = wave_end;
                continue;
            }

            if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
                if (run.anchor_wires.size() >
                    (std::numeric_limits<std::uint64_t>::max)()) {
                    throw vulkan_runtime_error(
                        "grouped-SoA seed-only prior host anchor size is not representable");
                }
            }
            const auto required_host_anchor_count = bioseq::common::checked_add(
                static_cast<std::uint64_t>(run.anchor_wires.size()), wave_anchor_count);
            if (!required_host_anchor_count.has_value() ||
                *required_host_anchor_count >
                    (std::numeric_limits<std::size_t>::max)()) {
                throw vulkan_runtime_error(
                    "grouped-SoA seed-only completed host anchor count overflow");
            }
            ensure_mps_host_anchor_capacity(
                run, static_cast<std::size_t>(*required_host_anchor_count));

            if (!emit_arena.ensure_admitted_capacity(
                    exact_emit_arena_bytes, 0u, exact_download_arena_bytes)) {
                throw vulkan_runtime_error(
                    "grouped-SoA seed-only exact emit-arena admission failed");
            }
            emit_mark = emit_arena.mark();
            emit_marked = true;
            for (mps_seed_wave_state& state : states) {
                if (state.total_anchors == 0u)
                    continue;
                const std::uint64_t anchor_bytes =
                    state.total_anchors * sizeof(gpu_anchor_wire);
                const std::uint64_t key_bytes =
                    state.total_anchors * sizeof(std::uint64_t);
                const std::uint64_t histogram_bytes =
                    static_cast<std::uint64_t>(
                        state.emit_workgroups.num_workgroups) *
                    256u * sizeof(std::uint32_t);
                state.anchors = emit_arena.allocate_device(anchor_bytes, 16u);
                state.anchor_keys = emit_arena.allocate_device(key_bytes, 16u);
                state.anchors_scratch =
                    emit_arena.allocate_device(anchor_bytes, 16u);
                state.anchor_keys_scratch =
                    emit_arena.allocate_device(key_bytes, 16u);
                state.histogram =
                    emit_arena.allocate_device(histogram_bytes, 16u);
                state.download =
                    emit_arena.allocate_download(anchor_bytes, 16u);
                if (!state.anchors.is_valid() || !state.anchor_keys.is_valid() ||
                    !state.anchors_scratch.is_valid() ||
                    !state.anchor_keys_scratch.is_valid() ||
                    !state.histogram.is_valid() || !state.download.is_valid() ||
                    state.download.buffer->mapped == nullptr) {
                    throw vulkan_runtime_error(
                        "grouped-SoA seed-only exact emit arena was oversubscribed");
                }
            }

            reset_dispatch_descriptor_pool_locked();
            collector->burn_before_reset(command_buffer_);
            check_vk(vkResetCommandBuffer(command_buffer_, 0),
                     "reset(grouped_soa_seed_only_emit)");
            begin = VkCommandBufferBeginInfo{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            check_vk(vkBeginCommandBuffer(command_buffer_, &begin),
                     "begin(grouped_soa_seed_only_emit)");
            collector->begin(command_buffer_);
            record_host_to_shader(command_buffer_);
            record_compute_barrier(command_buffer_);
            // Each window depends on the previous window of the same job;
            // jobs within one window have disjoint outputs and remain batched.
            for (const auto& window : resident_layout.windows) {
                for (mps_seed_wave_state& state : states) {
                    if (state.total_anchors == 0u)
                        continue;
                    gpu_grouped_soa_seed_push emit_push = state.push;
                    emit_push.emit_mode = 1u;
                    emit_push.anchor_capacity = state.total_anchors_u32;
                    std::array<VkDescriptorBufferInfo, 10> infos{{
                        {query_inputs.device_minimizer_words->buffer_handle,
                         0u,
                         query_layout.required_min_words_bytes},
                        {target.descriptors->buffer_handle, 0u, target.descriptor_bytes},
                        {target.controls->buffer_handle, 0u, resident_layout.control_padded_bytes},
                        {target.positions->buffer_handle, 0u, resident_layout.position_padded_bytes},
                        {query_records.buffer->buffer_handle,
                         query_records.offset +
                             static_cast<std::uint64_t>(state.local_index) *
                                 k_mps_query_record_stride,
                         sizeof(gpu_seed_query_record_wire)},
                        {state.counts.buffer->buffer_handle,
                         state.counts.offset,
                         state.counts.size},
                        {state.prefix.buffer->buffer_handle,
                         state.prefix.offset,
                         state.prefix.size},
                        {state.anchors.buffer->buffer_handle,
                         state.anchors.offset,
                         state.anchors.size},
                        {state.anchor_keys.buffer->buffer_handle,
                         state.anchor_keys.offset,
                         state.anchor_keys.size},
                        {state.repetitive.buffer->buffer_handle,
                         state.repetitive.offset,
                         state.repetitive.size},
                    }};
                    coordinate_pipeline_detail::bind_grouped_soa_buffer_window(
                        emit_push, infos, window, features_.max_storage_buffer_range);
                    record_compute_dispatch_locked(command_buffer_,
                                                   emit_pipe,
                                                   infos,
                                                   &emit_push,
                                                   sizeof(emit_push),
                                                   1u,
                                                   collector.get(),
                                                   state.job_index);
                }
                record_compute_barrier(command_buffer_);
            }
            for (mps_seed_wave_state& state : states) {
                if (state.total_anchors == 0u)
                    continue;
                const gpu_sort_plan sort_plan{
                    state.total_anchors,
                    sizeof(std::uint64_t),
                    sizeof(gpu_anchor_wire),
                    8u,
                    anchor_sort_plan.pass_count,
                };
                record_radix_sort_locked(command_buffer_,
                                         state.anchor_keys,
                                         state.anchors,
                                         state.anchor_keys_scratch,
                                         state.anchors_scratch,
                                         state.histogram,
                                         sort_plan,
                                         state.emit_workgroups.num_workgroups,
                                         state.emit_workgroups.elems_per_wg,
                                         3u,
                                         state.sorted_in_scratch,
                                         collector.get(),
                                         state.job_index);
            }

            VkMemoryBarrier compute_output{};
            compute_output.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            compute_output.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            compute_output.dstAccessMask =
                VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(command_buffer_,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                                 0u,
                                 1u,
                                 &compute_output,
                                 0u,
                                 nullptr,
                                 0u,
                                 nullptr);
            bool copied_anchors = false;
            for (const mps_seed_wave_state& state : states) {
                if (state.total_anchors == 0u)
                    continue;
                const gpu_arena_slice& sorted =
                    state.sorted_in_scratch ? state.anchors_scratch : state.anchors;
                const VkBufferCopy copy{
                    sorted.offset,
                    state.download.offset,
                    state.total_anchors * sizeof(gpu_anchor_wire),
                };
                vkCmdCopyBuffer(command_buffer_,
                                sorted.buffer->buffer_handle,
                                state.download.buffer->buffer_handle,
                                1u,
                                &copy);
                copied_anchors = true;
            }
            if (copied_anchors) {
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
            }
            submit_and_wait(device_,
                            queue_,
                            command_buffer_,
                            fence_,
                            "submit(grouped_soa_seed_only_emit)",
                            collector.get());

            for (mps_seed_wave_state& state : states) {
                if (state.query_record.minimizer_count == 0u) {
                    run.jobs[state.job_index] = grouped_soa_seed_wire_job_span{
                        .ordinal = static_cast<std::uint32_t>(state.job_index),
                        .offset = run.anchor_wires.size(),
                        .count = 0u,
                    };
                    continue;
                }
                std::uint32_t status = 0u;
                std::memcpy(&status,
                            device_base + state.counts.offset +
                                static_cast<std::uint64_t>(
                                    state.query_record.minimizer_count) *
                                    sizeof(std::uint32_t),
                            sizeof(status));
                if (status != 0u) {
                    throw vulkan_runtime_error(fmt::format(
                        "grouped-SoA seed-only emit rejected job {} with status 0x{:08x}",
                        state.job_index,
                        status));
                }

                const std::size_t host_offset = run.anchor_wires.size();
                const std::size_t job_anchor_count =
                    static_cast<std::size_t>(state.total_anchors);
                run.anchor_wires.resize(host_offset + job_anchor_count);
                if (job_anchor_count != 0u) {
                    std::memcpy(run.anchor_wires.data() + host_offset,
                                static_cast<const std::byte*>(
                                    state.download.buffer->mapped) +
                                    state.download.offset,
                                job_anchor_count * sizeof(gpu_anchor_wire));
                }
                const std::span<gpu_anchor_wire> all_wires{run.anchor_wires};
                const std::span<gpu_anchor_wire> job_wires =
                    all_wires.subspan(host_offset, job_anchor_count);
                const seed_anchor_tie_normalization_result normalization =
                    normalize_gpu_anchor_equal_ref_ties(job_wires);
                const seed_anchor_canonical_order_audit order_audit =
                    audit_gpu_anchor_canonical_order(job_wires);
                if (!normalization.gpu_ref_order_checked ||
                    !normalization.gpu_ref_order_accepted ||
                    !order_audit.canonical_order_checked ||
                    !order_audit.canonical_order_accepted ||
                    order_audit.anchor_count != state.total_anchors) {
                    throw vulkan_runtime_error(fmt::format(
                        "grouped-SoA seed-only anchor order authentication failed for job {}",
                        state.job_index));
                }
                collector->set_seed_anchor_authentication(
                    state.job_index, query_authority_identity, order_audit);
                run.jobs[state.job_index] = grouped_soa_seed_wire_job_span{
                    .ordinal = static_cast<std::uint32_t>(state.job_index),
                    .offset = host_offset,
                    .count = job_anchor_count,
                };
            }

            if (emit_marked) {
                emit_arena.rewind(emit_mark);
            }
            count_arena.rewind(count_mark);
            wave_begin = wave_end;
        } catch (...) {
            if (emit_marked) {
                emit_arena.rewind(emit_mark);
            }
            count_arena.rewind(count_mark);
            throw;
        }
    }

    collector->burn_before_reset(command_buffer_);
    check_vk(vkResetCommandBuffer(command_buffer_, 0),
             "reset(grouped_soa_seed_only-post)");
    const bool emit_arena_idle = emit_arena_trim.release_now();
    const bool count_arena_idle = count_arena_trim.release_now();
    if (!emit_arena_idle || !count_arena_idle) {
        throw vulkan_runtime_error(
            "grouped-SoA seed-only count or emit arena did not become idle");
    }
    if (!grouped_soa_seed_wire_jobs_cover(run.jobs, run.anchor_wires.size())) {
        throw vulkan_runtime_error(
            "grouped-SoA seed-only output job partition is invalid");
    }

    require_immutable_metal_policy_live("grouped-SoA seed-only receipt sealing");
    run.seed_operation_receipt = collector->seal();
    const runtime_pipeline_operation_audit& query_audit =
        query_authorization.pipeline_token_.audit();
    const runtime_pipeline_operation_audit& seed_audit =
        run.seed_operation_receipt.audit();
    if (!query_authorization.pipeline_token_.is_valid_for_query_sketch() ||
        !run.seed_operation_receipt.is_valid_for_grouped_soa_seed_anchor_roundtrip() ||
        query_audit.device_uuid != seed_audit.device_uuid ||
        query_audit.api_version != seed_audit.api_version ||
        query_audit.driver_version != seed_audit.driver_version ||
        query_audit.vendor_id != seed_audit.vendor_id ||
        query_audit.device_id != seed_audit.device_id ||
        query_audit.pipeline_cache_uuid != seed_audit.pipeline_cache_uuid ||
        query_audit.seed_sha256 != seed_audit.seed_sha256 ||
        query_audit.seed_size != seed_audit.seed_size ||
        query_audit.manifest_checksum != seed_audit.manifest_checksum ||
        query_audit.seed_receipt_identity != seed_audit.seed_receipt_identity ||
        query_audit.archive_set != seed_audit.archive_set ||
        query_audit.cache_qualification != seed_audit.cache_qualification) {
        throw vulkan_runtime_error(
            "grouped-SoA seed-only query and seed completion identities disagree");
    }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    // All query consumers inside this seed operation have completed. Retain the
    // existing causal proof for final SQL cache accounting; never mint a copy.
    run.query_operation_receipt = std::move(query_authorization.pipeline_token_);
#endif
    return run;
}

} // namespace bioseq::gpu::vulkan
