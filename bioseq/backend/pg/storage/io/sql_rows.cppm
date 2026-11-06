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
    [[bioseq_sql::column({.sql_type = bioseq::sql::pg::regclass})]]
    std::optional<Oid> reference_table;
};

/** Complete imported alignment facts, in original zero-based source coordinates.
 * CIGAR and sequence orientation describe stored values independently of FLAG.
 * Header context identifiers are local to the destination relation's IO metadata.
 */
struct [[bioseq_sql::type({.schema = bioseq::sql::fixed_schema<"bioseq">})]] alignment {
    std::optional<std::int32_t> flag;
    std::optional<std::int32_t> mapq;
    std::optional<text*> reference_name;
    std::optional<std::int64_t> reference_length;
    std::optional<std::int64_t> target_start;
    std::optional<std::int64_t> target_end;
    std::optional<std::int64_t> query_length;
    std::optional<std::int64_t> query_start;
    std::optional<std::int64_t> query_end;
    std::optional<text*> mate_reference;
    std::optional<std::int64_t> mate_start;
    std::optional<std::int64_t> template_length;
    [[bioseq_sql::column({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]]
    std::optional<Datum> cigar;
    [[bioseq_sql::column({.sql_type = bioseq::sql::pg::jsonb})]]
    std::optional<Datum> aux;
    std::optional<text*> source_orientation;
    std::optional<text*> cigar_form;
    std::optional<std::int64_t> header_context_id;
    std::optional<text*> qname;
    std::optional<std::int64_t> template_id;
    std::optional<std::int64_t> segment_id;
    std::optional<std::int64_t> reference_id;
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
