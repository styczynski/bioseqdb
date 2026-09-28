/**
 * @file stripe.cppm
 * @brief Store chunk statistics and look up columnar stripes by row or stripe id
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

export module bioseq.postgres.storage.metadata:stripe;

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

/// @brief Save a stripe's chunk locations and statistics in bioseq_internal.chunk.
/// @param relfilelocator Physical relation used to find the storage id.
/// @param stripe Stripe number for the new catalog rows.
/// @param chunk_list Chunk metadata for each column  and chunk group.
/// @param tuple_descriptor Column types used to encode optional min/max values.
export void save_stripe_skip_list(RelFileLocator relfilelocator,
                                  uint64_t stripe,
                                  stripe_skip_list* chunk_list,
                                  TupleDesc tuple_descriptor) {
    uint32_t column_index = 0;
    uint32_t chunk_index = 0;
    uint32_t column_count = chunk_list->column_count;

    uint64_t storage_id = lookup_storage_id(relfilelocator);
    Oid bioseq_column_chunk_oid = column_chunk_relation_oid();
    Relation bioseq_column_chunk = table_open(bioseq_column_chunk_oid, RowExclusiveLock);
    bioseq::storage::metadata::tuple_ops::relation_modification_state* relation_modification_state =
        bioseq::storage::metadata::tuple_ops::begin_relation_modification(bioseq_column_chunk);

    for (column_index = 0; column_index < column_count; column_index++) {
        for (chunk_index = 0; chunk_index < chunk_list->chunk_count; chunk_index++) {
            chunk_skip_node* chunk = &chunk_list->chunk_skip_nodes[column_index][chunk_index];

            std::optional<bytea*> minimum_value;
            std::optional<bytea*> maximum_value;
            if (chunk->has_min_max) {
                minimum_value = bioseq::postgres::support::pg_datum::from_raw(chunk->minimum_value)
                                    .to_bytea(bioseq::postgres::support::tuple_descriptor_attribute(tuple_descriptor, column_index));
                maximum_value = bioseq::postgres::support::pg_datum::from_raw(chunk->maximum_value)
                                    .to_bytea(bioseq::postgres::support::tuple_descriptor_attribute(tuple_descriptor, column_index));
            }
            chunk_codec::encoded_row encoded = chunk_codec::encode(
                chunk_row{
                    .storage_id = static_cast<std::int64_t>(storage_id),
                    .stripe_num = static_cast<std::int64_t>(stripe),
                    .attr_num = static_cast<std::int32_t>(column_index + 1),
                    .chunk_group_num = static_cast<std::int32_t>(chunk_index),
                    .minimum_value = minimum_value,
                    .maximum_value = maximum_value,
                    .value_stream_offset = static_cast<std::int64_t>(chunk->value_stream_offset),
                    .value_stream_length = static_cast<std::int64_t>(chunk->value_stream_length),
                    .exists_stream_offset = static_cast<std::int64_t>(chunk->exists_stream_offset),
                    .exists_stream_length = static_cast<std::int64_t>(chunk->exists_stream_length),
                    .value_compression_type =
                        static_cast<std::int32_t>(chunk->value_compression_algorithm),
                    .value_compression_level =
                        static_cast<std::int32_t>(chunk->value_compression_level),
                    .value_decompressed_length =
                        static_cast<std::int64_t>(chunk->value_decompressed_length),
                    .value_count = static_cast<std::int64_t>(chunk->row_count),
                },
                CurrentMemoryContext);
            const chunk_codec::encoded_tuple_data tuple_data =
                encoded.tuple_data(RelationGetDescr(bioseq_column_chunk));

            bioseq::storage::metadata::tuple_ops::insert_catalog_tuple(
                relation_modification_state, tuple_data.values, tuple_data.nulls);
        }
    }

    bioseq::storage::metadata::tuple_ops::finish_relation_modification(relation_modification_state);
    table_close(bioseq_column_chunk, RowExclusiveLock);
}

/** @brief Save chunk-group row counts with initial deleted-row counts of zero. */
export void
save_chunk_groups(RelFileLocator relfilelocator, uint64_t stripe, List* chunk_group_row_counts) {
    uint64_t storage_id = lookup_storage_id(relfilelocator);
    Oid bioseq_column_chunk_group_oid = column_chunk_group_relation_oid();
    Relation bioseq_column_chunk_group =
        table_open(bioseq_column_chunk_group_oid, RowExclusiveLock);
    bioseq::storage::metadata::tuple_ops::relation_modification_state* relation_modification_state =
        bioseq::storage::metadata::tuple_ops::begin_relation_modification(
            bioseq_column_chunk_group);

    int chunk_id = 0;

    for (int row_count : bioseq::postgres::support::postgres_integer_list{chunk_group_row_counts}) {
        chunk_group_codec::encoded_row encoded = chunk_group_codec::encode(chunk_group_row{
            .storage_id = static_cast<std::int64_t>(storage_id),
            .stripe_num = static_cast<std::int64_t>(stripe),
            .chunk_group_num = static_cast<std::int32_t>(chunk_id),
            .row_count = static_cast<std::int64_t>(row_count),
        });
        const chunk_group_codec::encoded_tuple_data tuple_data =
            encoded.tuple_data(RelationGetDescr(bioseq_column_chunk_group));

        bioseq::storage::metadata::tuple_ops::insert_catalog_tuple(
            relation_modification_state, tuple_data.values, tuple_data.nulls);
        chunk_id++;
    }

    bioseq::storage::metadata::tuple_ops::finish_relation_modification(relation_modification_state);
    table_close(bioseq_column_chunk_group, RowExclusiveLock);
}

// Stripe lookups by logical row number.

/// @brief Find the first stripe that starts after a logical row number.
/// @param relation Columnar table to search.
/// @param row_number Exclusive lower bound on the stripe's first row.
/// @param snapshot Snapshot used to read metadata.
/// @return Stripe metadata, or nullptr if no later stripe exists.
export stripe_metadata*
find_next_stripe_by_row_number(Relation relation, uint64_t row_number, Snapshot snapshot) {
    return find_stripe_metadata_by_row_number(
        relation, row_number, snapshot, row_number_lookup_mode::greater);
}

/** @brief Find the stripe containing a logical row number, or return nullptr. */
export stripe_metadata*
find_stripe_by_row_number(Relation relation, uint64_t row_number, Snapshot snapshot) {
    stripe_metadata* stripe = find_stripe_by_first_row_number(relation, row_number, snapshot);
    if (!stripe) {
        return nullptr;
    }

    if (row_number > stripe_highest_row_number(stripe)) {
        return nullptr;
    }

    return stripe;
}

/**
 * @brief Find the stripe with the latest start  at or before a row number.
 *
 * Check stripe_highest_row_number() separately to confirm that the row is
 * inside the stripe. Looking up the start alone also finds unflushed
 * reservations when checking for conflicts with concurrent writers.
 */
stripe_metadata*
find_stripe_by_first_row_number(Relation relation, uint64_t row_number, Snapshot snapshot) {
    return find_stripe_metadata_by_row_number(
        relation, row_number, snapshot, row_number_lookup_mode::less_or_equal);
}

/** @brief Find the firsc visible stripe at or after next_stripe_id.
 * Return its id through next_higher_stripe_id so the caller can atomically
 * claim it. Missing ids left by aborted writes are skipped.
 */

export stripe_metadata* find_next_stripe_for_parallel_worker(Relation relation,
                                                             Snapshot snapshot,
                                                             uint64_t next_stripe_id,
                                                             uint64_t* next_higher_stripe_id) {
    stripe_metadata* found_stripe_metadata = nullptr;

    uint64_t storage_id = column_storage_id(relation, false);
    ScanKeyData scan_keys[2];

    scan_keys[0] =
        stripe_codec::scan_key<&stripe_row::storage_id>(static_cast<std::int64_t>(storage_id));
    scan_keys[1] =
        stripe_codec::scan_key<&stripe_row::stripe_num,
                               bioseq::postgres::support::scan_comparison::greater_equal>(
            static_cast<std::int64_t>(next_stripe_id));

    Relation bioseq_column_stripes = table_open(column_stripe_relation_oid(), AccessShareLock);

    Relation index = index_open(column_stripe_primary_key_index_oid(), AccessShareLock);

    SysScanDesc scan_descriptor =
        systable_beginscan_ordered(bioseq_column_stripes, index, snapshot, 2, scan_keys);

    HeapTuple heap_tuple = systable_getnext_ordered(scan_descriptor, ForwardScanDirection);
    if (HeapTupleIsValid(heap_tuple)) {
        found_stripe_metadata = build_stripe_metadata(bioseq_column_stripes, heap_tuple);
        *next_higher_stripe_id = found_stripe_metadata->id;
    }

    systable_endscan_ordered(scan_descriptor);
    index_close(index, AccessShareLock);
    table_close(bioseq_column_stripes, AccessShareLock);

    return found_stripe_metadata;
}

/** @brief Classify a stripe as aborted, flushed or still being written */
export stripe_write_state stripe_write_state_for(stripe_metadata* stripe) {
    if (stripe->aborted) {
        return stripe_write_state::aborted;
    } else if (stripe->row_count > 0) {
        return stripe_write_state::flushed;
    } else {
        return stripe_write_state::in_progress;
    }
}

/** @brief Return the  inclusive last logical row number of a nonempty stripe. */
uint64_t stripe_highest_row_number(stripe_metadata* stripe) {
    return stripe->first_row_number + stripe->row_count - 1;
}

/** @brief Find the nearest stripe start on the requested side of a row number.
 * Search backward for less_or_equal and forward for greater. Return nullptr
 * when the snapshot contains no matching stripe.
 */
stripe_metadata* find_stripe_metadata_by_row_number(Relation relation,
                                                    uint64_t row_number,
                                                    Snapshot snapshot,
                                                    row_number_lookup_mode lookup_mode) {
    Assert(lookup_mode == row_number_lookup_mode::less_or_equal ||
           lookup_mode == row_number_lookup_mode::greater);

    stripe_metadata* found_stripe_metadata = nullptr;

    uint64_t storage_id = column_storage_id(relation, false);
    ScanKeyData scan_keys[2];
    scan_keys[0] =
        stripe_codec::scan_key<&stripe_row::storage_id>(static_cast<std::int64_t>(storage_id));

    if (lookup_mode == row_number_lookup_mode::less_or_equal) {
        scan_keys[1] =
            stripe_codec::scan_key<&stripe_row::first_row_number,
                                   bioseq::postgres::support::scan_comparison::less_equal>(
                static_cast<std::int64_t>(row_number));
    } else if (lookup_mode == row_number_lookup_mode::greater) {
        scan_keys[1] = stripe_codec::scan_key<&stripe_row::first_row_number,
                                              bioseq::postgres::support::scan_comparison::greater>(
            static_cast<std::int64_t>(row_number));
    }

    Relation bioseq_column_stripes = table_open(column_stripe_relation_oid(), AccessShareLock);
    Relation index = index_open(stripe_first_row_number_index_oid(), AccessShareLock);
    SysScanDesc scan_descriptor =
        systable_beginscan_ordered(bioseq_column_stripes, index, snapshot, 2, scan_keys);

    ScanDirection scan_direction = NoMovementScanDirection;
    if (lookup_mode == row_number_lookup_mode::less_or_equal) {
        scan_direction = BackwardScanDirection;
    } else if (lookup_mode == row_number_lookup_mode::greater) {
        scan_direction = ForwardScanDirection;
    }
    HeapTuple heap_tuple = systable_getnext_ordered(scan_descriptor, scan_direction);
    if (HeapTupleIsValid(heap_tuple)) {
        found_stripe_metadata = build_stripe_metadata(bioseq_column_stripes, heap_tuple);
    }

    systable_endscan_ordered(scan_descriptor);
    index_close(index, AccessShareLock);
    table_close(bioseq_column_stripes, AccessShareLock);

    return found_stripe_metadata;
}

/// @brief Rejects catalog fields that contradict a stripe's write state.
void validate_stripe_metadata(stripe_metadata* stripe) {
    bool stripe_looks_in_progress = stripe->row_count == 0 && stripe->chunk_count == 0 &&
                                    stripe->file_offset == invalid_logical_offset &&
                                    stripe->data_length == 0;

    /* A table with no columns can have rows without data bytes.
     * In that case, both the offset and length use, their empty values. */
    bool stripe_looks_flushed =
        stripe->row_count > 0 && stripe->chunk_count > 0 &&
        ((stripe->file_offset != invalid_logical_offset && stripe->data_length > 0) ||
         (stripe->file_offset == invalid_logical_offset && stripe->data_length == 0));

    const stripe_write_state write_state = stripe_write_state_for(stripe);
    if (write_state == stripe_write_state::flushed && stripe_looks_flushed) {
        /* A flushed stripe contains at least one row and chunk group. */
        return;
    } else if (write_state == stripe_write_state::in_progress && stripe_looks_in_progress) {
        /* Pending stripes must  match the initial reservation fields. */
        return;
    } else if (write_state == stripe_write_state::aborted &&
               (stripe_looks_in_progress || stripe_looks_flushed)) {
        /* A transaction can abort before or after its stripe is flushed. */
        return;
    }

    bioseq_log<logging_level::error, logging_target::postgres>(
        "unexpected stripe state, stripe metadata entry for stripe with id={} is not consistent",
        stripe->id);
}

/** @brief Find the stripe with the highest starting row number.
 * Return nullptr when no stripe is visible in the supplied snapshot.
 */
export stripe_metadata* find_highest_row_number_stripe(Relation relation, Snapshot snapshot) {
    stripe_metadata* stripe_with_highest_row_number = nullptr;

    uint64_t storage_id = column_storage_id(relation, false);
    ScanKeyData scan_keys[1];
    scan_keys[0] =
        stripe_codec::scan_key<&stripe_row::storage_id>(static_cast<std::int64_t>(storage_id));

    Relation bioseq_column_stripes = table_open(column_stripe_relation_oid(), AccessShareLock);
    Relation index = index_open(stripe_first_row_number_index_oid(), AccessShareLock);
    SysScanDesc scan_descriptor =
        systable_beginscan_ordered(bioseq_column_stripes, index, snapshot, 1, scan_keys);

    HeapTuple heap_tuple = systable_getnext_ordered(scan_descriptor, BackwardScanDirection);
    if (HeapTupleIsValid(heap_tuple)) {
        stripe_with_highest_row_number = build_stripe_metadata(bioseq_column_stripes, heap_tuple);
    }

    systable_endscan_ordered(scan_descriptor);
    index_close(index, AccessShareLock);
    table_close(bioseq_column_stripes, AccessShareLock);

    return stripe_with_highest_row_number;
}

/** @brief Read row and deletion counts for every chunk group in a strpe.
 * Allocate both result arrays in the current memory context. Entries with
 * no visible catalog row remain zero.
 */
void read_chunk_group_row_counts(uint64_t storage_id,
                                 uint64_t stripe,
                                 uint32_t chunk_group_count,
                                 uint32_t** chunk_group_row_counts,
                                 uint32_t** chunk_group_deleted_rows,
                                 Snapshot snapshot) {
    // Register an unowned MVCC snapshot  for this scan. Release it on completion.
    // PostgreSQL's ResourceOwner handles release if an error interrupts the scan.
    const bool retain_snapshot = snapshot != InvalidSnapshot && IsMVCCSnapshot(snapshot) &&
                                 snapshot->regd_count == 0 && snapshot->active_count == 0;
    if (retain_snapshot) {
        snapshot = RegisterSnapshot(snapshot);
    }
    Oid bioseq_column_chunk_group_oid = column_chunk_group_relation_oid();
    Relation bioseq_column_chunk_group = table_open(bioseq_column_chunk_group_oid, AccessShareLock);
    Relation index = index_open(column_chunk_group_index_oid(), AccessShareLock);

    std::array<ScanKeyData, 2> scan_keys{
        chunk_group_codec::scan_key<&chunk_group_row::storage_id>(
            static_cast<std::int64_t>(storage_id)),
        chunk_group_codec::scan_key<&chunk_group_row::stripe_num>(
            static_cast<std::int64_t>(stripe)),
    };

    SysScanDesc scan_descriptor = systable_beginscan_ordered(bioseq_column_chunk_group,
                                                             index,
                                                             snapshot,
                                                             static_cast<int>(scan_keys.size()),
                                                             scan_keys.data());

    HeapTuple heap_tuple = nullptr;

    *chunk_group_row_counts =
        bioseq::postgres::support::allocate_zeroed_array<uint32_t>(chunk_group_count);
    *chunk_group_deleted_rows =
        bioseq::postgres::support::allocate_zeroed_array<uint32_t>(chunk_group_count);

    /* Use the row version selected by the caller's snapshot An older
     * REPEATABLE READ transaction can still see a version marked
     * HEAP_HOT_UPDATED after another transaction updates the row. */
    while (HeapTupleIsValid(heap_tuple =
                                systable_getnext_ordered(scan_descriptor, ForwardScanDirection))) {
        const chunk_group_row row = chunk_group_codec::deform(
            heap_tuple, RelationGetDescr(bioseq_column_chunk_group), CurrentMemoryContext);
        const uint32_t tuple_chunk_group_index = static_cast<uint32_t>(row.chunk_group_num);

        if (tuple_chunk_group_index >= chunk_group_count) {
            bioseq_log<logging_level::warning, logging_target::postgres>(
                "Tuple chunk group higher than chunk group count: {}, {} (storage_id = {}, "
                "stripe_id = {})",
                tuple_chunk_group_index,
                chunk_group_count,
                storage_id,
                stripe);
            continue;
        }

        (*chunk_group_row_counts)[tuple_chunk_group_index] = static_cast<uint32_t>(row.row_count);

        (*chunk_group_deleted_rows)[tuple_chunk_group_index] =
            static_cast<uint32_t>(row.deleted_rows);
    }

    systable_endscan_ordered(scan_descriptor);
    if (retain_snapshot) {
        UnregisterSnapshot(snapshot);
    }
    index_close(index, AccessShareLock);
    table_close(bioseq_column_chunk_group, AccessShareLock);
}

/** @brief Replace the deleted-row count for one chunk group. */
export void update_chunk_group_deleted_rows(uint64_t storage_id,
                                            uint64_t stripe,
                                            uint32_t chunk_group_id,
                                            uint32_t deleted_row_number) {

    HeapTuple old_heap_tuple = nullptr;

    Oid bioseq_column_chunk_group_oid = column_chunk_group_relation_oid();
    Relation bioseq_column_chunk_group = table_open(bioseq_column_chunk_group_oid, AccessShareLock);
    TupleDesc tuple_descriptor = RelationGetDescr(bioseq_column_chunk_group);
    Relation index = index_open(column_chunk_group_index_oid(), AccessShareLock);

    std::array<ScanKeyData, 3> scan_keys{
        chunk_group_codec::scan_key<&chunk_group_row::storage_id>(
            static_cast<std::int64_t>(storage_id)),
        chunk_group_codec::scan_key<&chunk_group_row::stripe_num>(
            static_cast<std::int64_t>(stripe)),
        chunk_group_codec::scan_key<&chunk_group_row::chunk_group_num>(
            static_cast<std::int32_t>(chunk_group_id)),
    };

    SysScanDesc scan_descriptor = systable_beginscan_ordered(bioseq_column_chunk_group,
                                                             index,
                                                             nullptr,
                                                             static_cast<int>(scan_keys.size()),
                                                             scan_keys.data());

    old_heap_tuple = systable_getnext_ordered(scan_descriptor, BackwardScanDirection);

    index_close(index, AccessShareLock);

    if (HeapTupleIsValid(old_heap_tuple)) {
        chunk_group_codec::update update{};
        update.set<&chunk_group_row::deleted_rows>(static_cast<std::int64_t>(deleted_row_number));

        HeapTuple new_heap_tuple = update.modify(old_heap_tuple, tuple_descriptor);

        CatalogTupleUpdate(bioseq_column_chunk_group, &old_heap_tuple->t_self, new_heap_tuple);

        heap_freetuple(new_heap_tuple);
    }

    systable_endscan_ordered(scan_descriptor);
    table_close(bioseq_column_chunk_group, AccessShareLock);

    CommandCounterIncrement();
}

/** @brief Record an empty stripe reservation in bioseq_internal.stripe. */
void insert_empty_stripe_metadata_row(uint64_t storage_id,
                                      uint64_t stripe_id,
                                      uint32_t column_count,
                                      uint32_t chunk_group_row_count,
                                      uint64_t first_row_number) {
    stripe_codec::encoded_row encoded = stripe_codec::encode(stripe_row{
        .storage_id = static_cast<std::int64_t>(storage_id),
        .stripe_num = static_cast<std::int64_t>(stripe_id),
        .file_offset = static_cast<std::int64_t>(invalid_logical_offset),
        .data_length = 0,
        .column_count = static_cast<std::int32_t>(column_count),
        .chunk_row_count = static_cast<std::int32_t>(chunk_group_row_count),
        .row_count = 0,
        .chunk_group_count = 0,
        .first_row_number = static_cast<std::int64_t>(first_row_number),
    });

    Oid bioseq_column_stripes_oid = column_stripe_relation_oid();
    Relation bioseq_column_stripes = table_open(bioseq_column_stripes_oid, RowExclusiveLock);

    bioseq::storage::metadata::tuple_ops::relation_modification_state* relation_modification_state =
        bioseq::storage::metadata::tuple_ops::begin_relation_modification(bioseq_column_stripes);
    const stripe_codec::encoded_tuple_data tuple_data =
        encoded.tuple_data(RelationGetDescr(bioseq_column_stripes));

    bioseq::storage::metadata::tuple_ops::insert_catalog_tuple(
        relation_modification_state, tuple_data.values, tuple_data.nulls);

    bioseq::storage::metadata::tuple_ops::finish_relation_modification(relation_modification_state);

    table_close(bioseq_column_stripes, RowExclusiveLock);
}
