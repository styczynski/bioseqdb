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
 *
 * ```sql
 * -- Read the version reported by the installed cache extension.
 * SELECT bioseq_cache.version();
 * ```
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
 *
 * @remark website.field produced: Number of cache generations successfully published since counters were reset.
 * @remark website.field completed_hits: Number of hits on completed exact-result generations.
 * @remark website.field inflight_waits: Number of waits for a cache producer that was still running.
 * @remark website.field reuse_hits: Number of reusable-coverage cache hits.
 * @remark website.field fallbacks: Number of cache fallbacks to ordinary execution.
 * @remark website.field invalidations: Number of cache invalidations recorded since counters were reset.
 * @remark website.field evictions: Number of cache evictions recorded since counters were reset.
 * @remark website.field logical_bytes: Cumulative logical payload bytes recorded by cache producers.
 * @remark website.field stored_bytes: Cumulative stored payload bytes recorded by cache producers.
 *
 * ```sql
 * -- A monitoring role can inspect producer, hit, and fallback counters.
 * SELECT produced, completed_hits, reuse_hits, fallbacks
 * FROM bioseq_cache.stats();
 * ```
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
 * @brief Describe  exact and reusable cache generations.
 * The caller's role determines which identifying fields and SQL text are shown.
 * @ingroup bioseq_sql_public_api
 *
 * @remark website.field entry_key: Printable key identifying the cached result or reusable coverage.
 * @remark website.field generation: Generation number associated with the cache key.
 * @remark website.field kind: Cache entry kind: ExactFinalResult or ReusableCoverage.
 * @remark website.field state: Current lifecycle state of the cache generation.
 * @remark website.field database_oid: Database containing the cached work, or NULL when unavailable.
 * @remark website.field owner_role: Role that owns the generation; hidden when the caller lacks full disclosure.
 * @remark website.field producer_pid: Producer backend PID; hidden when unavailable or disclosure is restricted.
 * @remark website.field created_at: Time the generation started, or NULL when unavailable.
 * @remark website.field completed_at: Time production completed, or NULL while incomplete or unavailable.
 * @remark website.field age: Elapsed time since the generation started.
 * @remark website.field ttl: Configured lifetime of the generation after completion.
 * @remark website.field rows: Number of rows stored in the generation.
 * @remark website.field logical_bytes: Logical uncompressed payload size of the generation, in bytes.
 * @remark website.field stored_bytes: Physical payload size recorded for the generation, in bytes.
 * @remark website.field waiters: Number of callers waiting for the generation.
 * @remark website.field readers: Number of active reader pins on the generation.
 * @remark website.field hits: Recorded exact-result hit count; reusable-coverage rows currently report 0.
 * @remark website.field sql: Producer SQL text, or NULL when unavailable or disclosure is restricted.
 * @remark website.field producer_plan: Producer plan text, or NULL when unavailable or disclosure is restricted.
 * @remark website.field dependencies: JSONB dependency information, or NULL when disclosure is restricted.
 *
 * ```sql
 * -- A monitoring role can inspect cache generations ordered by stored payload size.
 * SELECT kind, state, generation, rows, stored_bytes
 * FROM bioseq_cache.entries()
 * ORDER BY stored_bytes DESC;
 * ```
 */
[[nodiscard]] inline
bioseq::postgres::support::table_rows<sql_rows::cache_entry_row>
entries(bioseq::postgres::support::function_call_context call) {
    return sql_backend::entries(call);
}

}  // namespace bioseq::cache::pg::sql_catalog
