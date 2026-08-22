/**
 * @file index_parts/configuration.cppm
 * @brief Defines tuple-buffer configuration, captured operand, and borrowed-owner lifetimes.
 */
module;

#include <bioseq/pg_compat.h>
extern "C" {
#include "executor/instrument.h"
#include "utils/snapmgr.h"
}

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// The chunked-scanner dispatch path is always compiled as a permanent part of
// the fragmented pipeline tree.
// The `bioseq.testing.chunked_scanner.enabled` GUC routes row sequences through a
// synthetic chunked scanner end-to-end. The
// `bioseq.testing.in_memory_scanner.enabled` GUC forces the materialise-then-
// in_memory baseline. Without either GUC the production path promotes locator
// values through the statement-owned source provider and runs from its
// canonical lazy facade.
module bioseq.postgres.scheduler.dispatch_buffer:configuration;

import bioseq.postgres.scheduler.dispatch_buffer;

import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.postgres.support;
import bioseq.postgres.support.datums;
import bioseq.postgres.configuration.core;
import bioseq.indexing.type;
import bioseq.postgres.storage.indexing.handles;
import bioseq.postgres.storage.indexing.chunked_storage;
import bioseq.storage.lazy_sequence; // allocation observer for route-aware sequence payload tracing
import bioseq.postgres.scheduler.dispatch_processor; // fixed-shape batch_processor, heavy pipeline in its own module
import bioseq.sequences;
import bioseq.sequences.simd;
import bioseq.pipeline.minimap_port;
import bioseq.sequences.cigar;
import bioseq.pipeline.pipe;
import bioseq.postgres.types.compact_sequence;
import bioseq.postgres.storage.manager;
import bioseq.storage.scanner;
import bioseq.storage.layout;
import bioseq.postgres.storage.scanner; // statement-owned source provider for locator targets
import bioseq.parallelism.memory_admission;
import bioseq.postgres.scheduler.supervisor;
import bioseq.postgres.column.table_access;

namespace bioseq::scheduler {

/** @brief Implements the require evaluated mmsearch index policy contract declared by the stable
 * buffer facade. */
void tuple_dispatch_buffer::require_evaluated_mmsearch_index_policy_() const {
    if (!evaluated_mmsearch_index_policy_valid(index_read_requirement,
                                               read_sequence_index_active)) {
        ereport(
            ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("evaluated bioseq mmsearch target requires no persisted target index"),
             errdetail("index policy must be require_absent and payload reads must be disabled")));
    }
}

/** @brief Implements the require registered mvcc snapshot contract declared by the stable buffer
 * facade. */
void tuple_dispatch_buffer::require_registered_mvcc_snapshot_(Snapshot snapshot) {
    if (snapshot == nullptr || !IsMVCCSnapshot(snapshot) || snapshot->regd_count == 0) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq dispatch requires a registered MVCC executor snapshot")));
    }
}

/** @brief Implements the retain expanded recipe contract declared by the stable buffer facade. */
[[nodiscard]] Datum tuple_dispatch_buffer::retain_expanded_recipe_(Datum input) {
    if (batch_payload_ctx == nullptr) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq dispatch recipe arena is not initialized")));
    }
    return bioseq::postgres::support::invoke_pg_as_cpp([this, input] {
        MemoryContext previous = MemoryContextSwitchTo(batch_payload_ctx);
        try {
            /* Force a copy even when the producer handed us its RW
             * wrapper: that wrapper commonly belongs to the current
             * per-tuple context.  The retained edge graph is small and
             * source-backed; no sequence payload is copied here. */
            const Datum read_only = MakeExpandedObjectReadOnly(input, false, -1);
            acquired_sequence acquired = acquire_writable_pg_sequence(read_only);
            const Datum retained = pg_sequence_ro_datum(acquired.value);
            MemoryContextSwitchTo(previous);
            return retained;
        } catch (...) {
            MemoryContextSwitchTo(previous);
            throw;
        }
    });
}

/** @brief Implements the require recipe descriptor contract declared by the stable buffer facade.
 */
void tuple_dispatch_buffer::require_recipe_descriptor_(const pg_sequence_descriptor& descriptor,
                                                       const std::uint8_t alphabet_code,
                                                       const std::uint8_t storage_unit_byte_width,
                                                       const char* operand) {
    const bool expanded = descriptor.kind == pg_sequence_datum_kind::expanded_read_only ||
                          descriptor.kind == pg_sequence_datum_kind::expanded_read_write;
    if (!expanded || !descriptor.alphabet_bound || descriptor.alphabet_code != alphabet_code ||
        descriptor.storage_unit_byte_width != storage_unit_byte_width) {
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("bioseq mmsearch %s recipe typmod changed after planning", operand),
                 errdetail("expected=(%u,%u) actual=(%u,%u) bound=%s kind=%u",
                           static_cast<unsigned>(alphabet_code),
                           static_cast<unsigned>(storage_unit_byte_width),
                           static_cast<unsigned>(descriptor.alphabet_code),
                           static_cast<unsigned>(descriptor.storage_unit_byte_width),
                           descriptor.alphabet_bound ? "true" : "false",
                           static_cast<unsigned>(descriptor.kind))));
    }
}

/** @brief Implements the capture target datum contract declared by the stable buffer facade. */
void tuple_dispatch_buffer::capture_target_datum_(buffered_scan_entry& entry, Datum datum) {
    if (VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(datum))) {
        const pg_sequence_descriptor descriptor = classify_and_describe_sequence(datum);
        require_recipe_descriptor_(descriptor,
                                   mmsearch_state.alphabet_code,
                                   mmsearch_state.storage_unit_byte_width,
                                   "target");
        entry.seq_recipe_datum = retain_expanded_recipe_(datum);
        entry.seq_recipe_descriptor = descriptor;
        entry.seq_is_null = false;
        return;
    }

    auto source = bioseq::postgres::support::detoast_as<bioseq::pg_types::seq>(
        bioseq::postgres::support::pg_datum::from_raw(datum));
    bioseq::pg_types::seq* header = source.ptr;
    if (bioseq::storage::is_locator_stub(header)) {
        entry.seq_loc = bioseq::storage::decode_locator_from_stub(header);
    } else {
        if (header->alphabet_code != mmsearch_state.alphabet_code ||
            header->storage_unit_byte_width != mmsearch_state.storage_unit_byte_width) {
            source.free_if_copy();
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("bioseq mmsearch target payload typmod changed after planning")));
        }
        const size_t total_bytes = static_cast<size_t>(VARSIZE(header));
        entry.seq_varlena.assign(reinterpret_cast<const char*>(header),
                                 reinterpret_cast<const char*>(header) + total_bytes);
    }
    entry.seq_is_null = false;
    source.free_if_copy();
}

/** @brief Implements the capture query datum contract declared by the stable buffer facade. */
void tuple_dispatch_buffer::capture_query_datum_(buffered_scan_entry& entry, Datum datum) {
    if (VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(datum))) {
        const pg_sequence_descriptor descriptor = classify_and_describe_sequence(datum);
        require_recipe_descriptor_(descriptor,
                                   mmsearch_state.query_alphabet_code,
                                   mmsearch_state.query_storage_unit_byte_width,
                                   "query");
        entry.per_row_query_recipe_datum = retain_expanded_recipe_(datum);
        entry.per_row_query_recipe_descriptor = descriptor;
        entry.per_row_query_is_null = false;
        return;
    }

    auto source = bioseq::postgres::support::detoast_as<bioseq::pg_types::seq>(
        bioseq::postgres::support::pg_datum::from_raw(datum));
    bioseq::pg_types::seq* header = source.ptr;
    if (bioseq::storage::is_locator_stub(header)) {
        entry.per_row_query_loc = bioseq::storage::decode_locator_from_stub(header);
        entry.per_row_query_is_null = false;
        source.free_if_copy();
        return;
    }
    if (header->alphabet_code != mmsearch_state.query_alphabet_code ||
        header->storage_unit_byte_width != mmsearch_state.query_storage_unit_byte_width) {
        source.free_if_copy();
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("bioseq mmsearch query payload typmod changed after planning")));
    }
    const bioseq::pg_types::seq* dispatch_header = header;
    bioseq::pg_types::seq* rewidth = nullptr;
    if (header->storage_unit_byte_width != mmsearch_state.storage_unit_byte_width) {
        rewidth = ::bioseq::simd::stdc_rewidth_to_type_params(
            *header,
            compact_sequence_stdc_type_params{mmsearch_state.alphabet_code,
                                              mmsearch_state.storage_unit_byte_width},
            pg_alloc);
        if (rewidth == nullptr) {
            source.free_if_copy();
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("bioseq mmsearch query rewidth rejected an incompatible alphabet")));
        }
        dispatch_header = rewidth;
    }
    const size_t total_bytes = static_cast<size_t>(VARSIZE(dispatch_header));
    entry.per_row_query_varlena.assign(reinterpret_cast<const char*>(dispatch_header),
                                       reinterpret_cast<const char*>(dispatch_header) +
                                           total_bytes);
    if (rewidth != nullptr) {
        pfree(rewidth);
    }
    entry.per_row_query_is_null = false;
    source.free_if_copy();
}

/** @brief Implements the retain expanded sequence output attributes contract declared by the stable
 * buffer facade. */
void tuple_dispatch_buffer::retain_expanded_sequence_output_attributes_(buffered_scan_entry& entry,
                                                                        TupleTableSlot* slot) {
    if (slot == nullptr || slot->tts_tupleDescriptor == nullptr || mmsearch_state.seq_attno <= 0 ||
        mmsearch_state.seq_attno > slot->tts_tupleDescriptor->natts) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq dispatch output slot descriptor is invalid")));
    }
    const Oid sequence_type_oid =
        TupleDescAttr(slot->tts_tupleDescriptor, mmsearch_state.seq_attno - 1)->atttypid;
    entry.expanded_sequence_attributes.clear();
    for (int index = 0; index < slot->tts_tupleDescriptor->natts; ++index) {
        if (slot->tts_isnull[index] ||
            TupleDescAttr(slot->tts_tupleDescriptor, index)->atttypid != sequence_type_oid) {
            continue;
        }
        const Datum value = slot->tts_values[index];
        if (value == Datum{0}) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("non-NULL bioseq dispatch output has a null Datum"),
                     errdetail("attribute=%d", index + 1)));
        }
        if (!VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(value))) {
            continue;
        }
        const AttrNumber attno = static_cast<AttrNumber>(index + 1);
        Datum retained = Datum{0};
        pg_sequence_descriptor descriptor{};
        if (attno == mmsearch_state.seq_attno && entry.has_seq_recipe() &&
            !entry.target_from_evaluated_expression) {
            retained = entry.seq_recipe_datum;
            descriptor = entry.seq_recipe_descriptor;
        } else if (attno == mmsearch_state.per_row_query_attno &&
                   entry.has_per_row_query_recipe() && !entry.query_from_evaluated_expression) {
            retained = entry.per_row_query_recipe_datum;
            descriptor = entry.per_row_query_recipe_descriptor;
        } else {
            descriptor = classify_and_describe_sequence(value);
            const bool expanded = descriptor.kind == pg_sequence_datum_kind::expanded_read_only ||
                                  descriptor.kind == pg_sequence_datum_kind::expanded_read_write;
            if (!expanded) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("expanded bioseq output has invalid descriptor")));
            }
            retained = retain_expanded_recipe_(value);
        }
        entry.expanded_sequence_attributes.push_back(
            buffered_scan_entry::retained_expanded_attribute{
                .attno = attno,
                .datum = retained,
                .descriptor = descriptor,
            });
    }
}

/** @brief Implements the tuple dispatch buffer contract declared by the stable buffer facade. */
tuple_dispatch_buffer::tuple_dispatch_buffer(const size_t estimated_tuple_count) {
    buffered_entries.reserve(estimated_tuple_count * 2);
    output_cursors.reserve(estimated_tuple_count * 2);
}

/** @brief Implements the bind persisted gpu operation stats sink contract declared by the stable
 * buffer facade. */
void tuple_dispatch_buffer::bind_persisted_gpu_operation_stats_sink(
    bioseq::pipeline_stats::persisted_gpu_operation_accumulator& sink) {
    if (persisted_gpu_operation_stats_sink != nullptr &&
        persisted_gpu_operation_stats_sink != &sink) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq persisted GPU stats sink changed during execution")));
    }
    persisted_gpu_operation_stats_sink = &sink;
}

/** @brief Implements the bind slice async owner contract declared by the stable buffer facade. */
void tuple_dispatch_buffer::bind_slice_async_owner(slice_async_owner_guard* owner) {
    if (owner == nullptr || !make_slice_async_pool_observer_noexcept(owner)) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq async owner is not accepting tracked jobs")));
    }
    if (slice_async_owner != nullptr && slice_async_owner != owner) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq async owner changed during dispatch")));
    }
    slice_async_owner = owner;
    mmsearch_target_payload_observer =
        make_slice_payload_allocation_observer_noexcept(owner->attachment,
                                                        0u,
                                                        materialization_mechanism::packed_region,
                                                        materialization_reason::mmsearch_target);
    mmsearch_query_payload_observer =
        make_slice_payload_allocation_observer_noexcept(owner->attachment,
                                                        0u,
                                                        materialization_mechanism::packed_region,
                                                        materialization_reason::mmsearch_query);
}

/** @brief Implements the bind sequence source provider contract declared by the stable buffer
 * facade. */
void tuple_dispatch_buffer::bind_sequence_source_provider(
    bioseq::storage::pg_sequence_source_provider* provider) {
    if (provider == nullptr || !provider->open || provider->lease.registry == nullptr) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq dispatch source provider is not live")));
    }
    if (source_provider != nullptr && source_provider != provider) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq dispatch source provider changed during execution")));
    }
    source_provider = provider;
}

/** @brief Implements the clear sequence source provider binding contract declared by the stable
 * buffer facade. */
void tuple_dispatch_buffer::clear_sequence_source_provider_binding() noexcept {
    source_provider = nullptr;
}

/** @brief Implements the release per row slice async owner after drain contract declared by the
 * stable buffer facade. */
void tuple_dispatch_buffer::release_per_row_slice_async_owner_after_drain() {
    if (join_mode || !buffered_entries.empty() || !match_map.empty() || !output_cursors.empty() ||
        output_position != -1 || active_wave_read_session != nullptr ||
        active_wave_permit.has_value()) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq per-row async owner was released before dispatch drained")));
    }
    slice_async_owner = nullptr;
    mmsearch_target_payload_observer = {};
    mmsearch_query_payload_observer = {};
}

/** @brief Implements the release join slice async owner after drain contract declared by the stable
 * buffer facade. */
void tuple_dispatch_buffer::release_join_slice_async_owner_after_drain() {
    if (!join_mode || join_target_active || join_target_prepared || !join_queries.empty() ||
        !join_match_map.empty() || !join_output_cursors.empty() ||
        active_wave_read_session != nullptr || active_wave_permit.has_value()) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq batch join async owner was released before dispatch drained")));
    }
    slice_async_owner = nullptr;
    mmsearch_target_payload_observer = {};
    mmsearch_query_payload_observer = {};
}

/** @brief Implements the configure params contract declared by the stable buffer facade. */
void tuple_dispatch_buffer::configure_params(
    const ::bioseq::simd::minimap2_parameters& params,
    bioseq::indexing::index_read_requirement read_requirement,
    bool read_sequence_index,
    Oid scan_user_relid,
    Oid scan_user_database_oid,
    Oid scan_user_tablespace_oid,
    Oid scan_user_main_relfilenode,
    Relation executor_relation,
    Snapshot executor_snapshot,
    MemoryContext executor_query_context) {
    require_registered_mvcc_snapshot_(executor_snapshot);
    if (executor_query_context == nullptr || !MemoryContextIsValid(executor_query_context)) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq dispatch executor query context is invalid")));
    }
    if (executor_relation == nullptr || RelationGetRelid(executor_relation) != scan_user_relid ||
        executor_relation->rd_locator.dbOid != scan_user_database_oid ||
        executor_relation->rd_locator.spcOid != scan_user_tablespace_oid ||
        static_cast<Oid>(executor_relation->rd_locator.relNumber) != scan_user_main_relfilenode) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq dispatch executor relation identity is invalid")));
    }
    base_params = params;
    index_read_requirement = read_requirement;
    read_sequence_index_active =
        read_requirement == bioseq::indexing::index_read_requirement::require_absent ? false
        : read_requirement == bioseq::indexing::index_read_requirement::require_compatible_persisted
            ? true
            : read_sequence_index;
    if (mmsearch_state.per_row_evaluated) {
        require_evaluated_mmsearch_index_policy_();
    }
    user_relid = scan_user_relid;
    user_main_database_oid = scan_user_database_oid;
    user_main_tablespace_oid = scan_user_tablespace_oid;
    user_main_relfilenode = scan_user_main_relfilenode;
    scan_snapshot = executor_snapshot;
    executor_query_mctx = executor_query_context;
    executor_read_context = bioseq::storage::indexing::borrowed_executor_read_context::bind_checked(
        executor_relation, executor_snapshot);
    if (!executor_read_context.has_value()) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq dispatch borrowed executor context is invalid")));
    }
    last_stats = ::bioseq::pipeline_stats::pipeline_execution_stats{};
    /*
     * The payload arena is parented on CurrentMemoryContext
     * at BeginCustomScan time (the executor per-query context), so the
     * bulk-fetched bioseq::pg_types::seq* survive the full scan, not just one tuple.
     */
    if (batch_payload_ctx == nullptr) {
        batch_payload_ctx = AllocSetContextCreate(
            executor_query_mctx, "tuple_dispatch_buffer payload arena", ALLOCSET_DEFAULT_SIZES);
    }
    if (fetch_bulk_sort_scratch.size() < dispatch_batch_capacity) {
        fetch_bulk_sort_scratch.resize(dispatch_batch_capacity);
    }
}

/** @brief Implements the replace executor snapshot contract declared by the stable buffer facade.
 */
void tuple_dispatch_buffer::replace_executor_snapshot(Snapshot executor_snapshot) {
    require_registered_mvcc_snapshot_(executor_snapshot);
    Relation executor_relation =
        executor_read_context.has_value() ? executor_read_context->relation() : nullptr;
    executor_read_context = bioseq::storage::indexing::borrowed_executor_read_context::bind_checked(
        executor_relation, executor_snapshot);
    if (!executor_read_context.has_value()) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq dispatch could not rebind its borrowed executor snapshot")));
    }
    scan_snapshot = executor_snapshot;
}

/** @brief Implements the configure mmsearch contract declared by the stable buffer facade. */
void tuple_dispatch_buffer::configure_mmsearch(AttrNumber seq_attno,
                                               std::string column_name,
                                               std::vector<char> query_varlena_bytes,
                                               uint8_t alphabet_code,
                                               uint8_t storage_unit_byte_width,
                                               AttrNumber primary_key_attno) {
    mmsearch_state.seq_attno = seq_attno;
    mmsearch_state.primary_key_attno = primary_key_attno;
    mmsearch_state.column_name = std::move(column_name);
    mmsearch_state.query_varlena = std::move(query_varlena_bytes);
    mmsearch_state.alphabet_code = alphabet_code;
    mmsearch_state.storage_unit_byte_width = storage_unit_byte_width;
    mmsearch_state.query_alphabet_code = alphabet_code;
    mmsearch_state.query_storage_unit_byte_width = storage_unit_byte_width;
    mmsearch_state.coalesce_active = false;
    mmsearch_state.coalesced_query_varlena.clear();
    mmsearch_state.per_row_query_attno = InvalidAttrNumber;
    mmsearch_state.per_row_self_alignment = false;
    mmsearch_state.per_row_evaluated = false;
    mmsearch_state.active = true;

    uint32_t query_bp_size = 0;
    if (const auto* hdr = mmsearch_state.query_hdr()) {
        query_bp_size = hdr->bp_size;
    }
    bioseq_log<logging_level::debug>(
        "[bioseq] pg: tuple_dispatch_buffer::configure_mmsearch() seq_attno={} pk_attno={} "
        "column='{}' typmod=({},{}) query_bp_size={}",
        static_cast<int>(seq_attno),
        static_cast<int>(primary_key_attno),
        mmsearch_state.column_name,
        static_cast<unsigned>(alphabet_code),
        static_cast<unsigned>(storage_unit_byte_width),
        query_bp_size);
}

/** @brief Implements the configure mmsearch per row contract declared by the stable buffer facade.
 */
void tuple_dispatch_buffer::configure_mmsearch_per_row(AttrNumber seq_attno,
                                                       std::string column_name,
                                                       AttrNumber per_row_query_attno,
                                                       bool per_row_self_alignment,
                                                       uint8_t alphabet_code,
                                                       uint8_t storage_unit_byte_width,
                                                       uint8_t query_alphabet_code,
                                                       uint8_t query_storage_unit_byte_width,
                                                       AttrNumber primary_key_attno) {
    if (alphabet_code != query_alphabet_code) {
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("bioseq mmsearch requires operands with the same bound alphabet")));
    }
    mmsearch_state.seq_attno = seq_attno;
    mmsearch_state.primary_key_attno = primary_key_attno;
    mmsearch_state.column_name = std::move(column_name);
    mmsearch_state.query_varlena.clear();
    mmsearch_state.coalesced_query_varlena.clear();
    mmsearch_state.coalesce_active = false;
    mmsearch_state.alphabet_code = alphabet_code;
    mmsearch_state.storage_unit_byte_width = storage_unit_byte_width;
    mmsearch_state.query_alphabet_code = query_alphabet_code;
    mmsearch_state.query_storage_unit_byte_width = query_storage_unit_byte_width;
    mmsearch_state.per_row_query_attno = per_row_query_attno;
    mmsearch_state.per_row_self_alignment = per_row_self_alignment;
    mmsearch_state.per_row_evaluated = false;
    mmsearch_state.active = true;

    bioseq_log<logging_level::debug>(
        "[bioseq] pg: tuple_dispatch_buffer::configure_mmsearch_per_row() seq_attno={} "
        "per_row_query_attno={} self_alignment={} target_typmod=({},{}) query_typmod=({},{})",
        static_cast<int>(seq_attno),
        static_cast<int>(per_row_query_attno),
        per_row_self_alignment,
        static_cast<unsigned>(alphabet_code),
        static_cast<unsigned>(storage_unit_byte_width),
        static_cast<unsigned>(query_alphabet_code),
        static_cast<unsigned>(query_storage_unit_byte_width));
}

/** @brief Implements the configure mmsearch per row evaluated contract declared by the stable
 * buffer facade. */
void tuple_dispatch_buffer::configure_mmsearch_per_row_evaluated(
    const AttrNumber seq_attno,
    std::string column_name,
    const std::uint8_t alphabet_code,
    const std::uint8_t storage_unit_byte_width,
    const std::uint8_t query_alphabet_code,
    const std::uint8_t query_storage_unit_byte_width,
    const AttrNumber primary_key_attno) {
    if (alphabet_code != query_alphabet_code) {
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("bioseq mmsearch requires operands with the same bound alphabet")));
    }
    mmsearch_state.seq_attno = seq_attno;
    mmsearch_state.primary_key_attno = primary_key_attno;
    mmsearch_state.column_name = std::move(column_name);
    mmsearch_state.query_varlena.clear();
    mmsearch_state.coalesced_query_varlena.clear();
    mmsearch_state.coalesce_active = false;
    mmsearch_state.alphabet_code = alphabet_code;
    mmsearch_state.storage_unit_byte_width = storage_unit_byte_width;
    mmsearch_state.query_alphabet_code = query_alphabet_code;
    mmsearch_state.query_storage_unit_byte_width = query_storage_unit_byte_width;
    mmsearch_state.per_row_query_attno = InvalidAttrNumber;
    mmsearch_state.per_row_self_alignment = false;
    mmsearch_state.per_row_evaluated = true;
    mmsearch_state.active = true;
}

/** @brief Implements the configure mmsearch join contract declared by the stable buffer facade. */
void tuple_dispatch_buffer::configure_mmsearch_join(
    const Oid target_relation_oid,
    const AttrNumber target_storage_seq_attno,
    const AttrNumber target_slot_seq_resno,
    const AttrNumber target_slot_rowno_resno,
    const AttrNumber query_slot_seq_resno,
    std::string target_column_name,
    const std::uint8_t alphabet_code,
    const std::uint8_t storage_unit_byte_width,
    const std::uint8_t query_alphabet_code,
    const std::uint8_t query_storage_unit_byte_width) {
    if (!OidIsValid(target_relation_oid) || target_storage_seq_attno <= 0 ||
        target_slot_seq_resno <= 0 || target_slot_rowno_resno <= 0 || query_slot_seq_resno <= 0) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bioseq batch join received invalid relation/attribute metadata")));
    }
    if (alphabet_code != query_alphabet_code) {
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("bioseq mmsearch requires operands with the same bound alphabet")));
    }
    if (OidIsValid(user_relid) && user_relid != target_relation_oid) {
        ereport(
            ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq batch join target relation disagrees with dispatch configuration")));
    }
    user_relid = target_relation_oid;
    join_mode = true;
    join_target_storage_seq_attno = target_storage_seq_attno;
    join_target_slot_seq_resno = target_slot_seq_resno;
    join_target_slot_rowno_resno = target_slot_rowno_resno;
    join_query_slot_seq_resno = query_slot_seq_resno;
    mmsearch_state = mmsearch_context{};
    mmsearch_state.seq_attno = target_slot_seq_resno;
    mmsearch_state.primary_key_attno = InvalidAttrNumber;
    mmsearch_state.column_name = std::move(target_column_name);
    mmsearch_state.alphabet_code = alphabet_code;
    mmsearch_state.storage_unit_byte_width = storage_unit_byte_width;
    mmsearch_state.query_alphabet_code = query_alphabet_code;
    mmsearch_state.query_storage_unit_byte_width = query_storage_unit_byte_width;
    mmsearch_state.active = true;
    reset_join_runtime_for_rescan();
}

/** @brief Implements the per row mode contract declared by the stable buffer facade. */
[[nodiscard]] bool tuple_dispatch_buffer::per_row_mode() const noexcept {
    return dispatch_uses_per_row_query(mmsearch_state.active,
                                       mmsearch_state.per_row_query_attno,
                                       mmsearch_state.per_row_evaluated);
}

/** @brief Implements the per row query attno contract declared by the stable buffer facade. */
[[nodiscard]] AttrNumber tuple_dispatch_buffer::per_row_query_attno() const {
    return mmsearch_state.per_row_query_attno;
}

/** @brief Implements the query source attno contract declared by the stable buffer facade. */
[[nodiscard]] AttrNumber tuple_dispatch_buffer::query_source_attno() const noexcept {
    if (!per_row_mode()) {
        return InvalidAttrNumber;
    }
    return mmsearch_state.per_row_query_attno != InvalidAttrNumber
               ? mmsearch_state.per_row_query_attno
               : mmsearch_state.seq_attno;
}

/** @brief Implements the coalesce active contract declared by the stable buffer facade. */
[[nodiscard]] bool tuple_dispatch_buffer::coalesce_active() const {
    return mmsearch_state.coalesce_active;
}

/** @brief Implements the per row self alignment contract declared by the stable buffer facade. */
[[nodiscard]] bool tuple_dispatch_buffer::per_row_self_alignment() const {
    return mmsearch_state.per_row_self_alignment;
}

/** @brief Implements the mmsearch alphabet code contract declared by the stable buffer facade. */
[[nodiscard]] uint8_t tuple_dispatch_buffer::mmsearch_alphabet_code() const {
    return mmsearch_state.alphabet_code;
}

/** @brief Implements the mmsearch storage unit byte width contract declared by the stable buffer
 * facade. */
[[nodiscard]] uint8_t tuple_dispatch_buffer::mmsearch_storage_unit_byte_width() const {
    return mmsearch_state.storage_unit_byte_width;
}

/** @brief Implements the clear mmsearch context contract declared by the stable buffer facade. */
void tuple_dispatch_buffer::clear_mmsearch_context(bool clear_extras) {
    mmsearch_state = mmsearch_context{};
    if (clear_extras) {
        extra_query_varlenas.clear();
    }
}

/** @brief Implements the add extra const query contract declared by the stable buffer facade. */
void tuple_dispatch_buffer::add_extra_const_query(std::vector<char> query_varlena_bytes) {
    const bioseq::pg_types::seq* hdr =
        reinterpret_cast<const bioseq::pg_types::seq*>(query_varlena_bytes.data());
    const uint32_t bp_size = (query_varlena_bytes.empty() || hdr == nullptr) ? 0u : hdr->bp_size;
    bioseq_log<logging_level::debug>(
        "[bioseq] pg: tuple_dispatch_buffer::add_extra_const_query() bp_size={} (extra index #{})",
        bp_size,
        extra_query_varlenas.size());
    extra_query_varlenas.push_back(std::move(query_varlena_bytes));
}

/** @brief Implements the extra query count contract declared by the stable buffer facade. */
[[nodiscard]] size_t tuple_dispatch_buffer::extra_query_count() const {
    return extra_query_varlenas.size();
}

} // namespace bioseq::scheduler
