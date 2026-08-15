/**
 * @file generation_fixture_read.cppm
 * @brief PostgreSQL grouped-SoA generation fixture read and failure support.
 */
module;
#include "access/heapam.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "commands/tablespace.h"
#include "executor/tuptable.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

#include <bioseq/pg_compat.h>
#include <libassert/assert.hpp>
extern "C" {
#include "storage/buffile.h"
}
#include <algorithm>
#include <array>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <expected>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
export module bioseq.postgres.storage.indexing.admin:generation_fixture_read;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.postgres.support;
import bioseq.postgres.sql_boundary;
import bioseq.postgres.configuration.aligner;
import bioseq.postgres.configuration.core;
import bioseq.postgres.storage.manager; // seq_caches, sequence_locator, is_locator_stub, decode_locator_from_stub
import bioseq.storage.layout;
import bioseq.storage.size;
import bioseq.storage.lazy_sequence;           // make_resident_lazy_sequence
import bioseq.storage.scanner;                 // sequence access requirements
import bioseq.postgres.storage.scanner;        // with_postgres_lazy_sequence
import bioseq.postgres.types.compact_sequence; // bioseq::pg_types::seq, dispatch_instance
import bioseq.sequences;                       // compact_sequence_range
import bioseq.common.seqan_adapter;            // seqan3::char_is_valid_for
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.registry;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.hash_major_minimizer_index;
import bioseq.indexing.minimizer_sequence;
import bioseq.pipeline.runtime_contract; // minimizer_algorithm
import bioseq.sequences.kmers;           // minimizer/index APIs
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.highway.dispatch_contract;
import bioseq.indexing.symbol_counts;
import bioseq.postgres.storage.indexing.chunked_storage;
import bioseq.postgres.storage.indexing.residency;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.mode;
import bioseq.parallelism.pool;
import bioseq.postgres.scheduler.supervisor;
import bioseq.postgres.scheduler.supervisor.vulkan;
import bioseq.postgres.table.io;
import bioseq.postgres.storage.indexing.handles;
import bioseq.postgres.column.table_access;
import bioseq.postgres.storage.metadata; // column_table_row_count
static_assert(__cplusplus >= 202302L, "C++23-or-later");
import :policy_catalog;
import :hash_major;
import :grouped_soa;
import :rebuild;
import :generation_fixture_write;
import :inspection_policy;

export namespace bioseq::storage::indexing {

/// Integration-only validation result for the one-scan full-generation payload.
struct minimizer_generation_full_payload_fixture_result {
    std::string table_name;
    std::string selected_backend_name;
    std::uint64_t generation_id = 0;
    std::uint64_t control_bytes = 0;
    std::uint64_t position_bytes = 0;
    std::uint64_t control_checksum = 0;
    std::uint64_t position_checksum = 0;
    minimizer_generation_read_stats read_stats{};
};

/// Reopen and consume the active fixture generation under one independently
/// registered MVCC snapshot. The selected wire format chooses the qualified
/// host reader only; this integration seam does not participate in mmsearch.
[[nodiscard]] inline minimizer_generation_integration_fixture_result
read_minimizer_generation_integration_fixture_impl(
    Oid rel_oid,
    std::string_view index_name,
    std::uint64_t sequence_rowno,
    std::uint16_t kmer_size,
    std::uint16_t window_size,
    bioseq::indexing::minimizer_generation_layout layout,
    bioseq::minimizers::minimizer_sketch_backend backend) {
    if (!OidIsValid(rel_oid) || !bioseq::postgres::support::invoke_pg_as_cpp([rel_oid] {
            return relation_oid_uses_column_table_access_method(rel_oid);
        })) {
        ready_minimizer_error_as_cpp(
            ERRCODE_WRONG_OBJECT_TYPE,
            "bioseq minimizer generation fixture reader requires a USING bioseq table");
    }
    if (!bioseq::postgres::support::invoke_pg_as_cpp([] { return superuser(); })) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INSUFFICIENT_PRIVILEGE,
            "bioseq minimizer generation fixture reader requires superuser");
    }
    if (index_name != k_minimizers_per_fragment_index_name &&
        index_name != k_minimizers_per_sequence_index_name) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "bioseq minimizer generation fixture reader requires a minimizer index");
    }
    if (kmer_size == 0 || window_size == 0 ||
        !bioseq::indexing::sql_bigint_can_represent(sequence_rowno) ||
        layout != bioseq::indexing::minimizer_generation_layout::grouped_soa_segment_local) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "bioseq minimizer generation fixture reader parameters are invalid");
    }

    const std::int16_t seq_attno = single_seq_attno_for_inspection(rel_oid);
    const relation_identity rid = index_relation_identity(rel_oid);
    minimizer_generation_administration_identity identity{
        .database_oid = rid.db_oid,
        .tablespace_oid = rid.tablespace_oid,
        .main_relfilenode = rid.relfilenode,
        .user_relid = rel_oid,
        .column_attno = seq_attno,
        .index_name = std::string{index_name},
    };

    struct fixture_read_state {
        Snapshot registered_snapshot = nullptr;
        std::optional<minimizer_generation_source_authorization> source_authorization;
        std::optional<minimizer_generation_catalog_source_authorization>
            catalog_source_authorization;
        std::optional<index_storage_fragment_generation_read_session> session;
        std::optional<bioseq::indexing::prepared_grouped_soa_query_reader> grouped_soa_reader;
        std::vector<std::uint32_t> control_ordinals;
        std::vector<std::uint32_t> position_ordinals;
        std::vector<minimizer_generation_payload_read_group> control_groups;
        std::vector<minimizer_generation_payload_read_group> position_groups;
        std::vector<minimizer_generation_payload_read_group> selective_probe_control_groups;
        std::vector<minimizer_generation_payload_read_group> selective_probe_position_groups;
        bool selective_probe_full_generation = false;
    };
    auto* state = new fixture_read_state{};
    minimizer_generation_integration_fixture_result result{};
    try {
        Snapshot active_snapshot =
            bioseq::postgres::support::invoke_pg_as_cpp([] { return GetActiveSnapshot(); });
        if (active_snapshot == nullptr || !IsMVCCSnapshot(active_snapshot)) {
            ready_minimizer_error_as_cpp(
                ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                "bioseq minimizer generation fixture reader requires an active MVCC snapshot");
        }
        state->registered_snapshot = bioseq::postgres::support::invoke_pg_as_cpp(
            [active_snapshot] { return RegisterSnapshot(active_snapshot); });
        state->source_authorization.emplace(authorize_minimizer_generation_source_pg(
            identity, sequence_rowno, state->registered_snapshot));
        minimizer_generation_source_locator source =
            state->source_authorization->source_for_diagnostics();
        const std::uint64_t source_content_checksum =
            state->source_authorization->source_content_checksum_for_diagnostics();
        if (source.locator.logical_length == 0) {
            ready_minimizer_error_as_cpp(
                ERRCODE_INVALID_PARAMETER_VALUE,
                "bioseq minimizer generation fixture reader requires a nonempty target");
        }
        result.table_name = format_seqidx_name(index_name,
                                               rid.db_oid,
                                               rid.tablespace_oid,
                                               rid.relfilenode,
                                               seq_attno,
                                               source.locator.bucket_index,
                                               source.locator.shard_index);
        result.layout = layout;
        result.source_content_checksum = source_content_checksum;
        result.source_num_fragments = source.locator.fragment_count;
        result.source_total_bp = source.locator.logical_length;
        result.source_padding_letters = source.padding_letters;
        identity.table_name = result.table_name;

        const auto expected =
            bioseq::indexing::expected_grouped_soa_generation_identity_for_backend(
                layout,
                backend,
                source.alphabet_code,
                source.storage_unit_byte_width,
                kmer_size,
                window_size,
                bioseq::indexing::k_grouped_soa_default_max_occurrence_cap);
        if (!expected) {
            ready_minimizer_error_as_cpp(
                ERRCODE_INVALID_PARAMETER_VALUE,
                "bioseq minimizer generation fixture reader storage contract is invalid",
                fmt::format("wire_error={}", static_cast<unsigned>(expected.error())));
        }
        state->session.emplace(result.table_name,
                               sequence_rowno,
                               state->registered_snapshot,
                               *expected,
                               rid.tablespace_oid,
                               minimizer_generation_snapshot_registration::borrow_registered);
        index_storage_fragment_generation_read_session& session = *state->session;
        const minimizer_generation_head head = session.read_active_head_once();
        const minimizer_generation_read_metadata& metadata =
            session.read_manifest_directory_once(head);
        session.consume_full_source_authorization(std::move(*state->source_authorization));
        state->source_authorization.reset();

        result.generation_id = head.generation_id;
        result.storage_contract_checksum =
            bioseq::indexing::compute_minimizer_generation_storage_contract_checksum(
                metadata.manifest);
        source.storage_contract_checksum = result.storage_contract_checksum;
        result.source_identity = minimizer_generation_source_identity(rid.relfilenode, source);
        result.control_count = metadata.manifest.control_count;
        result.position_count = metadata.manifest.position_count;
        result.directory_entries = metadata.directory.entries().size();
        result.position_segment_count = metadata.manifest.position_segment_count;
        if (head.layout != layout || metadata.manifest.layout != layout ||
            head.generation_id != metadata.manifest.generation_id ||
            head.source_identity != result.source_identity ||
            metadata.manifest.source_identity != result.source_identity ||
            head.source_content_checksum != source_content_checksum ||
            metadata.manifest.source_content_checksum != source_content_checksum ||
            head.storage_contract_checksum != result.storage_contract_checksum) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq minimizer generation fixture reader identity disagrees");
        }

        /* Integration-only cold-read telemetry probe.  A directory entry is
         * an authenticated control+position pair, so a strict subset exists
         * only when the generation owns more than one populated entry.  Read
         * one exact entry in that case.  A one-entry generation is already the
         * complete payload and must use the full-generation scan; an empty
         * directory must execute the empty-payload stage protocol. This
         * keeps route identity, row counts, byte counts, and TID transitions
         * tied to I/O that actually occurred. */
        const auto directory_entries = metadata.directory.entries();
        const bool generation_payload_empty = directory_entries.empty();
        std::uint64_t total_directory_payload_rows = 0;
        for (const grouped_soa_bucket_directory_entry& entry : directory_entries) {
            total_directory_payload_rows = bioseq::common::saturating_add(
                total_directory_payload_rows,
                bioseq::common::saturating_add(
                    static_cast<std::uint64_t>(entry.control_chunk_count),
                    static_cast<std::uint64_t>(entry.position_chunk_count)));
        }
        if (total_directory_payload_rows == (std::numeric_limits<std::uint64_t>::max)()) {
            ready_minimizer_error_as_cpp(
                ERRCODE_PROGRAM_LIMIT_EXCEEDED,
                "bioseq minimizer generation fixture directory payload-row count overflows");
        }
        std::optional<std::size_t> selective_probe_entry;
        if (!generation_payload_empty) {
            for (std::size_t ordinal = 0; ordinal < directory_entries.size(); ++ordinal) {
                const grouped_soa_bucket_directory_entry& entry = directory_entries[ordinal];
                const std::uint64_t entry_payload_rows = bioseq::common::saturating_add(
                    static_cast<std::uint64_t>(entry.control_chunk_count),
                    static_cast<std::uint64_t>(entry.position_chunk_count));
                if (entry.position_segment >= 0 && entry.control_chunk_count != 0 &&
                    entry.position_chunk_count != 0 &&
                    entry_payload_rows < total_directory_payload_rows &&
                    entry.sub_key != (std::numeric_limits<index_sub_key_t>::max)()) {
                    selective_probe_entry = ordinal;
                    break;
                }
            }
        }
        state->selective_probe_full_generation =
            !generation_payload_empty && !selective_probe_entry.has_value();
        if (selective_probe_entry) {
            const grouped_soa_bucket_directory_entry& entry =
                directory_entries[*selective_probe_entry];
            const auto position_sub_key = derive_minimizer_position_extent_sub_key(entry.sub_key);
            if (!position_sub_key ||
                *position_sub_key == (std::numeric_limits<index_sub_key_t>::max)()) {
                ready_minimizer_error_as_cpp(
                    ERRCODE_DATA_CORRUPTED,
                    "bioseq minimizer generation fixture selective position extent is invalid");
            }
            state->selective_probe_control_groups.push_back(minimizer_generation_payload_read_group{
                .range =
                    index_sub_key_range{
                        .generation_id = result.generation_id,
                        .position_segment = entry.position_segment,
                        .begin = entry.sub_key,
                        .end = entry.sub_key + 1,
                    },
                .requests =
                    {
                        index_sub_key_request{
                            .generation_id = result.generation_id,
                            .position_segment = entry.position_segment,
                            .sub_key = entry.sub_key,
                            .first_chunk_no = entry.control_first_chunk,
                            .expected_chunk_count = entry.control_chunk_count,
                            .expected_total_bytes = entry.control_encoded_bytes,
                            .expected_checksum = entry.control_checksum,
                        },
                    },
            });
            state->selective_probe_position_groups.push_back(
                minimizer_generation_payload_read_group{
                    .range =
                        index_sub_key_range{
                            .generation_id = result.generation_id,
                            .position_segment = entry.position_segment,
                            .begin = *position_sub_key,
                            .end = *position_sub_key + 1,
                        },
                    .requests =
                        {
                            index_sub_key_request{
                                .generation_id = result.generation_id,
                                .position_segment = entry.position_segment,
                                .sub_key = *position_sub_key,
                                .first_chunk_no = entry.position_first_chunk,
                                .expected_chunk_count = entry.position_chunk_count,
                                .expected_total_bytes = entry.position_encoded_bytes,
                                .expected_checksum = entry.position_checksum,
                            },
                        },
                });
        }

        const std::array<std::uint64_t, 1> query_hashes{
            generation_payload_empty ? std::uint64_t{1}
                                     : metadata.directory.entries().front().lo_hash};
        std::optional<minimizer_generation_encoding_error> reader_error;
        for (const grouped_soa_bucket_directory_entry& entry : metadata.directory.entries()) {
            if (entry.lo_hash <= query_hashes.front() && query_hashes.front() <= entry.hi_hash) {
                state->control_ordinals.push_back(entry.partition_id);
            }
        }
        state->position_ordinals = state->control_ordinals;
        auto reader = bioseq::indexing::prepared_grouped_soa_query_reader::begin(
            bioseq::parallelism::memory_admission_controller::instance(),
            head,
            metadata.manifest,
            *expected,
            metadata.directory.encoded_bytes(),
            query_hashes,
            metadata.manifest.max_occ_cap);
        if (reader) {
            state->grouped_soa_reader.emplace(std::move(*reader));
        } else {
            reader_error = reader.error();
        }
        if (reader_error) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq minimizer generation fixture reader could not prepare wire reader",
                fmt::format("layout={} wire_error={}",
                            static_cast<unsigned>(layout),
                            static_cast<unsigned>(*reader_error)));
        }
        if (!generation_payload_empty && state->control_ordinals.empty()) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq minimizer generation fixture reader selected no control extent");
        }

        std::uint64_t expected_control_bytes = 0;
        for (const std::uint32_t ordinal : state->control_ordinals) {
            const grouped_soa_bucket_directory_entry* entry =
                metadata.directory.entry_by_ordinal(ordinal);
            if (entry == nullptr || entry->sub_key < 0 ||
                entry->sub_key == (std::numeric_limits<index_sub_key_t>::max)()) {
                ready_minimizer_error_as_cpp(
                    ERRCODE_DATA_CORRUPTED,
                    "bioseq minimizer generation fixture reader control ordinal is invalid");
            }
            expected_control_bytes = bioseq::common::saturating_add(expected_control_bytes,
                                                                    entry->control_encoded_bytes);
            result.control_encoded_bytes = bioseq::common::saturating_add(
                result.control_encoded_bytes, entry->control_encoded_bytes);
            result.control_chunks =
                bioseq::common::saturating_add(result.control_chunks, entry->control_chunk_count);
            state->control_groups.emplace_back();
            minimizer_generation_payload_read_group& group = state->control_groups.back();
            group.range = index_sub_key_range{
                .generation_id = result.generation_id,
                .position_segment = entry->position_segment,
                .begin = entry->sub_key,
                .end = entry->sub_key + 1,
            };
            group.requests.push_back(index_sub_key_request{
                .generation_id = result.generation_id,
                .position_segment = entry->position_segment,
                .sub_key = entry->sub_key,
                .first_chunk_no = entry->control_first_chunk,
                .expected_chunk_count = entry->control_chunk_count,
                .expected_total_bytes = entry->control_encoded_bytes,
                .expected_checksum = entry->control_checksum,
            });
        }
        if (expected_control_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
            ready_minimizer_error_as_cpp(
                ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
                "bioseq minimizer generation fixture reader control bytes overflow");
        }
        session.plan_control_groups(state->control_groups);

        crc64_ecma182_accumulator control_crc{};
        crc64_ecma182_accumulator position_crc{};
        std::uint64_t control_bytes = 0;
        std::uint64_t position_bytes = 0;
        std::optional<minimizer_generation_encoding_error> direct_load_error;
        for (std::size_t group_index = 0; group_index < state->control_groups.size();
             ++group_index) {
            (void)session.read_chunks_in_range(
                group_index,
                index_chunk_cache_policy::bypass_one_shot,
                [state, &control_crc, &control_bytes, &direct_load_error](
                    const validated_minimizer_generation_row_receipt& receipt) -> void {
                    const bool visited = bioseq::indexing::visit_validated_minimizer_generation_row(
                        receipt,
                        [&control_crc, &control_bytes](const index_chunk_address&,
                                                       std::span<const std::byte> payload,
                                                       std::uint64_t) noexcept {
                            control_crc.update(payload);
                            control_bytes =
                                bioseq::common::saturating_add(control_bytes, payload.size());
                        });
                    if (!visited) {
                        direct_load_error = minimizer_generation_encoding_error::checksum;
                        return;
                    }
                    if (direct_load_error) {
                        return;
                    }
                    const auto accepted = state->grouped_soa_reader->accept(receipt);
                    if (!accepted) {
                        direct_load_error = accepted.error();
                    }
                });
        }
        if (direct_load_error || control_bytes != expected_control_bytes) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq minimizer generation fixture reader control load failed",
                fmt::format("wire_error={} actual_bytes={} expected_bytes={}",
                            direct_load_error ? static_cast<unsigned>(*direct_load_error) + 1u : 0u,
                            control_bytes,
                            expected_control_bytes));
        }

        if (!generation_payload_empty && state->position_ordinals.empty()) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq minimizer generation fixture reader selected no position extent");
        }

        std::uint64_t expected_position_bytes = 0;
        for (const std::uint32_t ordinal : state->position_ordinals) {
            const grouped_soa_bucket_directory_entry* entry =
                metadata.directory.entry_by_ordinal(ordinal);
            const auto position_sub_key =
                entry == nullptr
                    ? std::expected<index_sub_key_t,
                                    minimizer_generation_encoding_error>{std::unexpected(
                          minimizer_generation_encoding_error::partition_id)}
                    : derive_minimizer_position_extent_sub_key(entry->sub_key);
            if (entry == nullptr || !position_sub_key ||
                *position_sub_key == (std::numeric_limits<index_sub_key_t>::max)()) {
                ready_minimizer_error_as_cpp(
                    ERRCODE_DATA_CORRUPTED,
                    "bioseq minimizer generation fixture reader position ordinal is invalid");
            }
            expected_position_bytes = bioseq::common::saturating_add(expected_position_bytes,
                                                                     entry->position_encoded_bytes);
            result.position_encoded_bytes = bioseq::common::saturating_add(
                result.position_encoded_bytes, entry->position_encoded_bytes);
            result.position_chunks =
                bioseq::common::saturating_add(result.position_chunks, entry->position_chunk_count);
            state->position_groups.emplace_back();
            minimizer_generation_payload_read_group& group = state->position_groups.back();
            group.range = index_sub_key_range{
                .generation_id = result.generation_id,
                .position_segment = entry->position_segment,
                .begin = *position_sub_key,
                .end = *position_sub_key + 1,
            };
            group.requests.push_back(index_sub_key_request{
                .generation_id = result.generation_id,
                .position_segment = entry->position_segment,
                .sub_key = *position_sub_key,
                .first_chunk_no = entry->position_first_chunk,
                .expected_chunk_count = entry->position_chunk_count,
                .expected_total_bytes = entry->position_encoded_bytes,
                .expected_checksum = entry->position_checksum,
            });
        }
        if (expected_position_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
            ready_minimizer_error_as_cpp(
                ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
                "bioseq minimizer generation fixture reader position bytes overflow");
        }
        session.plan_position_groups(state->position_groups);
        for (std::size_t group_index = 0; group_index < state->position_groups.size();
             ++group_index) {
            (void)session.read_chunks_in_range(
                group_index,
                index_chunk_cache_policy::bypass_one_shot,
                [state, &position_crc, &position_bytes, &direct_load_error](
                    const validated_minimizer_generation_row_receipt& receipt) -> void {
                    const bool visited = bioseq::indexing::visit_validated_minimizer_generation_row(
                        receipt,
                        [&position_crc, &position_bytes](const index_chunk_address&,
                                                         std::span<const std::byte> payload,
                                                         std::uint64_t) noexcept {
                            position_crc.update(payload);
                            position_bytes =
                                bioseq::common::saturating_add(position_bytes, payload.size());
                        });
                    if (!visited) {
                        direct_load_error = minimizer_generation_encoding_error::checksum;
                        return;
                    }
                    if (direct_load_error) {
                        return;
                    }
                    const auto accepted = state->grouped_soa_reader->accept(receipt);
                    if (!accepted) {
                        direct_load_error = accepted.error();
                    }
                });
        }
        if (direct_load_error || position_bytes != expected_position_bytes) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq minimizer generation fixture reader position load failed",
                fmt::format("wire_error={} actual_bytes={} expected_bytes={}",
                            direct_load_error ? static_cast<unsigned>(*direct_load_error) + 1u : 0u,
                            position_bytes,
                            expected_position_bytes));
        }

        const auto finished = state->grouped_soa_reader->finish();
        if (!finished) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq grouped-SoA fixture reader final validation failed",
                fmt::format("wire_error={}", static_cast<unsigned>(finished.error())));
        }
        const auto lookup = state->grouped_soa_reader->lookup(query_hashes.front());
        if ((!generation_payload_empty &&
             (lookup.global_occurrence_count == 0 || !lookup.positions_complete ||
              lookup.positions.size() != lookup.global_occurrence_count ||
              std::ranges::any_of(lookup.positions,
                                  [&source](const std::uint64_t encoded_position) {
                                      return (encoded_position >> 1u) >=
                                             source.locator.logical_length;
                                  }))) ||
            (generation_payload_empty &&
             (lookup.global_occurrence_count != 0 || !lookup.positions.empty() ||
              !lookup.positions_complete))) {
            ready_minimizer_error_as_cpp(ERRCODE_DATA_CORRUPTED,
                                         "bioseq grouped-SoA fixture lookup failed",
                                         fmt::format("count={} positions={}",
                                                     lookup.global_occurrence_count,
                                                     lookup.positions.size()));
        }
        result.lookup_count = lookup.global_occurrence_count;
        result.lookup_bp = lookup.positions.empty() ? 0u : lookup.positions.front() >> 1u;
        result.lookup_reverse =
            lookup.positions.empty() ? false : (lookup.positions.front() & std::uint64_t{1}) != 0u;
        result.positions_complete = lookup.positions_complete;
        if ((!generation_payload_empty &&
             (result.lookup_count == 0 || result.lookup_bp >= source.locator.logical_length ||
              !result.positions_complete)) ||
            (generation_payload_empty &&
             (result.lookup_count != 0 || result.lookup_bp != 0 || result.lookup_reverse ||
              !result.positions_complete || metadata.manifest.control_count != 0 ||
              metadata.manifest.position_count != 0 || metadata.manifest.payload_bytes != 0))) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq minimizer generation fixture reader lookup validation disagrees");
        }

        result.validated_control_bytes = control_bytes;
        result.validated_position_bytes = position_bytes;
        result.validated_control_checksum = control_crc.checksum();
        result.validated_position_checksum = position_crc.checksum();
        result.read_stats = session.finish();
        if (result.read_stats.controls.plan_calls != 1 ||
            result.read_stats.positions.plan_calls != 1 ||
            result.read_stats.controls.planned_request_groups != state->control_groups.size() ||
            result.read_stats.positions.planned_request_groups != state->position_groups.size() ||
            result.read_stats.controls.executed_request_groups != state->control_groups.size() ||
            result.read_stats.positions.executed_request_groups != state->position_groups.size() ||
            result.read_stats.positions.control_authorization_checks !=
                state->position_groups.size() ||
            result.read_stats.payload_bytes_read != control_bytes + position_bytes) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq minimizer generation fixture reader two-stage validation disagrees");
        }

        state->grouped_soa_reader.reset();
        state->session.reset();

        if (state->selective_probe_full_generation) {
            state->catalog_source_authorization.emplace(
                authorize_minimizer_generation_catalog_source_pg(
                    identity, sequence_rowno, state->registered_snapshot));
        } else {
            state->source_authorization.emplace(authorize_minimizer_generation_source_pg(
                identity, sequence_rowno, state->registered_snapshot));
        }
        state->session.emplace(result.table_name,
                               sequence_rowno,
                               state->registered_snapshot,
                               *expected,
                               rid.tablespace_oid,
                               minimizer_generation_snapshot_registration::borrow_registered);
        index_storage_fragment_generation_read_session& probe_session = *state->session;
        const minimizer_generation_head probe_head = probe_session.read_active_head_once();
        const minimizer_generation_read_metadata& probe_metadata =
            probe_session.read_manifest_directory_once(probe_head);
        if (state->selective_probe_full_generation) {
            probe_session.consume_source_authorization(
                std::move(*state->catalog_source_authorization));
            state->catalog_source_authorization.reset();
        } else {
            probe_session.consume_full_source_authorization(
                std::move(*state->source_authorization));
            state->source_authorization.reset();
        }
        if (state->selective_probe_full_generation) {
            (void)probe_session.read_full_generation_payload_once(
                [](const validated_minimizer_generation_row_receipt&) {},
                [](const validated_minimizer_generation_row_receipt&) {});
        } else {
            probe_session.plan_control_groups(state->selective_probe_control_groups);
            if (!state->selective_probe_control_groups.empty()) {
                (void)probe_session.read_chunks_in_range(
                    0,
                    index_chunk_cache_policy::bypass_one_shot,
                    [](const index_chunk_address&, const char*, std::size_t) {});
            }
            probe_session.plan_position_groups(state->selective_probe_position_groups);
            if (!state->selective_probe_position_groups.empty()) {
                (void)probe_session.read_chunks_in_range(
                    0,
                    index_chunk_cache_policy::bypass_one_shot,
                    [](const index_chunk_address&, const char*, std::size_t) {});
            }
        }
        result.selective_probe_stats = probe_session.finish();
        const cold_index_preparation_stats& probe_cold = result.selective_probe_stats.cold;
        std::uint64_t probe_tid_transitions = 0;
        probe_tid_transitions =
            bioseq::common::saturating_add(probe_tid_transitions, probe_cold.heap_tid_adjacent);
        probe_tid_transitions =
            bioseq::common::saturating_add(probe_tid_transitions, probe_cold.heap_tid_forward_gap);
        probe_tid_transitions = bioseq::common::saturating_add(probe_tid_transitions,
                                                               probe_cold.heap_tid_backward_jump);
        const std::uint64_t expected_scanned_rows = bioseq::common::saturating_add(
            probe_cold.payload_rows_delivered, probe_cold.payload_rows_skipped);
        const bool expected_empty_route = generation_payload_empty;
        const bool expected_full_route = state->selective_probe_full_generation;
        const bool expected_selective_route = !expected_empty_route && !expected_full_route;
        const bool payload_equations_disagree =
            expected_empty_route
                ? probe_cold.payload_rows_requested != 0 ||
                      probe_cold.payload_rows_delivered != 0 ||
                      probe_cold.payload_rows_scanned != 0 ||
                      probe_cold.payload_rows_skipped != 0 ||
                      probe_cold.payload_bytes_requested != 0 ||
                      probe_cold.payload_bytes_delivered != 0 ||
                      probe_cold.payload_bytes_scanned != 0 || probe_cold.row_crc_bytes != 0 ||
                      probe_cold.extent_crc_bytes != 0 ||
                      probe_cold.extent_crc_raw_bytes_hashed != 0 ||
                      probe_cold.extent_crc_combined_bytes != 0 ||
                      probe_cold.extent_crc_combine_rows != 0 || probe_tid_transitions != 0
                : probe_cold.payload_rows_requested != probe_cold.payload_rows_delivered ||
                      probe_cold.payload_rows_delivered == 0 ||
                      probe_cold.payload_rows_skipped != 0 ||
                      probe_cold.payload_rows_scanned != expected_scanned_rows ||
                      probe_cold.payload_bytes_requested != probe_cold.payload_bytes_delivered ||
                      probe_cold.payload_bytes_delivered == 0 ||
                      probe_cold.payload_bytes_scanned != probe_cold.payload_bytes_delivered ||
                      probe_cold.row_crc_bytes != probe_cold.payload_bytes_scanned ||
                      probe_cold.extent_crc_bytes != probe_cold.payload_bytes_delivered ||
                      probe_cold.extent_crc_raw_bytes_hashed != 0 ||
                      probe_cold.extent_crc_combined_bytes != probe_cold.payload_bytes_delivered ||
                      probe_cold.extent_crc_combine_rows != probe_cold.payload_rows_delivered ||
                      probe_tid_transitions != probe_cold.payload_rows_scanned - 1u;
        if (probe_cold.route_empty != static_cast<std::uint64_t>(expected_empty_route) ||
            probe_cold.route_selective != static_cast<std::uint64_t>(expected_selective_route) ||
            probe_cold.route_full != static_cast<std::uint64_t>(expected_full_route) ||
            payload_equations_disagree ||
            expected_scanned_rows == (std::numeric_limits<std::uint64_t>::max)() ||
            probe_tid_transitions == (std::numeric_limits<std::uint64_t>::max)() ||
            probe_cold.decode_ns != 0 || probe_cold.transfer_ns != 0) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq minimizer generation fixture cold stats equations disagree");
        }
        state->session.reset();
        bioseq::postgres::support::invoke_pg_as_cpp(
            [state] { UnregisterSnapshot(state->registered_snapshot); });
        state->registered_snapshot = nullptr;
        delete state;
        state = nullptr;
    } catch (...) {
        std::exception_ptr error = std::current_exception();
        if (state != nullptr) {
            state->grouped_soa_reader.reset();
            state->session.reset();
            state->source_authorization.reset();
            state->catalog_source_authorization.reset();
        }
        if (state != nullptr && state->registered_snapshot != nullptr) {
            Snapshot cleanup = std::exchange(state->registered_snapshot, nullptr);
            try {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [cleanup] { UnregisterSnapshot(cleanup); });
            } catch (...) {
                error = std::current_exception();
            }
        }
        delete state;
        std::rethrow_exception(error);
    }
    return result;
}

/// Reopen a READY grouped-SoA generation and stream its complete opaque payload
/// through the generation-wide single-scan API. This seam deliberately does
/// not decode either payload and exists only for integration validation.
[[nodiscard]] inline minimizer_generation_full_payload_fixture_result
read_minimizer_generation_full_payload_fixture_impl(
    Oid rel_oid,
    std::string_view index_name,
    std::uint64_t sequence_rowno,
    std::uint16_t kmer_size,
    std::uint16_t window_size,
    bioseq::indexing::minimizer_generation_layout layout,
    bioseq::minimizers::minimizer_sketch_backend backend) {
    if (!OidIsValid(rel_oid) || !bioseq::postgres::support::invoke_pg_as_cpp([rel_oid] {
            return relation_oid_uses_column_table_access_method(rel_oid);
        })) {
        ready_minimizer_error_as_cpp(ERRCODE_WRONG_OBJECT_TYPE,
                                     "bioseq full-generation reader requires a USING bioseq table");
    }
    if (!bioseq::postgres::support::invoke_pg_as_cpp([] { return superuser(); })) {
        ready_minimizer_error_as_cpp(ERRCODE_INSUFFICIENT_PRIVILEGE,
                                     "bioseq full-generation reader requires superuser");
    }
    if (index_name != k_minimizers_per_fragment_index_name &&
        index_name != k_minimizers_per_sequence_index_name) {
        ready_minimizer_error_as_cpp(ERRCODE_INVALID_PARAMETER_VALUE,
                                     "bioseq full-generation reader requires a minimizer index");
    }
    if (kmer_size == 0 || window_size == 0 ||
        !bioseq::indexing::sql_bigint_can_represent(sequence_rowno) ||
        !bioseq::indexing::is_supported_minimizer_generation_layout(layout)) {
        ready_minimizer_error_as_cpp(ERRCODE_INVALID_PARAMETER_VALUE,
                                     "bioseq full-generation reader parameters are invalid");
    }

    const std::int16_t seq_attno = single_seq_attno_for_inspection(rel_oid);
    const relation_identity rid = index_relation_identity(rel_oid);
    minimizer_generation_administration_identity identity{
        .database_oid = rid.db_oid,
        .tablespace_oid = rid.tablespace_oid,
        .main_relfilenode = rid.relfilenode,
        .user_relid = rel_oid,
        .column_attno = seq_attno,
        .index_name = std::string{index_name},
    };
    struct read_state {
        Snapshot registered_snapshot = nullptr;
        std::optional<minimizer_generation_catalog_source_authorization> source_authorization;
        std::optional<index_storage_fragment_generation_read_session> session;
    };
    read_state* state = nullptr;
    minimizer_generation_full_payload_fixture_result result{};
    try {
        state = new read_state{};
        Snapshot active_snapshot =
            bioseq::postgres::support::invoke_pg_as_cpp([] { return GetActiveSnapshot(); });
        if (active_snapshot == nullptr || !IsMVCCSnapshot(active_snapshot)) {
            ready_minimizer_error_as_cpp(
                ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                "bioseq full-generation reader requires an active MVCC snapshot");
        }
        state->registered_snapshot = bioseq::postgres::support::invoke_pg_as_cpp(
            [active_snapshot] { return RegisterSnapshot(active_snapshot); });
        state->source_authorization.emplace(authorize_minimizer_generation_catalog_source_pg(
            identity, sequence_rowno, state->registered_snapshot));
        const minimizer_generation_source_locator source =
            state->source_authorization->source_for_diagnostics();
        result.table_name = format_seqidx_name(index_name,
                                               rid.db_oid,
                                               rid.tablespace_oid,
                                               rid.relfilenode,
                                               seq_attno,
                                               source.locator.bucket_index,
                                               source.locator.shard_index);
        result.selected_backend_name = bioseq::minimizers::minimizer_sketch_backend_name(backend);
        identity.table_name = result.table_name;
        const auto expected =
            bioseq::indexing::expected_grouped_soa_generation_identity_for_backend(
                layout,
                backend,
                source.alphabet_code,
                source.storage_unit_byte_width,
                kmer_size,
                window_size,
                layout == bioseq::indexing::minimizer_generation_layout::grouped_soa_segment_local
                    ? std::optional<
                          std::uint64_t>{bioseq::indexing::k_grouped_soa_default_max_occurrence_cap}
                    : std::nullopt);
        if (!expected) {
            ready_minimizer_error_as_cpp(
                ERRCODE_INVALID_PARAMETER_VALUE,
                "bioseq full-generation storage contract is invalid",
                fmt::format("wire_error={}", static_cast<unsigned>(expected.error())));
        }
        state->session.emplace(result.table_name,
                               sequence_rowno,
                               state->registered_snapshot,
                               *expected,
                               rid.tablespace_oid,
                               minimizer_generation_snapshot_registration::borrow_registered);
        index_storage_fragment_generation_read_session& session = *state->session;
        const minimizer_generation_head head = session.read_active_head_once();
        const minimizer_generation_read_metadata& metadata =
            session.read_manifest_directory_once(head);
        const minimizer_generation_catalog_source_stats source_stats =
            state->source_authorization->stats_for_diagnostics();
        session.consume_source_authorization(std::move(*state->source_authorization));
        state->source_authorization.reset();
        const auto requested_scheme =
            bioseq::indexing::minimizer_scheme_identity_for_backend(backend);
        const auto requested_producer_backend =
            bioseq::indexing::minimizer_producer_backend_identity_for_backend(backend);
        if (metadata.manifest.minimizer_scheme_id != requested_scheme.id ||
            metadata.manifest.minimizer_scheme_revision != requested_scheme.revision ||
            metadata.manifest.producer_backend_id != requested_producer_backend.id ||
            metadata.manifest.producer_identity_revision != requested_producer_backend.revision ||
            source_stats.parent_rows_read != 1 || source_stats.catalog_rows_read != 1 ||
            source_stats.catalog_index_scans != 1 || source_stats.fragment_rows_read != 0 ||
            source_stats.decoded_bp != 0) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq full-generation backend/source authorization disagrees");
        }
        result.generation_id = metadata.manifest.generation_id;
        crc64_ecma182_accumulator control_checksum{};
        crc64_ecma182_accumulator position_checksum{};
        const index_chunk_read_stats payload = session.read_full_generation_payload_once(
            [&result, &control_checksum](
                const index_chunk_address&, const char* bytes, std::size_t byte_count) -> void {
                control_checksum.update(std::as_bytes(std::span<const char>{bytes, byte_count}));
                result.control_bytes =
                    bioseq::common::saturating_add(result.control_bytes, byte_count);
            },
            [&result, &position_checksum](
                const index_chunk_address&, const char* bytes, std::size_t byte_count) -> void {
                position_checksum.update(std::as_bytes(std::span<const char>{bytes, byte_count}));
                result.position_bytes =
                    bioseq::common::saturating_add(result.position_bytes, byte_count);
            });
        result.control_checksum = control_checksum.checksum();
        result.position_checksum = position_checksum.checksum();
        result.read_stats = session.finish();
        if (result.control_bytes != result.read_stats.full_generation.control_bytes ||
            result.position_bytes != result.read_stats.full_generation.position_bytes ||
            payload.index_scans != 1 || result.read_stats.full_generation.scan_calls != 1 ||
            result.read_stats.range_scan_calls != 1 || result.read_stats.index_scans != 3 ||
            result.read_stats.source_parent_rows_read != 1 ||
            result.read_stats.source_catalog_rows_read != 1 ||
            result.read_stats.source_catalog_index_scans != 1 ||
            result.read_stats.source_fragment_rows_read != 0 ||
            result.read_stats.source_decoded_bp != 0 ||
            result.read_stats.payload.delivered_bytes !=
                result.control_bytes + result.position_bytes) {
            ready_minimizer_error_as_cpp(ERRCODE_DATA_CORRUPTED,
                                         "bioseq full-generation single-scan validation disagrees");
        }
        state->session.reset();
        bioseq::postgres::support::invoke_pg_as_cpp(
            [state] { UnregisterSnapshot(state->registered_snapshot); });
        state->registered_snapshot = nullptr;
        delete state;
        state = nullptr;
    } catch (...) {
        std::exception_ptr error = std::current_exception();
        if (state != nullptr) {
            state->session.reset();
            state->source_authorization.reset();
        }
        if (state != nullptr && state->registered_snapshot != nullptr) {
            Snapshot cleanup = std::exchange(state->registered_snapshot, nullptr);
            try {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [cleanup] { UnregisterSnapshot(cleanup); });
            } catch (...) {
                error = std::current_exception();
            }
        }
        delete state;
        std::rethrow_exception(error);
    }
    return result;
}

/// Integration-only source adversary: remove one referenced physical sequence
/// fragment by its locator ordinal. The public source authorization must subsequently
/// fail while walking the same locator under its registered snapshot.  Ordinal
/// zero is deliberately forbidden so this seam validates later-fragment
/// participation rather than merely checking the first physical row.
inline void remove_sequence_fragment_impl(Oid rel_oid,
                                          std::string_view index_name,
                                          std::uint64_t sequence_rowno,
                                          std::uint32_t fragment_ordinal) {
    if (!bioseq::postgres::support::invoke_pg_as_cpp([] { return superuser(); })) {
        ready_minimizer_error_as_cpp(ERRCODE_INSUFFICIENT_PRIVILEGE,
                                     "bioseq source-fragment adversary requires superuser");
    }
    if (!OidIsValid(rel_oid) || !bioseq::postgres::support::invoke_pg_as_cpp([rel_oid] {
            return relation_oid_uses_column_table_access_method(rel_oid);
        })) {
        ready_minimizer_error_as_cpp(
            ERRCODE_WRONG_OBJECT_TYPE,
            "bioseq source-fragment adversary requires a USING bioseq table");
    }
    if (index_name != k_minimizers_per_fragment_index_name &&
        index_name != k_minimizers_per_sequence_index_name) {
        ready_minimizer_error_as_cpp(ERRCODE_INVALID_PARAMETER_VALUE,
                                     "bioseq source-fragment adversary requires a minimizer index");
    }
    if (fragment_ordinal == 0 || !bioseq::indexing::sql_bigint_can_represent(sequence_rowno)) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "bioseq source-fragment adversary requires a non-first fragment");
    }

    const std::int16_t seq_attno = single_seq_attno_for_inspection(rel_oid);
    const relation_identity rid = index_relation_identity(rel_oid);
    minimizer_generation_administration_identity identity{
        .database_oid = rid.db_oid,
        .tablespace_oid = rid.tablespace_oid,
        .main_relfilenode = rid.relfilenode,
        .user_relid = rel_oid,
        .column_attno = seq_attno,
        .index_name = std::string{index_name},
    };

    struct remove_state {
        Snapshot registered_snapshot = nullptr;
        seq_caches* fragment_caches = nullptr;
    };
    auto* state = new remove_state{};
    try {
        Snapshot active_snapshot =
            bioseq::postgres::support::invoke_pg_as_cpp([] { return GetActiveSnapshot(); });
        if (active_snapshot == nullptr || !IsMVCCSnapshot(active_snapshot)) {
            ready_minimizer_error_as_cpp(
                ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                "bioseq source-fragment adversary requires an active MVCC snapshot");
        }
        state->registered_snapshot = bioseq::postgres::support::invoke_pg_as_cpp(
            [active_snapshot] { return RegisterSnapshot(active_snapshot); });
        minimizer_generation_source_authorization authorization =
            authorize_minimizer_generation_source_pg(
                identity, sequence_rowno, state->registered_snapshot);
        const minimizer_generation_source_locator source = authorization.source_for_diagnostics();
        if (source.locator.fragment_count < 3 ||
            fragment_ordinal >= source.locator.fragment_count) {
            ready_minimizer_error_as_cpp(ERRCODE_INVALID_PARAMETER_VALUE,
                                         "bioseq source-fragment adversary ordinal is out of range",
                                         fmt::format("ordinal={} fragments={}",
                                                     fragment_ordinal,
                                                     source.locator.fragment_count));
        }

        state->fragment_caches = bioseq::postgres::support::invoke_pg_as_cpp(
            [] { return alloc_seq_caches(CurrentMemoryContext); });
        state->fragment_caches->side = seq_caches::access_side::read;
        state->fragment_caches->lock_mode = RowExclusiveLock;
        state->fragment_caches->main_database_oid = rid.db_oid;
        state->fragment_caches->main_tablespace_oid = rid.tablespace_oid;
        state->fragment_caches->main_relfilenode = rid.relfilenode;
        [[maybe_unused]] Relation fragment_relation =
            bioseq::postgres::support::invoke_pg_as_cpp([rid, seq_attno, &source, state] {
                return open_sub_table_for_locator(rid.db_oid,
                                                  rid.tablespace_oid,
                                                  rid.relfilenode,
                                                  seq_attno,
                                                  source.locator,
                                                  RowExclusiveLock,
                                                  state->fragment_caches);
            });
        const std::uint64_t fragment_rowno = source.locator.first_fragment_row + fragment_ordinal;
        if (fragment_rowno < source.locator.first_fragment_row) {
            ready_minimizer_error_as_cpp(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
                                         "bioseq source-fragment row number overflow");
        }
        // Registered source fragments are immutable. Exercise the supported
        // mutation path by transactionally invalidating the exact epoch rather
        // than bypassing the TableAM guard and creating undetectable raw damage.
        bioseq::postgres::support::invoke_pg_as_cpp([&source] {
            sequence_source_version_manager::invalidate_epoch(source.locator.source_epoch);
        });
        const TM_Result deleted = TM_Ok;
        if (deleted != TM_Ok) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq source-fragment adversary could not remove the referenced row",
                fmt::format(
                    "ordinal={} tm_result={}", fragment_ordinal, static_cast<int>(deleted)));
        }
        bioseq::postgres::support::invoke_pg_as_cpp(
            [state] { close_and_destroy_seq_caches(state->fragment_caches, /*commit=*/true); });
        state->fragment_caches = nullptr;
        bioseq::postgres::support::invoke_pg_as_cpp(
            [state] { UnregisterSnapshot(state->registered_snapshot); });
        state->registered_snapshot = nullptr;
        bioseq::postgres::support::invoke_pg_as_cpp([] {
            CommandCounterIncrement();
            UpdateActiveSnapshotCommandId();
        });
        delete state;
        state = nullptr;
    } catch (...) {
        std::exception_ptr error = std::current_exception();
        if (state != nullptr && state->fragment_caches != nullptr) {
            seq_caches* cleanup = std::exchange(state->fragment_caches, nullptr);
            try {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [cleanup] { close_and_destroy_seq_caches(cleanup, /*commit=*/false); });
            } catch (...) {
                error = std::current_exception();
            }
        }
        if (state != nullptr && state->registered_snapshot != nullptr) {
            Snapshot cleanup = std::exchange(state->registered_snapshot, nullptr);
            try {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [cleanup] { UnregisterSnapshot(cleanup); });
            } catch (...) {
                error = std::current_exception();
            }
        }
        delete state;
        std::rethrow_exception(error);
    }
}

/// Exercise read-session failure transitions against the live fixture while
/// restoring the backend memory cap and registered snapshot on every ERROR.
inline void exercise_minimizer_generation_session_failure_impl(Oid rel_oid,
                                                               std::string_view index_name,
                                                               std::uint64_t sequence_rowno,
                                                               std::string_view mode) {
    if (!bioseq::postgres::support::invoke_pg_as_cpp([] { return superuser(); })) {
        ready_minimizer_error_as_cpp(ERRCODE_INSUFFICIENT_PRIVILEGE,
                                     "bioseq minimizer session fixture requires superuser");
    }
    if (mode != "repeated_head" && mode != "repeated_metadata" && mode != "early_payload" &&
        mode != "permit_denied" && mode != "scheme_mismatch" && mode != "layout_mismatch" &&
        mode != "position_before_controls" && mode != "finish_before_positions" &&
        mode != "repeated_control_plan" && mode != "control_position_namespace" &&
        mode != "unauthorized_position") {
        ready_minimizer_error_as_cpp(ERRCODE_INVALID_PARAMETER_VALUE,
                                     "bioseq minimizer session fixture mode is invalid");
    }
    const std::int16_t seq_attno = single_seq_attno_for_inspection(rel_oid);
    const relation_identity rid = index_relation_identity(rel_oid);
    minimizer_generation_administration_identity identity{
        .database_oid = rid.db_oid,
        .tablespace_oid = rid.tablespace_oid,
        .main_relfilenode = rid.relfilenode,
        .user_relid = rel_oid,
        .column_attno = seq_attno,
        .index_name = std::string{index_name},
    };
    struct failure_state {
        Snapshot registered_snapshot = nullptr;
        std::uint64_t saved_memory_cap = 0;
        bool cap_changed = false;
        std::optional<minimizer_generation_source_authorization> source_authorization;
        std::optional<index_storage_fragment_generation_read_session> read_session;
        std::vector<minimizer_generation_payload_read_group> groups;
    };
    auto* state = new failure_state{};
    try {
        Snapshot active_snapshot =
            bioseq::postgres::support::invoke_pg_as_cpp([] { return GetActiveSnapshot(); });
        if (active_snapshot == nullptr || !IsMVCCSnapshot(active_snapshot)) {
            ready_minimizer_error_as_cpp(
                ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                "bioseq minimizer session fixture requires an active MVCC snapshot");
        }
        state->registered_snapshot = bioseq::postgres::support::invoke_pg_as_cpp(
            [active_snapshot] { return RegisterSnapshot(active_snapshot); });
        state->source_authorization.emplace(authorize_minimizer_generation_source_pg(
            identity, sequence_rowno, state->registered_snapshot));
        const minimizer_generation_source_locator& source =
            state->source_authorization->source_for_diagnostics();
        identity.table_name = format_seqidx_name(index_name,
                                                 rid.db_oid,
                                                 rid.tablespace_oid,
                                                 rid.relfilenode,
                                                 seq_attno,
                                                 source.locator.bucket_index,
                                                 source.locator.shard_index);
        if (mode == "permit_denied") {
            auto& admission = bioseq::parallelism::memory_admission_controller::instance();
            state->saved_memory_cap = admission.index_memory_cap_bytes();
            admission.set_index_memory_cap_bytes(1);
            state->cap_changed = true;
        }
        auto expected = bioseq::indexing::expected_grouped_soa_generation_identity_for_backend(
            bioseq::indexing::minimizer_generation_layout::grouped_soa_segment_local,
            bioseq::minimizers::minimizer_sketch_backend::scalar,
            source.alphabet_code,
            source.storage_unit_byte_width,
            static_cast<std::uint16_t>(bioseq::aligner::settings::minimizer::kmer_size),
            static_cast<std::uint16_t>(bioseq::aligner::settings::minimizer::window_size),
            bioseq::indexing::k_grouped_soa_default_max_occurrence_cap);
        if (!expected) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq minimizer session expected storage contract is invalid");
        }
        if (mode == "scheme_mismatch") {
            expected->minimizer_scheme_id = bioseq::indexing::k_bounded_nthash_u32_r7_sum_scheme_id;
            expected->minimizer_scheme_revision =
                bioseq::indexing::k_bounded_nthash_u32_r7_sum_scheme_revision;
        } else if (mode == "layout_mismatch") {
            expected->layout = bioseq::indexing::minimizer_generation_layout::invalid;
        }
        state->read_session.emplace(identity.table_name,
                                    sequence_rowno,
                                    state->registered_snapshot,
                                    *expected,
                                    rid.tablespace_oid,
                                    minimizer_generation_snapshot_registration::borrow_registered);
        index_storage_fragment_generation_read_session& session = *state->read_session;
        const minimizer_generation_head head = session.read_active_head_once();
        if (mode == "repeated_head") {
            (void)session.read_active_head_once();
        } else if (mode == "early_payload") {
            session.plan_control_groups(std::span<const minimizer_generation_payload_read_group>{});
        } else {
            const minimizer_generation_read_metadata& metadata =
                session.read_manifest_directory_once(head);
            if (mode == "repeated_metadata") {
                (void)session.read_manifest_directory_once(head);
            } else if (mode == "position_before_controls") {
                session.consume_full_source_authorization(std::move(*state->source_authorization));
                state->source_authorization.reset();
                session.plan_position_groups(
                    std::span<const minimizer_generation_payload_read_group>{});
            } else if (mode == "finish_before_positions") {
                session.consume_full_source_authorization(std::move(*state->source_authorization));
                state->source_authorization.reset();
                session.plan_control_groups(
                    std::span<const minimizer_generation_payload_read_group>{});
                (void)session.finish();
            } else if (mode == "repeated_control_plan") {
                session.consume_full_source_authorization(std::move(*state->source_authorization));
                state->source_authorization.reset();
                session.plan_control_groups(
                    std::span<const minimizer_generation_payload_read_group>{});
                session.plan_control_groups(
                    std::span<const minimizer_generation_payload_read_group>{});
            } else if (mode == "control_position_namespace" || mode == "unauthorized_position") {
                session.consume_full_source_authorization(std::move(*state->source_authorization));
                state->source_authorization.reset();
                const auto entries = metadata.directory.entries();
                if (entries.empty()) {
                    ready_minimizer_error_as_cpp(
                        ERRCODE_DATA_CORRUPTED,
                        "bioseq minimizer session fixture has no payload entry");
                }
                const grouped_soa_bucket_directory_entry& entry = entries.front();
                const auto position_sub_key =
                    derive_minimizer_position_extent_sub_key(entry.sub_key);
                if (!position_sub_key) {
                    ready_minimizer_error_as_cpp(
                        ERRCODE_DATA_CORRUPTED,
                        "bioseq minimizer session fixture position key is invalid");
                }
                state->groups.push_back(minimizer_generation_payload_read_group{
                    .range =
                        index_sub_key_range{
                            .generation_id = head.generation_id,
                            .position_segment = entry.position_segment,
                            .begin = *position_sub_key,
                            .end = *position_sub_key + 1,
                        },
                    .requests =
                        {
                            index_sub_key_request{
                                .generation_id = head.generation_id,
                                .position_segment = entry.position_segment,
                                .sub_key = *position_sub_key,
                                .first_chunk_no = entry.position_first_chunk,
                                .expected_chunk_count = entry.position_chunk_count,
                                .expected_total_bytes = entry.position_encoded_bytes,
                                .expected_checksum = entry.position_checksum,
                            },
                        },
                });
                if (mode == "control_position_namespace") {
                    session.plan_control_groups(
                        std::span<const minimizer_generation_payload_read_group>{state->groups});
                } else {
                    session.plan_control_groups(
                        std::span<const minimizer_generation_payload_read_group>{});
                    session.plan_position_groups(
                        std::span<const minimizer_generation_payload_read_group>{state->groups});
                }
            }
        }
        if (state->cap_changed) {
            bioseq::parallelism::memory_admission_controller::instance().set_index_memory_cap_bytes(
                state->saved_memory_cap);
            state->cap_changed = false;
        }
        state->source_authorization.reset();
        state->read_session.reset();
        bioseq::postgres::support::invoke_pg_as_cpp(
            [state] { UnregisterSnapshot(state->registered_snapshot); });
        state->registered_snapshot = nullptr;
        ready_minimizer_error_as_cpp(
            ERRCODE_INTERNAL_ERROR,
            "bioseq minimizer session fixture did not reach its expected failure");
    } catch (...) {
        std::exception_ptr error = std::current_exception();
        if (state->cap_changed) {
            bioseq::parallelism::memory_admission_controller::instance().set_index_memory_cap_bytes(
                state->saved_memory_cap);
            state->cap_changed = false;
        }
        state->read_session.reset();
        state->source_authorization.reset();
        if (state->registered_snapshot != nullptr) {
            Snapshot cleanup = std::exchange(state->registered_snapshot, nullptr);
            try {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [cleanup] { UnregisterSnapshot(cleanup); });
            } catch (...) {
                error = std::current_exception();
            }
        }
        delete state;
        std::rethrow_exception(error);
    }
}

} // namespace bioseq::storage::indexing
