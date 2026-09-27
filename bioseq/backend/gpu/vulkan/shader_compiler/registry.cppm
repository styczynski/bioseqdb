/**
 * @file registry.cppm
 * @brief List Vulkan shaders and the buffer layouts and parameters they require.
 */
export module bioseq.gpu.vulkan.shader_compiler:registry;

import std;

export namespace bioseq::gpu::vulkan {

/// @brief Shader identifiers used by the Vulkan runtime and saved shader cache.
///
/// Their numeric values are stored in cache keys. Changing a value requires
/// updating the cache readers and rebuilding the affected shader files.
enum class shader_kind : std::uint8_t {
    chain_tiled_dp_u64 = 0,
    seed_lookup_anchors = 1,
    sort_anchors_bitonic = 2,
    // Ordered-hketch stages share a shader selected by emit_mode.
    // Prefix scans, sorting, index building, seeding and chain ranges have
    // their own shader bodies.
    ordered_sketch_tile = 3,
    sketch_emit_flags = 4,
    sketch_emit_carry = 5,
    prefix_scan_block = 6,
    prefix_scan_add = 7,
    sketch_emit_words = 8,
    seed_count = 9,
    seed_emit = 10,
    radix_sort_histogram = 11,
    radix_sort_scan = 12,
    radix_sort_scatter = 13,
    target_index_mark_runs = 14,
    target_index_emit_buckets = 15,
    target_index_claim_slots = 16,
    target_index_emit_slots = 17,
    chain_range_build = 18,
    coordinate_reduce = 19,
    coordinate_vote_reduce = 20,
    coordinate_vote_reduce_batch = 21,
    generate_units = 22,
    // DNA4 and generic sequence sorting compile to separate shaders.
    // The packed storage width  is passed at runtime in pc.unit_bits.
    seq_sort_extract_key_dna4 = 23,
    seq_sort_extract_key_generic = 24,
    seq_sort_tiebreak_dna4 = 25,
    seq_sort_tiebreak_generic = 26,
    seq_sort_global_compare_dna4 = 27,
    seq_sort_global_compare_generic = 28,
    seq_sort_dense_scatter_dna4 = 29,
    seq_sort_dense_scatter_generic = 30,
    seq_sort_prefix_check_dna4 = 31,
    seq_sort_prefix_check_generic = 32,
    align_extend_banded_affine = 33,
    align_extend_simple_cigar = 34,
    // Read grouped index controls and positions directly on the GPU.
    // Each array can use its own integer width, and positions are relative
    // to their sequence segment.
    grouped_soa_seed_count = 51,
    grouped_soa_seed_emit = 52,
    // SneakySnake filtering over unpacked symbol ranks.
    sneaky_snake_rank_batch = 53,
    // GateKeeper filtering over unpacked, symbol ranks.
    gatekeeper_rank_batch = 54,
    // Specialize GateKeeper for fixed sequence lengths and error limits
    // to reduce the registers needed by small, same-sized jobs.
    gatekeeper_rank_batch_l32_e2 = 55,
    gatekeeper_rank_batch_l32_e4 = 56,
    gatekeeper_rank_batch_l64_e2 = 57,
    gatekeeper_rank_batch_l64_e4 = 58,
    // Keep new MAPQ shader identifiers at the end to preserve cache keys.
    chain_candidate_reduce = 59,
    mapping_quality_reduce = 60,
};

/// @brief Return whether a shader kind belongs to the GateKeeper rank-batch family.
[[nodiscard]] inline constexpr bool is_gatekeeper_rank_batch_kind(shader_kind kind) noexcept {
    switch (kind) {
    case shader_kind::gatekeeper_rank_batch:
    case shader_kind::gatekeeper_rank_batch_l32_e2:
    case shader_kind::gatekeeper_rank_batch_l32_e4:
    case shader_kind::gatekeeper_rank_batch_l64_e2:
    case shader_kind::gatekeeper_rank_batch_l64_e4:
        return true;
    default:
        return false;
    }
}

/// @brief Buffer layouts that several shaders can share
enum class descriptor_layout_kind : std::uint8_t {
    chain_tiled_dp = 0,
    seed_lookup = 1,
    anchor_sort = 2,
    // Kernels that bind the same buffers share one cached layout.
    sketch = 3,
    prefix_scan = 4,
    seed = 5,
    radix_sort = 6,
    target_index = 7,
    chain_range = 8,
    coordinate_reduce = 9,
    generate_units = 10,
    seq_sort = 11,        // { sequence words, descriptors, keys/indices, values/ranges }
    align_extension = 12, // { QuerySymbols, TargetSymbols, Jobs, Results }
    align_extension_cigar = 13,
    // Ten buffers for reading  grouped indexes and emitting anchors.
    grouped_soa_seed = 18,
    // Chain extraction reads chaining buffers and writes candidate records.
    chain_candidate_reduce = 19,
    // MAPQ computation reads alignment candidates and results and writes quality scores.
    mapping_quality_reduce = 20,
};

/// @brief Number of std430 storage-buffer bindings in each descriptor layout.
[[nodiscard]] constexpr std::uint32_t
descriptor_binding_count(descriptor_layout_kind kind) noexcept {
    switch (kind) {
    case descriptor_layout_kind::chain_tiled_dp:
        return 4;
    case descriptor_layout_kind::seed_lookup:
        return 6;
    case descriptor_layout_kind::anchor_sort:
        return 2;
    // Layouts for sketching, index construction and the following GPU stages.
    case descriptor_layout_kind::sketch:
        return 9;
    case descriptor_layout_kind::prefix_scan:
        return 3;
    case descriptor_layout_kind::seed:
        return 8;
    case descriptor_layout_kind::radix_sort:
        return 5;
    case descriptor_layout_kind::target_index:
        return 7;
    case descriptor_layout_kind::chain_range:
        return 6;
    case descriptor_layout_kind::coordinate_reduce:
        return 6;
    case descriptor_layout_kind::generate_units:
        return 2;
    case descriptor_layout_kind::seq_sort:
        return 4;
    case descriptor_layout_kind::align_extension:
        return 4;
    case descriptor_layout_kind::align_extension_cigar:
        return 6;
    case descriptor_layout_kind::grouped_soa_seed:
        return 10;
    case descriptor_layout_kind::chain_candidate_reduce:
        return 10;
    case descriptor_layout_kind::mapping_quality_reduce:
        return 5;
    }
    return 0;
}

/// @brief Map a shader kind to the descriptor layout it binds.
[[nodiscard]] constexpr descriptor_layout_kind descriptor_layout_for(shader_kind kind) noexcept {
    switch (kind) {
    case shader_kind::chain_tiled_dp_u64:
        return descriptor_layout_kind::chain_tiled_dp;
    case shader_kind::seed_lookup_anchors:
        return descriptor_layout_kind::seed_lookup;
    case shader_kind::sort_anchors_bitonic:
        return descriptor_layout_kind::anchor_sort;
    case shader_kind::ordered_sketch_tile:
    case shader_kind::sketch_emit_flags:
    case shader_kind::sketch_emit_carry:
    case shader_kind::sketch_emit_words:
        return descriptor_layout_kind::sketch;
    case shader_kind::prefix_scan_block:
    case shader_kind::prefix_scan_add:
        return descriptor_layout_kind::prefix_scan;
    case shader_kind::seed_count:
    case shader_kind::seed_emit:
        return descriptor_layout_kind::seed;
    case shader_kind::grouped_soa_seed_count:
    case shader_kind::grouped_soa_seed_emit:
        return descriptor_layout_kind::grouped_soa_seed;
    case shader_kind::radix_sort_histogram:
    case shader_kind::radix_sort_scan:
    case shader_kind::radix_sort_scatter:
        return descriptor_layout_kind::radix_sort;
    case shader_kind::target_index_mark_runs:
    case shader_kind::target_index_emit_buckets:
    case shader_kind::target_index_claim_slots:
    case shader_kind::target_index_emit_slots:
        return descriptor_layout_kind::target_index;
    case shader_kind::chain_range_build:
        return descriptor_layout_kind::chain_range;
    case shader_kind::coordinate_reduce:
    case shader_kind::coordinate_vote_reduce:
    case shader_kind::coordinate_vote_reduce_batch:
        return descriptor_layout_kind::coordinate_reduce;
    case shader_kind::generate_units:
        return descriptor_layout_kind::generate_units;
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
        return descriptor_layout_kind::seq_sort;
    case shader_kind::align_extend_banded_affine:
        return descriptor_layout_kind::align_extension;
    case shader_kind::align_extend_simple_cigar:
        return descriptor_layout_kind::align_extension_cigar;
    case shader_kind::chain_candidate_reduce:
        return descriptor_layout_kind::chain_candidate_reduce;
    case shader_kind::mapping_quality_reduce:
        return descriptor_layout_kind::mapping_quality_reduce;
    case shader_kind::sneaky_snake_rank_batch:
    case shader_kind::gatekeeper_rank_batch:
    case shader_kind::gatekeeper_rank_batch_l32_e2:
    case shader_kind::gatekeeper_rank_batch_l32_e4:
    case shader_kind::gatekeeper_rank_batch_l64_e2:
    case shader_kind::gatekeeper_rank_batch_l64_e4:
        return descriptor_layout_kind::align_extension;
    }
    return descriptor_layout_kind::chain_tiled_dp;
}

/// @brief Return the byte size of a shader's push parameters.
/// Anchor sorting reads its ange from a buffer and needs no push parameters.
[[nodiscard]] constexpr std::uint32_t push_constant_size(shader_kind kind) noexcept {
    switch (kind) {
    case shader_kind::chain_tiled_dp_u64:
        return 36; // Chaining parameters.
    case shader_kind::seed_lookup_anchors:
        return 48; // 12 x uint32, including shared query-record base
    case shader_kind::sort_anchors_bitonic:
        return 0;
    // These sizes must match the C++ structures passed by the runtime.
    case shader_kind::ordered_sketch_tile:
    case shader_kind::sketch_emit_flags:
    case shader_kind::sketch_emit_carry:
    case shader_kind::sketch_emit_words:
        return 72; // Ordered-sketch parameters.
    case shader_kind::prefix_scan_block:
    case shader_kind::prefix_scan_add:
        return 16; // Prefix-scan parameters.
    case shader_kind::seed_count:
    case shader_kind::seed_emit:
        return 52; // Seed count and emission parameters.
    case shader_kind::grouped_soa_seed_count:
    case shader_kind::grouped_soa_seed_emit:
        return 124; // gpu_grouped_soa_seed_push
    case shader_kind::radix_sort_histogram:
    case shader_kind::radix_sort_scan:
    case shader_kind::radix_sort_scatter:
        return 20; // Radix-sort parameters.
    case shader_kind::target_index_mark_runs:
    case shader_kind::target_index_emit_buckets:
    case shader_kind::target_index_claim_slots:
    case shader_kind::target_index_emit_slots:
        return 24; // Target-index  construction parameters.
    case shader_kind::chain_range_build:
        return 8; // Chain-range parameters.
    case shader_kind::coordinate_reduce:
    case shader_kind::coordinate_vote_reduce:
    case shader_kind::coordinate_vote_reduce_batch:
        return 28; // Coordinate reduction parameters.
    case shader_kind::generate_units:
        return 48; // Packed-sequence generation parameters.
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
        return 40; // sequence_sort_push_constants (10 × u32)
    case shader_kind::align_extend_banded_affine:
        return 44; // alignment_extension_push_constants
    case shader_kind::align_extend_simple_cigar:
        return 92; // alignment_cigar_push_constants
    case shader_kind::chain_candidate_reduce:
        return 80; // gpu_chain_candidate_reduce_push
    case shader_kind::mapping_quality_reduce:
        return 60; // gpu_mapq_reduce_push
    case shader_kind::sneaky_snake_rank_batch:
    case shader_kind::gatekeeper_rank_batch:
    case shader_kind::gatekeeper_rank_batch_l32_e2:
    case shader_kind::gatekeeper_rank_batch_l32_e4:
    case shader_kind::gatekeeper_rank_batch_l64_e2:
    case shader_kind::gatekeeper_rank_batch_l64_e4:
        return 16; // Rank-prefilter parameters.
    }
    return 0;
}

/// @brief Parameters for compiling a shader and identifying its cached pipeline.
/// tile_size, local_size and max_span become GLSL definitions. Some shaders use
/// fixed workgroup dimensions, which expected_shader_workgroup_size() reports.
/// The remaining fields describe the packed input format in the cache key.
struct shader_compile_request {
    shader_kind kind;
    std::uint32_t tile_size = 384; // Compile-time tile width.
    std::uint32_t local_size = 128;
    std::uint32_t max_span = 256; // shader-specific compile-time span
    std::uint32_t word_kind = 0;
    std::uint32_t alphabet_kind = 0;
    std::uint32_t unit_bits = 0;
    std::uint32_t bits_per_char = 0;
};

/// @brief Workgroup dimensions encoded in a compiled SPIR-V module.
struct shader_workgroup_size {
    std::uint32_t x = 1;
    std::uint32_t y = 1;
    std::uint32_t z = 1;

    /// @brief Compares all encoded workgroup dimensions.
    [[nodiscard]] bool operator==(const shader_workgroup_size&) const = default;
};

/// @brief Return the workgroup dimensions that the shader will use.
/// Radix shaders use 256 lanes even though their cache key has local_size = 1.
/// Chain-range construction and banded alignment use one lane. Cache validation
/// must compare against these dimensions when checking a compiled shader.
[[nodiscard]] inline constexpr shader_workgroup_size
expected_shader_workgroup_size(const shader_compile_request& request) noexcept {
    switch (request.kind) {
    case shader_kind::radix_sort_histogram:
    case shader_kind::radix_sort_scan:
    case shader_kind::radix_sort_scatter:
        return shader_workgroup_size{256u, 1u, 1u};
    case shader_kind::chain_range_build:
    case shader_kind::align_extend_banded_affine:
        return shader_workgroup_size{1u, 1u, 1u};
    default:
        return shader_workgroup_size{request.local_size, 1u, 1u};
    }
}

/// @brief Report invalid shader parameters or failed GLSL compilation.
/// The Vulkan runtime converts this  exception to vulkan_runtime_error.
class shader_compile_error final : public std::runtime_error {
  public:
    /// @brief Construct the error from its, diagnostic message.
    using std::runtime_error::runtime_error;
};

/**
 * @brief Compile an embedded compute shader to SPIR-V.
 * @param[in] request Shader kind and compile-time parameters.
 * @return Compiled SPIR-V as 32-bit words.
 * @throws shader_compile_error If the parameters or shaser cannot be compiled.
 */
[[nodiscard]] std::vector<std::uint32_t>
compile_shader_spirv(const shader_compile_request& request);

/**
 * @brief Return a shader's GLSL body so source changes invalidate its cache
 * @param[in] kind Shader to look up.
 * @return Embedded GLSL without the compiler's added definitions and directives.
 */
[[nodiscard]] std::string shader_body_for_digest(shader_kind kind);

} // namespace bioseq::gpu::vulkan
