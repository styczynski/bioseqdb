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
#include <utility>
#include <vector>
extern "C" {
#include "postgres.h"

#include "access/relation.h"     // index_open / index_close
#include "access/sysattr.h"      // Bitmap offsets for index-expression attributes.
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
#include "utils/rls.h"           // Direct loads must not bypass row security.
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
import bioseq.postgres.column.table_access;
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

/** Reject targets whose INSERT semantics the streamed row writer cannot honor.
 * Keep the relation lock through the transaction, before any schema preparation
 * or source writes. PostgreSQL COPY likewise rejects active row security.
 * Return the locked physical identity for the direct-write cache notification. */
[[nodiscard]] RelFileLocator validate_load_target(Oid relation_id) {
    return bioseq::postgres::support::invoke_pg_as_cpp([relation_id] {
        const auto relation = table_open(relation_id, RowExclusiveLock);
        const auto acl = pg_class_aclcheck(relation_id, GetUserId(), ACL_INSERT);
        if (acl != ACLCHECK_OK)
            aclcheck_error(acl, OBJECT_TABLE, RelationGetRelationName(relation));
        if (relation->rd_rel->relkind != RELKIND_RELATION
            || !relation_uses_column_table_access_method(relation))
            ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                errmsg("bioseq.load target must be a table using the bioseq access method")));
        if (check_enable_rls(relation_id, InvalidOid, false) == RLS_ENABLED)
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("bioseq.load does not support targets with active row-level security"),
                errhint("Use INSERT to apply the table's row-security policies.")));
        const auto* triggers = relation->trigdesc;
        if (triggers && (triggers->trig_insert_before_row || triggers->trig_insert_instead_row
            || triggers->trig_insert_after_row || triggers->trig_insert_before_statement
            || triggers->trig_insert_after_statement || triggers->trig_insert_new_table))
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("bioseq.load does not support target INSERT triggers"),
                errhint("Use INSERT for supported BEFORE or statement triggers. AFTER ROW and constraint triggers are unsupported by the bioseq access method.")));
        const auto locator = relation->rd_locator;
        table_close(relation, NoLock);
        return locator;
    });
}

/** Determine whether an opened ordinary index reads a sequence-valued column.
 * Scalar keys avoid flushing streamed fragments just to populate their index;
 * expressions, predicates, INCLUDE columns and whole-row references are covered. */
[[nodiscard]] bool load_indexes_read_sequences(const load_context& ctx) {
    return bioseq::postgres::support::invoke_pg_as_cpp([&ctx] {
        Bitmapset* attributes = nullptr;
        for (int index = 0; index < ctx.result_rel_info->ri_NumIndices; ++index) {
            const auto* info = ctx.result_rel_info->ri_IndexRelationInfo[index];
            for (int column = 0; column < info->ii_NumIndexAttrs; ++column)
                if (info->ii_IndexAttrNumbers[column] != 0)
                    attributes = bms_add_member(attributes,
                        info->ii_IndexAttrNumbers[column] - FirstLowInvalidHeapAttributeNumber);
            pull_varattnos(reinterpret_cast<Node*>(info->ii_Expressions), 1, &attributes);
            pull_varattnos(reinterpret_cast<Node*>(info->ii_Predicate), 1, &attributes);
        }
        const bool whole_row = bms_is_member(-FirstLowInvalidHeapAttributeNumber, attributes);
        const auto sequence_type = load_seq_type_oid();
        bool reads_sequences = false;
        for (int column = 0; column < ctx.parent_td->natts && !reads_sequences; ++column) {
            const auto* attribute = TupleDescAttr(ctx.parent_td, column);
            reads_sequences = !attribute->attisdropped && getBaseType(attribute->atttypid) == sequence_type
                && (whole_row || bms_is_member(attribute->attnum - FirstLowInvalidHeapAttributeNumber, attributes));
        }
        bms_free(attributes);
        return reads_sequences;
    });
}

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
    const bool missing = bioseq::postgres::support::invoke_pg_as_cpp([relid, &name] {
        Relation relation = table_open(relid, RowExclusiveLock);
        const bool result = get_attnum(relid, name.c_str()) == InvalidAttrNumber;
        table_close(relation, result ? RowExclusiveLock : NoLock);
        return result;
    });
    if (!missing) return;
    // This owner must be above the PG error bridge, including event-trigger
    // callbacks. It stabilizes the OID's name while preserving normal SPI DDL.
    const bioseq::postgres::support::scoped_relation_identity identity(relid);
    const bool still_missing = bioseq::postgres::support::invoke_pg_as_cpp([relid, &name] {
        if (get_attnum(relid, name.c_str()) != InvalidAttrNumber) return false;
        if (!object_ownercheck(RelationRelationId, relid, GetUserId()))
            ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                errmsg("must be owner of table %s", get_rel_name(relid)),
                errhint("Ask the table owner to predeclare IO destination column \"%s\".", name.c_str())));
        return true;
    });
    if (!still_missing) return;
    const auto* qualified = identity.qualified_name();
    bioseq::postgres::support::scoped_spi_connection connection;
    bioseq::postgres::support::invoke_pg_as_cpp([qualified, &name, &type_sql] {
        const auto* quoted = quote_identifier(name.c_str());
        const auto* command = psprintf("ALTER TABLE %s ADD COLUMN %s %s", qualified, quoted, type_sql.c_str());
        if (SPI_execute(command, false, 0) != SPI_OK_UTILITY)
            elog(ERROR, "IO destination DDL failed");
        CommandCounterIncrement();
    });
    static_cast<void>(connection.finish());
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
    // Scalar limits are validation bounds; the decoder owns actual scratch admission.
    constexpr std::size_t scalar_capacity = 4096u;
    idp::text_record_shape shape{**canonical};
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
        if (attno <= 0)
            ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN),
                errmsg("alignment destination \"%s\" does not exist", name.c_str())));
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
            if (pk_idx->rd_index != nullptr && pk_idx->rd_index->indnkeyatts == 1) {
                pk_attno = static_cast<std::int16_t>(pk_idx->rd_index->indkey.values[0]);
            }
            bioseq::postgres::support::invoke_pg_as_cpp(
                [pk_idx] { index_close(pk_idx, AccessShareLock); });
        }
    }

    // Resolve both column and type defaults once, before telling the mapper
    // which destinations have defaults. Planning remains after mapping validation.
    std::vector<Expr*> default_expressions(static_cast<std::size_t>(td->natts), nullptr);
    ctx.domain_columns.clear();
    for (int index = 0; index < td->natts; ++index) {
        const auto* attribute = TupleDescAttr(td, index);
        if (attribute->attisdropped || attribute->attidentity || attribute->attgenerated
            || attribute->attnum == seq_attno || attribute->attnum == ctx.cols.quality_attno) continue;
        const auto descriptor = bioseq::postgres::support::invoke_pg_as_cpp([&ctx, attribute] {
            return std::pair{build_column_default(ctx.parent_rel, attribute->attnum),
                get_typtype(attribute->atttypid) == TYPTYPE_DOMAIN};
        });
        default_expressions[index] = reinterpret_cast<Expr*>(descriptor.first);
        if (descriptor.second) ctx.domain_columns.push_back(
            {attribute->attnum, attribute->atttypid, descriptor.first != nullptr});
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
        const bool is_text = load_type_accepts_header(attr->atttypid);
        columns.push_back(idp::column_descriptor{
            .name = std::string_view{NameStr(attr->attname)},
            .is_text = is_text,
            .is_primary_key = (pk_attno != 0 && attno == pk_attno),
            .not_null = attr->attnotnull,
            .has_default = default_expressions[i] != nullptr,
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
        if (ca.atttypid == TEXTOID)
            kind = dest_kind::text;
        else if (columns[ci].is_text)
            kind = dest_kind::text_input;
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
        auto* expression = default_expressions[index];
        if (!expression) continue;
        auto* planned = bioseq::postgres::support::invoke_pg_as_cpp([expression] {
            return expression_planner(expression);
        });
        auto* state = bioseq::postgres::support::invoke_pg_as_cpp([planned] { return ExecInitExpr(planned, nullptr); });
        ctx.defaults.push_back(default_col{attr->attnum, state});
    }

    // Reuse the relation executor for defaults, constraints and ordinary indexes.
    // It owns an AFTER-trigger query frame, including deferred index rechecks.
    const bool needs_private_executor =
        !ctx.defaults.empty() || !ctx.domain_columns.empty() || ctx.parent_rel->rd_att->constr != nullptr
        || ctx.parent_rel->rd_rel->relhasindex || ctx.parent_rel->trigdesc != nullptr;
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
    bioseq::postgres::support::invoke_pg_as_cpp(
        [&ctx] { ExecOpenIndices(ctx.result_rel_info, false); });
    ctx.indexes_read_sequences = load_indexes_read_sequences(ctx);
}
