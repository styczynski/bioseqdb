/**
 * @file stripe.cppm
 * @brief Storage-id, stripe, chunk-group, and skip-list metadata operations.
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

/// @brief Writes chunk skip-list rows to bioseq.column_chunk for one stripe.
/// @param relfilelocator Resolves storage_id for the encoded catalog rows.
/// @param stripe Stripe number stored on each inserted chunk row.
/// @param chunk_list Per-column chunk skip nodes and column or chunk counts.
/// @param tuple_descriptor Column types used to encode optional min and max bytea values.
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
                                    .to_bytea(TupleDescAttr(tuple_descriptor, column_index));
                maximum_value = bioseq::postgres::support::pg_datum::from_raw(chunk->maximum_value)
                                    .to_bytea(TupleDescAttr(tuple_descriptor, column_index));
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

/*
 * save_chunk_groups saves the metadata for given chunk groups in bioseq.chunk_group.
 */
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

/*
 * save_empty_row_mask saves the metadata for inserted rows in bioseq.mask_row
 */

/// @brief Returns stripe metadata for the stripe whose first_row_number exceeds row_number.
/// @param relation Column table whose stripe catalog rows are scanned.
/// @param row_number Row number used as an exclusive lower bound on first_row_number.
/// @param snapshot Visibility snapshot for the ordered index scan.
/// @return Next stripe metadata, or nullptr when no later stripe exists.
export stripe_metadata*
find_next_stripe_by_row_number(Relation relation, uint64_t row_number, Snapshot snapshot) {
    return find_stripe_metadata_by_row_number(
        relation, row_number, snapshot, row_number_lookup_mode::greater);
}

/*
 * find_stripe_by_row_number returns stripe_metadata for the stripe that contains
 * the row with row_number. If no such stripe exists, then returns null.
 */
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

/*
 * find_stripe_by_first_row_number returns a stripe_metadata object for
 * the stripe that has the greatest first_row_number among the stripes whose
 * first_row_number is smaller than or equal to given row_number. If no such
 * stripe exists, then returns null.
 *
 * Note that this doesn't mean that found stripe certainly contains the tuple
 * with given row_number. This is because, it also needs to be verified if
 * highest row number that found stripe contains is greater than or equal to
 * given row_number. For this reason, unless that additional check is done,
 * this function is mostly useful for checking against "possible" constraint
 * violations due to concurrent writes that are not flushed by other backends
 * yet.
 */
stripe_metadata*
find_stripe_by_first_row_number(Relation relation, uint64_t row_number, Snapshot snapshot) {
    return find_stripe_metadata_by_row_number(
        relation, row_number, snapshot, row_number_lookup_mode::less_or_equal);
}

/* find_next_stripe_for_parallel_worker returns next stripe that should be assigned
 * for worker. Approach here is to calculate module of stripe id with total number
 * of workers that are running for execution.
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

/*
 * stripe_write_state_for returns write state of given stripe.
 */
export stripe_write_state stripe_write_state_for(stripe_metadata* stripe) {
    if (stripe->aborted) {
        return stripe_write_state::aborted;
    } else if (stripe->row_count > 0) {
        return stripe_write_state::flushed;
    } else {
        return stripe_write_state::in_progress;
    }
}

/*
 * stripe_highest_row_number returns row_number of the row with highest
 * row_number in given stripe.
 */
uint64_t stripe_highest_row_number(stripe_metadata* stripe) {
    return stripe->first_row_number + stripe->row_count - 1;
}

/*
 * find_stripe_metadata_by_row_number returns stripe_metadata for the stripe whose
 * first_row_number is less than or equal to (row_number_lookup_mode::less_or_equal), or is
 * greater than (row_number_lookup_mode::greater) given row_number by doing backward index
 * scan on stripe_first_row_number_idx.
 * If no such stripe exists, then returns null.
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

    /*
     * Even if stripe is flushed, file_offset and data_length might be equal
     * to 0 for zero column tables, but those two should still be consistent
     * with respect to each other.
     */
    bool stripe_looks_flushed =
        stripe->row_count > 0 && stripe->chunk_count > 0 &&
        ((stripe->file_offset != invalid_logical_offset && stripe->data_length > 0) ||
         (stripe->file_offset == invalid_logical_offset && stripe->data_length == 0));

    const stripe_write_state write_state = stripe_write_state_for(stripe);
    if (write_state == stripe_write_state::flushed && stripe_looks_flushed) {
        /*
         * If stripe was flushed to disk, then we expect stripe to store
         * at least one tuple.
         */
        return;
    } else if (write_state == stripe_write_state::in_progress && stripe_looks_in_progress) {
        /*
         * If stripe was not flushed to disk, then values of given four
         * fields should match the columns inserted by
         * insert_empty_stripe_metadata_row.
         */
        return;
    } else if (write_state == stripe_write_state::aborted &&
               (stripe_looks_in_progress || stripe_looks_flushed)) {
        /*
         * Stripe metadata entry for an aborted write can be complete or
         * incomplete. We might have aborted the transaction before or after
         * inserting into stripe metadata.
         */
        return;
    }

    bioseq_log<logging_level::error, logging_target::postgres>(
        "unexpected stripe state, stripe metadata entry for stripe with id={} is not consistent",
        stripe->id);
}

/*
 * find_highest_row_number_stripe returns stripe_metadata for the stripe that
 * has the row with highest row_number by doing backward index scan on
 * stripe_first_row_number_idx. If given relation is empty, then returns null.
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

/*
 * read_chunk_group_row_counts returns an array of row counts of chunk groups and
 * deleted rows count for each chunk group for given stripe.
 * Arrays that are updated will be allocated in this function.
 */
void read_chunk_group_row_counts(uint64_t storage_id,
                                 uint64_t stripe,
                                 uint32_t chunk_group_count,
                                 uint32_t** chunk_group_row_counts,
                                 uint32_t** chunk_group_deleted_rows,
                                 Snapshot snapshot) {
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

    /*
     * The caller's snapshot selects the one visible version of each chunk-group
     * row.  A tuple that is HOT-updated can still be exactly the version visible
     * to an older REPEATABLE READ snapshot; filtering it by the physical
     * HEAP_HOT_UPDATED flag would incorrectly hide pre-update source rows.
     */
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
    index_close(index, AccessShareLock);
    table_close(bioseq_column_chunk_group, AccessShareLock);
}

/*
 * update_chunk_group_deleted_rows updates `deleted_rows` column for each chunk group.
 */
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

/*
 * insert_empty_stripe_metadata_row adds a row to bioseq.stripe for the empty
 * stripe reservation made for stripe_id.
 */
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
