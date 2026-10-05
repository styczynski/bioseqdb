/**
 * @file bootstrap.cppm
 * @brief Declares extension schemas and installation-session settings.
 */
export module bioseq.postgres.sql.bootstrap;

import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.configuration.core;

export namespace bioseq::sql_entities {

/** @brief Disables extension diagnostic logging for installation statements.
 *
 * ```sql
 * -- Disable diagnostic logging for statements in one transaction.
 * BEGIN;
 * SET LOCAL bioseq.diagnostics.logging.enabled = false;
 * SHOW bioseq.diagnostics.logging.enabled;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::session_setting]]
inline constexpr auto logging_off = ::bioseq::sql::set_after_guard(
    ::bioseq::settings::diagnostics::logging::enabled, false);

/** Private schema for Bioseq storage and metadata objects.
 *
 * ```sql
 * -- Inspect the private catalog tables installed by Bioseq.
 * SELECT tablename
 * FROM pg_catalog.pg_tables
 * WHERE schemaname = 'bioseq_internal'
 * ORDER BY tablename;
 * ```
 */
[[bioseq_sql::schema]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
    .scope = bioseq::sql::acl_scope::all_tables_in_schema,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
inline constexpr ::bioseq::sql::schema_entity bioseq_internal{};

[[bioseq_sql::schema]]
[[bioseq_sql::grant({
    .privileges = bioseq::sql::privileges::usage,
    .to = bioseq::sql::public_role,
})]]
/** Public schema for Bioseq SQL functions and views.
 *
 * ```sql
 * -- List the public metadata views installed in the bioseq schema.
 * SELECT viewname
 * FROM pg_catalog.pg_views
 * WHERE schemaname = 'bioseq'
 * ORDER BY viewname;
 * ```
 */
inline constexpr ::bioseq::sql::schema_entity bioseq{};

static_assert(
    logging_off.setting == &::bioseq::settings::diagnostics::logging::enabled
    && !logging_off.value);

}  // namespace bioseq::sql_entities
