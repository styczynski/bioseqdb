/**
 * @file glslang.cppm
 * @brief Selects embedded shader bodies and compiles them with glslang.
 */
module;

#include <glslang/Include/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

module bioseq.gpu.vulkan.shader_compiler:glslang;

import :registry;
import :chaining;
import :sketch_index;
import :seeding;
import :alignment_prefilter;
import :sequence_sort;
import bioseq.common.logging;
import std;

namespace bioseq::gpu::vulkan {

namespace {

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
        return seq_sort_body(/*dna4=*/true, sequence_sort_stage::key);
    case shader_kind::seq_sort_extract_key_generic:
        return seq_sort_body(/*dna4=*/false, sequence_sort_stage::key);
    case shader_kind::seq_sort_tiebreak_dna4:
        return seq_sort_body(/*dna4=*/true, sequence_sort_stage::tiebreak);
    case shader_kind::seq_sort_tiebreak_generic:
        return seq_sort_body(/*dna4=*/false, sequence_sort_stage::tiebreak);
    case shader_kind::seq_sort_global_compare_dna4:
        return seq_sort_body(/*dna4=*/true, sequence_sort_stage::global_compare);
    case shader_kind::seq_sort_global_compare_generic:
        return seq_sort_body(/*dna4=*/false, sequence_sort_stage::global_compare);
    case shader_kind::seq_sort_dense_scatter_dna4:
        return seq_sort_body(/*dna4=*/true, sequence_sort_stage::dense_scatter);
    case shader_kind::seq_sort_dense_scatter_generic:
        return seq_sort_body(/*dna4=*/false, sequence_sort_stage::dense_scatter);
    case shader_kind::seq_sort_prefix_check_dna4:
        return seq_sort_body(/*dna4=*/true, sequence_sort_stage::prefix_check);
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
/// race. Call it exactly once for the process lifetime. We intentionally do not
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

} // namespace

/**
 * @brief Compiles a registered compute shader to SPIR-V words.
 *
 * Builds the full GLSL source by prepending the Vulkan language version,
 * required integer extension, and compile-time geometry definitions before
 * driving glslang for Vulkan 1.2 and SPIR-V 1.5.
 *
 * @param[in] request Shader family and compile-time geometry selectors.
 * @return Compiled SPIR-V module as 32-bit words.
 * @throws shader_compile_error When geometry is invalid or glslang cannot
 * parse, link, or emit the module.
 */
[[nodiscard]] std::vector<std::uint32_t>
compile_shader_spirv(const shader_compile_request& request) {
    // Reject geometry the shared-array layout cannot represent BEFORE compiling.
    // 1024 is the largest tile the chaining pipeline selector requests
    // (choose_vulkan_chaining_pipeline), and a zero local size has no invocation
    // to run the tile loop. The device's maxComputeWorkGroupInvocations bound on
    // local_size is enforced by the runtime, which is the only place that knows
    // the limit.
    if (request.tile_size == 0 || request.tile_size > 1024) {
        throw shader_compile_error(fmt::format(
            "compile_shader_spirv: tile_size {} out of range (1..1024)", request.tile_size));
    }
    if (request.local_size == 0) {
        throw shader_compile_error("compile_shader_spirv: local_size must be non-zero");
    }
    if ((request.kind == shader_kind::align_extend_banded_affine ||
         request.kind == shader_kind::align_extend_simple_cigar) &&
        (request.max_span == 0 || request.max_span > 512)) {
        throw shader_compile_error(fmt::format(
            "compile_shader_spirv: alignment max_span {} out of range (1..512)", request.max_span));
    }
    if (request.kind == shader_kind::sneaky_snake_rank_batch && request.max_span != 512u) {
        throw shader_compile_error(fmt::format(
            "compile_shader_spirv: SneakySnake submaze span {} must be 512", request.max_span));
    }
    if (is_gatekeeper_rank_batch_kind(request.kind) &&
        (request.tile_size != 384u || request.local_size != 64u || request.max_span != 128u)) {
        throw shader_compile_error(
            fmt::format("compile_shader_spirv: GateKeeper key ({}, {}, {}) must be "
                        "(384, 64, 128)",
                        request.tile_size,
                        request.local_size,
                        request.max_span));
    }

    ensure_glslang_initialized();

    const std::string source =
        fmt::format("#version 460\n"
                    "#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require\n"
                    "#define compile_time_workgroup_size_x {}\n"
                    "#define compile_time_tile_size {}\n"
                    "#define compile_time_maximum_span {}\n"
                    "{}",
                    request.local_size,
                    request.tile_size,
                    request.max_span,
                    shader_body_for(request.kind));

    const TBuiltInResource resource = make_default_resource();

    glslang::TShader shader(EShLangCompute);
    const char* source_ptr = source.c_str();
    shader.setStrings(&source_ptr, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, EShLangCompute, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_2);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_5);

    const EShMessages messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);

    if (!shader.parse(&resource, 100, false, messages)) {
        const std::string log = fmt::format(
            "glslang parse failed: {} {}", shader.getInfoLog(), shader.getInfoDebugLog());
        bioseq_log<logging_level::error>("[shader_compiler] {}", log);
        throw shader_compile_error(log);
    }

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
        const std::string log = fmt::format(
            "glslang link failed: {} {}", program.getInfoLog(), program.getInfoDebugLog());
        bioseq_log<logging_level::error>("[shader_compiler] {}", log);
        throw shader_compile_error(log);
    }

    std::vector<std::uint32_t> spirv;
    glslang::SpvOptions spv_options;
    spv_options.disableOptimizer = true;
    spv_options.validate = false;
    spv::SpvBuildLogger logger;
    glslang::GlslangToSpv(*program.getIntermediate(EShLangCompute), spirv, &logger, &spv_options);
    if (spirv.empty()) {
        const std::string log =
            fmt::format("glslang produced empty SPIR-V: {}", logger.getAllMessages());
        bioseq_log<logging_level::error>("[shader_compiler] {}", log);
        throw shader_compile_error(log);
    }
    bioseq_log<logging_level::debug>(
        "[shader_compiler] compiled shader kind={} tile={} local={} max_span={} -> {} words",
        static_cast<int>(request.kind),
        request.tile_size,
        request.local_size,
        request.max_span,
        spirv.size());
    return spirv;
}

/**
 * @brief Returns the embedded GLSL body used by cache identity computation.
 * @param[in] kind Registered shader family.
 * @return Exact embedded GLSL body without the compiler prelude.
 */
[[nodiscard]] std::string shader_body_for_digest(shader_kind kind) {
    return shader_body_for(kind);
}

} // namespace bioseq::gpu::vulkan
