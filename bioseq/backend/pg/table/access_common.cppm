/**
 * @file
 * @brief Shared helpers for reading and  writing Bioseq table rows.
 */
module;

#include <bioseq/pg_compat.h>
#include <array>
#include <cstdint>
#include <exception>
#include <type_traits>
#include <vector>

export module bioseq.postgres.table:access_common;

import bioseq.common.logging;
import bioseq.postgres.storage.metadata;
import bioseq.postgres.storage.manager;
import bioseq.postgres.support;
import bioseq.postgres.table.am_cache;
import bioseq.postgres.table.io;
import bioseq.postgres.table.types;
import bioseq.postgres.types.compact_sequence;

namespace {

TupleTableSlotOps column_slot_operations{};

/** Copy a vistual slot and record any full sequence copies as heap-tuple materialization. */
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
        tuple = heap_form_tuple(slot->tts_tupleDescriptor, slot->tts_values, slot->tts_isnull);
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

} // namespace

/** Read the next visible row and report C++ failures as PostgreSQL errors. */
bool bioseq_getnextslot(TableScanDesc scan,
                        ScanDirection direction,
                        TupleTableSlot* slot);

/** Check or clear unfinished speculative sequence writes at transaction boundaries. */
void handle_pending_speculative_xact_event(XactEvent event);

/** Move pending speculative writes to the parent subtransaction, or discard aborted writes. */
void handle_pending_speculative_subxact_event(SubXactEvent event,
                                              SubTransactionId my_subid,
                                              SubTransactionId parent_subid);

/**
 * @brief Set up Bioseq tuple slots before the table access method is used.
 *
 * Start with PostgreSQL virtual-slot operations and replace the two copy
 * callbacks so sequence  materialization is recorded. Call once at startup.
 */
export void initialize_column_tuple_slot_operations() {
    column_slot_operations = TTSOpsVirtual;
    column_slot_operations.copyslot = copy_column_slot;
    column_slot_operations.copy_heap_tuple = copy_column_heap_tuple;
}

/** @return Tuple-slot operations used by every Bioseq table relation. */
export [[nodiscard]] const TupleTableSlotOps*
column_slot_callbacks(Relation /*relation*/) noexcept {
    return &column_slot_operations;
}

/** @return A child of the current memory context for one scan or fetch. */
export [[nodiscard]] MemoryContext create_column_scan_memory_context() {
    return AllocSetContextCreate(
        CurrentMemoryContext, "BioseqColumn Scan Context", ALLOCSET_DEFAULT_SIZES);
}

/** Get or create the sequence cache for this column write state and check its storage identity */
[[nodiscard]] inline bioseq::storage::seq_caches*
ensure_write_side_seq_caches(Relation relation, column_write_state* write_state) {
    if (relation == nullptr || write_state == nullptr
        || write_state->stripe_write_context == nullptr) {
        bioseq::postgres::support::invoke_pg_as_cpp([] {
            bioseq_log<logging_level::error, logging_target::postgres>(
                "bioseq storage: sequence write cache owner is unavailable");
        });
    }

    if (write_state->seq_caches == nullptr) {
        MemoryContext owner =
            MemoryContextGetParent(write_state->stripe_write_context);
        if (owner == nullptr) {
            bioseq::postgres::support::invoke_pg_as_cpp([] {
                bioseq_log<logging_level::error, logging_target::postgres>(
                    "bioseq storage: sequence write cache context is unavailable");
            });
        }
        auto* fresh = bioseq::postgres::support::invoke_pg_as_cpp(
            [owner] { return bioseq::storage::alloc_seq_caches(owner); });
        fresh->side = bioseq::storage::seq_caches::access_side::write;
        fresh->lock_mode = RowExclusiveLock;
        fresh->main_database_oid = relation->rd_locator.dbOid;
        fresh->main_tablespace_oid = relation->rd_locator.spcOid;
        fresh->main_relfilenode = relation->rd_locator.relNumber;
        fresh->cap_bytes_snapshot =
            bioseq::storage::read_max_sub_table_cap_bytes();
        write_state->seq_caches = fresh;
    }

    auto* caches =
        static_cast<bioseq::storage::seq_caches*>(write_state->seq_caches);
    if (caches->side != bioseq::storage::seq_caches::access_side::write
        || caches->main_database_oid != relation->rd_locator.dbOid
        || caches->main_tablespace_oid != relation->rd_locator.spcOid
        || caches->main_relfilenode != relation->rd_locator.relNumber) {
        bioseq::postgres::support::invoke_pg_as_cpp([] {
            bioseq_log<logging_level::error, logging_target::postgres>(
                "bioseq storage: sequence write cache relation identity changed");
        });
    }
    return caches;
}

/**
 * @brief Convert selected columns into PostgreSQL attribute numbers.
 *
 * The input bitmap uses zero-based positions. The returned list uses one-based
 * attribute numbers, skips dropped columns, and belongs to the current context.
 */
export [[nodiscard]] List*
build_needed_column_list(TupleDesc tuple_descriptor,
                         const bioseq::postgres::support::postgres_bitmapset& attributes_needed) {
    bioseq::postgres::support::postgres_integer_list columns;
    for (int attribute_index = 0; attribute_index < tuple_descriptor->natts; ++attribute_index) {
        if (bioseq::postgres::support::tuple_descriptor_attribute(tuple_descriptor, attribute_index)->attisdropped) {
            continue;
        }
        if (attributes_needed.has_member(attribute_index)) {
            columns.append(attribute_index + 1);
        }
    }
    return columns.release();
}

/**
 * @brief Initialize the column reader used by scans and exact-row fetches.
 * @param relation Relation that owns the column storage.
 * @param tuple_descriptor Descriptor used to decode returned rows.
 * @param attributes_needed Zero-based attributes selected by the caller.
 * @param scan_qualifiers Qualifiers evaluated by the column reader.
 * @param scan_context Memory context that owns the reader.
 * @param snapshot Visibility snapshot used for the read.
 * @param random_access Whether the reader must support nonsequential row access.
 * @param parallel_scan Optional shared parallel-scan state.
 * @return Reader owned by @p scan_context.
 */
[[nodiscard]] inline column_read_state*
init_bioseq_read_state(
    Relation relation,
    TupleDesc tuple_descriptor,
    const bioseq::postgres::support::postgres_bitmapset& attributes_needed,
    List* scan_qualifiers,
    MemoryContext scan_context,
    Snapshot snapshot,
    bool random_access,
    parallel_column_scan_handle parallel_scan) {
    bioseq::postgres::support::memory_context_scope context_guard{scan_context};
    List* needed_columns = build_needed_column_list(tuple_descriptor, attributes_needed);
    return begin_column_read(relation,
                             tuple_descriptor,
                             needed_columns,
                             scan_qualifiers,
                             scan_context,
                             snapshot,
                             random_access,
                             parallel_scan);
}

/**
 * @brief Run a table-access callback and turn C++ exceptions into PostgreSQL errors.
 *
 * C++ cleanup finishes before raising the PostgreSQL error, whose longjmp would
 * otherwise skip destructors. Existing PostgreSQL errors keep their details.
 * @tparam p_function Callback body type.
 * @param operation Callback name  included in diagnostics.
 * @param function Callback body.
 * @return The callback result, if it has one.
 */
template <class p_function>
decltype(auto) run_tableam_cpp_checked(const char* operation, p_function&& function) {
    ErrorData* exact_error = nullptr;
    std::array<char, 512> detail{};
    bool cpp_error = false;
    bool unknown_cpp_error = false;
    try {
        if constexpr (std::is_void_v<std::invoke_result_t<p_function&>>) {
            function();
            return;
        } else {
            return function();
        }
    } catch (bioseq::postgres::support::postgres_error& error) {
        exact_error = error.release_error_data();
    } catch (const std::exception& error) {
        strlcpy(detail.data(),
                error.what() == nullptr ? "std::exception without detail" : error.what(),
                detail.size());
        cpp_error = true;
    } catch (...) {
        unknown_cpp_error = true;
    }
    // Callback-local C++ objects hahe been destroyed, so a PostgreSQL error
    // can jump back to its handler.
    if (exact_error != nullptr) {
        ReThrowError(exact_error);
    }
    if (cpp_error) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("%s raised a C++ exception", operation),
                 errdetail_internal("%s", detail.data())));
    }
    if (unknown_cpp_error) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("%s raised an unknown C++ exception", operation)));
    }
    pg_unreachable();
}

/** Reject inserts into a Bioseq table included in an INSERT publication. */
export void validate_logical_replication_write(Relation relation) {
    if (!is_publishable_relation(relation)) {
        return;
    }

    PublicationDesc publication{};
    RelationBuildPublicationDesc(relation, &publication);
    if (publication.pubactions.pubinsert) {
        bioseq_report<logging_level::error, logging_target::postgres>()
            .with_sql_error_code(ERRCODE_FEATURE_NOT_SUPPORTED)
            .with_message("cannot insert into columnar table that is a part of a publication");
    }
}

/**
 * @brief Replace sequence locators in a tuple slot with full sequence values.
 *
 * Read fragments using the supplied snapshot and allocate the values in
 * payload_context. Null columns and values that are already stored in full
 * stay as they are. A null cache disables this step.
 * @param relation Table that owns the sequence fragments.
 * @param slot Populated slot to update.
 * @param caches Fragment caches, or null to leave the slot unchanged.
 * @param table_cache Sequence-column metadata for,  the table.
 * @param snapshot Visibility snapshot for fragment reads.
 * @param payload_context Memory context for the full values.
 */
export [[gnu::always_inline]] inline void
hydrate_sequence_columns_in_slot(Relation relation,
                                 TupleTableSlot* slot,
                                 bioseq::storage::seq_caches* caches,
                                 const bioseq::storage::table_access_cache& table_cache,
                                 Snapshot snapshot,
                                 MemoryContext payload_context) {
    if (caches == nullptr || table_cache.sequence_attribute_numbers.empty()) {
        return;
    }

    const RelFileLocator& relation_locator = relation->rd_locator;
    for (const std::int16_t attribute_number : table_cache.sequence_attribute_numbers) {
        const int attribute_index = attribute_number - 1;
        if (attribute_index < 0 || attribute_index >= slot->tts_tupleDescriptor->natts ||
            slot->tts_isnull[attribute_index]) {
            continue;
        }

        const auto* compact_sequence = reinterpret_cast<const bioseq::pg_types::seq*>(
            DatumGetPointer(slot->tts_values[attribute_index]));
        if (!bioseq::storage::is_locator_stub(compact_sequence)) {
            continue;
        }
        if (snapshot == nullptr) {
            ereport(ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                     errmsg("bioseq sequence hydration requires an explicit snapshot")));
        }

        const auto sequence_locator = bioseq::storage::decode_locator_from_stub(compact_sequence);
        bioseq::storage::validate_locator_flat_materialization_length(sequence_locator);

        // The storage manager checks the exact encoded allocation size before
        // materialization, using the fragment codec and alphabet metadata.
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
 * @brief Load full sequence values into a scan slot.
 *
 * A scan can defer this work until a value is needed. Setting force to true
 * loads the values immediately, using the scan reader's snapshot and context
 * @param table_scan Scan created by this table access method.
 * @param slot Populated slot to update.
 * @param force Load sequences even when the scan defers that work.
 */
export void hydrate_scan_slot(TableScanDesc table_scan, TupleTableSlot* slot, bool force) {
    auto* scan = reinterpret_cast<column_scan_handle>(table_scan);
    if (scan == nullptr || (!force && scan->defers_sequence_hydration)) {
        return;
    }
    if (scan->read_state == nullptr) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq scan hydration requires an initialized read state")));
    }

    const auto& table_cache = bioseq::storage::get_table_access_cache(scan->scan_base.rs_rd);
    auto* caches = static_cast<bioseq::storage::seq_caches*>(scan->sequence_cache);
    hydrate_sequence_columns_in_slot(scan->scan_base.rs_rd,
                                     slot,
                                     caches,
                                     table_cache,
                                     scan->read_state->snapshot,
                                     scan->scan_context);
}
