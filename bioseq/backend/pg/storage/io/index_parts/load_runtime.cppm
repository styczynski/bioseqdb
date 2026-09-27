/**
 * @file load_runtime.cppm
 * @brief Set up sequence loading, finish writes and release the load resources.
 */
module;
// Include Highway and PostgreSQL headers before the named module declaration.
// Imported sequence modules also depend on these declarations.
#include <bioseq/pg_compat.h>
#include <boost/container/small_vector.hpp>
#include <boost/integer.hpp>
#include <climits>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <hwy/highway.h>
#include <libassert/assert.hpp>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
extern "C" {
#include "postgres.h"

#include "access/relation.h"     // index_open / index_close
#include "access/table.h"        // table_open / table_close
#include "access/xact.h"         // CommandCounterIncrement
#include "catalog/namespace.h"
#include "catalog/pg_type_d.h"   // TEXTOID, VARCHAROID, BPCHAROID
#include "commands/sequence.h"   // ResetSequenceCaches
#include "executor/executor.h"   // ExecConstraints, ExecInitExpr, ExecEvalExpr
#include "executor/tuptable.h"   // TupleTableSlot, MakeTupleTableSlot, TTSOpsVirtual, Exec*Tuple
#include "nodes/makefuncs.h"     // makeNode (ResultRelInfo)
#include "optimizer/optimizer.h" // expression_planner
#include "utils/array.h"         // deconstruct_array, ARR_ELEMTYPE
#include "utils/builtins.h"      // text_to_cstring, cstring_to_text_with_len, regtypein
#include "utils/lsyscache.h"     // getTypeInputInfo, get_typtype, getBaseType
#include "utils/rel.h"           // RelationGetDescr
#include "utils/resowner.h"      // CurrentResourceOwner, TopTransactionResourceOwner
#include "utils/snapmgr.h" // Snapshot stack operations.
#include "utils/typcache.h" // lookup_rowtype_tupdesc

#include "fmgr.h"
#include "rewrite/rewriteHandler.h" // build_column_default
}

export module bioseq.postgres.storage.io:load_runtime;

import bioseq.common.logging; // Load diagnostics.
import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.support;
import bioseq.postgres.support.datums;
import bioseq.postgres.storage.io.sql_rows;
import bioseq.postgres.storage.io.sql_row_codecs;
import bioseq.postgres.configuration.core;
import bioseq.storage.size; // sequence_size_t
import bioseq.storage.layout;
import bioseq.common.templating.runtime_dispatch; // dispatch_exact_value_or_default
import bioseq.common.templating.value_sequence;
import bioseq.sequences;                       // Sequence alphabets and packed storage types.

import bioseq.postgres.types.compact_sequence; // PostgreSQL sequence values and supported alphabets.
import bioseq.postgres.storage.manager; // Sequence locators, fragment writers and relation caches.


import bioseq.postgres.table.io; // Column table row writes and per-row memory contexts.
import bioseq.io.options;                      // sequence_file_format, reader_options
import bioseq.io.file_kind;                    // detect_sequence_file_kind, sequence_file_kind
import bioseq.io.reader;                       // Stream sequence chunks and sample input headers.

import bioseq.io.identifier_parser.formats;
import bioseq.io.identifier_parser; // Map input header fields to destination columns.
import bioseq.postgres.storage.metadata;
import bioseq.postgres.storage.physical;

import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.postgres.scheduler.supervisor;
import :typed_row_writer;
import :stream_dispatch;
import :load_catalog;

/**
 * @brief Finish triggers and release the load's executor state.
 * @param[in,out] ctx Load context whose executor pointers are cleared.
 * @param[in] stable_context Context that remains alive after executor cleanup.
 * @throws PostgreSQL ERROR if trigger or executor cleanup fails.
 */
void teardown_load_executor(load_context& ctx, MemoryContext stable_context) {
    if (ctx.estate == nullptr) {
        return;
    }
    // FreeExecutorState deletes the executor memory contexts. Switch to a context
    // that survives it so invoke_pg_as_cpp can restore a valid current context.
    MemoryContextSwitchTo(stable_context);
    bioseq::postgres::support::invoke_pg_as_cpp([&ctx] { AfterTriggerEndQuery(ctx.estate); });
    bioseq::postgres::support::invoke_pg_as_cpp([&ctx] { ExecCloseResultRelations(ctx.estate); });
    bioseq::postgres::support::invoke_pg_as_cpp(
        [&ctx] { ExecCloseRangeTableRelations(ctx.estate); });
    bioseq::postgres::support::invoke_pg_as_cpp(
        [&ctx] { ExecResetTupleTable(ctx.estate->es_tupleTable, false); });
    bioseq::postgres::support::invoke_pg_as_cpp([&ctx] { FreeExecutorState(ctx.estate); });
    ctx.estate = nullptr;
    ctx.econtext = nullptr;
    ctx.result_rel_info = nullptr;
}

/**
 * @brief Release the executor, tuple slot and parent table reference for a load.
 *
 * The transaction keeps the parent table's RowExclusiveLock until it ends.
 * Finish the write states separately before calling this function on success.
 *
 * @param[in,out] ctx Load resources to release.
 * @param[in] commit Unused completion flag.
 * @param[in] stable_context Context used while releasing executor resources.
 * @throws PostgreSQL ERROR if resource cleanup fails.
 */
void cleanup_load_context(load_context& ctx, bool commit, MemoryContext stable_context) {
    MemoryContextSwitchTo(stable_context);
    // Release the executor before closing the parent table. Repeated calls do nothing
    // once the executor pointer has been cleared.
    teardown_load_executor(ctx, stable_context);
    if (ctx.slot != nullptr) {
        bioseq::postgres::support::invoke_pg_as_cpp(
            [&ctx] { ExecDropSingleTupleTableSlot(ctx.slot); });
        ctx.slot = nullptr;
    }
    // finish_load_write_states() handles write-state cleanup separately.
    (void)commit;
    ctx.caches = nullptr;
    const ResourceOwner saved = CurrentResourceOwner;
    if (ctx.parent_rel != nullptr) {
        Relation relation = ctx.parent_rel;
        bioseq::postgres::support::invoke_pg_as_cpp([relation, saved] {
            CurrentResourceOwner = TopTransactionResourceOwner;
            table_close(relation, NoLock);
            CurrentResourceOwner = saved;
        });
        ctx.parent_rel = nullptr;
    }
}

/**
 * @brief Load sequence records and finish their writes.
 * The target table keeps RowExclusiveLock until transaction end. Finish parent
 * and fragment writes before rebuilding indexes configured for update on insert.
 * Cleanup preserves the original failure.
 * @param[in] relid Target OID.
 * @param[in] path Borrowed input path.
 * @param[in] kind Detected file kind.
 * @param[in] requested_fmt Header parser.
 * @param[in] user_maps Borrowed header-to-column mappings.
 * @return Complete records inserted.
 * @throws postgres_error If setup, parsing, writing, indexing or cleanup fails.
 */
[[nodiscard]] std::int64_t run_load(Oid relid,
                                    const char* path,
                                    bioseq::io::sequence_file_kind kind,
                                    idp::header_format requested_fmt,
                                    std::span<const idp::user_column_mapping> user_maps) {
    load_context ctx{};
    ctx.main_relid = relid;
    const MemoryContext stable_context = CurrentMemoryContext;
    std::int64_t n = 0;
    std::exception_ptr failure;

    try {
        // Keep the parent table reference with the transaction. Restore the caller
        // resource owner before creating slots and tuple descriptors so those
        // resources are released with the caller's executor.
        const ResourceOwner saved = CurrentResourceOwner;
        ctx.parent_rel = bioseq::postgres::support::invoke_pg_as_cpp([relid, saved] {
            CurrentResourceOwner = TopTransactionResourceOwner;
            Relation result = table_open(relid, RowExclusiveLock);
            CurrentResourceOwner = saved;
            return result;
        });

        ctx.parent_td = RelationGetDescr(ctx.parent_rel);
        ctx.cols = resolve_load_columns(ctx.parent_rel);
        setup_typed_column_mapping(ctx, path, kind, requested_fmt, user_maps);
        ctx.db_oid = ctx.parent_rel->rd_locator.dbOid;
        ctx.spc_oid = ctx.parent_rel->rd_locator.spcOid;
        ctx.main_relfilenode = ctx.parent_rel->rd_locator.relNumber;
        ctx.main_storage_id = bioseq::postgres::support::invoke_pg_as_cpp(
            [&ctx] { return column_storage_id(ctx.parent_rel, false); });
        ctx.cap_bytes = bioseq::storage::read_max_sub_table_cap_bytes();

        const SubTransactionId subtransaction = bioseq::postgres::support::invoke_pg_as_cpp(
            [] { return GetCurrentSubTransactionId(); });
        ctx.write_state =
            initialize_column_write_state(ctx.parent_rel, ctx.parent_td, relid, subtransaction);
        if (ctx.write_state->seq_caches == nullptr) {
            auto* c = bioseq::postgres::support::invoke_pg_as_cpp([&ctx] {
                return bioseq::storage::alloc_seq_caches(
                    MemoryContextGetParent(ctx.write_state->stripe_write_context));
            });
            c->side = bioseq::storage::seq_caches::access_side::write;
            c->lock_mode = RowExclusiveLock;
            c->main_database_oid = ctx.db_oid;
            c->main_tablespace_oid = ctx.spc_oid;
            c->main_relfilenode = ctx.main_relfilenode;
            c->cap_bytes_snapshot = ctx.cap_bytes;
            ctx.write_state->seq_caches = c;
        }
        ctx.caches = static_cast<bioseq::storage::seq_caches*>(ctx.write_state->seq_caches);
        ctx.slot = bioseq::postgres::support::invoke_pg_as_cpp(
            [&ctx] {
                return MakeTupleTableSlot(ctx.parent_td, &TTSOpsVirtual
#if PG_VERSION_NUM >= 190000
                                          , 0
#endif
                );
            });

        pg_identifier_writer writer{ctx, ctx.dest, ctx.defaults};
        ctx.writer_ref = idp::make_identifier_writer_reference(writer);
        n = dispatch_and_stream(ctx, writer, path, kind);
        teardown_load_executor(ctx, stable_context);
        flush_pending_load_writes(ctx);
        MemoryContextSwitchTo(stable_context);
        finish_load_write_states(ctx);
        build_indexes_if_enabled(ctx.main_relid);
        reset_sequence_caches_after_load_storage_alloc();
    } catch (...) {
        failure = std::current_exception();
    }

    try {
        cleanup_load_context(ctx, failure == nullptr, stable_context);
    } catch (...) {
        if (failure == nullptr) {
            failure = std::current_exception();
        }
    }

    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
    return n;
}
