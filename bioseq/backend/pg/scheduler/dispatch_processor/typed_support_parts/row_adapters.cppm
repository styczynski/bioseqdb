/**
 * @file typed_support_parts/row_adapters.cppm
 * @brief Defines locator identity checks and PostgreSQL source-provider adapters.
 */
module;

#include <algorithm>
#include <array>
#include <bioseq/pg_compat.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <libassert/assert.hpp>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

export module bioseq.postgres.scheduler.dispatch_processor.typed_support:row_adapters;

import bioseq.postgres.scheduler.dispatch_processor;
import :policy_stats;
import :pg_alignment_errors;
import bioseq.highway.dispatch_contract;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.common.hashing;
import bioseq.common.seqan_adapter;
import bioseq.postgres.support;
import bioseq.postgres.configuration.core;
import bioseq.postgres.configuration.scheduler;
import bioseq.indexing.type;
import bioseq.indexing.traits;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_sequence;
import bioseq.indexing.gpu_minimizer_index;
import bioseq.postgres.storage.indexing.chunked_storage;
import bioseq.postgres.storage.indexing.handles;
import bioseq.postgres.storage.indexing.residency;
import bioseq.postgres.storage.indexing.sorted_runs;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.chaining.vulkan;
import bioseq.sequences.cigar;
import bioseq.pipeline.minimap_port;
import bioseq.pipeline.pipe;
import bioseq.gpu.vulkan.runtime;
import bioseq.postgres.types.compact_sequence;
import bioseq.instrumentation.clock;
import bioseq.instrumentation.configuration;
import bioseq.instrumentation.descriptor;
import bioseq.instrumentation.profiler_state;
import bioseq.instrumentation.region;
import bioseq.instrumentation.runtime;
import bioseq.postgres.storage.manager;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.storage.layout;
import bioseq.postgres.storage.scanner;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.pipeline_ops;
import bioseq.postgres.scheduler.supervisor;
import bioseq.postgres.scheduler.supervisor.vulkan;

export namespace bioseq::simd::typed_dispatch::typed_support_detail {

/// Compare every locator field before one resident target is shared by a
/// query-parallel batch. The comparison stays PostgreSQL-free so it can also
/// serve as a preflight stats before any worker is entered.
[[nodiscard]] constexpr bool
same_dispatch_target_locator(const bioseq::storage::sequence_locator& left,
                             const bioseq::storage::sequence_locator& right) noexcept {
    return left.payload_version == right.payload_version &&
           left.bucket_index == right.bucket_index && left.reserved_16 == right.reserved_16 &&
           left.shard_index == right.shard_index && left.fragment_count == right.fragment_count &&
           left.reserved_32 == right.reserved_32 && left.logical_length == right.logical_length &&
           left.first_fragment_row == right.first_fragment_row &&
           left.source_epoch == right.source_epoch;
}

/// Validate the complete immutable hash-major reader identity before it is
/// borrowed concurrently. No worker may observe a partially authenticated,
/// mixed-layout or storage-profile-transcoded prepared group.
[[nodiscard]] inline bool
hash_major_parallel_reader_identity_valid(const batch_processor::cpu_prepared_index_group& group,
                                          const std::uint64_t target_bp_size) noexcept {
    if (!group.persisted_hash_major_reader.has_value() ||
        group.persisted_fragment_mapping_guard.has_value() ||
        group.persisted_fragment_reader.has_value() ||
        group.persisted_grouped_soa_reader.has_value() || group.target_index ||
        group.target_index_source != ::bioseq::pipeline_stats::target_index_source::persisted ||
        group.persisted_generation_id == 0u || group.persisted_source_identity == 0u ||
        !group.persisted_hash_major_source_identity_present ||
        group.persisted_hash_major_source_identity == std::array<std::byte, 16>{} ||
        group.persisted_source_identity !=
            hash_major_source_diagnostic_token(group.persisted_hash_major_source_identity) ||
        !group.query_hash_union.empty() || group.query_hash_union.capacity() != 0u ||
        group.persisted_hash_major_reader->target_length() != target_bp_size) {
        return false;
    }

    const auto& receipt = group.persisted_read_stats.hash_major;
    const auto& memory = group.persisted_hash_major_reader->memory_stats();
    const std::uint64_t stage_peak = std::max({
        receipt.directory_stage_peak_bytes,
        receipt.control_stage_peak_bytes,
        receipt.posting_stage_peak_bytes,
    });
    return receipt.layout_revision == bioseq::indexing::k_hash_major_minimizer_layout_revision &&
           receipt.sessions == 1u && receipt.generation_id == group.persisted_generation_id &&
           receipt.source_identity == group.persisted_hash_major_source_identity &&
           receipt.directory_row_crc_receipts == receipt.directory_rows &&
           receipt.control_row_crc_receipts == receipt.control_rows &&
           receipt.posting_row_crc_receipts == receipt.posting_rows &&
           receipt.control_block_crc_receipts == receipt.selected_block_count &&
           receipt.control_semantic_receipts == receipt.selected_block_count &&
           receipt.posting_ranges_delivered == receipt.posting_semantic_receipts &&
           receipt.posting_range_crc_receipts <= receipt.posting_ranges_delivered &&
           receipt.query_count == memory.query_count &&
           memory.query_count == group.canonical_query_hash_union_u32.size() &&
           receipt.accepted_hash_count == memory.accepted_hash_count &&
           receipt.retained_position_count == memory.retained_position_count &&
           receipt.peak_admitted_bytes == stage_peak &&
           receipt.retained_owner_bytes == receipt.admitted_bytes &&
           receipt.admitted_bytes == group.persisted_hash_major_reader->admitted_bytes() &&
           receipt.generic_source_fallback_calls == 0u && receipt.failures == 0u &&
           !receipt.saturated;
}

/// grouped-SoA counterpart of the concurrent-reader stats. The reader owns only
/// independent fragment address plans. Query routing/count reduction is
/// transient wave state, and every persisted posting remains owned by exactly
/// one fragment.
[[nodiscard]] inline bool
fragment_parallel_reader_identity_valid(const batch_processor::cpu_prepared_index_group& group,
                                        const std::uint64_t target_bp_size) noexcept {
    if (!group.persisted_fragment_reader.has_value() ||
        !group.persisted_fragment_mapping_guard.has_value() ||
        !*group.persisted_fragment_mapping_guard ||
        group.persisted_grouped_soa_reader.has_value() ||
        group.persisted_hash_major_reader.has_value() || group.target_index ||
        group.target_index_source != ::bioseq::pipeline_stats::target_index_source::persisted ||
        group.persisted_generation_id == 0u || group.persisted_source_identity == 0u ||
        group.persisted_hash_major_source_identity_present ||
        group.persisted_hash_major_source_identity != std::array<std::byte, 16>{} ||
        !group.query_hash_union.empty() || group.query_hash_union.capacity() != 0u ||
        group.persisted_fragment_reader->target_length() != target_bp_size) {
        return false;
    }
    const auto& read = group.persisted_read_stats;
    const auto& reader = *group.persisted_fragment_reader;
    const auto memory = reader.memory_stats();
    const bool exact_lookup_backing_identity = [&group, &memory, &read, &reader]() noexcept {
        switch (group.persisted_fragment_lookup_route) {
        case bioseq::indexing::minimizer_fragment_lookup_route::direct_compact_hash_lookup:
            return reader.compact_direct_ready() && reader.compact_direct_only() &&
                   reader.mapped_grouped_soa_payload_bytes() == 0u &&
                   reader.grouped_soa_directory_entry_count() == 0u &&
                   memory.directory_capacity_bytes == 0u &&
                   memory.ordinal_route_capacity_bytes == 0u && read.directory_chunks_read == 0u &&
                   read.directory_bytes_read == 0u;
        case bioseq::indexing::minimizer_fragment_lookup_route::grouped_soa_control_scans:
            return !reader.compact_direct_ready() && !reader.compact_direct_only() &&
                   reader.mapped_grouped_soa_payload_bytes() > 0u &&
                   reader.grouped_soa_directory_entry_count() > 0u &&
                   memory.directory_capacity_bytes > 0u &&
                   memory.ordinal_route_capacity_bytes > 0u && read.directory_chunks_read > 0u &&
                   read.directory_bytes_read > 0u;
        }
        return false;
    }();
    const bool deferred_query_set_identity =
        group.compact_direct_query_set_validation_pending
            ? group.persisted_fragment_lookup_route ==
                      bioseq::indexing::minimizer_fragment_lookup_route::
                          direct_compact_hash_lookup &&
                  group.canonical_query_hash_union_u32.empty() &&
                  group.canonical_query_hash_union_u32.capacity() == 0u &&
                  group.key_set_checksum == 0u && !group.build_set_checksum_present &&
                  group.build_set_checksum == 0u
            : group.build_set_checksum_present &&
                  group.build_set_checksum == group.key_set_checksum;
    return reader.ready() && exact_lookup_backing_identity && deferred_query_set_identity &&
           memory.query_record_count == 0u && memory.query_record_capacity_bytes == 0u &&
           memory.query_hash_capacity_bytes == 0u && memory.capacity_accounting_passes == 1u &&
           memory.global_position_storage_preparations == 0u &&
           memory.global_position_storage_allocations == 0u &&
           memory.position_append_growth_reservations == 0u &&
           memory.retained_position_count == 0u && memory.retained_position_capacity == 0u &&
           memory.global_position_capacity_bytes == 0u &&
           memory.query_record_position_capacity_bytes == 0u &&
           memory.retained_selected_control_capacity_bytes == 0u &&
           memory.current_selected_control_capacity_bytes == 0u &&
           memory.selected_extent_capacity_bytes == 0u &&
           memory.control_payload_capacity_bytes == 0u && memory.retained_layout_equation_holds() &&
           memory.owned_capacity_bytes == reader.admitted_bytes() && read.relation_opens == 1u &&
           read.head_scans == 1u && read.manifest_directory_scans == 1u && read.index_scans == 2u &&
           read.metadata_repeated_reads == 0u && read.full_generation.scan_calls == 0u &&
           read.bulk_read_calls == 0u && read.range_scan_calls == 0u &&
           read.payload_heap_rows_read == 0u && read.payload_bytes_read == 0u &&
           read.source_parent_rows_read == 1u && read.source_catalog_rows_read == 1u &&
           read.source_catalog_index_scans == 1u && read.source_fragment_rows_read == 0u &&
           read.source_decoded_bp == 0u;
}

/// Attach the retained query-sketch stats after a worker has joined. The
/// prebuilt-minimizer entry point must not publish its own query stats. This
/// check makes duplicate attachment fail closed on the PostgreSQL thread.
inline void
attach_retained_hash_major_query_stats(match_result_collection& collection,
                                       const pipe::minimizer_range_execution_stats& stats) {
    const bool exact_query_stats_absent =
        !collection.exact_simd_query_execution_stats_available &&
        collection.exact_simd_query_execution_stats ==
            decltype(collection.exact_simd_query_execution_stats){};
    const bool scalar_query_stats_absent =
        collection.scalar_pipeline_stats.query_minimizers ==
        decltype(collection.scalar_pipeline_stats.query_minimizers){};
    const bool bounded_query_stats_absent =
        !collection.bounded_nthash_query_execution_stats_available &&
        collection.bounded_nthash_query_execution_stats ==
            decltype(collection.bounded_nthash_query_execution_stats){};
    if (!scalar_query_stats_absent || !exact_query_stats_absent || !bounded_query_stats_absent) {
        raise_typed_dispatch_pg_error_as_cpp(
            ERRCODE_INTERNAL_ERROR, "persisted query sketch stats were recorded more than once");
    }
    if (stats.exact_simd_available) {
        collection.record_exact_simd_query_execution_stats(stats.exact_simd);
    }
    if (stats.bounded_nthash_available) {
        collection.record_bounded_nthash_query_execution_stats(stats.bounded_nthash);
    }
    if (stats.scalar_available) {
        collection.record_scalar_query_minimizer_execution_stats(stats.scalar);
    }
}

/**
 * Promote one decoded sequence locator through the statement's exact source
 * provider, then expose only the canonical lazy facade to the algorithm.
 * The provider remains scan-owned. The read session owns the facade adapter
 * and is closed only after the callback has joined/drained all algorithm work.
 */
template <class p_alphabet, class p_function>
inline void with_source_provider_pg_locator(batch_processor::scanner_pg_context& context,
                                            const bioseq::storage::sequence_locator& locator,
                                            const AttrNumber column_attno,
                                            const std::uint8_t alphabet_code,
                                            const std::uint8_t storage_unit_byte_width,
                                            const std::uint16_t last_padding_letters,
                                            pg_sequence_read_session* active_read_session,
                                            p_function&& callback) {
    auto* provider = context.source_provider;
    auto* registry = provider != nullptr ? provider->lease.registry : nullptr;
    const bool exact_provider =
        provider != nullptr && provider->open &&
        provider->kind == bioseq::storage::pg_sequence_source_provider_kind::scan &&
        provider->lease.owned && registry != nullptr &&
        provider->lease.provider_generation == registry->generation &&
        registry->accepting_lookups && !registry->provider_closed && !registry->lease_fault &&
        !registry->xact_ended && registry->backend_thread.is_current() &&
        registry->user_relid == context.user_relid && registry->database_oid == context.db_oid &&
        registry->tablespace_oid == context.tablespace_oid &&
        registry->main_relfilenode == context.main_relfilenode &&
        registry->authoritative_snapshot == context.snapshot && context.snapshot != nullptr &&
        IsMVCCSnapshot(context.snapshot) && context.snapshot->regd_count > 0 && column_attno > 0 &&
        context.parent_mctx != nullptr;
    if (!exact_provider) {
        raise_typed_dispatch_pg_error_as_cpp(
            ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
            "locator sequence has no exact statement source-provider route");
    }

    bioseq::pg_types::seq* locator_stub = bioseq::postgres::support::invoke_pg_as_cpp(
        [&context, &locator, alphabet_code, storage_unit_byte_width, last_padding_letters] {
            return bioseq::storage::alloc_locator_stub_in(context.parent_mctx,
                                                          alphabet_code,
                                                          storage_unit_byte_width,
                                                          last_padding_letters,
                                                          locator.logical_length,
                                                          locator);
        });
    const Datum promoted = bioseq::storage::promote_pg_sequence_locator(
        provider, column_attno, PointerGetDatum(locator_stub), context.parent_mctx);
    const pg_sequence_descriptor descriptor = bioseq::postgres::support::invoke_pg_as_cpp(
        [promoted] { return ::classify_and_describe_sequence(promoted); });
    if (!descriptor.alphabet_bound ||
        (descriptor.kind != pg_sequence_datum_kind::expanded_read_only &&
         descriptor.kind != pg_sequence_datum_kind::expanded_read_write) ||
        descriptor.alphabet_code != alphabet_code ||
        descriptor.storage_unit_byte_width != storage_unit_byte_width ||
        descriptor.bp_length != locator.logical_length) {
        raise_typed_dispatch_pg_error_as_cpp(
            ERRCODE_DATA_CORRUPTED,
            "source-provider promotion returned mismatched sequence metadata");
    }

    if (active_read_session != nullptr) {
        auto* owner = bioseq::postgres::support::invoke_pg_as_cpp(
            [active_read_session, promoted, descriptor] {
                return ::make_pg_sequence_call_owner<p_alphabet>(
                    *active_read_session, promoted, descriptor);
            });
        std::forward<p_function>(callback)(owner->view(), active_read_session);
        return;
    }

    auto* local_session =
        bioseq::postgres::support::invoke_pg_as_cpp([parent_mctx = context.parent_mctx] {
            return ::begin_pg_sequence_read_session(parent_mctx);
        });
    try {
        auto* owner =
            bioseq::postgres::support::invoke_pg_as_cpp([local_session, promoted, descriptor] {
                return ::make_pg_sequence_call_owner<p_alphabet>(
                    *local_session, promoted, descriptor);
            });
        std::forward<p_function>(callback)(owner->view(), local_session);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [local_session] { ::finish_pg_sequence_read_session(local_session, true); });
    } catch (...) {
        ::abort_pg_sequence_read_session_noexcept(local_session);
        throw;
    }
}

template <class p_alphabet, class p_function>
inline void with_pg_sequence_recipe(batch_processor::scanner_pg_context& context,
                                    const Datum recipe,
                                    const pg_sequence_descriptor& descriptor,
                                    const std::uint8_t alphabet_code,
                                    const std::uint8_t storage_unit_byte_width,
                                    p_function&& callback) {
    const bool expanded = recipe != Datum{0} &&
                          VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(recipe)) &&
                          (descriptor.kind == pg_sequence_datum_kind::expanded_read_only ||
                           descriptor.kind == pg_sequence_datum_kind::expanded_read_write);
    if (!expanded || !descriptor.alphabet_bound || descriptor.alphabet_code != alphabet_code ||
        descriptor.storage_unit_byte_width != storage_unit_byte_width ||
        context.parent_mctx == nullptr) {
        raise_typed_dispatch_pg_error_as_cpp(ERRCODE_DATA_CORRUPTED,
                                             "retained sequence recipe metadata is invalid");
    }

    if (context.read_session != nullptr) {
        auto* owner = bioseq::postgres::support::invoke_pg_as_cpp(
            [read_session = context.read_session, recipe, descriptor] {
                return ::make_pg_sequence_call_owner<p_alphabet>(*read_session, recipe, descriptor);
            });
        std::forward<p_function>(callback)(owner->view(), context.read_session);
        return;
    }

    auto* local_session =
        bioseq::postgres::support::invoke_pg_as_cpp([parent_mctx = context.parent_mctx] {
            return ::begin_pg_sequence_read_session(parent_mctx);
        });
    try {
        auto* owner =
            bioseq::postgres::support::invoke_pg_as_cpp([local_session, recipe, descriptor] {
                return ::make_pg_sequence_call_owner<p_alphabet>(
                    *local_session, recipe, descriptor);
            });
        std::forward<p_function>(callback)(owner->view(), local_session);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [local_session] { ::finish_pg_sequence_read_session(local_session, true); });
    } catch (...) {
        ::abort_pg_sequence_read_session_noexcept(local_session);
        throw;
    }
}

template <class p_sequence, class p_function>
inline void with_source_provider_pg_target(batch_processor::scanner_pg_context& context,
                                           const bioseq::storage::sequence_locator& locator,
                                           const std::uint8_t alphabet_code,
                                           const std::uint8_t storage_unit_byte_width,
                                           const std::uint16_t last_padding_letters,
                                           pg_sequence_read_session* active_read_session,
                                           p_function&& callback) {
    using alphabet = typename compact_sequence_range<p_sequence>::traits::alphabet_type;
    with_source_provider_pg_locator<alphabet>(context,
                                              locator,
                                              static_cast<AttrNumber>(context.mmsearch_seq_attno),
                                              alphabet_code,
                                              storage_unit_byte_width,
                                              last_padding_letters,
                                              active_read_session,
                                              std::forward<p_function>(callback));
}

} // namespace bioseq::simd::typed_dispatch::typed_support_detail
