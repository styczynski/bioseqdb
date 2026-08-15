/**
 * @file
 * @brief Implements tuple insertion, speculative publication, and row mutation callbacks.
 */
module;
#include <algorithm>
#include <array>
#include <bioseq/pg_compat.h>
#include <cmath>
#include <cstring>
#include <exception>
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
import bioseq.postgres.storage.indexing.admin;
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
import :access_identity;
import :access_common;
import :access_hooks;
import :access_method_scan_callbacks;

/**
 * Pending sequence-source authority retained for one speculative parent row.
 */
struct pending_speculative_sources {
    SubTransactionId subtransaction_id = InvalidSubTransactionId;
    std::vector<bioseq::storage::pending_sequence_source> sources;
};

using pending_sequence_source_slots =
    std::vector<std::optional<bioseq::storage::pending_sequence_source>>;
using pending_speculative_key = std::pair<Oid, std::uint64_t>;
using pending_speculative_source_map =
    std::map<pending_speculative_key, pending_speculative_sources>;

static_assert(!std::is_trivially_destructible_v<pending_sequence_source_slots>);
static_assert(std::is_nothrow_move_constructible_v<bioseq::storage::pending_sequence_source>);

/** Returns active speculative authorities keyed by relation and virtual row. */
[[nodiscard]] auto& access_pending_speculative_sources() {
    static pending_speculative_source_map pending;
    return pending;
}

/** PostgreSQL error-context record for restoring the page-cache setting. */
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

/** Return the write-side sequence cache owned by a column write state. */
[[nodiscard]] static bioseq::storage::seq_caches*
ensure_write_side_seq_caches(Relation relation, column_write_state* write_state) {
    if (relation == nullptr || write_state == nullptr ||
        write_state->stripe_write_context == nullptr) {
        bioseq::postgres::support::invoke_pg_as_cpp([] {
            bioseq_log<logging_level::error, logging_target::postgres>(
                "bioseq storage: sequence write cache owner is unavailable");
        });
    }

    if (write_state->seq_caches == nullptr) {
        MemoryContext owner = MemoryContextGetParent(write_state->stripe_write_context);
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
        fresh->cap_bytes_snapshot = bioseq::storage::read_max_sub_table_cap_bytes();
        write_state->seq_caches = fresh;
    }

    auto* caches = static_cast<bioseq::storage::seq_caches*>(write_state->seq_caches);
    if (caches->side != bioseq::storage::seq_caches::access_side::write ||
        caches->main_database_oid != relation->rd_locator.dbOid ||
        caches->main_tablespace_oid != relation->rd_locator.spcOid ||
        caches->main_relfilenode != relation->rd_locator.relNumber) {
        bioseq::postgres::support::invoke_pg_as_cpp([] {
            bioseq_log<logging_level::error, logging_target::postgres>(
                "bioseq storage: sequence write cache relation identity changed");
        });
    }
    return caches;
}

/**
 * Persist every non-null sequence value and replace it with a locator stub.
 *
 * Rewrites each sequence column in `detoasted_values`
 * to a 40-byte locator stub. The original payload bytes are written to the
 * matching sub-table shard via sequence_storage_manager::insert_payload.
 * Returns a freshly-palloc'd Datum* in the per-tuple context. The caller
 * MUST hand THIS to write_column_row (NOT slot->tts_values) so that
 * ExecInsertIndexTuples — which reads from `slot->tts_values[]` to build
 * btree/hash/gin keys — never sees the storage-layout bytes.
 *
 * `out_per_col_locator[c]` is the locator for amc.sequence_attribute_numbers[c]. It remains
 * std::nullopt for null or already-stub values.
 */
[[nodiscard]] static Datum* rewrite_seq_columns_to_stubs(
    Relation relation,
    TupleTableSlot* slot,
    column_write_state* write_state,
    const bioseq::storage::table_access_cache& amc,
    Datum* detoasted_values,
    std::vector<std::optional<bioseq::storage::sequence_locator>>& out_per_col_locator,
    std::vector<std::optional<bioseq::storage::pending_sequence_source>>& out_pending_sources) {
    TupleDesc td = slot->tts_tupleDescriptor;
    Datum* values_for_write = bioseq::postgres::support::invoke_pg_as_cpp(
        [td] { return static_cast<Datum*>(palloc(sizeof(Datum) * td->natts)); });
    std::memcpy(values_for_write, detoasted_values, sizeof(Datum) * td->natts);

    out_per_col_locator.assign(amc.sequence_attribute_numbers.size(), std::nullopt);
    out_pending_sources.clear();
    out_pending_sources.resize(amc.sequence_attribute_numbers.size());
    if (amc.sequence_attribute_numbers.empty()) {
        return values_for_write;
    }
    // write_state_management.cppm cannot import bioseq_storage without a
    // module cycle, so the TableAM owns this lazy initialization.  The cache
    // lives above stripe_write_context and therefore survives stripe resets.
    auto* caches = ensure_write_side_seq_caches(relation, write_state);
    const auto cap_bytes = caches->cap_bytes_snapshot;
    const std::uint64_t main_storage_id = bioseq::postgres::support::invoke_pg_as_cpp(
        [relation] { return column_storage_id(relation, false); });

    for (std::size_t c = 0; c < amc.sequence_attribute_numbers.size(); ++c) {
        const std::int16_t attno = amc.sequence_attribute_numbers[c];
        const int idx = attno - 1;
        if (idx < 0 || idx >= td->natts)
            continue;
        if (slot->tts_isnull[idx])
            continue;
        auto* hdr =
            reinterpret_cast<bioseq::pg_types::seq*>(DatumGetPointer(values_for_write[idx]));
        if (bioseq::storage::is_locator_stub(hdr)) {
            bioseq::postgres::support::invoke_pg_as_cpp([] {
                bioseq_log<logging_level::error, logging_target::postgres>(
                    "bioseq storage: a locator stub cannot be shared by a second parent row");
            });
        }
        auto pending = bioseq::postgres::support::invoke_pg_as_cpp(
            [relation, main_storage_id, attno, td, hdr, cap_bytes, caches] {
                return bioseq::storage::bioseq_default_storage_manager::insert_payload_as_fragments(
                    RelationGetRelid(relation),
                    main_storage_id,
                    relation->rd_locator.dbOid,
                    relation->rd_locator.spcOid,
                    relation->rd_locator.relNumber,
                    attno,
                    td,
                    hdr,
                    cap_bytes,
                    caches);
            });
        const auto loc = pending.locator();
        out_per_col_locator[c] = loc;
        out_pending_sources[c].emplace(std::move(pending));
        bioseq::pg_types::seq* stub =
            bioseq::postgres::support::invoke_pg_as_cpp([write_state, hdr, loc] {
                return bioseq::storage::alloc_locator_stub_in(
                    get_column_write_per_tuple_context(write_state),
                    static_cast<std::uint8_t>(hdr->alphabet_code),
                    static_cast<std::uint8_t>(hdr->storage_unit_byte_width),
                    hdr->padding_letters,
                    /*total_bp_size=*/loc.logical_length,
                    loc);
            });
        values_for_write[idx] = PointerGetDatum(stub);
    }
    return values_for_write;
}

/** Publish pending sequence sources only after their parent row is durable. */
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

/** Detoast by-reference attributes before storage or index expression evaluation. */
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
        /* Destination-slot copyslot is the primary durable-write boundary.
         * Keep this TableAM scope as a fallback when an expanded value reaches
         * the access method without having been flattened there.  Each attached
         * EOH can still resolve its exact planned producer boundary. */
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
    Relation relation, TupleTableSlot* slot, CommandId cid, int options, BulkInsertState bistate) {
    column_write_state* write_state = nullptr;
    ordinary_insert_page_cache_restore cache_restore{
        .callback =
            {
                .previous = error_context_stack,
                .callback = &restore_ordinary_insert_page_cache_on_error,
                .arg = nullptr,
            },
        .previous_value = bioseq::settings::column_cache::enabled,
        .armed = true,
    };
    cache_restore.callback.arg = &cache_restore;
    error_context_stack = &cache_restore.callback;
    bioseq::settings::column_cache::enabled = false;
    run_tableam_cpp_checked("bioseq tuple insert", [relation, slot, &write_state] {
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
        const auto* amc = bioseq::postgres::support::invoke_pg_as_cpp(
            [relation] { return &bioseq::storage::get_table_access_cache(relation); });

        {
            bioseq::postgres::support::memory_context_scope ctx_guard{
                get_column_write_per_tuple_context(write_state)};

            bioseq::postgres::support::invoke_pg_as_cpp([slot] { slot_getallattrs(slot); });

            Datum* detoasted_values = detoast_values_for_heap_tuple(
                slot->tts_tupleDescriptor, slot->tts_values, slot->tts_isnull);

            /* rewrite SEQ columns to locator stubs. */
            std::vector<std::optional<bioseq::storage::sequence_locator>> per_col_locator;
            std::vector<std::optional<bioseq::storage::pending_sequence_source>> pending_sources;
            Datum* values_for_write = rewrite_seq_columns_to_stubs(relation,
                                                                   slot,
                                                                   write_state,
                                                                   *amc,
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

            /* build every UPDATE_ON_INSERT index (per SEQ column)
             * via the new chunked-storage drivers. detoasted_values carries the
             * ORIGINAL payload (pre-stub) so the index sees the real bytes. */
            bioseq::storage::indexing::index_build_on_insert_columns(
                RelationGetRelid(relation),
                amc->sequence_attribute_numbers,
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
    Assert(error_context_stack == &cache_restore.callback);
    bioseq::settings::column_cache::enabled = cache_restore.previous_value;
    cache_restore.armed = false;
    error_context_stack = cache_restore.callback.previous;
}

/** Insert one speculative row while retaining revocable source authorities. */
void bioseq_tuple_insert_speculative(Relation relation,
                                     TupleTableSlot* slot,
                                     CommandId cid,
                                     int options,
                                     BulkInsertState bistate,
                                     uint32_t spec_token) {
    previous_cache_enabled_state = bioseq::settings::column_cache::enabled;
    bioseq::settings::column_cache::enabled = false;

    ordinary_insert_page_cache_restore cache_restore{
        .callback =
            {
                .previous = error_context_stack,
                .callback = &restore_ordinary_insert_page_cache_on_error,
                .arg = nullptr,
            },
        .previous_value = previous_cache_enabled_state,
        .armed = true,
    };
    cache_restore.callback.arg = &cache_restore;
    error_context_stack = &cache_restore.callback;
    column_write_state* write_state = nullptr;
    run_tableam_cpp_checked("bioseq speculative tuple insert", [relation, slot, &write_state] {
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
        const auto* amc = bioseq::postgres::support::invoke_pg_as_cpp(
            [relation] { return &bioseq::storage::get_table_access_cache(relation); });

        {
            bioseq::postgres::support::memory_context_scope ctx_guard{
                get_column_write_per_tuple_context(write_state)};

            bioseq::postgres::support::invoke_pg_as_cpp([slot] { slot_getallattrs(slot); });

            Datum* detoasted_values = detoast_values_for_heap_tuple(
                slot->tts_tupleDescriptor, slot->tts_values, slot->tts_isnull);

            std::vector<std::optional<bioseq::storage::sequence_locator>> per_col_locator;
            std::vector<std::optional<bioseq::storage::pending_sequence_source>> pending_sources;
            Datum* values_for_write = rewrite_seq_columns_to_stubs(relation,
                                                                   slot,
                                                                   write_state,
                                                                   *amc,
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
    Assert(error_context_stack == &cache_restore.callback);
    cache_restore.armed = false;
    error_context_stack = cache_restore.callback.previous;
}

/** Publish or invalidate the sequence sources attached to a speculative row. */
void bioseq_tuple_complete_speculative(Relation relation,
                                       TupleTableSlot* slot,
                                       uint32_t spec_token,
                                       bool succeeded) {
    (void)spec_token;
    ordinary_insert_page_cache_restore cache_restore{
        .callback =
            {
                .previous = error_context_stack,
                .callback = &restore_ordinary_insert_page_cache_on_error,
                .arg = nullptr,
            },
        .previous_value = previous_cache_enabled_state,
        .armed = true,
    };
    cache_restore.callback.arg = &cache_restore;
    error_context_stack = &cache_restore.callback;
    run_tableam_cpp_checked(
        "bioseq complete speculative tuple insert", [relation, slot, succeeded] {
            const uint64_t storage_id = bioseq::postgres::support::invoke_pg_as_cpp(
                [relation] { return lookup_storage_id(relation->rd_locator); });
            /* Set lock for relation until transaction ends. */
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
    Assert(error_context_stack == &cache_restore.callback);
    bioseq::settings::column_cache::enabled = previous_cache_enabled_state;
    cache_restore.armed = false;
    error_context_stack = cache_restore.callback.previous;
}

/** Insert a batch of rows while preserving per-row source publication order. */
void bioseq_multi_insert(Relation relation,
                         TupleTableSlot** slots,
                         int ntuples,
                         CommandId cid,
                         int options,
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
        const auto* amc = bioseq::postgres::support::invoke_pg_as_cpp(
            [relation] { return &bioseq::storage::get_table_access_cache(relation); });

        {
            bioseq::postgres::support::memory_context_scope ctx_guard{
                get_column_write_per_tuple_context(write_state)};

            for (int i = 0; i < ntuples; i++) {
                TupleTableSlot* tuple_slot = slots[i];

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
                                                                       *amc,
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
                    amc->sequence_attribute_numbers,
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

/// Invalidate only the epoch(s) carried by the exact old parent row before its
/// row mask changes. The catalog update is MVCC: an older registered snapshot
/// continues to see the prior READY tuple, while a newer snapshot cannot admit
/// the masked source.
static void invalidate_sequence_source_epochs_for_row(Relation relation,
                                                      std::uint64_t row_number,
                                                      Snapshot snapshot) {
    const auto& amc = bioseq::storage::get_table_access_cache(relation);
    if (amc.sequence_attribute_numbers.empty())
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
            for (const std::int16_t attno : amc.sequence_attribute_numbers) {
                const int index = attno - 1;
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

/** Mask one row after invalidating its exact sequence-source epochs. */
TM_Result bioseq_tuple_delete(Relation relation,
                              ItemPointer tid,
                              CommandId cid,
                              Snapshot snapshot,
                              Snapshot crosscheck,
                              bool wait,
                              TM_FailureData* tmfd,
                              bool changing_part) {
    if (bioseq::storage::is_registered_sequence_fragment_relation(RelationGetRelid(relation))) {
        bioseq_log<logging_level::error, logging_target::postgres>(
            "bioseq storage: registered sequence fragments are immutable; DELETE is forbidden");
    }
    uint64_t row_number = tid_to_row_number(*tid);
    uint64_t storage_id = lookup_storage_id(relation->rd_locator);
    /* Set lock for relation until transaction ends */
    DirectFunctionCall1(pg_advisory_xact_lock_int8,
                        Int64GetDatum(static_cast<int64_t>(storage_id)));

    invalidate_sequence_source_epochs_for_row(relation, row_number, snapshot);
    if (!update_row_mask(relation->rd_locator, storage_id, snapshot, row_number))
        return TM_Deleted;

    pgstat_count_heap_delete(relation);

    return TM_Ok;
}

/** Mask the old row and append the replacement row with new source identities. */
TM_Result bioseq_tuple_update(Relation relation,
                              ItemPointer otid,
                              TupleTableSlot* slot,
                              CommandId cid,
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

    /* Set lock for relation until transaction ends */
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

/** Fetch a virtual row identity for PostgreSQL's tuple-lock callback. */
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
                                        CurrentMemoryContext, // to be checked
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
void bioseq_finish_bulk_insert(Relation relation, int options) {
    /*
     * Nothing to do here. We keep write states live until transaction end.
     */
}
