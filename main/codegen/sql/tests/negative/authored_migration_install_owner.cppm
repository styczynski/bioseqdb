// expected-failure: BIOSEQ_SQL_E_MIGRATION_OWNER_PROVENANCE

export module bioseq.authored.install_owner;

import bioseq.codegen.sql.annotation_model;

[[bioseq_sql::schema({
    .baseline_physical_id = "P001",
    .sql_identity = "create schema authored;",
})]]
inline constexpr int authored_schema = 0;
