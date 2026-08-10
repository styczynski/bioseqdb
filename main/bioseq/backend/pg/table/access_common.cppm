/**
 * @file
 * @brief Shared table-access helpers independent of routine assembly.
 */
module;

#include <bioseq/pg_compat.h>

#include <cstdint>

export module bioseq.postgres.table:access_common;

import bioseq.common.logging;
import bioseq.postgres.storage.manager;
import bioseq.postgres.support;
import bioseq.postgres.table.am_cache;
import bioseq.postgres.table.types;
import bioseq.postgres.types.compact_sequence;

namespace {

TupleTableSlotOps column_slot_operations{};

/** Materialize a virtual source slot at PostgreSQL's heap-tuple boundary. */
void copy_column_slot(TupleTableSlot* destination, TupleTableSlot* source) {
    const auto previous_scope = exchange_slice_materialization_scope({
        .attachment = nullptr,
        .boundary_id = 0u,
        .producer_value_id = 0u,
        .mechanism = materialization_mechanism::packed_full,
        .reason = materialization_reason::heap_tuple,
        .local_sink = nullptr,
    });
    PG_TRY();
    {
        TTSOpsVirtual.copyslot(destination, source);
        restore_slice_materialization_scope(previous_scope);
    }
    PG_CATCH();
    {
        restore_slice_materialization_scope(previous_scope);
        PG_RE_THROW();
    }
    PG_END_TRY();
}

/** Form an owned heap tuple from a populated Bioseq column slot. */
[[nodiscard]] HeapTuple copy_column_heap_tuple(TupleTableSlot* slot) {
    Assert(!TTS_EMPTY(slot));

    HeapTuple volatile tuple = nullptr;
    const auto previous_scope = exchange_slice_materialization_scope({
        .attachment = nullptr,
        .boundary_id = 0u,
        .producer_value_id = 0u,
        .mechanism = materialization_mechanism::packed_full,
        .reason = materialization_reason::heap_tuple,
        .local_sink = nullptr,
    });
    PG_TRY();
    {
        tuple = heap_form_tuple(
            slot->tts_tupleDescriptor, slot->tts_values, slot->tts_isnull);
        restore_slice_materialization_scope(previous_scope);
    }
    PG_CATCH();
    {
        restore_slice_materialization_scope(previous_scope);
        PG_RE_THROW();
    }
    PG_END_TRY();

    // PostgreSQL expects copy_heap_tuple to preserve the slot identity.
    tuple->t_self = slot->tts_tid;
    return tuple;
}

}  // namespace

/**
 * @brief Initialize the immutable tuple-slot operation table.
 *
 * Call once during extension initialization before publishing the table-AM
 * routine. All unspecified operations retain PostgreSQL virtual-slot behavior.
 */
export void initialize_column_tuple_slot_operations() {
    column_slot_operations = TTSOpsVirtual;
    column_slot_operations.copyslot = copy_column_slot;
    column_slot_operations.copy_heap_tuple = copy_column_heap_tuple;
}

/** @return Tuple-slot operations used by every Bioseq table relation. */
export [[nodiscard]] const TupleTableSlotOps* column_slot_callbacks(
        Relation /*relation*/) noexcept {
    return &column_slot_operations;
}

/** @return A child of the current memory context for one scan or fetch. */
export [[nodiscard]] MemoryContext create_column_scan_memory_context() {
    return AllocSetContextCreate(
        CurrentMemoryContext,
        "BioseqColumn Scan Context",
        ALLOCSET_DEFAULT_SIZES);
}

/**
 * @brief Build one-based attribute numbers selected by a zero-based bitmap.
 * @param tuple_descriptor Relation tuple descriptor.
 * @param attributes_needed Zero-based attributes required by the scan.
 * @return PostgreSQL list allocated in the current memory context.
 */
export [[nodiscard]] List* build_needed_column_list(
        TupleDesc tuple_descriptor, const bioseq::postgres::support::postgres_bitmapset& attributes_needed) {
    bioseq::postgres::support::postgres_integer_list columns;
    for (int attribute_index = 0;
         attribute_index < tuple_descriptor->natts;
         ++attribute_index) {
        if (TupleDescAttr(tuple_descriptor, attribute_index)->attisdropped) {
            continue;
        }
        if (attributes_needed.contains(attribute_index)) {
            columns.append(attribute_index + 1);
        }
    }
    return columns.release();
}

/**
 * @brief Reject writes whose changes cannot be represented by logical replication.
 * @param relation Target Bioseq table relation.
 */
export void validate_logical_replication_write(Relation relation) {
    if (!is_publishable_relation(relation)) {
        return;
    }

    PublicationDesc publication{};
    RelationBuildPublicationDesc(relation, &publication);
    if (publication.pubactions.pubinsert) {
        bioseq_report<logging_level::error, logging_target::postgres>()
            .with_sql_error_code(ERRCODE_FEATURE_NOT_SUPPORTED)
            .with_message(
                "cannot insert into columnar table that is a part of a publication");
    }
}

/**
 * @brief Replace sequence locator stubs in a slot with materialized values.
 * @param relation Relation that owns the sequence fragments.
 * @param slot Populated tuple slot to hydrate in place.
 * @param caches Per-column fragment caches, or null when hydration is disabled.
 * @param table_cache Sequence-column metadata for `relation`.
 * @param snapshot Explicit visibility snapshot used for fragment reads.
 * @param payload_context Memory context that owns hydrated values.
 */
export [[gnu::always_inline]] inline void hydrate_sequence_columns_in_slot(
        Relation relation,
        TupleTableSlot* slot,
        bioseq::storage::seq_caches* caches,
        const bioseq::storage::table_access_cache& table_cache,
        Snapshot snapshot,
        MemoryContext payload_context) {
    if (caches == nullptr
        || table_cache.sequence_attribute_numbers.empty()) {
        return;
    }

    const RelFileLocator& relation_locator = relation->rd_locator;
    for (const std::int16_t attribute_number :
         table_cache.sequence_attribute_numbers) {
        const int attribute_index = attribute_number - 1;
        if (attribute_index < 0
            || attribute_index >= slot->tts_tupleDescriptor->natts
            || slot->tts_isnull[attribute_index]) {
            continue;
        }

        const auto* compact_sequence =
            reinterpret_cast<const bioseq::pg_types::seq*>(
                DatumGetPointer(slot->tts_values[attribute_index]));
        if (!bioseq::storage::is_locator_stub(compact_sequence)) {
            continue;
        }
        if (snapshot == nullptr) {
            ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq sequence hydration requires an explicit snapshot")));
        }

        const auto sequence_locator =
            bioseq::storage::decode_locator_from_stub(compact_sequence);
        bioseq::storage::validate_locator_flat_materialization_length(
            sequence_locator);

        // Two bits per letter is the smallest supported packed payload. The
        // storage manager validates the exact alphabet width after reading the
        // first fragment.
        const std::uint64_t projected_payload_bytes = checked_ceil_div(
            sequence_locator.logical_length, 4u);
        (void)checked_materialization_allocation_size(
            projected_payload_bytes,
            bioseq::pg_types::seq::header_bytes(),
            "flat sequence");

        bioseq::pg_types::seq* sequence =
            bioseq::storage::bioseq_default_storage_manager::fetch_payload(
                relation_locator.dbOid,
                relation_locator.spcOid,
                relation_locator.relNumber,
                attribute_number,
                sequence_locator,
                snapshot,
                payload_context,
                caches);
        slot->tts_values[attribute_index] = PointerGetDatum(sequence);
    }
}

/**
 * @brief Hydrate a scan slot unless the scan explicitly defers hydration.
 * @param table_scan PostgreSQL scan descriptor created by this table AM.
 * @param slot Populated tuple slot to update in place.
 * @param force Ignore the scan's deferred-hydration mode when true.
 */
export void hydrate_scan_slot(
        TableScanDesc table_scan, TupleTableSlot* slot, bool force) {
    auto* scan = reinterpret_cast<column_scan_handle>(table_scan);
    if (scan == nullptr || (!force && scan->defers_sequence_hydration)) {
        return;
    }
    if (scan->read_state == nullptr) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq scan hydration requires an initialized read state")));
    }

    const auto& table_cache =
        bioseq::storage::get_table_access_cache(scan->scan_base.rs_rd);
    auto* caches =
        static_cast<bioseq::storage::seq_caches*>(scan->sequence_cache);
    hydrate_sequence_columns_in_slot(
        scan->scan_base.rs_rd,
        slot,
        caches,
        table_cache,
        scan->read_state->snapshot,
        scan->scan_context);
}
