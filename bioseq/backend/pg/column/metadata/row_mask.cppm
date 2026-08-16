/**
 * @file row_mask.cppm
 * @brief Row-mask catalog operations exposed by the metadata module.
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

export module bioseq.postgres.storage.metadata:row_mask;

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

export bool save_empty_row_mask(uint64_t storage_id,
                                uint64_t stripe_id,
                                uint64_t stripe_start_row_number,
                                List* chunk_group_row_counts) {
    Oid row_mask_relation_oid = column_row_mask_relation_oid();
    Oid bioseq_column_row_mask_seq = column_row_mask_sequence_oid();
    Relation row_mask_relation = table_open(row_mask_relation_oid, RowExclusiveLock);
    bioseq::storage::metadata::tuple_ops::relation_modification_state* relation_modification_state =
        bioseq::storage::metadata::tuple_ops::begin_relation_modification(row_mask_relation);

    uint64_t chunk_iteration_start_row_number = stripe_start_row_number;
    uint64_t chunk_iteration_end_row_number = stripe_start_row_number - 1;

    int chunk_id = 0;

    bool chunk_inserted = true;

    for (int row_count : bioseq::postgres::support::postgres_integer_list{chunk_group_row_counts}) {
        if (!chunk_inserted)
            break;

        uint16_t row_mask_iterations = (row_count % row_mask_chunk_row_capacity)
                                           ? (row_count / row_mask_chunk_row_capacity + 1)
                                           : (row_count / row_mask_chunk_row_capacity);

        for (uint16_t n = 0; n < row_mask_iterations; n++) {
            uint16_t mask_size;

            /* Last iteration */
            if (n == (row_mask_iterations - 1)) {
                uint16_t last_iteration_row_size = row_count % row_mask_chunk_row_capacity
                                                       ? row_count % row_mask_chunk_row_capacity
                                                       : row_mask_chunk_row_capacity;
                chunk_iteration_end_row_number += last_iteration_row_size;
                mask_size = (last_iteration_row_size) % 8 ? last_iteration_row_size / 8 + 1
                                                          : last_iteration_row_size / 8;
            } else {
                chunk_iteration_end_row_number += row_mask_chunk_row_capacity;
                mask_size = row_mask_chunk_row_capacity / 8;
            }

            bytea* initial_lookup_record =
                bioseq::postgres::support::allocate_zeroed_storage<bytea>(mask_size + VARHDRSZ);
            SET_VARSIZE(initial_lookup_record, mask_size + VARHDRSZ);

            int64_t next_seq_id = nextval_internal(bioseq_column_row_mask_seq, false);

            row_mask_codec::encoded_row encoded = row_mask_codec::encode(
                row_mask_row{
                    .id = static_cast<std::int64_t>(next_seq_id),
                    .storage_id = static_cast<std::int64_t>(storage_id),
                    .stripe_id = static_cast<std::int64_t>(stripe_id),
                    .chunk_id = static_cast<std::int32_t>(chunk_id),
                    .start_row_number = static_cast<std::int64_t>(chunk_iteration_start_row_number),
                    .end_row_number = static_cast<std::int64_t>(chunk_iteration_end_row_number),
                    .deleted_rows = 0,
                    .mask = initial_lookup_record,
                },
                CurrentMemoryContext);
            const row_mask_codec::encoded_tuple_data tuple_data =
                encoded.tuple_data(RelationGetDescr(row_mask_relation));

            /* A conflicting UNIQUE row means another writer owns this range. */
            PG_TRY();
            {
                bioseq::storage::metadata::tuple_ops::insert_catalog_tuple(
                    relation_modification_state, tuple_data.values, tuple_data.nulls);
            }

            PG_CATCH();
            {
                FlushErrorState();
                chunk_inserted = false;
                break;
            }

            PG_END_TRY();

            chunk_iteration_start_row_number += row_mask_chunk_row_capacity;
        }

        chunk_iteration_start_row_number = chunk_iteration_end_row_number + 1;
        chunk_id++;
    }

    bioseq::storage::metadata::tuple_ops::finish_relation_modification(relation_modification_state);
    table_close(row_mask_relation, RowExclusiveLock);

    return chunk_inserted;
}

/*
 * read_stripe_skip_list fetches chunk metadata for a given stripe.
 */
export stripe_skip_list* read_stripe_skip_list(RelFileLocator relfilelocator,
                                               uint64_t stripe,
                                               TupleDesc tuple_descriptor,
                                               uint32_t chunk_count,
                                               Snapshot snapshot) {
    int32_t column_index = 0;
    int32_t chunk_group_index = 0;
    int32_t chunk_group_row_offset_acc = 0;
    HeapTuple heap_tuple = nullptr;
    uint32_t column_count = tuple_descriptor->natts;
    ScanKeyData scan_keys[2];

    uint64_t storage_id = lookup_storage_id(relfilelocator);

    Oid bioseq_column_chunk_oid = column_chunk_relation_oid();
    Relation bioseq_column_chunk = table_open(bioseq_column_chunk_oid, AccessShareLock);
    Relation index = index_open(column_chunk_index_oid(), AccessShareLock);

    scan_keys[0] =
        chunk_codec::scan_key<&chunk_row::storage_id>(static_cast<std::int64_t>(storage_id));
    scan_keys[1] = chunk_codec::scan_key<&chunk_row::stripe_num>(static_cast<std::int64_t>(stripe));

    SysScanDesc scan_descriptor =
        systable_beginscan_ordered(bioseq_column_chunk, index, snapshot, 2, scan_keys);

    stripe_skip_list* chunk_list =
        bioseq::postgres::support::allocate_zeroed_object<stripe_skip_list>();
    chunk_list->chunk_count = chunk_count;
    chunk_list->column_count = column_count;
    chunk_list->chunk_skip_nodes =
        bioseq::postgres::support::allocate_zeroed_array<chunk_skip_node*>(column_count);
    for (column_index = 0; column_index < column_count; column_index++) {
        chunk_list->chunk_skip_nodes[column_index] =
            bioseq::postgres::support::allocate_zeroed_array<chunk_skip_node>(chunk_count);
    }

    while (HeapTupleIsValid(heap_tuple =
                                systable_getnext_ordered(scan_descriptor, ForwardScanDirection))) {
        const chunk_row row = chunk_codec::deform(
            heap_tuple, RelationGetDescr(bioseq_column_chunk), CurrentMemoryContext);
        const int32_t attr = row.attr_num;
        const int32_t chunk_index = row.chunk_group_num;

        if (attr <= 0 || attr > column_count) {
            bioseq_report<logging_level::error, logging_target::postgres>()
                .with_message("invalid columnar chunk entry")
                .with_detail("Attribute number out of range: {}", attr);
        }

        if (chunk_index < 0 || chunk_index >= chunk_count) {
            bioseq_report<logging_level::error, logging_target::postgres>()
                .with_message("invalid columnar chunk entry")
                .with_detail("Chunk number out of range: {}", chunk_index);
        }

        column_index = attr - 1;

        chunk_skip_node* chunk = &chunk_list->chunk_skip_nodes[column_index][chunk_index];
        chunk->row_count = row.value_count;
        chunk->value_stream_offset = row.value_stream_offset;
        chunk->value_stream_length = row.value_stream_length;
        chunk->exists_stream_offset = row.exists_stream_offset;
        chunk->exists_stream_length = row.exists_stream_length;
        chunk->value_compression_algorithm =
            static_cast<bioseq::compression_algorithm>(row.value_compression_type);
        chunk->value_compression_level = row.value_compression_level;
        chunk->value_decompressed_length = row.value_decompressed_length;

        if (!row.minimum_value.has_value() || !row.maximum_value.has_value()) {
            chunk->has_min_max = false;
        } else {
            bytea* min_value = *row.minimum_value;
            bytea* max_value = *row.maximum_value;

            chunk->minimum_value = bioseq::postgres::support::pg_datum::from_bytea(
                                       min_value, TupleDescAttr(tuple_descriptor, column_index))
                                       .raw();
            chunk->maximum_value = bioseq::postgres::support::pg_datum::from_bytea(
                                       max_value, TupleDescAttr(tuple_descriptor, column_index))
                                       .raw();

            chunk->has_min_max = true;
        }
    }

    systable_endscan_ordered(scan_descriptor);
    index_close(index, AccessShareLock);
    table_close(bioseq_column_chunk, AccessShareLock);

    read_chunk_group_row_counts(storage_id,
                                stripe,
                                chunk_count,
                                &chunk_list->chunk_group_row_counts,
                                &chunk_list->chunk_group_deleted_rows,
                                snapshot);

    chunk_list->chunk_group_row_offsets =
        bioseq::postgres::support::allocate_zeroed_array<uint32_t>(chunk_count);

    for (chunk_group_index = 0; chunk_group_index < chunk_count; chunk_group_index++) {
        chunk_list->chunk_group_row_offsets[chunk_group_index] = chunk_group_row_offset_acc;
        chunk_group_row_offset_acc += chunk_list->chunk_group_row_counts[chunk_group_index];
    }

    return chunk_list;
}

/*
 * read_chunk_row_mask fetches chunk row mask for columnar relation.
 */
export bytea* read_chunk_row_mask(RelFileLocator relfilelocator,
                                  Snapshot snapshot,
                                  MemoryContext cxt,
                                  uint64_t stripe_first_row_number,
                                  int row_count) {
    if (snapshot == nullptr) {
        bioseq_log<logging_level::error, logging_target::postgres>(
            "read_chunk_row_mask requires the caller's registered snapshot");
    }
    HeapTuple heap_tuple = nullptr;
    ScanKeyData scan_keys[3];

    uint64_t storage_id = lookup_storage_id(relfilelocator);

    Oid row_mask_relation_oid = column_row_mask_relation_oid();
    Relation row_mask_relation = table_open(row_mask_relation_oid, AccessShareLock);
    Relation index = index_open(column_row_mask_index_oid(), AccessShareLock);

    bytea* chunk_row_mask_bytea = nullptr;
    SysScanDesc scan_descriptor = nullptr;
    {
        bioseq::postgres::support::memory_context_scope context_guard{cxt};

        Size chunk_mask_size =
            (row_count % row_mask_chunk_row_capacity) ? (row_count / 8 + 1) : (row_count / 8);

        chunk_row_mask_bytea =
            bioseq::postgres::support::allocate_zeroed_storage<bytea>(chunk_mask_size + VARHDRSZ);
        SET_VARSIZE(chunk_row_mask_bytea, chunk_mask_size + VARHDRSZ);

        scan_keys[0] = row_mask_codec::scan_key<&row_mask_row::storage_id>(
            static_cast<std::int64_t>(storage_id));
        scan_keys[1] =
            row_mask_codec::scan_key<&row_mask_row::start_row_number,
                                     bioseq::postgres::support::scan_comparison::greater_equal>(
                static_cast<std::int64_t>(stripe_first_row_number));
        scan_keys[2] =
            row_mask_codec::scan_key<&row_mask_row::end_row_number,
                                     bioseq::postgres::support::scan_comparison::less_equal>(
                static_cast<std::int64_t>(stripe_first_row_number + row_count - 1));

        scan_descriptor =
            systable_beginscan_ordered(row_mask_relation, index, snapshot, 3, scan_keys);

        Size pos = 0;
        while (HeapTupleIsValid(
            heap_tuple = systable_getnext_ordered(scan_descriptor, ForwardScanDirection))) {
            const row_mask_row row =
                row_mask_codec::deform(heap_tuple, RelationGetDescr(row_mask_relation), cxt);
            if (!row.mask.has_value() || *row.mask == nullptr) {
                throw bioseq::postgres::support::row_codec_error{
                    "row mask catalog entry has a NULL mask"};
            }
            bytea* current_row_mask = *row.mask;

            if (row.start_row_number < 0 || row.end_row_number < row.start_row_number) {
                throw bioseq::postgres::support::row_codec_error{
                    "bioseq_internal.row_mask has an invalid row range"};
            }

            const std::uint64_t logical_rows = static_cast<std::uint64_t>(row.end_row_number) -
                                               static_cast<std::uint64_t>(row.start_row_number) + 1;
            const std::uint64_t logical_bytes = logical_rows / 8 + (logical_rows % 8 != 0 ? 1 : 0);
            const Size remaining = chunk_mask_size - pos;
            const Size bounded_logical_bytes =
                static_cast<Size>(std::min<std::uint64_t>(logical_bytes, remaining));
            const Size copy_bytes = std::min(static_cast<Size>(VARSIZE_ANY_EXHDR(current_row_mask)),
                                             bounded_logical_bytes);

            if (copy_bytes > 0) {
                memcpy(VARDATA(chunk_row_mask_bytea) + pos, VARDATA(current_row_mask), copy_bytes);
            }

            pos += bounded_logical_bytes;
            if (pos == chunk_mask_size) {
                break;
            }
        }
    }

    systable_endscan_ordered(scan_descriptor);
    index_close(index, AccessShareLock);
    table_close(row_mask_relation, AccessShareLock);

    return chunk_row_mask_bytea;
}

export void
flush_row_mask_cache(bioseq::storage::metadata::catalog_rows::row_mask* row_mask_entry) {
    HeapTuple old_heap_tuple = nullptr;

    ScanKeyData scan_keys;

    Oid row_mask_relation_oid = column_row_mask_relation_oid();
    Relation row_mask_relation = table_open(row_mask_relation_oid, AccessShareLock);
    TupleDesc tuple_descriptor = RelationGetDescr(row_mask_relation);

    Relation index = index_open(column_row_mask_index_oid(), AccessShareLock);

    scan_keys = row_mask_codec::scan_key<&row_mask_row::id>(row_mask_entry->id);

    SysScanDesc scan_descriptor =
        systable_beginscan_ordered(row_mask_relation, index, nullptr, 1, &scan_keys);

    old_heap_tuple = systable_getnext_ordered(scan_descriptor, BackwardScanDirection);

    index_close(index, AccessShareLock);

    if (HeapTupleIsValid(old_heap_tuple)) {
        row_mask_codec::update update{};
        update.set<&row_mask_row::deleted_rows>(row_mask_entry->deleted_rows)
            .set<&row_mask_row::mask>(row_mask_entry->mask, CurrentMemoryContext);

        HeapTuple new_heap_tuple = update.modify(old_heap_tuple, tuple_descriptor);

        CatalogTupleUpdate(row_mask_relation, &old_heap_tuple->t_self, new_heap_tuple);

        heap_freetuple(new_heap_tuple);
    }

    systable_endscan_ordered(scan_descriptor);
    table_close(row_mask_relation, AccessShareLock);

    CommandCounterIncrement();
}
