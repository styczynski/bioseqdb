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

/// The kinds of compute shader Phase A knows how to compile. Only the tiled DP
/// score-generation kernel exists in Phase A; seeding/extension kernels are
/// Phase B/C and will add enumerators here.
enum class ShaderKind : std::uint8_t {
    chain_tiled_dp_u64,
};

/// One shader-compilation request. `tile_size` and `local_size` are substituted
/// as compile-time `#define`s before glslang runs.
struct ShaderCompileRequest {
    ShaderKind kind;
    std::uint32_t tile_size;
    std::uint32_t local_size;
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

/// Resolve a shader kind to its embedded GLSL body (everything after the two
/// substituted `#define`s).
[[nodiscard]] std::string_view shader_body_for(ShaderKind kind) {
    switch (kind) {
        case ShaderKind::chain_tiled_dp_u64:
            return kChainTiledDpBody;
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
        "{}",
        request.local_size, request.tile_size, shader_body_for(request.kind));

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
        "[shader_compiler] compiled chain_tiled_dp_u64 tile={} local={} -> {} words",
        request.tile_size, request.local_size, spirv.size());
    return spirv;
}

}  // namespace bioseq::gpu::vulkan
