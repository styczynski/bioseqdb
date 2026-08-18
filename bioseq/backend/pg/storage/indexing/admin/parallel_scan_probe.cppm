/**
 * @file admin/parallel_scan_probe.cppm
 * @brief Owns hash-major restart inspection and authenticated parallel transport probes.
 */
module;

#include <bioseq/pg_compat.h>

export module bioseq.postgres.storage.indexing.admin.parallel_scan_probe;

import std;
import bioseq.postgres.storage.indexing.admin.diagnostic_support;
import bioseq.indexing.hash_major_minimizer_index;
import bioseq.postgres.configuration.aligner;
import bioseq.postgres.support;
import bioseq.postgres.column.table_access;
import bioseq.postgres.storage.indexing.chunked_storage;
import bioseq.postgres.storage.indexing.hash_major_session_admin;
import bioseq.postgres.storage.indexing.index_relation_identity;
import bioseq.postgres.storage.indexing.supported_indexes;
import bioseq.postgres.storage.indexing.admin:hash_major;

export namespace bioseq::storage::indexing {

/**
 * @brief Resolve and fully authenticate one READY hash-major generation.
 * @param rel_oid Source Bioseq relation.
 * @param index_name Registered logical index name.
 * @param sequence_rowno Source-row ordinal and query key.
 * @return Fixed-wire metadata plus recomputed directory, control, and posting receipts.
 *
 * The restart audit validates the 128-bit source identity and bounded-rereads every stream.
 */
[[nodiscard]] inline hash_major_minimizer_generation_inspection
inspect_hash_major_minimizer_generation(Oid rel_oid,
                                        std::string_view index_name,
                                        std::uint64_t sequence_rowno) {
    const bool is_bioseq_table =
        OidIsValid(rel_oid) && bioseq::postgres::support::invoke_pg_as_cpp([rel_oid] {
            return relation_oid_uses_column_table_access_method(rel_oid);
        });
    if (!is_bioseq_table) {
        ready_minimizer_error_as_cpp(ERRCODE_WRONG_OBJECT_TYPE,
                                     "bioseq hash-major inspection requires a USING bioseq table");
    }
    const Oid user_id = GetUserId();
    if (!bioseq::postgres::support::invoke_pg_as_cpp([rel_oid, user_id] {
            return object_ownercheck(RelationRelationId, rel_oid, user_id);
        })) {
        bioseq::postgres::support::invoke_pg_as_cpp(
            [rel_oid] { aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TABLE, get_rel_name(rel_oid)); });
    }
    if (index_name != k_minimizers_per_fragment_index_name &&
        index_name != k_minimizers_per_sequence_index_name) {
        ready_minimizer_error_as_cpp(ERRCODE_INVALID_PARAMETER_VALUE,
                                     "bioseq hash-major inspection requires a minimizer index",
                                     fmt::format("index_name={}", index_name));
    }
    if (!bioseq::indexing::sql_bigint_can_represent(sequence_rowno)) {
        ready_minimizer_error_as_cpp(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
                                     "bioseq hash-major inspection query key exceeds BIGINT");
    }
    if (bioseq::aligner::settings::minimizer::kmer_size != 15 ||
        bioseq::aligner::settings::minimizer::window_size != 10) {
        ready_minimizer_error_as_cpp(
            ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
            "bioseq hash-major inspection requires the registered k=15,w=10 profile");
    }

    const std::int16_t seq_attno = single_seq_attno_for_inspection(rel_oid);
    const relation_identity relation_identity = index_relation_identity(rel_oid);
    if (!OidIsValid(relation_identity.db_oid) || !OidIsValid(relation_identity.tablespace_oid) ||
        !OidIsValid(relation_identity.relfilenode)) {
        ready_minimizer_error_as_cpp(ERRCODE_UNDEFINED_TABLE,
                                     "bioseq hash-major inspection source relation is unavailable");
    }
    Snapshot active_snapshot = GetActiveSnapshot();
    if (active_snapshot == nullptr || !IsMVCCSnapshot(active_snapshot)) {
        ready_minimizer_error_as_cpp(
            ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
            "bioseq hash-major inspection requires an active MVCC snapshot");
    }

    Snapshot registered_snapshot = nullptr;
    hash_major_minimizer_generation_inspection result{};
    try {
        registered_snapshot = bioseq::postgres::support::invoke_pg_as_cpp(
            [active_snapshot] { return RegisterSnapshot(active_snapshot); });
        if (registered_snapshot == nullptr || registered_snapshot->regd_count == 0) {
            ready_minimizer_error_as_cpp(
                ERRCODE_INTERNAL_ERROR,
                "bioseq hash-major inspection could not register its snapshot");
        }

        const minimizer_generation_administration_identity identity{
            .database_oid = relation_identity.db_oid,
            .tablespace_oid = relation_identity.tablespace_oid,
            .main_relfilenode = relation_identity.relfilenode,
            .user_relid = rel_oid,
            .column_attno = seq_attno,
            .index_name = std::string{index_name},
        };
        const ready_hash_major_minimizer_metadata_identity inspected =
            inspect_ready_hash_major_minimizer_metadata_pg(
                identity, sequence_rowno, registered_snapshot);
        const std::string table_name = format_seqidx_name(index_name,
                                                          relation_identity.db_oid,
                                                          relation_identity.tablespace_oid,
                                                          relation_identity.relfilenode,
                                                          seq_attno,
                                                          inspected.source.locator.bucket_index,
                                                          inspected.source.locator.shard_index);
        if (table_name.empty() || table_name.size() >= result.table_name.size() ||
            index_name.size() >= result.index_name.size()) {
            ready_minimizer_error_as_cpp(ERRCODE_NAME_TOO_LONG,
                                         "bioseq hash-major inspection identity name is too long");
        }

        const auto expected_profile = bioseq::indexing::hash_major_minimizer_profile_for_backend(
            resolve_index_sketch_backend_from_gucs(),
            inspected.source.alphabet_code,
            15u,
            10u,
            inspected.source.locator.logical_length);
        if (!expected_profile || inspected.manifest.storage_profile != *expected_profile ||
            inspected.head.query_key != sequence_rowno ||
            inspected.source.sequence_rowno != sequence_rowno ||
            inspected.head.source_epoch != inspected.source.locator.source_epoch ||
            inspected.manifest.source_epoch != inspected.source.locator.source_epoch ||
            inspected.head.source_content_crc64 != inspected.source_content_checksum ||
            inspected.manifest.source_content_crc64 != inspected.source_content_checksum ||
            inspected.manifest.target_length != inspected.source.locator.logical_length ||
            inspected.head.source_identity !=
                bioseq::indexing::hash_major_minimizer_source_identity(
                    sequence_rowno,
                    inspected.source.locator.source_epoch,
                    inspected.source_content_checksum,
                    inspected.source.locator.logical_length) ||
            inspected.head.source_identity != inspected.manifest.source_identity ||
            !inspected.source_stats.timed_equation_holds()) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq hash-major inspection source/profile identity disagrees");
        }

        static_assert(bioseq::indexing::k_hash_major_minimizer_head_bytes == 224u);
        static_assert(bioseq::indexing::k_hash_major_minimizer_manifest_bytes == 336u);
        const auto ceil_rows = [](std::uint64_t bytes) noexcept {
            return bytes / bioseq::indexing::k_hash_major_minimizer_row_payload_bytes +
                   (bytes % bioseq::indexing::k_hash_major_minimizer_row_payload_bytes != 0u ? 1u
                                                                                             : 0u);
        };
        std::uint64_t expected_first_row = 0;
        for (std::size_t stream_index = 0; stream_index < inspected.manifest.streams.size();
             ++stream_index) {
            const auto& descriptor = inspected.manifest.streams[stream_index];
            const auto& catalog = inspected.exact_catalog_streams[stream_index];
            if (descriptor.encoding_revision != 1u ||
                descriptor.row_count != ceil_rows(descriptor.logical_bytes) ||
                descriptor.first_row_ordinal != expected_first_row ||
                catalog.kind != descriptor.kind ||
                catalog.exact_catalog_rows != descriptor.row_count ||
                catalog.exact_catalog_bytes != descriptor.logical_bytes ||
                catalog.exact_catalog_stream_crc64 != descriptor.stream_crc64 ||
                descriptor.row_count >
                    (std::numeric_limits<std::uint64_t>::max)() - expected_first_row) {
                ready_minimizer_error_as_cpp(
                    ERRCODE_DATA_CORRUPTED,
                    "bioseq hash-major inspection stream receipt disagrees",
                    fmt::format("stream_index={}", stream_index));
            }
            expected_first_row += descriptor.row_count;
            result.streams[stream_index] = {
                .kind = descriptor.kind,
                .element_count = descriptor.element_count,
                .logical_bytes = descriptor.logical_bytes,
                .row_count = descriptor.row_count,
                .stream_crc64 = descriptor.stream_crc64,
                .exact_catalog_rows = catalog.exact_catalog_rows,
                .exact_catalog_bytes = catalog.exact_catalog_bytes,
                .exact_catalog_stream_crc64 = catalog.exact_catalog_stream_crc64,
            };
        }
        const auto& directory = inspected.manifest.streams[0];
        const auto& controls = inspected.manifest.streams[1];
        const auto& postings = inspected.manifest.streams[2];
        const unsigned __int128 expected_directory_bytes =
            static_cast<unsigned __int128>(directory.element_count) *
            bioseq::indexing::k_hash_major_minimizer_directory_entry_bytes;
        const unsigned __int128 expected_posting_bytes =
            static_cast<unsigned __int128>(postings.element_count) *
            inspected.manifest.storage_profile.stored_posting_width_bytes;
        if (directory.kind != bioseq::indexing::hash_major_minimizer_stream_kind::directory ||
            controls.kind != bioseq::indexing::hash_major_minimizer_stream_kind::controls ||
            postings.kind != bioseq::indexing::hash_major_minimizer_stream_kind::postings ||
            directory.element_width !=
                bioseq::indexing::k_hash_major_minimizer_directory_entry_bytes ||
            directory.element_count != inspected.manifest.directory_entry_count ||
            expected_directory_bytes != directory.logical_bytes ||
            directory.stream_crc64 != inspected.manifest.authenticated_directory_crc64 ||
            controls.element_count != inspected.manifest.unique_hash_count ||
            controls.logical_bytes != inspected.manifest.total_control_bytes ||
            postings.element_count != inspected.manifest.total_posting_count ||
            postings.logical_bytes != inspected.manifest.total_posting_bytes ||
            expected_posting_bytes != postings.logical_bytes) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq hash-major inspection manifest stream equations disagree");
        }

        result.database_oid = relation_identity.db_oid;
        result.main_relfilenode = relation_identity.relfilenode;
        result.column_attno = seq_attno;
        result.query_key = sequence_rowno;
        std::ranges::copy(table_name, result.table_name.begin());
        std::ranges::copy(index_name, result.index_name.begin());
        result.source = inspected.source;
        result.source_content_crc64 = inspected.source_content_checksum;
        result.source_identity = inspected.head.source_identity;
        result.head = inspected.head;
        result.manifest = inspected.manifest;
        result.encoded_head = inspected.encoded_head;
        result.encoded_manifest = inspected.encoded_manifest;
        result.head_rows = 1;
        result.head_bytes = result.encoded_head.size();
        result.manifest_rows = 1;
        result.manifest_bytes = result.encoded_manifest.size();

        Snapshot finished_snapshot = std::exchange(registered_snapshot, nullptr);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [finished_snapshot] { UnregisterSnapshot(finished_snapshot); });
    } catch (...) {
        // The translated PostgreSQL ERROR owns snapshot cleanup.
        registered_snapshot = nullptr;
        throw;
    }
    return result;
}

/**
 * Run the authenticated process-parallel row transport probe.
 *
 * This path validates READY metadata and source authority once. It then
 * delegates the timed row transport to the hash-major storage session.
 */
[[nodiscard]] inline hash_major_minimizer_parallel_scan_stats
hash_major_minimizer_parallel_scan_probe_impl(Oid rel_oid,
                                              std::string_view index_name,
                                              std::uint64_t sequence_rowno,
                                              std::uint32_t requested_workers) {
    const bool is_bioseq_table =
        OidIsValid(rel_oid) && bioseq::postgres::support::invoke_pg_as_cpp([rel_oid] {
            return relation_oid_uses_column_table_access_method(rel_oid);
        });
    if (!is_bioseq_table) {
        ready_minimizer_error_as_cpp(
            ERRCODE_WRONG_OBJECT_TYPE,
            "bioseq hash-major parallel scan probe requires a USING bioseq table");
    }
    const Oid user_id = GetUserId();
    if (!bioseq::postgres::support::invoke_pg_as_cpp([rel_oid, user_id] {
            return object_ownercheck(RelationRelationId, rel_oid, user_id);
        })) {
        bioseq::postgres::support::invoke_pg_as_cpp(
            [rel_oid] { aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TABLE, get_rel_name(rel_oid)); });
    }
    if (index_name != k_minimizers_per_fragment_index_name &&
        index_name != k_minimizers_per_sequence_index_name) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "bioseq hash-major parallel scan probe requires a minimizer index",
            fmt::format("index_name={}", index_name));
    }
    if (!bioseq::indexing::sql_bigint_can_represent(sequence_rowno) || requested_workers > 64u) {
        ready_minimizer_error_as_cpp(
            ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
            "bioseq hash-major parallel scan probe parameter is out of range",
            fmt::format(
                "sequence_rowno={} requested_workers={}", sequence_rowno, requested_workers));
    }
    if (bioseq::aligner::settings::minimizer::kmer_size != 15 ||
        bioseq::aligner::settings::minimizer::window_size != 10) {
        ready_minimizer_error_as_cpp(
            ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
            "bioseq hash-major parallel scan probe requires the registered k=15,w=10 profile");
    }

    const std::int16_t seq_attno = single_seq_attno_for_inspection(rel_oid);
    const relation_identity relation_identity = index_relation_identity(rel_oid);
    if (!OidIsValid(relation_identity.db_oid) || !OidIsValid(relation_identity.tablespace_oid) ||
        !OidIsValid(relation_identity.relfilenode)) {
        ready_minimizer_error_as_cpp(
            ERRCODE_UNDEFINED_TABLE,
            "bioseq hash-major parallel scan probe source relation is unavailable");
    }
    Snapshot active_snapshot = GetActiveSnapshot();
    if (active_snapshot == nullptr || !IsMVCCSnapshot(active_snapshot)) {
        ready_minimizer_error_as_cpp(
            ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
            "bioseq hash-major parallel scan probe requires an active MVCC snapshot");
    }

    Snapshot registered_snapshot = nullptr;
    try {
        registered_snapshot = bioseq::postgres::support::invoke_pg_as_cpp(
            [active_snapshot] { return RegisterSnapshot(active_snapshot); });
        if (registered_snapshot == nullptr || registered_snapshot->regd_count == 0) {
            ready_minimizer_error_as_cpp(
                ERRCODE_INTERNAL_ERROR,
                "bioseq hash-major parallel scan probe could not register its snapshot");
        }
        const minimizer_generation_administration_identity identity{
            .database_oid = relation_identity.db_oid,
            .tablespace_oid = relation_identity.tablespace_oid,
            .main_relfilenode = relation_identity.relfilenode,
            .user_relid = rel_oid,
            .column_attno = seq_attno,
            .index_name = std::string{index_name},
        };
        minimizer_generation_catalog_source_authorization authorization =
            authorize_minimizer_generation_catalog_source_pg(
                identity, sequence_rowno, registered_snapshot);
        hash_major_minimizer_parallel_scan_stats result =
            probe_ready_hash_major_minimizer_parallel_scan_pg(identity,
                                                              sequence_rowno,
                                                              registered_snapshot,
                                                              std::move(authorization),
                                                              requested_workers);
        Snapshot finished_snapshot = std::exchange(registered_snapshot, nullptr);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [finished_snapshot] { UnregisterSnapshot(finished_snapshot); });
        return result;
    } catch (...) {
        // The SQL ERROR boundary owns registered snapshot cleanup.
        registered_snapshot = nullptr;
        throw;
    }
}

} // namespace bioseq::storage::indexing
