/**
 * @file shader_compiler.cppm
 * @brief Embedded GLSL selection and SPIR-V compilation for Vulkan compute kernels.
 *
 * Shader bodies are embedded as C++ raw-string literals, so compilation has no
 * runtime working-directory dependency. Workgroup, tile, and algorithm bounds
 * are injected as compile-time definitions because shared-array extents are part
 * of the emitted module layout. `Vulkan::glslang` is the compiler interface.
 */
module;

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

import bioseq.common.logging;

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
    // Rank-buffer SneakySnake prefilter.
    sneaky_snake_rank_batch = 53,
    // Rank-buffer GateKeeper threshold prefilter.
    gatekeeper_rank_batch = 54,
    // Compile-time production geometries reduce the generic kernel's fixed-DP
    // and cross-geometry register footprint for homogeneous small-seed runs.
    gatekeeper_rank_batch_l32_e2 = 55,
    gatekeeper_rank_batch_l32_e4 = 56,
    gatekeeper_rank_batch_l64_e2 = 57,
    gatekeeper_rank_batch_l64_e4 = 58,
};

/// @brief Return whether a shader kind belongs to the GateKeeper rank-batch family.
[[nodiscard]] inline constexpr bool is_gatekeeper_rank_batch_kind(
    shader_kind kind) noexcept
{
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

/// @brief Descriptor-set layouts shared by compatible shader families.
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
    seq_sort = 11,   // { sequence words, descriptors, keys/indices, values/ranges }
    align_extension = 12,  // { QuerySymbols, TargetSymbols, Jobs, Results }
    align_extension_cigar = 13,
    // Nine-buffer direct grouped-SoA seed layout.
    grouped_soa_seed = 18,
};

/// @brief Number of std430 storage-buffer bindings in each descriptor layout.
[[nodiscard]] constexpr std::uint32_t descriptor_binding_count(
    descriptor_layout_kind kind) noexcept
{
    switch (kind) {
        case descriptor_layout_kind::chain_tiled_dp:        return 4;
        case descriptor_layout_kind::seed_lookup:           return 6;
        case descriptor_layout_kind::anchor_sort:           return 2;
        // Resident frontend layouts.
        case descriptor_layout_kind::sketch:        return 9;
        case descriptor_layout_kind::prefix_scan:          return 3;
        case descriptor_layout_kind::seed:          return 8;
        case descriptor_layout_kind::radix_sort:          return 5;
        case descriptor_layout_kind::target_index:         return 7;
        case descriptor_layout_kind::chain_range:   return 6;
        case descriptor_layout_kind::coordinate_reduce: return 6;
        case descriptor_layout_kind::generate_units:         return 2;
        case descriptor_layout_kind::seq_sort:               return 4;
        case descriptor_layout_kind::align_extension:        return 4;
        case descriptor_layout_kind::align_extension_cigar:  return 6;
        case descriptor_layout_kind::grouped_soa_seed: return 9;
    }
    return 0;
}

/// @brief Map a shader kind to the descriptor layout it binds.
[[nodiscard]] constexpr descriptor_layout_kind descriptor_layout_for(
    shader_kind kind) noexcept
{
    switch (kind) {
        case shader_kind::chain_tiled_dp_u64:    return descriptor_layout_kind::chain_tiled_dp;
        case shader_kind::seed_lookup_anchors:   return descriptor_layout_kind::seed_lookup;
        case shader_kind::sort_anchors_bitonic:  return descriptor_layout_kind::anchor_sort;
        case shader_kind::ordered_sketch_tile:
        case shader_kind::sketch_emit_flags:
        case shader_kind::sketch_emit_carry:
        case shader_kind::sketch_emit_words:   return descriptor_layout_kind::sketch;
        case shader_kind::prefix_scan_block:
        case shader_kind::prefix_scan_add:             return descriptor_layout_kind::prefix_scan;
        case shader_kind::seed_count:
        case shader_kind::seed_emit:           return descriptor_layout_kind::seed;
        case shader_kind::grouped_soa_seed_count:
        case shader_kind::grouped_soa_seed_emit:
            return descriptor_layout_kind::grouped_soa_seed;
        case shader_kind::radix_sort_histogram:
        case shader_kind::radix_sort_scan:
        case shader_kind::radix_sort_scatter:  return descriptor_layout_kind::radix_sort;
        case shader_kind::target_index_mark_runs:
        case shader_kind::target_index_emit_buckets:
        case shader_kind::target_index_claim_slots:
        case shader_kind::target_index_emit_slots:    return descriptor_layout_kind::target_index;
        case shader_kind::chain_range_build:   return descriptor_layout_kind::chain_range;
        case shader_kind::coordinate_reduce:
        case shader_kind::coordinate_vote_reduce:
        case shader_kind::coordinate_vote_reduce_batch:
            return descriptor_layout_kind::coordinate_reduce;
        case shader_kind::generate_units:              return descriptor_layout_kind::generate_units;
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
        case shader_kind::align_extend_banded_affine:  return descriptor_layout_kind::align_extension;
        case shader_kind::align_extend_simple_cigar:   return descriptor_layout_kind::align_extension_cigar;
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
        case shader_kind::chain_tiled_dp_u64:    return 36;  // ChainingPushConstants
        case shader_kind::seed_lookup_anchors:   return 44;  // 11 x uint32
        case shader_kind::sort_anchors_bitonic:  return 0;
        // Per-family push blocks are mirrored by runtime wire structures.
        case shader_kind::ordered_sketch_tile:
        case shader_kind::sketch_emit_flags:
        case shader_kind::sketch_emit_carry:
        case shader_kind::sketch_emit_words:   return 72;  // SketchPushC (18 × u32)
        case shader_kind::prefix_scan_block:
        case shader_kind::prefix_scan_add:             return 16;  // ScanPushC (4 × u32)
        case shader_kind::seed_count:
        case shader_kind::seed_emit:           return 52;  // SeedPushC (13 × u32)
        case shader_kind::grouped_soa_seed_count:
        case shader_kind::grouped_soa_seed_emit:
            return 72;  // grouped-SoA seed push block
        case shader_kind::radix_sort_histogram:
        case shader_kind::radix_sort_scan:
        case shader_kind::radix_sort_scatter:  return 20;  // SortPushC (5 × u32)
        case shader_kind::target_index_mark_runs:
        case shader_kind::target_index_emit_buckets:
        case shader_kind::target_index_claim_slots:
        case shader_kind::target_index_emit_slots:    return 24;  // IndexPushC (6 × u32)
        case shader_kind::chain_range_build:   return 8;   // ChainRangePushC (2 × u32)
        case shader_kind::coordinate_reduce:
        case shader_kind::coordinate_vote_reduce:
        case shader_kind::coordinate_vote_reduce_batch:
            return 28;  // GpuCoordinateReducePush
        case shader_kind::generate_units:              return 48;  // GenerateUnitsPush (12 × u32)
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
            return 40;  // SeqSortPush (10 × u32)
        case shader_kind::align_extend_banded_affine:  return 44;  // AlignmentExtensionPush
        case shader_kind::align_extend_simple_cigar:   return 60;  // AlignmentCigarPush
        case shader_kind::sneaky_snake_rank_batch:
        case shader_kind::gatekeeper_rank_batch:
        case shader_kind::gatekeeper_rank_batch_l32_e2:
        case shader_kind::gatekeeper_rank_batch_l32_e4:
        case shader_kind::gatekeeper_rank_batch_l64_e2:
        case shader_kind::gatekeeper_rank_batch_l64_e4:
            return 16;  // GpuPrefilterPush
    }
    return 0;
}

/// @brief One shader-compilation request. `tile_size`, `local_size`, and `max_span`
/// are substituted as compile-time `#define`s before glslang runs. A shader may
/// intentionally bake a fixed workgroup geometry instead of consuming
/// `LOCAL_SIZE_X`; `expected_shader_workgroup_size()` is the authoritative
/// compile/cache/runtime ABI for that distinction. The remaining fields
/// describe runtime push-constant routing and are carried for the pipeline
/// cache key.
struct shader_compile_request {
    shader_kind kind;
    std::uint32_t tile_size = 384;       // chain only
    std::uint32_t local_size = 128;
    std::uint32_t max_span = 256;        // shader-specific compile-time span
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

    [[nodiscard]] bool operator==(const shader_workgroup_size&) const = default;
};

/// @brief Resolve compile-time execution geometry encoded in SPIR-V. Most kernels consume the
/// request's LOCAL_SIZE_X macro. The radix kernels deliberately keep their
/// established cache-key sentinel (`local_size == 1`) while their algorithm
/// requires one lane per radix bin. Keep this mapping explicit: treating a key
/// selector as reflected execution metadata rejects valid AOT artifacts and
/// makes archive publication/completeness disagree.
[[nodiscard]] inline constexpr shader_workgroup_size expected_shader_workgroup_size(
    const shader_compile_request& request) noexcept
{
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
/// `vulkan_runtime_error`; selected Vulkan algorithms propagate that error.
class shader_compile_error final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/**
 * @brief Compile a registered compute shader to SPIR-V words.
 *
 * Builds the full GLSL source by prepending `#version`, the int64 extension,
 * and the two computed `#define`s to the embedded shader body, then drives
 * glslang (Vulkan 1.2 client, SPIR-V 1.5 target).
 *
 * @param request  Shader kind plus the tile/local workgroup dimensions.
 * @return The compiled SPIR-V module as a vector of 32-bit words.
 * @throws shader_compile_error if `tile_size > 1024`, `local_size == 0`, or
 *         glslang fails to parse or link the shader.
 */
[[nodiscard]] std::vector<std::uint32_t> compile_shader_spirv(
    const shader_compile_request& request);

/// @brief Return the embedded GLSL body used by cache identity computation.
///
/// @param kind Registered shader family.
/// @return The exact embedded GLSL body without the compiler prelude.
[[nodiscard]] std::string shader_body_for_digest(shader_kind kind);

}  // namespace bioseq::gpu::vulkan

// ============================================================================
// Implementation
// ============================================================================

namespace bioseq::gpu::vulkan {

namespace {

/// @brief The tiled DP score-generation kernel body, beginning right after the two
/// `#define`s the compiler injects. Ported from G3SA's
/// `mm_compute_chain_long_tiled` (chaining_kernel.cu) but adapted to bioseq's
/// 24-byte `Anchor` ABI: full 64-bit ref/query positions, per-anchor
/// `query_span` instead of the hard-coded `MM_QSPAN=15`, runtime `max_iter`
/// instead of `MAX_ITER_CHAIN=5000`, and absolute predecessor indices instead
/// of G3SA's parent-distance encoding (so `backtrack_chains` consumes them
/// directly). One workgroup owns one independent hard range, so no atomics are
/// needed for the tail sweep.
constexpr std::string_view k_chain_tiled_dp_body = R"GLSL(
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

constexpr std::string_view k_seed_lookup_anchors_body = R"GLSL(
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

/// @brief Per-query bitonic anchor sort. One workgroup sorts up to
/// LOCAL_SIZE_X anchors by `ref_bits()` in shared memory. Larger anchor sets
/// are routed to the stable device radix path; host sorting is forbidden.
constexpr std::string_view k_sort_anchors_bitonic_body = R"GLSL(
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
// Resident frontend kernels
// ===========================================================================

/// @brief Ordered tile-replay minimizer sketch. One workgroup per tile
/// (1 active invocation) replays the exact `collect_minimizers_view` state
/// machine over `[replay_lo, loop_hi)`, emitting only minimizers whose trigger
/// position falls in this tile's owned window interval `[tile_lo, tile_hi)`.
/// The left-context overlap (`tile_lo - replay_lo`) reconstructs the ring buffer
/// and the valid-base/init state so the concatenation across tiles in tile
/// order equals the single serial pass — but it is parallel across tiles (never
/// a one-workgroup-per-sequence serial emit). `emit_mode==0` counts per tile;
/// `emit_mode==1` writes raw `MinimizerRepr` words at `out_base + local`.
constexpr std::string_view k_ordered_sketch_body = R"GLSL(
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

/// @brief Generic exclusive prefix scan over a `uint` array, producing block totals.
/// Each workgroup exclusive-scans its block into `out_prefix` and writes the
/// block total to `block_sums[wg]`. A second `add` pass adds the scanned block
/// offsets back. Two-level scan (block size = LOCAL_SIZE_X handled serially by
/// invocation 0 to stay portable on MoltenVK without subgroup ops).
constexpr std::string_view k_prefix_scan_block_body = R"GLSL(
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

/// @brief Prefix-scan offset addition. Invocation 0 of workgroup 0 exclusive-
/// scans `block_sums` in place, then every element adds its block's offset.
constexpr std::string_view k_prefix_scan_add_body = R"GLSL(
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

/// @brief GPU seed count/emit. One workgroup per query record; local
/// invocations stride over that record's minimizers. `emit_mode==0` writes the
/// per-minimizer anchor count (clamped by `max_occ`) to `counts`; `emit_mode==1`
/// reads the exclusive `prefix` and emits `GpuAnchorWire` records plus their
/// compact `(strand, ref)` sort key.
constexpr std::string_view k_seed_count_emit_body = R"GLSL(
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

/// @brief Direct persisted grouped-SoA seed count/emit. One workgroup owns one
/// query record and its lanes stride over query minimizers.  Each minimizer is
/// matched against the selected resident partition descriptors, binary-searches
/// the raw variable-width SoA hash arrays, and sums occurrences across every
/// position segment before applying `max_occ`.  Emit rereads the authenticated
/// prefix ranges and variable-width segment-local positions directly; it never
/// constructs a CPU-decoded slot table or a widened/globalized position array.
constexpr std::string_view k_grouped_soa_seed_body = R"GLSL(
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

/// @brief Device radix sort over parallel (key:uint64, value:uint64)
/// arrays. One 256-lane workgroup processes each contiguous chunk; lane 0
/// assigns stable scatter destinations and all lanes move the values. Histogram
/// pass: per-chunk 256-bin counts of digit `(key >> radix_shift) & 0xff`.
constexpr std::string_view k_radix_sort_histogram_body = R"GLSL(
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

/// @brief Multi-level histogram scan. One 256-lane workgroup
/// computes, for every (workgroup, bin), its stable global start offset:
/// `bin_base[bin] + sum_{wg' < wg} hist[wg'][bin]`, where `bin_base` is the
/// exclusive prefix over bin totals. Writes the offsets back into `histogram`.
constexpr std::string_view k_radix_sort_scan_body = R"GLSL(
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

/// @brief Stable scatter. Each 256-lane workgroup re-walks its
/// chunk in order, using the per-(wg,bin) global offsets from the scan as a
/// running cursor.
constexpr std::string_view k_radix_sort_scatter_body = R"GLSL(
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

/// @brief GPU target-index run marking. `flags[i] = 1` where a
/// new hash run begins in the sorted key array.
constexpr std::string_view k_index_mark_runs_body = R"GLSL(
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

/// @brief Dense bucket emission. Each element atomically adds to its bucket's
/// `pos_count`; run starts set the bucket `hash`/`pos_offset` (the sorted
/// position index). `bucket_index = inclusive_scan(flags) - 1`.
constexpr std::string_view k_index_emit_buckets_body = R"GLSL(
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

/// @brief Race-free slot claim via a 32-bit atomicCompSwap linear probe.
constexpr std::string_view k_index_claim_slots_body = R"GLSL(
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

/// @brief Write a SerializedBucket into its claimed slot.
constexpr std::string_view k_index_emit_slots_body = R"GLSL(
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

/// @brief Chain-range construction. One workgroup (1 invocation) per query
/// serially scans that query's sorted anchor span, emitting `GpuChainRange`
/// records at strand/chromosome changes and hard reference gaps, matching the
/// CPU `build_gpu_chain_ranges` predicate. When any hard range is very long,
/// the shader also performs the same endpoint-preserving thinning used by the
/// CPU-side Vulkan chainer so resident seed-to-coordinate does not feed huge
/// dense ranges into DP.
constexpr std::string_view k_chain_range_build_body = R"GLSL(
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

/// @brief Resident coordinate-only finalizer. It mirrors the bounded top-k CPU
/// coordinate extractor, but downloads only one compact mapping record.
constexpr std::string_view k_coordinate_reduce_body = R"GLSL(
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

constexpr std::string_view k_coordinate_vote_reduce_body = R"GLSL(
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

constexpr std::string_view k_coordinate_vote_reduce_batch_body = R"GLSL(
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

/// @brief Shared rank-prefilter GLSL ABI. Both algorithms consume byte-rank streams,
/// the canonical 20-byte job/16-byte result records, and the same bounds-aware
/// 16-byte push block. Every load helper checks the exact logical byte count;
/// algorithm bodies still reject a malformed job before making any load.
constexpr std::string_view k_rank_prefilter_preamble = R"GLSL(
layout(local_size_x = LOCAL_SIZE_X, local_size_y = 1, local_size_z = 1) in;

const uint STATUS_OK = 0u;
const uint STATUS_LENGTH_DIFFERENCE_ONLY = 1u;
const uint STATUS_UNSUPPORTED = 2u;

struct PrefilterJob {
    uint query_offset;
    uint target_offset;
    uint query_length;
    uint target_length;
    uint max_edits;
};

struct PrefilterResult {
    uint lower_bound;
    uint status;
    uint accepted;
    uint reserved;
};

layout(std430, set = 0, binding = 0) readonly buffer QueryRanks {
    uint query_rank_words[];
};
layout(std430, set = 0, binding = 1) readonly buffer TargetRanks {
    uint target_rank_words[];
};
layout(std430, set = 0, binding = 2) readonly buffer Jobs {
    PrefilterJob jobs[];
};
layout(std430, set = 0, binding = 3) buffer Results {
    PrefilterResult results[];
};

layout(push_constant) uniform Params {
    uint job_count;
    uint mode;
    uint query_rank_count;
    uint target_rank_count;
} pc;

bool rank_slice_is_in_bounds(uint offset, uint length, uint rank_count) {
    return offset <= rank_count && length <= rank_count - offset;
}

bool prefilter_job_is_in_bounds(PrefilterJob job) {
    return rank_slice_is_in_bounds(
               job.query_offset, job.query_length, pc.query_rank_count)
        && rank_slice_is_in_bounds(
               job.target_offset, job.target_length, pc.target_rank_count);
}

uint query_rank(uint rank_index) {
    if (rank_index >= pc.query_rank_count) return 0u;
    uint word = query_rank_words[rank_index >> 2u];
    return (word >> ((rank_index & 3u) << 3u)) & 0xffu;
}

uint target_rank(uint rank_index) {
    if (rank_index >= pc.target_rank_count) return 0u;
    uint word = target_rank_words[rank_index >> 2u];
    return (word >> ((rank_index & 3u) << 3u)) & 0xffu;
}

// These unchecked readers are only used after prefilter_job_is_in_bounds has
// accepted the complete slice and an algorithm has proved its relative index
// is inside that slice. Keeping that proof at the job boundary avoids repeating
// four push-constant comparisons for every byte/quad in short hot kernels.
uint query_rank_unchecked(uint rank_index) {
    uint word = query_rank_words[rank_index >> 2u];
    return (word >> ((rank_index & 3u) << 3u)) & 0xffu;
}

uint target_rank_unchecked(uint rank_index) {
    uint word = target_rank_words[rank_index >> 2u];
    return (word >> ((rank_index & 3u) << 3u)) & 0xffu;
}

uint query_rank_quad_unchecked(uint rank_index) {
    uint word_index = rank_index >> 2u;
    uint bit_offset = (rank_index & 3u) << 3u;
    uint word = query_rank_words[word_index] >> bit_offset;
    if (bit_offset != 0u) {
        word |= query_rank_words[word_index + 1u] << (32u - bit_offset);
    }
    return word;
}

uint target_rank_quad_unchecked(uint rank_index) {
    uint word_index = rank_index >> 2u;
    uint bit_offset = (rank_index & 3u) << 3u;
    uint word = target_rank_words[word_index] >> bit_offset;
    if (bit_offset != 0u) {
        word |= target_rank_words[word_index + 1u] << (32u - bit_offset);
    }
    return word;
}

uint query_rank_quad(uint rank_index) {
    if (rank_index > pc.query_rank_count
        || 4u > pc.query_rank_count - rank_index) {
        return 0u;
    }
    uint word_index = rank_index >> 2u;
    uint bit_offset = (rank_index & 3u) << 3u;
    uint word = query_rank_words[word_index] >> bit_offset;
    if (bit_offset != 0u) {
        word |= query_rank_words[word_index + 1u] << (32u - bit_offset);
    }
    return word;
}

uint target_rank_quad(uint rank_index) {
    if (rank_index > pc.target_rank_count
        || 4u > pc.target_rank_count - rank_index) {
        return 0u;
    }
    uint word_index = rank_index >> 2u;
    uint bit_offset = (rank_index & 3u) << 3u;
    uint word = target_rank_words[word_index] >> bit_offset;
    if (bit_offset != 0u) {
        word |= target_rank_words[word_index + 1u] << (32u - bit_offset);
    }
    return word;
}

void write_prefilter_result(
    uint job_index,
    uint lower_bound,
    uint status,
    bool accepted,
    uint diagnostic)
{
    results[job_index].lower_bound = lower_bound;
    results[job_index].status = status;
    results[job_index].accepted = accepted ? 1u : 0u;
    results[job_index].reserved = diagnostic;
}
)GLSL";

/// @brief Rank-buffer SneakySnake lower-bound filter. A uniform push-constant mode
/// keeps low-edit batches on independent scalar invocations and assigns high-
/// edit batches to fixed eight-lane logical teams. Equal-length inputs use
/// bounded MAX_SPAN-symbol submazes; unequal and unsupported shapes remain
/// conservative. Team-mode barriers are uniform and the implementation has no
/// subgroup-size assumption or atomics.
constexpr std::string_view k_sneaky_snake_rank_batch_body = R"GLSL(
const uint MAX_SUPPORTED_EDITS = 31u;
const uint TEAM_SIZE = 8u;
const uint TEAM_COUNT = LOCAL_SIZE_X / TEAM_SIZE;
shared PrefilterJob sh_job[TEAM_COUNT];
shared uint sh_position[TEAM_COUNT];
shared uint sh_submaze_end[TEAM_COUNT];
shared uint sh_escape_round[TEAM_COUNT];
shared uint sh_lower_bound[TEAM_COUNT];
shared uint sh_active[TEAM_COUNT];
shared uint sh_lane_best[LOCAL_SIZE_X];
shared uint sh_any_active;

uint matching_run(
    PrefilterJob job,
    uint position,
    uint submaze_end,
    uint query_cursor)
{
    uint run_limit = min(
        submaze_end - position,
        job.query_length - query_cursor);
    uint run = 0u;
    uint target_start = job.target_offset + position;
    uint query_start = job.query_offset + query_cursor;
    while (run < run_limit && ((target_start + run) & 3u) != 0u) {
        if (target_rank(target_start + run)
            != query_rank(query_start + run)) {
            return run;
        }
        ++run;
    }
    while (run_limit - run >= 4u) {
        uint query_word = query_rank_quad(query_start + run);
        uint target_word = target_rank_words[(target_start + run) >> 2u];
        uint different = query_word ^ target_word;
        if (different != 0u) {
            return run + (uint(findLSB(different)) >> 3u);
        }
        run += 4u;
    }
    while (run < run_limit) {
        if (target_rank(target_start + run)
            != query_rank(query_start + run)) {
            break;
        }
        ++run;
    }
    return run;
}

void write_result(
    uint job_index,
    uint lower_bound,
    uint status,
    bool accepted)
{
    write_prefilter_result(
        job_index, lower_bound, status, accepted, 0u);
}

void solve_job_scalar(uint job_index) {
    PrefilterJob job = jobs[job_index];
    if (!prefilter_job_is_in_bounds(job)
        || job.max_edits > MAX_SUPPORTED_EDITS || MAX_SPAN == 0) {
        write_result(job_index, 0u, STATUS_UNSUPPORTED, true);
        return;
    }

    if (job.query_length != job.target_length) {
        uint length_difference = job.query_length > job.target_length
            ? job.query_length - job.target_length
            : job.target_length - job.query_length;
        write_result(
            job_index,
            length_difference,
            STATUS_LENGTH_DIFFERENCE_ONLY,
            length_difference <= job.max_edits);
        return;
    }

    uint lower_bound = 0u;
    uint submaze_begin = 0u;
    while (submaze_begin < job.query_length) {
        uint submaze_end = submaze_begin + min(
            uint(MAX_SPAN), job.query_length - submaze_begin);
        uint position = submaze_begin;
        uint escape_round = 0u;

        while (position < submaze_end && escape_round < uint(MAX_SPAN)) {
            uint remaining_in_submaze = submaze_end - position;
            uint best_run = matching_run(
                job, position, submaze_end, position);
            uint max_shift = min(job.max_edits, job.query_length);
            for (uint shift = 1u;
                 shift <= max_shift && best_run < remaining_in_submaze;
                 ++shift) {
                if (shift <= position) {
                    best_run = max(
                        best_run,
                        matching_run(
                            job,
                            position,
                            submaze_end,
                            position - shift));
                }
                if (best_run == remaining_in_submaze) break;
                if (shift < job.query_length - position) {
                    best_run = max(
                        best_run,
                        matching_run(
                            job,
                            position,
                            submaze_end,
                            position + shift));
                }
            }

            if (best_run == remaining_in_submaze) {
                position = submaze_end;
                break;
            }

            ++lower_bound;
            if (lower_bound > job.max_edits) {
                write_result(job_index, lower_bound, STATUS_OK, false);
                return;
            }
            position += best_run + 1u;
            ++escape_round;
        }
        submaze_begin = submaze_end;
    }

    write_result(job_index, lower_bound, STATUS_OK, true);
}

void main() {
    if (pc.mode != TEAM_SIZE) {
        uint64_t stride = uint64_t(gl_NumWorkGroups.x)
            * uint64_t(gl_WorkGroupSize.x);
        if (stride == uint64_t(0)) return;
        for (uint64_t job_index = uint64_t(gl_GlobalInvocationID.x);
             job_index < uint64_t(pc.job_count);
             job_index += stride) {
            solve_job_scalar(uint(job_index));
        }
        return;
    }

    uint lane = gl_LocalInvocationID.x;
    uint team = lane / TEAM_SIZE;
    uint team_lane = lane & (TEAM_SIZE - 1u);
    uint64_t block_stride = uint64_t(gl_NumWorkGroups.x)
        * uint64_t(TEAM_COUNT);
    if (block_stride == uint64_t(0)) return;

    for (uint64_t block = uint64_t(gl_WorkGroupID.x)
             * uint64_t(TEAM_COUNT);
         block < uint64_t(pc.job_count);
         block += block_stride) {
        uint64_t job_wide = block + uint64_t(team);
        bool team_has_job = job_wide < uint64_t(pc.job_count);
        uint job_index = uint(job_wide);

        if (team_lane == 0u) {
            sh_active[team] = 0u;
            if (team_has_job) {
                PrefilterJob job = jobs[job_index];
                sh_job[team] = job;
                if (!prefilter_job_is_in_bounds(job)
                    || job.max_edits > MAX_SUPPORTED_EDITS
                    || MAX_SPAN == 0) {
                    write_result(job_index, 0u, STATUS_UNSUPPORTED, true);
                } else if (job.query_length != job.target_length) {
                    uint length_difference = job.query_length > job.target_length
                        ? job.query_length - job.target_length
                        : job.target_length - job.query_length;
                    write_result(
                        job_index,
                        length_difference,
                        STATUS_LENGTH_DIFFERENCE_ONLY,
                        length_difference <= job.max_edits);
                } else if (job.query_length == 0u) {
                    write_result(job_index, 0u, STATUS_OK, true);
                } else {
                    sh_position[team] = 0u;
                    sh_submaze_end[team] = min(
                        uint(MAX_SPAN), job.query_length);
                    sh_escape_round[team] = 0u;
                    sh_lower_bound[team] = 0u;
                    sh_active[team] = 1u;
                }
            }
        }
        barrier();

        while (true) {
            if (lane == 0u) {
                sh_any_active = 0u;
                for (uint slot = 0u; slot < TEAM_COUNT; ++slot) {
                    sh_any_active |= sh_active[slot];
                }
            }
            barrier();
            if (sh_any_active == 0u) break;

            uint local_best = 0u;
            if (sh_active[team] != 0u) {
                PrefilterJob job = sh_job[team];
                uint position = sh_position[team];
                uint submaze_end = sh_submaze_end[team];
                uint diagonal_count = 2u * job.max_edits + 1u;
                for (uint ordinal = team_lane;
                     ordinal < diagonal_count;
                     ordinal += TEAM_SIZE) {
                    uint query_cursor = position;
                    bool valid = true;
                    if (ordinal != 0u) {
                        uint shift = (ordinal + 1u) >> 1u;
                        bool before = (ordinal & 1u) != 0u;
                        if (before) {
                            if (shift > position) {
                                valid = false;
                            } else {
                                query_cursor = position - shift;
                            }
                        } else {
                            if (shift >= job.query_length - position) {
                                valid = false;
                            } else {
                                query_cursor = position + shift;
                            }
                        }
                    }
                    if (valid) {
                        local_best = max(
                            local_best,
                            matching_run(
                                job,
                                position,
                                submaze_end,
                                query_cursor));
                    }
                }
            }
            sh_lane_best[lane] = local_best;
            barrier();

            if (team_lane == 0u && sh_active[team] != 0u) {
                uint best_run = 0u;
                uint first_lane = team * TEAM_SIZE;
                for (uint offset = 0u; offset < TEAM_SIZE; ++offset) {
                    best_run = max(
                        best_run,
                        sh_lane_best[first_lane + offset]);
                }

                PrefilterJob job = sh_job[team];
                uint position = sh_position[team];
                uint submaze_end = sh_submaze_end[team];
                uint remaining_in_submaze = submaze_end - position;
                if (best_run == remaining_in_submaze) {
                    sh_position[team] = submaze_end;
                } else {
                    uint lower_bound = sh_lower_bound[team] + 1u;
                    sh_lower_bound[team] = lower_bound;
                    if (lower_bound > job.max_edits) {
                        write_result(
                            job_index, lower_bound, STATUS_OK, false);
                        sh_active[team] = 0u;
                    } else {
                        sh_position[team] = position + best_run + 1u;
                        sh_escape_round[team] += 1u;
                    }
                }

                if (sh_active[team] != 0u
                    && (sh_position[team] >= submaze_end
                        || sh_escape_round[team] >= uint(MAX_SPAN))) {
                    uint next_begin = submaze_end;
                    if (next_begin >= job.query_length) {
                        write_result(
                            job_index,
                            sh_lower_bound[team],
                            STATUS_OK,
                            true);
                        sh_active[team] = 0u;
                    } else {
                        sh_position[team] = next_begin;
                        sh_submaze_end[team] = next_begin + min(
                            uint(MAX_SPAN),
                            job.query_length - next_begin);
                        sh_escape_round[team] = 0u;
                    }
                }
            }
            barrier();
        }
    }
}
)GLSL";

/// @brief Clean-room GateKeeper threshold filter over caller-normalised rank bytes.
/// One invocation owns one job. Mismatch masks stream through four uint words;
/// a mask rejection is authoritative only after the exact fixed-band guard.
constexpr std::string_view k_gatekeeper_rank_batch_body = R"GLSL(
const uint MAX_GATEKEEPER_LENGTH = 128u;
const uint MAX_GATEKEEPER_EDITS = 8u;
const uint MASK_REJECTION_CANDIDATE_MASK = 0x00010000u;
const uint EXACT_GUARD_ACTIVATED_MASK = 0x00020000u;
const uint GUARD_CONFIRMED_REJECTION_MASK = 0x00040000u;
const uint HAMMING_EXACT_REJECTION_MASK = 0x00080000u;

uint packed_byte_mismatch_bits(uint difference) {
    // Fold every byte independently to its low bit, then compact byte-low bits
    // 0/8/16/24 into result bits 0/1/2/3. The largest cumulative right shift
    // is seven, so a set bit never crosses a byte boundary while folding.
    difference |= difference >> 4u;
    difference |= difference >> 2u;
    difference |= difference >> 1u;
    uint byte_low_bits = difference & 0x01010101u;
    return (byte_low_bits & 0x1u)
        | ((byte_low_bits >> 7u) & 0x2u)
        | ((byte_low_bits >> 14u) & 0x4u)
        | ((byte_low_bits >> 21u) & 0x8u);
}

void set_mask_bit(inout uvec4 mask, uint position) {
    mask[position >> 5u] |= 1u << (position & 31u);
}

void append_mismatch_quad(
    inout uvec4 mask,
    uint output_position,
    uint mismatch_bits)
{
    uint word_index = output_position >> 5u;
    uint word_shift = output_position & 31u;
    mask[word_index] |= mismatch_bits << word_shift;
    if (word_shift > 28u && word_index + 1u < 4u) {
        mask[word_index + 1u] |= mismatch_bits >> (32u - word_shift);
    }
}

uvec4 mismatch_mask_segment(
    PrefilterJob job,
    uint query_relative,
    uint target_relative,
    uint count,
    uint output_position)
{
    uvec4 mask = uvec4(0u);
    uint position = 0u;
    while (count - position >= 4u) {
        uint query_word = query_rank_quad_unchecked(
            job.query_offset + query_relative + position);
        uint target_word = target_rank_quad_unchecked(
            job.target_offset + target_relative + position);
        append_mismatch_quad(
            mask,
            output_position + position,
            packed_byte_mismatch_bits(query_word ^ target_word));
        position += 4u;
    }
    while (position < count) {
        if (query_rank_unchecked(
                job.query_offset + query_relative + position)
            != target_rank_unchecked(
                job.target_offset + target_relative + position)) {
            set_mask_bit(mask, output_position + position);
        }
        ++position;
    }
    return mask;
}

// Aligned production jobs load each rank word exactly once. Diagonal masks are
// then formed from register-resident packed bytes in four-word vectors. This
// replaces 5/9 complete SSBO rereads for E2/E4 with 16/32 total scalar loads.
// Unaligned direct-API jobs retain the generic bounds-aware path below.
struct PackedRanks32 {
    uvec4 first;
    uvec4 second;
};

struct PackedRanks64 {
    uvec4 first;
    uvec4 second;
    uvec4 third;
    uvec4 fourth;
};

// Raw (unamended, boundary-free) mismatch masks indexed by exact wavefront
// diagonal k=query_position-target_position. L32 uses only the low component.
struct ExactDiagonalMasks {
    uvec2 negative_two;
    uvec2 negative_one;
    uvec2 zero;
    uvec2 positive_one;
    uvec2 positive_two;
};

uvec4 compact_mismatch_nibbles(uvec4 difference) {
    difference |= difference >> 4u;
    difference |= difference >> 2u;
    difference |= difference >> 1u;
    uvec4 byte_low_bits = difference & uvec4(0x01010101u);
    return (byte_low_bits & uvec4(0x1u))
        | ((byte_low_bits >> 7u) & uvec4(0x2u))
        | ((byte_low_bits >> 14u) & uvec4(0x4u))
        | ((byte_low_bits >> 21u) & uvec4(0x8u));
}

uint pack_mismatch_nibbles(uvec4 nibbles) {
    return nibbles.x | (nibbles.y << 4u)
        | (nibbles.z << 8u) | (nibbles.w << 12u);
}

PackedRanks32 load_query_ranks_32(PrefilterJob job) {
    uint base = job.query_offset >> 2u;
    PackedRanks32 ranks;
    ranks.first = uvec4(
        query_rank_words[base], query_rank_words[base + 1u],
        query_rank_words[base + 2u], query_rank_words[base + 3u]);
    ranks.second = uvec4(
        query_rank_words[base + 4u], query_rank_words[base + 5u],
        query_rank_words[base + 6u], query_rank_words[base + 7u]);
    return ranks;
}

PackedRanks32 load_target_ranks_32(PrefilterJob job) {
    uint base = job.target_offset >> 2u;
    PackedRanks32 ranks;
    ranks.first = uvec4(
        target_rank_words[base], target_rank_words[base + 1u],
        target_rank_words[base + 2u], target_rank_words[base + 3u]);
    ranks.second = uvec4(
        target_rank_words[base + 4u], target_rank_words[base + 5u],
        target_rank_words[base + 6u], target_rank_words[base + 7u]);
    return ranks;
}

PackedRanks64 load_query_ranks_64(PrefilterJob job) {
    uint base = job.query_offset >> 2u;
    PackedRanks64 ranks;
    ranks.first = uvec4(
        query_rank_words[base], query_rank_words[base + 1u],
        query_rank_words[base + 2u], query_rank_words[base + 3u]);
    ranks.second = uvec4(
        query_rank_words[base + 4u], query_rank_words[base + 5u],
        query_rank_words[base + 6u], query_rank_words[base + 7u]);
    ranks.third = uvec4(
        query_rank_words[base + 8u], query_rank_words[base + 9u],
        query_rank_words[base + 10u], query_rank_words[base + 11u]);
    ranks.fourth = uvec4(
        query_rank_words[base + 12u], query_rank_words[base + 13u],
        query_rank_words[base + 14u], query_rank_words[base + 15u]);
    return ranks;
}

PackedRanks64 load_target_ranks_64(PrefilterJob job) {
    uint base = job.target_offset >> 2u;
    PackedRanks64 ranks;
    ranks.first = uvec4(
        target_rank_words[base], target_rank_words[base + 1u],
        target_rank_words[base + 2u], target_rank_words[base + 3u]);
    ranks.second = uvec4(
        target_rank_words[base + 4u], target_rank_words[base + 5u],
        target_rank_words[base + 6u], target_rank_words[base + 7u]);
    ranks.third = uvec4(
        target_rank_words[base + 8u], target_rank_words[base + 9u],
        target_rank_words[base + 10u], target_rank_words[base + 11u]);
    ranks.fourth = uvec4(
        target_rank_words[base + 12u], target_rank_words[base + 13u],
        target_rank_words[base + 14u], target_rank_words[base + 15u]);
    return ranks;
}

PackedRanks32 shift_rank_bytes_32(PackedRanks32 ranks, uint shift) {
    PackedRanks32 next_words;
    next_words.first = uvec4(
        ranks.first.y, ranks.first.z, ranks.first.w, ranks.second.x);
    next_words.second = uvec4(
        ranks.second.y, ranks.second.z, ranks.second.w, 0u);
    if (shift == 4u) return next_words;
    uint bits = shift << 3u;
    PackedRanks32 shifted;
    shifted.first = (ranks.first >> bits)
        | (next_words.first << (32u - bits));
    shifted.second = (ranks.second >> bits)
        | (next_words.second << (32u - bits));
    return shifted;
}

PackedRanks64 shift_rank_bytes_64(PackedRanks64 ranks, uint shift) {
    PackedRanks64 next_words;
    next_words.first = uvec4(
        ranks.first.y, ranks.first.z, ranks.first.w, ranks.second.x);
    next_words.second = uvec4(
        ranks.second.y, ranks.second.z, ranks.second.w, ranks.third.x);
    next_words.third = uvec4(
        ranks.third.y, ranks.third.z, ranks.third.w, ranks.fourth.x);
    next_words.fourth = uvec4(
        ranks.fourth.y, ranks.fourth.z, ranks.fourth.w, 0u);
    if (shift == 4u) return next_words;
    uint bits = shift << 3u;
    PackedRanks64 shifted;
    shifted.first = (ranks.first >> bits)
        | (next_words.first << (32u - bits));
    shifted.second = (ranks.second >> bits)
        | (next_words.second << (32u - bits));
    shifted.third = (ranks.third >> bits)
        | (next_words.third << (32u - bits));
    shifted.fourth = (ranks.fourth >> bits)
        | (next_words.fourth << (32u - bits));
    return shifted;
}

uint mismatch_mask_32(PackedRanks32 query, PackedRanks32 target) {
    uvec4 first = compact_mismatch_nibbles(query.first ^ target.first);
    uvec4 second = compact_mismatch_nibbles(query.second ^ target.second);
    return pack_mismatch_nibbles(first)
        | (pack_mismatch_nibbles(second) << 16u);
}

uvec2 mismatch_mask_64(PackedRanks64 query, PackedRanks64 target) {
    uvec4 first = compact_mismatch_nibbles(query.first ^ target.first);
    uvec4 second = compact_mismatch_nibbles(query.second ^ target.second);
    uvec4 third = compact_mismatch_nibbles(query.third ^ target.third);
    uvec4 fourth = compact_mismatch_nibbles(query.fourth ^ target.fourth);
    return uvec2(
        pack_mismatch_nibbles(first)
            | (pack_mismatch_nibbles(second) << 16u),
        pack_mismatch_nibbles(third)
            | (pack_mismatch_nibbles(fourth) << 16u));
}

uvec2 shift_left_64(uvec2 value, uint shift);

uint diagonal_mismatch_mask_32(
    PackedRanks32 query,
    PackedRanks32 target,
    uint shift,
    bool shift_target)
{
    uint mismatch = shift_target
        ? mismatch_mask_32(query, shift_rank_bytes_32(target, shift))
        : mismatch_mask_32(shift_rank_bytes_32(query, shift), target);
    mismatch &= 0xffffffffu >> shift;
    return shift_target ? mismatch << shift : mismatch;
}

uvec2 diagonal_mismatch_mask_64(
    PackedRanks64 query,
    PackedRanks64 target,
    uint shift,
    bool shift_target)
{
    uvec2 mismatch = shift_target
        ? mismatch_mask_64(query, shift_rank_bytes_64(target, shift))
        : mismatch_mask_64(shift_rank_bytes_64(query, shift), target);
    mismatch.y &= 0xffffffffu >> shift;
    return shift_target ? shift_left_64(mismatch, shift) : mismatch;
}

void set_mask_bit_64(inout uvec2 mask, uint position) {
    if (position < 32u) {
        mask.x |= 1u << position;
    } else {
        mask.y |= 1u << (position - 32u);
    }
}

uint amend_internal_short_zero_runs_32(uint mismatch) {
    uint left_one = mismatch << 1u;
    uint right_one = mismatch >> 1u;
    uint right_two = mismatch >> 2u;
    uint single = (~mismatch) & left_one & right_one;
    uint pair_start = (~mismatch) & (~right_one) & left_one & right_two;
    return mismatch | single | pair_start | (pair_start << 1u);
}

uvec2 shift_left_64(uvec2 value, uint shift) {
    return uvec2(
        value.x << shift,
        (value.y << shift) | (value.x >> (32u - shift)));
}

uvec2 shift_right_64(uvec2 value, uint shift) {
    return uvec2(
        (value.x >> shift) | (value.y << (32u - shift)),
        value.y >> shift);
}

uvec2 amend_internal_short_zero_runs_64(uvec2 mismatch) {
    uvec2 left_one = shift_left_64(mismatch, 1u);
    uvec2 right_one = shift_right_64(mismatch, 1u);
    uvec2 right_two = shift_right_64(mismatch, 2u);
    uvec2 single = (~mismatch) & left_one & right_one;
    uvec2 pair_start = (~mismatch) & (~right_one) & left_one & right_two;
    return mismatch | single | pair_start | shift_left_64(pair_start, 1u);
}

void intersect_shift_32(
    PackedRanks32 query,
    PackedRanks32 target,
    uint shift,
    out uint negative_raw,
    out uint positive_raw,
    inout uint final_mask)
{
    negative_raw = diagonal_mismatch_mask_32(
        query, target, shift, true);
    uint shifted_mask = amend_internal_short_zero_runs_32(negative_raw);
    shifted_mask |= 1u << (shift - 1u);
    final_mask &= shifted_mask;

    positive_raw = diagonal_mismatch_mask_32(
        query, target, shift, false);
    shifted_mask = amend_internal_short_zero_runs_32(positive_raw);
    shifted_mask |= 1u << (32u - shift);
    final_mask &= shifted_mask;
}

void intersect_shift_64(
    PackedRanks64 query,
    PackedRanks64 target,
    uint shift,
    out uvec2 negative_raw,
    out uvec2 positive_raw,
    inout uvec2 final_mask)
{
    negative_raw = diagonal_mismatch_mask_64(
        query, target, shift, true);
    uvec2 shifted_mask = amend_internal_short_zero_runs_64(negative_raw);
    set_mask_bit_64(shifted_mask, shift - 1u);
    final_mask &= shifted_mask;

    positive_raw = diagonal_mismatch_mask_64(
        query, target, shift, false);
    shifted_mask = amend_internal_short_zero_runs_64(positive_raw);
    set_mask_bit_64(shifted_mask, 64u - shift);
    final_mask &= shifted_mask;
}

uvec4 valid_mask(uint length) {
    uvec4 mask = uvec4(0u);
    uint full_words = length >> 5u;
    uint tail_bits = length & 31u;
    for (uint word_index = 0u; word_index < 4u; ++word_index) {
        if (word_index < full_words) {
            mask[word_index] = 0xffffffffu;
        } else if (word_index == full_words && tail_bits != 0u) {
            mask[word_index] = (1u << tail_bits) - 1u;
        }
    }
    return mask;
}

uvec4 mask_shift_left(uvec4 value, uint shift) {
    uvec4 shifted = uvec4(0u);
    for (uint word_index = 0u; word_index < 4u; ++word_index) {
        shifted[word_index] = value[word_index] << shift;
        if (word_index != 0u) {
            shifted[word_index] |= value[word_index - 1u] >> (32u - shift);
        }
    }
    return shifted;
}

uvec4 mask_shift_right(uvec4 value, uint shift) {
    uvec4 shifted = uvec4(0u);
    for (uint word_index = 0u; word_index < 4u; ++word_index) {
        shifted[word_index] = value[word_index] >> shift;
        if (word_index + 1u < 4u) {
            shifted[word_index] |= value[word_index + 1u] << (32u - shift);
        }
    }
    return shifted;
}

uvec4 amend_internal_short_zero_runs(uvec4 mismatch, uint length) {
    uvec4 left_one = mask_shift_left(mismatch, 1u);
    uvec4 right_one = mask_shift_right(mismatch, 1u);
    uvec4 right_two = mask_shift_right(mismatch, 2u);
    uvec4 single = (~mismatch) & left_one & right_one;
    uvec4 pair_start = (~mismatch) & (~right_one) & left_one & right_two;
    return (mismatch | single | pair_start
            | mask_shift_left(pair_start, 1u))
        & valid_mask(length);
}

uint mask_popcount(uvec4 mask) {
    return uint(bitCount(mask.x) + bitCount(mask.y)
        + bitCount(mask.z) + bitCount(mask.w));
}

uint nibble_cost(uint nibble) {
    if (nibble == 0u) return 0u;
    if (nibble == 0x5u || nibble == 0x6u || nibble == 0x9u
        || nibble == 0xau || nibble == 0xbu || nibble == 0xdu) {
        return 2u;
    }
    return 1u;
}

uint capped_word_estimate(
    uint word,
    uint estimate,
    uint max_edits)
{
    for (uint nibble_index = 0u; nibble_index < 8u; ++nibble_index) {
        estimate += nibble_cost(word & 0xfu);
        if (estimate > max_edits) return max_edits + 1u;
        word >>= 4u;
    }
    return estimate;
}

uint capped_mask_estimate_32(uint mask, uint max_edits) {
    return capped_word_estimate(mask, 0u, max_edits);
}

uint capped_mask_estimate_64(uvec2 mask, uint max_edits) {
    uint estimate = capped_word_estimate(mask.x, 0u, max_edits);
    if (estimate > max_edits) return max_edits + 1u;
    return capped_word_estimate(mask.y, estimate, max_edits);
}

uint gatekeeper_mask_estimate_32(
    PrefilterJob job,
    out uint hamming_distance,
    out ExactDiagonalMasks exact_masks)
{
#ifdef GATEKEEPER_SPECIALIZED_EDITS
    const uint active_max_edits = uint(GATEKEEPER_SPECIALIZED_EDITS);
#else
    uint active_max_edits = job.max_edits;
#endif
    PackedRanks32 query = load_query_ranks_32(job);
    PackedRanks32 target = load_target_ranks_32(job);
    uint hamming = mismatch_mask_32(query, target);
    exact_masks.zero = uvec2(hamming, 0u);
    hamming_distance = uint(bitCount(hamming));
    if (hamming_distance <= active_max_edits) return hamming_distance;

    uint final_mask = amend_internal_short_zero_runs_32(hamming);
    uint negative_raw = 0u;
    uint positive_raw = 0u;
    intersect_shift_32(
        query, target, 1u, negative_raw, positive_raw, final_mask);
    exact_masks.negative_one = uvec2(negative_raw, 0u);
    exact_masks.positive_one = uvec2(positive_raw, 0u);
    intersect_shift_32(
        query, target, 2u, negative_raw, positive_raw, final_mask);
    exact_masks.negative_two = uvec2(negative_raw, 0u);
    exact_masks.positive_two = uvec2(positive_raw, 0u);
#ifdef GATEKEEPER_SPECIALIZED_EDITS
#if GATEKEEPER_SPECIALIZED_EDITS == 4
    intersect_shift_32(
        query, target, 3u, negative_raw, positive_raw, final_mask);
    intersect_shift_32(
        query, target, 4u, negative_raw, positive_raw, final_mask);
#endif
#else
    if (active_max_edits == 4u) {
        intersect_shift_32(
            query, target, 3u, negative_raw, positive_raw, final_mask);
        intersect_shift_32(
            query, target, 4u, negative_raw, positive_raw, final_mask);
    }
#endif
    return capped_mask_estimate_32(final_mask, active_max_edits);
}

uint gatekeeper_mask_estimate_64(
    PrefilterJob job,
    out uint hamming_distance,
    out ExactDiagonalMasks exact_masks)
{
#ifdef GATEKEEPER_SPECIALIZED_EDITS
    const uint active_max_edits = uint(GATEKEEPER_SPECIALIZED_EDITS);
#else
    uint active_max_edits = job.max_edits;
#endif
    PackedRanks64 query = load_query_ranks_64(job);
    PackedRanks64 target = load_target_ranks_64(job);
    uvec2 hamming = mismatch_mask_64(query, target);
    exact_masks.zero = hamming;
    hamming_distance = uint(bitCount(hamming.x) + bitCount(hamming.y));
    if (hamming_distance <= active_max_edits) return hamming_distance;

    uvec2 final_mask = amend_internal_short_zero_runs_64(hamming);
    uvec2 negative_raw = uvec2(0u);
    uvec2 positive_raw = uvec2(0u);
    intersect_shift_64(
        query, target, 1u, negative_raw, positive_raw, final_mask);
    exact_masks.negative_one = negative_raw;
    exact_masks.positive_one = positive_raw;
    intersect_shift_64(
        query, target, 2u, negative_raw, positive_raw, final_mask);
    exact_masks.negative_two = negative_raw;
    exact_masks.positive_two = positive_raw;
#ifdef GATEKEEPER_SPECIALIZED_EDITS
#if GATEKEEPER_SPECIALIZED_EDITS == 4
    intersect_shift_64(
        query, target, 3u, negative_raw, positive_raw, final_mask);
    intersect_shift_64(
        query, target, 4u, negative_raw, positive_raw, final_mask);
#endif
#else
    if (active_max_edits == 4u) {
        intersect_shift_64(
            query, target, 3u, negative_raw, positive_raw, final_mask);
        intersect_shift_64(
            query, target, 4u, negative_raw, positive_raw, final_mask);
    }
#endif
    return capped_mask_estimate_64(final_mask, active_max_edits);
}

uint capped_mask_estimate(uvec4 mask, uint length, uint max_edits) {
    uint estimate = 0u;
    uint word_count = (length + 31u) >> 5u;
    for (uint word_index = 0u; word_index < word_count; ++word_index) {
        uint word = mask[word_index];
        for (uint nibble_index = 0u; nibble_index < 8u; ++nibble_index) {
            estimate += nibble_cost((word >> (4u * nibble_index)) & 0xfu);
            if (estimate > max_edits) return max_edits + 1u;
        }
    }
    return estimate;
}

#ifndef GATEKEEPER_SPECIALIZED_LENGTH
uint saturated_increment(uint value, uint cap) {
    return value >= cap ? cap : value + 1u;
}
#endif

uvec2 exact_mask_for_diagonal(
    ExactDiagonalMasks masks,
    int diagonal)
{
    if (diagonal <= -2) return masks.negative_two;
    if (diagonal == -1) return masks.negative_one;
    if (diagonal == 0) return masks.zero;
    if (diagonal == 1) return masks.positive_one;
    return masks.positive_two;
}

uint mismatch_distance_from_target_position(
    uvec2 mismatch,
    uint target_position,
    uint remaining)
{
    if (remaining == 0u || target_position >= 64u) return 0u;
    if (target_position < 32u) {
        uint low_suffix = mismatch.x >> target_position;
        if (low_suffix != 0u) {
            return min(remaining, uint(findLSB(low_suffix)));
        }
        uint high_distance = 32u - target_position;
        if (mismatch.y != 0u) {
            high_distance += uint(findLSB(mismatch.y));
        } else {
            high_distance = remaining;
        }
        return min(remaining, high_distance);
    }
    uint high_suffix = mismatch.y >> (target_position - 32u);
    return high_suffix == 0u
        ? remaining
        : min(remaining, uint(findLSB(high_suffix)));
}

uint exact_matching_run_from_masks(
    PrefilterJob job,
    ExactDiagonalMasks masks,
    uint query_position,
    uint target_position)
{
    uint remaining = min(
        job.query_length - query_position,
        job.target_length - target_position);
    int diagonal = int(query_position) - int(target_position);
    return mismatch_distance_from_target_position(
        exact_mask_for_diagonal(masks, diagonal),
        target_position,
        remaining);
}

int bounded_wavefront_position(
    PrefilterJob job,
    int diagonal,
    int query_position)
{
    if (query_position < 0 || query_position > int(job.query_length)) {
        return -1;
    }
    int target_position = query_position - diagonal;
    if (target_position < 0 || target_position > int(job.target_length)) {
        return -1;
    }
    return query_position;
}

int extend_wavefront_position(
    PrefilterJob job,
    ExactDiagonalMasks masks,
    int diagonal,
    int query_position)
{
    query_position = bounded_wavefront_position(
        job, diagonal, query_position);
    if (query_position < 0) return -1;
    uint target_position = uint(query_position - diagonal);
    return query_position + int(exact_matching_run_from_masks(
        job, masks, uint(query_position), target_position));
}

// Advance one exact Levenshtein wavefront cell. Diagonal k is
// query_position-target_position. The three predecessors are substitution on
// k, query deletion on k-1, and target insertion on k+1. For one score layer,
// the farthest valid query position dominates every shorter path on that same
// diagonal before a packed exact-match extension.
int next_wavefront_position(
    PrefilterJob job,
    ExactDiagonalMasks masks,
    int diagonal,
    int substitution,
    int deletion,
    int insertion)
{
    int best = -1;
    if (substitution >= 0) {
        best = max(best, bounded_wavefront_position(
            job, diagonal, substitution + 1));
    }
    if (deletion >= 0) {
        best = max(best, bounded_wavefront_position(
            job, diagonal, deletion + 1));
    }
    if (insertion >= 0) {
        best = max(best, bounded_wavefront_position(
            job, diagonal, insertion));
    }
    return extend_wavefront_position(job, masks, diagonal, best);
}

// The production E2/E4 guard is an exact scalar-specialised wavefront. It has
// no private arrays or dynamic diagonal indexing. Diagonals that cannot return
// to the equal-length final diagonal with the remaining score are pruned, so
// E2 evaluates four non-initial cells and E4 evaluates twelve. The generic API
// retains the fixed-band DP
// below for E1/E3/E5..E8.
bool exact_edit_distance_e2_or_e4_is_above_threshold(
    PrefilterJob job,
    ExactDiagonalMasks masks)
{
    const int invalid = -1;
    const int length = int(job.query_length);

    int score0_0 = extend_wavefront_position(job, masks, 0, 0);
    if (score0_0 >= length) return false;

    int score1_m1 = next_wavefront_position(
        job, masks, -1, invalid, invalid, score0_0);
    int score1_0 = next_wavefront_position(
        job, masks, 0, score0_0, invalid, invalid);
    int score1_p1 = next_wavefront_position(
        job, masks, 1, invalid, score0_0, invalid);
    if (score1_0 >= length) return false;

    int score2_0 = next_wavefront_position(
        job, masks, 0, score1_0, score1_m1, score1_p1);
#if defined(GATEKEEPER_SPECIALIZED_EDITS) \
    && GATEKEEPER_SPECIALIZED_EDITS == 2
    return score2_0 < length;
#else
    if (score2_0 >= length) return false;
#ifndef GATEKEEPER_SPECIALIZED_EDITS
    if (job.max_edits == 2u) {
        return score2_0 < length;
    }
#endif

    int score2_m2 = next_wavefront_position(
        job, masks, -2, invalid, invalid, score1_m1);
    int score2_m1 = next_wavefront_position(
        job, masks, -1, score1_m1, invalid, score1_0);
    int score2_p1 = next_wavefront_position(
        job, masks, 1, score1_p1, score1_0, invalid);
    int score2_p2 = next_wavefront_position(
        job, masks, 2, invalid, score1_p1, invalid);
    int score3_m1 = next_wavefront_position(
        job, masks, -1, score2_m1, score2_m2, score2_0);
    int score3_0 = next_wavefront_position(
        job, masks, 0, score2_0, score2_m1, score2_p1);
    int score3_p1 = next_wavefront_position(
        job, masks, 1, score2_p1, score2_0, score2_p2);
    if (score3_0 >= length) return false;

    int score4_0 = next_wavefront_position(
        job, masks, 0, score3_0, score3_m1, score3_p1);
    return score4_0 < length;
#endif
}

#ifndef GATEKEEPER_SPECIALIZED_LENGTH
bool exact_edit_distance_is_above_threshold(PrefilterJob job) {
    uint query_length = job.query_length;
    uint target_length = job.target_length;
    uint max_edits = job.max_edits;
    uint length_difference = query_length > target_length
        ? query_length - target_length
        : target_length - query_length;
    if (length_difference > max_edits) return true;
    if (max_edits >= max(query_length, target_length)) return false;

    uint cap = max_edits + 1u;
    uint center = max_edits + 1u;
    uint width = 2u * max_edits + 3u;
    uint previous[19];
    uint current[19];
    for (uint slot = 0u; slot < 19u; ++slot) previous[slot] = cap;
    for (uint column = 0u; column <= min(target_length, max_edits);
         ++column) {
        previous[center + column] = column;
    }

    for (uint row = 1u; row <= query_length; ++row) {
        for (uint slot = 0u; slot < 19u; ++slot) current[slot] = cap;
        uint column_begin = row > max_edits ? row - max_edits : 0u;
        uint column_end = min(target_length, row + max_edits);
        for (uint column = column_begin; column <= column_end; ++column) {
            int diagonal = int(column) - int(row);
            uint slot = uint(int(center) + diagonal);
            if (column == 0u) {
                current[slot] = min(row, cap);
            } else {
                uint deletion = saturated_increment(previous[slot + 1u], cap);
                uint insertion = saturated_increment(current[slot - 1u], cap);
                uint substitution = previous[slot];
                if (query_rank(job.query_offset + row - 1u)
                    != target_rank(job.target_offset + column - 1u)) {
                    substitution = saturated_increment(substitution, cap);
                }
                current[slot] = min(deletion, min(insertion, substitution));
            }
        }
        for (uint slot = 0u; slot < width; ++slot) {
            previous[slot] = current[slot];
        }
    }

    uint final_slot = uint(
        int(center) + int(target_length) - int(query_length));
    return previous[final_slot] > max_edits;
}
#endif

#ifndef GATEKEEPER_SPECIALIZED_LENGTH
void solve_gatekeeper_job(uint job_index) {
    PrefilterJob job = jobs[job_index];
    if (!prefilter_job_is_in_bounds(job)) {
        write_prefilter_result(
            job_index, 0u, STATUS_UNSUPPORTED, true, 0u);
        return;
    }
    if (job.query_length != job.target_length) {
        uint length_difference = job.query_length > job.target_length
            ? job.query_length - job.target_length
            : job.target_length - job.query_length;
        write_prefilter_result(
            job_index,
            length_difference,
            STATUS_LENGTH_DIFFERENCE_ONLY,
            length_difference <= job.max_edits,
            0u);
        return;
    }
    if (job.query_length == 0u) {
        write_prefilter_result(job_index, 0u, STATUS_OK, true, 0u);
        return;
    }
    if (pc.mode != 0u || uint(MAX_SPAN) != MAX_GATEKEEPER_LENGTH
        || job.query_length > MAX_GATEKEEPER_LENGTH
        || job.max_edits > MAX_GATEKEEPER_EDITS) {
        write_prefilter_result(
            job_index, 0u, STATUS_UNSUPPORTED, true, 0u);
        return;
    }

    uint length = job.query_length;
    uvec4 hamming = mismatch_mask_segment(job, 0u, 0u, length, 0u)
        & valid_mask(length);
    uint hamming_distance = mask_popcount(hamming);
    if (hamming_distance <= job.max_edits) {
        write_prefilter_result(
            job_index, 0u, STATUS_OK, true, hamming_distance);
        return;
    }
    if (job.max_edits == 0u) {
        write_prefilter_result(
            job_index,
            1u,
            STATUS_OK,
            false,
            1u | HAMMING_EXACT_REJECTION_MASK);
        return;
    }

    uvec4 final_mask = amend_internal_short_zero_runs(hamming, length);
    for (uint shift = 1u; shift <= job.max_edits; ++shift) {
        if (shift >= length) break;
        uint compared_length = length - shift;

        uvec4 shifted_mask = mismatch_mask_segment(
            job, 0u, shift, compared_length, shift);
        shifted_mask = amend_internal_short_zero_runs(
            shifted_mask, length);
        set_mask_bit(shifted_mask, shift - 1u);
        final_mask &= shifted_mask;

        shifted_mask = mismatch_mask_segment(
            job, shift, 0u, compared_length, 0u);
        shifted_mask = amend_internal_short_zero_runs(
            shifted_mask, length);
        set_mask_bit(shifted_mask, length - shift);
        final_mask &= shifted_mask;
    }
    final_mask &= valid_mask(length);
    uint estimate = capped_mask_estimate(
        final_mask, length, job.max_edits);
    if (estimate <= job.max_edits) {
        write_prefilter_result(
            job_index, 0u, STATUS_OK, true, estimate);
        return;
    }

    uint diagnostic = estimate | MASK_REJECTION_CANDIDATE_MASK
        | EXACT_GUARD_ACTIVATED_MASK;
    if (exact_edit_distance_is_above_threshold(job)) {
        write_prefilter_result(
            job_index,
            job.max_edits + 1u,
            STATUS_OK,
            false,
            diagnostic | GUARD_CONFIRMED_REJECTION_MASK);
        return;
    }
    write_prefilter_result(
        job_index, 0u, STATUS_OK, true, diagnostic);
}
#else
void solve_gatekeeper_job(uint job_index) {
    PrefilterJob job = jobs[job_index];
    if (!prefilter_job_is_in_bounds(job)) {
        write_prefilter_result(
            job_index, 0u, STATUS_UNSUPPORTED, true, 0u);
        return;
    }
    if (job.query_length != job.target_length) {
        uint length_difference = job.query_length > job.target_length
            ? job.query_length - job.target_length
            : job.target_length - job.query_length;
        write_prefilter_result(
            job_index,
            length_difference,
            STATUS_LENGTH_DIFFERENCE_ONLY,
            length_difference <= job.max_edits,
            0u);
        return;
    }
    if (job.query_length == 0u) {
        write_prefilter_result(job_index, 0u, STATUS_OK, true, 0u);
        return;
    }
    if (pc.mode != 0u || uint(MAX_SPAN) != MAX_GATEKEEPER_LENGTH
        || job.query_length != uint(GATEKEEPER_SPECIALIZED_LENGTH)
        || job.max_edits != uint(GATEKEEPER_SPECIALIZED_EDITS)
        || (job.query_offset & 3u) != 0u
        || (job.target_offset & 3u) != 0u) {
        write_prefilter_result(
            job_index, 0u, STATUS_UNSUPPORTED, true, 0u);
        return;
    }

    const uint active_max_edits = uint(GATEKEEPER_SPECIALIZED_EDITS);
    uint hamming_distance = 0u;
    ExactDiagonalMasks exact_masks;
#if GATEKEEPER_SPECIALIZED_LENGTH == 32
    uint estimate = gatekeeper_mask_estimate_32(
        job, hamming_distance, exact_masks);
#else
    uint estimate = gatekeeper_mask_estimate_64(
        job, hamming_distance, exact_masks);
#endif
    if (hamming_distance <= active_max_edits) {
        write_prefilter_result(
            job_index, 0u, STATUS_OK, true, hamming_distance);
        return;
    }
    if (estimate <= active_max_edits) {
        write_prefilter_result(
            job_index, 0u, STATUS_OK, true, estimate);
        return;
    }

    uint diagnostic = estimate | MASK_REJECTION_CANDIDATE_MASK
        | EXACT_GUARD_ACTIVATED_MASK;
    if (exact_edit_distance_e2_or_e4_is_above_threshold(
            job, exact_masks)) {
        write_prefilter_result(
            job_index,
            active_max_edits + 1u,
            STATUS_OK,
            false,
            diagnostic | GUARD_CONFIRMED_REJECTION_MASK);
        return;
    }
    write_prefilter_result(
        job_index, 0u, STATUS_OK, true, diagnostic);
}
#endif

void main() {
    uint64_t stride = uint64_t(gl_NumWorkGroups.x)
        * uint64_t(gl_WorkGroupSize.x);
    if (stride == uint64_t(0)) return;
    for (uint64_t job_index = uint64_t(gl_GlobalInvocationID.x);
         job_index < uint64_t(pc.job_count);
         job_index += stride) {
        solve_gatekeeper_job(uint(job_index));
    }
}
)GLSL";

/// @brief Score/end-point-only banded affine extension. This prototype runs one
/// scalar DP per job and dispatches the batch as grid-strided workgroups. The
/// ABI is intentionally symbol-rank based (`uint` per symbol) so parent
/// integration can feed any alphabet without dna4-specific decoding here.
constexpr std::string_view k_alignment_extension_banded_affine_body = R"GLSL(
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

constexpr std::string_view k_alignment_extension_simple_cigar_body = R"GLSL(
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

constexpr std::string_view k_generate_units_body = R"GLSL(
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
// GPU sequence-sort kernels (dna4-optimized and generic)
//
// Each kind's body is composed (in shader_body_for) as:
//   k_sequence_sort_push + kSeqSortCommon + (dna4?k_dna4_canonicalizer:k_generic_canonicalizer)
//                + one stage main
// so the dna4/generic canonicalizer is chosen BY shader_kind (no request
// threading). Storage width is runtime via pc.unit_bits + read_storage_unit64
// (the verbatim helper copied from the candidate body — it is function-local
// there, not a shared symbol). LOCAL_SIZE_X comes from the injected prelude.
// ===========================================================================

/// @brief Push-constant block (40 bytes == SeqSortPush in runtime.cppm). Identical for
/// all seq-sort kinds so one struct serves every stage.
constexpr std::string_view k_sequence_sort_push = R"GLSL(
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

/// @brief Descriptor bindings + packed-sequence reader + low_symbol_mask. Declared
/// BEFORE the canonicalizer so it can call low_symbol_mask, and before the
/// key/compare snippet which needs SeqDesc + read_unit. read_unit() forward-uses
/// unit_bits() (defined in the canon snippet that follows), which is fine in
/// GLSL because the canon snippet is concatenated between this and k_sequence_sort_key_compare.
constexpr std::string_view k_sequence_sort_readers = R"GLSL(
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

/// @brief Canonical key + full canonical compare. Uses canon()/lpu()/unit_bits() from
/// the canonicalizer snippet (concatenated immediately before this).
constexpr std::string_view k_sequence_sort_key_compare = R"GLSL(
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

/// @brief dna4 canonicalizer: compile-time bits_per_char==2, no masking (used_mask==~0).
constexpr std::string_view k_dna4_canonicalizer = R"GLSL(
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

/// @brief generic canonicalizer: alphabet params from push constants, masks used_mask.
constexpr std::string_view k_generic_canonicalizer = R"GLSL(
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

/// @brief Kernel A main: one invocation per row -> (key:uint64, value:uint64=row index).
constexpr std::string_view k_sequence_sort_key_main = R"GLSL(
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

/// @brief Kernel B main: one workgroup per equal-key run; bitonic over canon_cmp.
constexpr std::string_view k_sequence_sort_tiebreak_main = R"GLSL(
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

/// @brief Kernel C main: one global bitonic compare-swap pass over the padded index
/// buffer. Used only when a 64-bit key collision run is too large for the
/// per-workgroup tiebreak kernel.
constexpr std::string_view k_sequence_sort_global_compare_main = R"GLSL(
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

/// @brief Kernel D main: dense-rank scatter. This is a verified fast path for batches
/// whose selected 64-bit canonical key is exactly a permutation of [0,count).
/// For non-zero key offsets the shader also proves, on device, that all earlier
/// units match row 0; otherwise sorting by a later key would not be lexicographic.
constexpr std::string_view k_sequence_sort_dense_scatter_main = R"GLSL(
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

/// @brief Kernel E main: parallel common-prefix validation for dense tail keys. One
/// invocation checks one (row, unit-before-key) pair against row 0 and ORs the
/// status word on any mismatch. Dense scatter then runs after a command-buffer
/// barrier with pc.sort_size != 0 to avoid rechecking the prefix serially.
constexpr std::string_view k_sequence_sort_prefix_check_main = R"GLSL(
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

enum class sequence_sort_stage : std::uint8_t {
    key,
    tiebreak,
    global_compare,
    dense_scatter,
    prefix_check,
};

/// @brief Compose one rank-prefilter shader from the single bounds-aware wire/load
/// preamble and its algorithm-specific body.
[[nodiscard]] std::string rank_prefilter_body(std::string_view algorithm_body)
{
    std::string body;
    body.reserve(k_rank_prefilter_preamble.size() + algorithm_body.size());
    body += k_rank_prefilter_preamble;
    body += algorithm_body;
    return body;
}

[[nodiscard]] std::string specialised_gatekeeper_body(
    std::uint32_t length,
    std::uint32_t max_edits)
{
    std::string body;
    body.reserve(
        k_rank_prefilter_preamble.size() + k_gatekeeper_rank_batch_body.size() + 96u);
    body += k_rank_prefilter_preamble;
    body += fmt::format(
        "#define GATEKEEPER_SPECIALIZED_LENGTH {}\n"
        "#define GATEKEEPER_SPECIALIZED_EDITS {}\n",
        length,
        max_edits);
    body += k_gatekeeper_rank_batch_body;
    return body;
}

/// @brief Compose a seq-sort kind's full body from the shared snippets. Order matters:
/// push -> readers/low_symbol_mask -> canon (defines lpu/unit_bits/canon) ->
/// key/compare (uses canon) -> stage main.
[[nodiscard]] std::string seq_sort_body(bool dna4, sequence_sort_stage stage) {
    std::string s;
    s.reserve(k_sequence_sort_push.size() + k_sequence_sort_readers.size() + k_sequence_sort_key_compare.size() + 1536);
    s += k_sequence_sort_push;
    s += k_sequence_sort_readers;
    s += (dna4 ? k_dna4_canonicalizer : k_generic_canonicalizer);
    s += k_sequence_sort_key_compare;
    switch (stage) {
        case sequence_sort_stage::key:            s += k_sequence_sort_key_main; break;
        case sequence_sort_stage::tiebreak:       s += k_sequence_sort_tiebreak_main; break;
        case sequence_sort_stage::global_compare: s += k_sequence_sort_global_compare_main; break;
        case sequence_sort_stage::dense_scatter:  s += k_sequence_sort_dense_scatter_main; break;
        case sequence_sort_stage::prefix_check:   s += k_sequence_sort_prefix_check_main; break;
    }
    return s;
}

/// @brief Resolve a shader kind to its embedded GLSL body (everything after the
/// `#version`/`#extension`/`#define` prelude the compiler injects).
[[nodiscard]] std::string shader_body_for(shader_kind kind) {
    switch (kind) {
        case shader_kind::chain_tiled_dp_u64:
            return std::string{k_chain_tiled_dp_body};
        case shader_kind::seed_lookup_anchors:
            return std::string{k_seed_lookup_anchors_body};
        case shader_kind::sort_anchors_bitonic:
            return std::string{k_sort_anchors_bitonic_body};
        // The ordered-sketch family shares one tile-replay body (the
        // packed-sequence reader helpers are inlined inside it).
        case shader_kind::ordered_sketch_tile:
        case shader_kind::sketch_emit_flags:
        case shader_kind::sketch_emit_carry:
        case shader_kind::sketch_emit_words:
            return std::string{k_ordered_sketch_body};
        case shader_kind::prefix_scan_block:
            return std::string{k_prefix_scan_block_body};
        case shader_kind::prefix_scan_add:
            return std::string{k_prefix_scan_add_body};
        case shader_kind::seed_count:
        case shader_kind::seed_emit:
            return std::string{k_seed_count_emit_body};
        case shader_kind::grouped_soa_seed_count:
        case shader_kind::grouped_soa_seed_emit:
            return std::string{k_grouped_soa_seed_body};
        case shader_kind::radix_sort_histogram:
            return std::string{k_radix_sort_histogram_body};
        case shader_kind::radix_sort_scan:
            return std::string{k_radix_sort_scan_body};
        case shader_kind::radix_sort_scatter:
            return std::string{k_radix_sort_scatter_body};
        case shader_kind::target_index_mark_runs:
            return std::string{k_index_mark_runs_body};
        case shader_kind::target_index_emit_buckets:
            return std::string{k_index_emit_buckets_body};
        case shader_kind::target_index_claim_slots:
            return std::string{k_index_claim_slots_body};
        case shader_kind::target_index_emit_slots:
            return std::string{k_index_emit_slots_body};
        case shader_kind::chain_range_build:
            return std::string{k_chain_range_build_body};
        case shader_kind::coordinate_reduce:
            return std::string{k_coordinate_reduce_body};
        case shader_kind::coordinate_vote_reduce:
            return std::string{k_coordinate_vote_reduce_body};
        case shader_kind::coordinate_vote_reduce_batch:
            return std::string{k_coordinate_vote_reduce_batch_body};
        case shader_kind::generate_units:
            return std::string{k_generate_units_body};
        case shader_kind::seq_sort_extract_key_dna4:
            return seq_sort_body(/*dna4=*/true,  sequence_sort_stage::key);
        case shader_kind::seq_sort_extract_key_generic:
            return seq_sort_body(/*dna4=*/false, sequence_sort_stage::key);
        case shader_kind::seq_sort_tiebreak_dna4:
            return seq_sort_body(/*dna4=*/true,  sequence_sort_stage::tiebreak);
        case shader_kind::seq_sort_tiebreak_generic:
            return seq_sort_body(/*dna4=*/false, sequence_sort_stage::tiebreak);
        case shader_kind::seq_sort_global_compare_dna4:
            return seq_sort_body(/*dna4=*/true,  sequence_sort_stage::global_compare);
        case shader_kind::seq_sort_global_compare_generic:
            return seq_sort_body(/*dna4=*/false, sequence_sort_stage::global_compare);
        case shader_kind::seq_sort_dense_scatter_dna4:
            return seq_sort_body(/*dna4=*/true,  sequence_sort_stage::dense_scatter);
        case shader_kind::seq_sort_dense_scatter_generic:
            return seq_sort_body(/*dna4=*/false, sequence_sort_stage::dense_scatter);
        case shader_kind::seq_sort_prefix_check_dna4:
            return seq_sort_body(/*dna4=*/true,  sequence_sort_stage::prefix_check);
        case shader_kind::seq_sort_prefix_check_generic:
            return seq_sort_body(/*dna4=*/false, sequence_sort_stage::prefix_check);
        case shader_kind::align_extend_banded_affine:
            return std::string{k_alignment_extension_banded_affine_body};
        case shader_kind::align_extend_simple_cigar:
            return std::string{k_alignment_extension_simple_cigar_body};
        case shader_kind::sneaky_snake_rank_batch:
            return rank_prefilter_body(k_sneaky_snake_rank_batch_body);
        case shader_kind::gatekeeper_rank_batch:
            return rank_prefilter_body(k_gatekeeper_rank_batch_body);
        case shader_kind::gatekeeper_rank_batch_l32_e2:
            return specialised_gatekeeper_body(32u, 2u);
        case shader_kind::gatekeeper_rank_batch_l32_e4:
            return specialised_gatekeeper_body(32u, 4u);
        case shader_kind::gatekeeper_rank_batch_l64_e2:
            return specialised_gatekeeper_body(64u, 2u);
        case shader_kind::gatekeeper_rank_batch_l64_e4:
            return specialised_gatekeeper_body(64u, 4u);
    }
    throw shader_compile_error("compile_shader_spirv: unknown shader_kind");
}

/// @brief glslang's process-wide initialiser is reference-counted but not safe to
/// race; call it exactly once for the process lifetime. We intentionally do not
/// `FinalizeProcess()` — the cost is a small one-time leak at exit, and it
/// avoids a teardown race if any thread is mid-compile.
void ensure_glslang_initialized() {
    static std::once_flag init_flag;
    std::call_once(init_flag, []() { glslang::InitializeProcess(); });
}

/// @brief Default per-stage resource limits for a Vulkan compute shader. glslang's
/// `Vulkan::glslang` aggregate target does NOT ship `GetDefaultResources()`
/// (that lives in the separate default-resource-limits library), so this module
/// carries a conservative limit table. The values mirror glslang's
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

std::vector<std::uint32_t> compile_shader_spirv(const shader_compile_request& request) {
    // Reject geometry the shared-array layout cannot represent BEFORE compiling.
    // 1024 is the largest tile the chaining pipeline selector requests
    // (choose_vulkan_chaining_pipeline), and a zero local size has no invocation
    // to run the tile loop. The device's maxComputeWorkGroupInvocations bound on
    // local_size is enforced by the runtime, which is the only place that knows
    // the limit.
    if (request.tile_size == 0 || request.tile_size > 1024) {
        throw shader_compile_error(fmt::format(
            "compile_shader_spirv: tile_size {} out of range (1..1024)",
            request.tile_size));
    }
    if (request.local_size == 0) {
        throw shader_compile_error("compile_shader_spirv: local_size must be non-zero");
    }
    if ((request.kind == shader_kind::align_extend_banded_affine
         || request.kind == shader_kind::align_extend_simple_cigar)
        && (request.max_span == 0 || request.max_span > 512)) {
        throw shader_compile_error(fmt::format(
            "compile_shader_spirv: alignment max_span {} out of range (1..512)",
            request.max_span));
    }
    if (request.kind == shader_kind::sneaky_snake_rank_batch
        && request.max_span != 512u) {
        throw shader_compile_error(fmt::format(
            "compile_shader_spirv: SneakySnake submaze span {} must be 512",
            request.max_span));
    }
    if (is_gatekeeper_rank_batch_kind(request.kind)
        && (request.tile_size != 384u || request.local_size != 64u
            || request.max_span != 128u)) {
        throw shader_compile_error(fmt::format(
            "compile_shader_spirv: GateKeeper key ({}, {}, {}) must be "
            "(384, 64, 128)",
            request.tile_size, request.local_size, request.max_span));
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
        bioseq_log<logging_level::error>("[shader_compiler] {}", log);
        throw shader_compile_error(log);
    }

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
        const std::string log = fmt::format(
            "glslang link failed: {} {}", program.getInfoLog(),
            program.getInfoDebugLog());
        bioseq_log<logging_level::error>("[shader_compiler] {}", log);
        throw shader_compile_error(log);
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
        bioseq_log<logging_level::error>("[shader_compiler] {}", log);
        throw shader_compile_error(log);
    }
    bioseq_log<logging_level::debug>(
        "[shader_compiler] compiled shader kind={} tile={} local={} max_span={} -> {} words",
        static_cast<int>(request.kind), request.tile_size, request.local_size,
        request.max_span, spirv.size());
    return spirv;
}

std::string shader_body_for_digest(shader_kind kind) {
    return shader_body_for(kind);
}

}  // namespace bioseq::gpu::vulkan
