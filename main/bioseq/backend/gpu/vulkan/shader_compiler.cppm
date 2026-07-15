/**
 * @file shader_compiler.cppm
 * @brief GLSL-to-SPIR-V compilation for the Phase A Vulkan chaining kernel.
 *
 * Compiles the checked-in `chain_tiled_dp_u64` compute shader (embedded as a
 * C++ raw-string literal so there is no runtime working-directory dependency)
 * through `Vulkan::glslang`.  The two workgroup/tile dimensions are injected as
 * compile-time `#define`s — NOT specialization constants — because the GLSL
 * `shared` array lengths must be baked into the emitted SPIR-V module (a
 * `tile_size`-dependent shared-array stride is part of the module layout).
 *
 * The Phase A tech spec (DOC0304 §4.2) fixes the dependency: `vulkan-sdk-
 * components` provides `Vulkan::glslang`; `shaderc` is intentionally NOT used.
 */
module;

#include "common.h"

#include <cstdint>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <glslang/Public/ShaderLang.h>
#include <glslang/Include/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>

export module bioseq.gpu.vulkan.shader_compiler;

import bioseq.common.mlogging;

export namespace bioseq::gpu::vulkan {

/// The kinds of compute shader the runtime knows how to compile.
enum class ShaderKind : std::uint8_t {
    chain_tiled_dp_u64,
    seed_lookup_anchors,
    sort_anchors_bitonic,
    // DOC0310 Phase C — resident two-submit frontend kernels. The sketch family
    // shares one tile-replay body driven by a runtime `emit_mode` push field;
    // the prefix-scan, radix-sort, GPU-index-build, seed, and chain-range
    // kernels each have their own body (DOC0310 §8–§15).
    phase_c_sketch_tile,
    phase_c_sketch_emit_flags,
    phase_c_sketch_emit_carry,
    phase_c_prefix_scan_block,
    phase_c_prefix_scan_add,
    phase_c_sketch_emit_words,
    phase_c_seed_count,
    phase_c_seed_emit,
    phase_c_sort_radix_histogram,
    phase_c_sort_radix_scan,
    phase_c_sort_radix_scatter,
    phase_c_index_mark_runs,
    phase_c_index_emit_buckets,
    phase_c_index_claim_slots,
    phase_c_index_emit_slots,
    phase_c_chain_range_build,
    phase_c_coordinate_reduce,
    phase_c_coordinate_vote_reduce,
    phase_c_coordinate_vote_reduce_batch,
    generate_units,
    // DOC0346 — GPU sequence sort. Two stages × two alphabets. The cache keys on
    // ShaderKind, so the dna4 and generic variants are distinct SPIR-V modules
    // automatically; storage width is a RUNTIME push field (pc.unit_bits).
    seq_sort_extract_key_dna4,
    seq_sort_extract_key_generic,
    seq_sort_tiebreak_dna4,
    seq_sort_tiebreak_generic,
    seq_sort_global_compare_dna4,
    seq_sort_global_compare_generic,
    seq_sort_dense_scatter_dna4,
    seq_sort_dense_scatter_generic,
    seq_sort_prefix_check_dna4,
    seq_sort_prefix_check_generic,
    align_extend_banded_affine,
    align_extend_simple_cigar,
    // Appended to preserve every established ShaderKind cache ordinal.
    // DOC0371: consume resident query-minimizer words plus the authoritative
    // encoded revision-3 directory and return only selected directory ordinals.
    phase_c_persisted_directory_select,
    // Inactive format-4 ABI2 kernels. Appended so every established shader
    // cache ordinal remains stable; no production runtime route selects these.
    phase_c_grouped32_validate,
    phase_c_grouped32_accumulate_occurrences,
    phase_c_grouped32_finalize_max_occ,
    phase_c_grouped32_allocate_anchor_prefix,
    phase_c_grouped32_emit_global_anchors,
    phase_c_grouped32_build_sort_segments,
    phase_c_grouped32_adapt_query_sketch,
    phase_c_grouped32_prepare_indirect,
    phase_c_grouped32_clear_state,
    phase_c_grouped32_finalize_query_adapter,
    phase_c_grouped32_finalize_validation,
    // DOC0371 bounded persisted format-4 target stream.  These are distinct
    // from the older aggregate-residency ABI2 prototypes above: controls and
    // positions are consumed in separate bounded-slot phases.
    phase_c_grouped32_stream_control,
    phase_c_grouped32_stream_authorize,
    phase_c_grouped32_stream_position,
    phase_c_grouped32_stream_seal,
    // Direct persisted Format-5/ABI3 seed lookup.  These kernels consume the
    // variable-width SoA control and segment-local position byte streams; no
    // host-side control decode or target-index rebuild is part of this path.
    phase_c_shape_v5_seed_count,
    phase_c_shape_v5_seed_emit,
};

/// The descriptor-set layout each shader needs. The runtime keeps one cached
/// layout per kind (DOC0307 §4.5).
enum class DescriptorLayoutKind : std::uint8_t {
    chain_tiled_dp,
    seed_lookup,
    anchor_sort,
    // DOC0310 §8 — reusable Phase C layouts. One layout per kernel family;
    // kernels that bind the same buffers share a layout.
    phase_c_sketch,
    phase_c_scan,
    phase_c_seed,
    phase_c_sort,
    phase_c_index,
    phase_c_chain_range,
    phase_c_coordinate_reduce,
    generate_units,
    seq_sort,   // DOC0346 — { SeqWords, Descs, KeysOrIndex, ValsOrRanges }
    align_extension,  // { QuerySymbols, TargetSymbols, Jobs, Results }
    align_extension_cigar,  // { QuerySymbols, TargetSymbols, Jobs, Results, Cigar, Traceback }
    phase_c_persisted_directory_select,
    // Thirteen-buffer common ABI2 layout. Keeping one explicit layout makes the
    // inactive kernels cache-compatible without creating a live runtime path.
    phase_c_grouped32_abi2,
    // Existing packed sketch output -> grouped32 query-record adapter. The
    // preparation pass uses the core layout so it can length-check partitions
    // and query records before generating an indirect validation command.
    phase_c_grouped32_adapter_abi2,
    // Nine-buffer raw format-4 streaming layout.  It deliberately has no
    // aggregate control+position resident-set binding.
    phase_c_grouped32_stream,
    // Nine-buffer direct Format-5 seed layout: query minimizer words,
    // resident partition descriptors, raw control words, raw position words,
    // query records, counts, prefix, anchors, and anchor keys.
    phase_c_shape_v5_seed,
};

/// Number of std430 storage-buffer bindings in each descriptor layout.
[[nodiscard]] constexpr std::uint32_t descriptor_binding_count(
    DescriptorLayoutKind kind) noexcept
{
    switch (kind) {
        case DescriptorLayoutKind::chain_tiled_dp:        return 4;
        case DescriptorLayoutKind::seed_lookup:           return 6;
        case DescriptorLayoutKind::anchor_sort:           return 2;
        // DOC0310 Phase C layouts.
        case DescriptorLayoutKind::phase_c_sketch:        return 9;
        case DescriptorLayoutKind::phase_c_scan:          return 3;
        case DescriptorLayoutKind::phase_c_seed:          return 8;
        case DescriptorLayoutKind::phase_c_persisted_directory_select: return 4;
        case DescriptorLayoutKind::phase_c_sort:          return 5;
        case DescriptorLayoutKind::phase_c_index:         return 7;
        case DescriptorLayoutKind::phase_c_chain_range:   return 6;
        case DescriptorLayoutKind::phase_c_coordinate_reduce: return 6;
        case DescriptorLayoutKind::generate_units:         return 2;
        case DescriptorLayoutKind::seq_sort:               return 4;
        case DescriptorLayoutKind::align_extension:        return 4;
        case DescriptorLayoutKind::align_extension_cigar:  return 6;
        case DescriptorLayoutKind::phase_c_grouped32_abi2: return 13;
        case DescriptorLayoutKind::phase_c_grouped32_adapter_abi2: return 9;
        case DescriptorLayoutKind::phase_c_grouped32_stream: return 9;
        case DescriptorLayoutKind::phase_c_shape_v5_seed: return 9;
    }
    return 0;
}

/// Map a shader kind to the descriptor layout it binds.
[[nodiscard]] constexpr DescriptorLayoutKind descriptor_layout_for(
    ShaderKind kind) noexcept
{
    switch (kind) {
        case ShaderKind::chain_tiled_dp_u64:    return DescriptorLayoutKind::chain_tiled_dp;
        case ShaderKind::seed_lookup_anchors:   return DescriptorLayoutKind::seed_lookup;
        case ShaderKind::sort_anchors_bitonic:  return DescriptorLayoutKind::anchor_sort;
        case ShaderKind::phase_c_sketch_tile:
        case ShaderKind::phase_c_sketch_emit_flags:
        case ShaderKind::phase_c_sketch_emit_carry:
        case ShaderKind::phase_c_sketch_emit_words:   return DescriptorLayoutKind::phase_c_sketch;
        case ShaderKind::phase_c_prefix_scan_block:
        case ShaderKind::phase_c_prefix_scan_add:     return DescriptorLayoutKind::phase_c_scan;
        case ShaderKind::phase_c_seed_count:
        case ShaderKind::phase_c_seed_emit:           return DescriptorLayoutKind::phase_c_seed;
        case ShaderKind::phase_c_persisted_directory_select:
            return DescriptorLayoutKind::phase_c_persisted_directory_select;
        case ShaderKind::phase_c_grouped32_validate:
        case ShaderKind::phase_c_grouped32_accumulate_occurrences:
        case ShaderKind::phase_c_grouped32_finalize_max_occ:
        case ShaderKind::phase_c_grouped32_allocate_anchor_prefix:
        case ShaderKind::phase_c_grouped32_emit_global_anchors:
        case ShaderKind::phase_c_grouped32_build_sort_segments:
        case ShaderKind::phase_c_grouped32_prepare_indirect:
        case ShaderKind::phase_c_grouped32_clear_state:
        case ShaderKind::phase_c_grouped32_finalize_validation:
            return DescriptorLayoutKind::phase_c_grouped32_abi2;
        case ShaderKind::phase_c_grouped32_adapt_query_sketch:
        case ShaderKind::phase_c_grouped32_finalize_query_adapter:
            return DescriptorLayoutKind::phase_c_grouped32_adapter_abi2;
        case ShaderKind::phase_c_grouped32_stream_control:
        case ShaderKind::phase_c_grouped32_stream_authorize:
        case ShaderKind::phase_c_grouped32_stream_position:
        case ShaderKind::phase_c_grouped32_stream_seal:
            return DescriptorLayoutKind::phase_c_grouped32_stream;
        case ShaderKind::phase_c_shape_v5_seed_count:
        case ShaderKind::phase_c_shape_v5_seed_emit:
            return DescriptorLayoutKind::phase_c_shape_v5_seed;
        case ShaderKind::phase_c_sort_radix_histogram:
        case ShaderKind::phase_c_sort_radix_scan:
        case ShaderKind::phase_c_sort_radix_scatter:  return DescriptorLayoutKind::phase_c_sort;
        case ShaderKind::phase_c_index_mark_runs:
        case ShaderKind::phase_c_index_emit_buckets:
        case ShaderKind::phase_c_index_claim_slots:
        case ShaderKind::phase_c_index_emit_slots:    return DescriptorLayoutKind::phase_c_index;
        case ShaderKind::phase_c_chain_range_build:   return DescriptorLayoutKind::phase_c_chain_range;
        case ShaderKind::phase_c_coordinate_reduce:
        case ShaderKind::phase_c_coordinate_vote_reduce:
        case ShaderKind::phase_c_coordinate_vote_reduce_batch:
            return DescriptorLayoutKind::phase_c_coordinate_reduce;
        case ShaderKind::generate_units:              return DescriptorLayoutKind::generate_units;
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
            return DescriptorLayoutKind::seq_sort;
        case ShaderKind::align_extend_banded_affine:  return DescriptorLayoutKind::align_extension;
        case ShaderKind::align_extend_simple_cigar:   return DescriptorLayoutKind::align_extension_cigar;
    }
    return DescriptorLayoutKind::chain_tiled_dp;
}

/// Push-constant block size (bytes) each shader's `Params` uniform declares.
/// The anchor sort takes its range from a buffer and has no push constant.
[[nodiscard]] constexpr std::uint32_t push_constant_size(ShaderKind kind) noexcept {
    switch (kind) {
        case ShaderKind::chain_tiled_dp_u64:    return 36;  // ChainingPushConstants
        case ShaderKind::seed_lookup_anchors:   return 44;  // 11 x uint32
        case ShaderKind::sort_anchors_bitonic:  return 0;
        // DOC0310 Phase C — per-family push blocks (see runtime.cppm structs).
        case ShaderKind::phase_c_sketch_tile:
        case ShaderKind::phase_c_sketch_emit_flags:
        case ShaderKind::phase_c_sketch_emit_carry:
        case ShaderKind::phase_c_sketch_emit_words:   return 72;  // SketchPushC (18 × u32)
        case ShaderKind::phase_c_prefix_scan_block:
        case ShaderKind::phase_c_prefix_scan_add:     return 16;  // ScanPushC (4 × u32)
        case ShaderKind::phase_c_seed_count:
        case ShaderKind::phase_c_seed_emit:           return 52;  // SeedPushC (13 × u32)
        case ShaderKind::phase_c_persisted_directory_select:
            return 32;  // GpuPersistedDirectorySelectionPush
        case ShaderKind::phase_c_grouped32_validate:
        case ShaderKind::phase_c_grouped32_accumulate_occurrences:
        case ShaderKind::phase_c_grouped32_finalize_max_occ:
        case ShaderKind::phase_c_grouped32_allocate_anchor_prefix:
        case ShaderKind::phase_c_grouped32_emit_global_anchors:
        case ShaderKind::phase_c_grouped32_build_sort_segments:
        case ShaderKind::phase_c_grouped32_clear_state:
        case ShaderKind::phase_c_grouped32_finalize_validation:
            return 72;  // GpuGrouped32Abi2Push
        case ShaderKind::phase_c_grouped32_adapt_query_sketch:
        case ShaderKind::phase_c_grouped32_finalize_query_adapter:
            return 64;  // GpuGrouped32SketchAdapterPush
        case ShaderKind::phase_c_grouped32_prepare_indirect:
            return 48;  // GpuGrouped32IndirectPreparePush
        case ShaderKind::phase_c_grouped32_stream_control:
        case ShaderKind::phase_c_grouped32_stream_authorize:
        case ShaderKind::phase_c_grouped32_stream_position:
        case ShaderKind::phase_c_grouped32_stream_seal:
            return 80;  // GpuGrouped32StreamPush
        case ShaderKind::phase_c_shape_v5_seed_count:
        case ShaderKind::phase_c_shape_v5_seed_emit:
            return 72;  // GpuShapeV5SeedPush
        case ShaderKind::phase_c_sort_radix_histogram:
        case ShaderKind::phase_c_sort_radix_scan:
        case ShaderKind::phase_c_sort_radix_scatter:  return 20;  // SortPushC (5 × u32)
        case ShaderKind::phase_c_index_mark_runs:
        case ShaderKind::phase_c_index_emit_buckets:
        case ShaderKind::phase_c_index_claim_slots:
        case ShaderKind::phase_c_index_emit_slots:    return 24;  // IndexPushC (6 × u32)
        case ShaderKind::phase_c_chain_range_build:   return 8;   // ChainRangePushC (2 × u32)
        case ShaderKind::phase_c_coordinate_reduce:
        case ShaderKind::phase_c_coordinate_vote_reduce:
        case ShaderKind::phase_c_coordinate_vote_reduce_batch:
            return 28;  // GpuCoordinateReducePush
        case ShaderKind::generate_units:              return 48;  // GenerateUnitsPush (12 × u32)
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
            return 40;  // SeqSortPush (10 × u32)
        case ShaderKind::align_extend_banded_affine:  return 44;  // AlignmentExtensionPush
        case ShaderKind::align_extend_simple_cigar:   return 60;  // AlignmentCigarPush
    }
    return 0;
}

/// One shader-compilation request. `tile_size`, `local_size`, and `max_span`
/// are substituted as compile-time `#define`s before glslang runs. A shader may
/// intentionally bake a fixed workgroup geometry instead of consuming
/// `LOCAL_SIZE_X`; `expected_shader_workgroup_size()` is the authoritative
/// compile/cache/runtime ABI for that distinction. The remaining fields
/// describe runtime push-constant routing and are carried for the pipeline
/// cache key (DOC0307 §4.5, §12).
struct ShaderCompileRequest {
    ShaderKind kind;
    std::uint32_t tile_size = 384;       // chain only
    std::uint32_t local_size = 128;
    std::uint32_t max_span = 256;        // shader-specific compile-time span
    std::uint32_t word_kind = 0;
    std::uint32_t alphabet_kind = 0;
    std::uint32_t unit_bits = 0;
    std::uint32_t bits_per_char = 0;
};

struct ShaderWorkgroupSize {
    std::uint32_t x = 1;
    std::uint32_t y = 1;
    std::uint32_t z = 1;

    [[nodiscard]] bool operator==(const ShaderWorkgroupSize&) const = default;
};

/// Compile-time execution geometry encoded in SPIR-V. Most kernels consume the
/// request's LOCAL_SIZE_X macro. The radix kernels deliberately keep their
/// established cache-key sentinel (`local_size == 1`) while their algorithm
/// requires one lane per radix bin. Keep this mapping explicit: treating a key
/// selector as reflected execution metadata rejects valid AOT artifacts and
/// makes archive publication/completeness disagree.
[[nodiscard]] inline constexpr ShaderWorkgroupSize expected_shader_workgroup_size(
    const ShaderCompileRequest& request) noexcept
{
    switch (request.kind) {
        case ShaderKind::phase_c_sort_radix_histogram:
        case ShaderKind::phase_c_sort_radix_scan:
        case ShaderKind::phase_c_sort_radix_scatter:
            return ShaderWorkgroupSize{256u, 1u, 1u};
        case ShaderKind::phase_c_chain_range_build:
        case ShaderKind::align_extend_banded_affine:
            return ShaderWorkgroupSize{1u, 1u, 1u};
        default:
            return ShaderWorkgroupSize{request.local_size, 1u, 1u};
    }
}

/// Thrown by `compile_shader_spirv` on a rejected request or a glslang
/// parse/link failure. The Vulkan runtime catches this and re-wraps it as a
/// `VulkanRuntimeError`; selected Vulkan algorithms propagate that error.
class ShaderCompileError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/**
 * @brief Compile a Phase A compute shader to SPIR-V words.
 *
 * Builds the full GLSL source by prepending `#version`, the int64 extension,
 * and the two computed `#define`s to the embedded shader body, then drives
 * glslang (Vulkan 1.2 client, SPIR-V 1.5 target).
 *
 * @param request  Shader kind plus the tile/local workgroup dimensions.
 * @return The compiled SPIR-V module as a vector of 32-bit words.
 * @throws ShaderCompileError if `tile_size > 1024`, `local_size == 0`, or
 *         glslang fails to parse or link the shader.
 */
[[nodiscard]] std::vector<std::uint32_t> compile_shader_spirv(
    const ShaderCompileRequest& request);

/// Return the embedded GLSL body for cache digesting.
[[nodiscard]] std::string shader_body_for_digest(ShaderKind kind);

}  // namespace bioseq::gpu::vulkan

// ============================================================================
// Implementation
// ============================================================================

namespace bioseq::gpu::vulkan {

namespace {

/// The tiled DP score-generation kernel body, beginning right after the two
/// `#define`s the compiler injects. Ported from G3SA's
/// `mm_compute_chain_long_tiled` (chaining_kernel.cu) but adapted to bioseq's
/// 24-byte `Anchor` ABI: full 64-bit ref/query positions, per-anchor
/// `query_span` instead of the hard-coded `MM_QSPAN=15`, runtime `max_iter`
/// instead of `MAX_ITER_CHAIN=5000`, and absolute predecessor indices instead
/// of G3SA's parent-distance encoding (so `backtrack_chains` consumes them
/// directly). One workgroup owns one independent hard range, so no atomics are
/// needed for the tail sweep.
constexpr std::string_view kChainTiledDpBody = R"GLSL(
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_arithmetic : require
#extension GL_KHR_shader_subgroup_ballot : require

layout(local_size_x = LOCAL_SIZE_X, local_size_y = 1, local_size_z = 1) in;

const uint SUBGROUP_LANES_U = 32u;
const uint NO_PARENT = 0xffffffffu;
const int INVALID_SCORE = -2147483647 - 1;

struct AnchorWire {
    uint64_t ref_position;
    uint64_t query_position;
    uint is_reverse;
    uint span_flags_segment;
};

struct ChainRangeWire {
    uint begin;
    uint range_len;
};

layout(std430, set = 0, binding = 0) readonly buffer Anchors {
    AnchorWire anchors[];
};
layout(std430, set = 0, binding = 1) readonly buffer Ranges {
    ChainRangeWire ranges[];
};
layout(std430, set = 0, binding = 2) buffer BestScores {
    coherent int best_scores[];
};
layout(std430, set = 0, binding = 3) buffer Predecessors {
    int64_t predecessors[];
};

layout(push_constant) uniform Params {
    uint anchor_count;
    uint range_count;
    int max_dist_x;
    int max_dist_y;
    int bandwidth;
    int max_iter;
    float gap_penalty;
    float skip_penalty;
    int max_chain_skip;
} pc;

bool anchor_reverse(AnchorWire a) { return a.is_reverse != 0u; }
uint anchor_span(AnchorWire a) { return a.span_flags_segment & 0xffu; }

float fast_log2(float x) {
    uint z = floatBitsToUint(x);
    float log_2 = float((z >> 23u) & 255u) - 128.0;
    z &= ~(255u << 23u);
    z += 127u << 23u;
    float f = uintBitsToFloat(z);
    return log_2 + (-0.34484843 * f + 2.02466578) * f - 0.67487759;
}

int chain_score(AnchorWire current, AnchorWire pred) {
    if (anchor_reverse(current) != anchor_reverse(pred)) return INVALID_SCORE;
    if (current.query_position <= pred.query_position) return INVALID_SCORE;
    if (current.ref_position <= pred.ref_position) return INVALID_SCORE;

    uint64_t dq64 = current.query_position - pred.query_position;
    uint64_t dr64 = current.ref_position - pred.ref_position;
    if (dq64 > uint64_t(pc.max_dist_x)) return INVALID_SCORE;
    if (dr64 == uint64_t(0) || dq64 > uint64_t(pc.max_dist_y)) return INVALID_SCORE;
    if (dr64 > uint64_t(pc.max_dist_x)) return INVALID_SCORE;

    int dq = int(dq64);
    int dr = int(dr64);
    int dd = dr > dq ? dr - dq : dq - dr;
    if (dd > pc.bandwidth) return INVALID_SCORE;

    int dg = dr < dq ? dr : dq;
    int q_span = int(anchor_span(pred));
    int score = q_span < dg ? q_span : dg;
    if (dd != 0 || dg > q_span) {
        float lin_pen = pc.gap_penalty * float(dd) + pc.skip_penalty * float(dg);
        float log_pen = dd >= 1 ? fast_log2(float(dd + 1)) : 0.0;
        score -= int(lin_pen + 0.5 * log_pen);
    }
    return score;
}

uint signed_score_key(int score) {
    return uint(score) ^ 0x80000000u;
}

void main() {
    uint lane = gl_SubgroupInvocationID;

    for (uint range_id = gl_WorkGroupID.x; range_id < pc.range_count;
         range_id += gl_NumWorkGroups.x) {
        ChainRangeWire r = ranges[range_id];
        uint begin = r.begin;
        uint end = min(pc.anchor_count, r.begin + r.range_len);
        if (begin >= end) continue;

        for (uint global_i = begin; global_i < end; ++global_i) {
            AnchorWire a = anchors[global_i];
            int best = max(best_scores[global_i], int(anchor_span(a)));
            uint best_parent = NO_PARENT;
            uint64_t scanned = uint64_t(0);
            uint no_improve_batches = 0u;
            uint max_no_improve_batches = pc.max_chain_skip > 0
                ? (uint(pc.max_chain_skip) / SUBGROUP_LANES_U + 1u)
                : 0xffffffffu;

            while (true) {
                uint64_t remaining = uint64_t(global_i) - uint64_t(begin);
                uint64_t offset = scanned + uint64_t(lane) + uint64_t(1);
                bool in_range = offset <= remaining;

                uint pred = begin;
                AnchorWire pa = a;
                if (in_range) {
                    pred = global_i - uint(offset);
                    pa = anchors[pred];
                }

                uint first_out = subgroupMin(!in_range ? lane : SUBGROUP_LANES_U);
                uint first_iter = subgroupMin(
                    (in_range && offset > uint64_t(uint(pc.max_iter))) ? lane : SUBGROUP_LANES_U);
                uint first_ref = subgroupMin(
                    (in_range
                     && anchor_reverse(a) == anchor_reverse(pa)
                     && a.ref_position > pa.ref_position
                     && a.ref_position - pa.ref_position > uint64_t(pc.max_dist_x))
                        ? lane : SUBGROUP_LANES_U);
                uint first_stop = min(first_out, min(first_iter, first_ref));
                bool lane_active = in_range && lane < first_stop;

                int candidate_total = INVALID_SCORE;
                if (lane_active) {
                    int pair = chain_score(a, pa);
                    if (pair != INVALID_SCORE) {
                        candidate_total = pair + best_scores[pred];
                    }
                }

                uint candidate_score_key = signed_score_key(candidate_total);
                uint max_score_key = subgroupMax(candidate_score_key);
                bool is_max_score_lane = candidate_score_key == max_score_key;
                int batch_score = subgroupMax(is_max_score_lane ? candidate_total : INVALID_SCORE);
                uint batch_parent = subgroupMax(
                    (is_max_score_lane && lane_active && candidate_total != INVALID_SCORE) ? pred : 0u);

                bool stop_after_batch = first_stop < SUBGROUP_LANES_U;
                if (batch_score > best) {
                    best = batch_score;
                    best_parent = batch_parent;
                    no_improve_batches = 0u;
                } else if (pc.max_chain_skip > 0) {
                    ++no_improve_batches;
                    if (no_improve_batches >= max_no_improve_batches) {
                        stop_after_batch = true;
                    }
                }

                if (stop_after_batch) break;
                scanned += uint64_t(SUBGROUP_LANES_U);
            }

            if (subgroupElect()) {
                best_scores[global_i] = best;
                predecessors[global_i] = best_parent == NO_PARENT
                    ? int64_t(-1) : int64_t(best_parent);
            }
            memoryBarrierBuffer();
            barrier();
        }
    }
}
)GLSL";

constexpr std::string_view kSeedLookupAnchorsBody = R"GLSL(
layout(local_size_x = LOCAL_SIZE_X, local_size_y = 1, local_size_z = 1) in;

struct GpuMinimizerSlot {
    uint64_t hash;
    uint pos_offset;
    uint pos_count;
};

struct AnchorWire {
    uint64_t ref_position;
    uint64_t query_position;
    uint is_reverse;
    uint span_flags_segment; // span | flags<<8 | segment_id<<16
};

layout(std430, set = 0, binding = 0) readonly buffer MinWords { uint min_words[]; };
layout(std430, set = 0, binding = 1) readonly buffer Slots { GpuMinimizerSlot slots[]; };
layout(std430, set = 0, binding = 2) readonly buffer Positions { uint64_t positions[]; };
layout(std430, set = 0, binding = 3) readonly buffer PrefixCounts { uint prefix_counts[]; };
layout(std430, set = 0, binding = 4) buffer Anchors { AnchorWire anchors[]; };
layout(std430, set = 0, binding = 5) buffer AnchorKeys { uint64_t anchor_keys[]; };

layout(push_constant) uniform Params {
    uint minimizer_count;
    uint words32_per_minimizer;
    uint word_kind;
    uint hash_shift;
    uint pos_bits;
    uint query_len;
    uint kmer_size;
    uint slot_mask;
    uint bucket_count;
    uint exact_anchor_count;
    uint target_ref_bits;
} pc;

uint64_t anchor_sort_key(uint rev, uint64_t ref_position) {
    return (uint64_t(rev) << pc.target_ref_bits) | ref_position;
}

uint64_t read_min_hash(uint i) {
    if (pc.word_kind == 0u) {
        uint packed = min_words[i >> 1u];
        uint raw = (packed >> ((i & 1u) * 16u)) & 0xffffu;
        return uint64_t(raw) >> pc.hash_shift;
    }
    uint base = i * pc.words32_per_minimizer;
    if (pc.word_kind == 1u) {
        return uint64_t(min_words[base]) >> pc.hash_shift;
    }
    if (pc.word_kind == 2u) {
        uint64_t raw = uint64_t(min_words[base])
                     | (uint64_t(min_words[base + 1u]) << 32);
        return raw >> pc.hash_shift;
    }
    uint64_t high = uint64_t(min_words[base + 2u])
                  | (uint64_t(min_words[base + 3u]) << 32);
    return high >> 8;
}

uint64_t read_min_pos_strand(uint i) {
    if (pc.word_kind == 0u) {
        uint packed = min_words[i >> 1u];
        uint raw = (packed >> ((i & 1u) * 16u)) & 0xffffu;
        uint mask = (pc.pos_bits >= 15u) ? 0xffffu : ((1u << (pc.pos_bits + 1u)) - 1u);
        return uint64_t(raw & mask);
    }
    uint base = i * pc.words32_per_minimizer;
    if (pc.word_kind == 1u) {
        uint raw = min_words[base];
        uint mask = (pc.pos_bits >= 31u) ? 0xffffffffu : ((1u << (pc.pos_bits + 1u)) - 1u);
        return uint64_t(raw & mask);
    }
    if (pc.word_kind == 2u) {
        uint64_t raw = uint64_t(min_words[base])
                     | (uint64_t(min_words[base + 1u]) << 32);
        uint64_t mask = pc.pos_bits >= 63u
            ? ~uint64_t(0)
            : ((uint64_t(1) << (pc.pos_bits + 1u)) - uint64_t(1));
        return raw & mask;
    }
    return uint64_t(min_words[base])
         | (uint64_t(min_words[base + 1u]) << 32);
}

uint64_t splitmix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15UL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9UL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebUL;
    return x ^ (x >> 31);
}

uint find_slot(uint64_t hash) {
    if (pc.bucket_count != 0u) {
        uint lo = 0u;
        uint hi = pc.bucket_count;
        while (lo < hi) {
            uint mid = lo + ((hi - lo) >> 1u);
            if (slots[mid].hash < hash) lo = mid + 1u; else hi = mid;
        }
        if (lo < pc.bucket_count && slots[lo].hash == hash) return lo;
        return 0xffffffffu;
    }
    uint p = uint(splitmix64(hash)) & pc.slot_mask;
    for (uint step = 0u; step <= pc.slot_mask; ++step) {
        GpuMinimizerSlot s = slots[p];
        if (s.pos_count == 0u) return 0xffffffffu;
        if (s.hash == hash) return p;
        p = (p + 1u) & pc.slot_mask;
    }
    return 0xffffffffu;
}

void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= pc.minimizer_count) return;

    uint64_t hash = read_min_hash(i);
    uint slot_idx = find_slot(hash);
    if (slot_idx == 0xffffffffu) return;

    GpuMinimizerSlot slot = slots[slot_idx];
    uint begin = prefix_counts[i];
    uint end = prefix_counts[i + 1u];
    if (slot.pos_count == 0u || begin == end) return;

    uint64_t q_encoded = read_min_pos_strand(i);
    uint64_t q_pos = q_encoded >> 1;
    uint q_strand = uint(q_encoded & uint64_t(1));
    uint64_t reverse_base = uint64_t(pc.query_len - 2u + pc.kmer_size);
    uint span_flags_segment = pc.kmer_size & 0xffu;

    for (uint j = 0u; j < slot.pos_count; ++j) {
        uint dst = begin + j;
        if (dst >= pc.exact_anchor_count) return;
        uint64_t target = positions[slot.pos_offset + j]; // (pos<<1)|strand
        uint t_strand = uint(target & uint64_t(1));
        uint rev = t_strand ^ q_strand;
        uint64_t query_position = rev != 0u ? (reverse_base - q_pos) : q_pos;
        anchors[dst].ref_position = target >> 1;
        anchors[dst].query_position = query_position;
        anchors[dst].is_reverse = rev;
        anchors[dst].span_flags_segment = span_flags_segment;
        anchor_keys[dst] = anchor_sort_key(rev, target >> 1);
    }
}
)GLSL";

/// DOC0307 §7.2 — per-query bitonic anchor sort. One workgroup sorts up to
/// LOCAL_SIZE_X anchors by `ref_bits()` in shared memory. Larger anchor sets
/// are routed to the stable device radix path; host sorting is forbidden.
constexpr std::string_view kSortAnchorsBitonicBody = R"GLSL(
layout(local_size_x = LOCAL_SIZE_X, local_size_y = 1, local_size_z = 1) in;

struct AnchorWire {
    uint64_t ref_position;
    uint64_t query_position;
    uint is_reverse;
    uint span_flags_segment;
};

struct SortRange {
    uint begin;
    uint range_len;  // NB: not 'length' — that clashes with GLSL's .length() builtin
};

layout(std430, set = 0, binding = 0) buffer Anchors { AnchorWire anchors[]; };
layout(std430, set = 0, binding = 1) readonly buffer SortRanges { SortRange ranges[]; };

shared AnchorWire sh_anchor[LOCAL_SIZE_X];
shared uint sh_orig[LOCAL_SIZE_X];

uint64_t ref_bits(AnchorWire a) {
    return (uint64_t(a.is_reverse != 0u ? 1u : 0u) << 63)
         | (a.ref_position & 0x7fffffffffffffffUL);
}

bool anchor_less(AnchorWire a, AnchorWire b, uint ia, uint ib) {
    uint64_t ka = ref_bits(a);
    uint64_t kb = ref_bits(b);
    if (ka != kb) return ka < kb;
    return ia < ib;
}

AnchorWire max_anchor() {
    return AnchorWire(0x7fffffffffffffffUL, 0xffffffffffffffffUL, 1u, 0xffffffffu);
}

void main() {
    uint range_id = gl_WorkGroupID.x;
    uint tid = gl_LocalInvocationID.x;
    SortRange r = ranges[range_id];
    if (r.range_len > LOCAL_SIZE_X) return;

    if (tid < r.range_len) {
        sh_anchor[tid] = anchors[r.begin + tid];
        sh_orig[tid] = tid;
    } else {
        sh_anchor[tid] = max_anchor();
        sh_orig[tid] = tid;
    }
    barrier();

    for (uint size = 2u; size <= LOCAL_SIZE_X; size <<= 1u) {
        for (uint stride = size >> 1u; stride > 0u; stride >>= 1u) {
            uint other = tid ^ stride;
            if (other > tid) {
                bool up = (tid & size) == 0u;
                AnchorWire a = sh_anchor[tid];
                AnchorWire b = sh_anchor[other];
                uint ia = sh_orig[tid];
                uint ib = sh_orig[other];
                bool should_swap = up
                    ? anchor_less(b, a, ib, ia)
                    : anchor_less(a, b, ia, ib);
                if (should_swap) {
                    sh_anchor[tid] = b;
                    sh_anchor[other] = a;
                    sh_orig[tid] = ib;
                    sh_orig[other] = ia;
                }
            }
            barrier();
        }
    }

    if (tid < r.range_len) {
        anchors[r.begin + tid] = sh_anchor[tid];
    }
}
)GLSL";

// ===========================================================================
// DOC0310 Phase C kernels
// ===========================================================================

/// DOC0310 §9 — ordered tile-replay minimizer sketch. One workgroup per tile
/// (1 active invocation) replays the exact `collect_minimizers_view` state
/// machine over `[replay_lo, loop_hi)`, emitting only minimizers whose trigger
/// position falls in this tile's owned window interval `[tile_lo, tile_hi)`.
/// The left-context overlap (`tile_lo - replay_lo`) reconstructs the ring buffer
/// and the valid-base/init state so the concatenation across tiles in tile
/// order equals the single serial pass — but it is parallel across tiles (never
/// a one-workgroup-per-sequence serial emit). `emit_mode==0` counts per tile;
/// `emit_mode==1` writes raw `MinimizerRepr` words at `out_base + local`.
constexpr std::string_view kPhaseCSketchBody = R"GLSL(
layout(local_size_x = LOCAL_SIZE_X, local_size_y = 1, local_size_z = 1) in;

struct TileC {
    uint seq_word_offset;
    uint bp_count;
    uint replay_lo;
    uint tile_lo;
    uint tile_hi;
    uint out_base;
};

layout(std430, set = 0, binding = 0) readonly buffer SeqWords { uint seq_words[]; };
layout(std430, set = 0, binding = 1) readonly buffer RankLut {
    uint rank_to_code[256];
    uint rank_to_rc_code[256];
};
layout(std430, set = 0, binding = 2) readonly buffer Tiles { TileC tiles[]; };
layout(std430, set = 0, binding = 3) buffer TileCounts { uint tile_counts[]; };
layout(std430, set = 0, binding = 4) buffer OutWords { uint out_words[]; };
layout(std430, set = 0, binding = 5) buffer Counters { uint counters[]; };
layout(std430, set = 0, binding = 6) readonly buffer TileOffsets { uint tile_offsets[]; };
layout(std430, set = 0, binding = 7) buffer OutPairVals { uint64_t out_pair_vals[]; };
layout(std430, set = 0, binding = 8) readonly buffer QueryGate { uint query_gate_words[]; };

layout(push_constant) uniform Params {
    uint emit_mode;
    uint window_size;
    uint kmer_size;
    uint hash_bits;
    uint hash_shift;
    uint pos_bits;
    uint word_kind;
    uint words32_per_minimizer;
    uint span_value;
    uint alphabet_kind;
    uint unit_bits;
    uint bits_per_char;
    uint letters_per_unit;
    uint char_mask;
    uint tile_count;
    uint emit_pairs;
    uint query_gate_mask;
    uint query_gate_word_count;
} pc;

)GLSL"
R"GLSL(
uint read_u32_word(uint word_index) { return seq_words[word_index]; }
uint read_storage_unit(uint unit_index, uint unit_bits, uint byte_offset_words) {
    if (unit_bits == 64u) {
        uint lo = read_u32_word(byte_offset_words + unit_index * 2u);
        uint hi = read_u32_word(byte_offset_words + unit_index * 2u + 1u);
        return lo ^ hi;
    }
    if (unit_bits == 32u) { return read_u32_word(byte_offset_words + unit_index); }
    if (unit_bits == 16u) {
        uint w = read_u32_word(byte_offset_words + (unit_index >> 1u));
        return (w >> ((unit_index & 1u) * 16u)) & 0xffffu;
    }
    uint w = read_u32_word(byte_offset_words + (unit_index >> 2u));
    return (w >> ((unit_index & 3u) * 8u)) & 0xffu;
}
uint64_t read_storage_unit64(uint unit_index, uint unit_bits, uint byte_offset_words) {
    if (unit_bits == 64u) {
        uint lo = read_u32_word(byte_offset_words + unit_index * 2u);
        uint hi = read_u32_word(byte_offset_words + unit_index * 2u + 1u);
        return (uint64_t(hi) << 32) | uint64_t(lo);
    }
    return uint64_t(read_storage_unit(unit_index, unit_bits, byte_offset_words));
}
uint read_rank_at_bp(uint bp, uint bits_per_char, uint letters_per_unit,
                     uint unit_bits, uint byte_offset_words, uint char_mask) {
    uint unit_index = bp / letters_per_unit;
    uint in_unit = bp - unit_index * letters_per_unit;
    uint shift = in_unit * bits_per_char;
    uint64_t unit = read_storage_unit64(unit_index, unit_bits, byte_offset_words);
    return uint((unit >> shift) & uint64_t(char_mask));
}
uint64_t minimap_scramble(uint64_t key, uint64_t mask) {
    key = (~key + (key << 21)) & mask;
    key = key ^ (key >> 24);
    key = ((key + (key << 3)) + (key << 8)) & mask;
    key = key ^ (key >> 14);
    key = ((key + (key << 2)) + (key << 4)) & mask;
    key = key ^ (key >> 28);
    key = (key + (key << 31)) & mask;
    return key;
}
uint code_for_rank(uint rank) {
    if (pc.alphabet_kind == 0u) return rank;
    return rank_to_code[rank];
}
uint rc_code_for_rank(uint rank) {
    if (pc.alphabet_kind == 0u) return rank ^ 3u;
    return rank_to_rc_code[rank];
}

uint64_t hash_field_mask() {
    return pc.hash_bits >= 64u ? ~uint64_t(0)
                               : ((uint64_t(1) << pc.hash_bits) - uint64_t(1));
}

// Sketch element mirrors the scalar minimizer state machine element.
struct Sketch { uint64_t hash; uint pos; uint strand; uint is_max; };
Sketch max_sketch() { return Sketch(hash_field_mask(), 0u, 0u, 1u); }
bool sketch_leq(Sketch a, Sketch b) { return a.hash <= b.hash; }
bool sketch_geq(Sketch a, Sketch b) { return a.hash >= b.hash; }
bool sketch_match_diff(Sketch a, Sketch b) {
    return a.is_max == 0u && b.is_max == 0u && a.hash == b.hash
        && (a.pos != b.pos || a.strand != b.strand);
}

bool query_gate_accepts(uint64_t hash) {
    if (pc.query_gate_word_count == 0u) return true;
    uint bit = uint(hash & uint64_t(pc.query_gate_mask));
    uint word = bit >> 5u;
    // A malformed non-empty gate must never broaden the accepted target set.
    if (word >= pc.query_gate_word_count) return false;
    return (query_gate_words[word] & (1u << (bit & 31u))) != 0u;
}

		const uint SKETCH_CAND_MAX = 384u;            // small shared footprint -> high core occupancy
	const uint TARGET_EMIT_COMPLETE = 0x80000000u;
	const uint TARGET_EMIT_STATUS_CAPACITY = 1u;
	const uint TARGET_EMIT_STATUS_PARITY = 2u;
	uint g_seq_word_offset;
	uint g_local_count;
	uint g_out_base;
	uint g_owned_lo;
	uint g_owned_hi;
	uint g_expected_count;
	bool g_target_emit_failed;

void fail_target_emit(uint status) {
    // Failure-only telemetry. The successful deterministic target route has no
    // append/status atomics at all.
    atomicOr(counters[2], status);
    g_target_emit_failed = true;
}

void write_minimizer(uint dst, Sketch s) {
    // dst is tile-local; the global minimizer index is g_out_base + dst.
    uint gmin = g_out_base + dst;
    if (pc.word_kind == 0u) {
        uint raw = uint((s.hash << pc.hash_shift) | (uint64_t(s.pos) << 1) | uint64_t(s.strand));
        // u16 packs two minimizers per 32-bit word; the boundary word can be
        // shared by two parallel tiles, so the two half-word writes use 32-bit
        // atomics (the runtime zero-fills the output word range before emit).
        uint word = gmin >> 1u;
        uint shift = (gmin & 1u) * 16u;
        atomicAnd(out_words[word], ~(0xffffu << shift));
        atomicOr(out_words[word], (raw & 0xffffu) << shift);
    } else if (pc.word_kind == 1u) {
        uint base = gmin * pc.words32_per_minimizer;
        out_words[base] = uint((s.hash << pc.hash_shift) | (uint64_t(s.pos) << 1) | uint64_t(s.strand));
    } else if (pc.word_kind == 2u) {
        uint base = gmin * pc.words32_per_minimizer;
        uint64_t raw = (s.hash << pc.hash_shift) | (uint64_t(s.pos) << 1) | uint64_t(s.strand);
        out_words[base + 0u] = uint(raw);
        out_words[base + 1u] = uint(raw >> 32);
    } else {
        uint base = gmin * pc.words32_per_minimizer;
        uint64_t low = (uint64_t(s.pos) << 1) | uint64_t(s.strand);
        uint64_t high = (s.hash << 8) | uint64_t(pc.span_value & 0xffu);
        out_words[base + 0u] = uint(low);
        out_words[base + 1u] = uint(low >> 32);
        out_words[base + 2u] = uint(high);
        out_words[base + 3u] = uint(high >> 32);
    }
}

	bool owns_emit_position(TileC t, uint emit_pos) {
	    return emit_pos >= g_owned_lo && emit_pos < g_owned_hi;
	}

	uint replay_overlap_bp() {
	    uint trigger_span = pc.kmer_size + pc.window_size + 1u;
	    return min(trigger_span, SKETCH_CAND_MAX - 1u);
	}

	uint ownership_guard_bp() {
	    return 0u;
	}

void emit_one(TileC t, Sketch s, uint emit_pos) {
    if (s.is_max != 0u || !owns_emit_position(t, emit_pos)) return;
    if (!query_gate_accepts(s.hash)) return;
    uint dst = g_local_count;
    g_local_count = g_local_count + 1u;
    // emit_mode 2 is retained for the independent sorted-pair API. Target-index
    // construction uses mode 0 tile counts + prefix scan followed by mode 1;
    // it never enters this global append-atomic branch.
    if (pc.emit_mode == 2u) {
        uint gmin = atomicAdd(counters[0], 1u);
        if (gmin >= counters[1]) return;
        out_words[gmin * 2u + 0u] = uint(s.hash);
        out_words[gmin * 2u + 1u] = uint(s.hash >> 32);
        out_pair_vals[gmin] = (uint64_t(s.pos) << 1) | uint64_t(s.strand);
        return;
    }
    if (pc.emit_mode == 1u) {
        if (pc.emit_pairs == 1u) {
            // Target index path: every tile owns exactly the half-open prefix
            // range [offset, offset + counted). Never let a divergent second
            // pass overwrite a neighbouring tile or exceed exact capacity T.
            if (g_target_emit_failed || dst >= g_expected_count) {
                fail_target_emit(TARGET_EMIT_STATUS_PARITY);
                return;
            }
            uint64_t gmin64 = uint64_t(g_out_base) + uint64_t(dst);
            if (gmin64 >= uint64_t(counters[1])) {
                fail_target_emit(TARGET_EMIT_STATUS_CAPACITY);
                return;
            }
            uint gmin = uint(gmin64);
            out_words[gmin * 2u + 0u] = uint(s.hash);
            out_words[gmin * 2u + 1u] = uint(s.hash >> 32);
            out_pair_vals[gmin] = (uint64_t(s.pos) << 1) | uint64_t(s.strand);
        } else {
            write_minimizer(dst, s);
        }
    }
}

)GLSL"
R"GLSL(
// Rolling-hash ordered minimizer sketch (DOC0353). Each invocation owns ONE tile
// and runs minimap2's ordered window-minimum state machine over [replay_lo,
// loop_hi), emitting only for owned positions [tile_lo, tile_hi). The canonical
// k-mer hash is maintained INCREMENTALLY (forward/reverse rolled one base per
// step) instead of recomputed O(k) per position, and EVERY invocation runs the
// emit (no lane-0 serialization) — together these replace the prior
// parallel-hash-into-shared + single-lane-emit scheme that left 63/64 lanes idle
// and re-read each k-mer O(k) times. Output is bit-identical to
// `collect_minimizers_view`: `valid_since_reset` (forward-code validity) gates
// the state machine exactly as before, and a separate `valid_rc` counter
// suppresses the candidate while a reverse-complement-invalid base sits in the
// window (a no-op for dna4/dna5 where rc-invalid implies fwd-invalid).
void process_tile(uint tile_id) {
    TileC t = tiles[tile_id];
    g_seq_word_offset = t.seq_word_offset;
    g_owned_lo = t.tile_lo;
    g_owned_hi = tile_id + 1u >= pc.tile_count ? t.bp_count : t.tile_hi;
    uint loop_hi = tile_id + 1u >= pc.tile_count ? t.bp_count : min(t.bp_count, t.tile_hi);
    g_local_count = 0u;
    g_out_base = (pc.emit_mode == 1u) ? tile_offsets[tile_id] : 0u;
    g_expected_count = 0u;
    g_target_emit_failed = false;
    if (pc.emit_mode == 1u && pc.emit_pairs == 1u) {
        g_expected_count = tile_counts[tile_id];
        uint64_t range_end =
            uint64_t(g_out_base) + uint64_t(g_expected_count);
        if ((g_expected_count & TARGET_EMIT_COMPLETE) != 0u
            || range_end > uint64_t(counters[1])) {
            fail_target_emit(TARGET_EMIT_STATUS_CAPACITY);
        }
    }

    // Rolling canonical-kmer hash constants (constant per tile).
    uint step_bits = pc.alphabet_kind == 2u ? pc.bits_per_char : 2u;
    uint roll_bits = pc.kmer_size * step_bits;
    uint scramble_bits = pc.kmer_size * 2u;
    bool hash_ok = (roll_bits <= 64u) && (scramble_bits <= 64u);
    uint64_t roll_mask = (roll_bits >= 64u) ? ~uint64_t(0) : ((uint64_t(1) << roll_bits) - uint64_t(1));
    uint64_t scramble_mask = (scramble_bits >= 64u) ? ~uint64_t(0) : ((uint64_t(1) << scramble_bits) - uint64_t(1));
    uint shift_rc = step_bits * (pc.kmer_size - 1u);
    uint64_t hmask = hash_field_mask();

    uint64_t forward = uint64_t(0);
    uint64_t reverse = uint64_t(0);
    uint valid_rc = 0u;   // consecutive bases whose rc code < 4 (palindrome-safety vs candidate_at)

    Sketch ring[MAX_SPAN];
    uint buf_pos = 0u;
    uint valid_since_reset = 0u;
    bool init_mode = (t.replay_lo == 0u);
    Sketch current_min = max_sketch();
    uint current_min_pos = 0u;

    for (uint seq_pos = t.replay_lo; seq_pos < loop_hi; ++seq_pos) {
        bool is_init = init_mode;

        // --- rolling candidate ending at seq_pos (bit-exact with candidate_at) ---
        uint rank_here = read_rank_at_bp(seq_pos, pc.bits_per_char, pc.letters_per_unit,
                                         pc.unit_bits, g_seq_word_offset, pc.char_mask);
        uint code = code_for_rank(rank_here);
        uint rc = rc_code_for_rank(rank_here);
        bool base_valid = code < 4u;          // matches candidate_at base_valid (forward code only)
        if (code < 4u) {
            forward = ((forward << step_bits) | uint64_t(code)) & roll_mask;
        } else {
            forward = uint64_t(0);
        }
        if (rc < 4u) {
            reverse = (reverse >> step_bits) | (uint64_t(rc) << shift_rc);
            valid_rc = valid_rc + 1u;
        } else {
            reverse = uint64_t(0);
            valid_rc = 0u;
        }
        Sketch cand = max_sketch();

        Sketch pushed = max_sketch();
        if (!base_valid) {
            valid_since_reset = 0u;
        } else {
            valid_since_reset = valid_since_reset + 1u;
            if (hash_ok && valid_since_reset >= pc.kmer_size && valid_rc >= pc.kmer_size
                && forward != reverse) {
                uint64_t canonical = forward < reverse ? forward : reverse;
                uint64_t h = minimap_scramble(canonical, scramble_mask) & hmask;
                if (h != hmask) cand = Sketch(h, seq_pos, forward < reverse ? 0u : 1u, 0u);
            }
            if (valid_since_reset >= pc.kmer_size && cand.is_max == 0u) pushed = cand;
        }
        uint prev_pos = buf_pos;
        ring[buf_pos] = pushed;
        buf_pos = (buf_pos + 1u) % pc.window_size;

        if ((valid_since_reset == pc.window_size + pc.kmer_size - 1u && is_init)
            && current_min.is_max == 0u) {
            for (uint j = buf_pos; j < pc.window_size; ++j) {
                if (sketch_match_diff(current_min, ring[j])) emit_one(t, ring[j], seq_pos);
            }
            for (uint j = 0u; j < buf_pos; ++j) {
                if (sketch_match_diff(current_min, ring[j])) emit_one(t, ring[j], seq_pos);
            }
        }

        if (sketch_leq(pushed, current_min)) {
            if ((valid_since_reset >= pc.window_size + pc.kmer_size || !is_init)
                && current_min.is_max == 0u) {
                emit_one(t, current_min, seq_pos);
            }
            current_min_pos = prev_pos;
            current_min = pushed;
        } else if (prev_pos == current_min_pos) {
            if ((valid_since_reset >= pc.window_size + pc.kmer_size - 1u || !is_init)
                && current_min.is_max == 0u) {
                emit_one(t, current_min, seq_pos);
            }
            current_min = max_sketch();
            for (uint j = buf_pos; j < pc.window_size; ++j) {
                if (sketch_geq(current_min, ring[j])) { current_min = ring[j]; current_min_pos = j; }
            }
            for (uint j = 0u; j < buf_pos; ++j) {
                if (sketch_geq(current_min, ring[j])) { current_min = ring[j]; current_min_pos = j; }
            }
            if ((valid_since_reset >= pc.window_size + pc.kmer_size - 1u || !is_init)
                && current_min.is_max == 0u) {
                for (uint j = buf_pos; j < pc.window_size; ++j) {
                    if (sketch_match_diff(current_min, ring[j])) emit_one(t, ring[j], seq_pos);
                }
                for (uint j = 0u; j < buf_pos; ++j) {
                    if (sketch_match_diff(current_min, ring[j])) emit_one(t, ring[j], seq_pos);
                }
            }
        }

        if (init_mode && valid_since_reset >= pc.window_size + pc.kmer_size) init_mode = false;
    }

    // Final emit belongs to the sequence end; only the owning final tile does it.
    if (loop_hi >= t.bp_count && current_min.is_max == 0u) {
        emit_one(t, current_min, t.bp_count > 0u ? t.bp_count - 1u : 0u);
    }

    if (pc.emit_mode == 0u) {
        tile_counts[tile_id] = g_local_count;
    } else if (pc.emit_mode == 1u && pc.emit_pairs == 1u) {
        // One invocation owns each tile count word, so completion publication
        // needs no atomic. The host accepts the result only when every marker
        // agrees with the adjacent prefix offsets and exact T.
        if (!g_target_emit_failed
            && g_local_count == g_expected_count
            && (g_local_count & TARGET_EMIT_COMPLETE) == 0u) {
            tile_counts[tile_id] = TARGET_EMIT_COMPLETE | g_local_count;
        } else {
            fail_target_emit(TARGET_EMIT_STATUS_PARITY);
        }
    }
}

void main() {
    if (pc.window_size == 0u || pc.window_size > MAX_SPAN || pc.kmer_size == 0u) return;
    // One invocation per tile, grid-strided so the dispatch workgroup count is
    // free to be ceil(tile_count / local_size) (every lane active) yet still
    // correct for any larger dispatch.
    uint stride = gl_NumWorkGroups.x * gl_WorkGroupSize.x;
    for (uint tile_id = gl_GlobalInvocationID.x; tile_id < pc.tile_count; tile_id += stride) {
        process_tile(tile_id);
    }
}
)GLSL";

/// DOC0310 §11 — generic exclusive prefix scan over a `uint` array, block phase.
/// Each workgroup exclusive-scans its block into `out_prefix` and writes the
/// block total to `block_sums[wg]`. A second `add` pass adds the scanned block
/// offsets back. Two-level scan (block size = LOCAL_SIZE_X handled serially by
/// invocation 0 to stay portable on MoltenVK without subgroup ops).
constexpr std::string_view kPhaseCScanBlockBody = R"GLSL(
layout(local_size_x = LOCAL_SIZE_X, local_size_y = 1, local_size_z = 1) in;
layout(std430, set = 0, binding = 0) readonly buffer InData { uint in_data[]; };
layout(std430, set = 0, binding = 1) buffer OutData { uint out_prefix[]; };
layout(std430, set = 0, binding = 2) buffer BlockSums { uint block_sums[]; };
layout(push_constant) uniform Params {
    uint element_count;
    uint block_size;
    uint num_blocks;
    uint pass;
} pc;
void main() {
    if (gl_LocalInvocationID.x != 0u) return;
    // Grid-stride blocks let callers clamp dispatch X to the physical-device
    // limit without dropping logical scan blocks on XXL inputs.
    for (uint block = gl_WorkGroupID.x;
         block < pc.num_blocks;
         block += gl_NumWorkGroups.x) {
        uint lo = block * pc.block_size;
        uint hi = min(lo + pc.block_size, pc.element_count);
        uint running = 0u;
        for (uint i = lo; i < hi; ++i) {
            out_prefix[i] = running;
            running += in_data[i];
        }
        block_sums[block] = running;
    }
}
)GLSL";

/// DOC0310 §11 — prefix scan add phase. Invocation 0 of workgroup 0 exclusive-
/// scans `block_sums` in place, then every element adds its block's offset.
constexpr std::string_view kPhaseCScanAddBody = R"GLSL(
layout(local_size_x = LOCAL_SIZE_X, local_size_y = 1, local_size_z = 1) in;
layout(std430, set = 0, binding = 0) readonly buffer InData { uint in_data[]; };
layout(std430, set = 0, binding = 1) buffer OutData { uint out_prefix[]; };
layout(std430, set = 0, binding = 2) buffer BlockSums { uint block_sums[]; };
layout(push_constant) uniform Params {
    uint element_count;
    uint block_size;
    uint num_blocks;
    uint pass;
} pc;
void main() {
    uint gid = gl_GlobalInvocationID.x;
    // pass 0: single invocation exclusive-scans block_sums into block offsets.
    if (pc.pass == 0u) {
        if (gid != 0u) return;
        uint running = 0u;
        for (uint b = 0u; b < pc.num_blocks; ++b) {
            uint v = block_sums[b];
            block_sums[b] = running;
            running += v;
        }
        return;
    }
    // pass 1: add the scanned block offset to every element.
    uint stride = gl_NumWorkGroups.x * gl_WorkGroupSize.x;
    for (uint i = gid; i < pc.element_count; i += stride) {
        uint block = i / pc.block_size;
        out_prefix[i] += block_sums[block];
    }
}
)GLSL";

/// DOC0310 §14 — GPU seed count/emit. One workgroup per query record; local
/// invocations stride over that record's minimizers. `emit_mode==0` writes the
/// per-minimizer anchor count (clamped by `max_occ`) to `counts`; `emit_mode==1`
/// reads the exclusive `prefix` and emits `GpuAnchorWire` records plus their
/// compact `(strand, ref)` sort key.
constexpr std::string_view kPhaseCSeedBody = R"GLSL(
layout(local_size_x = LOCAL_SIZE_X, local_size_y = 1, local_size_z = 1) in;

struct GpuMinimizerSlot { uint64_t hash; uint pos_offset; uint pos_count; };
struct QRec { uint min_offset; uint min_count; uint query_len; uint flags; };
struct AnchorWire { uint64_t ref_position; uint64_t query_position; uint is_reverse; uint span_flags_segment; };

layout(std430, set = 0, binding = 0) readonly buffer MinWords { uint min_words[]; };
layout(std430, set = 0, binding = 1) readonly buffer Slots { GpuMinimizerSlot slots[]; };
layout(std430, set = 0, binding = 2) readonly buffer Positions { uint64_t positions[]; };
layout(std430, set = 0, binding = 3) readonly buffer QueryRecords { QRec qrecs[]; };
layout(std430, set = 0, binding = 4) buffer Counts { uint counts[]; };
layout(std430, set = 0, binding = 5) readonly buffer PrefixCounts { uint prefix[]; };
layout(std430, set = 0, binding = 6) buffer Anchors { AnchorWire anchors[]; };
layout(std430, set = 0, binding = 7) buffer AnchorKeys { uint64_t anchor_keys[]; };

layout(push_constant) uniform Params {
    uint words32_per_minimizer;
    uint word_kind;
    uint hash_shift;
    uint pos_bits;
    uint kmer_size;
    uint slot_mask;
    uint max_occ;
    uint emit_mode;
    uint record_count;
    uint bucket_count;   // >0 ⇒ Slots[] is a hash-sorted dense bucket array (binary search)
    uint target_pos_offset_lo;
    uint target_pos_offset_hi;
    uint target_ref_bits;
} pc;

uint64_t target_pos_offset() {
    return uint64_t(pc.target_pos_offset_lo) | (uint64_t(pc.target_pos_offset_hi) << 32);
}

uint64_t anchor_sort_key(uint rev, uint64_t ref_position) {
    if (pc.target_ref_bits >= 63u) {
        return (uint64_t(rev) << 63) | ref_position;
    }
    return (uint64_t(rev) << pc.target_ref_bits) | ref_position;
}

uint64_t read_min_hash(uint i) {
    if (pc.word_kind == 0u) {
        uint packed = min_words[i >> 1u];
        uint raw = (packed >> ((i & 1u) * 16u)) & 0xffffu;
        return uint64_t(raw) >> pc.hash_shift;
    }
    uint base = i * pc.words32_per_minimizer;
    if (pc.word_kind == 1u) return uint64_t(min_words[base]) >> pc.hash_shift;
    if (pc.word_kind == 2u) {
        uint64_t raw = uint64_t(min_words[base]) | (uint64_t(min_words[base + 1u]) << 32);
        return raw >> pc.hash_shift;
    }
    uint64_t high = uint64_t(min_words[base + 2u]) | (uint64_t(min_words[base + 3u]) << 32);
    return high >> 8;
}
uint64_t read_min_pos_strand(uint i) {
    if (pc.word_kind == 0u) {
        uint packed = min_words[i >> 1u];
        uint raw = (packed >> ((i & 1u) * 16u)) & 0xffffu;
        uint mask = (pc.pos_bits >= 15u) ? 0xffffu : ((1u << (pc.pos_bits + 1u)) - 1u);
        return uint64_t(raw & mask);
    }
    uint base = i * pc.words32_per_minimizer;
    if (pc.word_kind == 1u) {
        uint raw = min_words[base];
        uint mask = (pc.pos_bits >= 31u) ? 0xffffffffu : ((1u << (pc.pos_bits + 1u)) - 1u);
        return uint64_t(raw & mask);
    }
    if (pc.word_kind == 2u) {
        uint64_t raw = uint64_t(min_words[base]) | (uint64_t(min_words[base + 1u]) << 32);
        uint64_t mask = pc.pos_bits >= 63u ? ~uint64_t(0) : ((uint64_t(1) << (pc.pos_bits + 1u)) - uint64_t(1));
        return raw & mask;
    }
    return uint64_t(min_words[base]) | (uint64_t(min_words[base + 1u]) << 32);
}
uint64_t splitmix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15UL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9UL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebUL;
    return x ^ (x >> 31);
}
uint find_slot(uint64_t hash) {
    if (pc.bucket_count != 0u) {
        // Hash-sorted dense representation → binary search. It may be built
        // by the GPU index builder or selected explicitly by the bounded
        // legacy materializer; bucket_count attests the representation.
        uint lo = 0u;
        uint hi = pc.bucket_count;
        while (lo < hi) {
            uint mid = lo + ((hi - lo) >> 1u);
            if (slots[mid].hash < hash) lo = mid + 1u; else hi = mid;
        }
        if (lo < pc.bucket_count && slots[lo].hash == hash) return lo;
        return 0xffffffffu;
    }
    // Open-addressed representation → linear probe.
    uint p = uint(splitmix64(hash)) & pc.slot_mask;
    for (uint step = 0u; step <= pc.slot_mask; ++step) {
        GpuMinimizerSlot s = slots[p];
        if (s.pos_count == 0u) return 0xffffffffu;
        if (s.hash == hash) return p;
        p = (p + 1u) & pc.slot_mask;
    }
    return 0xffffffffu;
}

void main() {
    // Fast fused-MPF path: Runtime binds one QRec per problem. The QRec points
    // into a concatenated minimizer-word buffer, while counts/prefix are local
    // to the current problem.
    if (pc.record_count == 1u) {
        QRec q = qrecs[0];
        uint64_t reverse_base = uint64_t(q.query_len - 2u + pc.kmer_size);
        uint stride = gl_NumWorkGroups.x * gl_WorkGroupSize.x;
        for (uint local = gl_GlobalInvocationID.x; local < q.min_count; local += stride) {
            uint gi = q.min_offset + local;
            uint64_t hash = read_min_hash(gi);
            uint slot_idx = find_slot(hash);
            uint cnt = 0u;
            if (slot_idx != 0xffffffffu) {
                GpuMinimizerSlot slot = slots[slot_idx];
                if (slot.pos_count <= pc.max_occ) cnt = slot.pos_count;
            }
            if (pc.emit_mode == 0u) {
                counts[local] = cnt;
                continue;
            }
            if (cnt == 0u) continue;
            GpuMinimizerSlot slot = slots[slot_idx];
            uint dst = prefix[local];
            uint64_t q_encoded = read_min_pos_strand(gi);
            uint64_t q_pos = q_encoded >> 1;
            uint q_strand = uint(q_encoded & uint64_t(1));
            uint span_flags_segment = pc.kmer_size & 0xffu;
            for (uint j = 0u; j < cnt; ++j) {
                uint64_t target = positions[slot.pos_offset + j];
                uint t_strand = uint(target & uint64_t(1));
                uint rev = t_strand ^ q_strand;
                uint64_t query_position = rev != 0u ? (reverse_base - q_pos) : q_pos;
                uint64_t ref_position = (target >> 1) + target_pos_offset();
                anchors[dst + j].ref_position = ref_position;
                anchors[dst + j].query_position = query_position;
                anchors[dst + j].is_reverse = rev;
                anchors[dst + j].span_flags_segment = span_flags_segment;
                anchor_keys[dst + j] = anchor_sort_key(rev, ref_position);
            }
        }
        return;
    }

    // General multi-record fallback, kept for ABI completeness.
    uint rec = gl_WorkGroupID.x;
    if (rec >= pc.record_count) return;
    QRec q = qrecs[rec];
    uint64_t reverse_base = uint64_t(q.query_len - 2u + pc.kmer_size);
    for (uint local = gl_LocalInvocationID.x; local < q.min_count; local += gl_WorkGroupSize.x) {
        uint gi = q.min_offset + local;
        uint64_t hash = read_min_hash(gi);
        uint slot_idx = find_slot(hash);
        uint cnt = 0u;
        if (slot_idx != 0xffffffffu) {
            GpuMinimizerSlot slot = slots[slot_idx];
            if (slot.pos_count <= pc.max_occ) cnt = slot.pos_count;
        }
        if (pc.emit_mode == 0u) {
            counts[gi] = cnt;
            continue;
        }
        if (cnt == 0u) continue;
        GpuMinimizerSlot slot = slots[slot_idx];
        uint dst = prefix[gi];
        uint64_t q_encoded = read_min_pos_strand(gi);
        uint64_t q_pos = q_encoded >> 1;
        uint q_strand = uint(q_encoded & uint64_t(1));
        uint span_flags_segment = pc.kmer_size & 0xffu;
        for (uint j = 0u; j < cnt; ++j) {
            uint64_t target = positions[slot.pos_offset + j];
            uint t_strand = uint(target & uint64_t(1));
            uint rev = t_strand ^ q_strand;
            uint64_t query_position = rev != 0u ? (reverse_base - q_pos) : q_pos;
            uint64_t ref_position = (target >> 1) + target_pos_offset();
            anchors[dst + j].ref_position = ref_position;
            anchors[dst + j].query_position = query_position;
            anchors[dst + j].is_reverse = rev;
            anchors[dst + j].span_flags_segment = span_flags_segment;
            anchor_keys[dst + j] = anchor_sort_key(rev, ref_position);
        }
    }
}
)GLSL";

/// Direct persisted Format-5/ABI3 seed count/emit.  One workgroup owns one
/// query record and its lanes stride over query minimizers.  Each minimizer is
/// matched against the selected resident partition descriptors, binary-searches
/// the raw variable-width SoA hash arrays, and sums occurrences across every
/// position segment before applying `max_occ`.  Emit rereads the authenticated
/// prefix ranges and variable-width segment-local positions directly; it never
/// constructs a CPU-decoded slot table or a widened/globalized position array.
constexpr std::string_view kPhaseCShapeV5SeedBody = R"GLSL(
layout(local_size_x = LOCAL_SIZE_X, local_size_y = 1, local_size_z = 1) in;

const uint SHAPE_V5_ABI_VERSION = 3u;
const uint SHAPE_V5_REQUIRED_FLAGS = 7u;
const uint SHAPE_V5_STATUS_CONTRACT = 1u;
const uint SHAPE_V5_STATUS_QUERY = 2u;
const uint SHAPE_V5_STATUS_CONTROL = 4u;
const uint SHAPE_V5_STATUS_POSITION = 8u;
const uint SHAPE_V5_STATUS_EMISSION = 16u;

struct ShapeV5Partition {
    uint partition_id;
    uint directory_ordinal;
    uint position_segment;
    uint flags;
    uint64_t hash_byte_offset;
    uint64_t prefix_byte_offset;
    uint64_t count_byte_offset;
    uint64_t position_byte_offset;
    uint control_count;
    uint position_count;
    uint hash_width;
    uint offset_width;
    uint count_width;
    uint position_width;
    uint64_t low_hash;
    uint64_t high_hash;
    uint64_t segment_base_bp;
    uint64_t segment_end_bp;
};
struct ShapeV5QueryRecord {
    uint minimizer_offset;
    uint minimizer_count;
    uint query_len;
    uint flags;
};
struct AnchorWire {
    uint64_t ref_position;
    uint64_t query_position;
    uint is_reverse;
    uint span_flags_segment;
};

layout(std430, set = 0, binding = 0) readonly buffer MinWords {
    uint min_words[];
};
layout(std430, set = 0, binding = 1) readonly buffer Partitions {
    ShapeV5Partition partitions[];
};
layout(std430, set = 0, binding = 2) readonly buffer RawControls {
    uint control_words[];
};
layout(std430, set = 0, binding = 3) readonly buffer RawPositions {
    uint position_words[];
};
layout(std430, set = 0, binding = 4) readonly buffer QueryRecords {
    ShapeV5QueryRecord query_records[];
};
layout(std430, set = 0, binding = 5) buffer Counts {
    uint counts[];
};
layout(std430, set = 0, binding = 6) readonly buffer PrefixCounts {
    uint prefix[];
};
layout(std430, set = 0, binding = 7) buffer Anchors {
    AnchorWire anchors[];
};
layout(std430, set = 0, binding = 8) buffer AnchorKeys {
    uint64_t anchor_keys[];
};

layout(push_constant) uniform Params {
    uint abi_version;
    uint words32_per_minimizer;
    uint word_kind;
    uint hash_shift;
    uint pos_bits;
    uint kmer_size;
    uint partition_count;
    uint max_occ;
    uint emit_mode;
    uint record_count;
    uint target_ref_bits;
    uint query_minimizer_count;
    uint control_word_count;
    uint position_word_count;
    uint anchor_capacity;
    uint max_occ_cap_lo;
    uint max_occ_cap_hi;
    uint status_index;
} pc;

void shape_signal_status(uint status_bit) {
    if (pc.status_index < uint(counts.length())) {
        atomicOr(counts[pc.status_index], status_bit);
    }
}

bool shape_width_valid(uint width) {
    return width == 2u || width == 4u || width == 8u;
}

bool shape_aligned(uint64_t byte_offset, uint width) {
    return shape_width_valid(width)
        && (byte_offset & uint64_t(width - 1u)) == uint64_t(0);
}

bool shape_byte_range_valid(
    uint64_t byte_offset,
    uint width,
    uint word_count)
{
    if (!shape_aligned(byte_offset, width)) return false;
    uint64_t byte_limit = uint64_t(word_count) * uint64_t(4u);
    return byte_offset <= byte_limit
        && uint64_t(width) <= byte_limit - byte_offset;
}

bool shape_scalar_address(
    uint64_t base,
    uint index,
    uint width,
    out uint64_t byte_offset)
{
    uint64_t relative = uint64_t(index) * uint64_t(width);
    byte_offset = base + relative;
    return byte_offset >= base;
}

bool shape_read_control_scalar(
    uint64_t byte_offset,
    uint width,
    out uint64_t value)
{
    value = uint64_t(0);
    if (!shape_byte_range_valid(
            byte_offset, width, pc.control_word_count)) {
        return false;
    }
    uint word_index = uint(byte_offset >> 2u);
    uint shift = uint(byte_offset & uint64_t(3u)) * 8u;
    if (width == 2u) {
        value = uint64_t((control_words[word_index] >> shift) & 0xffffu);
        return true;
    }
    if (width == 4u) {
        value = uint64_t(control_words[word_index]);
        return true;
    }
    value = uint64_t(control_words[word_index])
        | (uint64_t(control_words[word_index + 1u]) << 32u);
    return true;
}

bool shape_read_position_scalar(
    uint64_t byte_offset,
    uint width,
    out uint64_t value)
{
    value = uint64_t(0);
    if (!shape_byte_range_valid(
            byte_offset, width, pc.position_word_count)) {
        return false;
    }
    uint word_index = uint(byte_offset >> 2u);
    uint shift = uint(byte_offset & uint64_t(3u)) * 8u;
    if (width == 2u) {
        value = uint64_t((position_words[word_index] >> shift) & 0xffffu);
        return true;
    }
    if (width == 4u) {
        value = uint64_t(position_words[word_index]);
        return true;
    }
    value = uint64_t(position_words[word_index])
        | (uint64_t(position_words[word_index + 1u]) << 32u);
    return true;
}

uint64_t shape_max_occ_cap() {
    return uint64_t(pc.max_occ_cap_lo)
        | (uint64_t(pc.max_occ_cap_hi) << 32u);
}

bool shape_partition_valid(ShapeV5Partition descriptor) {
    if (descriptor.flags != SHAPE_V5_REQUIRED_FLAGS
        || descriptor.control_count == 0u
        || descriptor.position_count == 0u
        || descriptor.low_hash > descriptor.high_hash
        || descriptor.segment_base_bp >= descriptor.segment_end_bp
        || !shape_width_valid(descriptor.hash_width)
        || !shape_width_valid(descriptor.offset_width)
        || !shape_width_valid(descriptor.count_width)
        || !shape_width_valid(descriptor.position_width)
        || !shape_aligned(
            descriptor.hash_byte_offset, descriptor.hash_width)
        || !shape_aligned(
            descriptor.prefix_byte_offset, descriptor.offset_width)
        || !shape_aligned(
            descriptor.count_byte_offset, descriptor.count_width)
        || !shape_aligned(
            descriptor.position_byte_offset, descriptor.position_width)) {
        return false;
    }
    uint position_bits = descriptor.position_width * 8u;
    uint64_t maximum_segment_span = uint64_t(1)
        << (position_bits - 1u);
    return descriptor.segment_end_bp - descriptor.segment_base_bp
        <= maximum_segment_span;
}

uint64_t shape_read_min_hash(uint index) {
    if (pc.word_kind == 0u) {
        uint packed = min_words[index >> 1u];
        uint raw = (packed >> ((index & 1u) * 16u)) & 0xffffu;
        return uint64_t(raw) >> pc.hash_shift;
    }
    uint base = index * pc.words32_per_minimizer;
    if (pc.word_kind == 1u) {
        return uint64_t(min_words[base]) >> pc.hash_shift;
    }
    if (pc.word_kind == 2u) {
        uint64_t raw = uint64_t(min_words[base])
            | (uint64_t(min_words[base + 1u]) << 32u);
        return raw >> pc.hash_shift;
    }
    uint64_t high = uint64_t(min_words[base + 2u])
        | (uint64_t(min_words[base + 3u]) << 32u);
    return high >> 8u;
}

uint64_t shape_read_min_position(uint index) {
    if (pc.word_kind == 0u) {
        uint packed = min_words[index >> 1u];
        uint raw = (packed >> ((index & 1u) * 16u)) & 0xffffu;
        uint mask = pc.pos_bits >= 15u
            ? 0xffffu : ((1u << (pc.pos_bits + 1u)) - 1u);
        return uint64_t(raw & mask);
    }
    uint base = index * pc.words32_per_minimizer;
    if (pc.word_kind == 1u) {
        uint mask = pc.pos_bits >= 31u
            ? 0xffffffffu : ((1u << (pc.pos_bits + 1u)) - 1u);
        return uint64_t(min_words[base] & mask);
    }
    if (pc.word_kind == 2u) {
        uint64_t raw = uint64_t(min_words[base])
            | (uint64_t(min_words[base + 1u]) << 32u);
        uint64_t mask = pc.pos_bits >= 63u
            ? ~uint64_t(0)
            : ((uint64_t(1) << (pc.pos_bits + 1u)) - uint64_t(1));
        return raw & mask;
    }
    return uint64_t(min_words[base])
        | (uint64_t(min_words[base + 1u]) << 32u);
}

bool shape_query_words_valid(uint index) {
    if (pc.word_kind > 3u || pc.words32_per_minimizer == 0u) return false;
    if (pc.word_kind == 0u) {
        return uint64_t(index >> 1u) < uint64_t(min_words.length());
    }
    uint required_words = pc.word_kind == 1u
        ? 1u : pc.word_kind == 2u ? 2u : 4u;
    if (pc.words32_per_minimizer < required_words) return false;
    uint64_t end = (uint64_t(index) + uint64_t(1u))
        * uint64_t(pc.words32_per_minimizer);
    return end <= uint64_t(min_words.length());
}

// Returns 0 for no hash in this partition, 1 for an admitted range, and 2 for
// malformed bytes.  The terminal prefix is authoritative; the saturated count
// is checked against it and the persisted generation cap before use.
uint shape_find_control(
    ShapeV5Partition descriptor,
    uint64_t wanted_hash,
    out uint64_t position_begin,
    out uint64_t position_end,
    out uint64_t saturated_count)
{
    position_begin = uint64_t(0);
    position_end = uint64_t(0);
    saturated_count = uint64_t(0);
    if (!shape_partition_valid(descriptor)) return 2u;
    if (wanted_hash < descriptor.low_hash
        || wanted_hash > descriptor.high_hash) {
        return 0u;
    }
    uint lo = 0u;
    uint hi = descriptor.control_count;
    while (lo < hi) {
        uint mid = lo + ((hi - lo) >> 1u);
        uint64_t address;
        uint64_t stored_hash;
        if (!shape_scalar_address(
                descriptor.hash_byte_offset,
                mid,
                descriptor.hash_width,
                address)
            || !shape_read_control_scalar(
                address, descriptor.hash_width, stored_hash)) {
            return 2u;
        }
        if (stored_hash < wanted_hash) lo = mid + 1u; else hi = mid;
    }
    if (lo >= descriptor.control_count) return 0u;
    uint64_t hash_address;
    uint64_t stored_hash;
    if (!shape_scalar_address(
            descriptor.hash_byte_offset,
            lo,
            descriptor.hash_width,
            hash_address)
        || !shape_read_control_scalar(
            hash_address, descriptor.hash_width, stored_hash)) {
        return 2u;
    }
    if (stored_hash != wanted_hash) return 0u;

    uint64_t begin_address;
    uint64_t end_address;
    uint64_t count_address;
    if (!shape_scalar_address(
            descriptor.prefix_byte_offset,
            lo,
            descriptor.offset_width,
            begin_address)
        || !shape_scalar_address(
            descriptor.prefix_byte_offset,
            lo + 1u,
            descriptor.offset_width,
            end_address)
        || !shape_scalar_address(
            descriptor.count_byte_offset,
            lo,
            descriptor.count_width,
            count_address)
        || !shape_read_control_scalar(
            begin_address, descriptor.offset_width, position_begin)
        || !shape_read_control_scalar(
            end_address, descriptor.offset_width, position_end)
        || !shape_read_control_scalar(
            count_address, descriptor.count_width, saturated_count)
        || position_begin >= position_end
        || position_end > uint64_t(descriptor.position_count)) {
        return 2u;
    }
    uint64_t exact_count = position_end - position_begin;
    uint64_t cap = shape_max_occ_cap();
    if (cap == ~uint64_t(0)) return 2u;
    uint64_t expected_saturated = exact_count > cap
        ? cap + uint64_t(1u) : exact_count;
    return saturated_count == expected_saturated ? 1u : 2u;
}

bool shape_count_hash(uint64_t hash, out uint accepted_count) {
    accepted_count = 0u;
    uint64_t total = uint64_t(0);
    uint64_t threshold = uint64_t(pc.max_occ);
    for (uint partition_index = 0u;
         partition_index < pc.partition_count;
         ++partition_index) {
        uint64_t position_begin;
        uint64_t position_end;
        uint64_t saturated_count;
        uint status = shape_find_control(
            partitions[partition_index],
            hash,
            position_begin,
            position_end,
            saturated_count);
        if (status == 2u) return false;
        if (status == 0u) continue;
        if (saturated_count > threshold) {
            accepted_count = 0u;
            return true;
        }
        uint64_t exact_count = position_end - position_begin;
        if (total > threshold || exact_count > threshold - total) {
            accepted_count = 0u;
            return true;
        }
        total += exact_count;
    }
    accepted_count = uint(total);
    return true;
}

uint64_t shape_anchor_sort_key(uint reverse, uint64_t ref_position) {
    if (pc.target_ref_bits >= 63u) {
        return (uint64_t(reverse) << 63u) | ref_position;
    }
    return (uint64_t(reverse) << pc.target_ref_bits) | ref_position;
}

bool shape_global_contract_valid() {
    uint64_t cap = shape_max_occ_cap();
    return pc.abi_version == SHAPE_V5_ABI_VERSION
        && pc.emit_mode <= 1u
        && pc.kmer_size != 0u
        && pc.kmer_size <= 255u
        && pc.hash_shift < 64u
        && pc.pos_bits < 64u
        && pc.target_ref_bits <= 63u
        && cap != ~uint64_t(0)
        && uint64_t(pc.max_occ) <= cap
        && pc.partition_count != 0u
        && pc.partition_count <= uint(partitions.length())
        && pc.record_count == 1u
        && uint(query_records.length()) >= 1u
        && pc.query_minimizer_count != 0u
        && pc.status_index != 0u
        && pc.status_index < uint(counts.length())
        && pc.status_index == uint(prefix.length())
        && pc.control_word_count <= uint(control_words.length())
        && pc.position_word_count <= uint(position_words.length())
        && pc.anchor_capacity <= uint(anchors.length())
        && pc.anchor_capacity <= uint(anchor_keys.length());
}

void main() {
    uint record_index = gl_WorkGroupID.x;
    bool contract_valid = shape_global_contract_valid();
    if (!contract_valid || record_index != 0u) {
        shape_signal_status(SHAPE_V5_STATUS_CONTRACT);
        return;
    }
    ShapeV5QueryRecord record = query_records[record_index];
    uint64_t query_end = uint64_t(record.minimizer_offset)
        + uint64_t(record.minimizer_count);
    bool record_valid = query_end <= uint64_t(pc.query_minimizer_count)
        && record.minimizer_count == pc.status_index
        && record.query_len >= pc.kmer_size
        && record.flags == 0u;
    if (!record_valid) {
        shape_signal_status(SHAPE_V5_STATUS_QUERY);
        return;
    }

    for (uint local_index = gl_LocalInvocationID.x;
         local_index < record.minimizer_count;
         local_index += gl_WorkGroupSize.x) {
        uint minimizer_index = record.minimizer_offset + local_index;
        if (pc.emit_mode == 0u) counts[local_index] = 0u;
        if (minimizer_index >= pc.query_minimizer_count
            || !shape_query_words_valid(minimizer_index)) {
            shape_signal_status(SHAPE_V5_STATUS_QUERY);
            continue;
        }

        uint64_t hash = shape_read_min_hash(minimizer_index);
        uint accepted_count = 0u;
        if (!shape_count_hash(hash, accepted_count)) {
            shape_signal_status(SHAPE_V5_STATUS_CONTROL);
            continue;
        }
        if (pc.emit_mode == 0u) {
            counts[local_index] = accepted_count;
            continue;
        }
        if (counts[local_index] != accepted_count) {
            shape_signal_status(SHAPE_V5_STATUS_EMISSION);
            continue;
        }
        if (accepted_count == 0u) continue;

        uint64_t destination = uint64_t(prefix[local_index]);
        if (destination > uint64_t(pc.anchor_capacity)
            || uint64_t(accepted_count)
                > uint64_t(pc.anchor_capacity) - destination) {
            shape_signal_status(SHAPE_V5_STATUS_EMISSION);
            continue;
        }
        uint64_t query_encoded = shape_read_min_position(minimizer_index);
        uint64_t query_bp = query_encoded >> 1u;
        uint query_strand = uint(query_encoded & uint64_t(1u));
        if (query_bp >= uint64_t(record.query_len)
            || query_bp + uint64_t(1u) < uint64_t(pc.kmer_size)) {
            shape_signal_status(SHAPE_V5_STATUS_QUERY);
            continue;
        }
        uint64_t reverse_base = uint64_t(record.query_len)
            + uint64_t(pc.kmer_size) - uint64_t(2u);
        uint emitted = 0u;
        bool emission_valid = true;
        for (uint partition_index = 0u;
             emission_valid && partition_index < pc.partition_count;
             ++partition_index) {
            ShapeV5Partition descriptor = partitions[partition_index];
            uint64_t position_begin;
            uint64_t position_end;
            uint64_t saturated_count;
            uint status = shape_find_control(
                descriptor,
                hash,
                position_begin,
                position_end,
                saturated_count);
            if (status == 2u) {
                shape_signal_status(SHAPE_V5_STATUS_CONTROL);
                emission_valid = false;
                break;
            }
            if (status == 0u) continue;
            for (uint64_t position_index = position_begin;
                 position_index < position_end;
                 ++position_index) {
                uint64_t position_address;
                uint64_t target_encoded;
                if (!shape_scalar_address(
                        descriptor.position_byte_offset,
                        uint(position_index),
                        descriptor.position_width,
                        position_address)
                    || !shape_read_position_scalar(
                        position_address,
                        descriptor.position_width,
                        target_encoded)) {
                    shape_signal_status(SHAPE_V5_STATUS_POSITION);
                    emission_valid = false;
                    break;
                }
                uint64_t local_bp = target_encoded >> 1u;
                uint64_t segment_span = descriptor.segment_end_bp
                    - descriptor.segment_base_bp;
                if (local_bp >= segment_span || emitted >= accepted_count) {
                    shape_signal_status(
                        local_bp >= segment_span
                            ? SHAPE_V5_STATUS_POSITION
                            : SHAPE_V5_STATUS_EMISSION);
                    emission_valid = false;
                    break;
                }
                uint target_strand = uint(target_encoded & uint64_t(1u));
                uint reverse = target_strand ^ query_strand;
                uint64_t ref_position = descriptor.segment_base_bp + local_bp;
                uint64_t query_position = reverse != 0u
                    ? reverse_base - query_bp : query_bp;
                uint64_t output_index = destination + uint64_t(emitted);
                anchors[uint(output_index)].ref_position = ref_position;
                anchors[uint(output_index)].query_position = query_position;
                anchors[uint(output_index)].is_reverse = reverse;
                anchors[uint(output_index)].span_flags_segment =
                    pc.kmer_size & 0xffu;
                anchor_keys[uint(output_index)] =
                    shape_anchor_sort_key(reverse, ref_position);
                ++emitted;
            }
        }
        // A validated immutable resident generation makes this equality
        // deterministic between count and emit.  No CPU/SIMD repair path is
        // provided if the resident bytes change or fail validation.
        if (!emission_valid || emitted != accepted_count) {
            shape_signal_status(SHAPE_V5_STATUS_EMISSION);
        }
    }
}
)GLSL";

// DOC0371 persisted-index foundation. One workgroup deliberately owns the
// complete target-wide query union: all lanes mark a global bitset, synchronize,
// then lane zero compacts it in stable directory order. This avoids exposing a
// host query-hash vector and gives the later PG reader only opaque ordinals.
// The authoritative directory is the header-free revision-3 96-byte entry
// stream (24 little-endian uint words per entry).
constexpr std::string_view kPhaseCPersistedDirectorySelectBody = R"GLSL(
layout(local_size_x = LOCAL_SIZE_X, local_size_y = 1, local_size_z = 1) in;

layout(std430, set = 0, binding = 0) readonly buffer MinWords { uint min_words[]; };
layout(std430, set = 0, binding = 1) readonly buffer DirectoryWords { uint directory_words[]; };
layout(std430, set = 0, binding = 2) buffer SelectedBits { uint selected_bits[]; };
layout(std430, set = 0, binding = 3) buffer SelectedOrdinals { uint selected_ordinals[]; };

layout(push_constant) uniform Params {
    uint minimizer_count;
    uint minimizer_word_kind;
    uint minimizer_words32_per_record;
    uint hash_shift;
    uint directory_entry_count;
    uint reserved_directory;
    uint bitset_word_count;
    uint reserved;
} pc;

uint64_t read_min_hash(uint i) {
    if (pc.minimizer_word_kind == 0u) {
        uint packed = min_words[i >> 1u];
        uint raw = (packed >> ((i & 1u) * 16u)) & 0xffffu;
        return uint64_t(raw) >> pc.hash_shift;
    }
    uint base = i * pc.minimizer_words32_per_record;
    if (pc.minimizer_word_kind == 1u) {
        return uint64_t(min_words[base]) >> pc.hash_shift;
    }
    if (pc.minimizer_word_kind == 2u) {
        uint64_t raw = uint64_t(min_words[base])
            | (uint64_t(min_words[base + 1u]) << 32);
        return raw >> pc.hash_shift;
    }
    uint64_t high = uint64_t(min_words[base + 2u])
        | (uint64_t(min_words[base + 3u]) << 32);
    return high >> 8;
}

uint64_t directory_u64(uint ordinal, uint word_offset) {
    uint base = ordinal * 24u + word_offset;
    return uint64_t(directory_words[base])
        | (uint64_t(directory_words[base + 1u]) << 32);
}

void main() {
    for (uint i = gl_LocalInvocationID.x; i < pc.minimizer_count;
        i += gl_WorkGroupSize.x) {
        uint64_t hash = read_min_hash(i);
        // Scan the complete authoritative directory. Hash ranges repeat once
        // per persisted position core, so a caller-supplied segment filter
        // would silently omit valid target anchors.
        for (uint ordinal = 0u; ordinal < pc.directory_entry_count; ++ordinal) {
            uint64_t low_hash = directory_u64(ordinal, 2u);
            uint64_t high_hash = directory_u64(ordinal, 4u);
            if (low_hash <= hash && hash <= high_hash) {
                atomicOr(selected_bits[ordinal >> 5u], 1u << (ordinal & 31u));
            }
        }
    }
    memoryBarrierBuffer();
    barrier();

    if (gl_LocalInvocationID.x == 0u) {
        uint count = 0u;
        for (uint ordinal = 0u; ordinal < pc.directory_entry_count; ++ordinal) {
            uint present = selected_bits[ordinal >> 5u] >> (ordinal & 31u);
            if ((present & 1u) != 0u) {
                selected_ordinals[1u + count] = ordinal;
                ++count;
            }
        }
        selected_ordinals[0] = count;
    }
}
)GLSL";

// Inactive format-4 ABI2 shader family. The core kernels share one exact
// std430 contract. Controls and positions are the authenticated persisted
// payload itself: a 12-byte control array plus 4-byte segment-local positions.
// Production dispatch is intentionally absent until a PG resident authority
// and hardware-fence execution proof are wired atomically.
constexpr std::string_view kPhaseCGrouped32Abi2Common = R"GLSL(
layout(local_size_x = LOCAL_SIZE_X, local_size_y = 1, local_size_z = 1) in;

struct Grouped32Partition {
    uint partition_id;
    uint directory_ordinal;
    uint position_segment;
    uint control_offset;
    uint control_count;
    uint position_offset;
    uint position_count;
    uint low_hash;
    uint high_hash;
    uint flags;
    uint64_t segment_base_bp;
    uint64_t segment_end_bp;
    uint reserved0;
    uint reserved1;
};

struct Grouped32Control {
    uint hash;
    uint position_offset;
    uint position_count;
};

struct Grouped32QueryMinimizer {
    uint hash;
    uint query_bp_strand;
    uint occurrence_index;
    uint query_record_index;
};

struct Grouped32QueryRecord {
    uint minimizer_offset;
    uint minimizer_count;
    uint query_length;
    uint query_state_index;
    uint occurrence_offset;
    uint anchor_base;
    uint anchor_capacity;
    uint flags;
    uint64_t batch_join_ordinal;
    uint reserved0;
    uint reserved1;
};

struct Grouped32OccurrenceState {
    uint global_count;
    uint accepted_count;
    uint anchor_offset;
    uint emitted_count;
};

struct Grouped32QueryState {
    uint anchor_count;
    uint rejected_minimizers;
    uint completed_minimizers;
    uint status;
};

struct Grouped32GlobalAnchor {
    uint64_t ref_position;
    uint64_t query_position;
    uint is_reverse;
    uint span_flags_segment;
};

struct Grouped32AnchorRadixKey {
    uint64_t ref_position;
    uint is_reverse;
    uint query_state_index;
};

struct Grouped32SortSegment {
    uint anchor_begin;
    uint anchor_count;
    uint query_state_index;
    uint flags;
    uint64_t batch_join_ordinal;
};

struct Grouped32ValidationState {
    uint status;
    uint failed_partitions;
    uint controls_checked;
    uint positions_checked;
    uint completed_workgroups;
    uint expected_workgroups;
    uint completion;
    uint reserved;
};

struct Grouped32QueryAdapterState {
    uint status;
    uint completed_records;
    uint expected_records;
    uint total_minimizers;
    uint max_query_minimizers;
    uint completion;
    uint reserved0;
    uint reserved1;
};

struct Grouped32DispatchIndirectCommand {
    uint group_count_x;
    uint group_count_y;
    uint group_count_z;
};

layout(std430, set = 0, binding = 0) readonly buffer Partitions {
    Grouped32Partition partitions[];
};
layout(std430, set = 0, binding = 1) readonly buffer Controls {
    Grouped32Control controls[];
};
layout(std430, set = 0, binding = 2) readonly buffer Positions {
    uint local_positions[];
};
layout(std430, set = 0, binding = 3) readonly buffer QueryRecords {
    Grouped32QueryRecord query_records[];
};
layout(std430, set = 0, binding = 4) readonly buffer QueryMinimizers {
    Grouped32QueryMinimizer query_minimizers[];
};
layout(std430, set = 0, binding = 5) buffer OccurrenceStates {
    Grouped32OccurrenceState occurrence_states[];
};
layout(std430, set = 0, binding = 6) buffer QueryStates {
    Grouped32QueryState query_states[];
};
layout(std430, set = 0, binding = 7) buffer GlobalAnchors {
    Grouped32GlobalAnchor global_anchors[];
};
layout(std430, set = 0, binding = 8) buffer ValidationStates {
    Grouped32ValidationState validation_states[];
};
layout(std430, set = 0, binding = 9) buffer IndirectCommands {
    Grouped32DispatchIndirectCommand indirect_commands[];
};
layout(std430, set = 0, binding = 10) buffer AnchorRadixKeys {
    Grouped32AnchorRadixKey anchor_radix_keys[];
};
layout(std430, set = 0, binding = 11) buffer SortSegments {
    Grouped32SortSegment sort_segments[];
};
layout(std430, set = 0, binding = 12) buffer AdapterStates {
    Grouped32QueryAdapterState adapter_states[];
};

layout(push_constant) uniform Params {
    uint partition_count;
    uint query_record_count;
    uint query_minimizer_capacity;
    uint occurrence_count;
    uint control_count;
    uint position_count;
    uint anchor_capacity;
    uint max_occ;
    uint kmer_size;
    uint validation_index;
    uint validation_state_count;
    uint indirect_base;
    uint indirect_command_count;
    uint max_partition_groups_y;
    uint max_workgroups_x;
    uint flags;
    uint adapter_state_index;
    uint adapter_state_count;
} pc;

const uint G32_DESCRIPTOR_BOUNDS = 1u << 0u;
const uint G32_DESCRIPTOR_IDENTITY = 1u << 1u;
const uint G32_SEGMENT_BOUNDS = 1u << 2u;
const uint G32_CONTROL_ORDER = 1u << 3u;
const uint G32_CONTROL_POSITIONS = 1u << 4u;
const uint G32_POSITION_BOUNDS = 1u << 5u;
const uint G32_POSITION_ORDER = 1u << 6u;
const uint G32_QUERY_BOUNDS = 1u << 7u;
const uint G32_ANCHOR_CAPACITY = 1u << 8u;
const uint G32_EMITTED_COUNT = 1u << 9u;
const uint G32_OCCURRENCE_OVERFLOW = 1u << 10u;
const uint G32_VALIDATION_GATE = 1u << 11u;
const uint G32_ADAPTER_STATE = 1u << 12u;
const uint G32_INDIRECT_BOUNDS = 1u << 13u;
const uint G32_SORT_OWNERSHIP = 1u << 14u;

const uint G32_COMPLETION_PENDING = 0u;
const uint G32_COMPLETION_ACCEPTED = 1u;
const uint G32_COMPLETION_REJECTED = 2u;

const uint G32_STAGE_VALIDATE = 0u;
const uint G32_STAGE_ACCUMULATE = 1u;
const uint G32_STAGE_FINALIZE = 2u;
const uint G32_STAGE_ALLOCATE = 3u;
const uint G32_STAGE_SORT_SEGMENTS = 4u;
const uint G32_STAGE_EMIT = 5u;
const uint G32_STAGE_RADIX = 6u;
const uint G32_STAGE_COUNT = 7u;

bool grouped32_validation_addressable() {
    return pc.validation_index < uint(validation_states.length());
}

bool grouped32_validation_bound() {
    return pc.validation_state_count <= uint(validation_states.length())
        && pc.validation_index < pc.validation_state_count;
}

bool grouped32_adapter_addressable() {
    return pc.adapter_state_index < uint(adapter_states.length());
}

bool grouped32_adapter_bound() {
    return pc.adapter_state_count <= uint(adapter_states.length())
        && pc.adapter_state_index < pc.adapter_state_count;
}

bool grouped32_core_buffers_bound() {
    return pc.partition_count <= uint(partitions.length())
        && pc.control_count <= uint(controls.length())
        && pc.position_count <= uint(local_positions.length())
        && pc.query_record_count <= uint(query_records.length())
        && pc.query_record_count <= uint(query_states.length())
        && pc.query_record_count <= uint(sort_segments.length())
        && pc.query_minimizer_capacity <= uint(query_minimizers.length())
        && pc.occurrence_count <= uint(occurrence_states.length())
        && pc.anchor_capacity <= uint(global_anchors.length())
        && pc.anchor_capacity <= uint(anchor_radix_keys.length())
        && pc.validation_state_count <= uint(validation_states.length())
        && pc.indirect_command_count <= uint(indirect_commands.length())
        && pc.adapter_state_count <= uint(adapter_states.length());
}

uint grouped32_query_minimizer_count() {
    if (!grouped32_core_buffers_bound() || !grouped32_adapter_bound()) return 0u;
    Grouped32QueryAdapterState state = adapter_states[pc.adapter_state_index];
    if (state.completion != G32_COMPLETION_ACCEPTED || state.status != 0u
        || state.total_minimizers > pc.query_minimizer_capacity
        || state.total_minimizers > pc.occurrence_count) {
        return 0u;
    }
    return state.total_minimizers;
}

bool grouped32_indirect_stage_bound(uint stage) {
    return stage < G32_STAGE_COUNT
        && pc.indirect_command_count <= uint(indirect_commands.length())
        && uint64_t(pc.indirect_base) + uint64_t(stage)
            < uint64_t(pc.indirect_command_count);
}

void grouped32_fail(uint status_bit) {
    if (grouped32_validation_addressable()) {
        atomicOr(validation_states[pc.validation_index].status, status_bit);
    }
}

bool grouped32_validation_gate_open() {
    if (!grouped32_core_buffers_bound()) {
        grouped32_fail(G32_DESCRIPTOR_BOUNDS);
        return false;
    }
    if (!grouped32_validation_bound()) {
        grouped32_fail(G32_VALIDATION_GATE);
        return false;
    }
    if (!grouped32_adapter_bound()) {
        grouped32_fail(G32_ADAPTER_STATE);
        return false;
    }
    Grouped32ValidationState validation =
        validation_states[pc.validation_index];
    Grouped32QueryAdapterState adapter =
        adapter_states[pc.adapter_state_index];
    return validation.completion == G32_COMPLETION_ACCEPTED
        && validation.status == 0u
        && adapter.completion == G32_COMPLETION_ACCEPTED
        && adapter.status == 0u;
}

uint grouped32_ceil_div(uint value, uint divisor) {
    return divisor == 0u ? 0u : value / divisor + (value % divisor != 0u ? 1u : 0u);
}

void grouped32_zero_command(uint stage) {
    if (!grouped32_indirect_stage_bound(stage)) return;
    uint command = pc.indirect_base + stage;
    indirect_commands[command].group_count_x = 0u;
    indirect_commands[command].group_count_y = 0u;
    indirect_commands[command].group_count_z = 0u;
}

void grouped32_write_command(uint stage, uint x, uint y, uint z) {
    if (!grouped32_indirect_stage_bound(stage)) {
        grouped32_fail(G32_INDIRECT_BOUNDS);
        return;
    }
    uint command = pc.indirect_base + stage;
    indirect_commands[command].group_count_x = x;
    indirect_commands[command].group_count_y = y;
    indirect_commands[command].group_count_z = z;
}

bool grouped32_partition_bounds_safe(Grouped32Partition p) {
    return uint64_t(p.control_offset) + uint64_t(p.control_count)
            <= uint64_t(pc.control_count)
        && uint64_t(p.position_offset) + uint64_t(p.position_count)
            <= uint64_t(pc.position_count);
}

bool grouped32_query_record_safe(
    uint record_index,
    Grouped32QueryRecord record)
{
    uint query_minimizer_count = grouped32_query_minimizer_count();
    uint64_t minimizer_end = uint64_t(record.minimizer_offset)
        + uint64_t(record.minimizer_count);
    uint64_t occurrence_end = uint64_t(record.occurrence_offset)
        + uint64_t(record.minimizer_count);
    uint64_t anchor_end = uint64_t(record.anchor_base)
        + uint64_t(record.anchor_capacity);
    if (record_index >= pc.query_record_count
        || record.query_state_index != record_index
        || record.query_state_index >= pc.query_record_count
        || record.occurrence_offset != record.minimizer_offset
        || minimizer_end > uint64_t(query_minimizer_count)
        || occurrence_end > uint64_t(pc.occurrence_count)
        || anchor_end > uint64_t(pc.anchor_capacity)
        || record.query_length > 0x80000000u
        || (record.minimizer_count != 0u
            && (pc.kmer_size != 15u || record.query_length < 15u))
        || record.flags != 0u || record.reserved0 != 0u
        || record.reserved1 != 0u) {
        grouped32_fail(G32_QUERY_BOUNDS);
        return false;
    }
    return true;
}

bool grouped32_query_minimizer_safe(
    uint minimizer_index,
    Grouped32QueryMinimizer minimizer,
    out Grouped32QueryRecord record)
{
    if (minimizer.query_record_index >= pc.query_record_count
        || minimizer.occurrence_index >= pc.occurrence_count) {
        grouped32_fail(G32_QUERY_BOUNDS);
        return false;
    }
    record = query_records[minimizer.query_record_index];
    if (!grouped32_query_record_safe(
            minimizer.query_record_index, record)
        || minimizer_index < record.minimizer_offset
        || uint64_t(minimizer_index)
            >= uint64_t(record.minimizer_offset)
                + uint64_t(record.minimizer_count)
        || minimizer.occurrence_index != minimizer_index
        || pc.kmer_size != 15u
        || record.query_length > 0x80000000u
        || record.query_length < pc.kmer_size
        || (minimizer.query_bp_strand >> 1u) >= record.query_length
        || (minimizer.query_bp_strand >> 1u) + 1u < pc.kmer_size) {
        grouped32_fail(G32_QUERY_BOUNDS);
        return false;
    }
    return true;
}

bool grouped32_find_control(
    Grouped32Partition part,
    uint hash,
    out Grouped32Control found)
{
    uint lo = 0u;
    uint hi = part.control_count;
    while (lo < hi) {
        uint mid = lo + ((hi - lo) >> 1u);
        Grouped32Control control = controls[part.control_offset + mid];
        if (control.hash < hash) lo = mid + 1u; else hi = mid;
    }
    if (lo < part.control_count) {
        Grouped32Control control = controls[part.control_offset + lo];
        if (control.hash == hash) {
            found = control;
            return true;
        }
    }
    return false;
}
)GLSL";

constexpr std::string_view kPhaseCGrouped32ClearStateMain = R"GLSL(
void main() {
    bool validation_addressable = grouped32_validation_addressable();
    bool adapter_addressable = grouped32_adapter_addressable();
    if (!validation_addressable || !adapter_addressable) return;
    if (!grouped32_core_buffers_bound()
        || !grouped32_validation_bound() || !grouped32_adapter_bound()
        || pc.kmer_size != 15u || pc.flags != 0u
        || uint64_t(pc.indirect_base) + uint64_t(G32_STAGE_COUNT)
            > uint64_t(pc.indirect_command_count)) {
        if (gl_GlobalInvocationID.x == 0u) {
            validation_states[pc.validation_index].status =
                G32_DESCRIPTOR_BOUNDS;
            validation_states[pc.validation_index].completion =
                G32_COMPLETION_REJECTED;
            adapter_states[pc.adapter_state_index].status =
                G32_DESCRIPTOR_BOUNDS;
            adapter_states[pc.adapter_state_index].completion =
                G32_COMPLETION_REJECTED;
        }
        return;
    }

    uint stride = gl_NumWorkGroups.x * gl_WorkGroupSize.x;
    for (uint index = gl_GlobalInvocationID.x;
         index < pc.occurrence_count; index += stride) {
        occurrence_states[index].global_count = 0u;
        occurrence_states[index].accepted_count = 0u;
        occurrence_states[index].anchor_offset = 0u;
        occurrence_states[index].emitted_count = 0u;
    }
    for (uint index = gl_GlobalInvocationID.x;
         index < pc.query_record_count; index += stride) {
        query_states[index].anchor_count = 0u;
        query_states[index].rejected_minimizers = 0u;
        query_states[index].completed_minimizers = 0u;
        query_states[index].status = 0u;
        sort_segments[index].anchor_begin = 0u;
        sort_segments[index].anchor_count = 0u;
        sort_segments[index].query_state_index = index;
        sort_segments[index].flags = 0u;
        sort_segments[index].batch_join_ordinal = uint64_t(0);
    }
    for (uint stage = gl_GlobalInvocationID.x;
         stage < G32_STAGE_COUNT; stage += stride) {
        uint command = pc.indirect_base + stage;
        indirect_commands[command].group_count_x = 0u;
        indirect_commands[command].group_count_y = 0u;
        indirect_commands[command].group_count_z = 0u;
    }
    memoryBarrierBuffer();
    barrier();
    if (gl_GlobalInvocationID.x == 0u) {
        Grouped32QueryAdapterState adapter;
        adapter.status = 0u;
        adapter.completed_records = 0u;
        adapter.expected_records = pc.query_record_count;
        adapter.total_minimizers = 0u;
        adapter.max_query_minimizers = 0u;
        adapter.completion = G32_COMPLETION_PENDING;
        adapter.reserved0 = 0u;
        adapter.reserved1 = 0u;
        adapter_states[pc.adapter_state_index] = adapter;

        Grouped32ValidationState validation;
        validation.status = 0u;
        validation.failed_partitions = 0u;
        validation.controls_checked = 0u;
        validation.positions_checked = 0u;
        validation.completed_workgroups = 0u;
        validation.expected_workgroups = 0u;
        validation.completion = G32_COMPLETION_PENDING;
        validation.reserved = 0u;
        validation_states[pc.validation_index] = validation;
    }
}
)GLSL";

constexpr std::string_view kPhaseCGrouped32FinalizeValidationMain = R"GLSL(
uint grouped32_capped_groups(uint count) {
    uint groups = grouped32_ceil_div(count, gl_WorkGroupSize.x);
    return min(groups, pc.max_workgroups_x);
}

void grouped32_publish_validated_commands(uint query_minimizer_count) {
    uint query_groups = grouped32_capped_groups(query_minimizer_count);
    uint record_groups = grouped32_capped_groups(pc.query_record_count);
    uint partition_groups = min(pc.partition_count, pc.max_partition_groups_y);
    grouped32_write_command(
        G32_STAGE_ACCUMULATE, query_groups,
        query_groups == 0u ? 0u : partition_groups, 1u);
    grouped32_write_command(G32_STAGE_FINALIZE, query_groups, 1u, 1u);
    grouped32_write_command(G32_STAGE_ALLOCATE, record_groups, 1u, 1u);
    grouped32_write_command(G32_STAGE_SORT_SEGMENTS, record_groups, 1u, 1u);
    grouped32_write_command(
        G32_STAGE_EMIT, query_groups,
        query_groups == 0u ? 0u : partition_groups, 1u);
    grouped32_zero_command(G32_STAGE_RADIX);
}

void main() {
    if (gl_GlobalInvocationID.x != 0u
        || !grouped32_validation_addressable()) return;
    if (!grouped32_core_buffers_bound()
        || !grouped32_validation_bound() || !grouped32_adapter_bound()) {
        grouped32_fail(G32_DESCRIPTOR_BOUNDS);
        validation_states[pc.validation_index].completion =
            G32_COMPLETION_REJECTED;
        return;
    }
    for (uint stage = 0u; stage < G32_STAGE_COUNT; ++stage) {
        grouped32_zero_command(stage);
    }
    Grouped32ValidationState state =
        validation_states[pc.validation_index];
    Grouped32QueryAdapterState adapter =
        adapter_states[pc.adapter_state_index];
    uint query_minimizer_count = grouped32_query_minimizer_count();
    uint required_workgroups = max(1u, grouped32_ceil_div(
        max(pc.partition_count,
            max(pc.query_record_count, query_minimizer_count)),
        gl_WorkGroupSize.x));
    bool accepted = gl_NumWorkGroups.x == 1u
        && gl_NumWorkGroups.y == 1u && gl_NumWorkGroups.z == 1u
        && state.completion == G32_COMPLETION_PENDING
        && state.expected_workgroups == required_workgroups
        && state.completed_workgroups == state.expected_workgroups
        && state.status == 0u && state.reserved == 0u
        && adapter.completion == G32_COMPLETION_ACCEPTED
        && adapter.status == 0u && adapter.reserved0 == 0u
        && adapter.reserved1 == 0u
        && adapter.expected_records == pc.query_record_count
        && adapter.completed_records == adapter.expected_records
        && adapter.total_minimizers == query_minimizer_count
        && pc.kmer_size == 15u && pc.flags == 0u
        && pc.max_workgroups_x != 0u
        && required_workgroups <= pc.max_workgroups_x
        && pc.partition_count <= pc.max_partition_groups_y
        && uint64_t(pc.indirect_base) + uint64_t(G32_STAGE_COUNT)
            <= uint64_t(pc.indirect_command_count);
    if (accepted) {
        grouped32_publish_validated_commands(query_minimizer_count);
        accepted = validation_states[pc.validation_index].status == 0u;
    }
    memoryBarrierBuffer();
    validation_states[pc.validation_index].completion = accepted
        ? G32_COMPLETION_ACCEPTED : G32_COMPLETION_REJECTED;
    if (!accepted && validation_states[pc.validation_index].status == 0u) {
        validation_states[pc.validation_index].status = G32_VALIDATION_GATE;
    }
}
)GLSL";

constexpr std::string_view kPhaseCGrouped32ValidateMain = R"GLSL(
void main() {
    if (!grouped32_validation_addressable()) return;
    if (!grouped32_core_buffers_bound() || !grouped32_validation_bound()) {
        if (gl_GlobalInvocationID.x == 0u) {
            grouped32_fail(G32_DESCRIPTOR_BOUNDS);
        }
        return;
    }
    uint stride = gl_NumWorkGroups.x * gl_WorkGroupSize.x;
    uint query_minimizer_count = grouped32_query_minimizer_count();
    if (gl_GlobalInvocationID.x == 0u) {
        bool index_axes_disagree = pc.partition_count == 0u
            ? pc.control_count != 0u || pc.position_count != 0u
            : pc.control_count == 0u || pc.position_count == 0u;
        if (index_axes_disagree) grouped32_fail(G32_DESCRIPTOR_BOUNDS);
        if (!grouped32_adapter_bound()) {
            grouped32_fail(G32_ADAPTER_STATE);
        } else {
            Grouped32QueryAdapterState adapter =
                adapter_states[pc.adapter_state_index];
            if (adapter.completion != G32_COMPLETION_ACCEPTED
                || adapter.status != 0u
                || adapter.total_minimizers > pc.query_minimizer_capacity
                || adapter.total_minimizers > pc.occurrence_count) {
                grouped32_fail(G32_ADAPTER_STATE);
            }
        }
        if (pc.occurrence_count < query_minimizer_count
            || (pc.query_record_count == 0u
                && query_minimizer_count != 0u)) {
            grouped32_fail(G32_QUERY_BOUNDS);
        }
        if (pc.kmer_size != 15u || pc.flags != 0u
            || pc.max_workgroups_x == 0u
            || (pc.partition_count != 0u
                && pc.max_partition_groups_y == 0u)) {
            grouped32_fail(G32_DESCRIPTOR_IDENTITY);
        }
        if (grouped32_ceil_div(query_minimizer_count, gl_WorkGroupSize.x)
                > pc.max_workgroups_x
            || grouped32_ceil_div(pc.query_record_count, gl_WorkGroupSize.x)
                > pc.max_workgroups_x
            || pc.partition_count > pc.max_partition_groups_y) {
            grouped32_fail(G32_INDIRECT_BOUNDS);
        }
        if (uint64_t(pc.indirect_base) + uint64_t(G32_STAGE_COUNT)
                > uint64_t(pc.indirect_command_count)) {
            grouped32_fail(G32_INDIRECT_BOUNDS);
        }
        Grouped32ValidationState state =
            validation_states[pc.validation_index];
        if (gl_NumWorkGroups.y != 1u || gl_NumWorkGroups.z != 1u
            || state.expected_workgroups != gl_NumWorkGroups.x
            || state.completion != G32_COMPLETION_PENDING) {
            grouped32_fail(G32_VALIDATION_GATE);
        }
    }
    for (uint ordinal = gl_GlobalInvocationID.x;
         ordinal < pc.partition_count; ordinal += stride) {
        Grouped32Partition part = partitions[ordinal];
        bool failed = false;
        if (!grouped32_partition_bounds_safe(part)
            || part.control_count == 0u
            || part.position_count == 0u) {
            grouped32_fail(G32_DESCRIPTOR_BOUNDS);
            failed = true;
        }
        if (part.directory_ordinal != ordinal
            || part.flags != 0u
            || part.reserved0 != 0u
            || part.reserved1 != 0u) {
            grouped32_fail(G32_DESCRIPTOR_IDENTITY);
            failed = true;
        }
        uint64_t control_end = uint64_t(part.control_offset)
            + uint64_t(part.control_count);
        uint64_t position_end = uint64_t(part.position_offset)
            + uint64_t(part.position_count);
        if ((ordinal == 0u
                && (part.control_offset != 0u
                    || part.position_offset != 0u))
            || (ordinal != 0u
                && (uint64_t(partitions[ordinal - 1u].control_offset)
                        + uint64_t(partitions[ordinal - 1u].control_count)
                            != uint64_t(part.control_offset)
                    || uint64_t(partitions[ordinal - 1u].position_offset)
                        + uint64_t(partitions[ordinal - 1u].position_count)
                            != uint64_t(part.position_offset)))
            || (ordinal + 1u == pc.partition_count
                && (control_end != uint64_t(pc.control_count)
                    || position_end != uint64_t(pc.position_count)))) {
            grouped32_fail(G32_DESCRIPTOR_BOUNDS);
            failed = true;
        }
        if (ordinal != 0u) {
            Grouped32Partition previous = partitions[ordinal - 1u];
            bool ordered = part.partition_id > previous.partition_id
                && (part.position_segment
                    == previous.position_segment
                ? part.segment_base_bp == previous.segment_base_bp
                    && part.segment_end_bp == previous.segment_end_bp
                    && part.low_hash > previous.high_hash
                : part.position_segment > previous.position_segment
                    && part.segment_base_bp >= previous.segment_end_bp);
            if (!ordered) {
                grouped32_fail(G32_DESCRIPTOR_IDENTITY);
                failed = true;
            }
        }
        if (part.segment_base_bp >= part.segment_end_bp
            || part.segment_end_bp - part.segment_base_bp
                > uint64_t(0x80000000u)
            || part.low_hash > part.high_hash) {
            grouped32_fail(G32_SEGMENT_BOUNDS);
            failed = true;
        }
        if (!grouped32_partition_bounds_safe(part)
            || part.control_count == 0u
            || part.position_count == 0u
            || part.segment_base_bp >= part.segment_end_bp) {
            if (failed) {
                atomicAdd(
                    validation_states[pc.validation_index].failed_partitions,
                    1u);
            }
            continue;
        }

        uint expected_position_offset = 0u;
        uint previous_hash = 0u;
        bool have_previous_hash = false;
        uint checked_positions = 0u;
        for (uint local_control = 0u;
             local_control < part.control_count; ++local_control) {
            Grouped32Control control =
                controls[part.control_offset + local_control];
            if ((have_previous_hash && control.hash <= previous_hash)
                || control.hash < part.low_hash
                || control.hash > part.high_hash) {
                grouped32_fail(G32_CONTROL_ORDER);
                failed = true;
            }
            if (control.position_count == 0u
                || control.position_offset != expected_position_offset
                || uint64_t(control.position_offset)
                    + uint64_t(control.position_count)
                        > uint64_t(part.position_count)) {
                grouped32_fail(G32_CONTROL_POSITIONS);
                failed = true;
                continue;
            }
            previous_hash = control.hash;
            have_previous_hash = true;
            expected_position_offset =
                control.position_offset + control.position_count;

            uint previous_position = 0u;
            bool have_previous_position = false;
            for (uint local_position = 0u;
                 local_position < control.position_count; ++local_position) {
                uint encoded = local_positions[
                    part.position_offset + control.position_offset
                    + local_position];
                uint local_bp = encoded >> 1u;
                if (uint64_t(local_bp)
                    >= part.segment_end_bp - part.segment_base_bp) {
                    grouped32_fail(G32_POSITION_BOUNDS);
                    failed = true;
                }
                if (have_previous_position && encoded <= previous_position) {
                    grouped32_fail(G32_POSITION_ORDER);
                    failed = true;
                }
                previous_position = encoded;
                have_previous_position = true;
                ++checked_positions;
            }
        }
        if (!have_previous_hash
            || controls[part.control_offset].hash != part.low_hash
            || previous_hash != part.high_hash
            || expected_position_offset != part.position_count) {
            grouped32_fail(G32_CONTROL_POSITIONS);
            failed = true;
        }
        atomicAdd(
            validation_states[pc.validation_index].controls_checked,
            part.control_count);
        atomicAdd(
            validation_states[pc.validation_index].positions_checked,
            checked_positions);
        if (failed) {
            atomicAdd(
                validation_states[pc.validation_index].failed_partitions, 1u);
        }
    }

    // Query records are device-produced ABI, not host biological metadata.
    // Validate their complete, non-overlapping slices in the same inactive
    // structural pass. A future indirect dispatch must size this stage from
    // max(partition_count, query_record_count, query_minimizer_count).
    for (uint record_index = gl_GlobalInvocationID.x;
         record_index < pc.query_record_count; record_index += stride) {
        Grouped32QueryRecord record = query_records[record_index];
        uint64_t minimizer_end = uint64_t(record.minimizer_offset)
            + uint64_t(record.minimizer_count);
        uint64_t anchor_end = uint64_t(record.anchor_base)
            + uint64_t(record.anchor_capacity);
        bool invalid = record.query_state_index != record_index
            || record.occurrence_offset != record.minimizer_offset
            || minimizer_end > uint64_t(query_minimizer_count)
            || anchor_end > uint64_t(pc.anchor_capacity)
            || record.query_length > 0x80000000u
            || record.flags != 0u
            || record.reserved0 != 0u
            || record.reserved1 != 0u;
        if (record_index == 0u) {
            invalid = invalid || record.minimizer_offset != 0u;
        } else {
            Grouped32QueryRecord previous = query_records[record_index - 1u];
            invalid = invalid
                || uint64_t(previous.minimizer_offset)
                    + uint64_t(previous.minimizer_count)
                        != uint64_t(record.minimizer_offset)
                || uint64_t(previous.anchor_base)
                    + uint64_t(previous.anchor_capacity)
                        > uint64_t(record.anchor_base)
                || previous.batch_join_ordinal >= record.batch_join_ordinal;
        }
        if (record_index + 1u == pc.query_record_count) {
            invalid = invalid
                || minimizer_end != uint64_t(query_minimizer_count);
        }
        if (record.minimizer_count != 0u
            && (pc.kmer_size != 15u || record.query_length < 15u)) {
            invalid = true;
        }
        if (invalid) grouped32_fail(G32_QUERY_BOUNDS);
    }
    for (uint minimizer_index = gl_GlobalInvocationID.x;
         minimizer_index < query_minimizer_count;
         minimizer_index += stride) {
        Grouped32QueryMinimizer minimizer = query_minimizers[minimizer_index];
        Grouped32QueryRecord record;
        if (!grouped32_query_minimizer_safe(
                minimizer_index, minimizer, record)) {
            continue;
        }
    }

    // Workgroups only publish their completion count. A separately dispatched
    // finalize shader consumes it after the typed compute-write/read barrier.
    memoryBarrierBuffer();
    barrier();
    if (gl_LocalInvocationID.x == 0u) {
        uint prior = atomicAdd(
            validation_states[pc.validation_index].completed_workgroups, 1u);
        uint expected =
            validation_states[pc.validation_index].expected_workgroups;
        if (prior + 1u > expected) {
            grouped32_fail(G32_VALIDATION_GATE);
        }
    }
}
)GLSL";

constexpr std::string_view kPhaseCGrouped32AccumulateOccurrencesMain = R"GLSL(
void grouped32_saturating_target_add(uint occurrence_index, uint increment) {
    // max_occ only supplies a safe saturation ceiling. No partition or segment
    // is filtered here; the separate finalize kernel decides acceptance after
    // every matching partition has contributed to this one global accumulator.
    uint ceiling = pc.max_occ == 0xffffffffu
        ? 0xffffffffu : pc.max_occ + 1u;
    uint observed = occurrence_states[occurrence_index].global_count;
    for (;;) {
        if (observed >= ceiling) {
            if (pc.max_occ == 0xffffffffu && increment != 0u) {
                grouped32_fail(G32_OCCURRENCE_OVERFLOW);
            }
            return;
        }
        if (pc.max_occ == 0xffffffffu
            && increment > 0xffffffffu - observed) {
            // With filtering disabled a wrapped u32 would look accepted while
            // losing anchors. Saturate for memory safety and poison proof.
            grouped32_fail(G32_OCCURRENCE_OVERFLOW);
        }
        uint next = observed + min(increment, ceiling - observed);
        uint prior = atomicCompSwap(
            occurrence_states[occurrence_index].global_count,
            observed,
            next);
        if (prior == observed) return;
        observed = prior;
    }
}

void main() {
    if (!grouped32_validation_gate_open()) return;
    uint query_minimizer_count = grouped32_query_minimizer_count();
    uint minimizer_stride = gl_NumWorkGroups.x * gl_WorkGroupSize.x;
    uint partition_stride = gl_NumWorkGroups.y;
    for (uint minimizer_index = gl_GlobalInvocationID.x;
         minimizer_index < query_minimizer_count;
         minimizer_index += minimizer_stride) {
        Grouped32QueryMinimizer minimizer = query_minimizers[minimizer_index];
        Grouped32QueryRecord record;
        if (!grouped32_query_minimizer_safe(
                minimizer_index, minimizer, record)) {
            continue;
        }
        for (uint partition_index = gl_GlobalInvocationID.y;
             partition_index < pc.partition_count;
             partition_index += partition_stride) {
            Grouped32Partition part = partitions[partition_index];
            if (!grouped32_partition_bounds_safe(part)) {
                grouped32_fail(G32_DESCRIPTOR_BOUNDS);
                continue;
            }
            if (part.segment_base_bp >= part.segment_end_bp
                || part.segment_end_bp - part.segment_base_bp
                    > uint64_t(0x80000000u)) {
                grouped32_fail(G32_SEGMENT_BOUNDS);
                continue;
            }
            if (minimizer.hash < part.low_hash
                || minimizer.hash > part.high_hash) {
                continue;
            }
            Grouped32Control control;
            if (grouped32_find_control(part, minimizer.hash, control)) {
                grouped32_saturating_target_add(
                    minimizer.occurrence_index, control.position_count);
            }
        }
    }
}
)GLSL";

constexpr std::string_view kPhaseCGrouped32FinalizeMaxOccMain = R"GLSL(
void main() {
    if (!grouped32_validation_gate_open()) return;
    uint query_minimizer_count = grouped32_query_minimizer_count();
    uint stride = gl_NumWorkGroups.x * gl_WorkGroupSize.x;
    for (uint minimizer_index = gl_GlobalInvocationID.x;
         minimizer_index < query_minimizer_count;
         minimizer_index += stride) {
        Grouped32QueryMinimizer minimizer = query_minimizers[minimizer_index];
        Grouped32QueryRecord record;
        if (!grouped32_query_minimizer_safe(
                minimizer_index, minimizer, record)) {
            continue;
        }
        uint total = occurrence_states[minimizer.occurrence_index].global_count;
        uint accepted = total <= pc.max_occ ? total : 0u;
        occurrence_states[minimizer.occurrence_index].accepted_count = accepted;
        occurrence_states[minimizer.occurrence_index].anchor_offset = 0u;
        occurrence_states[minimizer.occurrence_index].emitted_count = 0u;
        if (accepted == 0u && total != 0u) {
            atomicAdd(
                query_states[record.query_state_index].rejected_minimizers,
                1u);
        }
        atomicAdd(
            query_states[record.query_state_index].completed_minimizers, 1u);
    }
}
)GLSL";

constexpr std::string_view kPhaseCGrouped32AllocateAnchorPrefixMain = R"GLSL(
void main() {
    if (!grouped32_validation_gate_open()) return;
    // Coarse scan candidate: one invocation owns one complete query slice.
    // Benchmark it against hierarchical_segmented_scan before activation.
    uint stride = gl_NumWorkGroups.x * gl_WorkGroupSize.x;
    for (uint record_index = gl_GlobalInvocationID.x;
        record_index < pc.query_record_count; record_index += stride) {
        Grouped32QueryRecord record = query_records[record_index];
        if (!grouped32_query_record_safe(record_index, record)) continue;
        uint64_t total = uint64_t(0);
        bool occurrence_bounds_valid = true;
        for (uint local = 0u; local < record.minimizer_count; ++local) {
            uint64_t occurrence_index_wide = uint64_t(record.occurrence_offset)
                + uint64_t(local);
            if (occurrence_index_wide >= uint64_t(pc.occurrence_count)) {
                grouped32_fail(G32_QUERY_BOUNDS);
                occurrence_bounds_valid = false;
                break;
            }
            uint occurrence_index = uint(occurrence_index_wide);
            total += uint64_t(
                occurrence_states[occurrence_index].accepted_count);
        }
        if (!occurrence_bounds_valid) continue;
        if (total > uint64_t(record.anchor_capacity)) {
            query_states[record.query_state_index].status = G32_ANCHOR_CAPACITY;
            grouped32_fail(G32_ANCHOR_CAPACITY);
            continue;
        }
        uint64_t cursor = uint64_t(record.anchor_base);
        for (uint local = 0u; local < record.minimizer_count; ++local) {
            uint64_t occurrence_index_wide = uint64_t(record.occurrence_offset)
                + uint64_t(local);
            if (occurrence_index_wide >= uint64_t(pc.occurrence_count)
                || cursor > uint64_t(0xffffffffu)) {
                grouped32_fail(G32_QUERY_BOUNDS);
                occurrence_bounds_valid = false;
                break;
            }
            uint occurrence_index = uint(occurrence_index_wide);
            occurrence_states[occurrence_index].anchor_offset = uint(cursor);
            occurrence_states[occurrence_index].emitted_count = 0u;
            cursor += uint64_t(
                occurrence_states[occurrence_index].accepted_count);
        }
        if (occurrence_bounds_valid) {
            query_states[record.query_state_index].anchor_count = uint(total);
        }
    }
}
)GLSL";

constexpr std::string_view kPhaseCGrouped32BuildSortSegmentsMain = R"GLSL(
void main() {
    if (!grouped32_validation_gate_open()) return;
    uint stride = gl_NumWorkGroups.x * gl_WorkGroupSize.x;
    for (uint record_index = gl_GlobalInvocationID.x;
        record_index < pc.query_record_count; record_index += stride) {
        Grouped32QueryRecord record = query_records[record_index];
        if (!grouped32_query_record_safe(record_index, record)) continue;
        Grouped32QueryState state = query_states[record.query_state_index];
        if (state.status != 0u
            || state.anchor_count > record.anchor_capacity) {
            grouped32_fail(G32_SORT_OWNERSHIP);
            continue;
        }
        sort_segments[record_index].anchor_begin = record.anchor_base;
        sort_segments[record_index].anchor_count = state.anchor_count;
        sort_segments[record_index].query_state_index = record.query_state_index;
        sort_segments[record_index].flags = 0u;
        sort_segments[record_index].batch_join_ordinal =
            record.batch_join_ordinal;
    }
}
)GLSL";

constexpr std::string_view kPhaseCGrouped32EmitGlobalAnchorsMain = R"GLSL(
void main() {
    if (!grouped32_validation_gate_open()) return;
    uint query_minimizer_count = grouped32_query_minimizer_count();
    uint minimizer_stride = gl_NumWorkGroups.x * gl_WorkGroupSize.x;
    uint partition_stride = gl_NumWorkGroups.y;
    for (uint minimizer_index = gl_GlobalInvocationID.x;
         minimizer_index < query_minimizer_count;
         minimizer_index += minimizer_stride) {
        Grouped32QueryMinimizer minimizer = query_minimizers[minimizer_index];
        Grouped32QueryRecord record;
        if (!grouped32_query_minimizer_safe(
                minimizer_index, minimizer, record)) {
            continue;
        }
        uint accepted =
            occurrence_states[minimizer.occurrence_index].accepted_count;
        if (accepted == 0u) continue;
        uint query_bp = minimizer.query_bp_strand >> 1u;
        uint query_strand = minimizer.query_bp_strand & 1u;
        uint64_t reverse_base = uint64_t(record.query_length)
            + uint64_t(pc.kmer_size) - uint64_t(2);

        for (uint partition_index = gl_GlobalInvocationID.y;
             partition_index < pc.partition_count;
             partition_index += partition_stride) {
            Grouped32Partition part = partitions[partition_index];
            if (!grouped32_partition_bounds_safe(part)) {
                grouped32_fail(G32_DESCRIPTOR_BOUNDS);
                continue;
            }
            if (part.segment_base_bp >= part.segment_end_bp
                || part.segment_end_bp - part.segment_base_bp
                    > uint64_t(0x80000000u)) {
                grouped32_fail(G32_SEGMENT_BOUNDS);
                continue;
            }
            if (minimizer.hash < part.low_hash
                || minimizer.hash > part.high_hash) {
                continue;
            }
            Grouped32Control control;
            if (!grouped32_find_control(part, minimizer.hash, control)) {
                continue;
            }
            if (uint64_t(control.position_offset)
                    + uint64_t(control.position_count)
                        > uint64_t(part.position_count)) {
                grouped32_fail(G32_CONTROL_POSITIONS);
                continue;
            }

            uint emitted_before = atomicAdd(
                occurrence_states[minimizer.occurrence_index].emitted_count,
                control.position_count);
            if (emitted_before > accepted
                || control.position_count > accepted - emitted_before) {
                grouped32_fail(G32_EMITTED_COUNT);
                continue;
            }
            uint64_t anchor_base_wide = uint64_t(
                occurrence_states[minimizer.occurrence_index].anchor_offset)
                + uint64_t(emitted_before);
            for (uint local = 0u; local < control.position_count; ++local) {
                uint encoded = local_positions[
                    part.position_offset + control.position_offset + local];
                uint local_bp = encoded >> 1u;
                if (uint64_t(local_bp)
                    >= part.segment_end_bp - part.segment_base_bp) {
                    grouped32_fail(G32_POSITION_BOUNDS);
                    continue;
                }
                uint target_strand = encoded & 1u;
                uint reverse = target_strand ^ query_strand;
                uint64_t query_position = reverse != 0u
                    ? reverse_base - uint64_t(query_bp)
                    : uint64_t(query_bp);
                uint64_t anchor_index_wide = anchor_base_wide
                    + uint64_t(local);
                if (anchor_index_wide >= uint64_t(pc.anchor_capacity)
                    || anchor_index_wide < uint64_t(record.anchor_base)
                    || anchor_index_wide
                        >= uint64_t(record.anchor_base)
                            + uint64_t(record.anchor_capacity)) {
                    grouped32_fail(G32_ANCHOR_CAPACITY);
                    continue;
                }
                uint anchor_index = uint(anchor_index_wide);
                global_anchors[anchor_index].ref_position =
                    part.segment_base_bp + uint64_t(local_bp);
                global_anchors[anchor_index].query_position = query_position;
                global_anchors[anchor_index].is_reverse = reverse;
                global_anchors[anchor_index].span_flags_segment =
                    pc.kmer_size & 0xffu;
                anchor_radix_keys[anchor_index].ref_position =
                    part.segment_base_bp + uint64_t(local_bp);
                anchor_radix_keys[anchor_index].is_reverse = reverse;
                anchor_radix_keys[anchor_index].query_state_index =
                    record.query_state_index;
            }
        }
    }
}
)GLSL";

// Adapter from the existing device-resident Phase-C packed-sketch output to
// grouped32 query records/minimizers. Biological hash/position decoding and
// the total minimizer count remain on device; the host supplies only trusted
// scheduling/query identity metadata. One workgroup owns one whole query so
// record publication cannot race across queries. A COMPUTE_SHADER write ->
// COMPUTE_SHADER read barrier is required before indirect preparation.
constexpr std::string_view kPhaseCGrouped32AdaptQuerySketchBody = R"GLSL(
layout(local_size_x = LOCAL_SIZE_X, local_size_y = 1, local_size_z = 1) in;

struct Grouped32SketchQueryPlan {
    uint first_tile;
    uint tile_count;
    uint query_length;
    uint query_state_index;
    uint anchor_base;
    uint anchor_capacity;
    uint flags;
    uint reserved0;
    uint64_t batch_join_ordinal;
    uint64_t reserved1;
};
struct Grouped32QueryRecord {
    uint minimizer_offset;
    uint minimizer_count;
    uint query_length;
    uint query_state_index;
    uint occurrence_offset;
    uint anchor_base;
    uint anchor_capacity;
    uint flags;
    uint64_t batch_join_ordinal;
    uint reserved0;
    uint reserved1;
};
struct Grouped32QueryMinimizer {
    uint hash;
    uint query_bp_strand;
    uint occurrence_index;
    uint query_record_index;
};
struct Grouped32QueryAdapterState {
    uint status;
    uint completed_records;
    uint expected_records;
    uint total_minimizers;
    uint max_query_minimizers;
    uint completion;
    uint reserved0;
    uint reserved1;
};
struct Grouped32ValidationState {
    uint status;
    uint failed_partitions;
    uint controls_checked;
    uint positions_checked;
    uint completed_workgroups;
    uint expected_workgroups;
    uint completion;
    uint reserved;
};
struct Grouped32DispatchIndirectCommand {
    uint group_count_x;
    uint group_count_y;
    uint group_count_z;
};

layout(std430, set = 0, binding = 0) readonly buffer PackedMinimizers {
    uint packed_words[];
};
layout(std430, set = 0, binding = 1) readonly buffer TileCounts {
    uint tile_counts[];
};
layout(std430, set = 0, binding = 2) readonly buffer TileOffsets {
    uint tile_offsets[];
};
layout(std430, set = 0, binding = 3) readonly buffer QueryPlans {
    Grouped32SketchQueryPlan query_plans[];
};
layout(std430, set = 0, binding = 4) buffer QueryRecords {
    Grouped32QueryRecord query_records[];
};
layout(std430, set = 0, binding = 5) buffer QueryMinimizers {
    Grouped32QueryMinimizer query_minimizers[];
};
layout(std430, set = 0, binding = 6) buffer AdapterStates {
    Grouped32QueryAdapterState adapter_states[];
};
layout(std430, set = 0, binding = 7) buffer ValidationStates {
    Grouped32ValidationState validation_states[];
};
layout(std430, set = 0, binding = 8) buffer IndirectCommands {
    Grouped32DispatchIndirectCommand indirect_commands[];
};

layout(push_constant) uniform Params {
    uint query_plan_count;
    uint tile_count;
    uint packed_minimizer_capacity;
    uint minimizer_word_kind;
    uint minimizer_words32;
    uint hash_shift;
    uint pos_bits;
    uint kmer_size;
    uint adapter_state_index;
    uint adapter_state_count;
    uint validation_index;
    uint validation_state_count;
    uint indirect_base;
    uint indirect_command_count;
    uint max_workgroups_x;
    uint max_partition_groups_y;
} pc;

const uint G32_QUERY_BOUNDS = 1u << 7u;
const uint G32_DESCRIPTOR_BOUNDS = 1u << 0u;
const uint G32_ADAPTER_STATE = 1u << 12u;
const uint G32_INDIRECT_BOUNDS = 1u << 13u;
const uint G32_COMPLETION_PENDING = 0u;
const uint G32_COMPLETION_ACCEPTED = 1u;
const uint G32_COMPLETION_REJECTED = 2u;
const uint G32_STAGE_COUNT = 7u;

shared uint g32a_begin;
shared uint g32a_count;
shared uint g32a_valid;

bool g32a_state_addressable() {
    return pc.adapter_state_index < uint(adapter_states.length());
}
bool g32a_state_bound() {
    return pc.adapter_state_count <= uint(adapter_states.length())
        && pc.adapter_state_index < pc.adapter_state_count;
}
bool g32a_buffers_bound() {
    return pc.minimizer_words32 <= uint(packed_words.length())
        && pc.tile_count <= uint(tile_counts.length())
        && pc.tile_count <= uint(tile_offsets.length())
        && pc.query_plan_count <= uint(query_plans.length())
        && pc.query_plan_count <= uint(query_records.length())
        && pc.packed_minimizer_capacity <= uint(query_minimizers.length())
        && pc.adapter_state_count <= uint(adapter_states.length())
        && pc.validation_state_count <= uint(validation_states.length())
        && pc.indirect_command_count <= uint(indirect_commands.length());
}
void g32a_fail(uint bit) {
    if (g32a_state_addressable()) {
        atomicOr(adapter_states[pc.adapter_state_index].status, bit);
    }
    atomicAnd(g32a_valid, 0u);
}
bool g32a_word_bound(uint64_t word) {
    return pc.minimizer_words32 <= uint(packed_words.length())
        && word < uint64_t(pc.minimizer_words32);
}

bool g32a_decode(uint minimizer_index, out uint hash, out uint encoded) {
    uint64_t hash64 = uint64_t(0);
    uint64_t encoded64 = uint64_t(0);
    if (pc.minimizer_word_kind == 0u) {
        uint64_t word = uint64_t(minimizer_index) >> 1u;
        if (!g32a_word_bound(word) || pc.hash_shift >= 16u) return false;
        uint raw = (packed_words[uint(word)]
            >> ((minimizer_index & 1u) * 16u)) & 0xffffu;
        uint mask = pc.pos_bits >= 15u
            ? 0xffffu : ((1u << (pc.pos_bits + 1u)) - 1u);
        hash64 = uint64_t(raw >> pc.hash_shift);
        encoded64 = uint64_t(raw & mask);
    } else if (pc.minimizer_word_kind == 1u) {
        uint64_t word = uint64_t(minimizer_index);
        if (!g32a_word_bound(word) || pc.hash_shift >= 32u) return false;
        uint raw = packed_words[uint(word)];
        uint mask = pc.pos_bits >= 31u
            ? 0xffffffffu : ((1u << (pc.pos_bits + 1u)) - 1u);
        hash64 = uint64_t(raw) >> pc.hash_shift;
        encoded64 = uint64_t(raw & mask);
    } else if (pc.minimizer_word_kind == 2u) {
        uint64_t word = uint64_t(minimizer_index) * uint64_t(2);
        if (!g32a_word_bound(word + uint64_t(1))
            || pc.hash_shift >= 64u) return false;
        uint64_t raw = uint64_t(packed_words[uint(word)])
            | (uint64_t(packed_words[uint(word + uint64_t(1))]) << 32u);
        uint64_t mask = pc.pos_bits >= 63u
            ? ~uint64_t(0)
            : ((uint64_t(1) << (pc.pos_bits + 1u)) - uint64_t(1));
        hash64 = raw >> pc.hash_shift;
        encoded64 = raw & mask;
    } else if (pc.minimizer_word_kind == 3u) {
        uint64_t word = uint64_t(minimizer_index) * uint64_t(4);
        if (!g32a_word_bound(word + uint64_t(3))) return false;
        encoded64 = uint64_t(packed_words[uint(word)])
            | (uint64_t(packed_words[uint(word + uint64_t(1))]) << 32u);
        uint64_t high = uint64_t(packed_words[uint(word + uint64_t(2))])
            | (uint64_t(packed_words[uint(word + uint64_t(3))]) << 32u);
        if ((high & uint64_t(0xff)) != uint64_t(15)) return false;
        hash64 = high >> 8u;
    } else {
        return false;
    }
    if (hash64 > uint64_t(0xffffffffu)
        || encoded64 > uint64_t(0xffffffffu)) return false;
    hash = uint(hash64);
    encoded = uint(encoded64);
    return true;
}

void main() {
    uint record_index = gl_WorkGroupID.x;
    if (record_index >= pc.query_plan_count || !g32a_state_addressable()) return;
    if (gl_LocalInvocationID.x == 0u) {
        g32a_valid = 1u;
        g32a_begin = 0u;
        g32a_count = 0u;
        if (!g32a_buffers_bound() || !g32a_state_bound()) {
            g32a_fail(G32_DESCRIPTOR_BOUNDS);
        } else {
        Grouped32QueryAdapterState state =
            adapter_states[pc.adapter_state_index];
        if (state.expected_records != pc.query_plan_count
            || state.completion != G32_COMPLETION_PENDING
            || state.reserved0 != 0u || state.reserved1 != 0u) {
            g32a_fail(G32_ADAPTER_STATE);
        }
        if (pc.kmer_size != 15u || pc.pos_bits > 31u
            || pc.minimizer_word_kind > 3u
            || (pc.minimizer_word_kind <= 2u
                && pc.hash_shift != pc.pos_bits + 1u)
            || pc.max_workgroups_x == 0u
            || pc.query_plan_count > pc.max_workgroups_x
            || gl_NumWorkGroups.x != pc.query_plan_count
            || gl_NumWorkGroups.y != 1u || gl_NumWorkGroups.z != 1u) {
            g32a_fail(G32_QUERY_BOUNDS);
        }
        if (pc.validation_index >= pc.validation_state_count
            || uint64_t(pc.indirect_base) + uint64_t(G32_STAGE_COUNT)
                > uint64_t(pc.indirect_command_count)) {
            g32a_fail(G32_INDIRECT_BOUNDS);
        }

        Grouped32SketchQueryPlan plan = query_plans[record_index];
        uint64_t tile_end = uint64_t(plan.first_tile)
            + uint64_t(plan.tile_count);
        if (plan.query_state_index != record_index
            || plan.tile_count == 0u || tile_end > uint64_t(pc.tile_count)
            || plan.query_length > 0x80000000u
            || plan.flags != 0u || plan.reserved0 != 0u
            || plan.reserved1 != uint64_t(0)) {
            g32a_fail(G32_QUERY_BOUNDS);
        } else {
            uint begin = tile_offsets[plan.first_tile];
            uint64_t count64 = uint64_t(0);
            for (uint local_tile = 0u; local_tile < plan.tile_count;
                 ++local_tile) {
                uint tile = plan.first_tile + local_tile;
                uint offset = tile_offsets[tile];
                if (uint64_t(offset) != uint64_t(begin) + count64) {
                    g32a_fail(G32_QUERY_BOUNDS);
                    break;
                }
                count64 += uint64_t(tile_counts[tile]);
            }
            if (uint64_t(begin) + count64
                    > uint64_t(pc.packed_minimizer_capacity)
                || count64 > uint64_t(0xffffffffu)
                || (count64 != uint64_t(0) && plan.query_length < 15u)) {
                g32a_fail(G32_QUERY_BOUNDS);
            }
            g32a_begin = begin;
            g32a_count = uint(count64);
        }

        if (record_index == 0u) {
            if (query_plans[record_index].first_tile != 0u
                || g32a_begin != 0u) g32a_fail(G32_QUERY_BOUNDS);
        } else {
            Grouped32SketchQueryPlan previous = query_plans[record_index - 1u];
            uint64_t previous_tile_end = uint64_t(previous.first_tile)
                + uint64_t(previous.tile_count);
            if (previous_tile_end
                    != uint64_t(query_plans[record_index].first_tile)
                || previous.batch_join_ordinal
                    >= query_plans[record_index].batch_join_ordinal
                || uint64_t(previous.anchor_base)
                    + uint64_t(previous.anchor_capacity)
                        > uint64_t(query_plans[record_index].anchor_base)) {
                g32a_fail(G32_QUERY_BOUNDS);
            }
        }
        if (record_index + 1u == pc.query_plan_count
            && uint64_t(query_plans[record_index].first_tile)
                + uint64_t(query_plans[record_index].tile_count)
                    != uint64_t(pc.tile_count)) {
            g32a_fail(G32_QUERY_BOUNDS);
        }

        if (g32a_valid == 0u) g32a_count = 0u;

        query_records[record_index].minimizer_offset = g32a_begin;
        query_records[record_index].minimizer_count = g32a_count;
        query_records[record_index].query_length = plan.query_length;
        query_records[record_index].query_state_index = record_index;
        query_records[record_index].occurrence_offset = g32a_begin;
        query_records[record_index].anchor_base = plan.anchor_base;
        query_records[record_index].anchor_capacity = plan.anchor_capacity;
        query_records[record_index].flags = 0u;
        query_records[record_index].batch_join_ordinal =
            plan.batch_join_ordinal;
        query_records[record_index].reserved0 = 0u;
        query_records[record_index].reserved1 = 0u;
        atomicMax(adapter_states[pc.adapter_state_index].max_query_minimizers,
                  g32a_count);
        }
    }
    barrier();

    for (uint local = gl_LocalInvocationID.x; local < g32a_count;
         local += gl_WorkGroupSize.x) {
        uint minimizer_index = g32a_begin + local;
        uint hash = 0u;
        uint encoded = 0u;
        Grouped32SketchQueryPlan plan = query_plans[record_index];
        if (!g32a_decode(minimizer_index, hash, encoded)
            || (encoded >> 1u) >= plan.query_length
            || (encoded >> 1u) + 1u < pc.kmer_size) {
            g32a_fail(G32_QUERY_BOUNDS);
            continue;
        }
        query_minimizers[minimizer_index].hash = hash;
        query_minimizers[minimizer_index].query_bp_strand = encoded;
        query_minimizers[minimizer_index].occurrence_index = minimizer_index;
        query_minimizers[minimizer_index].query_record_index = record_index;
    }
    memoryBarrierBuffer();
    barrier();

    if (gl_LocalInvocationID.x == 0u) {
        // Contiguous query slices make max(end) the authoritative device-side
        // total regardless of which workgroup reaches completion last.
        atomicMax(adapter_states[pc.adapter_state_index].total_minimizers,
                  g32a_begin + g32a_count);
        uint prior = atomicAdd(
            adapter_states[pc.adapter_state_index].completed_records, 1u);
        uint expected = adapter_states[pc.adapter_state_index].expected_records;
        if (prior + 1u > expected) {
            g32a_fail(G32_ADAPTER_STATE);
        }
    }
}
)GLSL";

constexpr std::string_view kPhaseCGrouped32FinalizeQueryAdapterBody = R"GLSL(
layout(local_size_x = LOCAL_SIZE_X, local_size_y = 1, local_size_z = 1) in;
struct Grouped32SketchQueryPlan {
    uint first_tile; uint tile_count; uint query_length; uint query_state_index;
    uint anchor_base; uint anchor_capacity; uint flags; uint reserved0;
    uint64_t batch_join_ordinal; uint64_t reserved1;
};
struct Grouped32QueryRecord {
    uint minimizer_offset; uint minimizer_count; uint query_length;
    uint query_state_index; uint occurrence_offset; uint anchor_base;
    uint anchor_capacity; uint flags; uint64_t batch_join_ordinal;
    uint reserved0; uint reserved1;
};
struct Grouped32QueryMinimizer {
    uint hash; uint query_bp_strand; uint occurrence_index;
    uint query_record_index;
};
struct Grouped32QueryAdapterState {
    uint status; uint completed_records; uint expected_records;
    uint total_minimizers; uint max_query_minimizers; uint completion;
    uint reserved0; uint reserved1;
};
struct Grouped32ValidationState {
    uint status; uint failed_partitions; uint controls_checked;
    uint positions_checked; uint completed_workgroups;
    uint expected_workgroups; uint completion; uint reserved;
};
struct Grouped32DispatchIndirectCommand {
    uint group_count_x; uint group_count_y; uint group_count_z;
};
layout(std430, set = 0, binding = 0) readonly buffer PackedMinimizers {
    uint packed_words[];
};
layout(std430, set = 0, binding = 1) readonly buffer TileCounts {
    uint tile_counts[];
};
layout(std430, set = 0, binding = 2) readonly buffer TileOffsets {
    uint tile_offsets[];
};
layout(std430, set = 0, binding = 3) readonly buffer QueryPlans {
    Grouped32SketchQueryPlan query_plans[];
};
layout(std430, set = 0, binding = 4) readonly buffer QueryRecords {
    Grouped32QueryRecord query_records[];
};
layout(std430, set = 0, binding = 5) readonly buffer QueryMinimizers {
    Grouped32QueryMinimizer query_minimizers[];
};
layout(std430, set = 0, binding = 6) buffer AdapterStates {
    Grouped32QueryAdapterState adapter_states[];
};
layout(std430, set = 0, binding = 7) readonly buffer ValidationStates {
    Grouped32ValidationState validation_states[];
};
layout(std430, set = 0, binding = 8) readonly buffer IndirectCommands {
    Grouped32DispatchIndirectCommand indirect_commands[];
};
layout(push_constant) uniform Params {
    uint query_plan_count; uint tile_count; uint packed_minimizer_capacity;
    uint minimizer_word_kind; uint minimizer_words32; uint hash_shift;
    uint pos_bits; uint kmer_size; uint adapter_state_index;
    uint adapter_state_count; uint validation_index;
    uint validation_state_count; uint indirect_base;
    uint indirect_command_count; uint max_workgroups_x;
    uint max_partition_groups_y;
} pc;
const uint G32_DESCRIPTOR_BOUNDS = 1u << 0u;
const uint G32_ADAPTER_STATE = 1u << 12u;
const uint G32_COMPLETION_PENDING = 0u;
const uint G32_COMPLETION_ACCEPTED = 1u;
const uint G32_COMPLETION_REJECTED = 2u;
const uint G32_STAGE_COUNT = 7u;
void main() {
    bool state_addressable =
        pc.adapter_state_index < uint(adapter_states.length());
    if (gl_GlobalInvocationID.x != 0u || !state_addressable) return;
    bool buffers_bound = pc.minimizer_words32 <= uint(packed_words.length())
        && pc.tile_count <= uint(tile_counts.length())
        && pc.tile_count <= uint(tile_offsets.length())
        && pc.query_plan_count <= uint(query_plans.length())
        && pc.query_plan_count <= uint(query_records.length())
        && pc.packed_minimizer_capacity <= uint(query_minimizers.length())
        && pc.adapter_state_count <= uint(adapter_states.length())
        && pc.adapter_state_index < pc.adapter_state_count
        && pc.validation_state_count <= uint(validation_states.length())
        && pc.validation_index < pc.validation_state_count
        && pc.indirect_command_count <= uint(indirect_commands.length())
        && uint64_t(pc.indirect_base) + uint64_t(G32_STAGE_COUNT)
            <= uint64_t(pc.indirect_command_count);
    if (!buffers_bound) {
        adapter_states[pc.adapter_state_index].status |= G32_DESCRIPTOR_BOUNDS;
        adapter_states[pc.adapter_state_index].completion =
            G32_COMPLETION_REJECTED;
        return;
    }
    Grouped32QueryAdapterState state =
        adapter_states[pc.adapter_state_index];
    bool accepted = gl_NumWorkGroups.x == 1u
        && gl_NumWorkGroups.y == 1u && gl_NumWorkGroups.z == 1u
        && state.completion == G32_COMPLETION_PENDING
        && state.expected_records == pc.query_plan_count
        && state.completed_records == state.expected_records
        && state.status == 0u && state.reserved0 == 0u
        && state.reserved1 == 0u
        && state.total_minimizers <= pc.packed_minimizer_capacity
        && state.max_query_minimizers <= state.total_minimizers
        && pc.kmer_size == 15u && pc.pos_bits <= 31u
        && pc.minimizer_word_kind <= 3u
        && (pc.minimizer_word_kind > 2u
            || pc.hash_shift == pc.pos_bits + 1u)
        && pc.max_workgroups_x != 0u
        && pc.query_plan_count <= pc.max_workgroups_x;
    if (accepted && pc.query_plan_count != 0u) {
        Grouped32QueryRecord last = query_records[pc.query_plan_count - 1u];
        accepted = uint64_t(last.minimizer_offset)
                + uint64_t(last.minimizer_count)
                    == uint64_t(state.total_minimizers);
    } else if (accepted) {
        accepted = state.total_minimizers == 0u;
    }
    adapter_states[pc.adapter_state_index].completion = accepted
        ? G32_COMPLETION_ACCEPTED : G32_COMPLETION_REJECTED;
    if (!accepted && adapter_states[pc.adapter_state_index].status == 0u) {
        adapter_states[pc.adapter_state_index].status = G32_ADAPTER_STATE;
    }
}
)GLSL";

// Device-side command preparation. This pass is the only writer that resets
// the bound validation token and opens the validation indirect command. The
// command buffer inserts a COMPUTE_SHADER write -> INDIRECT_COMMAND_READ
// barrier before vkCmdDispatchIndirect(validate), a compute-write/read barrier
// before the separate validation finalizer, and only then a compute-write ->
// indirect/compute barrier before downstream consumption.
constexpr std::string_view kPhaseCGrouped32PrepareIndirectBody = R"GLSL(
layout(local_size_x = LOCAL_SIZE_X, local_size_y = 1, local_size_z = 1) in;
struct Grouped32Partition {
    uint partition_id;
    uint directory_ordinal;
    uint position_segment;
    uint control_offset;
    uint control_count;
    uint position_offset;
    uint position_count;
    uint low_hash;
    uint high_hash;
    uint flags;
    uint64_t segment_base_bp;
    uint64_t segment_end_bp;
    uint reserved0;
    uint reserved1;
};
struct Grouped32QueryRecord {
    uint minimizer_offset;
    uint minimizer_count;
    uint query_length;
    uint query_state_index;
    uint occurrence_offset;
    uint anchor_base;
    uint anchor_capacity;
    uint flags;
    uint64_t batch_join_ordinal;
    uint reserved0;
    uint reserved1;
};
struct Grouped32QueryAdapterState {
    uint status;
    uint completed_records;
    uint expected_records;
    uint total_minimizers;
    uint max_query_minimizers;
    uint completion;
    uint reserved0;
    uint reserved1;
};
struct Grouped32ValidationState {
    uint status;
    uint failed_partitions;
    uint controls_checked;
    uint positions_checked;
    uint completed_workgroups;
    uint expected_workgroups;
    uint completion;
    uint reserved;
};
struct Grouped32DispatchIndirectCommand {
    uint group_count_x;
    uint group_count_y;
    uint group_count_z;
};
layout(std430, set = 0, binding = 0) readonly buffer Partitions {
    Grouped32Partition partitions[];
};
layout(std430, set = 0, binding = 3) readonly buffer QueryRecords {
    Grouped32QueryRecord query_records[];
};
layout(std430, set = 0, binding = 12) readonly buffer AdapterStates {
    Grouped32QueryAdapterState adapter_states[];
};
layout(std430, set = 0, binding = 8) buffer ValidationStates {
    Grouped32ValidationState validation_states[];
};
layout(std430, set = 0, binding = 9) buffer IndirectCommands {
    Grouped32DispatchIndirectCommand indirect_commands[];
};
layout(push_constant) uniform Params {
    uint partition_count;
    uint query_record_count;
    uint adapter_state_index;
    uint adapter_state_count;
    uint validation_index;
    uint validation_state_count;
    uint indirect_base;
    uint indirect_command_count;
    uint max_workgroups_x;
    uint max_partition_groups_y;
    uint local_size_x;
    uint flags;
} pc;
const uint G32_DESCRIPTOR_BOUNDS = 1u << 0u;
const uint G32_ADAPTER_STATE = 1u << 12u;
const uint G32_INDIRECT_BOUNDS = 1u << 13u;
const uint G32_COMPLETION_PENDING = 0u;
const uint G32_COMPLETION_ACCEPTED = 1u;
const uint G32_COMPLETION_REJECTED = 2u;
const uint G32_STAGE_VALIDATE = 0u;
const uint G32_STAGE_COUNT = 7u;
uint g32p_ceil_div(uint value, uint divisor) {
    return divisor == 0u ? 0u
        : value / divisor + (value % divisor != 0u ? 1u : 0u);
}
void main() {
    if (gl_GlobalInvocationID.x != 0u) return;
    bool validation_addressable =
        pc.validation_index < uint(validation_states.length());
    if (!validation_addressable) return;
    bool validation_bound =
        pc.validation_state_count <= uint(validation_states.length())
        && pc.validation_index < pc.validation_state_count;
    bool adapter_bound =
        pc.adapter_state_count <= uint(adapter_states.length())
        && pc.adapter_state_index < pc.adapter_state_count;
    bool indirect_bound =
        pc.indirect_command_count <= uint(indirect_commands.length())
        && uint64_t(pc.indirect_base)
        + uint64_t(G32_STAGE_COUNT)
            <= uint64_t(pc.indirect_command_count);
    bool input_counts_bound =
        pc.partition_count <= uint(partitions.length())
        && pc.query_record_count <= uint(query_records.length());
    if (!validation_bound || !adapter_bound || !indirect_bound
        || !input_counts_bound) {
        Grouped32ValidationState rejected;
        rejected.status = !validation_bound || !input_counts_bound
            ? G32_DESCRIPTOR_BOUNDS
            : (!adapter_bound ? G32_ADAPTER_STATE : G32_INDIRECT_BOUNDS);
        rejected.failed_partitions = 0u;
        rejected.controls_checked = 0u;
        rejected.positions_checked = 0u;
        rejected.completed_workgroups = 0u;
        rejected.expected_workgroups = 0u;
        rejected.completion = G32_COMPLETION_REJECTED;
        rejected.reserved = 0u;
        validation_states[pc.validation_index] = rejected;
        memoryBarrierBuffer();
        return;
    }
    for (uint stage = 0u; stage < G32_STAGE_COUNT; ++stage) {
        uint command = pc.indirect_base + stage;
        indirect_commands[command].group_count_x = 0u;
        indirect_commands[command].group_count_y = 0u;
        indirect_commands[command].group_count_z = 0u;
    }
    Grouped32QueryAdapterState adapter =
        adapter_states[pc.adapter_state_index];
    Grouped32ValidationState validation;
    validation.status = 0u;
    validation.failed_partitions = 0u;
    validation.controls_checked = 0u;
    validation.positions_checked = 0u;
    validation.completed_workgroups = 0u;
    validation.expected_workgroups = 0u;
    validation.completion = G32_COMPLETION_PENDING;
    validation.reserved = 0u;
    bool adapter_valid = adapter.completion == G32_COMPLETION_ACCEPTED
        && adapter.status == 0u && adapter.reserved0 == 0u
        && adapter.reserved1 == 0u && pc.local_size_x == LOCAL_SIZE_X
        && pc.local_size_x != 0u && pc.max_workgroups_x != 0u
        && adapter.expected_records == pc.query_record_count
        && adapter.completed_records == adapter.expected_records
        && pc.flags == 0u
        && gl_NumWorkGroups.x == 1u && gl_NumWorkGroups.y == 1u
        && gl_NumWorkGroups.z == 1u
        && (pc.partition_count == 0u || pc.max_partition_groups_y != 0u);
    uint maximum = max(pc.partition_count,
        max(pc.query_record_count, adapter.total_minimizers));
    uint groups = max(1u, g32p_ceil_div(maximum, pc.local_size_x));
    bool dispatch_valid = groups <= pc.max_workgroups_x
        && pc.partition_count <= pc.max_partition_groups_y;
    bool valid = adapter_valid && dispatch_valid;
    validation.expected_workgroups = min(groups, pc.max_workgroups_x);
    if (!valid) {
        validation.status = !adapter_valid
            ? (adapter.status == 0u ? G32_ADAPTER_STATE : adapter.status)
            : G32_INDIRECT_BOUNDS;
        validation.completion = G32_COMPLETION_REJECTED;
    }
    validation_states[pc.validation_index] = validation;
    memoryBarrierBuffer();
    if (valid) {
        uint command = pc.indirect_base + G32_STAGE_VALIDATE;
        indirect_commands[command].group_count_x = groups;
        indirect_commands[command].group_count_y = 1u;
        indirect_commands[command].group_count_z = 1u;
    }
    memoryBarrierBuffer();
}
)GLSL";

[[nodiscard]] std::string grouped32_abi2_body(std::string_view stage_main) {
    std::string body;
    body.reserve(kPhaseCGrouped32Abi2Common.size() + stage_main.size());
    body += kPhaseCGrouped32Abi2Common;
    body += stage_main;
    return body;
}

// DOC0371 persisted format-4 streaming ABI.  Unlike the older aggregate ABI2
// prototype, binding 1 contains exactly one bounded raw payload group: 12-byte
// controls in stage 44 or 4-byte local positions in stage 46.  The only state
// retained across groups is target-global occurrence/match/authorization state.
constexpr std::string_view kPhaseCGrouped32StreamCommon = R"GLSL(
layout(local_size_x = LOCAL_SIZE_X, local_size_y = 1, local_size_z = 1) in;

struct G32SPartition {
    uint partition_id; uint directory_ordinal; uint position_segment;
    uint control_offset; uint control_count; uint position_offset;
    uint position_count; uint low_hash; uint high_hash; uint flags;
    uint64_t segment_base_bp; uint64_t segment_end_bp;
    uint reserved0; uint reserved1;
};
struct G32SQueryRecord {
    uint minimizer_offset; uint minimizer_count; uint query_length;
    uint query_state_index; uint occurrence_offset; uint anchor_base;
    uint anchor_capacity; uint flags; uint64_t batch_join_ordinal;
    uint reserved0; uint reserved1;
};
struct G32SQueryMinimizer {
    uint hash; uint query_bp_strand; uint occurrence_index;
    uint query_record_index;
};
struct G32SOccurrence {
    uint global_count; uint accepted_count; uint anchor_offset;
    uint emitted_count;
};
struct G32SMatch {
    uint directory_ordinal; uint query_minimizer_index;
    uint position_offset; uint position_count;
};
struct G32SState {
    uint status; uint match_count; uint matched_anchor_upper_bound;
    uint authorized_anchor_count; uint authorized_partition_count;
    uint emitted_anchor_count; uint completion;
    uint control_groups_completed; uint position_groups_completed;
    uint authorization_digest_lo; uint authorization_digest_hi;
    uint anchor_digest_lo; uint anchor_digest_hi;
    uint reserved0; uint reserved1; uint reserved2;
};
struct G32SAnchor {
    uint64_t ref_position; uint64_t query_position;
    uint is_reverse; uint span_flags_segment;
};

layout(std430, set=0, binding=0) readonly buffer G32SPartitions {
    G32SPartition partitions[];
};
layout(std430, set=0, binding=1) readonly buffer G32SRawPayload {
    uint raw_words[];
};
layout(std430, set=0, binding=2) readonly buffer G32SQueryRecords {
    G32SQueryRecord query_records[];
};
layout(std430, set=0, binding=3) readonly buffer G32SQueryMinimizers {
    G32SQueryMinimizer query_minimizers[];
};
layout(std430, set=0, binding=4) buffer G32SOccurrences {
    G32SOccurrence occurrences[];
};
layout(std430, set=0, binding=5) buffer G32SMatches {
    G32SMatch matches[];
};
layout(std430, set=0, binding=6) buffer G32SStates {
    G32SState stream_states[];
};
layout(std430, set=0, binding=7) buffer G32SAuthorizationFlags {
    uint authorization_flags[];
};
layout(std430, set=0, binding=8) buffer G32SAnchors {
    G32SAnchor anchors[];
};

layout(push_constant) uniform G32SParams {
    uint partition_count; uint control_count; uint position_count;
    uint query_record_count; uint query_minimizer_count;
    uint occurrence_count; uint match_capacity; uint directory_entry_count;
    uint anchor_capacity; uint max_occ; uint kmer_size; uint group_ordinal;
    uint expected_control_groups; uint expected_position_groups;
    uint query_identity_lo; uint query_identity_hi;
    uint control_state_identity_lo; uint control_state_identity_hi;
    uint target_buffer_identity_lo; uint target_buffer_identity_hi;
} pc;

const uint G32S_DESCRIPTOR = 1u << 0u;
const uint G32S_CONTROL = 1u << 1u;
const uint G32S_POSITION = 1u << 2u;
const uint G32S_QUERY = 1u << 3u;
const uint G32S_MATCH_CAPACITY = 1u << 4u;
const uint G32S_COUNT_OVERFLOW = 1u << 5u;
const uint G32S_PHASE = 1u << 6u;
const uint G32S_ANCHOR_CAPACITY = 1u << 7u;
const uint G32S_EMITTED_COUNT = 1u << 8u;

void g32s_fail(uint bit) { atomicOr(stream_states[0].status, bit); }
uint g32s_mix(uint x) {
    x ^= x >> 16u; x *= 0x7feb352du; x ^= x >> 15u;
    x *= 0x846ca68bu; return x ^ (x >> 16u);
}
bool g32s_query_valid(uint qi, G32SQueryMinimizer q) {
    if (qi >= pc.query_minimizer_count || q.occurrence_index != qi
        || q.occurrence_index >= pc.occurrence_count
        || q.query_record_index >= pc.query_record_count) {
        g32s_fail(G32S_QUERY); return false;
    }
    G32SQueryRecord r = query_records[q.query_record_index];
    uint64_t end = uint64_t(r.minimizer_offset) + uint64_t(r.minimizer_count);
    if (r.query_state_index != q.query_record_index || r.flags != 0u
        || r.reserved0 != 0u || r.reserved1 != 0u
        || r.occurrence_offset != r.minimizer_offset
        || qi < r.minimizer_offset || uint64_t(qi) >= end
        || end > uint64_t(pc.query_minimizer_count)
        || (q.query_bp_strand >> 1u) >= r.query_length
        || (q.query_bp_strand >> 1u) + 1u < pc.kmer_size
        || (r.minimizer_count != 0u
            && (pc.kmer_size != 15u || r.query_length < pc.kmer_size))) {
        g32s_fail(G32S_QUERY); return false;
    }
    return true;
}
bool g32s_partition_common_valid(G32SPartition p, uint required_flag) {
    bool valid = p.flags == required_flag && p.reserved0 == 0u
        && p.reserved1 == 0u && p.partition_id == p.directory_ordinal
        && p.directory_ordinal < pc.directory_entry_count
        && p.control_count != 0u && p.position_count != 0u
        && p.low_hash <= p.high_hash
        && p.segment_base_bp < p.segment_end_bp
        && p.segment_end_bp - p.segment_base_bp <= uint64_t(0x80000000u);
    if (!valid) g32s_fail(G32S_DESCRIPTOR);
    return valid;
}
)GLSL";

constexpr std::string_view kPhaseCGrouped32StreamControlMain = R"GLSL(
void main() {
    if (uint(stream_states.length()) == 0u
        || pc.match_capacity > uint(matches.length())
        || pc.partition_count > uint(partitions.length())
        || pc.query_record_count > uint(query_records.length())
        || pc.query_minimizer_count > uint(query_minimizers.length())
        || pc.occurrence_count > uint(occurrences.length())
        || pc.directory_entry_count > uint(authorization_flags.length())
        || uint64_t(pc.control_count) * uint64_t(3u)
            > uint64_t(raw_words.length())
        || pc.query_minimizer_count != pc.occurrence_count
        || stream_states[0].completion != 0u) {
        if (uint(stream_states.length()) != 0u) g32s_fail(G32S_PHASE);
        return;
    }
    uint stride = gl_NumWorkGroups.x * gl_WorkGroupSize.x;
    for (uint pi = gl_GlobalInvocationID.x; pi < pc.partition_count; pi += stride) {
        G32SPartition p = partitions[pi];
        bool valid = g32s_partition_common_valid(p, 1u)
            && p.position_offset == 0u
            && uint64_t(p.control_offset) + uint64_t(p.control_count)
                <= uint64_t(pc.control_count);
        uint expected_position = 0u; uint previous_hash = 0u;
        for (uint ci = 0u; valid && ci < p.control_count; ++ci) {
            uint base = (p.control_offset + ci) * 3u;
            uint hash = raw_words[base];
            uint offset = raw_words[base + 1u];
            uint count = raw_words[base + 2u];
            valid = count != 0u && offset == expected_position
                && uint64_t(offset) + uint64_t(count)
                    <= uint64_t(p.position_count)
                && (ci == 0u ? hash == p.low_hash : hash > previous_hash)
                && hash >= p.low_hash && hash <= p.high_hash;
            expected_position = offset + count; previous_hash = hash;
        }
        valid = valid && expected_position == p.position_count
            && previous_hash == p.high_hash;
        if (!valid) g32s_fail(G32S_CONTROL);
    }

    uint64_t pair_count = uint64_t(pc.partition_count)
        * uint64_t(pc.query_minimizer_count);
    for (uint64_t pair = uint64_t(gl_GlobalInvocationID.x); pair < pair_count;
         pair += uint64_t(stride)) {
        uint pi = uint(pair / uint64_t(pc.query_minimizer_count));
        uint qi = uint(pair - uint64_t(pi) * uint64_t(pc.query_minimizer_count));
        G32SPartition p = partitions[pi];
        G32SQueryMinimizer q = query_minimizers[qi];
        if (!g32s_partition_common_valid(p, 1u) || !g32s_query_valid(qi, q)
            || q.hash < p.low_hash || q.hash > p.high_hash
            || uint64_t(p.control_offset) + uint64_t(p.control_count)
                > uint64_t(pc.control_count)) continue;
        uint lo = 0u, hi = p.control_count;
        while (lo < hi) {
            uint mid = lo + ((hi - lo) >> 1u);
            uint hash = raw_words[(p.control_offset + mid) * 3u];
            if (hash < q.hash) lo = mid + 1u; else hi = mid;
        }
        if (lo == p.control_count) continue;
        uint base = (p.control_offset + lo) * 3u;
        if (raw_words[base] != q.hash) continue;
        uint position_offset = raw_words[base + 1u];
        uint position_count = raw_words[base + 2u];
        uint match_index = atomicAdd(stream_states[0].match_count, 1u);
        if (match_index >= pc.match_capacity) {
            g32s_fail(G32S_MATCH_CAPACITY); continue;
        }
        matches[match_index].directory_ordinal = p.directory_ordinal;
        matches[match_index].query_minimizer_index = qi;
        matches[match_index].position_offset = position_offset;
        matches[match_index].position_count = position_count;
        uint ceiling = pc.max_occ == 0xffffffffu
            ? 0xffffffffu : pc.max_occ + 1u;
        uint observed = occurrences[qi].global_count;
        for (;;) {
            if (observed >= ceiling) break;
            uint next = observed + min(position_count, ceiling - observed);
            uint prior = atomicCompSwap(occurrences[qi].global_count, observed, next);
            if (prior == observed) break;
            observed = prior;
        }
        uint prior_upper = atomicAdd(
            stream_states[0].matched_anchor_upper_bound, position_count);
        if (prior_upper > 0xffffffffu - position_count) {
            g32s_fail(G32S_COUNT_OVERFLOW);
        }
    }
    if (gl_GlobalInvocationID.x == 0u) {
        atomicAdd(stream_states[0].control_groups_completed, 1u);
    }
}
)GLSL";

constexpr std::string_view kPhaseCGrouped32StreamAuthorizeMain = R"GLSL(
void main() {
    if (gl_GlobalInvocationID.x != 0u
        || uint(stream_states.length()) == 0u) return;
    G32SState state = stream_states[0];
    if (state.completion != 0u
        || state.control_groups_completed != pc.expected_control_groups
        || state.match_count > pc.match_capacity
        || pc.match_capacity > uint(matches.length())
        || pc.occurrence_count > uint(occurrences.length())
        || pc.directory_entry_count > uint(authorization_flags.length())) {
        g32s_fail(G32S_PHASE); return;
    }
    uint cursor = 0u;
    for (uint oi = 0u; oi < pc.occurrence_count; ++oi) {
        uint total = occurrences[oi].global_count;
        uint accepted = total <= pc.max_occ ? total : 0u;
        occurrences[oi].accepted_count = accepted;
        occurrences[oi].anchor_offset = cursor;
        occurrences[oi].emitted_count = 0u;
        if (accepted > 0xffffffffu - cursor) {
            g32s_fail(G32S_COUNT_OVERFLOW); return;
        }
        cursor += accepted;
    }
    for (uint mi = 0u; mi < state.match_count; ++mi) {
        G32SMatch m = matches[mi];
        if (m.directory_ordinal >= pc.directory_entry_count
            || m.query_minimizer_index >= pc.occurrence_count) {
            g32s_fail(G32S_DESCRIPTOR); continue;
        }
        if (occurrences[m.query_minimizer_index].accepted_count != 0u) {
            authorization_flags[m.directory_ordinal] = 1u;
        }
    }
    uint authorized = 0u;
    uint ordinal_digest_lo = 0u, ordinal_digest_hi = 0u;
    for (uint ordinal = 0u; ordinal < pc.directory_entry_count; ++ordinal) {
        if (authorization_flags[ordinal] != 0u) {
            // Compact in place.  The output cursor never passes the input
            // ordinal, so no unread authorization flag is overwritten.
            authorization_flags[authorized] = ordinal;
            ordinal_digest_lo ^= g32s_mix(ordinal ^ authorized);
            ordinal_digest_hi ^= g32s_mix(
                ordinal ^ (authorized * 0x9e3779b9u));
            ++authorized;
        }
    }
    uint lo = g32s_mix(cursor ^ pc.query_identity_lo
        ^ pc.control_state_identity_lo ^ authorized ^ ordinal_digest_lo);
    uint hi = g32s_mix(state.match_count ^ pc.query_identity_hi
        ^ pc.control_state_identity_hi ^ (authorized * 0x9e3779b9u)
        ^ ordinal_digest_hi);
    if ((lo | hi) == 0u) lo = 1u;
    stream_states[0].authorized_anchor_count = cursor;
    stream_states[0].authorized_partition_count = authorized;
    stream_states[0].authorization_digest_lo = lo;
    stream_states[0].authorization_digest_hi = hi;
    if (cursor > state.matched_anchor_upper_bound) {
        g32s_fail(G32S_COUNT_OVERFLOW);
    }
    memoryBarrierBuffer();
    stream_states[0].completion = stream_states[0].status == 0u ? 1u : 0u;
}
)GLSL";

constexpr std::string_view kPhaseCGrouped32StreamPositionMain = R"GLSL(
void main() {
    if (uint(stream_states.length()) == 0u
        || stream_states[0].completion != 1u
        || pc.partition_count > uint(partitions.length())
        || pc.position_count > uint(raw_words.length())
        || pc.anchor_capacity > uint(anchors.length())
        || pc.match_capacity > uint(matches.length())) {
        if (uint(stream_states.length()) != 0u) g32s_fail(G32S_PHASE);
        return;
    }
    uint stride = gl_NumWorkGroups.x * gl_WorkGroupSize.x;
    for (uint pi = gl_GlobalInvocationID.x; pi < pc.partition_count; pi += stride) {
        G32SPartition p = partitions[pi];
        bool valid = g32s_partition_common_valid(p, 2u)
            && p.control_offset == 0u
            && uint64_t(p.position_offset) + uint64_t(p.position_count)
                <= uint64_t(pc.position_count);
        uint previous = 0u;
        for (uint i = 0u; valid && i < p.position_count; ++i) {
            uint encoded = raw_words[p.position_offset + i];
            uint local_bp = encoded >> 1u;
            valid = uint64_t(local_bp) < p.segment_end_bp - p.segment_base_bp
                && (i == 0u || encoded > previous);
            previous = encoded;
        }
        if (!valid) g32s_fail(G32S_POSITION);
    }
    uint match_count = stream_states[0].match_count;
    for (uint mi = gl_GlobalInvocationID.x; mi < match_count; mi += stride) {
        G32SMatch m = matches[mi];
        uint found = 0xffffffffu;
        for (uint pi = 0u; pi < pc.partition_count; ++pi) {
            if (partitions[pi].directory_ordinal == m.directory_ordinal) {
                found = pi; break;
            }
        }
        if (found == 0xffffffffu) continue;
        G32SPartition p = partitions[found];
        if (!g32s_partition_common_valid(p, 2u)
            || m.query_minimizer_index >= pc.query_minimizer_count
            || uint64_t(m.position_offset) + uint64_t(m.position_count)
                > uint64_t(p.position_count)) {
            g32s_fail(G32S_POSITION); continue;
        }
        G32SQueryMinimizer q = query_minimizers[m.query_minimizer_index];
        if (!g32s_query_valid(m.query_minimizer_index, q)) continue;
        uint accepted = occurrences[q.occurrence_index].accepted_count;
        if (accepted == 0u) continue;
        uint emitted_before = atomicAdd(
            occurrences[q.occurrence_index].emitted_count, m.position_count);
        if (emitted_before > accepted || m.position_count > accepted - emitted_before) {
            g32s_fail(G32S_EMITTED_COUNT); continue;
        }
        uint anchor_output_offset =
            occurrences[q.occurrence_index].anchor_offset + emitted_before;
        G32SQueryRecord r = query_records[q.query_record_index];
        uint query_bp = q.query_bp_strand >> 1u;
        uint query_strand = q.query_bp_strand & 1u;
        uint64_t reverse_base = uint64_t(r.query_length)
            + uint64_t(pc.kmer_size) - uint64_t(2u);
        for (uint i = 0u; i < m.position_count; ++i) {
            uint encoded = raw_words[p.position_offset + m.position_offset + i];
            uint local_bp = encoded >> 1u;
            uint reverse = (encoded & 1u) ^ query_strand;
            uint64_t output_index =
                uint64_t(anchor_output_offset) + uint64_t(i);
            if (output_index >= uint64_t(pc.anchor_capacity)
                || uint64_t(local_bp) >= p.segment_end_bp - p.segment_base_bp) {
                g32s_fail(G32S_ANCHOR_CAPACITY); continue;
            }
            anchors[uint(output_index)].ref_position =
                p.segment_base_bp + uint64_t(local_bp);
            anchors[uint(output_index)].query_position = reverse != 0u
                ? reverse_base - uint64_t(query_bp) : uint64_t(query_bp);
            anchors[uint(output_index)].is_reverse = reverse;
            anchors[uint(output_index)].span_flags_segment = pc.kmer_size & 0xffu;
        }
        atomicAdd(stream_states[0].emitted_anchor_count, m.position_count);
    }
    if (gl_GlobalInvocationID.x == 0u) {
        atomicAdd(stream_states[0].position_groups_completed, 1u);
    }
}
)GLSL";

constexpr std::string_view kPhaseCGrouped32StreamSealMain = R"GLSL(
shared uint g32s_digest_lo[128];
shared uint g32s_digest_hi[128];
void main() {
    uint lane = gl_LocalInvocationID.x;
    uint dlo = 0u, dhi = 0u;
    if (uint(stream_states.length()) == 0u || gl_NumWorkGroups.x != 1u
        || stream_states[0].completion != 1u
        || pc.anchor_capacity > uint(anchors.length())) {
        if (uint(stream_states.length()) != 0u) g32s_fail(G32S_PHASE);
    } else {
        for (uint oi = lane; oi < pc.occurrence_count; oi += LOCAL_SIZE_X) {
            if (occurrences[oi].emitted_count != occurrences[oi].accepted_count) {
                g32s_fail(G32S_EMITTED_COUNT);
            }
        }
        for (uint ai = lane; ai < pc.anchor_capacity; ai += LOCAL_SIZE_X) {
            G32SAnchor a = anchors[ai];
            uint ref_lo = uint(a.ref_position);
            uint ref_hi = uint(a.ref_position >> 32u);
            uint query_lo = uint(a.query_position);
            uint query_hi = uint(a.query_position >> 32u);
            dlo ^= g32s_mix(ref_lo ^ query_hi ^ a.is_reverse ^ ai);
            dhi ^= g32s_mix(ref_hi ^ query_lo ^ a.span_flags_segment
                ^ (ai * 0x9e3779b9u));
        }
    }
    g32s_digest_lo[lane] = dlo; g32s_digest_hi[lane] = dhi;
    memoryBarrierBuffer(); barrier();
    for (uint width = LOCAL_SIZE_X >> 1u; width != 0u; width >>= 1u) {
        if (lane < width) {
            g32s_digest_lo[lane] ^= g32s_digest_lo[lane + width];
            g32s_digest_hi[lane] ^= g32s_digest_hi[lane + width];
        }
        barrier();
    }
    if (lane == 0u) {
        if (stream_states[0].position_groups_completed
                != pc.expected_position_groups
            || stream_states[0].emitted_anchor_count != pc.anchor_capacity
            || stream_states[0].authorized_anchor_count != pc.anchor_capacity) {
            g32s_fail(G32S_EMITTED_COUNT);
        }
        uint lo = g32s_digest_lo[0] ^ pc.target_buffer_identity_lo
            ^ stream_states[0].authorization_digest_lo;
        uint hi = g32s_digest_hi[0] ^ pc.target_buffer_identity_hi
            ^ stream_states[0].authorization_digest_hi;
        if (pc.anchor_capacity != 0u && (lo | hi) == 0u) lo = 1u;
        stream_states[0].anchor_digest_lo = lo;
        stream_states[0].anchor_digest_hi = hi;
        memoryBarrierBuffer();
        if (stream_states[0].status == 0u) stream_states[0].completion = 2u;
    }
}
)GLSL";

[[nodiscard]] std::string grouped32_stream_body(std::string_view stage_main) {
    std::string body;
    body.reserve(kPhaseCGrouped32StreamCommon.size() + stage_main.size());
    body += kPhaseCGrouped32StreamCommon;
    body += stage_main;
    return body;
}

/// DOC0310 §13 — device radix sort over parallel (key:uint64, value:uint64)
/// arrays. One 256-lane workgroup processes each contiguous chunk; lane 0
/// assigns stable scatter destinations and all lanes move the values. Histogram
/// pass: per-chunk 256-bin counts of digit `(key >> radix_shift) & 0xff`.
constexpr std::string_view kPhaseCSortHistogramBody = R"GLSL(
// Cooperative per-workgroup histogram: 256 threads (one per bin) zero and write
// the shared histogram; all threads count this workgroup's chunk into it with
// shared atomics. Replaces the 1-thread-per-workgroup version whose serial reads
// were a large fraction of the GPU index build's sort cost (DOC0311).
layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;
layout(std430, set = 0, binding = 0) readonly buffer KeysIn { uint64_t keys_in[]; };
layout(std430, set = 0, binding = 1) readonly buffer ValsIn { uint64_t vals_in[]; };
layout(std430, set = 0, binding = 2) buffer KeysOut { uint64_t keys_out[]; };
layout(std430, set = 0, binding = 3) buffer ValsOut { uint64_t vals_out[]; };
layout(std430, set = 0, binding = 4) buffer Histogram { uint histogram[]; };
layout(push_constant) uniform Params {
    uint element_count;
    uint radix_shift;
    uint num_workgroups;
    uint elems_per_wg;
    uint value_words;
} pc;
shared uint sh_hist[256];
void main() {
    uint wg = gl_WorkGroupID.x;
    if (wg >= pc.num_workgroups) return;
    uint tid = gl_LocalInvocationID.x;
    sh_hist[tid] = 0u;
    barrier();
    uint64_t lo64 = min(
        uint64_t(wg) * uint64_t(pc.elems_per_wg),
        uint64_t(pc.element_count));
    uint lo = uint(lo64);
    uint hi = uint(min(
        lo64 + uint64_t(pc.elems_per_wg), uint64_t(pc.element_count)));
    uint64_t chunk_n = uint64_t(hi) - uint64_t(lo);
    for (uint64_t off = uint64_t(tid); off < chunk_n; off += uint64_t(256)) {
        uint i = uint(lo64 + off);
        uint digit = uint((keys_in[i] >> pc.radix_shift) & uint64_t(0xff));
        atomicAdd(sh_hist[digit], 1u);
    }
    barrier();
    histogram[wg * 256u + tid] = sh_hist[tid];
}
)GLSL";

/// DOC0310 §13 — multi-level histogram scan. One 256-lane workgroup
/// computes, for every (workgroup, bin), its stable global start offset:
/// `bin_base[bin] + sum_{wg' < wg} hist[wg'][bin]`, where `bin_base` is the
/// exclusive prefix over bin totals. Writes the offsets back into `histogram`.
constexpr std::string_view kPhaseCSortScanBody = R"GLSL(
// One workgroup of 256 threads — one thread per radix bin. Each bin's thread
// reduces its column of the per-workgroup histograms and then writes that
// column's per-workgroup global offsets. This keeps the scan O(num_workgroups)
// per thread (256-way parallel) instead of O(num_workgroups * 256) on a single
// lane, which was the dominant fixed cost of the GPU index build.
layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;
layout(std430, set = 0, binding = 0) readonly buffer KeysIn { uint64_t keys_in[]; };
layout(std430, set = 0, binding = 1) readonly buffer ValsIn { uint64_t vals_in[]; };
layout(std430, set = 0, binding = 2) buffer KeysOut { uint64_t keys_out[]; };
layout(std430, set = 0, binding = 3) buffer ValsOut { uint64_t vals_out[]; };
layout(std430, set = 0, binding = 4) buffer Histogram { uint histogram[]; };
layout(push_constant) uniform Params {
    uint element_count;
    uint radix_shift;
    uint num_workgroups;
    uint elems_per_wg;
    uint value_words;
} pc;
shared uint sh_total[256];
shared uint sh_base[256];
void main() {
    uint b = gl_LocalInvocationID.x;          // radix bin (0..255)
    uint sum = 0u;
    for (uint wg = 0u; wg < pc.num_workgroups; ++wg) sum += histogram[wg * 256u + b];
    sh_total[b] = sum;
    barrier();
    if (b == 0u) {
        uint running = 0u;
        for (uint i = 0u; i < 256u; ++i) { sh_base[i] = running; running += sh_total[i]; }
    }
    barrier();
    uint acc = sh_base[b];
    for (uint wg = 0u; wg < pc.num_workgroups; ++wg) {
        uint idx = wg * 256u + b;
        uint c = histogram[idx];
        histogram[idx] = acc;
        acc += c;
    }
}
)GLSL";

/// DOC0310 §13 — stable scatter. Each 256-lane workgroup re-walks its
/// chunk in order, using the per-(wg,bin) global offsets from the scan as a
/// running cursor.
constexpr std::string_view kPhaseCSortScatterBody = R"GLSL(
// Cooperative stable scatter. Lane 0 walks the workgroup's chunk in order to
// assign each element its destination (= the per-bin global offset, advanced
// stably), which keeps the radix sort STABLE. All 256 lanes then perform the
// actual (uncoalesced) key/value writes in parallel — those scattered global
// writes were the dominant cost of the 1-lane scatter (DOC0311). The chunk is
// processed in SUB-BLOCKS of 512 so the stable-destination array sh_dst[512] is
// never overrun even when elems_per_wg > 512 — which occurs once num_workgroups
// is clamped to the device maxComputeWorkGroupCount[0] (the Vulkan minimum is
// 65535; Apple Silicon reports ~1e9 so a single block usually suffices, but the
// loop keeps it correct on any device). The per-bin cursor persists across
// sub-blocks, so LSD stability holds across the whole chunk.
layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;
layout(std430, set = 0, binding = 0) readonly buffer KeysIn { uint64_t keys_in[]; };
layout(std430, set = 0, binding = 1) readonly buffer ValsIn { uint64_t vals_in[]; };
layout(std430, set = 0, binding = 2) buffer KeysOut { uint64_t keys_out[]; };
layout(std430, set = 0, binding = 3) buffer ValsOut { uint64_t vals_out[]; };
layout(std430, set = 0, binding = 4) buffer Histogram { uint histogram[]; };
layout(push_constant) uniform Params {
    uint element_count;
    uint radix_shift;
    uint num_workgroups;
    uint elems_per_wg;
    uint value_words;
} pc;
shared uint sh_cursor[256];
shared uint sh_dst[512];
void main() {
    uint wg = gl_WorkGroupID.x;
    if (wg >= pc.num_workgroups) return;
    uint tid = gl_LocalInvocationID.x;
    uint64_t lo64 = uint64_t(wg) * uint64_t(pc.elems_per_wg);
    if (lo64 >= uint64_t(pc.element_count)) return;
    uint lo = uint(lo64);
    uint hi = uint(min(
        lo64 + uint64_t(pc.elems_per_wg), uint64_t(pc.element_count)));
    sh_cursor[tid] = histogram[wg * 256u + tid];
    barrier();
    uint vw = pc.value_words;
    for (uint block_lo = lo; block_lo < hi;) {
        uint block_hi = hi - block_lo < 512u ? hi : block_lo + 512u;
        uint block_n = block_hi - block_lo;
        if (tid == 0u) {
            for (uint i = 0u; i < block_n; ++i) {
                uint digit = uint((keys_in[block_lo + i] >> pc.radix_shift) & uint64_t(0xff));
                sh_dst[i] = sh_cursor[digit];
                sh_cursor[digit] = sh_cursor[digit] + 1u;
            }
        }
        barrier();
        for (uint i = tid; i < block_n; i += 256u) {
            uint dst = sh_dst[i];
            keys_out[dst] = keys_in[block_lo + i];
            for (uint w = 0u; w < vw; ++w) vals_out[dst * vw + w] = vals_in[(block_lo + i) * vw + w];
        }
        barrier();  // writes must finish before the next sub-block overwrites sh_dst
        block_lo = block_hi;
    }
}
)GLSL";

/// DOC0310 §12 — GPU target index build, run-marking. `flags[i] = 1` where a
/// new hash run begins in the sorted key array.
constexpr std::string_view kPhaseCIndexMarkRunsBody = R"GLSL(
layout(local_size_x = LOCAL_SIZE_X, local_size_y = 1, local_size_z = 1) in;
struct BucketRec { uint64_t hash; uint pos_offset; uint pos_count; };
struct SerializedBucket { uint64_t hash; uint pos_offset; uint pos_count; };
layout(std430, set = 0, binding = 0) readonly buffer SortedKeys { uint64_t sorted_keys[]; };
layout(std430, set = 0, binding = 1) buffer Flags { uint flags[]; };
layout(std430, set = 0, binding = 2) readonly buffer Scanned { uint scanned[]; };
layout(std430, set = 0, binding = 3) buffer Buckets { BucketRec buckets[]; };
layout(std430, set = 0, binding = 4) buffer SlotClaims { uint slot_claims[]; };
layout(std430, set = 0, binding = 5) buffer Slots { SerializedBucket slots[]; };
layout(std430, set = 0, binding = 6) buffer BucketSlot { uint bucket_slot[]; };
layout(push_constant) uniform Params {
    uint element_count;
    uint slot_mask;
    uint slot_capacity;
    uint num_buckets;
    uint probe_cap;
    uint pass;
} pc;
void main() {
    uint stride = gl_NumWorkGroups.x * gl_WorkGroupSize.x;
    for (uint i = gl_GlobalInvocationID.x;
         i < pc.element_count;
         i += stride) {
        flags[i] =
            (i == 0u || sorted_keys[i] != sorted_keys[i - 1u]) ? 1u : 0u;
    }
}
)GLSL";

/// DOC0310 §12 — dense bucket emit. Each element atomically adds to its bucket's
/// `pos_count`; run starts set the bucket `hash`/`pos_offset` (the sorted
/// position index). `bucket_index = inclusive_scan(flags) - 1`.
constexpr std::string_view kPhaseCIndexEmitBucketsBody = R"GLSL(
layout(local_size_x = LOCAL_SIZE_X, local_size_y = 1, local_size_z = 1) in;
struct BucketRec { uint64_t hash; uint pos_offset; uint pos_count; };
struct SerializedBucket { uint64_t hash; uint pos_offset; uint pos_count; };
layout(std430, set = 0, binding = 0) readonly buffer SortedKeys { uint64_t sorted_keys[]; };
layout(std430, set = 0, binding = 1) readonly buffer Flags { uint flags[]; };
layout(std430, set = 0, binding = 2) readonly buffer Scanned { uint scanned[]; };
layout(std430, set = 0, binding = 3) buffer Buckets { BucketRec buckets[]; };
layout(std430, set = 0, binding = 4) buffer SlotClaims { uint slot_claims[]; };
layout(std430, set = 0, binding = 5) buffer Slots { SerializedBucket slots[]; };
layout(std430, set = 0, binding = 6) buffer BucketSlot { uint bucket_slot[]; };
layout(push_constant) uniform Params {
    uint element_count;
    uint slot_mask;
    uint slot_capacity;
    uint num_buckets;
    uint probe_cap;
    uint pass;
} pc;
void main() {
    uint stride = gl_NumWorkGroups.x * gl_WorkGroupSize.x;
    for (uint i = gl_GlobalInvocationID.x;
         i < pc.element_count;
         i += stride) {
        uint bidx = scanned[i] + flags[i] - 1u;
        atomicAdd(buckets[bidx].pos_count, 1u);
        if (flags[i] == 1u) {
            buckets[bidx].hash = sorted_keys[i];
            buckets[bidx].pos_offset = i;
        }
    }
}
)GLSL";

/// DOC0310 §12 — race-free slot claim via 32-bit atomicCompSwap linear probe.
constexpr std::string_view kPhaseCIndexClaimSlotsBody = R"GLSL(
layout(local_size_x = LOCAL_SIZE_X, local_size_y = 1, local_size_z = 1) in;
struct BucketRec { uint64_t hash; uint pos_offset; uint pos_count; };
struct SerializedBucket { uint64_t hash; uint pos_offset; uint pos_count; };
layout(std430, set = 0, binding = 0) readonly buffer SortedKeys { uint64_t sorted_keys[]; };
layout(std430, set = 0, binding = 1) buffer Flags { uint flags[]; };
layout(std430, set = 0, binding = 2) readonly buffer Scanned { uint scanned[]; };
layout(std430, set = 0, binding = 3) readonly buffer Buckets { BucketRec buckets[]; };
layout(std430, set = 0, binding = 4) buffer SlotClaims { uint slot_claims[]; };
layout(std430, set = 0, binding = 5) buffer Slots { SerializedBucket slots[]; };
layout(std430, set = 0, binding = 6) buffer BucketSlot { uint bucket_slot[]; };
layout(push_constant) uniform Params {
    uint element_count;
    uint slot_mask;
    uint slot_capacity;
    uint num_buckets;
    uint probe_cap;
    uint pass;
} pc;
uint64_t splitmix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15UL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9UL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebUL;
    return x ^ (x >> 31);
}
void main() {
    uint b = gl_GlobalInvocationID.x;
    if (b >= pc.num_buckets) return;
    uint p = uint(splitmix64(buckets[b].hash)) & pc.slot_mask;
    for (uint step = 0u; step < pc.probe_cap; ++step) {
        uint old = atomicCompSwap(slot_claims[p], 0xffffffffu, b);
        if (old == 0xffffffffu) { bucket_slot[b] = p; return; }
        p = (p + 1u) & pc.slot_mask;
    }
    bucket_slot[b] = 0xffffffffu;
}
)GLSL";

/// DOC0310 §12 — write the SerializedBucket into its claimed slot.
constexpr std::string_view kPhaseCIndexEmitSlotsBody = R"GLSL(
layout(local_size_x = LOCAL_SIZE_X, local_size_y = 1, local_size_z = 1) in;
struct BucketRec { uint64_t hash; uint pos_offset; uint pos_count; };
struct SerializedBucket { uint64_t hash; uint pos_offset; uint pos_count; };
layout(std430, set = 0, binding = 0) readonly buffer SortedKeys { uint64_t sorted_keys[]; };
layout(std430, set = 0, binding = 1) buffer Flags { uint flags[]; };
layout(std430, set = 0, binding = 2) readonly buffer Scanned { uint scanned[]; };
layout(std430, set = 0, binding = 3) readonly buffer Buckets { BucketRec buckets[]; };
layout(std430, set = 0, binding = 4) buffer SlotClaims { uint slot_claims[]; };
layout(std430, set = 0, binding = 5) buffer Slots { SerializedBucket slots[]; };
layout(std430, set = 0, binding = 6) readonly buffer BucketSlot { uint bucket_slot[]; };
layout(push_constant) uniform Params {
    uint element_count;
    uint slot_mask;
    uint slot_capacity;
    uint num_buckets;
    uint probe_cap;
    uint pass;
} pc;
void main() {
    uint b = gl_GlobalInvocationID.x;
    if (b >= pc.num_buckets) return;
    uint p = bucket_slot[b];
    if (p == 0xffffffffu) return;
    slots[p].hash = buckets[b].hash;
    slots[p].pos_offset = buckets[b].pos_offset;
    slots[p].pos_count = buckets[b].pos_count;
}
)GLSL";

/// DOC0310 §15 — chain range build. One workgroup (1 invocation) per query
/// serially scans that query's sorted anchor span, emitting `GpuChainRange`
/// records at strand/chromosome changes and hard reference gaps, matching the
/// CPU `build_gpu_chain_ranges` predicate. When any hard range is very long,
/// the shader also performs the same endpoint-preserving thinning used by the
/// CPU-side Vulkan chainer so resident seed-to-coordinate does not feed huge
/// dense ranges into DP.
constexpr std::string_view kPhaseCChainRangeBuildBody = R"GLSL(
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
struct AnchorWire { uint64_t ref_position; uint64_t query_position; uint is_reverse; uint span_flags_segment; };
struct ChainRangeWire { uint begin; uint range_len; };
struct QuerySpan { uint anchor_lo; uint anchor_hi; uint range_lo; uint range_out; };
layout(std430, set = 0, binding = 0) readonly buffer Anchors { AnchorWire anchors[]; };
layout(std430, set = 0, binding = 1) buffer Counts { uint counts[]; };
layout(std430, set = 0, binding = 2) buffer RangesOut { ChainRangeWire ranges[]; };
layout(std430, set = 0, binding = 3) buffer Unused3 { uint unused3[]; };
layout(std430, set = 0, binding = 4) buffer CompactedAnchors { AnchorWire compacted_anchors[]; };
layout(std430, set = 0, binding = 5) readonly buffer QuerySpans { QuerySpan spans[]; };
layout(push_constant) uniform Params {
    uint query_count;
    uint max_ref_gap;
} pc;
const uint COMPACT_RANGE_THRESHOLD = 131072u;
const uint COMPACT_RANGE_TARGET = 65536u;
uint chrom_key(AnchorWire a) { return (a.is_reverse != 0u) ? 1u : 0u; }
bool range_break(AnchorWire prev, AnchorWire cur) {
    bool key_changed = chrom_key(cur) != chrom_key(prev);
    bool hard_gap = (cur.ref_position > prev.ref_position)
        && ((cur.ref_position - prev.ref_position) > uint64_t(pc.max_ref_gap));
    return key_changed || hard_gap;
}
uint compact_stride(uint range_len) {
    if (range_len <= COMPACT_RANGE_THRESHOLD) return 1u;
    uint stride = (range_len + COMPACT_RANGE_TARGET - 1u) / COMPACT_RANGE_TARGET;
    return max(stride, 1u);
}
uint emit_compacted_range(uint src_begin, uint src_end, uint range_index, uint anchor_out) {
    uint range_len = src_end - src_begin;
    uint stride = compact_stride(range_len);
    uint dst_begin = anchor_out;
    for (uint j = 0u; j < range_len; ++j) {
        if ((j % stride) == 0u || j + 1u == range_len) {
            compacted_anchors[anchor_out] = anchors[src_begin + j];
            anchor_out += 1u;
        }
    }
    ranges[range_index].begin = dst_begin;
    ranges[range_index].range_len = anchor_out - dst_begin;
    return anchor_out;
}
void main() {
    uint qi = gl_WorkGroupID.x;
    if (qi >= pc.query_count) return;
    QuerySpan s = spans[qi];
    uint lo = s.anchor_lo;
    uint hi = s.anchor_hi;
    uint out_idx = s.range_lo;
    uint produced = 0u;
    uint count_idx = qi * 2u;
    if (lo >= hi) {
        counts[count_idx] = 0u;
        counts[count_idx + 1u] = 0u;
        return;
    }
    uint begin = lo;
    bool needs_compaction = false;
    for (uint i = lo + 1u; i < hi; ++i) {
        AnchorWire prev = anchors[i - 1u];
        AnchorWire cur = anchors[i];
        if (range_break(prev, cur)) {
            uint range_len = i - begin;
            needs_compaction = needs_compaction || range_len > COMPACT_RANGE_THRESHOLD;
            ranges[out_idx + produced].begin = begin;
            ranges[out_idx + produced].range_len = range_len;
            produced += 1u;
            begin = i;
        }
    }
    uint last_len = hi - begin;
    needs_compaction = needs_compaction || last_len > COMPACT_RANGE_THRESHOLD;
    ranges[out_idx + produced].begin = begin;
    ranges[out_idx + produced].range_len = last_len;
    produced += 1u;
    counts[count_idx] = produced;
    counts[count_idx + 1u] = 0u;

    if (!needs_compaction) return;

    produced = 0u;
    uint anchor_out = s.range_out;
    begin = lo;
    for (uint i = lo + 1u; i < hi; ++i) {
        AnchorWire prev = anchors[i - 1u];
        AnchorWire cur = anchors[i];
        if (range_break(prev, cur)) {
            anchor_out = emit_compacted_range(
                begin, i, out_idx + produced, anchor_out);
            produced += 1u;
            begin = i;
        }
    }
    anchor_out = emit_compacted_range(begin, hi, out_idx + produced, anchor_out);
    counts[count_idx + 1u] = anchor_out - s.range_out;
}
)GLSL";

/// Resident coordinate-only finalizer. It mirrors the bounded top-k CPU
/// coordinate extractor, but downloads only one compact mapping record.
constexpr std::string_view kPhaseCCoordinateReduceBody = R"GLSL(
layout(local_size_x = LOCAL_SIZE_X, local_size_y = 1, local_size_z = 1) in;

struct AnchorWire {
    uint64_t ref_position;
    uint64_t query_position;
    uint is_reverse;
    uint span_flags_segment;
};

struct CoordinateResult {
    uint64_t query_start;
    uint64_t query_end;
    uint64_t target_start;
    uint64_t target_end;
    uint64_t match_length;
    uint64_t block_length;
    int chain_score;
    int seed_count;
    uint is_reverse;
    uint valid;
};

layout(std430, set = 0, binding = 0) readonly buffer Anchors { AnchorWire anchors[]; };
layout(std430, set = 0, binding = 1) readonly buffer BestScores { int best_scores[]; };
layout(std430, set = 0, binding = 2) readonly buffer Predecessors { int64_t predecessors[]; };
layout(std430, set = 0, binding = 3) buffer Visited { uint visited[]; };
layout(std430, set = 0, binding = 4) buffer Trace { uint trace[]; };
layout(std430, set = 0, binding = 5) buffer Result { CoordinateResult result; };

layout(push_constant) uniform Params {
    uint anchor_count;
    uint query_len_lo;
    uint query_len_hi;
    int min_chain_score;
    int min_seed_count;
    uint probe_limit;
    uint reserved;
} pc;

shared int sh_score[LOCAL_SIZE_X];
shared uint sh_index[LOCAL_SIZE_X];
shared int selected_score;
shared uint selected_anchor;
shared uint selected_valid;
shared uint done;

uint anchor_span(AnchorWire a) { return a.span_flags_segment & 0xffu; }

uint64_t query_len() {
    return uint64_t(pc.query_len_lo) | (uint64_t(pc.query_len_hi) << 32);
}

bool better_candidate(int score, uint idx, int best_score, uint best_idx) {
    return score > best_score || (score == best_score && idx > best_idx);
}

void write_result(uint chain_len, int candidate_score) {
    uint first_idx = trace[chain_len - 1u];
    uint last_idx = trace[0u];
    AnchorWire first_anchor = anchors[first_idx];
    AnchorWire last_anchor = anchors[last_idx];
    uint first_span_u = anchor_span(first_anchor);
    int64_t first_span = int64_t(first_span_u);

    int64_t ref_start_i = int64_t(first_anchor.ref_position) + int64_t(1) - first_span;
    if (ref_start_i < int64_t(0)) ref_start_i = int64_t(0);
    uint64_t ref_end = last_anchor.ref_position + uint64_t(1);

    int64_t raw_q_start = int64_t(first_anchor.query_position) + int64_t(1) - first_span;
    if (raw_q_start < int64_t(0)) raw_q_start = int64_t(0);
    uint64_t raw_q_end = last_anchor.query_position + uint64_t(1);

    uint64_t q_start;
    uint64_t q_end;
    if (first_anchor.is_reverse == 0u) {
        q_start = uint64_t(raw_q_start);
        q_end = raw_q_end;
    } else {
        uint64_t qlen = query_len();
        q_start = qlen - raw_q_end;
        q_end = qlen - uint64_t(raw_q_start);
    }

    int64_t match_len = int64_t(first_span_u);
    int64_t block_len = int64_t(first_span_u);
    uint prev_idx = first_idx;
    int t = int(chain_len) - 2;
    while (t >= 0) {
        uint cur_idx = trace[uint(t)];
        AnchorWire prev = anchors[prev_idx];
        AnchorWire cur = anchors[cur_idx];
        int span = int(anchor_span(cur));
        int target_gap = int(cur.ref_position - prev.ref_position);
        int query_gap = int(cur.query_position - prev.query_position);
        int max_gap = target_gap > query_gap ? target_gap : query_gap;
        int min_gap = target_gap < query_gap ? target_gap : query_gap;
        block_len += int64_t(max_gap);
        match_len += int64_t((target_gap > span && query_gap > span) ? span : min_gap);
        prev_idx = cur_idx;
        --t;
    }

    result.query_start = q_start;
    result.query_end = q_end;
    result.target_start = uint64_t(ref_start_i);
    result.target_end = ref_end;
    result.match_length = uint64_t(match_len);
    result.block_length = uint64_t(block_len);
    result.chain_score = candidate_score;
    result.seed_count = int(chain_len);
    result.is_reverse = first_anchor.is_reverse != 0u ? 1u : 0u;
    result.valid = 1u;
}

void main() {
    uint lane = gl_LocalInvocationID.x;
    if (lane == 0u) {
        result.valid = 0u;
        done = 0u;
    }
    barrier();

    for (uint attempt = 0u; attempt < pc.probe_limit; ++attempt) {
        if (done != 0u) break;

        int local_score = pc.min_chain_score - 1;
        uint local_idx = 0u;
        for (uint i = lane; i < pc.anchor_count; i += LOCAL_SIZE_X) {
            int score = best_scores[i];
            if (visited[i] == 0u && better_candidate(score, i, local_score, local_idx)) {
                local_score = score;
                local_idx = i;
            }
        }
        sh_score[lane] = local_score;
        sh_index[lane] = local_idx;
        barrier();

        if (lane == 0u) {
            int best_score = pc.min_chain_score - 1;
            uint best_idx = 0u;
            uint valid = 0u;
            for (uint i = 0u; i < LOCAL_SIZE_X; ++i) {
                if (better_candidate(sh_score[i], sh_index[i], best_score, best_idx)) {
                    best_score = sh_score[i];
                    best_idx = sh_index[i];
                    valid = 1u;
                }
            }
            selected_score = best_score;
            selected_anchor = best_idx;
            selected_valid = valid;
        }
        barrier();

        if (lane == 0u) {
            if (selected_valid == 0u || selected_score < pc.min_chain_score) {
                done = 1u;
            } else {
                uint chain_len = 0u;
                uint stop_idx = 0xffffffffu;
                int64_t trace_idx = int64_t(selected_anchor);
                while (trace_idx >= int64_t(0)
                       && uint64_t(trace_idx) < uint64_t(pc.anchor_count)
                       && chain_len < pc.anchor_count) {
                    uint idx = uint(trace_idx);
                    if (visited[idx] != 0u) {
                        stop_idx = idx;
                        break;
                    }
                    trace[chain_len] = idx;
                    ++chain_len;
                    int64_t next_idx = predecessors[idx];
                    if (next_idx >= trace_idx) {
                        chain_len = 0u;
                        break;
                    }
                    trace_idx = next_idx;
                }

                int candidate_score = selected_score;
                if (stop_idx != 0xffffffffu) {
                    candidate_score -= best_scores[stop_idx];
                }

                if (candidate_score >= pc.min_chain_score
                    && chain_len >= uint(pc.min_seed_count)) {
                    write_result(chain_len, candidate_score);
                    done = 1u;
                } else {
                    for (uint i = 0u; i < chain_len; ++i) {
                        visited[trace[i]] = 1u;
                    }
                }
            }
        }
        barrier();
    }
}
)GLSL";

constexpr std::string_view kPhaseCCoordinateVoteReduceBody = R"GLSL(
layout(local_size_x = LOCAL_SIZE_X, local_size_y = 1, local_size_z = 1) in;

struct AnchorWire {
    uint64_t ref_position;
    uint64_t query_position;
    uint is_reverse;
    uint span_flags_segment;
};

struct CoordinateResult {
    uint64_t query_start;
    uint64_t query_end;
    uint64_t target_start;
    uint64_t target_end;
    uint64_t match_length;
    uint64_t block_length;
    int chain_score;
    int seed_count;
    uint is_reverse;
    uint valid;
};

layout(std430, set = 0, binding = 0) readonly buffer Anchors { AnchorWire anchors[]; };
layout(std430, set = 0, binding = 1) readonly buffer Unused1 { int unused1[]; };
layout(std430, set = 0, binding = 2) readonly buffer Unused2 { int64_t unused2[]; };
layout(std430, set = 0, binding = 3) buffer Unused3 { uint unused3[]; };
layout(std430, set = 0, binding = 4) buffer Unused4 { uint unused4[]; };
layout(std430, set = 0, binding = 5) buffer Result { CoordinateResult result; };

layout(push_constant) uniform Params {
    uint anchor_count;
    uint query_len_lo;
    uint query_len_hi;
    int min_chain_score;
    int min_seed_count;
    uint probe_limit;
    uint reserved;
} pc;

shared int sh_score[LOCAL_SIZE_X];
shared uint sh_index[LOCAL_SIZE_X];
shared int selected_score;
shared uint selected_anchor;

uint anchor_span(AnchorWire a) { return a.span_flags_segment & 0xffu; }

uint64_t query_len() {
    return uint64_t(pc.query_len_lo) | (uint64_t(pc.query_len_hi) << 32);
}

int64_t anchor_diag(AnchorWire a) {
    return int64_t(a.ref_position) - int64_t(a.query_position);
}

bool better_candidate(int score, uint idx, int best_score, uint best_idx) {
    return score > best_score || (score == best_score && idx < best_idx);
}

void write_vote_result(uint idx, int score) {
    AnchorWire a = anchors[idx];
    uint span = anchor_span(a);
    int64_t diag = anchor_diag(a);
    if (diag < int64_t(0)) {
        diag = int64_t(0);
    }
    uint64_t qlen = query_len();
    uint64_t target_start = uint64_t(diag);
    result.query_start = uint64_t(0);
    result.query_end = qlen;
    result.target_start = target_start;
    result.target_end = target_start + qlen;
    result.match_length = qlen;
    result.block_length = qlen;
    result.chain_score = score * int(span == 0u ? 1u : span);
    result.seed_count = score;
    result.is_reverse = a.is_reverse != 0u ? 1u : 0u;
    result.valid = 1u;
}

void main() {
    uint lane = gl_LocalInvocationID.x;
    if (lane == 0u) {
        result.valid = 0u;
        selected_score = 0;
        selected_anchor = 0u;
    }
    barrier();

    int local_score = 0;
    uint local_idx = 0u;
    for (uint i = lane; i < pc.anchor_count; i += LOCAL_SIZE_X) {
        AnchorWire cand = anchors[i];
        int64_t cand_diag = anchor_diag(cand);
        int score = 0;
        for (uint j = 0u; j < pc.anchor_count; ++j) {
            AnchorWire other = anchors[j];
            if (other.is_reverse != cand.is_reverse) {
                continue;
            }
            int64_t d = anchor_diag(other) - cand_diag;
            if (d < int64_t(0)) {
                d = -d;
            }
            if (d <= int64_t(50)) {
                ++score;
            }
        }
        if (better_candidate(score, i, local_score, local_idx)) {
            local_score = score;
            local_idx = i;
        }
    }
    sh_score[lane] = local_score;
    sh_index[lane] = local_idx;
    barrier();

    if (lane == 0u) {
        int best_score = 0;
        uint best_idx = 0u;
        for (uint i = 0u; i < LOCAL_SIZE_X; ++i) {
            if (better_candidate(sh_score[i], sh_index[i], best_score, best_idx)) {
                best_score = sh_score[i];
                best_idx = sh_index[i];
            }
        }
        selected_score = best_score;
        selected_anchor = best_idx;
    }
    barrier();

    if (lane == 0u) {
        if (pc.anchor_count != 0u
            && selected_score >= pc.min_seed_count
            && selected_score * int(anchor_span(anchors[selected_anchor])) >= pc.min_chain_score) {
            write_vote_result(selected_anchor, selected_score);
        }
    }
}
)GLSL";

constexpr std::string_view kPhaseCCoordinateVoteReduceBatchBody = R"GLSL(
layout(local_size_x = LOCAL_SIZE_X, local_size_y = 1, local_size_z = 1) in;

struct AnchorWire {
    uint64_t ref_position;
    uint64_t query_position;
    uint is_reverse;
    uint span_flags_segment;
};

struct CoordinateResult {
    uint64_t query_start;
    uint64_t query_end;
    uint64_t target_start;
    uint64_t target_end;
    uint64_t match_length;
    uint64_t block_length;
    int chain_score;
    int seed_count;
    uint is_reverse;
    uint valid;
};

struct VoteJob {
    uint anchor_offset;
    uint anchor_count;
    uint query_len_lo;
    uint query_len_hi;
};

layout(std430, set = 0, binding = 0) readonly buffer Anchors { AnchorWire anchors[]; };
layout(std430, set = 0, binding = 1) readonly buffer VoteJobs { VoteJob jobs[]; };
layout(std430, set = 0, binding = 2) readonly buffer Unused2 { int unused2[]; };
layout(std430, set = 0, binding = 3) buffer Unused3 { uint unused3[]; };
layout(std430, set = 0, binding = 4) buffer Unused4 { uint unused4[]; };
layout(std430, set = 0, binding = 5) buffer Results { CoordinateResult results[]; };

layout(push_constant) uniform Params {
    uint job_count;
    uint unused_query_len_lo;
    uint unused_query_len_hi;
    int min_chain_score;
    int min_seed_count;
    uint probe_limit;
    uint reserved;
} pc;

const uint VOTE_SAMPLE_LIMIT = 32u;

shared int sh_score[LOCAL_SIZE_X];
shared uint sh_index[LOCAL_SIZE_X];
shared int selected_score;
shared uint selected_anchor;

uint anchor_span(AnchorWire a) { return a.span_flags_segment & 0xffu; }

uint64_t query_len(VoteJob job) {
    return uint64_t(job.query_len_lo) | (uint64_t(job.query_len_hi) << 32);
}

AnchorWire job_anchor(VoteJob job, uint idx) {
    return anchors[job.anchor_offset + idx];
}

int64_t anchor_diag(AnchorWire a) {
    return int64_t(a.ref_position) - int64_t(a.query_position);
}

bool better_candidate(int score, uint idx, int best_score, uint best_idx) {
    return score > best_score || (score == best_score && idx < best_idx);
}

void write_vote_result(uint job_id, VoteJob job, uint idx, int score) {
    AnchorWire a = job_anchor(job, idx);
    uint span = anchor_span(a);
    int64_t diag = anchor_diag(a);
    if (diag < int64_t(0)) {
        diag = int64_t(0);
    }
    uint64_t qlen = query_len(job);
    uint64_t target_start = uint64_t(diag);
    results[job_id].query_start = uint64_t(0);
    results[job_id].query_end = qlen;
    results[job_id].target_start = target_start;
    results[job_id].target_end = target_start + qlen;
    results[job_id].match_length = qlen;
    results[job_id].block_length = qlen;
    results[job_id].chain_score = score * int(span == 0u ? 1u : span);
    results[job_id].seed_count = score;
    results[job_id].is_reverse = a.is_reverse != 0u ? 1u : 0u;
    results[job_id].valid = 1u;
}

void main() {
    uint job_id = gl_WorkGroupID.x;
    if (job_id >= pc.job_count) return;

    VoteJob job = jobs[job_id];
    uint lane = gl_LocalInvocationID.x;
    if (lane == 0u) {
        results[job_id].valid = 0u;
        selected_score = 0;
        selected_anchor = 0u;
    }
    barrier();

    uint sample_count = min(job.anchor_count, VOTE_SAMPLE_LIMIT);
    int local_score = 0;
    uint local_idx = 0u;
    for (uint i = lane; i < job.anchor_count; i += LOCAL_SIZE_X) {
        AnchorWire cand = job_anchor(job, i);
        int64_t cand_diag = anchor_diag(cand);
        int score = 0;
        for (uint s = 0u; s < sample_count; ++s) {
            uint j = (s * job.anchor_count) / sample_count;
            AnchorWire other = job_anchor(job, j);
            if (other.is_reverse != cand.is_reverse) {
                continue;
            }
            int64_t d = anchor_diag(other) - cand_diag;
            if (d < int64_t(0)) {
                d = -d;
            }
            if (d <= int64_t(50)) {
                ++score;
            }
        }
        if (better_candidate(score, i, local_score, local_idx)) {
            local_score = score;
            local_idx = i;
        }
    }
    sh_score[lane] = local_score;
    sh_index[lane] = local_idx;
    barrier();

    if (lane == 0u) {
        int best_score = 0;
        uint best_idx = 0u;
        for (uint i = 0u; i < LOCAL_SIZE_X; ++i) {
            if (better_candidate(sh_score[i], sh_index[i], best_score, best_idx)) {
                best_score = sh_score[i];
                best_idx = sh_index[i];
            }
        }
        selected_score = best_score;
        selected_anchor = best_idx;
    }
    barrier();

    if (lane == 0u) {
        uint estimated_score = sample_count == 0u
            ? 0u
            : (uint(selected_score) * job.anchor_count + sample_count - 1u) / sample_count;
        if (job.anchor_count != 0u
            && estimated_score >= uint(pc.min_seed_count)
            && int(estimated_score) * int(anchor_span(job_anchor(job, selected_anchor)))
                >= pc.min_chain_score) {
            write_vote_result(job_id, job, selected_anchor, int(estimated_score));
        }
    }
}
)GLSL";

/// Score/end-point-only banded affine extension. This prototype runs one
/// scalar DP per job and dispatches the batch as grid-strided workgroups. The
/// ABI is intentionally symbol-rank based (`uint` per symbol) so parent
/// integration can feed any alphabet without dna4-specific decoding here.
constexpr std::string_view kAlignmentExtensionBandedAffineBody = R"GLSL(
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

const int NEG_INF = -1073741824;
const uint STATUS_OK = 0u;
const uint STATUS_UNSUPPORTED_GEOMETRY = 1u;

struct AlignmentExtensionJob {
    uint query_offset;
    uint target_offset;
    uint query_len;
    uint target_len;
    uint band_width;
    uint flags;
    uint reserved0;
    uint reserved1;
};

struct AlignmentExtensionResult {
    int score;
    uint query_end;
    uint target_end;
    uint status;
};

layout(std430, set = 0, binding = 0) readonly buffer QuerySymbols { uint query_symbols[]; };
layout(std430, set = 0, binding = 1) readonly buffer TargetSymbols { uint target_symbols[]; };
layout(std430, set = 0, binding = 2) readonly buffer Jobs { AlignmentExtensionJob jobs[]; };
layout(std430, set = 0, binding = 3) buffer Results { AlignmentExtensionResult results[]; };

layout(push_constant) uniform Params {
    uint job_count;
    uint max_query_len;
    uint max_target_len;
    uint max_band_width;
    uint alphabet_size;
    int match_score;
    int mismatch_penalty;
    int gap_open_penalty;
    int gap_extend_penalty;
    uint flags;
    uint reserved0;
} pc;

int bioseq_max3(int a, int b, int c) {
    return max(max(a, b), c);
}

int substitution_score(uint query_symbol, uint target_symbol) {
    return query_symbol == target_symbol ? pc.match_score : -pc.mismatch_penalty;
}

void reject_job(uint job_index) {
    results[job_index].score = 0;
    results[job_index].query_end = 0u;
    results[job_index].target_end = 0u;
    results[job_index].status = STATUS_UNSUPPORTED_GEOMETRY;
}

void solve_job(uint job_index) {
    AlignmentExtensionJob job = jobs[job_index];
    if (job.query_len > pc.max_query_len || job.target_len > pc.max_target_len
        || job.band_width > pc.max_band_width || job.flags != 0u
        || job.reserved0 != 0u || job.reserved1 != 0u
        || pc.flags != 0u || pc.reserved0 != 0u || pc.alphabet_size == 0u) {
        reject_job(job_index);
        return;
    }

    int prev_m[MAX_SPAN + 1];
    int prev_i[MAX_SPAN + 1];
    int prev_d[MAX_SPAN + 1];
    int curr_m[MAX_SPAN + 1];
    int curr_i[MAX_SPAN + 1];
    int curr_d[MAX_SPAN + 1];

    for (uint j = 0u; j <= job.target_len; ++j) {
        prev_m[j] = 0;
        prev_i[j] = NEG_INF;
        prev_d[j] = NEG_INF;
        curr_m[j] = 0;
        curr_i[j] = NEG_INF;
        curr_d[j] = NEG_INF;
    }

    int best_score = 0;
    uint best_query_end = 0u;
    uint best_target_end = 0u;
    int gap_open_total = pc.gap_open_penalty + pc.gap_extend_penalty;

    for (uint i = 1u; i <= job.query_len; ++i) {
        curr_m[0] = 0;
        curr_i[0] = NEG_INF;
        curr_d[0] = NEG_INF;
        uint query_symbol = query_symbols[job.query_offset + i - 1u];
        if (query_symbol >= pc.alphabet_size) {
            reject_job(job_index);
            return;
        }
        for (uint j = 1u; j <= job.target_len; ++j) {
            uint delta = i > j ? (i - j) : (j - i);
            if (delta > job.band_width) {
                curr_m[j] = 0;
                curr_i[j] = NEG_INF;
                curr_d[j] = NEG_INF;
                continue;
            }

            uint target_symbol = target_symbols[job.target_offset + j - 1u];
            if (target_symbol >= pc.alphabet_size) {
                reject_job(job_index);
                return;
            }
            int diag = bioseq_max3(
                prev_m[j - 1u], prev_i[j - 1u], prev_d[j - 1u])
                + substitution_score(query_symbol, target_symbol);
            int match_state = max(0, diag);
            int insert_state = max(
                max(prev_m[j] - gap_open_total, prev_i[j] - pc.gap_extend_penalty), 0);
            int delete_state = max(
                max(curr_m[j - 1u] - gap_open_total, curr_d[j - 1u] - pc.gap_extend_penalty), 0);

            curr_m[j] = match_state;
            curr_i[j] = insert_state;
            curr_d[j] = delete_state;

            int cell_best = bioseq_max3(
                match_state, insert_state, delete_state);
            if (cell_best > best_score) {
                best_score = cell_best;
                best_query_end = i;
                best_target_end = j;
            }
        }
        for (uint j = 0u; j <= job.target_len; ++j) {
            prev_m[j] = curr_m[j];
            prev_i[j] = curr_i[j];
            prev_d[j] = curr_d[j];
        }
    }

    results[job_index].score = best_score;
    results[job_index].query_end = best_query_end;
    results[job_index].target_end = best_target_end;
    results[job_index].status = STATUS_OK;
}

void main() {
    uint stride = gl_NumWorkGroups.x;
    if (stride == 0u) return;
    for (uint job_index = gl_WorkGroupID.x; job_index < pc.job_count; job_index += stride) {
        solve_job(job_index);
    }
}
)GLSL";

constexpr std::string_view kAlignmentExtensionSimpleCigarBody = R"GLSL(
layout(local_size_x = LOCAL_SIZE_X, local_size_y = 1, local_size_z = 1) in;

const int NEG_INF = -1073741824;
const uint STATUS_OK = 0u;
const uint STATUS_UNSUPPORTED_GEOMETRY = 1u;
const uint STATUS_CIGAR_OVERFLOW = 2u;
const uint CIGAR_MATCH = 0u;
const uint CIGAR_INS = 1u;
const uint CIGAR_DEL = 2u;
const uint SIMPLE_GAP_MAX = 256u;
const uint SIMPLE_GAP_OP_MAX = 64u;
const uint SIMPLE_GAP_ANCHOR_MIN = 8u;

struct AlignmentCigarJob {
    uint query_symbol_offset;
    uint target_symbol_offset;
    uint query_len;
    uint target_len;
    uint query_global_start;
    uint target_global_start;
    uint band_width;
    uint flags;
    uint cigar_offset;
    uint cigar_capacity;
    uint traceback_tile_offset;
    uint traceback_tile_count;
};

struct AlignmentCigarResult {
    int score;
    int max_score;
    uint query_start;
    uint query_end;
    uint target_start;
    uint target_end;
    uint cigar_offset;
    uint cigar_length;
    uint status;
    uint reserved;
};

layout(std430, set = 0, binding = 0) readonly buffer QuerySymbols { uint query_symbols[]; };
layout(std430, set = 0, binding = 1) readonly buffer TargetSymbols { uint target_symbols[]; };
layout(std430, set = 0, binding = 2) readonly buffer Jobs { AlignmentCigarJob jobs[]; };
layout(std430, set = 0, binding = 3) buffer Results { AlignmentCigarResult results[]; };
layout(std430, set = 0, binding = 4) buffer PackedCigar { uint packed_cigar[]; };
layout(std430, set = 0, binding = 5) buffer Traceback { uint traceback[]; };

layout(push_constant) uniform Params {
    uint job_count;
    uint alphabet_size;
    uint max_band_width;
    uint tile_query;
    uint tile_target;
    int match_score;
    int mismatch_penalty;
    int gap_open_short;
    int gap_extend_short;
    int gap_open_long;
    int gap_extend_long;
    int z_drop;
    int end_bonus;
    uint flags;
    uint reserved;
} pc;

void write_reject(uint job_index, uint status) {
    results[job_index].score = 0;
    results[job_index].max_score = 0;
    results[job_index].query_start = 0u;
    results[job_index].query_end = 0u;
    results[job_index].target_start = 0u;
    results[job_index].target_end = 0u;
    results[job_index].cigar_offset = 0u;
    results[job_index].cigar_length = 0u;
    results[job_index].status = status;
    results[job_index].reserved = 0u;
}

int substitution_score(uint query_symbol, uint target_symbol) {
    return query_symbol == target_symbol ? pc.match_score : -pc.mismatch_penalty;
}

int gap_cost_short(uint len) {
    return pc.gap_open_short + pc.gap_extend_short * int(len);
}

int gap_cost_long(uint len) {
    return pc.gap_open_long + pc.gap_extend_long * int(len);
}

uint traceback_index(AlignmentCigarJob job, uint target_cols, uint query_i, uint target_j) {
    return job.traceback_tile_offset + query_i * target_cols + target_j;
}

uint best_gap_state(uint len, bool insertion) {
    int short_cost = gap_cost_short(len);
    int long_cost = gap_cost_long(len);
    if (long_cost < short_cost) {
        return insertion ? 3u : 4u;
    }
    return insertion ? 1u : 2u;
}

void write_success(
    uint job_index,
    AlignmentCigarJob job,
    int score,
    uint cigar_length) {
    results[job_index].score = score;
    results[job_index].max_score = score;
    results[job_index].query_start = job.query_global_start;
    results[job_index].query_end = job.query_global_start + job.query_len;
    results[job_index].target_start = job.target_global_start;
    results[job_index].target_end = job.target_global_start + job.target_len;
    results[job_index].cigar_offset = job.cigar_offset;
    results[job_index].cigar_length = cigar_length;
    results[job_index].status = STATUS_OK;
    results[job_index].reserved = 0u;
}

bool append_reverse_cigar(AlignmentCigarJob job, inout uint cigar_length, uint op) {
    if (cigar_length > 0u) {
        uint last_index = job.cigar_offset + cigar_length - 1u;
        if ((packed_cigar[last_index] & 15u) == op) {
            packed_cigar[last_index] += 16u;
            return true;
        }
    }
    if (cigar_length >= job.cigar_capacity) {
        return false;
    }
    packed_cigar[job.cigar_offset + cigar_length] = (1u << 4) | op;
    ++cigar_length;
    return true;
}

void reverse_cigar_slice(AlignmentCigarJob job, uint cigar_length) {
    for (uint i = 0u; i < cigar_length / 2u; ++i) {
        uint left_index = job.cigar_offset + i;
        uint right_index = job.cigar_offset + cigar_length - 1u - i;
        uint tmp = packed_cigar[left_index];
        packed_cigar[left_index] = packed_cigar[right_index];
        packed_cigar[right_index] = tmp;
    }
}

int best_gap_cost_value(uint len) {
    int short_cost = gap_cost_short(len);
    int long_cost = gap_cost_long(len);
    return long_cost < short_cost ? long_cost : short_cost;
}

bool append_forward_cigar(
    AlignmentCigarJob job,
    inout uint cigar_length,
    uint len,
    uint op) {
    if (len == 0u) {
        return true;
    }
    if (cigar_length > 0u) {
        uint last_index = job.cigar_offset + cigar_length - 1u;
        if ((packed_cigar[last_index] & 15u) == op) {
            packed_cigar[last_index] += (len << 4u);
            return true;
        }
    }
    if (cigar_length >= job.cigar_capacity) {
        return false;
    }
    packed_cigar[job.cigar_offset + cigar_length] = (len << 4u) | op;
    ++cigar_length;
    return true;
}

uint common_match_run(AlignmentCigarJob job, uint query_pos, uint target_pos) {
    uint run = 0u;
    while (query_pos + run < job.query_len
           && target_pos + run < job.target_len
           && query_symbols[job.query_symbol_offset + query_pos + run]
              == target_symbols[job.target_symbol_offset + target_pos + run]) {
        ++run;
    }
    return run;
}

bool try_one_gap_script(
    AlignmentCigarJob job,
    uint gap_op,
    uint gap_len,
    out uint prefix_len,
    out uint suffix_len) {
    prefix_len = common_match_run(job, 0u, 0u);
    uint query_pos = prefix_len;
    uint target_pos = prefix_len;
    if (gap_op == CIGAR_INS) {
        query_pos += gap_len;
    } else {
        target_pos += gap_len;
    }
    if (query_pos > job.query_len || target_pos > job.target_len) {
        return false;
    }
    suffix_len = common_match_run(job, query_pos, target_pos);
    return query_pos + suffix_len == job.query_len
        && target_pos + suffix_len == job.target_len;
}

bool try_two_gap_script(
    AlignmentCigarJob job,
    uint first_op,
    uint first_len,
    uint second_op,
    uint second_len,
    out uint prefix_len,
    out uint mid_len,
    out uint suffix_len) {
    prefix_len = common_match_run(job, 0u, 0u);
    uint query_pos = prefix_len;
    uint target_pos = prefix_len;
    if (first_op == CIGAR_INS) {
        query_pos += first_len;
    } else {
        target_pos += first_len;
    }
    if (query_pos > job.query_len || target_pos > job.target_len) {
        return false;
    }
    mid_len = common_match_run(job, query_pos, target_pos);
    query_pos += mid_len;
    target_pos += mid_len;
    if (second_op == CIGAR_INS) {
        query_pos += second_len;
    } else {
        target_pos += second_len;
    }
    if (query_pos > job.query_len || target_pos > job.target_len) {
        return false;
    }
    suffix_len = common_match_run(job, query_pos, target_pos);
    return query_pos + suffix_len == job.query_len
        && target_pos + suffix_len == job.target_len;
}

bool better_greedy_gap_candidate(
    uint run_len,
    uint gap_len,
    uint op,
    uint best_run,
    uint best_len,
    uint best_op) {
    return run_len > best_run
        || (run_len == best_run
            && (best_len == 0u || gap_len < best_len
                || (gap_len == best_len && op < best_op)));
}

bool try_emit_unit_gap_walk_cigar(uint job_index, AlignmentCigarJob job) {
    uint query_pos = 0u;
    uint target_pos = 0u;
    uint cigar_length = 0u;
    uint match_count = 0u;
    uint gap_cost = 0u;
    uint gap_count = 0u;

    while (query_pos < job.query_len || target_pos < job.target_len) {
        uint run_len = 0u;
        if (query_pos < job.query_len && target_pos < job.target_len) {
            run_len = common_match_run(job, query_pos, target_pos);
        }
        if (run_len != 0u) {
            if (!append_forward_cigar(job, cigar_length, run_len, CIGAR_MATCH)) {
                write_reject(job_index, STATUS_CIGAR_OVERFLOW);
                return true;
            }
            query_pos += run_len;
            target_pos += run_len;
            match_count += run_len;
            continue;
        }

        if (gap_count >= SIMPLE_GAP_OP_MAX) {
            return false;
        }

        if (query_pos == job.query_len) {
            uint gap_len = job.target_len - target_pos;
            if (!append_forward_cigar(job, cigar_length, gap_len, CIGAR_DEL)) {
                write_reject(job_index, STATUS_CIGAR_OVERFLOW);
                return true;
            }
            gap_cost += uint(best_gap_cost_value(gap_len));
            target_pos = job.target_len;
            ++gap_count;
            continue;
        }
        if (target_pos == job.target_len) {
            uint gap_len = job.query_len - query_pos;
            if (!append_forward_cigar(job, cigar_length, gap_len, CIGAR_INS)) {
                write_reject(job_index, STATUS_CIGAR_OVERFLOW);
                return true;
            }
            gap_cost += uint(best_gap_cost_value(gap_len));
            query_pos = job.query_len;
            ++gap_count;
            continue;
        }

        if (target_pos + 1u < job.target_len
            && query_symbols[job.query_symbol_offset + query_pos]
                == target_symbols[job.target_symbol_offset + target_pos + 1u]) {
            if (!append_forward_cigar(job, cigar_length, 1u, CIGAR_DEL)) {
                write_reject(job_index, STATUS_CIGAR_OVERFLOW);
                return true;
            }
            gap_cost += uint(best_gap_cost_value(1u));
            ++target_pos;
            ++gap_count;
            continue;
        }
        if (query_pos + 1u < job.query_len
            && query_symbols[job.query_symbol_offset + query_pos + 1u]
                == target_symbols[job.target_symbol_offset + target_pos]) {
            if (!append_forward_cigar(job, cigar_length, 1u, CIGAR_INS)) {
                write_reject(job_index, STATUS_CIGAR_OVERFLOW);
                return true;
            }
            gap_cost += uint(best_gap_cost_value(1u));
            ++query_pos;
            ++gap_count;
            continue;
        }

        return false;
    }

    int score = int(match_count) * pc.match_score - int(gap_cost);
    write_success(job_index, job, score, cigar_length);
    return true;
}

bool try_emit_greedy_gap_cigar(uint job_index, AlignmentCigarJob job) {
    uint query_pos = 0u;
    uint target_pos = 0u;
    uint cigar_length = 0u;
    uint match_count = 0u;
    uint gap_cost = 0u;
    uint gap_count = 0u;

    while (query_pos < job.query_len || target_pos < job.target_len) {
        uint run_len = 0u;
        if (query_pos < job.query_len && target_pos < job.target_len) {
            run_len = common_match_run(job, query_pos, target_pos);
        }
        if (run_len != 0u) {
            if (!append_forward_cigar(job, cigar_length, run_len, CIGAR_MATCH)) {
                write_reject(job_index, STATUS_CIGAR_OVERFLOW);
                return true;
            }
            query_pos += run_len;
            target_pos += run_len;
            match_count += run_len;
            continue;
        }

        if (query_pos == job.query_len) {
            uint gap_len = job.target_len - target_pos;
            if (!append_forward_cigar(job, cigar_length, gap_len, CIGAR_DEL)) {
                write_reject(job_index, STATUS_CIGAR_OVERFLOW);
                return true;
            }
            gap_cost += uint(best_gap_cost_value(gap_len));
            target_pos = job.target_len;
            ++gap_count;
            continue;
        }
        if (target_pos == job.target_len) {
            uint gap_len = job.query_len - query_pos;
            if (!append_forward_cigar(job, cigar_length, gap_len, CIGAR_INS)) {
                write_reject(job_index, STATUS_CIGAR_OVERFLOW);
                return true;
            }
            gap_cost += uint(best_gap_cost_value(gap_len));
            query_pos = job.query_len;
            ++gap_count;
            continue;
        }

        uint best_op = 99u;
        uint best_len = 0u;
        uint best_run = 0u;
        for (uint gap_len = 1u; gap_len <= SIMPLE_GAP_MAX; ++gap_len) {
            if (target_pos + gap_len < job.target_len
                && query_symbols[job.query_symbol_offset + query_pos]
                    == target_symbols[job.target_symbol_offset + target_pos + gap_len]) {
                uint candidate_run =
                    common_match_run(job, query_pos, target_pos + gap_len);
                if (better_greedy_gap_candidate(
                        candidate_run, gap_len, CIGAR_DEL,
                        best_run, best_len, best_op)) {
                    best_run = candidate_run;
                    best_len = gap_len;
                    best_op = CIGAR_DEL;
                }
            }
            if (query_pos + gap_len < job.query_len
                && query_symbols[job.query_symbol_offset + query_pos + gap_len]
                    == target_symbols[job.target_symbol_offset + target_pos]) {
                uint candidate_run =
                    common_match_run(job, query_pos + gap_len, target_pos);
                if (better_greedy_gap_candidate(
                        candidate_run, gap_len, CIGAR_INS,
                        best_run, best_len, best_op)) {
                    best_run = candidate_run;
                    best_len = gap_len;
                    best_op = CIGAR_INS;
                }
            }
        }

        if (best_len == 0u || best_run < SIMPLE_GAP_ANCHOR_MIN
            || gap_count >= SIMPLE_GAP_OP_MAX) {
            return false;
        }
        if (!append_forward_cigar(job, cigar_length, best_len, best_op)) {
            write_reject(job_index, STATUS_CIGAR_OVERFLOW);
            return true;
        }
        gap_cost += uint(best_gap_cost_value(best_len));
        ++gap_count;
        if (best_op == CIGAR_INS) {
            query_pos += best_len;
        } else {
            target_pos += best_len;
        }
    }

    int score = int(match_count) * pc.match_score - int(gap_cost);
    write_success(job_index, job, score, cigar_length);
    return true;
}

bool emit_simple_script_success(
    uint job_index,
    AlignmentCigarJob job,
    uint prefix_len,
    uint first_op,
    uint first_len,
    uint mid_len,
    uint second_op,
    uint second_len,
    uint suffix_len,
    uint gap_count) {
    uint cigar_length = 0u;
    if (!append_forward_cigar(job, cigar_length, prefix_len, CIGAR_MATCH)
        || (gap_count >= 1u
            && !append_forward_cigar(job, cigar_length, first_len, first_op))
        || !append_forward_cigar(job, cigar_length, mid_len, CIGAR_MATCH)
        || (gap_count >= 2u
            && !append_forward_cigar(job, cigar_length, second_len, second_op))
        || !append_forward_cigar(job, cigar_length, suffix_len, CIGAR_MATCH)) {
        return false;
    }

    int score = int(prefix_len + mid_len + suffix_len) * pc.match_score;
    if (gap_count >= 1u) {
        score -= best_gap_cost_value(first_len);
    }
    if (gap_count >= 2u) {
        score -= best_gap_cost_value(second_len);
    }
    write_success(job_index, job, score, cigar_length);
    return true;
}

bool try_emit_simple_cigar(uint job_index, AlignmentCigarJob job) {
    if (job.cigar_capacity == 0u) {
        write_reject(job_index, STATUS_CIGAR_OVERFLOW);
        return true;
    }

    uint exact_len = common_match_run(job, 0u, 0u);
    if (exact_len == job.query_len && exact_len == job.target_len) {
        return emit_simple_script_success(
            job_index, job,
            exact_len, CIGAR_MATCH, 0u,
            0u, CIGAR_MATCH, 0u,
            0u, 0u);
    }

    int net_query = int(job.query_len) - int(job.target_len);
    int abs_net_query = net_query < 0 ? -net_query : net_query;
    if (abs_net_query > int(SIMPLE_GAP_MAX)) {
        return false;
    }

    if (net_query > 0) {
        uint prefix_len = 0u;
        uint suffix_len = 0u;
        uint gap_len = uint(net_query);
        if (try_one_gap_script(
                job, CIGAR_INS, gap_len, prefix_len, suffix_len)) {
            return emit_simple_script_success(
                job_index, job,
                prefix_len, CIGAR_INS, gap_len,
                suffix_len, CIGAR_MATCH, 0u,
                0u, 1u);
        }
    } else if (net_query < 0) {
        uint prefix_len = 0u;
        uint suffix_len = 0u;
        uint gap_len = uint(-net_query);
        if (try_one_gap_script(
                job, CIGAR_DEL, gap_len, prefix_len, suffix_len)) {
            return emit_simple_script_success(
                job_index, job,
                prefix_len, CIGAR_DEL, gap_len,
                suffix_len, CIGAR_MATCH, 0u,
                0u, 1u);
        }
    }

    for (uint del_len = 1u; del_len <= SIMPLE_GAP_MAX; ++del_len) {
        int ins_len_i = int(del_len) + net_query;
        if (ins_len_i <= 0 || ins_len_i > int(SIMPLE_GAP_MAX)) {
            continue;
        }
        uint ins_len = uint(ins_len_i);

        uint prefix_len = 0u;
        uint mid_len = 0u;
        uint suffix_len = 0u;
        if (try_two_gap_script(
                job,
                CIGAR_DEL, del_len,
                CIGAR_INS, ins_len,
                prefix_len, mid_len, suffix_len)) {
            return emit_simple_script_success(
                job_index, job,
                prefix_len, CIGAR_DEL, del_len,
                mid_len, CIGAR_INS, ins_len,
                suffix_len, 2u);
        }

        prefix_len = 0u;
        mid_len = 0u;
        suffix_len = 0u;
        if (try_two_gap_script(
                job,
                CIGAR_INS, ins_len,
                CIGAR_DEL, del_len,
                prefix_len, mid_len, suffix_len)) {
            return emit_simple_script_success(
                job_index, job,
                prefix_len, CIGAR_INS, ins_len,
                mid_len, CIGAR_DEL, del_len,
                suffix_len, 2u);
        }
    }

    if (job.query_len != job.target_len
        && try_emit_unit_gap_walk_cigar(job_index, job)) {
        return true;
    }

    if (try_emit_greedy_gap_cigar(job_index, job)) {
        return true;
    }

    return false;
}

uint ceil_div_u(uint value, uint divisor) {
    return divisor == 0u ? 0u : (value + divisor - 1u) / divisor;
}

uint min_u(uint a, uint b) {
    return a < b ? a : b;
}

uint gridded_query_tiles(AlignmentCigarJob job) {
    return ceil_div_u(job.query_len, pc.tile_query);
}

uint gridded_target_tiles(AlignmentCigarJob job) {
    return ceil_div_u(job.target_len, pc.tile_target);
}

uint gridded_record_words() {
    return 5u * ((pc.tile_query + 1u) + (pc.tile_target + 1u));
}

uint gridded_boundary_words(AlignmentCigarJob job) {
    return gridded_query_tiles(job) * gridded_target_tiles(job)
        * gridded_record_words();
}

uint gridded_scratch_words() {
    return (pc.tile_query + 1u) * (pc.tile_target + 1u);
}

uint encode_score(int value) {
    return uint(value + 1073741824);
}

int decode_score(uint value) {
    return int(value) - 1073741824;
}

uint tile_base(AlignmentCigarJob job, uint tile_row, uint tile_col) {
    return job.traceback_tile_offset
        + (tile_row * gridded_target_tiles(job) + tile_col)
            * gridded_record_words();
}

uint bottom_state_index(
    AlignmentCigarJob job,
    uint tile_row,
    uint tile_col,
    uint local_target,
    uint state_index) {
    return tile_base(job, tile_row, tile_col)
        + local_target * 5u + state_index;
}

uint right_state_index(
    AlignmentCigarJob job,
    uint tile_row,
    uint tile_col,
    uint local_query,
    uint state_index) {
    return tile_base(job, tile_row, tile_col)
        + 5u * (pc.tile_target + 1u)
        + local_query * 5u + state_index;
}

uint gridded_scratch_offset(AlignmentCigarJob job) {
    return job.traceback_tile_offset + gridded_boundary_words(job);
}

uint gridded_scratch_index(
    AlignmentCigarJob job,
    uint local_query,
    uint local_target,
    uint scratch_cols) {
    return gridded_scratch_offset(job) + local_query * scratch_cols + local_target;
}

void init_axis_state(
    uint query_pos,
    uint target_pos,
    out int h,
    out int i1,
    out int i2,
    out int d1,
    out int d2) {
    if (query_pos == 0u && target_pos == 0u) {
        h = 0;
        i1 = NEG_INF;
        i2 = NEG_INF;
        d1 = NEG_INF;
        d2 = NEG_INF;
        return;
    }
    if (target_pos == 0u) {
        i1 = -gap_cost_short(query_pos);
        i2 = -gap_cost_long(query_pos);
        h = i2 > i1 ? i2 : i1;
        d1 = NEG_INF;
        d2 = NEG_INF;
        return;
    }
    if (query_pos == 0u) {
        d1 = -gap_cost_short(target_pos);
        d2 = -gap_cost_long(target_pos);
        h = d2 > d1 ? d2 : d1;
        i1 = NEG_INF;
        i2 = NEG_INF;
        return;
    }
    h = NEG_INF;
    i1 = NEG_INF;
    i2 = NEG_INF;
    d1 = NEG_INF;
    d2 = NEG_INF;
}

void load_top_state(
    AlignmentCigarJob job,
    uint tile_row,
    uint tile_col,
    uint local_target,
    out int h,
    out int i1,
    out int i2,
    out int d1,
    out int d2) {
    if (tile_row == 0u) {
        init_axis_state(0u, tile_col * pc.tile_target + local_target,
                        h, i1, i2, d1, d2);
        return;
    }
    uint above = tile_row - 1u;
    h = decode_score(traceback[bottom_state_index(job, above, tile_col, local_target, 0u)]);
    i1 = decode_score(traceback[bottom_state_index(job, above, tile_col, local_target, 1u)]);
    i2 = decode_score(traceback[bottom_state_index(job, above, tile_col, local_target, 2u)]);
    d1 = decode_score(traceback[bottom_state_index(job, above, tile_col, local_target, 3u)]);
    d2 = decode_score(traceback[bottom_state_index(job, above, tile_col, local_target, 4u)]);
}

void load_left_state(
    AlignmentCigarJob job,
    uint tile_row,
    uint tile_col,
    uint local_query,
    out int h,
    out int i1,
    out int i2,
    out int d1,
    out int d2) {
    if (tile_col == 0u) {
        init_axis_state(tile_row * pc.tile_query + local_query, 0u,
                        h, i1, i2, d1, d2);
        return;
    }
    uint left = tile_col - 1u;
    h = decode_score(traceback[right_state_index(job, tile_row, left, local_query, 0u)]);
    i1 = decode_score(traceback[right_state_index(job, tile_row, left, local_query, 1u)]);
    i2 = decode_score(traceback[right_state_index(job, tile_row, left, local_query, 2u)]);
    d1 = decode_score(traceback[right_state_index(job, tile_row, left, local_query, 3u)]);
    d2 = decode_score(traceback[right_state_index(job, tile_row, left, local_query, 4u)]);
}

void store_bottom_state(
    AlignmentCigarJob job,
    uint tile_row,
    uint tile_col,
    uint local_target,
    int h,
    int i1,
    int i2,
    int d1,
    int d2) {
    traceback[bottom_state_index(job, tile_row, tile_col, local_target, 0u)] = encode_score(h);
    traceback[bottom_state_index(job, tile_row, tile_col, local_target, 1u)] = encode_score(i1);
    traceback[bottom_state_index(job, tile_row, tile_col, local_target, 2u)] = encode_score(i2);
    traceback[bottom_state_index(job, tile_row, tile_col, local_target, 3u)] = encode_score(d1);
    traceback[bottom_state_index(job, tile_row, tile_col, local_target, 4u)] = encode_score(d2);
}

void store_right_state(
    AlignmentCigarJob job,
    uint tile_row,
    uint tile_col,
    uint local_query,
    int h,
    int i1,
    int i2,
    int d1,
    int d2) {
    traceback[right_state_index(job, tile_row, tile_col, local_query, 0u)] = encode_score(h);
    traceback[right_state_index(job, tile_row, tile_col, local_query, 1u)] = encode_score(i1);
    traceback[right_state_index(job, tile_row, tile_col, local_query, 2u)] = encode_score(i2);
    traceback[right_state_index(job, tile_row, tile_col, local_query, 3u)] = encode_score(d1);
    traceback[right_state_index(job, tile_row, tile_col, local_query, 4u)] = encode_score(d2);
}

void compute_gridded_tile(
    AlignmentCigarJob job,
    uint tile_row,
    uint tile_col,
    bool store_boundaries,
    bool store_trace) {
    uint query_origin = tile_row * pc.tile_query;
    uint target_origin = tile_col * pc.tile_target;
    uint query_rows = min_u(pc.tile_query, job.query_len - query_origin);
    uint target_cols_active = min_u(pc.tile_target, job.target_len - target_origin);
    uint scratch_cols = target_cols_active + 1u;

    int h_prev[MAX_SPAN + 1];
    int h_cur[MAX_SPAN + 1];
    int i1_prev[MAX_SPAN + 1];
    int i1_cur[MAX_SPAN + 1];
    int i2_prev[MAX_SPAN + 1];
    int i2_cur[MAX_SPAN + 1];
    int d1_cur[MAX_SPAN + 1];
    int d2_cur[MAX_SPAN + 1];

    int unused_d1 = NEG_INF;
    int unused_d2 = NEG_INF;
    for (uint local_target = 0u; local_target <= target_cols_active; ++local_target) {
        load_top_state(job, tile_row, tile_col, local_target,
                       h_prev[local_target], i1_prev[local_target],
                       i2_prev[local_target], unused_d1, unused_d2);
    }

    if (store_boundaries) {
        int top_h = NEG_INF;
        int top_i1 = NEG_INF;
        int top_i2 = NEG_INF;
        int top_d1 = NEG_INF;
        int top_d2 = NEG_INF;
        load_top_state(job, tile_row, tile_col, target_cols_active,
                       top_h, top_i1, top_i2, top_d1, top_d2);
        store_right_state(job, tile_row, tile_col, 0u,
                          top_h, top_i1, top_i2, top_d1, top_d2);
    }

    for (uint local_query = 1u; local_query <= query_rows; ++local_query) {
        load_left_state(job, tile_row, tile_col, local_query,
                        h_cur[0], i1_cur[0], i2_cur[0],
                        d1_cur[0], d2_cur[0]);

        for (uint local_target = 1u; local_target <= target_cols_active; ++local_target) {
            int i1_open = h_prev[local_target] - pc.gap_open_short - pc.gap_extend_short;
            int i1_extend = i1_prev[local_target] - pc.gap_extend_short;
            bool i1_cont = i1_extend >= i1_open;
            i1_cur[local_target] = i1_cont ? i1_extend : i1_open;

            int i2_open = h_prev[local_target] - pc.gap_open_long - pc.gap_extend_long;
            int i2_extend = i2_prev[local_target] - pc.gap_extend_long;
            bool i2_cont = i2_extend >= i2_open;
            i2_cur[local_target] = i2_cont ? i2_extend : i2_open;

            int d1_open = h_cur[local_target - 1u] - pc.gap_open_short - pc.gap_extend_short;
            int d1_extend = d1_cur[local_target - 1u] - pc.gap_extend_short;
            bool d1_cont = d1_extend >= d1_open;
            d1_cur[local_target] = d1_cont ? d1_extend : d1_open;

            int d2_open = h_cur[local_target - 1u] - pc.gap_open_long - pc.gap_extend_long;
            int d2_extend = d2_cur[local_target - 1u] - pc.gap_extend_long;
            bool d2_cont = d2_extend >= d2_open;
            d2_cur[local_target] = d2_cont ? d2_extend : d2_open;

            uint q = query_symbols[job.query_symbol_offset + query_origin + local_query - 1u];
            uint t = target_symbols[job.target_symbol_offset + target_origin + local_target - 1u];
            int best_score = h_prev[local_target - 1u] + substitution_score(q, t);
            uint best_source = 0u;
            if (i1_cur[local_target] > best_score) {
                best_score = i1_cur[local_target];
                best_source = 1u;
            }
            if (d1_cur[local_target] > best_score) {
                best_score = d1_cur[local_target];
                best_source = 2u;
            }
            if (i2_cur[local_target] > best_score) {
                best_score = i2_cur[local_target];
                best_source = 3u;
            }
            if (d2_cur[local_target] > best_score) {
                best_score = d2_cur[local_target];
                best_source = 4u;
            }
            h_cur[local_target] = best_score;

            if (store_trace) {
                uint trace = best_source;
                if (i1_cont) trace |= 1u << 3u;
                if (d1_cont) trace |= 1u << 4u;
                if (i2_cont) trace |= 1u << 5u;
                if (d2_cont) trace |= 1u << 6u;
                traceback[gridded_scratch_index(
                    job, local_query, local_target, scratch_cols)] = trace;
            }
        }

        if (store_boundaries) {
            store_right_state(job, tile_row, tile_col, local_query,
                              h_cur[target_cols_active],
                              i1_cur[target_cols_active],
                              i2_cur[target_cols_active],
                              d1_cur[target_cols_active],
                              d2_cur[target_cols_active]);
        }

        for (uint local_target = 0u; local_target <= target_cols_active; ++local_target) {
            h_prev[local_target] = h_cur[local_target];
            i1_prev[local_target] = i1_cur[local_target];
            i2_prev[local_target] = i2_cur[local_target];
        }
    }

    if (store_boundaries) {
        for (uint local_target = 0u; local_target <= target_cols_active; ++local_target) {
            store_bottom_state(job, tile_row, tile_col, local_target,
                               h_prev[local_target],
                               i1_prev[local_target],
                               i2_prev[local_target],
                               d1_cur[local_target],
                               d2_cur[local_target]);
        }
    }
}

bool solve_gridded_dp(uint job_index, AlignmentCigarJob job) {
    if (pc.tile_query == 0u || pc.tile_query > MAX_SPAN
        || pc.tile_target == 0u || pc.tile_target > MAX_SPAN) {
        write_reject(job_index, STATUS_UNSUPPORTED_GEOMETRY);
        return true;
    }
    uint required_words = gridded_boundary_words(job) + gridded_scratch_words();
    if (job.traceback_tile_count < required_words) {
        write_reject(job_index, STATUS_UNSUPPORTED_GEOMETRY);
        return true;
    }

    uint query_tiles = gridded_query_tiles(job);
    uint target_tiles = gridded_target_tiles(job);
    for (uint tile_row = 0u; tile_row < query_tiles; ++tile_row) {
        for (uint tile_col = 0u; tile_col < target_tiles; ++tile_col) {
            compute_gridded_tile(job, tile_row, tile_col, true, false);
        }
    }

    uint last_tile_row = query_tiles - 1u;
    uint last_tile_col = target_tiles - 1u;
    uint last_target = job.target_len - last_tile_col * pc.tile_target;
    int score = decode_score(
        traceback[bottom_state_index(job, last_tile_row, last_tile_col, last_target, 0u)]);

    uint global_query = job.query_len;
    uint global_target = job.target_len;
    uint state = 0u;
    uint cigar_length = 0u;

    while (global_query > 0u || global_target > 0u) {
        if (global_query == 0u) {
            if (!append_reverse_cigar(job, cigar_length, CIGAR_DEL)) {
                write_reject(job_index, STATUS_CIGAR_OVERFLOW);
                return true;
            }
            --global_target;
            continue;
        }
        if (global_target == 0u) {
            if (!append_reverse_cigar(job, cigar_length, CIGAR_INS)) {
                write_reject(job_index, STATUS_CIGAR_OVERFLOW);
                return true;
            }
            --global_query;
            continue;
        }

        uint tile_row = (global_query - 1u) / pc.tile_query;
        uint tile_col = (global_target - 1u) / pc.tile_target;
        uint query_origin = tile_row * pc.tile_query;
        uint target_origin = tile_col * pc.tile_target;
        uint target_cols_active = min_u(pc.tile_target, job.target_len - target_origin);
        uint scratch_cols = target_cols_active + 1u;
        compute_gridded_tile(job, tile_row, tile_col, false, true);

        while (global_query > query_origin || global_target > target_origin) {
            uint local_query = global_query - query_origin;
            uint local_target = global_target - target_origin;
            if (local_query == 0u || local_target == 0u) {
                break;
            }
            uint trace = traceback[gridded_scratch_index(
                job, local_query, local_target, scratch_cols)];
            if (state == 0u) {
                uint source = trace & 7u;
                if (source == 0u) {
                    if (!append_reverse_cigar(job, cigar_length, CIGAR_MATCH)) {
                        write_reject(job_index, STATUS_CIGAR_OVERFLOW);
                        return true;
                    }
                    --global_query;
                    --global_target;
                } else if (source == 1u || source == 3u) {
                    state = source;
                } else if (source == 2u || source == 4u) {
                    state = source;
                } else {
                    write_reject(job_index, STATUS_UNSUPPORTED_GEOMETRY);
                    return true;
                }
            } else if (state == 1u || state == 3u) {
                if (!append_reverse_cigar(job, cigar_length, CIGAR_INS)) {
                    write_reject(job_index, STATUS_CIGAR_OVERFLOW);
                    return true;
                }
                bool cont = state == 1u ? ((trace & (1u << 3u)) != 0u)
                                        : ((trace & (1u << 5u)) != 0u);
                --global_query;
                state = cont ? state : 0u;
            } else {
                if (!append_reverse_cigar(job, cigar_length, CIGAR_DEL)) {
                    write_reject(job_index, STATUS_CIGAR_OVERFLOW);
                    return true;
                }
                bool cont = state == 2u ? ((trace & (1u << 4u)) != 0u)
                                        : ((trace & (1u << 6u)) != 0u);
                --global_target;
                state = cont ? state : 0u;
            }
        }
    }

    reverse_cigar_slice(job, cigar_length);
    write_success(job_index, job, score, cigar_length);
    return true;
}

void solve_bounded_dp(uint job_index) {
    AlignmentCigarJob job = jobs[job_index];
    if (pc.alphabet_size == 0u || pc.flags != 0u || pc.reserved != 0u
        || job.flags != 0u || job.query_len == 0u || job.target_len == 0u
        || job.band_width > pc.max_band_width) {
        write_reject(job_index, STATUS_UNSUPPORTED_GEOMETRY);
        return;
    }
    if (job.cigar_capacity == 0u) {
        write_reject(job_index, STATUS_CIGAR_OVERFLOW);
        return;
    }

    for (uint i = 0u; i < job.query_len; ++i) {
        if (query_symbols[job.query_symbol_offset + i] >= pc.alphabet_size) {
            write_reject(job_index, STATUS_UNSUPPORTED_GEOMETRY);
            return;
        }
    }
    for (uint i = 0u; i < job.target_len; ++i) {
        if (target_symbols[job.target_symbol_offset + i] >= pc.alphabet_size) {
            write_reject(job_index, STATUS_UNSUPPORTED_GEOMETRY);
            return;
        }
    }

    if (try_emit_simple_cigar(job_index, job)) {
        return;
    }

    uint target_cols = job.target_len + 1u;
    uint traceback_cells = (job.query_len + 1u) * target_cols;
    if (job.query_len > MAX_SPAN || job.target_len > MAX_SPAN) {
        solve_gridded_dp(job_index, job);
        return;
    }
    if (job.traceback_tile_count < traceback_cells) {
        write_reject(job_index, STATUS_UNSUPPORTED_GEOMETRY);
        return;
    }

    int h_prev[MAX_SPAN + 1];
    int h_cur[MAX_SPAN + 1];
    int i1_prev[MAX_SPAN + 1];
    int i1_cur[MAX_SPAN + 1];
    int i2_prev[MAX_SPAN + 1];
    int i2_cur[MAX_SPAN + 1];
    int d1_cur[MAX_SPAN + 1];
    int d2_cur[MAX_SPAN + 1];

    h_prev[0] = 0;
    i1_prev[0] = NEG_INF;
    i2_prev[0] = NEG_INF;
    traceback[traceback_index(job, target_cols, 0u, 0u)] = 0u;
    for (uint j = 1u; j <= job.target_len; ++j) {
        int short_score = -gap_cost_short(j);
        int long_score = -gap_cost_long(j);
        bool use_long = long_score > short_score;
        h_prev[j] = use_long ? long_score : short_score;
        i1_prev[j] = NEG_INF;
        i2_prev[j] = NEG_INF;
        uint source = use_long ? 4u : 2u;
        uint cont_bit = j > 1u ? (use_long ? (1u << 6u) : (1u << 4u)) : 0u;
        traceback[traceback_index(job, target_cols, 0u, j)] = source | cont_bit;
    }

    for (uint i = 1u; i <= job.query_len; ++i) {
        int short_score = -gap_cost_short(i);
        int long_score = -gap_cost_long(i);
        bool use_long = long_score > short_score;
        h_cur[0] = use_long ? long_score : short_score;
        i1_cur[0] = short_score;
        i2_cur[0] = long_score;
        d1_cur[0] = NEG_INF;
        d2_cur[0] = NEG_INF;
        uint source = use_long ? 3u : 1u;
        uint cont_bit = i > 1u ? (use_long ? (1u << 5u) : (1u << 3u)) : 0u;
        traceback[traceback_index(job, target_cols, i, 0u)] = source | cont_bit;

        for (uint j = 1u; j <= job.target_len; ++j) {
            int i1_open = h_prev[j] - pc.gap_open_short - pc.gap_extend_short;
            int i1_extend = i1_prev[j] - pc.gap_extend_short;
            bool i1_cont = i1_extend >= i1_open;
            i1_cur[j] = i1_cont ? i1_extend : i1_open;

            int i2_open = h_prev[j] - pc.gap_open_long - pc.gap_extend_long;
            int i2_extend = i2_prev[j] - pc.gap_extend_long;
            bool i2_cont = i2_extend >= i2_open;
            i2_cur[j] = i2_cont ? i2_extend : i2_open;

            int d1_open = h_cur[j - 1u] - pc.gap_open_short - pc.gap_extend_short;
            int d1_extend = d1_cur[j - 1u] - pc.gap_extend_short;
            bool d1_cont = d1_extend >= d1_open;
            d1_cur[j] = d1_cont ? d1_extend : d1_open;

            int d2_open = h_cur[j - 1u] - pc.gap_open_long - pc.gap_extend_long;
            int d2_extend = d2_cur[j - 1u] - pc.gap_extend_long;
            bool d2_cont = d2_extend >= d2_open;
            d2_cur[j] = d2_cont ? d2_extend : d2_open;

            uint q = query_symbols[job.query_symbol_offset + i - 1u];
            uint t = target_symbols[job.target_symbol_offset + j - 1u];
            int best_score = h_prev[j - 1u] + substitution_score(q, t);
            uint best_source = 0u;
            if (i1_cur[j] > best_score) {
                best_score = i1_cur[j];
                best_source = 1u;
            }
            if (d1_cur[j] > best_score) {
                best_score = d1_cur[j];
                best_source = 2u;
            }
            if (i2_cur[j] > best_score) {
                best_score = i2_cur[j];
                best_source = 3u;
            }
            if (d2_cur[j] > best_score) {
                best_score = d2_cur[j];
                best_source = 4u;
            }
            h_cur[j] = best_score;

            uint trace = best_source;
            if (i1_cont) trace |= 1u << 3u;
            if (d1_cont) trace |= 1u << 4u;
            if (i2_cont) trace |= 1u << 5u;
            if (d2_cont) trace |= 1u << 6u;
            traceback[traceback_index(job, target_cols, i, j)] = trace;
        }

        for (uint j = 0u; j <= job.target_len; ++j) {
            h_prev[j] = h_cur[j];
            i1_prev[j] = i1_cur[j];
            i2_prev[j] = i2_cur[j];
        }
    }

    uint i = job.query_len;
    uint j = job.target_len;
    uint state = 0u;
    uint cigar_length = 0u;
    while (i > 0u || j > 0u) {
        uint trace = traceback[traceback_index(job, target_cols, i, j)];
        if (state == 0u) {
            uint source = trace & 7u;
            if (source == 0u && i > 0u && j > 0u) {
                if (!append_reverse_cigar(job, cigar_length, CIGAR_MATCH)) {
                    write_reject(job_index, STATUS_CIGAR_OVERFLOW);
                    return;
                }
                --i;
                --j;
            } else if ((source == 1u || source == 3u) && i > 0u) {
                state = source;
            } else if ((source == 2u || source == 4u) && j > 0u) {
                state = source;
            } else if (i > 0u) {
                state = best_gap_state(i, true);
            } else if (j > 0u) {
                state = best_gap_state(j, false);
            } else {
                break;
            }
        } else if (state == 1u || state == 3u) {
            if (!append_reverse_cigar(job, cigar_length, CIGAR_INS)) {
                write_reject(job_index, STATUS_CIGAR_OVERFLOW);
                return;
            }
            bool cont = state == 1u ? ((trace & (1u << 3u)) != 0u)
                                    : ((trace & (1u << 5u)) != 0u);
            --i;
            state = cont ? state : 0u;
        } else {
            if (!append_reverse_cigar(job, cigar_length, CIGAR_DEL)) {
                write_reject(job_index, STATUS_CIGAR_OVERFLOW);
                return;
            }
            bool cont = state == 2u ? ((trace & (1u << 4u)) != 0u)
                                    : ((trace & (1u << 6u)) != 0u);
            --j;
            state = cont ? state : 0u;
        }
    }

    reverse_cigar_slice(job, cigar_length);
    write_success(job_index, job, h_prev[job.target_len], cigar_length);
}

void main() {
    uint stride = gl_NumWorkGroups.x * gl_WorkGroupSize.x;
    if (stride == 0u) return;
    for (uint job_index = gl_GlobalInvocationID.x; job_index < pc.job_count; job_index += stride) {
        solve_bounded_dp(job_index);
    }
}
)GLSL";

constexpr std::string_view kGenerateUnitsBody = R"GLSL(
layout(local_size_x = LOCAL_SIZE_X, local_size_y = 1, local_size_z = 1) in;

#define UNIT_BITS MAX_SPAN
#if UNIT_BITS == 64
  #define UNIT uint64_t
#else
  #define UNIT uint
#endif

layout(std430, set = 0, binding = 0) writeonly buffer OutUnits { UNIT out_units[]; };
layout(std430, set = 0, binding = 1) readonly  buffer Cdf      { uint cdf[]; };

layout(push_constant) uniform Params {
    uint symbols_per_seq;
    uint units_per_seq;
    uint chunks_per_seq;
    uint chunk_units;
    uint letters_per_unit;
    uint bits_per_char;
    uint alphabet_size;
    uint total_chunks;
    uint chunk_base;
    uint seed_lo;
    uint seed_hi;
    uint _pad;
} pc;

uint64_t splitmix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15UL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9UL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebUL;
    return x ^ (x >> 31);
}

uint taus_step(uint z, int s1, int s2, int s3, uint m) {
    uint b = (((z << s1) ^ z) >> s2);
    return (((z & m) << s3) ^ b);
}

uint next_u32(inout uvec4 s) {
    s.x = taus_step(s.x, 13, 19, 12, 4294967294u);
    s.y = taus_step(s.y,  2, 25,  4, 4294967288u);
    s.z = taus_step(s.z,  3, 11, 17, 4294967280u);
    s.w = s.w * 1664525u + 1013904223u;
    return s.x ^ s.y ^ s.z ^ s.w;
}

uvec4 seed_stream(uint64_t seed, uint64_t stream_id) {
    uint64_t a = splitmix64(seed ^ (stream_id * 0x9e3779b97f4a7c15UL));
    uint64_t b = splitmix64(a);
    uvec4 s = uvec4(uint(a), uint(a >> 32), uint(b), uint(b >> 32));
    if (s.x < 129u) s.x += 129u;
    if (s.y < 129u) s.y += 129u;
    if (s.z < 129u) s.z += 129u;
    return s;
}

uint sample_rank(uint u) {
    uint rank = 0u;
    while (rank + 1u < pc.alphabet_size && u >= cdf[rank]) rank += 1u;
    return rank;
}

void main() {
    uint chunk = pc.chunk_base + gl_GlobalInvocationID.x;
    if (chunk >= pc.total_chunks) return;

    uint seq = chunk / pc.chunks_per_seq;
    uint local_chunk = chunk - seq * pc.chunks_per_seq;
    uint seq_unit0 = seq * pc.units_per_seq;
    uint local_u0 = local_chunk * pc.chunk_units;

    uvec4 st = seed_stream(
        (uint64_t(pc.seed_hi) << 32) | uint64_t(pc.seed_lo),
        uint64_t(chunk));

    for (uint u = 0u; u < pc.chunk_units; ++u) {
        uint local_unit = local_u0 + u;
        if (local_unit >= pc.units_per_seq) break;
        uint sym0 = local_unit * pc.letters_per_unit;
        uint valid = min(pc.letters_per_unit, pc.symbols_per_seq - sym0);
        UNIT acc = UNIT(0);
        for (uint slot = 0u; slot < valid; ++slot) {
            uint r = sample_rank(next_u32(st));
            acc |= UNIT(r) << (slot * pc.bits_per_char);
        }
        out_units[seq_unit0 + local_unit] = acc;
    }
}
)GLSL";

// ===========================================================================
// DOC0346 — GPU sequence sort kernels (dna4-optimised + generic)
//
// Each kind's body is composed (in shader_body_for) as:
//   kSeqSortPush + kSeqSortCommon + (dna4?kCanonDna4:kCanonGeneric)
//                + one stage main
// so the dna4/generic canonicalizer is chosen BY ShaderKind (no request
// threading). Storage width is runtime via pc.unit_bits + read_storage_unit64
// (the verbatim helper copied from the candidate body — it is function-local
// there, not a shared symbol). LOCAL_SIZE_X comes from the injected prelude.
// ===========================================================================

/// Push-constant block (40 bytes == SeqSortPush in runtime.cppm). Identical for
/// all seq-sort kinds so one struct serves every stage.
constexpr std::string_view kSeqSortPush = R"GLSL(
layout(push_constant) uniform P {
    uint count;              // row_count (key stage) / range_count (tiebreak stage)
    uint unit_bits;          // 8/16/32/64
    uint bits_per_char;      // generic only (dna4 uses const 2)
    uint letters_per_unit;   // generic only
    uint char_mask;          // generic only
    uint used_mask_lo;       // used_storage_mask low 32
    uint used_mask_hi;       // used_storage_mask high 32
    uint key_unit_offset;       // dense scatter stage: first unit for the 64-bit key
    uint sort_size;          // global bitonic stage: k
    uint sort_stride;        // global bitonic stage: j
} pc;
)GLSL";

/// Descriptor bindings + packed-sequence reader + low_symbol_mask. Declared
/// BEFORE the canonicalizer so it can call low_symbol_mask, and before the
/// key/compare snippet which needs SeqDesc + read_unit. read_unit() forward-uses
/// unit_bits() (defined in the canon snippet that follows), which is fine in
/// GLSL because the canon snippet is concatenated between this and kSeqSortKeyCmp.
constexpr std::string_view kSeqSortReaders = R"GLSL(
struct SeqDesc { uint word_offset; uint unit_count; uint bp_size; uint pad; };  // std430, 16B

layout(std430, set=0, binding=0) readonly buffer SeqWords { uint    seq_words[]; };
layout(std430, set=0, binding=1) readonly buffer Descs    { SeqDesc descs[];     };

uint read_u32_word(uint word_index) { return seq_words[word_index]; }

uint read_storage_unit(uint unit_index, uint ubits, uint byte_offset_words) {
    if (ubits == 64u) {
        uint lo = read_u32_word(byte_offset_words + unit_index * 2u);
        uint hi = read_u32_word(byte_offset_words + unit_index * 2u + 1u);
        return lo ^ hi; // unused for 64-bit (read_storage_unit64 handles it)
    }
    if (ubits == 32u) return read_u32_word(byte_offset_words + unit_index);
    if (ubits == 16u) {
        uint w = read_u32_word(byte_offset_words + (unit_index >> 1u));
        return (w >> ((unit_index & 1u) * 16u)) & 0xffffu;
    }
    uint w = read_u32_word(byte_offset_words + (unit_index >> 2u));
    return (w >> ((unit_index & 3u) * 8u)) & 0xffu;
}

uint64_t read_storage_unit64(uint unit_index, uint ubits, uint byte_offset_words) {
    if (ubits == 64u) {
        uint lo = read_u32_word(byte_offset_words + unit_index * 2u);
        uint hi = read_u32_word(byte_offset_words + unit_index * 2u + 1u);
        return (uint64_t(hi) << 32) | uint64_t(lo);
    }
    return uint64_t(read_storage_unit(unit_index, ubits, byte_offset_words));
}

// keep low valid_syms*bpc bits, zero the rest
uint64_t low_symbol_mask(uint valid_syms, uint bpc) {
    uint bits = valid_syms * bpc;
    return bits >= 64u ? ~uint64_t(0) : ((uint64_t(1) << bits) - uint64_t(1));
}
)GLSL";

/// Canonical key + full canonical compare. Uses canon()/lpu()/unit_bits() from
/// the canonicalizer snippet (concatenated immediately before this).
constexpr std::string_view kSeqSortKeyCmp = R"GLSL(
uint64_t read_unit(SeqDesc d, uint j) {
    return read_storage_unit64(j, unit_bits(), d.word_offset);
}

// 64-bit lexicographic radix key = first 64 canonical bits (zero-filled tail).
uint64_t canon_key(SeqDesc d) {
    uint64_t key = 0ul; uint filled = 0u;
    uint ub = unit_bits();
    for (uint j = 0u; j < d.unit_count && filled < 64u; ++j) {
        uint base0 = j * lpu();
        uint valid = min(lpu(), d.bp_size - base0);
        uint64_t cu = canon(read_unit(d, j), valid);   // valid bases in TOP ub bits
        if (filled + ub <= 64u) key |= cu << (64u - filled - ub);
        else                    key |= cu >> (filled + ub - 64u);
        filled += ub;
    }
    return key;
}

// 64-bit lexicographic radix key starting at a unit offset. This is used by the
// dense scatter fast path after a GPU common-prefix proof has established that
// earlier units are identical across the whole batch.
uint64_t canon_key_at(SeqDesc d, uint start_unit) {
    uint64_t key = 0ul; uint filled = 0u;
    uint ub = unit_bits();
    for (uint j = start_unit; j < d.unit_count && filled < 64u; ++j) {
        uint base0 = j * lpu();
        if (base0 >= d.bp_size) break;
        uint valid = min(lpu(), d.bp_size - base0);
        uint64_t cu = canon(read_unit(d, j), valid);
        if (filled + ub <= 64u) key |= cu << (64u - filled - ub);
        else                    key |= cu >> (filled + ub - 64u);
        filled += ub;
    }
    return key;
}

// Full canonical compare: -1 / 0 / +1 (matches compare_sequences_scalar exactly).
int canon_cmp(SeqDesc a, SeqDesc b) {
    uint units = min(a.unit_count, b.unit_count);
    for (uint j = 0u; j < units; ++j) {
        uint base0 = j * lpu();
        uint v = min(min(lpu(), a.bp_size - base0), min(lpu(), b.bp_size - base0));
        uint64_t ca = canon(read_unit(a, j), v);
        uint64_t cb = canon(read_unit(b, j), v);
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    return a.bp_size < b.bp_size ? -1 : (a.bp_size > b.bp_size ? 1 : 0);  // length tiebreak
}
)GLSL";

/// dna4 canonicalizer: compile-time bits_per_char==2, no masking (used_mask==~0).
constexpr std::string_view kCanonDna4 = R"GLSL(
uint lpu()       { return pc.unit_bits / 2u; }    // bits_per_char == 2
uint unit_bits() { return pc.unit_bits; }

uint64_t canon_dna4(uint64_t x, uint ubits) {     // x has data in low ubits bits
    if (ubits == 64u) {
        x = ((x & 0x3333333333333333UL) << 2)  | ((x & 0xCCCCCCCCCCCCCCCCUL) >> 2);
        x = ((x & 0x0F0F0F0F0F0F0F0FUL) << 4)  | ((x & 0xF0F0F0F0F0F0F0F0UL) >> 4);
        x = ((x & 0x00FF00FF00FF00FFUL) << 8)  | ((x & 0xFF00FF00FF00FF00UL) >> 8);
        x = ((x & 0x0000FFFF0000FFFFUL) << 16) | ((x & 0xFFFF0000FFFF0000UL) >> 16);
        return (x << 32) | (x >> 32);
    }
    if (ubits == 32u) { uint y = uint(x);
        y = ((y & 0x33333333u)<<2)|((y & 0xCCCCCCCCu)>>2);
        y = ((y & 0x0F0F0F0Fu)<<4)|((y & 0xF0F0F0F0u)>>4);
        y = ((y & 0x00FF00FFu)<<8)|((y & 0xFF00FF00u)>>8);
        y = (y<<16)|(y>>16); return uint64_t(y); }
    if (ubits == 16u) { uint y = uint(x) & 0xFFFFu;
        y = ((y & 0x3333u)<<2)|((y & 0xCCCCu)>>2);
        y = ((y & 0x0F0Fu)<<4)|((y & 0xF0F0u)>>4);
        y = ((y & 0x00FFu)<<8)|((y & 0xFF00u)>>8); return uint64_t(y); }
    uint y = uint(x) & 0xFFu;                       // ubits == 8
        y = ((y & 0x33u)<<2)|((y & 0xCCu)>>2);
        y = ((y & 0x0Fu)<<4)|((y & 0xF0u)>>4); return uint64_t(y);
}

uint64_t canon(uint64_t raw, uint valid) {
    uint64_t x = (valid >= lpu()) ? raw : (raw & low_symbol_mask(valid, 2u));
    return canon_dna4(x, pc.unit_bits);
}
)GLSL";

/// generic canonicalizer: alphabet params from push constants, masks used_mask.
constexpr std::string_view kCanonGeneric = R"GLSL(
uint lpu()       { return pc.letters_per_unit; }
uint unit_bits() { return pc.unit_bits; }

uint64_t canon_generic(uint64_t raw, uint lp, uint bpc, uint64_t cmask) {
    uint64_t outv = 0ul;
    for (uint k = 0u; k < lp; ++k) {
        uint64_t sym = (raw >> (k * bpc)) & cmask;          // base k (LSB-first in raw)
        outv |= sym << ((lp - 1u - k) * bpc);               // -> reversed slot
    }
    return outv;   // valid bases occupy the TOP lp*bpc bits; low bits = 0
}

uint64_t canon(uint64_t raw, uint valid) {
    uint64_t used = (uint64_t(pc.used_mask_hi) << 32) | uint64_t(pc.used_mask_lo);
    uint64_t m = raw & used;                                // clear unit_leftover_bits
    if (valid < lpu()) m &= low_symbol_mask(valid, pc.bits_per_char);
    return canon_generic(m, lpu(), pc.bits_per_char, uint64_t(pc.char_mask));
}
)GLSL";

/// Kernel A main: one invocation per row -> (key:uint64, value:uint64=row index).
constexpr std::string_view kSeqSortKeyMain = R"GLSL(
layout(local_size_x = LOCAL_SIZE_X) in;
layout(std430, set=0, binding=2) writeonly buffer Keys { uint64_t keys[]; };
layout(std430, set=0, binding=3) writeonly buffer Vals { uint64_t vals[]; };
void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= pc.count) return;
    keys[i] = canon_key(descs[i]);
    vals[i] = uint64_t(i);
}
)GLSL";

/// Kernel B main: one workgroup per equal-key run; bitonic over canon_cmp.
constexpr std::string_view kSeqSortTiebreakMain = R"GLSL(
layout(local_size_x = LOCAL_SIZE_X) in;
layout(std430, set=0, binding=2) buffer Index           { uint index[];  };       // radix-sorted row ids (in/out)
struct SeqSortRange { uint begin; uint len; };
layout(std430, set=0, binding=3) readonly buffer Ranges { SeqSortRange ranges[]; };
shared uint sh_idx[LOCAL_SIZE_X];
void main() {
    uint rid = gl_WorkGroupID.x; uint t = gl_LocalInvocationID.x;
    if (rid >= pc.count) return;
    SeqSortRange r = ranges[rid];
    if (r.len > LOCAL_SIZE_X) return;                       // global GPU pass handles oversized runs
    sh_idx[t] = (t < r.len) ? index[r.begin + t] : 0xffffffffu;   // sentinel sorts last
    barrier();
    for (uint size = 2u; size <= LOCAL_SIZE_X; size <<= 1u)
      for (uint stride = size >> 1u; stride > 0u; stride >>= 1u) {
        uint o = t ^ stride;
        if (o > t) {
            uint ia = sh_idx[t], ib = sh_idx[o];
            bool up = (t & size) == 0u;
            int c;
            if (ia == 0xffffffffu) c = 1; else if (ib == 0xffffffffu) c = -1;
            else { c = canon_cmp(descs[ia], descs[ib]); if (c == 0) c = (ia < ib) ? -1 : 1; }
            bool do_swap = up ? (c > 0) : (c < 0);
            if (do_swap) { sh_idx[t] = ib; sh_idx[o] = ia; }
        }
        barrier();
      }
    if (t < r.len) index[r.begin + t] = sh_idx[t];
}
)GLSL";

/// Kernel C main: one global bitonic compare-swap pass over the padded index
/// buffer. Used only when a 64-bit key collision run is too large for the
/// per-workgroup tiebreak kernel.
constexpr std::string_view kSeqSortGlobalCompareMain = R"GLSL(
layout(local_size_x = LOCAL_SIZE_X) in;
layout(std430, set=0, binding=2) buffer Index { uint index[]; };
layout(std430, set=0, binding=3) readonly buffer UnusedRanges { uint unused_ranges[]; };
void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= pc.count) return;
    uint other = i ^ pc.sort_stride;
    if (other <= i || other >= pc.count) return;

    uint ia = index[i];
    uint ib = index[other];
    bool up = (i & pc.sort_size) == 0u;
    int c;
    if (ia == 0xffffffffu) c = 1;
    else if (ib == 0xffffffffu) c = -1;
    else {
        c = canon_cmp(descs[ia], descs[ib]);
        if (c == 0) c = (ia < ib) ? -1 : 1;
    }
    bool do_swap = up ? (c > 0) : (c < 0);
    if (do_swap) {
        index[i] = ib;
        index[other] = ia;
    }
}
)GLSL";

/// Kernel D main: dense-rank scatter. This is a verified fast path for batches
/// whose selected 64-bit canonical key is exactly a permutation of [0,count).
/// For non-zero key offsets the shader also proves, on device, that all earlier
/// units match row 0; otherwise sorting by a later key would not be lexicographic.
constexpr std::string_view kSeqSortDenseScatterMain = R"GLSL(
layout(local_size_x = LOCAL_SIZE_X) in;
layout(std430, set=0, binding=2) buffer Index { uint index[]; };
layout(std430, set=0, binding=3) buffer Status { uint status[]; };

bool prefix_matches_first(SeqDesc d) {
    if (pc.key_unit_offset == 0u) return true;
    if (pc.sort_size != 0u) return true;  // prefix_check stage already proved it
    SeqDesc first = descs[0];
    if (d.bp_size != first.bp_size || d.unit_count != first.unit_count) return false;
    if (pc.key_unit_offset > d.unit_count) return false;
    for (uint j = 0u; j < pc.key_unit_offset; ++j) {
        uint base0 = j * lpu();
        if (base0 >= d.bp_size) break;
        uint valid = min(lpu(), d.bp_size - base0);
        uint64_t ca = canon(read_unit(d, j), valid);
        uint64_t cb = canon(read_unit(first, j), valid);
        if (ca != cb) return false;
    }
    return true;
}

void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= pc.count) return;
    SeqDesc d = descs[i];
    if (!prefix_matches_first(d)) {
        atomicOr(status[0], 2u);
        return;
    }
    uint64_t key = canon_key_at(d, pc.key_unit_offset);
    if ((key >> 32) != 0ul || uint(key) >= pc.count) {
        atomicOr(status[0], 1u);
        return;
    }
    uint dst = uint(key);
    uint old = atomicExchange(index[dst], i);
    if (old != 0xffffffffu) {
        atomicOr(status[0], 1u);
    }
}
)GLSL";

/// Kernel E main: parallel common-prefix proof for dense tail keys. One
/// invocation checks one (row, unit-before-key) pair against row 0 and ORs the
/// status word on any mismatch. Dense scatter then runs after a command-buffer
/// barrier with pc.sort_size != 0 to avoid rechecking the prefix serially.
constexpr std::string_view kSeqSortPrefixCheckMain = R"GLSL(
layout(local_size_x = LOCAL_SIZE_X) in;
layout(std430, set=0, binding=2) readonly buffer UnusedIndex { uint unused_index[]; };
layout(std430, set=0, binding=3) buffer Status { uint status[]; };

void main() {
    if (pc.key_unit_offset == 0u) return;
    uint linear_id = gl_GlobalInvocationID.x;
    uint row = linear_id / pc.key_unit_offset;
    uint j = linear_id - row * pc.key_unit_offset;
    if (row >= pc.count) return;

    SeqDesc d = descs[row];
    SeqDesc first = descs[0];
    if (d.bp_size != first.bp_size || d.unit_count != first.unit_count
        || pc.key_unit_offset > d.unit_count) {
        atomicOr(status[0], 2u);
        return;
    }
    uint base0 = j * lpu();
    if (base0 >= d.bp_size) return;
    uint valid = min(lpu(), d.bp_size - base0);
    uint64_t ca = canon(read_unit(d, j), valid);
    uint64_t cb = canon(read_unit(first, j), valid);
    if (ca != cb) {
        atomicOr(status[0], 2u);
    }
}
)GLSL";

enum class SeqSortStage : std::uint8_t {
    key,
    tiebreak,
    global_compare,
    dense_scatter,
    prefix_check,
};

/// Compose a seq-sort kind's full body from the shared snippets. Order matters:
/// push -> readers/low_symbol_mask -> canon (defines lpu/unit_bits/canon) ->
/// key/compare (uses canon) -> stage main.
[[nodiscard]] std::string seq_sort_body(bool dna4, SeqSortStage stage) {
    std::string s;
    s.reserve(kSeqSortPush.size() + kSeqSortReaders.size() + kSeqSortKeyCmp.size() + 1536);
    s += kSeqSortPush;
    s += kSeqSortReaders;
    s += (dna4 ? kCanonDna4 : kCanonGeneric);
    s += kSeqSortKeyCmp;
    switch (stage) {
        case SeqSortStage::key:            s += kSeqSortKeyMain; break;
        case SeqSortStage::tiebreak:       s += kSeqSortTiebreakMain; break;
        case SeqSortStage::global_compare: s += kSeqSortGlobalCompareMain; break;
        case SeqSortStage::dense_scatter:  s += kSeqSortDenseScatterMain; break;
        case SeqSortStage::prefix_check:   s += kSeqSortPrefixCheckMain; break;
    }
    return s;
}

/// Resolve a shader kind to its embedded GLSL body (everything after the
/// `#version`/`#extension`/`#define` prelude the compiler injects).
[[nodiscard]] std::string shader_body_for(ShaderKind kind) {
    switch (kind) {
        case ShaderKind::chain_tiled_dp_u64:
            return std::string{kChainTiledDpBody};
        case ShaderKind::seed_lookup_anchors:
            return std::string{kSeedLookupAnchorsBody};
        case ShaderKind::sort_anchors_bitonic:
            return std::string{kSortAnchorsBitonicBody};
        // DOC0310 Phase C. The sketch family shares one tile-replay body (the
        // packed-sequence reader helpers are inlined inside it).
        case ShaderKind::phase_c_sketch_tile:
        case ShaderKind::phase_c_sketch_emit_flags:
        case ShaderKind::phase_c_sketch_emit_carry:
        case ShaderKind::phase_c_sketch_emit_words:
            return std::string{kPhaseCSketchBody};
        case ShaderKind::phase_c_prefix_scan_block:
            return std::string{kPhaseCScanBlockBody};
        case ShaderKind::phase_c_prefix_scan_add:
            return std::string{kPhaseCScanAddBody};
        case ShaderKind::phase_c_seed_count:
        case ShaderKind::phase_c_seed_emit:
            return std::string{kPhaseCSeedBody};
        case ShaderKind::phase_c_shape_v5_seed_count:
        case ShaderKind::phase_c_shape_v5_seed_emit:
            return std::string{kPhaseCShapeV5SeedBody};
        case ShaderKind::phase_c_persisted_directory_select:
            return std::string{kPhaseCPersistedDirectorySelectBody};
        case ShaderKind::phase_c_grouped32_validate:
            return grouped32_abi2_body(kPhaseCGrouped32ValidateMain);
        case ShaderKind::phase_c_grouped32_accumulate_occurrences:
            return grouped32_abi2_body(
                kPhaseCGrouped32AccumulateOccurrencesMain);
        case ShaderKind::phase_c_grouped32_finalize_max_occ:
            return grouped32_abi2_body(kPhaseCGrouped32FinalizeMaxOccMain);
        case ShaderKind::phase_c_grouped32_allocate_anchor_prefix:
            return grouped32_abi2_body(
                kPhaseCGrouped32AllocateAnchorPrefixMain);
        case ShaderKind::phase_c_grouped32_emit_global_anchors:
            return grouped32_abi2_body(
                kPhaseCGrouped32EmitGlobalAnchorsMain);
        case ShaderKind::phase_c_grouped32_build_sort_segments:
            return grouped32_abi2_body(
                kPhaseCGrouped32BuildSortSegmentsMain);
        case ShaderKind::phase_c_grouped32_adapt_query_sketch:
            return std::string{kPhaseCGrouped32AdaptQuerySketchBody};
        case ShaderKind::phase_c_grouped32_prepare_indirect:
            return std::string{kPhaseCGrouped32PrepareIndirectBody};
        case ShaderKind::phase_c_grouped32_clear_state:
            return grouped32_abi2_body(kPhaseCGrouped32ClearStateMain);
        case ShaderKind::phase_c_grouped32_finalize_query_adapter:
            return std::string{kPhaseCGrouped32FinalizeQueryAdapterBody};
        case ShaderKind::phase_c_grouped32_finalize_validation:
            return grouped32_abi2_body(
                kPhaseCGrouped32FinalizeValidationMain);
        case ShaderKind::phase_c_grouped32_stream_control:
            return grouped32_stream_body(
                kPhaseCGrouped32StreamControlMain);
        case ShaderKind::phase_c_grouped32_stream_authorize:
            return grouped32_stream_body(
                kPhaseCGrouped32StreamAuthorizeMain);
        case ShaderKind::phase_c_grouped32_stream_position:
            return grouped32_stream_body(
                kPhaseCGrouped32StreamPositionMain);
        case ShaderKind::phase_c_grouped32_stream_seal:
            return grouped32_stream_body(
                kPhaseCGrouped32StreamSealMain);
        case ShaderKind::phase_c_sort_radix_histogram:
            return std::string{kPhaseCSortHistogramBody};
        case ShaderKind::phase_c_sort_radix_scan:
            return std::string{kPhaseCSortScanBody};
        case ShaderKind::phase_c_sort_radix_scatter:
            return std::string{kPhaseCSortScatterBody};
        case ShaderKind::phase_c_index_mark_runs:
            return std::string{kPhaseCIndexMarkRunsBody};
        case ShaderKind::phase_c_index_emit_buckets:
            return std::string{kPhaseCIndexEmitBucketsBody};
        case ShaderKind::phase_c_index_claim_slots:
            return std::string{kPhaseCIndexClaimSlotsBody};
        case ShaderKind::phase_c_index_emit_slots:
            return std::string{kPhaseCIndexEmitSlotsBody};
        case ShaderKind::phase_c_chain_range_build:
            return std::string{kPhaseCChainRangeBuildBody};
        case ShaderKind::phase_c_coordinate_reduce:
            return std::string{kPhaseCCoordinateReduceBody};
        case ShaderKind::phase_c_coordinate_vote_reduce:
            return std::string{kPhaseCCoordinateVoteReduceBody};
        case ShaderKind::phase_c_coordinate_vote_reduce_batch:
            return std::string{kPhaseCCoordinateVoteReduceBatchBody};
        case ShaderKind::generate_units:
            return std::string{kGenerateUnitsBody};
        case ShaderKind::seq_sort_extract_key_dna4:
            return seq_sort_body(/*dna4=*/true,  SeqSortStage::key);
        case ShaderKind::seq_sort_extract_key_generic:
            return seq_sort_body(/*dna4=*/false, SeqSortStage::key);
        case ShaderKind::seq_sort_tiebreak_dna4:
            return seq_sort_body(/*dna4=*/true,  SeqSortStage::tiebreak);
        case ShaderKind::seq_sort_tiebreak_generic:
            return seq_sort_body(/*dna4=*/false, SeqSortStage::tiebreak);
        case ShaderKind::seq_sort_global_compare_dna4:
            return seq_sort_body(/*dna4=*/true,  SeqSortStage::global_compare);
        case ShaderKind::seq_sort_global_compare_generic:
            return seq_sort_body(/*dna4=*/false, SeqSortStage::global_compare);
        case ShaderKind::seq_sort_dense_scatter_dna4:
            return seq_sort_body(/*dna4=*/true,  SeqSortStage::dense_scatter);
        case ShaderKind::seq_sort_dense_scatter_generic:
            return seq_sort_body(/*dna4=*/false, SeqSortStage::dense_scatter);
        case ShaderKind::seq_sort_prefix_check_dna4:
            return seq_sort_body(/*dna4=*/true,  SeqSortStage::prefix_check);
        case ShaderKind::seq_sort_prefix_check_generic:
            return seq_sort_body(/*dna4=*/false, SeqSortStage::prefix_check);
        case ShaderKind::align_extend_banded_affine:
            return std::string{kAlignmentExtensionBandedAffineBody};
        case ShaderKind::align_extend_simple_cigar:
            return std::string{kAlignmentExtensionSimpleCigarBody};
    }
    throw ShaderCompileError("compile_shader_spirv: unknown ShaderKind");
}

/// glslang's process-wide initialiser is reference-counted but not safe to
/// race; call it exactly once for the process lifetime. We intentionally do not
/// `FinalizeProcess()` — the cost is a small one-time leak at exit, and it
/// avoids a teardown race if any thread is mid-compile.
void ensure_glslang_initialized() {
    static std::once_flag init_flag;
    std::call_once(init_flag, []() { glslang::InitializeProcess(); });
}

/// Default per-stage resource limits for a Vulkan compute shader. glslang's
/// `Vulkan::glslang` aggregate target does NOT ship `GetDefaultResources()`
/// (that lives in the separate default-resource-limits library), so the Phase A
/// kernel carries its own conservative limit table. The values mirror glslang's
/// canonical `DefaultTBuiltInResource`.
[[nodiscard]] TBuiltInResource make_default_resource() {
    TBuiltInResource resource{};
    resource.maxLights = 32;
    resource.maxClipPlanes = 6;
    resource.maxTextureUnits = 32;
    resource.maxTextureCoords = 32;
    resource.maxVertexAttribs = 64;
    resource.maxVertexUniformComponents = 4096;
    resource.maxVaryingFloats = 64;
    resource.maxVertexTextureImageUnits = 32;
    resource.maxCombinedTextureImageUnits = 80;
    resource.maxTextureImageUnits = 32;
    resource.maxFragmentUniformComponents = 4096;
    resource.maxDrawBuffers = 32;
    resource.maxVertexUniformVectors = 128;
    resource.maxVaryingVectors = 8;
    resource.maxFragmentUniformVectors = 16;
    resource.maxVertexOutputVectors = 16;
    resource.maxFragmentInputVectors = 15;
    resource.minProgramTexelOffset = -8;
    resource.maxProgramTexelOffset = 7;
    resource.maxClipDistances = 8;
    resource.maxComputeWorkGroupCountX = 65535;
    resource.maxComputeWorkGroupCountY = 65535;
    resource.maxComputeWorkGroupCountZ = 65535;
    resource.maxComputeWorkGroupSizeX = 1024;
    resource.maxComputeWorkGroupSizeY = 1024;
    resource.maxComputeWorkGroupSizeZ = 64;
    resource.maxComputeUniformComponents = 1024;
    resource.maxComputeTextureImageUnits = 16;
    resource.maxComputeImageUniforms = 8;
    resource.maxComputeAtomicCounters = 8;
    resource.maxComputeAtomicCounterBuffers = 1;
    resource.maxVaryingComponents = 60;
    resource.maxVertexOutputComponents = 64;
    resource.maxGeometryInputComponents = 64;
    resource.maxGeometryOutputComponents = 128;
    resource.maxFragmentInputComponents = 128;
    resource.maxImageUnits = 8;
    resource.maxCombinedImageUnitsAndFragmentOutputs = 8;
    resource.maxCombinedShaderOutputResources = 8;
    resource.maxImageSamples = 0;
    resource.maxVertexImageUniforms = 0;
    resource.maxTessControlImageUniforms = 0;
    resource.maxTessEvaluationImageUniforms = 0;
    resource.maxGeometryImageUniforms = 0;
    resource.maxFragmentImageUniforms = 8;
    resource.maxCombinedImageUniforms = 8;
    resource.maxGeometryTextureImageUnits = 16;
    resource.maxGeometryOutputVertices = 256;
    resource.maxGeometryTotalOutputComponents = 1024;
    resource.maxGeometryUniformComponents = 1024;
    resource.maxGeometryVaryingComponents = 64;
    resource.maxTessControlInputComponents = 128;
    resource.maxTessControlOutputComponents = 128;
    resource.maxTessControlTextureImageUnits = 16;
    resource.maxTessControlUniformComponents = 1024;
    resource.maxTessControlTotalOutputComponents = 4096;
    resource.maxTessEvaluationInputComponents = 128;
    resource.maxTessEvaluationOutputComponents = 128;
    resource.maxTessEvaluationTextureImageUnits = 16;
    resource.maxTessEvaluationUniformComponents = 1024;
    resource.maxTessPatchComponents = 120;
    resource.maxPatchVertices = 32;
    resource.maxTessGenLevel = 64;
    resource.maxViewports = 16;
    resource.maxVertexAtomicCounters = 0;
    resource.maxTessControlAtomicCounters = 0;
    resource.maxTessEvaluationAtomicCounters = 0;
    resource.maxGeometryAtomicCounters = 0;
    resource.maxFragmentAtomicCounters = 8;
    resource.maxCombinedAtomicCounters = 8;
    resource.maxAtomicCounterBindings = 1;
    resource.maxVertexAtomicCounterBuffers = 0;
    resource.maxTessControlAtomicCounterBuffers = 0;
    resource.maxTessEvaluationAtomicCounterBuffers = 0;
    resource.maxGeometryAtomicCounterBuffers = 0;
    resource.maxFragmentAtomicCounterBuffers = 1;
    resource.maxCombinedAtomicCounterBuffers = 1;
    resource.maxAtomicCounterBufferSize = 16384;
    resource.maxTransformFeedbackBuffers = 4;
    resource.maxTransformFeedbackInterleavedComponents = 64;
    resource.maxCullDistances = 8;
    resource.maxCombinedClipAndCullDistances = 8;
    resource.maxSamples = 4;
    resource.maxMeshOutputVerticesNV = 256;
    resource.maxMeshOutputPrimitivesNV = 512;
    resource.maxMeshWorkGroupSizeX_NV = 32;
    resource.maxMeshWorkGroupSizeY_NV = 1;
    resource.maxMeshWorkGroupSizeZ_NV = 1;
    resource.maxTaskWorkGroupSizeX_NV = 32;
    resource.maxTaskWorkGroupSizeY_NV = 1;
    resource.maxTaskWorkGroupSizeZ_NV = 1;
    resource.maxMeshViewCountNV = 4;
    resource.limits.nonInductiveForLoops = true;
    resource.limits.whileLoops = true;
    resource.limits.doWhileLoops = true;
    resource.limits.generalUniformIndexing = true;
    resource.limits.generalAttributeMatrixVectorIndexing = true;
    resource.limits.generalVaryingIndexing = true;
    resource.limits.generalSamplerIndexing = true;
    resource.limits.generalVariableIndexing = true;
    resource.limits.generalConstantMatrixVectorIndexing = true;
    return resource;
}

}  // namespace

std::vector<std::uint32_t> compile_shader_spirv(const ShaderCompileRequest& request) {
    // Reject geometry the shared-array layout cannot represent BEFORE compiling.
    // 1024 is the largest tile the Phase A pipeline selector ever requests
    // (choose_vulkan_chaining_pipeline), and a zero local size has no invocation
    // to run the tile loop. The device's maxComputeWorkGroupInvocations bound on
    // local_size is enforced by the runtime, which is the only place that knows
    // the limit.
    if (request.tile_size == 0 || request.tile_size > 1024) {
        throw ShaderCompileError(fmt::format(
            "compile_shader_spirv: tile_size {} out of range (1..1024)",
            request.tile_size));
    }
    if (request.local_size == 0) {
        throw ShaderCompileError("compile_shader_spirv: local_size must be non-zero");
    }
    if ((request.kind == ShaderKind::align_extend_banded_affine
         || request.kind == ShaderKind::align_extend_simple_cigar)
        && (request.max_span == 0 || request.max_span > 512)) {
        throw ShaderCompileError(fmt::format(
            "compile_shader_spirv: alignment max_span {} out of range (1..512)",
            request.max_span));
    }

    ensure_glslang_initialized();

    const std::string source = fmt::format(
        "#version 460\n"
        "#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require\n"
        "#define LOCAL_SIZE_X {}\n"
        "#define TILE_SIZE {}\n"
        "#define MAX_SPAN {}\n"
        "{}",
        request.local_size, request.tile_size, request.max_span,
        shader_body_for(request.kind));

    const TBuiltInResource resource = make_default_resource();

    glslang::TShader shader(EShLangCompute);
    const char* source_ptr = source.c_str();
    shader.setStrings(&source_ptr, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, EShLangCompute,
                       glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_2);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_5);

    const EShMessages messages =
        static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);

    if (!shader.parse(&resource, 100, false, messages)) {
        const std::string log = fmt::format(
            "glslang parse failed: {} {}", shader.getInfoLog(),
            shader.getInfoDebugLog());
        bioseq_log<ERR>("[shader_compiler] {}", log);
        throw ShaderCompileError(log);
    }

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
        const std::string log = fmt::format(
            "glslang link failed: {} {}", program.getInfoLog(),
            program.getInfoDebugLog());
        bioseq_log<ERR>("[shader_compiler] {}", log);
        throw ShaderCompileError(log);
    }

    std::vector<std::uint32_t> spirv;
    glslang::SpvOptions spv_options;
    spv_options.disableOptimizer = true;
    spv_options.validate = false;
    spv::SpvBuildLogger logger;
    glslang::GlslangToSpv(*program.getIntermediate(EShLangCompute), spirv,
                          &logger, &spv_options);
    if (spirv.empty()) {
        const std::string log = fmt::format(
            "glslang produced empty SPIR-V: {}", logger.getAllMessages());
        bioseq_log<ERR>("[shader_compiler] {}", log);
        throw ShaderCompileError(log);
    }
    bioseq_log<DBG>(
        "[shader_compiler] compiled shader kind={} tile={} local={} max_span={} -> {} words",
        static_cast<int>(request.kind), request.tile_size, request.local_size,
        request.max_span, spirv.size());
    return spirv;
}

std::string shader_body_for_digest(ShaderKind kind) {
    return shader_body_for(kind);
}

}  // namespace bioseq::gpu::vulkan
