/**
 * @file load_runtime.cppm
 * @brief Set up sequence loading, finish writes and relese the  load resources.
 */
module;
// Include Highway and PostgreSQL headers before the named module declaration.
// Imported sequence modules also depend on these declarations.
#include <bioseq/pg_compat.h>
#include <algorithm>
#include <expected>
#include <limits>
#include <boost/container/small_vector.hpp>
#include <boost/integer.hpp>
#include <climits>
#include <condition_variable>
#include <functional>
#include <map>
#include <set>
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
#include "utils/rel.h"
#include "utils/acl.h"           // RelationGetDescr
#include "utils/resowner.h"      // CurrentResourceOwner, TopTransactionResourceOwner
#include "utils/snapmgr.h" // Snapshot stack operations.
#include "utils/typcache.h" // lookup_rowtype_tupdesc

#include "fmgr.h"
#include "rewrite/rewriteHandler.h" // build_column_default
}

export module bioseq.postgres.storage.io:load_runtime;

import bioseq.common.checked_arithmetic;
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
import :record_ingestion;
import bioseq.io.record_schema;
import bioseq.io.identifier_parser.preservation;
import bioseq.io.alignment;
import bioseq.postgres.storage.io.provenance;

/**
 * @brief Finish triggers and release the load's executor state.
 * @param[in,out] ctx Load context whose executor pointers are cleared.
 * @param[in] stable_context Context that  remains alive after executor cleanup.
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
 * @brief Release the executor, tuple slot and parent table reference for a load
 *
 * The transaction keeps the parent table's RowExclusiveLock until it ends.
 * Finish the write states separately before calling this function on success.
 *
 * @param[in,out] ctx Load resources to  release.
 * @param[in] commit Unused completion flg.
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

/** Open the target writer only after source metadata has prepared nullable destinations. */
void initialize_load_context(load_context& ctx, const char* path, bioseq::io::sequence_file_kind kind,
        idp::header_format requested_fmt, std::span<const idp::user_column_mapping> user_maps,
        const std::string& quality_name, bool with_quality, std::string_view alignment_name = {}) {
    const auto relid = ctx.main_relid;
        // Keep the parent table reference with the transaction. Restore the caller
        // resource owner before creating slots and tuple descriptors so those
        // resources are released with the caller's executor.
        const ResourceOwner saved = CurrentResourceOwner;
        ctx.parent_rel = bioseq::postgres::support::invoke_pg_as_cpp([relid, saved] {
            CurrentResourceOwner = TopTransactionResourceOwner;
            Relation result = table_open(relid, RowExclusiveLock);
            const auto acl = pg_class_aclcheck(relid, GetUserId(), ACL_INSERT);
            if (acl != ACLCHECK_OK) aclcheck_error(acl, OBJECT_TABLE, RelationGetRelationName(result));
            CurrentResourceOwner = saved;
            return result;
        });

        ctx.parent_td = RelationGetDescr(ctx.parent_rel);
        ctx.cols = resolve_load_columns(ctx.parent_rel, quality_name, with_quality, alignment_name);
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


}

/** Serialize one import manifest without merging independent reference identities. */
void append_load_manifest(load_json_sink& sink, const bioseq::io::record_source_metadata& metadata,
        std::span<const bioseq::io::alignment::manifest_wire_reference> preserved_wire = {}) {
    using bioseq::io::identifier_parser::preservation_reference_view;
    if (!preserved_wire.empty() && preserved_wire.size() != metadata.references.size())
        raise_load_error_as_cpp(ERRCODE_DATA_CORRUPTED, "manifest wire dictionary length disagrees with source dictionary");
    const auto bytes = bioseq::common::checked_mul(metadata.references.size(), sizeof(preservation_reference_view) + sizeof(std::string) + std::size_t{256});
    if (!bytes) raise_load_error_as_cpp(ERRCODE_PROGRAM_LIMIT_EXCEEDED, "reference manifest capacity overflows");
    auto admitted = bioseq::parallelism::memory_admission_controller::instance().try_reserve(*bytes);
    if (*bytes && !admitted) raise_load_error_as_cpp(ERRCODE_OUT_OF_MEMORY, "cannot admit reference manifest views");
    std::vector<preservation_reference_view> references;
    std::vector<std::string> generated_names;
    std::set<std::string_view> reserved;
    references.reserve(metadata.references.size());
    generated_names.reserve(metadata.references.size());
    for (const auto& reference : metadata.references) reserved.insert(reference.name);
    for (std::size_t index = 0; index < metadata.references.size(); ++index) {
        const auto& reference = metadata.references[index];
        bool synthetic = false;
        if (reference.alphabet_code) {
            const auto protein = bioseq::io::alignment::protein_alphabet_code(*reference.alphabet_code);
            if (!protein) raise_load_error_as_cpp(ERRCODE_DATA_EXCEPTION, protein.error());
            synthetic = *protein;
        }
        std::string_view wire_name = reference.name;
        if (!preserved_wire.empty()) wire_name = preserved_wire[index].name;
        else if (synthetic) {
            std::string name = "bioseq_protein_" + std::to_string(reference.identity);
            while (reserved.contains(name)) name.push_back('_');
            generated_names.push_back(std::move(name));
            wire_name = generated_names.back();
            reserved.insert(wire_name);
        }
        references.push_back({reference, wire_name, synthetic});
    }
    const auto result = idp::append_preservation_manifest(sink, metadata, references);
    if (!result) raise_load_error_as_cpp(ERRCODE_DATA_EXCEPTION, result.error());
}

/** Preserve complete import dictionaries in the existing per-table options catalog. */
[[nodiscard]] std::uint64_t save_load_provenance(load_context& ctx,
        const bioseq::io::record_source_metadata& metadata) {
    const auto previous = bioseq::postgres::storage::io::load_provenance(ctx.main_relid);
    std::uint64_t context_base = 0;
    std::uint64_t import_id = 1;
    load_json_sink sink;
    const auto append = [&sink](std::string_view bytes) {
        if (auto written = sink.append(bytes); !written) raise_load_error_as_cpp(ERRCODE_OUT_OF_MEMORY, written.error());
    };
    append("{\"version\":1,\"imports\":[");
    bool first = true;
    if (previous) for (const auto& imported : previous->imports) {
        if (!first) append(",");
        first = false;
        const auto count = imported.manifest.source_metadata.header_contexts.size() + 1;
        const auto next = bioseq::common::checked_add(imported.context_base, static_cast<std::uint64_t>(count));
        if (!next || *next > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
            || imported.id >= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            raise_load_error_as_cpp(ERRCODE_PROGRAM_LIMIT_EXCEEDED, "import provenance identity overflows");
        context_base = std::max(context_base, *next);
        import_id = std::max(import_id, imported.id + 1);
        append("{\"id\":" + std::to_string(imported.id) + ",\"context_base\":" + std::to_string(imported.context_base)
            + ",\"sequence_attno\":" + std::to_string(imported.sequence_attno)
            + ",\"alignment_attno\":" + std::to_string(imported.alignment_attno) + ",\"manifest\":");
        append_load_manifest(sink, imported.manifest.source_metadata, imported.manifest.wire_references);
        append("}");
    }
    const auto context_end = bioseq::common::checked_add(context_base,
        static_cast<std::uint64_t>(metadata.header_contexts.size()) + 1);
    if (!context_end || *context_end > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        raise_load_error_as_cpp(ERRCODE_PROGRAM_LIMIT_EXCEEDED, "import header context count exceeds SQL bigint");
    if (!first) append(",");
    append("{\"id\":" + std::to_string(import_id) + ",\"context_base\":" + std::to_string(context_base)
        + ",\"sequence_attno\":" + std::to_string(ctx.cols.seq_attno)
        + ",\"alignment_attno\":" + std::to_string(ctx.cols.alignment_attno) + ",\"manifest\":");
    append_load_manifest(sink, metadata);
    append("}]}");
    write_io_provenance(ctx.main_relid, sink.text);
    return context_base;
}

/** Backend-only consumer of already validated, ordered native record batches. */
struct alignment_load_consumer {
    load_context& ctx;
    const char* path;
    bioseq::io::sequence_file_kind kind;
    idp::header_format requested_format;
    std::span<const idp::user_column_mapping> mappings;
    const std::string& quality_name;
    std::optional<std::string> phred_type;
    std::optional<std::string> alignment_name;
    load_reference_lookup references;
    std::map<std::uint64_t, std::uint64_t> context_ids;
    std::uint64_t implicit_context{};

    /** Prepare exact destinations and catalog context scope before accepting records. */
    [[nodiscard]] std::expected<void, std::string> header(const bioseq::io::record_source_metadata& metadata) {
        if (metadata.declared_quality_alphabet_codes.size() > 1)
            return std::unexpected("input uses multiple original quality alphabets; load target requires one declared alphabet");
        auto quality_codec = phred_type;
        if (!metadata.declared_quality_alphabet_codes.empty()) {
            const auto original = pgseq_supported_alphabets::name_of_alphabet_code(metadata.declared_quality_alphabet_codes.front());
            if (quality_codec && *quality_codec != original && !(*quality_codec == "phred" && original == "phred42"))
                return std::unexpected("phred_type conflicts with the preserved original quality alphabet");
            quality_codec = std::string{original};
        }
        prepare_quality_destination(ctx.main_relid, quality_name, quality_codec, "phred94");
        const auto destination = prepare_alignment_destination(ctx.main_relid, alignment_name);
        initialize_load_context(ctx, path, kind, requested_format, mappings, quality_name, true, destination);
        for (const auto original : metadata.declared_alphabet_codes)
            if (original != ctx.cols.alphabet_code)
                return std::unexpected("preserved file alphabet differs from the destination column");
        implicit_context = save_load_provenance(ctx, metadata);
        for (std::size_t index = 0; index < metadata.header_contexts.size(); ++index)
            context_ids.emplace(metadata.header_contexts[index].id, implicit_context + index + 1);
        return {};
    }

    /** Store original facts; file-local context IDs become table-local import slots. */
    [[nodiscard]] std::expected<void, std::string> record(bioseq::io::decoded_record& decoded) {
        auto row = decoded.record;
        if (decoded.projected_flag) {
            const auto& sequence = decoded.sequence_descriptor(bioseq::io::record_field_role::sequence);
            const auto& quality = decoded.sequence_descriptor(bioseq::io::record_field_role::quality);
            if (sequence.alphabet_code && *sequence.alphabet_code != ctx.cols.alphabet_code)
                return std::unexpected("preserved sequence alphabet differs from the destination column");
            if (quality.alphabet_code && *quality.alphabet_code != ctx.cols.quality_alphabet_code)
                return std::unexpected("preserved quality alphabet differs from the destination column");
        }
        if (row.alignment) {
            if (row.alignment->header_context_id) {
                const auto found = context_ids.find(*row.alignment->header_context_id);
                if (found == context_ids.end()) return std::unexpected("record uses an undefined header context");
                row.alignment->header_context_id = found->second;
            } else row.alignment->header_context_id = implicit_context;
        }
        insert_loaded_record(ctx, row);
        return {};
    }

    /** Resolve only native '=' symbols through retained backend rank windows. */
    [[nodiscard]] std::expected<std::optional<bioseq::storage::borrowed_rank_window_source>, std::string>
    reference_sequence(std::string_view name) { return references.find(name); }
};

/**
 * @brief Load sequence records and finish their writes.
 * The target table keeps RowExclusiveLock until transaction end. Finish parent
 * and, fragment writes before rebuilding indexes configured for update  on insert.
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
                                    std::span<const idp::user_column_mapping> user_maps,
                                    const std::string& quality_name, std::optional<std::string> phred_type,
                                    std::optional<std::string> alignment_name,
                                    std::optional<Oid> reference_table) {
    const bool fastq = kind.format == bioseq::io::sequence_file_format::fastq;
    const bool alignment_file = kind.format == bioseq::io::sequence_file_format::sam || kind.format == bioseq::io::sequence_file_format::bam;
    const auto text_shape = alignment_file ? std::optional<idp::text_record_shape>{} : inspect_text_record_shape(path, kind.format);
    if (fastq) {
        auto codec = phred_type;
        if (text_shape && text_shape->sequences[1].alphabet_code) {
            const auto original = pgseq_supported_alphabets::name_of_alphabet_code(*text_shape->sequences[1].alphabet_code);
            if (codec && *codec != original && !(*codec == "phred" && original == "phred42"))
                raise_load_error_as_cpp(ERRCODE_DATATYPE_MISMATCH, "phred_type conflicts with preserved quality alphabet");
            codec = std::string{original};
        }
        prepare_quality_destination(relid, quality_name, codec);
    }
    load_context ctx{};
    ctx.main_relid = relid;
    const MemoryContext stable_context = CurrentMemoryContext;
    std::int64_t n = 0;
    std::exception_ptr failure;

    try {
        if (alignment_file) {
            alignment_load_consumer consumer{ctx, path, kind, requested_fmt, user_maps, quality_name,
                phred_type, alignment_name, load_reference_lookup{reference_table}};
            const auto loaded = bioseq::io::read_alignment_records(path, kind.format, consumer);
            if (!loaded) raise_load_error_as_cpp(ERRCODE_DATA_EXCEPTION, loaded.error());
            if (*loaded > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                raise_load_error_as_cpp(ERRCODE_PROGRAM_LIMIT_EXCEEDED, "loaded record count exceeds SQL bigint");
            n = static_cast<std::int64_t>(*loaded);
        } else {
            initialize_load_context(ctx, path, kind, requested_fmt, user_maps, quality_name, fastq);
            if (text_shape && text_shape->sequences[0].alphabet_code
                && *text_shape->sequences[0].alphabet_code != ctx.cols.alphabet_code)
                raise_load_error_as_cpp(ERRCODE_DATATYPE_MISMATCH, "preserved sequence alphabet differs from destination column");
            pg_identifier_writer writer{ctx, ctx.dest, ctx.defaults};
            ctx.writer_ref = idp::make_identifier_writer_reference(writer);
            n = dispatch_and_stream(ctx, writer, path, kind);
        }
        teardown_load_executor(ctx, stable_context);
        if (ctx.cols.quality_attno) inject_load_failure(4);
        flush_pending_load_writes(ctx);
        if (ctx.cols.quality_attno) inject_load_failure(5);
        MemoryContextSwitchTo(stable_context);
        finish_load_write_states(ctx);
        build_indexes_if_enabled(ctx.main_relid);
        if (ctx.cols.quality_attno) inject_load_failure(6);
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
