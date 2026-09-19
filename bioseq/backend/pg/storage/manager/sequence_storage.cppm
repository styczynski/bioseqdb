/**
 * @file sequence_storage.cppm
 * @brief Stable facade for PostgreSQL sequence storage partitions.
 *
 * Re-exports catalog, layout, DDL, write and read contracts while retaining
 * the authored orphan-reaper and vacuum SQL entry points.
 */
module;

#include <bioseq/pg_compat.h>

extern "C" {
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/genam.h"
#include "access/xact.h"
#include "executor/spi.h"
#include "storage/lmgr.h"
#include "utils/snapmgr.h"
#include "utils/resowner.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "catalog/dependency.h"
#include "catalog/namespace.h"
#include "catalog/indexing.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_opclass_d.h"
#include "catalog/pg_sequence.h"
#include "catalog/pg_type_d.h"
#include "commands/extension.h"
#include "commands/defrem.h"
#include "commands/sequence.h"
#include "commands/vacuum.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "fmgr.h"
#include "nodes/makefuncs.h"
#include "nodes/parsenodes.h"
#include "nodes/primnodes.h"
#include "parser/analyze.h"
#include "parser/parser.h"
#include "rewrite/rewriteHandler.h"
#include "utils/tuplestore.h"
}

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.postgres.storage.manager:sequence_storage;

import bioseq.common.logging;
import bioseq.postgres.support;
import bioseq.postgres.sql_boundary;
import bioseq.postgres.configuration.core;
import bioseq.postgres.configuration.storage;
import bioseq.postgres.storage.metadata;
import bioseq.postgres.storage.physical;
import bioseq.postgres.table.io;
import bioseq.postgres.types.compact_sequence;
import bioseq.sequences;
import bioseq.sequences.simd;
import bioseq.indexing.storage;
import bioseq.common.seqan_adapter;  // ceil_log2 used in letters_per_unit_for
import bioseq.storage.size;
import bioseq.storage.coordinates;
import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.storage.manager.sql_rows;
import bioseq.postgres.storage.manager.sql_row_codecs;
import bioseq.postgres.sql.generated.core.storage_manager_sequence_storage_manager.entity_handles;
// The PG-free bucket math and sequence_locator value live in their own
// module so the unit-test TU can link them without the Postgres backend.
// Re-exported here so existing `import bioseq_storage` consumers are unchanged.
export import bioseq.storage.layout;

export import :sequence_storage_catalog;
export import :sequence_storage_layout_cache;
export import :sequence_storage_catalog_ddl;
export import :sequence_storage_streamed_writes;
export import :sequence_storage_reads_sidecars;

static_assert(__cplusplus >= 202302L,
              "sequence storage requires C++23-or-later (project default: C++26)");

namespace bioseq::storage {

// Orphan relation maintenance.
// ---------------------------------------------------------------------------

/**
 * @brief Sweep stale shard rows and unreferenced fragment or sidecar relations.
 * @return One event per deleted catalog row, dropped relation, or recoverable
 * relation-drop failure.
 * @throws A translated PostgreSQL error or `std::runtime_error` when catalog
 * discovery, locking, SPI, or an unrecoverable drop fails.
 *
 * Catalog-row cleanup holds `ShareRowExclusiveLock` on the registry. Physical
 * candidates are revalidated while holding `AccessExclusiveLock` on the
 * candidate and `ShareRowExclusiveLock` on the registry. Each physical drop
 * runs in a subtransaction so a recoverable PostgreSQL error can be reported.
 */
export [[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::volatile_,
    .parallel = bioseq::sql::parallel_safety::unsafe,
})]]
[[bioseq_sql::grant({
    .privileges = bioseq::sql::privileges::execute,
    .to = bioseq::sql::public_role,
})]]
[[nodiscard]] bioseq::postgres::support::table_rows<reaper_event>
reap_orphan_seq_sub_tables() {
    bioseq::postgres::support::spi_session spi;
    std::vector<reaper_event> events;
#if defined(BIOSEQ_SAFETY_RUNTIME_PROBES)
    bioseq::postgres::support::safety_runtime_probe::destructor_sentinel
        reaper_call_sentinel{"orphan_reaper_call"};
#endif
    // Append one owning result row while keeping fault injection at this boundary.
    const auto append_event =
        [&events](reaper_event event) {
#if defined(BIOSEQ_SAFETY_RUNTIME_PROBES)
            if (bioseq::postgres::support::safety_runtime_probe::consume_fault(
                    "orphan_reaper.event_allocation")) {
                bioseq::postgres::support::safety_runtime_probe::record_site_visit(
                    "orphan_reaper.event_allocation");
                throw std::bad_alloc{};
            }
#endif
            events.push_back(std::move(event));
        };
    const int connect_status = spi.connect();
    if (connect_status != SPI_OK_CONNECT) {
        throw std::runtime_error{
            "could not connect to SPI for the orphan relation reaper"};
    }

    /*
     * Serialize Direction-A discovery/deletion against seq_sub_table writers.
     * Direction B reacquires this self-conflicting mode per physical candidate
     * in the production writer's physical-then-catalog lock order.
     */
    const Oid internal_namespace_oid =
        bioseq::postgres::support::invoke_pg_as_cpp([] {
            return get_namespace_oid("bioseq_internal", false);
        });
    const Oid seq_sub_table_oid =
        bioseq::postgres::support::invoke_pg_as_cpp(
            [internal_namespace_oid] {
                return get_relname_relid(
                    "seq_sub_table", internal_namespace_oid);
            });
    if (!OidIsValid(seq_sub_table_oid)) {
        throw std::runtime_error{
            "bioseq_internal.seq_sub_table is missing"};
    }
    bioseq::postgres::support::invoke_pg_as_cpp(
        [seq_sub_table_oid] {
            LockRelationOid(
                seq_sub_table_oid, ShareRowExclusiveLock);
        });

    // Convert unexpected SPI statuses into the function's C++ error path.
    const auto require_status = [](
            int actual,
            int expected,
            const char* operation) {
        if (actual == expected)
            return;
        throw std::runtime_error{
            std::string{"SPI "} + operation
            + " returned status " + std::to_string(actual)
            + ", expected " + std::to_string(expected)};
    };

    const char* find_orphan_rows =
        "SELECT s.database_oid::text || '/' || "
        "       s.main_relfilenode::text || '/' || "
        "       s.column_attno::text || '/' || "
        "       s.bucket_idx::text || '/' || "
        "       s.shard_idx::text "
        "FROM bioseq_internal.seq_sub_table s "
        "WHERE s.database_oid = $1 "
        "  AND NOT EXISTS (SELECT 1 FROM pg_class c "
        "                  WHERE c.oid = s.sub_table_relid);";
    std::array<Oid, 1> argument_types{OIDOID};
    std::array<Datum, 1> argument_values{
        ObjectIdGetDatum(MyDatabaseId)};
    const int find_rows_status = bioseq::postgres::support::invoke_pg_as_cpp(
        [find_orphan_rows, &argument_types, &argument_values] {
#if defined(BIOSEQ_SAFETY_RUNTIME_PROBES)
            if (bioseq::postgres::support::safety_runtime_probe::consume_fault(
                    "orphan_reaper.direction_a_select")) {
                bioseq::postgres::support::safety_runtime_probe::record_site_visit(
                    "orphan_reaper.direction_a_select");
                ereport(
                    ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg_internal(
                         "bioseq safety runtime injected reaper "
                         "Direction-A SELECT error")));
            }
#endif
            return SPI_execute_with_args(
                find_orphan_rows,
                static_cast<int>(argument_types.size()),
                argument_types.data(),
                argument_values.data(),
                /*Nulls=*/nullptr, /*read_only=*/false, /*tcount=*/0);
        });
    require_status(
        find_rows_status,
        SPI_OK_SELECT,
        "orphan catalog SELECT");

    SPITupleTable* const orphan_rows = SPI_tuptable;
    const std::uint64_t orphan_row_count = SPI_processed;
    for (std::uint64_t row = 0; row < orphan_row_count; ++row) {
        char* key = bioseq::postgres::support::invoke_pg_as_cpp(
            [orphan_rows, row] {
#if defined(BIOSEQ_SAFETY_RUNTIME_PROBES)
                if (bioseq::postgres::support::safety_runtime_probe::consume_fault(
                        "orphan_reaper.direction_a_tuple_copy")) {
                    bioseq::postgres::support::safety_runtime_probe::
                        record_site_visit(
                            "orphan_reaper.direction_a_tuple_copy");
                    ereport(
                        ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg_internal(
                             "bioseq safety runtime injected reaper "
                             "Direction-A tuple-copy error")));
                }
#endif
                return SPI_getvalue(
                    orphan_rows->vals[row],
                    orphan_rows->tupdesc,
                    1);
            });
        if (key != nullptr) {
            append_event({
                .action = "delete-catalog-row",
                .target = key,
            });
            bioseq::postgres::support::invoke_pg_as_cpp(
                [key] { pfree(key); });
        }
    }
    if (orphan_rows != nullptr) {
        bioseq::postgres::support::invoke_pg_as_cpp(
            [orphan_rows] { SPI_freetuptable(orphan_rows); });
    }

    const char* del_orphan_rows =
        "DELETE FROM bioseq_internal.seq_sub_table s "
        "WHERE s.database_oid = $1 "
        "  AND NOT EXISTS (SELECT 1 FROM pg_class c "
        "                  WHERE c.oid = s.sub_table_relid);";
    const int delete_rows_status = bioseq::postgres::support::invoke_pg_as_cpp(
        [del_orphan_rows, &argument_types, &argument_values] {
#if defined(BIOSEQ_SAFETY_RUNTIME_PROBES)
            if (bioseq::postgres::support::safety_runtime_probe::consume_fault(
                    "orphan_reaper.direction_a_delete")) {
                bioseq::postgres::support::safety_runtime_probe::record_site_visit(
                    "orphan_reaper.direction_a_delete");
                ereport(
                    ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg_internal(
                         "bioseq safety runtime injected reaper "
                         "Direction-A DELETE error")));
            }
#endif
            return SPI_execute_with_args(
                del_orphan_rows,
                static_cast<int>(argument_types.size()),
                argument_types.data(),
                argument_values.data(),
                /*Nulls=*/nullptr,
                /*read_only=*/false, /*tcount=*/0);
        });
    require_status(
        delete_rows_status,
        SPI_OK_DELETE,
        "orphan catalog DELETE");
    bioseq::postgres::support::invoke_pg_as_cpp(
        [seq_sub_table_oid] {
            UnlockRelationOid(
                seq_sub_table_oid, ShareRowExclusiveLock);
        });

    const char* find_orphan_relations =
        "SELECT c.oid, c.relname, "
        "       n.nspname || '.' || quote_ident(c.relname) "
        "FROM pg_class c JOIN pg_namespace n ON c.relnamespace = n.oid "
        "WHERE n.nspname = 'bioseq_internal' "
        "  AND (c.relname LIKE 'seq\\_sub\\_%' ESCAPE '\\' "
        "       OR c.relname LIKE 'idx\\_sidecar\\_%' ESCAPE '\\') "
        "  AND c.relkind <> 'i' "
        "  AND c.oid <> 'bioseq_internal.seq_sub_table'::regclass "
        "  AND NOT EXISTS ("
        "      SELECT 1 FROM bioseq_internal.seq_sub_table s "
        "      WHERE s.sub_table_relid = c.oid OR s.sidecar_relid = c.oid) "
        "ORDER BY c.oid;";
    /// Catalog snapshot of one physical relation eligible for revalidation.
    struct orphan_relation_candidate final {
        Oid relation_oid = InvalidOid;
        std::string leaf_name;
        std::string qualified_name;
    };
    std::vector<orphan_relation_candidate> to_drop;
    const int find_relations_status = bioseq::postgres::support::invoke_pg_as_cpp(
        [find_orphan_relations] {
#if defined(BIOSEQ_SAFETY_RUNTIME_PROBES)
            if (bioseq::postgres::support::safety_runtime_probe::consume_fault(
                    "orphan_reaper.direction_b_select")) {
                bioseq::postgres::support::safety_runtime_probe::record_site_visit(
                    "orphan_reaper.direction_b_select");
                ereport(
                    ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg_internal(
                         "bioseq safety runtime injected reaper "
                         "Direction-B SELECT error")));
            }
#endif
            return SPI_execute(
                find_orphan_relations,
                /*read_only=*/true,
                /*tcount=*/0);
        });
    require_status(
        find_relations_status,
        SPI_OK_SELECT,
        "orphan relation SELECT");

    SPITupleTable* const orphan_relations = SPI_tuptable;
    const std::uint64_t orphan_relation_count = SPI_processed;
    to_drop.reserve(orphan_relation_count);
    for (std::uint64_t row = 0;
         row < orphan_relation_count;
         ++row) {
        const auto relation_oid = bioseq::postgres::support::invoke_pg_as_cpp(
            [orphan_relations, row] {
#if defined(BIOSEQ_SAFETY_RUNTIME_PROBES)
                if (bioseq::postgres::support::safety_runtime_probe::consume_fault(
                        "orphan_reaper.direction_b_tuple_copy")) {
                    bioseq::postgres::support::safety_runtime_probe::
                        record_site_visit(
                            "orphan_reaper.direction_b_tuple_copy");
                    ereport(
                        ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg_internal(
                             "bioseq safety runtime injected reaper "
                             "Direction-B tuple-copy error")));
                }
#endif
                bool is_null = false;
                const Datum value = SPI_getbinval(
                    orphan_relations->vals[row],
                    orphan_relations->tupdesc,
                    1,
                    &is_null);
                return std::pair{
                    is_null
                        ? InvalidOid
                        : DatumGetObjectId(value),
                    is_null};
            });
        char* leaf_name = bioseq::postgres::support::invoke_pg_as_cpp(
            [orphan_relations, row] {
                return SPI_getvalue(
                    orphan_relations->vals[row],
                    orphan_relations->tupdesc,
                    2);
            });
        char* qualified_name = bioseq::postgres::support::invoke_pg_as_cpp(
            [orphan_relations, row] {
                return SPI_getvalue(
                    orphan_relations->vals[row],
                    orphan_relations->tupdesc,
                    3);
            });
        if (!relation_oid.second
            && OidIsValid(relation_oid.first)
            && leaf_name != nullptr
            && qualified_name != nullptr) {
            to_drop.push_back({
                .relation_oid = relation_oid.first,
                .leaf_name = leaf_name,
                .qualified_name = qualified_name,
            });
        }
        if (leaf_name != nullptr) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [leaf_name] { pfree(leaf_name); });
        }
        if (qualified_name != nullptr) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [qualified_name] { pfree(qualified_name); });
        }
    }
    if (orphan_relations != nullptr) {
        bioseq::postgres::support::invoke_pg_as_cpp(
            [orphan_relations] {
                SPI_freetuptable(orphan_relations);
            });
    }

    // Recheck the registry after both the physical and catalog locks are held.
    const auto relation_is_still_orphan =
        [&require_status](Oid relation_oid) {
            static constexpr char statement[] =
                "SELECT NOT EXISTS ("
                "  SELECT 1 "
                "    FROM bioseq_internal.seq_sub_table s "
                "   WHERE s.sub_table_relid = $1 "
                "      OR s.sidecar_relid = $1)";
            std::array<Oid, 1> types{OIDOID};
            std::array<Datum, 1> values{
                ObjectIdGetDatum(relation_oid)};
            const int status =
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [&types, &values] {
                        return SPI_execute_with_args(
                            statement,
                            static_cast<int>(types.size()),
                            types.data(),
                            values.data(),
                            /*Nulls=*/nullptr,
                            /*read_only=*/false,
                            /*tcount=*/1);
                    });
            require_status(
                status,
                SPI_OK_SELECT,
                "orphan relation revalidation SELECT");
            SPITupleTable* const table = SPI_tuptable;
            const bool valid_shape =
                table != nullptr && SPI_processed == 1;
            bool is_null = true;
            bool is_orphan = false;
            if (valid_shape) {
                is_orphan =
                    bioseq::postgres::support::invoke_pg_as_cpp(
                        [table, &is_null] {
                            const Datum value = SPI_getbinval(
                                table->vals[0],
                                table->tupdesc,
                                1,
                                &is_null);
                            return !is_null
                                && DatumGetBool(value);
                        });
            }
            if (table != nullptr) {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [table] { SPI_freetuptable(table); });
            }
            return valid_shape && !is_null && is_orphan;
        };

#if defined(BIOSEQ_SAFETY_RUNTIME_PROBES)
    bool catchable_drop_requires_continuation = false;
#endif
    for (const orphan_relation_candidate& candidate : to_drop) {
#if defined(BIOSEQ_SAFETY_RUNTIME_PROBES)
        if (catchable_drop_requires_continuation) {
            bioseq::postgres::support::safety_runtime_probe::record_cleanup_complete(
                "orphan_reaper.catchable_drop.continued");
            throw std::runtime_error{
                "bioseq safety runtime observed catchable reaper "
                "DROP continuation"};
        }
#endif
        const std::string drop_sql =
            "DROP TABLE IF EXISTS " + candidate.qualified_name;
#if defined(BIOSEQ_TEST_INDEXES)
        orphan_reaper_test_support::pass_drop_barrier();
#endif
        auto subtransaction =
            bioseq::postgres::support::subtransaction::begin();
#if defined(BIOSEQ_SAFETY_RUNTIME_PROBES)
        bool catchable_drop_probe_injected = false;
#endif
        try {
#if defined(BIOSEQ_SAFETY_RUNTIME_PROBES)
            bioseq::postgres::support::safety_runtime_probe::destructor_sentinel
                reaper_object_sentinel{"orphan_reaper_object"};
            if (bioseq::postgres::support::safety_runtime_probe::consume_fault(
                    "orphan_reaper.query_canceled")) {
                bioseq::postgres::support::safety_runtime_probe::record_site_visit(
                    "orphan_reaper.query_canceled");
                bioseq::postgres::support::invoke_pg_as_cpp([] {
                    ereport(
                        ERROR,
                        (errcode(ERRCODE_QUERY_CANCELED),
                         errmsg(
                             "bioseq safety runtime injected reaper "
                             "query cancellation")));
                });
            }
            if (bioseq::postgres::support::safety_runtime_probe::consume_fault(
                    "orphan_reaper.assertion_failure")) {
                bioseq::postgres::support::safety_runtime_probe::record_site_visit(
                    "orphan_reaper.assertion_failure");
                bioseq::postgres::support::invoke_pg_as_cpp([] {
                    ereport(
                        ERROR,
                        (errcode(ERRCODE_ASSERT_FAILURE),
                         errmsg(
                             "bioseq safety runtime injected reaper "
                             "assertion failure")));
                });
            }
            if (bioseq::postgres::support::safety_runtime_probe::consume_fault(
                    "orphan_reaper.std_exception")) {
                bioseq::postgres::support::safety_runtime_probe::record_site_visit(
                    "orphan_reaper.std_exception");
                throw std::runtime_error{
                    "bioseq safety runtime injected reaper "
                    "std::runtime_error"};
            }
            if (bioseq::postgres::support::safety_runtime_probe::consume_fault(
                    "orphan_reaper.unknown_exception")) {
                bioseq::postgres::support::safety_runtime_probe::record_site_visit(
                    "orphan_reaper.unknown_exception");
                throw std::uint32_t{388};
            }
#endif
            bioseq::postgres::support::invoke_pg_as_cpp(
                [relation_oid = candidate.relation_oid] {
                    LockRelationOid(
                        relation_oid, AccessExclusiveLock);
                });
            /*
             * Writers lock a physical shard before recording or updating its
             * catalog row.  Match that order, then hold the catalog lock
             * through revalidation and DROP.  Releasing this one explicit
             * lock before the next candidate avoids cross-candidate
             * physical/catalog lock inversion.
             */
            bioseq::postgres::support::invoke_pg_as_cpp(
                [seq_sub_table_oid] {
                    LockRelationOid(
                        seq_sub_table_oid,
                        ShareRowExclusiveLock);
                });
            const bool identity_is_current =
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [&candidate, internal_namespace_oid] {
                        HeapTuple tuple = SearchSysCache1(
                            RELOID,
                            ObjectIdGetDatum(
                                candidate.relation_oid));
                        if (!HeapTupleIsValid(tuple))
                            return false;
                        const Form_pg_class relation =
                            reinterpret_cast<Form_pg_class>(
                                GETSTRUCT(tuple));
                        const bool matches =
                            relation->relnamespace
                                == internal_namespace_oid
                            && std::strncmp(
                                   NameStr(relation->relname),
                                   candidate.leaf_name.c_str(),
                                   NAMEDATALEN) == 0;
                        ReleaseSysCache(tuple);
                        return matches;
                    });
            if (!identity_is_current
                || !relation_is_still_orphan(
                    candidate.relation_oid)) {
                subtransaction.rollback();
                continue;
            }
#if defined(BIOSEQ_SAFETY_RUNTIME_PROBES)
            catchable_drop_probe_injected =
                bioseq::postgres::support::safety_runtime_probe::consume_fault(
                    "orphan_reaper.direction_b_drop_catchable");
            if (catchable_drop_probe_injected) {
                bioseq::postgres::support::safety_runtime_probe::record_site_visit(
                    "orphan_reaper.direction_b_drop_catchable");
                bioseq::postgres::support::invoke_pg_as_cpp([] {
                    ereport(
                        ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg_internal(
                             "bioseq safety runtime injected reaper "
                             "Direction-B catchable DROP error")));
                });
            }
#endif
            const int drop_status =
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [statement = drop_sql.c_str()] {
                        return SPI_execute(
                            statement,
                            /*read_only=*/false,
                            /*tcount=*/0);
                    });
            require_status(
                drop_status,
                SPI_OK_UTILITY,
                "orphan relation DROP TABLE");
            bioseq::postgres::support::invoke_pg_as_cpp(
                [seq_sub_table_oid] {
                    UnlockRelationOid(
                        seq_sub_table_oid,
                        ShareRowExclusiveLock);
                });
        } catch (
                const bioseq::postgres::support::postgres_error& error) {
            if (error.get_sql_state_code() == ERRCODE_QUERY_CANCELED
                || error.get_sql_state_code() == ERRCODE_ASSERT_FAILURE) {
                subtransaction.rollback();
                throw;
            }
            std::string failure_action =
                "drop-relation-failed:"
                + std::string{error.what()};
            subtransaction.rollback();
#if defined(BIOSEQ_SAFETY_RUNTIME_PROBES)
            if (catchable_drop_probe_injected) {
                bioseq::postgres::support::safety_runtime_probe::
                    record_cleanup_complete(
                        "orphan_reaper.catchable_drop.rollback");
            }
#endif
            append_event({
                .action = std::move(failure_action),
                .target = candidate.qualified_name,
            });
#if defined(BIOSEQ_SAFETY_RUNTIME_PROBES)
            catchable_drop_requires_continuation =
                catchable_drop_probe_injected;
#endif
            continue;
        }

        subtransaction.commit();
        append_event({
            .action = "drop-relation",
            .target = candidate.qualified_name,
        });
    }
#if defined(BIOSEQ_SAFETY_RUNTIME_PROBES)
    if (catchable_drop_requires_continuation) {
        throw std::runtime_error{
            "bioseq safety runtime catchable reaper DROP fixture "
            "did not provide a continuation candidate"};
    }
#endif

    require_status(
        spi.finish(),
        SPI_OK_FINISH,
        "SPI_finish");
    return bioseq::postgres::support::table_rows<reaper_event>{
        std::move(events)};
}

namespace {

/// Sequence shard and sidecar relation pair selected for maintenance.
struct vacuum_target_pair {
    /// Fragment relation vacuumed before its co-sharded sidecar.
    Oid sub_relation = InvalidOid;
    /// Sidecar relation paired with `sub_relation`.
    Oid sidecar_relation = InvalidOid;
};

}  // namespace

/**
 * @brief Vacuum and analyze every fragment and sidecar shard owned by a table.
 * @param rel Parent relation OID. SQL NULL performs no maintenance.
 * @throws A translated PostgreSQL error or `std::runtime_error` when the call
 * is atomic, SPI state cannot be changed, or vacuum fails for a reason other
 * than a concurrently removed relation.
 *
 * The injected call context enforces non-atomic procedure execution and
 * manages transaction boundaries; it is not a SQL-visible parameter.
 *
 * The procedure commits before each relation pair because PostgreSQL VACUUM
 * cannot execute inside the caller's transaction. A concurrently dropped shard
 * is skipped after rolling back the maintenance transaction.
 */
export [[bioseq_sql::procedure({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::grant({
    .privileges = bioseq::sql::privileges::execute,
    .to = bioseq::sql::public_role,
})]]
void vacuum_seq_storage(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::regclass,
        })]]
        std::optional<Oid> rel,
        bioseq::postgres::support::function_call_context context) {
    bioseq::postgres::support::spi_session spi;
    std::vector<vacuum_target_pair> targets;
    if (rel.has_value()) {
        HeapTuple relation_tuple =
            bioseq::postgres::support::invoke_pg_as_cpp(
                [relation_id = *rel] {
                    return SearchSysCache1(
                        RELOID,
                        ObjectIdGetDatum(relation_id));
                });
        if (HeapTupleIsValid(relation_tuple)) {
            const auto* relation_form =
                reinterpret_cast<Form_pg_class>(
                    GETSTRUCT(relation_tuple));
            Oid tablespace_id = relation_form->reltablespace;
            if (!OidIsValid(tablespace_id)) {
                tablespace_id = MyDatabaseTableSpace;
            }
            const Oid relfilenode = relation_form->relfilenode;
            bioseq::postgres::support::invoke_pg_as_cpp(
                [relation_tuple] {
                    ReleaseSysCache(relation_tuple);
                });

            auto rows = sequence_sub_table_manager::list_shards_for_main(
                MyDatabaseId,
                tablespace_id,
                relfilenode);
            std::ranges::sort(
                rows,
                {},
                [](const auto& row) {
                    return std::tuple{
                        row.column_attno,
                        row.bucket_idx,
                        row.shard_idx};
                });
            targets.reserve(rows.size());
            for (const auto& row : rows) {
                targets.push_back({
                    .sub_relation = row.sub_table_relid,
                    .sidecar_relation = row.sidecar_relid,
                });
            }
        }
    }

    if (targets.empty()) {
        return;
    }

    context.require_nonatomic_call();
    const int connect_status = spi.connect(SPI_OPT_NONATOMIC);
    if (connect_status != SPI_OK_CONNECT) {
        throw std::runtime_error{
            "could not start a non-atomic SPI connection for shard vacuum"};
    }

    // Build and execute PostgreSQL's VACUUM ANALYZE utility statement by OID.
    const auto vacuum_analyze = [](Oid relation_id) {
        auto* statement = makeNode(VacuumStmt);
        statement->options =
            list_make1(
                makeDefElem(pstrdup("analyze"), nullptr, -1));
        statement->rels =
            list_make1(
                makeVacuumRelation(
                    nullptr,
                    relation_id,
                    NIL));
        statement->is_vacuumcmd = true;
        ExecVacuum(nullptr, statement, true);
    };

    for (const vacuum_target_pair& target : targets) {
        context.commit_transaction();
        const int pre_vacuum_finish_status = spi.finish();
        if (pre_vacuum_finish_status != SPI_OK_FINISH) {
            throw std::runtime_error{
                "could not suspend the non-atomic SPI connection "
                "before shard vacuum"};
        }
        try {
            bioseq::postgres::support::invoke_pg_across_transactions_as_cpp(
                [vacuum_analyze,
                 relation_id = target.sub_relation] {
                    vacuum_analyze(relation_id);
                });
            bioseq::postgres::support::invoke_pg_across_transactions_as_cpp(
                [vacuum_analyze,
                 relation_id = target.sidecar_relation] {
                    vacuum_analyze(relation_id);
                });
        } catch (bioseq::postgres::support::postgres_error& error) {
            if (error.get_sql_state_code() != ERRCODE_UNDEFINED_TABLE) {
                throw;
            }
            const int reconnect_status =
                spi.connect(SPI_OPT_NONATOMIC);
            if (reconnect_status != SPI_OK_CONNECT) {
                throw std::runtime_error{
                    "could not restore the non-atomic SPI connection "
                    "after shard vacuum failure"};
            }
            FreeErrorData(error.release_error_data());
            context.rollback_transaction();
            continue;
        }
        const int reconnect_status =
            spi.connect(SPI_OPT_NONATOMIC);
        if (reconnect_status != SPI_OK_CONNECT) {
            throw std::runtime_error{
                "could not restore the non-atomic SPI connection "
                "after shard vacuum"};
        }
    }

    const int finish_status = spi.finish();
    if (finish_status != SPI_OK_FINISH) {
        throw std::runtime_error{
            "could not finish the non-atomic SPI connection "
            "after shard vacuum"};
    }
}

}  // namespace bioseq::storage
