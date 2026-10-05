/* SQL functions for superusers to invalidate cache entries and reset counters. */
module;

#include <cstdint>

export module bioseq.cache.pg.sql:admin;

import bioseq.cache.pg;
import bioseq.codegen.sql.annotation_model;

export namespace bioseq::cache::pg::sql_catalog {

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache">,
    .null_input = bioseq::sql::null_input_policy::strict,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/**
 * @brief Retire all exact and reusable cache generations.
 * Requires a PostgreSQL superuser.
 * @ingroup bioseq_sql_public_api
 *
 * ```sql
 * -- As a superuser, retire all exact and reusable generations and report the count.
 * SELECT bioseq_cache.invalidate_all() AS retired_generations;
 * ```
 */
[[nodiscard]] inline std::int64_t invalidate_all() {
    return sql_backend::invalidate_all();
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache">,
    .null_input = bioseq::sql::null_input_policy::strict,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/**
 * @brief Retire cache generations that depend on the given relation.
 * Requires a PostgreSQL superuser.
 * @ingroup bioseq_sql_public_api
 *
 * ```sql
 * -- As a superuser, create a temporary relation and retire any cache generations depending on it.
 * BEGIN;
 * CREATE TEMP TABLE doc_cache_values (value integer);
 * SELECT bioseq_cache.invalidate_relation('doc_cache_values'::regclass) AS retired_generations;
 * ROLLBACK;
 * ```
 */
[[nodiscard]] inline std::int64_t invalidate_relation(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::regclass,
        })]]
        std::uint32_t relation_oid) {
    return sql_backend::invalidate_relation(relation_oid);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache">,
    .null_input = bioseq::sql::null_input_policy::strict,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/**
 * @brief Reset the shared public cache counters.
 * Requires a PostgreSQL superuser.
 * @ingroup bioseq_sql_public_api
 *
 * ```sql
 * -- As a superuser, reset shared cache counters before measuring a workload.
 * SELECT bioseq_cache.reset_stats();
 * SELECT produced, completed_hits, reuse_hits, fallbacks
 * FROM bioseq_cache.stats();
 * ```
 */
inline void reset_stats() {
    sql_backend::reset_stats();
}

}  // namespace bioseq::cache::pg::sql_catalog
