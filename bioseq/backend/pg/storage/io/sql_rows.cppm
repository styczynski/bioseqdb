/**
 * @file sql_rows.cppm
 * @brief SQL type for mapping input fields to columns during sequence loading.
 */
module;

#include <bioseq/pg_compat.h>

#include <optional>
#include <type_traits>
#include <utility>

extern "C" {
#include "fmgr.h"
#include "utils/builtins.h"
}

export module bioseq.postgres.storage.io.sql_rows;

import bioseq.codegen.sql.annotation_model;
import bioseq.io.identifier_parser.formats;
import bioseq.postgres.support;

export namespace bioseq::storage::load_pg::composite_rows {

/// Header parser/generator formats, exposed as a native PostgreSQL enum.
using header_format [[bioseq_sql::type({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .enum_labels = bioseq::io::identifier_parser::header_format_sql_names,
})]] = bioseq::io::identifier_parser::header_format;

/// A destination column and the input field used to fill it.

/**
 * ```sql
 * -- A column mapping stores the destination column first and the parsed FASTA header field second.
 * SELECT (mapping).column_name, (mapping).data_source
 * FROM (SELECT ROW('protein_id', 'accession')::bioseq.column_mapping AS mapping) AS sample;
 * ```
 */
struct [[bioseq_sql::type({
            .schema = bioseq::sql::fixed_schema<"bioseq">,
        })]] column_mapping {
    /// Destination column name as PostgreSQL text, or SQL NULL.
    std::optional<text*> column_name;
    /// Input header fild or expression  to read, stored as PostgreSQL text or SQL NULL.
    std::optional<text*> data_source;
};

static_assert(std::is_aggregate_v<column_mapping>);

/** FASTQ quality codec; NULL uses the declared destination or phred42. */
struct [[bioseq_sql::type({.schema = bioseq::sql::fixed_schema<"bioseq">})]] load_options {
    std::optional<text*> phred_type;
};

}  // namespace bioseq::storage::load_pg::composite_rows

export namespace bioseq::storage::load_pg {

/** Decode a native SQL enum Datum; a SQL NULL selects the caller's default. */
[[nodiscard]] inline bioseq::io::identifier_parser::header_format decode_header_format(
    std::optional<Datum> value,
    bioseq::io::identifier_parser::header_format null_default) {
    if (!value) return null_default;
    char* const label = bioseq::postgres::support::invoke_pg_as_cpp([value] {
        return DatumGetCString(DirectFunctionCall1(enum_out, *value));
    });
    auto parsed = bioseq::io::identifier_parser::parse_header_format_name(label);
    pfree(label);
    if (!parsed) {
        bioseq::postgres::support::raise_sql_error(
            ERRCODE_INVALID_PARAMETER_VALUE, {.message = std::move(parsed.error())});
    }
    return *parsed;
}

}  // namespace bioseq::storage::load_pg
