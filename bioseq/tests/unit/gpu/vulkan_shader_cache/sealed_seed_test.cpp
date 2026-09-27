/**
 * @file sealed_seed_test.cpp
 * @brief Checks shader-cache manifests, saved file contents, and, atomic publication.
 */

#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.gpu.vulkan.shader_cache;
import bioseq.gpu.vulkan.shader_compiler;
import bioseq.gpu.vulkan.shader_layout;
import bioseq.gpu.vulkan.shader_variants;
import bioseq.gpu.vulkan.abi;
import bioseq.gpu.vulkan.runtime;
import bioseq.parallelism.memory_admission;
import bioseq.prefilter.types;

import bioseq.tests.unit.gpu.vulkan_shader_cache_support;

namespace gv = bioseq::gpu::vulkan;
namespace pf = bioseq::prefilter;
using namespace bioseq::tests::unit::gpu::vulkan_shader_cache_test_support;

TEST_CASE("sealed shader cache manifest fixes exact AOT membership and bytes",
          "[component:bioseq][layer:unit][feature:gpu_runtime][resource:filesystem]") {
    const auto abc = bytes_of("abc");
    constexpr std::array<std::uint8_t, 32> abc_sha256{
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
    };
    CHECK(gv::sealed_shader_cache_detail::sha256(abc) == abc_sha256);

    temporary_directory temp;
    constexpr std::string_view digest = "0123456789abcdef";
    const gv::shader_variant_key key_64 = gv::make_shader_key(
        gv::shader_kind::generate_units, 384u, 256u, 64u);
    const gv::shader_variant_key key_32 = gv::make_shader_key(
        gv::shader_kind::generate_units, 384u, 256u, 32u);
    const std::array requested{key_64, key_32};
    write_fake_sealed_shader_artifacts(temp.path, requested);

    const auto sealed = gv::seal_shader_cache_manifest(
        temp.path, digest, requested);
    REQUIRE(sealed);
    REQUIRE(sealed->keys.size() == requested.size());
    CHECK(std::none_of(
        sealed->artifacts.begin(),
        sealed->artifacts.end(),
        [](const gv::sealed_shader_cache_artifact& artifact) {
            return artifact.kind
                == gv::sealed_shader_cache_artifact_kind::reserved_command_metallib;
        }));
    CHECK(sealed->keys[0] == key_32);
    CHECK(sealed->keys[1] == key_64);
    CHECK(sealed->seed_size > 0u);
    CHECK(sealed->manifest_checksum != 0u);
    CHECK(std::filesystem::file_size(
              gv::sealed_shader_cache_manifest_path(temp.path)) > 0u);

    // Callers can list the expected keys in any order. The stored manifest uses sorted keys.
    const std::array reverse_expected{key_64, key_32};
    const auto loaded = gv::load_validated_sealed_shader_cache_manifest(
        temp.path, digest, reverse_expected);
    REQUIRE(loaded);
    CHECK(loaded->seed_sha256 == sealed->seed_sha256);
    CHECK(loaded->artifacts == sealed->artifacts);

    shader_module_create_called = false;
    const auto module = gv::load_sealed_cached_module(
        VK_NULL_HANDLE,
        temp.path,
        digest,
        reverse_expected,
        key_64,
        &record_shader_module_create);
    REQUIRE(module);
    CHECK(shader_module_create_called);

    shader_module_create_called = false;
    const gv::shader_variant_key nonmember = gv::make_shader_key(
        gv::shader_kind::align_extend_banded_affine, 384u, 1u, 512u);
    const auto rejected = gv::load_sealed_cached_module(
        VK_NULL_HANDLE,
        temp.path,
        digest,
        reverse_expected,
        nonmember,
        &record_shader_module_create);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().find("not a member of the sealed AOT seed")
          != std::string::npos);
    CHECK_FALSE(shader_module_create_called);

    const std::array wrong_expected{key_64};
    const auto wrong_membership =
        gv::load_validated_sealed_shader_cache_manifest(
            temp.path, digest, wrong_expected);
    REQUIRE_FALSE(wrong_membership);
    CHECK(wrong_membership.error().find("key membership mismatch")
          != std::string::npos);
}

TEST_CASE("sealed shader module uses the exact already-hashed metallib image",
          "[component:bioseq][layer:unit][feature:gpu_runtime][resource:filesystem]") {
    temporary_directory temp;
    constexpr std::string_view digest = "1122334455667788";
    const gv::shader_variant_key key = gv::make_shader_key(
        gv::shader_kind::generate_units, 384u, 256u, 64u);
    const std::array keys{key};
    write_fake_sealed_shader_artifacts(temp.path, keys);
    REQUIRE(gv::seal_shader_cache_manifest(temp.path, digest, keys));

    auto image = gv::load_sealed_shader_cache_key_image(
        temp.path, digest, keys, key);
    REQUIRE(image);
    expected_shader_module_metallib = image->metallib_bytes;
    const auto replacement = bytes_of("mutated-after-verified-image-load");
    REQUIRE(gv::durable_write_file(
        gv::cache_metallib_path(temp.path, key), replacement));

    shader_module_create_called = false;
    shader_module_payload_matches = false;
    const auto module = gv::create_shader_module_from_sealed_key_image(
        VK_NULL_HANDLE, *image, &record_exact_shader_module_create);
    REQUIRE(module);
    CHECK(shader_module_create_called);
    CHECK(shader_module_payload_matches);

    // A fresh load must detect the changed file before creating a Vulkan module.
    // The image loaded earlier keeps the bytes that already passed validation.
    shader_module_create_called = false;
    const auto rejected = gv::load_sealed_cached_module(
        VK_NULL_HANDLE,
        temp.path,
        digest,
        keys,
        key,
        &record_shader_module_create);
    REQUIRE_FALSE(rejected);
    CHECK_FALSE(shader_module_create_called);
    CHECK((rejected.error().find("artifact size mismatch") != std::string::npos
           || rejected.error().find("artifact SHA-256 mismatch")
               != std::string::npos));
    expected_shader_module_metallib.clear();
}

TEST_CASE("process seed reads once and survives source mutation and deletion",
          "[component:bioseq][layer:unit][feature:gpu_runtime][resource:filesystem]") {
    STATIC_REQUIRE_FALSE(std::is_default_constructible_v<
                         gv::sealed_shader_cache_seed_receipt>);
    STATIC_REQUIRE_FALSE(std::is_default_constructible_v<
                         gv::sealed_shader_cache_seed_image>);
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<
                         gv::sealed_shader_cache_seed_image>);

    temporary_directory temp;
    constexpr std::string_view digest = "22446688aaccee00";
    const gv::shader_variant_key key = gv::make_shader_key(
        gv::shader_kind::generate_units, 384u, 256u, 64u);
    const gv::shader_variant_key earlier_key = gv::make_shader_key(
        gv::shader_kind::generate_units, 384u, 256u, 32u);
    // Pass keys in the producer's order and check that the cache sorts them for comparison.
    const std::array keys{key, earlier_key};
    const auto canonical = gv::canonical_sealed_shader_cache_keys(keys);
    REQUIRE(canonical);
    REQUIRE(canonical->front() == earlier_key);
    write_fake_sealed_shader_artifacts(temp.path, keys);
    REQUIRE(gv::seal_shader_cache_manifest(temp.path, digest, keys));

    gv::sealed_shader_cache_process_store process_store;
    auto first = process_store.load_once(temp.path, digest, keys);
    REQUIRE(first);
    const std::array reverse_keys{earlier_key, key};
    auto second = process_store.load_once(temp.path, digest, reverse_keys);
    REQUIRE(second);
    CHECK(first->get() == second->get());
    CHECK((*first)->receipt().identity() == (*second)->receipt().identity());
    CHECK(process_store.filesystem_load_count() == 1u);
    const auto seed = *first;
    REQUIRE(seed->is_accepted());
    const auto& receipt = seed->receipt();
    CHECK(receipt.load_count() == 1u);
    CHECK(receipt.manifest_read_count() == 1u);
    CHECK(receipt.artifact_read_count() == seed->artifacts().size());
    CHECK(receipt.artifact_bytes() == seed->manifest().seed_size);
    CHECK(receipt.manifest_bytes() == seed->manifest_bytes().size());
    CHECK(receipt.load_ns() > 0u);
    CHECK(receipt.admission_count() == 1u);
    CHECK(receipt.peak_admitted_bytes() >=
          receipt.retained_capacity_bytes());
    CHECK(receipt.retained_capacity_bytes()
          == seed->owned_capacity_bytes());
    CHECK_FALSE(receipt.admission_saturated());

    auto key_view = gv::make_sealed_shader_cache_key_view(*seed, key);
    REQUIRE(key_view);
    expected_shader_module_metallib.assign(
        key_view->metallib_bytes.begin(), key_view->metallib_bytes.end());

    const auto archive = std::find_if(
        seed->artifacts().begin(), seed->artifacts().end(),
        [](const gv::sealed_shader_cache_artifact_image& image) {
            return image.descriptor.kind
                == gv::sealed_shader_cache_artifact_kind::metal_binary_archive;
        });
    REQUIRE(archive != seed->artifacts().end());
    const auto qualification = std::find_if(
        seed->artifacts().begin(), seed->artifacts().end(),
        [&archive](const gv::sealed_shader_cache_artifact_image& image) {
            return image.descriptor.kind
                    == gv::sealed_shader_cache_artifact_kind::
                        metal_binary_archive_qualification
                && image.descriptor.filename
                    == archive->descriptor.filename + ".qualification";
        });
    REQUIRE(qualification != seed->artifacts().end());
    REQUIRE(gv::sealed_shader_cache_detail::
        parse_metal_archive_qualification_receipt(qualification->bytes));

    REQUIRE(gv::durable_write_file(
        gv::cache_metallib_path(temp.path, key), bytes_of("mutated")));
    std::error_code remove_error;
    std::filesystem::remove_all(temp.path, remove_error);
    REQUIRE_FALSE(remove_error);

    shader_module_create_called = false;
    shader_module_payload_matches = false;
    const auto module = gv::load_sealed_cached_module(
        VK_NULL_HANDLE, *seed, key, &record_exact_shader_module_create);
    REQUIRE(module);
    CHECK(shader_module_create_called);
    CHECK(shader_module_payload_matches);

    auto& admission =
        bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t previous_cap = admission.index_memory_cap_bytes();
    admission.set_index_memory_cap_bytes(admission.reserved_bytes());
    shader_module_create_called = false;
    shader_module_payload_matches = false;
    const auto denied_module = gv::load_sealed_cached_module(
        VK_NULL_HANDLE, *seed, key, &record_exact_shader_module_create);
    admission.set_index_memory_cap_bytes(previous_cap);
    REQUIRE_FALSE(denied_module);
    CHECK(denied_module.error().find("module admission denied")
          != std::string::npos);
    CHECK_FALSE(shader_module_create_called);

    shader_module_create_called = false;
    shader_module_payload_matches = false;
    const auto second_module = gv::load_sealed_cached_module(
        VK_NULL_HANDLE, *seed, key, &record_exact_shader_module_create);
    REQUIRE(second_module);
    CHECK(shader_module_create_called);
    CHECK(shader_module_payload_matches);
    CHECK(gv::sealed_shader_cache_detail::sha256(archive->bytes)
          == archive->descriptor.sha256);
    expected_shader_module_metallib.clear();
}

TEST_CASE("process seed rejects oversized manifest before artifact allocation",
          "[component:bioseq][layer:unit][feature:gpu_runtime][resource:filesystem]") {
    temporary_directory temp;
    constexpr std::string_view digest = "33557799bbddff11";
    const gv::shader_variant_key key = gv::make_shader_key(
        gv::shader_kind::generate_units, 384u, 256u, 64u);
    const std::array keys{key};
    write_fake_sealed_shader_artifacts(temp.path, keys);
    const auto sealed = gv::seal_shader_cache_manifest(
        temp.path, digest, keys);
    REQUIRE(sealed);

    gv::sealed_shader_cache_manifest oversized = *sealed;
    oversized.seed_size =
        gv::sealed_shader_cache_detail::k_maximum_seed_image_bytes + 1u;
    const auto wire =
        gv::sealed_shader_cache_detail::serialize_manifest(oversized);
    REQUIRE(wire);
    REQUIRE(gv::durable_write_file(
        gv::sealed_shader_cache_manifest_path(temp.path), wire->first));
    const auto rejected = gv::load_validated_sealed_shader_cache_seed(
        temp.path, digest, keys);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().find("exceeds bounded ownership")
          != std::string::npos);
}

TEST_CASE("seal rejects oversized seed before publishing or reading it",
          "[component:bioseq][layer:unit][feature:gpu_runtime][resource:filesystem]") {
    temporary_directory temp;
    constexpr std::string_view digest = "22446688aaccee00";
    const gv::shader_variant_key key = gv::make_shader_key(
        gv::shader_kind::generate_units, 384u, 256u, 64u);
    const std::array keys{key};
    write_fake_sealed_shader_artifacts(temp.path, keys);
    std::error_code error;
    std::filesystem::resize_file(
        gv::cache_metallib_path(temp.path, key),
        gv::sealed_shader_cache_detail::k_maximum_seed_image_bytes + 1u,
        error);
    REQUIRE_FALSE(error);
    const auto sealed = gv::seal_shader_cache_manifest(
        temp.path, digest, keys);
    REQUIRE_FALSE(sealed);
    CHECK(sealed.error().find("beyond byte cap") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(
        gv::sealed_shader_cache_manifest_path(temp.path)));
    CHECK_FALSE(std::filesystem::exists(
        gv::sealed_shader_cache_manifest_path(temp.path).string() + ".tmp"));
}

TEST_CASE("seal validates the admitted candidate before atomic publication",
          "[component:bioseq][layer:unit][feature:gpu_runtime][resource:filesystem]") {
    temporary_directory temp;
    constexpr std::string_view digest = "1133557799bbddff";
    const gv::shader_variant_key key = gv::make_shader_key(
        gv::shader_kind::generate_units, 384u, 256u, 64u);
    const std::array keys{key};
    write_fake_sealed_shader_artifacts(temp.path, keys);
    REQUIRE(gv::durable_write_file(
        gv::cache_meta_path(temp.path, key), bytes_of("bad-meta")));
    const auto sealed = gv::seal_shader_cache_manifest(
        temp.path, digest, keys);
    REQUIRE_FALSE(sealed);
    CHECK(sealed.error().find("metadata") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(
        gv::sealed_shader_cache_manifest_path(temp.path)));
    CHECK_FALSE(std::filesystem::exists(
        gv::sealed_shader_cache_manifest_path(temp.path).string() + ".tmp"));
}

TEST_CASE("seal rejects same-size mutation between read and publication",
          "[component:bioseq][layer:unit][feature:gpu_runtime][resource:filesystem]") {
    temporary_directory temp;
    constexpr std::string_view digest = "0011223344556677";
    const gv::shader_variant_key key = gv::make_shader_key(
        gv::shader_kind::generate_units, 384u, 256u, 64u);
    const std::array keys{key};
    write_fake_sealed_shader_artifacts(temp.path, keys);
    seal_identity_mutation_path = gv::cache_metallib_path(temp.path, key);
    seal_identity_mutation_succeeded = false;
    const auto sealed = gv::seal_shader_cache_manifest_for_test(
        temp.path, digest, keys, &mutate_seal_artifact_same_size);
    seal_identity_mutation_path.clear();
    REQUIRE(seal_identity_mutation_succeeded);
    REQUIRE_FALSE(sealed);
    CHECK(sealed.error().find("identity changed before publication")
          != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(
        gv::sealed_shader_cache_manifest_path(temp.path)));
    CHECK_FALSE(std::filesystem::exists(
        gv::sealed_shader_cache_manifest_path(temp.path).string() + ".tmp"));
}

TEST_CASE("private archive staging canonicalizes a symlinked temp parent",
          "[component:bioseq][layer:unit][feature:gpu_runtime][resource:filesystem]") {
#if !defined(__APPLE__)
    SKIP("private Metal archive snapshots are Apple-only");
#else
    temporary_directory temp;
    const std::filesystem::path real_parent = temp.path / "real";
    const std::filesystem::path linked_parent = temp.path / "linked";
    std::error_code error;
    std::filesystem::create_directory(real_parent, error);
    REQUIRE_FALSE(error);
    std::filesystem::create_directory_symlink(
        real_parent, linked_parent, error);
    REQUIRE_FALSE(error);

    const auto retained = bytes_of("archive-source-is-already-deleted");
    const std::filesystem::path source = temp.path / "source.metalar";
    REQUIRE(gv::durable_write_file(source, retained));
    std::filesystem::remove(source, error);
    REQUIRE_FALSE(error);
    REQUIRE_FALSE(std::filesystem::exists(source));

    const auto snapshot = gv::verify_runtime_private_archive_snapshot_for_test(
        "safe-test.metalar", retained, linked_parent);
    REQUIRE(snapshot);
    CHECK(snapshot->snapshot_path.is_absolute());
    CHECK(snapshot->snapshot_path.parent_path()
          == std::filesystem::canonical(real_parent));
    CHECK(snapshot->streamed_bytes == retained.size());
    CHECK(snapshot->scratch_capacity_bytes == 64u * 1024u);
    CHECK(snapshot->exact_seed_compared);
    CHECK(snapshot->tamper_rejected);
    CHECK_FALSE(std::filesystem::exists(snapshot->snapshot_path));
#endif
}

TEST_CASE("sealed shader cache rejects artifact, wire, and symlink mutation",
          "[component:bioseq][layer:unit][feature:gpu_runtime][resource:filesystem]") {
    temporary_directory temp;
    constexpr std::string_view digest = "fedcba9876543210";
    const gv::shader_variant_key key = gv::make_shader_key(
        gv::shader_kind::generate_units, 384u, 256u, 64u);
    const std::array keys{key};
    write_fake_sealed_shader_artifacts(temp.path, keys);
    REQUIRE(gv::seal_shader_cache_manifest(temp.path, digest, keys));

    const std::filesystem::path metallib =
        gv::cache_metallib_path(temp.path, key);
    const std::vector<std::byte> original_metallib = read_file_bytes(metallib);
    const auto corrupted = bytes_of("same-cache-name-different-metallib");
    REQUIRE(gv::durable_write_file(metallib, corrupted));
    auto rejected = gv::load_validated_sealed_shader_cache_manifest(
        temp.path, digest, keys);
    REQUIRE_FALSE(rejected);
    CHECK((rejected.error().find("artifact size mismatch") != std::string::npos
           || rejected.error().find("artifact SHA-256 mismatch")
               != std::string::npos));
    REQUIRE(gv::durable_write_file(metallib, original_metallib));
    REQUIRE(gv::load_validated_sealed_shader_cache_manifest(
        temp.path, digest, keys));

    const std::filesystem::path qualification =
        gv::cache_binary_archive_qualification_path(
            gv::cache_binary_archive_path(temp.path, key));
    const std::vector<std::byte> original_qualification =
        read_file_bytes(qualification);
    std::vector<std::byte> corrupt_qualification = original_qualification;
    REQUIRE(corrupt_qualification.size() > 50u);
    corrupt_qualification[50u] ^= std::byte{0x01};
    REQUIRE(gv::durable_write_file(qualification, corrupt_qualification));
    rejected = gv::load_validated_sealed_shader_cache_manifest(
        temp.path, digest, keys);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().find("artifact SHA-256 mismatch")
          != std::string::npos);
    REQUIRE(gv::durable_write_file(
        qualification, original_qualification));
    REQUIRE(gv::load_validated_sealed_shader_cache_manifest(
        temp.path, digest, keys));

    const std::filesystem::path manifest_path =
        gv::sealed_shader_cache_manifest_path(temp.path);
    const std::vector<std::byte> original_manifest =
        read_file_bytes(manifest_path);
    std::vector<std::byte> corrupt_manifest = original_manifest;
    REQUIRE(corrupt_manifest.size() > 8u);
    corrupt_manifest.back() ^= std::byte{0x01};
    REQUIRE(gv::durable_write_file(manifest_path, corrupt_manifest));
    rejected = gv::load_validated_sealed_shader_cache_manifest(
        temp.path, digest, keys);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().find("checksum mismatch") != std::string::npos);
    REQUIRE(gv::durable_write_file(manifest_path, original_manifest));

    const std::filesystem::path owned_metallib = metallib.string() + ".owned";
    std::error_code ec;
    std::filesystem::rename(metallib, owned_metallib, ec);
    REQUIRE_FALSE(ec);
    std::filesystem::create_symlink(
        owned_metallib.filename(), metallib, ec);
    REQUIRE_FALSE(ec);
    rejected = gv::load_validated_sealed_shader_cache_manifest(
        temp.path, digest, keys);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().find("must not be a symlink") != std::string::npos);
}
