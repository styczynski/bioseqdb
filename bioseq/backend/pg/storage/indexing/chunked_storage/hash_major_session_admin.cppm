/**
 * @file hash_major_session_admin.cppm
 * @brief Defines hash-major session opening, inspection, probing, and generation cleanup.
 */
module;
#include <bioseq/pg_compat.h>
#include <libassert/assert.hpp>

extern "C" {
#include "access/heapam.h"
#include "access/nbtree.h"
#include "access/parallel.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h" // RelnameGetRelid
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "catalog/pg_opclass_d.h"
#include "catalog/pg_sequence.h"
#include "catalog/pg_type_d.h"
#include "commands/sequence.h"
#include "commands/tablespace.h"
#include "executor/instrument.h"
#include "executor/spi.h"
#include "executor/tuptable.h"
#include "storage/buffile.h"
#include "storage/dsm.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/procarray.h"
#include "storage/relfilelocator.h"
#include "storage/shm_toc.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/xid8.h"

#include "common/relpath.h"
#include "miscadmin.h"
#include "port.h"
#include "port/atomics.h"
}

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
export module bioseq.postgres.storage.indexing.hash_major_session_admin;
import std;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.common.memory_budget;
import bioseq.postgres.support; // bioseq::postgres::support::try_open_postgres_relation, bioseq::postgres::support::find_primary_key_oid
import bioseq.postgres.support.datums;
import bioseq.postgres.sql_boundary;
import bioseq.postgres.storage.manager; // seq_caches, sequence_locator, format helpers
import bioseq.storage.layout;           // sequence_locator
import bioseq.storage.size;             // sequence_size_t
import bioseq.indexing.type; // sequence_index_layout, index_sub_key_t, k_max_index_chunk_size_bytes, sentinels
import bioseq.indexing.storage;           // the concepts
import bioseq.indexing.minimizer_hashmap; // encoded header/bucket validation
import bioseq.indexing.minimizer_fragment_image; // explicit fixed-endian minimizer fragment image image ABI
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.indexing.minimizer_fragment_compact_hash32_adapter;
import bioseq.postgres.table.io; // begin_column_read/read_next_column_row/end_column_read/column_read_state
import bioseq.postgres.storage.scanner;        // PostgreSQL-backed lazy sequence access
import bioseq.storage.lazy_sequence;           // lazy_sequence
import bioseq.postgres.types.compact_sequence; // bioseq::pg_types::seq, dispatch_instance
import bioseq.sequences;                       // compact_sequence_range
import bioseq.sequences.simd;                  // canonical bounded base decoding
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.postgres.storage.indexing.residency;
import bioseq.postgres.storage.indexing.sorted_runs;
import bioseq.instrumentation.clock;
import bioseq.postgres.column.table_access;
import bioseq.postgres.storage.metadata;
import bioseq.postgres.storage.physical;
import bioseq.postgres.storage.indexing.sql_row_codecs;
import bioseq.postgres.storage.manager.sql_row_codecs;
import bioseq.postgres.sql.generated.core.storage_indexing_pg_chunked_storage.entity_handles;
import bioseq.postgres.storage.indexing.chunked_storage;
import bioseq.postgres.storage.indexing.hash_major_parallel_probe;
import bioseq.postgres.storage.indexing.hash_major_session_reader;
export namespace bioseq::storage::indexing {
/// Normal open outcome with either an owned reader or classified absence.
struct hash_major_minimizer_read_session_open_result {
    std::optional<index_storage_hash_major_minimizer_read_session> session{};
    minimizer_generation_relation_inspection relation_failure{
        .kind = minimizer_generation_relation_kind::malformed,
        .schema_error = minimizer_generation_schema_error::none,
    };
    bool head_absent = false;

    [[nodiscard]] explicit operator bool() const noexcept {
        return session.has_value();
    }
};

/// Open the exact current-generation reader or return a classified absence
/// without weakening schema validation.
[[nodiscard]] inline hash_major_minimizer_read_session_open_result
open_exact_hash_major_minimizer_read_session_pg(
    std::string_view table_name,
    std::string_view index_name,
    std::uint64_t query_key,
    const borrowed_executor_read_context& executor_read) {
    if (!executor_read.valid()) {
        minimizer_generation_storage_error_as_cpp(
            "hash-major read requires a valid executor context");
    }
    hash_major_minimizer_read_session_open_result result{};
    try {
        result.session.emplace(table_name,
                               index_name,
                               query_key,
                               executor_read.snapshot(),
                               executor_read.tablespace_oid());
        if (!result.session->read_active_head_manifest_once()) {
            result.session->close_head_absent();
            result.session.reset();
            result.head_absent = true;
            result.relation_failure = {
                .kind = minimizer_generation_relation_kind::current_exact,
                .schema_error = minimizer_generation_schema_error::none,
            };
        } else {
            result.relation_failure = {
                .kind = minimizer_generation_relation_kind::current_exact,
                .schema_error = minimizer_generation_schema_error::none,
            };
        }
    } catch (const minimizer_generation_relation_open_failure& failure) {
        result.relation_failure = failure.inspection;
    }
    return result;
}

/// Read-only restart/audit receipt for the complete fixed hash-major metadata
/// surface. parallel_scan_stream descriptors carry the directory/control/posting counts,
/// byte lengths and CRCs. The manifest carries the generation CRC.
struct ready_hash_major_minimizer_metadata_identity {
    hash_major_minimizer_head head{};
    hash_major_minimizer_manifest manifest{};
    encoded_hash_major_minimizer_head encoded_head{};
    encoded_hash_major_minimizer_manifest encoded_manifest{};
    minimizer_generation_source_locator source{};
    std::uint64_t source_content_checksum = 0;
    minimizer_generation_catalog_source_stats source_stats{};
    std::array<hash_major_minimizer_catalog_stream_receipt, 3> exact_catalog_streams{};
};

[[nodiscard]] inline ready_hash_major_minimizer_metadata_identity
inspect_ready_hash_major_minimizer_metadata_pg(
    const minimizer_generation_administration_identity& identity,
    std::uint64_t query_key,
    Snapshot registered_snapshot,
    minimizer_generation_catalog_source_authorization&& authorization) {
    if (identity.database_oid != MyDatabaseId || identity.index_name.empty() ||
        registered_snapshot == nullptr || !IsMVCCSnapshot(registered_snapshot) ||
        registered_snapshot->regd_count == 0) {
        minimizer_generation_storage_error_as_cpp(
            "hash-major metadata inspection input is invalid");
    }
    const std::string table_name{authorization.table_name_for_diagnostics()};
    if (table_name.empty() || (!identity.table_name.empty() && identity.table_name != table_name)) {
        minimizer_generation_storage_error_as_cpp(
            "hash-major metadata inspection source/table binding is invalid");
    }
    const minimizer_generation_source_locator source = authorization.source_for_diagnostics();
    const std::uint64_t source_content_checksum =
        authorization.source_content_checksum_for_diagnostics();
    const minimizer_generation_catalog_source_stats source_stats =
        authorization.stats_for_diagnostics();
    index_storage_hash_major_minimizer_read_session session{
        table_name,
        identity.index_name,
        query_key,
        registered_snapshot,
        identity.tablespace_oid,
    };
    if (!session.read_active_head_manifest_once()) {
        session.close_head_absent();
        minimizer_generation_storage_error_as_cpp(
            "hash-major metadata inspection found no READY head");
    }
    (void)session.consume_source_authorization(std::move(authorization));
    ready_hash_major_minimizer_metadata_identity result{
        .head = session.head(),
        .manifest = session.manifest(),
        .source = source,
        .source_content_checksum = source_content_checksum,
        .source_stats = source_stats,
    };
    std::ranges::copy(session.encoded_head(), result.encoded_head.begin());
    std::ranges::copy(session.encoded_manifest(), result.encoded_manifest.begin());
    result.exact_catalog_streams = session.audit_complete_streams();
    (void)session.finish();
    return result;
}

[[nodiscard]] inline ready_hash_major_minimizer_metadata_identity
inspect_ready_hash_major_minimizer_metadata_pg(
    const minimizer_generation_administration_identity& identity,
    std::uint64_t query_key,
    Snapshot registered_snapshot) {
    minimizer_generation_catalog_source_authorization authorization =
        authorize_minimizer_generation_catalog_source_pg(identity, query_key, registered_snapshot);
    return inspect_ready_hash_major_minimizer_metadata_pg(
        identity, query_key, registered_snapshot, std::move(authorization));
}

/// Test-only transport probe. It authenticates the same READY head, manifest
/// and source binding as the production reader, then scans only the three
/// current-generation hash-major stream subkeys through either one serial heap
/// scan or one core ParallelContext/shared ParallelTableScanDesc.
[[nodiscard]] inline hash_major_minimizer_parallel_scan_stats
probe_ready_hash_major_minimizer_parallel_scan_pg(
    const minimizer_generation_administration_identity& identity,
    std::uint64_t query_key,
    Snapshot registered_snapshot,
    minimizer_generation_catalog_source_authorization&& authorization,
    std::uint32_t requested_workers) {
    if (identity.database_oid != MyDatabaseId || identity.index_name.empty() ||
        registered_snapshot == nullptr || !IsMVCCSnapshot(registered_snapshot) ||
        registered_snapshot->regd_count == 0) {
        minimizer_generation_storage_error_as_cpp(
            "hash-major parallel scan probe input is invalid");
    }
    const std::string table_name{authorization.table_name_for_diagnostics()};
    if (table_name.empty()) {
        minimizer_generation_storage_error_as_cpp(
            "hash-major parallel scan probe table binding is empty");
    }
    index_storage_hash_major_minimizer_read_session session{
        table_name,
        identity.index_name,
        query_key,
        registered_snapshot,
        identity.tablespace_oid,
    };
    if (!session.read_active_head_manifest_once()) {
        session.close_head_absent();
        minimizer_generation_storage_error_as_cpp(
            "hash-major parallel scan probe found no READY head");
    }
    (void)session.consume_source_authorization(std::move(authorization));
    hash_major_minimizer_parallel_scan_stats result =
        session.run_parallel_scan_probe(requested_workers);
    (void)session.finish();
    return result;
}

inline void hash_major_minimizer_parallel_scan_probe_worker_main_pg(dsm_segment* segment,
                                                                    shm_toc* toc) {
    using namespace hash_major_minimizer_parallel_probe_detail;
    if (segment == nullptr || toc == nullptr || !IsParallelWorker()) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq hash-major parallel scan worker binding is invalid")));
    }
    auto* header = static_cast<parallel_scan_header*>(shm_toc_lookup(toc, k_header_toc_key, false));
    auto* parallel_scan =
        static_cast<ParallelTableScanDesc>(shm_toc_lookup(toc, k_parallel_scan_toc_key, false));
    auto* seen_words =
        static_cast<pg_atomic_uint64*>(shm_toc_lookup(toc, k_seen_bitset_toc_key, false));
    if (header == nullptr || parallel_scan == nullptr || seen_words == nullptr ||
        header->magic != k_toc_magic ||
        header->format_revision != k_parallel_header_format_revision ||
        header->relation_locator.dbOid != MyDatabaseId || GetActiveSnapshot() == nullptr ||
        !IsMVCCSnapshot(GetActiveSnapshot())) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq hash-major parallel scan worker state is invalid")));
    }

    // ParallelWorkerMain joined the leader's lock group before loading this
    // symbol, so NoLock reuses the leader-held AccessShareLock and cannot
    // release it independently.
    Relation relation = table_open(header->relation_oid, NoLock);
    parallel_scan_pg_leaf(relation, parallel_scan, header, seen_words);
    table_close(relation, NoLock);
}

} // namespace bioseq::storage::indexing
