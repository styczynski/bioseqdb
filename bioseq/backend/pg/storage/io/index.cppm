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
import :load_runtime;

export namespace bioseq::storage::load_pg::sql_routines {

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
/**
 * @brief Load FASTA or FASTQ records into a Bioseq table.
 * Finish fragment and parent writes before updating indexes configured for insertion.
 * @param[in] target_table Target OID. SQL NULL is rejected.
 * @param[in] file_path Input path. SQL NULL is rejected.
 * @param[in] header_format Parser name. NULL or omitted means `auto`.
 * @param[in] columns Optional header-to-column mappings.
 * @return Number of complete records inserted, as SQL BIGINT.
 * @throws postgres_error For invalid input, unsupported tables, read/write errors or index failures.
 */
[[nodiscard]] std::int64_t load([[bioseq_sql::param({
                                    .sql_type = bioseq::sql::pg::regclass,
                                })]] std::optional<Oid> target_table,
                                std::optional<std::string> file_path,
                                [[bioseq_sql::param({
                                    .default_value = bioseq::sql::checked_sql{"'auto'"},
                                })]] std::optional<std::string> header_format,
                                [[bioseq_sql::param({
                                    .sql_type = bioseq::sql::custom_type<"bioseq.column_mapping[]">,
                                    .default_value = bioseq::sql::checked_sql{"NULL"},
                                })]] std::optional<ArrayType*> columns) {
    if (!target_table.has_value() || !file_path.has_value()) {
        raise_load_error_as_cpp(ERRCODE_NULL_VALUE_NOT_ALLOWED,
                                "target_table and file_path must not be NULL");
    }
    const Oid relid = *target_table;
    const char* const path = file_path->c_str();
    const char* const fmt_arg = header_format.has_value() ? header_format->c_str() : "auto";

    // Identify FASTA or FASTQ input and report detection errors to PostgreSQL.
    auto kind_or = bioseq::io::detect_sequence_file_kind(path);
    if (!kind_or) {
        raise_load_error_as_cpp(ERRCODE_INVALID_PARAMETER_VALUE, kind_or.error());
    }
    const auto kind = *kind_or;

    // Resolve the requested header parser.
    auto fmt_or = bioseq::io::identifier_parser::parse_header_format_name(fmt_arg);
    if (!fmt_or) {
        raise_load_error_as_cpp(ERRCODE_INVALID_PARAMETER_VALUE, fmt_or.error());
    }
    const bioseq::io::identifier_parser::header_format requested_fmt = *fmt_or;

    // Read optional mappings from the SQL column_mapping array.
    std::vector<bioseq::io::identifier_parser::user_column_mapping> user_maps;
    if (columns.has_value()) {
        user_maps = parse_column_mapping_array(*columns);
    }

    // Stream the records and finish the table and index writes.
    const std::int64_t n = run_load(relid, path, kind, requested_fmt, user_maps);
    bioseq_log<logging_level::debug>(
        "[bioseq] load: '{}' -> relid={} loaded {} records", path, relid, n);
    return n;
}

} // namespace bioseq::storage::load_pg::sql_routines
