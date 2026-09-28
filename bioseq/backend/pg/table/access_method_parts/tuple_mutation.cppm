/**
 * @file
 * @brief Insert, update, and delete Bioseq rows and their sequence sources.
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
export module bioseq.postgres.table:access_method_tuple_mutation;

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
import bioseq.postgres.storage.indexing.admin.policy_catalog;
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

/** Sequence writes waiting for confirmation of one speculative row. */
struct pending_speculative_sources {
    SubTransactionId subtransaction_id = InvalidSubTransactionId;
    std::vector<bioseq::storage::pending_sequence_source> sources;
};

/** Pending sequence writes indexed by the parent row's sezuence columns. */
using pending_sequence_source_slots =
    std::vector<std::optional<bioseq::storage::pending_sequence_source>>;

/** Relation OID and row number identifying a speculative insert. */
using pending_speculative_key = std::pair<Oid, std::uint64_t>;

/** Own  pending sequence writes until their speculative insert succeeds or fails. */
using pending_speculative_source_map =
    std::map<pending_speculative_key, pending_speculative_sources>;

static_assert(!std::is_trivially_destructible_v<pending_sequence_source_slots>);
static_assert(
    std::is_nothrow_move_constructible_v<bioseq::storage::pending_sequence_source>);

/** Return pending speculative inserts indexed by relation and row number */
[[nodiscard]] auto& access_pending_speculative_sources() {
    static pending_speculative_source_map pending;
    return pending;
}

/** Fixed-size diagnostics for unfinished inserts found after transaction completion. */
struct pending_speculative_terminal_quarantine {
    std::uint64_t invariant_events = 0u;
    std::uint64_t quarantined_rows = 0u;
    std::uint64_t quarantined_sources = 0u;
    XactEvent last_event = XACT_EVENT_COMMIT;
    bool storage_exhausted = false;
    bool active_map_quarantined = false;
};

/** Return storage for unfinished inserts saved after transaction completion. */
[[nodiscard]] auto& quarantined_speculative_sources_noexcept() noexcept {
    static std::array<pending_speculative_source_map, 4u> quarantined;
    return quarantined;
}

/** Return counters and flags for unfinished writes saved after transaction completion. */
[[nodiscard]] auto& speculative_source_quarantine_state_noexcept() noexcept {
    static pending_speculative_terminal_quarantine state;
    return state;
}

static_assert(noexcept(std::declval<pending_speculative_source_map&>().swap(
    std::declval<pending_speculative_source_map&>())));

/** Save  unexpected pending writes after transaction completion without allocating or throwing.
 *
 * Swapping into a spare map avoids running cleanup after the transaction has
 * ended. If every spare map is occupied, flag the active map to skip cleanup.
 */
void quarantine_pending_speculative_sources_noexcept(XactEvent event) noexcept {
    constexpr auto k_max_count = std::numeric_limits<std::uint64_t>::max();
    auto& pending = access_pending_speculative_sources();
    if (pending.empty()) return;

    std::uint64_t source_count = 0u;
    for (const auto& [key, entry] : pending) {
        (void)key;
        const auto entry_count =
            static_cast<std::uint64_t>(entry.sources.size());
        source_count = k_max_count - source_count < entry_count
                           ? k_max_count
                           : source_count + entry_count;
    }

    auto& state = speculative_source_quarantine_state_noexcept();
    if (state.invariant_events != k_max_count) ++state.invariant_events;
    const auto pending_rows = static_cast<std::uint64_t>(pending.size());
    state.quarantined_rows =
        k_max_count - state.quarantined_rows < pending_rows
            ? k_max_count
            : state.quarantined_rows + pending_rows;
    state.quarantined_sources =
        k_max_count - state.quarantined_sources < source_count
            ? k_max_count
            : state.quarantined_sources + source_count;
    state.last_event = event;

    for (auto& quarantine : quarantined_speculative_sources_noexcept()) {
        if (quarantine.empty()) {
            quarantine.swap(pending);
            return;
        }
    }
    state.storage_exhausted = true;
    state.active_map_quarantined = true;
}

/** Reject commit or prepare while a speculative insert is still unresolved. */
void require_no_pending_speculative_sources(const char* operation) {
    const auto& pending = access_pending_speculative_sources();
    if (pending.empty()) return;

    std::size_t source_count = 0;
    for (const auto& [key, entry] : pending) {
        (void)key;
        source_count += entry.sources.size();
    }
    ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq speculative source publication authority was not consumed"),
             errdetail("operation=%s speculative_rows=%zu pending_sources=%zu",
                       operation,
                       pending.size(),
                       source_count)));
}

/** Check unfinished speculative inserts before  commit and discard them on abort. */
void handle_pending_speculative_xact_event(XactEvent event) {
    switch (event) {
    case XACT_EVENT_COMMIT:
    case XACT_EVENT_PARALLEL_COMMIT:
    case XACT_EVENT_PREPARE:
        quarantine_pending_speculative_sources_noexcept(event);
        break;
    case XACT_EVENT_ABORT:
    case XACT_EVENT_PARALLEL_ABORT:
        if (!speculative_source_quarantine_state_noexcept()
                 .active_map_quarantined) {
            access_pending_speculative_sources().clear();
        }
        break;
    case XACT_EVENT_PRE_COMMIT:
    case XACT_EVENT_PARALLEL_PRE_COMMIT:
    case XACT_EVENT_PRE_PREPARE:
        require_no_pending_speculative_sources("pre-commit/pre-prepare");
        break;
    }
}

/** Reassign pendink inserts to a parent subtransaction or discard aborted inserts. */
void handle_pending_speculative_subxact_event(
    SubXactEvent event,
    SubTransactionId my_subid,
    SubTransactionId parent_subid) {
    switch (event) {
    case SUBXACT_EVENT_START_SUB:
    case SUBXACT_EVENT_COMMIT_SUB:
        for (auto& [key, pending] : access_pending_speculative_sources()) {
            (void)key;
            if (pending.subtransaction_id == my_subid) {
                pending.subtransaction_id = parent_subid;
            }
        }
        break;
    case SUBXACT_EVENT_ABORT_SUB:
        std::erase_if(
            access_pending_speculative_sources(),
            [my_subid](const auto& entry) {
                return entry.second.subtransaction_id == my_subid;
            });
        break;
    case SUBXACT_EVENT_PRE_COMMIT_SUB:
        break;
    }
}

#if PG_VERSION_NUM < PG_VERSION_18
/** PG17 error-context record for restoring the page-cache setting. */
struct ordinary_insert_page_cache_restore {
    ErrorContextCallback callback{};
    bool previous_value{};
    bool armed{};
};

static_assert(std::is_trivially_copyable_v<ordinary_insert_page_cache_restore>);
static_assert(std::is_standard_layout_v<ordinary_insert_page_cache_restore>);

/** Restore the page-cache setting when PostgreSQL unwinds an ordinary insert. */
static void restore_ordinary_insert_page_cache_on_error(void* argument) noexcept {
    auto* restore = static_cast<ordinary_insert_page_cache_restore*>(argument);
    if (geterrlevel() < ERROR || restore == nullptr || !restore->armed) {
        return;
    }
    bioseq::settings::column_cache::enabled = restore->previous_value;
    restore->armed = false;
}
#endif

/** Run an insert and restore the page-cache setting if it fails.
 *
 * Ordinary inserts also restore it on success. Speculative inserts keep, the
 * cache disabled until completion. Catch PostgreSQL errors only after C++
 * cleanup has finished, using a frame with no destructors to skip.
 */
template <bool restore_on_success, class p_function>
inline void run_insert_with_page_cache_restore(
    const char* operation, bool previous_value, p_function&& function) {
    static_assert(std::is_trivially_destructible_v<std::remove_reference_t<p_function>>);
#if PG_VERSION_NUM >= PG_VERSION_18
    PG_TRY();
    {
        run_tableam_cpp_checked(operation, std::forward<p_function>(function));
    }
    PG_CATCH();
    {
        bioseq::settings::column_cache::enabled = previous_value;
        PG_RE_THROW();
    }
    PG_END_TRY();
#else
    ordinary_insert_page_cache_restore cache_restore{
        .callback =
            {
                .previous = error_context_stack,
                .callback = &restore_ordinary_insert_page_cache_on_error,
                .arg = nullptr,
            },
        .previous_value = previous_value,
        .armed = true,
    };
    cache_restore.callback.arg = &cache_restore;
    error_context_stack = &cache_restore.callback;
    run_tableam_cpp_checked(operation, std::forward<p_function>(function));
    Assert(error_context_stack == &cache_restore.callback);
    cache_restore.armed = false;
    error_context_stack = cache_restore.callback.previous;
#endif
    if constexpr (restore_on_success) {
        bioseq::settings::column_cache::enabled = previous_value;
    }
}

/** Store sequence fragments and return row values containing their locators.
 *
 * Pass this array to write_column_row. The original  slot keeps full values for
 * PostgreSQL index keys. Both output vectors follow
 * table_cache.sequence_attribute_numbers, with null columns left empty.
 * Incoming locators are rejected because each parent row needs its own source
 *
 * Publish pending sources after writing the parent row. The returned array
 * belongs to the caller's current per-tuple context.
 */
[[nodiscard]] static Datum* rewrite_seq_columns_to_stubs(
    Relation relation,
    TupleTableSlot* slot,
    column_write_state* write_state,
    const bioseq::storage::table_access_cache& table_cache,
    Datum* detoasted_values,
    std::vector<std::optional<bioseq::storage::sequence_locator>>& out_per_col_locator,
    std::vector<std::optional<bioseq::storage::pending_sequence_source>>& out_pending_sources) {
    TupleDesc tuple_descriptor = slot->tts_tupleDescriptor;
    Datum* values_for_write = bioseq::postgres::support::invoke_pg_as_cpp([tuple_descriptor] {
        return static_cast<Datum*>(palloc(sizeof(Datum) * tuple_descriptor->natts));
    });
    std::memcpy(values_for_write, detoasted_values, sizeof(Datum) * tuple_descriptor->natts);

    out_per_col_locator.assign(table_cache.sequence_attribute_numbers.size(), std::nullopt);
    out_pending_sources.clear();
    out_pending_sources.resize(table_cache.sequence_attribute_numbers.size());
    if (table_cache.sequence_attribute_numbers.empty()) {
        return values_for_write;
    }
    // Create the cache here to avoid a circular import in write-state
    // management. Its parent context survives stripe-buffer resets.
    auto* caches = ensure_write_side_seq_caches(relation, write_state);
    const auto cap_bytes = caches->cap_bytes_snapshot;
    const std::uint64_t main_storage_id = bioseq::postgres::support::invoke_pg_as_cpp(
        [relation] { return column_storage_id(relation, false); });

    for (std::size_t sequence_column_index = 0;
         sequence_column_index < table_cache.sequence_attribute_numbers.size();
         ++sequence_column_index) {
        const std::int16_t attribute_number =
            table_cache.sequence_attribute_numbers[sequence_column_index];
        const int attribute_index = attribute_number - 1;
        if (attribute_index < 0 || attribute_index >= tuple_descriptor->natts)
            continue;
        if (slot->tts_isnull[attribute_index])
            continue;
        auto* sequence_header = reinterpret_cast<bioseq::pg_types::seq*>(
            DatumGetPointer(values_for_write[attribute_index]));
        if (bioseq::storage::is_locator_stub(sequence_header)) {
            bioseq::postgres::support::invoke_pg_as_cpp([] {
                bioseq_log<logging_level::error, logging_target::postgres>(
                    "bioseq storage: a locator stub cannot be shared by a second parent row");
            });
        }
        auto pending = bioseq::postgres::support::invoke_pg_as_cpp([relation,
                                                                    main_storage_id,
                                                                    attribute_number,
                                                                    tuple_descriptor,
                                                                    sequence_header,
                                                                    cap_bytes,
                                                                    caches] {
            return bioseq::storage::bioseq_default_storage_manager::insert_payload_as_fragments(
                RelationGetRelid(relation),
                main_storage_id,
                relation->rd_locator.dbOid,
                relation->rd_locator.spcOid,
                relation->rd_locator.relNumber,
                attribute_number,
                tuple_descriptor,
                sequence_header,
                cap_bytes,
                caches);
        });
        const auto sequence_locator = pending.locator();
        out_per_col_locator[sequence_column_index] = sequence_locator;
        out_pending_sources[sequence_column_index].emplace(std::move(pending));
        bioseq::pg_types::seq* locator_stub = bioseq::postgres::support::invoke_pg_as_cpp(
            [write_state, sequence_header, sequence_locator] {
                return bioseq::storage::alloc_locator_stub_in(
                    get_column_write_per_tuple_context(write_state),
                    static_cast<std::uint8_t>(sequence_header->alphabet_code),
                    static_cast<std::uint8_t>(sequence_header->storage_unit_byte_width),
                    sequence_header->padding_letters,
                    /*total_bp_size=*/sequence_locator.logical_length,
                    sequence_locator);
            });
        values_for_write[attribute_index] = PointerGetDatum(locator_stub);
    }
    return values_for_write;
}

/** Publish pending  sequence sources after the parent row has been written. */
static void publish_sequence_sources_after_parent_row(
    std::vector<std::optional<bioseq::storage::pending_sequence_source>>& sources) {
    for (auto& source : sources) {
        if (source.has_value()) {
            Assert(source->pending());
            bioseq::postgres::support::invoke_pg_as_cpp(
                [&source] { source->publish_after_parent_row(); });
            Assert(!source->pending());
        }
    }
}

/** Read full by-reference values before storing the row or builhing index values. */
[[nodiscard]] static Datum*
detoast_values_for_heap_tuple(TupleDesc tuple_desc, Datum* values, bool* nulls) {
    const auto previous_scope = exchange_slice_materialization_scope({
        .attachment = nullptr,
        .boundary_id = 0u,
        .producer_value_id = 0u,
        .mechanism = materialization_mechanism::packed_full,
        .reason = materialization_reason::heap_tuple,
        .local_sink = nullptr,
    });
    const auto scope_was_restored = [&previous_scope] {
        const auto active = get_current_slice_materialization_scope();
        return active.attachment == previous_scope.attachment &&
               active.boundary_id == previous_scope.boundary_id &&
               active.producer_value_id == previous_scope.producer_value_id &&
               active.mechanism == previous_scope.mechanism &&
               active.reason == previous_scope.reason &&
               active.local_sink == previous_scope.local_sink;
    };
    try {
        /* Slot copying normally materializes expanded values before storage.
         * Record materialization here too if a value reaches the table access
         * method still expanded. Its attached source can identify the planned
         * operation that caused the copy. */
        Datum* detoasted_values =
            bioseq::postgres::support::invoke_pg_as_cpp([tuple_desc, values, nulls] {
                return bioseq::postgres::support::detoast_values(tuple_desc, values, nulls);
            });
        restore_slice_materialization_scope(previous_scope);
        Assert(scope_was_restored());
        return detoasted_values;
    } catch (...) {
        restore_slice_materialization_scope(previous_scope);
        Assert(scope_was_restored());
        throw;
    }
}

/** Insert one row and publish sequence sources only after its parent write. */
void bioseq_tuple_insert(
    Relation relation, TupleTableSlot* slot, CommandId cid,
#if PG_VERSION_NUM >= 190000
    std::uint32_t options,
#else
    int options,
#endif
    BulkInsertState bistate) {
    column_write_state* write_state = nullptr;
    const bool previous_cache_value = bioseq::settings::column_cache::enabled;
    bioseq::settings::column_cache::enabled = false;
    run_insert_with_page_cache_restore<true>(
        "bioseq tuple insert", previous_cache_value, [relation, slot, &write_state] {
        if (relation->rd_rel->relpersistence == RELPERSISTENCE_TEMP) {
            bioseq::postgres::support::invoke_pg_as_cpp([] {
                bioseq_log<logging_level::error, logging_target::postgres>(
                    "bioseq tables cannot be TEMPORARY; use UNLOGGED or permanent");
            });
        }
        write_state = bioseq::postgres::support::invoke_pg_as_cpp([relation, slot] {
            return initialize_column_write_state(relation,
                                                 RelationGetDescr(relation),
                                                 slot->tts_tableOid,
                                                 GetCurrentSubTransactionId());
        });
        bioseq::postgres::support::invoke_pg_as_cpp(
            [relation] { validate_logical_replication_write(relation); });
        const auto* table_cache = bioseq::postgres::support::invoke_pg_as_cpp(
            [relation] { return &bioseq::storage::get_table_access_cache(relation); });

        {
            bioseq::postgres::support::memory_context_scope write_context_scope{
                get_column_write_per_tuple_context(write_state)};

            bioseq::postgres::support::invoke_pg_as_cpp([slot] { slot_getallattrs(slot); });

            Datum* detoasted_values = detoast_values_for_heap_tuple(
                slot->tts_tupleDescriptor, slot->tts_values, slot->tts_isnull);

            /* Keep full values for indexes while writing sequence locators to the row. */
            std::vector<std::optional<bioseq::storage::sequence_locator>> per_col_locator;
            std::vector<std::optional<bioseq::storage::pending_sequence_source>> pending_sources;
            Datum* values_for_write = rewrite_seq_columns_to_stubs(relation,
                                                                   slot,
                                                                   write_state,
                                                                   *table_cache,
                                                                   detoasted_values,
                                                                   per_col_locator,
                                                                   pending_sources);

            const uint64_t written_row_number =
                bioseq::postgres::support::invoke_pg_as_cpp([write_state, values_for_write, slot] {
                    return write_column_row(write_state, values_for_write, slot->tts_isnull);
                });
            slot->tts_tid = bioseq::postgres::support::invoke_pg_as_cpp(
                [written_row_number] { return row_number_to_tid(written_row_number); });
            publish_sequence_sources_after_parent_row(pending_sources);

            /* Update UPDATE_ON_INSERT sequence indexes using the  original payloads.
             * Storage locators cannot supply the index builder's sequence bytes. */
            bioseq::storage::indexing::index_build_on_insert_columns(
                RelationGetRelid(relation),
                table_cache->sequence_attribute_numbers,
                slot->tts_isnull,
                detoasted_values,
                per_col_locator,
                written_row_number,
                get_column_write_per_tuple_context(write_state));
        }
        bioseq::postgres::support::invoke_pg_as_cpp(
            [write_state] { MemoryContextReset(get_column_write_per_tuple_context(write_state)); });
        bioseq::postgres::support::invoke_pg_as_cpp(
            [relation] { pgstat_count_heap_insert(relation, 1); });
    });
    Assert(write_state != nullptr);
}

/** Insert a speculative row and keep its sequence writes until confirmation. */
void bioseq_tuple_insert_speculative(Relation relation,
                                     TupleTableSlot* slot,
                                     CommandId cid,
#if PG_VERSION_NUM >= 190000
                                     std::uint32_t options,
#else
                                     int options,
#endif
                                     BulkInsertState bistate,
                                     uint32_t spec_token) {
    previous_cache_enabled_state = bioseq::settings::column_cache::enabled;
    bioseq::settings::column_cache::enabled = false;

    column_write_state* write_state = nullptr;
    run_insert_with_page_cache_restore<false>(
        "bioseq speculative tuple insert", previous_cache_enabled_state,
        [relation, slot, &write_state] {
        if (relation->rd_rel->relpersistence == RELPERSISTENCE_TEMP) {
            bioseq::postgres::support::invoke_pg_as_cpp([] {
                bioseq_log<logging_level::error, logging_target::postgres>(
                    "bioseq tables cannot be TEMPORARY; use UNLOGGED or permanent");
            });
        }
        write_state = bioseq::postgres::support::invoke_pg_as_cpp([relation, slot] {
            return initialize_column_write_state(relation,
                                                 RelationGetDescr(relation),
                                                 slot->tts_tableOid,
                                                 GetCurrentSubTransactionId());
        });
        bioseq::postgres::support::invoke_pg_as_cpp(
            [relation] { validate_logical_replication_write(relation); });
        const auto* table_cache = bioseq::postgres::support::invoke_pg_as_cpp(
            [relation] { return &bioseq::storage::get_table_access_cache(relation); });

        {
            bioseq::postgres::support::memory_context_scope write_context_scope{
                get_column_write_per_tuple_context(write_state)};

            bioseq::postgres::support::invoke_pg_as_cpp([slot] { slot_getallattrs(slot); });

            Datum* detoasted_values = detoast_values_for_heap_tuple(
                slot->tts_tupleDescriptor, slot->tts_values, slot->tts_isnull);

            std::vector<std::optional<bioseq::storage::sequence_locator>> per_col_locator;
            std::vector<std::optional<bioseq::storage::pending_sequence_source>> pending_sources;
            Datum* values_for_write = rewrite_seq_columns_to_stubs(relation,
                                                                   slot,
                                                                   write_state,
                                                                   *table_cache,
                                                                   detoasted_values,
                                                                   per_col_locator,
                                                                   pending_sources);

            const uint64_t storage_id = bioseq::postgres::support::invoke_pg_as_cpp(
                [relation] { return lookup_storage_id(relation->rd_locator); });
            const uint64_t written_row_number =
                bioseq::postgres::support::invoke_pg_as_cpp([write_state, values_for_write, slot] {
                    return write_column_row(write_state, values_for_write, slot->tts_isnull);
                });
            bioseq::postgres::support::invoke_pg_as_cpp([relation, storage_id, written_row_number] {
                update_row_mask(relation->rd_locator, storage_id, nullptr, written_row_number);
            });
            slot->tts_tid = bioseq::postgres::support::invoke_pg_as_cpp(
                [written_row_number] { return row_number_to_tid(written_row_number); });
            const SubTransactionId subtransaction_id = bioseq::postgres::support::invoke_pg_as_cpp(
                [] { return GetCurrentSubTransactionId(); });
            pending_speculative_sources speculative{
                .subtransaction_id = subtransaction_id,
            };
            speculative.sources.reserve(pending_sources.size());
            for (auto& source : pending_sources) {
                if (source.has_value()) {
                    speculative.sources.push_back(std::move(*source));
                    Assert(!source->pending());
                    Assert(speculative.sources.back().pending());
                }
            }
            const pending_speculative_key key{RelationGetRelid(relation), written_row_number};
            if (!access_pending_speculative_sources().emplace(key, std::move(speculative)).second) {
                bioseq::postgres::support::invoke_pg_as_cpp([] {
                    bioseq_log<logging_level::error, logging_target::postgres>(
                        "bioseq storage: duplicate speculative source publication key");
                });
            }
        }
        bioseq::postgres::support::invoke_pg_as_cpp(
            [write_state] { MemoryContextReset(get_column_write_per_tuple_context(write_state)); });
        bioseq::postgres::support::invoke_pg_as_cpp(
            [relation] { pgstat_count_heap_insert(relation, 1); });
    });
    Assert(write_state != nullptr);
}

/** Publish or invalidate the sequence sources attached to a speculative row. */
void bioseq_tuple_complete_speculative(Relation relation,
                                       TupleTableSlot* slot,
                                       uint32_t spec_token,
                                       bool succeeded) {
    (void)spec_token;
    run_insert_with_page_cache_restore<true>(
        "bioseq complete speculative tuple insert", previous_cache_enabled_state,
        [relation, slot, succeeded] {
            const uint64_t storage_id = bioseq::postgres::support::invoke_pg_as_cpp(
                [relation] { return lookup_storage_id(relation->rd_locator); });
            /* Serialize changes to this storage ID until the transaction ends. */
            bioseq::postgres::support::invoke_pg_as_cpp([storage_id] {
                (void)DirectFunctionCall1(pg_advisory_xact_lock_int8,
                                          Int64GetDatum(static_cast<int64_t>(storage_id)));
            });
            const std::uint64_t row_number = bioseq::postgres::support::invoke_pg_as_cpp(
                [slot] { return tid_to_row_number(slot->tts_tid); });
            const pending_speculative_key key{RelationGetRelid(relation), row_number};
            auto pending = access_pending_speculative_sources().find(key);
            if (pending == access_pending_speculative_sources().end()) {
                bioseq::postgres::support::invoke_pg_as_cpp([] {
                    bioseq_log<logging_level::error, logging_target::postgres>(
                        "bioseq storage: speculative source publication authority is missing");
                });
            }
            if (succeeded) {
                for (auto& source : pending->second.sources) {
                    Assert(source.pending());
                    bioseq::postgres::support::invoke_pg_as_cpp(
                        [&source] { source.publish_after_parent_row(); });
                    Assert(!source.pending());
                }
            } else {
                for (auto& source : pending->second.sources) {
                    Assert(source.pending());
                    bioseq::postgres::support::invoke_pg_as_cpp(
                        [&source] { source.invalidate_without_parent_row(); });
                    Assert(!source.pending());
                }
            }
            access_pending_speculative_sources().erase(pending);
        });
}

/** Insert a batch, publishing each row's sequence sources after writing that row. */
void bioseq_multi_insert(Relation relation,
                         TupleTableSlot** slots,
                         int ntuples,
                         CommandId cid,
#if PG_VERSION_NUM >= 190000
                         std::uint32_t options,
#else
                         int options,
#endif
                         BulkInsertState bistate) {
    run_tableam_cpp_checked("bioseq multi insert", [relation, slots, ntuples] {
        if (relation->rd_rel->relpersistence == RELPERSISTENCE_TEMP) {
            bioseq::postgres::support::invoke_pg_as_cpp([] {
                bioseq_log<logging_level::error, logging_target::postgres>(
                    "bioseq tables cannot be TEMPORARY; use UNLOGGED or permanent");
            });
        }
        column_write_state* write_state =
            bioseq::postgres::support::invoke_pg_as_cpp([relation, slots] {
                return initialize_column_write_state(relation,
                                                     RelationGetDescr(relation),
                                                     slots[0]->tts_tableOid,
                                                     GetCurrentSubTransactionId());
            });
        bioseq::postgres::support::invoke_pg_as_cpp(
            [relation] { validate_logical_replication_write(relation); });
        const auto* table_cache = bioseq::postgres::support::invoke_pg_as_cpp(
            [relation] { return &bioseq::storage::get_table_access_cache(relation); });

        {
            bioseq::postgres::support::memory_context_scope write_context_scope{
                get_column_write_per_tuple_context(write_state)};

            for (int tuple_index = 0; tuple_index < ntuples; tuple_index++) {
                TupleTableSlot* tuple_slot = slots[tuple_index];

                bioseq::postgres::support::invoke_pg_as_cpp(
                    [tuple_slot] { slot_getallattrs(tuple_slot); });

                Datum* detoasted_values =
                    detoast_values_for_heap_tuple(tuple_slot->tts_tupleDescriptor,
                                                  tuple_slot->tts_values,
                                                  tuple_slot->tts_isnull);

                std::vector<std::optional<bioseq::storage::sequence_locator>> per_col_locator;
                std::vector<std::optional<bioseq::storage::pending_sequence_source>>
                    pending_sources;
                Datum* values_for_write = rewrite_seq_columns_to_stubs(relation,
                                                                       tuple_slot,
                                                                       write_state,
                                                                       *table_cache,
                                                                       detoasted_values,
                                                                       per_col_locator,
                                                                       pending_sources);

                const uint64_t written_row_number = bioseq::postgres::support::invoke_pg_as_cpp(
                    [write_state, values_for_write, tuple_slot] {
                        return write_column_row(
                            write_state, values_for_write, tuple_slot->tts_isnull);
                    });
                publish_sequence_sources_after_parent_row(pending_sources);

                bioseq::storage::indexing::index_build_on_insert_columns(
                    RelationGetRelid(relation),
                    table_cache->sequence_attribute_numbers,
                    tuple_slot->tts_isnull,
                    detoasted_values,
                    per_col_locator,
                    written_row_number,
                    get_column_write_per_tuple_context(write_state));

                EState* estate = bioseq::postgres::support::invoke_pg_as_cpp([relation] {
                    return bioseq::storage::metadata::tuple_ops::create_relation_executor_state(
                        relation);
                });

                ResultRelInfo* result_rel_info = bioseq::postgres::support::invoke_pg_as_cpp(
                    [] { return makeNode(ResultRelInfo); });
                bioseq::postgres::support::invoke_pg_as_cpp([result_rel_info, relation] {
                    InitResultRelInfo(result_rel_info, relation, 1, nullptr, 0);
                });

                bioseq::postgres::support::invoke_pg_as_cpp(
                    [result_rel_info] { ExecOpenIndices(result_rel_info, false); });

                if (relation->rd_att->constr)
                    bioseq::postgres::support::invoke_pg_as_cpp(
                        [result_rel_info, tuple_slot, estate] {
                            ExecConstraints(result_rel_info, tuple_slot, estate);
                        });

                bioseq::postgres::support::invoke_pg_as_cpp(
                    [result_rel_info] { ExecCloseIndices(result_rel_info); });

                bioseq::postgres::support::invoke_pg_as_cpp(
                    [estate] { AfterTriggerEndQuery(estate); });
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [estate] { ExecCloseResultRelations(estate); });
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [estate] { ExecCloseRangeTableRelations(estate); });

                bioseq::postgres::support::invoke_pg_as_cpp(
                    [estate] { ExecResetTupleTable(estate->es_tupleTable, false); });
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [estate] { FreeExecutorState(estate); });

                tuple_slot->tts_tid = bioseq::postgres::support::invoke_pg_as_cpp(
                    [written_row_number] { return row_number_to_tid(written_row_number); });

                bioseq::postgres::support::invoke_pg_as_cpp([write_state] {
                    MemoryContextReset(get_column_write_per_tuple_context(write_state));
                });
            }
        }
        bioseq::postgres::support::invoke_pg_as_cpp(
            [relation, ntuples] { pgstat_count_heap_insert(relation, ntuples); });
    });
}

/// Invalidate the sequence versions named by the old row before hiding it
/// MVCC keeps their, previous READY catalog versions visible to older snapshots.
/// Snapshots that see the invalidation can no longer open  those sources.
static void invalidate_sequence_source_epochs_for_row(Relation relation,
                                                      std::uint64_t row_number,
                                                      Snapshot snapshot) {
    const auto& table_cache = bioseq::storage::get_table_access_cache(relation);
    if (table_cache.sequence_attribute_numbers.empty())
        return;
    Snapshot read_snapshot = snapshot != nullptr ? snapshot : GetTransactionSnapshot();
    MemoryContext read_context = create_column_scan_memory_context();
    {
        bioseq::postgres::support::memory_context_scope context_guard{read_context};
        TupleDesc descriptor = RelationGetDescr(relation);
        bioseq::postgres::support::postgres_bitmapset attributes =
            bioseq::postgres::support::postgres_bitmapset::make_member_range(descriptor->natts);
        column_read_state* read_state = init_bioseq_read_state(relation,
                                                               descriptor,
                                                               attributes,
                                                               NIL,
                                                               read_context,
                                                               read_snapshot,
                                                               /*randomAccess=*/true,
                                                               nullptr);
        Datum* values = static_cast<Datum*>(
            palloc0(sizeof(Datum) * static_cast<std::size_t>(descriptor->natts)));
        bool* nulls =
            static_cast<bool*>(palloc0(sizeof(bool) * static_cast<std::size_t>(descriptor->natts)));
        if (read_column_row_by_number(read_state, row_number, values, nulls)) {
            for (const std::int16_t attribute_number : table_cache.sequence_attribute_numbers) {
                const int index = attribute_number - 1;
                if (index < 0 || index >= descriptor->natts || nulls[index])
                    continue;
                const auto* header =
                    reinterpret_cast<const bioseq::pg_types::seq*>(DatumGetPointer(values[index]));
                if (!bioseq::storage::is_locator_stub(header))
                    continue;
                const auto locator = bioseq::storage::decode_locator_from_stub(header);
                bioseq::storage::sequence_source_version_manager::invalidate_epoch(
                    locator.source_epoch);
            }
        }
        end_column_read(read_state);
    }
    MemoryContextDelete(read_context);
}

/** Invalidate a row's sequence sources, then mark the row as deleted. */
TM_Result bioseq_tuple_delete(Relation relation,
                              ItemPointer tid,
                              CommandId cid,
#if PG_VERSION_NUM >= 190000
                              std::uint32_t options,
#endif
                              Snapshot snapshot,
                              Snapshot crosscheck,
                              bool wait,
                              TM_FailureData* tmfd
#if PG_VERSION_NUM < 190000
                              , bool changing_part
#endif
) {
    if (bioseq::storage::is_registered_sequence_fragment_relation(RelationGetRelid(relation))) {
        bioseq_log<logging_level::error, logging_target::postgres>(
            "bioseq storage: registered sequence fragments are immutable; DELETE is forbidden");
    }
    uint64_t row_number = tid_to_row_number(*tid);
    uint64_t storage_id = lookup_storage_id(relation->rd_locator);
    /* Serialize changes to this storage ID until the transaction ends. */
    DirectFunctionCall1(pg_advisory_xact_lock_int8,
                        Int64GetDatum(static_cast<int64_t>(storage_id)));

    invalidate_sequence_source_epochs_for_row(relation, row_number, snapshot);
    if (!update_row_mask(relation->rd_locator, storage_id, snapshot, row_number))
        return TM_Deleted;

    pgstat_count_heap_delete(relation);

    return TM_Ok;
}

/** Hide the old row and insert its replacement with new jequence sources. */
TM_Result bioseq_tuple_update(Relation relation,
                              ItemPointer otid,
                              TupleTableSlot* slot,
                              CommandId cid,
#if PG_VERSION_NUM >= 190000
                              std::uint32_t options,
#endif
                              Snapshot snapshot,
                              Snapshot crosscheck,
                              bool wait,
                              TM_FailureData* tmfd,
                              LockTupleMode* lockmode,
                              TU_UpdateIndexes* update_indexes) {
    if (bioseq::storage::is_registered_sequence_fragment_relation(RelationGetRelid(relation))) {
        bioseq_log<logging_level::error, logging_target::postgres>(
            "bioseq storage: registered sequence fragments are immutable; UPDATE is forbidden");
    }

    uint64_t row_number = tid_to_row_number(*otid);
    uint64_t storage_id = lookup_storage_id(relation->rd_locator);

    /* Serialize changes to this storage ID until the transaction ends. */
    DirectFunctionCall1(pg_advisory_xact_lock_int8,
                        Int64GetDatum(static_cast<int64_t>(storage_id)));

    invalidate_sequence_source_epochs_for_row(relation, row_number, snapshot);
    if (!update_row_mask(relation->rd_locator, storage_id, snapshot, row_number))
        return TM_Deleted;

    bioseq_tuple_insert(relation, slot, cid, 0, nullptr);

    *update_indexes = static_cast<TU_UpdateIndexes>(true);

    pgstat_count_heap_update(relation, false, false);
    return TM_Ok;
}

/** Read a row into the tuple-lock callback's slot using the transaction snapshot. */
TM_Result bioseq_tuple_lock(Relation relation,
                            ItemPointer tid,
                            Snapshot snapshot,
                            TupleTableSlot* slot,
                            CommandId cid,
                            LockTupleMode mode,
                            LockWaitPolicy wait_policy,
                            uint8_t flags,
                            TM_FailureData* tmfd) {
    uint64_t row_number = tid_to_row_number(*tid);
    column_read_state* read_state = nullptr;

    int natts = relation->rd_att->natts;
    bioseq::postgres::support::postgres_bitmapset attr_needed =
        bioseq::postgres::support::postgres_bitmapset::make_member_range(natts);

    List* scan_qual = NIL;

    bool random_access = true;

    read_state = init_bioseq_read_state(relation,
                                        slot->tts_tupleDescriptor,
                                        attr_needed,
                                        scan_qual,
                                        CurrentMemoryContext, // TODO: Check the lifetime of the reader allocated here.
                                        GetTransactionSnapshot(),
                                        random_access,
                                        nullptr);

    read_column_row_by_number(read_state, row_number, slot->tts_values, slot->tts_isnull);

    slot->tts_tableOid = RelationGetRelid(relation);
    slot->tts_tid = *tid;

    if (TTS_EMPTY(slot)) {
        ExecStoreVirtualTuple(slot);
    }

    return TM_Ok;
}

/** Leave write states open for transaction-end flushing after bulk insertion. */
void bioseq_finish_bulk_insert(Relation relation,
#if PG_VERSION_NUM >= 190000
                              std::uint32_t options
#else
                              int options
#endif
) {

}
