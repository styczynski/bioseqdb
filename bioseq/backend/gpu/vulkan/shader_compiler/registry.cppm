/**
 * @file registry.cppm
 * @brief Defines the public Vulkan shader registry and compilation contract.
 */
export module bioseq.gpu.vulkan.shader_compiler:registry;

import std;

export namespace bioseq::gpu::vulkan {

/// @brief Compute shader families registered with the runtime and persistent cache.
///
/// Enumerator ordinals are serialized cache identity and must move atomically
/// with every manifest reader, runtime consumer, and compiled artifact.
enum class shader_kind : std::uint8_t {
    chain_tiled_dp_u64 = 0,
    seed_lookup_anchors = 1,
    sort_anchors_bitonic = 2,
    // The resident frontend shares one ordered-sketch body driven by the
    // `emit_mode` push field. Scan, sort, index, seed, and chain-range families
    // use dedicated bodies.
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
    // Sequence-sort families have distinct dna4 and generic SPIR-V identities;
    // storage width remains a runtime push field (`pc.unit_bits`).
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
    // Direct persisted grouped-SoA seed lookup consumes variable-width control
    // and segment-local position streams without host-side control decoding.
    grouped_soa_seed_count = 51,
    grouped_soa_seed_emit = 52,
    // Rank-buffer sneaky_snake_buffer prefilter.
    sneaky_snake_rank_batch = 53,
    // Rank-buffer gatekeeper_buffer threshold prefilter.
    gatekeeper_rank_batch = 54,
    // Compile-time production geometries reduce the generic kernel's fixed-DP
    // and cross-geometry register footprint for homogeneous small-seed runs.
    gatekeeper_rank_batch_l32_e2 = 55,
    gatekeeper_rank_batch_l32_e4 = 56,
    gatekeeper_rank_batch_l64_e2 = 57,
    gatekeeper_rank_batch_l64_e4 = 58,
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

/// @brief Descriptor-set layouts shared by shader families with one binding contract.
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
    // Nine-buffer direct grouped-SoA seed layout.
    grouped_soa_seed = 18,
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
    // Resident frontend layouts.
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
        return 9;
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

/// @brief Push-constant block size (bytes) each shader's `Params` uniform declares.
/// The anchor sort takes its range from a buffer and has no push constant.
[[nodiscard]] constexpr std::uint32_t push_constant_size(shader_kind kind) noexcept {
    switch (kind) {
    case shader_kind::chain_tiled_dp_u64:
        return 36; // ChainingPushConstants
    case shader_kind::seed_lookup_anchors:
        return 44; // 11 x uint32
    case shader_kind::sort_anchors_bitonic:
        return 0;
    // Per-family push blocks are mirrored by runtime wire structures.
    case shader_kind::ordered_sketch_tile:
    case shader_kind::sketch_emit_flags:
    case shader_kind::sketch_emit_carry:
    case shader_kind::sketch_emit_words:
        return 72; // SketchPushC (18 × u32)
    case shader_kind::prefix_scan_block:
    case shader_kind::prefix_scan_add:
        return 16; // ScanPushC (4 × u32)
    case shader_kind::seed_count:
    case shader_kind::seed_emit:
        return 52; // SeedPushC (13 × u32)
    case shader_kind::grouped_soa_seed_count:
    case shader_kind::grouped_soa_seed_emit:
        return 72; // grouped-SoA seed push block
    case shader_kind::radix_sort_histogram:
    case shader_kind::radix_sort_scan:
    case shader_kind::radix_sort_scatter:
        return 20; // SortPushC (5 × u32)
    case shader_kind::target_index_mark_runs:
    case shader_kind::target_index_emit_buckets:
    case shader_kind::target_index_claim_slots:
    case shader_kind::target_index_emit_slots:
        return 24; // IndexPushC (6 × u32)
    case shader_kind::chain_range_build:
        return 8; // ChainRangePushC (2 × u32)
    case shader_kind::coordinate_reduce:
    case shader_kind::coordinate_vote_reduce:
    case shader_kind::coordinate_vote_reduce_batch:
        return 28; // GpuCoordinateReducePush
    case shader_kind::generate_units:
        return 48; // GenerateUnitsPush (12 × u32)
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
        return 60; // alignment_cigar_push_constants
    case shader_kind::sneaky_snake_rank_batch:
    case shader_kind::gatekeeper_rank_batch:
    case shader_kind::gatekeeper_rank_batch_l32_e2:
    case shader_kind::gatekeeper_rank_batch_l32_e4:
    case shader_kind::gatekeeper_rank_batch_l64_e2:
    case shader_kind::gatekeeper_rank_batch_l64_e4:
        return 16; // GpuPrefilterPush
    }
    return 0;
}

/// @brief One shader-compilation request. `tile_size`, `local_size`, and `max_span`
/// are substituted as compile-time `#define`s before glslang runs. A shader may
/// intentionally bake a fixed workgroup geometry instead of consuming
/// `compile_time_workgroup_size_x`. `expected_shader_workgroup_size()` is the authoritative
/// compile/cache/runtime ABI for that distinction. The remaining fields
/// describe runtime push-constant routing and are carried for the pipeline
/// cache key.
struct shader_compile_request {
    shader_kind kind;
    std::uint32_t tile_size = 384; // chain only
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

/// @brief Resolve compile-time execution geometry encoded in SPIR-V. Most kernels consume the
/// request's compile_time_workgroup_size_x macro. The radix kernels deliberately keep their
/// established cache-key sentinel (`local_size == 1`) while their algorithm
/// requires one lane per radix bin. Keep this mapping explicit: treating a key
/// selector as reflected execution metadata rejects valid AOT artifacts and
/// makes archive publication/completeness disagree.
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

/// @brief Thrown by `compile_shader_spirv` on a rejected request or a glslang
/// parse/link failure. The Vulkan runtime catches this and re-wraps it as a
/// `vulkan_runtime_error`. Selected Vulkan algorithms propagate that error.
class shader_compile_error final : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

/**
 * @brief Compiles a registered compute shader to SPIR-V words.
 * @param[in] request Shader family and compile-time geometry selectors.
 * @return Compiled SPIR-V module as 32-bit words.
 * @throws shader_compile_error When geometry is invalid or glslang rejects the shader.
 */
[[nodiscard]] std::vector<std::uint32_t>
compile_shader_spirv(const shader_compile_request& request);

/**
 * @brief Returns the embedded GLSL body used by cache identity computation.
 * @param[in] kind Registered shader family.
 * @return Exact embedded GLSL body without the compiler prelude.
 */
[[nodiscard]] std::string shader_body_for_digest(shader_kind kind);

} // namespace bioseq::gpu::vulkan
