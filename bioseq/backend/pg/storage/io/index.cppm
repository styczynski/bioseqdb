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
        user_maps = parse_column_mapping_array(*columns);
    }

    std::optional<std::string> phred_type = options ? parse_load_options(
        bioseq::postgres::support::invoke_pg_as_cpp([options] {
            return DatumGetHeapTupleHeader(*options);
        })) : std::nullopt;
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
        } else header_maps.push_back(mapping);
    }
    if (kind.format != bioseq::io::sequence_file_format::fastq && (mapped_quality || phred_type))
        raise_load_error_as_cpp(ERRCODE_INVALID_PARAMETER_VALUE, "quality mapping and phred_type require FASTQ input");
    const std::int64_t n = run_load(relid, path, kind, requested_fmt, header_maps, quality_name, phred_type);
    bioseq_log<logging_level::debug>(
        "[bioseq] load: '{}' -> relid={} loaded {} records", path, relid, n);
    return n;
}

} // namespace bioseq::storage::load_pg::sql_routines
