/**
 * @file typed_support_parts/vulkan_target_routes.cppm
 * @brief Adapts MPF and MPS Vulkan seed results to the shared host suffix.
 */
module;

extern "C" {
#include "postgres.h"
#include "miscadmin.h"
#include "utils/errcodes.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <fmt/format.h>
#include <new>

export module bioseq.postgres.scheduler.dispatch_processor.typed_support.vulkan_target_routes;

import std;
import bioseq.common.checked_arithmetic;
import bioseq.common.context;
import bioseq.common.logging;
import bioseq.alignment.contracts;
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_sequence;
import bioseq.indexing.gpu_minimizer_index;
import bioseq.sequences;
import bioseq.sequences.kmers.minimizer_physical_registry;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.seeding.vulkan;
import bioseq.pipeline.minimap_port;
import bioseq.pipeline.minimap_port.match_result_contract;
import bioseq.pipeline.pipe;
import bioseq.gpu.vulkan.runtime;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
import bioseq.gpu.vulkan.shader_compiler;
#endif
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.parallelism.memory_admission;
import bioseq.postgres.scheduler.dispatch_processor.typed_support.policy_stats;
import bioseq.postgres.scheduler.dispatch_processor.typed_support.pg_alignment_errors;
import bioseq.postgres.scheduler.dispatch_processor.typed_support.transient_storage;
import bioseq.postgres.scheduler.dispatch_processor.typed_support.vulkan_query_route;
import bioseq.postgres.support;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
import bioseq.postgres.storage.indexing.minimizer_read_stats;
#endif

export namespace bioseq::simd::typed_dispatch::typed_support_detail {

/** Logical target representation selected by the active read policy. */
enum class gpu_seed_target_layout : std::uint8_t {
    invalid = 0,
    minimizers_per_fragment,
    minimizers_per_sequence,
};

/** Physical owner topology selected independently of campaign policy flags. */
enum class vulkan_seed_target_ownership : std::uint8_t {
    invalid = 0,
    shared_mpf,
    shared_mps,
    ordinary_mpf,
    ordinary_mps,
};

/**
 * Authenticate one and only one owner topology for the selected target layout.
 *
 * Shared BatchJoin owners and the ordinary per-row preparation hook are
 * mutually exclusive.  This classifier intentionally has no benchmark or
 * campaign input: an ordinary SQL scan cannot be disabled by campaign shape.
 */
[[nodiscard]] constexpr vulkan_seed_target_ownership
classify_vulkan_seed_target_ownership(
    const gpu_seed_target_layout layout,
    const bool shared_mpf_ready,
    const bool shared_mps_ready,
    const bool ordinary_hook_ready) noexcept {
    const unsigned owner_count = static_cast<unsigned>(shared_mpf_ready) +
                                 static_cast<unsigned>(shared_mps_ready) +
                                 static_cast<unsigned>(ordinary_hook_ready);
    if (owner_count != 1u) {
        return vulkan_seed_target_ownership::invalid;
    }
    switch (layout) {
    case gpu_seed_target_layout::minimizers_per_fragment:
        if (shared_mpf_ready) {
            return vulkan_seed_target_ownership::shared_mpf;
        }
        return ordinary_hook_ready ? vulkan_seed_target_ownership::ordinary_mpf
                                   : vulkan_seed_target_ownership::invalid;
    case gpu_seed_target_layout::minimizers_per_sequence:
        if (shared_mps_ready) {
            return vulkan_seed_target_ownership::shared_mps;
        }
        return ordinary_hook_ready ? vulkan_seed_target_ownership::ordinary_mps
                                   : vulkan_seed_target_ownership::invalid;
    case gpu_seed_target_layout::invalid:
        return vulkan_seed_target_ownership::invalid;
    }
    return vulkan_seed_target_ownership::invalid;
}

static_assert(classify_vulkan_seed_target_ownership(
                  gpu_seed_target_layout::minimizers_per_fragment,
                  true,
                  false,
                  false) == vulkan_seed_target_ownership::shared_mpf);
static_assert(classify_vulkan_seed_target_ownership(
                  gpu_seed_target_layout::minimizers_per_sequence,
                  false,
                  false,
                  true) == vulkan_seed_target_ownership::ordinary_mps);
static_assert(classify_vulkan_seed_target_ownership(
                  gpu_seed_target_layout::minimizers_per_fragment,
                  true,
                  false,
                  true) == vulkan_seed_target_ownership::invalid);

/** Resolve MPF/MPS without accepting aliases or inferring from owner shape. */
[[nodiscard]] constexpr gpu_seed_target_layout
classify_gpu_seed_target_layout(const std::string_view index_name) noexcept {
    if (index_name == bioseq::indexing::k_minimizers_per_fragment_index_name) {
        return gpu_seed_target_layout::minimizers_per_fragment;
    }
    if (index_name == bioseq::indexing::k_minimizers_per_sequence_index_name) {
        return gpu_seed_target_layout::minimizers_per_sequence;
    }
    return gpu_seed_target_layout::invalid;
}

static_assert(classify_gpu_seed_target_layout(
                  bioseq::indexing::k_minimizers_per_fragment_index_name) ==
              gpu_seed_target_layout::minimizers_per_fragment);
static_assert(classify_gpu_seed_target_layout(
                  bioseq::indexing::k_minimizers_per_sequence_index_name) ==
              gpu_seed_target_layout::minimizers_per_sequence);

/** Return true only for aliases of the exact immutable statement control block. */
[[nodiscard]] inline bool same_statement_owner(
    const pipe::statement_execution_plan_owner& lhs,
    const pipe::statement_execution_plan_owner& rhs) noexcept {
    return lhs && rhs && lhs.get() == rhs.get() && !lhs.owner_before(rhs) &&
           !rhs.owner_before(lhs);
}

/** Append one completed collection while retaining scheduler query-id order. */
inline void append_canonical_results(
    match_result_map& results,
    const std::int32_t query_id,
    match_result_collection& collection) {
    auto [entry, inserted] = results.try_emplace(query_id);
    auto& destination = entry->second;
    if (inserted || destination.empty()) {
        destination = std::move(collection.results);
        return;
    }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    match_result_collection admitted_destination{std::move(destination)};
    admitted_destination.prepare_result_append(collection.results.size());
    for (auto& result : collection.results) {
        admitted_destination.results.push_back(std::move(result));
    }
    destination = std::move(admitted_destination.results);
#else
    destination.insert(destination.end(),
                       std::make_move_iterator(collection.results.begin()),
                       std::make_move_iterator(collection.results.end()));
#endif
    collection.results.clear();
}

/** One completed suffix job retained until every job in its GPU-seed wave succeeds. */
struct canonical_suffix_row_completion {
    std::size_t row = 0u;
    std::int32_t query_id = 0;
    ::bioseq::pipeline_stats::target_index_source actual_source =
        ::bioseq::pipeline_stats::target_index_source::none;
#if defined(BIOSEQ_TEST_INDEXES) && !defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    std::uint64_t seed_lookup_dispatches = 0u;
#endif
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    vulkan_seed_frontend_job_execution seed_frontend{};
#endif
    match_result_collection collection{};
};

/**
 * One ordinary row's private Vulkan proof set, retained until ROOT commits the
 * wave's result publication.  Owners are declared before one-shot authorities
 * so reverse destruction always releases tokens before their backing objects.
 */
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
struct ordinary_vulkan_row_proof {
    std::shared_ptr<const void> query_sketch_owner{};
    std::shared_ptr<const void> target_owner{};
    bioseq::gpu::vulkan::runtime_sketch_dispatch_stats query_sketch_dispatch_stats{};
    vulkan_seed_target_execution target_execution{};
    bioseq::storage::indexing::minimizer_generation_read_stats persisted_read_stats{};
    bioseq::gpu::vulkan::device_query_sketch_authorization mpf_query_authorization{};
    bioseq::gpu::vulkan::runtime_pipeline_operation_token query_operation{};
    bioseq::gpu::vulkan::runtime_pipeline_operation_token seed_operation{};
    bioseq::gpu::vulkan::runtime_pipeline_operation_token chaining_operation{};
    vulkan_seed_target_ownership ownership = vulkan_seed_target_ownership::invalid;
    std::size_t row = 0u;
    std::int64_t target_rowno = -1;
    std::uint64_t target_bp_size = 0u;
    std::uint64_t target_position_offset = 0u;
    std::uint64_t target_operation_identity = 0u;
    std::uint64_t target_generation_id = 0u;
    std::uint64_t target_source_identity = 0u;
    std::uint64_t target_cold_load_identity = 0u;
    bool target_persisted = false;
    bool target_canonical_empty = false;
};
#endif

/** Completed ordinary-route state that remains private through result commit. */
struct ordinary_vulkan_scan_route_completion {
    std::uint64_t query_sketch_host_ns = 0u;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    bioseq::parallelism::memory_permit row_proofs_permit{};
    std::vector<ordinary_vulkan_row_proof> row_proofs{};
#endif
};

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
/** Validate that an ordinary target carries exactly the source-owned read class. */
[[nodiscard]] constexpr bool ordinary_persisted_read_receipt_matches_source(
    const bioseq::storage::indexing::minimizer_generation_read_stats& read,
    const bool persisted) noexcept {
    if (persisted) {
        return read.relation_opens == 1u && read.head_scans == 1u &&
               read.head_rows_read == 1u && read.manifest_directory_scans == 1u &&
               read.manifest_rows_read == 1u && read.source_parent_rows_read == 1u &&
               read.source_catalog_rows_read == 1u &&
               read.source_catalog_index_scans == 1u;
    }
    return read.relation_opens == 0u && read.index_scans == 0u &&
           read.head_scans == 0u && read.manifest_directory_scans == 0u &&
           read.total_heap_rows_read == 0u && read.total_bytes_read == 0u &&
           read.source_parent_rows_read == 0u && read.source_catalog_rows_read == 0u &&
           read.source_catalog_index_scans == 0u;
}
static_assert(ordinary_persisted_read_receipt_matches_source(
    bioseq::storage::indexing::minimizer_generation_read_stats{}, false));
static_assert([] {
    bioseq::storage::indexing::minimizer_generation_read_stats read{};
    read.relation_opens = 1u;
    read.head_scans = 1u;
    read.head_rows_read = 1u;
    read.manifest_directory_scans = 1u;
    read.manifest_rows_read = 1u;
    read.source_parent_rows_read = 1u;
    read.source_catalog_rows_read = 1u;
    read.source_catalog_index_scans = 1u;
    return ordinary_persisted_read_receipt_matches_source(read, true) &&
           !ordinary_persisted_read_receipt_matches_source(read, false);
}());
#endif

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
/** Project an authenticated MPF completion without moving its causal token. */
[[nodiscard]] inline vulkan_seed_frontend_job_execution
make_mpf_seed_frontend_execution(
    const bioseq::seeding::vulkan_seed_lookup_decline& decline,
    const bioseq::gpu::vulkan::runtime_pipeline_operation_token& operation,
    const std::uint64_t query_authority_identity,
    const std::uint32_t query_ordinal,
    const std::uint64_t minimizer_count,
    const std::uint64_t target_len,
    const std::uint64_t anchor_count,
    const vulkan_seed_target_completion_kind target_completion,
    const std::uint64_t target_operation_identity,
    const std::uint64_t persisted_generation_id,
    const std::uint64_t persisted_source_identity,
    const bool canonical_empty_target) {
    partial_vulkan_execution_stats seed{};
    seed.seed_lookup_dispatches = decline.seed_lookup_dispatches;
    seed.seed_open_addressed_lookup_dispatches =
        decline.open_addressed_lookup_dispatches;
    seed.seed_sorted_dense_lookup_dispatches = decline.sorted_dense_lookup_dispatches;
    seed.seed_bitonic_sort_dispatches = decline.sort_stats.bitonic_sort_dispatches;
    seed.seed_radix_sort_operations = decline.sort_stats.radix_sort_operations;
    seed.seed_radix_anchor_count = decline.sort_stats.radix_anchor_count;
    seed.seed_radix_passes = decline.sort_stats.radix_passes;
    seed.seed_radix_histogram_dispatches =
        decline.sort_stats.radix_histogram_dispatches;
    seed.seed_radix_scan_dispatches = decline.sort_stats.radix_scan_dispatches;
    seed.seed_radix_scatter_dispatches = decline.sort_stats.radix_scatter_dispatches;
    seed.seed_host_sort_fallbacks = decline.sort_stats.host_sort_fallbacks;
    const bool audited = decline.canonical_anchor_order_checked;
    seed.seed_anchor_stats_operations = audited || canonical_empty_target ? 1u : 0u;
    seed.seed_canonical_anchor_count = canonical_empty_target
        ? 0u : decline.canonical_anchor_count;
    seed.seed_canonical_anchor_order_checks = audited || canonical_empty_target ? 1u : 0u;
    seed.seed_canonical_anchor_order_accepts =
        canonical_empty_target ? 1u : decline.canonical_anchor_order_accepted ? 1u : 0u;
    seed.seed_canonical_tie_normalization_runs =
        decline.canonical_tie_normalization_runs;
    seed.seed_canonical_tie_normalization_anchors =
        decline.canonical_tie_normalization_anchors;
    seed.seed_anchor_digest_lane0 = canonical_empty_target
        ? bioseq::gpu::vulkan::k_seed_anchor_digest_seed_low
        : decline.canonical_anchor_digest_lo;
    seed.seed_anchor_digest_lane1 = canonical_empty_target
        ? bioseq::gpu::vulkan::k_seed_anchor_digest_seed_high
        : decline.canonical_anchor_digest_hi;
    const bool zero_query = minimizer_count == 0u;
    const auto completion = zero_query
        ? vulkan_seed_frontend_completion_kind::mpf_zero_query
        : canonical_empty_target
            ? vulkan_seed_frontend_completion_kind::mpf_empty_target
            : vulkan_seed_frontend_completion_kind::mpf_runtime;
    const std::uint64_t operation_identity =
        completion == vulkan_seed_frontend_completion_kind::mpf_runtime
            ? operation.audit().operation_identity : 0u;
    return {
        .completion = completion,
        .seed_stats = seed,
        .operation_identity = operation_identity,
        .query_authority_identity = query_authority_identity,
        .query_ordinal = query_ordinal,
        .minimizer_count = minimizer_count,
        .target_len = target_len,
        .target_position_offset = 0u,
        .anchor_count = anchor_count,
        .target_completion = target_completion,
        .target_operation_identity = target_operation_identity,
        .persisted_generation_id = persisted_generation_id,
        .persisted_source_identity = persisted_source_identity,
        .operation_job_index = 0u,
    };
}

/** Project one MPS job from the completed shared runtime operation audit. */
[[nodiscard]] inline vulkan_seed_frontend_job_execution
make_mps_seed_frontend_execution(
    const bioseq::gpu::vulkan::runtime_pipeline_operation_token& operation,
    const std::size_t job_index,
    const std::uint32_t query_ordinal,
    const vulkan_seed_target_completion_kind target_completion,
    const std::uint64_t target_operation_identity,
    const std::uint64_t persisted_generation_id,
    const std::uint64_t persisted_source_identity) {
    if (job_index >= operation.audit().jobs.size()) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "MPS seed proof has no matching runtime job");
    }
    const auto& job = operation.audit().jobs[job_index];
    partial_vulkan_execution_stats seed{};
    seed.seed_lookup_dispatches =
        job.count(bioseq::gpu::vulkan::shader_kind::grouped_soa_seed_count);
    seed.seed_sorted_dense_lookup_dispatches = seed.seed_lookup_dispatches;
    seed.seed_bitonic_sort_dispatches =
        job.count(bioseq::gpu::vulkan::shader_kind::sort_anchors_bitonic);
    seed.seed_radix_histogram_dispatches =
        job.count(bioseq::gpu::vulkan::shader_kind::radix_sort_histogram);
    seed.seed_radix_scan_dispatches =
        job.count(bioseq::gpu::vulkan::shader_kind::radix_sort_scan);
    seed.seed_radix_scatter_dispatches =
        job.count(bioseq::gpu::vulkan::shader_kind::radix_sort_scatter);
    seed.seed_radix_sort_operations = seed.seed_radix_histogram_dispatches != 0u ? 1u : 0u;
    seed.seed_radix_anchor_count =
        seed.seed_radix_sort_operations != 0u ? job.anchor_count : 0u;
    seed.seed_radix_passes = seed.seed_radix_histogram_dispatches;
    seed.seed_anchor_stats_operations = 1u;
    seed.seed_canonical_anchor_count = job.anchor_count;
    seed.seed_canonical_anchor_order_checks = job.anchor_order_checked ? 1u : 0u;
    seed.seed_canonical_anchor_order_accepts = job.anchor_order_accepted ? 1u : 0u;
    seed.seed_anchor_digest_lane0 = job.anchor_digest_lo;
    seed.seed_anchor_digest_lane1 = job.anchor_digest_hi;
    return {
        .completion = job.minimizer_count == 0u
            ? vulkan_seed_frontend_completion_kind::mps_zero_query
            : vulkan_seed_frontend_completion_kind::mps_runtime,
        .seed_stats = seed,
        .operation_identity = operation.audit().operation_identity,
        .query_authority_identity = job.query_authority_identity,
        .query_ordinal = query_ordinal,
        .minimizer_count = job.minimizer_count,
        .target_len = job.target_len,
        .target_position_offset = job.target_position_offset,
        .anchor_count = job.anchor_count,
        .target_completion = target_completion,
        .target_operation_identity = target_operation_identity,
        .persisted_generation_id = persisted_generation_id,
        .persisted_source_identity = persisted_source_identity,
        .operation_job_index = job_index,
    };
}

/** Project the exact retained MPF target receipt without relabelling host construction. */
template <class p_target_owner>
[[nodiscard]] vulkan_seed_target_execution
make_mpf_target_execution(const p_target_owner& owner) {
    const bool has_spill = owner.transient_target_spill_receipt.has_value();
    vulkan_seed_target_execution proof{
        .completion = owner.persisted
            ? vulkan_seed_target_completion_kind::mpf_persisted
            : vulkan_seed_target_completion_kind::mpf_runtime_generated,
        .device_uuid = owner.resident_key.device_uuid,
        .target_operation_identity = owner.operation_identity,
        .target_len = owner.target_bp_size,
        .persisted_generation_id = owner.persisted
            ? owner.persisted_generation_id : 0u,
        .persisted_source_identity = owner.persisted
            ? owner.persisted_source_identity : 0u,
        .producer_roundtrips = has_spill
            ? owner.transient_target_spill_receipt->vulkan_stats.completed_roundtrips
            : 0u,
        .resident_materializations = owner.resident_materialization_stats.has_value()
            ? static_cast<std::uint64_t>(
                  owner.resident_materialization_stats->actual_slot_allocation_bytes != 0u) +
                  static_cast<std::uint64_t>(
                      owner.resident_materialization_stats
                              ->actual_position_allocation_bytes != 0u)
            : 0u,
        .resident_ingestions = owner.persisted &&
                owner.resident_materialization_stats.has_value()
            ? owner.resident_materialization_stats->complete_mapped_traversals
            : 0u,
        .canonical_empty = owner.canonical_empty,
        .query_empty_bypass = !owner.persisted && !has_spill,
        .producer_receipt_present = has_spill,
    };
    if (!owner.ready() || !proof.is_internally_consistent()) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "MPF target has no authentic spill/replay/materialization publication proof");
    }
    return proof;
}

/** Project the exact retained MPS producer and physical resident receipts. */
template <class p_target_owner>
[[nodiscard]] vulkan_seed_target_execution
make_mps_target_execution(const p_target_owner& owner) {
    if (!owner.ready()) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "MPS target has no authentic generation publication proof");
    }
    std::uint64_t materialization_count = 0u;
    for (const auto& materialization : owner.resident->materializations()) {
        const auto& snapshot = materialization.allocation_snapshot;
        if (materialization.admitted_bytes == 0u ||
            snapshot.actual_allocation_bytes != materialization.admitted_bytes ||
            snapshot.fully_touched_bytes < materialization.admitted_bytes) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "MPS target has an unauthenticated physical allocation");
        }
        ++materialization_count;
    }
    const auto& identity = owner.resident->identity();
    vulkan_seed_target_execution proof{
        .completion = owner.persisted
            ? vulkan_seed_target_completion_kind::mps_persisted
            : vulkan_seed_target_completion_kind::mps_runtime_generated,
        .device_uuid = identity.device_uuid,
        .target_operation_identity = owner.operation_identity,
        .target_len = owner.target_bp_size,
        .persisted_generation_id = owner.persisted ? identity.generation_id : 0u,
        .persisted_source_identity = owner.persisted ? identity.source_identity : 0u,
        .persisted_cold_load_identity = owner.persisted
            ? identity.cold_load_identity : 0u,
        .producer_roundtrips = owner.persisted
            ? 0u : owner.transient_generation_receipt->vulkan_stats.completed_roundtrips,
        .resident_materializations = materialization_count,
        .resident_ingestions = owner.resident->cold_load_stats().full_generations_ingested,
        .canonical_empty = owner.canonical_empty,
        .producer_receipt_present = !owner.persisted,
    };
    if (!proof.is_internally_consistent()) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "MPS target producer and resident receipts disagree");
    }
    return proof;
}
#endif

/** Translate the direct GPU owner's failure into the publisher's fixed error ABI. */
[[nodiscard]] constexpr bioseq::indexing::minimizer_generation_encoding_error
direct_grouped_soa_encoding_error(
    bioseq::indexing::gpu_grouped_soa_ingest_error error) noexcept {
    using source = bioseq::indexing::gpu_grouped_soa_ingest_error;
    using destination = bioseq::indexing::minimizer_generation_encoding_error;
    switch (error) {
    case source::arithmetic_overflow:
        return destination::arithmetic_overflow;
    case source::admission:
    case source::non_mappable_device_memory:
        return destination::admission;
    case source::byte_count:
    case source::extent_exceeds_slot:
        return destination::byte_count;
    case source::checksum:
        return destination::checksum;
    case source::wrong_stream_phase:
    case source::slot_busy:
    case source::stale_slot:
    case source::authorization_pending:
    case source::capture_mismatch:
    case source::completion_mismatch:
    case source::poisoned_stream:
        return destination::row_order;
    default:
        return destination::storage_contract_mismatch;
    }
}

/**
 * Statement-local direct sink from canonical publisher rows to final GPU buffers.
 *
 * Payload bytes are callback-scoped: each row is checksummed, authenticated,
 * and copied directly to its final mapped buffer before the callback returns.
 * This sink retains only directory/manifest counters; the encoder continues
 * to own its bounded plan/directory metadata. Publication is withheld until
 * the encoder supplies the finalized metadata seal.
 */
struct transient_grouped_soa_direct_gpu_sink {
    bioseq::gpu::vulkan::runtime* runtime = nullptr;
    bioseq::indexing::expected_minimizer_generation_identity expected{};
    std::uint64_t cold_load_identity = 0u;
    std::optional<bioseq::indexing::gpu_grouped_soa_generation_ingest> ingest{};
    std::shared_ptr<const bioseq::indexing::gpu_grouped_soa_resident_target> resident{};
    bioseq::indexing::crc64_ecma182_accumulator directory_checksum{};
    bioseq::indexing::crc64_ecma182_accumulator manifest_checksum{};
    std::uint64_t generation_id = 0u;
    std::uint64_t directory_bytes = 0u;
    std::uint64_t manifest_bytes = 0u;
    std::uint64_t manifest_row_checksum = 0u;
    std::uint32_t expected_directory_chunks = 0u;
    std::uint32_t next_directory_chunk = 1u;
    bool begun = false;
    bool manifest_seen = false;
    bool sealed = false;

    [[nodiscard]] std::expected<void, bioseq::indexing::minimizer_generation_encoding_error>
    begin_direct_grouped_soa_generation(
        const bioseq::indexing::direct_grouped_soa_generation_plan_view& plan) {
        using error = bioseq::indexing::minimizer_generation_encoding_error;
        constexpr std::uint64_t quantum =
            bioseq::indexing::k_grouped_soa_extent_payload_bytes;
        const std::uint64_t directory_chunks =
            plan.storage_plan.directory_bytes / quantum +
            (plan.storage_plan.directory_bytes % quantum != 0u ? 1u : 0u);
        if (begun || sealed || runtime == nullptr || ingest || resident ||
            cold_load_identity == 0u ||
            plan.storage_plan.generation_id != plan.options.generation_id ||
            plan.storage_plan.layout !=
                bioseq::indexing::minimizer_generation_layout::grouped_soa_segment_local ||
            !bioseq::indexing::sql_integer_can_represent(directory_chunks)) {
            return std::unexpected(error::storage_contract_mismatch);
        }
        auto begun_ingest = bioseq::indexing::gpu_grouped_soa_generation_ingest::begin_planned(
            *runtime, plan, expected, cold_load_identity);
        if (!begun_ingest) {
            return std::unexpected(direct_grouped_soa_encoding_error(begun_ingest.error()));
        }
        ingest.emplace(std::move(*begun_ingest));
        generation_id = plan.options.generation_id;
        expected_directory_chunks = static_cast<std::uint32_t>(directory_chunks);
        begun = true;
        return {};
    }

    [[nodiscard]] std::expected<bioseq::indexing::minimizer_generation_row_write_receipt,
                                bioseq::indexing::minimizer_generation_encoding_error>
    write_grouped_soa_payload(
        const bioseq::indexing::unchecked_minimizer_generation_row_view& row) {
        using error = bioseq::indexing::minimizer_generation_encoding_error;
        if (!begun || sealed || manifest_seen || !ingest ||
            row.address.generation_id != generation_id ||
            row.address.position_segment < 0 ||
            row.bytes.size() > (std::numeric_limits<std::uint32_t>::max)()) {
            return std::unexpected(error::row_order);
        }
        const std::uint64_t checksum = bioseq::indexing::crc64_index_chunk_row(
            row.address.generation_id,
            row.address.position_segment,
            row.address.sub_key,
            row.address.chunk_no,
            static_cast<std::uint32_t>(row.bytes.size()),
            row.bytes);
        const auto validated = bioseq::indexing::validate_minimizer_generation_row_receipt(
            bioseq::indexing::minimizer_generation_row_view{
                .address = row.address,
                .bytes = row.bytes,
                .row_checksum = checksum,
            });
        if (!validated) {
            return std::unexpected(validated.error());
        }
        if (auto accepted = ingest->accept_validated_row(*validated); !accepted) {
            return std::unexpected(direct_grouped_soa_encoding_error(accepted.error()));
        }
        return bioseq::indexing::minimizer_generation_row_write_receipt{
            .row_checksum = checksum};
    }

    [[nodiscard]] std::expected<void, bioseq::indexing::minimizer_generation_encoding_error>
    write(const bioseq::indexing::minimizer_generation_row_view& row) {
        using error = bioseq::indexing::minimizer_generation_encoding_error;
        if (!begun || sealed || !ingest || row.address.generation_id != generation_id ||
            row.address.position_segment != -1 ||
            row.address.sub_key != bioseq::indexing::k_directory_sub_key ||
            row.bytes.size() > (std::numeric_limits<std::uint32_t>::max)() ||
            !bioseq::indexing::validate_minimizer_generation_row_receipt(row)) {
            return std::unexpected(error::row_order);
        }
        if (next_directory_chunk <= expected_directory_chunks) {
            if (manifest_seen || row.address.chunk_no != next_directory_chunk) {
                return std::unexpected(error::row_order);
            }
            directory_checksum.update(row.bytes);
            directory_bytes = bioseq::common::saturating_add(
                directory_bytes, static_cast<std::uint64_t>(row.bytes.size()));
            if (directory_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
                return std::unexpected(error::arithmetic_overflow);
            }
            ++next_directory_chunk;
            return {};
        }
        if (manifest_seen || row.address.chunk_no != 0u) {
            return std::unexpected(error::row_order);
        }
        manifest_checksum.update(row.bytes);
        manifest_bytes = static_cast<std::uint64_t>(row.bytes.size());
        manifest_row_checksum = row.row_checksum;
        manifest_seen = true;
        return {};
    }

    [[nodiscard]] std::expected<void, bioseq::indexing::minimizer_generation_encoding_error>
    seal_direct_grouped_soa_generation(
        const bioseq::indexing::encoded_grouped_soa_generation& generation) {
        using error = bioseq::indexing::minimizer_generation_encoding_error;
        const auto head_row = bioseq::indexing::minimizer_generation_row_view{
            .address = generation.unpublished_head_row.address,
            .bytes = generation.unpublished_head_row.bytes(),
            .row_checksum = generation.unpublished_head_row.row_checksum,
        };
        if (!begun || sealed || !ingest || resident || !manifest_seen ||
            next_directory_chunk != expected_directory_chunks + 1u ||
            generation.manifest.generation_id != generation_id ||
            directory_bytes != generation.manifest.directory_bytes ||
            directory_checksum.checksum() != generation.manifest.directory_checksum ||
            manifest_bytes != generation.manifest_row.byte_count ||
            manifest_checksum.checksum() != generation.head.manifest_checksum ||
            manifest_row_checksum != generation.manifest_row.row_checksum ||
            !bioseq::indexing::validate_minimizer_generation_row_receipt(head_row)) {
            return std::unexpected(error::checksum);
        }
        if (auto authenticated = ingest->seal_planned_generation(
                generation.head, generation.manifest, expected, generation.directory);
            !authenticated) {
            return std::unexpected(direct_grouped_soa_encoding_error(authenticated.error()));
        }
        auto completed = ingest->finish();
        if (!completed) {
            return std::unexpected(direct_grouped_soa_encoding_error(completed.error()));
        }
        resident = std::move(*completed);
        ingest.reset();
        sealed = true;
        return {};
    }

    [[nodiscard]] std::shared_ptr<const bioseq::indexing::gpu_grouped_soa_resident_target>
    take_resident() {
        if (!sealed || !resident) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "direct grouped-SoA sink has no sealed resident");
        }
        sealed = false;
        return std::move(resident);
    }
};

static_assert(bioseq::indexing::minimizer_generation_row_sink<
              transient_grouped_soa_direct_gpu_sink>);
static_assert(bioseq::indexing::grouped_soa_minimizer_payload_sink<
              transient_grouped_soa_direct_gpu_sink>);
static_assert(bioseq::indexing::direct_grouped_soa_generation_sink<
              transient_grouped_soa_direct_gpu_sink>);

/** Bind an immutable grouped-SoA resident to its existing runtime view ABI. */
template <class p_owner>
void bind_grouped_soa_resident_view(p_owner& owner) {
    if (owner.resident == nullptr) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "grouped-SoA target has no completed resident owner");
    }
    const auto& identity = owner.resident->identity();
    owner.resident_view = bioseq::gpu::vulkan::gpu_grouped_soa_resident_view{
        .descriptors = &owner.resident->descriptors(),
        .controls = &owner.resident->controls(),
        .positions = &owner.resident->positions(),
        .descriptor_bytes = owner.resident->descriptor_bytes(),
        .control_bytes = owner.resident->control_bytes(),
        .position_bytes = owner.resident->position_bytes(),
        .position_count = owner.resident->position_count(),
        .max_occ_cap = bioseq::indexing::k_grouped_soa_default_max_occurrence_cap,
        .generation_id = identity.generation_id,
        .source_identity = identity.source_identity,
        .cold_load_identity = identity.cold_load_identity,
        .partition_count = owner.resident->descriptor_count(),
        .abi_revision = identity.gpu_abi_revision,
        .device_uuid = identity.device_uuid,
    };
    owner.canonical_empty = owner.resident_view.position_count == 0u;
}

/** Build the canonical MPS grouped-SoA resident once from the real target view. */
template <class p_sequence, class p_minimizer_type, class p_target_view, class p_owner>
void prepare_transient_mps_resident(
    bioseq::gpu::vulkan::runtime& runtime,
    const std::uint8_t alphabet_code,
    const std::uint8_t storage_unit_byte_width,
    p_target_view target_view,
    const pipe::statement_execution_plan_owner& execution_plan,
    p_owner& owner) {
    if (!same_statement_owner(owner.execution_plan, execution_plan) || owner.persisted ||
        !owner.source_ready() || owner.resident != nullptr
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        ||
        owner.transient_generation_receipt.has_value() ||
        owner.transient_generation_publication.has_value() ||
        owner.transient_generation_stats_published ||
        owner.resident_materialization_stats_published
#endif
        ||
        owner.target_bp_size != target_view.bp_size()) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "transient MPS preparation has no fresh statement target owner");
    }
    const auto& params = execution_plan->frozen_parameters;
    const auto algorithm_identity =
        bioseq::indexing::select_minimizer_generation_algorithm_identity_runtime(
            params.minimizer_algorithm,
            owner.target_bp_size,
            alphabet_code,
            static_cast<std::uint8_t>(storage_unit_byte_width * 8u),
            static_cast<std::uint16_t>(params.kmer_size),
            static_cast<std::uint16_t>(params.window_size));
    if (!algorithm_identity) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "transient MPS has no generated algorithm identity");
    }
    constexpr minimizer_sketch_backend backend = minimizer_sketch_backend::vulkan_segment;
    const auto producer_identity =
        bioseq::indexing::minimizer_producer_backend_identity_for_backend(backend);
    if (!bioseq::indexing::is_valid_minimizer_generation_algorithm_identity(
            *algorithm_identity, producer_identity) ||
        !bioseq::indexing::minimizer_generation_kernel_binding_is_valid(
            backend, *algorithm_identity, nullptr)) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "transient MPS rejected the selected Vulkan producer identity");
    }
    const auto scheme = bioseq::indexing::minimizer_scheme_identity_for_backend(backend);
    const bioseq::indexing::grouped_soa_generation_options options{
        .layout = bioseq::indexing::minimizer_generation_layout::grouped_soa_segment_local,
        .generation_id = owner.operation_identity,
        .source_identity = owner.operation_identity,
        .source_content_checksum = 0u,
        .target_length = owner.target_bp_size,
        .position_segment_span_bp =
            bioseq::indexing::k_minimizer_preferred_segment_span_bp,
        .alphabet_code = alphabet_code,
        .storage_unit_byte_width = storage_unit_byte_width,
        .kmer_size = static_cast<std::uint16_t>(params.kmer_size),
        .window_size = static_cast<std::uint16_t>(params.window_size),
        .minimizer_scheme_id = scheme.id,
        .minimizer_scheme_revision = scheme.revision,
        .algorithm_identity = *algorithm_identity,
        .max_occ_cap = bioseq::indexing::k_grouped_soa_default_max_occurrence_cap,
    };
    const auto expected = bioseq::indexing::expected_grouped_soa_generation_identity_for_backend(
        options.layout,
        backend,
        *algorithm_identity,
        alphabet_code,
        storage_unit_byte_width,
        options.kmer_size,
        options.window_size,
        options.max_occ_cap);
    if (!expected) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "transient MPS expected grouped-SoA identity is invalid");
    }
    using alphabet_type = typename compact_sequence_range<p_sequence>::traits::alphabet_type;
    bioseq::indexing::minimizer_fragment_generation_replay_producer<alphabet_type> producer{
        target_view,
        backend,
        nullptr,
        *algorithm_identity,
        static_cast<std::uint16_t>(params.window_size),
        static_cast<std::uint16_t>(params.kmer_size),
        alphabet_code};
    transient_grouped_soa_direct_gpu_sink sink{
        .runtime = std::addressof(runtime),
        .expected = *expected,
        .cold_load_identity = owner.operation_identity,
    };
    auto encoded = bioseq::indexing::encode_minimizer_generation(
        producer, sink, options, bioseq::indexing::grouped_soa_generation_limits{});
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    const auto producer_stats = encoded
        ? producer.publication_execution_stats(*encoded)
        : producer.execution_stats();
    const auto publication = encoded
        ? std::optional{bioseq::indexing::make_minimizer_generation_publication_binding(*encoded)}
        : std::nullopt;
    if (!encoded || !publication || !producer_stats.publication_accepted(*publication)) {
#else
    const auto producer_stats = producer.execution_stats();
    if (!encoded || !producer_stats.is_accepted()) {
#endif
        throw bioseq::gpu::vulkan::vulkan_runtime_error(fmt::format(
            "transient MPS canonical publisher rejected storage_error={}",
            encoded ? 0u : static_cast<unsigned>(encoded.error())));
    }
    owner.resident = sink.take_resident();
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    owner.transient_generation_receipt.emplace(producer_stats);
    owner.transient_generation_publication.emplace(*publication);
#endif
    bind_grouped_soa_resident_view(owner);
    if (!owner.ready()) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "transient MPS resident failed its final statement ownership check");
    }
}

/**
 * Resolve exact row views and run one canonical job through the existing
 * chain/region/alignment/MAPQ suffix. No coordinate reducer, GPU CIGAR, or GPU
 * MAPQ stage is constructible below this boundary.
 */
template <class p_sequence,
          std::size_t p_batch_capacity,
          class p_minimizer_type,
          class p_sequence_header,
          class p_scanner_context>
void run_canonical_suffix_row(
    const std::size_t row,
    const std::size_t query_index,
    const std::size_t job_index,
    const p_sequence_header* query_hdr,
    const std::array<const p_sequence_header*, p_batch_capacity>* per_row_query_hdrs,
    const std::array<const p_sequence_header*, p_batch_capacity>& target_hdrs,
    const std::array<std::int32_t, p_batch_capacity>& query_ids,
    const pipe::statement_execution_plan_owner& execution_plan,
    const std::uint64_t broadcast_query_ordinal,
    p_scanner_context* scanner_pg_ctx,
    const bool per_row_query_mode,
    const ::bioseq::pipeline_stats::target_index_source actual_source,
    vulkan_query_batch_state<p_sequence, p_batch_capacity, p_minimizer_type>& state,
    const canonical_seed_batch& seed_batch,
    pipe::route_attempt_memory& attempt,
    std::vector<canonical_suffix_row_completion>& completed_rows
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    , const vulkan_seed_frontend_job_execution& seed_frontend
    , bioseq::gpu::vulkan::runtime_pipeline_operation_token* completed_chaining
#endif
    ) {
    using state_type =
        vulkan_query_batch_state<p_sequence, p_batch_capacity, p_minimizer_type>;
    using row_view = typename state_type::gpu_row_view;
    const auto authenticated_query_index = state.query_index_for_row(row);
    if (!same_statement_owner(execution_plan, state.execution_plan) ||
        row >= p_batch_capacity || query_index >= state.query_sidecars.size() ||
        state.query_sidecars[query_index].row_index != row ||
        !authenticated_query_index || *authenticated_query_index != query_index ||
        job_index >= seed_batch.jobs.size()) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "canonical Vulkan suffix row ownership is inconsistent");
    }
    const p_sequence_header* const row_query_hdr =
        per_row_query_mode ? (*per_row_query_hdrs)[row] : query_hdr;
    const p_sequence_header* const row_target_hdr = target_hdrs[row];
    const bool recipe_query = state.gpu_query_recipe_views[row].has_value();
    const bool recipe_target = state.gpu_target_recipe_views[row].has_value();
    if ((row_query_hdr == nullptr) == !recipe_query ||
        (row_target_hdr == nullptr) == !recipe_target) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "canonical Vulkan suffix requires exclusive flat-or-source row owners");
    }

    std::optional<compact_sequence_range<p_sequence>> flat_query;
    std::optional<compact_sequence_range<p_sequence>> flat_target;
    const row_view query_view = recipe_query
        ? *state.gpu_query_recipe_views[row]
        : [flat_query_owner = std::addressof(flat_query), row_query_hdr]() -> row_view {
              flat_query_owner->emplace(*row_query_hdr);
              return bioseq::storage::make_lazy_sequence_view(**flat_query_owner);
          }();
    const row_view target_view = recipe_target
        ? *state.gpu_target_recipe_views[row]
        : [flat_target_owner = std::addressof(flat_target), row_target_hdr]() -> row_view {
              flat_target_owner->emplace(*row_target_hdr);
              return bioseq::storage::make_lazy_sequence_view(**flat_target_owner);
          }();
    if (query_view.bp_size() != state.query_sidecars[query_index].query_len ||
        target_view.bp_size() == 0u ||
        seed_batch.jobs[job_index].ordinal != state.query_sidecars[query_index].ordinal) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "canonical Vulkan suffix sequence/job identity changed after seed publication");
    }

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    const auto operation_origin = bind_benchmark_e2e_operation_origin(
        execution_plan,
        scanner_pg_ctx,
        row,
        per_row_query_mode,
        broadcast_query_ordinal);
#endif
    match_result_collection collection;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    if (!seed_frontend.is_internally_consistent() || completed_chaining == nullptr ||
        completed_chaining->audit().kind !=
            bioseq::gpu::vulkan::runtime_pipeline_operation_kind::invalid) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "canonical Vulkan suffix has no fresh caller-owned chain receipt slot");
    }
#endif
    pipe::dispatch_post_seed_job(seed_batch,
                                 job_index,
                                 state.query_sidecars[query_index].ordinal,
                                 query_view,
                                 target_view,
                                 state.host_query_minimizers(query_index),
                                 execution_plan,
                                 collection,
                                 attempt
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                                 , completed_chaining
#endif
                                 );
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    const bool chaining_dispatched =
        collection.partial_vulkan_stats.chain_score_dispatches == 1u;
    const bool chaining_receipt_issued =
        completed_chaining->is_valid_for_chaining_tiled_dp();
    if (collection.partial_vulkan_stats.chain_score_dispatches > 1u ||
        chaining_dispatched != chaining_receipt_issued ||
        (execution_plan->frozen_parameters.chaining_algorithm !=
             chaining_algorithm::vulkan_tiled_dynamic_programming &&
         chaining_receipt_issued)) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "canonical Vulkan suffix chaining receipt differs from completed work");
    }
    collection.record_completed_minimizer_algorithm_identity(
        minimizer_algorithm::vulkan_minimap_integer);
#endif
    if (completed_rows.size() == completed_rows.capacity()) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "canonical Vulkan suffix wave exceeded its admitted completion owner");
    }
    completed_rows.push_back(canonical_suffix_row_completion{
        .row = row,
        .query_id = query_ids[row],
        .actual_source = actual_source,
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        .seed_frontend = seed_frontend,
#endif
        .collection = std::move(collection),
    });
}

/**
 * Copy the complete authenticated GPU query partition into one native-width union.
 *
 * This is shared by persisted and transient MPF target preparation. The caller
 * retains the matching admission permit until target projection completes.
 */
template <class p_sequence,
          std::size_t p_batch_capacity,
          class p_minimizer_type,
          class p_hash>
void prepare_statement_mpf_query_hash_union(
    vulkan_query_batch_state<p_sequence, p_batch_capacity, p_minimizer_type>& state,
    std::vector<p_hash>& selected_hashes,
    bioseq::parallelism::memory_permit& selected_hashes_permit) {
    static_assert(std::same_as<p_hash, std::uint32_t> ||
                  std::same_as<p_hash, std::uint64_t>);
    const std::uint64_t total_query_minimizers =
        state.query_minimizer_device_span.count();
    if (total_query_minimizers >
            static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()) ||
        !state.submitted_rows.valid(p_batch_capacity) ||
        state.submitted_rows.row_count != state.query_records.size() ||
        state.query_records.size() != state.query_sidecars.size() ||
        state.row_to_query_index.size() != state.submitted_rows.row_count) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "MPF query union has no matching generated hash owner");
    }
    prepare_vulkan_query_host_capacity(
        selected_hashes,
        selected_hashes_permit,
        static_cast<std::size_t>(total_query_minimizers));
    std::uint64_t covered_query_minimizers = 0u;
    for (std::size_t query_index = 0u;
         query_index < state.query_records.size();
         ++query_index) {
        const auto& record = state.query_records[query_index];
        const std::size_t row = state.query_sidecars[query_index].row_index;
        const auto authenticated_query_index = state.query_index_for_row(row);
        const auto query_minimizers = state.host_query_minimizers(query_index);
        const auto records = query_minimizers.as_span();
        if (!state.submitted_rows.contains(row) || !authenticated_query_index ||
            *authenticated_query_index != query_index || record.flags != 0u ||
            record.minimizer_offset != covered_query_minimizers ||
            records.size() != record.minimizer_count ||
            covered_query_minimizers > total_query_minimizers ||
            record.minimizer_count > total_query_minimizers - covered_query_minimizers) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "MPF query records do not form the complete device partition");
        }
        for (const p_minimizer_type& minimizer : records) {
            const std::uint64_t hash = minimizer.kmer();
            if (hash > static_cast<std::uint64_t>(
                           (std::numeric_limits<p_hash>::max)())) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error(
                    "MPF query hash exceeds its generated native width");
            }
            selected_hashes.push_back(static_cast<p_hash>(hash));
        }
        covered_query_minimizers += record.minimizer_count;
    }
    if (covered_query_minimizers != total_query_minimizers ||
        selected_hashes.size() != total_query_minimizers) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "MPF query union omitted part of the shared GPU sketch");
    }
    std::ranges::sort(selected_hashes);
    selected_hashes.erase(
        std::ranges::unique(selected_hashes).begin(), selected_hashes.end());
}

/** Collect selected Vulkan target cores into 572's qualified segmented spill. */
template <class p_storage, sequence_alphabet p_alphabet, class p_hash>
[[nodiscard]] auto spill_selected_vulkan_mpf_target(
    p_storage& storage,
    bioseq::storage::lazy_sequence_view<p_alphabet> target_view,
    const bioseq::indexing::minimizer_generation_algorithm_identity& algorithm_identity,
    std::uint8_t alphabet_code,
    std::uint64_t position_segment_span_bp,
    std::span<const p_hash> selected_hashes,
    bioseq::indexing::minimizer_sequence_generation_execution_receipt& receipt) {
    using sequence_index = bioseq::indexing::direct_minimizer_bucket_index<p_storage>;
    constexpr minimizer_sketch_backend backend = minimizer_sketch_backend::vulkan_segment;
    if constexpr (std::same_as<p_hash, std::uint32_t>) {
        return sequence_index::spill_generation_replay_runs_serial(
            storage,
            target_view,
            backend,
            /*kernels=*/nullptr,
            algorithm_identity,
            /*core_span_bp=*/0u,
            position_segment_span_bp,
            std::addressof(receipt),
            alphabet_code,
            selected_hashes,
            std::span<const std::uint64_t>{},
            sizeof(p_hash));
    } else {
        static_assert(std::same_as<p_hash, std::uint64_t>);
        return sequence_index::spill_generation_replay_runs_serial(
            storage,
            target_view,
            backend,
            /*kernels=*/nullptr,
            algorithm_identity,
            /*core_span_bp=*/0u,
            position_segment_span_bp,
            std::addressof(receipt),
            alphabet_code,
            std::span<const std::uint32_t>{},
            selected_hashes,
            sizeof(p_hash));
    }
}

/**
 * Build one statement-local MPF resident for the exact physical query-hash union.
 *
 * Every query record is already GPU-generated and host-authenticated. Target
 * collection remains the selected Vulkan producer; filtering happens only after
 * each canonical core has been collected and deduplicated. The qualified spill
 * is replayed twice into one global sorted-dense resident.
 */
template <class p_sequence,
          std::size_t p_batch_capacity,
          class p_minimizer_type,
          class p_hash,
          class p_target_view,
          class p_prepared_target>
void prepare_transient_mpf_query_union_resident_for_hash(
    bioseq::gpu::vulkan::runtime& runtime,
    std::uint8_t alphabet_code,
    p_target_view target_view,
    const pipe::statement_execution_plan_owner& execution_plan,
    const bioseq::indexing::minimizer_generation_algorithm_identity& algorithm_identity,
    p_prepared_target& prepared_target,
    vulkan_query_batch_state<p_sequence, p_batch_capacity, p_minimizer_type>& state) {
    static_assert(std::same_as<p_hash, std::uint32_t> ||
                  std::same_as<p_hash, std::uint64_t>);
    const auto& params = execution_plan->frozen_parameters;
    const auto expected_width = std::same_as<p_hash, std::uint32_t>
        ? bioseq::minimizers::minimizer_scalar_width::u32
        : bioseq::minimizers::minimizer_scalar_width::u64;
    constexpr std::uint32_t alphabet_size = static_cast<std::uint32_t>(
        compact_sequence_range<p_sequence>::traits::alphabet_size_n);
    if (
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        prepared_target.transient_target_spill_receipt.has_value() ||
        prepared_target.transient_target_replay_receipt.has_value() ||
        prepared_target.resident_materialization_stats.has_value() ||
        prepared_target.target_physical_stats_published ||
#endif
        algorithm_identity.hash_width != expected_width ||
        !bioseq::indexing::gpu_source_direct_tuple_supported<p_minimizer_type>(
            alphabet_size, params.kmer_size, params.window_size)) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "no-index MPF query union has no eligible sorted-dense generated record");
    }

    bioseq::parallelism::memory_permit selected_hashes_permit;
    std::vector<p_hash> selected_hashes;
    prepare_statement_mpf_query_hash_union<
        p_sequence,
        p_batch_capacity,
        p_minimizer_type,
        p_hash>(
        state,
        selected_hashes,
        selected_hashes_permit);

    if (selected_hashes.empty()) {
        prepared_target.resident_key =
            bioseq::indexing::make_gpu_mapped_fragment_sorted_dense_key<
                p_minimizer_type>(runtime,
                                  prepared_target.operation_identity,
                                  prepared_target.target_bp_size,
                                  0u,
                                  0u,
                                  alphabet_size,
                                  static_cast<std::uint32_t>(params.kmer_size),
                                  static_cast<std::uint32_t>(params.window_size));
        prepared_target.canonical_empty = true;
        return;
    }

    opportunistic_sequence_index_storage storage;
    bioseq::indexing::minimizer_sequence_generation_execution_receipt spill_receipt{};
    // The selected Vulkan producer remains bounded to its 32,768-bp cores. One
    // target-wide spill segment keeps the number of retained sorted-run writer
    // owners constant while the store's admitted merge-reader compaction and
    // position batches preserve bounded replay memory and global ordering.
    const std::uint64_t spill_segment_span_bp =
        static_cast<std::uint64_t>(prepared_target.target_bp_size);
    auto spill = spill_selected_vulkan_mpf_target(
        storage,
        target_view,
        algorithm_identity,
        alphabet_code,
        spill_segment_span_bp,
        std::span<const p_hash>{selected_hashes},
        spill_receipt);
    if (!spill || !spill_receipt.is_spill_valid() ||
        spill_receipt.selected_backend != minimizer_sketch_backend::vulkan_segment ||
        spill_receipt.algorithm_identity != algorithm_identity ||
        spill_receipt.target_length != prepared_target.target_bp_size ||
        spill_receipt.spill_qualification_scan_count != 1u ||
        spill_receipt.spill_qualification_merge_count != 1u ||
        (spill_receipt.spill_run_count == 0u) !=
            (spill_receipt.spilled_event_count == 0u) ||
        spill_receipt.spilled_event_count !=
            (spill ? spill->execution_receipt().spilled_event_count : 0u)) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(fmt::format(
            "no-index MPF selected Vulkan spill rejected ({})",
            spill ? 0u : static_cast<unsigned>(spill.error())));
    }
    const std::uint64_t event_count = spill_receipt.spilled_event_count;
    if (event_count == 0u) {
        storage.release_runs();
        prepared_target.resident_key =
            bioseq::indexing::make_gpu_mapped_fragment_sorted_dense_key<
                p_minimizer_type>(runtime,
                                  prepared_target.operation_identity,
                                  prepared_target.target_bp_size,
                                  0u,
                                  0u,
                                  alphabet_size,
                                  static_cast<std::uint32_t>(params.kmer_size),
                                  static_cast<std::uint32_t>(params.window_size));
        prepared_target.canonical_empty = true;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        prepared_target.transient_target_spill_receipt.emplace(
            std::move(spill_receipt));
#endif
        return;
    }

    bioseq::parallelism::memory_permit occurrence_counts_permit;
    std::vector<std::uint64_t> occurrence_counts;
    prepare_vulkan_query_host_capacity(
        occurrence_counts,
        occurrence_counts_permit,
        selected_hashes.size());
    occurrence_counts.resize(selected_hashes.size(), 0u);
    using sequence_index =
        bioseq::indexing::direct_minimizer_bucket_index<
            opportunistic_sequence_index_storage>;
    auto replay = sequence_index::make_generation_replay_producer(
        storage,
        static_cast<std::uint64_t>(prepared_target.target_bp_size),
        std::move(*spill),
        spill_segment_span_bp);
    auto materialized =
        bioseq::indexing::materialize_sorted_dense_from_segmented_spill_replay<
            p_minimizer_type,
            p_hash>(runtime,
                    replay,
                    std::span<const p_hash>{selected_hashes},
                    std::span<std::uint64_t>{occurrence_counts},
                    event_count,
                    spill_segment_span_bp,
                    prepared_target.target_bp_size,
                    prepared_target.operation_identity,
                    alphabet_size,
                    static_cast<std::uint32_t>(params.kmer_size),
                    static_cast<std::uint32_t>(params.window_size));
    const auto completed_replay = replay.execution_receipt();
    const auto replay_storage_binding = storage.segment_spill_storage_binding();
    const std::uint64_t expected_visited =
        bioseq::common::saturating_mul(event_count, 2u);
    if (!materialized || !materialized->resident ||
        !completed_replay.is_spill_valid() ||
        completed_replay.selected_backend != minimizer_sketch_backend::vulkan_segment ||
        completed_replay.algorithm_identity != algorithm_identity ||
        completed_replay.generation_layout !=
            bioseq::indexing::minimizer_generation_layout::invalid ||
        completed_replay.replay_scan_count != 2u ||
        completed_replay.replay_merge_pass_count != 2u ||
        completed_replay.replay_reroute_pass_count != 0u ||
        completed_replay.replay_completed_scan_count != 2u ||
        completed_replay.replay_failed_scan_count != 0u ||
        completed_replay.replay_binding_checked_failed_scan_count != 0u ||
        completed_replay.replay_illegal_span_transition_count != 0u ||
        completed_replay.replay_first_completed_event_count != event_count ||
        completed_replay.replay_last_completed_event_count != event_count ||
        completed_replay.replay_first_completed_content_digest !=
            completed_replay.spill_content_digest ||
        completed_replay.replay_last_completed_content_digest !=
            completed_replay.spill_content_digest ||
        completed_replay.replay_final_span_bp != spill_segment_span_bp ||
        completed_replay.replay_final_storage_binding !=
            completed_replay.spill_storage_binding ||
        replay_storage_binding != completed_replay.replay_final_storage_binding ||
        completed_replay.replay_visited_event_count != expected_visited) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(fmt::format(
            "no-index MPF bounded resident materialization rejected ({})",
            materialized ? 0u
                         : static_cast<unsigned>(materialized.error().error)));
    }
    storage.release_runs();
    prepared_target.resident_key = materialized->resident->key;
    auto inserted = prepared_target.resident_cache->insert_built(
        prepared_target.resident_key, materialized->resident);
    if (!inserted) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(fmt::format(
            "no-index MPF bounded resident cache insertion rejected ({})",
            static_cast<unsigned>(inserted.error())));
    }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    prepared_target.transient_target_spill_receipt.emplace(
        std::move(spill_receipt));
    prepared_target.transient_target_replay_receipt.emplace(completed_replay);
    prepared_target.resident_materialization_stats.emplace(materialized->stats);
#endif
    prepared_target.resident = std::move(materialized->resident);
}

/** Select the exact generated record/hash width before the no-index target instantiation. */
template <class p_sequence,
          std::size_t p_batch_capacity,
          class p_minimizer_type,
          class p_target_view,
          class p_prepared_target>
void prepare_transient_mpf_query_union_resident(
    bioseq::gpu::vulkan::runtime& runtime,
    std::uint8_t alphabet_code,
    const bioseq::minimizers::minimizer_physical_layout_selection& physical_layout,
    p_target_view target_view,
    const pipe::statement_execution_plan_owner& execution_plan,
    p_prepared_target& prepared_target,
    vulkan_query_batch_state<p_sequence, p_batch_capacity, p_minimizer_type>& state) {
    const auto& params = execution_plan->frozen_parameters;
    if (params.kmer_size <= 0 || params.window_size <= 0 ||
        physical_layout.requested_algorithm != params.minimizer_algorithm ||
        physical_layout.effective_algorithm != params.minimizer_algorithm ||
        physical_layout.alphabet_id != alphabet_code ||
        physical_layout.runtime_kmer_size != params.kmer_size ||
        physical_layout.runtime_window_size != params.window_size) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "no-index MPF generated target tuple is invalid");
    }
    const auto algorithm_identity =
        bioseq::indexing::make_minimizer_generation_algorithm_identity(physical_layout);
    const bool record_matches = bioseq::minimizers::visit_minimizer_physical_layout(
        physical_layout,
        []<class p_selected_record>() noexcept {
            return std::same_as<p_selected_record, p_minimizer_type>;
        });
    constexpr minimizer_sketch_backend backend = minimizer_sketch_backend::vulkan_segment;
    if (!algorithm_identity || !record_matches ||
        !bioseq::indexing::is_valid_minimizer_generation_algorithm_identity(
            *algorithm_identity,
            bioseq::indexing::minimizer_producer_backend_identity_for_backend(backend)) ||
        !bioseq::indexing::minimizer_generation_kernel_binding_is_valid(
            backend, *algorithm_identity, nullptr)) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "no-index MPF generated target identity does not match the query record");
    }
    using layout = typename p_minimizer_type::layout_type;
    constexpr std::uint32_t available_hash_bits =
        layout::k_storage_bits == 128u
            ? std::min<std::uint32_t>(layout::k_hash_bits, 56u)
            : layout::k_hash_bits;
    constexpr std::uint32_t selected_hash_bits =
        std::min<std::uint32_t>(2u * layout::k_physical_kmer_size, 56u);
    static_assert(selected_hash_bits > 0u && selected_hash_bits <= available_hash_bits &&
                  available_hash_bits <= 64u);
    using selected_hash = std::conditional_t<
        selected_hash_bits <= (std::numeric_limits<std::uint32_t>::digits),
        std::uint32_t,
        std::uint64_t>;
    constexpr auto expected_width = std::same_as<selected_hash, std::uint32_t>
        ? bioseq::minimizers::minimizer_scalar_width::u32
        : bioseq::minimizers::minimizer_scalar_width::u64;
    if (physical_layout.hash_width != expected_width) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "no-index MPF generated target selected a mismatched hash width");
    }
    prepare_transient_mpf_query_union_resident_for_hash<
        p_sequence,
        p_batch_capacity,
        p_minimizer_type,
        selected_hash>(runtime,
                       alphabet_code,
                       target_view,
                       execution_plan,
                       *algorithm_identity,
                       prepared_target,
                       state);
}

/**
 * Project one authenticated persisted MPF source onto the complete query union.
 *
 * The reader remains the only target owner. Its control stream is traversed to
 * compute global selected counts, then its posting stream is traversed once to
 * fill the exact final resident; target minimizers are never regenerated.
 */
template <class p_sequence,
          std::size_t p_batch_capacity,
          class p_minimizer_type,
          class p_hash,
          class p_prepared_target>
void prepare_persisted_mpf_query_union_resident_for_hash(
    bioseq::gpu::vulkan::runtime& runtime,
    const bioseq::minimizers::minimizer_physical_layout_selection& physical_layout,
    const pipe::statement_execution_plan_owner& execution_plan,
    const bioseq::indexing::prepared_minimizer_fragment_reader& reader,
    p_prepared_target& prepared_target,
    vulkan_query_batch_state<p_sequence, p_batch_capacity, p_minimizer_type>& state) {
    static_assert(std::same_as<p_hash, std::uint32_t> ||
                  std::same_as<p_hash, std::uint64_t>);
    const auto expected_width = std::same_as<p_hash, std::uint32_t>
        ? bioseq::minimizers::minimizer_scalar_width::u32
        : bioseq::minimizers::minimizer_scalar_width::u64;
    if (
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        prepared_target.transient_target_spill_receipt.has_value() ||
        prepared_target.transient_target_replay_receipt.has_value() ||
        prepared_target.resident_materialization_stats.has_value() ||
        prepared_target.target_physical_stats_published ||
#endif
        physical_layout.hash_width != expected_width || !reader.gpu_source_ready()) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "persisted MPF query union has no matching native-width reader");
    }

    const std::uint64_t source_controls =
        reader.fragment_source_control_upper_bound();
    const std::uint64_t source_positions =
        reader.fragment_source_position_count();
    if ((source_controls == 0u) != (source_positions == 0u) ||
        source_controls > source_positions) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "persisted MPF source has inconsistent control/posting counts");
    }

    bioseq::parallelism::memory_permit selected_hashes_permit;
    std::vector<p_hash> selected_hashes;
    prepare_statement_mpf_query_hash_union<
        p_sequence,
        p_batch_capacity,
        p_minimizer_type,
        p_hash>(
        state,
        selected_hashes,
        selected_hashes_permit);

    const auto& params = execution_plan->frozen_parameters;
    constexpr std::uint32_t alphabet_size = static_cast<std::uint32_t>(
        compact_sequence_range<p_sequence>::traits::alphabet_size_n);
    if (source_positions == 0u || selected_hashes.empty()) {
        prepared_target.resident_key =
            bioseq::indexing::make_gpu_mapped_fragment_sorted_dense_key<
                p_minimizer_type>(runtime,
                                  prepared_target.operation_identity,
                                  prepared_target.target_bp_size,
                                  0u,
                                  0u,
                                  alphabet_size,
                                  static_cast<std::uint32_t>(params.kmer_size),
                                  static_cast<std::uint32_t>(params.window_size));
        prepared_target.canonical_empty = true;
        return;
    }

    bioseq::parallelism::memory_permit occurrence_counts_permit;
    std::vector<std::uint64_t> occurrence_counts;
    prepare_vulkan_query_host_capacity(
        occurrence_counts,
        occurrence_counts_permit,
        selected_hashes.size());
    occurrence_counts.resize(selected_hashes.size(), 0u);
    const auto selected_shape =
        bioseq::indexing::inspect_mapped_fragment_query_union<p_hash>(
            reader,
            std::span<const p_hash>{selected_hashes},
            std::span<std::uint64_t>{occurrence_counts});
    if (!selected_shape) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(fmt::format(
            "persisted MPF query-union control traversal failed ({})",
            static_cast<unsigned>(selected_shape.error().error)));
    }
    if ((selected_shape->minimizer_count == 0u) !=
        (selected_shape->distinct_count == 0u)) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "persisted MPF query-union shape is inconsistent");
    }
    if (selected_shape->minimizer_count == 0u) {
        prepared_target.resident_key =
            bioseq::indexing::make_gpu_mapped_fragment_sorted_dense_key<
                p_minimizer_type>(runtime,
                                  prepared_target.operation_identity,
                                  prepared_target.target_bp_size,
                                  0u,
                                  0u,
                                  alphabet_size,
                                  static_cast<std::uint32_t>(params.kmer_size),
                                  static_cast<std::uint32_t>(params.window_size));
        prepared_target.canonical_empty = true;
        return;
    }

    auto materialized =
        bioseq::indexing::materialize_sorted_dense_from_mapped_fragment_query_union<
            p_minimizer_type,
            p_hash>(runtime,
                    reader,
                    std::span<const p_hash>{selected_hashes},
                    std::span<const std::uint64_t>{occurrence_counts},
                    *selected_shape,
                    prepared_target.target_bp_size,
                    prepared_target.operation_identity,
                    alphabet_size,
                    static_cast<std::uint32_t>(params.kmer_size),
                    static_cast<std::uint32_t>(params.window_size));
    if (!materialized || !materialized->resident ||
        materialized->stats.shape.minimizer_count !=
            selected_shape->minimizer_count ||
        materialized->stats.shape.distinct_count != selected_shape->distinct_count) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(fmt::format(
            "persisted MPF query-union resident materialization failed ({})",
            materialized ? 0u
                         : static_cast<unsigned>(materialized.error().error)));
    }
    prepared_target.resident_key = materialized->resident->key;
    auto inserted = prepared_target.resident_cache->insert_built(
        prepared_target.resident_key, materialized->resident);
    if (!inserted) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(fmt::format(
            "persisted MPF query-union resident cache insertion rejected ({})",
            static_cast<unsigned>(inserted.error())));
    }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    prepared_target.resident_materialization_stats.emplace(materialized->stats);
#endif
    prepared_target.resident = std::move(materialized->resident);
}

/** Select the native persisted hash lane after the physical query record is fixed. */
template <class p_sequence,
          std::size_t p_batch_capacity,
          class p_minimizer_type,
          class p_prepared_target>
void prepare_persisted_mpf_query_union_resident(
    bioseq::gpu::vulkan::runtime& runtime,
    std::uint8_t alphabet_code,
    const bioseq::minimizers::minimizer_physical_layout_selection& physical_layout,
    const pipe::statement_execution_plan_owner& execution_plan,
    const bioseq::indexing::prepared_minimizer_fragment_reader& reader,
    p_prepared_target& prepared_target,
    vulkan_query_batch_state<p_sequence, p_batch_capacity, p_minimizer_type>& state) {
    const auto& params = execution_plan->frozen_parameters;
    if (params.kmer_size <= 0 || params.window_size <= 0 ||
        physical_layout.requested_algorithm != params.minimizer_algorithm ||
        physical_layout.effective_algorithm != params.minimizer_algorithm ||
        physical_layout.alphabet_id != alphabet_code ||
        physical_layout.runtime_kmer_size != params.kmer_size ||
        physical_layout.runtime_window_size != params.window_size) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "persisted MPF query tuple is invalid");
    }
    const bool record_matches = bioseq::minimizers::visit_minimizer_physical_layout(
        physical_layout,
        []<class p_selected_record>() noexcept {
            return std::same_as<p_selected_record, p_minimizer_type>;
        });
    if (!record_matches) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "persisted MPF physical query record changed before target projection");
    }
    using layout = typename p_minimizer_type::layout_type;
    constexpr std::uint32_t available_hash_bits =
        layout::k_storage_bits == 128u
            ? std::min<std::uint32_t>(layout::k_hash_bits, 56u)
            : layout::k_hash_bits;
    constexpr std::uint32_t selected_hash_bits =
        std::min<std::uint32_t>(2u * layout::k_physical_kmer_size, 56u);
    static_assert(selected_hash_bits > 0u && selected_hash_bits <= available_hash_bits &&
                  available_hash_bits <= 64u);
    using selected_hash = std::conditional_t<
        selected_hash_bits <= (std::numeric_limits<std::uint32_t>::digits),
        std::uint32_t,
        std::uint64_t>;
    constexpr auto expected_width = std::same_as<selected_hash, std::uint32_t>
        ? bioseq::minimizers::minimizer_scalar_width::u32
        : bioseq::minimizers::minimizer_scalar_width::u64;
    if (physical_layout.hash_width != expected_width) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "persisted MPF query selected a mismatched hash width");
    }
    prepare_persisted_mpf_query_union_resident_for_hash<
        p_sequence,
        p_batch_capacity,
        p_minimizer_type,
        selected_hash>(runtime,
                       physical_layout,
                       execution_plan,
                       reader,
                       prepared_target,
                       state);
}

/**
 * Execute MPF Vulkan seeding against one statement query-selected resident built
 * from either the mapped READY source or the real no-index target view.
 */
template <class p_sequence,
          std::size_t p_batch_capacity,
          class p_minimizer_type,
          class p_sequence_header,
          class p_scanner_context,
          class p_runtime,
          class p_prepared_target>
void process_vulkan_mpf_seed_route(
    const std::size_t batch_size,
    const std::uint8_t alphabet_code,
    const bioseq::minimizers::minimizer_physical_layout_selection& physical_layout,
    const p_sequence_header* query_hdr,
    const std::array<const p_sequence_header*, p_batch_capacity>* per_row_query_hdrs,
    const std::array<const p_sequence_header*, p_batch_capacity>& target_hdrs,
    const std::array<std::int32_t, p_batch_capacity>& query_ids,
    const pipe::statement_execution_plan_owner& execution_plan,
    const std::uint64_t broadcast_query_ordinal,
    p_scanner_context* scanner_pg_ctx,
    const bool per_row_query_mode,
    p_runtime& runtime,
    p_prepared_target& prepared_target,
    vulkan_query_batch_state<p_sequence, p_batch_capacity, p_minimizer_type>& state,
    std::vector<canonical_suffix_row_completion>& completed_rows
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    , std::span<bioseq::gpu::vulkan::runtime_pipeline_operation_token> completed_chaining
    , std::vector<bioseq::gpu::vulkan::runtime_pipeline_operation_token>&
          completed_seed_operations
    , bioseq::gpu::vulkan::device_query_sketch_authorization*
          completed_query_authorization
#endif
    ) {
    const auto& params = execution_plan->frozen_parameters;
    const bool target_build_selected = params.vulkan_gpu_index_build_enabled;
    if (!same_statement_owner(prepared_target.execution_plan, execution_plan) ||
        !prepared_target.source_ready() || prepared_target.target_rowno < 0 ||
        prepared_target.target_bp_size == 0u || prepared_target.query_count < batch_size ||
        prepared_target.dispatched_query_count > prepared_target.query_count ||
        batch_size > prepared_target.query_count - prepared_target.dispatched_query_count ||
        params.max_occ < 0 || params.vulkan_fused_frontend_enabled ||
        target_build_selected != !prepared_target.persisted ||
        !state.submitted_rows.valid(p_batch_capacity) ||
        state.submitted_rows.row_count != batch_size ||
        state.query_records.size() != batch_size || state.query_sidecars.size() != batch_size ||
        !completed_rows.empty() || completed_rows.capacity() < batch_size) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "MPF Vulkan seed route has no matching statement target owner");
    }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    if (completed_chaining.size() != batch_size || completed_query_authorization == nullptr ||
        completed_query_authorization->is_valid() || !completed_seed_operations.empty() ||
        completed_seed_operations.capacity() < batch_size) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "MPF Vulkan seed route has no fresh whole-wave proof owner");
    }
#endif
    using row_view = typename vulkan_query_batch_state<
        p_sequence, p_batch_capacity, p_minimizer_type>::gpu_row_view;
    const std::size_t first_row = state.query_sidecars.front().row_index;
    std::optional<compact_sequence_range<p_sequence>> flat_target;
    const bool recipe_target = state.gpu_target_recipe_views[first_row].has_value();
    const p_sequence_header* const flat_target_header = target_hdrs[first_row];
    if ((flat_target_header == nullptr) == !recipe_target) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "MPF Vulkan target requires one exclusive flat or source owner");
    }
    const row_view target_view = recipe_target
        ? *state.gpu_target_recipe_views[first_row]
        : [flat_target_owner = std::addressof(flat_target), flat_target_header]() -> row_view {
              flat_target_owner->emplace(*flat_target_header);
              return bioseq::storage::make_lazy_sequence_view(**flat_target_owner);
          }();
    if (target_view.bp_size() != prepared_target.target_bp_size) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "MPF Vulkan target view changed its statement length");
    }

    if (!prepared_target.ready()) {
        if (prepared_target.resident != nullptr || prepared_target.canonical_empty) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "MPF Vulkan target contains a partial prior materialization");
        }
        // Query-selected target ownership is sealed only from the complete statement
        // union. This route currently supports exactly one nonempty scheduler wave
        // (including a singleton); a split or replayed statement is rejected before
        // target traversal, so no undispatched query can be omitted from the union.
        if (prepared_target.dispatched_query_count != 0u ||
            prepared_target.query_count != batch_size) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "query-selected MPF resident requires the statement-complete first wave");
        }
        if (prepared_target.persisted) {
            const auto& reader = *prepared_target.persisted_fragment_reader;
            prepare_persisted_mpf_query_union_resident(
                runtime,
                alphabet_code,
                physical_layout,
                execution_plan,
                reader,
                prepared_target,
                state);
        } else {
            prepare_transient_mpf_query_union_resident(
                runtime,
                alphabet_code,
                physical_layout,
                target_view,
                execution_plan,
                prepared_target,
                state);
        }
    }
    if (!prepared_target.ready()) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "MPF Vulkan target did not publish exactly one resident or empty proof");
    }
    std::shared_ptr<const bioseq::indexing::gpu_resident_minimizer_index>
        wave_resident;
    if (!prepared_target.canonical_empty) {
        auto required = prepared_target.resident_cache
                            ->template find_required_preinserted_typed<p_minimizer_type>(
                                runtime, prepared_target.resident_key);
        if (!required || !*required || required->get() != prepared_target.resident.get()) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(fmt::format(
                "MPF required preinserted resident lookup rejected ({})",
                required ? 0u : static_cast<unsigned>(required.error())));
        }
        wave_resident = std::move(*required);
    }

    if (scanner_pg_ctx != nullptr &&
        scanner_pg_ctx->read_index_active != prepared_target.persisted) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "MPF Vulkan target owner disagrees with the frozen index-read route");
    }
    const auto actual_source = prepared_target.persisted
        ? ::bioseq::pipeline_stats::target_index_source::persisted
        : ::bioseq::pipeline_stats::target_index_source::recomputed;
    for (std::size_t query_index = 0u; query_index < batch_size; ++query_index) {
        bioseq::postgres::support::invoke_pg_as_cpp([] { CHECK_FOR_INTERRUPTS(); });
        const std::size_t row = state.query_sidecars[query_index].row_index;
        if (scanner_pg_ctx != nullptr &&
            scanner_pg_ctx->target_rownos[row] != prepared_target.target_rowno) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "MPF Vulkan seed route changed its prepared target row");
        }
        pipe::route_attempt_memory attempt;
        context::installed_context_scope install{attempt.fresh_context};
        bioseq::parallelism::memory_permit job_permit;
        canonical_seed_batch seeds;
        attempt.prepare_seed_job_storage(seeds.jobs, job_permit, 1u);
        bioseq::seeding::vulkan_seed_lookup_decline seed_decline{};
        bioseq::seeding::vulkan_seed_lookup_completion completion{};
        if (prepared_target.canonical_empty) {
            auto empty_job_storage = std::move(seeds.jobs);
            if (!bioseq::seeding::publish_empty_canonical_seed_batch(
                    state.query_sidecars[query_index].ordinal,
                    seeds,
                    std::move(empty_job_storage))) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error(
                    "empty MPF target failed canonical job publication");
            }
        } else {
            const auto query_minimizers = state.host_query_minimizers(query_index);
            const auto& query_record = state.query_records[query_index];
            const auto query_device_span = state.query_minimizer_device_span.subspan(
                query_record.minimizer_offset, query_record.minimizer_count);
            completion = bioseq::seeding::collect_seed_hits_vulkan<p_minimizer_type>(
                query_minimizers.as_span(),
                query_device_span,
                static_cast<const bioseq::indexing::minimizer_hashmap_fragment_index*>(nullptr),
                wave_resident,
                params.vulkan_enabled,
                state.query_sidecars[query_index].query_len,
                params.max_occ,
                params.kmer_size,
                state.query_sidecars[query_index].ordinal,
                seeds,
                /*context_backing_is_admitted=*/true,
                params.vulkan_fused_frontend_enabled,
                &seed_decline
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                , std::addressof(state.query_sketch_authority),
                static_cast<std::uint32_t>(query_index)
#endif
                );
            if (!completion) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error(
                    "selected MPF Vulkan seed lookup declined without a host fallback");
            }
            if (completion.retained_anchor_permit) {
                if (completion.retained_anchor_bytes != completion.retained_anchor_permit.bytes() ||
                    !attempt.retention.retain_context_block(
                        std::move(completion.retained_anchor_permit))) {
                    throw std::logic_error{"MPF canonical anchor permit lost its attempt owner"};
                }
            }
        }
        const std::array<std::uint32_t, 1u> ordinal{
            state.query_sidecars[query_index].ordinal};
        if (!canonical_seed_jobs_match_submission(seeds, ordinal)) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "MPF Vulkan seed result changed its submitted canonical job");
        }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        const bool seed_dispatched = seed_decline.seed_lookup_dispatches == 1u;
        const bool seed_receipt = completion.seed_operation_receipt
                                      .is_valid_for_mpf_seed_anchor_roundtrip(
                                          runtime.features().max_workgroup_count_x);
        if (seed_decline.seed_lookup_dispatches > 1u || seed_dispatched != seed_receipt) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "MPF Vulkan seed receipt differs from its completed runtime dispatch");
        }
        const auto seed_frontend = make_mpf_seed_frontend_execution(
            seed_decline,
            completion.seed_operation_receipt,
            state.query_sketch_authority.identity(),
            state.query_sidecars[query_index].ordinal,
            state.query_records[query_index].minimizer_count,
            prepared_target.target_bp_size,
            seeds.jobs.front().count,
            prepared_target.persisted
                ? vulkan_seed_target_completion_kind::mpf_persisted
                : vulkan_seed_target_completion_kind::mpf_runtime_generated,
            prepared_target.operation_identity,
            prepared_target.persisted ? prepared_target.persisted_generation_id : 0u,
            prepared_target.persisted ? prepared_target.persisted_source_identity : 0u,
            prepared_target.canonical_empty);
        if (!seed_frontend.is_internally_consistent() ||
            !seed_frontend.matches_runtime_operation(
                completion.seed_operation_receipt,
                runtime.features().max_workgroup_count_x)) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "MPF Vulkan seed frontend differs from its completed operation");
        }
        if (seed_receipt) {
            if (completed_seed_operations.size() == completed_seed_operations.capacity()) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error(
                    "MPF Vulkan seed receipt owner exceeded its admitted capacity");
            }
            completed_seed_operations.push_back(
                std::move(completion.seed_operation_receipt));
        }
#endif
        run_canonical_suffix_row(row,
                                 query_index,
                                 0u,
                                 query_hdr,
                                 per_row_query_hdrs,
                                 target_hdrs,
                                 query_ids,
                                 execution_plan,
                                 broadcast_query_ordinal,
                                 scanner_pg_ctx,
                                 per_row_query_mode,
                                 actual_source,
                                 state,
                                 seeds,
                                 attempt,
                                 completed_rows
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                                 , seed_frontend
                                 , std::addressof(completed_chaining[query_index])
#endif
                                 );
#if defined(BIOSEQ_TEST_INDEXES) && !defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        if (completed_rows.empty() || completed_rows.back().row != row) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "MPF Vulkan seed stats lost their completed suffix owner");
        }
        completed_rows.back().seed_lookup_dispatches = seed_decline.seed_lookup_dispatches;
#endif
    }
    if (!state.query_sketch_owner ||
        !state.query_sketch_owner->host_readback_completed ||
        !state.query_sketch_authority.is_valid()) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "MPF Vulkan seed route consumed its shared query authority before its last suffix");
    }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    *completed_query_authorization = std::move(state.query_sketch_authority);
    if (!completed_query_authorization->is_valid()) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "MPF Vulkan seed route failed its post-consumer query-authority handoff");
    }
#endif
    if (completed_rows.size() != batch_size) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "MPF Vulkan seed route did not retain every completed suffix job");
    }
    if (prepared_target.dispatched_query_count >
            (std::numeric_limits<std::uint64_t>::max)() - batch_size ||
        prepared_target.dispatch_waves == (std::numeric_limits<std::uint64_t>::max)()) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "MPF prepared-target lifecycle counters saturated");
    }
    prepared_target.dispatched_query_count += batch_size;
    ++prepared_target.dispatch_waves;
}

/**
 * Execute grouped-SoA MPS seed-only once for the wave, authenticate its jobs,
 * and run the identical host suffix once per job (including empty jobs).
 */
template <class p_sequence,
          std::size_t p_batch_capacity,
          class p_minimizer_type,
          class p_sequence_header,
          class p_scanner_context,
          class p_runtime,
          class p_prepared_target>
void process_vulkan_mps_seed_route(
    const std::size_t batch_size,
    const std::uint8_t alphabet_code,
    const std::uint8_t storage_unit_byte_width,
    const p_sequence_header* query_hdr,
    const std::array<const p_sequence_header*, p_batch_capacity>* per_row_query_hdrs,
    const std::array<const p_sequence_header*, p_batch_capacity>& target_hdrs,
    const std::array<std::int32_t, p_batch_capacity>& query_ids,
    const pipe::statement_execution_plan_owner& execution_plan,
    const std::uint64_t broadcast_query_ordinal,
    p_scanner_context* scanner_pg_ctx,
    const bool per_row_query_mode,
    p_runtime& runtime,
    p_prepared_target& prepared_target,
    vulkan_query_batch_state<p_sequence, p_batch_capacity, p_minimizer_type>& state,
    std::vector<canonical_suffix_row_completion>& completed_rows
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    , bioseq::gpu::vulkan::runtime_pipeline_operation_token*
          completed_query_operation
    , bioseq::gpu::vulkan::runtime_pipeline_operation_token*
          completed_seed_operation
    , std::span<bioseq::gpu::vulkan::runtime_pipeline_operation_token>
          completed_chaining
#endif
    ) {
    const auto& params = execution_plan->frozen_parameters;
    const bool target_build_selected = params.vulkan_gpu_index_build_enabled;
    if (!same_statement_owner(prepared_target.execution_plan, execution_plan) ||
        !prepared_target.source_ready() || prepared_target.target_bp_size == 0u ||
        prepared_target.query_count < batch_size ||
        prepared_target.dispatched_query_count > prepared_target.query_count ||
        batch_size > prepared_target.query_count - prepared_target.dispatched_query_count ||
        params.max_occ < 0 || params.vulkan_fused_frontend_enabled ||
        target_build_selected != !prepared_target.persisted ||
        state.query_records.size() != batch_size || state.query_sidecars.size() != batch_size ||
        !completed_rows.empty() || completed_rows.capacity() < batch_size ||
        state.query_minimizer_device_span.count() >
            static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)()) ||
        state.query_minimizer_device_span.offset() != 0u) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "MPS Vulkan seed route has no matching grouped-SoA source owner");
    }
    if (!prepared_target.persisted && !prepared_target.ready()) {
        using row_view = typename vulkan_query_batch_state<
            p_sequence, p_batch_capacity, p_minimizer_type>::gpu_row_view;
        const std::size_t first_row = state.query_sidecars.front().row_index;
        std::optional<compact_sequence_range<p_sequence>> flat_target;
        const bool recipe_target = state.gpu_target_recipe_views[first_row].has_value();
        const p_sequence_header* const flat_target_header = target_hdrs[first_row];
        if ((flat_target_header == nullptr) == !recipe_target) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "transient MPS target requires one exclusive flat or source owner");
        }
        const row_view target_view = recipe_target
            ? *state.gpu_target_recipe_views[first_row]
            : [flat_target_owner = std::addressof(flat_target), flat_target_header]() -> row_view {
                  flat_target_owner->emplace(*flat_target_header);
                  return bioseq::storage::make_lazy_sequence_view(**flat_target_owner);
              }();
        prepare_transient_mps_resident<p_sequence, p_minimizer_type>(runtime,
                                                                     alphabet_code,
                                                                     storage_unit_byte_width,
                                                                     target_view,
                                                                     execution_plan,
                                                                     prepared_target);
    }
    if (!prepared_target.ready() || prepared_target.resident == nullptr ||
        static_cast<std::uint64_t>(params.max_occ) > prepared_target.resident_view.max_occ_cap) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "MPS Vulkan seed route did not publish a compatible resident target");
    }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    if (completed_query_operation == nullptr || completed_seed_operation == nullptr ||
        completed_query_operation == completed_seed_operation ||
        completed_query_operation->audit().kind !=
            bioseq::gpu::vulkan::runtime_pipeline_operation_kind::invalid ||
        completed_seed_operation->audit().kind !=
            bioseq::gpu::vulkan::runtime_pipeline_operation_kind::invalid ||
        completed_chaining.size() != batch_size) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "MPS Vulkan seed route has no fresh caller-owned receipt handoff");
    }
#endif

    bioseq::parallelism::memory_permit submitted_ordinals_permit;
    std::vector<std::uint32_t> submitted_ordinals;
    prepare_vulkan_query_host_capacity(
        submitted_ordinals, submitted_ordinals_permit, batch_size);
    for (std::size_t query_index = 0u; query_index < batch_size; ++query_index) {
        const auto& sidecar = state.query_sidecars[query_index];
        const auto authenticated_query_index =
            state.query_index_for_row(sidecar.row_index);
        if (!authenticated_query_index || *authenticated_query_index != query_index ||
            (query_index != 0u && sidecar.ordinal <= submitted_ordinals.back()) ||
            (scanner_pg_ctx != nullptr &&
             scanner_pg_ctx->target_rownos[sidecar.row_index] != prepared_target.target_rowno)) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "MPS Vulkan seed wave changed query ordering or target ownership");
        }
        submitted_ordinals.push_back(sidecar.ordinal);
    }
    bioseq::gpu::vulkan::gpu_seed_query_inputs query_inputs{
        .host_minimizer_bytes = {},
        .device_minimizer_words = state.query_minimizer_device_span.words(),
        .minimizer_count =
            static_cast<std::uint32_t>(state.query_minimizer_device_span.count()),
        .minimizer_word_kind = static_cast<std::uint32_t>(
            bioseq::gpu::vulkan::minimizer_wire_traits<p_minimizer_type>::k_word_kind),
        .minimizer_words32_per_record =
            bioseq::gpu::vulkan::minimizer_wire_traits<
                p_minimizer_type>::k_words32_per_minimizer,
        .hash_shift =
            bioseq::gpu::vulkan::minimizer_wire_traits<p_minimizer_type>::k_hash_shift,
        .pos_bits =
            bioseq::gpu::vulkan::minimizer_wire_traits<p_minimizer_type>::k_position_bits,
        .window_size = static_cast<std::uint32_t>(params.window_size),
        .host_query_records = state.query_records,
        .device_query_records = nullptr,
    };
    const std::uint64_t query_authority_identity = state.query_sketch_authority.identity();
    if (query_authority_identity == 0u) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "MPS Vulkan seed wave has no query sketch authority");
    }

    pipe::route_attempt_memory attempt;
    context::installed_context_scope install{attempt.fresh_context};
    bioseq::parallelism::memory_permit job_permit;
    canonical_seed_batch seeds;
    attempt.prepare_seed_job_storage(seeds.jobs, job_permit, batch_size);
    auto job_storage = std::move(seeds.jobs);
    auto readback = runtime.run_grouped_soa_seed_anchor_roundtrip(
        prepared_target.resident_view,
        query_inputs,
        params.max_occ,
        static_cast<std::uint32_t>(params.kmer_size),
        std::move(state.query_sketch_authority));
    auto completion = bioseq::seeding::collect_seed_hits_vulkan(
        std::move(readback),
        state.query_records,
        submitted_ordinals,
        /*target_position_offset=*/0u,
        prepared_target.target_bp_size,
        static_cast<std::uint32_t>(params.kmer_size),
        query_authority_identity,
        seeds,
        std::move(job_storage));
    if (!completion || !canonical_seed_jobs_match_submission(seeds, submitted_ordinals)) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "selected MPS Vulkan seed readback failed canonical authentication");
    }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    *completed_query_operation = std::move(completion.query_operation_receipt);
    *completed_seed_operation = std::move(completion.seed_operation_receipt);
    if (!completed_query_operation->is_valid_for_query_sketch() ||
        !completed_seed_operation->is_valid_for_grouped_soa_seed_anchor_roundtrip()) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "selected MPS Vulkan seed completion lost its authenticated receipts");
    }
#endif

    if (scanner_pg_ctx != nullptr &&
        scanner_pg_ctx->read_index_active != prepared_target.persisted) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "MPS Vulkan target owner disagrees with the frozen index-read route");
    }
    const auto actual_source = prepared_target.persisted
        ? ::bioseq::pipeline_stats::target_index_source::persisted
        : ::bioseq::pipeline_stats::target_index_source::recomputed;
    for (std::size_t query_index = 0u; query_index < batch_size; ++query_index) {
        bioseq::postgres::support::invoke_pg_as_cpp([] { CHECK_FOR_INTERRUPTS(); });
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        const auto seed_frontend = make_mps_seed_frontend_execution(
            *completed_seed_operation,
            query_index,
            state.query_sidecars[query_index].ordinal,
            prepared_target.persisted
                ? vulkan_seed_target_completion_kind::mps_persisted
                : vulkan_seed_target_completion_kind::mps_runtime_generated,
            prepared_target.operation_identity,
            prepared_target.persisted ? prepared_target.resident->identity().generation_id : 0u,
            prepared_target.persisted ? prepared_target.resident->identity().source_identity : 0u);
        if (!seed_frontend.is_internally_consistent() ||
            !seed_frontend.matches_runtime_operation(
                *completed_seed_operation,
                runtime.features().max_workgroup_count_x)) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "MPS Vulkan seed frontend differs from its completed operation");
        }
#endif
        run_canonical_suffix_row(state.query_sidecars[query_index].row_index,
                                 query_index,
                                 query_index,
                                 query_hdr,
                                 per_row_query_hdrs,
                                 target_hdrs,
                                 query_ids,
                                 execution_plan,
                                 broadcast_query_ordinal,
                                 scanner_pg_ctx,
                                 per_row_query_mode,
                                 actual_source,
                                 state,
                                 seeds,
                                 attempt,
                                 completed_rows
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                                 , seed_frontend
                                 , std::addressof(completed_chaining[query_index])
#endif
                                 );
    }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    if (!state.query_sketch_owner ||
        !state.query_sketch_owner->host_readback_completed ||
        !completed_query_operation->is_valid_for_query_sketch() ||
        !completed_seed_operation->is_valid_for_grouped_soa_seed_anchor_roundtrip()) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "MPS Vulkan seed route lost its query or seed receipt after the shared suffix");
    }
#endif
    if (completed_rows.size() != batch_size) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "MPS Vulkan seed route did not retain every completed suffix job");
    }
    if (prepared_target.dispatched_query_count >
            (std::numeric_limits<std::uint64_t>::max)() - batch_size ||
        prepared_target.dispatch_waves == (std::numeric_limits<std::uint64_t>::max)()) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "MPS prepared-target lifecycle counters saturated");
    }
    prepared_target.dispatched_query_count += batch_size;
    ++prepared_target.dispatch_waves;
}

/**
 * Run an ordinary scan as independently owned one-row canonical submissions.
 *
 * Unlike BatchJoin, an ordinary scan has no statement-wide target: each row
 * names a different target and therefore receives a different MPF/MPS owner.
 * MPS additionally consumes its query authorization, so a checked singleton
 * query partition is prepared for every row.  Both layouts still enter the
 * same `process_vulkan_*_seed_route` functions and `dispatch_post_seed_job`
 * suffix used by the shared-target route.
 */
template <class p_sequence,
          std::size_t p_batch_capacity,
          class p_minimizer_type,
          class p_sequence_header,
          class p_scanner_context,
          class p_runtime,
          class p_source_view_resolver>
void process_ordinary_vulkan_scan_route(
    const std::size_t batch_size,
    const std::uint8_t alphabet_code,
    const std::uint8_t storage_unit_byte_width,
    const bioseq::minimizers::minimizer_physical_layout_selection& physical_layout,
    const p_sequence_header* query_hdr,
    const std::array<const p_sequence_header*, p_batch_capacity>* per_row_query_hdrs,
    const std::array<const p_sequence_header*, p_batch_capacity>& target_hdrs,
    const std::array<std::int32_t, p_batch_capacity>& query_ids,
    const pipe::statement_execution_plan_owner& execution_plan,
    const std::uint64_t broadcast_query_ordinal,
    p_scanner_context* scanner_pg_ctx,
    const bool per_row_query_mode,
    p_runtime& runtime,
    const vulkan_seed_target_ownership ownership,
    p_source_view_resolver& source_view_for_row,
    std::vector<canonical_suffix_row_completion>& completed_rows,
    ordinary_vulkan_scan_route_completion& route_completion) {
    const bool ordinary_mpf = ownership == vulkan_seed_target_ownership::ordinary_mpf;
    const bool ordinary_mps = ownership == vulkan_seed_target_ownership::ordinary_mps;
    if ((!ordinary_mpf && !ordinary_mps) || batch_size == 0u ||
        batch_size > p_batch_capacity || scanner_pg_ctx == nullptr ||
        !scanner_pg_ctx->ordinary_vulkan_target_preparation.valid() ||
        scanner_pg_ctx->gpu_prepared_mpf_target != nullptr ||
        scanner_pg_ctx->gpu_prepared_grouped_soa_target != nullptr ||
        !same_statement_owner(scanner_pg_ctx->execution_plan, execution_plan) ||
        per_row_query_mode != (per_row_query_hdrs != nullptr) ||
        !completed_rows.empty() || completed_rows.capacity() < batch_size ||
        route_completion.query_sketch_host_ns != 0u) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "ordinary Vulkan route has no exclusive row-local target owner");
    }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    if (!route_completion.row_proofs.empty() ||
        route_completion.row_proofs.capacity() != 0u ||
        route_completion.row_proofs_permit) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "ordinary Vulkan route received a reused proof owner");
    }
    prepare_vulkan_query_host_capacity(route_completion.row_proofs,
                                       route_completion.row_proofs_permit,
                                       batch_size);
#endif

    auto& hook = scanner_pg_ctx->ordinary_vulkan_target_preparation;
    bioseq::parallelism::memory_permit row_completion_permit;
    std::vector<canonical_suffix_row_completion> row_completion;
    prepare_vulkan_query_host_capacity(row_completion, row_completion_permit, 1u);
    for (std::size_t row = 0u; row < batch_size; ++row) {
        bioseq::postgres::support::invoke_pg_as_cpp([] { CHECK_FOR_INTERRUPTS(); });
        vulkan_query_batch_state<p_sequence, p_batch_capacity, p_minimizer_type> query_state;
        prepare_vulkan_query_route_rows<p_sequence, p_batch_capacity, p_minimizer_type>(
            vulkan_query_row_partition{.first_row = row, .row_count = 1u},
            alphabet_code,
            storage_unit_byte_width,
            query_hdr,
            per_row_query_hdrs,
            execution_plan,
            scanner_pg_ctx,
            per_row_query_mode,
            broadcast_query_ordinal,
            source_view_for_row,
            query_state);
        const auto query_index = query_state.query_index_for_row(row);
        if (query_state.submitted_rows.first_row != row ||
            query_state.submitted_rows.row_count != 1u || !query_index ||
            *query_index != 0u || query_state.query_sidecars.size() != 1u ||
            query_state.query_sidecars.front().row_index != row ||
            query_state.query_records.size() != 1u) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "ordinary Vulkan query preparation changed its singleton row identity");
        }
        const auto accumulated_query_ns = bioseq::common::checked_add(
            route_completion.query_sketch_host_ns, query_state.gpu_query_sketch_host_ns);
        if (!accumulated_query_ns) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "ordinary Vulkan query-sketch host duration overflowed");
        }
        route_completion.query_sketch_host_ns = *accumulated_query_ns;

        const bool source_target = query_state.gpu_target_recipe_views[row].has_value();
        const p_sequence_header* const flat_target = target_hdrs[row];
        if ((flat_target == nullptr) == !source_target) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "ordinary Vulkan target requires one exclusive flat or source row");
        }
        const std::uint64_t target_bp_size = source_target
            ? query_state.gpu_target_recipe_views[row]->bp_size()
            : flat_target->bp_size;
        if (target_bp_size == 0u || scanner_pg_ctx->target_rownos[row] < 0) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "ordinary Vulkan target has no physical row identity or extent");
        }

        const std::size_t completion_base = completed_rows.size();
        if (ordinary_mpf) {
            using target_owner_type = typename p_scanner_context::gpu_mpf_target_owner;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            auto target_owner = std::allocate_shared<target_owner_type>(
                admitted_alignment_node_allocator<target_owner_type>{});
            target_owner_type& prepared_target = *target_owner;
#else
            target_owner_type target_owner;
            target_owner_type& prepared_target = target_owner;
#endif
            hook.prepare_mpf(hook.state,
                             *scanner_pg_ctx,
                             row,
                             target_bp_size,
                             alphabet_code,
                             storage_unit_byte_width,
                             execution_plan,
                             prepared_target);
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            std::array<bioseq::gpu::vulkan::runtime_pipeline_operation_token, 1u>
                completed_chaining{};
            bioseq::parallelism::memory_permit completed_seed_operations_permit;
            std::vector<bioseq::gpu::vulkan::runtime_pipeline_operation_token>
                completed_seed_operations;
            prepare_vulkan_query_host_capacity(completed_seed_operations,
                                               completed_seed_operations_permit,
                                               1u);
            bioseq::gpu::vulkan::device_query_sketch_authorization
                completed_query_authorization;
#endif
            process_vulkan_mpf_seed_route(1u,
                                          alphabet_code,
                                          physical_layout,
                                          query_hdr,
                                          per_row_query_hdrs,
                                          target_hdrs,
                                          query_ids,
                                          execution_plan,
                                          broadcast_query_ordinal,
                                          scanner_pg_ctx,
                                          per_row_query_mode,
                                          runtime,
                                          prepared_target,
                                          query_state,
                                          row_completion
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                                          , completed_chaining
                                          , completed_seed_operations
                                          , std::addressof(completed_query_authorization)
#endif
                                          );
            if (!prepared_target.ready() || prepared_target.target_rowno !=
                                                scanner_pg_ctx->target_rownos[row] ||
                prepared_target.target_bp_size != target_bp_size ||
                prepared_target.query_count != 1u ||
                prepared_target.dispatched_query_count != 1u ||
                prepared_target.dispatch_waves != 1u ||
                prepared_target.operation_identity == 0u ||
                (prepared_target.persisted !=
                 (prepared_target.persisted_generation_id != 0u &&
                  prepared_target.persisted_source_identity != 0u))) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error(
                    "ordinary MPF route changed its row-local target identity");
            }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            if (!query_state.query_sketch_owner ||
                !completed_query_authorization.is_valid() ||
                completed_seed_operations.size() > 1u ||
                route_completion.row_proofs.size() ==
                    route_completion.row_proofs.capacity()) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error(
                    "ordinary MPF route returned an incomplete row proof set");
            }
            ordinary_vulkan_row_proof proof{
                .query_sketch_owner = std::static_pointer_cast<const void>(
                    query_state.query_sketch_owner),
                .target_owner = std::static_pointer_cast<const void>(target_owner),
                .query_sketch_dispatch_stats =
                    query_state.query_sketch_owner->dispatch_stats,
                .target_execution = make_mpf_target_execution(prepared_target),
                .persisted_read_stats = prepared_target.persisted_read_stats,
                .mpf_query_authorization = std::move(completed_query_authorization),
                .ownership = ownership,
                .row = row,
                .target_rowno = prepared_target.target_rowno,
                .target_bp_size = prepared_target.target_bp_size,
                .target_position_offset = 0u,
                .target_operation_identity = prepared_target.operation_identity,
                .target_generation_id = prepared_target.persisted_generation_id,
                .target_source_identity = prepared_target.persisted_source_identity,
                .target_cold_load_identity = 0u,
                .target_persisted = prepared_target.persisted,
                .target_canonical_empty = prepared_target.canonical_empty,
            };
            if (!completed_seed_operations.empty()) {
                proof.seed_operation = std::move(completed_seed_operations.front());
            }
            proof.chaining_operation = std::move(completed_chaining.front());
            route_completion.row_proofs.push_back(std::move(proof));
#endif
        } else {
            using target_owner_type = typename p_scanner_context::gpu_mps_target_owner;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            auto target_owner = std::allocate_shared<target_owner_type>(
                admitted_alignment_node_allocator<target_owner_type>{});
            target_owner_type& prepared_target = *target_owner;
            bioseq::gpu::vulkan::runtime_pipeline_operation_token
                completed_query_operation;
            bioseq::gpu::vulkan::runtime_pipeline_operation_token
                completed_seed_operation;
            std::array<bioseq::gpu::vulkan::runtime_pipeline_operation_token, 1u>
                completed_chaining{};
#else
            target_owner_type target_owner;
            target_owner_type& prepared_target = target_owner;
#endif
            hook.prepare_mps(hook.state,
                             *scanner_pg_ctx,
                             row,
                             target_bp_size,
                             alphabet_code,
                             storage_unit_byte_width,
                             execution_plan,
                             prepared_target);
            process_vulkan_mps_seed_route(1u,
                                          alphabet_code,
                                          storage_unit_byte_width,
                                          query_hdr,
                                          per_row_query_hdrs,
                                          target_hdrs,
                                          query_ids,
                                          execution_plan,
                                          broadcast_query_ordinal,
                                          scanner_pg_ctx,
                                          per_row_query_mode,
                                          runtime,
                                          prepared_target,
                                          query_state,
                                          row_completion
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                                          , std::addressof(completed_query_operation)
                                          , std::addressof(completed_seed_operation)
                                          , completed_chaining
#endif
                                          );
            if (!prepared_target.ready() || prepared_target.target_rowno !=
                                                scanner_pg_ctx->target_rownos[row] ||
                prepared_target.target_bp_size != target_bp_size ||
                prepared_target.query_count != 1u ||
                prepared_target.dispatched_query_count != 1u ||
                prepared_target.dispatch_waves != 1u ||
                prepared_target.operation_identity == 0u ||
                prepared_target.resident_view.generation_id == 0u ||
                prepared_target.resident_view.source_identity == 0u ||
                prepared_target.resident_view.cold_load_identity == 0u) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error(
                    "ordinary MPS route changed its row-local target identity");
            }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            if (!query_state.query_sketch_owner ||
                !completed_query_operation.is_valid_for_query_sketch() ||
                !completed_seed_operation
                     .is_valid_for_grouped_soa_seed_anchor_roundtrip() ||
                route_completion.row_proofs.size() ==
                    route_completion.row_proofs.capacity()) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error(
                    "ordinary MPS route returned an incomplete row proof set");
            }
            route_completion.row_proofs.push_back(ordinary_vulkan_row_proof{
                .query_sketch_owner = std::static_pointer_cast<const void>(
                    query_state.query_sketch_owner),
                .target_owner = std::static_pointer_cast<const void>(target_owner),
                .query_sketch_dispatch_stats =
                    query_state.query_sketch_owner->dispatch_stats,
                .target_execution = make_mps_target_execution(prepared_target),
                .persisted_read_stats = prepared_target.persisted_read_stats,
                .query_operation = std::move(completed_query_operation),
                .seed_operation = std::move(completed_seed_operation),
                .chaining_operation = std::move(completed_chaining.front()),
                .ownership = ownership,
                .row = row,
                .target_rowno = prepared_target.target_rowno,
                .target_bp_size = prepared_target.target_bp_size,
                .target_position_offset = 0u,
                .target_operation_identity = prepared_target.operation_identity,
                .target_generation_id = prepared_target.resident_view.generation_id,
                .target_source_identity = prepared_target.resident_view.source_identity,
                .target_cold_load_identity = prepared_target.resident_view.cold_load_identity,
                .target_persisted = prepared_target.persisted,
                .target_canonical_empty = prepared_target.canonical_empty,
            });
#endif
        }
        if (row_completion.size() != 1u || row_completion.front().row != row ||
            row_completion.front().query_id != query_ids[row] ||
            completed_rows.size() != completion_base ||
            completed_rows.size() == completed_rows.capacity()) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "ordinary Vulkan route changed physical row or query-id order");
        }
        completed_rows.push_back(std::move(row_completion.front()));
        row_completion.clear();
        if (completed_rows.size() != completion_base + 1u) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "ordinary Vulkan route failed its admitted row handoff");
        }
    }
    if (completed_rows.size() != batch_size
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        || route_completion.row_proofs.size() != batch_size
#endif
        ) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "ordinary Vulkan route did not retain every completed row");
    }
}

} // namespace bioseq::simd::typed_dispatch::typed_support_detail
