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
    generate_units,
    // DOC0346 — GPU sequence sort. Two stages × two alphabets. The cache keys on
    // ShaderKind, so the dna4 and generic variants are distinct SPIR-V modules
    // automatically; storage width is a RUNTIME push field (pc.unit_bits).
    seq_sort_extract_key_dna4,
    seq_sort_extract_key_generic,
    seq_sort_tiebreak_dna4,
    seq_sort_tiebreak_generic,
    align_extend_banded_affine,
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
};

/// Number of std430 storage-buffer bindings in each descriptor layout.
[[nodiscard]] constexpr std::uint32_t descriptor_binding_count(
    DescriptorLayoutKind kind) noexcept
{
    switch (kind) {
        case DescriptorLayoutKind::chain_tiled_dp:        return 4;
        case DescriptorLayoutKind::seed_lookup:           return 5;
        case DescriptorLayoutKind::anchor_sort:           return 2;
        // DOC0310 Phase C layouts.
        case DescriptorLayoutKind::phase_c_sketch:        return 9;
        case DescriptorLayoutKind::phase_c_scan:          return 3;
        case DescriptorLayoutKind::phase_c_seed:          return 8;
        case DescriptorLayoutKind::phase_c_sort:          return 5;
        case DescriptorLayoutKind::phase_c_index:         return 7;
        case DescriptorLayoutKind::phase_c_chain_range:   return 6;
        case DescriptorLayoutKind::phase_c_coordinate_reduce: return 6;
        case DescriptorLayoutKind::generate_units:         return 2;
        case DescriptorLayoutKind::seq_sort:               return 4;
        case DescriptorLayoutKind::align_extension:        return 4;
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
        case ShaderKind::phase_c_sort_radix_histogram:
        case ShaderKind::phase_c_sort_radix_scan:
        case ShaderKind::phase_c_sort_radix_scatter:  return DescriptorLayoutKind::phase_c_sort;
        case ShaderKind::phase_c_index_mark_runs:
        case ShaderKind::phase_c_index_emit_buckets:
        case ShaderKind::phase_c_index_claim_slots:
        case ShaderKind::phase_c_index_emit_slots:    return DescriptorLayoutKind::phase_c_index;
        case ShaderKind::phase_c_chain_range_build:   return DescriptorLayoutKind::phase_c_chain_range;
        case ShaderKind::phase_c_coordinate_reduce:   return DescriptorLayoutKind::phase_c_coordinate_reduce;
        case ShaderKind::generate_units:              return DescriptorLayoutKind::generate_units;
        case ShaderKind::seq_sort_extract_key_dna4:
        case ShaderKind::seq_sort_extract_key_generic:
        case ShaderKind::seq_sort_tiebreak_dna4:
        case ShaderKind::seq_sort_tiebreak_generic:   return DescriptorLayoutKind::seq_sort;
        case ShaderKind::align_extend_banded_affine:  return DescriptorLayoutKind::align_extension;
    }
    return DescriptorLayoutKind::chain_tiled_dp;
}

/// Push-constant block size (bytes) each shader's `Params` uniform declares.
/// The anchor sort takes its range from a buffer and has no push constant.
[[nodiscard]] constexpr std::uint32_t push_constant_size(ShaderKind kind) noexcept {
    switch (kind) {
        case ShaderKind::chain_tiled_dp_u64:    return 36;  // ChainingPushConstants
        case ShaderKind::seed_lookup_anchors:   return 36;  //  9 × uint32
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
        case ShaderKind::phase_c_sort_radix_histogram:
        case ShaderKind::phase_c_sort_radix_scan:
        case ShaderKind::phase_c_sort_radix_scatter:  return 20;  // SortPushC (5 × u32)
        case ShaderKind::phase_c_index_mark_runs:
        case ShaderKind::phase_c_index_emit_buckets:
        case ShaderKind::phase_c_index_claim_slots:
        case ShaderKind::phase_c_index_emit_slots:    return 24;  // IndexPushC (6 × u32)
        case ShaderKind::phase_c_chain_range_build:   return 8;   // ChainRangePushC (2 × u32)
        case ShaderKind::phase_c_coordinate_reduce:   return 28;  // GpuCoordinateReducePush
        case ShaderKind::generate_units:              return 48;  // GenerateUnitsPush (12 × u32)
        case ShaderKind::seq_sort_extract_key_dna4:
        case ShaderKind::seq_sort_extract_key_generic:
        case ShaderKind::seq_sort_tiebreak_dna4:
        case ShaderKind::seq_sort_tiebreak_generic:   return 32;  // SeqSortPush (8 × u32)
        case ShaderKind::align_extend_banded_affine:  return 44;  // AlignmentExtensionPush
    }
    return 0;
}

/// One shader-compilation request. `tile_size`, `local_size`, and `max_span`
/// are substituted as compile-time `#define`s before glslang runs; only those
/// three affect the emitted SPIR-V (shared-array lengths). The remaining fields
/// describe the runtime push-constant routing and are carried for the pipeline
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

layout(push_constant) uniform Params {
    uint minimizer_count;
    uint words32_per_minimizer;
    uint word_kind;
    uint hash_shift;
    uint pos_bits;
    uint query_len;
    uint kmer_size;
    uint slot_mask;
    uint exact_anchor_count;
} pc;

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
    }
}
)GLSL";

/// DOC0307 §7.2 — per-query bitonic anchor sort. One workgroup sorts up to
/// LOCAL_SIZE_X anchors by `ref_bits()` in shared memory. Larger anchor sets
/// (range length > LOCAL_SIZE_X) are left untouched and the runtime CPU-sorts
/// the downloaded anchors instead.
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
    if (word >= pc.query_gate_word_count) return true;
    return (query_gate_words[word] & (1u << (bit & 31u))) != 0u;
}

		const uint SKETCH_CAND_MAX = 384u;            // small shared footprint -> high core occupancy
	uint g_seq_word_offset;
	uint g_local_count;
	uint g_out_base;
	uint g_owned_lo;
	uint g_owned_hi;

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
    // emit_mode 3 is a query-gated count-only pass. emit_mode 2 (DOC0311
    // single-pass) atomically reserves output slots; the pairs are radix-sorted
    // by hash afterwards, so the arbitrary emission order is irrelevant.
    // counters[0] is the running total (= tm); counters[1] is the output
    // capacity guard so an under-estimate cannot write out of bounds.
    if (pc.emit_mode == 3u) {
        atomicAdd(counters[0], 1u);
        return;
    }
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
            // Target index path: write (hash, pos_strand) parallel arrays for the
            // device radix sort. `out_words` is reinterpreted as uint64 hashes.
            uint gmin = g_out_base + dst;
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
    uint block = gl_WorkGroupID.x;
    if (block >= pc.num_blocks) return;
    uint lo = block * pc.block_size;
    uint hi = min(lo + pc.block_size, pc.element_count);
    uint running = 0u;
    for (uint i = lo; i < hi; ++i) {
        out_prefix[i] = running;
        running += in_data[i];
    }
    block_sums[block] = running;
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
    if (gid >= pc.element_count) return;
    uint block = gid / pc.block_size;
    out_prefix[gid] += block_sums[block];
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
struct QRec { uint min_offset; uint min_count; uint anchor_offset; uint query_len; };
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
        // GPU-built index: dense buckets sorted by hash → binary search.
        uint lo = 0u;
        uint hi = pc.bucket_count;
        while (lo < hi) {
            uint mid = (lo + hi) >> 1u;
            if (slots[mid].hash < hash) lo = mid + 1u; else hi = mid;
        }
        if (lo < pc.bucket_count && slots[lo].hash == hash) return lo;
        return 0xffffffffu;
    }
    // CPU-uploaded fallback index: open-addressed linear probe.
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
    // Fast fused-MPF path: Runtime currently provides one QRec. Flatten the
    // record so every global invocation owns minimizers by grid stride instead
    // of making one workgroup loop over the entire query.
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

/// DOC0310 §13 — device radix sort over parallel (key:uint64, value:uint64)
/// arrays. One thread per workgroup processes a contiguous chunk; this keeps the
/// scatter stable (in-chunk order preserved, chunks ordered by id) without
/// shared-atomics. Histogram pass: per-chunk 256-bin counts of digit
/// `(key >> radix_shift) & 0xff`.
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
    uint lo = wg * pc.elems_per_wg;
    uint hi = min(lo + pc.elems_per_wg, pc.element_count);
    for (uint i = lo + tid; i < hi; i += 256u) {
        uint digit = uint((keys_in[i] >> pc.radix_shift) & uint64_t(0xff));
        atomicAdd(sh_hist[digit], 1u);
    }
    barrier();
    histogram[wg * 256u + tid] = sh_hist[tid];
}
)GLSL";

/// DOC0310 §13 — multi-level histogram scan. One workgroup (1 invocation)
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

/// DOC0310 §13 — stable scatter. Each workgroup (1 invocation) re-walks its
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
    uint lo = wg * pc.elems_per_wg;
    uint hi = min(lo + pc.elems_per_wg, pc.element_count);
    sh_cursor[tid] = histogram[wg * 256u + tid];
    barrier();
    uint vw = pc.value_words;
    for (uint block_lo = lo; block_lo < hi; block_lo += 512u) {
        uint block_hi = min(block_lo + 512u, hi);
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
    uint i = gl_GlobalInvocationID.x;
    if (i >= pc.element_count) return;
    flags[i] = (i == 0u || sorted_keys[i] != sorted_keys[i - 1u]) ? 1u : 0u;
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
    uint i = gl_GlobalInvocationID.x;
    if (i >= pc.element_count) return;
    uint bidx = scanned[i] + flags[i] - 1u;
    atomicAdd(buckets[bidx].pos_count, 1u);
    if (flags[i] == 1u) {
        buckets[bidx].hash = sorted_keys[i];
        buckets[bidx].pos_offset = i;
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

int max3(int a, int b, int c) {
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
            int diag = max3(prev_m[j - 1u], prev_i[j - 1u], prev_d[j - 1u])
                + substitution_score(query_symbol, target_symbol);
            int match_state = max(0, diag);
            int insert_state = max(
                max(prev_m[j] - gap_open_total, prev_i[j] - pc.gap_extend_penalty), 0);
            int delete_state = max(
                max(curr_m[j - 1u] - gap_open_total, curr_d[j - 1u] - pc.gap_extend_penalty), 0);

            curr_m[j] = match_state;
            curr_i[j] = insert_state;
            curr_d[j] = delete_state;

            int cell_best = max3(match_state, insert_state, delete_state);
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
//                + (key?kSeqSortKeyMain:kSeqSortTiebreakMain)
// so the dna4/generic canonicalizer is chosen BY ShaderKind (no request
// threading). Storage width is runtime via pc.unit_bits + read_storage_unit64
// (the verbatim helper copied from the candidate body — it is function-local
// there, not a shared symbol). LOCAL_SIZE_X comes from the injected prelude.
// ===========================================================================

/// Push-constant block (32 bytes == SeqSortPush in runtime.cppm). Identical for
/// all four kinds so one struct serves every stage.
constexpr std::string_view kSeqSortPush = R"GLSL(
layout(push_constant) uniform P {
    uint count;              // row_count (key stage) / range_count (tiebreak stage)
    uint unit_bits;          // 8/16/32/64
    uint bits_per_char;      // generic only (dna4 uses const 2)
    uint letters_per_unit;   // generic only
    uint char_mask;          // generic only
    uint used_mask_lo;       // used_storage_mask low 32
    uint used_mask_hi;       // used_storage_mask high 32
    uint pad;
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
    if (r.len > LOCAL_SIZE_X) return;                       // CPU handles oversized runs
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

/// Compose a seq-sort kind's full body from the shared snippets. Order matters:
/// push -> readers/low_symbol_mask -> canon (defines lpu/unit_bits/canon) ->
/// key/compare (uses canon) -> stage main.
[[nodiscard]] std::string seq_sort_body(bool dna4, bool key_stage) {
    std::string s;
    s.reserve(kSeqSortPush.size() + kSeqSortReaders.size() + kSeqSortKeyCmp.size() + 1536);
    s += kSeqSortPush;
    s += kSeqSortReaders;
    s += (dna4 ? kCanonDna4 : kCanonGeneric);
    s += kSeqSortKeyCmp;
    s += (key_stage ? kSeqSortKeyMain : kSeqSortTiebreakMain);
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
        case ShaderKind::generate_units:
            return std::string{kGenerateUnitsBody};
        case ShaderKind::seq_sort_extract_key_dna4:
            return seq_sort_body(/*dna4=*/true,  /*key_stage=*/true);
        case ShaderKind::seq_sort_extract_key_generic:
            return seq_sort_body(/*dna4=*/false, /*key_stage=*/true);
        case ShaderKind::seq_sort_tiebreak_dna4:
            return seq_sort_body(/*dna4=*/true,  /*key_stage=*/false);
        case ShaderKind::seq_sort_tiebreak_generic:
            return seq_sort_body(/*dna4=*/false, /*key_stage=*/false);
        case ShaderKind::align_extend_banded_affine:
            return std::string{kAlignmentExtensionBandedAffineBody};
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
    if (request.kind == ShaderKind::align_extend_banded_affine
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
