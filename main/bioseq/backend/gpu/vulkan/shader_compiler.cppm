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

export module bioseq_gpu.vulkan.shader_compiler;

import mlogging;

export namespace bioseq::gpu::vulkan {

/// The kinds of compute shader the runtime knows how to compile. Phase A added
/// `chain_tiled_dp_u64`; DOC0307 Phase B adds the four seeding kernels (two
/// sketch dispatches, one seed-hit lookup, one anchor sort).
enum class ShaderKind : std::uint8_t {
    chain_tiled_dp_u64,
    minimizer_candidates,
    minimizer_emit_exact,
    seed_lookup_anchors,
    sort_anchors_bitonic,
};

/// The descriptor-set layout each shader needs. Phase A's fixed four-storage-
/// buffer layout does not fit the Phase B pipelines (3/3/5/2 storage buffers),
/// so the runtime keeps one cached layout per kind (DOC0307 §4.5).
enum class DescriptorLayoutKind : std::uint8_t {
    chain_tiled_dp,
    minimizer_candidates,
    minimizer_emit_exact,
    seed_lookup,
    anchor_sort,
};

/// Number of std430 storage-buffer bindings in each descriptor layout.
[[nodiscard]] constexpr std::uint32_t descriptor_binding_count(
    DescriptorLayoutKind kind) noexcept
{
    switch (kind) {
        case DescriptorLayoutKind::chain_tiled_dp:        return 4;
        case DescriptorLayoutKind::minimizer_candidates:  return 3;
        case DescriptorLayoutKind::minimizer_emit_exact:  return 3;
        case DescriptorLayoutKind::seed_lookup:           return 5;
        case DescriptorLayoutKind::anchor_sort:           return 2;
    }
    return 0;
}

/// Map a shader kind to the descriptor layout it binds.
[[nodiscard]] constexpr DescriptorLayoutKind descriptor_layout_for(
    ShaderKind kind) noexcept
{
    switch (kind) {
        case ShaderKind::chain_tiled_dp_u64:    return DescriptorLayoutKind::chain_tiled_dp;
        case ShaderKind::minimizer_candidates:  return DescriptorLayoutKind::minimizer_candidates;
        case ShaderKind::minimizer_emit_exact:  return DescriptorLayoutKind::minimizer_emit_exact;
        case ShaderKind::seed_lookup_anchors:   return DescriptorLayoutKind::seed_lookup;
        case ShaderKind::sort_anchors_bitonic:  return DescriptorLayoutKind::anchor_sort;
    }
    return DescriptorLayoutKind::chain_tiled_dp;
}

/// Push-constant block size (bytes) each shader's `Params` uniform declares.
/// The anchor sort takes its range from a buffer and has no push constant.
[[nodiscard]] constexpr std::uint32_t push_constant_size(ShaderKind kind) noexcept {
    switch (kind) {
        case ShaderKind::chain_tiled_dp_u64:    return 32;  // ChainingPushConstants
        case ShaderKind::minimizer_candidates:  return 40;  // 10 × uint32
        case ShaderKind::minimizer_emit_exact:  return 40;  // 10 × uint32
        case ShaderKind::seed_lookup_anchors:   return 36;  //  9 × uint32
        case ShaderKind::sort_anchors_bitonic:  return 0;
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
    std::uint32_t max_span = 256;        // minimizer_emit_exact ring length
    std::uint32_t word_kind = 0;
    std::uint32_t alphabet_kind = 0;
    std::uint32_t unit_bits = 0;
    std::uint32_t bits_per_char = 0;
};

/// Thrown by `compile_shader_spirv` on a rejected request or a glslang
/// parse/link failure. The Vulkan runtime catches this and re-wraps it as a
/// `VulkanRuntimeError` so the chaining backend falls back to the CPU kernel.
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
layout(local_size_x = LOCAL_SIZE_X, local_size_y = 1, local_size_z = 1) in;
const uint TILE_SIZE_U = uint(TILE_SIZE);
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
    uint range_len;  // NB: not 'length' — that clashes with GLSL's .length() builtin
};

layout(std430, set = 0, binding = 0) readonly buffer Anchors {
    AnchorWire anchors[];
};
layout(std430, set = 0, binding = 1) readonly buffer Ranges {
    ChainRangeWire ranges[];
};
layout(std430, set = 0, binding = 2) buffer BestScores {
    int best_scores[];
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
} pc;

shared uint64_t sh_ref_position[TILE_SIZE];
shared uint64_t sh_query_position[TILE_SIZE];
shared uint sh_is_reverse[TILE_SIZE];
shared uint sh_span_flags_segment[TILE_SIZE];
shared int sh_score[TILE_SIZE];
shared uint sh_parent[TILE_SIZE];

bool anchor_reverse(AnchorWire a) { return a.is_reverse != 0u; }
uint anchor_span(AnchorWire a) { return a.span_flags_segment & 0xffu; }

AnchorWire shared_anchor(uint idx) {
    return AnchorWire(
        sh_ref_position[idx],
        sh_query_position[idx],
        sh_is_reverse[idx],
        sh_span_flags_segment[idx]);
}

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

void write_best(uint target, int best, uint parent) {
    if (parent != NO_PARENT && best >= best_scores[target]) {
        best_scores[target] = best;
        predecessors[target] = int64_t(parent);
    }
}

void main() {
    uint tid = gl_LocalInvocationID.x;
    uint range_id = gl_WorkGroupID.x;
    if (range_id >= pc.range_count) return;

    ChainRangeWire r = ranges[range_id];
    uint begin = r.begin;
    uint end = min(pc.anchor_count, r.begin + r.range_len);
    if (begin >= end) return;

    for (uint tile_begin = begin; tile_begin < end; tile_begin += TILE_SIZE_U) {
        uint tile_count = min(TILE_SIZE_U, end - tile_begin);

        for (uint i = tid; i < tile_count; i += gl_WorkGroupSize.x) {
            uint global_i = tile_begin + i;
            AnchorWire a = anchors[global_i];
            int64_t parent64 = predecessors[global_i];
            int score = max(best_scores[global_i], int(anchor_span(a)));
            sh_ref_position[i] = a.ref_position;
            sh_query_position[i] = a.query_position;
            sh_is_reverse[i] = a.is_reverse;
            sh_span_flags_segment[i] = a.span_flags_segment;
            sh_score[i] = score;
            sh_parent[i] = parent64 < int64_t(0) ? NO_PARENT : uint(parent64);
        }
        barrier();

        // G3SA-style prologue: all-to-all inside the tile. Predecessor rows
        // advance in j order so sh_score[j] is already final when later anchors
        // compare against j.
        for (uint j = 0u; j < tile_count; ++j) {
            for (uint k = j + 1u + tid; k < tile_count; k += gl_WorkGroupSize.x) {
                if (k - j > uint(pc.max_iter)) continue;
                int pair = chain_score(shared_anchor(k), shared_anchor(j));
                if (pair == INVALID_SCORE) continue;
                int total = pair + sh_score[j];
                if (total >= sh_score[k]) {
                    sh_score[k] = total;
                    sh_parent[k] = tile_begin + j;
                }
            }
            barrier();
        }

        // Tail update: targets after the tile compare against every shared
        // predecessor, matching G3SA's register-held target sweep. Each range is
        // owned by exactly one workgroup, so no atomics are needed.
        uint tail_limit = min(end, tile_begin + TILE_SIZE_U + uint(pc.max_iter));
        for (uint target_base = tile_begin + TILE_SIZE_U; target_base < tail_limit;
             target_base += gl_WorkGroupSize.x) {
            uint target = target_base + tid;
            if (target >= tail_limit) continue;
            AnchorWire cur = anchors[target];
            int best = max(best_scores[target], int(anchor_span(cur)));
            int64_t parent64 = predecessors[target];
            uint best_parent = parent64 < int64_t(0) ? NO_PARENT : uint(parent64);
            for (uint step = 0u; step < tile_count; ++step) {
                uint j = (tid + step) % tile_count;
                uint pred = tile_begin + j;
                if (target <= pred || target - pred > uint(pc.max_iter)) continue;
                int pair = chain_score(cur, shared_anchor(j));
                if (pair == INVALID_SCORE) continue;
                int total = pair + sh_score[j];
                if (total >= best) {
                    best = total;
                    best_parent = pred;
                }
            }
            write_best(target, best, best_parent);
        }
        memoryBarrierBuffer();
        barrier();

        for (uint i = tid; i < tile_count; i += gl_WorkGroupSize.x) {
            uint global_i = tile_begin + i;
            best_scores[global_i] = sh_score[i];
            predecessors[global_i] = sh_parent[i] == NO_PARENT
                ? int64_t(-1)
                : int64_t(sh_parent[i]);
        }
        memoryBarrierBuffer();
        barrier();
    }
}
)GLSL";

/// DOC0307 §6.2 — the candidate-k-mer sketch shader. One invocation per base
/// reads the packed bioseq sequence units directly (no G3SA ASCII transcode),
/// rolls the canonical k-mer with `step_bits` per char, and scrambles it with
/// the byte-for-byte port of `minimap_int_hasher::scramble` (§6.1 header). The
/// shader is alphabet-generic: `pc.alphabet_kind == 0` is the dna4 fast path
/// (XOR complement), otherwise the rank LUTs drive code/rc-code/reset.
constexpr std::string_view kMinimizerCandidatesBody = R"GLSL(
layout(local_size_x = LOCAL_SIZE_X, local_size_y = 1, local_size_z = 1) in;

struct Candidate {
    uint64_t hash;
    uint pos;
    uint strand;
    uint valid;
    uint base_valid;
    uint reserved0;
    uint reserved1;
};

layout(std430, set = 0, binding = 0) readonly buffer SeqWords { uint seq_words[]; };
layout(std430, set = 0, binding = 1) buffer Candidates { Candidate candidates[]; };
layout(std430, set = 0, binding = 2) readonly buffer RankLut {
    uint rank_to_code[256];
    uint rank_to_rc_code[256];
};

layout(push_constant) uniform Params {
    uint bp_size;
    uint window_size;
    uint kmer_size;
    uint unit_bits;
    uint bits_per_char;
    uint letters_per_unit;
    uint alphabet_size;
    uint alphabet_kind;
    uint seq_word_offset;
    uint char_mask;
} pc;

uint read_u32_word(uint word_index) {
    return seq_words[word_index];
}

uint read_storage_unit(uint unit_index, uint unit_bits, uint byte_offset_words) {
    if (unit_bits == 64u) {
        uint lo = read_u32_word(byte_offset_words + unit_index * 2u);
        uint hi = read_u32_word(byte_offset_words + unit_index * 2u + 1u);
        return lo ^ hi; // only used by generic fallback guards
    }
    if (unit_bits == 32u) {
        return read_u32_word(byte_offset_words + unit_index);
    }
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
                     uint unit_bits, uint byte_offset_words, uint char_mask)
{
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
    if (pc.alphabet_kind == 0u) return rank;      // dna4 fast path
    return rank_to_code[rank];                   // dna5 and generic
}

uint rc_code_for_rank(uint rank) {
    if (pc.alphabet_kind == 0u) return rank ^ 3u; // dna4 fast path
    return rank_to_rc_code[rank];                // dna5 and generic
}

void main() {
    uint pos = gl_GlobalInvocationID.x;
    if (pos >= pc.bp_size) return;

    Candidate outc;
    outc.hash = uint64_t(0);
    outc.pos = pos;
    outc.strand = 0u;
    outc.valid = 0u;
    outc.base_valid = 0u;
    outc.reserved0 = 0u;
    outc.reserved1 = 0u;

    uint rank_here = read_rank_at_bp(
        pos, pc.bits_per_char, pc.letters_per_unit, pc.unit_bits,
        pc.seq_word_offset, pc.char_mask);
    outc.base_valid = code_for_rank(rank_here) < 4u ? 1u : 0u;

    if (pc.kmer_size == 0u || pos + 1u < pc.kmer_size) {
        candidates[pos] = outc;
        return;
    }

    uint step_bits = pc.alphabet_kind == 2u ? pc.bits_per_char : 2u;
    uint roll_bits = pc.kmer_size * step_bits;
    uint scramble_bits = pc.kmer_size * 2u;
    if (roll_bits > 64u || scramble_bits > 64u) {
        candidates[pos] = outc;
        return;
    }

    uint64_t roll_mask = (roll_bits == 64u)
        ? ~uint64_t(0)
        : ((uint64_t(1) << roll_bits) - uint64_t(1));
    uint64_t scramble_mask = (scramble_bits == 64u)
        ? ~uint64_t(0)
        : ((uint64_t(1) << scramble_bits) - uint64_t(1));
    uint shift_rc = step_bits * (pc.kmer_size - 1u);
    uint64_t forward = uint64_t(0);
    uint64_t reverse = uint64_t(0);
    bool ok = true;

    uint start = pos + 1u - pc.kmer_size;
    for (uint i = 0u; i < pc.kmer_size; ++i) {
        uint rank = read_rank_at_bp(
            start + i, pc.bits_per_char, pc.letters_per_unit, pc.unit_bits,
            pc.seq_word_offset, pc.char_mask);
        uint code = code_for_rank(rank);
        uint rc_code = rc_code_for_rank(rank);
        if (code >= 4u || rc_code >= 4u) { ok = false; break; }
        forward = ((forward << step_bits) | uint64_t(code)) & roll_mask;
        reverse = (reverse >> step_bits) | (uint64_t(rc_code) << shift_rc);
    }

    if (!ok || forward == reverse) {
        candidates[pos] = outc;
        return;
    }

    bool forward_wins = forward < reverse;
    uint64_t canonical = forward_wins ? forward : reverse;
    outc.hash = minimap_scramble(canonical, scramble_mask);
    outc.strand = forward_wins ? 0u : 1u;
    outc.valid = 1u;
    candidates[pos] = outc;
}
)GLSL";

/// DOC0307 §6.3 — exact emit-minimizers shader. A single-invocation state
/// machine over the `candidates[]` stream that mirrors `collect_minimizers_view`
/// byte-for-byte (duplicate emission, rescan order, palindrome suppression,
/// dna5 N-reset). Writes the chosen `MinimizerRepr<L,K>` word layout directly.
constexpr std::string_view kMinimizerEmitExactBody = R"GLSL(
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

struct Candidate {
    uint64_t hash;
    uint pos;
    uint strand;
    uint valid;
    uint base_valid;
    uint reserved0;
    uint reserved1;
};

layout(std430, set = 0, binding = 0) readonly buffer Candidates { Candidate candidates[]; };
layout(std430, set = 0, binding = 1) buffer OutWords { uint out_words[]; };
layout(std430, set = 0, binding = 2) buffer Counter { uint out_count; uint overflow; };

layout(push_constant) uniform Params {
    uint bp_size;
    uint window_size;
    uint kmer_size;
    uint pos_bits;
    uint hash_bits;
    uint hash_shift;
    uint word_kind;
    uint words32_per_minimizer;
    uint span_value;
    uint max_minimizers;
} pc;

struct Sketch {
    uint64_t hash;
    uint pos;
    uint strand;
    uint is_max;
};

shared Sketch ring[MAX_SPAN];

uint64_t hash_field_mask() {
    return pc.hash_bits >= 64u
        ? ~uint64_t(0)
        : ((uint64_t(1) << pc.hash_bits) - uint64_t(1));
}

Sketch max_sketch() {
    return Sketch(hash_field_mask(), 0u, 0u, 1u);
}

Sketch from_candidate(Candidate c) {
    if (c.valid == 0u) return max_sketch();
    uint64_t h = c.hash & hash_field_mask();
    if (h == hash_field_mask()) return max_sketch();
    return Sketch(h, c.pos, c.strand, 0u);
}

bool sketch_leq(Sketch a, Sketch b) {
    return a.hash <= b.hash;
}

bool sketch_geq(Sketch a, Sketch b) {
    return a.hash >= b.hash;
}

bool sketch_match_but_different_identity(Sketch a, Sketch b) {
    return a.is_max == 0u && b.is_max == 0u
        && a.hash == b.hash
        && (a.pos != b.pos || a.strand != b.strand);
}

void write_minimizer(uint dst, Sketch s) {
    if (pc.word_kind == 0u) {
        uint raw = uint((s.hash << pc.hash_shift)
              | (uint64_t(s.pos) << 1) | uint64_t(s.strand));
        uint word = dst >> 1u;
        uint shift = (dst & 1u) * 16u;
        uint old_word = out_words[word];
        uint clear_mask = ~(0xffffu << shift);
        out_words[word] = (old_word & clear_mask) | ((raw & 0xffffu) << shift);
    } else if (pc.word_kind == 1u) {
        uint base = dst * pc.words32_per_minimizer;
        out_words[base] = uint((s.hash << pc.hash_shift)
              | (uint64_t(s.pos) << 1) | uint64_t(s.strand));
    } else if (pc.word_kind == 2u) {
        uint base = dst * pc.words32_per_minimizer;
        uint64_t raw = (s.hash << pc.hash_shift)
             | (uint64_t(s.pos) << 1) | uint64_t(s.strand);
        out_words[base + 0u] = uint(raw);
        out_words[base + 1u] = uint(raw >> 32);
    } else {
        uint base = dst * pc.words32_per_minimizer;
        uint64_t low = (uint64_t(s.pos) << 1) | uint64_t(s.strand);
        uint64_t high = (s.hash << 8) | uint64_t(pc.span_value & 0xffu);
        out_words[base + 0u] = uint(low);
        out_words[base + 1u] = uint(low >> 32);
        out_words[base + 2u] = uint(high);
        out_words[base + 3u] = uint(high >> 32);
    }
}

void emit_one(Sketch s) {
    if (s.is_max != 0u) return;
    uint dst = out_count;
    out_count = out_count + 1u;
    if (dst >= pc.max_minimizers) {
        overflow = 1u;
        return;
    }
    write_minimizer(dst, s);
}

void for_each_filled(uint buf_pos, uint capacity, inout Sketch current_min,
                     inout uint current_min_pos, bool update_min, bool emit_matches)
{
    for (uint j = buf_pos; j < capacity; ++j) {
        Sketch el = ring[j];
        if (update_min && sketch_geq(current_min, el)) {
            current_min = el;
            current_min_pos = j;
        }
        if (emit_matches && sketch_match_but_different_identity(current_min, el)) {
            emit_one(el);
        }
    }
    for (uint j = 0u; j < buf_pos; ++j) {
        Sketch el = ring[j];
        if (update_min && sketch_geq(current_min, el)) {
            current_min = el;
            current_min_pos = j;
        }
        if (emit_matches && sketch_match_but_different_identity(current_min, el)) {
            emit_one(el);
        }
    }
}

void main() {
    if (pc.window_size == 0u || pc.window_size > MAX_SPAN
        || pc.kmer_size == 0u || pc.bp_size == 0u) {
        return;
    }

    out_count = 0u;
    overflow = 0u;

    uint buf_pos = 0u;
    uint prev_pos = 0u;
    uint valid_since_reset = 0u;
    bool init_mode = true;
    Sketch current_min = max_sketch();
    uint current_min_pos = 0u;

    for (uint seq_pos = 0u; seq_pos < pc.bp_size; ++seq_pos) {
        bool is_init = init_mode;
        Candidate c = candidates[seq_pos];
        Sketch pushed = max_sketch();

        if (c.base_valid == 0u) {
            valid_since_reset = 0u;
        } else {
            valid_since_reset = valid_since_reset + 1u;
            if (valid_since_reset >= pc.kmer_size) {
                pushed = from_candidate(c);
            }
        }

        prev_pos = buf_pos;
        ring[buf_pos] = pushed;
        buf_pos = (buf_pos + 1u) % pc.window_size;

        if ((valid_since_reset == pc.window_size + pc.kmer_size - 1u && is_init)
            && current_min.is_max == 0u) {
            Sketch ignored_min = current_min;
            uint ignored_pos = current_min_pos;
            for_each_filled(buf_pos, pc.window_size, ignored_min, ignored_pos, false, true);
        }

        if (sketch_leq(pushed, current_min)) {
            if ((valid_since_reset >= pc.window_size + pc.kmer_size || !is_init)
                && current_min.is_max == 0u) {
                emit_one(current_min);
            }
            current_min_pos = prev_pos;
            current_min = pushed;
        } else if (prev_pos == current_min_pos) {
            if ((valid_since_reset >= pc.window_size + pc.kmer_size - 1u || !is_init)
                && current_min.is_max == 0u) {
                emit_one(current_min);
            }
            current_min = max_sketch();
            for_each_filled(buf_pos, pc.window_size, current_min, current_min_pos, true, false);
            if ((valid_since_reset >= pc.window_size + pc.kmer_size - 1u || !is_init)
                && current_min.is_max == 0u) {
                for_each_filled(buf_pos, pc.window_size, current_min, current_min_pos, false, true);
            }
        }

        if (init_mode && valid_since_reset >= pc.window_size + pc.kmer_size) {
            init_mode = false;
        }
    }

    if (current_min.is_max == 0u) {
        emit_one(current_min);
    }
}
)GLSL";

/// DOC0307 §7.1 — seed-hit lookup. One invocation per query minimizer probes
/// the GPU-resident open-addressed slot table, then emits one `Anchor` per
/// target position into the exclusive-prefix window `[prefix[i], prefix[i+1])`.
/// Writes `bioseq::chaining::Anchor` directly (full 64-bit ref position).
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

/// Resolve a shader kind to its embedded GLSL body (everything after the
/// `#version`/`#extension`/`#define` prelude the compiler injects).
[[nodiscard]] std::string_view shader_body_for(ShaderKind kind) {
    switch (kind) {
        case ShaderKind::chain_tiled_dp_u64:
            return kChainTiledDpBody;
        case ShaderKind::minimizer_candidates:
            return kMinimizerCandidatesBody;
        case ShaderKind::minimizer_emit_exact:
            return kMinimizerEmitExactBody;
        case ShaderKind::seed_lookup_anchors:
            return kSeedLookupAnchorsBody;
        case ShaderKind::sort_anchors_bitonic:
            return kSortAnchorsBitonicBody;
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

}  // namespace bioseq::gpu::vulkan
