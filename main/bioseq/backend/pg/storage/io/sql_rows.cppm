module;

#include <bioseq/pg_compat.h>

#include <optional>
#include <type_traits>

export module bioseq.postgres.storage.io.sql_rows;

import bioseq.codegen.sql.annotation_model;

export namespace bioseq::storage::load_pg::composite_rows {

struct [[bioseq_sql::type({
            .schema = bioseq::sql::fixed_schema<"bioseq">,
        })]] column_mapping {
    std::optional<text*> column_name;
    std::optional<text*> data_source;
};

static_assert(std::is_aggregate_v<column_mapping>);

}  // namespace bioseq::storage::load_pg::composite_rows
