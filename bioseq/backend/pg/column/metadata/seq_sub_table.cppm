/**
 * @file seq_sub_table.cppm
 * @brief Manage columnar stripe metadata and sequence-storage shards.
 *
 * Stripe operations track reserved rows and storage ranges. The sequence
 * sub-table registry maps each column's length buckets to physical shards
 * and records their byte counts.
 */
module;
#include <algorithm>
#include <array>
#include <bioseq/pg_compat.h>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <sys/stat.h>
#include <type_traits>
#include <utility>
#include <vector>

#include "safe_lib.h"

export module bioseq.postgres.storage.metadata:seq_sub_table;

import bioseq.common.checked_arithmetic;
import bioseq.postgres.storage.physical;
import bioseq.postgres.storage.compression;
import bioseq.common.logging;
import bioseq.postgres.configuration.core;
import bioseq.postgres.support;
import bioseq.postgres.support.datums;
import bioseq.postgres.sql.generated.core.storage_metadata_core.entity_handles;
import bioseq.postgres.storage.metadata.sql_row_codecs;
import :types;
import :tuple_ops;
import :core;

/// @brief List a relation's stripes in first-row order under the transaction snapshot.
/// scan_direction selects ascending or descending order.
export List* list_stripes_for_relation(RelFileLocator relfilelocator,
                                       ScanDirection scan_direction) {
    uint64_t storage_id = lookup_storage_id(relfilelocator);

    return read_data_file_stripes(storage_id, GetTransactionSnapshot(), scan_direction);
}

/// @brief Sum the row counts of a relation's visible stripes, including deleted rows.
export uint64_t column_table_row_count(Relation relation) {
    uint64_t total_row_count = 0;
    List* stripe_list = list_stripes_for_relation(relation->rd_locator, ForwardScanDirection);

    for (stripe_metadata* stripe :
         bioseq::postgres::support::postgres_list<stripe_metadata>{stripe_list}) {
        total_row_count += stripe->row_count;
    }

    return total_row_count;
}

/** @brief Sum deleted-row counts across a stripe's chunk groups. */
export uint32_t
deleted_rows_for_stripe(RelFileLocator relfilelocator, uint32_t chunk_count, uint64_t stripe_id) {
    uint64_t storage_id = lookup_storage_id(relfilelocator);

    uint32_t* chunk_group_row_counts;
    uint32_t* chunk_group_deleted_rows;
    int i;

    uint32_t deleted_rows = 0;

    read_chunk_group_row_counts(storage_id,
                                stripe_id,
                                chunk_count,
                                &chunk_group_row_counts,
                                &chunk_group_deleted_rows,
                                GetTransactionSnapshot());

    for (i = 0; i < chunk_count; i++) {
        deleted_rows += chunk_group_deleted_rows[i];
    }

    pfree(chunk_group_row_counts);
    pfree(chunk_group_deleted_rows);

    return deleted_rows;
}

/** @brief Sum the uncompressed value-buffer sizes of all chunks in a stripe. */
export Size decompressed_length_for_stripe(RelFileLocator relfilelocator, uint64_t stripe_id) {
    HeapTuple heap_tuple = nullptr;
    ScanKeyData scan_keys[2];

    uint64_t storage_id = lookup_storage_id(relfilelocator);

    Oid bioseq_column_chunk_oid = column_chunk_relation_oid();
    Relation bioseq_column_chunk = table_open(bioseq_column_chunk_oid, AccessShareLock);
    Relation index = index_open(column_chunk_index_oid(), AccessShareLock);

    scan_keys[0] =
        chunk_codec::scan_key<&chunk_row::storage_id>(static_cast<std::int64_t>(storage_id));
    scan_keys[1] =
        chunk_codec::scan_key<&chunk_row::stripe_num>(static_cast<std::int64_t>(stripe_id));

    Snapshot snapshot = RegisterSnapshot(GetTransactionSnapshot());
    SysScanDesc scan_descriptor = systable_beginscan_ordered(
        bioseq_column_chunk, index, snapshot, 2, scan_keys);

    Size decompressed_chunk_size = 0;

    while (HeapTupleIsValid(heap_tuple =
                                systable_getnext_ordered(scan_descriptor, ForwardScanDirection))) {
        const chunk_row row = chunk_codec::deform(
            heap_tuple, RelationGetDescr(bioseq_column_chunk), CurrentMemoryContext);
        decompressed_chunk_size += row.value_decompressed_length;
    }

    systable_endscan_ordered(scan_descriptor);
    UnregisterSnapshot(snapshot);
    index_close(index, AccessShareLock);
    table_close(bioseq_column_chunk, AccessShareLock);

    return decompressed_chunk_size;
}

/**
 * @brief Find the last occupied logical byte for VACUUM truncation.
 *
 * Use a dirty snapshot to include stripes from transactions still writing.
 * With no stripes, keep the reserved metapage range.
 */
export uint64_t find_highest_used_address(RelFileLocator relfilelocator) {
    uint64_t storage_id = lookup_storage_id(relfilelocator);

    uint64_t highest_used_address = 0;
    uint64_t highest_used_id = 0;
    find_highest_used_address_and_id(storage_id, &highest_used_address, &highest_used_id);

    return highest_used_address;
}

/** @brief Find the highest stripe id and last occupied logical byte.
 * Use a dirty snapshot so concurrent stripe reservations are included.
 */
void find_highest_used_address_and_id(uint64_t storage_id,
                                      uint64_t* highest_used_address,
                                      uint64_t* highest_used_id) {
    SnapshotData snapshot_dirty;
    InitDirtySnapshot(snapshot_dirty);

    List* stripe_metadata_list =
        read_data_file_stripes(storage_id, &snapshot_dirty, ForwardScanDirection);

    *highest_used_id = 0;

    /* Keep the metapage range even when there are no stripes. */
    *highest_used_address = usable_column_bytes_per_page;

    for (stripe_metadata* stripe :
         bioseq::postgres::support::postgres_list<stripe_metadata>{stripe_metadata_list}) {
        uint64_t last_byte = stripe->file_offset + stripe->data_length - 1;
        *highest_used_address = std::max(*highest_used_address, last_byte);
        *highest_used_id = std::max(*highest_used_id, static_cast<uint64_t>(stripe->id));
    }
}

/**
 * @brief Reserve a stripe id and logical row range, then record the reservation.
 *
 * The metapage counters assign separate ranges to concurrent writers.
 * Flushing later assigns storage bytes and completes the catalog row.
 */
export empty_stripe_reservation* reserve_empty_stripe(Relation rel,
                                                      uint64_t column_count,
                                                      uint64_t chunk_group_row_count,
                                                      uint64_t stripe_row_limit) {
    empty_stripe_reservation* stripe_reservation =
        bioseq::postgres::support::allocate_zeroed_object<empty_stripe_reservation>();

    uint64_t storage_id = column_storage_id(rel, false);

    stripe_reservation->stripe_id = reserve_column_stripe_id(rel);
    stripe_reservation->stripe_first_row_number = reserve_column_row_numbers(rel, stripe_row_limit);

    /* Record the reservation now. Flushing the stripe completes this row in place. */
    insert_empty_stripe_metadata_row(storage_id,
                                     stripe_reservation->stripe_id,
                                     column_count,
                                     chunk_group_row_count,
                                     stripe_reservation->stripe_first_row_number);

    return stripe_reservation;
}

/** @brief Reserve bytes for a flushed stripe and complete its catalog row.
 * Store its byte range, row count and chunk-group count in the existing reservation.
 */
export stripe_metadata* complete_stripe_reservation(Relation rel,
                                                    uint64_t stripe_id,
                                                    uint64_t size_bytes,
                                                    uint64_t row_count,
                                                    uint64_t chunk_count) {
    uint64_t res_logical_start = reserve_column_storage_bytes(rel, size_bytes);
    uint64_t storage_id = column_storage_id(rel, false);

    stripe_codec::update update{};
    update.set<&stripe_row::file_offset>(static_cast<std::int64_t>(res_logical_start))
        .set<&stripe_row::data_length>(static_cast<std::int64_t>(size_bytes))
        .set<&stripe_row::row_count>(static_cast<std::int64_t>(row_count))
        .set<&stripe_row::chunk_group_count>(static_cast<std::int32_t>(chunk_count));

    return update_stripe_metadata_row(storage_id, stripe_id, update);
}

/** @brief Updates non-indexed fields of one stripe catalog row in place.
 *
 * The caller must not use this path for fields covered by a catalog index.
 */
stripe_metadata*
update_stripe_metadata_row(uint64_t storage_id, uint64_t stripe_id, stripe_codec::update& update) {
    ScanKeyData scan_keys[2];
    scan_keys[0] =
        stripe_codec::scan_key<&stripe_row::storage_id>(static_cast<std::int64_t>(storage_id));
    scan_keys[1] =
        stripe_codec::scan_key<&stripe_row::stripe_num>(static_cast<std::int64_t>(stripe_id));

    Oid bioseq_column_stripes_oid = column_stripe_relation_oid();
    Relation bioseq_column_stripes = table_open(bioseq_column_stripes_oid, AccessShareLock);
    TupleDesc tuple_descriptor = RelationGetDescr(bioseq_column_stripes);

    Oid index_id = column_stripe_primary_key_index_oid();
    bool index_ok = OidIsValid(index_id);

    void* state = nullptr;
    HeapTuple old_tuple = nullptr;
    systable_inplace_update_begin(bioseq_column_stripes,
                                  index_id,
                                  index_ok,
                                  /* snapshot */ nullptr,
                                  2,
                                  scan_keys,
                                  &old_tuple,
                                  &state);

    if (!HeapTupleIsValid(old_tuple)) {
        systable_inplace_update_cancel(state);
        table_close(bioseq_column_stripes, AccessShareLock);
        bioseq::postgres::support::raise_sql_error(
            ERRCODE_INTERNAL_ERROR,
            {
                .message = fmt::format("attempted to modify an unexpected stripe, columnar "
                                       "storage with id={} does not have stripe with id={}",
                                       storage_id,
                                       stripe_id),
            });
    }

    HeapTuple modified_tuple = update.modify(old_tuple, tuple_descriptor);

    systable_inplace_update_finish(state, modified_tuple);

    /* Decode the updated values from a heap tuple with transaction header fields. */
    stripe_metadata* modified_stripe_metadata =
        build_stripe_metadata(bioseq_column_stripes, modified_tuple);

    CommandCounterIncrement();

    heap_freetuple(modified_tuple);
    table_close(bioseq_column_stripes, AccessShareLock);

    /* Return the updated stripe metadata. */
    return modified_stripe_metadata;
}

/** @brief Read visible stripes in first-row order using the supplied snapshot. */
List* read_data_file_stripes(uint64_t storage_id, Snapshot snapshot, ScanDirection scan_direction) {
    // Keep an unregistered MVCC snapshot alive for the ordered scan.
    // The caller manages the lifetime of an already registered or active snapshot.
    // Dirty snapshots are used directly.
    const bool retain_snapshot = snapshot != InvalidSnapshot && IsMVCCSnapshot(snapshot) &&
                                 snapshot->regd_count == 0 && snapshot->active_count == 0;
    if (retain_snapshot) {
        snapshot = RegisterSnapshot(snapshot);
    }
    bioseq::postgres::support::postgres_list<stripe_metadata> stripe_metadata_list;
    ScanKeyData scan_keys[1];
    HeapTuple heap_tuple;

    scan_keys[0] =
        stripe_codec::scan_key<&stripe_row::storage_id>(static_cast<std::int64_t>(storage_id));

    Oid bioseq_column_stripes_oid = column_stripe_relation_oid();

    Relation bioseq_column_stripes = table_open(bioseq_column_stripes_oid, AccessShareLock);
    Relation index = index_open(stripe_first_row_number_index_oid(), AccessShareLock);

    SysScanDesc scan_descriptor =
        systable_beginscan_ordered(bioseq_column_stripes, index, snapshot, 1, scan_keys);

    while (
        HeapTupleIsValid(heap_tuple = systable_getnext_ordered(scan_descriptor, scan_direction))) {
        stripe_metadata* stripe = build_stripe_metadata(bioseq_column_stripes, heap_tuple);
        stripe_metadata_list.append(stripe);
    }

    systable_endscan_ordered(scan_descriptor);
    if (retain_snapshot) {
        UnregisterSnapshot(snapshot);
    }
    index_close(index, AccessShareLock);
    table_close(bioseq_column_stripes, AccessShareLock);

    return stripe_metadata_list.release();
}

/** @brief Decode and validate a stripe catalog tuple.
 * The tuple must include a heap header because its inserting transaction
 * determines the stripe's aborted and current-transaction flags.
 */
stripe_metadata* build_stripe_metadata(Relation bioseq_column_stripes, HeapTuple heap_tuple) {
    Assert(RelationGetRelid(bioseq_column_stripes) == column_stripe_relation_oid());

    const stripe_row row = stripe_codec::deform(
        heap_tuple, RelationGetDescr(bioseq_column_stripes), CurrentMemoryContext);

    stripe_metadata* stripe = bioseq::postgres::support::allocate_zeroed_object<stripe_metadata>();
    stripe->id = row.stripe_num;
    stripe->file_offset = row.file_offset;
    stripe->data_length = row.data_length;
    stripe->column_count = row.column_count;
    stripe->chunk_count = row.chunk_group_count;
    stripe->chunk_group_row_count = row.chunk_row_count;
    stripe->row_count = row.row_count;
    stripe->first_row_number = static_cast<std::uint64_t>(row.first_row_number);

    /* Committed subtransactions have already flushed their stripes.
     * Scans reject pending writes in parent transactions, so the inserting
     * transaction id is enough to classify the stripe here. */
    TransactionId entry_xmin = HeapTupleHeaderGetXmin(heap_tuple->t_data);
    stripe->aborted = !TransactionIdIsInProgress(entry_xmin) && TransactionIdDidAbort(entry_xmin);
    stripe->inserted_by_current_transaction = TransactionIdIsCurrentTransactionId(entry_xmin);

    validate_stripe_metadata(stripe);

    return stripe;
}

/** @brief Delete stripe, chunk and row-mask metadata for a physical relation. */
export void delete_metadata_rows(RelFileLocator relfilelocator) {
    /* Binary-upgrade restore order does not guarantee catalog availability. */
    if (IsBinaryUpgrade) {
        return;
    }

    uint64_t storage_id = lookup_storage_id(relfilelocator);

    delete_storage_metadata_rows(column_stripe_relation_oid(),
                                 stripe_codec::attribute_number<&stripe_row::storage_id>(),
                                 column_stripe_primary_key_index_oid(),
                                 storage_id);
    delete_storage_metadata_rows(
        column_chunk_group_relation_oid(),
        chunk_group_codec::attribute_number<&chunk_group_row::storage_id>(),
        column_chunk_group_index_oid(),
        storage_id);
    delete_storage_metadata_rows(column_chunk_relation_oid(),
                                 chunk_codec::attribute_number<&chunk_row::storage_id>(),
                                 column_chunk_index_oid(),
                                 storage_id);
    delete_storage_metadata_rows(column_row_mask_relation_oid(),
                                 row_mask_codec::attribute_number<&row_mask_row::storage_id>(),
                                 column_row_mask_index_oid(),
                                 storage_id);
}

/** @brief Delete one stripe and its chunk and row-mask metadata. */
export void delete_metadata_rows_for_stripe(RelFileLocator relfilelocator, uint64_t stripe_id) {
    /* Binary-upgrade restore order does not guarantee catalog availability. */
    if (IsBinaryUpgrade) {
        return;
    }

    uint64_t storage_id = lookup_storage_id(relfilelocator);

    delete_stripe_metadata_rows(column_stripe_relation_oid(),
                                stripe_codec::attribute_number<&stripe_row::storage_id>(),
                                stripe_codec::attribute_number<&stripe_row::stripe_num>(),
                                column_stripe_primary_key_index_oid(),
                                storage_id,
                                stripe_id);
    delete_stripe_metadata_rows(column_chunk_group_relation_oid(),
                                chunk_group_codec::attribute_number<&chunk_group_row::storage_id>(),
                                chunk_group_codec::attribute_number<&chunk_group_row::stripe_num>(),
                                column_chunk_group_index_oid(),
                                storage_id,
                                stripe_id);
    delete_stripe_metadata_rows(column_chunk_relation_oid(),
                                chunk_codec::attribute_number<&chunk_row::storage_id>(),
                                chunk_codec::attribute_number<&chunk_row::stripe_num>(),
                                column_chunk_index_oid(),
                                storage_id,
                                stripe_id);
    delete_stripe_metadata_rows(column_row_mask_relation_oid(),
                                row_mask_codec::attribute_number<&row_mask_row::storage_id>(),
                                row_mask_codec::attribute_number<&row_mask_row::stripe_id>(),
                                column_row_mask_stripe_index_oid(),
                                storage_id,
                                stripe_id);
}

/** @brief Delete every row for a storage id from the given metadata table. */
void delete_storage_metadata_rows(Oid metadata_table_id,
                                  AttrNumber storage_id_attribute_number,
                                  Oid storage_id_index_id,
                                  uint64_t storage_id) {
    ScanKeyData scan_keys[1];
    ScanKeyInit(&scan_keys[0],
                storage_id_attribute_number,
                BTEqualStrategyNumber,
                F_INT8EQ,
                UInt64GetDatum(storage_id));

    Relation metadata_table =
        bioseq::postgres::support::try_open_postgres_relation(metadata_table_id, AccessShareLock);
    if (metadata_table == nullptr) {
        /* The metadata table is already gone. */
        return;
    }

    Relation index = index_open(storage_id_index_id, AccessShareLock);

    SysScanDesc scan_descriptor =
        systable_beginscan_ordered(metadata_table, index, nullptr, 1, scan_keys);

    bioseq::storage::metadata::tuple_ops::relation_modification_state* relation_modification_state =
        bioseq::storage::metadata::tuple_ops::begin_relation_modification(metadata_table);

    HeapTuple heap_tuple;
    while (HeapTupleIsValid(heap_tuple =
                                systable_getnext_ordered(scan_descriptor, ForwardScanDirection))) {
        bioseq::storage::metadata::tuple_ops::delete_catalog_tuple(relation_modification_state,
                                                                   heap_tuple);
    }

    systable_endscan_ordered(scan_descriptor);

    bioseq::storage::metadata::tuple_ops::finish_relation_modification(relation_modification_state);

    index_close(index, AccessShareLock);
    table_close(metadata_table, AccessShareLock);
}

/** @brief Delete metadata rows matching both a storage id and stripe id. */
void delete_stripe_metadata_rows(Oid metadata_table_id,
                                 AttrNumber storage_id_attribute_number,
                                 AttrNumber stripe_id_attr_number,
                                 Oid storage_id_index_id,
                                 uint64_t storage_id,
                                 uint64_t stripe_id) {
    ScanKeyData scan_keys[2];
    ScanKeyInit(&scan_keys[0],
                storage_id_attribute_number,
                BTEqualStrategyNumber,
                F_INT8EQ,
                UInt64GetDatum(storage_id));
    ScanKeyInit(&scan_keys[1],
                stripe_id_attr_number,
                BTEqualStrategyNumber,
                F_INT8EQ,
                UInt64GetDatum(stripe_id));

    Relation metadata_table =
        bioseq::postgres::support::try_open_postgres_relation(metadata_table_id, RowShareLock);
    if (metadata_table == nullptr) {
        /* The metadata table is already gone. */
        return;
    }

    Relation index = index_open(storage_id_index_id, RowShareLock);
    SysScanDesc scan_descriptor =
        systable_beginscan_ordered(metadata_table, index, nullptr, 2, scan_keys);

    bioseq::storage::metadata::tuple_ops::relation_modification_state* relation_modification_state =
        bioseq::storage::metadata::tuple_ops::begin_relation_modification(metadata_table);

    HeapTuple heap_tuple;
    while (HeapTupleIsValid(heap_tuple =
                                systable_getnext_ordered(scan_descriptor, ForwardScanDirection))) {
        bioseq::storage::metadata::tuple_ops::delete_catalog_tuple(relation_modification_state,
                                                                   heap_tuple);
    }

    systable_endscan_ordered(scan_descriptor);

    bioseq::storage::metadata::tuple_ops::finish_relation_modification(relation_modification_state);

    index_close(index, RowShareLock);
    table_close(metadata_table, RowShareLock);
}

/* Resolve bioseq_internal.storageid_seq, or return InvalidOid if missing. */
Oid column_storage_id_sequence_oid(void) {
    return metadata_entity_handles::storageid_seq.resolve_oid(
        bioseq::postgres::support::catalog_missing_policy::return_invalid);
}

/* Look up bioseq_internal.stripe on each call so catalog recreation
 * cannot leave a cached OID pointing to the old table. */
Oid column_stripe_relation_oid(void) {
    return get_relname_relid("stripe", column_namespace_oid());
}

/* Look up the current bioseq_internal.stripe_pkey OID. */
Oid column_stripe_primary_key_index_oid(void) {
    return get_relname_relid("stripe_pkey", column_namespace_oid());
}

/* Look up the current bioseq_internal.stripe_first_row_number_idx OID. */
Oid stripe_first_row_number_index_oid(void) {
    return get_relname_relid("stripe_first_row_number_idx", column_namespace_oid());
}

/* Look up bioseq_internal.options. */
Oid column_options_relation_oid(void) {

    return get_relname_relid("options", column_namespace_oid());
}

/* Look up bioseq_internal.options_pkey. */
Oid column_options_index_oid(void) {
    bioseq_log<logging_level::debug>("[bioseq] pg: column_options_index_oid() entered");
    return get_relname_relid("options_pkey", column_namespace_oid());
}

/* Look up the current bioseq_internal.chunk OID. */
Oid column_chunk_relation_oid(void) {
    bioseq_log<logging_level::debug>("[bioseq] pg: column_chunk_relation_oid() entered");
    return get_relname_relid("chunk", column_namespace_oid());
}

/* Look up the current bioseq_internal.chunk_group OID. */
Oid column_chunk_group_relation_oid(void) {
    bioseq_log<logging_level::debug>("[bioseq] pg: column_chunk_group_relation_oid() entered");
    return get_relname_relid("chunk_group", column_namespace_oid());
}

/* Look up bioseq_internal.row_mask. */
Oid column_row_mask_relation_oid(void) {
    bioseq_log<logging_level::debug>("[bioseq] pg: column_row_mask_relation_oid() entered");
    return get_relname_relid("row_mask", column_namespace_oid());
}

/* Resolve bioseq_internal.row_mask_seq, or return InvalidOid if missing. */
Oid column_row_mask_sequence_oid(void) {
    bioseq_log<logging_level::debug>("[bioseq] pg: column_row_mask_sequence_oid() entered");
    return metadata_entity_handles::row_mask_seq.resolve_oid(
        bioseq::postgres::support::catalog_missing_policy::return_invalid);
}

/* Look up the current bioseq_internal.chunk_pkey OID. */
Oid column_chunk_index_oid(void) {
    bioseq_log<logging_level::debug>("[bioseq] pg: column_chunk_index_oid() entered");
    return get_relname_relid("chunk_pkey", column_namespace_oid());
}

/* Look up the current bioseq_internal.chunk_group_pkey OID. */
Oid column_chunk_group_index_oid(void) {
    bioseq_log<logging_level::debug>("[bioseq] pg: column_chunk_group_index_oid() entered");
    return get_relname_relid("chunk_group_pkey", column_namespace_oid());
}

/* Look up bioseq_internal.row_mask_pkey. */
Oid column_row_mask_index_oid(void) {
    bioseq_log<logging_level::debug>("[bioseq] pg: column_row_mask_index_oid() entered");
    return get_relname_relid("row_mask_pkey", column_namespace_oid());
}

/* Look up bioseq_internal.row_mask_stripe_unique. */
Oid column_row_mask_stripe_index_oid(void) {
    bioseq_log<logging_level::debug>("[bioseq] pg: column_row_mask_stripe_index_oid() entered");
    return get_relname_relid("row_mask_stripe_unique", column_namespace_oid());
}

/* Find the bioseq_internal schema used for columnar metadata. */
Oid column_namespace_oid(void) {
    Oid nspace = get_namespace_oid("bioseq_internal", false);
    bioseq_log<logging_level::debug>(
        "[bioseq] pg: column_namespace_oid() looking up bioseq_internal namespace");
    bioseq_log<logging_level::debug>("[bioseq] pg: column_namespace_oid() returning oid");
    return nspace;
}

/** @brief Read a relation's storage id from its metapage.
 * The relation must already have a metapage in the current format.
 */
uint64_t lookup_storage_id(RelFileLocator relfilelocator) {
    bioseq_log<logging_level::debug>("[bioseq] pg: lookup_storage_id() entered");
    Oid relation_oid = RelidByRelfilenumber(relfilelocator.spcOid, relfilelocator.relNumber);

    Relation relation =
        bioseq::postgres::support::open_postgres_relation(relation_oid, AccessShareLock);
    uint64_t storage_id = column_storage_id(relation, false);
    table_close(relation, AccessShareLock);

    return storage_id;
}

/** @brief Allocate a storage id from bioseq_internal.storageid_seq. */
export uint64_t allocate_column_storage_id() {
    bioseq_log<logging_level::debug>("[bioseq] pg: allocate_column_storage_id() entered");
    return nextval_internal(column_storage_id_sequence_oid(), false);
}

/** @brief Initialize or update the relation's columnar metapage.
 *
 * An empty relation gets a new storage id and metapage. Otherwise, rebuild
 * reservation counters from stripe metadata before updating the format.
 * Return immediately if the metapage already uses the current format.
 */
export void ensure_current_column_storage_format(Relation rel, bool is_upgrade) {
    bioseq_log<logging_level::debug>("[bioseq] pg: ensure_current_column_storage_format() entered");
    if (column_storage_uses_current_format(rel)) {
        return;
    }

    BlockNumber nblocks = smgrnblocks(RelationGetSmgr(rel), MAIN_FORKNUM);
    bioseq_log<logging_level::debug>(
        "[bioseq] pg: ensure_current_column_storage_format() checking nblocks<2");
    if (nblocks < 2) {
        initialize_column_storage(rel->rd_smgr, allocate_column_storage_id());
        return;
    }

    uint64_t storage_id = column_storage_id(rel, true);

    uint64_t highest_id;
    uint64_t highest_offset;
    find_highest_used_address_and_id(storage_id, &highest_offset, &highest_id);

    const uint64_t highest_row_number = find_highest_used_row_number(storage_id);
    const std::optional<uint64_t> next_stripe_id =
        bioseq::common::checked_add(highest_id, static_cast<uint64_t>(1));
    const std::optional<uint64_t> next_row_number =
        bioseq::common::checked_add(highest_row_number, static_cast<uint64_t>(1));

    std::optional<uint64_t> next_logical_offset = first_logical_offset;
    if (highest_id != 0) {
        if (highest_offset < first_logical_offset) {
            bioseq::postgres::support::raise_sql_error(
                ERRCODE_DATA_CORRUPTED,
                {
                    .message = fmt::format(
                        "cannot reconstruct column storage counters for relation {}", rel->rd_id),
                    .detail = fmt::format("Stripe metadata ends at logical offset {}, below the "
                                          "first valid offset {}.",
                                          highest_offset,
                                          first_logical_offset),
                });
        }
        next_logical_offset = bioseq::common::checked_add(highest_offset, static_cast<uint64_t>(1));
    }

    if (!next_stripe_id.has_value() || !next_row_number.has_value() ||
        !next_logical_offset.has_value()) {
        bioseq::postgres::support::raise_sql_error(
            ERRCODE_PROGRAM_LIMIT_EXCEEDED,
            {
                .message = fmt::format("cannot reconstruct column storage counters for relation {}",
                                       rel->rd_id),
                .detail = "A reconstructed stripe, row, or logical-offset "
                          "counter would overflow its 64-bit storage field.",
            });
    }

    update_column_storage_format(
        rel, is_upgrade, *next_stripe_id, *next_row_number, *next_logical_offset);
}

/**
 * @brief Find the highest row number in the visible stripe metadata.
 *
 * This rebuilds the metapage counter during a storage-format update.
 * Return invalid_column_row_number when there are no stripes.
 */
uint64_t find_highest_used_row_number(uint64_t storage_id) {
    uint64_t highest_row_number = invalid_column_row_number;

    List* stripe_metadata_list =
        read_data_file_stripes(storage_id, GetTransactionSnapshot(), ForwardScanDirection);
    for (stripe_metadata* stripe :
         bioseq::postgres::support::postgres_list<stripe_metadata>{stripe_metadata_list}) {
        highest_row_number = std::max(highest_row_number, stripe_highest_row_number(stripe));
    }

    return highest_row_number;
}

/** @brief Update a stripe's location and counts after VACUUM moves its data. */
export stripe_metadata* rewrite_stripe_metadata(Relation rel,
                                                uint64_t stripe_id,
                                                uint64_t size_bytes,
                                                uint64_t file_offset,
                                                uint64_t row_count,
                                                uint64_t chunk_count) {
    uint64_t storage_id = column_storage_id(rel, false);

    stripe_codec::update update{};
    update.set<&stripe_row::file_offset>(static_cast<std::int64_t>(file_offset))
        .set<&stripe_row::data_length>(static_cast<std::int64_t>(size_bytes))
        .set<&stripe_row::row_count>(static_cast<std::int64_t>(row_count))
        .set<&stripe_row::chunk_group_count>(static_cast<std::int32_t>(chunk_count));

    return update_stripe_metadata_row(storage_id, stripe_id, update);
}

// Read sequence-shard registrations through PostgreSQL catalog scans.

/** @brief A registered shard for one sequence column and length bucket.
 *
 * Its key identifies the parent relation, column, bucket and shard number.
 * The row stores the shard and sidecar relation OIDs, byte counts and full flag.
 */
export struct sequence_sub_table_row {
    Oid database_oid = InvalidOid;
    Oid tablespace_oid = InvalidOid;
    Oid main_relfilenode = InvalidOid;
    int16_t column_attno = 0;
    int16_t bucket_idx = 0;
    int32_t shard_idx = 0;
    Oid sub_table_relid = InvalidOid;
    Oid sidecar_relid = InvalidOid;
    std::uint64_t bytes_written = 0;
    bool is_full = false;
    std::uint64_t total_payload_bytes = 0;

    /// @brief Default-constructs a row with invalid OIDs and zero byte counters.
    sequence_sub_table_row() = default;

    /// @brief Decodes field values from one seq_sub_table heap tuple.
    sequence_sub_table_row(HeapTuple heap_tuple, TupleDesc tuple_descriptor) {
        const seq_sub_table_row row =
            seq_sub_table_codec::deform(heap_tuple, tuple_descriptor, CurrentMemoryContext);
        database_oid = row.database_oid;
        tablespace_oid = row.tablespace_oid;
        main_relfilenode = row.main_relfilenode;
        column_attno = row.column_attno;
        bucket_idx = row.bucket_idx;
        shard_idx = row.shard_idx;
        sub_table_relid = row.sub_table_relid;
        sidecar_relid = row.sidecar_relid;
        bytes_written = static_cast<std::uint64_t>(row.bytes_written);
        is_full = row.is_full;
        total_payload_bytes = static_cast<std::uint64_t>(row.total_payload_bytes);
    }
};

/** @brief Find and update sequence-shard registrations in bioseq_internal.seq_sub_table.
 * Catalog scans and writes follow PostgreSQL transaction visibility.
 */
export struct sequence_sub_table_manager {
    // Look up the registry table and its indexes.
    /// @brief Returns the OID of bioseq_internal.seq_sub_table, resolved per call.
    [[gnu::always_inline]] static inline Oid relid() {
        return get_relname_relid("seq_sub_table", column_namespace_oid());
    }
    /// @brief Returns the OID of seq_sub_table_pkey.
    [[gnu::always_inline]] static inline Oid pkey_index_relid() {
        return get_relname_relid("seq_sub_table_pkey", column_namespace_oid());
    }
    /// @brief Returns the OID of seq_sub_table_active_shard_idx.
    [[gnu::always_inline]] static inline Oid active_shard_index_relid() {
        return get_relname_relid("seq_sub_table_active_shard_idx", column_namespace_oid());
    }

    /// @brief Find a registered non-full shard for this length bucket.
    [[nodiscard]] static std::optional<sequence_sub_table_row>
    lookup_active_shard(Oid database_oid,
                        Oid tablespace_oid,
                        Oid main_relfilenode,
                        int16_t column_attno,
                        int16_t bucket_idx) {
        const Oid rel_oid = relid();
        if (!OidIsValid(rel_oid)) {
            return std::nullopt;
        }
        Relation rel = bioseq::postgres::support::open_postgres_relation(rel_oid, AccessShareLock);
        TupleDesc tuple_descriptor = RelationGetDescr(rel);

        const Oid idx_oid = active_shard_index_relid();
        const bool idx_ok = OidIsValid(idx_oid);

        std::array<ScanKeyData, 5> keys{
            seq_sub_table_codec::scan_key<&seq_sub_table_row::database_oid>(database_oid),
            seq_sub_table_codec::scan_key<&seq_sub_table_row::tablespace_oid>(tablespace_oid),
            seq_sub_table_codec::scan_key<&seq_sub_table_row::main_relfilenode>(main_relfilenode),
            seq_sub_table_codec::scan_key<&seq_sub_table_row::column_attno>(column_attno),
            seq_sub_table_codec::scan_key<&seq_sub_table_row::bucket_idx>(bucket_idx),
        };

        SysScanDesc scan = systable_beginscan(
            rel, idx_oid, idx_ok, nullptr, static_cast<int>(keys.size()), keys.data());
        std::optional<sequence_sub_table_row> result;
        HeapTuple heap_tuple = nullptr;
        while (HeapTupleIsValid(heap_tuple = systable_getnext(scan))) {
            sequence_sub_table_row row(heap_tuple, tuple_descriptor);
            if (!row.is_full) {
                result = row;
                break;
            }
        }
        systable_endscan(scan);
        table_close(rel, AccessShareLock);
        return result;
    }

    /** @brief Find the registration for one bucket and shard number.
     * Return no value if the registry table or matching row is absent.
     */
    [[nodiscard]] static std::optional<sequence_sub_table_row> lookup_shard(Oid database_oid,
                                                                            Oid tablespace_oid,
                                                                            Oid main_relfilenode,
                                                                            int16_t column_attno,
                                                                            int16_t bucket_idx,
                                                                            int32_t shard_idx) {
        const Oid rel_oid = relid();
        if (!OidIsValid(rel_oid)) {
            return std::nullopt;
        }
        Relation rel = bioseq::postgres::support::open_postgres_relation(rel_oid, AccessShareLock);
        TupleDesc tuple_descriptor = RelationGetDescr(rel);

        const Oid idx_oid = pkey_index_relid();
        const bool idx_ok = OidIsValid(idx_oid);

        std::array<ScanKeyData, 6> keys{
            seq_sub_table_codec::scan_key<&seq_sub_table_row::database_oid>(database_oid),
            seq_sub_table_codec::scan_key<&seq_sub_table_row::tablespace_oid>(tablespace_oid),
            seq_sub_table_codec::scan_key<&seq_sub_table_row::main_relfilenode>(main_relfilenode),
            seq_sub_table_codec::scan_key<&seq_sub_table_row::column_attno>(column_attno),
            seq_sub_table_codec::scan_key<&seq_sub_table_row::bucket_idx>(bucket_idx),
            seq_sub_table_codec::scan_key<&seq_sub_table_row::shard_idx>(shard_idx),
        };

        SysScanDesc scan = systable_beginscan(
            rel, idx_oid, idx_ok, nullptr, static_cast<int>(keys.size()), keys.data());
        std::optional<sequence_sub_table_row> result;
        HeapTuple heap_tuple = systable_getnext(scan);
        if (HeapTupleIsValid(heap_tuple)) {
            result = sequence_sub_table_row(heap_tuple, tuple_descriptor);
        }
        systable_endscan(scan);
        table_close(rel, AccessShareLock);
        return result;
    }

    /** @brief Register a new shard unless this bucket already has an active one.
     * Return the existing active row when the initial lookup finds it.
     * A missing registry or a conflicting insert raises a PostgreSQL error.
     */
    [[nodiscard]] static sequence_sub_table_row insert_shard(Oid database_oid,
                                                             Oid tablespace_oid,
                                                             Oid main_relfilenode,
                                                             int16_t column_attno,
                                                             int16_t bucket_idx,
                                                             int32_t shard_idx,
                                                             Oid sub_table_relid,
                                                             Oid sidecar_relid) {
        const Oid rel_oid = relid();
        if (!OidIsValid(rel_oid)) {
            bioseq::postgres::support::raise_sql_error(
                ERRCODE_INTERNAL_ERROR,
                {
                    .message = "bioseq storage: seq_sub_table catalog not present",
                });
        }

        // Reuse an active shard already visible for this parent column and bucket.
        {
            std::optional<sequence_sub_table_row> existing = lookup_active_shard(
                database_oid, tablespace_oid, main_relfilenode, column_attno, bucket_idx);
            if (existing.has_value() && existing->shard_idx == shard_idx) {
                return *existing;
            }
            if (existing.has_value() && existing->shard_idx != shard_idx) {
                return *existing;
            }
        }

        Relation rel = bioseq::postgres::support::open_postgres_relation(rel_oid, RowExclusiveLock);
        TupleDesc tuple_descriptor = RelationGetDescr(rel);

        const seq_sub_table_row catalog_row{
            .database_oid = database_oid,
            .tablespace_oid = tablespace_oid,
            .main_relfilenode = main_relfilenode,
            .column_attno = column_attno,
            .bucket_idx = bucket_idx,
            .shard_idx = shard_idx,
            .sub_table_relid = sub_table_relid,
            .sidecar_relid = sidecar_relid,
            .bytes_written = 0,
            .is_full = false,
            .total_payload_bytes = 0,
        };
        // Use the executor-backed writer to maintain the active-shard partial index.
        // It checks the index predicate when inserting the new row.
        auto encoded = seq_sub_table_codec::encode(catalog_row, CurrentMemoryContext);
        const auto tuple_data = encoded.tuple_data(tuple_descriptor);
        auto* modification =
            bioseq::storage::metadata::tuple_ops::begin_relation_modification(rel);
        bioseq::storage::metadata::tuple_ops::insert_catalog_tuple(
            modification, tuple_data.values, tuple_data.nulls);
        bioseq::storage::metadata::tuple_ops::finish_relation_modification(modification);
        table_close(rel, RowExclusiveLock);

        sequence_sub_table_row row;
        row.database_oid = database_oid;
        row.tablespace_oid = tablespace_oid;
        row.main_relfilenode = main_relfilenode;
        row.column_attno = column_attno;
        row.bucket_idx = bucket_idx;
        row.shard_idx = shard_idx;
        row.sub_table_relid = sub_table_relid;
        row.sidecar_relid = sidecar_relid;
        row.bytes_written = 0;
        row.is_full = false;
        row.total_payload_bytes = 0;
        return row;
    }

    /** @brief Add bytes to a shard's counters and mark it full at a nonzero cap.
     * A zero cap leaves the full flag unchanged. Sequence insertion handles
     * that mode's shard rollover. Missing registry rows raise an error.
     */
    static sequence_sub_table_row update_shard_bytes_written(Oid database_oid,
                                                             Oid tablespace_oid,
                                                             Oid main_relfilenode,
                                                             int16_t column_attno,
                                                             int16_t bucket_idx,
                                                             int32_t shard_idx,
                                                             std::uint64_t delta_bytes,
                                                             std::uint64_t cap_bytes) {
        const Oid rel_oid = relid();
        const Oid idx_oid = pkey_index_relid();
        if (!OidIsValid(rel_oid)) {
            bioseq::postgres::support::raise_sql_error(
                ERRCODE_INTERNAL_ERROR,
                {
                    .message = "bioseq storage: seq_sub_table catalog not present",
                });
        }

        Relation rel = bioseq::postgres::support::open_postgres_relation(rel_oid, RowExclusiveLock);
        TupleDesc tuple_descriptor = RelationGetDescr(rel);

        std::array<ScanKeyData, 6> keys{
            seq_sub_table_codec::scan_key<&seq_sub_table_row::database_oid>(database_oid),
            seq_sub_table_codec::scan_key<&seq_sub_table_row::tablespace_oid>(tablespace_oid),
            seq_sub_table_codec::scan_key<&seq_sub_table_row::main_relfilenode>(main_relfilenode),
            seq_sub_table_codec::scan_key<&seq_sub_table_row::column_attno>(column_attno),
            seq_sub_table_codec::scan_key<&seq_sub_table_row::bucket_idx>(bucket_idx),
            seq_sub_table_codec::scan_key<&seq_sub_table_row::shard_idx>(shard_idx),
        };

        SysScanDesc scan = systable_beginscan(
            rel, idx_oid, OidIsValid(idx_oid), nullptr, static_cast<int>(keys.size()), keys.data());
        HeapTuple old_tuple = systable_getnext(scan);
        if (!HeapTupleIsValid(old_tuple)) {
            systable_endscan(scan);
            table_close(rel, RowExclusiveLock);
            bioseq::postgres::support::raise_sql_error(
                ERRCODE_INTERNAL_ERROR,
                {
                    .message = fmt::format("bioseq storage: cannot update missing sub-table "
                                           "shard (db={}, spc={}, relfilenode={}, attno={}, "
                                           "bucket={}, shard={})",
                                           static_cast<unsigned>(database_oid),
                                           static_cast<unsigned>(tablespace_oid),
                                           static_cast<unsigned>(main_relfilenode),
                                           static_cast<int>(column_attno),
                                           static_cast<int>(bucket_idx),
                                           static_cast<int>(shard_idx)),
                });
        }

        sequence_sub_table_row current(old_tuple, tuple_descriptor);
        const std::uint64_t new_bytes_written = current.bytes_written + delta_bytes;
        const std::uint64_t new_total_payload = current.total_payload_bytes + delta_bytes;
        /* With a zero cap, flushing byte counts does not mark the shard full.
         * Sequence insertion closes it when the next shard is needed. */
        const bool now_full = current.is_full || (cap_bytes != 0 && new_bytes_written >= cap_bytes);

        seq_sub_table_codec::update update{};
        update.set<&seq_sub_table_row::bytes_written>(static_cast<std::int64_t>(new_bytes_written))
            .set<&seq_sub_table_row::total_payload_bytes>(
                static_cast<std::int64_t>(new_total_payload))
            .set<&seq_sub_table_row::is_full>(now_full);

        HeapTuple new_tuple = update.modify(old_tuple, tuple_descriptor);
        auto* modification =
            bioseq::storage::metadata::tuple_ops::begin_relation_modification(rel);
        bioseq::storage::metadata::tuple_ops::update_catalog_tuple(
            modification, old_tuple, new_tuple);
        systable_endscan(scan);
        bioseq::storage::metadata::tuple_ops::finish_relation_modification(modification);
        heap_freetuple(new_tuple);
        table_close(rel, RowExclusiveLock);

        current.bytes_written = new_bytes_written;
        current.total_payload_bytes = new_total_payload;
        current.is_full = now_full;
        return current;
    }

    /** @brief List a parent relation's shards across all columns and buckets.
     * Scan the registry by database, tablespace and parent file number.
     * Return an empty vector if the registry table is absent.
     */
    [[nodiscard]] static std::vector<sequence_sub_table_row>
    list_shards_for_main(Oid database_oid, Oid tablespace_oid, Oid main_relfilenode) {
        std::vector<sequence_sub_table_row> shards;
        const Oid rel_oid = bioseq::postgres::support::invoke_pg_as_cpp([] { return relid(); });
        if (!OidIsValid(rel_oid)) {
            return shards;
        }
        Relation rel = bioseq::postgres::support::invoke_pg_as_cpp([rel_oid] {
            return bioseq::postgres::support::try_open_postgres_relation(rel_oid, AccessShareLock);
        });
        if (rel == nullptr) {
            return shards;
        }
        TupleDesc tuple_descriptor = RelationGetDescr(rel);

        std::array<ScanKeyData, 3> keys{};
        SysScanDesc scan = nullptr;
        std::exception_ptr failure;
        try {
            keys = {
                seq_sub_table_codec::scan_key<&seq_sub_table_row::database_oid>(database_oid),
                seq_sub_table_codec::scan_key<&seq_sub_table_row::tablespace_oid>(tablespace_oid),
                seq_sub_table_codec::scan_key<&seq_sub_table_row::main_relfilenode>(
                    main_relfilenode),
            };
            scan = bioseq::postgres::support::invoke_pg_as_cpp([rel, &keys] {
                return systable_beginscan(
                    rel, InvalidOid, false, nullptr, static_cast<int>(keys.size()), keys.data());
            });
            for (;;) {
                HeapTuple tuple = bioseq::postgres::support::invoke_pg_as_cpp(
                    [scan] { return systable_getnext(scan); });
                if (!HeapTupleIsValid(tuple))
                    break;
                sequence_sub_table_row row =
                    bioseq::postgres::support::invoke_pg_as_cpp([tuple, tuple_descriptor] {
                        return sequence_sub_table_row(tuple, tuple_descriptor);
                    });
                shards.push_back(std::move(row));
            }
        } catch (...) {
            failure = std::current_exception();
        }
        if (scan != nullptr) {
            record_cleanup_failure(failure, [scan] {
                bioseq::postgres::support::invoke_pg_as_cpp([scan] { systable_endscan(scan); });
            });
        }
        record_cleanup_failure(failure, [rel] {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [rel] { table_close(rel, AccessShareLock); });
        });
        if (failure != nullptr)
            std::rethrow_exception(failure);
        return shards;
    }

    /// @brief Delete a parent relation's shard registrations. Return if the registry
    /// table is absent. The physical shard tables are left to the caller.
    static void delete_shards_for_main(Oid database_oid, Oid tablespace_oid, Oid main_relfilenode) {
        const Oid rel_oid = relid();
        if (!OidIsValid(rel_oid)) {
            return;
        }
        Relation rel =
            bioseq::postgres::support::try_open_postgres_relation(rel_oid, RowExclusiveLock);
        if (rel == nullptr) {
            return;
        }

        std::array<ScanKeyData, 3> keys{
            seq_sub_table_codec::scan_key<&seq_sub_table_row::database_oid>(database_oid),
            seq_sub_table_codec::scan_key<&seq_sub_table_row::tablespace_oid>(tablespace_oid),
            seq_sub_table_codec::scan_key<&seq_sub_table_row::main_relfilenode>(main_relfilenode),
        };

        SysScanDesc scan = systable_beginscan(
            rel, InvalidOid, false, nullptr, static_cast<int>(keys.size()), keys.data());
        HeapTuple heap_tuple = nullptr;
        while (HeapTupleIsValid(heap_tuple = systable_getnext(scan))) {
            CatalogTupleDelete(rel, &heap_tuple->t_self);
        }
        systable_endscan(scan);
        table_close(rel, RowExclusiveLock);
        CommandCounterIncrement();
    }
};
