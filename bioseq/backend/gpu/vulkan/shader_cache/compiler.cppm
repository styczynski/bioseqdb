/**
 * @file compiler.cppm
 * @brief Owns shader cache identity, SPIR-V validation, MSL translation, and compilation.
 */
module;

#include <spirv_cross/spirv_msl.hpp>

export module bioseq.gpu.vulkan.shader_cache:compiler;

import :paths_metadata;
import :subprocess;
import :archive_helper;
import bioseq.common.hashing;
import bioseq.gpu.vulkan.shader_compiler;
import bioseq.gpu.vulkan.shader_layout;
import bioseq.gpu.vulkan.shader_variants;
import std;

export namespace bioseq::gpu::vulkan {

/// @brief Computes the cache identity for toolchain, device, ABI, and shader inputs.
/// @param[in] inputs Static cache-build inputs.
/// @return Sixteen-character lowercase hexadecimal digest.
[[nodiscard]] inline std::string shader_cache_digest_hex(const shader_cache_build_inputs& inputs) {
    std::uint64_t hash = bioseq::common::fnv1a_64("bioseq-shader-cache-contract");
    hash = bioseq::common::hash_combine_64(hash, inputs.moltenvk_build);
    hash = bioseq::common::hash_combine_64(hash, inputs.device_uuid);
    hash = bioseq::common::hash_combine_64(hash, probe_metal_version());
    hash = bioseq::common::hash_combine_64(hash, system_uname_digest_component());
    hash = bioseq::common::hash_combine_64(hash, inputs.msl.major);
    hash = bioseq::common::hash_combine_64(hash, inputs.msl.minor);
    hash = bioseq::common::hash_combine_64(hash, inputs.msl.patch);
    hash = bioseq::common::hash_combine_64(
        hash, "bioseq-metal-binary-archive-qualified-helper-device-contract");
    // SPIR-V -> MSL resource indices are pipeline ABI, even when the GLSL
    // shader body itself is unchanged. Include the resource mapping and
    // concrete layout so cached artifacts must satisfy the current pipeline
    // contract.
    hash = bioseq::common::hash_combine_64(hash, "bioseq-spirv-to-msl-resource-abi");

    for_each_static_shader_variant(inputs.variants, [&hash](const shader_variant_key& key) {
        const shader_kind kind = checked_shader_kind(key.kind);
        const descriptor_layout_kind descriptor_layout = descriptor_layout_for(kind);
        hash = bioseq::common::hash_combine_64(hash, key.kind);
        hash = bioseq::common::hash_combine_64(hash, key.tile_size);
        hash = bioseq::common::hash_combine_64(hash, key.local_size);
        hash = bioseq::common::hash_combine_64(hash, key.max_span);
        hash = bioseq::common::hash_combine_64(hash, static_cast<std::uint32_t>(descriptor_layout));
        hash = bioseq::common::hash_combine_64(hash, descriptor_binding_count(descriptor_layout));
        hash = bioseq::common::hash_combine_64(hash, push_constant_size(kind));
        hash = bioseq::common::hash_combine_64(hash, shader_body_for_digest(kind));
    });
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << hash;
    return out.str();
}

/// @brief Validates reflected SPIR-V storage and push-constant layouts for a variant.
/// @param[in] key Shader variant identity.
/// @param[in] spirv Compiled SPIR-V words.
/// @return Success or the first ABI mismatch.
[[nodiscard]] inline std::expected<void, std::string>
validate_spirv_layout(const shader_variant_key& key, std::span<const std::uint32_t> spirv) {
    const spirv_layout_scan scan = scan_spirv_layout(spirv);
    const shader_kind kind = checked_shader_kind(key.kind);
    if (kind == shader_kind::chain_tiled_dp_u64) {
        if (!scan.has_array_stride(24) || !scan.has_struct_offsets({0u, 8u, 16u, 20u})) {
            return std::unexpected("SPIR-V chain_tiled_dp_u64 ABI mismatch");
        }
    }
    if (kind == shader_kind::seed_lookup_anchors &&
        (!scan.has_binding(0) || !scan.has_binding(1) || !scan.has_binding(2) ||
         !scan.has_binding(3) || !scan.has_binding(4) || !scan.has_binding(5) ||
         !scan.has_array_stride(24) || !scan.has_array_stride(8) ||
         !scan.has_struct_offsets({0u, 8u, 16u, 20u}))) {
        return std::unexpected("SPIR-V seed_lookup_anchors ABI mismatch");
    }
    if (kind == shader_kind::sort_anchors_bitonic && !scan.has_array_stride(8)) {
        return std::unexpected("SPIR-V sort_anchors_bitonic range stride mismatch");
    }
    if (kind == shader_kind::generate_units && (!scan.has_binding(0) || !scan.has_binding(1) ||
                                                !scan.has_array_stride(key.max_span / 8u))) {
        return std::unexpected("SPIR-V generate_units layout mismatch");
    }
    const auto binding_scalar = [&scan](std::uint32_t binding, std::uint32_t stride) {
        return scan.has_binding_array_stride(0u, binding, stride);
    };
    const auto binding_struct = [&scan](std::uint32_t binding,
                                        std::uint32_t stride,
                                        std::initializer_list<std::uint32_t> offsets) {
        return scan.has_binding_array_stride(0u, binding, stride) &&
               scan.has_binding_struct_offsets(
                   0u, binding, std::span<const std::uint32_t>{offsets.begin(), offsets.size()});
    };
    if (kind == shader_kind::align_extend_banded_affine &&
        (!scan.has_binding(0) || !scan.has_binding(1) || !scan.has_binding(2) ||
         !scan.has_binding(3) || !scan.has_array_stride(4) || !scan.has_array_stride(16) ||
         !scan.has_array_stride(32))) {
        return std::unexpected("SPIR-V alignment extension layout mismatch");
    }
    if (kind == shader_kind::align_extend_simple_cigar &&
        (!scan.has_binding(0) || !scan.has_binding(1) || !scan.has_binding(2) ||
         !scan.has_binding(3) || !scan.has_binding(4) || !scan.has_binding(5) ||
         !scan.has_array_stride(4) || !scan.has_array_stride(40) || !scan.has_array_stride(48))) {
        return std::unexpected("SPIR-V alignment CIGAR extension layout mismatch");
    }
    if ((kind == shader_kind::sneaky_snake_rank_batch || is_gatekeeper_rank_batch_kind(kind)) &&
        (!binding_scalar(0u, 4u) || !binding_scalar(1u, 4u) ||
         !binding_struct(2u, 20u, {0u, 4u, 8u, 12u, 16u}) ||
         !binding_struct(3u, 16u, {0u, 4u, 8u, 12u}) ||
         !scan.has_push_constant_offsets(std::array<std::uint32_t, 4>{0u, 4u, 8u, 12u}))) {
        return std::unexpected("SPIR-V rank-prefilter batch layout mismatch");
    }
    return {};
}

/// @brief MSL source and reflected execution metadata produced by SPIRV-Cross.
struct msl_compile_result {
    std::string msl;
    std::string entry_name;
    std::uint32_t wg_x = 1;
    std::uint32_t wg_y = 1;
    std::uint32_t wg_z = 1;
    bool needs_buffer_size_buffer = false;
    std::uint32_t buffer_size_buffer_index = 25u;
};

/// @brief Translates SPIR-V to MSL with the runtime descriptor mapping.
/// @param[in] spirv Compiled SPIR-V words consumed by SPIRV-Cross.
/// @param[in] key Shader variant identity.
/// @param[in] version Requested Metal shading-language version.
/// @return MSL source and reflected metadata, or a translation diagnostic.
[[nodiscard]] inline std::expected<msl_compile_result, std::string>
spirv_to_msl(std::vector<std::uint32_t> spirv, const shader_variant_key& key, msl_version version) {
    try {
        spirv_cross::CompilerMSL compiler(std::move(spirv));
        spirv_cross::CompilerMSL::Options options{};
        options.set_msl_version(version.major, version.minor, version.patch);
        options.argument_buffers = false;
        // MoltenVK and SPIRV-Cross reserve Metal buffer 25 for the runtime
        // array-length table used by OpArrayLength. Keep it explicit and reject
        // any descriptor mapping that would collide with that implicit input.
        options.buffer_size_buffer_index = 25u;
        compiler.set_msl_options(options);

        const shader_kind kind = checked_shader_kind(key.kind);
        const std::uint32_t push_size = push_constant_size(kind);
        const std::uint32_t binding_count = descriptor_binding_count(descriptor_layout_for(kind));
        const std::uint32_t base = push_size > 0 ? 1u : 0u;
        if (base + binding_count > options.buffer_size_buffer_index) {
            return std::unexpected("SPIR-V descriptor mapping collides with MSL buffer-size input");
        }
        const auto resources = compiler.get_shader_resources();
        std::vector<std::uint64_t> observed_storage_bindings;
        observed_storage_bindings.reserve(resources.storage_buffers.size());
        for (const auto& resource : resources.storage_buffers) {
            if (!compiler.has_decoration(resource.id, spv::DecorationDescriptorSet) ||
                !compiler.has_decoration(resource.id, spv::DecorationBinding)) {
                return std::unexpected("SPIR-V storage resource lacks set/binding decoration");
            }
            const std::uint32_t descriptor_set =
                compiler.get_decoration(resource.id, spv::DecorationDescriptorSet);
            const std::uint32_t binding =
                compiler.get_decoration(resource.id, spv::DecorationBinding);
            if (descriptor_set != 0u || binding >= binding_count) {
                return std::unexpected(
                    shader_cache_message("SPIR-V resource outside descriptor ABI: set=",
                                         descriptor_set,
                                         " binding=",
                                         binding,
                                         " binding_count=",
                                         binding_count));
            }
            const std::uint64_t identity = (std::uint64_t(descriptor_set) << 32u) | binding;
            if (std::find(observed_storage_bindings.begin(),
                          observed_storage_bindings.end(),
                          identity) != observed_storage_bindings.end()) {
                return std::unexpected(
                    shader_cache_message("SPIR-V duplicate storage binding: set=",
                                         descriptor_set,
                                         " binding=",
                                         binding));
            }
            observed_storage_bindings.push_back(identity);
        }
        const std::size_t expected_push_count = push_size == 0u ? 0u : 1u;
        if (resources.push_constant_buffers.size() != expected_push_count) {
            return std::unexpected(
                shader_cache_message("SPIR-V push-constant resource count mismatch: observed=",
                                     resources.push_constant_buffers.size(),
                                     " expected=",
                                     expected_push_count));
        }
        auto add_storage_binding = [&compiler](std::uint32_t desc_set,
                                               std::uint32_t binding,
                                               std::uint32_t count,
                                               std::uint32_t msl_buffer) {
            spirv_cross::MSLResourceBinding rb{};
            rb.stage = spv::ExecutionModelGLCompute;
            rb.basetype = spirv_cross::SPIRType::Unknown;
            rb.desc_set = desc_set;
            rb.binding = binding;
            rb.count = count;
            rb.msl_buffer = msl_buffer;
            compiler.add_msl_resource_binding(rb);
        };
        if (push_size > 0) {
            add_storage_binding(spirv_cross::ResourceBindingPushConstantDescriptorSet,
                                spirv_cross::ResourceBindingPushConstantBinding,
                                1u,
                                0u);
        }
        for (std::uint32_t binding = 0; binding < binding_count; ++binding) {
            add_storage_binding(0u, binding, 1u, base + binding);
        }

        msl_compile_result result{};
        result.msl = compiler.compile();
        result.entry_name =
            compiler.get_cleansed_entry_point_name("main", spv::ExecutionModelGLCompute);
        result.wg_x = compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, 0);
        result.wg_y = compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, 1);
        result.wg_z = compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, 2);
        result.needs_buffer_size_buffer = compiler.needs_buffer_size_buffer();
        result.buffer_size_buffer_index = options.buffer_size_buffer_index;
        const shader_workgroup_size expected = expected_shader_workgroup_size(key);
        if (result.wg_x != expected.x || result.wg_y != expected.y || result.wg_z != expected.z) {
            return std::unexpected(shader_cache_message("SPIR-V/MSL workgroup mismatch: reflected=",
                                                        result.wg_x,
                                                        'x',
                                                        result.wg_y,
                                                        'x',
                                                        result.wg_z,
                                                        " expected=",
                                                        expected.x,
                                                        'x',
                                                        expected.y,
                                                        'x',
                                                        expected.z));
        }
        return result;
    } catch (const std::exception& exception) {
        return std::unexpected(
            shader_cache_message("SPIR-V to MSL failed: ", exception.what()));
    }
}

/// @brief Compiles one shader variant through SPIR-V, MSL, metallib, and archive stages.
/// @param[in] cache_dir Shader-cache directory.
/// @param[in] key Shader variant identity.
/// @param[in] msl_version Requested Metal shading-language version.
/// @param[in] force_semantic_rebuild Whether all prior artifacts must be replaced.
/// @return Success or the first compilation, ABI, archive, or I/O diagnostic.
[[nodiscard]] inline std::expected<void, std::string>
compile_variant_to_cache(const std::filesystem::path& cache_dir,
                         const shader_variant_key& key,
                         msl_version msl_version,
                         bool force_semantic_rebuild = false) {
    if (!is_shader_variant_key_valid(key)) {
        return std::unexpected(
            shader_cache_message("invalid shader variant ", cache_file_stem(key)));
    }
    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);
    if (ec) {
        return std::unexpected(shader_cache_message(
            "create cache dir ", cache_dir.string(), " failed: ", ec.message()));
    }
    const std::filesystem::path stem = cache_dir / cache_file_stem(key);
    const std::filesystem::path metal_path = stem.string() + ".tmp.metal";
    const std::filesystem::path air_path = stem.string() + ".air.tmp";
    const std::filesystem::path temporary_metallib_path = stem.string() + ".metallib.tmp";
    const std::filesystem::path metallib_final = cache_metallib_path(cache_dir, key);
    if (force_semantic_rebuild) {
        std::vector<std::filesystem::path> stale_paths{
            cache_meta_path(cache_dir, key),
            metallib_final,
            metal_path,
            air_path,
            temporary_metallib_path,
        };
        for (const std::filesystem::path& archive : cache_binary_archive_paths(cache_dir, key)) {
            stale_paths.push_back(archive);
            stale_paths.emplace_back(archive.string() + ".tmp");
            const std::filesystem::path qualification =
                cache_binary_archive_qualification_path(archive);
            stale_paths.push_back(qualification);
            stale_paths.emplace_back(qualification.string() + ".tmp");
        }
        for (const std::filesystem::path& stale : stale_paths) {
            ec.clear();
            std::filesystem::remove(stale, ec);
            if (ec) {
                return std::unexpected(
                    shader_cache_message("remove unsealed shader artifact before semantic rebuild ",
                                         stale.string(),
                                         " failed: ",
                                         ec.message()));
            }
        }
    }
    if (!force_semantic_rebuild && cache_entry_complete(cache_dir, key)) {
        return {};
    }
    if (!force_semantic_rebuild && cache_metallib_entry_complete(cache_dir, key)) {
        std::string existing_entry;
        auto existing_meta = read_meta(cache_meta_path(cache_dir, key), &existing_entry);
        if (!existing_meta) {
            return std::unexpected(existing_meta.error());
        }
        return ensure_binary_archive(cache_dir, key, *existing_meta, existing_entry, true);
    }

    std::vector<std::uint32_t> spirv;
    try {
        spirv = compile_shader_spirv(key.to_request());
    } catch (const std::exception& exception) {
        return std::unexpected(shader_cache_message("glslang failed: ", exception.what()));
    }
    if (auto valid = validate_spirv_layout(key, spirv); !valid) {
        return valid;
    }
    auto msl = spirv_to_msl(std::move(spirv), key, msl_version);
    if (!msl) {
        return std::unexpected(msl.error());
    }
    const shader_workgroup_size expected_workgroup = expected_shader_workgroup_size(key);
    const shader_workgroup_size compiled_workgroup{msl->wg_x == 0 ? 1u : msl->wg_x,
                                                   msl->wg_y == 0 ? 1u : msl->wg_y,
                                                   msl->wg_z == 0 ? 1u : msl->wg_z};
    if (!(compiled_workgroup == expected_workgroup)) {
        return std::unexpected(shader_cache_message("compiled SPIR-V workgroup mismatch for ",
                                                    cache_file_stem(key),
                                                    ": reflected=",
                                                    compiled_workgroup.x,
                                                    'x',
                                                    compiled_workgroup.y,
                                                    'x',
                                                    compiled_workgroup.z,
                                                    " expected=",
                                                    expected_workgroup.x,
                                                    'x',
                                                    expected_workgroup.y,
                                                    'x',
                                                    expected_workgroup.z));
    }

    const std::span<const std::byte> msl_bytes{reinterpret_cast<const std::byte*>(msl->msl.data()),
                                               msl->msl.size()};
    if (auto written = durable_write_file(metal_path, msl_bytes); !written) {
        return written;
    }

    const std::string metal_std =
        shader_cache_message("metal", msl_version.major, '.', msl_version.minor);
    const std::vector<std::string> metal_args{xcrun_executable(),
                                              "-sdk",
                                              "macosx",
                                              "metal",
                                              "-std=" + metal_std,
                                              "-c",
                                              metal_path.string(),
                                              "-o",
                                              air_path.string()};
    if (auto compiled = spawn_wait(metal_args); !compiled) {
        return compiled;
    }
    const std::vector<std::string> metallib_args{xcrun_executable(),
                                                 "-sdk",
                                                 "macosx",
                                                 "metallib",
                                                 air_path.string(),
                                                 "-o",
                                                 temporary_metallib_path.string()};
    if (auto linked = spawn_wait(metallib_args); !linked) {
        return linked;
    }
    if (auto renamed = durable_rename_existing(temporary_metallib_path, metallib_final); !renamed) {
        return renamed;
    }

    shader_cache_metadata meta{};
    meta.key = key;
    meta.wg_x = compiled_workgroup.x;
    meta.wg_y = compiled_workgroup.y;
    meta.wg_z = compiled_workgroup.z;
    meta.entry_len = static_cast<std::uint32_t>(std::min<std::size_t>(msl->entry_name.size(), 64u));
    const std::string entry = msl->entry_name.substr(0, meta.entry_len);
    const std::vector<std::byte> meta_bytes = meta_to_bytes(meta, entry);
    if (auto meta_written = durable_write_file(cache_meta_path(cache_dir, key), meta_bytes);
        !meta_written) {
        return meta_written;
    }
    if (auto archive_written = ensure_binary_archive(cache_dir, key, meta, entry, true);
        !archive_written) {
        return archive_written;
    }
    std::filesystem::remove(metal_path, ec);
    std::filesystem::remove(air_path, ec);
    return {};
}

/// @brief Ensures one complete cache entry, compiling it when absent.
/// @return Success or the compilation diagnostic.
[[nodiscard]] inline std::expected<void, std::string> ensure_compiled(
    const std::filesystem::path& cache_dir, const shader_variant_key& key, msl_version version) {
    if (cache_entry_complete(cache_dir, key)) {
        return {};
    }
    return compile_variant_to_cache(cache_dir, key, version);
}

/// @brief Rebuilds one AOT entry for a seed that has not yet been sealed.
///
/// Metadata and metallib bytes are regenerated from the in-process
/// SPIR-V/layout/MSL pipeline. Every binary archive is rebuilt and passes the
/// reopen and `FailOnBinaryArchiveMiss` controls.
///
/// @return Success or the compilation or completeness diagnostic.
[[nodiscard]] inline std::expected<void, std::string> ensure_compiled_for_sealed_seed(
    const std::filesystem::path& cache_dir, const shader_variant_key& key, msl_version version) {
    auto compiled = compile_variant_to_cache(cache_dir, key, version, true);
    if (!compiled)
        return compiled;
    if (auto status = cache_entry_status(cache_dir, key); !status) {
        return std::unexpected(
            shader_cache_message("sealed-seed compile produced incomplete entry for ",
                                 cache_file_stem(key),
                                 ": ",
                                 status.error()));
    }
    return {};
}

} // namespace bioseq::gpu::vulkan
