/**
 * @file shader_variants.cppm
 * @brief Choose shader parameters and identify compiled shaders in the cache.
 */
module;

export module bioseq.gpu.vulkan.shader_variants;

import std;
import bioseq.gpu.vulkan.shader_compiler;
import bioseq.gpu.vulkan.abi;

export namespace bioseq::gpu::vulkan {

/// @brief Grouped-index layout version stored in shader cache keys.
inline constexpr std::uint32_t k_grouped_soa_seed_cache_abi_revision = 6u;

/// @brief Window capacities included in the precompiled sketch cache.
/// Pick the smallest capacity that holds the requested minimizer window.
/// Keeping this list fixed lets the supervisor and backend agree on cache
/// contents even when PostgreSQL sessions use different window settings.
inline constexpr std::array<std::uint32_t, 6> k_ordered_sketch_window_capacities{
    10u, 16u, 32u, 64u, 128u, 256u};

/// @brief Choose the smallest precompiled ring capacity for a minimizer window.
/// @param[in] window_size Number of k-mers in the runtime window.
/// @return A supported capacity, or zero if the window is empty or too large.
[[nodiscard]] inline constexpr std::uint32_t
choose_ordered_sketch_window_capacity(std::uint32_t window_size) noexcept {
    for (const std::uint32_t capacity : k_ordered_sketch_window_capacities) {
        if (window_size != 0u && window_size <= capacity)
            return capacity;
    }
    return 0u;
}

/// @brief Compile-time tile and workgroup sizes for anchor chaining.
struct chaining_pipeline_key {
    std::uint32_t tile_size = 384;
    std::uint32_t local_size = 128;

    /// @brief Compare the tile and workgroup sizes.
    [[nodiscard]] bool operator==(const chaining_pipeline_key&) const = default;
};

/// @brief Read a shader kind from its stored numeric value.
/// @param[in] raw Value read from a shader key.
/// @return The registered shader kind.
/// @throws shader_compile_error If the value is unknown.
[[nodiscard]] inline shader_kind checked_shader_kind(std::uint32_t raw) {
    switch (raw) {
    case static_cast<std::uint32_t>(shader_kind::chain_tiled_dp_u64):
    case static_cast<std::uint32_t>(shader_kind::seed_lookup_anchors):
    case static_cast<std::uint32_t>(shader_kind::sort_anchors_bitonic):
    case static_cast<std::uint32_t>(shader_kind::ordered_sketch_tile):
    case static_cast<std::uint32_t>(shader_kind::sketch_emit_flags):
    case static_cast<std::uint32_t>(shader_kind::sketch_emit_carry):
    case static_cast<std::uint32_t>(shader_kind::prefix_scan_block):
    case static_cast<std::uint32_t>(shader_kind::prefix_scan_add):
    case static_cast<std::uint32_t>(shader_kind::sketch_emit_words):
    case static_cast<std::uint32_t>(shader_kind::seed_count):
    case static_cast<std::uint32_t>(shader_kind::seed_emit):
    case static_cast<std::uint32_t>(shader_kind::grouped_soa_seed_count):
    case static_cast<std::uint32_t>(shader_kind::grouped_soa_seed_emit):
    case static_cast<std::uint32_t>(shader_kind::radix_sort_histogram):
    case static_cast<std::uint32_t>(shader_kind::radix_sort_scan):
    case static_cast<std::uint32_t>(shader_kind::radix_sort_scatter):
    case static_cast<std::uint32_t>(shader_kind::target_index_mark_runs):
    case static_cast<std::uint32_t>(shader_kind::target_index_emit_buckets):
    case static_cast<std::uint32_t>(shader_kind::target_index_claim_slots):
    case static_cast<std::uint32_t>(shader_kind::target_index_emit_slots):
    case static_cast<std::uint32_t>(shader_kind::chain_range_build):
    case static_cast<std::uint32_t>(shader_kind::coordinate_reduce):
    case static_cast<std::uint32_t>(shader_kind::coordinate_vote_reduce):
    case static_cast<std::uint32_t>(shader_kind::coordinate_vote_reduce_batch):
    case static_cast<std::uint32_t>(shader_kind::generate_units):
    case static_cast<std::uint32_t>(shader_kind::seq_sort_extract_key_dna4):
    case static_cast<std::uint32_t>(shader_kind::seq_sort_extract_key_generic):
    case static_cast<std::uint32_t>(shader_kind::seq_sort_tiebreak_dna4):
    case static_cast<std::uint32_t>(shader_kind::seq_sort_tiebreak_generic):
    case static_cast<std::uint32_t>(shader_kind::seq_sort_global_compare_dna4):
    case static_cast<std::uint32_t>(shader_kind::seq_sort_global_compare_generic):
    case static_cast<std::uint32_t>(shader_kind::seq_sort_dense_scatter_dna4):
    case static_cast<std::uint32_t>(shader_kind::seq_sort_dense_scatter_generic):
    case static_cast<std::uint32_t>(shader_kind::seq_sort_prefix_check_dna4):
    case static_cast<std::uint32_t>(shader_kind::seq_sort_prefix_check_generic):
    case static_cast<std::uint32_t>(shader_kind::align_extend_banded_affine):
    case static_cast<std::uint32_t>(shader_kind::align_extend_simple_cigar):
    case static_cast<std::uint32_t>(shader_kind::sneaky_snake_rank_batch):
    case static_cast<std::uint32_t>(shader_kind::gatekeeper_rank_batch):
    case static_cast<std::uint32_t>(shader_kind::gatekeeper_rank_batch_l32_e2):
    case static_cast<std::uint32_t>(shader_kind::gatekeeper_rank_batch_l32_e4):
    case static_cast<std::uint32_t>(shader_kind::gatekeeper_rank_batch_l64_e2):
    case static_cast<std::uint32_t>(shader_kind::gatekeeper_rank_batch_l64_e4):
    case static_cast<std::uint32_t>(shader_kind::chain_candidate_reduce):
    case static_cast<std::uint32_t>(shader_kind::mapping_quality_reduce):
        return static_cast<shader_kind>(raw);
    default:
        throw shader_compile_error("unknown shader kind " + std::to_string(raw));
    }
}

/// @brief Shader kind and compile-time parameters used as a cache key.
/// Some shaders use fixed workgroup sizes regardless of local_size.
/// Use expected_shader_workgroup_size() to get their compiled dimensions.
struct shader_variant_key {
    std::uint32_t kind = 0;
    std::uint32_t tile_size = 384;
    std::uint32_t local_size = 128;
    std::uint32_t max_span = 0;

    /// @brief Compare all fields of the cache key.
    [[nodiscard]] bool operator==(const shader_variant_key&) const = default;

    /// @brief Turn this cache key into a compiler request.
    /// @return A request with the same parameters. Only the shader kind is checked here.
    /// @throws shader_compile_error If kind is unknown.
    [[nodiscard]] shader_compile_request to_request() const {
        shader_compile_request request{};
        request.kind = checked_shader_kind(kind);
        request.tile_size = tile_size;
        request.local_size = local_size;
        request.max_span = max_span;
        return request;
    }
};
static_assert(sizeof(shader_variant_key) == 16);
static_assert(std::is_standard_layout_v<shader_variant_key>);

/// @brief Device limits needed to choose a shader without a Vulkan device object.
struct vulkan_static_variant_inputs {
    std::uint32_t max_workgroup_invocations = 0;
    std::uint32_t max_shared_memory_bytes = 0;
};

/// @brief Choose the 32-lane shader used for anchor chaining.
/// @param[in] inputs Reserved for capability checks. Currently ignored.
/// @return A tile size and workgroup size of 32.
[[nodiscard]] inline constexpr chaining_pipeline_key
choose_chaining_variant(const vulkan_static_variant_inputs& inputs) noexcept {
    // Exactly one 32-lane subgroup owns each range. A larger local size would
    // place multiple subgroups in one workgroup and make them race on the range.
    (void)inputs;
    return chaining_pipeline_key{32u, 32u};
}

/// @brief Find the largest bitonic anchor-sort capacity allowed by the device.
/// @param[in] inputs Workgroup-size and shared-memory limits.
/// @return A power of two up to 1024, or zero if even one lane will not fit.
[[nodiscard]] inline constexpr std::uint32_t
anchor_sort_capacity(const vulkan_static_variant_inputs& inputs) noexcept {
    constexpr std::uint32_t per_lane_shared_bytes = 28u;
    std::uint32_t capacity = 0;
    for (std::uint32_t candidate = 1; candidate <= 1024u; candidate <<= 1u) {
        if (candidate > inputs.max_workgroup_invocations)
            break;
        if (static_cast<std::uint64_t>(candidate) * per_lane_shared_bytes >
            inputs.max_shared_memory_bytes) {
            break;
        }
        capacity = candidate;
    }
    return capacity;
}

/// @brief Build the cache key for a shader compilation.
/// @param[in] kind Shader to compile.
/// @param[in] tile_size Compile-time tile parameter.
/// @param[in] local_size Compile-time workgroup parameter.
/// @param[in] max_span Extra bound or layout version used by the shader.
/// @return The key with these values.
[[nodiscard]] inline constexpr shader_variant_key make_shader_key(shader_kind kind,
                                                                  std::uint32_t tile_size,
                                                                  std::uint32_t local_size,
                                                                  std::uint32_t max_span) noexcept {
    return shader_variant_key{static_cast<std::uint32_t>(kind), tile_size, local_size, max_span};
}

/// @brief Get the workgroup dimensions a compiled shader will use.
/// @param[in] key Shader cache key.
/// @return The shader's x, y and z workgroup sizes.
[[nodiscard]] inline shader_workgroup_size
expected_shader_workgroup_size(const shader_variant_key& key) {
    return expected_shader_workgroup_size(key.to_request());
}

/// @brief Check whether a key selects the supported anchor-chaining shader.
/// @param[in] key Shader cache key.
/// @return true for the chaining kind with its required parameters.
[[nodiscard]] inline constexpr bool is_chain_key(const shader_variant_key& key) noexcept {
    if (key.kind != static_cast<std::uint32_t>(shader_kind::chain_tiled_dp_u64)) {
        return false;
    }
    return key.tile_size == 32u && key.local_size == 32u && key.max_span == 0u;
}

/// @brief Check whether a shader key uses a supported set of parameters.
/// @param[in] key Shader cache key to check.
/// @return true if the kind and all parameters are accepted.
[[nodiscard]] inline constexpr bool
is_shader_variant_key_valid(const shader_variant_key& key) noexcept {
    if (key.tile_size == 0 || key.tile_size > 1024 || key.local_size == 0) {
        return false;
    }
    if (is_chain_key(key))
        return true;
    const auto kind = static_cast<shader_kind>(key.kind);
    switch (kind) {
    case shader_kind::seed_lookup_anchors:
        return key.tile_size == 384u && key.max_span == 256u && key.local_size == 128u;
    case shader_kind::sort_anchors_bitonic:
        return key.tile_size == 384u && key.max_span == 256u &&
               (key.local_size == 128u || key.local_size == 256u || key.local_size == 512u ||
                key.local_size == 1024u);
    case shader_kind::ordered_sketch_tile:
        return key.tile_size == 384u && key.local_size == 64u && key.max_span >= 1u &&
               key.max_span <= 256u;
    case shader_kind::prefix_scan_block:
    case shader_kind::radix_sort_histogram:
    case shader_kind::radix_sort_scan:
    case shader_kind::radix_sort_scatter:
        return key.tile_size == 384u && key.local_size == 1u && key.max_span == 1u;
    case shader_kind::prefix_scan_add:
    case shader_kind::seed_count:
    case shader_kind::seed_emit:
    case shader_kind::target_index_mark_runs:
    case shader_kind::target_index_emit_buckets:
        return key.tile_size == 384u && key.local_size == 128u && key.max_span == 1u;
    case shader_kind::grouped_soa_seed_count:
    case shader_kind::grouped_soa_seed_emit:
        // For grouped indexes, max_span stores the shader layout version.
        return key.tile_size == 384u && key.local_size == 128u &&
               key.max_span == k_grouped_soa_seed_cache_abi_revision;
    case shader_kind::chain_range_build:
        return key.tile_size == 384u && key.local_size == 1u && key.max_span == 1u;
    case shader_kind::coordinate_reduce:
    case shader_kind::coordinate_vote_reduce:
    case shader_kind::coordinate_vote_reduce_batch:
        return key.tile_size == 384u && key.local_size == 128u && key.max_span == 1u;
    case shader_kind::chain_candidate_reduce:
        return key.tile_size == 384u && key.local_size == 128u &&
               key.max_span == k_gpu_persisted_mapq_candidate_capacity;
    case shader_kind::mapping_quality_reduce:
        return key.tile_size == 384u && key.local_size == 1u &&
               key.max_span == k_gpu_persisted_mapq_candidate_capacity;
    case shader_kind::generate_units:
        return key.tile_size == 384u && key.local_size == 256u &&
               (key.max_span == 32u || key.max_span == 64u);
    case shader_kind::seq_sort_extract_key_dna4:
    case shader_kind::seq_sort_extract_key_generic:
    case shader_kind::seq_sort_tiebreak_dna4:
    case shader_kind::seq_sort_tiebreak_generic:
    case shader_kind::seq_sort_global_compare_dna4:
    case shader_kind::seq_sort_global_compare_generic:
    case shader_kind::seq_sort_dense_scatter_dna4:
    case shader_kind::seq_sort_dense_scatter_generic:
    case shader_kind::seq_sort_prefix_check_dna4:
    case shader_kind::seq_sort_prefix_check_generic:
        return key.tile_size == 384u && key.local_size == 256u && key.max_span == 0u;
    case shader_kind::align_extend_banded_affine:
        return key.tile_size == 384u && key.local_size == 1u && key.max_span == 512u;
    case shader_kind::align_extend_simple_cigar:
        return key.tile_size == 384u && (key.local_size == 1u || key.local_size == 64u) &&
               key.max_span == 512u;
    case shader_kind::sneaky_snake_rank_batch:
        return key.tile_size == 384u && key.local_size == 64u && key.max_span == 512u;
    case shader_kind::gatekeeper_rank_batch:
    case shader_kind::gatekeeper_rank_batch_l32_e2:
    case shader_kind::gatekeeper_rank_batch_l32_e4:
    case shader_kind::gatekeeper_rank_batch_l64_e2:
    case shader_kind::gatekeeper_rank_batch_l64_e4:
        return key.tile_size == 384u && key.local_size == 64u && key.max_span == 128u;
    default:
        return false;
    }
}

/// @brief Make the filename stem used for a cached shader.
/// @param[in] key Shader cache key.
/// @return The shader kind and parameters in hexadecimal, prefixed with sh_.
[[nodiscard]] inline std::string cache_file_stem(const shader_variant_key& key) {
    char buffer[32]{};
    std::snprintf(buffer,
                  sizeof(buffer),
                  "sh_%02x_%04x_%04x_%04x",
                  key.kind,
                  key.tile_size,
                  key.local_size,
                  key.max_span);
    return std::string(buffer);
}

/// @brief Visit the fixed list of precompiled shader variants.
/// @tparam p_visitor Callable accepting a shader_variant_key.
/// @param[in] inputs Reserved for device limits. Currently ignored.
/// @param[in] visitor Called once for each supported key in the list.
template <class p_visitor>
inline void for_each_static_shader_variant(const vulkan_static_variant_inputs& inputs,
                                           p_visitor&& visitor) {
    (void)inputs;
    // Pass only supported keys to the visitor.
    const auto emit = [&visitor](shader_variant_key key) {
        if (is_shader_variant_key_valid(key)) {
            visitor(key);
        }
    };

    emit(make_shader_key(shader_kind::chain_tiled_dp_u64, 32u, 32u, 0u));
    emit(make_shader_key(shader_kind::seed_lookup_anchors, 384u, 128u, 256u));
    emit(make_shader_key(shader_kind::sort_anchors_bitonic, 384u, 128u, 256u));
    emit(make_shader_key(shader_kind::sort_anchors_bitonic, 384u, 256u, 256u));
    emit(make_shader_key(shader_kind::sort_anchors_bitonic, 384u, 512u, 256u));
    emit(make_shader_key(shader_kind::sort_anchors_bitonic, 384u, 1024u, 256u));
    for (const std::uint32_t capacity : k_ordered_sketch_window_capacities) {
        emit(make_shader_key(shader_kind::ordered_sketch_tile, 384u, 64u, capacity));
    }
    emit(make_shader_key(shader_kind::prefix_scan_block, 384u, 1u, 1u));
    emit(make_shader_key(shader_kind::prefix_scan_add, 384u, 128u, 1u));
    emit(make_shader_key(shader_kind::seed_count, 384u, 128u, 1u));
    emit(make_shader_key(shader_kind::seed_emit, 384u, 128u, 1u));
    emit(make_shader_key(
        shader_kind::grouped_soa_seed_count, 384u, 128u, k_grouped_soa_seed_cache_abi_revision));
    emit(make_shader_key(
        shader_kind::grouped_soa_seed_emit, 384u, 128u, k_grouped_soa_seed_cache_abi_revision));
    emit(make_shader_key(shader_kind::radix_sort_histogram, 384u, 1u, 1u));
    emit(make_shader_key(shader_kind::radix_sort_scan, 384u, 1u, 1u));
    emit(make_shader_key(shader_kind::radix_sort_scatter, 384u, 1u, 1u));
    emit(make_shader_key(shader_kind::target_index_mark_runs, 384u, 128u, 1u));
    emit(make_shader_key(shader_kind::target_index_emit_buckets, 384u, 128u, 1u));
    emit(make_shader_key(shader_kind::chain_range_build, 384u, 1u, 1u));
    emit(make_shader_key(shader_kind::coordinate_reduce, 384u, 128u, 1u));
    emit(make_shader_key(shader_kind::coordinate_vote_reduce, 384u, 128u, 1u));
    emit(make_shader_key(shader_kind::coordinate_vote_reduce_batch, 384u, 128u, 1u));
    emit(make_shader_key(
        shader_kind::chain_candidate_reduce, 384u, 128u, k_gpu_persisted_mapq_candidate_capacity));
    emit(make_shader_key(
        shader_kind::mapping_quality_reduce, 384u, 1u, k_gpu_persisted_mapq_candidate_capacity));
    emit(make_shader_key(shader_kind::generate_units, 384u, 256u, 32u));
    emit(make_shader_key(shader_kind::generate_units, 384u, 256u, 64u));
    emit(make_shader_key(shader_kind::seq_sort_extract_key_dna4, 384u, 256u, 0u));
    emit(make_shader_key(shader_kind::seq_sort_extract_key_generic, 384u, 256u, 0u));
    emit(make_shader_key(shader_kind::seq_sort_tiebreak_dna4, 384u, 256u, 0u));
    emit(make_shader_key(shader_kind::seq_sort_tiebreak_generic, 384u, 256u, 0u));
    emit(make_shader_key(shader_kind::seq_sort_global_compare_dna4, 384u, 256u, 0u));
    emit(make_shader_key(shader_kind::seq_sort_global_compare_generic, 384u, 256u, 0u));
    emit(make_shader_key(shader_kind::seq_sort_dense_scatter_dna4, 384u, 256u, 0u));
    emit(make_shader_key(shader_kind::seq_sort_dense_scatter_generic, 384u, 256u, 0u));
    emit(make_shader_key(shader_kind::seq_sort_prefix_check_dna4, 384u, 256u, 0u));
    emit(make_shader_key(shader_kind::seq_sort_prefix_check_generic, 384u, 256u, 0u));
    emit(make_shader_key(shader_kind::align_extend_banded_affine, 384u, 1u, 512u));
    emit(make_shader_key(shader_kind::align_extend_simple_cigar, 384u, 1u, 512u));
    emit(make_shader_key(shader_kind::align_extend_simple_cigar, 384u, 64u, 512u));
    emit(make_shader_key(shader_kind::sneaky_snake_rank_batch, 384u, 64u, 512u));
    emit(make_shader_key(shader_kind::gatekeeper_rank_batch, 384u, 64u, 128u));
    emit(make_shader_key(shader_kind::gatekeeper_rank_batch_l32_e2, 384u, 64u, 128u));
    emit(make_shader_key(shader_kind::gatekeeper_rank_batch_l32_e4, 384u, 64u, 128u));
    emit(make_shader_key(shader_kind::gatekeeper_rank_batch_l64_e2, 384u, 64u, 128u));
    emit(make_shader_key(shader_kind::gatekeeper_rank_batch_l64_e4, 384u, 64u, 128u));
}

} // namespace bioseq::gpu::vulkan
