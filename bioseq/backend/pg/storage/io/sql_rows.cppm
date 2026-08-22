/**
 * @file sql_rows.cppm
 * @brief SQL composite-row layouts for streamed sequence loading.
 */
module;

#include <bioseq/pg_compat.h>

#include <optional>
#include <type_traits>

export module bioseq.postgres.storage.io.sql_rows;

import bioseq.codegen.sql.annotation_model;

export namespace bioseq::storage::load_pg::composite_rows {

/// User column name and input-source mapping for a streamed load.
struct [[bioseq_sql::type({
            .schema = bioseq::sql::fixed_schema<"bioseq">,
        })]] column_mapping {
    /// Destination column name as PostgreSQL text, or SQL NULL.
    std::optional<text*> column_name;
    /// Identifier-parser source expression as PostgreSQL text, or SQL NULL.
    std::optional<text*> data_source;
};

static_assert(std::is_aggregate_v<column_mapping>);

}  // namespace bioseq::storage::load_pg::composite_rows
