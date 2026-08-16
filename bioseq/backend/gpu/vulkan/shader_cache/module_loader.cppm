/**
 * @file module_loader.cppm
 * @brief Owns MoltenVK shader-module encoding and Vulkan module loading.
 */
module;

#include <cstdint>
#include <cstring>

#if defined(__APPLE__)
#if __has_include(<MoltenVK/mvk_private_api.h>)
#include <MoltenVK/mvk_private_api.h>
#elif __has_include(<mvk_private_api.h>)
#include <mvk_private_api.h>
#else
using MVKMSLSPIRVHeader = std::uint32_t;
inline constexpr std::uint32_t kMVKMagicNumberMSLCompiledCode = 0x19981215u;
#endif
#else
using MVKMSLSPIRVHeader = std::uint32_t;
inline constexpr std::uint32_t kMVKMagicNumberMSLCompiledCode = 0x19981215u;
#endif

#include <vulkan/vulkan.h>

export module bioseq.gpu.vulkan.shader_cache:module_loader;

import :paths_metadata;
import :archive_helper;
import :sealed_seed;
import bioseq.common.checked_arithmetic;
import bioseq.parallelism.memory_admission;
import std;

export namespace bioseq::gpu::vulkan {

/// @brief Loaded Vulkan shader module and its dispatch geometry.
struct shader_module_load {
    VkShaderModule module = VK_NULL_HANDLE; ///< Created module owned by the caller device.
    std::string entry_name; ///< Entry point recorded in cache metadata.
    std::uint32_t wg_x = 1; ///< Local workgroup size X in threads.
    std::uint32_t wg_y = 1; ///< Local workgroup size Y in threads.
    std::uint32_t wg_z = 1; ///< Local workgroup size Z in threads.
};
static_assert(std::is_nothrow_move_constructible_v<shader_module_load>);

/// @brief MoltenVK shader-module bytes retained under memory admission.
struct moltenvk_shader_module_code {
    // Declared first so words release before the transient permit.
    bioseq::parallelism::memory_permit permit; ///< Peak reservation held while words live.
    std::vector<std::uint32_t> words; ///< Header plus metallib payload in MoltenVK module layout.
    std::size_t code_size_bytes = 0; ///< Byte length passed to VkShaderModuleCreateInfo::codeSize.
    std::size_t header_size_bytes = sizeof(MVKMSLSPIRVHeader); ///< Leading MVKMSLSPIRVHeader size.
    std::uint64_t admission_count = 0u; ///< Number of admission reservations taken.
    std::uint64_t peak_admitted_bytes = 0u; ///< Largest admitted byte budget for this object.
    std::uint64_t retained_capacity_bytes = 0u; ///< Vector capacity currently charged to the permit.
    bool admission_saturated = false; ///< Set when allocation would exceed the admitted peak.
};

/// @brief Returns the MoltenVK compiled-MSL shader module header size.
[[nodiscard]] inline constexpr std::size_t moltenvk_msl_spirv_header_size() noexcept {
    return sizeof(MVKMSLSPIRVHeader);
}

/// @brief Returns the MoltenVK compiled-MSL shader module magic value.
[[nodiscard]] inline constexpr std::uint32_t moltenvk_msl_compiled_code_magic() noexcept {
    return static_cast<std::uint32_t>(kMVKMagicNumberMSLCompiledCode);
}

/// @brief Wraps admitted metallib bytes in the MoltenVK compiled-MSL module format.
/// @param[in] metallib_bytes Validated Metal library bytes.
/// @return Admitted module words or an admission or size diagnostic.
[[nodiscard]] inline std::expected<moltenvk_shader_module_code, std::string>
make_moltenvk_metallib_module_code(std::span<const std::byte> metallib_bytes) {
    if (metallib_bytes.empty()) {
        return std::unexpected("metallib empty");
    }
    if (metallib_bytes.size() >
        (std::numeric_limits<std::size_t>::max)() - sizeof(MVKMSLSPIRVHeader)) {
        return std::unexpected("metallib module size overflow");
    }

    moltenvk_shader_module_code code{};
    code.header_size_bytes = sizeof(MVKMSLSPIRVHeader);
    code.code_size_bytes = code.header_size_bytes + metallib_bytes.size();
    const std::size_t word_count =
        (code.code_size_bytes + sizeof(std::uint32_t) - 1u) / sizeof(std::uint32_t);
    const std::uint64_t exact_request = bioseq::common::saturating_mul(
        static_cast<std::uint64_t>(word_count), sizeof(std::uint32_t));
    const std::uint64_t peak_request = bioseq::common::saturating_mul(
        exact_request, bioseq::parallelism::k_vector_preallocation_headroom_factor);
    if (exact_request == (std::numeric_limits<std::uint64_t>::max)() ||
        peak_request == (std::numeric_limits<std::uint64_t>::max)()) {
        return std::unexpected("metallib module admission size saturated");
    }
    bioseq::parallelism::memory_admission_failure admission_failure{};
    auto permit = bioseq::parallelism::memory_admission_controller::instance().try_reserve(
        peak_request, &admission_failure);
    if (!permit) {
        return std::unexpected(shader_cache_message(
            "metallib module admission denied: reason=",
            bioseq::parallelism::memory_admission_failure_reason_name(admission_failure.reason),
            " requested=",
            admission_failure.requested_bytes));
    }
    code.permit = std::move(*permit);
    code.admission_count = 1u;
    code.peak_admitted_bytes = peak_request;
    code.words.resize(word_count);
    const std::uint64_t retained = bioseq::common::saturating_mul(
        static_cast<std::uint64_t>(code.words.capacity()), sizeof(std::uint32_t));
    if (retained == (std::numeric_limits<std::uint64_t>::max)() || retained > code.permit.bytes()) {
        code.admission_saturated = true;
        return std::unexpected("metallib module allocation exceeded admitted peak");
    }
    code.permit.shrink_to(retained);
    code.retained_capacity_bytes = retained;

    auto* bytes = reinterpret_cast<std::byte*>(code.words.data());
    const MVKMSLSPIRVHeader header = static_cast<MVKMSLSPIRVHeader>(kMVKMagicNumberMSLCompiledCode);
    std::memcpy(bytes, &header, sizeof(header));
    std::memcpy(bytes + code.header_size_bytes, metallib_bytes.data(), metallib_bytes.size());
    return std::move(code);
}

/// @brief Reads a Metal library and wraps it in the MoltenVK module format.
/// @param[in] metallib_path Metal library path.
/// @return Admitted module words or an I/O, admission, or size diagnostic.
[[nodiscard]] inline std::expected<moltenvk_shader_module_code, std::string>
make_moltenvk_metallib_module_code(const std::filesystem::path& metallib_path) {
    auto bytes = sealed_shader_cache_detail::read_regular_file(metallib_path, "metallib");
    if (!bytes)
        return std::unexpected(bytes.error());
    return make_moltenvk_metallib_module_code(*bytes);
}

/// @brief Creates a Vulkan shader module from an on-disk complete cache entry.
/// @param[in] device Vulkan device that will own the module.
/// @param[in] cache_dir Shader-cache directory.
/// @param[in] key Shader variant identity.
/// @param[in] create_shader_module Device dispatch entry for module creation.
/// @return Loaded shader module and geometry or a cache or Vulkan diagnostic.
[[nodiscard]] inline std::expected<shader_module_load, std::string>
load_cached_module(VkDevice device,
                   const std::filesystem::path& cache_dir,
                   const shader_variant_key& key,
                   PFN_vkCreateShaderModule create_shader_module) {
    if (create_shader_module == nullptr) {
        return std::unexpected("vkCreateShaderModule function pointer is null");
    }
    auto code = make_moltenvk_metallib_module_code(cache_metallib_path(cache_dir, key));
    if (!code) {
        return std::unexpected(code.error());
    }

    // Re-read and validate metadata after the metallib bytes are resident and
    // immediately before module creation.  The earlier completeness probe is
    // only an optimization; it cannot be the trust boundary because a compile
    // service may report success without publishing a contract-valid record, and
    // cache files may be replaced between the probe and this loader.
    std::string entry;
    auto meta = read_validated_cache_meta(cache_dir, key, &entry);
    if (!meta) {
        return std::unexpected(meta.error());
    }

    shader_module_load result{
        .module = VK_NULL_HANDLE,
        .entry_name = std::move(entry),
        .wg_x = meta->wg_x,
        .wg_y = meta->wg_y,
        .wg_z = meta->wg_z,
    };
    VkShaderModuleCreateInfo module_info{};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = code->code_size_bytes;
    module_info.pCode = code->words.data();
    const VkResult vk_result = create_shader_module(device, &module_info, nullptr, &result.module);
    if (vk_result != VK_SUCCESS) {
        return std::unexpected(shader_cache_message("vkCreateShaderModule(metallib): VkResult ",
                                                    static_cast<int>(vk_result)));
    }
    return std::expected<shader_module_load, std::string>{std::in_place, std::move(result)};
}

/// @brief Creates a Vulkan shader module from one owned sealed key image.
/// @param[in] device Vulkan device that will own the module.
/// @param[in] image Sealed cache key image with metallib bytes and metadata.
/// @param[in] create_shader_module Device dispatch entry for module creation.
/// @return Loaded shader module and geometry or an image or Vulkan diagnostic.
[[nodiscard]] inline std::expected<shader_module_load, std::string>
create_shader_module_from_sealed_key_image(VkDevice device,
                                           const sealed_shader_cache_key_image& image,
                                           PFN_vkCreateShaderModule create_shader_module) {
    if (create_shader_module == nullptr) {
        return std::unexpected("vkCreateShaderModule function pointer is null");
    }
    if (image.entry_name.empty() || image.metallib_bytes.empty() ||
        !(image.meta.key == image.key)) {
        return std::unexpected("sealed shader cache key image is incomplete");
    }
    auto code = make_moltenvk_metallib_module_code(image.metallib_bytes);
    if (!code)
        return std::unexpected(code.error());

    shader_module_load result{
        .module = VK_NULL_HANDLE,
        .entry_name = image.entry_name,
        .wg_x = image.meta.wg_x,
        .wg_y = image.meta.wg_y,
        .wg_z = image.meta.wg_z,
    };
    VkShaderModuleCreateInfo module_info{};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = code->code_size_bytes;
    module_info.pCode = code->words.data();
    const VkResult vk_result = create_shader_module(device, &module_info, nullptr, &result.module);
    if (vk_result != VK_SUCCESS) {
        return std::unexpected(shader_cache_message(
            "vkCreateShaderModule(sealed metallib): VkResult ", static_cast<int>(vk_result)));
    }
    return std::expected<shader_module_load, std::string>{std::in_place, std::move(result)};
}

/// @brief Creates a Vulkan shader module from one borrowed sealed key view.
/// @param[in] device Vulkan device that will own the module.
/// @param[in] view Borrowed sealed cache key view with metallib bytes and metadata.
/// @param[in] create_shader_module Device dispatch entry for module creation.
/// @return Loaded shader module and geometry or a view or Vulkan diagnostic.
[[nodiscard]] inline std::expected<shader_module_load, std::string>
create_shader_module_from_sealed_key_view(VkDevice device,
                                          const sealed_shader_cache_key_view& view,
                                          PFN_vkCreateShaderModule create_shader_module) {
    if (create_shader_module == nullptr) {
        return std::unexpected("vkCreateShaderModule function pointer is null");
    }
    if (view.entry_name.empty() || view.metallib_bytes.empty() || !(view.meta.key == view.key)) {
        return std::unexpected("sealed shader cache key view is incomplete");
    }
    auto code = make_moltenvk_metallib_module_code(view.metallib_bytes);
    if (!code)
        return std::unexpected(code.error());

    shader_module_load result{
        .module = VK_NULL_HANDLE,
        .entry_name = view.entry_name,
        .wg_x = view.meta.wg_x,
        .wg_y = view.meta.wg_y,
        .wg_z = view.meta.wg_z,
    };
    VkShaderModuleCreateInfo module_info{};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = code->code_size_bytes;
    module_info.pCode = code->words.data();
    const VkResult vk_result = create_shader_module(device, &module_info, nullptr, &result.module);
    if (vk_result != VK_SUCCESS) {
        return std::unexpected(shader_cache_message(
            "vkCreateShaderModule(sealed seed metallib): VkResult ", static_cast<int>(vk_result)));
    }
    return std::expected<shader_module_load, std::string>{std::in_place, std::move(result)};
}

/// @brief Loads one shader module from a retained process seed.
/// @param[in] device Vulkan device that will own the module.
/// @param[in] seed Retained sealed shader-cache seed image.
/// @param[in] key Shader variant identity to resolve inside the seed.
/// @param[in] create_shader_module Device dispatch entry for module creation.
/// @return Loaded shader module and geometry or a membership or Vulkan diagnostic.
[[nodiscard]] inline std::expected<shader_module_load, std::string>
load_sealed_cached_module(VkDevice device,
                          const sealed_shader_cache_seed_image& seed,
                          const shader_variant_key& key,
                          PFN_vkCreateShaderModule create_shader_module) {
    auto view = make_sealed_shader_cache_key_view(seed, key);
    if (!view) {
        return std::unexpected(shader_cache_message("sealed shader cache process image rejected ",
                                                    cache_file_stem(key),
                                                    ": ",
                                                    view.error()));
    }
    return create_shader_module_from_sealed_key_view(device, *view, create_shader_module);
}

/// @brief Loads one shader module from a validated on-disk sealed seed.
/// @param[in] device Vulkan device that will own the module.
/// @param[in] cache_dir Shader-cache directory.
/// @param[in] cache_digest Cache identity digest for the sealed seed.
/// @param[in] exact_aot_keys Exact AOT variant keys recorded in the seed.
/// @param[in] key Shader variant identity to load.
/// @param[in] create_shader_module Device dispatch entry for module creation.
/// @return Loaded shader module and geometry or a seed or Vulkan diagnostic.
[[nodiscard]] inline std::expected<shader_module_load, std::string>
load_sealed_cached_module(VkDevice device,
                          const std::filesystem::path& cache_dir,
                          std::string_view cache_digest,
                          std::span<const shader_variant_key> exact_aot_keys,
                          const shader_variant_key& key,
                          PFN_vkCreateShaderModule create_shader_module) {
    auto image = load_sealed_shader_cache_key_image(cache_dir, cache_digest, exact_aot_keys, key);
    if (!image) {
        return std::unexpected(shader_cache_message(
            "sealed shader cache provider rejected ", cache_file_stem(key), ": ", image.error()));
    }
    return create_shader_module_from_sealed_key_image(device, *image, create_shader_module);
}

/// @brief Loads one module after asking a compile service to fill an absent entry.
/// @tparam p_service Runtime compile service type satisfying `is_runtime_compile_service`.
/// @param[in] device Vulkan device that will own the module.
/// @param[in] cache_dir Shader-cache directory.
/// @param[in] key Shader variant identity.
/// @param[in] create_shader_module Device dispatch entry for module creation.
/// @param[in] service Compile service invoked when the cache entry is incomplete.
/// @return Loaded shader module and geometry or a compile, cache, or Vulkan diagnostic.
template <is_runtime_compile_service p_service>
[[nodiscard]] inline std::expected<shader_module_load, std::string>
load_module(VkDevice device,
            const std::filesystem::path& cache_dir,
            const shader_variant_key& key,
            PFN_vkCreateShaderModule create_shader_module,
            p_service& service) {
    if (!cache_entry_complete(cache_dir, key)) {
        auto compile = service.ensure_variant_compiled(key);
        if (!compile) {
            return std::unexpected(compile.error());
        }
        if (auto published = cache_entry_status(cache_dir, key); !published) {
            return std::unexpected(
                shader_cache_message("shader compile service returned success without a complete "
                                     "contract-valid cache entry for ",
                                     cache_file_stem(key),
                                     ": ",
                                     published.error()));
        }
    }
    return load_cached_module(device, cache_dir, key, create_shader_module);
}

} // namespace bioseq::gpu::vulkan
