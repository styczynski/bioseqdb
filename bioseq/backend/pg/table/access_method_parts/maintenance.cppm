/**
 * @file
 * @brief Implements relation maintenance, analyze, sizing, and transaction adapters.
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

/** @brief Repeatedly acquire or release a relation lock until the bounded deadline expires. */
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

    /**
     * Cleans old metadata and state only when the physical storage identity changes.
     */
    if (rel->rd_locator.relNumber != newrnode->relNumber) {
        mark_write_state_relation_dropped(rel->rd_locator.relNumber, GetCurrentSubTransactionId());

        delete_metadata_rows(rel->rd_locator);

        /* cascade-drop sub-tables under the OLD
         * relfilenode triple. */
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

    /* we will lazily initialize metadata in first stripe reservation */
}

/** Reinitialize storage for a relation truncated outside transactional rewrite. */
void bioseq_relation_nontransactional_truncate(Relation rel) {
    if (bioseq::storage::is_registered_sequence_fragment_relation(RelationGetRelid(rel))) {
        bioseq_log<logging_level::error, logging_target::postgres>(
            "bioseq storage: TRUNCATE of a registered sequence fragment relation is forbidden");
    }
    RelFileLocator relfilelocator = rel->rd_locator;

    drop_write_state_outside_transaction(relfilelocator.relNumber);
    /* Delete old relfilelocator metadata */
    delete_metadata_rows(relfilelocator);

    /**
     * Reuses the current locator because a transaction-local relation is not visible
     * to other transactions.
     */
    RelationTruncate(rel, 0);

    bioseq_log<logging_level::debug>(
        "[bioseq] pg: bioseq_relation_nontransactional_truncate() initialising columnar storage");
    uint64_t storage_id = allocate_column_storage_id();

    initialize_column_storage(RelationGetSmgr(rel), storage_id);
}

/** Copy every physical fork into a newly allocated relation locator. */
void bioseq_relation_copy_data(Relation rel, const RelFileLocator* newrnode) {
    char persistence = rel->rd_rel->relpersistence;

    /**
     * Flushes source buffers before copying relation forks under the exclusive lock.
     */
    FlushRelationBuffers(rel);

    SMgrRelation srel = RelationCreateStorage(*newrnode, persistence, true);

    /* copy main fork */
    RelationCopyStorage(RelationGetSmgr(rel), srel, MAIN_FORKNUM, persistence);

    for (ForkNumber fork_num = static_cast<ForkNumber>(MAIN_FORKNUM + 1); fork_num <= MAX_FORKNUM;
         fork_num = static_cast<ForkNumber>(static_cast<int>(fork_num) + 1)) {
        if (smgrexists(srel, fork_num)) {
            smgrcreate(srel, fork_num, false);

            /*
             * WAL log creation if the relation is persistent, or
             * this is the init fork of an unlogged relation.
             */
            if (RelationIsPermanent(rel) ||
                (persistence == RELPERSISTENCE_UNLOGGED && fork_num == INIT_FORKNUM))
                log_smgrcreate(newrnode, fork_num);

            RelationCopyStorage(RelationGetSmgr(rel), srel, fork_num, persistence);
        }
    }
}

/**
 * Copy visible rows and metadata during a `VACUUM FULL` rewrite.
 *
 * This callback is used by `VACUUM FULL`, at which
 * we should copy data from OldHeap to NewHeap.
 *
 * In general TableAM case this can also be called for the CLUSTER command
 * which is not applicable for columnar since it doesn't support indexes.
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

    /*
     * copy_table_data in cluster.c assumes tuple descriptors are exactly
     * the same. Even dropped columns exist and are marked as attisdropped
     * in the target relation.
     */
    Assert(source_desc->natts == target_desc->natts);

    /* read settings from old heap, relfilelocator will be swapped at the end */
    column_options bioseq_column_options =
        read_column_options(old_heap->rd_id).value_or(column_options{});

    /**
     * Copies CLUSTER rows through deferred source hydration so old locator stubs
     * resolve against the source storage and are rewritten for the destination.
     */
    column_write_state* write_state = initialize_column_write_state(
        new_heap, target_desc, RelationGetRelid(new_heap), GetCurrentSubTransactionId());
    auto* write_caches = ensure_write_side_seq_caches(new_heap, write_state);
    const auto cap_bytes = write_caches->cap_bytes_snapshot;

    /* we need all columns */
    int natts = old_heap->rd_att->natts;
    bioseq::postgres::support::postgres_bitmapset attr_needed =
        bioseq::postgres::support::postgres_bitmapset::make_member_range(natts);

    /* no quals for table rewrite */
    List* scan_qual = NIL;

    /* Unlike heap tuples, columnar dead-row state lives in an MVCC row-mask
     * relation. SnapshotAny would see both the superseded and current mask and
     * can therefore copy an UPDATE's dead version into the rewritten table.
     * Keep one registered transaction snapshot authoritative for the main row,
     * row mask, and sequence-source fragments throughout the rewrite. */
#if PG_VERSION_NUM >= 190000
    // PG19 supplies the MVCC horizon for concurrent relation rewrites. Keep
    // that same horizon for rows, row masks and sequence fragments.
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

    /* Read-side seq cache: hydrates OldHeap's stubs to real payloads. */
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
        /* Hydrate any SEQ stub columns from OldHeap's sub-tables. */
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

        /* Rewrite SEQ columns into stubs targeting NewHeap's sub-tables.
         * The write-side cache shares the destination write state's lifetime. */
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

    /* cascade-drop OldHeap's sub-tables before the
     * finish_heap_swap. After the swap the relfilenodes are swapped and
     * OldHeap's sub-tables become orphans under the (now-defunct) old
     * triple. The reaper is the long-tail safety net; this call makes
     * the cleanup deterministic. */
    bioseq::storage::bioseq_default_storage_manager::drop_sub_tables_for_main(
        old_heap->rd_locator.dbOid, old_heap->rd_locator.spcOid, old_heap->rd_locator.relNumber);
}

/**
 * Compact eligible tail stripes without changing PostgreSQL visibility semantics.
 *
 * Combines the last eligible stripes so they can fit the
 * maximum number of rows per stripe. Stripes that are going to be combined are deleted
 * and new stripe will be written at the end of untouched last stripe. We also include
 * information about deleted rows for this implementation.
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

    /* Get current columnar options */
    column_options bioseq_column_options =
        read_column_options(rel->rd_id).value_or(column_options{});

    /* Get all stripes in reverse order */
    List* stripe_metadata_list = list_stripes_for_relation(rel->rd_locator, BackwardScanDirection);
    /* Empty table nothing to do */
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

    /*
     * One stripe that is "full" so do nothing.
     */
    if (starting_stripe_list_position == 0) {
        return false;
    }

    /*
     * There is only one stripe that is candidate. Maybe we should vacuum
     * it if condition is met.
     */
    else if (starting_stripe_list_position == 1) {
        /* Maybe we should vacuum only one stripe if count of
         * deleted rows is higher than 20 percent.
         */
        float percentage_of_delete_rows =
            static_cast<float>(last_stripe_deleted_rows) /
            static_cast<float>(total_row_number_count + last_stripe_deleted_rows);

        bool should_vacuum_only_stripe = percentage_of_delete_rows > 0.2f;

        if (!should_vacuum_only_stripe) {
            return false;
        }
    }

    /**
     * Clears PROC_IN_VACUUM while writing metadata so concurrent xmin calculation
     * does not classify the new catalog tuples as dead.
     */

    LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
    MyProc->statusFlags = 0;
    ProcGlobal->statusFlags[MyProc->pgxactoff] = MyProc->statusFlags;

    LWLockRelease(ProcArrayLock);

    /* We need to re-assing RecentXmin here */
    PushActiveSnapshot(GetTransactionSnapshot());
    column_write_state* write_state =
        begin_column_write(rel->rd_locator, bioseq_column_options, tuple_desc);

    /* we need all columns */
    int natts = rel->rd_att->natts;
    bioseq::postgres::support::postgres_bitmapset attr_needed =
        bioseq::postgres::support::postgres_bitmapset::make_member_range(natts);

    /* no quals for table rewrite */
    List* scan_qual = NIL;

    MemoryContext scan_context = create_column_scan_memory_context();
    bool random_access = true;
    // Row-mask metadata is MVCC: SnapshotAny can pick a superseded mask and
    // resurrect deleted/updated rows during compaction. The active snapshot
    // was pushed above and remains owned until this reader is closed.
    column_read_state* read_state = init_bioseq_read_state(
        rel, tuple_desc, attr_needed, scan_qual, scan_context,
        GetActiveSnapshot(), random_access, nullptr);

    set_stripe_read_state(read_state,
                          static_cast<stripe_metadata*>(
                              list_nth(stripe_metadata_list, starting_stripe_list_position - 1)));

    Datum* values = bioseq::postgres::support::allocate_zeroed_array<Datum>(tuple_desc->natts);
    bool* nulls = bioseq::postgres::support::allocate_zeroed_array<bool>(tuple_desc->natts);

    /* we don't need to know row_number here */
    while (read_next_column_row(read_state, values, nulls, nullptr)) {
        write_column_row(write_state, values, nulls);
    }

    uint64_t new_data_reservation;

    if (list_length(stripe_metadata_list) > starting_stripe_list_position) {
        stripe_metadata* boundary_stripe = static_cast<stripe_metadata*>(
            list_nth(stripe_metadata_list, starting_stripe_list_position));
        new_data_reservation = boundary_stripe->file_offset + boundary_stripe->data_length - 1;
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

/** Run Bioseq's non-FULL VACUUM bookkeeping and optional tail truncation. */
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

            /*
             * If metapage version of relation is older, then we hint users to VACUUM
             * the relation in validate_column_metapage_version. So if needed, upgrade
             * the metapage before doing anything.
             */
            const bool upgrades_storage = true;
            ensure_current_column_storage_format(rel, upgrades_storage);

            int elevel = (params->options & VACOPT_VERBOSE) ? INFO : DEBUG2;

            /* this should have been resolved by vacuum.c until now */
            Assert(params->truncate != VACOPTVALUE_UNSPECIFIED);

            if (params->options & VACOPT_VERBOSE) {
                log_relation_statistics(rel, elevel);
            }

            /*
             * We don't have updates, deletes, or concurrent updates, so all we
             * care for now is truncating the unused space at the end of storage.
             */
            if (params->truncate == VACOPTVALUE_ENABLED) {
                truncate_column_storage_tail(rel, elevel);
            }

            BlockNumber new_rel_pages = smgrnblocks(RelationGetSmgr(rel), MAIN_FORKNUM);

            /* get the number of indexes */
            List* index_list = RelationGetIndexList(rel);
            int nindexes = list_length(index_list);

            VacuumCutoffs cutoffs;
            vacuum_get_cutoffs(rel, params, &cutoffs);

            Assert(MultiXactIdPrecedesOrEquals(cutoffs.MultiXactCutoff, cutoffs.OldestMxact));
            Assert(TransactionIdPrecedesOrEquals(cutoffs.FreezeLimit, cutoffs.OldestXmin));

            /*
             * BioseqColumn storage doesn't hold any transaction IDs, so we can always
             * just advance to the most aggressive value.
             */
            TransactionId new_rel_frozen_xid = cutoffs.OldestXmin;
            MultiXactId new_relmin_mxid = cutoffs.OldestMxact;
            double new_live_tuples = column_table_tuple_count(rel);

            /* all visible pages are always 0 */
            BlockNumber new_rel_allvisible = 0;

            bool frozenxid_updated;
            bool minmulti_updated;

            /**
             * PostgreSQL 18 requires a frozen-page count, which column storage reports as zero.
             */
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

            /**
             * PostgreSQL 18 requires a start time (zero means unknown); PG19
             * takes the open relation rather than its OID/shared flag.
             */
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

                    /* ignore zero length chunks for dropped attributes */
                    if (skipnode->value_stream_length > 0) {
                        compression_stats[static_cast<int>(
                            skipnode->value_compression_algorithm)]++;
                        chunk_count++;

                        if (attr_dropped) {
                            dropped_chunks_with_data++;
                        }
                    }

                    /*
                     * We don't compress exists buffer, so its compressed & decompressed
                     * lengths are the same.
                     */
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

        /* skip if this compression algorithm has not been compiled */
        if (compression_name == nullptr) {
            continue;
        }

        /* skip if no chunks use this compression type */
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

/**
 * Truncate unused space at the end of a Bioseq main fork.
 *
 * Unused space can be created by aborted transactions. The lock policy avoids
 * blocking or deadlocking against callers that already hold weaker locks.
 *
 * The implementation keeps the row identities of every retained stripe.
 */
static void truncate_column_storage_tail(Relation rel, int elevel) {
    PGRUsage ru0;

    pg_rusage_init(&ru0);

    /* Report that we are now truncating */
    pgstat_progress_update_param(PROGRESS_VACUUM_PHASE, PROGRESS_VACUUM_PHASE_TRUNCATE);

    /**
     * Acquires AccessExclusiveLock conditionally so truncation cannot block or
     * deadlock behind the weaker lock already held by the caller.
     */
    if (!try_lock_relation_until_timeout(
            rel,
            AccessExclusiveLock,
            bioseq::build_config::bioseq_vacuum_truncate_lock_timeout_milliseconds,
            bioseq::build_config::bioseq_vacuum_truncate_lock_wait_interval_milliseconds,
            false)) {
        /*
         * We failed to establish the lock in the specified number of
         * retries. This means we give up truncating.
         */
        bioseq_log_pg(elevel,
                      "\"{}\": stopping truncate due to conflicting lock request",
                      RelationGetRelationName(rel));
        return;
    }

    const bool locator_pinned_fragment =
        bioseq::storage::is_registered_sequence_fragment_relation(RelationGetRelid(rel));
    // Sequence locators persist exact physical row numbers.  The ordinary
    // columnar compactor rewrites surviving tuples at fresh row numbers, so it
    // is never legal for a registered fragment relation.  Physical tail
    // truncation below remains allowed because it preserves every live stripe,
    // row number, and reserved-row-number watermark.
    bool stripes_truncated =
        locator_pinned_fragment ? false : truncate_and_compact_column_stripes(rel, elevel);
    if (locator_pinned_fragment) {
        bioseq_log_pg(
            elevel,
            "\"{}\": skipping row-moving compaction for locator-pinned sequence fragments",
            RelationGetRelationName(rel));
    }

    /*
     * If we didn't truncate and combine tail stripes we could have
     * stituation where we need to truncate storage at the end.
     */
    if (!stripes_truncated) {

        /*
         * Due to the AccessExclusive lock there's no danger that
         * new stripes be added beyond highestPhysicalAddress while
         * we're truncating.
         */
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

    /**
     * Releases the exclusive lock after smgr truncation has published invalidation.
     */
    UnlockRelation(rel, AccessExclusiveLock);
}

/**
 * Acquire a relation lock with bounded conditional retries.
 *
 * Tries until
 * it either succeeds or timesout. It doesn't enter wait queue and instead it
 * sleeps between lock tries.
 *
 */
static bool try_lock_relation_until_timeout(
    Relation rel, LOCKMODE lock_mode, int timeout, int retry_interval, bool acquire) {
    int lock_retry = 0;

    while (true) {
        if (ConditionalLockRelation(rel, lock_mode)) {
            break;
        }

        /*
         * Check for interrupts while trying to (re-)acquire the lock
         */
        CHECK_FOR_INTERRUPTS();

        if (!acquire && (++lock_retry > (timeout / retry_interval))) {
            return false;
        }

        pg_usleep(retry_interval * 1000L);
    }

    return true;
}

/** Present the page-independent relation as one logical ANALYZE block. */
bool bioseq_scan_analyze_next_block(TableScanDesc scan, ReadStream* stream) {
    /*
     * Our access method is not pages based, i.e. tuples are not confined
     * to page boundaries. Present the relation as one logical block so
     * acquire_sample_rows() scans it once, then terminates.
     */
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

    /**
     * Streams every visible row because the page-oriented sampler cannot select
     * representative pages for column storage.
     */
    if (bioseq_getnextslot(scan, ForwardScanDirection, slot)) {
        (*liverows)++;
        return true;
    }

    return false;
}

/** Return bytes used by one relation fork or all forks. */
uint64 bioseq_relation_size(Relation rel, ForkNumber fork_number) {
    uint64 nblocks = 0;

    /* InvalidForkNumber indicates returning the size for all forks */
    if (fork_number == InvalidForkNumber) {
        for (int fork_index = 0; fork_index < MAX_FORKNUM; fork_index++) {
            nblocks += smgrnblocks(RelationGetSmgr(rel), static_cast<ForkNumber>(fork_index));
        }
    } else {
        nblocks = smgrnblocks(RelationGetSmgr(rel), fork_number);
    }

    return nblocks * BLCKSZ;
}

/** Report that Bioseq relations store variable-width values outside heap TOAST. */
bool bioseq_relation_needs_toast_table(Relation rel) {
    return false;
}

/** Populate planner row, page, width, and visibility estimates. */
void bioseq_estimate_rel_size(
    Relation rel, int32_t* attr_widths, BlockNumber* pages, double* tuples, double* allvisfrac) {
    *pages = smgrnblocks(RelationGetSmgr(rel), MAIN_FORKNUM);
    *tuples = column_table_row_count(rel);

    /*
     * Append-only, so everything is visible except in-progress or rolled-back
     * transactions.
     */
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

/** Flush, discard, or quarantine table state at PostgreSQL transaction boundaries. */
void column_transaction_callback(XactEvent event, void* arg) {
    switch (event) {
    case XACT_EVENT_COMMIT:
    case XACT_EVENT_PARALLEL_COMMIT:
    case XACT_EVENT_PREPARE: {
        // PRE_COMMIT/PRE_PREPARE below must have proved this empty. A
        // surviving move-only authority here is an internal invariant breach;
        // quarantine it without a PostgreSQL jump or destructor-bearing
        // callback frame after the terminal transaction boundary.
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

/** Reparent, flush, or discard table state at subtransaction boundaries. */
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

/** Flush parent-column writes and every cached sequence-fragment shard. */
void bioseq_flush_pending_writes_adapter(column_write_state* write_state) {
    flush_pending_column_writes(write_state);
    if (write_state->seq_caches != nullptr) {
        bioseq::storage::flush_seq_cache_write_states(
            static_cast<bioseq::storage::seq_caches*>(write_state->seq_caches),
            GetCurrentSubTransactionId());
    }
}

/** Close and destroy an opaque sequence-cache owner at a storage boundary. */
void bioseq_seq_caches_close_adapter(void* p, bool commit) {
    bioseq::storage::close_and_destroy_seq_caches(static_cast<bioseq::storage::seq_caches*>(p),
                                                  commit);
}

/** Return the number of chunk groups pruned by an initialized scan. */
export int64_t filtered_chunk_group_count(column_scan_handle bioseq_column_scan_desc) {
    column_read_state* read_state = bioseq_column_scan_desc->read_state;

    /* read_state is initialized lazily */
    if (read_state != nullptr) {
        return read_chunk_groups_filtered(read_state);
    } else {
        return 0;
    }
}
