/**
 * @file
 * @brief Table maintenance, planner statisics, and transaction cleanup.
 */
module;
#include <algorithm>
#include <array>
#include <bioseq/pg_compat.h>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
export module bioseq.postgres.table:access_method_maintenance;

import bioseq.build_config;
import bioseq.postgres.storage.physical;
import bioseq.postgres.storage.compression;
import bioseq.common.logging;
import bioseq.postgres.storage.metadata;
import bioseq.postgres.storage.metadata.sql_entities;
import bioseq.postgres.storage.metadata.sql_rows;
import bioseq.postgres.vectorization.types;
import bioseq.postgres.table.types;
import bioseq.postgres.configuration.core;
import bioseq.postgres.table.io;
import bioseq.postgres.support;
import bioseq.postgres.support.sql_boundary;
import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.storage.indexing.chunked_storage;
import bioseq.postgres.storage.indexing.chunked_storage.sql_entities;
import bioseq.postgres.storage.manager;
import bioseq.postgres.storage.manager.sequence_storage.sql_entities;
import bioseq.postgres.storage.manager.sql_rows;
import bioseq.postgres.storage.scanner;
import bioseq.storage.layout;
import bioseq.postgres.table.am_cache;
import bioseq.postgres.types.compact_sequence;
import bioseq.postgres.column.table_access;
import :access_identity;
import :access_common;
import :access_hooks;

/** Retry a relation lock, with an optional timeout. */
static bool try_lock_relation_until_timeout(
    Relation rel, LOCKMODE lock_mode, int timeout, int retry_interval, bool acquire);
/** @brief Emit relation storage statistics at the requested PostgreSQL log level. */
static void log_relation_statistics(Relation rel, int elevel);
/** @brief Remove unused physical storage beyond the relation's last live stripe. */
static void truncate_column_storage_tail(Relation rel, int elevel);
/** @brief Compact eligible stripes and report whether physical storage changed. */
static bool truncate_and_compact_column_stripes(Relation rel, int elevel);

/** Replace a relation's physical storage and initialize Bioseq metadata. */
void bioseq_relation_set_new_filenode(Relation rel,
                                      const RelFileLocator* newrnode,
                                      char persistence,
                                      TransactionId* freeze_xid,
                                      MultiXactId* minmulti) {
    if (bioseq::storage::is_registered_sequence_fragment_relation(RelationGetRelid(rel))) {
        bioseq_log<logging_level::error, logging_target::postgres>(
            "bioseq storage: destructive rewrite of a registered sequence fragment relation is "
            "forbidden");
    }
    if (persistence == RELPERSISTENCE_TEMP) {
        bioseq_log<logging_level::error, logging_target::postgres>(
            "bioseq tables cannot be TEMPORARY; use UNLOGGED or permanent");
    }

    /* Remove old metadata when the physical file number changes. */
    if (rel->rd_locator.relNumber != newrnode->relNumber) {
        mark_write_state_relation_dropped(rel->rd_locator.relNumber, GetCurrentSubTransactionId());

        delete_metadata_rows(rel->rd_locator);

        /* Drop sequence tables belonging to the old physical file. */
        bioseq::storage::bioseq_default_storage_manager::drop_sub_tables_for_main(
            rel->rd_locator.dbOid, rel->rd_locator.spcOid, rel->rd_locator.relNumber);
    }
    *freeze_xid = RecentXmin;
    *minmulti = GetOldestMultiXactId();
    SMgrRelation srel = RelationCreateStorage(*newrnode, persistence, true);

    bioseq_log<logging_level::debug>(
        "[bioseq] pg: bioseq_relation_set_new_filenode() initialising columnar storage");
    initialize_column_storage(srel, allocate_column_storage_id());
    bioseq_log<logging_level::debug>(
        "[bioseq] pg: bioseq_relation_set_new_filenode() initialising columnar options");
    initialize_column_options(rel->rd_id);

    smgrclose(srel);

    /* The first stripe reservation creates the remaining metadata. */
}

/** Reinitialize storage for a relation truncated outside transactional rewrite. */
void bioseq_relation_nontransactional_truncate(Relation rel) {
    if (bioseq::storage::is_registered_sequence_fragment_relation(RelationGetRelid(rel))) {
        bioseq_log<logging_level::error, logging_target::postgres>(
            "bioseq storage: TRUNCATE of a registered sequence fragment relation is forbidden");
    }
    RelFileLocator relfilelocator = rel->rd_locator;

    drop_write_state_outside_transaction(relfilelocator.relNumber);

    delete_metadata_rows(relfilelocator);

    /* PostgreSQL uses this path  for storage that is private to, the current
     * transaction, so it can truncate the existing file in place. */
    RelationTruncate(rel, 0);

    bioseq_log<logging_level::debug>(
        "[bioseq] pg: bioseq_relation_nontransactional_truncate() initialising columnar storage");
    uint64_t storage_id = allocate_column_storage_id();

    initialize_column_storage(RelationGetSmgr(rel), storage_id);
}

/** Copy physical storage to a newly allocated relation locator */
void bioseq_relation_copy_data(Relation rel, const RelFileLocator* newrnode) {
    char persistence = rel->rd_rel->relpersistence;

    /* Flush dirty buffers so the disk copy includes recent writes. */
    FlushRelationBuffers(rel);

    SMgrRelation srel = RelationCreateStorage(*newrnode, persistence, true);


    RelationCopyStorage(RelationGetSmgr(rel), srel, MAIN_FORKNUM, persistence);

    for (ForkNumber fork_num = static_cast<ForkNumber>(MAIN_FORKNUM + 1); fork_num <= MAX_FORKNUM;
         fork_num = static_cast<ForkNumber>(static_cast<int>(fork_num) + 1)) {
        if (smgrexists(srel, fork_num)) {
            smgrcreate(srel, fork_num, false);

            /* Log fork creation for permanent tables and unlogged-table init forks. */
            if (RelationIsPermanent(rel) ||
                (persistence == RELPERSISTENCE_UNLOGGED && fork_num == INIT_FORKNUM))
                log_smgrcreate(newrnode, fork_num);

            RelationCopyStorage(RelationGetSmgr(rel), srel, fork_num, persistence);
        }
    }
}

/** Copy visible rows and their sequences for VACUUM FULL.
 *
 * PostgreSQL also uses this callback for CLUSTER, but ordering a Bioseq table
 * by an index is unsupported. Each copied sequence gets storage  in the new
 * table before the old table's sequence fragments are removed.
 */
void bioseq_relation_copy_for_cluster(Relation old_heap,
                                      Relation new_heap,
                                      Relation old_index,
                                      bool use_sort,
                                      TransactionId oldest_xmin,
#if PG_VERSION_NUM >= 190000
                                      Snapshot rewrite_snapshot,
#endif
                                      TransactionId* xid_cutoff,
                                      MultiXactId* multi_cutoff,
                                      double* num_tuples,
                                      double* tups_vacuumed,
                                      double* tups_recently_dead) {
    TupleDesc source_desc = RelationGetDescr(old_heap);
    TupleDesc target_desc = RelationGetDescr(new_heap);

    if (old_index != nullptr || use_sort) {
        bioseq_report<logging_level::error, logging_target::postgres>()
            .with_sql_error_code(ERRCODE_FEATURE_NOT_SUPPORTED)
            .with_message("clustering columnar tables using indexes is not supported");
    }

    /* PostgreSQL preserves dropped-column positions during a rewrite, so
     * both tuple descriptors must have the same number of attributes. */
    Assert(source_desc->natts == target_desc->natts);

    /* Read options using the old table identxty before PostgreSQL swaps files. */
    column_options bioseq_column_options =
        read_column_options(old_heap->rd_id).value_or(column_options{});

    /* Write through the new table's state so its sequence fragments receive
     * the destination storage identity. */
    column_write_state* write_state = initialize_column_write_state(
        new_heap, target_desc, RelationGetRelid(new_heap), GetCurrentSubTransactionId());
    auto* write_caches = ensure_write_side_seq_caches(new_heap, write_state);
    const auto cap_bytes = write_caches->cap_bytes_snapshot;

    /* Copy every column. */
    int natts = old_heap->rd_att->natts;
    bioseq::postgres::support::postgres_bitmapset attr_needed =
        bioseq::postgres::support::postgres_bitmapset::make_member_range(natts);

    /* Copy all visible rows without additional predicates. */
    List* scan_qual = NIL;

    /* Row masks have MVCC versions. SnapshotAny could select an old mask and
     * copy a deleted row into the  new table. Use one registered snapshot for
     * rows, row masks, and sequence fragments throughout the rewrite. */
#if PG_VERSION_NUM >= 190000
    // Use PostgreSQL 19's supplied rewrite snapshot when available.
    Snapshot snapshot = RegisterSnapshot(
        rewrite_snapshot != nullptr ? rewrite_snapshot : GetTransactionSnapshot());
#else
    Snapshot snapshot = RegisterSnapshot(GetTransactionSnapshot());
#endif

    MemoryContext scan_context = create_column_scan_memory_context();
    bool random_access = false;
    column_read_state* read_state = init_bioseq_read_state(old_heap,
                                                           source_desc,
                                                           attr_needed,
                                                           scan_qual,
                                                           scan_context,
                                                           snapshot,
                                                           random_access,
                                                           nullptr);

    /* Cache fragment reads from the old table. */
    auto* read_caches = bioseq::storage::alloc_seq_caches(scan_context);
    read_caches->side = bioseq::storage::seq_caches::access_side::read;
    read_caches->lock_mode = AccessShareLock;
    read_caches->read_state_mctx = scan_context;
    read_caches->main_database_oid = old_heap->rd_locator.dbOid;
    read_caches->main_tablespace_oid = old_heap->rd_locator.spcOid;
    read_caches->main_relfilenode = old_heap->rd_locator.relNumber;

    Datum* values = bioseq::postgres::support::allocate_zeroed_array<Datum>(source_desc->natts);
    bool* nulls = bioseq::postgres::support::allocate_zeroed_array<bool>(source_desc->natts);

    const auto& source_amc = bioseq::storage::get_table_access_cache(old_heap);
    const std::uint64_t new_main_storage_id = column_storage_id(new_heap, false);

    *num_tuples = 0;

    while (read_next_column_row(read_state, values, nulls, nullptr)) {
        std::vector<bioseq::storage::pending_sequence_source> pending_sources;
        /* Read full sequences from the old table's fragments. */
        for (std::int16_t attno : source_amc.sequence_attribute_numbers) {
            const int idx = attno - 1;
            if (idx < 0 || idx >= source_desc->natts)
                continue;
            if (nulls[idx])
                continue;
            auto* hdr =
                reinterpret_cast<const bioseq::pg_types::seq*>(DatumGetPointer(values[idx]));
            if (!bioseq::storage::is_locator_stub(hdr))
                continue;
            const auto loc = bioseq::storage::decode_locator_from_stub(hdr);
            bioseq::storage::validate_locator_flat_materialization_length(loc);
            bioseq::pg_types::seq* real =
                bioseq::storage::bioseq_default_storage_manager::fetch_payload(
                    old_heap->rd_locator.dbOid,
                    old_heap->rd_locator.spcOid,
                    old_heap->rd_locator.relNumber,
                    attno,
                    loc,
                    read_state->snapshot,
                    scan_context,
                    read_caches);
            values[idx] = PointerGetDatum(real);
        }

        /* Store sequences in the new table and replace their values with its
         * locators. The destination write state owns the cache */
        for (std::int16_t attno : source_amc.sequence_attribute_numbers) {
            const int idx = attno - 1;
            if (idx < 0 || idx >= source_desc->natts)
                continue;
            if (nulls[idx])
                continue;
            auto* hdr =
                reinterpret_cast<const bioseq::pg_types::seq*>(DatumGetPointer(values[idx]));
            if (bioseq::storage::is_locator_stub(hdr))
                continue;
            auto pending =
                bioseq::storage::bioseq_default_storage_manager::insert_payload_as_fragments(
                    RelationGetRelid(old_heap),
                    new_main_storage_id,
                    new_heap->rd_locator.dbOid,
                    new_heap->rd_locator.spcOid,
                    new_heap->rd_locator.relNumber,
                    attno,
                    target_desc,
                    hdr,
                    cap_bytes,
                    write_caches);
            const auto loc = pending.locator();
            bioseq::pg_types::seq* stub = bioseq::storage::alloc_locator_stub_in(
                get_column_write_per_tuple_context(write_state),
                static_cast<std::uint8_t>(hdr->alphabet_code),
                static_cast<std::uint8_t>(hdr->storage_unit_byte_width),
                hdr->padding_letters,
                loc.logical_length,
                loc);
            values[idx] = PointerGetDatum(stub);
            pending_sources.push_back(std::move(pending));
        }

        write_column_row(write_state, values, nulls);
        for (auto& source : pending_sources) {
            source.publish_after_parent_row();
        }
        (*num_tuples)++;
    }

    *tups_vacuumed = 0;

    read_caches->close_all(/*commit=*/true);
    bioseq::storage::destroy_seq_caches(read_caches);

    end_column_read(read_state);
    UnregisterSnapshot(snapshot);
    MemoryContextDelete(scan_context);

    /* Remove old sequence tables before PostgreSQL swaps the physical files.
     * After the swap, their original parent storage  identity would be gone. */
    bioseq::storage::bioseq_default_storage_manager::drop_sub_tables_for_main(
        old_heap->rd_locator.dbOid, old_heap->rd_locator.spcOid, old_heap->rd_locator.relNumber);
}

/** Merge small stripes at the end of the table and remove deleted rows.
 *
 * Select a yail that fits the row and memory limits, read its visible rows,
 * and write them after the last untouched stripe. Rewritten rows receive new
 * row numbers. A single candidate is rewritten only when enough rows were
 * deleted to make the, work worthwhile.
 */
static bool truncate_and_compact_column_stripes(Relation rel, int elevel) {
    uint64_t total_row_number_count = 0;
    uint32_t starting_stripe_list_position = 0;

    TupleDesc tuple_desc = RelationGetDescr(rel);

    if (tuple_desc->natts == 0) {
        bioseq_log_pg(elevel,
                      "\"{}\": stopping vacuum due to zero column table",
                      RelationGetRelationName(rel));
        return false;
    }


    column_options bioseq_column_options =
        read_column_options(rel->rd_id).value_or(column_options{});

    /* Consider the newest stripes first. */
    List* stripe_metadata_list = list_stripes_for_relation(rel->rd_locator, BackwardScanDirection);

    if (stripe_metadata_list == NIL) {
        bioseq_log_pg(
            elevel, "\"{}\": stopping vacuum due to empty table", RelationGetRelationName(rel));
        return false;
    }

    uint32_t last_stripe_deleted_rows = 0;

    Size total_decompressed_stripe_length = 0;

    for (auto stripe_metadata_entry :
         bioseq::postgres::support::postgres_list<stripe_metadata>(stripe_metadata_list)) {
        last_stripe_deleted_rows = deleted_rows_for_stripe(
            rel->rd_locator, stripe_metadata_entry->chunk_count, stripe_metadata_entry->id);
        total_decompressed_stripe_length +=
            decompressed_length_for_stripe(rel->rd_locator, stripe_metadata_entry->id);

        if (total_decompressed_stripe_length >= 1024000000) {
            break;
        }

        uint64_t stripe_row_limit = stripe_metadata_entry->row_count - last_stripe_deleted_rows;

        if ((total_row_number_count + stripe_row_limit >= bioseq_column_options.stripe_row_limit)) {
            break;
        }

        total_row_number_count += stripe_row_limit;
        starting_stripe_list_position++;
    }

    /* No tail stripe fits within the compaction limits. */
    if (starting_stripe_list_position == 0) {
        return false;
    }

    /* Rewriting a single stripe only saves space if enough rows were deleted. */
    else if (starting_stripe_list_position == 1) {

        float percentage_of_delete_rows =
            static_cast<float>(last_stripe_deleted_rows) /
            static_cast<float>(total_row_number_count + last_stripe_deleted_rows);

        bool should_vacuum_only_stripe = percentage_of_delete_rows > 0.2f;

        if (!should_vacuum_only_stripe) {
            return false;
        }
    }

    /* Clear the vacuum status before  writing catalog rows so other backends
     * include this transaction when computing the oldest active transaction. */

    LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
    MyProc->statusFlags = 0;
    ProcGlobal->statusFlags[MyProc->pgxactoff] = MyProc->statusFlags;

    LWLockRelease(ProcArrayLock);

    /* Take a transaction snapshot after clearing the vacuum status. */
    PushActiveSnapshot(GetTransactionSnapshot());
    column_write_state* write_state =
        begin_column_write(rel->rd_locator, bioseq_column_options, tuple_desc);

    /* Preserve every column while rewriting the tail. */
    int natts = rel->rd_att->natts;
    bioseq::postgres::support::postgres_bitmapset attr_needed =
        bioseq::postgres::support::postgres_bitmapset::make_member_range(natts);

    /* Copy all visible rows without additional predicates. */
    List* scan_qual = NIL;

    MemoryContext scan_context = create_column_scan_memory_context();
    bool random_access = true;
    // Use the active snapshot to read current row masks. SnapshotAny could
    // pick an old mask and copy deleted rows back into the table.
    column_read_state* read_state = init_bioseq_read_state(
        rel, tuple_desc, attr_needed, scan_qual, scan_context,
        GetActiveSnapshot(), random_access, nullptr);

    set_stripe_read_state(read_state,
                          static_cast<stripe_metadata*>(
                              list_nth(stripe_metadata_list, starting_stripe_list_position - 1)));

    Datum* values = bioseq::postgres::support::allocate_zeroed_array<Datum>(tuple_desc->natts);
    bool* nulls = bioseq::postgres::support::allocate_zeroed_array<bool>(tuple_desc->natts);

    /* Compacted rows receive new row numbers. */
    while (read_next_column_row(read_state, values, nulls, nullptr)) {
        write_column_row(write_state, values, nulls);
    }

    uint64_t new_data_reservation;

    if (list_length(stripe_metadata_list) > starting_stripe_list_position) {
        stripe_metadata* boundary_stripe = static_cast<stripe_metadata*>(
            list_nth(stripe_metadata_list, starting_stripe_list_position));
        // Keep the whole last  untouched stripe. The reservation points just
        // past its final byte.
        new_data_reservation = boundary_stripe->file_offset + boundary_stripe->data_length;
    } else {
        stripe_metadata* boundary_stripe = static_cast<stripe_metadata*>(
            list_nth(stripe_metadata_list, starting_stripe_list_position - 1));
        new_data_reservation = boundary_stripe->file_offset;
    }

    truncate_column_storage(rel, new_data_reservation);

    end_column_write(write_state);
    end_column_read(read_state);
    MemoryContextDelete(scan_context);

    for (int stripe_index = 0; stripe_index < starting_stripe_list_position; stripe_index++) {
        stripe_metadata* metadata =
            static_cast<stripe_metadata*>(list_nth(stripe_metadata_list, stripe_index));
        delete_metadata_rows_for_stripe(rel->rd_locator, metadata->id);
    }

    PopActiveSnapshot();

    return true;
}

/** Return the physical row count recorded by a relation's stripe metadata. */
static uint64_t column_table_tuple_count(Relation relation) {
    List* stripe_list = list_stripes_for_relation(relation->rd_locator, ForwardScanDirection);
    uint64_t tuple_count = 0;

    for (auto stripe : bioseq::postgres::support::postgres_list<stripe_metadata>(stripe_list)) {
        tuple_count += stripe->row_count;
    }

    return tuple_count;
}

/** Run VACUUM storage cleanup and update PostgreSQL's table sqatistics */
void bioseq_vacuum_rel(Relation rel,
#if PG_VERSION_NUM >= 190000
                       const VacuumParams* params,
#else
                       VacuumParams* params,
#endif
                       BufferAccessStrategy bstrategy) {
    static_cast<void>(bstrategy);
    run_tableam_cpp_checked("bioseq_vacuum_rel", [rel, params] {
        bioseq::postgres::support::page_cache_disable_scope cache_guard{
            bioseq::settings::column_cache::enabled};
        bioseq::postgres::support::invoke_pg_as_cpp([rel, params] {
            pgstat_progress_start_command(PROGRESS_COMMAND_VACUUM, RelationGetRelid(rel));

            /* VACUUM is also the recovery path for an older storage format. Upgrade
             * its metapage before reading storage metadata. */
            const bool upgrades_storage = true;
            ensure_current_column_storage_format(rel, upgrades_storage);

            int elevel = (params->options & VACOPT_VERBOSE) ? INFO : DEBUG2;

            /* PostgreSQL resolves the truncate option before calling the table AM. */
            Assert(params->truncate != VACOPTVALUE_UNSPECIFIED);

            if (params->options & VACOPT_VERBOSE) {
                log_relation_statistics(rel, elevel);
            }

            /* Compact eligible tail stripes and release unused space when requested. */
            if (params->truncate == VACOPTVALUE_ENABLED) {
                truncate_column_storage_tail(rel, elevel);
            }

            BlockNumber new_rel_pages = smgrnblocks(RelationGetSmgr(rel), MAIN_FORKNUM);


            List* index_list = RelationGetIndexList(rel);
            int nindexes = list_length(index_list);

            VacuumCutoffs cutoffs;
            vacuum_get_cutoffs(rel, params, &cutoffs);

            Assert(MultiXactIdPrecedesOrEquals(cutoffs.MultiXactCutoff, cutoffs.OldestMxact));
            Assert(TransactionIdPrecedesOrEquals(cutoffs.FreezeLimit, cutoffs.OldestXmin));

            /* Transaction IDs live in metadata tables, so the main column file  needs
             * no tuple freezing. Advance its, freeze limits to the current cutoffs. */
            TransactionId new_rel_frozen_xid = cutoffs.OldestXmin;
            MultiXactId new_relmin_mxid = cutoffs.OldestMxact;
            double new_live_tuples = column_table_tuple_count(rel);

            /* Column storage has no heap visibility-map pages. */
            BlockNumber new_rel_allvisible = 0;

            bool frozenxid_updated;
            bool minmulti_updated;

            /* PostgreSQL 18 also requests a frozen-page count. Column storage has none. */
#if PG_VERSION_NUM >= PG_VERSION_18
            BlockNumber new_rel_allfrozen = 0;
            vac_update_relstats(rel,
                                new_rel_pages,
                                new_live_tuples,
                                new_rel_allvisible,
                                new_rel_allfrozen,
                                nindexes > 0,
                                new_rel_frozen_xid,
                                new_relmin_mxid,
                                &frozenxid_updated,
                                &minmulti_updated,
                                false);
#else
	vac_update_relstats(rel, new_rel_pages, new_live_tuples,
						new_rel_allvisible, nindexes > 0,
							new_rel_frozen_xid, new_relmin_mxid,
						&frozenxid_updated, &minmulti_updated, false);
#endif

            /* PostgreSQL 18 adds a start time, with zero meaning unknown. PostgreSQL
             * 19 takes the open Relation instead of its OID and shared-table flag. */
#if PG_VERSION_NUM >= 190000
            pgstat_report_vacuum(rel,
                                 std::max<int64_t>(new_live_tuples, 0),
                                 0,
                                 0 /* starttime */);
#elif PG_VERSION_NUM >= PG_VERSION_18
            pgstat_report_vacuum(RelationGetRelid(rel),
                                 rel->rd_rel->relisshared,
                                 std::max<int64_t>(new_live_tuples, 0),
                                 0,
                                 0 /* starttime */);
#else
	pgstat_report_vacuum(RelationGetRelid(rel),
							rel->rd_rel->relisshared,
							std::max<int64_t>(new_live_tuples, 0),
							0);
#endif
            pgstat_progress_end_command();
        });
    });
}

/** Emit stripe, compression, and row statistics for `VACUUM VERBOSE`. */
static void log_relation_statistics(Relation rel, int elevel) {
    ListCell* stripe_metadata_cell = nullptr;
    RelFileLocator relfilelocator = rel->rd_locator;
    StringInfo info_buf = makeStringInfo();

    std::array<int, static_cast<std::size_t>(bioseq::compression_algorithm::count)>
        compression_stats{};
    uint64_t total_stripe_length = 0;
    uint64_t tuple_count = 0;
    uint64_t chunk_count = 0;
    TupleDesc tuple_descriptor = RelationGetDescr(rel);
    uint64_t dropped_chunks_with_data = 0;
    uint64_t total_decompressed_length = 0;

    List* stripe_list = list_stripes_for_relation(relfilelocator, ForwardScanDirection);
    int stripe_count = list_length(stripe_list);

    MemoryContext relation_statistics_context = AllocSetContextCreate(
        CurrentMemoryContext, "Vacuum Relation Stats Context", ALLOCSET_SMALL_SIZES);

    {
        bioseq::postgres::support::memory_context_scope statistics_context_scope{
            relation_statistics_context};

        for (auto stripe : bioseq::postgres::support::postgres_list<stripe_metadata>(stripe_list)) {
            stripe_skip_list* skiplist = read_stripe_skip_list(relfilelocator,
                                                               stripe->id,
                                                               RelationGetDescr(rel),
                                                               stripe->chunk_count,
                                                               GetTransactionSnapshot());
            for (uint32_t column = 0; column < skiplist->column_count; column++) {
                bool attr_dropped = bioseq::postgres::support::tuple_descriptor_attribute(tuple_descriptor, column)->attisdropped;
                for (uint32_t chunk = 0; chunk < skiplist->chunk_count; chunk++) {
                    chunk_skip_node* skipnode = &skiplist->chunk_skip_nodes[column][chunk];

                    /* Count only chunks with stored values, including dropped columns that
                     * still occupy space. */
                    if (skipnode->value_stream_length > 0) {
                        compression_stats[static_cast<int>(
                            skipnode->value_compression_algorithm)]++;
                        chunk_count++;

                        if (attr_dropped) {
                            dropped_chunks_with_data++;
                        }
                    }

                    /* The null bitmap is stored uncompressed, so its byte count is already
                     * the uncompressed size. */
                    total_decompressed_length += skipnode->exists_stream_length;
                    total_decompressed_length += skipnode->value_decompressed_length;
                }
            }

            tuple_count += stripe->row_count;
            total_stripe_length += stripe->data_length;

            MemoryContextReset(relation_statistics_context);
        }
    }

    uint64_t rel_pages = smgrnblocks(RelationGetSmgr(rel), MAIN_FORKNUM);
    RelationCloseSmgr(rel);

    const std::int64_t storage_id = static_cast<std::int64_t>(column_storage_id(rel, false));

    double compression_rate = total_stripe_length ? static_cast<double>(total_decompressed_length) /
                                                        static_cast<double>(total_stripe_length)
                                                  : 1.0;

    appendStringInfo(info_buf, "storage id: %ld\n", storage_id);
    appendStringInfo(info_buf,
                     "total file size: %ld, total data size: %ld\n",
                     rel_pages * BLCKSZ,
                     total_stripe_length);
    appendStringInfo(info_buf, "compression rate: %.2fx\n", compression_rate);
    appendStringInfo(info_buf,
                     "total row count: %ld, stripe count: %d, "
                     "average rows per stripe: %ld\n",
                     tuple_count,
                     stripe_count,
                     stripe_count ? tuple_count / stripe_count : 0);
    appendStringInfo(info_buf,
                     "chunk count: %ld"
                     ", containing data for dropped columns: %ld",
                     chunk_count,
                     dropped_chunks_with_data);
    for (int compression_algorithm = 0;
         compression_algorithm < static_cast<int>(bioseq::compression_algorithm::count);
         compression_algorithm++) {
        const char* compression_name = compression_algorithm_name(
            static_cast<bioseq::compression_algorithm>(compression_algorithm));

        /* Omit  compression algorithms unavailable in this build. */
        if (compression_name == nullptr) {
            continue;
        }


        if (compression_stats[compression_algorithm] == 0) {
            continue;
        }

        appendStringInfo(info_buf,
                         ", %s compressed: %d",
                         compression_name,
                         compression_stats[compression_algorithm]);
    }
    appendStringInfoString(info_buf, "\n");

    bioseq_log_pg(
        elevel, "statistics for \"{}\":\n{}", RelationGetRelationName(rel), info_buf->data);
}

/** Release unused space at the end of a Bioseq table.
 *
 * Try the exclusive lock without joining its waut queue, then compact eligible
 * tail stripes or truncate unused bytes. Sequence-fragment tables only allow
 * truncation because their locators must keep the same row numbers.
 */
static void truncate_column_storage_tail(Relation rel, int elevel) {
    PGRUsage ru0;

    pg_rusage_init(&ru0);


    pgstat_progress_update_param(PROGRESS_VACUUM_PHASE, PROGRESS_VACUUM_PHASE_TRUNCATE);

    /* Avoid waiting in the lock queue while holding a weaker relation lock
     * Conditional retries let VACUUM give up if  another transaction conflicts. */
    if (!try_lock_relation_until_timeout(
            rel,
            AccessExclusiveLock,
            bioseq::build_config::bioseq_vacuum_truncate_lock_timeout_milliseconds,
            bioseq::build_config::bioseq_vacuum_truncate_lock_wait_interval_milliseconds,
            false)) {
        /* Leave the tail in place when the lock retry limit is reached. */
        bioseq_log_pg(elevel,
                      "\"{}\": stopping truncate due to conflicting lock request",
                      RelationGetRelationName(rel));
        return;
    }

    const bool locator_pinned_fragment =
        bioseq::storage::is_registered_sequence_fragment_relation(RelationGetRelid(rel));
    // Sequence locators contain physical row numbers. Compaction changes
    // those numbers, so registered fragment tables can only truncate unused
    // tail space. Their live stripes and row-number reservations must stay put.
    bool stripes_truncated =
        locator_pinned_fragment ? false : truncate_and_compact_column_stripes(rel, elevel);
    if (locator_pinned_fragment) {
        bioseq_log_pg(
            elevel,
            "\"{}\": skipping row-moving compaction for locator-pinned sequence fragments",
            RelationGetRelationName(rel));
    }

    /* A tail can contain unused bytes even when no stripes were compacted. */
    if (!stripes_truncated) {

        /* The exclusive lock prevents new stripes from appearing while we
         * calculate and truncate, the unused tail. */
        uint64_t new_data_reservation = std::max<uint64_t>(
            find_highest_used_address(rel->rd_locator) + 1, first_logical_offset);

        BlockNumber old_rel_pages = smgrnblocks(RelationGetSmgr(rel), MAIN_FORKNUM);

        if (!truncate_column_storage(rel, new_data_reservation)) {
            UnlockRelation(rel, AccessExclusiveLock);
            return;
        }

        BlockNumber new_rel_pages = smgrnblocks(rel->rd_smgr, MAIN_FORKNUM);

        bioseq_report_pg_lazy(elevel, [elevel, rel, old_rel_pages, new_rel_pages, &ru0] {
            bioseq_report_pg(elevel)
                .with_message("\"{}\": truncated {} to {} pages",
                              RelationGetRelationName(rel),
                              old_rel_pages,
                              new_rel_pages)
                .with_detail("{}", pg_rusage_show(&ru0));
        });
    }

    /* Release  the lock after truncation has notified other backends. */
    UnlockRelation(rel, AccessExclusiveLock);
}

/** Acquire a relation lock with repeated nonblockig attempts.
 *
 * Sleep between attempts and check interrupts. A false acquire argument
 * enables the timeout. When acquire is true, retry until the lock is obtained.
 */
static bool try_lock_relation_until_timeout(
    Relation rel, LOCKMODE lock_mode, int timeout, int retry_interval, bool acquire) {
    int lock_retry = 0;

    while (true) {
        if (ConditionalLockRelation(rel, lock_mode)) {
            break;
        }


        CHECK_FOR_INTERRUPTS();

        if (!acquire && (++lock_retry > (timeout / retry_interval))) {
            return false;
        }

        pg_usleep(retry_interval * 1000L);
    }

    return true;
}

/** Present the whole table to ANALYZE as one logical block. */
bool bioseq_scan_analyze_next_block(TableScanDesc scan, ReadStream* stream) {
    /* Column rows can cross page boundaries. Treat the table as one block
     * so ANALYZE scans its rows once and samples from that stream. */
    const auto bioseq_scan = reinterpret_cast<column_scan_handle>(scan);
    return bioseq_scan->read_state == nullptr;
}

/** Feed every visible row to PostgreSQL's ANALYZE reservoir sampler. */
bool bioseq_scan_analyze_next_tuple(TableScanDesc scan,
#if PG_VERSION_NUM < 190000
                                    TransactionId oldest_xmin,
#endif
                                    double* liverows,
                                    double* deadrows,
                                    TupleTableSlot* slot) {
    /* Disable the page cache while ANALYZE collects sample rows. */
    bioseq::postgres::support::page_cache_disable_scope cache_guard{
        bioseq::settings::column_cache::enabled};

    /* Supply every visible row because page  sampling does not fit column
     * storage. PostgreSQL chooses the sample from these rows. */
    if (bioseq_getnextslot(scan, ForwardScanDirection, slot)) {
        (*liverows)++;
        return true;
    }

    return false;
}

/** Return the stored byte count for a requested fork or combined forks. */
uint64 bioseq_relation_size(Relation rel, ForkNumber fork_number) {
    uint64 nblocks = 0;

    /* InvalidForkNumber requests the combined fork size */
    if (fork_number == InvalidForkNumber) {
        for (int fork_index = 0; fork_index < MAX_FORKNUM; fork_index++) {
            nblocks += smgrnblocks(RelationGetSmgr(rel), static_cast<ForkNumber>(fork_index));
        }
    } else {
        nblocks = smgrnblocks(RelationGetSmgr(rel), fork_number);
    }

    return nblocks * BLCKSZ;
}

/** Return false because Bioseq tables do not use a heap TOAST table. */
bool bioseq_relation_needs_toast_table(Relation rel) {
    return false;
}

/** Populate planner row, page, width, and visibility estimates. */
void bioseq_estimate_rel_size(
    Relation rel, int32_t* attr_widths, BlockNumber* pages, double* tuples, double* allvisfrac) {
    *pages = smgrnblocks(RelationGetSmgr(rel), MAIN_FORKNUM);
    *tuples = column_table_row_count(rel);

    /* Use a fixed all-visible estimate for the planner. Row reads still
     * check the snapshot and row mask. */
    *allvisfrac = 1.0;

    get_rel_data_width(rel, attr_widths);
}

/** Reject unsupported TABLESAMPLE block selection. */
bool bioseq_scan_sample_next_block(TableScanDesc scan, SampleScanState* scanstate) {
    bioseq_log<logging_level::error, logging_target::postgres>(
        "bioseq_scan_sample_next_block not implemented");
}

/** Reject unsupported TABLESAMPLE tuple selection. */
bool bioseq_scan_sample_next_tuple(TableScanDesc scan,
                                   SampleScanState* scanstate,
                                   TupleTableSlot* slot) {
    bioseq_log<logging_level::error, logging_target::postgres>(
        "bioseq_scan_sample_next_tuple not implemented");
}

/** Flush writes before commit, discard them on abort, and release readers. */
void column_transaction_callback(XactEvent event, void* arg) {
    switch (event) {
    case XACT_EVENT_COMMIT:
    case XACT_EVENT_PARALLEL_COMMIT:
    case XACT_EVENT_PREPARE: {
        // Pending speculative writes should have been rejected before  commit.
        // Save any remaining objects without rurning their cleanup. Transaction
        // completion is too late to raise an error or call cleanup that could throw.
        handle_pending_speculative_xact_event(event);
        break;
    }

    case XACT_EVENT_ABORT:
    case XACT_EVENT_PARALLEL_ABORT: {
        discard_all_relation_write_states(GetCurrentSubTransactionId(), 0);
        release_subtransaction_read_states(GetCurrentSubTransactionId());
        handle_pending_speculative_xact_event(event);
        break;
    }

    case XACT_EVENT_PRE_COMMIT:
    case XACT_EVENT_PARALLEL_PRE_COMMIT:
    case XACT_EVENT_PRE_PREPARE: {
        handle_pending_speculative_xact_event(event);
        flush_all_relation_write_states(GetCurrentSubTransactionId(), 0);
        release_subtransaction_read_states(GetCurrentSubTransactionId());
        break;
    }
    }
}

/** Flush or discard subtransaction writes and update pending speculative inserts. */
void column_subtransaction_callback(SubXactEvent event,
                                    SubTransactionId my_subid,
                                    SubTransactionId parent_subid,
                                    void* arg) {
    switch (event) {
    case SUBXACT_EVENT_START_SUB:
    case SUBXACT_EVENT_COMMIT_SUB: {
        handle_pending_speculative_subxact_event(event, my_subid, parent_subid);
        break;
    }

    case SUBXACT_EVENT_ABORT_SUB: {
        discard_all_relation_write_states(my_subid, parent_subid);
        release_subtransaction_read_states(my_subid);
        handle_pending_speculative_subxact_event(event, my_subid, parent_subid);
        break;
    }

    case SUBXACT_EVENT_PRE_COMMIT_SUB: {
        flush_all_relation_write_states(my_subid, parent_subid);
        release_subtransaction_read_states(my_subid);
        break;
    }
    }
}

/** Flush table rows and all cached sequence-fragment writes. */
void bioseq_flush_pending_writes_adapter(column_write_state* write_state) {
    flush_pending_column_writes(write_state);
    if (write_state->seq_caches != nullptr) {
        bioseq::storage::flush_seq_cache_write_states(
            static_cast<bioseq::storage::seq_caches*>(write_state->seq_caches),
            GetCurrentSubTransactionId());
    }
}

/** Close and destroy a sequence cache passed through the storage hook. */
void bioseq_seq_caches_close_adapter(void* p, bool commit) {
    bioseq::storage::close_and_destroy_seq_caches(static_cast<bioseq::storage::seq_caches*>(p),
                                                  commit);
}

/** Return the number of skipped chunk groups, or zero before the reader starts. */
export int64_t filtered_chunk_group_count(column_scan_handle bioseq_column_scan_desc) {
    column_read_state* read_state = bioseq_column_scan_desc->read_state;

    /* The first row read creates read_state. */
    if (read_state != nullptr) {
        return read_chunk_groups_filtered(read_state);
    } else {
        return 0;
    }
}
