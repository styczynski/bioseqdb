/**
 * @file
 * @brief Read Bioseq rows through table and index scans.
 */
module;

extern "C" {
#include "postgres.h"

#include "access/skey.h"
#include "access/tableam.h"
#if PG_VERSION_NUM >= 200000
#include "access/tableam_indexscan.h"
#endif
#include "access/xact.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
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


/** Begin a scan with selected columns, pushed-down predicates, and optional vector results. */
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

/** Begin a scan and choose whether to read full sequence values immediately. */
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

/** Begin a scan that returns sequence locators for the caller to read.
 *
 * Custom scans use this to group sequence reads and avoid loading values that
 * the query never needs. The column reader still applies its predicates.
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

/** C entry point for a scan that loads full sequences when returning individual rows. */
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

/** C entry point for a scan that leaves sequence locators for the caller to read. */
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
    /* This context owns the scan and its reader. The caller's current context
     * must survive until the scan ends. */
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

        /* Wait for the first tuple slot before choosing the row descriptor.
         * ALTER COLUMN rewrites can require a descriptor different from the table's. */
        scan->read_state = nullptr;
        scan->attributes_needed =
            bioseq::postgres::support::postgres_bitmapset{attributes_needed}.clone();
        scan->scan_qualifiers =
            bioseq::postgres::support::postgres_list<Node>{copyObject(scan_qualifiers)};
        scan->scan_context = scan_context;

        ;
        scan->parallel_scan = parallel_column_scan;

        
        scan->returns_vectorized_tuple = returns_vectorized_tuples;

        /* Keep fragment readers and the sequence-loading policy for this scan. */
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

    /* Close fragment readers and release their relation references. */
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

    /* Clear the shared column-page cache when it is enabled. */
    if (bioseq::settings::column_cache::enabled == true) {
        reset_column_cache();
    }

    MemoryContextDelete(scan->scan_context);

    bioseq::settings::column_cache::enabled = previous_cache_enabled_state;
}

/** Restart an initialized reader with a new qualifier list. */
void bioseq_rescan(TableScanDesc sscan,
                   ScanKey key,
                   bool set_params,
                   bool allow_strat,
                   bool allow_sync,
                   bool allow_pagemode) {
    column_scan_handle scan = reinterpret_cast<column_scan_handle>(sscan);

    /* This caller passes the qualifier List through the ScanKey argument. */
    List* scan_qual = reinterpret_cast<List*>(key);

    if (scan->read_state != nullptr) {
        rescan_column_reader(scan->read_state, scan_qual);
    }
}

/** Read the next row or vector. Individual rows load full sequences unless deferred. */
static bool
bioseq_getnextslot_impl(TableScanDesc sscan, ScanDirection direction, TupleTableSlot* slot) {
    column_scan_handle scan = reinterpret_cast<column_scan_handle>(sscan);

    
    if (scan->read_state == nullptr) {
        bool random_access = false;
        // Old stripes need column defaults added by ALTER TABLE. Vector-slot
        // descriptors omit those defaults, so use the open relation's descriptor
        // when decoding vector results.
        TupleDesc read_descriptor = scan->returns_vectorized_tuple
                                        ? RelationGetDescr(scan->scan_base.rs_rd)
                                        : slot->tts_tupleDescriptor;
        Assert(read_descriptor->natts == slot->tts_tupleDescriptor->natts);
        scan->read_state = init_bioseq_read_state(scan->scan_base.rs_rd,
                                                  read_descriptor,
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

        /* Load full sequence values unless the caller will read the locators. */
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

/** Reject the unsupported parallel table-scan size request. */
Size bioseq_parallelscan_estimate(Relation rel) {
    bioseq_log<logging_level::error, logging_target::postgres>(
        "bioseq_parallelscan_estimate not implemented");
}

/** Reject unsupported parallel table-scan initialization. */
Size bioseq_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan) {
    bioseq_log<logging_level::error, logging_target::postgres>(
        "bioseq_parallelscan_initialize not implemented");
}

/** Reject unsupported parallel table-scan reset. */
void bioseq_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan) {
    bioseq_log<logging_level::error, logging_target::postgres>(
        "bioseq_parallelscan_reinitialize not implemented");
}

/** Allocate the fragment cache used to read sequences from index-selected rows. */
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

/** Create index-fetch state, choosing columns when the first row is requested. */
column_index_fetch_base* bioseq_index_fetch_begin(Relation rel
#if PG_VERSION_NUM >= 190000
                                             , std::uint32_t flags
#endif
) {
    Oid relfilelocator = rel->rd_locator.relNumber;
    if (writes_pending_in_parent_transactions(relfilelocator, GetCurrentSubTransactionId())) {
        /* A subtransaction cannot read unflushed writes from its parent. */
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

/** Create index-fetch state for the selected columns. */
export column_index_fetch_base* bioseq_index_fetch_begin_extended(Relation rel,
                                                              Bitmapset* attr_needed) {
    Oid relfilenode = rel->rd_locator.relNumber;
    if (writes_pending_in_parent_transactions(relfilenode, GetCurrentSubTransactionId())) {
        /* A subtransaction cannot read unflushed writes from its parent. */
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

/** Keep the reader and caches for reuse. Index-fetch reset needs no work. */
void bioseq_index_fetch_reset(column_index_fetch_base* sscan) {
    /* no-op */
}

/** End an index fetch and release its reader, caches, and memory context. */
void bioseq_index_fetch_end(column_index_fetch_base* sscan) {
    bioseq_index_fetch_reset(sscan);

    index_fetch_state* scan = reinterpret_cast<index_fetch_state*>(sscan);

#if PG_VERSION_NUM >= 200000
    if (scan->visibility_slot != nullptr) {
        ExecDropSingleTupleTableSlot(scan->visibility_slot);
        scan->visibility_slot = nullptr;
    }
#endif

    /* Close the fragment readers held by this index fetch. */
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

    /* Clear the shared column-page cache when it is enabled. */
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

/** Fetch a row selected by an index under the supplied snapshot.
 *
 * Flushed rows are checked by the column reader. Dirty-snapshot constraint
 * checks can also find unfinished writes and wait for their transaction.
 */
bool bioseq_index_fetch_tuple(column_index_fetch_base* sscan,
                              ItemPointer tid,
                              Snapshot snapshot,
                              TupleTableSlot* slot,
                              bool* call_again,
                              bool* all_dead) {
    /* Each TID identifies one row. There is no HOT chain to follow. */
    *call_again = false;

    /* Keep all_dead false because simple index-entry deletion is unsupported. */
    if (all_dead) {
        *all_dead = false;
    }

    ExecClearTuple(slot);

    index_fetch_state* scan = reinterpret_cast<index_fetch_state*>(sscan);
    Relation bioseq_column_relation = scan->fetch_base.rel;

    
    if (scan->read_state == nullptr) {
        /* The index scan evaluates its own predicates. */
        List* scan_qual = NIL;

        if (scan->attributes_needed.is_empty()) {
            /* A scan without a projection reads all columns. */
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
        /* No candidate stripe covers this row number. */
        return false;
    }

    stripe_write_state stripe_write_state_value = stripe_write_state_for(stripe_metadata_entry);

    if (stripe_write_state_value == stripe_write_state::flushed &&
        !read_column_row_by_number(
            scan->read_state, row_number, slot->tts_values, slot->tts_isnull)) {
        /* A candidate stripe may end before this row number, or its row mask
         * may hide the row. The column reader performs those checks. */
        return false;
    } else if (stripe_write_state_value == stripe_write_state::aborted) {
        /* Dirty-snapshot constraint checks can see an aborted, unflushed
         * stripe. It contributes no conflicting row. */
        pfree(stripe_metadata_entry);
        Assert(snapshot->snapshot_type == SNAPSHOT_DIRTY);
        return false;
    } else if (stripe_write_state_value == stripe_write_state::in_progress) {
        if (stripe_metadata_entry->inserted_by_current_transaction) {
            /* Flush our own pending writes before reading this row. */
            flush_pending_writes_for_read(scan->read_state);

            /* The row must be readable after flushing our write. */
            read_column_row_by_number_or_error(
                scan->read_state, row_number, slot->tts_values, slot->tts_isnull);
        } else {
            /* An unfinished write from another transaction requires a dirty snapshot. */
            Assert(snapshot->snapshot_type == SNAPSHOT_DIRTY);

            /* A uniqueness check can use this row identity to wait for the writer
             * before retrying. Its unflushed column values are unavailable here. */
            memset(slot->tts_isnull, true, slot->tts_nvalid * sizeof(bool));
        }
    } else {
        /* The flushed row was read successfully above. */
        Assert(stripe_write_state_value == stripe_write_state::flushed);
    }

    if (!scan->is_select_query)
        pfree(stripe_metadata_entry);
    slot->tts_tableOid = RelationGetRelid(bioseq_column_relation);
    slot->tts_tid = *tid;
    ExecStoreVirtualTuple(slot);

    /* Read full sequences unless the custom scan will resolve the locators. */
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

#if PG_VERSION_NUM >= 200000
/** Read index TIDs until a visible table row is found, then fill the result slot. */
template <bool p_index_only>
static bool bioseq_index_next_slot(IndexScanDesc descriptor,
                                   ScanDirection direction,
                                   TupleTableSlot* slot) {
    auto* scan = static_cast<index_fetch_state*>(descriptor->xs_table_opaque);
    while (tableam_index_getnext_tid(descriptor, direction)) {
        CHECK_FOR_INTERRUPTS();
        bool call_again = false;
        bool all_dead = false;
        TupleTableSlot* row_slot = slot;
        if constexpr (p_index_only) {
            row_slot = scan->visibility_slot;
            if (descriptor->instrument != nullptr)
                ++descriptor->instrument->ntabletuplefetches;
        }
        if (!bioseq_index_fetch_tuple(&scan->fetch_base, &descriptor->xs_heaptid,
                                       descriptor->xs_snapshot, row_slot,
                                       &call_again, &all_dead))
            continue;
        // Rows have no HOT chains. An invisible row may still be visible to
        // another snapshot, so this reader never marks its index TID as dead.
        Assert(!call_again);
        pgstat_count_heap_fetch(descriptor->indexRelation);
        if constexpr (p_index_only)
            tableam_index_fill_ios_slot(descriptor, slot);
        return true;
    }
    ExecClearTuple(slot);
    return false;
}

/** Create the index reader and choose regular or index-only tuple output. */
export void bioseq_index_scan_begin_projected(IndexScanDesc descriptor,
                                             std::uint32_t flags,
                                             Bitmapset* attributes_needed) {
    column_index_fetch_base* base = attributes_needed != nullptr
        ? bioseq_index_fetch_begin_extended(descriptor->heapRelation, attributes_needed)
        : bioseq_index_fetch_begin(descriptor->heapRelation, flags);
    base->flags = flags;
    auto* scan = reinterpret_cast<index_fetch_state*>(base);
    descriptor->xs_table_opaque = scan;
    if (descriptor->xs_want_itup) {
        bioseq::postgres::support::memory_context_scope context{scan->scan_context};
        scan->visibility_slot = table_slot_create(descriptor->heapRelation, nullptr);
        descriptor->xs_getnext_slot = bioseq_index_next_slot<true>;
    } else {
        descriptor->xs_getnext_slot = bioseq_index_next_slot<false>;
    }
}

/** Begin a PostgreSQL index scan that reads all columns. */
void bioseq_index_scan_begin(IndexScanDesc descriptor, std::uint32_t flags) {
    bioseq_index_scan_begin_projected(descriptor, flags, nullptr);
}

/** Keep the index reader and caches for reuse. */
void bioseq_index_scan_reset(IndexScanDesc descriptor) {
    auto* scan = static_cast<index_fetch_state*>(descriptor->xs_table_opaque);
    bioseq_index_fetch_reset(&scan->fetch_base);
}

/** Close the index reader and release its caches and memory context. */
void bioseq_index_scan_end(IndexScanDesc descriptor) {
    auto* scan = static_cast<index_fetch_state*>(descriptor->xs_table_opaque);
    bioseq_index_fetch_end(&scan->fetch_base);
    descriptor->xs_table_opaque = nullptr;
}

/** Check a single index TID under the supplied snapshot, including dirty-snapshot checks. */
bool bioseq_fetch_tid(Relation relation, ItemPointer tuple_id,
                      Snapshot snapshot, bool* all_dead) {
    column_index_fetch_base* base = bioseq_index_fetch_begin(relation, SO_NONE);
    auto* scan = reinterpret_cast<index_fetch_state*>(base);
    {
        bioseq::postgres::support::memory_context_scope context{scan->scan_context};
        scan->visibility_slot = table_slot_create(relation, nullptr);
    }
    bool call_again = false;
    const bool found = bioseq_index_fetch_tuple(
        base, tuple_id, snapshot, scan->visibility_slot, &call_again, all_dead);
    Assert(!call_again);
    bioseq_index_fetch_end(base);
    return found;
}
#endif

/** Read a row version and its sequence values into the caller's slot. */
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

    /* Open a temporary fragment cache for this fetch and close it after
     * loading the sequence values. The row reader itself is reused. */
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

/** Reject the unsupported request to follow a row to its latest TID. */
void bioseq_get_latest_tid(TableScanDesc sscan, ItemPointer tid) {
    bioseq_log<logging_level::error, logging_target::postgres>(
        "bioseq_get_latest_tid not implemented");
}

/** Reject the unsupported tuple-identifier validity check. */
bool bioseq_tuple_tid_valid(TableScanDesc scan, ItemPointer tid) {
    bioseq_log<logging_level::error, logging_target::postgres>(
        "bioseq_tuple_tid_valid not implemented");
}

/** Check whether the row number belongs to a stripe visible under the snapshot. */
bool bioseq_tuple_satisfies_snapshot(Relation rel, TupleTableSlot* slot, Snapshot snapshot) {
    uint64_t row_number = tid_to_row_number(slot->tts_tid);
    stripe_metadata* stripe_metadata_entry = find_stripe_by_row_number(rel, row_number, snapshot);
    return stripe_metadata_entry != nullptr;
}

/** Report no removable rows for bottom-up index cleanup and reject simple deletion. */
TransactionId bioseq_index_delete_tuples(Relation rel, TM_IndexDeleteOp* delstate) {
    previous_cache_enabled_state = bioseq::settings::column_cache::enabled;
    bioseq::settings::column_cache::enabled = false;

    

    if (delstate->bottomup) {
        /* Keep all index entries during bottom-up btree cleanup. */
        delstate->ndeltids = 0;
        return InvalidTransactionId;
    } else {
        /* Index fetches never set all_dead, so simple deletion should not reach
         * this callback. */
        bioseq_log<logging_level::error, logging_target::postgres>(
            "bioseq_index_delete_tuples not implemented for simple deletion");
    }
}

/** Read the next result and convert C++ exceptions into PostgreSQL errors. */
bool bioseq_getnextslot(TableScanDesc scan, ScanDirection direction, TupleTableSlot* slot) {
    return run_tableam_cpp_checked("bioseq getnextslot", [scan, direction, slot] {
        return bioseq_getnextslot_impl(scan, direction, slot);
    });
}
