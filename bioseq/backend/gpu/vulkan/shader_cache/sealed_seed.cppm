/**
 * @file sealed_seed.cppm
 * @brief Owns sealed shader-seed serialization, validation, admission, and publication.
 */
module;

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

export module bioseq.gpu.vulkan.shader_cache:sealed_seed;

import :paths_metadata;
import :subprocess;
import :archive_helper;
import bioseq.common.checked_arithmetic;
import bioseq.parallelism.memory_admission;
import std;

export namespace bioseq::gpu::vulkan {

namespace sealed_shader_cache_detail {

/// @brief Serializes a sealed shader manifest and computes its checksum.
/// @param[in] manifest Manifest to serialize.
/// @return Serialized bytes and checksum, or a contract diagnostic.
[[nodiscard]] inline std::expected<std::pair<std::vector<std::byte>, std::uint64_t>, std::string>
serialize_manifest(const sealed_shader_cache_manifest& manifest) {
    if (!valid_digest(manifest.cache_digest)) {
        return std::unexpected("sealed shader cache digest is not lowercase hex16");
    }
    if (manifest.keys.empty() || manifest.keys.size() > k_maximum_manifest_keys ||
        manifest.artifacts.empty() || manifest.artifacts.size() > k_maximum_manifest_artifacts) {
        return std::unexpected("sealed shader cache manifest count is invalid");
    }
    std::vector<std::byte> bytes;
    bytes.reserve(256u + manifest.artifacts.size() * 128u);
    append_text(bytes, k_manifest_magic);
    append_u32(bytes, k_manifest_wire_revision);
    append_u32(bytes, 0u);
    append_u32(bytes, static_cast<std::uint32_t>(manifest.cache_digest.size()));
    append_u32(bytes, static_cast<std::uint32_t>(manifest.keys.size()));
    append_u32(bytes, static_cast<std::uint32_t>(manifest.artifacts.size()));
    append_u32(bytes, 0u);
    append_text(bytes, manifest.cache_digest);
    for (const shader_variant_key& key : manifest.keys) {
        append_u32(bytes, key.kind);
        append_u32(bytes, key.tile_size);
        append_u32(bytes, key.local_size);
        append_u32(bytes, key.max_span);
    }
    for (const sealed_shader_cache_artifact& artifact : manifest.artifacts) {
        if (!safe_filename(artifact.filename)) {
            return std::unexpected(
                shader_cache_message("unsafe shader cache manifest filename: ", artifact.filename));
        }
        append_u32(bytes, static_cast<std::uint32_t>(artifact.kind));
        append_u32(bytes, artifact.key_index);
        append_u32(bytes, static_cast<std::uint32_t>(artifact.filename.size()));
        append_u32(bytes, 0u);
        append_u64(bytes, artifact.size);
        append_digest(bytes, artifact.sha256);
        append_text(bytes, artifact.filename);
    }
    append_u64(bytes, manifest.seed_size);
    append_digest(bytes, manifest.seed_sha256);
    const std::uint64_t checksum = manifest_checksum(bytes);
    append_u64(bytes, checksum);
    if (bytes.size() > k_maximum_manifest_bytes) {
        return std::unexpected("sealed shader cache manifest is too large");
    }
    return std::pair{std::move(bytes), checksum};
}

/// @brief Parses and validates a sealed shader-cache manifest.
/// @param[in] bytes Serialized manifest bytes.
/// @return Validated manifest or a wire-format diagnostic.
[[nodiscard]] inline std::expected<sealed_shader_cache_manifest, std::string>
parse_manifest(std::span<const std::byte> bytes) {
    if (bytes.size() > k_maximum_manifest_bytes) {
        return std::unexpected("sealed shader cache manifest is too large");
    }
    if (bytes.size() < k_manifest_magic.size() + 6u * sizeof(std::uint32_t) + 16u +
                           sizeof(std::uint64_t) + 32u + sizeof(std::uint64_t)) {
        return std::unexpected("sealed shader cache manifest is too small");
    }
    manifest_reader reader(bytes);
    auto magic = reader.text(k_manifest_magic.size());
    if (!magic || *magic != k_manifest_magic) {
        return std::unexpected("sealed shader cache manifest magic mismatch");
    }
    auto wire_revision = reader.u32();
    auto reserved0 = reader.u32();
    auto digest_size = reader.u32();
    auto key_count = reader.u32();
    auto artifact_count = reader.u32();
    auto reserved1 = reader.u32();
    if (!wire_revision || !reserved0 || !digest_size || !key_count || !artifact_count ||
        !reserved1) {
        return std::unexpected("sealed shader cache manifest header is truncated");
    }
    if (*wire_revision != k_manifest_wire_revision || *reserved0 != 0u || *reserved1 != 0u) {
        return std::unexpected("sealed shader cache manifest serialized contract mismatch");
    }
    if (*digest_size != 16u || *key_count == 0u || *key_count > k_maximum_manifest_keys ||
        *artifact_count == 0u || *artifact_count > k_maximum_manifest_artifacts) {
        return std::unexpected("sealed shader cache manifest header count is invalid");
    }

    sealed_shader_cache_manifest manifest{};
    auto cache_digest = reader.text(*digest_size);
    if (!cache_digest || !valid_digest(*cache_digest)) {
        return std::unexpected("sealed shader cache manifest digest is invalid");
    }
    manifest.cache_digest = std::move(*cache_digest);
    manifest.keys.reserve(*key_count);
    for (std::uint32_t index = 0u; index < *key_count; ++index) {
        auto kind = reader.u32();
        auto tile_size = reader.u32();
        auto local_size = reader.u32();
        auto max_span = reader.u32();
        if (!kind || !tile_size || !local_size || !max_span) {
            return std::unexpected("sealed shader cache key table is truncated");
        }
        manifest.keys.push_back(shader_variant_key{
            .kind = *kind,
            .tile_size = *tile_size,
            .local_size = *local_size,
            .max_span = *max_span,
        });
    }
    manifest.artifacts.reserve(*artifact_count);
    for (std::uint32_t index = 0u; index < *artifact_count; ++index) {
        auto raw_kind = reader.u32();
        auto key_index = reader.u32();
        auto filename_size = reader.u32();
        auto reserved = reader.u32();
        auto size = reader.u64();
        auto digest = reader.digest();
        if (!raw_kind || !key_index || !filename_size || !reserved || !size || !digest) {
            return std::unexpected("sealed shader cache artifact table is truncated");
        }
        if (*raw_kind >
                static_cast<std::uint32_t>(
                    sealed_shader_cache_artifact_kind::moltenvk_command_library) ||
            *filename_size == 0u || *filename_size > k_maximum_manifest_filename_bytes ||
            *reserved != 0u || *size == 0u) {
            return std::unexpected("sealed shader cache artifact record is invalid");
        }
        auto filename = reader.text(*filename_size);
        if (!filename || !safe_filename(*filename)) {
            return std::unexpected("sealed shader cache artifact filename is unsafe");
        }
        manifest.artifacts.push_back(sealed_shader_cache_artifact{
            .kind = static_cast<sealed_shader_cache_artifact_kind>(*raw_kind),
            .key_index = *key_index,
            .filename = std::move(*filename),
            .size = *size,
            .sha256 = *digest,
        });
    }
    auto seed_size = reader.u64();
    auto seed_sha256 = reader.digest();
    if (!seed_size || !seed_sha256) {
        return std::unexpected("sealed shader cache seed identity is truncated");
    }
    manifest.seed_size = *seed_size;
    manifest.seed_sha256 = *seed_sha256;
    const std::size_t checksum_offset = reader.cursor();
    auto checksum = reader.u64();
    if (!checksum || !reader.at_end()) {
        return std::unexpected("sealed shader cache manifest has trailing/truncated data");
    }
    manifest.manifest_checksum = *checksum;
    const std::uint64_t computed = manifest_checksum(bytes.first(checksum_offset));
    if (*checksum == 0u || *checksum != computed) {
        return std::unexpected("sealed shader cache manifest checksum mismatch");
    }
    return manifest;
}

[[nodiscard]] inline std::expected<std::vector<std::byte>, std::string>
load_validated_artifact_bytes(const std::filesystem::path& cache_dir,
                              const sealed_shader_cache_artifact& artifact);

/// @brief Validates manifest digest, key membership, artifact order, and seed identity.
/// @param[in] cache_dir Shader-cache directory.
/// @param[in] manifest Parsed manifest.
/// @param[in] expected_digest Expected cache digest.
/// @param[in] expected_keys Expected shader key membership.
/// @return Success or the first manifest-shape diagnostic.
[[nodiscard]] inline std::expected<void, std::string>
validate_shape(const std::filesystem::path& cache_dir,
               const sealed_shader_cache_manifest& manifest,
               std::string_view expected_digest,
               std::span<const shader_variant_key> expected_keys) {
    if (!valid_digest(expected_digest) || manifest.cache_digest != expected_digest) {
        return std::unexpected(
            shader_cache_message("sealed shader cache digest mismatch: manifest=",
                                 manifest.cache_digest,
                                 " expected=",
                                 expected_digest));
    }
    auto canonical = canonical_keys(expected_keys);
    if (!canonical)
        return std::unexpected(canonical.error());
    if (manifest.keys != *canonical) {
        return std::unexpected("sealed shader cache key membership mismatch");
    }
    auto expected = expected_artifacts(cache_dir, *canonical);
    if (!expected)
        return std::unexpected(expected.error());
    if (manifest.artifacts.size() != expected->size()) {
        return std::unexpected(shader_cache_message(
            "sealed shader cache artifact membership count mismatch: manifest=",
            manifest.artifacts.size(),
            " expected=",
            expected->size()));
    }
    for (std::size_t index = 0u; index < expected->size(); ++index) {
        const sealed_shader_cache_artifact& actual = manifest.artifacts[index];
        const expected_artifact& wanted = (*expected)[index];
        if (actual.kind != wanted.kind || actual.key_index != wanted.key_index ||
            actual.filename != wanted.filename) {
            return std::unexpected(shader_cache_message(
                "sealed shader cache artifact membership mismatch at ordinal ", index));
        }
    }
    const auto command_artifact = std::find_if(
        manifest.artifacts.begin(),
        manifest.artifacts.end(),
        [](const sealed_shader_cache_artifact& artifact) {
            return artifact.kind ==
                       sealed_shader_cache_artifact_kind::moltenvk_command_library &&
                   artifact.key_index == (std::numeric_limits<std::uint32_t>::max)() &&
                   artifact.filename == "moltenvk_command.sealed";
        });
    if (command_artifact == manifest.artifacts.end())
        return std::unexpected("sealed shader cache has no exact MoltenVK command library");
    auto command_bytes = load_validated_artifact_bytes(cache_dir, *command_artifact);
    if (!command_bytes)
        return std::unexpected(command_bytes.error());
    auto command_bundle = parse_moltenvk_command_library_bundle(*command_bytes);
    if (!command_bundle || !command_bundle->qualification.is_production_accepted()) {
        return std::unexpected(shader_cache_message(
            "sealed MoltenVK command library is invalid: ",
            command_bundle ? "not production-qualified" : command_bundle.error()));
    }
#if defined(__APPLE__)
    if (auto current = validate_moltenvk_command_receipt_against_process_helper(
            command_bundle->qualification);
        !current) {
        return std::unexpected(current.error());
    }
#endif
    auto seed = seed_identity(manifest.cache_digest, manifest.keys, manifest.artifacts);
    if (!seed)
        return std::unexpected(seed.error());
    if (manifest.seed_sha256 != seed->first || manifest.seed_size != seed->second) {
        return std::unexpected("sealed shader cache seed identity mismatch");
    }
    return {};
}

/// @brief Validates artifact bytes against their sealed size and SHA-256.
/// @param[in] artifact Sealed artifact descriptor.
/// @param[in] bytes Candidate artifact bytes.
/// @return Success or a size or digest diagnostic.
[[nodiscard]] inline std::expected<void, std::string>
validate_artifact_bytes(const sealed_shader_cache_artifact& artifact,
                        std::span<const std::byte> bytes) {
    if (bytes.size() != artifact.size) {
        return std::unexpected(
            shader_cache_message("sealed shader cache artifact size mismatch for ",
                                 artifact.filename,
                                 ": actual=",
                                 bytes.size(),
                                 " manifest=",
                                 artifact.size));
    }
    if (sha256(bytes) != artifact.sha256) {
        return std::unexpected(shader_cache_message(
            "sealed shader cache artifact SHA-256 mismatch for ", artifact.filename));
    }
    return {};
}

/// @brief Reads and validates one artifact from a sealed cache directory.
/// @param[in] cache_dir Shader-cache directory.
/// @param[in] artifact Sealed artifact descriptor.
/// @return Validated bytes or an identity, size, digest, or I/O diagnostic.
[[nodiscard]] inline std::expected<std::vector<std::byte>, std::string>
load_validated_artifact_bytes(const std::filesystem::path& cache_dir,
                              const sealed_shader_cache_artifact& artifact) {
    auto bytes = read_regular_file(cache_dir / artifact.filename,
                                   "sealed shader cache artifact",
                                   k_maximum_seed_artifact_bytes);
    if (!bytes)
        return std::unexpected(bytes.error());
    if (auto valid = validate_artifact_bytes(artifact, *bytes); !valid) {
        return std::unexpected(valid.error());
    }
    return bytes;
}

/// @brief Validates one on-disk artifact against its sealed descriptor.
/// @param[in] cache_dir Shader-cache directory.
/// @param[in] artifact Sealed artifact descriptor.
/// @return Success or the artifact diagnostic.
[[nodiscard]] inline std::expected<void, std::string>
validate_artifact(const std::filesystem::path& cache_dir,
                  const sealed_shader_cache_artifact& artifact) {
    auto bytes = load_validated_artifact_bytes(cache_dir, artifact);
    if (!bytes)
        return std::unexpected(bytes.error());
    return {};
}

/// @brief Validates all seed artifacts and cross-artifact qualification bindings.
/// @param[in] cache_dir Shader-cache directory.
/// @param[in] manifest Parsed sealed manifest.
/// @return Success or the first artifact or qualification diagnostic.
[[nodiscard]] inline std::expected<void, std::string>
validate_all_artifacts(const std::filesystem::path& cache_dir,
                       const sealed_shader_cache_manifest& manifest) {
    for (const sealed_shader_cache_artifact& artifact : manifest.artifacts) {
        if (auto valid = validate_artifact(cache_dir, artifact); !valid) {
            return valid;
        }
    }
    const auto command_artifact = std::find_if(
        manifest.artifacts.begin(),
        manifest.artifacts.end(),
        [](const sealed_shader_cache_artifact& artifact) {
            return artifact.kind ==
                   sealed_shader_cache_artifact_kind::moltenvk_command_library;
        });
    if (command_artifact == manifest.artifacts.end())
        return std::unexpected("sealed shader cache has no command-library artifact");
    auto command_bytes = load_validated_artifact_bytes(cache_dir, *command_artifact);
    if (!command_bytes)
        return std::unexpected(command_bytes.error());
    auto command_bundle = parse_moltenvk_command_library_bundle(*command_bytes);
    if (!command_bundle || !command_bundle->qualification.is_production_accepted()) {
        return std::unexpected("sealed command-library qualification is invalid");
    }
    for (const shader_variant_key& key : manifest.keys) {
        if (auto status = cache_entry_status(cache_dir, key); !status) {
            return std::unexpected(
                shader_cache_message("sealed shader cache entry violates the cache contract for ",
                                     cache_file_stem(key),
                                     ": ",
                                     status.error()));
        }
    }
    std::optional<metal_archive_negative_error_fingerprint> common_negative_error;
    using helper_identity = std::tuple<std::uint32_t,
                                       std::string,
                                       std::string,
                                       std::uint64_t,
                                       std::array<std::uint8_t, 32>,
                                       std::uint64_t>;
    std::optional<helper_identity> common_helper;
    for (const sealed_shader_cache_artifact& archive : manifest.artifacts) {
        if (archive.kind != sealed_shader_cache_artifact_kind::metal_binary_archive) {
            continue;
        }
        const std::string qualification_filename = archive.filename + ".qualification";
        const auto qualification_artifact = std::find_if(
            manifest.artifacts.begin(),
            manifest.artifacts.end(),
            [&archive, &qualification_filename](const sealed_shader_cache_artifact& candidate) {
                return candidate.kind ==
                           sealed_shader_cache_artifact_kind::metal_binary_archive_qualification &&
                       candidate.key_index == archive.key_index &&
                       candidate.filename == qualification_filename;
            });
        if (qualification_artifact == manifest.artifacts.end()) {
            return std::unexpected(
                shader_cache_message("Metal archive qualification missing for ", archive.filename));
        }
        auto qualification_bytes =
            load_validated_artifact_bytes(cache_dir, *qualification_artifact);
        if (!qualification_bytes) {
            return std::unexpected(qualification_bytes.error());
        }
        auto qualification = parse_metal_archive_qualification_receipt(*qualification_bytes);
        if (!qualification) {
            return std::unexpected(shader_cache_message("Metal archive qualification invalid for ",
                                                        archive.filename,
                                                        ": ",
                                                        qualification.error()));
        }
        if (!qualification->is_production_accepted()) {
            return std::unexpected(shader_cache_message(
                "Metal archive qualification is not production-qualified for ", archive.filename));
        }
#if defined(__APPLE__)
        if (auto current = validate_metal_archive_receipt_against_process_helper(*qualification);
            !current) {
            return std::unexpected(
                shader_cache_message("Metal archive qualification helper mismatch for ",
                                     archive.filename,
                                     ": ",
                                     current.error()));
        }
#endif
        if (qualification->archive_size != archive.size ||
            qualification->archive_sha256 != archive.sha256) {
            return std::unexpected(shader_cache_message(
                "Metal archive qualification is bound to different bytes for ", archive.filename));
        }
        if (archive.key_index >= manifest.keys.size()) {
            return std::unexpected("Metal archive qualification key index is out of range");
        }
        const shader_variant_key& key = manifest.keys[archive.key_index];
        const std::string stem = cache_file_stem(key);
        const auto metallib_artifact = std::find_if(
            manifest.artifacts.begin(),
            manifest.artifacts.end(),
            [archive_key = archive.key_index,
             &stem](const sealed_shader_cache_artifact& candidate) {
                return candidate.kind == sealed_shader_cache_artifact_kind::shader_metallib &&
                       candidate.key_index == archive_key &&
                       candidate.filename == stem + ".metallib";
            });
        std::string expected_entry_name;
        auto meta = read_validated_cache_meta(cache_dir, key, &expected_entry_name);
        if (metallib_artifact == manifest.artifacts.end() || !meta ||
            qualification->shader_key != key ||
            qualification->metallib_size != metallib_artifact->size ||
            qualification->metallib_sha256 != metallib_artifact->sha256 ||
            qualification->entry_name != expected_entry_name) {
            return std::unexpected(shader_cache_message(
                "Metal archive qualification key/metallib/entry mismatch for ", archive.filename));
        }
        const auto paths = cache_binary_archive_paths(cache_dir, key);
        const auto path = std::find_if(
            paths.begin(), paths.end(), [&archive](const std::filesystem::path& candidate) {
                return candidate.filename() == archive.filename;
            });
        if (path == paths.end()) {
            return std::unexpected(shader_cache_message(
                "Metal archive qualification has unknown descriptor cap for ", archive.filename));
        }
        const shader_workgroup_size workgroup = expected_shader_workgroup_size(key);
        const std::uint64_t exact_threads = static_cast<std::uint64_t>(workgroup.x) *
                                            static_cast<std::uint64_t>(workgroup.y) *
                                            static_cast<std::uint64_t>(workgroup.z);
        const std::uint64_t expected_threads = path == paths.begin() ? exact_threads : 256u;
        if (qualification->max_total_threads_per_threadgroup != expected_threads ||
            qualification->threadgroup_size_is_multiple) {
            return std::unexpected(shader_cache_message(
                "Metal archive qualification descriptor mismatch for ", archive.filename));
        }
        if (common_negative_error && *common_negative_error != qualification->negative_error) {
            return std::unexpected("Metal archive qualifications disagree on NSError fingerprint");
        }
        common_negative_error = qualification->negative_error;
        helper_identity helper_identity{
            qualification->helper_wire_revision,
            qualification->helper_build_identity,
            qualification->helper_source,
            qualification->helper_executable_size,
            qualification->helper_executable_sha256,
            qualification->helper_metal_registry_id,
        };
        if (common_helper && *common_helper != helper_identity) {
            return std::unexpected(
                "Metal archive qualifications disagree on helper or registry identity");
        }
        common_helper = std::move(helper_identity);
    }
    if (!common_negative_error || !common_helper) {
        return std::unexpected("sealed shader cache has no Metal archive qualification receipt");
    }
    helper_identity command_helper{
        command_bundle->qualification.helper_wire_revision,
        command_bundle->qualification.helper_build_identity,
        command_bundle->qualification.helper_source,
        command_bundle->qualification.helper_executable_size,
        command_bundle->qualification.helper_executable_sha256,
        command_bundle->qualification.helper_metal_registry_id,
    };
    if (*common_helper != command_helper) {
        return std::unexpected(
            "MoltenVK command and pipeline archives disagree on helper/device provenance");
    }
    return {};
}

/// @brief Finds one retained artifact by kind, key ordinal, and filename.
/// @param[in] images Retained seed artifacts.
/// @param[in] kind Required artifact kind.
/// @param[in] key_index Required shader key ordinal.
/// @param[in] filename Required artifact filename.
/// @return Borrowed image or `nullptr` when absent.
[[nodiscard]] inline const sealed_shader_cache_artifact_image*
find_artifact_image(std::span<const sealed_shader_cache_artifact_image> images,
                    sealed_shader_cache_artifact_kind kind,
                    std::uint32_t key_index,
                    std::string_view filename) noexcept {
    const auto image = std::find_if(
        images.begin(),
        images.end(),
        [kind, key_index, filename](const sealed_shader_cache_artifact_image& candidate) {
            return candidate.descriptor.kind == kind &&
                   candidate.descriptor.key_index == key_index &&
                   candidate.descriptor.filename == filename;
        });
    return image == images.end() ? nullptr : &*image;
}

/// @brief Validates all semantic relationships using retained artifact
/// bytes. File size/SHA validation happened during their one loader read.
/// @param[in] cache_dir Shader-cache directory used to derive archive names.
/// @param[in] manifest Parsed sealed manifest.
/// @param[in] images Retained artifact images in manifest order.
/// @return Success or the first relationship diagnostic.
[[nodiscard]] inline std::expected<void, std::string>
validate_all_artifact_images(const std::filesystem::path& cache_dir,
                             const sealed_shader_cache_manifest& manifest,
                             std::span<const sealed_shader_cache_artifact_image> images) {
    if (images.size() != manifest.artifacts.size()) {
        return std::unexpected("sealed shader cache seed image artifact count mismatch");
    }
    for (std::size_t ordinal = 0u; ordinal < images.size(); ++ordinal) {
        if (images[ordinal].descriptor != manifest.artifacts[ordinal] ||
            images[ordinal].bytes.empty()) {
            return std::unexpected(shader_cache_message(
                "sealed shader cache seed image mismatch at ordinal ", ordinal));
        }
    }

    const auto command_image = std::find_if(
        images.begin(), images.end(), [](const sealed_shader_cache_artifact_image& artifact) {
            return artifact.descriptor.kind ==
                       sealed_shader_cache_artifact_kind::moltenvk_command_library &&
                   artifact.descriptor.key_index ==
                       (std::numeric_limits<std::uint32_t>::max)() &&
                   artifact.descriptor.filename == "moltenvk_command.sealed";
        });
    if (command_image == images.end())
        return std::unexpected("sealed seed image has no exact MoltenVK command library");
    auto command_bundle = parse_moltenvk_command_library_bundle(command_image->bytes);
    if (!command_bundle || !command_bundle->qualification.is_production_accepted()) {
        return std::unexpected(shader_cache_message(
            "sealed seed MoltenVK command library is invalid: ",
            command_bundle ? "not production-qualified" : command_bundle.error()));
    }
#if defined(__APPLE__)
    if (auto current = validate_moltenvk_command_receipt_against_process_helper(
            command_bundle->qualification);
        !current) {
        return std::unexpected(current.error());
    }
#endif

    for (std::size_t key_ordinal = 0u; key_ordinal < manifest.keys.size(); ++key_ordinal) {
        const shader_variant_key& key = manifest.keys[key_ordinal];
        const std::uint32_t key_index = static_cast<std::uint32_t>(key_ordinal);
        const std::string stem = cache_file_stem(key);
        const auto* metadata = find_artifact_image(
            images, sealed_shader_cache_artifact_kind::shader_meta, key_index, stem + ".meta");
        const auto* metallib =
            find_artifact_image(images,
                                sealed_shader_cache_artifact_kind::shader_metallib,
                                key_index,
                                stem + ".metallib");
        if (metadata == nullptr || metallib == nullptr || metallib->bytes.empty()) {
            return std::unexpected(
                shader_cache_message("sealed shader cache seed key image incomplete for ", stem));
        }
        std::string entry_name;
        if (auto meta = validated_cache_meta_from_bytes(metadata->bytes, key, &entry_name);
            !meta || entry_name.empty()) {
            return std::unexpected(shader_cache_message(
                "sealed shader cache seed metadata violates the cache contract for ",
                stem,
                ": ",
                meta ? "entry name is empty" : meta.error()));
        }
    }

    std::optional<metal_archive_negative_error_fingerprint> common_negative_error;
    using seed_helper_identity = std::tuple<std::uint32_t,
                                            std::string,
                                            std::string,
                                            std::uint64_t,
                                            std::array<std::uint8_t, 32>,
                                            std::uint64_t>;
    std::optional<seed_helper_identity> common_helper;
    for (const sealed_shader_cache_artifact_image& archive : images) {
        if (archive.descriptor.kind != sealed_shader_cache_artifact_kind::metal_binary_archive) {
            if (archive.descriptor.kind ==
                sealed_shader_cache_artifact_kind::reserved_command_metallib) {
                return std::unexpected(
                    "sealed shader cache seed contains forbidden placeholder command artifact");
            }
            continue;
        }
        const std::string qualification_filename = archive.descriptor.filename + ".qualification";
        const auto* qualification_image = find_artifact_image(
            images,
            sealed_shader_cache_artifact_kind::metal_binary_archive_qualification,
            archive.descriptor.key_index,
            qualification_filename);
        if (qualification_image == nullptr) {
            return std::unexpected(shader_cache_message("Metal archive qualification missing for ",
                                                        archive.descriptor.filename));
        }
        auto qualification = parse_metal_archive_qualification_receipt(qualification_image->bytes);
        if (!qualification) {
            return std::unexpected(shader_cache_message("Metal archive qualification invalid for ",
                                                        archive.descriptor.filename,
                                                        ": ",
                                                        qualification.error()));
        }
        if (!qualification->is_production_accepted()) {
            return std::unexpected(
                shader_cache_message("Metal archive qualification is not production-qualified for ",
                                     archive.descriptor.filename));
        }
#if defined(__APPLE__)
        if (auto current = validate_metal_archive_receipt_against_process_helper(*qualification);
            !current) {
            return std::unexpected(
                shader_cache_message("Metal archive qualification helper mismatch for ",
                                     archive.descriptor.filename,
                                     ": ",
                                     current.error()));
        }
#endif
        if (qualification->archive_size != archive.descriptor.size ||
            qualification->archive_sha256 != archive.descriptor.sha256) {
            return std::unexpected(
                shader_cache_message("Metal archive qualification is bound to different bytes for ",
                                     archive.descriptor.filename));
        }
        if (archive.descriptor.key_index >= manifest.keys.size()) {
            return std::unexpected("Metal archive qualification key index is out of range");
        }
        const shader_variant_key& key = manifest.keys[archive.descriptor.key_index];
        const std::string stem = cache_file_stem(key);
        const auto* metallib =
            find_artifact_image(images,
                                sealed_shader_cache_artifact_kind::shader_metallib,
                                archive.descriptor.key_index,
                                stem + ".metallib");
        const auto* metadata = find_artifact_image(images,
                                                   sealed_shader_cache_artifact_kind::shader_meta,
                                                   archive.descriptor.key_index,
                                                   stem + ".meta");
        std::string expected_entry_name;
        auto meta =
            metadata == nullptr
                ? std::expected<shader_cache_metadata, std::string>{std::unexpected(
                      "metadata missing")}
                : validated_cache_meta_from_bytes(metadata->bytes, key, &expected_entry_name);
        if (metallib == nullptr || !meta || qualification->shader_key != key ||
            qualification->metallib_size != metallib->descriptor.size ||
            qualification->metallib_sha256 != metallib->descriptor.sha256 ||
            qualification->entry_name != expected_entry_name) {
            return std::unexpected(
                shader_cache_message("Metal archive qualification key/metallib/entry mismatch for ",
                                     archive.descriptor.filename));
        }
        const auto paths = cache_binary_archive_paths(cache_dir, key);
        const auto path = std::find_if(
            paths.begin(), paths.end(), [&archive](const std::filesystem::path& candidate) {
                return candidate.filename() == archive.descriptor.filename;
            });
        if (path == paths.end()) {
            return std::unexpected(
                shader_cache_message("Metal archive qualification has unknown descriptor cap for ",
                                     archive.descriptor.filename));
        }
        const shader_workgroup_size workgroup = expected_shader_workgroup_size(key);
        const std::uint64_t exact_threads = static_cast<std::uint64_t>(workgroup.x) *
                                            static_cast<std::uint64_t>(workgroup.y) *
                                            static_cast<std::uint64_t>(workgroup.z);
        const std::uint64_t expected_threads = path == paths.begin() ? exact_threads : 256u;
        if (qualification->max_total_threads_per_threadgroup != expected_threads ||
            qualification->threadgroup_size_is_multiple) {
            return std::unexpected(
                shader_cache_message("Metal archive qualification descriptor mismatch for ",
                                     archive.descriptor.filename));
        }
        if (common_negative_error && *common_negative_error != qualification->negative_error) {
            return std::unexpected("Metal archive qualifications disagree on NSError fingerprint");
        }
        common_negative_error = qualification->negative_error;
        seed_helper_identity helper_identity{
            qualification->helper_wire_revision,
            qualification->helper_build_identity,
            qualification->helper_source,
            qualification->helper_executable_size,
            qualification->helper_executable_sha256,
            qualification->helper_metal_registry_id,
        };
        if (common_helper && *common_helper != helper_identity) {
            return std::unexpected(
                "Metal archive qualifications disagree on helper or registry identity");
        }
        common_helper = std::move(helper_identity);
    }
    if (!common_negative_error || !common_helper) {
        return std::unexpected(
            "sealed shader cache seed has no Metal archive qualification receipt");
    }
    seed_helper_identity command_helper{
        command_bundle->qualification.helper_wire_revision,
        command_bundle->qualification.helper_build_identity,
        command_bundle->qualification.helper_source,
        command_bundle->qualification.helper_executable_size,
        command_bundle->qualification.helper_executable_sha256,
        command_bundle->qualification.helper_metal_registry_id,
    };
    if (*common_helper != command_helper) {
        return std::unexpected(
            "MoltenVK command and pipeline archives disagree on helper/device provenance");
    }
    return {};
}

/// @brief Computes the nonzero identity for one completed seed load.
/// @param[in] manifest Accepted manifest.
/// @param[in] artifact_read_count Number of artifact reads.
/// @param[in] manifest_bytes Loaded manifest extent.
/// @param[in] artifact_bytes Aggregate loaded artifact extent.
/// @param[in] load_ns Measured load duration.
/// @return Nonzero receipt identity.
[[nodiscard]] inline std::uint64_t
sealed_seed_receipt_identity(const sealed_shader_cache_manifest& manifest,
                             std::uint64_t artifact_read_count,
                             std::uint64_t manifest_bytes,
                             std::uint64_t artifact_bytes,
                             std::uint64_t load_ns) noexcept {
    constexpr std::uint64_t k_fnv_offset = 1469598103934665603ull;
    constexpr std::uint64_t k_fnv_prime = 1099511628211ull;
    std::uint64_t identity = k_fnv_offset;
    const auto append_u64_to_identity = [&identity](std::uint64_t value) {
        constexpr std::uint64_t k_local_fnv_prime = 1099511628211ull;
        for (std::uint32_t shift = 0u; shift != 64u; shift += 8u) {
            identity ^= static_cast<std::uint8_t>(value >> shift);
            identity *= k_local_fnv_prime;
        }
    };
    for (const std::uint8_t byte : manifest.seed_sha256) {
        identity ^= byte;
        identity *= k_fnv_prime;
    }
    append_u64_to_identity(manifest.manifest_checksum);
    append_u64_to_identity(artifact_read_count);
    append_u64_to_identity(manifest_bytes);
    append_u64_to_identity(artifact_bytes);
    append_u64_to_identity(load_ns);
    return identity == 0u ? k_fnv_offset : identity;
}

/// @brief Computes peak memory admission before loading a sealed seed image.
/// @param[in] manifest Accepted manifest.
/// @param[in] manifest_bytes Loaded manifest extent.
/// @return Saturating peak byte estimate.
[[nodiscard]] inline std::uint64_t
seed_image_preallocation_peak_bytes(const sealed_shader_cache_manifest& manifest,
                                    std::uint64_t manifest_bytes) noexcept {
    using bioseq::common::saturating_add;
    using bioseq::common::saturating_mul;
    std::uint64_t retained = sizeof(sealed_shader_cache_seed_image);
    retained = saturating_add(retained, manifest_bytes);
    retained = saturating_add(retained, static_cast<std::uint64_t>(manifest.cache_digest.size()));
    retained = saturating_add(retained,
                              saturating_mul(static_cast<std::uint64_t>(manifest.keys.size()),
                                             sizeof(shader_variant_key)));
    retained = saturating_add(retained,
                              saturating_mul(static_cast<std::uint64_t>(manifest.artifacts.size()),
                                             sizeof(sealed_shader_cache_artifact) +
                                                 sizeof(sealed_shader_cache_artifact_image)));
    std::uint64_t maximum_transient_artifact = 0u;
    for (const sealed_shader_cache_artifact& artifact : manifest.artifacts) {
        retained = saturating_add(
            retained, saturating_mul(static_cast<std::uint64_t>(artifact.filename.size()), 2u));
        maximum_transient_artifact = std::max(maximum_transient_artifact, artifact.size);
    }
    retained = saturating_add(retained, manifest.seed_size);
    const std::uint64_t retained_headroom =
        saturating_mul(retained, bioseq::parallelism::k_vector_preallocation_headroom_factor);
    return saturating_add(retained_headroom, maximum_transient_artifact);
}

/// @brief Bounded header facts read before admitting a complete seed image.
struct manifest_admission_probe {
    std::uint64_t file_bytes = 0u;
    std::uint64_t seed_bytes = 0u;
    std::uint32_t key_count = 0u;
    std::uint32_t artifact_count = 0u;
    std::uint64_t peak_admitted_bytes = 0u;
};

/// @brief Decodes one little-endian 32-bit header field.
[[nodiscard]] inline std::uint32_t probe_u32(std::span<const std::byte, 4> bytes) noexcept {
    std::uint32_t value = 0u;
    for (std::size_t index = 0u; index < bytes.size(); ++index) {
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[index]))
                 << (index * 8u);
    }
    return value;
}

/// @brief Decodes one little-endian 64-bit header field.
[[nodiscard]] inline std::uint64_t probe_u64(std::span<const std::byte, 8> bytes) noexcept {
    std::uint64_t value = 0u;
    for (std::size_t index = 0u; index < bytes.size(); ++index) {
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[index]))
                 << (index * 8u);
    }
    return value;
}

/// @brief Computes a conservative admission bound from manifest header facts.
/// @param[in] manifest_bytes Manifest file extent.
/// @param[in] seed_bytes Aggregate artifact extent.
/// @param[in] key_count Manifest key count.
/// @param[in] artifact_count Manifest artifact count.
/// @return Saturating peak byte estimate.
[[nodiscard]] inline std::uint64_t
seed_image_probe_peak_bytes(std::uint64_t manifest_bytes,
                            std::uint64_t seed_bytes,
                            std::uint32_t key_count,
                            std::uint32_t artifact_count) noexcept {
    using bioseq::common::saturating_add;
    using bioseq::common::saturating_mul;
    std::uint64_t retained = sizeof(sealed_shader_cache_seed_image);
    retained = saturating_add(retained, manifest_bytes);
    retained = saturating_add(retained, 16u);
    retained = saturating_add(retained, saturating_mul(key_count, sizeof(shader_variant_key)));
    retained = saturating_add(retained,
                              saturating_mul(artifact_count,
                                             sizeof(sealed_shader_cache_artifact) +
                                                 sizeof(sealed_shader_cache_artifact_image) +
                                                 2u * k_maximum_manifest_filename_bytes));
    retained = saturating_add(retained, seed_bytes);
    retained =
        saturating_mul(retained, bioseq::parallelism::k_vector_preallocation_headroom_factor);
    // During each exact read the retained image and one incoming artifact may
    // coexist. The largest possible artifact is the entire bounded seed.
    return saturating_add(retained, seed_bytes);
}

/// @brief Reads bounded manifest header and tail fields before memory admission.
/// @param[in] path Sealed manifest path.
/// @return Stable admission facts or a header, size, identity, or I/O diagnostic.
[[nodiscard]] inline std::expected<manifest_admission_probe, std::string>
probe_manifest_for_admission(const std::filesystem::path& path) {
    constexpr std::size_t k_header_bytes = 32u;
    constexpr std::size_t k_tail_bytes = 48u;
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return std::unexpected(
            shader_cache_message("sealed shader cache manifest probe open failed: errno ", errno));
    }
    struct stat stat_before{};
    if (::fstat(fd, &stat_before) != 0 || !S_ISREG(stat_before.st_mode) ||
        stat_before.st_size <= 0 ||
        static_cast<std::uint64_t>(stat_before.st_size) > k_maximum_manifest_bytes ||
        static_cast<std::uint64_t>(stat_before.st_size) < k_header_bytes + k_tail_bytes) {
        const int saved = errno;
        ::close(fd);
        return std::unexpected(shader_cache_message(
            "sealed shader cache manifest probe stat/size invalid: errno ", saved));
    }
    std::array<std::byte, k_header_bytes> header{};
    std::array<std::byte, k_tail_bytes> tail{};
    const auto read_exact = [fd](std::span<std::byte> destination, off_t offset) noexcept {
        std::size_t consumed = 0u;
        while (consumed < destination.size()) {
            const ssize_t count = ::pread(fd,
                                          destination.data() + consumed,
                                          destination.size() - consumed,
                                          offset + static_cast<off_t>(consumed));
            if (count > 0) {
                consumed += static_cast<std::size_t>(count);
                continue;
            }
            if (count < 0 && errno == EINTR)
                continue;
            return false;
        }
        return true;
    };
    const bool read_ok = read_exact(header, 0) &&
                         read_exact(tail, stat_before.st_size - static_cast<off_t>(tail.size()));
    struct stat stat_after{};
    const bool stable = ::fstat(fd, &stat_after) == 0 && stat_after.st_dev == stat_before.st_dev &&
                        stat_after.st_ino == stat_before.st_ino &&
                        stat_after.st_size == stat_before.st_size &&
                        stat_after.st_mtime == stat_before.st_mtime;
    ::close(fd);
    if (!read_ok || !stable ||
        std::memcmp(header.data(), k_manifest_magic.data(), k_manifest_magic.size()) != 0 ||
        probe_u32(std::span<const std::byte, 4>{header.data() + 8u, 4u}) !=
            k_manifest_wire_revision ||
        probe_u32(std::span<const std::byte, 4>{header.data() + 12u, 4u}) != 0u ||
        probe_u32(std::span<const std::byte, 4>{header.data() + 16u, 4u}) != 16u ||
        probe_u32(std::span<const std::byte, 4>{header.data() + 28u, 4u}) != 0u) {
        return std::unexpected("sealed shader cache manifest admission probe is invalid");
    }
    manifest_admission_probe result{};
    result.file_bytes = static_cast<std::uint64_t>(stat_before.st_size);
    result.key_count = probe_u32(std::span<const std::byte, 4>{header.data() + 20u, 4u});
    result.artifact_count = probe_u32(std::span<const std::byte, 4>{header.data() + 24u, 4u});
    result.seed_bytes = probe_u64(std::span<const std::byte, 8>{tail.data(), 8u});
    if (result.key_count == 0u || result.key_count > k_maximum_manifest_keys ||
        result.artifact_count == 0u || result.artifact_count > k_maximum_seed_image_artifacts ||
        result.seed_bytes == 0u || result.seed_bytes > k_maximum_seed_image_bytes) {
        return std::unexpected(
            "sealed shader cache manifest admission probe exceeds bounded ownership");
    }
    result.peak_admitted_bytes = seed_image_probe_peak_bytes(
        result.file_bytes, result.seed_bytes, result.key_count, result.artifact_count);
    if (result.peak_admitted_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
        return std::unexpected("sealed shader cache manifest admission probe saturated");
    }
    return result;
}

/// @brief Reads and validates manifest shape without reading artifacts.
/// @param[in] cache_dir Shader-cache directory.
/// @param[in] expected_digest Expected cache-build digest.
/// @param[in] expected_keys Expected shader key membership.
/// @return Validated manifest or a shape or I/O diagnostic.
[[nodiscard]] inline std::expected<sealed_shader_cache_manifest, std::string>
read_manifest_shape(const std::filesystem::path& cache_dir,
                    std::string_view expected_digest,
                    std::span<const shader_variant_key> expected_keys) {
    auto bytes = read_regular_file(sealed_shader_cache_manifest_path(cache_dir),
                                   "sealed shader cache manifest",
                                   k_maximum_manifest_bytes);
    if (!bytes)
        return std::unexpected(bytes.error());
    auto manifest = parse_manifest(*bytes);
    if (!manifest)
        return std::unexpected(manifest.error());
    if (auto valid = validate_shape(cache_dir, *manifest, expected_digest, expected_keys); !valid) {
        return std::unexpected(valid.error());
    }
    return manifest;
}

} // namespace sealed_shader_cache_detail

/// @brief Returns validated canonical shader-key order for a sealed seed.
/// @param[in] requested_keys Requested shader keys.
/// @return Sorted unique keys or a validation diagnostic.
[[nodiscard]] inline std::expected<std::vector<shader_variant_key>, std::string>
canonical_sealed_shader_cache_keys(std::span<const shader_variant_key> requested_keys) {
    return sealed_shader_cache_detail::canonical_keys(requested_keys);
}

/// @brief Loads one complete sealed seed under bounded memory admission.
/// @param[in] cache_dir Shader-cache directory.
/// @param[in] cache_digest Expected cache-build digest.
/// @param[in] expected_keys Expected shader key membership.
/// @return Immutable process image or the first validation or admission diagnostic.
[[nodiscard]] std::expected<sealed_shader_cache_seed_image, std::string>
load_validated_sealed_shader_cache_seed(const std::filesystem::path& cache_dir,
                                        std::string_view cache_digest,
                                        std::span<const shader_variant_key> expected_keys) {
    using namespace sealed_shader_cache_detail;
    const auto started = std::chrono::steady_clock::now();
    const std::filesystem::path manifest_path = sealed_shader_cache_manifest_path(cache_dir);
    auto admission_probe = probe_manifest_for_admission(manifest_path);
    if (!admission_probe)
        return std::unexpected(admission_probe.error());
    bioseq::parallelism::memory_admission_failure admission_failure{};
    auto permit = bioseq::parallelism::memory_admission_controller::instance().try_reserve(
        admission_probe->peak_admitted_bytes, &admission_failure);
    if (!permit) {
        return std::unexpected(shader_cache_message(
            "sealed shader cache seed admission denied before manifest read: reason=",
            bioseq::parallelism::memory_admission_failure_reason_name(admission_failure.reason),
            " requested=",
            admission_failure.requested_bytes,
            " available=",
            admission_failure.allocatable_after_reservations_bytes));
    }
    auto manifest_bytes = read_regular_file(manifest_path,
                                            "sealed shader cache manifest",
                                            k_maximum_manifest_bytes,
                                            admission_probe->file_bytes);
    if (!manifest_bytes)
        return std::unexpected(manifest_bytes.error());
    auto manifest = parse_manifest(*manifest_bytes);
    if (!manifest)
        return std::unexpected(manifest.error());
    if (manifest->seed_size > k_maximum_seed_image_bytes ||
        manifest->seed_size >
            static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()) ||
        manifest->artifacts.size() > k_maximum_seed_image_artifacts) {
        return std::unexpected(shader_cache_message(
            "sealed shader cache process image exceeds bounded ownership: bytes=",
            manifest->seed_size,
            " artifacts=",
            manifest->artifacts.size()));
    }
    if (manifest->seed_size != admission_probe->seed_bytes ||
        manifest->keys.size() != admission_probe->key_count ||
        manifest->artifacts.size() != admission_probe->artifact_count) {
        return std::unexpected("sealed shader cache manifest changed after admission probe");
    }
    if (auto valid = validate_shape(cache_dir, *manifest, cache_digest, expected_keys); !valid) {
        return std::unexpected(valid.error());
    }

    const std::uint64_t peak_admitted_bytes = seed_image_preallocation_peak_bytes(
        *manifest, static_cast<std::uint64_t>(manifest_bytes->size()));
    if (peak_admitted_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
        peak_admitted_bytes > permit->bytes()) {
        return std::unexpected("sealed shader cache seed admission probe underestimated ownership");
    }

    std::vector<sealed_shader_cache_artifact_image> images;
    images.reserve(manifest->artifacts.size());
    std::uint64_t artifact_bytes = 0u;
    for (const sealed_shader_cache_artifact& artifact : manifest->artifacts) {
        auto bytes = load_validated_artifact_bytes(cache_dir, artifact);
        if (!bytes)
            return std::unexpected(bytes.error());
        if (bytes->size() > (std::numeric_limits<std::uint64_t>::max)() - artifact_bytes) {
            return std::unexpected("sealed shader cache seed loaded byte count overflow");
        }
        artifact_bytes += static_cast<std::uint64_t>(bytes->size());
        images.push_back(sealed_shader_cache_artifact_image{
            .descriptor = artifact,
            .bytes = std::move(*bytes),
        });
    }
    if (artifact_bytes != manifest->seed_size) {
        return std::unexpected(
            shader_cache_message("sealed shader cache seed loaded byte count mismatch: actual=",
                                 artifact_bytes,
                                 " manifest=",
                                 manifest->seed_size));
    }
    if (auto valid = validate_all_artifact_images(cache_dir, *manifest, images); !valid) {
        return std::unexpected(valid.error());
    }
    const auto finished = std::chrono::steady_clock::now();
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count();
    if (elapsed <= 0) {
        return std::unexpected("sealed shader cache seed load produced no positive elapsed time");
    }
    const std::uint64_t manifest_size = static_cast<std::uint64_t>(manifest_bytes->size());
    const std::uint64_t artifact_read_count = static_cast<std::uint64_t>(images.size());
    const std::uint64_t load_ns = static_cast<std::uint64_t>(elapsed);
    const std::uint64_t receipt_identity = sealed_seed_receipt_identity(
        *manifest, artifact_read_count, manifest_size, artifact_bytes, load_ns);
    sealed_shader_cache_seed_image image{std::move(*manifest),
                                         std::move(*manifest_bytes),
                                         std::move(images),
                                         receipt_identity,
                                         artifact_read_count,
                                         manifest_size,
                                         artifact_bytes,
                                         load_ns,
                                         admission_probe->peak_admitted_bytes,
                                         std::move(*permit)};
    if (!image.reconcile_admission() || !image.is_accepted()) {
        return std::unexpected("sealed shader cache seed loader produced an invalid receipt");
    }
    return std::move(image);
}

/// @brief Install-once process owner for an immutable seed. Both production and
/// focused tests use this exact store, so a second acquisition reuses the
/// original loader receipt without repeating filesystem validation.
class sealed_shader_cache_process_store {
  public:
    /// @brief Loads the process seed once and requires later calls to match it.
    /// @param[in] cache_dir Shader-cache directory.
    /// @param[in] digest Expected cache-build digest.
    /// @param[in] requested_keys Expected shader key membership.
    /// @return Shared immutable seed image or a contract diagnostic.
    [[nodiscard]] std::expected<std::shared_ptr<const sealed_shader_cache_seed_image>, std::string>
    load_once(const std::filesystem::path& cache_dir,
              std::string_view digest,
              std::span<const shader_variant_key> requested_keys) {
        auto canonical = canonical_sealed_shader_cache_keys(requested_keys);
        if (!canonical)
            return std::unexpected(canonical.error());
        std::lock_guard<std::mutex> guard(mutex_);
        const std::filesystem::path normalized = cache_dir.lexically_normal();
        if (image_) {
            if (cache_dir_ != normalized || image_->manifest().cache_digest != digest ||
                image_->manifest().keys != *canonical) {
                return std::unexpected(
                    "process shader seed is already sealed to a different cache contract");
            }
            return image_;
        }
        auto loaded = load_validated_sealed_shader_cache_seed(cache_dir, digest, *canonical);
        if (!loaded)
            return std::unexpected(loaded.error());
        auto retained = std::make_shared<const sealed_shader_cache_seed_image>(std::move(*loaded));
        if (!retained->is_accepted()) {
            return std::unexpected("process shader seed has an invalid loader receipt");
        }
        cache_dir_ = normalized;
        image_ = retained;
        ++filesystem_load_count_;
        return retained;
    }

    /// @brief Captures the installed immutable seed image.
    [[nodiscard]] std::shared_ptr<const sealed_shader_cache_seed_image> snapshot() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return image_;
    }

    /// @brief Returns the number of successful filesystem seed loads.
    [[nodiscard]] std::uint64_t filesystem_load_count() const noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        return filesystem_load_count_;
    }

  private:
    mutable std::mutex mutex_;
    std::filesystem::path cache_dir_;
    std::shared_ptr<const sealed_shader_cache_seed_image> image_;
    std::uint64_t filesystem_load_count_ = 0u;
};

namespace sealed_shader_cache_detail {

/// @brief Seals the canonical artifact set after stable reads and identity checks.
/// @param[in] cache_dir Shader-cache directory.
/// @param[in] cache_digest Cache-build digest.
/// @param[in] requested_keys Requested shader key membership.
/// @param[in] before_identity_check Optional test hook before final identity checks.
/// @return Published manifest or the first validation, admission, or I/O diagnostic.
[[nodiscard]] inline std::expected<sealed_shader_cache_manifest, std::string>
seal_shader_cache_manifest_impl(const std::filesystem::path& cache_dir,
                                std::string_view cache_digest,
                                std::span<const shader_variant_key> requested_keys,
                                seal_before_identity_check_hook before_identity_check) {
    if (!valid_digest(cache_digest)) {
        return std::unexpected("sealed shader cache digest is not lowercase hex16");
    }
    auto keys = canonical_keys(requested_keys);
    if (!keys)
        return std::unexpected(keys.error());
    auto expected = expected_artifacts(cache_dir, *keys);
    if (!expected)
        return std::unexpected(expected.error());
    if (expected->size() > k_maximum_seed_image_artifacts) {
        return std::unexpected(shader_cache_message(
            "cannot seal shader cache seed beyond artifact cap: artifacts=", expected->size()));
    }

    const auto artifact_file_size =
        [](const expected_artifact& artifact) -> std::expected<std::uint64_t, std::string> {
        std::error_code error;
        const std::filesystem::file_status status =
            std::filesystem::symlink_status(artifact.path, error);
        if (error || std::filesystem::is_symlink(status) ||
            !std::filesystem::is_regular_file(status)) {
            return std::unexpected(shader_cache_message("cannot seal non-regular shader artifact ",
                                                        artifact.path.string(),
                                                        error ? ": " + error.message() : ""));
        }
        const std::uintmax_t size = std::filesystem::file_size(artifact.path, error);
        if (error || size == 0u || size > k_maximum_seed_artifact_bytes ||
            size > (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(shader_cache_message("cannot seal invalid shader artifact size ",
                                                        artifact.path.string(),
                                                        ": bytes=",
                                                        size));
        }
        return static_cast<std::uint64_t>(size);
    };

    std::uint64_t preflight_seed_bytes = 0u;
    std::uint64_t preflight_max_artifact = 0u;
    for (const expected_artifact& artifact : *expected) {
        auto size = artifact_file_size(artifact);
        if (!size)
            return std::unexpected(size.error());
        if (*size > k_maximum_seed_image_bytes - preflight_seed_bytes) {
            return std::unexpected(
                shader_cache_message("cannot seal shader cache seed beyond byte cap: bytes>",
                                     k_maximum_seed_image_bytes));
        }
        preflight_seed_bytes += *size;
        preflight_max_artifact = std::max(preflight_max_artifact, *size);
    }
    const std::uint64_t estimated_manifest_bytes = bioseq::common::saturating_add(
        256u + static_cast<std::uint64_t>(keys->size()) * sizeof(shader_variant_key),
        bioseq::common::saturating_mul(static_cast<std::uint64_t>(expected->size()),
                                       56u + k_maximum_manifest_filename_bytes));
    if (estimated_manifest_bytes > k_maximum_manifest_bytes) {
        return std::unexpected("cannot seal shader cache seed with oversized manifest");
    }
    const std::uint64_t peak_admitted_bytes =
        seed_image_probe_peak_bytes(estimated_manifest_bytes,
                                    preflight_seed_bytes,
                                    static_cast<std::uint32_t>(keys->size()),
                                    static_cast<std::uint32_t>(expected->size()));
    if (peak_admitted_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
        return std::unexpected("cannot seal shader cache seed because admission saturated");
    }
    bioseq::parallelism::memory_admission_failure admission_failure{};
    auto permit = bioseq::parallelism::memory_admission_controller::instance().try_reserve(
        peak_admitted_bytes, &admission_failure);
    if (!permit) {
        return std::unexpected(shader_cache_message(
            "cannot seal shader cache seed because admission was denied: reason=",
            bioseq::parallelism::memory_admission_failure_reason_name(admission_failure.reason),
            " requested=",
            admission_failure.requested_bytes));
    }

    std::vector<std::uint64_t> exact_sizes;
    exact_sizes.reserve(expected->size());
    std::uint64_t confirmed_seed_bytes = 0u;
    std::uint64_t confirmed_max_artifact = 0u;
    for (const expected_artifact& artifact : *expected) {
        auto size = artifact_file_size(artifact);
        if (!size)
            return std::unexpected(size.error());
        if (*size > k_maximum_seed_image_bytes - confirmed_seed_bytes) {
            return std::unexpected("shader cache seed grew beyond cap during seal admission");
        }
        confirmed_seed_bytes += *size;
        confirmed_max_artifact = std::max(confirmed_max_artifact, *size);
        exact_sizes.push_back(*size);
    }
    if (confirmed_seed_bytes != preflight_seed_bytes ||
        confirmed_max_artifact != preflight_max_artifact) {
        return std::unexpected("shader cache seed artifact sizes changed during seal admission");
    }

    sealed_shader_cache_manifest manifest{};
    manifest.cache_digest = std::string(cache_digest);
    manifest.keys = *keys;
    manifest.artifacts.reserve(expected->size());
    std::vector<sealed_shader_cache_artifact_image> images;
    images.reserve(expected->size());
    std::vector<regular_file_identity> stable_identities;
    stable_identities.reserve(expected->size());
    for (std::size_t ordinal = 0u; ordinal < expected->size(); ++ordinal) {
        const expected_artifact& artifact = (*expected)[ordinal];
        auto stable = read_stable_regular_file(artifact.path,
                                               "shader cache seed artifact",
                                               exact_sizes[ordinal],
                                               k_maximum_seed_artifact_bytes);
        if (!stable)
            return std::unexpected(stable.error());
        sealed_shader_cache_artifact descriptor{
            .kind = artifact.kind,
            .key_index = artifact.key_index,
            .filename = artifact.filename,
            .size = static_cast<std::uint64_t>(stable->bytes.size()),
            .sha256 = sha256(stable->bytes),
        };
        manifest.artifacts.push_back(descriptor);
        images.push_back(sealed_shader_cache_artifact_image{
            .descriptor = std::move(descriptor),
            .bytes = std::move(stable->bytes),
        });
        stable_identities.push_back(stable->identity);
    }
    auto seed = seed_identity(manifest.cache_digest, manifest.keys, manifest.artifacts);
    if (!seed)
        return std::unexpected(seed.error());
    manifest.seed_sha256 = seed->first;
    manifest.seed_size = seed->second;
    if (manifest.seed_size != confirmed_seed_bytes ||
        manifest.seed_size > k_maximum_seed_image_bytes ||
        manifest.artifacts.size() > k_maximum_seed_image_artifacts) {
        return std::unexpected("shader cache seed candidate exceeds admitted publication bounds");
    }
    if (auto shape = validate_shape(cache_dir, manifest, cache_digest, *keys); !shape) {
        return std::unexpected(shape.error());
    }
    if (auto valid = validate_all_artifact_images(cache_dir, manifest, images); !valid) {
        return std::unexpected(valid.error());
    }
    auto serialized = serialize_manifest(manifest);
    if (!serialized)
        return std::unexpected(serialized.error());
    manifest.manifest_checksum = serialized->second;
    auto parsed = parse_manifest(serialized->first);
    if (!parsed)
        return std::unexpected(parsed.error());
    if (auto shape = validate_shape(cache_dir, *parsed, cache_digest, *keys); !shape) {
        return std::unexpected(shape.error());
    }
    if (auto valid = validate_all_artifact_images(cache_dir, *parsed, images); !valid) {
        return std::unexpected(valid.error());
    }
    if (before_identity_check != nullptr)
        before_identity_check();
    for (std::size_t ordinal = 0u; ordinal < expected->size(); ++ordinal) {
        if (!same_regular_file_identity((*expected)[ordinal].path, stable_identities[ordinal])) {
            return std::unexpected(shader_cache_message(
                "shader cache seed artifact identity changed before publication: ",
                (*expected)[ordinal].filename));
        }
    }
    if (auto written =
            durable_write_file(sealed_shader_cache_manifest_path(cache_dir), serialized->first);
        !written) {
        return std::unexpected(written.error());
    }
    return parsed;
}

} // namespace sealed_shader_cache_detail

/// @brief Seals a shader cache without a test mutation hook.
/// @param[in] cache_dir Shader-cache directory.
/// @param[in] cache_digest Cache-build digest.
/// @param[in] requested_keys Requested shader key membership.
/// @return Published manifest or the first validation, admission, or I/O diagnostic.
[[nodiscard]] inline std::expected<sealed_shader_cache_manifest, std::string>
seal_shader_cache_manifest(const std::filesystem::path& cache_dir,
                           std::string_view cache_digest,
                           std::span<const shader_variant_key> requested_keys) {
    return sealed_shader_cache_detail::seal_shader_cache_manifest_impl(
        cache_dir, cache_digest, requested_keys, nullptr);
}

/// @brief Seals a shader cache with a hook before final artifact identity checks.
/// @param[in] cache_dir Shader-cache directory.
/// @param[in] cache_digest Cache-build digest.
/// @param[in] requested_keys Requested shader key membership.
/// @param[in] before_identity_check Test hook invoked before publication checks.
/// @return Published manifest or the first validation, admission, or I/O diagnostic.
[[nodiscard]] inline std::expected<sealed_shader_cache_manifest, std::string>
seal_shader_cache_manifest_for_test(
    const std::filesystem::path& cache_dir,
    std::string_view cache_digest,
    std::span<const shader_variant_key> requested_keys,
    sealed_shader_cache_detail::seal_before_identity_check_hook before_identity_check) {
    return sealed_shader_cache_detail::seal_shader_cache_manifest_impl(
        cache_dir, cache_digest, requested_keys, before_identity_check);
}

/// @brief Loads a complete sealed seed and returns its validated manifest.
/// @param[in] cache_dir Shader-cache directory.
/// @param[in] cache_digest Expected cache-build digest.
/// @param[in] expected_keys Expected shader key membership.
/// @return Validated manifest or the seed-load diagnostic.
[[nodiscard]] inline std::expected<sealed_shader_cache_manifest, std::string>
load_validated_sealed_shader_cache_manifest(const std::filesystem::path& cache_dir,
                                            std::string_view cache_digest,
                                            std::span<const shader_variant_key> expected_keys) {
    auto seed = load_validated_sealed_shader_cache_seed(cache_dir, cache_digest, expected_keys);
    if (!seed)
        return std::unexpected(seed.error());
    return seed->manifest();
}

/// @brief Resolves one borrowed shader key image from a retained process seed.
/// @param[in] seed Accepted process seed image.
/// @param[in] requested_key Requested shader variant.
/// @return Borrowed key image or a membership or metadata diagnostic.
[[nodiscard]] inline std::expected<sealed_shader_cache_key_view, std::string>
make_sealed_shader_cache_key_view(const sealed_shader_cache_seed_image& seed,
                                  const shader_variant_key& requested_key) {
    if (!seed.is_accepted()) {
        return std::unexpected("sealed shader cache process image has invalid loader receipt");
    }
    const sealed_shader_cache_manifest& manifest = seed.manifest();
    const auto key = std::find(manifest.keys.begin(), manifest.keys.end(), requested_key);
    if (key == manifest.keys.end()) {
        return std::unexpected(
            shader_cache_message("requested shader is not a member of the sealed AOT seed: ",
                                 cache_file_stem(requested_key)));
    }
    const std::uint32_t key_index =
        static_cast<std::uint32_t>(std::distance(manifest.keys.begin(), key));
    const std::string stem = cache_file_stem(requested_key);
    const auto* metadata = sealed_shader_cache_detail::find_artifact_image(
        seed.artifacts(),
        sealed_shader_cache_artifact_kind::shader_meta,
        key_index,
        stem + ".meta");
    const auto* metallib = sealed_shader_cache_detail::find_artifact_image(
        seed.artifacts(),
        sealed_shader_cache_artifact_kind::shader_metallib,
        key_index,
        stem + ".metallib");
    if (metadata == nullptr || metallib == nullptr || metallib->bytes.empty()) {
        return std::unexpected(
            shader_cache_message("sealed shader cache key view incomplete for ", stem));
    }
    sealed_shader_cache_key_view view{};
    view.key = requested_key;
    auto meta = validated_cache_meta_from_bytes(metadata->bytes, requested_key, &view.entry_name);
    if (!meta) {
        return std::unexpected(
            shader_cache_message("sealed shader cache metadata violates the cache contract for ",
                                 stem,
                                 ": ",
                                 meta.error()));
    }
    view.meta = *meta;
    view.metallib_bytes = metallib->bytes;
    return view;
}

/// @brief Loads one owned shader key image from a validated sealed seed.
/// @param[in] cache_dir Shader-cache directory.
/// @param[in] cache_digest Expected cache-build digest.
/// @param[in] expected_keys Expected shader key membership.
/// @param[in] requested_key Requested shader variant.
/// @return Owned key image or the seed-load or membership diagnostic.
[[nodiscard]] inline std::expected<sealed_shader_cache_key_image, std::string>
load_sealed_shader_cache_key_image(const std::filesystem::path& cache_dir,
                                   std::string_view cache_digest,
                                   std::span<const shader_variant_key> expected_keys,
                                   const shader_variant_key& requested_key) {
    auto seed = load_validated_sealed_shader_cache_seed(cache_dir, cache_digest, expected_keys);
    if (!seed)
        return std::unexpected(seed.error());
    const sealed_shader_cache_manifest& manifest = seed->manifest();
    const auto key = std::find(manifest.keys.begin(), manifest.keys.end(), requested_key);
    if (key == manifest.keys.end()) {
        return std::unexpected(
            shader_cache_message("requested shader is not a member of the sealed AOT seed: ",
                                 cache_file_stem(requested_key)));
    }
    const std::uint32_t key_index =
        static_cast<std::uint32_t>(std::distance(manifest.keys.begin(), key));
    sealed_shader_cache_key_image image{};
    image.manifest = manifest;
    image.key = requested_key;
    bool metadata_loaded = false;
    bool metallib_loaded = false;
    for (const sealed_shader_cache_artifact_image& artifact : seed->artifacts()) {
        if (artifact.descriptor.key_index != key_index) {
            continue;
        }
        switch (artifact.descriptor.kind) {
        case sealed_shader_cache_artifact_kind::shader_meta: {
            auto meta =
                validated_cache_meta_from_bytes(artifact.bytes, requested_key, &image.entry_name);
            if (!meta) {
                return std::unexpected(shader_cache_message(
                    "sealed shader cache metadata violates the cache contract for ",
                    cache_file_stem(requested_key),
                    ": ",
                    meta.error()));
            }
            image.meta = *meta;
            metadata_loaded = true;
            break;
        }
        case sealed_shader_cache_artifact_kind::shader_metallib:
            image.metallib_bytes = artifact.bytes;
            metallib_loaded = true;
            break;
        case sealed_shader_cache_artifact_kind::metal_binary_archive:
        case sealed_shader_cache_artifact_kind::metal_binary_archive_qualification:
        case sealed_shader_cache_artifact_kind::moltenvk_command_library:
            break;
        case sealed_shader_cache_artifact_kind::reserved_command_metallib:
            return std::unexpected(
                "sealed shader cache contains forbidden placeholder command artifact");
        }
    }
    if (!metadata_loaded || !metallib_loaded || image.metallib_bytes.empty()) {
        return std::unexpected(shader_cache_message("sealed shader cache key image incomplete for ",
                                                    cache_file_stem(requested_key)));
    }
    return image;
}

/// @brief Loads a sealed seed for one required key and returns its manifest.
/// @return Validated manifest or the seed-load or membership diagnostic.
[[nodiscard]] inline std::expected<sealed_shader_cache_manifest, std::string>
load_sealed_shader_cache_manifest_for_key(const std::filesystem::path& cache_dir,
                                          std::string_view cache_digest,
                                          std::span<const shader_variant_key> expected_keys,
                                          const shader_variant_key& requested_key) {
    auto image =
        load_sealed_shader_cache_key_image(cache_dir, cache_digest, expected_keys, requested_key);
    if (!image)
        return std::unexpected(image.error());
    return std::move(image->manifest);
}

} // namespace bioseq::gpu::vulkan
