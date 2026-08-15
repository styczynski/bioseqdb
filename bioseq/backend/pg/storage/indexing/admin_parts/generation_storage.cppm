/**
 * @file generation_storage.cppm
 * @brief PostgreSQL generation storage residency and transaction fixtures.
 */
module;
#include "access/heapam.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "commands/tablespace.h"
#include "executor/tuptable.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

#include <bioseq/pg_compat.h>
#include <libassert/assert.hpp>
extern "C" {
#include "storage/buffile.h"
}
#include <algorithm>
#include <array>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <expected>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
export module bioseq.postgres.storage.indexing.admin:generation_storage;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.postgres.support;
import bioseq.postgres.sql_boundary;
import bioseq.postgres.configuration.aligner;
import bioseq.postgres.configuration.core;
import bioseq.postgres.storage.manager; // seq_caches, sequence_locator, is_locator_stub, decode_locator_from_stub
import bioseq.storage.layout;
import bioseq.storage.size;
import bioseq.storage.lazy_sequence;           // make_resident_lazy_sequence
import bioseq.storage.scanner;                 // sequence access requirements
import bioseq.postgres.storage.scanner;        // with_postgres_lazy_sequence
import bioseq.postgres.types.compact_sequence; // bioseq::pg_types::seq, dispatch_instance
import bioseq.sequences;                       // compact_sequence_range
import bioseq.common.seqan_adapter;            // seqan3::char_is_valid_for
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.registry;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.hash_major_minimizer_index;
import bioseq.indexing.minimizer_sequence;
import bioseq.pipeline.runtime_contract; // minimizer_algorithm
import bioseq.sequences.kmers;           // minimizer/index APIs
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.highway.dispatch_contract;
import bioseq.indexing.symbol_counts;
import bioseq.postgres.storage.indexing.chunked_storage;
import bioseq.postgres.storage.indexing.residency;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.mode;
import bioseq.parallelism.pool;
import bioseq.postgres.scheduler.supervisor;
import bioseq.postgres.scheduler.supervisor.vulkan;
import bioseq.postgres.table.io;
import bioseq.postgres.storage.indexing.handles;
import bioseq.postgres.column.table_access;
import bioseq.postgres.storage.metadata; // column_table_row_count
static_assert(__cplusplus >= 202302L, "C++23-or-later");
import :policy_catalog;
import :hash_major;
import :grouped_soa;
import :rebuild;
import :inspection_policy;

export namespace bioseq::storage::indexing {

/// Integration-only observations for the generation-storage lifecycle. Optional
/// fields are emitted as SQL NULL by the fmgr wrapper when an operation does not
/// exercise that part of the contract.
struct minimizer_generation_storage_state_fixture_result {
    Oid before_oid = InvalidOid;
    Oid operation_oid = InvalidOid;
    Oid after_oid = InvalidOid;
    std::optional<bool> same_handle_rebound;
    std::optional<bool> generation_schema_after;
    std::optional<bool> chunk_cache_hit;
    std::optional<bool> assembled_cache_hit;
};
namespace minimizer_generation_storage_test_detail {

/// Chunk and assembled-cache keys addressing one fixture generation.
struct cache_keys {
    index_chunk_key chunk{};
    assembled_index_key assembled{};
};

/// Derive fixture cache keys from one physical source identity.
[[nodiscard]] inline cache_keys make_cache_keys(Oid user_relid,
                                                const relation_identity& relation_identity,
                                                std::int16_t column_attno,
                                                std::string_view index_name,
                                                const minimizer_generation_source_locator& source,
                                                std::uint64_t sequence_rowno) {
    const std::uint64_t name_hash = index_name_hash(relation_identity.db_oid,
                                                    relation_identity.tablespace_oid,
                                                    relation_identity.relfilenode,
                                                    column_attno,
                                                    index_name);
    return cache_keys{
        .chunk =
            index_chunk_key{
                .db_oid = relation_identity.db_oid,
                .user_relid = user_relid,
                .tablespace_oid = relation_identity.tablespace_oid,
                .main_relfilenode = relation_identity.relfilenode,
                .column_attno = column_attno,
                .index_name_hash = name_hash,
                .bucket_idx = static_cast<std::uint8_t>(source.locator.bucket_index),
                .shard_idx = source.locator.shard_index,
                .query_key = sequence_rowno,
                .sub_key = 0,
                .chunk_no = 0,
            },
        .assembled =
            assembled_index_key{
                .db_oid = relation_identity.db_oid,
                .user_relid = user_relid,
                .tablespace_oid = relation_identity.tablespace_oid,
                .main_relfilenode = relation_identity.relfilenode,
                .column_attno = column_attno,
                .index_name_hash = name_hash,
                .sketch_backend = bioseq::minimizers::minimizer_sketch_backend::scalar,
                .minimizer_scheme = bioseq::indexing::k_minimap_scheme_identity,
                .query_key = sequence_rowno,
                .query_signature = 0x7265736964656e74ULL,
                .clip_lo_bp = 0,
                .clip_hi_bp = source.locator.logical_length,
            },
    };
}

/// Check whether a relation exposes the canonical generation schema.
[[nodiscard]] inline bool relation_has_generation_schema(Oid relation_oid) {
    if (!OidIsValid(relation_oid))
        return false;
    Relation relation = bioseq::postgres::support::invoke_pg_as_cpp([relation_oid] {
        return bioseq::postgres::support::try_open_postgres_relation(relation_oid, AccessShareLock);
    });
    if (relation == nullptr)
        return false;
    const minimizer_generation_schema_inspection schema =
        bioseq::postgres::support::invoke_pg_as_cpp(
            [relation] { return inspect_minimizer_generation_schema(relation); });
    bioseq::postgres::support::invoke_pg_as_cpp(
        [relation] { relation_close(relation, AccessShareLock); });
    return static_cast<bool>(schema);
}

} // namespace minimizer_generation_storage_test_detail

/// Integration seam for storage invariants that SQL alone cannot validate:
/// explicit clear/create rollback must rebind C++ handles, committed
/// clear/create must evict both residency forms in another backend, normal
/// ensure must fail closed on noncanonical relations, and generation clear
/// must invalidate its initiating OID handle.
[[nodiscard]] inline minimizer_generation_storage_state_fixture_result
exercise_minimizer_generation_storage_state_impl(Oid rel_oid,
                                                 std::string_view index_name,
                                                 std::uint64_t sequence_rowno,
                                                 std::string_view operation) {
    if (!OidIsValid(rel_oid) || !bioseq::postgres::support::invoke_pg_as_cpp([rel_oid] {
            return relation_oid_uses_column_table_access_method(rel_oid);
        })) {
        ready_minimizer_error_as_cpp(
            ERRCODE_WRONG_OBJECT_TYPE,
            "bioseq generation-storage fixture requires a USING bioseq table");
    }
    if (!bioseq::postgres::support::invoke_pg_as_cpp([] { return superuser(); })) {
        ready_minimizer_error_as_cpp(ERRCODE_INSUFFICIENT_PRIVILEGE,
                                     "bioseq generation-storage fixture requires superuser");
    }
    if (index_name != k_minimizers_per_fragment_index_name &&
        index_name != k_minimizers_per_sequence_index_name) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "bioseq generation-storage fixture requires a minimizer index");
    }
    if (!bioseq::indexing::sql_bigint_can_represent(sequence_rowno)) {
        ready_minimizer_error_as_cpp(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
                                     "bioseq generation-storage fixture query key exceeds BIGINT");
    }
    const bool savepoint_rollback = operation == "savepoint_rollback";
    const bool warm_noncanonical_residency = operation == "warm_noncanonical_residency";
    const bool warm_generation_residency = operation == "warm_generation_residency";
    const bool ensure_generation = operation == "ensure_generation";
    const bool ensure_without_clear = operation == "ensure_generation_without_clear";
    const bool probe_residency = operation == "probe_residency";
    const bool clear_generation = operation == "clear_generation";
    const bool clear_generation_absent = operation == "clear_generation_absent";
    const bool clear_generation_rollback = operation == "clear_generation_savepoint_rollback";
    if (!savepoint_rollback && !warm_noncanonical_residency && !warm_generation_residency &&
        !ensure_generation && !ensure_without_clear && !probe_residency && !clear_generation &&
        !clear_generation_absent && !clear_generation_rollback) {
        ready_minimizer_error_as_cpp(ERRCODE_INVALID_PARAMETER_VALUE,
                                     "bioseq generation-storage fixture operation is invalid",
                                     fmt::format("operation={}", operation));
    }

    // The next command normally drains sinval first.  Calling this explicitly
    // makes the two-backend cache validation independent of scheduler timing.
    if (probe_residency) {
        bioseq::postgres::support::invoke_pg_as_cpp([] { AcceptInvalidationMessages(); });
    }

    const std::int16_t seq_attno = single_seq_attno_for_inspection(rel_oid);
    const relation_identity relation_identity = index_relation_identity(rel_oid);
    if (!OidIsValid(relation_identity.tablespace_oid) ||
        !OidIsValid(relation_identity.relfilenode)) {
        ready_minimizer_error_as_cpp(
            ERRCODE_UNDEFINED_TABLE,
            "bioseq generation-storage fixture source relation is unavailable");
    }
    Snapshot active_snapshot =
        bioseq::postgres::support::invoke_pg_as_cpp([] { return GetActiveSnapshot(); });
    if (active_snapshot == nullptr || !IsMVCCSnapshot(active_snapshot)) {
        ready_minimizer_error_as_cpp(
            ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
            "bioseq generation-storage fixture requires an active MVCC snapshot");
    }

    struct fixture_state {
        Snapshot registered_snapshot = nullptr;
        std::optional<minimizer_generation_source_authorization> source_authorization;
        minimizer_generation_source_locator source{};
        minimizer_generation_administration_identity identity{};
        std::optional<fragment_index_storage> storage;
        std::string table_name;
    };
    auto* state = new fixture_state{};
    state->identity = minimizer_generation_administration_identity{
        .database_oid = relation_identity.db_oid,
        .tablespace_oid = relation_identity.tablespace_oid,
        .main_relfilenode = relation_identity.relfilenode,
        .user_relid = rel_oid,
        .column_attno = seq_attno,
        .index_name = std::string{index_name},
    };

    minimizer_generation_storage_state_fixture_result result{};
    MemoryContext saved_context = CurrentMemoryContext;
    try {
        state->registered_snapshot = bioseq::postgres::support::invoke_pg_as_cpp(
            [active_snapshot] { return RegisterSnapshot(active_snapshot); });
        state->source_authorization.emplace(authorize_minimizer_generation_source_pg(
            state->identity, sequence_rowno, state->registered_snapshot));
        state->source = state->source_authorization->source_for_diagnostics();
        state->source_authorization.reset();
        bioseq::postgres::support::invoke_pg_as_cpp(
            [state] { UnregisterSnapshot(state->registered_snapshot); });
        state->registered_snapshot = nullptr;

        state->storage.emplace(open_fragment_storage(rel_oid,
                                                     relation_identity.db_oid,
                                                     relation_identity.tablespace_oid,
                                                     relation_identity.relfilenode,
                                                     seq_attno,
                                                     state->identity.index_name,
                                                     state->source.locator,
                                                     sequence_rowno,
                                                     nullptr,
                                                     saved_context));
        fragment_index_storage& storage = *state->storage;
        state->table_name = storage.this_table_name();
        const Oid internal_namespace = bioseq_internal_ns();
        const auto resolve_table_oid = [state, internal_namespace] {
            return bioseq::postgres::support::invoke_pg_as_cpp([state, internal_namespace] {
                return get_relname_relid(state->table_name.c_str(), internal_namespace);
            });
        };
        const auto cache_keys =
            minimizer_generation_storage_test_detail::make_cache_keys(rel_oid,
                                                                      relation_identity,
                                                                      seq_attno,
                                                                      state->identity.index_name,
                                                                      state->source,
                                                                      sequence_rowno);

        const bool generation_input =
            warm_generation_residency || clear_generation || clear_generation_rollback;
        if (ensure_generation || ensure_without_clear) {
            result.before_oid = resolve_table_oid();
            if (!OidIsValid(result.before_oid)) {
                minimizer_generation_storage_error(
                    "generation-storage ensure fixture relation is missing");
            }
        } else if (generation_input) {
            result.before_oid = resolve_table_oid();
            if (!OidIsValid(result.before_oid) ||
                !minimizer_generation_storage_test_detail::relation_has_generation_schema(
                    result.before_oid)) {
                minimizer_generation_storage_error(
                    "generation-storage fixture requires a canonical generation relation");
            }
            const Oid cached_oid = storage.resolve_relid_(false);
            if (cached_oid != result.before_oid || storage.index_relid_ != result.before_oid) {
                minimizer_generation_storage_error(
                    "generation-storage fixture could not cache the generation OID");
            }
        } else if (!probe_residency && !clear_generation_absent) {
            Relation noncanonical_relation = bioseq::postgres::support::invoke_pg_as_cpp(
                [&storage] { return storage.open_index_relation_(false, AccessShareLock); });
            if (noncanonical_relation == nullptr) {
                minimizer_generation_storage_error(
                    "generation-storage fixture noncanonical relation is missing");
            }
            result.before_oid = RelationGetRelid(noncanonical_relation);
            bioseq::postgres::support::invoke_pg_as_cpp([noncanonical_relation] {
                relation_close(noncanonical_relation, AccessShareLock);
            });
        } else {
            result.before_oid = resolve_table_oid();
        }

        if (warm_noncanonical_residency || warm_generation_residency) {
            constexpr std::array<char, 8> k_chunk_sentinel{'r', 'e', 's', 'i', 'd', 'e', 'n', 't'};
            const auto stored_chunk = store_cached_chunk(
                cache_keys.chunk,
                std::span<const char>{k_chunk_sentinel.data(), k_chunk_sentinel.size()},
                /*written_in_current_xact=*/false);
            minimizer_hashmap_fragment_index assembled_sentinel{};
            assembled_sentinel.target_len =
                std::max<std::uint64_t>(state->source.locator.logical_length, 1u);
            store_assembled_index(cache_keys.assembled, std::move(assembled_sentinel));
            const bool chunk_hit =
                stored_chunk.has_value() && lookup_cached_chunk(cache_keys.chunk).has_value();
            const bool assembled_hit = lookup_assembled_index(cache_keys.assembled).has_value();
            if (!chunk_hit || !assembled_hit) {
                minimizer_generation_storage_error(
                    "generation-storage fixture could not warm both residency forms");
            }
            result.after_oid = result.before_oid;
            result.generation_schema_after = warm_generation_residency;
            result.chunk_cache_hit = chunk_hit;
            result.assembled_cache_hit = assembled_hit;
        } else if (ensure_generation) {
            if (!minimizer_generation_storage_test_detail::relation_has_generation_schema(
                    result.before_oid)) {
                // Normalize the fixture's noncanonical relation before the
                // generation ensure. The production generation clear deliberately
                // ignores noncanonical same-name relations, so use the
                // existing broad index reset seam before exercising normal
                // non-destructive generation ensure.
                storage.clear(state->identity.index_name);
                if (OidIsValid(resolve_table_oid())) {
                    minimizer_generation_storage_error(
                        "generation-storage fixture noncanonical clear failed");
                }
            }
            result.operation_oid = storage.ensure_minimizer_generation_storage();
            result.after_oid = resolve_table_oid();
            const bool exact =
                result.after_oid == result.operation_oid &&
                minimizer_generation_storage_test_detail::relation_has_generation_schema(
                    result.after_oid);
            if (!exact) {
                minimizer_generation_storage_error(
                    "generation-storage fixture ensure did not publish the exact schema");
            }
            result.generation_schema_after = true;
        } else if (ensure_without_clear) {
            (void)storage.ensure_minimizer_generation_storage();
            minimizer_generation_storage_error(
                "generation-storage fixture unexpectedly accepted a noncanonical relation");
        } else if (probe_residency) {
            const bool chunk_hit = lookup_cached_chunk(cache_keys.chunk).has_value();
            const bool assembled_hit = lookup_assembled_index(cache_keys.assembled).has_value();
            result.after_oid = resolve_table_oid();
            result.generation_schema_after =
                minimizer_generation_storage_test_detail::relation_has_generation_schema(
                    result.after_oid);
            result.chunk_cache_hit = chunk_hit;
            result.assembled_cache_hit = assembled_hit;
        } else if (clear_generation || clear_generation_absent) {
            const minimizer_generation_clear_result cleared =
                storage.clear_minimizer_generation_storage();
            const bool expected_absent = clear_generation_absent;
            if ((expected_absent &&
                 (cleared.dropped_relation_count != 0 || cleared.deleted_metadata_age_rows != 0)) ||
                (!expected_absent &&
                 (cleared.dropped_relation_count == 0 || cleared.deleted_metadata_age_rows != 1))) {
                minimizer_generation_storage_error(
                    fmt::format("generation clear fixture observed unexpected counts "
                                "(relations={}, metadata_age={}, expected_absent={})",
                                cleared.dropped_relation_count,
                                cleared.deleted_metadata_age_rows,
                                expected_absent));
            }
            result.operation_oid = result.before_oid;
            result.after_oid = resolve_table_oid();
            const bool chunk_hit = lookup_cached_chunk(cache_keys.chunk).has_value();
            const bool assembled_hit = lookup_assembled_index(cache_keys.assembled).has_value();
            if (OidIsValid(result.after_oid) || OidIsValid(storage.index_relid_) || chunk_hit ||
                assembled_hit) {
                minimizer_generation_storage_error(
                    "generation clear fixture retained physical or logical residency");
            }
            result.generation_schema_after = false;
            result.chunk_cache_hit = false;
            result.assembled_cache_hit = false;
        } else if (clear_generation_rollback) {
            result.operation_oid = result.before_oid;
            {
                auto subtransaction = bioseq::postgres::support::subtransaction::begin();
                const minimizer_generation_clear_result cleared =
                    storage.clear_minimizer_generation_storage();
                const bool chunk_hit = lookup_cached_chunk(cache_keys.chunk).has_value();
                const bool assembled_hit = lookup_assembled_index(cache_keys.assembled).has_value();
                if (cleared.dropped_relation_count != 1 || cleared.deleted_metadata_age_rows != 1 ||
                    OidIsValid(resolve_table_oid()) || OidIsValid(storage.index_relid_) ||
                    chunk_hit || assembled_hit) {
                    minimizer_generation_storage_error(
                        "generation clear rollback fixture did not clear inside its savepoint");
                }
                subtransaction.rollback();
            }

            bioseq::postgres::support::invoke_pg_as_cpp([] { CommandCounterIncrement(); });
            result.after_oid = storage.resolve_relid_(false);
            Relation restored =
                OidIsValid(result.after_oid)
                    ? bioseq::postgres::support::invoke_pg_as_cpp([&result] {
                          return bioseq::postgres::support::try_open_postgres_relation(
                              result.after_oid, AccessShareLock);
                      })
                    : nullptr;
            const bool restored_schema =
                restored != nullptr && RelationGetRelid(restored) == result.before_oid &&
                minimizer_generation_storage_test_detail::relation_has_generation_schema(
                    result.after_oid);
            if (restored != nullptr) {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [restored] { relation_close(restored, AccessShareLock); });
            }
            const bool chunk_hit = lookup_cached_chunk(cache_keys.chunk).has_value();
            const bool assembled_hit = lookup_assembled_index(cache_keys.assembled).has_value();
            if (!restored_schema || storage.index_relid_ != result.before_oid || chunk_hit ||
                assembled_hit) {
                minimizer_generation_storage_error(
                    "generation clear rollback fixture did not restore and rebind safely");
            }
            result.same_handle_rebound = true;
            result.generation_schema_after = true;
            result.chunk_cache_hit = false;
            result.assembled_cache_hit = false;
        } else {
            {
                auto subtransaction = bioseq::postgres::support::subtransaction::begin();
                // Savepoint rollback starts from a noncanonical relation and
                // must restore that exact object. The generation-only clear
                // cannot remove this input.
                storage.clear(state->identity.index_name);
                if (OidIsValid(resolve_table_oid())) {
                    minimizer_generation_storage_error(
                        "generation-storage rollback fixture explicit clear failed");
                }
                result.operation_oid = storage.ensure_minimizer_generation_storage();
                // Deliberately seed speculative member state before rollback.
                // The relation resolver records only the authoritative name/OID;
                // it does not apply column attributes here.
                const Oid deliberately_cached = storage.resolve_relid_(false);
                if (deliberately_cached != result.operation_oid) {
                    minimizer_generation_storage_error(
                        "generation-storage fixture could not cache the speculative OID");
                }
                subtransaction.rollback();
            }

            bioseq::postgres::support::invoke_pg_as_cpp([] { CommandCounterIncrement(); });
            result.after_oid = storage.resolve_relid_(false);
            Relation restored = bioseq::postgres::support::invoke_pg_as_cpp(
                [&storage] { return storage.open_index_relation_(false, AccessShareLock); });
            const Oid reopened_oid = restored == nullptr ? InvalidOid : RelationGetRelid(restored);
            if (restored != nullptr) {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [restored] { relation_close(restored, AccessShareLock); });
            }
            const Oid authoritative_oid = resolve_table_oid();
            const bool rebound =
                OidIsValid(result.before_oid) && OidIsValid(result.operation_oid) &&
                result.operation_oid != result.before_oid &&
                result.after_oid == result.before_oid && reopened_oid == result.before_oid &&
                authoritative_oid == result.before_oid && storage.index_relid_ == result.before_oid;
            if (!rebound) {
                minimizer_generation_storage_error(
                    "generation-storage fixture retained a stale speculative OID");
            }
            result.same_handle_rebound = true;
            result.generation_schema_after = false;
        }

        state->storage->close_checked();
        state->storage.reset();
        delete state;
        state = nullptr;
    } catch (...) {
        std::exception_ptr error = std::current_exception();
        if (state != nullptr) {
            state->source_authorization.reset();
            if (state->registered_snapshot != nullptr) {
                Snapshot cleanup = std::exchange(state->registered_snapshot, nullptr);
                try {
                    bioseq::postgres::support::invoke_pg_as_cpp(
                        [cleanup] { UnregisterSnapshot(cleanup); });
                } catch (...) {
                    error = std::current_exception();
                }
            }
            if (state->storage.has_value()) {
                state->storage->abandon_after_pg_error_noexcept();
                state->storage.reset();
            }
            delete state;
        }
        std::rethrow_exception(error);
    }
    return result;
}

} // namespace bioseq::storage::indexing
