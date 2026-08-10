/*-------------------------------------------------------------------------
 *
 * Annotated privileged bioseq_cache administration routines.
 *
 *-------------------------------------------------------------------------
 */
module;

#include <cstdint>

export module bioseq.cache.pg.sql:admin;

import bioseq.cache.pg;
import bioseq.codegen.sql.annotation_model;

export namespace bioseq::cache::pg::sql_catalog {

/**
 * @brief Retire every exact and reusable cache generation.
 * @ingroup bioseq_sql_public_api
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache">,
    .null_input = bioseq::sql::null_input_policy::strict,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
[[nodiscard]] inline std::int64_t invalidate_all() {
    return sql_backend::invalidate_all();
}

/**
 * @brief Retire generations that depend on one PostgreSQL relation.
 * @ingroup bioseq_sql_public_api
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache">,
    .null_input = bioseq::sql::null_input_policy::strict,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
[[nodiscard]] inline std::int64_t invalidate_relation(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::regclass,
        })]]
        std::uint32_t relation_oid) {
    return sql_backend::invalidate_relation(relation_oid);
}

/**
 * @brief Reset all public cache counters.
 * @ingroup bioseq_sql_public_api
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache">,
    .null_input = bioseq::sql::null_input_policy::strict,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
inline void reset_stats() {
    sql_backend::reset_stats();
}

}  // namespace bioseq::cache::pg::sql_catalog
