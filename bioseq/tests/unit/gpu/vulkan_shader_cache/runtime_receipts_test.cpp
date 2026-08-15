/**
 * @file runtime_receipts_test.cpp
 * @brief Tests immutable Metal archive configuration and runtime receipts.
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <tests/support/common.hpp>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>
#include <vulkan/vulkan.h>

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

namespace {

[[nodiscard]] gv::runtime_metal_archive_configuration
accepted_runtime_metal_archive_configuration() {
    temporary_directory temp;
    constexpr std::string_view digest = "0123456789abcdef";
    const gv::shader_variant_key key = gv::make_shader_key(
        gv::shader_kind::chain_tiled_dp_u64, 32u, 32u, 0u);
    const std::array keys{key};
    REQUIRE(gv::cache_binary_archive_paths(temp.path, key).size() == 2u);
    write_fake_sealed_shader_artifacts(temp.path, keys);
    REQUIRE(gv::seal_shader_cache_manifest(temp.path, digest, keys));
    auto loaded = gv::load_validated_sealed_shader_cache_seed(
        temp.path, digest, keys);
    REQUIRE(loaded);
    const auto seed = std::make_shared<const gv::sealed_shader_cache_seed_image>(
        std::move(*loaded));
    gv::runtime_metal_archive_configuration configuration{};
    configuration.seed_image = seed;
    configuration.vulkan_device_uuid[0] = 0x42u;
    configuration.metal_device_registry_id = 0x1234u;
    for (std::size_t ordinal = 0u;
         ordinal < seed->artifacts().size(); ++ordinal) {
        const auto& artifact = seed->artifacts()[ordinal];
        if (artifact.descriptor.kind
            != gv::sealed_shader_cache_artifact_kind::metal_binary_archive) {
            continue;
        }
        const auto* qualification =
            gv::sealed_shader_cache_detail::find_artifact_image(
                seed->artifacts(),
                gv::sealed_shader_cache_artifact_kind::
                    metal_binary_archive_qualification,
                artifact.descriptor.key_index,
                artifact.descriptor.filename + ".qualification");
        REQUIRE(qualification != nullptr);
        const auto parsed = gv::sealed_shader_cache_detail::
            parse_metal_archive_qualification_receipt(qualification->bytes);
        REQUIRE(parsed);
        if (configuration.qualification_receipt_count == 0u) {
            configuration.qualification_wire_revision =
                parsed->wire_revision;
            configuration.helper_wire_revision =
                parsed->helper_wire_revision;
            configuration.helper_build_identity =
                parsed->helper_build_identity;
            configuration.helper_source = parsed->helper_source;
            configuration.helper_executable_size =
                parsed->helper_executable_size;
            configuration.helper_executable_sha256 =
                parsed->helper_executable_sha256;
            configuration.helper_metal_registry_id =
                parsed->helper_metal_registry_id;
        }
        ++configuration.qualification_receipt_count;
        configuration.archives.push_back(gv::runtime_metal_archive_spec{
            .artifact_ordinal = static_cast<std::uint32_t>(ordinal),
            .artifact_filename = artifact.descriptor.filename,
            .size = artifact.descriptor.size,
            .sha256 = artifact.descriptor.sha256,
            .negative_error_domain = parsed->negative_error.domain,
            .negative_error_code = parsed->negative_error.code,
        });
    }
    return configuration;
}

[[nodiscard]] gv::runtime_metal_archive_pipeline_audit
accepted_runtime_metal_archive_audit() {
    const auto configuration = accepted_runtime_metal_archive_configuration();
    const auto archive_identity = gv::make_runtime_metal_archive_set_identity(
        configuration.archives);
    gv::runtime_metal_archive_pipeline_audit audit{};
    audit.receipt_identity = 7u;
    audit.scope_identity = 8u;
    const auto& manifest = configuration.seed_image->manifest();
    const auto& seed_receipt = configuration.seed_image->receipt();
    audit.cache_digest = manifest.cache_digest;
    audit.seed_sha256 = manifest.seed_sha256;
    audit.seed_size = manifest.seed_size;
    audit.manifest_checksum = manifest.manifest_checksum;
    audit.seed_receipt_identity = seed_receipt.identity();
    audit.seed_load_count = seed_receipt.load_count();
    audit.seed_manifest_read_count = seed_receipt.manifest_read_count();
    audit.seed_artifact_read_count = seed_receipt.artifact_read_count();
    audit.seed_manifest_bytes = seed_receipt.manifest_bytes();
    audit.seed_artifact_bytes = seed_receipt.artifact_bytes();
    audit.seed_load_ns = seed_receipt.load_ns();
    audit.seed_admission_count = seed_receipt.admission_count();
    audit.seed_peak_admitted_bytes = seed_receipt.peak_admitted_bytes();
    audit.seed_retained_capacity_bytes =
        seed_receipt.retained_capacity_bytes();
    audit.seed_admission_saturated = seed_receipt.admission_saturated();
    audit.shader_key = gv::make_shader_key(
        gv::shader_kind::chain_tiled_dp_u64, 32u, 32u, 0u);
    audit.expected_entry_name = "main0";
    audit.observed_entry_name = audit.expected_entry_name;
    audit.matched_descriptor_functions = 1u;
    audit.expected_archives = archive_identity;
    audit.loaded_archives = archive_identity;
    audit.archive_pre_post_identity_checks = archive_identity.archive_count;
    audit.pipeline_pre_identity_checks = 0u;
    audit.pipeline_post_identity_checks = 0u;
    audit.command_interpose_installed = true;
    audit.command_interpose_immutable_fail_closed = true;
    audit.configured_vulkan_device_uuid = configuration.vulkan_device_uuid;
    audit.runtime_vulkan_device_uuid = configuration.vulkan_device_uuid;
    audit.api_version = 0x00403000u;
    audit.driver_version = 0x01020304u;
    audit.vendor_id = 0x0000106bu;
    audit.device_id = 0x00000001u;
    audit.pipeline_cache_uuid[0] = 0x42u;
    audit.pipeline_cache_uuid[15] = 0x24u;
    audit.cache_qualification = gv::runtime_metal_archive_process_qualification{
        .wire_revision = 2u,
        .archive_set = archive_identity,
        .receipt_count = configuration.qualification_receipt_count,
        .hash_passes = 1u,
        .hashed_bytes = archive_identity.total_size,
        .private_files_created = archive_identity.archive_count,
        .private_files_unlinked = archive_identity.archive_count,
        .live_private_paths = 0u,
        .pipeline_filesystem_hash_passes = 0u,
        .pipeline_filesystem_hashed_bytes = 0u,
        .helper_wire_revision = configuration.helper_wire_revision,
        .helper_build_identity = "bioseq_gpu_archive_builder",
        .helper_source = "installed-sibling",
        .helper_executable_size = configuration.helper_executable_size,
        .helper_executable_sha256 =
            configuration.helper_executable_sha256,
        .helper_metal_registry_id =
            configuration.helper_metal_registry_id,
        .vulkan_device_uuid = audit.runtime_vulkan_device_uuid,
        .api_version = audit.api_version,
        .driver_version = audit.driver_version,
        .vendor_id = audit.vendor_id,
        .device_id = audit.device_id,
        .pipeline_cache_uuid = audit.pipeline_cache_uuid,
        .seed_receipt_identity = audit.seed_receipt_identity,
        .seed_sha256 = audit.seed_sha256,
        .seed_size = audit.seed_size,
        .manifest_checksum = audit.manifest_checksum,
        .saturated = false,
    };
    audit.cache_qualification.identity =
        gv::runtime_metal_archive_process_qualification_identity(
            audit.cache_qualification);
    audit.pipeline_create_ns = 17u;
    audit.expected_metal_registry_id =
        configuration.metal_device_registry_id;
    audit.command_loader_metal_registry_id =
        configuration.metal_device_registry_id;
    audit.command_source_methods_present = 2u;
    audit.command_source_wrapper_methods = 2u;
    audit.loader_metal_registry_id = configuration.metal_device_registry_id;
    audit.intercepted_metal_registry_id =
        configuration.metal_device_registry_id;
    audit.scoped_interceptions = 1u;
    audit.descriptor_replacements = 1u;
    audit.fail_on_miss_enforcements = 1u;
    audit.same_descriptor_negative_controls = 1u;
    audit.negative_control_misses = 1u;
    audit.expected_negative_error_domain =
        configuration.archives.front().negative_error_domain;
    audit.expected_negative_error_code =
        configuration.archives.front().negative_error_code;
    audit.observed_negative_error_domain =
        audit.expected_negative_error_domain;
    audit.observed_negative_error_code = audit.expected_negative_error_code;
    audit.original_sync_negative_calls = 1u;
    audit.actual_sync_calls = 1u;
    audit.actual_async_calls = 0u;
    audit.outer_vulkan_pipeline_succeeded = true;
    return audit;
}

}  // namespace

TEST_CASE("immutable live gate uses cached scalar configuration result",
          "[component:bioseq][layer:unit][feature:gpu_runtime]") {
    const auto before = gv::capture_runtime_metal_archive_live_gate_result();
    REQUIRE_FALSE(before.installed_accepted);
    for (std::size_t iteration = 0u; iteration < 8u; ++iteration) {
        gv::probe_immutable_metal_policy_live_for_test();
    }
    const auto after = gv::capture_runtime_metal_archive_live_gate_result();
    CHECK(after.install_validation_count == before.install_validation_count);
    CHECK(after.live_gate_calls == before.live_gate_calls + 8u);
    CHECK(after.live_configuration_scan_count == 0u);
}

TEST_CASE("immutable Metal archive configuration binds the exact ordered set",
          "[component:bioseq][layer:unit][feature:gpu_runtime][resource:filesystem]") {
    auto& admission =
        bioseq::parallelism::memory_admission_controller::instance();
    auto accepted = accepted_runtime_metal_archive_configuration();
    REQUIRE(accepted.is_valid());
    const std::uint64_t retained_seed_bytes =
        accepted.seed_image->receipt().retained_capacity_bytes();
    const std::uint64_t reserved_with_seed = admission.reserved_bytes();
    REQUIRE(reserved_with_seed >= retained_seed_bytes);
    // The first qualification also process-retains the installed helper. Keep
    // that intentional singleton reservation in the baseline while proving
    // that this configuration's seed and derived image release independently.
    const std::uint64_t process_qualification_baseline =
        reserved_with_seed - retained_seed_bytes;
    const auto identity = gv::make_runtime_metal_archive_set_identity(
        accepted.archives);
    CHECK(identity.is_valid());
    CHECK(identity.archive_count == 2u);
    CHECK(identity.total_size > 0u);
    {
        const auto admitted =
            gv::make_runtime_metal_archive_configuration_image(
                accepted.seed_image,
                accepted.vulkan_device_uuid,
                accepted.metal_device_registry_id);
        REQUIRE(admitted);
        REQUIRE((*admitted)->is_valid());
        CHECK((*admitted)->configuration() == accepted);
        CHECK((*admitted)->retained_capacity_bytes() > 0u);
        CHECK((*admitted)->retained_capacity_bytes()
              <= (*admitted)->peak_admitted_bytes());
    }
    CHECK(admission.reserved_bytes() == reserved_with_seed);

    SECTION("configuration rejects missing immutable identities") {
        auto invalid = accepted;
        invalid.seed_image.reset();
        CHECK_FALSE(invalid.is_valid());

        // Device identities may be unknown to the supervisor before runtime
        // selects VkPhysicalDevice/MTLDevice. Zero is an explicit request for
        // runtime to seal the actual identities; the receipt never accepts
        // zero after that sealing step.
        auto runtime_sealed = accepted;
        runtime_sealed.vulkan_device_uuid = {};
        runtime_sealed.metal_device_registry_id = 0u;
        CHECK(runtime_sealed.is_valid());
    }

    SECTION("archive entries are retained-image-bound and strictly ordered") {
        auto invalid = accepted;
        invalid.archives.clear();
        CHECK_FALSE(invalid.is_valid());
        invalid = accepted;
        invalid.archives[0].artifact_filename += ".other";
        CHECK_FALSE(invalid.is_valid());
        invalid = accepted;
        ++invalid.archives[0].artifact_ordinal;
        CHECK_FALSE(invalid.is_valid());
        invalid = accepted;
        invalid.archives[0].size = 0u;
        CHECK_FALSE(invalid.is_valid());
        invalid = accepted;
        invalid.archives[0].sha256 = {};
        CHECK_FALSE(invalid.is_valid());
        invalid = accepted;
        invalid.archives[0].negative_error_domain.clear();
        CHECK_FALSE(invalid.is_valid());
        invalid = accepted;
        ++invalid.archives[1].negative_error_code;
        CHECK_FALSE(invalid.is_valid());
        invalid = accepted;
        invalid.archives[1].negative_error_domain += ".other";
        CHECK_FALSE(invalid.is_valid());
        invalid = accepted;
        std::swap(invalid.archives[0], invalid.archives[1]);
        CHECK_FALSE(invalid.is_valid());
        invalid = accepted;
        invalid.archives[1] = invalid.archives[0];
        CHECK_FALSE(invalid.is_valid());
    }

    SECTION("helper qualification metadata is exact") {
        auto invalid = accepted;
        invalid.qualification_wire_revision = 1u;
        CHECK_FALSE(invalid.is_valid());
        invalid = accepted;
        --invalid.qualification_receipt_count;
        CHECK_FALSE(invalid.is_valid());
        invalid = accepted;
        invalid.helper_wire_revision = 1u;
        CHECK_FALSE(invalid.is_valid());
        invalid = accepted;
        invalid.helper_build_identity += ".other";
        CHECK_FALSE(invalid.is_valid());
        invalid = accepted;
        invalid.helper_source = "test-env-override";
        CHECK_FALSE(invalid.is_valid());
        invalid = accepted;
        ++invalid.helper_executable_size;
        CHECK_FALSE(invalid.is_valid());
        invalid = accepted;
        invalid.helper_executable_sha256 = {};
        CHECK_FALSE(invalid.is_valid());
        invalid = accepted;
        invalid.helper_metal_registry_id = 0u;
        CHECK_FALSE(invalid.is_valid());
    }

    SECTION("every byte identity component changes the ordered set") {
        auto changed = accepted;
        changed.archives[0].artifact_filename += ".other";
        CHECK(gv::make_runtime_metal_archive_set_identity(changed.archives)
              != identity);
        changed = accepted;
        ++changed.archives[0].artifact_ordinal;
        CHECK(gv::make_runtime_metal_archive_set_identity(changed.archives)
              != identity);
        changed = accepted;
        ++changed.archives[0].size;
        CHECK(gv::make_runtime_metal_archive_set_identity(changed.archives)
              != identity);
        changed = accepted;
        changed.archives[0].sha256[1] = 0x77u;
        CHECK(gv::make_runtime_metal_archive_set_identity(changed.archives)
              != identity);
        changed = accepted;
        changed.archives[0].negative_error_domain += ".other";
        CHECK(gv::make_runtime_metal_archive_set_identity(changed.archives)
              != identity);
        changed = accepted;
        ++changed.archives[0].negative_error_code;
        CHECK(gv::make_runtime_metal_archive_set_identity(changed.archives)
              != identity);
    }
    accepted = {};
    CHECK(admission.reserved_bytes() == process_qualification_baseline);
}

TEST_CASE("immutable Metal archive pipeline receipt is strict and retained",
          "[component:bioseq][layer:unit][feature:gpu_runtime][resource:filesystem]") {
    STATIC_REQUIRE(!std::is_copy_constructible_v<
                   gv::runtime_metal_archive_pipeline_receipt>);
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<
                   gv::runtime_metal_archive_pipeline_receipt>);

    const auto accepted = accepted_runtime_metal_archive_audit();
    REQUIRE(accepted.is_internally_consistent());
    CHECK(gv::runtime_metal_archive_pipeline_receipt::
          self_test_move_and_reuse_contract(accepted));

    SECTION("exact expected and loaded archive sets must agree") {
        auto invalid = accepted;
        ++invalid.loaded_archives.archive_count;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        ++invalid.loaded_archives.total_size;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        ++invalid.loaded_archives.ordered_checksum;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        --invalid.archive_pre_post_identity_checks;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        --invalid.pipeline_pre_identity_checks;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        --invalid.pipeline_post_identity_checks;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.unsealed_metal_source_requests = 1u;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.metal_source_fallback_calls = 1u;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.manifest_checksum = 0u;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.cache_digest.clear();
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.cache_digest = "0000000000000000";
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.cache_digest[0] = 'A';
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.seed_sha256 = {};
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.seed_size = 0u;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.seed_receipt_identity = 0u;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.seed_load_count = 0u;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.seed_manifest_read_count = 0u;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.seed_artifact_read_count = 0u;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        ++invalid.seed_artifact_bytes;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.seed_load_ns = 0u;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.seed_admission_count = 0u;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.seed_retained_capacity_bytes =
            invalid.seed_peak_admitted_bytes + 1u;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.seed_admission_saturated = true;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.shader_key = {};
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.observed_entry_name += ".other";
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.matched_descriptor_functions = 0u;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.command_interpose_installed = false;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.command_interpose_immutable_fail_closed = false;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        ++invalid.command_loader_metal_registry_id;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        --invalid.command_source_methods_present;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        --invalid.command_source_wrapper_methods;
        CHECK_FALSE(invalid.is_internally_consistent());
    }

    SECTION("Vulkan loader and intercepted Metal device identities are exact") {
        auto invalid = accepted;
        invalid.runtime_vulkan_device_uuid[1] = 0x44u;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.api_version = 0u;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.driver_version = 0u;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.pipeline_cache_uuid = {};
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.pipeline_create_ns = 0u;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        ++invalid.loader_metal_registry_id;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        ++invalid.intercepted_metal_registry_id;
        CHECK_FALSE(invalid.is_internally_consistent());
    }

    SECTION("every scoped call replaces archives enforces miss and runs control") {
        auto invalid = accepted;
        --invalid.descriptor_replacements;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        --invalid.fail_on_miss_enforcements;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        --invalid.same_descriptor_negative_controls;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        --invalid.negative_control_misses;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        --invalid.original_sync_negative_calls;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        ++invalid.actual_async_calls;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        ++invalid.scoped_interceptions;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.observed_negative_error_domain += ".other";
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        ++invalid.observed_negative_error_code;
        CHECK_FALSE(invalid.is_internally_consistent());
    }

    SECTION("outside nested sticky and failed outer execution fail closed") {
        auto invalid = accepted;
        invalid.outside_scope_interceptions = 1u;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.nested_scope = true;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.sticky_failure = true;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.outer_vulkan_pipeline_succeeded = false;
        CHECK_FALSE(invalid.is_internally_consistent());
    }

    SECTION("process qualification equations reject every filesystem shortcut") {
        const auto reject = [&accepted](auto mutate) {
            auto invalid = accepted;
            mutate(invalid.cache_qualification);
            invalid.cache_qualification.identity =
                gv::runtime_metal_archive_process_qualification_identity(
                    invalid.cache_qualification);
            CHECK_FALSE(invalid.is_internally_consistent());
        };
        reject([](auto& value) { value.hash_passes = 2u; });
        reject([](auto& value) { ++value.hashed_bytes; });
        reject([](auto& value) { --value.private_files_created; });
        reject([](auto& value) { --value.private_files_unlinked; });
        reject([](auto& value) { value.live_private_paths = 1u; });
        reject([](auto& value) {
            value.pipeline_filesystem_hash_passes = 1u;
        });
        reject([](auto& value) {
            value.pipeline_filesystem_hashed_bytes = 1u;
        });
        reject([](auto& value) { value.helper_source = "test-env-override"; });
        reject([](auto& value) { value.helper_metal_registry_id = 0u; });
        reject([](auto& value) { value.vulkan_device_uuid[0] ^= 0x80u; });
        reject([](auto& value) { ++value.api_version; });
        reject([](auto& value) { ++value.driver_version; });
        reject([](auto& value) { ++value.vendor_id; });
        reject([](auto& value) { ++value.device_id; });
        reject([](auto& value) { value.pipeline_cache_uuid[0] ^= 0x80u; });
        reject([](auto& value) { value.seed_sha256[0] ^= 0x80u; });
        reject([](auto& value) { ++value.seed_size; });
        reject([](auto& value) { ++value.manifest_checksum; });

        auto invalid_identity = accepted;
        invalid_identity.cache_qualification.identity = 0u;
        CHECK_FALSE(invalid_identity.is_internally_consistent());
    }
}

TEST_CASE("runtime pipeline PREPARED and USED receipts are causal and one-shot",
          "[component:bioseq][layer:unit][feature:gpu_runtime][resource:filesystem]") {
    STATIC_REQUIRE(!std::is_copy_constructible_v<
                   gv::runtime_pipeline_operation_token>);
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<
                   gv::runtime_pipeline_operation_token>);
    STATIC_REQUIRE(!std::is_copy_constructible_v<
                   gv::persisted_grouped_soa_coordinate_accepted_operation>);
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<
                   gv::persisted_grouped_soa_coordinate_accepted_operation>);
    STATIC_REQUIRE(!std::is_copy_constructible_v<
                   gv::runtime_gpu_cache_metrics_delta>);
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<
                   gv::runtime_gpu_cache_metrics_delta>);

    auto& admission =
        bioseq::parallelism::memory_admission_controller::instance();
    const auto base_audit = accepted_runtime_metal_archive_audit();
    REQUIRE(base_audit.is_internally_consistent());
    // Constructing the first audit also once-qualifies and process-retains the
    // installed helper executable. That reservation intentionally lives until
    // process shutdown, so leak checks begin after this process-wide baseline.
    const std::uint64_t reserved_after_process_qualification =
        admission.reserved_bytes();
    CHECK(gv::runtime_pipeline_operation_token::
          self_test_causal_collector_contract(
              base_audit));
    CHECK(admission.reserved_bytes()
          == reserved_after_process_qualification);
    CHECK(gv::runtime::
          self_test_persisted_grouped_soa_finalizer_preclaim_contract(
              base_audit));
    CHECK(admission.reserved_bytes()
          == reserved_after_process_qualification);
    CHECK(gv::runtime::self_test_gpu_cache_partial_delta_contract(
        base_audit));
    CHECK(admission.reserved_bytes()
          == reserved_after_process_qualification);
}

TEST_CASE("runtime GPU environment identity is canonical and byte exact",
          "[component:bioseq][layer:unit][feature:gpu_runtime]") {
    gv::vulkan_feature_set features{};
    features.api_version = 0x01020304u;
    features.driver_version = 0xa0b0c0d0u;
    features.vendor_id = 0x0000106bu;
    features.device_id = 0x0000002au;
    for (std::size_t i = 0u; i < features.pipeline_cache_uuid.size(); ++i) {
        features.pipeline_cache_uuid[i] = static_cast<std::uint8_t>(i);
    }
    const auto identity = gv::canonical_vulkan_driver_identity(features);
    CHECK(std::string(identity.data())
          == "api=01020304;driver=a0b0c0d0;vendor=0000106b;"
             "device=0000002a;pcuuid=000102030405060708090a0b0c0d0e0f");
    const auto terminator = std::find(
        identity.begin(), identity.end(), '\0');
    REQUIRE(terminator != identity.end());
    CHECK(std::all_of(
        std::next(terminator), identity.end(),
        [](char byte) { return byte == '\0'; }));

    auto changed = features;
    ++changed.api_version;
    CHECK(gv::canonical_vulkan_driver_identity(changed) != identity);
    changed = features;
    ++changed.driver_version;
    CHECK(gv::canonical_vulkan_driver_identity(changed) != identity);
    changed = features;
    ++changed.vendor_id;
    CHECK(gv::canonical_vulkan_driver_identity(changed) != identity);
    changed = features;
    ++changed.device_id;
    CHECK(gv::canonical_vulkan_driver_identity(changed) != identity);
    changed = features;
    std::swap(changed.pipeline_cache_uuid[0],
              changed.pipeline_cache_uuid[1]);
    CHECK(gv::canonical_vulkan_driver_identity(changed) != identity);
}

TEST_CASE("runtime GPU cache delta is bounded by its pre-finalizer maximum",
          "[component:bioseq][layer:unit][feature:gpu_runtime]") {
    const gv::runtime_gpu_cache_metrics_audit maximum{
        .token_identity = 77u,
        .gpu_pipeline_cache_loads = 1u,
        .gpu_pipeline_cache_seed_bytes = 4096u,
        .gpu_pipeline_cache_load_ns = 11u,
        .gpu_pipeline_creates = 8u,
        .gpu_pipeline_creation_ns = 88u,
    };
    const gv::runtime_gpu_cache_metrics_audit first = maximum;
    const gv::runtime_gpu_cache_metrics_audit later{
        .token_identity = 77u,
    };
    REQUIRE(maximum.is_internally_consistent());
    CHECK(first.is_componentwise_bounded_by(maximum));
    CHECK(later.is_componentwise_bounded_by(maximum));
    auto invalid = later;
    invalid.token_identity = 78u;
    CHECK_FALSE(invalid.is_componentwise_bounded_by(maximum));
    invalid = first;
    ++invalid.gpu_pipeline_creation_ns;
    CHECK_FALSE(invalid.is_componentwise_bounded_by(maximum));
    invalid = first;
    invalid.saturated = true;
    CHECK_FALSE(invalid.is_componentwise_bounded_by(maximum));
}

TEST_CASE("grouped-SoA radix pass count is exact at byte boundaries",
          "[component:bioseq][layer:unit][feature:gpu_runtime]") {
    const auto passes = [](std::uint64_t target_end) {
        return gv::runtime_grouped_soa_radix_pass_count(
            0u, target_end + 1u);
    };
    CHECK(passes(0u) == 1u);                      // 1 coord + strand
    CHECK(passes(0x7fu) == 1u);                   // 7 coord + strand
    CHECK(passes(0x80u) == 2u);                   // 8 coord + strand
    CHECK(passes(0xffu) == 2u);                   // 8 coord + strand
    CHECK(passes(0x100u) == 2u);                  // 9 coord + strand
    CHECK(passes(0x7fffu) == 2u);                 // 15 coord + strand
    CHECK(passes(0x8000u) == 3u);                 // 16 coord + strand
    CHECK(passes(0xffffu) == 3u);                 // 16 coord + strand
    CHECK(passes(0x10000u) == 3u);                // 17 coord + strand
    CHECK(passes(9'999'999u) == 4u);              // FAST_LONG: 24 + strand
    CHECK(passes(499'999'999u) == 4u);            // XXL_HIFI: 29 + strand
    CHECK(passes(std::uint64_t{1} << 62u) == 8u); // 63 coord + strand
    CHECK_FALSE(gv::runtime_grouped_soa_radix_pass_count(0u, 0u));
    CHECK_FALSE(gv::runtime_grouped_soa_radix_pass_count(
        0u, (std::uint64_t{1} << 63u) + 1u));
    CHECK_FALSE(gv::runtime_grouped_soa_radix_pass_count(
        (std::numeric_limits<std::uint64_t>::max)(), 2u));
}
