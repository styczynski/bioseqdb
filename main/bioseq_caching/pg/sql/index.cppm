/*-------------------------------------------------------------------------
 *
 * Annotated bioseq_cache SQL catalog.
 *
 * The extension control file provisions the fixed production schema.  This
 * module owns its ACLs and exports the typed production SQL routines; the
 * test-support profile adds the conditional test-control partition.
 *
 *-------------------------------------------------------------------------
 */
export module bioseq.cache.pg.sql;

import bioseq.codegen.sql.annotation_model;

export import bioseq.cache.pg.sql.rows;
export import :admin;
export import :diagnostics;

#if defined(BIOSEQ_CACHE_TESTING)
export import :test_controls;
#endif

export namespace bioseq::cache::pg::sql_catalog {

/** Fixed namespace provisioned by bioseq_cache.control. */
[[bioseq_sql::schema({
    .provision = bioseq::sql::schema_provision::extension_control,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::create,
    .from = bioseq::sql::public_role,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
[[bioseq_sql::grant({
    .privileges = bioseq::sql::privileges::usage,
    .to = "pg_read_all_stats",
})]]
inline constexpr bioseq::sql::schema_entity bioseq_cache{};

}  // namespace bioseq::cache::pg::sql_catalog
