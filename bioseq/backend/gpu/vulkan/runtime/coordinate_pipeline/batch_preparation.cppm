/**
 * @file batch_preparation.cppm
 * @brief Validates batched coordinate inputs and owns per-wave host state.
 */
module;

#include <cstdint>
#include <libassert/assert.hpp>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

export module bioseq.gpu.vulkan.runtime:coordinate_pipeline.batch_preparation;

import :frontend_types;
import :support;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.parallelism.memory_admission;
import bioseq.instrumentation;
import std;

export namespace bioseq::gpu::vulkan::coordinate_pipeline_detail {

/// Maximum logical jobs retained in one bounded submission wave.
inline constexpr std::uint32_t k_gpu_seed_coordinate_batch_wave_size = 4096u;

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
    gpu_arena_slice repetitive_s{};
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
    buffer candidate_header_buf;
    buffer candidates_buf;
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

/// @brief Representation and resident-generation identity shared by one batch.
struct coordinate_batch_shape {
    gpu_seed_index_representation representation = gpu_seed_index_representation::invalid;
    bool grouped_soa_selected = false;
    persisted_grouped_soa_coordinate_config_identity operation_configuration{};
    const gpu_grouped_soa_resident_view* common_view = nullptr;
    std::uint64_t common_target_len = 0u;
};

/// @brief Validated word extents for a grouped-SoA resident generation.
struct grouped_soa_resident_layout {
    std::uint32_t control_word_count = 0u;
    std::uint32_t position_word_count = 0u;
    std::uint64_t control_padded_bytes = 0u;
    std::uint64_t position_padded_bytes = 0u;
    std::vector<gpu_grouped_soa_buffer_window> windows{};
};

/// @brief Validated minimizer layout used by upload and resident-buffer paths.
struct coordinate_query_layout {
    std::uint32_t required_words_per_minimizer = 0u;
    std::uint64_t required_min_words_bytes = 0u;
};

/// @brief Bounds and storage budget for one ordered submission wave.
struct coordinate_wave_plan {
    std::size_t end = 0u;
    std::size_t count = 0u;
    std::uint64_t arena_bytes = 0u;
};

/// @brief Device binding selected for minimizer words after host preparation.
struct coordinate_minimizer_binding {
    VkBuffer buffer = VK_NULL_HANDLE;
    std::uint64_t offset = 0u;
    std::uint64_t range = 0u;
};

/// @brief Derives shader-visible SSBO element extents from the exact
/// descriptor ranges used by one grouped-SoA count, diagnostic, or emit pass.
/// @throws vulkan_runtime_error When a descriptor is implicit, empty, or too large.
inline void
populate_grouped_soa_seed_descriptor_extents(gpu_grouped_soa_seed_push& push,
                                             const std::array<VkDescriptorBufferInfo, 10>& infos,
                                             std::uint64_t max_storage_buffer_range) {
    constexpr std::array<std::uint64_t, 10> element_strides{
        sizeof(std::uint32_t),
        sizeof(gpu_grouped_soa_resident_partition_descriptor),
        sizeof(std::uint32_t),
        sizeof(std::uint32_t),
        sizeof(gpu_seed_query_record_wire),
        sizeof(std::uint32_t),
        sizeof(std::uint32_t),
        sizeof(gpu_anchor_wire),
        sizeof(std::uint64_t),
        sizeof(std::uint32_t),
    };
    std::array<std::uint32_t, 10> extents{};
    for (std::size_t index = 0u; index < infos.size(); ++index) {
        const VkDescriptorBufferInfo& info = infos[index];
        if (info.buffer == VK_NULL_HANDLE || info.range == 0u || info.range == VK_WHOLE_SIZE ||
            info.range > max_storage_buffer_range) {
            throw vulkan_runtime_error(
                "gpu grouped-SoA seed descriptor range is not explicit and bounded");
        }
        // This is the SSBO runtime-array element count: floor(range / stride).
        // Count-mode anchor bindings are inert dummies and may retain a tail.
        const std::uint64_t extent = info.range / element_strides[index];
        if (extent > (std::numeric_limits<std::uint32_t>::max)()) {
            throw vulkan_runtime_error(
                "gpu grouped-SoA seed descriptor element extent exceeds push ABI");
        }
        extents[index] = static_cast<std::uint32_t>(extent);
    }
    push.minimizer_word_extent = extents[0];
    push.partition_extent = extents[1];
    push.control_word_extent = extents[2];
    push.position_word_extent = extents[3];
    push.query_record_extent = extents[4];
    push.count_extent = extents[5];
    push.prefix_extent = extents[6];
    push.anchor_extent = extents[7];
    push.anchor_key_extent = extents[8];
    push.repetitive_word_extent = extents[9];
    if (std::uint64_t{extents[9]} < std::uint64_t{push.status_index} * 2u) {
        throw vulkan_runtime_error("gpu grouped-SoA repetitive-flag descriptor is undersized");
    }
}

/// @brief Bind one complete-partition window while preserving aggregate offsets in metadata.
inline void bind_grouped_soa_buffer_window(
    gpu_grouped_soa_seed_push& push,
    std::array<VkDescriptorBufferInfo, 10>& infos,
    const gpu_grouped_soa_buffer_window& window,
    std::uint64_t max_storage_buffer_range) {
    push.partition_begin = window.partition_begin;
    push.partition_count = window.partition_end;
    push.control_base_word = static_cast<std::uint32_t>(window.control_offset / 4u);
    push.position_base_word = static_cast<std::uint32_t>(window.position_offset / 4u);
    infos[2].offset = window.control_offset;
    infos[2].range = window.control_bytes;
    infos[3].offset = window.position_offset;
    infos[3].range = window.position_bytes;
    populate_grouped_soa_seed_descriptor_extents(push, infos, max_storage_buffer_range);
}

/// @brief Classifies a batch and validates its common target identity.
/// @param[in] jobs Coordinate jobs that must share one index representation.
/// @param[in] query_inputs Query records used to validate every job index.
/// @param[in] coordinate_vote_selected Whether diagonal-vote reduction was selected.
/// @param[in] features Live device identity and capability limits.
/// @param[in] kmer_size K-mer span used by the seed stage.
/// @param[in] max_occ Maximum target occurrences emitted per minimizer.
/// @param[in] coordinate_algorithm Requested coordinate reduction algorithm.
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
/// @return Uniform representation and grouped-SoA configuration identity.
/// @throws vulkan_runtime_error When jobs mix representations or resident identities.
[[nodiscard]] inline coordinate_batch_shape
inspect_coordinate_batch_shape(std::span<const gpu_seed_coordinate_job> jobs,
                               const gpu_seed_query_inputs& query_inputs,
                               bool coordinate_vote_selected,
                               const vulkan_feature_set& features,
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
                               const gpu_persisted_mapq_config& mapq_config) {
    coordinate_batch_shape shape{
        .representation = jobs.empty() ? gpu_seed_index_representation::invalid
                                       : classify_gpu_seed_index_representation(jobs.front()),
    };
    shape.grouped_soa_selected =
        shape.representation == gpu_seed_index_representation::grouped_soa;
    const bool mapq_selected = mapq_config.is_valid();
    if (mapq_config != gpu_persisted_mapq_config{} && !mapq_selected) {
        throw vulkan_runtime_error("gpu persisted MAPQ configuration identity is invalid");
    }
    if (mapq_selected && (!shape.grouped_soa_selected || coordinate_vote_selected)) {
        throw vulkan_runtime_error("gpu persisted MAPQ requires grouped-SoA tiled-DP execution");
    }
    if (mapq_selected &&
        (min_chain_score <= 0 || min_seed_count <= 0 || features.max_workgroup_invocations < 128u ||
         features.max_workgroup_size_x < 128u ||
         features.max_storage_buffer_range <
             static_cast<std::uint64_t>(mapq_config.candidate_capacity) *
                 sizeof(gpu_chain_candidate_wire))) {
        throw vulkan_runtime_error(
            "gpu persisted MAPQ device/configuration bounds are unsupported");
    }
    if (shape.grouped_soa_selected && coordinate_vote_selected) {
        throw vulkan_runtime_error(
            "gpu grouped-SoA direct route requires Vulkan tiled-DP coordinate output");
    }
    if (shape.grouped_soa_selected && features.software_device) {
        throw vulkan_runtime_error("gpu grouped-SoA direct route rejects software Vulkan devices");
    }
    shape.operation_configuration =
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
                                                              probe_limit,
                                                              mapq_config);
    if (shape.grouped_soa_selected && !shape.operation_configuration.is_valid()) {
        throw vulkan_runtime_error("gpu grouped-SoA coordinate configuration identity is invalid");
    }

    for (const gpu_seed_coordinate_job& job : jobs) {
        const gpu_seed_index_representation job_representation =
            classify_gpu_seed_index_representation(job);
        if (job_representation == gpu_seed_index_representation::invalid ||
            job_representation != shape.representation) {
            throw vulkan_runtime_error("gpu seed-to-coordinate resident index representation must "
                                       "be strict XOR and uniform");
        }
        if (job.query_index >= query_inputs.host_query_records.size()) {
            throw vulkan_runtime_error("gpu seed-to-coordinate job query_index out of range");
        }
        if (job.target_len == 0u) {
            throw vulkan_runtime_error("gpu seed-to-coordinate target length must be nonzero");
        }
        if (shape.grouped_soa_selected) {
            if (job.target_len > (std::uint64_t{1} << 63u)) {
                throw vulkan_runtime_error("gpu grouped-SoA target length exceeds anchor-key ABI");
            }
            if (shape.common_view == nullptr) {
                shape.common_view = job.grouped_soa;
                shape.common_target_len = job.target_len;
            } else if (job.grouped_soa != shape.common_view) {
                throw vulkan_runtime_error(
                    "gpu grouped-SoA batch must bind one resident generation");
            } else if (job.target_len != shape.common_target_len) {
                throw vulkan_runtime_error(
                    "gpu grouped-SoA batch target identity is inconsistent");
            }
        }
    }
    return shape;
}

/// @brief Validates grouped-SoA resident buffers and records their audit identity.
/// @param[in] owner Runtime that must own every resident buffer.
/// @param[in] shape Common representation and resident generation.
/// @param[in] query_inputs Query sketch ABI traits.
/// @param[in] features Live Vulkan identity and storage-range limit.
/// @param[in] kmer_size K-mer span required by the persisted generation.
/// @param[in] max_occ Requested occurrence cap.
/// @param[in,out] run Batch result receiving resident-generation statistics.
/// @return Padded shader-visible word extents for the resident generation.
/// @throws vulkan_runtime_error When identity, layout, ownership, or range checks fail.
[[nodiscard]] inline grouped_soa_resident_layout validate_grouped_soa_resident(
    const runtime* owner,
    const coordinate_batch_shape& shape,
    const gpu_seed_query_inputs& query_inputs,
    const vulkan_feature_set& features,
    std::uint32_t kmer_size,
    std::int32_t max_occ,
    gpu_seed_coordinate_batch_result& run) {
    grouped_soa_resident_layout layout{};
    if (!shape.grouped_soa_selected) {
        return layout;
    }
    const gpu_grouped_soa_resident_view* common_shape_view = shape.common_view;
    if (common_shape_view == nullptr) {
        throw vulkan_runtime_error("gpu grouped-SoA route has no resident generation");
    }
    if (kmer_size == 0u || kmer_size > 255u || query_inputs.hash_shift >= 64u ||
        query_inputs.pos_bits >= 64u) {
        throw vulkan_runtime_error("gpu grouped-SoA query sketch traits exceed the grouped-SoA ABI");
    }
    // Accepts only coherent live buffers owned by the executing runtime.
    const auto valid_resident_buffer = [owner](const buffer* candidate) {
        return candidate != nullptr && candidate->owner == owner && candidate->is_valid() &&
               candidate->is_host_visible_coherent() && candidate->mapped != nullptr;
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
        common_shape_view->controls->buffer_handle == common_shape_view->positions->buffer_handle) {
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
    // Converts a nonempty byte extent to its bounded shader-visible word count.
    const auto checked_word_count = [](std::uint64_t bytes, const char* label) {
        constexpr std::uint64_t maximum_word_bytes =
            static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)()) * 4u;
        if (bytes == 0u || bytes > maximum_word_bytes) {
            throw vulkan_runtime_error(
                fmt::format("gpu grouped-SoA {} byte extent exceeds GLSL word ABI", label));
        }
        return static_cast<std::uint32_t>((bytes + 3u) / 4u);
    };
    layout.control_word_count = checked_word_count(common_shape_view->control_bytes, "control");
    layout.position_word_count = checked_word_count(common_shape_view->position_bytes, "position");
    layout.control_padded_bytes = static_cast<std::uint64_t>(layout.control_word_count) * 4u;
    layout.position_padded_bytes = static_cast<std::uint64_t>(layout.position_word_count) * 4u;
    if (layout.control_padded_bytes > common_shape_view->controls->bytes ||
        layout.position_padded_bytes > common_shape_view->positions->bytes ||
        common_shape_view->descriptor_bytes > features.max_storage_buffer_range) {
        throw vulkan_runtime_error(
            "gpu grouped-SoA resident byte extent exceeds its buffer or Vulkan range");
    }
    const bool nil_view_uuid = std::all_of(common_shape_view->device_uuid.begin(),
                                           common_shape_view->device_uuid.end(),
                                           [](std::uint8_t byte) { return byte == 0u; });
    if (nil_view_uuid || common_shape_view->device_uuid != features.device_uuid) {
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
            descriptor.segment_end_bp > shape.common_target_len) {
            throw vulkan_runtime_error("gpu grouped-SoA resident descriptor validation failed");
        }
    }
    const std::span<const std::byte> descriptor_span{
        descriptor_bytes, static_cast<std::size_t>(common_shape_view->descriptor_bytes)};
    for (std::uint32_t first = 0u; first < common_shape_view->partition_count;) {
        const auto window = plan_gpu_grouped_soa_buffer_window(
            descriptor_span, first, common_shape_view->control_bytes, common_shape_view->position_bytes,
            features.max_storage_buffer_range, features.min_storage_buffer_offset_alignment);
        if (!window) {
            throw vulkan_runtime_error("gpu grouped-SoA partition cannot fit an aligned storage-buffer binding");
        }
        layout.windows.push_back(*window);
        first = window->partition_end;
    }
    if (max_occ >= 0 && static_cast<std::uint64_t>(max_occ) > common_shape_view->max_occ_cap) {
        throw vulkan_runtime_error("gpu grouped-SoA query max_occ exceeds persisted generation cap");
    }
    run.grouped_soa_direct.generation_id = common_shape_view->generation_id;
    run.grouped_soa_direct.source_identity = common_shape_view->source_identity;
    run.grouped_soa_direct.cold_load_identity = common_shape_view->cold_load_identity;
    return layout;
}

/// @brief Validates query records and minimizer storage extents.
/// @param[in] owner Runtime that must own a resident minimizer buffer.
/// @param[in] query_inputs Query records and either host or device minimizer storage.
/// @param[in] features Live device storage-buffer limit.
/// @param[in] grouped_soa_selected Whether the direct persisted route is active.
/// @param[in] kmer_size Minimum valid grouped-SoA query length.
/// @return Required word shape and minimum byte extent.
/// @throws vulkan_runtime_error When records, representation, ownership, or sizes are invalid.
[[nodiscard]] inline coordinate_query_layout validate_coordinate_query_layout(
    const runtime* owner,
    const gpu_seed_query_inputs& query_inputs,
    const vulkan_feature_set& features,
    bool grouped_soa_selected,
    std::uint32_t kmer_size) {
    const std::uint32_t total_mc = query_inputs.minimizer_count;
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
                 features.max_storage_buffer_range)) {
            throw vulkan_runtime_error(
                "gpu grouped-SoA query record violates local count/status ABI");
        }
    }

    coordinate_query_layout layout{};
    layout.required_words_per_minimizer =
        query_inputs.minimizer_word_kind == 0u || query_inputs.minimizer_word_kind == 1u ? 1u
        : query_inputs.minimizer_word_kind == 2u                                         ? 2u
                                                                                         : 4u;
    if (query_inputs.minimizer_word_kind > 3u ||
        query_inputs.minimizer_words32_per_record < layout.required_words_per_minimizer ||
        (query_inputs.minimizer_word_kind != 0u &&
         static_cast<std::uint64_t>(total_mc) >
             (std::numeric_limits<std::uint64_t>::max)() /
                 (static_cast<std::uint64_t>(query_inputs.minimizer_words32_per_record) *
                  sizeof(std::uint32_t)))) {
        throw vulkan_runtime_error(
            "gpu seed-to-coordinate minimizer word shape is invalid or overflows");
    }
    layout.required_min_words_bytes = std::max<std::uint64_t>(
        minimizer_device_bytes(
            query_inputs.minimizer_word_kind, query_inputs.minimizer_words32_per_record, total_mc),
        4u);
    if (layout.required_min_words_bytes > features.max_storage_buffer_range) {
        throw vulkan_runtime_error(
            "gpu seed-to-coordinate minimizer words exceed Vulkan descriptor range");
    }
    if (query_inputs.device_minimizer_words != nullptr &&
        (!query_inputs.device_minimizer_words->is_valid() ||
         query_inputs.device_minimizer_words->owner != owner ||
         layout.required_min_words_bytes > query_inputs.device_minimizer_words->bytes)) {
        throw vulkan_runtime_error(
            "gpu seed-to-coordinate device minimizer buffer is invalid or undersized");
    }
    if (grouped_soa_selected && (query_inputs.device_minimizer_words == nullptr ||
                                 !query_inputs.host_minimizer_bytes.empty())) {
        throw vulkan_runtime_error(
            "gpu grouped-SoA direct route requires an unambiguous GPU-resident query sketch");
    }
    return layout;
}

/// @brief Checks whether two jobs may share one resident target submission.
/// @param[in] lhs First job identity.
/// @param[in] rhs Second job identity.
/// @return `true` when both jobs bind the same target buffers and coordinate domain.
[[nodiscard]] inline bool
has_same_coordinate_target(const gpu_seed_coordinate_job& lhs,
                           const gpu_seed_coordinate_job& rhs) noexcept {
    return lhs.grouped_soa == rhs.grouped_soa && lhs.slots == rhs.slots &&
           lhs.positions == rhs.positions && lhs.target_len == rhs.target_len &&
           lhs.slot_mask == rhs.slot_mask && lhs.position_count == rhs.position_count &&
           lhs.bucket_count == rhs.bucket_count &&
           lhs.target_position_offset == rhs.target_position_offset;
}

/// @brief Computes one bounded wave and its arena requirement.
/// @param[in] jobs Complete ordered job list.
/// @param[in] query_records Query records indexed by the jobs.
/// @param[in] wave_begin First job included in this wave.
/// @param[in] coordinate_vote_selected Whether compatible vote jobs may be coalesced.
/// @param[in] grouped_soa_selected Whether each problem needs a status word.
/// @param[in] min_words_bytes Shared query minimizer allocation in bytes.
/// @param[in] query_record_bytes Shared query-record allocation in bytes.
/// @return Wave end, problem count, and complete arena capacity requirement.
[[nodiscard]] inline coordinate_wave_plan prepare_coordinate_wave(
    std::span<const gpu_seed_coordinate_job> jobs,
    std::span<const gpu_seed_query_record_wire> query_records,
    std::size_t wave_begin,
    bool coordinate_vote_selected,
    bool grouped_soa_selected,
    std::uint64_t min_words_bytes,
    std::uint64_t query_record_bytes,
    std::uint32_t maximum_jobs_per_wave = k_gpu_seed_coordinate_batch_wave_size) {
    std::size_t wave_end =
        std::min<std::size_t>(jobs.size(), wave_begin + maximum_jobs_per_wave);
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
    const std::size_t wave_count = wave_end - wave_begin;
    std::uint64_t per_wave_problem_bytes = 0u;
    for (std::size_t job_index = wave_begin; job_index < wave_end; ++job_index) {
        const gpu_seed_query_record_wire& qrec = query_records[jobs[job_index].query_index];
        const std::uint32_t qblocks = std::max<std::uint32_t>(
            bioseq::common::ceil_div(qrec.minimizer_count, k_prefix_scan_block_size), 1u);
        per_wave_problem_bytes = bioseq::common::saturating_add(
            per_wave_problem_bytes,
            bioseq::common::saturating_add(
                bioseq::common::saturating_add(std::uint64_t{qrec.minimizer_count} *
                                                   (grouped_soa_selected ? 16u : 8u),
                                               grouped_soa_selected ? 4u : 0u),
                std::uint64_t{qblocks} * 4u));
    }
    return coordinate_wave_plan{
        .end = wave_end,
        .count = wave_count,
        .arena_bytes = bioseq::common::saturating_add(
            bioseq::common::saturating_add(min_words_bytes, query_record_bytes),
            bioseq::common::saturating_add(
                per_wave_problem_bytes,
                bioseq::common::saturating_mul(static_cast<std::uint64_t>((grouped_soa_selected ? 4u : 3u) * wave_count + 2u),
                                               std::uint64_t{k_gpu_buffer_offset_alignment}))),
    };
}

/// @brief Copies optional host minimizers and dense query records into arena slices.
/// @param[out] base Mapped base address of the arena device buffer.
/// @param[in] query_record_slice Destination slice for strided query records.
/// @param[in] minimizer_slice Destination used only for host minimizer bytes.
/// @param[in] query_inputs Host records and selected minimizer storage.
/// @param[in] query_record_stride Destination stride in bytes.
/// @param[in] required_minimizer_bytes Exact validated descriptor range.
/// @return Descriptor binding for the selected minimizer storage.
[[nodiscard]] inline coordinate_minimizer_binding
prepare_coordinate_query_upload(std::byte* base,
                                const gpu_arena_slice& query_record_slice,
                                const gpu_arena_slice& minimizer_slice,
                                const gpu_seed_query_inputs& query_inputs,
                                std::uint64_t query_record_stride,
                                std::uint64_t required_minimizer_bytes) {
    const coordinate_minimizer_binding binding{
        .buffer = query_inputs.device_minimizer_words != nullptr
                      ? query_inputs.device_minimizer_words->buffer_handle
                      : minimizer_slice.buffer->buffer_handle,
        .offset = query_inputs.device_minimizer_words != nullptr ? 0u : minimizer_slice.offset,
        .range = required_minimizer_bytes,
    };
    if (query_inputs.device_minimizer_words == nullptr) {
        std::memset(base + minimizer_slice.offset,
                    0,
                    static_cast<std::size_t>(minimizer_slice.size));
        if (!query_inputs.host_minimizer_bytes.empty()) {
            std::memcpy(base + minimizer_slice.offset,
                        query_inputs.host_minimizer_bytes.data(),
                        query_inputs.host_minimizer_bytes.size());
        }
    }
    std::memset(base + query_record_slice.offset,
                0,
                static_cast<std::size_t>(query_record_slice.size));
    for (std::size_t index = 0; index < query_inputs.host_query_records.size(); ++index) {
        std::memcpy(base + query_record_slice.offset +
                        static_cast<std::uint64_t>(index) * query_record_stride,
                    &query_inputs.host_query_records[index],
                    sizeof(gpu_seed_query_record_wire));
    }
    return binding;
}

/// @brief Checks whether all active wave states bind the same source-direct target.
/// @param[in] active_states Non-null states retained for this submission.
/// @return `true` when at least two states share the same resident target.
[[nodiscard]] inline bool has_same_active_coordinate_target(
    std::span<gpu_seed_coordinate_wave_state* const> active_states) noexcept {
    if (active_states.size() < 2u) {
        return false;
    }
    const gpu_seed_coordinate_wave_state* first_active = active_states.front();
    for (const gpu_seed_coordinate_wave_state* state : active_states) {
        if (state->job.slots != first_active->job.slots ||
            state->job.positions != first_active->job.positions ||
            state->job.target_len != first_active->job.target_len ||
            state->job.slot_mask != first_active->job.slot_mask ||
            state->job.position_count != first_active->job.position_count ||
            state->job.bucket_count != first_active->job.bucket_count ||
            state->job.target_position_offset != first_active->job.target_position_offset) {
            return false;
        }
    }
    return true;
}

/// @brief Initializes the immutable grouped-SoA audit fields for one job.
/// @param[in,out] state Wave state whose audit record is populated.
/// @param[in] resident_view Authenticated persisted generation.
/// @param[in] query_minimizer_buffer Resident query output buffer.
/// @param[in] operation_configuration Immutable algorithm configuration identity.
/// @param[in] operation_identity Runtime operation sequence identity.
/// @param[in] query_authority_identity Identity carried by the query sketch authority.
/// @param[in] query_tile_count Tiles contributing to this query.
/// @param[in] query_dispatch_stats Causal count, scan, and emit statistics.
inline void initialize_grouped_soa_job_audit(
    gpu_seed_coordinate_wave_state& state,
    const gpu_grouped_soa_resident_view& resident_view,
    const buffer* query_minimizer_buffer,
    const persisted_grouped_soa_coordinate_config_identity& operation_configuration,
    std::uint64_t operation_identity,
    std::uint64_t query_authority_identity,
    std::uint32_t query_tile_count,
    const runtime_sketch_dispatch_stats& query_dispatch_stats) {
    auto& audit = state.operation_audit;
    audit.operation_identity = operation_identity;
    audit.query_authority_identity = query_authority_identity;
    audit.resident_descriptors = make_runtime_buffer_identity(resident_view.descriptors);
    audit.resident_controls = make_runtime_buffer_identity(resident_view.controls);
    audit.resident_positions = make_runtime_buffer_identity(resident_view.positions);
    audit.query_minimizer_buffer = make_runtime_buffer_identity(query_minimizer_buffer);
    audit.device_uuid = resident_view.device_uuid;
    audit.descriptor_bytes = resident_view.descriptor_bytes;
    audit.control_bytes = resident_view.control_bytes;
    audit.position_bytes = resident_view.position_bytes;
    audit.position_count = resident_view.position_count;
    audit.max_occ_cap = resident_view.max_occ_cap;
    audit.partition_count = resident_view.partition_count;
    audit.abi_revision = resident_view.abi_revision;
    audit.generation_id = resident_view.generation_id;
    audit.source_identity = resident_view.source_identity;
    audit.cold_load_identity = resident_view.cold_load_identity;
    audit.target_len = state.job.target_len;
    audit.problem_index = state.result_index;
    audit.query_index = state.job.query_index;
    audit.logical_target_row = state.job.logical_target_row;
    audit.logical_query_ordinal = state.job.logical_query_ordinal;
    audit.query_record = state.query_record;
    audit.configuration = operation_configuration;
    audit.query_tile_count = query_tile_count;
    audit.query_count_dispatches = query_dispatch_stats.count_dispatches();
    audit.query_scan_dispatches = query_dispatch_stats.prefix_scan_dispatches();
    audit.query_emit_dispatches = query_dispatch_stats.emit_dispatches();
    audit.query_completed_roundtrips = query_dispatch_stats.completed_roundtrips();
}

/// @brief Builds both seed push layouts without changing their dispatch values.
/// @param[in,out] state Job state receiving source-direct and grouped-SoA push blocks.
/// @param[in] query_inputs Shared minimizer representation.
/// @param[in] grouped_soa_selected Whether to populate the persisted push block.
/// @param[in] kmer_size K-mer span encoded into anchors.
/// @param[in] max_occ Maximum occurrences emitted per minimizer.
/// @param[in] total_mc Total query minimizers visible to the batch.
/// @param[in] resident_layout Validated grouped-SoA word extents.
inline void prepare_seed_push_constants(gpu_seed_coordinate_wave_state& state,
                                        const gpu_seed_query_inputs& query_inputs,
                                        bool grouped_soa_selected,
                                        std::uint32_t kmer_size,
                                        std::int32_t max_occ,
                                        std::uint32_t total_mc,
                                        const grouped_soa_resident_layout& resident_layout) {
    const std::uint32_t target_offset_lo =
        static_cast<std::uint32_t>(state.job.target_position_offset & 0xffffffffull);
    const std::uint32_t target_offset_hi =
        static_cast<std::uint32_t>(state.job.target_position_offset >> 32);
    const gpu_anchor_sort_key_plan anchor_sort_plan =
        make_anchor_sort_key_plan(state.job.target_len, state.job.target_position_offset);
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
            .abi_revision = k_gpu_grouped_soa_seed_push_abi_revision,
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
            .control_word_count = resident_layout.control_word_count,
            .position_word_count = resident_layout.position_word_count,
            .anchor_capacity = 0u,
            .max_occ_cap_lo = static_cast<std::uint32_t>(view.max_occ_cap & 0xffffffffull),
            .max_occ_cap_hi = static_cast<std::uint32_t>(view.max_occ_cap >> 32u),
            .status_index = state.minimizer_count,
        };
    }
}

} // namespace bioseq::gpu::vulkan::coordinate_pipeline_detail
