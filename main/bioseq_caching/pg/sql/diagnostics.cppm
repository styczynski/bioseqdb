/*-------------------------------------------------------------------------
 *
 * Annotated public bioseq_cache diagnostics.
 *
 *-------------------------------------------------------------------------
 */
module;

#include <string>

export module bioseq.cache.pg.sql:diagnostics;

import bioseq.cache.pg;
import bioseq.cache.pg.sql.rows;
import bioseq.codegen.sql.annotation_model;
import bioseq.pg_sql_boundary;

export namespace bioseq::cache::pg::sql_catalog {

/** Return the installed sibling extension version. */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache">,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .null_input = bioseq::sql::null_input_policy::strict,
})]]
[[bioseq_sql::grant({
    .privileges = bioseq::sql::privileges::execute,
    .to = "pg_read_all_stats",
})]]
[[nodiscard]] inline std::string version() {
    return "1.0";
}

/** Return the process-wide cache counters visible to the calling role. */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache">,
    .parallel = bioseq::sql::parallel_safety::restricted,
    .null_input = bioseq::sql::null_input_policy::called,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
[[bioseq_sql::grant({
    .privileges = bioseq::sql::privileges::execute,
    .to = "pg_read_all_stats",
})]]
[[nodiscard]] inline bioseq::pg_utils::returns_table<sql_rows::stats_row>
stats(bioseq::pg_utils::sql_call_context call) {
    return sql_backend::stats(call);
}

/** Return exact and reusable cache generations visible to the calling role. */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache">,
    .parallel = bioseq::sql::parallel_safety::restricted,
    .null_input = bioseq::sql::null_input_policy::called,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
[[bioseq_sql::grant({
    .privileges = bioseq::sql::privileges::execute,
    .to = "pg_read_all_stats",
})]]
[[nodiscard]] inline
bioseq::pg_utils::returns_table<sql_rows::cache_entry_row>
entries(bioseq::pg_utils::sql_call_context call) {
    return sql_backend::entries(call);
}

}  // namespace bioseq::cache::pg::sql_catalog
