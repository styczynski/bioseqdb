/**
 * @file coordinate_receipts.cppm
 * @brief Validates and finalizes persisted grouped-SoA coordinate operation receipts.
 */
module;

#include <cstdint>
#include <libassert/assert.hpp>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

module bioseq.gpu.vulkan.runtime:coordinate_receipts;

import :frontend_types;
import :support;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.parallelism.memory_admission;
import bioseq.instrumentation;
import std;

namespace bioseq::gpu::vulkan {

/// @brief Prepares a side-effect-free claim plan after validating receipt and live identities.
/// @param[in] receipt Sealed operation receipt whose jobs and pipeline uses are inspected.
/// @param[in] expected_preview Caller-provided SQL-visible acceptance preview.
/// @param[in] live_features Current Vulkan device and driver identity.
/// @param[in] seed_metrics_claimed Whether process-wide seed metrics were already reported.
/// @return Valid claim plan on exact identity agreement, otherwise an invalid plan.
runtime_detail::persisted_grouped_soa_coordinate_finalization_plan
runtime::prepare_persisted_grouped_soa_coordinate_finalization(
    const persisted_grouped_soa_coordinate_operation_receipt& receipt,
    const persisted_grouped_soa_coordinate_operation_receipt_preview& expected_preview,
    const vulkan_feature_set& live_features,
    const bool seed_metrics_claimed) noexcept {
    runtime_detail::persisted_grouped_soa_coordinate_finalization_plan result{};
    const persisted_grouped_soa_coordinate_operation_receipt_preview receipt_preview =
        receipt.operation_receipt_preview();
    if (!receipt.are_all_jobs_accepted() || !expected_preview.is_internally_consistent() ||
        !receipt_preview.is_internally_consistent() || receipt_preview != expected_preview) {
        return result;
    }
    const runtime_gpu_execution_environment_snapshot environment_preview =
        receipt.environment_preview();
    const runtime_gpu_cache_metrics_audit cache_metrics_maximum = receipt.cache_metrics_preview();
    if (!environment_preview.is_internally_consistent() ||
        !cache_metrics_maximum.is_internally_consistent()) {
        return result;
    }
    const runtime_pipeline_operation_audit& query = receipt.query_pipeline_token_.audit();
    const runtime_pipeline_operation_audit& shape = receipt.grouped_soa_pipeline_token_.audit();
    const bool alignment_present = receipt.has_alignment_cigar_receipt();
    const runtime_pipeline_operation_audit* alignment =
        alignment_present ? std::addressof(receipt.alignment_pipeline_token_.audit()) : nullptr;
    const std::uint64_t alignment_pipeline_receipt_count =
        alignment_present ? static_cast<std::uint64_t>(alignment->prepared_count) : 0u;
    const std::uint64_t alignment_submission_count =
        alignment_present ? alignment->committed_submissions : 0u;
    const bool mapq_present = receipt.has_mapq_receipt();
    const runtime_pipeline_operation_audit* mapq =
        mapq_present ? std::addressof(receipt.mapq_pipeline_token_.audit()) : nullptr;
    const std::uint64_t mapq_pipeline_receipt_count =
        mapq_present ? static_cast<std::uint64_t>(mapq->prepared_count) : 0u;
    const std::uint64_t mapq_submission_count = mapq_present ? mapq->committed_submissions : 0u;
    if (alignment_present &&
        (!receipt.alignment_pipeline_token_.is_valid_for_persisted_alignment_cigar() ||
         alignment->operation_identity != receipt.operation_identity_ ||
         alignment_pipeline_receipt_count != 1u || alignment_submission_count == 0u)) {
        return result;
    }
    if (mapq_present != receipt.requires_mapq_receipt() ||
        (mapq_present && (!receipt.mapq_pipeline_token_.is_valid_for_persisted_mapq() ||
                          mapq->operation_identity != receipt.operation_identity_ ||
                          mapq_pipeline_receipt_count != 1u || mapq_submission_count != 1u))) {
        return result;
    }
    const bool environment_equal =
        query.device_uuid == shape.device_uuid && query.api_version == shape.api_version &&
        query.driver_version == shape.driver_version && query.vendor_id == shape.vendor_id &&
        query.device_id == shape.device_id &&
        query.pipeline_cache_uuid == shape.pipeline_cache_uuid &&
        query.seed_sha256 == shape.seed_sha256 && query.seed_size == shape.seed_size &&
        query.manifest_checksum == shape.manifest_checksum &&
        query.seed_receipt_identity == shape.seed_receipt_identity &&
        query.archive_set == shape.archive_set && query.device_uuid == live_features.device_uuid &&
        query.api_version == live_features.api_version &&
        query.driver_version == live_features.driver_version &&
        query.vendor_id == live_features.vendor_id && query.device_id == live_features.device_id &&
        query.pipeline_cache_uuid == live_features.pipeline_cache_uuid &&
        (!alignment_present ||
         (query.device_uuid == alignment->device_uuid &&
          query.api_version == alignment->api_version &&
          query.driver_version == alignment->driver_version &&
          query.vendor_id == alignment->vendor_id && query.device_id == alignment->device_id &&
          query.pipeline_cache_uuid == alignment->pipeline_cache_uuid &&
          query.seed_sha256 == alignment->seed_sha256 && query.seed_size == alignment->seed_size &&
          query.manifest_checksum == alignment->manifest_checksum &&
          query.seed_receipt_identity == alignment->seed_receipt_identity &&
          query.archive_set == alignment->archive_set &&
          query.cache_qualification == alignment->cache_qualification)) &&
        (!mapq_present ||
         (query.device_uuid == mapq->device_uuid && query.api_version == mapq->api_version &&
          query.driver_version == mapq->driver_version && query.vendor_id == mapq->vendor_id &&
          query.device_id == mapq->device_id &&
          query.pipeline_cache_uuid == mapq->pipeline_cache_uuid &&
          query.seed_sha256 == mapq->seed_sha256 && query.seed_size == mapq->seed_size &&
          query.manifest_checksum == mapq->manifest_checksum &&
          query.seed_receipt_identity == mapq->seed_receipt_identity &&
          query.archive_set == mapq->archive_set &&
          query.cache_qualification == mapq->cache_qualification));
    if (!environment_equal)
        return result;

    const auto union_receipts = receipt.receipt_union();
    if (!union_receipts.valid)
        return result;
    result.cache_claim_plan = runtime_detail::runtime_gpu_cache_metrics_claim_plan::prepare(
        receipt.operation_identity_,
        cache_metrics_maximum,
        seed_metrics_claimed,
        std::span<const runtime_metal_archive_pipeline_receipt* const>{
            union_receipts.receipts.data(), union_receipts.count});
    if (!result.cache_claim_plan.is_valid())
        return {};

    vulkan_feature_set identity_features{};
    identity_features.api_version = query.api_version;
    identity_features.driver_version = query.driver_version;
    identity_features.vendor_id = query.vendor_id;
    identity_features.device_id = query.device_id;
    identity_features.pipeline_cache_uuid = query.pipeline_cache_uuid;
    result.environment = runtime_gpu_execution_environment_snapshot{
        .gpu_environment_present = true,
        .gpu_device_uuid = query.device_uuid,
        .gpu_driver_identity = canonical_vulkan_driver_identity(identity_features),
        .cache_policy = runtime_pipeline_cache_policy::immutable_preseed_all_hit,
        .seed_sha256 = query.seed_sha256,
        .seed_size = query.seed_size,
        .manifest_checksum = query.manifest_checksum,
        .pipeline_cache_all_hit = true,
    };
    const auto coordinate_submission_count =
        bioseq::common::checked_add(query.committed_submissions, shape.committed_submissions);
    if (!coordinate_submission_count.has_value()) {
        return {};
    }
    const auto coordinate_alignment_submission_count =
        bioseq::common::checked_add(*coordinate_submission_count, alignment_submission_count);
    if (!coordinate_alignment_submission_count.has_value()) {
        return {};
    }
    const auto committed_submission_count =
        bioseq::common::checked_add(*coordinate_alignment_submission_count, mapq_submission_count);
    if (!committed_submission_count.has_value()) {
        return {};
    }
    result.acceptance = persisted_grouped_soa_coordinate_operation_acceptance_audit{
        .operation_identity = receipt.operation_identity_,
        .expected_job_count = static_cast<std::uint64_t>(receipt.records_.size()),
        .consumed_count = static_cast<std::uint64_t>(receipt.consumed_count()),
        .accepted_count = static_cast<std::uint64_t>(receipt.accepted_count()),
        .burned_count = static_cast<std::uint64_t>(receipt.burned_count()),
        .query_pipeline_receipt_count = static_cast<std::uint64_t>(query.prepared_count),
        .shape_pipeline_receipt_count = static_cast<std::uint64_t>(shape.prepared_count),
        .alignment_pipeline_receipt_count = alignment_pipeline_receipt_count,
        .alignment_submission_count = alignment_submission_count,
        .mapq_pipeline_receipt_count = mapq_pipeline_receipt_count,
        .mapq_submission_count = mapq_submission_count,
        .mapq_result_count = mapq_present ? receipt.mapq_result_count_ : 0u,
        .mapq_published_count = mapq_present ? receipt.mapq_published_count_ : 0u,
        .mapq_result_digest = mapq_present ? receipt.mapq_result_digest_ : 0u,
        .committed_submission_count = *committed_submission_count,
        .seed_receipt_identity = query.seed_receipt_identity,
    };
    if (!result.environment.is_internally_consistent() ||
        result.environment != environment_preview ||
        !result.acceptance.is_internally_consistent() ||
        result.acceptance != expected_preview.acceptance) {
        return {};
    }
    result.valid = true;
    return result;
}

/// @brief Exercises finalizer rejection paths without consuming cache metric claims.
/// @param[in] base_audit Valid pipeline audit used to construct sealed test receipts.
/// @return `true` when every mutated identity is rejected without claiming metrics.
bool runtime::self_test_persisted_grouped_soa_finalizer_preclaim_contract(
    runtime_metal_archive_pipeline_audit base_audit) {
    if (!base_audit.is_internally_consistent() || base_audit.pipeline_create_ns == 0u) {
        return false;
    }
    const std::array<shader_variant_key, 12> keys{
        make_shader_key(shader_kind::ordered_sketch_tile, 384u, 64u, 10u),
        make_shader_key(shader_kind::prefix_scan_block, 384u, 1u, 1u),
        make_shader_key(shader_kind::prefix_scan_add, 384u, 128u, 1u),
        make_shader_key(
            shader_kind::grouped_soa_seed_count, 384u, 128u, k_grouped_soa_seed_cache_abi_revision),
        make_shader_key(
            shader_kind::grouped_soa_seed_emit, 384u, 128u, k_grouped_soa_seed_cache_abi_revision),
        make_shader_key(shader_kind::chain_range_build, 384u, 1u, 1u),
        make_shader_key(shader_kind::coordinate_reduce, 384u, 128u, 1u),
        make_shader_key(shader_kind::chain_tiled_dp_u64, 32u, 32u, 0u),
        make_shader_key(shader_kind::radix_sort_histogram, 384u, 1u, 1u),
        make_shader_key(shader_kind::radix_sort_scan, 384u, 1u, 1u),
        make_shader_key(shader_kind::radix_sort_scatter, 384u, 1u, 1u),
        make_shader_key(
            shader_kind::align_extend_simple_cigar, 384u, 64u, k_gpu_alignment_extension_max_len),
    };
    if (!std::all_of(keys.begin(), keys.end(), [](const shader_variant_key& key) {
            return is_shader_variant_key_valid(key);
        })) {
        return false;
    }
    std::array<runtime_metal_archive_pipeline_receipt, 12> receipts{};
    for (std::size_t i = 0u; i < receipts.size(); ++i) {
        runtime_metal_archive_pipeline_audit audit = base_audit;
        audit.receipt_identity = 8000u + i;
        audit.scope_identity = 8100u + i;
        audit.shader_key = keys[i];
        receipts[i] = runtime_metal_archive_pipeline_receipt{std::move(audit)};
        if (!receipts[i].is_valid())
            return false;
    }
    // Creates stable non-null command identities without allocating Vulkan objects.
    const auto command = [](const std::uintptr_t identity) {
        return reinterpret_cast<VkCommandBuffer>(identity);
    };

    runtime_pipeline_operation_token query{};
    runtime_pipeline_operation_token shape{};
    runtime_pipeline_operation_token alignment{};
    try {
        auto query_collector = runtime_pipeline_use_collector::create(
            runtime_pipeline_operation_kind::query_sketch, 1u, 101u);
        for (std::size_t i = 0u; i < 3u; ++i) {
            query_collector->prepare(keys[i], &receipts[i]);
        }
        query_collector->begin(command(1u));
        query_collector->use(command(1u), 0u, keys[0], &receipts[0], 3u);
        query_collector->use(command(1u), 0u, keys[1], &receipts[1], 2u);
        query_collector->use(command(1u), 0u, keys[2], &receipts[2], 2u);
        query_collector->commit(command(1u));
        query_collector->begin(command(2u));
        query_collector->use(command(2u), 0u, keys[0], &receipts[0], 3u);
        query_collector->use(command(2u), 0u, keys[2], &receipts[2], 1u);
        query_collector->commit(command(2u));
        query = query_collector->seal();

        auto shape_collector = runtime_pipeline_use_collector::create(
            runtime_pipeline_operation_kind::grouped_soa_coordinate, 1u, 202u);
        constexpr std::array<std::size_t, 7> shape_base_slots{3u, 4u, 1u, 2u, 5u, 6u, 7u};
        for (const std::size_t slot : shape_base_slots) {
            shape_collector->prepare(keys[slot], &receipts[slot]);
        }
        for (std::size_t slot = 8u; slot < 11u; ++slot) {
            shape_collector->prepare(keys[slot], &receipts[slot]);
        }
        shape_collector->set_shape_input(0u, 1u, 255u, 0u);
        shape_collector->set_shape_outcome(0u, 1u, 1u);

        shape_collector->begin(command(11u));
        shape_collector->use(command(11u), 0u, keys[3], &receipts[3], 1u);
        shape_collector->use(command(11u), 0u, keys[1], &receipts[1], 1u);
        shape_collector->use(command(11u), 0u, keys[2], &receipts[2], 1u);
        shape_collector->use(command(11u), 0u, keys[2], &receipts[2], 1u);
        shape_collector->commit(command(11u));

        shape_collector->begin(command(12u));
        shape_collector->use(command(12u), 0u, keys[4], &receipts[4], 1u);
        shape_collector->commit(command(12u));

        shape_collector->begin(command(13u));
        for (std::size_t slot = 8u; slot < 11u; ++slot) {
            shape_collector->use(command(13u), 0u, keys[slot], &receipts[slot], 1u);
            shape_collector->use(command(13u), 0u, keys[slot], &receipts[slot], 1u);
        }
        shape_collector->use(command(13u), 0u, keys[5], &receipts[5], 1u);
        shape_collector->commit(command(13u));

        shape_collector->begin(command(14u));
        shape_collector->use(command(14u), 0u, keys[7], &receipts[7], 1u);
        shape_collector->use(command(14u), 0u, keys[6], &receipts[6], 1u);
        shape_collector->commit(command(14u));
        shape = shape_collector->seal();

        auto alignment_collector = runtime_pipeline_use_collector::create(
            runtime_pipeline_operation_kind::persisted_alignment_cigar, 1u, 202u);
        alignment_collector->set_shape_input(0u, 20'000u, 20'000u, 9'000u);
        alignment_collector->set_shape_outcome(0u, 1u, 1u);
        alignment_collector->prepare(keys[11], &receipts[11]);
        alignment_collector->begin(command(21u));
        alignment_collector->use(command(21u), 0u, keys[11], &receipts[11], 1u);
        alignment_collector->commit(command(21u));
        alignment_collector->prepare(keys[11], &receipts[11]);
        alignment_collector->begin(command(22u));
        alignment_collector->use(command(22u), 0u, keys[11], &receipts[11], 1u);
        alignment_collector->commit(command(22u));
        alignment = alignment_collector->seal();
    } catch (...) {
        return false;
    }
    if (!query.is_valid_for_query_sketch() || !shape.is_valid_for_grouped_soa_coordinate() ||
        !alignment.is_valid_for_persisted_alignment_cigar() ||
        alignment.audit().operation_identity != 202u || alignment.audit().prepared_count != 1u ||
        alignment.audit().committed_submissions != 2u) {
        return false;
    }

    persisted_grouped_soa_coordinate_operation_receipt receipt{};
    receipt.operation_identity_ = 202u;
    try {
        receipt.admit_records(1u);
    } catch (...) {
        return false;
    }
    receipt.records_[0].state =
        persisted_grouped_soa_coordinate_operation_receipt::record_state::accepted;
    receipt.records_[0].audit.result.is_valid = 1u;
    receipt.accepted_count_ = 1u;
    receipt.query_pipeline_token_ = std::move(query);
    receipt.grouped_soa_pipeline_token_ = std::move(shape);
    receipt.alignment_pipeline_token_ = std::move(alignment);
    receipt.sealed_ = true;
    const persisted_grouped_soa_coordinate_operation_receipt_preview expected =
        receipt.operation_receipt_preview();
    const auto coordinate_submission_count = bioseq::common::checked_add(
        receipt.query_pipeline_token_.audit().committed_submissions,
        receipt.grouped_soa_pipeline_token_.audit().committed_submissions);
    if (!coordinate_submission_count.has_value()) {
        return false;
    }
    const auto committed_submission_count = bioseq::common::checked_add(
        *coordinate_submission_count,
        receipt.alignment_pipeline_token_.audit().committed_submissions);
    if (!receipt.are_all_jobs_accepted() || !expected.is_internally_consistent() ||
        expected.acceptance.shape_pipeline_receipt_count != 10u ||
        expected.acceptance.alignment_pipeline_receipt_count != 1u ||
        expected.acceptance.alignment_submission_count != 2u ||
        !committed_submission_count.has_value() ||
        expected.acceptance.committed_submission_count != *committed_submission_count) {
        return false;
    }
    vulkan_feature_set live_features{};
    live_features.device_uuid = expected.device_uuid;
    live_features.api_version = expected.api_version;
    live_features.driver_version = expected.driver_version;
    live_features.vendor_id = expected.vendor_id;
    live_features.device_id = expected.device_id;
    live_features.pipeline_cache_uuid = expected.pipeline_cache_uuid;
    bool seed_claimed = false;
    // Confirms that validation has not consumed any report-once claim.
    const auto claims_unchanged = [&receipts, &seed_claimed]() {
        return !seed_claimed &&
               std::none_of(receipts.begin(), receipts.end(), [](const auto& receipt) {
                   return receipt.metrics_claimed_;
               });
    };
    if (!claims_unchanged())
        return false;
    {
        auto accepted = prepare_persisted_grouped_soa_coordinate_finalization(
            receipt, expected, live_features, seed_claimed);
        if (!accepted.is_valid() || accepted.acceptance != expected.acceptance ||
            accepted.environment != receipt.environment_preview() ||
            accepted.cache_claim_plan.audit() != expected.maximum_cache_metrics ||
            accepted.cache_claim_plan.is_committed()) {
            return false;
        }
    }
    if (!claims_unchanged())
        return false;

    // Requires a candidate mismatch to fail before any claim is committed.
    const auto rejected = [&claims_unchanged, &receipt, &seed_claimed](
                              const auto& candidate, const vulkan_feature_set& live) {
        auto plan = prepare_persisted_grouped_soa_coordinate_finalization(
            receipt, candidate, live, seed_claimed);
        return !plan.is_valid() && claims_unchanged();
    };
    // Applies one mutation to the sealed caller preview.
    const auto rejected_preview = [&expected, &live_features, &rejected](auto mutate) {
        auto candidate = expected;
        mutate(candidate);
        return rejected(candidate, live_features);
    };
    // Applies one mutation to the live device identity.
    const auto rejected_live = [&expected, &live_features, &rejected](auto mutate) {
        auto candidate = live_features;
        mutate(candidate);
        return rejected(expected, candidate);
    };

    // Every fixed field crossing the SQL-preview/finalizer equality gate gets
    // an independent mutation. Rejection must leave all report-once bits clear.
    if (!rejected_preview([](auto& value) {
            ++value.acceptance.operation_identity;
            value.shape_pipeline_operation_identity = value.acceptance.operation_identity;
            value.maximum_cache_metrics.token_identity = value.acceptance.operation_identity;
        }) ||
        !rejected_preview([](auto& value) { ++value.acceptance.expected_job_count; }) ||
        !rejected_preview([](auto& value) { ++value.acceptance.consumed_count; }) ||
        !rejected_preview([](auto& value) { ++value.acceptance.accepted_count; }) ||
        !rejected_preview([](auto& value) { value.acceptance.burned_count = 1u; }) ||
        !rejected_preview([](auto& value) { ++value.acceptance.query_pipeline_receipt_count; }) ||
        !rejected_preview([](auto& value) { --value.acceptance.shape_pipeline_receipt_count; }) ||
        !rejected_preview([](auto& value) {
            value.acceptance.committed_submission_count -=
                value.acceptance.alignment_submission_count;
            value.acceptance.alignment_pipeline_receipt_count = 0u;
            value.acceptance.alignment_submission_count = 0u;
        }) ||
        !rejected_preview([](auto& value) {
            ++value.acceptance.alignment_submission_count;
            ++value.acceptance.committed_submission_count;
        }) ||
        !rejected_preview([](auto& value) { ++value.acceptance.committed_submission_count; }) ||
        !rejected_preview([](auto& value) { ++value.acceptance.seed_receipt_identity; }) ||
        !rejected_preview([](auto& value) { ++value.query_pipeline_operation_identity; }) ||
        !rejected_preview([](auto& value) { ++value.shape_pipeline_operation_identity; }) ||
        !rejected_preview([](auto& value) { ++value.query_submission_ordinals[0]; }) ||
        !rejected_preview([](auto& value) { ++value.query_submission_ordinals[1]; }) ||
        !rejected_preview([](auto& value) { value.device_uuid[0] ^= 0x80u; }) ||
        !rejected_preview([](auto& value) { ++value.api_version; }) ||
        !rejected_preview([](auto& value) { ++value.driver_version; }) ||
        !rejected_preview([](auto& value) { ++value.vendor_id; }) ||
        !rejected_preview([](auto& value) { ++value.device_id; }) ||
        !rejected_preview([](auto& value) { value.pipeline_cache_uuid[0] ^= 0x80u; }) ||
        !rejected_preview([](auto& value) { value.seed_sha256[0] ^= 0x80u; }) ||
        !rejected_preview([](auto& value) { ++value.seed_size; }) ||
        !rejected_preview([](auto& value) { ++value.manifest_checksum; }) ||
        !rejected_preview([](auto& value) { ++value.maximum_cache_metrics.token_identity; }) ||
        !rejected_preview([](auto& value) {
            value.maximum_cache_metrics.gpu_pipeline_cache_loads = 0u;
            value.maximum_cache_metrics.gpu_pipeline_cache_seed_bytes = 0u;
            value.maximum_cache_metrics.gpu_pipeline_cache_load_ns = 0u;
        }) ||
        !rejected_preview(
            [](auto& value) { ++value.maximum_cache_metrics.gpu_pipeline_cache_seed_bytes; }) ||
        !rejected_preview(
            [](auto& value) { ++value.maximum_cache_metrics.gpu_pipeline_cache_load_ns; }) ||
        !rejected_preview(
            [](auto& value) { ++value.maximum_cache_metrics.gpu_pipeline_creates; }) ||
        !rejected_preview(
            [](auto& value) { ++value.maximum_cache_metrics.gpu_pipeline_creation_ns; }) ||
        !rejected_preview([](auto& value) { value.maximum_cache_metrics.saturated = true; })) {
        return false;
    }

    if (!rejected_live([](auto& value) { value.device_uuid[0] ^= 0x40u; }) ||
        !rejected_live([](auto& value) { ++value.api_version; }) ||
        !rejected_live([](auto& value) { ++value.driver_version; }) ||
        !rejected_live([](auto& value) { ++value.vendor_id; }) ||
        !rejected_live([](auto& value) { ++value.device_id; }) ||
        !rejected_live([](auto& value) { value.pipeline_cache_uuid[0] ^= 0x40u; })) {
        return false;
    }

    // Mutates one retained pipeline receipt while preserving its standalone validity.
    const auto rejected_receipt =
        [&claims_unchanged, &expected, &live_features, &receipts, &rejected](auto mutate) {
            runtime_metal_archive_pipeline_audit saved = receipts[10].audit_;
            mutate(receipts[10].audit_);
            // Receipt mutations exercise the finalizer's frozen PREPARED/USED
            // equality gate, not the qualification's own self-consistency gate.
            // Keep the process qualification bound to the mutated live audit so
            // the receipt remains independently accepted before the finalizer
            // rejects its drift from the sealed operation receipt.
            runtime_metal_archive_process_qualification& qualification =
                receipts[10].audit_.cache_qualification;
            qualification.archive_set = receipts[10].audit_.expected_archives;
            qualification.vulkan_device_uuid = receipts[10].audit_.runtime_vulkan_device_uuid;
            qualification.api_version = receipts[10].audit_.api_version;
            qualification.driver_version = receipts[10].audit_.driver_version;
            qualification.vendor_id = receipts[10].audit_.vendor_id;
            qualification.device_id = receipts[10].audit_.device_id;
            qualification.pipeline_cache_uuid = receipts[10].audit_.pipeline_cache_uuid;
            qualification.seed_receipt_identity = receipts[10].audit_.seed_receipt_identity;
            qualification.seed_sha256 = receipts[10].audit_.seed_sha256;
            qualification.seed_size = receipts[10].audit_.seed_size;
            qualification.manifest_checksum = receipts[10].audit_.manifest_checksum;
            qualification.identity =
                runtime_metal_archive_process_qualification_identity(qualification);
            const bool remained_valid = receipts[10].is_valid();
            const bool result = remained_valid && rejected(expected, live_features);
            receipts[10].audit_ = std::move(saved);
            return result && claims_unchanged();
        };
    // Changes a nonzero scalar without wrapping it to zero.
    const auto change_nonzero = [](auto& value) {
        using value_type = std::remove_reference_t<decltype(value)>;
        value = value == (std::numeric_limits<value_type>::max)() ? value - 1u : value + 1u;
    };
    // Changes the first identity byte while keeping it nonzero.
    const auto change_nonzero_byte = [](auto& bytes) {
        bytes[0] = bytes[0] == 0xffu ? static_cast<std::uint8_t>(0xfeu)
                                     : static_cast<std::uint8_t>(bytes[0] + 1u);
    };
    if (!rejected_receipt(
            [&change_nonzero](auto& audit) { change_nonzero(audit.receipt_identity); }) ||
        !rejected_receipt([&keys](auto& audit) { audit.shader_key = keys[0]; }) ||
        !rejected_receipt([&change_nonzero_byte](auto& audit) {
            change_nonzero_byte(audit.configured_vulkan_device_uuid);
            audit.runtime_vulkan_device_uuid = audit.configured_vulkan_device_uuid;
        }) ||
        !rejected_receipt([&change_nonzero](auto& audit) { change_nonzero(audit.api_version); }) ||
        !rejected_receipt(
            [&change_nonzero](auto& audit) { change_nonzero(audit.driver_version); }) ||
        !rejected_receipt([&change_nonzero](auto& audit) { change_nonzero(audit.vendor_id); }) ||
        !rejected_receipt([&change_nonzero](auto& audit) { change_nonzero(audit.device_id); }) ||
        !rejected_receipt([&change_nonzero_byte](auto& audit) {
            change_nonzero_byte(audit.pipeline_cache_uuid);
        }) ||
        !rejected_receipt(
            [&change_nonzero_byte](auto& audit) { change_nonzero_byte(audit.seed_sha256); }) ||
        !rejected_receipt([&change_nonzero](auto& audit) {
            change_nonzero(audit.seed_size);
            audit.seed_artifact_bytes = audit.seed_size;
        }) ||
        !rejected_receipt(
            [&change_nonzero](auto& audit) { change_nonzero(audit.manifest_checksum); }) ||
        !rejected_receipt(
            [&change_nonzero](auto& audit) { change_nonzero(audit.seed_receipt_identity); }) ||
        !rejected_receipt([&change_nonzero](auto& audit) {
            change_nonzero(audit.expected_archives.ordered_checksum);
            audit.loaded_archives = audit.expected_archives;
        }) ||
        !rejected_receipt(
            [&change_nonzero](auto& audit) { change_nonzero(audit.pipeline_create_ns); })) {
        return false;
    }
    return claims_unchanged();
}

/// @brief Exercises partial cache-metric deltas and their one-shot claim behavior.
/// @param[in] base_audit Valid pipeline audit used to create independent receipts.
/// @return `true` when partial and zero deltas obey their one-shot contracts.
bool runtime::self_test_gpu_cache_partial_delta_contract(
    runtime_metal_archive_pipeline_audit base_audit) {
    if (!base_audit.is_internally_consistent() || base_audit.pipeline_create_ns == 0u) {
        return false;
    }
    const std::array<shader_variant_key, 3> keys{
        make_shader_key(shader_kind::ordered_sketch_tile, 384u, 64u, 10u),
        make_shader_key(shader_kind::prefix_scan_block, 384u, 1u, 1u),
        make_shader_key(shader_kind::prefix_scan_add, 384u, 128u, 1u),
    };
    if (!std::all_of(keys.begin(), keys.end(), [](const shader_variant_key& key) {
            return is_shader_variant_key_valid(key);
        })) {
        return false;
    }
    std::array<runtime_metal_archive_pipeline_receipt, 3> receipts{};
    for (std::size_t i = 0u; i < receipts.size(); ++i) {
        runtime_metal_archive_pipeline_audit audit = base_audit;
        audit.receipt_identity = 7000u + i;
        audit.scope_identity = 7100u + i;
        audit.shader_key = keys[i];
        receipts[i] = runtime_metal_archive_pipeline_receipt{std::move(audit)};
        if (!receipts[i].is_valid())
            return false;
    }
    std::array<const runtime_metal_archive_pipeline_receipt*, 3> pointers{};
    runtime_gpu_cache_metrics_audit maximum{
        .token_identity = 7200u,
        .gpu_pipeline_cache_loads = 1u,
        .gpu_pipeline_cache_seed_bytes = receipts[0].audit().seed_size,
        .gpu_pipeline_cache_load_ns = receipts[0].audit().seed_load_ns,
        .gpu_pipeline_creates = static_cast<std::uint64_t>(receipts.size()),
    };
    for (std::size_t i = 0u; i < receipts.size(); ++i) {
        pointers[i] = &receipts[i];
        if (receipts[i].audit().pipeline_create_ns >
            (std::numeric_limits<std::uint64_t>::max)() - maximum.gpu_pipeline_creation_ns) {
            return false;
        }
        maximum.gpu_pipeline_creation_ns += receipts[i].audit().pipeline_create_ns;
    }
    if (!maximum.is_internally_consistent())
        return false;

    bool seed_claimed = false;
    runtime_gpu_cache_metrics_audit subset_maximum{
        .token_identity = 7199u,
        .gpu_pipeline_cache_loads = 1u,
        .gpu_pipeline_cache_seed_bytes = receipts[0].audit().seed_size,
        .gpu_pipeline_cache_load_ns = receipts[0].audit().seed_load_ns,
        .gpu_pipeline_creates = 1u,
        .gpu_pipeline_creation_ns = receipts[0].audit().pipeline_create_ns,
    };
    auto subset = runtime_detail::runtime_gpu_cache_metrics_claim_plan::prepare(
        subset_maximum.token_identity,
        subset_maximum,
        seed_claimed,
        std::span<const runtime_metal_archive_pipeline_receipt* const>{pointers.data(), 1u});
    if (!subset.is_valid() || subset.audit() != subset_maximum) {
        return false;
    }
    subset.commit(seed_claimed);
    if (!subset.is_committed() || !seed_claimed || !receipts[0].metrics_claimed_ ||
        receipts[1].metrics_claimed_ || receipts[2].metrics_claimed_) {
        return false;
    }

    auto remainder = runtime_detail::runtime_gpu_cache_metrics_claim_plan::prepare(
        maximum.token_identity,
        maximum,
        seed_claimed,
        std::span<const runtime_metal_archive_pipeline_receipt* const>{pointers.data(),
                                                                       pointers.size()});
    if (!remainder.is_valid())
        return false;
    const runtime_gpu_cache_metrics_audit partial = remainder.audit();
    if (!partial.is_componentwise_bounded_by(maximum) || partial == maximum ||
        partial.gpu_pipeline_cache_loads != 0u || partial.gpu_pipeline_cache_seed_bytes != 0u ||
        partial.gpu_pipeline_cache_load_ns != 0u || partial.gpu_pipeline_creates != 2u ||
        partial.gpu_pipeline_creation_ns == 0u) {
        return false;
    }
    remainder.commit(seed_claimed);
    if (!remainder.is_committed() ||
        std::any_of(receipts.begin(), receipts.end(), [](const auto& receipt) {
            return !receipt.metrics_claimed_;
        })) {
        return false;
    }

    runtime_gpu_cache_metrics_delta issued{};
    issued.audit_ = partial;
    issued.issued_ = true;
    runtime_gpu_cache_metrics_delta moved{std::move(issued)};
    if (issued.is_valid() || issued.audit() != runtime_gpu_cache_metrics_audit{} ||
        !moved.is_valid() || moved.audit() != partial) {
        return false;
    }
    moved.consume();
    if (moved.is_valid() || moved.audit() != runtime_gpu_cache_metrics_audit{}) {
        return false;
    }

    runtime_gpu_cache_metrics_audit later_maximum = maximum;
    later_maximum.token_identity = 7201u;
    auto later = runtime_detail::runtime_gpu_cache_metrics_claim_plan::prepare(
        later_maximum.token_identity,
        later_maximum,
        seed_claimed,
        std::span<const runtime_metal_archive_pipeline_receipt* const>{pointers.data(),
                                                                       pointers.size()});
    const runtime_gpu_cache_metrics_audit zero = later.audit();
    if (!later.is_valid() || zero.token_identity != later_maximum.token_identity ||
        zero.gpu_pipeline_cache_loads != 0u || zero.gpu_pipeline_cache_seed_bytes != 0u ||
        zero.gpu_pipeline_cache_load_ns != 0u || zero.gpu_pipeline_creates != 0u ||
        zero.gpu_pipeline_creation_ns != 0u || zero.saturated) {
        return false;
    }
    later.commit(seed_claimed);
    if (!later.is_committed())
        return false;
    runtime_gpu_cache_metrics_delta zero_delta{};
    zero_delta.audit_ = zero;
    zero_delta.issued_ = true;
    if (!zero_delta.is_valid())
        return false;
    zero_delta.consume();
    return !zero_delta.is_valid() && zero_delta.audit() == runtime_gpu_cache_metrics_audit{};
}

/// @brief Validates and commits one persisted grouped-SoA coordinate receipt.
/// @param[in,out] receipt Rvalue receipt cleared after successful publication.
/// @param[in] expected_preview Caller-provided acceptance preview that must match exactly.
/// @return Single-use accepted operation carrying environment and cache-metric audits.
/// @throws vulkan_runtime_error When validation or the final live policy gate fails.
persisted_grouped_soa_coordinate_accepted_operation
runtime::finalize_persisted_grouped_soa_coordinate_operation(
    persisted_grouped_soa_coordinate_operation_receipt&& receipt,
    const persisted_grouped_soa_coordinate_operation_receipt_preview& expected_preview) {
    std::scoped_lock lock(mu_);
    auto plan = prepare_persisted_grouped_soa_coordinate_finalization(
        receipt, expected_preview, features_, seed_metrics_claimed_);
    if (!plan.is_valid()) {
        throw vulkan_runtime_error(
            "persisted grouped-SoA operation finalization payload is invalid");
    }

    // This is deliberately the last external-state check. Everything after it
    // is fixed-field move/copy construction and cannot dispatch or allocate.
    require_immutable_metal_policy_live("persisted grouped-SoA operation final publication");

    // No failing work follows this mutation. The queue/cache mutex makes the
    // seed and receipt claims one transaction with authority minting.
    const runtime_gpu_cache_metrics_audit cache_metrics = plan.cache_claim_plan.audit();
    plan.cache_claim_plan.commit(seed_metrics_claimed_);
    persisted_grouped_soa_coordinate_accepted_operation accepted{};
    accepted.audit_ = plan.acceptance;
    accepted.environment_ = plan.environment;
    accepted.cache_metrics_.audit_ = cache_metrics;
    accepted.cache_metrics_.issued_ = true;
    accepted.issued_ = true;
    receipt = persisted_grouped_soa_coordinate_operation_receipt{};
    return accepted;
}

/// @brief Runs a coordinate batch and returns only its host-visible result wires.
/// @param[in] query_inputs Shared minimizer storage for the synthetic single query record.
/// @param[in] jobs Ordered coordinate jobs.
/// @param[in] query_len Query length in symbols.
/// @param[in] kmer_size K-mer span encoded into anchors.
/// @param[in] max_occ Maximum target occurrences emitted per minimizer.
/// @param[in] coordinate_algorithm Tiled-DP or diagonal-vote reduction.
/// @param[in] chain_key Chaining pipeline specialization.
/// @param[in] max_dist_x Maximum reference-coordinate chaining distance.
/// @param[in] max_dist_y Maximum query-coordinate chaining distance.
/// @param[in] bandwidth Chaining diagonal bandwidth.
/// @param[in] max_iter Maximum predecessors examined per anchor.
/// @param[in] gap_penalty Chaining gap penalty.
/// @param[in] skip_penalty Chaining skip penalty.
/// @param[in] max_chain_skip Maximum consecutive unproductive predecessors.
/// @param[in] min_chain_score Minimum accepted chain score.
/// @param[in] min_seed_count Minimum accepted seed count.
/// @param[in] probe_limit Maximum coordinate candidates examined.
/// @return One host-visible coordinate result per job.
/// @throws vulkan_runtime_error When validation or batched execution fails.
std::vector<gpu_coordinate_result_wire> runtime::run_seed_to_coordinate_results_roundtrip(
    const gpu_seed_query_inputs& query_inputs,
    std::span<const gpu_seed_coordinate_job> jobs,
    std::uint64_t query_len,
    std::uint32_t kmer_size,
    std::int32_t max_occ,
    gpu_seed_coordinate_algorithm coordinate_algorithm,
    const chaining_pipeline_key& chain_key,
    std::int32_t max_dist_x,
    std::int32_t max_dist_y,
    std::int32_t bandwidth,
    std::int32_t max_iter,
    float gap_penalty,
    float skip_penalty,
    std::int32_t max_chain_skip,
    std::int32_t min_chain_score,
    std::int32_t min_seed_count,
    std::uint32_t probe_limit) {
    if (query_len > std::numeric_limits<std::uint32_t>::max()) {
        throw vulkan_runtime_error("gpu seed-to-coordinate query length exceeds ABI");
    }
    const gpu_seed_query_record_wire qrec{
        .minimizer_offset = 0u,
        .minimizer_count = query_inputs.minimizer_count,
        .query_len = static_cast<std::uint32_t>(query_len),
        .flags = 0u,
    };
    gpu_seed_query_inputs forwarded = query_inputs;
    forwarded.host_query_records = std::span<const gpu_seed_query_record_wire>{&qrec, 1u};
    forwarded.device_query_records = nullptr;
    return run_seed_to_coordinate_batch_roundtrip(forwarded,
                                                  jobs,
                                                  kmer_size,
                                                  max_occ,
                                                  coordinate_algorithm,
                                                  chain_key,
                                                  max_dist_x,
                                                  max_dist_y,
                                                  bandwidth,
                                                  max_iter,
                                                  gap_penalty,
                                                  skip_penalty,
                                                  max_chain_skip,
                                                  min_chain_score,
                                                  min_seed_count,
                                                  probe_limit)
        .results;
}

} // namespace bioseq::gpu::vulkan
