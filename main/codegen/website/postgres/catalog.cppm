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
        // Base/composite/domain/enum types that are pg_depend members of extension
        // bioseq (deptype 'e') - §12.2 forbids inferring the public surface from
        // schema/name alone - excluding the implicit array types (typcategory 'A')
        // PostgreSQL auto-creates for each of them.
        const std::string query =
            "select t.typname, obj_description(t.oid, 'pg_type') "
            "from pg_type t join pg_namespace n on n.oid = t.typnamespace "
            "join pg_depend d on d.classid = 'pg_type'::regclass and d.objid = t.oid "
            "  and d.refclassid = 'pg_extension'::regclass and d.deptype = 'e' "
            "join pg_extension e on e.oid = d.refobjid and e.extname = 'bioseq' "
            "where n.nspname = '" + schema + "' and t.typtype in ('b','c','d','e') "
            "and t.typcategory <> 'A' order by t.typname";
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

/// @brief Reads schema-scoped function entities from the live catalog.
/// @param conninfo Unix-socket conninfo for the private cluster.
/// @param include_schemas Schemas whose functions are catalogued.
/// @return Documented SQL function entities with identity arguments, in order.
[[nodiscard]] std::vector<documented_entity> read_sql_function_catalog(
    const std::string& conninfo,
    const std::vector<std::string>& include_schemas) {
    std::vector<documented_entity> entities;
    bioseq::libpq::connection connection(conninfo);
    // format_type() qualifies every type that is not on the search path, so pinning
    // the path to pg_catalog gives schema-qualified identity types for extension
    // types (bioseq.seq, public.bioseq_aligner_options) while built-ins stay bare
    // (text) - the parameter-name-free, canonically-spelled form §12.1 requires and
    // that sql_api.yaml is authored in. pg_get_function_identity_arguments keeps the
    // parameter names, so we rebuild the signature structurally instead.
    connection.exec("set search_path = pg_catalog");
    for (const std::string& schema : include_schemas) {
        // Only pg_depend members of extension bioseq (deptype 'e') are candidates
        // (§12.2); their identity argument types are canonically spelled and joined
        // without parameter names, defaults, or insignificant whitespace.
        const std::string query =
            "select p.proname, "
            "coalesce((select string_agg(format_type(t, null), ',' order by ord) "
            "          from unnest(p.proargtypes) with ordinality as u(t, ord)), '') "
            "  as identity_types, "
            "pg_get_function_result(p.oid), obj_description(p.oid, 'pg_proc') "
            "from pg_proc p join pg_namespace n on n.oid = p.pronamespace "
            "join pg_depend d on d.classid = 'pg_proc'::regclass and d.objid = p.oid "
            "  and d.refclassid = 'pg_extension'::regclass and d.deptype = 'e' "
            "join pg_extension e on e.oid = d.refobjid and e.extname = 'bioseq' "
            "where n.nspname = '" + schema + "' order by p.proname, p.oid";
        const bioseq::libpq::result result = connection.exec(query);
        for (int row = 0; row < result.rows(); ++row) {
            documented_entity entity;
            entity.domain = entity_domain::sql;
            entity.kind = entity_kind::sql_function;
            entity.display_name = std::string(result.value(row, 0));
            const std::string arguments =
                result.is_null(row, 1) ? std::string{} : std::string(result.value(row, 1));
            entity.qualified_name = schema + "." + entity.display_name;
            entity.normalized_signature =
                entity.qualified_name + "(" + arguments + ")";
            if (!result.is_null(row, 2)) {
                entity.return_type = std::string(result.value(row, 2));
            }
            entity.canonical_id = sql_canonical_id(
                schema, entity.display_name + "(" + arguments + ")", "function");
            if (!result.is_null(row, 3)) {
                entity.summary_text = std::string(result.value(row, 3));
            }
            entities.push_back(std::move(entity));
        }
    }
    return entities;
}

}  // namespace bioseq::codegen::website
