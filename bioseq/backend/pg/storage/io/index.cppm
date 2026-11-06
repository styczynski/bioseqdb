/**
 * @file index.cppm
 * @brief Load FASTA and FASTQ records into PostgreSQL a sequence chunk at a time.
 */
module;
// Include Highway and PostgreSQL headers before the named module declaration.
// Imported sequence modules also depend on these declarations.
#include <bioseq/pg_compat.h>
#include <boost/container/small_vector.hpp>
#include <boost/integer.hpp>
#include <climits>
#include <algorithm>
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
#include "catalog/pg_authid_d.h"
#include "miscadmin.h"
#include "utils/acl.h"
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

export module bioseq.postgres.storage.io;

import bioseq.common.logging; // Load progress logging.
import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.support;
import bioseq.postgres.support.datums;
import bioseq.postgres.storage.io.sql_rows;
import bioseq.postgres.storage.io.column_mapping;
import bioseq.postgres.storage.io.exporting;
import bioseq.postgres.storage.io.sql_row_codecs;
import bioseq.postgres.configuration.core;
import bioseq.storage.size; // sequence_size_t
import bioseq.storage.layout;
import bioseq.common.templating.runtime_dispatch; // dispatch_exact_value_or_default
import bioseq.common.templating.value_sequence;
import bioseq.sequences;                       // Sequence alphabets and packed storage types.

import bioseq.postgres.types.compact_sequence; // PostgreSQL sequence values and supported alphabets.
import bioseq.postgres.storage.manager; // Sequence locators, fragment writers and relation caches.


import bioseq.postgres.table.io; // Column  table row writes and per-row memory contexts.
import bioseq.io.options;                      // sequence_file_format, reader_options
import bioseq.io.file_kind;                    // detect_sequence_file_kind, sequence_file_kind
import bioseq.io.reader;                       // Stream sequence chunks and, sample input headers.

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
import :load_runtime;

export namespace bioseq::storage::load_pg::sql_routines {

/** Arm one test-only import failure; zero clears it. Stages 1..6 cover the two
 * finishes, parent write, two publications, flush boundaries and index maintenance. */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
void arm_load_failure(std::int32_t stage) {
    arm_load_failure_for_test(stage);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
/**
 * @brief Load FASTA or FASTQ records into a Bioseq table
 * Finish fragment and parent writes before updating indexes configured for insertion.
 * @param[in] target_table Target OID. SQL NULL is rejected.
 * @param[in] file_path Input path. SQL NULL is rejected.
 * @param[in] header_format Native header-format enum. NULL or omitted means `auto`.
 * @param[in] columns Optional header-to-column mappings.
 * @return Number of complete records inserted, as SQL BIGINT.
 * @throws postgres_error For invalid input, unsupported tables, read/write errors or index failures.
 *
 * ```sql
 * -- Load a FASTA file into a Bioseq table. With one text column and one sequence column, auto mode places the full FASTA header in the text column. The file path is on the PostgreSQL server.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_imported (header text, seq bioseq.seq('dna4', 64)) USING bioseq;
 * SELECT bioseq.load('doc_imported'::regclass, '/data/reads.fasta');
 * SELECT header, bioseq.len(seq) FROM doc_imported;
 * ROLLBACK;
 * ```
 */
[[nodiscard]] std::int64_t load([[bioseq_sql::param({
                                    .sql_type = bioseq::sql::pg::regclass,
                                })]] std::optional<Oid> target_table,
                                std::optional<std::string> file_path,
                                [[bioseq_sql::param({
                                    .sql_type = bioseq::sql::custom_type<"bioseq.header_format">,
                                    .default_value = bioseq::sql::checked_sql{"'auto'"},
                                })]] std::optional<Datum> header_format,
                                [[bioseq_sql::param({
                                    .sql_type = bioseq::sql::custom_type<"bioseq.column_mapping[]">,
                                    .default_value = bioseq::sql::checked_sql{"NULL"},
                                })]] std::optional<ArrayType*> columns,
                                [[bioseq_sql::param({
                                    .sql_type = bioseq::sql::custom_type<"bioseq.load_options">,
                                    .default_value = bioseq::sql::checked_sql{"NULL"},
                                })]] std::optional<Datum> options) {
    if (!target_table.has_value() || !file_path.has_value()) {
        raise_load_error_as_cpp(ERRCODE_NULL_VALUE_NOT_ALLOWED,
                                "target_table and file_path must not be NULL");
    }
    const Oid relid = *target_table;
    const char* const path = file_path->c_str();
    if (file_path->find('\0') != std::string::npos)
        raise_load_error_as_cpp(ERRCODE_INVALID_PARAMETER_VALUE, "file_path contains a NUL byte");
    bioseq::postgres::support::invoke_pg_as_cpp([] {
        if (!has_privs_of_role(GetUserId(), ROLE_PG_READ_SERVER_FILES))
            ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                errmsg("bioseq.load requires privileges of pg_read_server_files")));
    });

    // Identify FASTA or FASTQ input and repor detection errors to PostgreSQL.
    auto kind_or = bioseq::io::detect_sequence_file_kind(path);
    if (!kind_or) {
        raise_load_error_as_cpp(ERRCODE_INVALID_PARAMETER_VALUE, kind_or.error());
    }
    const auto kind = *kind_or;

    const auto requested_fmt = bioseq::storage::load_pg::decode_header_format(
        header_format, bioseq::io::identifier_parser::header_format::automatic);

    // Read optional mappings from the SQL column_mapping array.
    std::vector<bioseq::io::identifier_parser::user_column_mapping> user_maps;
    if (columns.has_value()) {
        user_maps = bioseq::postgres::storage::io::parse_column_mapping_array(*columns);
    }

    const load_request_options request_options = options ? parse_load_options(
        bioseq::postgres::support::invoke_pg_as_cpp([options] {
            return DatumGetHeapTupleHeader(*options);
        })) : load_request_options{};
    const auto& phred_type = request_options.phred_type;
    std::optional<std::string> alignment_name;
    std::string quality_name = "quality";
    bool mapped_quality = false;
    std::vector<bioseq::io::identifier_parser::user_column_mapping> header_maps;
    std::vector<std::string_view> destinations;
    for (const auto& mapping : user_maps) {
        if (std::find(destinations.begin(), destinations.end(), mapping.column_name) != destinations.end())
            raise_load_error_as_cpp(ERRCODE_INVALID_PARAMETER_VALUE, "duplicate column mapping destination");
        destinations.push_back(mapping.column_name);
        if (mapping.data_source == "quality") {
            if (mapped_quality || mapping.column_name.empty())
                raise_load_error_as_cpp(ERRCODE_INVALID_PARAMETER_VALUE, "quality source must have exactly one nonempty destination");
            quality_name = mapping.column_name;
            mapped_quality = true;
        } else if (mapping.data_source == "alignment") {
            if (alignment_name || mapping.column_name.empty())
                raise_load_error_as_cpp(ERRCODE_INVALID_PARAMETER_VALUE, "alignment source requires one nonempty destination");
            alignment_name = mapping.column_name;
        } else header_maps.push_back(mapping);
    }
    const bool alignment_file = kind.format == bioseq::io::sequence_file_format::sam
        || kind.format == bioseq::io::sequence_file_format::bam;
    if (!alignment_file && kind.format != bioseq::io::sequence_file_format::fastq && (mapped_quality || phred_type))
        raise_load_error_as_cpp(ERRCODE_INVALID_PARAMETER_VALUE, "quality mapping and phred_type require FASTQ, SAM or BAM input");
    if (!alignment_file && (alignment_name || request_options.reference_table))
        raise_load_error_as_cpp(ERRCODE_INVALID_PARAMETER_VALUE, "alignment mapping and reference_table require SAM/BAM input");
    if (alignment_file) bioseq::postgres::support::invoke_pg_as_cpp([relid] {
        // Serialize the table's provenance read/modify/write before any weaker target lock.
        const auto relation = table_open(relid, ShareRowExclusiveLock);
        table_close(relation, NoLock);
    });
    if (alignment_file || kind.format == bioseq::io::sequence_file_format::fastq)
        quality_name = resolve_quality_destination_name(relid, mapped_quality ? std::optional{quality_name} : std::nullopt);
    const std::int64_t n = run_load(relid, path, kind, requested_fmt, header_maps, quality_name,
        phred_type, alignment_name, request_options.reference_table);
    bioseq_log<logging_level::debug>(
        "[bioseq] load: '{}' -> relid={} loaded {} records", path, relid, n);
    return n;
}

/** Export a relation using inferred typed fields and the shared IO custom executor. */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::volatile_,
    .parallel = bioseq::sql::parallel_safety::unsafe,
})]]
[[nodiscard]] std::int64_t write(
    [[bioseq_sql::param({.sql_type = bioseq::sql::pg::regclass})]] std::optional<Oid> source_table,
    std::optional<std::string> file_path,
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::custom_type<"bioseq.column_mapping[]">,
        .default_value = bioseq::sql::checked_sql{"NULL"},
    })]] std::optional<ArrayType*> columns,
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::pg::jsonb,
        .default_value = bioseq::sql::checked_sql{"'{}'::jsonb"},
    })]] std::optional<Datum> options) {
    if (!source_table || !file_path)
        bioseq::postgres::support::raise_sql_error(ERRCODE_NULL_VALUE_NOT_ALLOWED,
            {.message = "source_table and file_path must not be NULL"});
    return bioseq::postgres::storage::io::exporting::execute_export_request({
        .relation = source_table, .destination = std::move(*file_path),
        .mappings = columns, .options = options,
        .function_oid = bioseq::postgres::storage::io::exporting::export_function_oid(true),
    });
}

/** Export a SELECT once; direct helper lineage survives nested identity projections. */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::volatile_,
    .parallel = bioseq::sql::parallel_safety::unsafe,
})]]
[[nodiscard]] std::int64_t write(
    std::optional<std::string> query,
    std::optional<std::string> file_path,
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::custom_type<"bioseq.column_mapping[]">,
        .default_value = bioseq::sql::checked_sql{"NULL"},
    })]] std::optional<ArrayType*> columns,
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::pg::jsonb,
        .default_value = bioseq::sql::checked_sql{"'{}'::jsonb"},
    })]] std::optional<Datum> options) {
    if (!query || !file_path)
        bioseq::postgres::support::raise_sql_error(ERRCODE_NULL_VALUE_NOT_ALLOWED,
            {.message = "query and file_path must not be NULL"});
    return bioseq::postgres::storage::io::exporting::execute_export_request({
        .query = std::move(*query), .destination = std::move(*file_path),
        .mappings = columns, .options = options,
        .function_oid = bioseq::postgres::storage::io::exporting::export_function_oid(false),
    });
}

} // namespace bioseq::storage::load_pg::sql_routines
