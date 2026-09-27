/* SQL functions for the cache version, counters and entry diagnostics. */
module;

#include <string>

export module bioseq.cache.pg.sql:diagnostics;

import bioseq.cache.pg;
import bioseq.cache.pg.sql.rows;
import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.sql_boundary;

export namespace bioseq::cache::pg::sql_catalog {

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
/**
 * @brief Return the installed cache extension version.
 * @ingroup bioseq_sql_public_api
 */
[[nodiscard]] inline std::string version() {
    return "1.0";
}

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
/**
 * @brief Read the shared cache counters after checking diagnostic access.
 * @ingroup bioseq_sql_public_api
 */
[[nodiscard]] inline bioseq::postgres::support::table_rows<sql_rows::stats_row>
stats(bioseq::postgres::support::function_call_context call) {
    return sql_backend::stats(call);
}

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
/**
 * @brief Describe exact and reusable cache generations.
 * The caller's role determines which identifying fields and SQL text are shown.
 * @ingroup bioseq_sql_public_api
 */
[[nodiscard]] inline
bioseq::postgres::support::table_rows<sql_rows::cache_entry_row>
entries(bioseq::postgres::support::function_call_context call) {
    return sql_backend::entries(call);
}

}  // namespace bioseq::cache::pg::sql_catalog
