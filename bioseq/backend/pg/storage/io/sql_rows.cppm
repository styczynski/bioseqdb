/**
 * @file sql_rows.cppm
 * @brief SQL type for mapping input fields to columns during sequence loading.
 */
module;

#include <bioseq/pg_compat.h>

#include <optional>
#include <type_traits>

export module bioseq.postgres.storage.io.sql_rows;

import bioseq.codegen.sql.annotation_model;

export namespace bioseq::storage::load_pg::composite_rows {

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

}  // namespace bioseq::storage::load_pg::composite_rows
