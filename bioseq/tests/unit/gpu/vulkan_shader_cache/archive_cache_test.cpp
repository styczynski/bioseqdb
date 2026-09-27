/**
 * @file archive_cache_test.cpp
 * @brief Checks archive-builder results, cache files and shader loading.
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

TEST_CASE("archive helper success sentinel is unique and final",
          "[component:bioseq][layer:unit][feature:gpu_runtime]") {
    CHECK(gv::archive_helper_transcript_is_exact_success("qualification record\nRESULT: ok\n"));
    CHECK_FALSE(
        gv::archive_helper_transcript_is_exact_success("qualification record\nRESULT: failed\n"));
    CHECK_FALSE(gv::archive_helper_transcript_is_exact_success(
        "RESULT: ok\ntrailing output\n"));
    CHECK_FALSE(gv::archive_helper_transcript_is_exact_success(
        "RESULT: failed\nRESULT: ok\n"));
    CHECK_FALSE(gv::archive_helper_transcript_is_exact_success("RESULT: ok"));
}

TEST_CASE("sealed MoltenVK command library round-trips and rejects tamper",
          "[component:bioseq][layer:unit][feature:gpu_runtime]") {
    const auto bundle = fake_moltenvk_command_library_bundle();
    auto wire = gv::sealed_shader_cache_detail::
        serialize_moltenvk_command_library_bundle(bundle);
    REQUIRE(wire);
    const auto parsed = gv::sealed_shader_cache_detail::
        parse_moltenvk_command_library_bundle(*wire);
    REQUIRE(parsed);
    CHECK(*parsed == bundle);

    wire->back() ^= std::byte{0x01u};
    const auto tampered = gv::sealed_shader_cache_detail::
        parse_moltenvk_command_library_bundle(*wire);
    REQUIRE_FALSE(tampered);
    CHECK(tampered.error().find("checksum mismatch") != std::string::npos);
}

TEST_CASE("archive miss NSError fingerprint and receipt are canonical",
          "[component:bioseq][layer:unit][feature:gpu_runtime]") {
    constexpr std::string_view transcript =
        "same-descriptor empty-archive negative control: ok\n"
        "NEGATIVE_CONTROL_NSError domain_hex="
        "4d544c4c6962726172794572726f72446f6d61696e code=-7\n"
        "RESULT: ok\n";
    const auto fingerprint =
        gv::parse_archive_helper_negative_error_fingerprint(transcript);
    REQUIRE(fingerprint);
    CHECK(fingerprint->domain == "MTLLibraryErrorDomain");
    CHECK(fingerprint->code == -7);

    CHECK_FALSE(gv::parse_archive_helper_negative_error_fingerprint(
        "NEGATIVE_CONTROL_NSError domain_hex=4D54 code=1\nRESULT: ok\n"));
    CHECK_FALSE(gv::parse_archive_helper_negative_error_fingerprint(
        "NEGATIVE_CONTROL_NSError domain_hex=4d54 code=nope\nRESULT: ok\n"));
    CHECK_FALSE(gv::parse_archive_helper_negative_error_fingerprint(
        "prefix NEGATIVE_CONTROL_NSError domain_hex=4d54 code=1\n"
        "RESULT: ok\n"));
    CHECK_FALSE(gv::parse_archive_helper_negative_error_fingerprint(
        std::string{transcript} + std::string{transcript}));

    const auto helper_qualification =
        gv::parse_archive_helper_qualification(
            "HELPER_QUALIFICATION wire_revision=2 "
            "build=bioseq_gpu_archive_builder registry_id=4660\n"
            "RESULT: ok\n");
    REQUIRE(helper_qualification);
    CHECK(helper_qualification->wire_revision == 2u);
    CHECK(helper_qualification->build_identity
          == "bioseq_gpu_archive_builder");
    CHECK(helper_qualification->metal_registry_id == 4660u);
    CHECK_FALSE(gv::parse_archive_helper_qualification(
        "HELPER_QUALIFICATION wire_revision=1 "
        "build=bioseq_gpu_archive_builder registry_id=4660\n"));
    CHECK_FALSE(gv::parse_archive_helper_qualification(
        "HELPER_QUALIFICATION wire_revision=2 build=other registry_id=4660\n"));
    CHECK_FALSE(gv::parse_archive_helper_qualification(
        "HELPER_QUALIFICATION wire_revision=2 "
        "build=bioseq_gpu_archive_builder registry_id=0\n"));

    const auto archive = bytes_of("qualified-archive-byte-image");
    const auto metallib = bytes_of("qualified-metallib-byte-image");
    const auto helper = bytes_of("qualified-helper-byte-image");
    const gv::shader_variant_key key = gv::make_shader_key(
        gv::shader_kind::generate_units, 384u, 256u, 64u);
    const gv::metal_archive_qualification_receipt receipt{
        .wire_revision = gv::sealed_shader_cache_detail::
            k_archive_qualification_wire_revision,
        .shader_key = key,
        .metallib_size = static_cast<std::uint64_t>(metallib.size()),
        .metallib_sha256 =
            gv::sealed_shader_cache_detail::sha256(metallib),
        .entry_name = "main0",
        .max_total_threads_per_threadgroup = 256u,
        .threadgroup_size_is_multiple = false,
        .negative_error = *fingerprint,
        .archive_size = static_cast<std::uint64_t>(archive.size()),
        .archive_sha256 = gv::sealed_shader_cache_detail::sha256(archive),
        .helper_wire_revision = 2u,
        .helper_build_identity = "bioseq_gpu_archive_builder",
        .helper_source = "installed-sibling",
        .helper_executable_size = static_cast<std::uint64_t>(helper.size()),
        .helper_executable_sha256 =
            gv::sealed_shader_cache_detail::sha256(helper),
        .helper_metal_registry_id = 0x1234u,
    };
    auto wire = gv::sealed_shader_cache_detail::
        serialize_metal_archive_qualification_receipt(receipt);
    REQUIRE(wire);
    const auto parsed = gv::sealed_shader_cache_detail::
        parse_metal_archive_qualification_receipt(*wire);
    REQUIRE(parsed);
    CHECK(*parsed == receipt);

    REQUIRE(wire->size() > 50u);
    wire->at(50u) ^= std::byte{0x01};
    const auto corrupted = gv::sealed_shader_cache_detail::
        parse_metal_archive_qualification_receipt(*wire);
    REQUIRE_FALSE(corrupted);
    CHECK(corrupted.error().find("checksum mismatch") != std::string::npos);
}

TEST_CASE("failed archive helper output is never promoted",
          "[component:bioseq][layer:unit][feature:gpu_runtime][resource:subprocess]") {
#if !defined(__APPLE__)
    SKIP("Metal archive helper is an Apple-only publication path");
#else
    temporary_directory temp;
    const std::filesystem::path helper = temp.path / "false_archive_helper.sh";
    const auto script = bytes_of(
        "#!/bin/sh\n"
        "printf 'partial archive bytes' > \"$2\"\n"
        "printf 'qualification failed without success sentinel\\n'\n"
        "exit 0\n");
    REQUIRE(gv::durable_write_file(helper, script));
    REQUIRE(::chmod(helper.c_str(), 0700) == 0);
    scoped_environment_variable test_only_env{
        "BIOSEQ_GPU_ARCHIVE_HELPER_TEST_ONLY", "1"};
    scoped_environment_variable helper_env{
        "BIOSEQ_GPU_ARCHIVE_HELPER", helper.string()};
    scoped_archive_helper_store_reset helper_store_reset;

    const gv::shader_variant_key key = gv::make_shader_key(
        gv::shader_kind::generate_units, 384u, 256u, 64u);
    const std::array keys{key};
    write_fake_sealed_shader_artifacts(temp.path, keys);
    constexpr std::string_view forged_entry = "wrong_semantic_entry";
    const gv::shader_workgroup_size workgroup =
        gv::expected_shader_workgroup_size(key);
    gv::shader_cache_metadata forged_meta{};
    forged_meta.key = key;
    forged_meta.wg_x = workgroup.x;
    forged_meta.wg_y = workgroup.y;
    forged_meta.wg_z = workgroup.z;
    forged_meta.entry_len = static_cast<std::uint32_t>(forged_entry.size());
    REQUIRE(gv::durable_write_file(
        gv::cache_meta_path(temp.path, key),
        gv::meta_to_bytes(forged_meta, forged_entry)));
    REQUIRE(gv::cache_entry_complete(temp.path, key));
    const std::vector<std::byte> stale_meta = read_file_bytes(
        gv::cache_meta_path(temp.path, key));
    const std::vector<std::byte> stale_metallib = read_file_bytes(
        gv::cache_metallib_path(temp.path, key));
    const std::filesystem::path final =
        gv::cache_binary_archive_path(temp.path, key);
    REQUIRE(std::filesystem::exists(final));

    // Rebuilding must remove the old archive, before checking its replacement.
    // If the helper fails, sealing the cache must fail too
    const auto result = gv::ensure_compiled_for_sealed_seed(
        temp.path, key, gv::msl_version{});
    REQUIRE_FALSE(result);
    CHECK(result.error().find("without exact success sentinel")
          != std::string::npos);
    CHECK(read_file_bytes(gv::cache_meta_path(temp.path, key)) != stale_meta);
    CHECK(read_file_bytes(gv::cache_metallib_path(temp.path, key))
          != stale_metallib);
    CHECK_FALSE(std::filesystem::exists(final));
    CHECK_FALSE(std::filesystem::exists(final.string() + ".tmp"));
    const auto cannot_seal = gv::seal_shader_cache_manifest(
        temp.path, "0123456789abcdef", keys);
    REQUIRE_FALSE(cannot_seal);
    CHECK(cannot_seal.error().find(
              "cannot seal non-regular shader artifact")
          != std::string::npos);
    CHECK(cannot_seal.error().find(final.string()) != std::string::npos);
#endif
}

TEST_CASE("archive helper qualification is terminal and hashes once",
          "[component:bioseq][layer:unit][feature:gpu_runtime][resource:filesystem]") {
#if !defined(__APPLE__)
    SKIP("archive helper qualification is Apple-only");
#else
    temporary_directory temp;
    const std::filesystem::path helper = temp.path / "helper.sh";
    const auto helper_bytes = bytes_of("#!/bin/sh\nexit 1\n");
    REQUIRE(gv::durable_write_file(helper, helper_bytes));
    REQUIRE(::chmod(helper.c_str(), 0700) == 0);
    scoped_environment_variable test_only{
        "BIOSEQ_GPU_ARCHIVE_HELPER_TEST_ONLY", "1"};
    scoped_environment_variable override{
        "BIOSEQ_GPU_ARCHIVE_HELPER", helper.string()};

    gv::qualified_metal_archive_helper_process_store store;
    const auto first = store.load_once();
    REQUIRE(first);
    CHECK((**first).source == "test-env-override");
    const gv::shader_cache_build_inputs cache_inputs{};
    const auto first_cache_digest =
        gv::shader_cache_digest_hex_with_qualified_helper(cache_inputs, **first);
    REQUIRE(first_cache_digest);
    CHECK(store.qualification_attempt_count() == 1u);
    CHECK(store.filesystem_hash_count() == 1u);
    REQUIRE(gv::durable_write_file(helper, bytes_of("mutated")));
    REQUIRE(::chmod(helper.c_str(), 0700) == 0);
    const auto second = store.load_once();
    REQUIRE(second);
    CHECK(first->get() == second->get());
    CHECK((**second).executable_bytes == helper_bytes);
    const auto second_cache_digest =
        gv::shader_cache_digest_hex_with_qualified_helper(cache_inputs, **second);
    REQUIRE(second_cache_digest);
    CHECK(*second_cache_digest == *first_cache_digest);
    CHECK(store.qualification_attempt_count() == 1u);
    CHECK(store.filesystem_hash_count() == 1u);

    gv::qualified_metal_archive_helper_process_store changed_store;
    const auto changed = changed_store.load_once();
    REQUIRE(changed);
    const auto changed_cache_digest =
        gv::shader_cache_digest_hex_with_qualified_helper(cache_inputs, **changed);
    REQUIRE(changed_cache_digest);
    CHECK(*changed_cache_digest != *first_cache_digest);
    CHECK(changed_store.qualification_attempt_count() == 1u);
    CHECK(changed_store.filesystem_hash_count() == 1u);

    const std::filesystem::path metallib = temp.path / "input.metallib";
    REQUIRE(gv::durable_write_file(metallib, bytes_of("metallib")));
    const int metallib_fd = ::open(
        metallib.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    REQUIRE(metallib_fd >= 0);
    auto staged = gv::stage_archive_helper_invocation(**first, metallib_fd);
    REQUIRE(staged);
    const std::filesystem::path staged_path = staged->helper_path();
    const std::filesystem::path staged_directory = staged_path.parent_path();
    CHECK(staged->helper_identity_matches());
    CHECK(std::filesystem::exists(staged_path));
    REQUIRE(staged->finish());
    CHECK_FALSE(std::filesystem::exists(staged_path));
    CHECK_FALSE(std::filesystem::exists(staged_directory));
    REQUIRE(::close(metallib_fd) == 0);

    const std::filesystem::path symlink = temp.path / "helper-link";
    REQUIRE(::symlink(helper.c_str(), symlink.c_str()) == 0);
    scoped_environment_variable symlink_override{
        "BIOSEQ_GPU_ARCHIVE_HELPER", symlink.string()};
    gv::qualified_metal_archive_helper_process_store failed_store;
    CHECK_FALSE(failed_store.load_once());
    CHECK_FALSE(failed_store.load_once());
    CHECK(failed_store.qualification_attempt_count() == 1u);
    CHECK(failed_store.filesystem_hash_count() == 0u);

    REQUIRE(::chmod(helper.c_str(), 0702) == 0);
    scoped_environment_variable writable_override{
        "BIOSEQ_GPU_ARCHIVE_HELPER", helper.string()};
    gv::qualified_metal_archive_helper_process_store writable_store;
    CHECK_FALSE(writable_store.load_once());
    CHECK(writable_store.qualification_attempt_count() == 1u);
    CHECK(writable_store.filesystem_hash_count() == 0u);
#endif
}

TEST_CASE("archive helper override is explicitly test-only",
          "[component:bioseq][layer:unit][feature:gpu_runtime][resource:filesystem]") {
#if !defined(__APPLE__)
    SKIP("archive helper qualification is Apple-only");
#else
    temporary_directory temp;
    const std::filesystem::path helper = temp.path / "helper.sh";
    REQUIRE(gv::durable_write_file(helper, bytes_of("#!/bin/sh\nexit 1\n")));
    REQUIRE(::chmod(helper.c_str(), 0700) == 0);
    scoped_environment_variable test_only{
        "BIOSEQ_GPU_ARCHIVE_HELPER_TEST_ONLY", "0"};
    scoped_environment_variable override{
        "BIOSEQ_GPU_ARCHIVE_HELPER", helper.string()};
    gv::qualified_metal_archive_helper_process_store store;
    CHECK_FALSE(store.load_once());
    CHECK_FALSE(store.load_once());
    CHECK(store.qualification_attempt_count() == 1u);
    CHECK(store.filesystem_hash_count() == 0u);
#endif
}

TEST_CASE("shader cache metadata and MoltenVK header payload are exact",
          "[component:bioseq][layer:unit][feature:gpu_runtime][resource:filesystem]") {
    temporary_directory temp;
    const gv::shader_variant_key key = gv::make_shader_key(
        gv::shader_kind::generate_units, 384u, 256u, 64u);
    constexpr std::string_view entry = "main0";

    gv::shader_cache_metadata meta{};
    meta.key = key;
    meta.wg_x = 256u;
    meta.wg_y = 1u;
    meta.wg_z = 1u;
    meta.entry_len = static_cast<std::uint32_t>(entry.size());

    const std::vector<std::byte> meta_bytes = gv::meta_to_bytes(meta, entry);
    auto meta_write = gv::durable_write_file(
        gv::cache_meta_path(temp.path, key),
        std::span<const std::byte>(meta_bytes.data(), meta_bytes.size()));
    REQUIRE(meta_write);

    const std::vector<std::byte> metallib = bytes_of("not-a-real-metallib");
    auto metallib_write = gv::durable_write_file(
        gv::cache_metallib_path(temp.path, key),
        std::span<const std::byte>(metallib.data(), metallib.size()));
    REQUIRE(metallib_write);

    // Add the archive and its validation record to mak the test cache entry complete.
    const std::vector<std::byte> fake_archive = bytes_of("not-a-real-metalar");
    auto archive_write = gv::durable_write_file(
        gv::cache_binary_archive_path(temp.path, key),
        std::span<const std::byte>(fake_archive.data(), fake_archive.size()));
    REQUIRE(archive_write);
    write_fake_archive_qualification(
        gv::cache_binary_archive_path(temp.path, key), fake_archive,
        key, metallib, entry);
    REQUIRE(gv::cache_entry_complete(temp.path, key));

    std::string decoded_entry;
    const auto decoded_meta = gv::read_meta(
        gv::cache_meta_path(temp.path, key), &decoded_entry);
    REQUIRE(decoded_meta);
    CHECK(decoded_meta->key == key);
    CHECK(decoded_meta->wg_x == 256u);
    CHECK(decoded_entry == entry);

    // The shader key alone is insufficient. Reject metadata whose workgroup size
    // differs from the compiled variant.
    gv::shader_cache_metadata mismatched_meta = meta;
    mismatched_meta.wg_x = 128u;
    const std::vector<std::byte> mismatched_meta_bytes =
        gv::meta_to_bytes(mismatched_meta, entry);
    auto mismatched_meta_write = gv::durable_write_file(
        gv::cache_meta_path(temp.path, key),
        std::span<const std::byte>(
            mismatched_meta_bytes.data(), mismatched_meta_bytes.size()));
    REQUIRE(mismatched_meta_write);
    CHECK_FALSE(gv::cache_metallib_entry_complete(temp.path, key));
    CHECK_FALSE(gv::cache_entry_complete(temp.path, key));

    shader_module_create_called = false;
    const auto loader_rejected = gv::load_cached_module(
        VK_NULL_HANDLE, temp.path, key, &record_shader_module_create);
    REQUIRE_FALSE(loader_rejected);
    CHECK(loader_rejected.error().find("meta workgroup mismatch")
          != std::string::npos);
    CHECK_FALSE(shader_module_create_called);

    no_publish_compile_service no_publish_service{};
    const auto post_compile_rejected = gv::load_module(
        VK_NULL_HANDLE,
        temp.path,
        key,
        &record_shader_module_create,
        no_publish_service);
    REQUIRE_FALSE(post_compile_rejected);
    CHECK(post_compile_rejected.error().find("cache entry")
          != std::string::npos);
    CHECK(post_compile_rejected.error().find("meta workgroup mismatch")
          != std::string::npos);
    CHECK(no_publish_service.calls == 1);
    CHECK_FALSE(shader_module_create_called);

    auto& admission =
        bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t reserved_before = admission.reserved_bytes();
    {
        const auto module_code = gv::make_moltenvk_metallib_module_code(
            gv::cache_metallib_path(temp.path, key));
        REQUIRE(module_code);
        CHECK(module_code->admission_count == 1u);
        CHECK(module_code->peak_admitted_bytes
              >= module_code->retained_capacity_bytes);
        CHECK(module_code->retained_capacity_bytes > 0u);
        CHECK_FALSE(module_code->admission_saturated);
        CHECK(module_code->permit.bytes()
              == module_code->retained_capacity_bytes);
        CHECK(module_code->header_size_bytes
              == gv::moltenvk_msl_spirv_header_size());
        CHECK(module_code->code_size_bytes
              == gv::moltenvk_msl_spirv_header_size() + metallib.size());
        CHECK(reinterpret_cast<std::uintptr_t>(module_code->words.data())
              % alignof(std::uint32_t) == 0u);

        const auto* submitted =
            reinterpret_cast<const std::byte*>(module_code->words.data());
        REQUIRE(module_code->header_size_bytes >= sizeof(std::uint32_t));
        std::uint32_t magic = 0;
        std::memcpy(&magic, submitted, sizeof(magic));
        CHECK(magic == gv::moltenvk_msl_compiled_code_magic());
        CHECK(std::memcmp(
                  submitted + module_code->header_size_bytes,
                  metallib.data(),
                  metallib.size()) == 0);
    }
    CHECK(admission.reserved_bytes() == reserved_before);
}

TEST_CASE("fixed radix cache accepts reflected workgroup and exact archive",
          "[component:bioseq][layer:unit][feature:gpu_runtime][resource:filesystem]") {
    temporary_directory temp;
    const gv::shader_variant_key key = gv::make_shader_key(
        gv::shader_kind::radix_sort_histogram, 384u, 1u, 1u);
    constexpr std::string_view entry = "main0";

    gv::shader_cache_metadata meta{};
    meta.key = key;
    meta.wg_x = 256u;
    meta.wg_y = 1u;
    meta.wg_z = 1u;
    meta.entry_len = static_cast<std::uint32_t>(entry.size());
    const std::vector<std::byte> meta_bytes = gv::meta_to_bytes(meta, entry);
    REQUIRE(gv::durable_write_file(
        gv::cache_meta_path(temp.path, key),
        std::span<const std::byte>(meta_bytes.data(), meta_bytes.size())));

    const std::vector<std::byte> metallib = bytes_of("fixed-radix-metallib");
    REQUIRE(gv::durable_write_file(
        gv::cache_metallib_path(temp.path, key),
        std::span<const std::byte>(metallib.data(), metallib.size())));
    const std::vector<std::byte> archive = bytes_of("fixed-radix-metalar");
    REQUIRE(gv::durable_write_file(
        gv::cache_binary_archive_path(temp.path, key),
        std::span<const std::byte>(archive.data(), archive.size())));
    write_fake_archive_qualification(
        gv::cache_binary_archive_path(temp.path, key), archive,
        key, metallib, entry);

    // This radix shader compiles to 256 threads even though its key stores local_size=1.
    // The base archive is  enough, so no extra max_threads_256 archive should be required.
    CHECK(gv::cache_binary_archive_paths(temp.path, key).size() == 1u);
    REQUIRE(gv::cache_entry_status(temp.path, key));
    REQUIRE(gv::cache_entry_complete(temp.path, key));

    shader_module_create_called = false;
    no_publish_compile_service should_not_compile{};
    const auto loaded = gv::load_module(
        VK_NULL_HANDLE,
        temp.path,
        key,
        &record_shader_module_create,
        should_not_compile);
    REQUIRE(loaded);
    CHECK(loaded->wg_x == 256u);
    CHECK(should_not_compile.calls == 0);
    CHECK(shader_module_create_called);
}
