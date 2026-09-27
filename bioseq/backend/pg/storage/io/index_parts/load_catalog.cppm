/**
 * @file load_catalog.cppm
 * @brief Owns load-time catalog, relation, tuple descriptor, and executor setup.
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
export module bioseq.postgres.storage.io:load_catalog;

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
import bioseq.postgres.storage.io.index_maintenance;
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

// --------------------------------------------------------------------------
// Batched index build after the streamed insert
// --------------------------------------------------------------------------

/** @brief Builds update-on-insert indexes after all streamed writes finish. */
/// @param main_relid Open load target identified by relation OID.
/// @throws PostgreSQL ERROR when configured index maintenance fails.
///
/// The caller must keep the portal's active snapshot installed and must flush
/// parent and fragment write states before calling this function.
void build_indexes_if_enabled(Oid main_relid) {
    if (!bioseq::settings::index::insert_maintenance::enabled) {
        return;
    }
    // The grouped-SoA opt-in deliberately forbids the ordinary per-tuple INSERT
    // hook: that hook owns pre-publication bytes and cannot reopen a committed
    // PostgreSQL locator. bioseq.load is the explicit exception because it
    // first finishes every streamed parent/source write, then reaches this
    // batched SPI bioseq.update_index path. The normal grouped-SoA guards still
    // require both async-persistence GUCs OFF and the authoritative post-load
    // row scan to prove every non-NULL source is locator-backed before clear.
    // bioseq.load is one UDF inside one SQL command. The batched build
    // (build_policy_update_on_insert_indexes → SPI `bioseq.update_index`) issues
    // catalog SPI_execute(read_only=true) calls and columnar reads, both of which
    // require the executor's active snapshot. run_load calls this AFTER
    // finish_load_write_states (so the streamed rows are committed to stripes and
    // read back cleanly) but BEFORE reset_sequence_caches_after_load_storage_alloc
    // (whose DISCARD-SEQUENCES teardown leaves the snapshot unusable — building
    // after it SIGSEGV'd inside SPI_execute). At this point the portal snapshot
    // the SELECT pushed is still on the stack.
    DEBUG_ASSERT(ActiveSnapshotSet(),
                 "bioseq.load: index build needs the portal's active snapshot");
    bioseq::postgres::storage::io::maintain_insert_policy_indexes(main_relid);
}

/** @brief Clears sequence caches invalidated by post-load storage allocation. */
///
/// This resets PostgreSQL's backend-local sequence cache and does not alter
/// persisted sequence values.
void reset_sequence_caches_after_load_storage_alloc() {
    // Shard allocation advances an internal sequence. Clear its backend-local
    // cache before a same-command index scan and again before returning.
    bioseq::postgres::support::invoke_pg_as_cpp([] { ResetSequenceCaches(); });
}

// --------------------------------------------------------------------------
// Open, stream, flush, close, and index
// --------------------------------------------------------------------------

/** @brief Flushes the current column stripe before post-load index work. */
/// @param ctx Active load context owning parent and fragment writer state.
/// @throws PostgreSQL ERROR when either writer cannot be flushed.
///
/// Writer state and relation locks remain owned by the load after this call.
void flush_pending_load_writes(load_context& ctx) {
    // Parent rows go through ctx.write_state; fragment rows through per-subtable
    // write states opened from ctx.caches. update_index_impl uses raw columnar
    // readers + random reads, so both layers must be flushed before the same UDF
    // starts indexing.
    const SubTransactionId subtransaction =
        bioseq::postgres::support::invoke_pg_as_cpp([] { return GetCurrentSubTransactionId(); });
    bioseq::postgres::support::invoke_pg_as_cpp([&ctx, subtransaction] {
        flush_relation_write_state(ctx.main_relfilenode, subtransaction);
    });
    bioseq::postgres::support::invoke_pg_as_cpp([&ctx, subtransaction] {
        bioseq::storage::flush_seq_cache_write_states(ctx.caches, subtransaction);
    });
    bioseq::postgres::support::invoke_pg_as_cpp([] { CommandCounterIncrement(); });
}

/** @brief Finalizes parent and sequence-subtable write states exactly once. */
/// @param ctx Active load context whose write-state pointers are cleared.
/// @throws PostgreSQL ERROR when a parent or shard writer cannot commit.
///
/// Each write state is popped under `TopTransactionResourceOwner`. This does
/// not end the surrounding SQL transaction.
void finish_load_write_states(load_context& ctx) {
    std::vector<Oid> shard_relfilenodes;
    shard_relfilenodes.reserve(ctx.caches == nullptr ? 0 : ctx.caches->entries.size());
    if (ctx.caches != nullptr) {
        for (const auto& kv : ctx.caches->entries) {
            const auto& e = kv.second;
            if (e.rel != nullptr) {
                shard_relfilenodes.push_back(e.rel->rd_locator.relNumber);
            }
        }
    }

    const ResourceOwner saved = CurrentResourceOwner;
    const SubTransactionId subtransaction =
        bioseq::postgres::support::invoke_pg_as_cpp([] { return GetCurrentSubTransactionId(); });
    for (Oid relfilenode : shard_relfilenodes) {
        bioseq::postgres::support::invoke_pg_as_cpp([saved, relfilenode, subtransaction] {
            CurrentResourceOwner = TopTransactionResourceOwner;
            pop_relation_column_write_state(relfilenode, subtransaction, 0, /*commit=*/true);
            CurrentResourceOwner = saved;
        });
    }
    bioseq::postgres::support::invoke_pg_as_cpp([&ctx, saved, subtransaction] {
        CurrentResourceOwner = TopTransactionResourceOwner;
        pop_relation_column_write_state(ctx.main_relfilenode, subtransaction, 0, /*commit=*/true);
        CurrentResourceOwner = saved;
    });

    ctx.write_state = nullptr;
    ctx.caches = nullptr;
    bioseq::postgres::support::invoke_pg_as_cpp([] { CommandCounterIncrement(); });
}

// --------------------------------------------------------------------------
// Typed multi-column mapping setup
// --------------------------------------------------------------------------

/** @brief Minimal PostgreSQL type metadata retained for a mapped column. */
struct column_attr {
    /// One-based destination attribute number.
    std::int16_t attno;
    /// PostgreSQL destination type OID.
    Oid atttypid;
    /// Destination typmod passed to the type input function.
    std::int32_t atttypmod;
};

/// Decode bioseq.column_mapping[] using the element OID carried by the array.
/// Returned string views remain in the caller's memory context for the load.
/// @param arr Borrowed PostgreSQL array with a valid composite element type.
/// @return Owning mapping vector whose string views refer to text copied into
/// the current PostgreSQL memory context.
/// @throws PostgreSQL ERROR when the descriptor or any array element is invalid.
[[nodiscard]] std::vector<idp::user_column_mapping> parse_column_mapping_array(ArrayType* arr) {
    std::vector<idp::user_column_mapping> maps;
    const Oid mapping_type_oid = ARR_ELEMTYPE(arr);
    Datum* elems = nullptr;
    bool* nulls = nullptr;
    int nelems = 0;
    bioseq::postgres::support::invoke_pg_as_cpp([arr, mapping_type_oid, &elems, &nulls, &nelems] {
        deconstruct_array(arr, mapping_type_oid, -1, false, 'd', &elems, &nulls, &nelems);
    });
    TupleDesc cmdesc = bioseq::postgres::support::invoke_pg_as_cpp(
        [mapping_type_oid] { return lookup_rowtype_tupdesc(mapping_type_oid, -1); });
    const MemoryContext mapping_result_context = CurrentMemoryContext;
    std::exception_ptr failure;
    try {
        column_mapping_codec::validate_descriptor(cmdesc);
        for (int i = 0; i < nelems; ++i) {
            if (nulls[i]) {
                raise_load_error_as_cpp(ERRCODE_NULL_VALUE_NOT_ALLOWED,
                                        fmt::format("columns[] element {} is NULL", i + 1));
            }
            HeapTupleHeader th = bioseq::postgres::support::pg_datum::to_heap_tuple_header(
                bioseq::postgres::support::pg_datum::from_raw(elems[i]));
            HeapTupleData tmp{};
            tmp.t_len = HeapTupleHeaderGetDatumLength(th);
            tmp.t_data = th;
            const column_mapping_row mapping =
                column_mapping_codec::deform(&tmp, cmdesc, mapping_result_context);
            if (!mapping.column_name.has_value() || *mapping.column_name == nullptr) {
                raise_load_error_as_cpp(ERRCODE_NULL_VALUE_NOT_ALLOWED,
                                        fmt::format("columns[{}].column_name is NULL", i + 1));
            }
            const char* col = bioseq::postgres::support::invoke_pg_as_cpp(
                [value = *mapping.column_name] { return text_to_cstring(value); });
            const char* ds =
                !mapping.data_source.has_value() || *mapping.data_source == nullptr
                    ? ""
                    : bioseq::postgres::support::invoke_pg_as_cpp(
                          [value = *mapping.data_source] { return text_to_cstring(value); });
            maps.push_back(idp::user_column_mapping{std::string_view{col}, std::string_view{ds}});
        }
    } catch (...) {
        failure = std::current_exception();
    }
    try {
        bioseq::postgres::support::invoke_pg_as_cpp([cmdesc] { ReleaseTupleDesc(cmdesc); });
    } catch (...) {
        if (failure == nullptr) {
            failure = std::current_exception();
        }
    }
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
    return maps;
}

/// Resolve column mappings and prepare coercion, defaults, and constraints
/// before streaming.
/// @param ctx Active load context whose parser and executor state is populated.
/// @param path Null-terminated input path used for optional header sampling.
/// @param kind Detected file kind and compression metadata.
/// @param requested_fmt User-selected or automatic header format.
/// @param user_maps Borrowed mappings that remain valid for this call.
/// @throws PostgreSQL ERROR for unsupported columns, invalid mapping, type
/// metadata failure, or executor setup failure.
///
/// Any inspected primary index is held with `AccessShareLock` only during
/// setup. Executor objects are retained in `ctx` until load cleanup.
void setup_typed_column_mapping(load_context& ctx,
                                const char* path,
                                bioseq::io::sequence_file_kind kind,
                                idp::header_format requested_fmt,
                                std::span<const idp::user_column_mapping> user_maps) {
    ctx.requested_fmt = requested_fmt;
    const TupleDesc td = ctx.parent_td;
    const std::int16_t seq_attno = ctx.cols.seq_attno;

    // Record a single-column primary key; composite keys need no special path.
    std::int16_t pk_attno = 0;
    {
        const Oid pk_oid = bioseq::postgres::support::invoke_pg_as_cpp(
            [&ctx] { return bioseq::postgres::support::find_primary_key_oid(ctx.parent_rel); });
        if (OidIsValid(pk_oid)) {
            Relation pk_idx = bioseq::postgres::support::invoke_pg_as_cpp(
                [pk_oid] { return index_open(pk_oid, AccessShareLock); });
            if (pk_idx->rd_index != nullptr && pk_idx->rd_index->indnatts == 1) {
                pk_attno = static_cast<std::int16_t>(pk_idx->rd_index->indkey.values[0]);
            }
            bioseq::postgres::support::invoke_pg_as_cpp(
                [pk_idx] { index_close(pk_idx, AccessShareLock); });
        }
    }

    // Flatten non-sequence columns without leaking PostgreSQL types into the
    // identifier resolver. Identity and generated columns are unsupported.
    std::vector<idp::column_descriptor> columns;
    std::vector<column_attr> col_attrs;
    for (int i = 0; i < td->natts; ++i) {
        const Form_pg_attribute attr = bioseq::postgres::support::tuple_descriptor_attribute(td, i);
        if (attr->attisdropped || attr->attnum <= 0) {
            continue;
        }
        const std::int16_t attno = static_cast<std::int16_t>(i + 1);
        if (attno == seq_attno) {
            continue;
        }
        if (attr->attidentity != '\0' || attr->attgenerated != '\0') {
            raise_load_error_as_cpp(
                ERRCODE_INVALID_TABLE_DEFINITION,
                fmt::format("column \"{}\" is an identity or generated column; "
                            "bioseq.load cannot populate identity/generated columns",
                            NameStr(attr->attname)));
        }
        const bool is_text = (attr->atttypid == TEXTOID || attr->atttypid == VARCHAROID ||
                              attr->atttypid == BPCHAROID);
        columns.push_back(idp::column_descriptor{
            .name = std::string_view{NameStr(attr->attname)},
            .is_text = is_text,
            .is_primary_key = (pk_attno != 0 && attno == pk_attno),
            .not_null = attr->attnotnull,
            .has_default = (attr->atthasdef != false),
            .is_identity_or_generated = false,
        });
        col_attrs.push_back(column_attr{attno, attr->atttypid, attr->atttypmod});
    }

    // Preserve the whole header when AUTO has one text destination and no
    // explicit mapping.
    if (requested_fmt == idp::header_format::automatic && user_maps.empty() &&
        columns.size() == 1 && columns[0].is_text) {
        ctx.parser_active = false;
        ctx.effective_fmt = idp::header_format::plain;
        ctx.cols.header_attno = col_attrs[0].attno;
        return;
    }
    ctx.parser_active = true;

    // Sample headers to determine the effective AUTO format.
    idp::header_format effective_fmt = requested_fmt;
    if (requested_fmt == idp::header_format::automatic) {
        const auto samples = bioseq::io::sample_sequence_headers(
            path, kind.format, bioseq::io::default_reader_options);
        std::vector<std::string_view> views;
        if (samples) {
            for (const std::string& sample : *samples) {
                views.emplace_back(sample);
            }
        }
        effective_fmt = idp::detect_header_format(views);
        if (effective_fmt == idp::header_format::plain) {
            const bool has_metadata_target = columns.size() > 1 ||
                                             (columns.size() == 1 && !columns[0].is_text) ||
                                             !user_maps.empty();
            if (has_metadata_target) {
                effective_fmt = idp::header_format::automatic;
            }
        }
    }
    ctx.effective_fmt = effective_fmt;

    // Resolve the PostgreSQL-independent identifier mapping.
    auto map = idp::resolve_identifier_mapping(requested_fmt, effective_fmt, columns, user_maps);
    if (!map) {
        raise_load_error_as_cpp(ERRCODE_INVALID_TABLE_DEFINITION, map.error());
    }
    ctx.routing = std::move(map->routing);
    const std::vector<std::uint16_t> slot_to_column = std::move(map->slot_to_column);

    // Reject an unmapped NOT NULL column that has no default. Per-row NULL
    // violations are left to ExecConstraints.
    std::vector<char> referenced(columns.size(), 0);
    for (const std::uint16_t column_index : slot_to_column) {
        referenced[column_index] = 1;
    }
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (referenced[i]) {
            continue;
        }
        if (columns[i].not_null && !columns[i].has_default) {
            raise_load_error_as_cpp(
                ERRCODE_INVALID_TABLE_DEFINITION,
                fmt::format("column \"{}\" is NOT NULL but is unmapped and has no default",
                            columns[i].name));
        }
    }

    // Build one cached coercion entry per resolved destination.
    ctx.dest.clear();
    ctx.dest.reserve(slot_to_column.size());
    for (const std::uint16_t ci : slot_to_column) {
        const column_attr& ca = col_attrs[ci];
        Oid input_fn = InvalidOid;
        Oid io_param = InvalidOid;
        char typtype = '\0';
        bioseq::postgres::support::invoke_pg_as_cpp([&ca, &input_fn, &io_param, &typtype] {
            getTypeInputInfo(ca.atttypid, &input_fn, &io_param);
            typtype = get_typtype(getBaseType(ca.atttypid));
        });
        dest_kind kind;
        if (columns[ci].is_text)
            kind = ca.atttypmod < 0 ? dest_kind::text : dest_kind::text_with_typmod;
        else if (typtype == TYPTYPE_RANGE)
            kind = dest_kind::range;
        else if (typtype == TYPTYPE_MULTIRANGE)
            kind = dest_kind::multirange;
        else
            kind = dest_kind::scalar;
        ctx.dest.push_back(dest_coercion{
            .attno = ca.attno,
            .column_name = std::string{columns[ci].name},
            .kind = kind,
            .input_fn = input_fn,
            .io_param = io_param,
            .typmod = ca.atttypmod,
        });
    }

    // Compile defaults as executor expressions so volatile defaults evaluate
    // once per row.
    ctx.defaults.clear();
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (!columns[i].has_default) {
            continue;
        }
        Node* const raw = bioseq::postgres::support::invoke_pg_as_cpp([&ctx, &col_attrs, i] {
            return build_column_default(ctx.parent_rel, col_attrs[i].attno);
        });
        if (raw == nullptr) {
            continue;
        }
        Expr* const planned = bioseq::postgres::support::invoke_pg_as_cpp(
            [raw] { return expression_planner(reinterpret_cast<Expr*>(raw)); });
        ExprState* const state = bioseq::postgres::support::invoke_pg_as_cpp(
            [planned] { return ExecInitExpr(planned, nullptr); });
        ctx.defaults.push_back(default_col{col_attrs[i].attno, state});
    }

    // A private executor is needed only when PostgreSQL must evaluate a default
    // or enforce a relation constraint. Header parsing/coercion alone does not
    // need an EState. Besides avoiding unnecessary executor setup on the hot
    // load path, this keeps a nested AfterTrigger query frame out of ordinary
    // typed loads before their same-statement SPI index build.
    const bool needs_private_executor =
        !ctx.defaults.empty() || ctx.parent_rel->rd_att->constr != nullptr;
    if (!needs_private_executor) {
        return;
    }
    ctx.estate = bioseq::postgres::support::invoke_pg_as_cpp([&ctx] {
        return bioseq::storage::metadata::tuple_ops::create_relation_executor_state(ctx.parent_rel);
    });
    ctx.econtext = bioseq::postgres::support::invoke_pg_as_cpp(
        [&ctx] { return GetPerTupleExprContext(ctx.estate); });
    ctx.result_rel_info =
        bioseq::postgres::support::invoke_pg_as_cpp([] { return makeNode(ResultRelInfo); });
    bioseq::postgres::support::invoke_pg_as_cpp(
        [&ctx] { InitResultRelInfo(ctx.result_rel_info, ctx.parent_rel, 1, nullptr, 0); });
    // NB: NO ExecOpenIndices here — this path only restores NOT NULL / CHECK
    // enforcement around write_column_row; index maintenance stays on the
    // existing Bioseq load/index policy path.
}
