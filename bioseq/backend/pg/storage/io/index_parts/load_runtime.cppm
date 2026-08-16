/**
 * @file load_runtime.cppm
 * @brief Runs and cleans up the PostgreSQL load lifecycle in resource-safe order.
 */
module;
// Highway and PostgreSQL declarations precede the module unit because this
// owner imports Highway-bearing module graphs.
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
#include "catalog/namespace.h"   // (parity w/ sample_table_udf; relid arrives as regclass)
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
#include "utils/snapmgr.h" // PushActiveSnapshot/PopActiveSnapshot/GetTransactionSnapshot, ActiveSnapshotSet
#include "utils/typcache.h" // lookup_rowtype_tupdesc

#include "fmgr.h"
#include "rewrite/rewriteHandler.h" // build_column_default
}
// flush_relation_write_state declarations
export module bioseq.postgres.storage.io:load_runtime;

import bioseq.common.logging; // bioseq_log<logging_level::debug>(bioseq::settings::diagnostics::logging::enabled)
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
import bioseq.sequences;                       // compact_sequence_stdc_type_params::decode,
                                               //   dispatch_storage_unit_byte_width, traits
import bioseq.postgres.types.compact_sequence; // bioseq::pg_types::seq, pgseq_supported_alphabets
import bioseq.postgres.storage.manager; // alloc_locator_stub_in, alloc_seq_caches, seq_caches,
                                        //   read_max_sub_table_cap_bytes,
//   k_sequence_fragment_storage_units, sequence_stream_writer,
//   flush_seq_cache_write_states, sequence_locator
import bioseq.postgres.table.io; // write_column_row, get_column_write_per_tuple_context
import bioseq.postgres.storage.indexing.admin; // build_policy_update_on_insert_indexes
import bioseq.io.options;                      // sequence_file_format, reader_options
import bioseq.io.file_kind;                    // detect_sequence_file_kind, sequence_file_kind
import bioseq.io.reader;                       // stream_sequences_file, packed_sequence_chunk,
                                               //   sample_sequence_headers
import bioseq.io.identifier_parser.formats;
import bioseq.io.identifier_parser; // resolve_identifier_mapping, parse_and_route_header
import bioseq.postgres.storage.metadata;
import bioseq.postgres.storage.physical;
// initialize_column_write_state +
// pop_relation_column_write_state
// (PostgreSQL support already imported above: bioseq::postgres::support::find_primary_key_oid)
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.postgres.scheduler.supervisor;
import :typed_row_writer;
import :stream_dispatch;
import :load_catalog;

/**
 * @brief Tears down optional executor state for a streamed load.
 * @param[in,out] ctx Load context whose executor pointers are cleared.
 * @param[in] stable_context Caller-owned context that survives executor deletion.
 * @throws PostgreSQL ERROR when executor or trigger teardown fails.
 */
void teardown_load_executor(load_context& ctx, MemoryContext stable_context) {
    if (ctx.estate == nullptr) {
        return;
    }
    // FreeExecutorState destroys the estate's executor contexts.  The checked
    // PostgreSQL boundary restores the context that was current on entry, so it
    // must enter from a caller-owned context that survives the deletion.
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
 * @brief Releases invocation-owned executor, slot, and parent-relation state.
 *
 * The parent relation was opened under `RowExclusiveLock`, but this cleanup
 * closes it with `NoLock` so the transaction retains that lock until end.
 *
 * @param[in,out] ctx Load context to make inert.
 * @param[in] commit Whether the load completed successfully.
 * @param[in] stable_context Caller-owned context used during cleanup.
 * @throws PostgreSQL ERROR when executor, slot, or relation cleanup fails.
 */
void cleanup_load_context(load_context& ctx, bool commit, MemoryContext stable_context) {
    MemoryContextSwitchTo(stable_context);
    // Release the per-load executor state before
    // closing the parent relation. Idempotent (no-op if already torn down).
    teardown_load_executor(ctx, stable_context);
    if (ctx.slot != nullptr) {
        bioseq::postgres::support::invoke_pg_as_cpp(
            [&ctx] { ExecDropSingleTupleTableSlot(ctx.slot); });
        ctx.slot = nullptr;
    }
    // Write-state teardown is explicit in finish_load_write_states(). This
    // cleanup owns only the slot and parent relation.
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
 * @brief Owns the complete streamed-load lifecycle and converges cleanup.
 *
 * The parent table remains protected by `RowExclusiveLock` through the
 * transaction. Fragment writes are flushed before policy-driven index rebuilds,
 * and cleanup preserves the initiating exception if a secondary close fails.
 *
 * @param[in] relid Target table OID.
 * @param[in] path Input path borrowed for the duration of the call.
 * @param[in] kind Detected FASTA or FASTQ source kind.
 * @param[in] requested_fmt Header parser selected by the SQL caller.
 * @param[in] user_maps Parsed-column mappings borrowed for the load.
 * @return Number of complete records inserted.
 * @throws PostgreSQL ERROR when relation setup, parsing, persistence, index
 * maintenance, or cleanup fails.
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
        // Open the parent under TopTransactionResourceOwner, then restore the
        // caller's (executor/Portal) owner for the stream itself so the slot +
        // tuple descriptors stay owned by the Portal. Fragment shards are
        // similarly pinned by sequence_stream_writer.
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
            [&ctx] { return MakeTupleTableSlot(ctx.parent_td, &TTSOpsVirtual); });

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
