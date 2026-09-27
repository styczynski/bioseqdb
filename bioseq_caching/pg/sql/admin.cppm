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
 */
inline void reset_stats() {
    sql_backend::reset_stats();
}

}  // namespace bioseq::cache::pg::sql_catalog
