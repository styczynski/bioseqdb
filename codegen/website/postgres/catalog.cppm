module;

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.codegen.website.postgres:catalog;

import bioseq.libpq.client;
import bioseq.codegen.website.model;
import bioseq.codegen.support;

// PostgreSQL catalog access stays behind source-neutral website model types. Raw
// catalog object identifiers never cross this module boundary.
export namespace bioseq::codegen::website {

/// @brief Builds the stable website identity for a PostgreSQL catalog entity.
/// @param schema Owning schema name.
/// @param name Object name.
/// @param object_kind One of type/function/schema.
/// @return A normalized `sql:` identity.
[[nodiscard]] std::string make_sql_entity_id(
    std::string_view schema,
    std::string_view name,
    std::string_view object_kind) {
    std::string entity_id = "sql:";
    entity_id += object_kind;
    entity_id += ':';
    entity_id += schema;
    entity_id += '.';
    entity_id += name;
    return entity_id;
}

/// @brief Loads extension-owned type entities from the live PostgreSQL catalog.
/// @param connection_info Unix-socket connection parameters for the private cluster.
/// @param included_schemas Schemas whose extension objects may be returned.
/// @return SQL type entities ordered first by requested schema and then by name.
/// @throws bioseq::libpq::command_error When the catalog command fails.
[[nodiscard]] std::vector<reference_entity> load_extension_type_entities(
    const std::string& connection_info,
    const std::vector<std::string>& included_schemas) {
    std::vector<reference_entity> entities;
    bioseq::libpq::client_connection connection(connection_info);
    for (const std::string& schema : included_schemas) {
        // Extension membership, rather than schema/name coincidence, defines the
        // public surface. PostgreSQL-generated array types are excluded.
        const std::string query =
            "select t.typname, obj_description(t.oid, 'pg_type') "
            "from pg_type t join pg_namespace n on n.oid = t.typnamespace "
            "join pg_depend d on d.classid = 'pg_type'::regclass and d.objid = t.oid "
            "  and d.refclassid = 'pg_extension'::regclass and d.deptype = 'e' "
            "join pg_extension e on e.oid = d.refobjid and e.extname = 'bioseq' "
            "where n.nspname = '" + schema + "' and t.typtype in ('b','c','d','e') "
            "and t.typcategory <> 'A' order by t.typname";
        const bioseq::libpq::command_result catalog_rows =
            connection.execute(query);
        for (std::size_t row = 0; row < catalog_rows.row_count(); ++row) {
            reference_entity entity;
            entity.domain = entity_domain::sql;
            entity.kind = entity_kind::sql_type;
            entity.display_name = std::string(catalog_rows.text_value(row, 0));
            entity.qualified_name = schema + "." + entity.display_name;
            entity.canonical_id =
                make_sql_entity_id(schema, entity.display_name, "type");
            if (!catalog_rows.cell_is_null(row, 1)) {
                entity.attributes.emplace(
                    "catalog_comment",
                    std::string(catalog_rows.text_value(row, 1)));
            }
            entities.push_back(std::move(entity));
        }
    }
    return entities;
}

/// @brief Loads extension-owned function entities from the live PostgreSQL catalog.
/// @param connection_info Unix-socket connection parameters for the private cluster.
/// @param included_schemas Schemas whose extension functions may be returned.
/// @return SQL function entities with normalized identity arguments, ordered first
/// by requested schema and then by catalog identity.
/// @throws bioseq::libpq::command_error When the catalog command fails.
[[nodiscard]] std::vector<reference_entity> load_extension_function_entities(
    const std::string& connection_info,
    const std::vector<std::string>& included_schemas) {
    std::vector<reference_entity> entities;
    bioseq::libpq::client_connection connection(connection_info);
    // format_type() qualifies every type that is not on the search path, so pinning
    // the path to pg_catalog gives schema-qualified identity types for extension
    // types (bioseq.seq, public.bioseq_aligner_options) while built-ins stay bare
    // (text). The generated manifest uses this parameter-name-free normalized
    // spelling for its install-resolved identities. pg_get_function_identity_arguments
    // keeps parameter names, so we rebuild the signature structurally instead.
    (void)connection.execute("set search_path = pg_catalog");
    for (const std::string& schema : included_schemas) {
        // Only pg_depend members of extension bioseq (deptype 'e') are candidates
        // Their identity argument types are normalized and joined
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
        const bioseq::libpq::command_result catalog_rows =
            connection.execute(query);
        for (std::size_t row = 0; row < catalog_rows.row_count(); ++row) {
            reference_entity entity;
            entity.domain = entity_domain::sql;
            entity.kind = entity_kind::sql_function;
            entity.display_name = std::string(catalog_rows.text_value(row, 0));
            const std::string arguments =
                catalog_rows.cell_is_null(row, 1)
                    ? std::string{}
                    : std::string(catalog_rows.text_value(row, 1));
            entity.qualified_name = schema + "." + entity.display_name;
            entity.normalized_signature =
                entity.qualified_name + "(" + arguments + ")";
            if (!catalog_rows.cell_is_null(row, 2)) {
                entity.return_type = std::string(catalog_rows.text_value(row, 2));
            }
            entity.canonical_id = make_sql_entity_id(
                schema, entity.display_name + "(" + arguments + ")", "function");
            if (!catalog_rows.cell_is_null(row, 3)) {
                entity.attributes.emplace(
                    "catalog_comment",
                    std::string(catalog_rows.text_value(row, 3)));
            }
            entities.push_back(std::move(entity));
        }
    }
    return entities;
}

}  // namespace bioseq::codegen::website
