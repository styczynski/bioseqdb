export module bioseq.sql.bootstrap;

import bioseq.codegen.sql.annotation_model;
import bioseq.sql_guc_settings;

export namespace bioseq::sql_entities {

// Disables extension diagnostic logging for installation statements.
[[bioseq_sql::session_setting]]
inline constexpr auto logging_off = ::bioseq::sql::set_after_guard(
    ::bioseq::settings::logging, false);

// Namespaces extension-owned SQL objects.
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
inline constexpr ::bioseq::sql::schema_entity bioseq{};

static_assert(
    logging_off.setting == &::bioseq::settings::logging
    && !logging_off.value);

}  // namespace bioseq::sql_entities
