/**
 * @file load_catalog.cppm
 * @brief Prepare column mappings and finish table writes before building indexes.
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
#include "catalog/pg_class_d.h"
#include "catalog/pg_type_d.h"   // TEXTOID, VARCHAROID, BPCHAROID
#include "commands/sequence.h"   // ResetSequenceCaches
#include "executor/executor.h"   // ExecConstraints, ExecInitExpr, ExecEvalExpr
#include "executor/tuptable.h"   // TupleTableSlot, MakeTupleTableSlot, TTSOpsVirtual, Exec*Tuple
#include "executor/spi.h"
#include "utils/acl.h"
#include "miscadmin.h"
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

export module bioseq.postgres.storage.io:load_catalog;

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
import bioseq.postgres.storage.manager; // Sequence locators, fragment writers and  relation caches.


import bioseq.postgres.table.io; // Column table row writes and per-row memory contexts.
import bioseq.postgres.storage.io.index_maintenance;
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

// Build indexes after the streamed insert.

/** @brief Build indexes configured for update on insert after the load finishes writing. */
/// @param main_relid Target table OID.
/// @throws PostgreSQL ERROR if configured index maintenance fails.
///
/// Keep the caller's active snapshot installed and finish parent and fragment
/// writes befose calling this function
void build_indexes_if_enabled(Oid main_relid) {
    if (!bioseq::settings::index::insert_maintenance::enabled) {
        return;
    }
    // bioseq.load finishes parent and fragment writes before calling bioseq.update_index,
    // so the rebuild can,  read published locators in the same transaction.
    // Grouped indexes still require both asynchronous persistence settings to be off,
    // and the rebuild checks every non-NULL source locator before clearing the index.
    // Keep the caller's active snapshot installed for SPI calls and column reads.
    // Run this before resetting PostgreSQL's sequence caches.
    DEBUG_ASSERT(ActiveSnapshotSet(),
                 "bioseq.load: index build needs the portal's active snapshot");
    bioseq::postgres::storage::io::maintain_insert_policy_indexes(main_relid);
}

/** @brief Clear PostgreSQL sequence caches after allocating storage during a load. */
///
/// Stored sequence values remain unchanged. This resets PostgreSQL's local cache.
void reset_sequence_caches_after_load_storage_alloc() {
    // Shard allocation uses an internal PostgreSQL sequence. Clear its cached state
    // after  the load and index work finish.
    bioseq::postgres::support::invoke_pg_as_cpp([] { ResetSequenceCaches(); });
}

// Finish table and fragment writes.

/** @brief Flush parent and fragment writes so the index builder can read them. */
/// @param ctx Load context holding parent and fragment write states.
/// @throws PostgreSQL ERROR if either writer fails to flush.
///
/// The load keeps its write states and relation locks after this call.
void flush_pending_load_writes(load_context& ctx) {
    // The index builder reads both the parent locators and tjeir sequence fragments.
    // Flush both sets of rows before building indexes within  this SQL call.
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

/** @brief Finish the fragment and parent write states, then clear the load's references. */
/// @param ctx Load context whose write-state pointers are cleared.
/// @throws PostgreSQL ERROR if finishing a parent or fragment writer fails.
///
/// Finalize these writes under TopTransactionResourceOwner while keeping the
/// surrounding SQL transaction open
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

/** Decode generated nullable load options without relying on composite layout. */
[[nodiscard]] std::optional<std::string> parse_load_options(HeapTupleHeader header) {
    using codec = bioseq::postgres::storage::io::sql_row_codecs::load_options_codec;
    auto* descriptor = bioseq::postgres::support::invoke_pg_as_cpp([header] {
        return lookup_rowtype_tupdesc(HeapTupleHeaderGetTypeId(header), HeapTupleHeaderGetTypMod(header));
    });
    try {
        HeapTupleData tuple{};
        tuple.t_len = HeapTupleHeaderGetDatumLength(header);
        tuple.t_data = header;
        const auto options = codec::deform(&tuple, descriptor, CurrentMemoryContext);
        std::optional<std::string> name;
        if (options.phred_type) {
            name = bioseq::postgres::support::invoke_pg_as_cpp([value = *options.phred_type] {
                return text_to_cstring(value);
            });
        }
        bioseq::postgres::support::invoke_pg_as_cpp([descriptor] { ReleaseTupleDesc(descriptor); });
        return name;
    } catch (...) {
        bioseq::postgres::support::invoke_pg_as_cpp([descriptor] { ReleaseTupleDesc(descriptor); });
        throw;
    }
}

/** Resolve and, if needed, add a FASTQ quality column before any live writer exists. */
void prepare_quality_destination(Oid relid, const std::string& name,
        std::optional<std::string> requested) {
    if (name.empty() || name.size() >= NAMEDATALEN || name.find('\0') != std::string::npos)
        raise_load_error_as_cpp(ERRCODE_INVALID_PARAMETER_VALUE, "invalid quality destination name");
    if (requested && *requested == "phred") *requested = "phred42";
    const auto code = requested ? pgseq_supported_alphabets::try_index_of_name(requested->c_str())
                               : std::optional<pgseq_supported_alphabets::alphabet_code_t>{};
    if (requested && (!code || !is_quality_alphabet_code(*code)))
        raise_load_error_as_cpp(ERRCODE_INVALID_PARAMETER_VALUE,
            "phred_type must be phred, phred42, phred63 or phred68-legacy");
    // Recheck after acquiring the stronger lock; never hold row/fragment resources during DDL.
    bioseq::postgres::support::invoke_pg_as_cpp([relid, &name, &requested, &code] {
        Relation relation = table_open(relid, RowExclusiveLock);
        AttrNumber attno = get_attnum(relid, name.c_str());
        if (attno == InvalidAttrNumber) {
            table_close(relation, RowExclusiveLock);
            relation = table_open(relid, AccessExclusiveLock);
            attno = get_attnum(relid, name.c_str());
            if (attno == InvalidAttrNumber) {
                if (!object_ownercheck(RelationRelationId, relid, GetUserId()))
                    ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                        errmsg("must be owner of table %s", RelationGetRelationName(relation)),
                        errhint("Ask the table owner to predeclare quality column \"%s\" before loading FASTQ.", name.c_str())));
                const auto* qualified = quote_qualified_identifier(
                    get_namespace_name(RelationGetNamespace(relation)), RelationGetRelationName(relation));
                const auto* quoted = quote_identifier(name.c_str());
                const auto* alphabet = requested ? requested->c_str() : "phred42";
                char* command = psprintf("ALTER TABLE %s ADD COLUMN %s bioseq.seq('%s')", qualified, quoted, alphabet);
                if (SPI_connect() != SPI_OK_CONNECT) elog(ERROR, "cannot connect SPI for quality column DDL");
                const int status = SPI_execute(command, false, 0);
                if (status != SPI_OK_UTILITY) elog(ERROR, "quality column DDL failed");
                SPI_finish();
                CommandCounterIncrement();
                table_close(relation, NoLock);
                relation = table_open(relid, NoLock);
                attno = get_attnum(relid, name.c_str());
            }
        }
        if (attno <= 0) ereport(ERROR, (errcode(ERRCODE_INVALID_TABLE_DEFINITION), errmsg("quality destination is missing")));
        auto* attr = TupleDescAttr(RelationGetDescr(relation), attno - 1);
        const auto type = compact_sequence_stdc_type_params::decode(attr->atttypmod < 0 ? 0 : attr->atttypmod);
        if (attr->atttypid != load_seq_type_oid() || attr->atttypmod < 0
            || !is_quality_alphabet_code(type.alphabet_code)
            || !pgseq_supported_storage_unit_byte_widths::contains_runtime(type.storage_unit_byte_width)
            || attr->attidentity || attr->attgenerated)
            ereport(ERROR, (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                errmsg("quality destination \"%s\" must be a writable bound quality sequence", name.c_str())));
        if (code && type.alphabet_code != *code)
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("phred_type disagrees with the declared quality destination")));
        table_close(relation, NoLock);
    });
}

// Map header fields to typed columns.

/** @brief Column number and type information needed to convert header values. */
struct column_attr {
    /// Destination column number, starting at one.
    std::int16_t attno;
    /// PostgreSQL destination type OID.
    Oid atttypid;
    /// Type modifier passed to the input  function, such, as a text length limit.
    std::int32_t atttypmod;
};

/// Read header-to-column mappings from a bioseq.column_mapping array.
/// @param arr Borrowed PostgreSQL array whose element type is checked.
/// @return A vector of mappings whose strings live in the current PostgreSQL memory context.
/// @throws PostgreSQL ERROR for an invalid element type, a NULL element or a NULL column name.
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

/// Prepare header parsing and conversion into the target columns.
/// Cache type input functions and defaults before reading records, eeping any
/// executor state in ctx until cleanup. The primary-index lock is released here.
/// @param ctx Load  context to populate.
/// @param path Input path used to sample headers.
/// @param kind Input format and compression information.
/// @param requested_fmt Requested header format, including automatic detection.
/// @param user_maps Mappings borrowed during setup.
/// @throws PostgreSQL ERROR for unsupported columns, invalid mappings or failed setup.
void setup_typed_column_mapping(load_context& ctx,
                                const char* path,
                                bioseq::io::sequence_file_kind kind,
                                idp::header_format requested_fmt,
                                std::span<const idp::user_column_mapping> user_maps) {
    ctx.requested_fmt = requested_fmt;
    const TupleDesc td = ctx.parent_td;
    const std::int16_t seq_attno = ctx.cols.seq_attno;

    // Tell the header mapper about a primary key made from one column.
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

    // Give the header mapper plain column descriptions. Identity and generated
    // columns are rejected because this loader does not compute their values.
    std::vector<idp::column_descriptor> columns;
    std::vector<column_attr> col_attrs;
    for (int i = 0; i < td->natts; ++i) {
        const Form_pg_attribute attr = bioseq::postgres::support::tuple_descriptor_attribute(td, i);
        if (attr->attisdropped || attr->attnum <= 0) {
            continue;
        }
        const std::int16_t attno = static_cast<std::int16_t>(i + 1);
        if (attno == seq_attno || attno == ctx.cols.quality_attno) {
            continue;
        }
        if (attr->attidentity != '\0' || attr->attgenerated != '\0') {
            raise_load_error_as_cpp(
                ERRCODE_INVALID_TABLE_DEFINITION,
                fmt::format("column \"{}\" is an identity or generated column; "
                            "bioseq.load cannot populate identity/generated columns",
                            NameStr(attr->attname)));
        }
        if (attr->atttypid == load_seq_type_oid() && attr->atttypmod >= 0
            && is_quality_alphabet_code(compact_sequence_stdc_type_params::decode(attr->atttypmod).alphabet_code)) {
            if (attr->attnotnull && !attr->atthasdef)
                raise_load_error_as_cpp(ERRCODE_INVALID_TABLE_DEFINITION, "unmapped FASTA quality column requires NULL or a default");
            continue;
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
    } else {
    ctx.parser_active = true;

    // Sample headers to choose the format for  automatic parsing.
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

    // Decide which parsed fields fill which destination columns.
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

    // Cache the PostgreSQL input function and type limits for each mapped column
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

    }

    // Prepare default expressions once, then evaluate them separately for each row.
    ctx.defaults.clear();
    for (int index = 0; index < td->natts; ++index) {
        const auto* attr = TupleDescAttr(td, index);
        if (attr->attisdropped || !attr->atthasdef || attr->attnum == seq_attno
            || attr->attnum == ctx.cols.quality_attno) continue;
        Node* raw = bioseq::postgres::support::invoke_pg_as_cpp([&ctx, attr] {
            return build_column_default(ctx.parent_rel, attr->attnum);
        });
        if (!raw) continue;
        auto* planned = bioseq::postgres::support::invoke_pg_as_cpp([raw] {
            return expression_planner(reinterpret_cast<Expr*>(raw));
        });
        auto* state = bioseq::postgres::support::invoke_pg_as_cpp([planned] { return ExecInitExpr(planned, nullptr); });
        ctx.defaults.push_back(default_col{attr->attnum, state});
    }

    // Create an executor only for defaults and table constraints. Loads that only
    // parse and convert header fields can skip this setup and its trigger frame.
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
    // This executor checks NOT NULL and CHECK constraints around direct row writes.
    // Bioseq index maintenance runs after, the load. PostgreSQL indexes are not opened here.
}
