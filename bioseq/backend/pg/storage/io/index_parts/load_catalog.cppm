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

import bioseq.io.sinks;

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
import bioseq.io.identifier_parser.preservation;
import bioseq.io.simd;
import bioseq.parallelism.memory_admission;
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

/** Plain decoded options used only on the PostgreSQL caller thread. */
struct load_request_options {
    std::optional<std::string> phred_type;
    std::optional<Oid> reference_table;
};

/** Decode generated nullable load options without relying on composite layout. */
[[nodiscard]] load_request_options parse_load_options(HeapTupleHeader header) {
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
        return {std::move(name), options.reference_table};
    } catch (...) {
        bioseq::postgres::support::invoke_pg_as_cpp([descriptor] { ReleaseTupleDesc(descriptor); });
        throw;
    }
}

/** Create one nullable IO destination under the existing owner/lock/DDL contract.
 * The type SQL is produced internally from registry names, never caller text. */
void prepare_load_destination(Oid relid, const std::string& name, const std::string& type_sql) {
    if (name.empty() || name.size() >= NAMEDATALEN || name.find('\0') != std::string::npos)
        raise_load_error_as_cpp(ERRCODE_INVALID_PARAMETER_VALUE, "invalid IO destination name");
    bioseq::postgres::support::invoke_pg_as_cpp([relid, &name, &type_sql] {
        Relation relation = table_open(relid, RowExclusiveLock);
        if (get_attnum(relid, name.c_str()) == InvalidAttrNumber) {
            table_close(relation, RowExclusiveLock);
            relation = table_open(relid, AccessExclusiveLock);
            if (get_attnum(relid, name.c_str()) == InvalidAttrNumber) {
                if (!object_ownercheck(RelationRelationId, relid, GetUserId()))
                    ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                        errmsg("must be owner of table %s", RelationGetRelationName(relation)),
                        errhint("Ask the table owner to predeclare IO destination column \"%s\".", name.c_str())));
                const auto* qualified = quote_qualified_identifier(
                    get_namespace_name(RelationGetNamespace(relation)), RelationGetRelationName(relation));
                const auto* quoted = quote_identifier(name.c_str());
                char* command = psprintf("ALTER TABLE %s ADD COLUMN %s %s", qualified, quoted, type_sql.c_str());
                table_close(relation, NoLock);
                relation = nullptr;
                if (SPI_connect() != SPI_OK_CONNECT) elog(ERROR, "cannot connect SPI for IO destination DDL");
                if (SPI_execute(command, false, 0) != SPI_OK_UTILITY) elog(ERROR, "IO destination DDL failed");
                SPI_finish();
                CommandCounterIncrement();
                relation = table_open(relid, NoLock);
            }
        }
        table_close(relation, NoLock);
    });
}

/** Reuse the unique declared quality column unless the caller explicitly maps it. */
[[nodiscard]] std::string resolve_quality_destination_name(Oid relid, std::optional<std::string> requested) {
    if (requested) return *requested;
    std::string name;
    bioseq::postgres::support::invoke_pg_as_cpp([relid, &name] {
        Relation relation = table_open(relid, RowExclusiveLock);
        const auto descriptor = RelationGetDescr(relation);
        for (int index = 0; index < descriptor->natts; ++index) {
            const auto* attribute = TupleDescAttr(descriptor, index);
            if (attribute->attisdropped || attribute->atttypid != load_seq_type_oid() || attribute->atttypmod < 0
                || !is_quality_alphabet_code(compact_sequence_stdc_type_params::decode(attribute->atttypmod).alphabet_code)) continue;
            if (!name.empty()) ereport(ERROR, (errcode(ERRCODE_AMBIGUOUS_COLUMN),
                errmsg("multiple quality columns; specify column_mapping")));
            name = NameStr(attribute->attname);
        }
        table_close(relation, NoLock);
    });
    return name.empty() ? "quality" : name;
}

/** Inspect the first canonical text envelope before automatic destination DDL.
 * Prefix windows grow only after existing memory admission succeeds. */
[[nodiscard]] std::optional<idp::text_record_shape> inspect_text_record_shape(const char* path,
        bioseq::io::sequence_file_format format) {
    const auto raw = bioseq::io::read_first_sequence_header(path, format, bioseq::io::default_reader_options);
    if (!raw) raise_load_error_as_cpp(ERRCODE_DATA_EXCEPTION, raw.error());
    if (!*raw) return std::nullopt;
    const auto canonical = idp::parse_canonical_header((**raw).text(), idp::canonical_parse_mode::shape);
    if (!canonical) raise_load_error_as_cpp(ERRCODE_DATA_EXCEPTION, canonical.error());
    if (!*canonical) return std::nullopt;
    // Only SQL identifiers/type names and decimal descriptors allocate strings here.
    constexpr std::size_t scalar_capacity = 4096u;
    auto scalar_permit = bioseq::parallelism::memory_admission_controller::instance().try_reserve(scalar_capacity * 4u);
    if (!scalar_permit) raise_load_error_as_cpp(ERRCODE_OUT_OF_MEMORY, "cannot admit canonical descriptor scratch");
    idp::text_record_shape shape;
    const auto decoded = idp::decode_preservation_record((**canonical).payload, shape, true, scalar_capacity);
    if (!decoded) raise_load_error_as_cpp(ERRCODE_DATA_EXCEPTION, decoded.error());
    return shape;
}

/** Resolve a quality destination, creating its exact declared codec when absent. */
void prepare_quality_destination(Oid relid, const std::string& name,
        std::optional<std::string> requested, std::string_view default_alphabet = "phred42") {
    if (requested && *requested == "phred") *requested = "phred42";
    const auto code = requested ? pgseq_supported_alphabets::try_index_of_name(requested->c_str())
                               : std::optional<pgseq_supported_alphabets::alphabet_code_t>{};
    if (requested && (!code || !is_quality_alphabet_code(*code)
            || pgseq_supported_alphabets::name_of_alphabet_code(*code) != *requested))
        raise_load_error_as_cpp(ERRCODE_INVALID_PARAMETER_VALUE,
            "phred_type must be phred, phred42, phred63, phred94 or phred68-legacy");
    prepare_load_destination(relid, name, "bioseq.seq('" + requested.value_or(std::string{default_alphabet}) + "')");
    bioseq::postgres::support::invoke_pg_as_cpp([relid, &name, &code] {
        Relation relation = table_open(relid, RowExclusiveLock);
        const auto attno = get_attnum(relid, name.c_str());
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

/** Select one full alignment composite, or add its default nullable destination. */
[[nodiscard]] std::string prepare_alignment_destination(Oid relid, std::optional<std::string> requested) {
    std::string name = requested.value_or(std::string{});
    bioseq::postgres::support::invoke_pg_as_cpp([relid, &name, &requested] {
        Relation relation = table_open(relid, RowExclusiveLock);
        const auto type_oid = bioseq_type_oid_missing_ok("alignment");
        const auto descriptor = RelationGetDescr(relation);
        for (int index = 0; index < descriptor->natts; ++index) {
            const auto* attribute = TupleDescAttr(descriptor, index);
            if (attribute->attisdropped || attribute->atttypid != type_oid) continue;
            if (requested && name != NameStr(attribute->attname)) continue;
            if (!requested && !name.empty())
                ereport(ERROR, (errcode(ERRCODE_AMBIGUOUS_COLUMN), errmsg("multiple alignment destinations; specify column_mapping")));
            name = NameStr(attribute->attname);
        }
        table_close(relation, NoLock);
    });
    if (name.empty()) name = "alignment";
    prepare_load_destination(relid, name, "bioseq.alignment");
    bioseq::postgres::support::invoke_pg_as_cpp([relid, &name] {
        Relation relation = table_open(relid, RowExclusiveLock);
        const auto attno = get_attnum(relid, name.c_str());
        const auto* attribute = TupleDescAttr(RelationGetDescr(relation), attno - 1);
        if (attribute->atttypid != bioseq_type_oid_missing_ok("alignment") || attribute->attidentity || attribute->attgenerated)
            ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH), errmsg("alignment destination must be a writable bioseq.alignment column")));
        table_close(relation, NoLock);
    });
    return name;
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
        if (attno == seq_attno || attno == ctx.cols.quality_attno || attno == ctx.cols.alignment_attno) {
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

    // Canonical headers must pass through routing even with a sole text
    // destination. Ordinary headers retain the existing whole-header behavior.
    const bool preserve_whole_header =
        requested_fmt == idp::header_format::automatic && user_maps.empty() &&
        columns.size() == 1 && columns[0].is_text;
    ctx.parser_active = true;

    // Sample headers to choose the format for  automatic parsing.
    idp::header_format effective_fmt = preserve_whole_header
        ? idp::header_format::plain : requested_fmt;
    if (requested_fmt == idp::header_format::automatic && !preserve_whole_header
        && kind.format != bioseq::io::sequence_file_format::sam && kind.format != bioseq::io::sequence_file_format::bam) {
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
