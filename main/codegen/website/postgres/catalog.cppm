module;

#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.codegen.website.postgres:catalog;

import bioseq.libpq.client;
import bioseq.codegen.website.model;
import bioseq.codegen.support;

// SQL catalog policy (§12.2). Reads the live catalog for the configured include
// schemas and projects pg_type/pg_proc rows onto documented_entity records
// without exposing OIDs to the rest of website generation. The catalog query
// set expands in Phase 7; this partition owns the schema-scoped projection.
export namespace bioseq::codegen::website {

/// @brief Builds the canonical ID for a SQL catalog entity.
/// @param schema Owning schema name.
/// @param name Object name.
/// @param object_kind One of type/function/schema.
/// @return A normalized sql: canonical identifier.
[[nodiscard]] std::string sql_canonical_id(
    std::string_view schema,
    std::string_view name,
    std::string_view object_kind) {
    std::string id = "sql:";
    id += object_kind;
    id += ':';
    id += schema;
    id += '.';
    id += name;
    return id;
}

/// @brief Reads bioseq-schema type entities from the live catalog.
/// @param conninfo Unix-socket conninfo for the private cluster.
/// @param include_schemas Schemas whose objects are catalogued.
/// @return Documented SQL type entities, in catalog order.
[[nodiscard]] std::vector<documented_entity> read_sql_type_catalog(
    const std::string& conninfo,
    const std::vector<std::string>& include_schemas) {
    std::vector<documented_entity> entities;
    bioseq::libpq::connection connection(conninfo);
    for (const std::string& schema : include_schemas) {
        const std::string query =
            "select t.typname, obj_description(t.oid, 'pg_type') "
            "from pg_type t join pg_namespace n on n.oid = t.typnamespace "
            "where n.nspname = '" + schema + "' and t.typtype in ('b','c','d') "
            "order by t.typname";
        const bioseq::libpq::result result = connection.exec(query);
        for (int row = 0; row < result.rows(); ++row) {
            documented_entity entity;
            entity.domain = entity_domain::sql;
            entity.kind = entity_kind::sql_type;
            entity.display_name = std::string(result.value(row, 0));
            entity.qualified_name = schema + "." + entity.display_name;
            entity.canonical_id = sql_canonical_id(schema, entity.display_name, "type");
            if (!result.is_null(row, 1)) {
                entity.summary_text = std::string(result.value(row, 1));
            }
            entities.push_back(std::move(entity));
        }
    }
    return entities;
}

}  // namespace bioseq::codegen::website
