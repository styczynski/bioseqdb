/**
 * @file
 * @brief Implements scan, exact-row fetch, and parallel-scan TableAM callbacks.
 */
module;

extern "C" {
#include "postgres.h"

#include "access/skey.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "executor/tuptable.h"
#include "nodes/bitmapset.h"
#include "nodes/nodes.h"
#include "nodes/pg_list.h"
#include "storage/itemptr.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
}

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <new>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.postgres.table:access_method_scan_callbacks;

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
import :access_callback_state;
import :access_identity;
import :access_common;
import :access_hooks;

static_assert(!std::is_trivially_destructible_v<bioseq::postgres::support::memory_context_scope>);

/** Begin a table scan with explicit projection and executor modes. */
/** @brief Begin a column scan with explicit projection, qualifier, and vectorization state. */
export TableScanDesc bioseq_beginscan_extended(Relation relation,
                                               Snapshot snapshot,
                                               int key_count,
                                               ScanKey key,
                                               ParallelTableScanDesc parallel_table_scan,
                                               uint32_t flags,
                                               Bitmapset* attributes_needed,
                                               List* scan_qualifiers,
                                               parallel_column_scan_handle parallel_column_scan,
                                               bool returns_vectorized_tuples);

/** Begin a scan with an explicit sequence-hydration policy. */
TableScanDesc bioseq_beginscan_with_options(Relation relation,
                                            Snapshot snapshot,
                                            int key_count,
                                            ScanKey key,
                                            ParallelTableScanDesc parallel_table_scan,
                                            uint32_t flags,
                                            Bitmapset* attributes_needed,
                                            List* scan_qualifiers,
                                            parallel_column_scan_handle parallel_column_scan,
                                            bool returns_vectorized_tuples,
                                            bool defer_sequence_hydration);

/** Begin a regular table-AM scan that projects every live attribute. */
TableScanDesc bioseq_beginscan(Relation relation,
                               Snapshot snapshot,
                               int key_count,
                               ScanKey key,
                               ParallelTableScanDesc parallel_table_scan,
                               uint32_t flags) {
    previous_cache_enabled_state = bioseq::settings::column_cache::enabled;

    int natts = relation->rd_att->natts;

    /* attributes_needed represents 0-indexed attribute numbers */
    bioseq::postgres::support::postgres_bitmapset attributes_needed =
        bioseq::postgres::support::postgres_bitmapset::make_member_range(natts);

    TableScanDesc scandesc = bioseq_beginscan_extended(relation,
                                                       snapshot,
                                                       key_count,
                                                       key,
                                                       parallel_table_scan,
                                                       flags,
                                                       attributes_needed.get(),
                                                       nullptr,
                                                       nullptr,
                                                       false);

    attributes_needed.clear();

    return scandesc;
}

export TableScanDesc bioseq_beginscan_extended(Relation relation,
                                               Snapshot snapshot,
                                               int key_count,
                                               ScanKey key,
                                               ParallelTableScanDesc parallel_table_scan,
                                               uint32_t flags,
                                               Bitmapset* attributes_needed,
                                               List* scan_qualifiers,
                                               parallel_column_scan_handle parallel_column_scan,
                                               bool returns_vectorized_tuples) {
    return bioseq_beginscan_with_options(relation,
                                         snapshot,
                                         key_count,
                                         key,
                                         parallel_table_scan,
                                         flags,
                                         attributes_needed,
                                         scan_qualifiers,
                                         parallel_column_scan,
                                         returns_vectorized_tuples,
                                         /*defer_sequence_hydration=*/false);
}

/**
 * Begin a custom scan that defers sequence locator hydration.
 *
 * This has the same
 * signature as bioseq_beginscan_extended but turns the deferred-hydration
 * flag on, so bioseq_getnextslot returns locator stubs verbatim and the
 * dispatch buffer hydrates them in per-bucket bulk. `export`ed
 * so :custom_scan reaches it via `import :access_method` instead of an
 * `extern "C++"` forward decl.
 */
export TableScanDesc bioseq_beginscan_deferred(Relation relation,
                                               Snapshot snapshot,
                                               int key_count,
                                               ScanKey key,
                                               ParallelTableScanDesc parallel_table_scan,
                                               uint32_t flags,
                                               Bitmapset* attributes_needed,
                                               List* scan_qualifiers,
                                               parallel_column_scan_handle parallel_column_scan,
                                               bool returns_vectorized_tuples) {
    return bioseq_beginscan_with_options(relation,
                                         snapshot,
                                         key_count,
                                         key,
                                         parallel_table_scan,
                                         flags,
                                         attributes_needed,
                                         scan_qualifiers,
                                         parallel_column_scan,
                                         returns_vectorized_tuples,
                                         /*defer_sequence_hydration=*/true);
}

/** C ABI entry point for a fully hydrated projected scan. */
export extern "C" TableScanDesc
bioseq_beginscan_extended_c(Relation relation,
                            Snapshot snapshot,
                            int key_count,
                            ScanKey key,
                            ParallelTableScanDesc parallel_table_scan,
                            uint32_t flags,
                            Bitmapset* attributes_needed,
                            List* scan_qualifiers,
                            parallel_column_scan_handle parallel_column_scan,
                            bool returns_vectorized_tuples) {
    return bioseq_beginscan_with_options(relation,
                                         snapshot,
                                         key_count,
                                         key,
                                         parallel_table_scan,
                                         flags,
                                         attributes_needed,
                                         scan_qualifiers,
                                         parallel_column_scan,
                                         returns_vectorized_tuples,
                                         /*defer_sequence_hydration=*/false);
}

/** C ABI entry point for a projected scan with deferred hydration. */
export extern "C" TableScanDesc
bioseq_beginscan_deferred_c(Relation relation,
                            Snapshot snapshot,
                            int key_count,
                            ScanKey key,
                            ParallelTableScanDesc parallel_table_scan,
                            uint32_t flags,
                            Bitmapset* attributes_needed,
                            List* scan_qualifiers,
                            parallel_column_scan_handle parallel_column_scan,
                            bool returns_vectorized_tuples) {
    return bioseq_beginscan_with_options(relation,
                                         snapshot,
                                         key_count,
                                         key,
                                         parallel_table_scan,
                                         flags,
                                         attributes_needed,
                                         scan_qualifiers,
                                         parallel_column_scan,
                                         returns_vectorized_tuples,
                                         /*defer_sequence_hydration=*/true);
}

/** Allocate and initialize the common column scan descriptor. */
TableScanDesc bioseq_beginscan_with_options(Relation relation,
                                            Snapshot snapshot,
                                            int key_count,
                                            ScanKey key,
                                            ParallelTableScanDesc parallel_table_scan,
                                            uint32_t flags,
                                            Bitmapset* attributes_needed,
                                            List* scan_qualifiers,
                                            parallel_column_scan_handle parallel_column_scan,
                                            bool returns_vectorized_tuples,
                                            bool defer_sequence_hydration) {
    previous_cache_enabled_state = bioseq::settings::column_cache::enabled;
    Oid relfilelocator = relation->rd_locator.relNumber;
    /*
     * A memory context to use for scan-wide data, including the lazily
     * initialized read state. We assume that beginscan is called in a
     * context that will last until end of scan.
     */
    MemoryContext scan_context = create_column_scan_memory_context();
    column_scan_handle scan;
    {
        bioseq::postgres::support::memory_context_scope ctx_guard{scan_context};

        scan = ::new (palloc0(sizeof(column_scan_descriptor))) column_scan_descriptor{};
        scan->scan_base.rs_rd = relation;
        scan->scan_base.rs_snapshot = snapshot;
        scan->scan_base.rs_nkeys = key_count;
        scan->scan_base.rs_key = key;
        scan->scan_base.rs_flags = flags;
        scan->scan_base.rs_parallel = parallel_table_scan;

        /**
         * Defers reader initialization until the executor slot supplies the descriptor
         * that matches storage during ALTER COLUMN rewrites.
         */
        scan->read_state = nullptr;
        scan->attributes_needed =
            bioseq::postgres::support::postgres_bitmapset{attributes_needed}.clone();
        scan->scan_qualifiers =
            bioseq::postgres::support::postgres_list<Node>{copyObject(scan_qualifiers)};
        scan->scan_context = scan_context;

        /* Parallel execution scan data */;
        scan->parallel_scan = parallel_column_scan;

        /* Vectorized result */
        scan->returns_vectorized_tuple = returns_vectorized_tuples;

        /* per-scan storage cache and deferred-hydration flag. */
        scan->defers_sequence_hydration = defer_sequence_hydration;
        auto* caches = bioseq::storage::alloc_seq_caches(scan_context);
        caches->side = bioseq::storage::seq_caches::access_side::read;
        caches->lock_mode = AccessShareLock;
        caches->read_state_mctx = scan_context;
        caches->main_database_oid = relation->rd_locator.dbOid;
        caches->main_tablespace_oid = relation->rd_locator.spcOid;
        caches->main_relfilenode = relation->rd_locator.relNumber;
        scan->sequence_cache = caches;

        if (writes_pending_in_parent_transactions(relfilelocator, GetCurrentSubTransactionId())) {
            bioseq_log<logging_level::error, logging_target::postgres>(
                "cannot read from table when there is unflushed data in upper transactions");
        }
    }

    return reinterpret_cast<TableScanDesc>(scan);
}

/** End a table scan and release its reader, caches, and memory context. */
void bioseq_endscan(TableScanDesc sscan) {
    column_scan_handle scan = reinterpret_cast<column_scan_handle>(sscan);

    /* close per-scan seq_cache (releases relation pins,
     * tears down any cached BioseqColumnReadStates on sub-tables). */
    if (scan->sequence_cache != nullptr) {
        auto* caches = static_cast<bioseq::storage::seq_caches*>(scan->sequence_cache);
        caches->close_all(/*commit=*/true);
        bioseq::storage::destroy_seq_caches(caches);
        scan->sequence_cache = nullptr;
    }

    if (scan->read_state != nullptr) {
        end_column_read(scan->read_state);
        scan->read_state = nullptr;
    }

    if (scan->scan_base.rs_flags & SO_TEMP_SNAPSHOT) {
        UnregisterSnapshot(scan->scan_base.rs_snapshot);
    }

    /* clean up any caches. */
    if (bioseq::settings::column_cache::enabled == true) {
        reset_column_cache();
    }

    MemoryContextDelete(scan->scan_context);

    bioseq::settings::column_cache::enabled = previous_cache_enabled_state;
}

/** Reinitialize a table scan with new keys and visibility parameters. */
void bioseq_rescan(TableScanDesc sscan,
                   ScanKey key,
                   bool set_params,
                   bool allow_strat,
                   bool allow_sync,
                   bool allow_pagemode) {
    column_scan_handle scan = reinterpret_cast<column_scan_handle>(sscan);

    /* Adapts the scan-key pointer to the column reader's qualifier list. */
    List* scan_qual = reinterpret_cast<List*>(key);

    if (scan->read_state != nullptr) {
        rescan_column_reader(scan->read_state, scan_qual);
    }
}

/** Read the next visible row and hydrate sequence values unless explicitly deferred. */
static bool
bioseq_getnextslot_impl(TableScanDesc sscan, ScanDirection direction, TupleTableSlot* slot) {
    column_scan_handle scan = reinterpret_cast<column_scan_handle>(sscan);

    /*
     * if this is the first row, initialize read state.
     */
    if (scan->read_state == nullptr) {
        bool random_access = false;
        scan->read_state = init_bioseq_read_state(scan->scan_base.rs_rd,
                                                  slot->tts_tupleDescriptor,
                                                  scan->attributes_needed,
                                                  scan->scan_qualifiers.get(),
                                                  scan->scan_context,
                                                  scan->scan_base.rs_snapshot,
                                                  random_access,
                                                  scan->parallel_scan);
    }

    ExecClearTuple(slot);

    if (scan->returns_vectorized_tuple) {
        auto* vector_slot = reinterpret_cast<vector_tuple_table_slot_storage*>(slot);

        int new_vector_size = 0;

        bool next_row_found = read_next_column_vector(scan->read_state,
                                                      vector_slot->virtual_slot.base.tts_values,
                                                      vector_slot->virtual_slot.base.tts_isnull,
                                                      vector_slot->source_row_numbers.data(),
                                                      &new_vector_size);

        if (!next_row_found)
            return false;

        vector_slot->row_count = new_vector_size;

        memset(vector_slot->row_is_active.data(), true, new_vector_size);

        ExecStoreVirtualTuple(slot);
    } else {
        uint64_t row_number;
        bool next_row_found =
            read_next_column_row(scan->read_state, slot->tts_values, slot->tts_isnull, &row_number);

        if (!next_row_found) {
            return false;
        }

        ExecStoreVirtualTuple(slot);
        slot->tts_tid = row_number_to_tid(row_number);

        /*
         * hydrate every SEQ column unless the caller
         * asked for deferred hydration (custom scan / dispatch buffer
         * path, which performs per-bucket bulk hydration later).
         */
        if (!scan->defers_sequence_hydration) {
            const auto& amc = bioseq::storage::get_table_access_cache(scan->scan_base.rs_rd);
            auto* caches = static_cast<bioseq::storage::seq_caches*>(scan->sequence_cache);
            hydrate_sequence_columns_in_slot(scan->scan_base.rs_rd,
                                             slot,
                                             caches,
                                             amc,
                                             scan->read_state->snapshot,
                                             scan->scan_context);
        }
    }

    return true;
}

/** Reject unsupported core parallel-scan estimation through PostgreSQL ERROR. */
Size bioseq_parallelscan_estimate(Relation rel) {
    bioseq_log<logging_level::error, logging_target::postgres>(
        "bioseq_parallelscan_estimate not implemented");
}

/** Reject unsupported core parallel-scan initialization through PostgreSQL ERROR. */
Size bioseq_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan) {
    bioseq_log<logging_level::error, logging_target::postgres>(
        "bioseq_parallelscan_initialize not implemented");
}

/** Reject unsupported core parallel-scan reinitialization through PostgreSQL ERROR. */
void bioseq_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan) {
    bioseq_log<logging_level::error, logging_target::postgres>(
        "bioseq_parallelscan_reinitialize not implemented");
}

/** Allocate exact-row sequence caches in the fetch memory context. */
[[gnu::always_inline]]
static inline void
init_index_fetch_seq_cache(index_fetch_state* scan, Relation rel, MemoryContext scan_context) {
    auto* caches = bioseq::storage::alloc_seq_caches(scan_context);
    caches->side = bioseq::storage::seq_caches::access_side::read;
    caches->lock_mode = AccessShareLock;
    caches->read_state_mctx = scan_context;
    caches->main_database_oid = rel->rd_locator.dbOid;
    caches->main_tablespace_oid = rel->rd_locator.spcOid;
    caches->main_relfilenode = rel->rd_locator.relNumber;
    scan->sequence_cache = caches;
    scan->defers_sequence_hydration = false;
}

/** Begin an index fetch before the executor supplies projected attributes. */
IndexFetchTableData* bioseq_index_fetch_begin(Relation rel
#if PG_VERSION_NUM >= 190000
                                             , std::uint32_t flags
#endif
) {
    Oid relfilelocator = rel->rd_locator.relNumber;
    if (writes_pending_in_parent_transactions(relfilelocator, GetCurrentSubTransactionId())) {
        /* Rejects index fetches while a parent transaction has pending writes. */
        bioseq_log<logging_level::error, logging_target::postgres>(
            "cannot read from index when there is unflushed data in "
            "upper transactions");
    }

    MemoryContext scan_context = create_column_scan_memory_context();
    index_fetch_state* scan;
    {
        bioseq::postgres::support::memory_context_scope ctx_guard{scan_context};

        scan = ::new (palloc0(sizeof(index_fetch_state))) index_fetch_state{};
        scan->fetch_base.rel = rel;
#if PG_VERSION_NUM >= 190000
        scan->fetch_base.flags = flags;
#endif
        scan->read_state = nullptr;
        scan->stripe_metadata = {};
        scan->scan_context = scan_context;
        scan->is_select_query = false;
        init_index_fetch_seq_cache(scan, rel, scan_context);
    }

    return &scan->fetch_base;
}

/** Begin an index fetch with an explicit projected-attribute bitmap. */
export IndexFetchTableData* bioseq_index_fetch_begin_extended(Relation rel,
                                                              Bitmapset* attr_needed) {
    Oid relfilenode = rel->rd_locator.relNumber;
    if (writes_pending_in_parent_transactions(relfilenode, GetCurrentSubTransactionId())) {
        /* Rejects index fetches while a parent transaction has pending writes. */
        bioseq_log<logging_level::error, logging_target::postgres>(
            "cannot read from index when there is unflushed data in "
            "upper transactions");
    }

    MemoryContext scan_context = create_column_scan_memory_context();
    index_fetch_state* scan;
    {
        bioseq::postgres::support::memory_context_scope ctx_guard{scan_context};

        scan = ::new (palloc0(sizeof(index_fetch_state))) index_fetch_state{};
        scan->fetch_base.rel = rel;
        scan->read_state = nullptr;
        scan->stripe_metadata = {};
        scan->scan_context = scan_context;

        scan->attributes_needed =
            bioseq::postgres::support::postgres_bitmapset{attr_needed}.clone();
        scan->is_select_query = true;
        init_index_fetch_seq_cache(scan, rel, scan_context);
    }

    return &scan->fetch_base;
}

/** Reset an index fetch whose state is recreated lazily on the next tuple. */
void bioseq_index_fetch_reset(IndexFetchTableData* sscan) {
    /* no-op */
}

/** End an index fetch and release its reader, caches, and memory context. */
void bioseq_index_fetch_end(IndexFetchTableData* sscan) {
    bioseq_index_fetch_reset(sscan);

    index_fetch_state* scan = reinterpret_cast<index_fetch_state*>(sscan);

    /* close per-fetch seq_cache. */
    if (scan->sequence_cache != nullptr) {
        auto* caches = static_cast<bioseq::storage::seq_caches*>(scan->sequence_cache);
        caches->close_all(/*commit=*/true);
        bioseq::storage::destroy_seq_caches(caches);
        scan->sequence_cache = nullptr;
    }

    if (scan->read_state) {
        end_column_read(scan->read_state);
        scan->read_state = nullptr;
    }

    scan->attributes_needed.clear();

    /* clean up any caches. */
    if (bioseq::settings::column_cache::enabled == true) {
        reset_column_cache();
    }

    MemoryContextDelete(scan->scan_context);
}

/** Locate a cached stripe by logical row number using binary search. */
static stripe_metadata* find_stripe_metadata_by_row_number(index_fetch_state* scan,
                                                           uint64_t row_number) {
    ListCell* lc = nullptr;

    int high = scan->stripe_metadata.size() - 1;
    int low = 0;

    while (low <= high) {
        int mid = low + (high - low) / 2;

        lc = list_nth_cell(scan->stripe_metadata.get(), mid);

        stripe_metadata* stripe_metadata_entry = static_cast<stripe_metadata*>(lc->ptr_value);

        if (row_number >= stripe_metadata_entry->first_row_number &&
            row_number <
                stripe_metadata_entry->first_row_number + stripe_metadata_entry->row_count) {
            return stripe_metadata_entry;
        }

        if (stripe_metadata_entry->first_row_number > row_number) {
            high = mid - 1;
        } else {
            low = mid + 1;
        }
    }
    return nullptr;
}

/** Fetch an index-selected row after MVCC and row-mask validation. */
bool bioseq_index_fetch_tuple(IndexFetchTableData* sscan,
                              ItemPointer tid,
                              Snapshot snapshot,
                              TupleTableSlot* slot,
                              bool* call_again,
                              bool* all_dead) {
    /* no HOT chains are possible in columnar, directly set it to false */
    *call_again = false;

    /**
     * Never reports all_dead until simple virtual-TID index deletion is implemented.
     */
    if (all_dead) {
        *all_dead = false;
    }

    ExecClearTuple(slot);

    index_fetch_state* scan = reinterpret_cast<index_fetch_state*>(sscan);
    Relation bioseq_column_relation = scan->fetch_base.rel;

    /* initialize read state for the first row */
    if (scan->read_state == nullptr) {
        /* no quals for index scan */
        List* scan_qual = NIL;

        if (scan->attributes_needed.is_empty()) {
            /* we need all columns */
            int natts = bioseq_column_relation->rd_att->natts;
            scan->attributes_needed.clear();
            scan->attributes_needed =
                bioseq::postgres::support::postgres_bitmapset::make_member_range(natts);
        }

        bool random_access = true;
        scan->read_state = init_bioseq_read_state(bioseq_column_relation,
                                                  slot->tts_tupleDescriptor,
                                                  scan->attributes_needed,
                                                  scan_qual,
                                                  scan->scan_context,
                                                  snapshot,
                                                  random_access,
                                                  nullptr);
        if (scan->is_select_query)
            scan->stripe_metadata =
                bioseq::postgres::support::postgres_list<stripe_metadata>{list_stripes_for_relation(
                    bioseq_column_relation->rd_locator, ForwardScanDirection)};
    }

    uint64_t row_number = tid_to_row_number(*tid);

    stripe_metadata* stripe_metadata_entry = nullptr;

    if (scan->is_select_query)
        stripe_metadata_entry = find_stripe_metadata_by_row_number(scan, row_number);
    else
        stripe_metadata_entry =
            find_stripe_by_first_row_number(bioseq_column_relation, row_number, snapshot);

    if (!stripe_metadata_entry) {
        /* it is certain that tuple with row_number doesn't exist */
        return false;
    }

    stripe_write_state stripe_write_state_value = stripe_write_state_for(stripe_metadata_entry);

    if (stripe_write_state_value == stripe_write_state::flushed &&
        !read_column_row_by_number(
            scan->read_state, row_number, slot->tts_values, slot->tts_isnull)) {
        /*
         * find_stripe_by_first_row_number doesn't verify upper row
         * number boundary of found stripe. For this reason, we didn't
         * certainly know if given row number belongs to one of the stripes.
         */
        return false;
    } else if (stripe_write_state_value == stripe_write_state::aborted) {
        /*
         * We only expect to see un-flushed stripes when checking against
         * constraint violation. In that case, indexAM provides dirty
         * snapshot to index_fetch_tuple callback.
         */
        pfree(stripe_metadata_entry);
        Assert(snapshot->snapshot_type == SNAPSHOT_DIRTY);
        return false;
    } else if (stripe_write_state_value == stripe_write_state::in_progress) {
        if (stripe_metadata_entry->inserted_by_current_transaction) {
            /**
             * Flushes a current-transaction stripe before retrying the row lookup.
             */
            flush_pending_writes_for_read(scan->read_state);

            /*
             * Fill the tupleslot and fall through to return true, it
             * certainly exists.
             */
            read_column_row_by_number_or_error(
                scan->read_state, row_number, slot->tts_values, slot->tts_isnull);
        } else {
            /* similar to aborted writes, it should be dirty snapshot */
            Assert(snapshot->snapshot_type == SNAPSHOT_DIRTY);

            /**
             * Returns an all-null slot for an unflushed foreign-transaction stripe.
             *
             * Unique btree checks only need the row identity while they wait for the writer
             * transaction. Other supported constraints receive flushed single-row stripes.
             */
            memset(slot->tts_isnull, true, slot->tts_nvalid * sizeof(bool));
        }
    } else {
        /*
         * At this point, we certainly know that stripe is flushed and
         * read_column_row_by_number successfully filled the tupleslot.
         */
        Assert(stripe_write_state_value == stripe_write_state::flushed);
    }

    if (!scan->is_select_query)
        pfree(stripe_metadata_entry);
    slot->tts_tableOid = RelationGetRelid(bioseq_column_relation);
    slot->tts_tid = *tid;
    ExecStoreVirtualTuple(slot);

    /**
     * Hydrates sequence locators unless a bulk custom scan explicitly deferred it.
     */
    if (!scan->defers_sequence_hydration) {
        const auto& amc = bioseq::storage::get_table_access_cache(bioseq_column_relation);
        auto* caches = static_cast<bioseq::storage::seq_caches*>(scan->sequence_cache);
        hydrate_sequence_columns_in_slot(bioseq_column_relation,
                                         slot,
                                         caches,
                                         amc,
                                         scan->read_state->snapshot,
                                         scan->scan_context);
    }

    return true;
}

/** Fetch an exact visible row version into a caller-provided slot. */
bool bioseq_fetch_row_version(Relation relation,
                              ItemPointer tid,
                              Snapshot snapshot,
                              TupleTableSlot* slot) {
    uint64_t row_number = tid_to_row_number(*tid);
    column_read_state** read_state =
        find_column_read_state_slot(relation, GetCurrentSubTransactionId());

    if (read_state == nullptr) {
        read_state = acquire_column_read_state_slot(relation, GetCurrentSubTransactionId());

        int natts = relation->rd_att->natts;
        bioseq::postgres::support::postgres_bitmapset attr_needed =
            bioseq::postgres::support::postgres_bitmapset::make_member_range(natts);

        List* scan_qual = NIL;

        bool random_access = false;

        *read_state = init_bioseq_read_state(relation,
                                             slot->tts_tupleDescriptor,
                                             attr_needed,
                                             scan_qual,
                                             column_read_state_memory_context(),
                                             snapshot,
                                             random_access,
                                             nullptr);
    }

    {
        bioseq::postgres::support::memory_context_scope ctx_guard{
            column_read_state_memory_context()};
        read_column_row_by_number(*read_state, row_number, slot->tts_values, slot->tts_isnull);
    }

    slot->tts_tableOid = RelationGetRelid(relation);
    slot->tts_tid = *tid;

    if (TTS_EMPTY(slot))
        ExecStoreVirtualTuple(slot);

    /* hydrate any SEQ stubs in the slot. We do NOT
     * cache a per-fetch seq_caches here because the row-version fetch
     * is one-shot; allocate, use, destroy. */
    {
        const auto& amc = bioseq::storage::get_table_access_cache(relation);
        if (!amc.sequence_attribute_numbers.empty()) {
            auto* caches = bioseq::storage::alloc_seq_caches(column_read_state_memory_context());
            caches->side = bioseq::storage::seq_caches::access_side::read;
            caches->lock_mode = AccessShareLock;
            caches->read_state_mctx = column_read_state_memory_context();
            caches->main_database_oid = relation->rd_locator.dbOid;
            caches->main_tablespace_oid = relation->rd_locator.spcOid;
            caches->main_relfilenode = relation->rd_locator.relNumber;
            hydrate_sequence_columns_in_slot(relation,
                                             slot,
                                             caches,
                                             amc,
                                             (*read_state)->snapshot,
                                             column_read_state_memory_context());
            caches->close_all(/*commit=*/true);
            bioseq::storage::destroy_seq_caches(caches);
        }
    }

    return true;
}

/** Reject unsupported HOT-chain traversal for stable virtual TIDs. */
void bioseq_get_latest_tid(TableScanDesc sscan, ItemPointer tid) {
    bioseq_log<logging_level::error, logging_target::postgres>(
        "bioseq_get_latest_tid not implemented");
}

/** Reject the unimplemented generic virtual-TID validity callback. */
bool bioseq_tuple_tid_valid(TableScanDesc scan, ItemPointer tid) {
    bioseq_log<logging_level::error, logging_target::postgres>(
        "bioseq_tuple_tid_valid not implemented");
}

/** Test row-mask visibility for the slot's virtual TID under a snapshot. */
bool bioseq_tuple_satisfies_snapshot(Relation rel, TupleTableSlot* slot, Snapshot snapshot) {
    uint64_t row_number = tid_to_row_number(slot->tts_tid);
    stripe_metadata* stripe_metadata_entry = find_stripe_by_row_number(rel, row_number, snapshot);
    return stripe_metadata_entry != nullptr;
}

/** Apply bottom-up index deletion semantics for virtual row identities. */
TransactionId bioseq_index_delete_tuples(Relation rel, TM_IndexDeleteOp* delstate) {
    previous_cache_enabled_state = bioseq::settings::column_cache::enabled;
    bioseq::settings::column_cache::enabled = false;

    /**
     * Supports bottom-up deletion suppression but rejects simple index deletion.
     */

    if (delstate->bottomup) {
        /**
         * Reports no deletable TIDs for PostgreSQL bottom-up btree cleanup.
         */
        delstate->ndeltids = 0;
        return InvalidTransactionId;
    } else {
        /**
         * Rejects simple deletion because index fetch never marks virtual rows all-dead.
         */
        bioseq_log<logging_level::error, logging_target::postgres>(
            "bioseq_index_delete_tuples not implemented for simple deletion");
    }
}

/** Enter the next-row implementation through the table-AM exception boundary. */
bool bioseq_getnextslot(TableScanDesc scan, ScanDirection direction, TupleTableSlot* slot) {
    return run_tableam_cpp_checked("bioseq getnextslot", [scan, direction, slot] {
        return bioseq_getnextslot_impl(scan, direction, slot);
    });
}
