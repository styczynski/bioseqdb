/**
 * @file paths_metadata.cppm
 * @brief Defines shader-cache paths, metadata, immutable seed contracts, and file identity.
 */
module;

#include <cerrno>
#include <cstdint>
#include <cstring>
#if defined(__APPLE__)
#include <dlfcn.h>
#endif
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

export module bioseq.gpu.vulkan.shader_cache:paths_metadata;

import bioseq.common.checked_arithmetic;
import bioseq.gpu.vulkan.shader_compiler;
import bioseq.gpu.vulkan.shader_variants;
import bioseq.parallelism.memory_admission;
import std;

export namespace bioseq::gpu::vulkan {

#if defined(__APPLE__)
/// @brief Resolve a packaged loader beside its owning image, or the build-owned loader.
/// Only module initialization uses this path; shader and numerical kernels are unchanged.
[[nodiscard]] std::filesystem::path selected_vulkan_loader_path(std::error_code& error) {
    static const unsigned char image_anchor = 0;
    Dl_info owner{};
    if (::dladdr(&image_anchor, &owner) != 0 && owner.dli_fname != nullptr) {
        const auto sibling = std::filesystem::path(owner.dli_fname).parent_path() /
                             "bioseq_runtime" / "libvulkan.1.dylib";
        if (std::filesystem::is_regular_file(sibling, error)) {
            return std::filesystem::canonical(sibling, error);
        }
    }
    error.clear();
    return std::filesystem::canonical(BIOSEQ_RECEIPT_OWNED_VULKAN_LOADER_PATH, error);
}
#endif

/// @brief Formats one shader-cache diagnostic without constraining argument types.
/// @tparam p_argument_types Stream-insertable diagnostic argument types.
/// @param[in] arguments Values appended in order.
/// @return Concatenated diagnostic text.
template <class... p_argument_types>
[[nodiscard]] inline std::string shader_cache_message(p_argument_types&&... arguments) {
    std::ostringstream out;
    (out << ... << std::forward<p_argument_types>(arguments));
    return out.str();
}

/// @brief Stable error categories returned by shader-cache orchestration.
enum class shader_cache_error : std::uint32_t {
    ok = 0,                     ///< Operation completed successfully.
    no_toolchain = 1,           ///< Required compiler toolchain is unavailable.
    glslang = 2,                ///< GLSL-to-SPIR-V compilation failed.
    spirv_to_msl = 3,           ///< SPIR-V-to-MSL translation failed.
    metal_spawn = 4,            ///< Metal compiler process could not be started.
    metal_exit = 5,             ///< Metal compiler process returned failure.
    metallib_write = 6,         ///< Metal library artifact could not be written.
    metadata_io = 7,            ///< Metadata read or write failed.
    abi_scan = 8,               ///< Compiled shader layout violated its host ABI.
    dynamic_symbol_missing = 9, ///< Required runtime symbol is unavailable.
    queue_full = 10,            ///< Compiler service queue has no capacity.
    supervisor_gone = 11,       ///< Compiler supervisor is no longer reachable.
    timeout = 12,               ///< Bounded compiler operation expired.
};

/// @brief Fixed metadata stored next to one compiled shader artifact.
struct shader_cache_metadata {
    std::uint32_t magic = 0xB10C0CACu;
    std::uint32_t wire_revision = 1u;
    shader_variant_key key{};
    std::uint32_t wg_x = 1;
    std::uint32_t wg_y = 1;
    std::uint32_t wg_z = 1;
    std::uint32_t entry_len = 0;
};
static_assert(std::is_standard_layout_v<shader_cache_metadata>);

/// @brief Metal Shading Language version included in the cache identity.
struct msl_version {
    std::uint32_t major = 3;
    std::uint32_t minor = 2;
    std::uint32_t patch = 0;
};

/// @brief Static inputs that define a shader-cache build identity.
struct shader_cache_build_inputs {
    vulkan_static_variant_inputs variants{};
    msl_version msl{};
    std::string moltenvk_build = "conservative";
    std::string device_uuid = "conservative";
};

/// @brief Serialized artifact categories in a sealed shader-cache seed.
enum class sealed_shader_cache_artifact_kind : std::uint32_t {
    // Wire value 0 remains reserved and cannot identify a seed artifact.
    reserved_command_metallib = 0,        ///< Reserved wire value.
    shader_meta = 1,                      ///< Shader metadata record.
    shader_metallib = 2,                  ///< Compiled Metal library.
    metal_binary_archive = 3,             ///< Metal binary archive.
    metal_binary_archive_qualification = 4, ///< Archive qualification receipt.
    moltenvk_command_library = 5,         ///< Exact source/options/metallib/provenance bundle.
};

/// @brief Manifest descriptor for one sealed shader-cache artifact.
struct sealed_shader_cache_artifact {
    sealed_shader_cache_artifact_kind kind =
        sealed_shader_cache_artifact_kind::reserved_command_metallib;
    std::uint32_t key_index = (std::numeric_limits<std::uint32_t>::max)();
    std::string filename;
    std::uint64_t size = 0;
    std::array<std::uint8_t, 32> sha256{};

    /// @brief Compares artifact kind, key ordinal, filename, size, and digest.
    [[nodiscard]] bool operator==(const sealed_shader_cache_artifact&) const noexcept = default;
};

/// @brief Canonical manifest for one immutable shader-cache seed.
struct sealed_shader_cache_manifest {
    std::string cache_digest;
    std::vector<shader_variant_key> keys;
    std::vector<sealed_shader_cache_artifact> artifacts;
    std::array<std::uint8_t, 32> seed_sha256{};
    std::uint64_t seed_size = 0;
    std::uint64_t manifest_checksum = 0;
};

/// @brief Manifest descriptor paired with the exact retained artifact bytes.
struct sealed_shader_cache_artifact_image {
    sealed_shader_cache_artifact descriptor;
    std::vector<std::byte> bytes;
};

/// @brief Loader-generated validation receipt for one complete process-local seed image.
/// Construction is private: runtime configuration cannot manufacture the
/// counters or copy them from manifest metadata without executing the loader.
class sealed_shader_cache_seed_receipt {
  public:
    /// @brief Returns the nonzero loader receipt identity.
    [[nodiscard]] std::uint64_t identity() const noexcept {
        return identity_;
    }
    /// @brief Returns the number of complete seed loads represented by the receipt.
    [[nodiscard]] std::uint64_t load_count() const noexcept {
        return load_count_;
    }
    /// @brief Returns the number of manifest reads.
    [[nodiscard]] std::uint64_t manifest_read_count() const noexcept {
        return manifest_read_count_;
    }
    /// @brief Returns the number of artifact reads.
    [[nodiscard]] std::uint64_t artifact_read_count() const noexcept {
        return artifact_read_count_;
    }
    /// @brief Returns the loaded manifest extent.
    [[nodiscard]] std::uint64_t manifest_bytes() const noexcept {
        return manifest_bytes_;
    }
    /// @brief Returns the aggregate loaded artifact extent.
    [[nodiscard]] std::uint64_t artifact_bytes() const noexcept {
        return artifact_bytes_;
    }
    /// @brief Returns the measured seed load duration.
    [[nodiscard]] std::uint64_t load_ns() const noexcept {
        return load_ns_;
    }
    /// @brief Returns the number of memory-admission operations.
    [[nodiscard]] std::uint64_t admission_count() const noexcept {
        return admission_count_;
    }
    /// @brief Returns the peak admitted byte count.
    [[nodiscard]] std::uint64_t peak_admitted_bytes() const noexcept {
        return peak_admitted_bytes_;
    }
    /// @brief Returns the retained process-image capacity.
    [[nodiscard]] std::uint64_t retained_capacity_bytes() const noexcept {
        return retained_capacity_bytes_;
    }
    /// @brief Reports whether byte accounting saturated.
    [[nodiscard]] bool admission_saturated() const noexcept {
        return admission_saturated_;
    }
    /// @brief Validates load counts, byte counts, timing, and admission accounting.
    [[nodiscard]] bool is_accepted() const noexcept {
        return identity_ != 0u && load_count_ == 1u && manifest_read_count_ == 1u &&
               artifact_read_count_ != 0u && manifest_bytes_ != 0u && artifact_bytes_ != 0u &&
               load_ns_ != 0u && admission_count_ == 1u && peak_admitted_bytes_ != 0u &&
               retained_capacity_bytes_ != 0u && retained_capacity_bytes_ <= peak_admitted_bytes_ &&
               !admission_saturated_;
    }

  private:
    sealed_shader_cache_seed_receipt(std::uint64_t identity,
                                     std::uint64_t artifact_read_count,
                                     std::uint64_t manifest_bytes,
                                     std::uint64_t artifact_bytes,
                                     std::uint64_t load_ns,
                                     std::uint64_t peak_admitted_bytes) noexcept
        : identity_(identity), artifact_read_count_(artifact_read_count),
          manifest_bytes_(manifest_bytes), artifact_bytes_(artifact_bytes), load_ns_(load_ns),
          peak_admitted_bytes_(peak_admitted_bytes) {}

    std::uint64_t identity_ = 0u;
    std::uint64_t load_count_ = 1u;
    std::uint64_t manifest_read_count_ = 1u;
    std::uint64_t artifact_read_count_ = 0u;
    std::uint64_t manifest_bytes_ = 0u;
    std::uint64_t artifact_bytes_ = 0u;
    std::uint64_t load_ns_ = 0u;
    std::uint64_t admission_count_ = 1u;
    std::uint64_t peak_admitted_bytes_ = 0u;
    std::uint64_t retained_capacity_bytes_ = 0u;
    bool admission_saturated_ = false;

    friend class sealed_shader_cache_seed_image;
};

/// @brief Complete immutable shader seed retained by one backend process. Every
/// consumer receives spans into these exact verified reads. No consumer needs
/// to reopen a cache pathname after this image is installed.
class sealed_shader_cache_seed_image {
  public:
    sealed_shader_cache_seed_image(const sealed_shader_cache_seed_image&) = delete;
    sealed_shader_cache_seed_image& operator=(const sealed_shader_cache_seed_image&) = delete;
    sealed_shader_cache_seed_image(sealed_shader_cache_seed_image&&) noexcept = default;
    sealed_shader_cache_seed_image& operator=(sealed_shader_cache_seed_image&&) noexcept = delete;

    /// @brief Returns the retained validated manifest.
    [[nodiscard]] const sealed_shader_cache_manifest& manifest() const noexcept {
        return manifest_;
    }
    /// @brief Returns the exact retained manifest bytes.
    [[nodiscard]] std::span<const std::byte> manifest_bytes() const noexcept {
        return manifest_bytes_;
    }
    /// @brief Returns retained artifact images in manifest order.
    [[nodiscard]] std::span<const sealed_shader_cache_artifact_image> artifacts() const noexcept {
        return artifacts_;
    }
    /// @brief Returns the loader-issued seed receipt.
    [[nodiscard]] const sealed_shader_cache_seed_receipt& receipt() const noexcept {
        return receipt_;
    }
    /// @brief Returns one retained artifact by ordinal.
    [[nodiscard]] const sealed_shader_cache_artifact_image*
    artifact(std::size_t ordinal) const noexcept {
        return ordinal < artifacts_.size() ? &artifacts_[ordinal] : nullptr;
    }
    /// @brief Validates retained artifacts, receipt counts, and memory admission.
    [[nodiscard]] bool is_accepted() const noexcept {
        return receipt_.is_accepted() && artifacts_.size() == manifest_.artifacts.size() &&
               receipt_.artifact_read_count() == artifacts_.size() &&
               receipt_.artifact_bytes() == manifest_.seed_size &&
               receipt_.manifest_bytes() == manifest_bytes_.size() && permit_ &&
               permit_.bytes() == receipt_.retained_capacity_bytes();
    }
    /// @brief Computes process-owned storage retained by the seed image.
    [[nodiscard]] std::uint64_t owned_capacity_bytes() const noexcept {
        using bioseq::common::saturating_add;
        using bioseq::common::saturating_mul;
        std::uint64_t owned = sizeof(sealed_shader_cache_seed_image);
        owned =
            saturating_add(owned, static_cast<std::uint64_t>(manifest_.cache_digest.capacity()));
        owned = saturating_add(owned,
                               saturating_mul(static_cast<std::uint64_t>(manifest_.keys.capacity()),
                                              sizeof(shader_variant_key)));
        owned = saturating_add(
            owned,
            saturating_mul(static_cast<std::uint64_t>(manifest_.artifacts.capacity()),
                           sizeof(sealed_shader_cache_artifact)));
        for (const sealed_shader_cache_artifact& artifact : manifest_.artifacts) {
            owned = saturating_add(owned, static_cast<std::uint64_t>(artifact.filename.capacity()));
        }
        owned = saturating_add(owned, static_cast<std::uint64_t>(manifest_bytes_.capacity()));
        owned = saturating_add(owned,
                               saturating_mul(static_cast<std::uint64_t>(artifacts_.capacity()),
                                              sizeof(sealed_shader_cache_artifact_image)));
        for (const sealed_shader_cache_artifact_image& artifact : artifacts_) {
            owned = saturating_add(
                owned, static_cast<std::uint64_t>(artifact.descriptor.filename.capacity()));
            owned = saturating_add(owned, static_cast<std::uint64_t>(artifact.bytes.capacity()));
        }
        return owned;
    }

  private:
    sealed_shader_cache_seed_image(sealed_shader_cache_manifest manifest,
                                   std::vector<std::byte> manifest_bytes,
                                   std::vector<sealed_shader_cache_artifact_image> artifacts,
                                   std::uint64_t receipt_identity,
                                   std::uint64_t artifact_read_count,
                                   std::uint64_t loaded_manifest_bytes,
                                   std::uint64_t loaded_artifact_bytes,
                                   std::uint64_t load_ns,
                                   std::uint64_t peak_admitted_bytes,
                                   bioseq::parallelism::memory_permit permit) noexcept
        : permit_(std::move(permit)), manifest_(std::move(manifest)),
          manifest_bytes_(std::move(manifest_bytes)), artifacts_(std::move(artifacts)),
          receipt_(receipt_identity,
                   artifact_read_count,
                   loaded_manifest_bytes,
                   loaded_artifact_bytes,
                   load_ns,
                   peak_admitted_bytes) {}

    [[nodiscard]] bool reconcile_admission() noexcept {
        const std::uint64_t retained = owned_capacity_bytes();
        if (retained == (std::numeric_limits<std::uint64_t>::max)() || !permit_ ||
            retained > permit_.bytes()) {
            receipt_.admission_saturated_ = true;
            return false;
        }
        permit_.shrink_to(retained);
        receipt_.retained_capacity_bytes_ = retained;
        return true;
    }

    // Declared first so reverse member destruction releases every admitted
    // allocation before returning the lifetime permit to the controller.
    bioseq::parallelism::memory_permit permit_;
    sealed_shader_cache_manifest manifest_;
    std::vector<std::byte> manifest_bytes_;
    std::vector<sealed_shader_cache_artifact_image> artifacts_;
    sealed_shader_cache_seed_receipt receipt_;

    friend std::expected<sealed_shader_cache_seed_image, std::string>
    load_validated_sealed_shader_cache_seed(const std::filesystem::path&,
                                            std::string_view,
                                            std::span<const shader_variant_key>);
};

/// @brief Exact immutable inputs retained from the same reads whose sizes and
/// SHA-256 values were checked against the sealed manifest. Module creation
/// consumes these bytes directly and never reopens a cache pathname.
struct sealed_shader_cache_key_image {
    sealed_shader_cache_manifest manifest;
    shader_variant_key key{};
    shader_cache_metadata meta{};
    std::string entry_name;
    std::vector<std::byte> metallib_bytes;
};

/// @brief Borrowed view into a process-owned seed image. The caller must keep the
/// seed alive until module creation returns.
struct sealed_shader_cache_key_view {
    shader_variant_key key{};
    shader_cache_metadata meta{};
    std::string entry_name;
    std::span<const std::byte> metallib_bytes;
};

/// @brief Compile service accepted by the runtime cache loader.
/// @tparam p_service Service type exposing `ensure_variant_compiled`.
template <class p_service>
concept is_runtime_compile_service = requires(p_service& service, shader_variant_key key) {
    { service.ensure_variant_compiled(key) } -> std::same_as<std::expected<void, std::string>>;
};

/// @brief Resolves the compiled Metal library path for one shader variant.
/// @param[in] cache_dir Shader-cache directory.
/// @param[in] key Shader variant identity.
/// @return Variant Metal library path.
[[nodiscard]] inline std::filesystem::path
cache_metallib_path(const std::filesystem::path& cache_dir, const shader_variant_key& key) {
    return cache_dir / (cache_file_stem(key) + ".metallib");
}

/// @brief Resolves the primary Metal binary archive path for one shader variant.
/// @param[in] cache_dir Shader-cache directory.
/// @param[in] key Shader variant identity.
/// @return Primary archive path.
[[nodiscard]] inline std::filesystem::path
cache_binary_archive_path(const std::filesystem::path& cache_dir, const shader_variant_key& key) {
    return cache_dir / (cache_file_stem(key) + ".metalar");
}

/// @brief Resolves the qualification receipt adjacent to a Metal archive.
/// @param[in] archive_path Metal archive path.
/// @return Qualification receipt path.
[[nodiscard]] inline std::filesystem::path
cache_binary_archive_qualification_path(const std::filesystem::path& archive_path) {
    return archive_path.string() + ".qualification";
}

/// @brief Enumerates exact and conservative-thread archive paths for a variant.
/// @param[in] cache_dir Shader-cache directory.
/// @param[in] key Shader variant identity.
/// @return Ordered archive paths accepted for the variant.
[[nodiscard]] inline std::vector<std::filesystem::path>
cache_binary_archive_paths(const std::filesystem::path& cache_dir, const shader_variant_key& key) {
    constexpr std::uint32_t k_conservative_moltenvk_cap = 256u;
    const shader_workgroup_size workgroup = expected_shader_workgroup_size(key);
    const std::uint64_t exact_threads = static_cast<std::uint64_t>(workgroup.x) *
                                        static_cast<std::uint64_t>(workgroup.y) *
                                        static_cast<std::uint64_t>(workgroup.z);
    std::vector<std::filesystem::path> paths{cache_binary_archive_path(cache_dir, key)};
    if (exact_threads != k_conservative_moltenvk_cap) {
        paths.push_back(cache_dir / (cache_file_stem(key) + ".max_threads_" +
                                     std::to_string(k_conservative_moltenvk_cap) + ".metalar"));
    }
    return paths;
}

/// @brief Resolves the immutable MoltenVK command-library bundle path.
/// @param[in] cache_dir Shader-cache directory.
/// @return Global command-library artifact path for the seed.
[[nodiscard]] inline std::filesystem::path
cache_moltenvk_command_library_path(const std::filesystem::path& cache_dir) {
    return cache_dir / "moltenvk_command.sealed";
}

/// @brief Resolves the metadata path for one shader variant.
/// @param[in] cache_dir Shader-cache directory.
/// @param[in] key Shader variant identity.
/// @return Variant metadata path.
[[nodiscard]] inline std::filesystem::path cache_meta_path(const std::filesystem::path& cache_dir,
                                                           const shader_variant_key& key) {
    return cache_dir / (cache_file_stem(key) + ".meta");
}

/// @brief Resolves the sealed seed manifest path in a cache directory.
/// @param[in] cache_dir Shader-cache directory.
/// @return Sealed manifest path.
[[nodiscard]] inline std::filesystem::path
sealed_shader_cache_manifest_path(const std::filesystem::path& cache_dir) {
    return cache_dir / "bioseq_shader_cache.sealed";
}

namespace sealed_shader_cache_detail {

/// @brief Incremental SHA-256 state used for sealed artifact identities.
class sha256_hasher {
  private:
    std::array<std::uint32_t, 8> state_{0x6a09e667u,
                                        0xbb67ae85u,
                                        0x3c6ef372u,
                                        0xa54ff53au,
                                        0x510e527fu,
                                        0x9b05688cu,
                                        0x1f83d9abu,
                                        0x5be0cd19u};
    std::array<std::byte, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t total_bytes_ = 0;

    inline static constexpr std::array<std::uint32_t, 64> k_round_constants{
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
        0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
        0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
        0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
        0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
        0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
        0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
        0xc67178f2u};

    /// @brief Compresses one complete 64-byte SHA-256 block.
    /// @param[in] block Pointer to 64 readable bytes.
    void transform(const std::byte* block) noexcept {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16u; ++index) {
            const std::size_t offset = index * 4u;
            words[index] =
                static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(block[offset])) << 24u |
                static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(block[offset + 1u]))
                    << 16u |
                static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(block[offset + 2u]))
                    << 8u |
                static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(block[offset + 3u]));
        }
        for (std::size_t index = 16u; index < words.size(); ++index) {
            const std::uint32_t s0 = std::rotr(words[index - 15u], 7) ^
                                     std::rotr(words[index - 15u], 18) ^ (words[index - 15u] >> 3u);
            const std::uint32_t s1 = std::rotr(words[index - 2u], 17) ^
                                     std::rotr(words[index - 2u], 19) ^ (words[index - 2u] >> 10u);
            words[index] = words[index - 16u] + s0 + words[index - 7u] + s1;
        }
        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        std::uint32_t f = state_[5];
        std::uint32_t g = state_[6];
        std::uint32_t h = state_[7];
        for (std::size_t index = 0; index < words.size(); ++index) {
            const std::uint32_t sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const std::uint32_t choose = (e & f) ^ (~e & g);
            const std::uint32_t temporary1 =
                h + sum1 + choose + k_round_constants[index] + words[index];
            const std::uint32_t sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temporary2 = sum0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

  public:
    /// @brief Appends raw bytes to the digest state.
    /// @param[in] bytes Bytes consumed before return.
    void update(std::span<const std::byte> bytes) noexcept {
        for (const std::byte value : bytes) {
            buffer_[buffered_++] = value;
            ++total_bytes_;
            if (buffered_ == buffer_.size()) {
                transform(buffer_.data());
                buffered_ = 0;
            }
        }
    }

    /// @brief Appends string bytes to the digest state.
    /// @param[in] text Text bytes consumed before return.
    void update(std::string_view text) noexcept {
        update(std::as_bytes(std::span{text.data(), text.size()}));
    }

    /// @brief Appends one unsigned value in canonical little-endian order.
    /// @param[in] value Value serialized as eight bytes.
    void update_little_endian(std::uint64_t value) noexcept {
        std::array<std::byte, 8> bytes{};
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            bytes[index] = static_cast<std::byte>(value & 0xffu);
            value >>= 8u;
        }
        update(bytes);
    }

    /// @brief Finalizes the accumulated bytes.
    /// @return SHA-256 digest bytes.
    [[nodiscard]] std::array<std::uint8_t, 32> finalize() noexcept {
        const std::uint64_t bit_length = total_bytes_ * 8u;
        buffer_[buffered_++] = std::byte{0x80};
        if (buffered_ > 56u) {
            while (buffered_ < buffer_.size()) {
                buffer_[buffered_++] = std::byte{0};
            }
            transform(buffer_.data());
            buffered_ = 0;
        }
        while (buffered_ < 56u) {
            buffer_[buffered_++] = std::byte{0};
        }
        for (std::size_t index = 0; index < 8u; ++index) {
            buffer_[63u - index] = static_cast<std::byte>((bit_length >> (index * 8u)) & 0xffu);
        }
        transform(buffer_.data());
        std::array<std::uint8_t, 32> digest{};
        for (std::size_t word = 0; word < state_.size(); ++word) {
            for (std::size_t byte = 0; byte < 4u; ++byte) {
                digest[word * 4u + byte] =
                    static_cast<std::uint8_t>((state_[word] >> (24u - byte * 8u)) & 0xffu);
            }
        }
        return digest;
    }
};

/// @brief Computes the nonzero FNV checksum stored in a sealed manifest.
/// @param[in] bytes Serialized manifest bytes before the checksum field.
/// @return Nonzero manifest checksum.
[[nodiscard]] inline std::uint64_t manifest_checksum(std::span<const std::byte> bytes) noexcept {
    std::uint64_t hash = 14695981039346656037ull;
    for (const std::byte value : bytes) {
        hash ^= std::to_integer<std::uint8_t>(value);
        hash *= 1099511628211ull;
    }
    return hash == 0u ? 0x9e3779b97f4a7c15ull : hash;
}

} // namespace sealed_shader_cache_detail

/// @brief Serializes fixed shader metadata followed by its entrypoint bytes.
/// @param[in] meta Fixed metadata header.
/// @param[in] entry_name Metal entrypoint name.
/// @return Serialized metadata payload.
[[nodiscard]] inline std::vector<std::byte> meta_to_bytes(const shader_cache_metadata& meta,
                                                          std::string_view entry_name) {
    std::vector<std::byte> bytes(sizeof(shader_cache_metadata) + entry_name.size());
    std::memcpy(bytes.data(), &meta, sizeof(meta));
    std::memcpy(bytes.data() + sizeof(meta), entry_name.data(), entry_name.size());
    return bytes;
}

/// @brief Parses fixed shader metadata and its bounded entrypoint name.
/// @param[in] bytes Serialized metadata payload.
/// @param[out] entry_name Optional destination for the entrypoint name.
/// @return Parsed metadata or a wire-format diagnostic.
[[nodiscard]] inline std::expected<shader_cache_metadata, std::string>
parse_cache_meta_bytes(std::span<const std::byte> bytes, std::string* entry_name) {
    if (bytes.size() < sizeof(shader_cache_metadata)) {
        return std::unexpected("meta too small");
    }
    shader_cache_metadata meta{};
    std::memcpy(&meta, bytes.data(), sizeof(meta));
    if (meta.magic != 0xB10C0CACu || meta.wire_revision != 1u) {
        return std::unexpected("metadata serialized contract mismatch");
    }
    if (meta.entry_len > 64u || sizeof(shader_cache_metadata) + meta.entry_len > bytes.size()) {
        return std::unexpected("meta entry length invalid");
    }
    if (entry_name != nullptr) {
        entry_name->assign(
            reinterpret_cast<const char*>(bytes.data() + sizeof(shader_cache_metadata)),
            meta.entry_len);
    }
    return meta;
}

/// @brief Reads and parses one shader metadata file.
/// @param[in] path Metadata file path.
/// @param[out] entry_name Optional destination for the entrypoint name.
/// @return Parsed metadata or an I/O or wire-format diagnostic.
[[nodiscard]] inline std::expected<shader_cache_metadata, std::string>
read_meta(const std::filesystem::path& path, std::string* entry_name) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::unexpected("meta missing");
    }
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    input.seekg(0, std::ios::beg);
    if (size < static_cast<std::streamoff>(sizeof(shader_cache_metadata))) {
        return std::unexpected("meta too small");
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!input) {
        return std::unexpected("meta read failed");
    }
    return parse_cache_meta_bytes(bytes, entry_name);
}

/// @brief Validates parsed metadata against an exact shader variant.
/// @param[in] bytes Serialized metadata payload.
/// @param[in] key Expected shader variant identity.
/// @param[out] entry_name Optional destination for the validated entrypoint.
/// @return Validated metadata or the first identity mismatch.
[[nodiscard]] inline std::expected<shader_cache_metadata, std::string>
validated_cache_meta_from_bytes(std::span<const std::byte> bytes,
                                const shader_variant_key& key,
                                std::string* entry_name) {
    std::string entry;
    auto meta = parse_cache_meta_bytes(bytes, &entry);
    if (!meta) {
        return std::unexpected(meta.error());
    }
    if (!(meta->key == key)) {
        return std::unexpected("meta key mismatch");
    }
    if (entry.empty()) {
        return std::unexpected("meta entry name is empty");
    }
    const shader_workgroup_size expected = expected_shader_workgroup_size(key);
    if (meta->wg_x != expected.x || meta->wg_y != expected.y || meta->wg_z != expected.z) {
        return std::unexpected(shader_cache_message("meta workgroup mismatch: cached=",
                                                    meta->wg_x,
                                                    'x',
                                                    meta->wg_y,
                                                    'x',
                                                    meta->wg_z,
                                                    " expected=",
                                                    expected.x,
                                                    'x',
                                                    expected.y,
                                                    'x',
                                                    expected.z));
    }
    if (entry_name != nullptr) {
        *entry_name = std::move(entry);
    }
    return *meta;
}

/// @brief Reads and validates cached metadata for one shader variant.
/// @param[in] cache_dir Shader-cache directory.
/// @param[in] key Expected shader variant identity.
/// @param[out] entry_name Optional destination for the validated entrypoint.
/// @return Validated metadata or an I/O or identity diagnostic.
[[nodiscard]] inline std::expected<shader_cache_metadata, std::string>
read_validated_cache_meta(const std::filesystem::path& cache_dir,
                          const shader_variant_key& key,
                          std::string* entry_name) {
    std::ifstream input(cache_meta_path(cache_dir, key), std::ios::binary);
    if (!input) {
        return std::unexpected("meta missing");
    }
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    input.seekg(0, std::ios::beg);
    if (size < static_cast<std::streamoff>(sizeof(shader_cache_metadata))) {
        return std::unexpected("meta too small");
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!input) {
        return std::unexpected("meta read failed");
    }
    return validated_cache_meta_from_bytes(bytes, key, entry_name);
}

/// @brief Requires a cache artifact to exist with a nonzero size.
/// @param[in] path Artifact path.
/// @param[in] artifact Human-readable artifact kind for diagnostics.
/// @return Success or a filesystem diagnostic.
[[nodiscard]] inline std::expected<void, std::string>
require_nonempty_cache_file(const std::filesystem::path& path, std::string_view artifact) {
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) {
        return std::unexpected(
            shader_cache_message(artifact, " stat failed for ", path.string(), ": ", ec.message()));
    }
    if (!exists) {
        return std::unexpected(shader_cache_message(artifact, " missing: ", path.string()));
    }
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        return std::unexpected(
            shader_cache_message(artifact, " size failed for ", path.string(), ": ", ec.message()));
    }
    if (size == 0) {
        return std::unexpected(shader_cache_message(artifact, " empty: ", path.string()));
    }
    return {};
}

/// @brief Validates metadata and Metal library presence for one variant.
/// @param[in] cache_dir Shader-cache directory.
/// @param[in] key Shader variant identity.
/// @return Success or the first cache-entry diagnostic.
[[nodiscard]] inline std::expected<void, std::string>
cache_metallib_entry_status(const std::filesystem::path& cache_dir, const shader_variant_key& key) {
    if (auto meta = read_validated_cache_meta(cache_dir, key, nullptr); !meta) {
        return std::unexpected(
            shader_cache_message("metadata cache-contract validation failed: ", meta.error()));
    }
    return require_nonempty_cache_file(cache_metallib_path(cache_dir, key), "metallib");
}

/// @brief Reports whether one variant has valid metadata and a Metal library.
/// @param[in] cache_dir Shader-cache directory.
/// @param[in] key Shader variant identity.
/// @return `true` when the Metal library entry is complete.
[[nodiscard]] inline bool cache_metallib_entry_complete(const std::filesystem::path& cache_dir,
                                                        const shader_variant_key& key) {
    return cache_metallib_entry_status(cache_dir, key).has_value();
}

namespace sealed_shader_cache_detail {

/// @brief Sealed seed manifest magic.
inline constexpr std::string_view k_manifest_magic = "BSCSEED4";
/// @brief Current sealed seed manifest wire revision.
inline constexpr std::uint32_t k_manifest_wire_revision = 4u;
/// @brief Maximum accepted serialized manifest size.
inline constexpr std::size_t k_maximum_manifest_bytes = 16u << 20u;
/// @brief Maximum accepted size of one seed artifact.
inline constexpr std::size_t k_maximum_seed_artifact_bytes = 1u << 30u;
/// @brief Maximum aggregate artifact bytes retained by one process seed.
inline constexpr std::uint64_t k_maximum_seed_image_bytes = 512ull << 20u;
/// @brief Maximum artifact count retained by one process seed.
inline constexpr std::size_t k_maximum_seed_image_artifacts = 16'384u;
/// @brief Maximum shader-key count encoded by a manifest.
inline constexpr std::uint32_t k_maximum_manifest_keys = 4096u;
/// @brief Maximum artifact count encoded by a manifest.
inline constexpr std::uint32_t k_maximum_manifest_artifacts = 65536u;
/// @brief Maximum bytes in one manifest artifact filename.
inline constexpr std::uint32_t k_maximum_manifest_filename_bytes = 512u;
/// @brief Test hook invoked immediately before final seal identity checks.
using seal_before_identity_check_hook = void (*)() noexcept;

/// @brief Canonical artifact identity derived from one sealed shader key.
struct expected_artifact {
    sealed_shader_cache_artifact_kind kind =
        sealed_shader_cache_artifact_kind::reserved_command_metallib;
    std::uint32_t key_index = (std::numeric_limits<std::uint32_t>::max)();
    std::string filename;
    std::filesystem::path path;
};

/// @brief Orders shader keys by every serialized identity selector.
/// @param[in] lhs First key.
/// @param[in] rhs Second key.
/// @return `true` when `lhs` precedes `rhs`.
[[nodiscard]] inline bool shader_key_less(const shader_variant_key& lhs,
                                          const shader_variant_key& rhs) noexcept {
    return std::tie(lhs.kind, lhs.tile_size, lhs.local_size, lhs.max_span) <
           std::tie(rhs.kind, rhs.tile_size, rhs.local_size, rhs.max_span);
}

/// @brief Validates, sorts, and deduplicates a sealed seed key set.
/// @param[in] requested Requested shader keys.
/// @return Canonical key order or a validation diagnostic.
[[nodiscard]] inline std::expected<std::vector<shader_variant_key>, std::string>
canonical_keys(std::span<const shader_variant_key> requested) {
    if (requested.empty()) {
        return std::unexpected("sealed shader cache key set is empty");
    }
    if (requested.size() > k_maximum_manifest_keys) {
        return std::unexpected(shader_cache_message("sealed shader cache key count exceeds ",
                                                    k_maximum_manifest_keys));
    }
    std::vector<shader_variant_key> keys(requested.begin(), requested.end());
    for (const shader_variant_key& key : keys) {
        if (!is_shader_variant_key_valid(key)) {
            return std::unexpected(shader_cache_message("sealed shader cache contains invalid key ",
                                                        cache_file_stem(key)));
        }
    }
    std::sort(keys.begin(), keys.end(), shader_key_less);
    if (std::adjacent_find(keys.begin(), keys.end()) != keys.end()) {
        return std::unexpected("sealed shader cache key set contains duplicates");
    }
    return keys;
}

/// @brief Validates the lowercase hexadecimal cache digest form.
/// @param[in] digest Candidate digest.
/// @return `true` for a sixteen-character lowercase hexadecimal digest.
[[nodiscard]] inline bool valid_digest(std::string_view digest) noexcept {
    return digest.size() == 16u && std::all_of(digest.begin(), digest.end(), [](char value) {
               return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
           });
}

/// @brief Validates a manifest filename as one bounded path component.
/// @param[in] filename Candidate artifact filename.
/// @return `true` when the filename cannot escape the cache directory.
[[nodiscard]] inline bool safe_filename(std::string_view filename) noexcept {
    return !filename.empty() && filename.size() <= k_maximum_manifest_filename_bytes &&
           filename != "." && filename != ".." && filename.find('/') == std::string_view::npos &&
           filename.find('\\') == std::string_view::npos &&
           filename.find('\0') == std::string_view::npos;
}

/// @brief Derives the complete ordered artifact set for canonical shader keys.
/// @param[in] cache_dir Shader-cache directory.
/// @param[in] canonical Canonical shader key order.
/// @return Expected artifacts or a filename or count diagnostic.
[[nodiscard]] inline std::expected<std::vector<expected_artifact>, std::string>
expected_artifacts(const std::filesystem::path& cache_dir,
                   std::span<const shader_variant_key> canonical) {
    std::vector<expected_artifact> artifacts;
    artifacts.reserve(3u + canonical.size() * 6u);
    // Adds one artifact using its filename as the manifest path component.
    const auto append = [&artifacts](sealed_shader_cache_artifact_kind kind,
                                     std::uint32_t key_index,
                                     const std::filesystem::path& path) {
        artifacts.push_back(expected_artifact{
            .kind = kind,
            .key_index = key_index,
            .filename = path.filename().string(),
            .path = path,
        });
    };
    append(sealed_shader_cache_artifact_kind::moltenvk_command_library,
           (std::numeric_limits<std::uint32_t>::max)(),
           cache_moltenvk_command_library_path(cache_dir));
    for (std::size_t index = 0; index < canonical.size(); ++index) {
        if (index > (std::numeric_limits<std::uint32_t>::max)()) {
            return std::unexpected("sealed shader cache key index overflow");
        }
        const std::uint32_t key_index = static_cast<std::uint32_t>(index);
        append(sealed_shader_cache_artifact_kind::shader_meta,
               key_index,
               cache_meta_path(cache_dir, canonical[index]));
        append(sealed_shader_cache_artifact_kind::shader_metallib,
               key_index,
               cache_metallib_path(cache_dir, canonical[index]));
        for (const std::filesystem::path& archive :
             cache_binary_archive_paths(cache_dir, canonical[index])) {
            append(sealed_shader_cache_artifact_kind::metal_binary_archive, key_index, archive);
            append(sealed_shader_cache_artifact_kind::metal_binary_archive_qualification,
                   key_index,
                   cache_binary_archive_qualification_path(archive));
        }
    }
    if (artifacts.size() > k_maximum_manifest_artifacts) {
        return std::unexpected(shader_cache_message("sealed shader cache artifact count exceeds ",
                                                    k_maximum_manifest_artifacts));
    }
    for (const expected_artifact& artifact : artifacts) {
        if (!safe_filename(artifact.filename)) {
            return std::unexpected(shader_cache_message("unsafe generated shader cache filename: ",
                                                        artifact.filename));
        }
    }
    for (std::size_t index = 0u; index < artifacts.size(); ++index) {
        const auto duplicate =
            std::find_if(artifacts.begin() + static_cast<std::ptrdiff_t>(index + 1u),
                         artifacts.end(),
                         [&artifacts, index](const expected_artifact& candidate) {
                             return candidate.filename == artifacts[index].filename;
                         });
        if (duplicate != artifacts.end()) {
            return std::unexpected(shader_cache_message(
                "duplicate generated shader cache filename: ", artifacts[index].filename));
        }
    }
    return artifacts;
}

/// @brief Reads one bounded regular file without following symbolic links.
/// @param[in] path Artifact path.
/// @param[in] artifact Human-readable artifact kind for diagnostics.
/// @param[in] maximum_bytes Maximum accepted file size.
/// @param[in] exact_bytes Optional exact accepted file size.
/// @return Complete file bytes or an identity, size, or I/O diagnostic.
[[nodiscard]] inline std::expected<std::vector<std::byte>, std::string>
read_regular_file(const std::filesystem::path& path,
                  std::string_view artifact,
                  std::size_t maximum_bytes = (std::numeric_limits<std::size_t>::max)(),
                  std::optional<std::uint64_t> exact_bytes = std::nullopt) {
    std::error_code ec;
    const std::filesystem::file_status status = std::filesystem::symlink_status(path, ec);
    if (ec) {
        return std::unexpected(
            shader_cache_message(artifact, " stat failed for ", path.string(), ": ", ec.message()));
    }
    if (std::filesystem::is_symlink(status)) {
        return std::unexpected(
            shader_cache_message(artifact, " must not be a symlink: ", path.string()));
    }
    if (!std::filesystem::is_regular_file(status)) {
        return std::unexpected(
            shader_cache_message(artifact, " is not a regular file: ", path.string()));
    }
    const std::uintmax_t file_bytes = std::filesystem::file_size(path, ec);
    if (ec) {
        return std::unexpected(
            shader_cache_message(artifact, " size failed for ", path.string(), ": ", ec.message()));
    }
    if (file_bytes == 0u) {
        return std::unexpected(shader_cache_message(artifact, " is empty: ", path.string()));
    }
    if (exact_bytes && file_bytes != *exact_bytes) {
        return std::unexpected(shader_cache_message(artifact,
                                                    " size changed before admitted read: ",
                                                    path.string(),
                                                    " actual=",
                                                    file_bytes,
                                                    " expected=",
                                                    *exact_bytes));
    }
    if (file_bytes > maximum_bytes || file_bytes > (std::numeric_limits<std::size_t>::max)() ||
        file_bytes > static_cast<std::uintmax_t>((std::numeric_limits<std::streamsize>::max)())) {
        return std::unexpected(shader_cache_message(
            artifact, " is too large: ", path.string(), " bytes=", file_bytes));
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::unexpected(shader_cache_message(artifact, " open failed: ", path.string()));
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(file_bytes));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input || static_cast<std::size_t>(input.gcount()) != bytes.size()) {
        return std::unexpected(shader_cache_message(artifact, " short read: ", path.string()));
    }
    return bytes;
}

/// @brief Stable filesystem identity captured before and after an artifact read.
struct regular_file_identity {
    std::uint64_t device = 0u;
    std::uint64_t inode = 0u;
    std::uint64_t size = 0u;
    std::int64_t modified_seconds = 0;
    std::int64_t modified_nanoseconds = 0;
    std::int64_t changed_seconds = 0;
    std::int64_t changed_nanoseconds = 0;

    /// @brief Compares every stable file-identity field.
    [[nodiscard]] bool operator==(const regular_file_identity&) const noexcept = default;
};

/// @brief File bytes paired with the identity observed after a stable read.
struct stable_regular_file_image {
    std::vector<std::byte> bytes;
    regular_file_identity identity{};
};

/// @brief Owns one descriptor used for stable artifact reads.
class scoped_file_descriptor {
  public:
    /// @brief Adopts one open descriptor.
    /// @param[in] fd Descriptor closed on destruction unless closed earlier.
    explicit scoped_file_descriptor(int fd) noexcept : fd_(fd) {}
    /// @brief Prevents copying descriptor ownership.
    scoped_file_descriptor(const scoped_file_descriptor&) = delete;
    /// @brief Prevents copy assignment of descriptor ownership.
    scoped_file_descriptor& operator=(const scoped_file_descriptor&) = delete;
    /// @brief Closes the descriptor when ownership remains.
    ~scoped_file_descriptor() {
        if (fd_ >= 0)
            (void)::close(fd_);
    }
    /// @brief Closes the descriptor and relinquishes ownership.
    /// @return Result from `close`, or zero when already closed.
    [[nodiscard]] int close_now() noexcept {
        const int fd = std::exchange(fd_, -1);
        return fd >= 0 ? ::close(fd) : 0;
    }

  private:
    int fd_ = -1;
};

/// @brief Converts platform stat fields to the canonical file identity.
/// @param[in] value Platform stat record.
/// @return Canonical regular-file identity.
[[nodiscard]] inline regular_file_identity
regular_file_identity_from_stat(const struct stat& value) noexcept {
    regular_file_identity result{
        .device = static_cast<std::uint64_t>(value.st_dev),
        .inode = static_cast<std::uint64_t>(value.st_ino),
        .size = static_cast<std::uint64_t>(value.st_size),
    };
#if defined(__APPLE__)
    result.modified_seconds = value.st_mtimespec.tv_sec;
    result.modified_nanoseconds = value.st_mtimespec.tv_nsec;
    result.changed_seconds = value.st_ctimespec.tv_sec;
    result.changed_nanoseconds = value.st_ctimespec.tv_nsec;
#else
    result.modified_seconds = value.st_mtim.tv_sec;
    result.modified_nanoseconds = value.st_mtim.tv_nsec;
    result.changed_seconds = value.st_ctim.tv_sec;
    result.changed_nanoseconds = value.st_ctim.tv_nsec;
#endif
    return result;
}

/// @brief Reads exact bytes while requiring an unchanged regular-file identity.
/// @param[in] path Artifact path.
/// @param[in] artifact Human-readable artifact kind for diagnostics.
/// @param[in] exact_bytes Required file size.
/// @param[in] maximum_bytes Maximum admitted file size.
/// @return Stable file image or an identity, size, or I/O diagnostic.
[[nodiscard]] inline std::expected<stable_regular_file_image, std::string>
read_stable_regular_file(const std::filesystem::path& path,
                         std::string_view artifact,
                         std::uint64_t exact_bytes,
                         std::uint64_t maximum_bytes) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return std::unexpected(shader_cache_message(
            artifact, " stable open failed for ", path.string(), ": errno ", errno));
    }
    scoped_file_descriptor fd_owner{fd};
    const auto fail =
        [&path, artifact](
            std::string_view reason) -> std::expected<stable_regular_file_image, std::string> {
        const int saved = errno;
        return std::unexpected(
            shader_cache_message(artifact, ' ', reason, " for ", path.string(), ": errno ", saved));
    };
    struct stat before{};
    if (::fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) || before.st_size <= 0 ||
        static_cast<std::uint64_t>(before.st_size) != exact_bytes || exact_bytes > maximum_bytes ||
        exact_bytes > (std::numeric_limits<std::size_t>::max)()) {
        return fail("stable pre-read identity invalid");
    }
    stable_regular_file_image image{};
    image.bytes.resize(static_cast<std::size_t>(exact_bytes));
    std::size_t consumed = 0u;
    while (consumed < image.bytes.size()) {
        const ssize_t count =
            ::read(fd, image.bytes.data() + consumed, image.bytes.size() - consumed);
        if (count > 0) {
            consumed += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        return fail("stable read failed");
    }
    struct stat after{};
    if (::fstat(fd, &after) != 0 ||
        regular_file_identity_from_stat(before) != regular_file_identity_from_stat(after)) {
        return fail("identity changed during stable read");
    }
    if (fd_owner.close_now() != 0) {
        return std::unexpected(shader_cache_message(
            artifact, " stable close failed for ", path.string(), ": errno ", errno));
    }
    image.identity = regular_file_identity_from_stat(after);
    return image;
}

/// @brief Compares the current regular-file identity with an accepted snapshot.
/// @param[in] path Artifact path.
/// @param[in] expected Accepted identity.
/// @return `true` when the current identity matches exactly.
[[nodiscard]] inline bool
same_regular_file_identity(const std::filesystem::path& path,
                           const regular_file_identity& expected) noexcept {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return false;
    struct stat current{};
    const bool accepted = ::fstat(fd, &current) == 0 && S_ISREG(current.st_mode) &&
                          regular_file_identity_from_stat(current) == expected;
    (void)::close(fd);
    return accepted;
}

/// @brief Streamed file size, digest, and stable identity.
struct stable_regular_file_digest {
    std::uint64_t size = 0u;
    std::array<std::uint8_t, 32> sha256{};
    regular_file_identity identity{};
};

/// @brief Streams and hashes a bounded regular file under identity checks.
/// @param[in] path Artifact path.
/// @param[in] artifact Human-readable artifact kind for diagnostics.
/// @param[in] maximum_bytes Maximum accepted file size.
/// @return Stable digest record or an identity, size, or I/O diagnostic.
[[nodiscard]] inline std::expected<stable_regular_file_digest, std::string>
stream_stable_regular_file_digest(const std::filesystem::path& path,
                                  std::string_view artifact,
                                  std::uint64_t maximum_bytes) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return std::unexpected(shader_cache_message(
            artifact, " stream open failed for ", path.string(), ": errno ", errno));
    }
    scoped_file_descriptor fd_owner{fd};
    struct stat before{};
    if (::fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) || before.st_size <= 0 ||
        static_cast<std::uint64_t>(before.st_size) > maximum_bytes) {
        return std::unexpected(shader_cache_message(
            artifact, " stream pre-read identity invalid for ", path.string()));
    }
    constexpr std::size_t k_scratch_bytes = 64u << 10u;
    std::array<std::byte, k_scratch_bytes> scratch{};
    sha256_hasher hasher;
    std::uint64_t streamed = 0u;
    for (;;) {
        const ssize_t count = ::read(fd, scratch.data(), scratch.size());
        if (count > 0) {
            const auto chunk = static_cast<std::size_t>(count);
            if (chunk > maximum_bytes - streamed) {
                return std::unexpected(shader_cache_message(
                    artifact, " grew beyond stream bound for ", path.string()));
            }
            hasher.update(std::span<const std::byte>{scratch.data(), chunk});
            streamed += chunk;
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0) {
            return std::unexpected(shader_cache_message(
                artifact, " stream read failed for ", path.string(), ": errno ", errno));
        }
        break;
    }
    struct stat after{};
    if (::fstat(fd, &after) != 0 ||
        regular_file_identity_from_stat(before) != regular_file_identity_from_stat(after) ||
        streamed != static_cast<std::uint64_t>(after.st_size)) {
        return std::unexpected(
            shader_cache_message(artifact, " identity changed during stream for ", path.string()));
    }
    if (fd_owner.close_now() != 0) {
        return std::unexpected(shader_cache_message(
            artifact, " stream close failed for ", path.string(), ": errno ", errno));
    }
    return stable_regular_file_digest{
        .size = streamed,
        .sha256 = hasher.finalize(),
        .identity = regular_file_identity_from_stat(after),
    };
}

/// @brief Computes SHA-256 for one in-memory byte range.
/// @param[in] bytes Bytes to hash.
/// @return SHA-256 digest.
[[nodiscard]] inline std::array<std::uint8_t, 32>
sha256(std::span<const std::byte> bytes) noexcept {
    sha256_hasher hasher;
    hasher.update(bytes);
    return hasher.finalize();
}

/// @brief Computes the canonical sealed seed digest and aggregate artifact size.
/// @param[in] cache_digest Cache-build digest.
/// @param[in] keys Canonical shader keys.
/// @param[in] artifacts Canonical artifact descriptors.
/// @return Seed digest and byte size, or an overflow diagnostic.
[[nodiscard]] inline std::expected<std::pair<std::array<std::uint8_t, 32>, std::uint64_t>,
                                   std::string>
seed_identity(std::string_view cache_digest,
              std::span<const shader_variant_key> keys,
              std::span<const sealed_shader_cache_artifact> artifacts) {
    sha256_hasher hasher;
    hasher.update("bioseq-sealed-shader-seed-contract-v4-command-library");
    hasher.update_little_endian(cache_digest.size());
    hasher.update(cache_digest);
    hasher.update_little_endian(keys.size());
    for (const shader_variant_key& key : keys) {
        hasher.update_little_endian(key.kind);
        hasher.update_little_endian(key.tile_size);
        hasher.update_little_endian(key.local_size);
        hasher.update_little_endian(key.max_span);
    }
    hasher.update_little_endian(artifacts.size());
    std::uint64_t seed_size = 0u;
    for (const sealed_shader_cache_artifact& artifact : artifacts) {
        if (artifact.size > (std::numeric_limits<std::uint64_t>::max)() - seed_size) {
            return std::unexpected("sealed shader cache seed size overflow");
        }
        seed_size += artifact.size;
        hasher.update_little_endian(static_cast<std::uint32_t>(artifact.kind));
        hasher.update_little_endian(artifact.key_index);
        hasher.update_little_endian(artifact.filename.size());
        hasher.update(artifact.filename);
        hasher.update_little_endian(artifact.size);
        hasher.update(std::as_bytes(std::span{artifact.sha256}));
    }
    return std::pair{hasher.finalize(), seed_size};
}

/// @brief Appends one 32-bit value in canonical little-endian order.
inline void append_u32(std::vector<std::byte>& bytes, std::uint32_t value) {
    for (unsigned shift = 0u; shift < 32u; shift += 8u) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
    }
}

/// @brief Appends one 64-bit value in canonical little-endian order.
inline void append_u64(std::vector<std::byte>& bytes, std::uint64_t value) {
    for (unsigned shift = 0u; shift < 64u; shift += 8u) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
    }
}

/// @brief Appends text bytes without a terminator.
inline void append_text(std::vector<std::byte>& bytes, std::string_view text) {
    const auto view = std::as_bytes(std::span{text.data(), text.size()});
    bytes.insert(bytes.end(), view.begin(), view.end());
}

/// @brief Appends one fixed SHA-256 digest.
inline void append_digest(std::vector<std::byte>& bytes,
                          const std::array<std::uint8_t, 32>& digest) {
    const auto view = std::as_bytes(std::span{digest});
    bytes.insert(bytes.end(), view.begin(), view.end());
}

/// @brief Bounds-checked little-endian reader for sealed cache records.
class manifest_reader {
  private:
    std::span<const std::byte> bytes_;
    std::size_t cursor_ = 0u;

  public:
    /// @brief Starts a reader over borrowed manifest bytes.
    /// @param[in] bytes Storage that must outlive the reader.
    explicit manifest_reader(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

    /// @brief Returns the next unread byte offset.
    [[nodiscard]] std::size_t cursor() const noexcept {
        return cursor_;
    }
    /// @brief Reports whether all bytes were consumed.
    [[nodiscard]] bool at_end() const noexcept {
        return cursor_ == bytes_.size();
    }

    /// @brief Consumes one bounded byte range.
    /// @param[in] count Number of bytes to consume.
    /// @return Borrowed subspan or a truncation diagnostic.
    [[nodiscard]] std::expected<std::span<const std::byte>, std::string> take(std::size_t count) {
        if (count > bytes_.size() - cursor_) {
            return std::unexpected("sealed shader cache manifest is truncated");
        }
        const auto result = bytes_.subspan(cursor_, count);
        cursor_ += count;
        return result;
    }

    /// @brief Reads one little-endian 32-bit value.
    [[nodiscard]] std::expected<std::uint32_t, std::string> u32() {
        auto raw = take(4u);
        if (!raw)
            return std::unexpected(raw.error());
        std::uint32_t value = 0u;
        for (std::size_t index = 0; index < raw->size(); ++index) {
            value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>((*raw)[index]))
                     << (index * 8u);
        }
        return value;
    }

    /// @brief Reads one little-endian 64-bit value.
    [[nodiscard]] std::expected<std::uint64_t, std::string> u64() {
        auto raw = take(8u);
        if (!raw)
            return std::unexpected(raw.error());
        std::uint64_t value = 0u;
        for (std::size_t index = 0; index < raw->size(); ++index) {
            value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>((*raw)[index]))
                     << (index * 8u);
        }
        return value;
    }

    /// @brief Reads an owned text field of the requested length.
    /// @param[in] count Number of text bytes.
    [[nodiscard]] std::expected<std::string, std::string> text(std::size_t count) {
        auto raw = take(count);
        if (!raw)
            return std::unexpected(raw.error());
        return std::string(reinterpret_cast<const char*>(raw->data()), raw->size());
    }

    /// @brief Reads one fixed 32-byte SHA-256 digest.
    [[nodiscard]] std::expected<std::array<std::uint8_t, 32>, std::string> digest() {
        auto raw = take(32u);
        if (!raw)
            return std::unexpected(raw.error());
        std::array<std::uint8_t, 32> result{};
        std::memcpy(result.data(), raw->data(), result.size());
        return result;
    }
};

} // namespace sealed_shader_cache_detail

} // namespace bioseq::gpu::vulkan
