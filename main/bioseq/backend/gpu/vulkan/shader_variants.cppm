/**
 * @file shader_variants.cppm
 * @brief Device-free Vulkan shader variant keys and selector helpers.
 */
module;
#include "common.h"

#include <cstdio>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>

export module bioseq.gpu.vulkan.shader_variants;

import bioseq.gpu.vulkan.shader_compiler;

export namespace bioseq::gpu::vulkan {

/// Pipeline selector result for the tiled-DP chaining shader.
struct ChainingPipelineKey {
    std::uint32_t tile_size = 384;
    std::uint32_t local_size = 128;

    [[nodiscard]] bool operator==(const ChainingPipelineKey&) const = default;
};

[[nodiscard]] ShaderKind checked_shader_kind(std::uint32_t raw);

/// Shared shader-cache ABI key. Only these fields affect emitted SPIR-V.
struct ShaderVariantKey {
    std::uint32_t kind = 0;
    std::uint32_t tile_size = 384;
    std::uint32_t local_size = 128;
    std::uint32_t max_span = 0;

    [[nodiscard]] bool operator==(const ShaderVariantKey&) const = default;

    [[nodiscard]] ShaderCompileRequest to_request() const {
        ShaderCompileRequest request{};
        request.kind = checked_shader_kind(kind);
        request.tile_size = tile_size;
        request.local_size = local_size;
        request.max_span = max_span;
        return request;
    }
};
static_assert(sizeof(ShaderVariantKey) == 16);
static_assert(std::is_standard_layout_v<ShaderVariantKey>);

/// POD inputs for selectors that must run without a Vulkan device object.
struct VulkanStaticVariantInputs {
    std::uint32_t max_workgroup_invocations = 0;
    std::uint32_t max_shared_memory_bytes = 0;
    std::uint32_t default_window_size = 10;
};

[[nodiscard]] inline ShaderKind checked_shader_kind(std::uint32_t raw)
{
    switch (raw) {
        case static_cast<std::uint32_t>(ShaderKind::chain_tiled_dp_u64):
        case static_cast<std::uint32_t>(ShaderKind::seed_lookup_anchors):
        case static_cast<std::uint32_t>(ShaderKind::sort_anchors_bitonic):
        case static_cast<std::uint32_t>(ShaderKind::phase_c_sketch_tile):
        case static_cast<std::uint32_t>(ShaderKind::phase_c_sketch_emit_flags):
        case static_cast<std::uint32_t>(ShaderKind::phase_c_sketch_emit_carry):
        case static_cast<std::uint32_t>(ShaderKind::phase_c_prefix_scan_block):
        case static_cast<std::uint32_t>(ShaderKind::phase_c_prefix_scan_add):
        case static_cast<std::uint32_t>(ShaderKind::phase_c_sketch_emit_words):
        case static_cast<std::uint32_t>(ShaderKind::phase_c_seed_count):
        case static_cast<std::uint32_t>(ShaderKind::phase_c_seed_emit):
        case static_cast<std::uint32_t>(ShaderKind::phase_c_persisted_directory_select):
        case static_cast<std::uint32_t>(ShaderKind::phase_c_sort_radix_histogram):
        case static_cast<std::uint32_t>(ShaderKind::phase_c_sort_radix_scan):
        case static_cast<std::uint32_t>(ShaderKind::phase_c_sort_radix_scatter):
        case static_cast<std::uint32_t>(ShaderKind::phase_c_index_mark_runs):
        case static_cast<std::uint32_t>(ShaderKind::phase_c_index_emit_buckets):
        case static_cast<std::uint32_t>(ShaderKind::phase_c_index_claim_slots):
        case static_cast<std::uint32_t>(ShaderKind::phase_c_index_emit_slots):
        case static_cast<std::uint32_t>(ShaderKind::phase_c_chain_range_build):
        case static_cast<std::uint32_t>(ShaderKind::phase_c_coordinate_reduce):
        case static_cast<std::uint32_t>(ShaderKind::phase_c_coordinate_vote_reduce):
        case static_cast<std::uint32_t>(ShaderKind::phase_c_coordinate_vote_reduce_batch):
        case static_cast<std::uint32_t>(ShaderKind::generate_units):
        case static_cast<std::uint32_t>(ShaderKind::seq_sort_extract_key_dna4):
        case static_cast<std::uint32_t>(ShaderKind::seq_sort_extract_key_generic):
        case static_cast<std::uint32_t>(ShaderKind::seq_sort_tiebreak_dna4):
        case static_cast<std::uint32_t>(ShaderKind::seq_sort_tiebreak_generic):
        case static_cast<std::uint32_t>(ShaderKind::seq_sort_global_compare_dna4):
        case static_cast<std::uint32_t>(ShaderKind::seq_sort_global_compare_generic):
        case static_cast<std::uint32_t>(ShaderKind::seq_sort_dense_scatter_dna4):
        case static_cast<std::uint32_t>(ShaderKind::seq_sort_dense_scatter_generic):
        case static_cast<std::uint32_t>(ShaderKind::seq_sort_prefix_check_dna4):
        case static_cast<std::uint32_t>(ShaderKind::seq_sort_prefix_check_generic):
        case static_cast<std::uint32_t>(ShaderKind::align_extend_simple_cigar):
            return static_cast<ShaderKind>(raw);
        default:
            throw ShaderCompileError("unknown shader kind " + std::to_string(raw));
    }
}

[[nodiscard]] inline constexpr ChainingPipelineKey choose_chaining_variant(
    const VulkanStaticVariantInputs& inputs) noexcept
{
    // DOC0353: subgroup-per-range chain — exactly ONE subgroup (32 lanes) owns one
    // range, so LOCAL_SIZE_X must equal the Apple/MoltenVK subgroup size (32). A
    // local_size > 32 would put multiple subgroups in a workgroup, racing one range.
    (void)inputs;
    return ChainingPipelineKey{32u, 32u};
}

[[nodiscard]] inline constexpr std::uint32_t anchor_sort_capacity(
    const VulkanStaticVariantInputs& inputs) noexcept
{
    constexpr std::uint32_t per_lane_shared_bytes = 28u;
    std::uint32_t capacity = 0;
    for (std::uint32_t candidate = 1; candidate <= 1024u; candidate <<= 1u) {
        if (candidate > inputs.max_workgroup_invocations) break;
        if (static_cast<std::uint64_t>(candidate) * per_lane_shared_bytes
            > inputs.max_shared_memory_bytes) {
            break;
        }
        capacity = candidate;
    }
    return capacity;
}

[[nodiscard]] inline constexpr ShaderVariantKey make_shader_key(
    ShaderKind kind,
    std::uint32_t tile_size,
    std::uint32_t local_size,
    std::uint32_t max_span) noexcept
{
    return ShaderVariantKey{
        static_cast<std::uint32_t>(kind), tile_size, local_size, max_span};
}

[[nodiscard]] inline constexpr bool is_chain_key(
    const ShaderVariantKey& key) noexcept
{
    if (key.kind != static_cast<std::uint32_t>(ShaderKind::chain_tiled_dp_u64)) {
        return false;
    }
    return key.tile_size == 32u && key.local_size == 32u && key.max_span == 0u;
}

[[nodiscard]] inline constexpr bool valid_shader_variant_key(
    const ShaderVariantKey& key) noexcept
{
    if (key.tile_size == 0 || key.tile_size > 1024 || key.local_size == 0) {
        return false;
    }
    if (is_chain_key(key)) return true;
    const auto kind = static_cast<ShaderKind>(key.kind);
    switch (kind) {
        case ShaderKind::seed_lookup_anchors:
            return key.tile_size == 384u
                && key.max_span == 256u
                && key.local_size == 128u;
        case ShaderKind::sort_anchors_bitonic:
            return key.tile_size == 384u
                && key.max_span == 256u
                && (key.local_size == 128u || key.local_size == 256u
                    || key.local_size == 512u || key.local_size == 1024u);
        case ShaderKind::phase_c_sketch_tile:
            return key.tile_size == 384u && key.local_size == 64u
                && key.max_span >= 1u && key.max_span <= 256u;
        case ShaderKind::phase_c_prefix_scan_block:
        case ShaderKind::phase_c_sort_radix_histogram:
        case ShaderKind::phase_c_sort_radix_scan:
        case ShaderKind::phase_c_sort_radix_scatter:
            return key.tile_size == 384u && key.local_size == 1u && key.max_span == 1u;
        case ShaderKind::phase_c_prefix_scan_add:
        case ShaderKind::phase_c_seed_count:
        case ShaderKind::phase_c_seed_emit:
        case ShaderKind::phase_c_persisted_directory_select:
        case ShaderKind::phase_c_index_mark_runs:
        case ShaderKind::phase_c_index_emit_buckets:
            return key.tile_size == 384u && key.local_size == 128u && key.max_span == 1u;
        case ShaderKind::phase_c_chain_range_build:
            return key.tile_size == 384u && key.local_size == 1u && key.max_span == 1u;
        case ShaderKind::phase_c_coordinate_reduce:
        case ShaderKind::phase_c_coordinate_vote_reduce:
        case ShaderKind::phase_c_coordinate_vote_reduce_batch:
            return key.tile_size == 384u && key.local_size == 128u && key.max_span == 1u;
        case ShaderKind::generate_units:
            return key.tile_size == 384u && key.local_size == 256u
                && (key.max_span == 32u || key.max_span == 64u);
        case ShaderKind::seq_sort_extract_key_dna4:
        case ShaderKind::seq_sort_extract_key_generic:
        case ShaderKind::seq_sort_tiebreak_dna4:
        case ShaderKind::seq_sort_tiebreak_generic:
        case ShaderKind::seq_sort_global_compare_dna4:
        case ShaderKind::seq_sort_global_compare_generic:
        case ShaderKind::seq_sort_dense_scatter_dna4:
        case ShaderKind::seq_sort_dense_scatter_generic:
        case ShaderKind::seq_sort_prefix_check_dna4:
        case ShaderKind::seq_sort_prefix_check_generic:
            return key.tile_size == 384u && key.local_size == 256u
                && key.max_span == 0u;
        case ShaderKind::align_extend_simple_cigar:
            return key.tile_size == 384u
                && (key.local_size == 1u || key.local_size == 64u)
                && key.max_span == 512u;
        default:
            return false;
    }
}

[[nodiscard]] inline std::string cache_file_stem(const ShaderVariantKey& key)
{
    char buffer[32]{};
    std::snprintf(
        buffer, sizeof(buffer), "sh_%02x_%04x_%04x_%04x",
        key.kind, key.tile_size, key.local_size, key.max_span);
    return std::string(buffer);
}

template <class Fn>
inline void for_each_static_shader_variant(
    const VulkanStaticVariantInputs& inputs,
    Fn&& visit)
{
    const auto emit = [&visit](ShaderVariantKey key) {
        if (valid_shader_variant_key(key)) {
            visit(key);
        }
    };

    emit(make_shader_key(ShaderKind::chain_tiled_dp_u64, 32u, 32u, 0u));
    emit(make_shader_key(ShaderKind::seed_lookup_anchors, 384u, 128u, 256u));
    emit(make_shader_key(ShaderKind::sort_anchors_bitonic, 384u, 128u, 256u));
    emit(make_shader_key(ShaderKind::sort_anchors_bitonic, 384u, 256u, 256u));
    emit(make_shader_key(ShaderKind::sort_anchors_bitonic, 384u, 512u, 256u));
    emit(make_shader_key(ShaderKind::sort_anchors_bitonic, 384u, 1024u, 256u));
    emit(make_shader_key(
        ShaderKind::phase_c_sketch_tile, 384u, 64u,
        inputs.default_window_size == 0 ? 1u : inputs.default_window_size));
    emit(make_shader_key(ShaderKind::phase_c_prefix_scan_block, 384u, 1u, 1u));
    emit(make_shader_key(ShaderKind::phase_c_prefix_scan_add, 384u, 128u, 1u));
    emit(make_shader_key(ShaderKind::phase_c_seed_count, 384u, 128u, 1u));
    emit(make_shader_key(ShaderKind::phase_c_seed_emit, 384u, 128u, 1u));
    emit(make_shader_key(
        ShaderKind::phase_c_persisted_directory_select, 384u, 128u, 1u));
    emit(make_shader_key(ShaderKind::phase_c_sort_radix_histogram, 384u, 1u, 1u));
    emit(make_shader_key(ShaderKind::phase_c_sort_radix_scan, 384u, 1u, 1u));
    emit(make_shader_key(ShaderKind::phase_c_sort_radix_scatter, 384u, 1u, 1u));
    emit(make_shader_key(ShaderKind::phase_c_index_mark_runs, 384u, 128u, 1u));
    emit(make_shader_key(ShaderKind::phase_c_index_emit_buckets, 384u, 128u, 1u));
    emit(make_shader_key(ShaderKind::phase_c_chain_range_build, 384u, 1u, 1u));
    emit(make_shader_key(ShaderKind::phase_c_coordinate_reduce, 384u, 128u, 1u));
    emit(make_shader_key(ShaderKind::phase_c_coordinate_vote_reduce, 384u, 128u, 1u));
    emit(make_shader_key(ShaderKind::phase_c_coordinate_vote_reduce_batch, 384u, 128u, 1u));
    emit(make_shader_key(ShaderKind::generate_units, 384u, 256u, 32u));
    emit(make_shader_key(ShaderKind::generate_units, 384u, 256u, 64u));
    emit(make_shader_key(ShaderKind::seq_sort_extract_key_dna4, 384u, 256u, 0u));
    emit(make_shader_key(ShaderKind::seq_sort_extract_key_generic, 384u, 256u, 0u));
    emit(make_shader_key(ShaderKind::seq_sort_tiebreak_dna4, 384u, 256u, 0u));
    emit(make_shader_key(ShaderKind::seq_sort_tiebreak_generic, 384u, 256u, 0u));
    emit(make_shader_key(ShaderKind::seq_sort_global_compare_dna4, 384u, 256u, 0u));
    emit(make_shader_key(ShaderKind::seq_sort_global_compare_generic, 384u, 256u, 0u));
    emit(make_shader_key(ShaderKind::seq_sort_dense_scatter_dna4, 384u, 256u, 0u));
    emit(make_shader_key(ShaderKind::seq_sort_dense_scatter_generic, 384u, 256u, 0u));
    emit(make_shader_key(ShaderKind::seq_sort_prefix_check_dna4, 384u, 256u, 0u));
    emit(make_shader_key(ShaderKind::seq_sort_prefix_check_generic, 384u, 256u, 0u));
    emit(make_shader_key(ShaderKind::align_extend_simple_cigar, 384u, 1u, 512u));
    emit(make_shader_key(ShaderKind::align_extend_simple_cigar, 384u, 64u, 512u));
}

} // namespace bioseq::gpu::vulkan
