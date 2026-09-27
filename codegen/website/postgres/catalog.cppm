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

// Reads PostgreSQL catalog rows into the website model.
// Catalog lookups stay  here so rendering does not need a database connection.
export namespace bioseq::codegen::website {

/// @brief Builds an ID of the form sql:kind:schema.name.
/// @param schema Owning schema name.
/// @param name Object name.
/// @param object_kind Object kind, such as type, function, or schema.
/// @return The ID with the supplied names unchanged.
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
/// @param extension_name Exact owning extension name from the validated manifest.
/// @param included_schemas Schemas whose extension objects may be returned.
/// @return SQL type entities ordered first by requested schema and then by name.
/// @throws bioseq::libpq::command_error When the catalog command fails.
[[nodiscard]] std::vector<reference_entity> load_extension_type_entities(
    const std::string& connection_info,
    const std::string& extension_name,
    const std::vector<std::string>& included_schemas) {
    std::vector<reference_entity> entities;
    bioseq::libpq::client_connection connection(connection_info);
    const std::string extension_literal =
        connection.escape_literal(extension_name);
    for (const std::string& schema : included_schemas) {
        const std::string schema_literal = connection.escape_literal(schema);
        // Check extension membership so unrelated objects in the same schema
        // stay out of the reference. Exclude PostgreSQL-generated array types.
        const std::string query =
            "select t.typname, obj_description(t.oid, 'pg_type') "
            "from pg_type t join pg_namespace n on n.oid = t.typnamespace "
            "join pg_depend d on d.classid = 'pg_type'::regclass and d.objid = t.oid "
            "  and d.refclassid = 'pg_extension'::regclass and d.deptype = 'e' "
            "join pg_extension e on e.oid = d.refobjid and e.extname = "
            + extension_literal + " where n.nspname = " + schema_literal
            + " and t.typtype in ('b','c','d','e') "
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

/// @brief Loads functions owned by the requested extension.
/// @param connection_info Unix-socket, connection parameters for the private cluster.
/// @param extension_name Owning extension name from the validated manifest.
/// @param included_schemas Schemas to read, in output order
/// @return Function entities with  argument types in their identities, ordered
/// by schema order, function name, and OID.
/// @throws bioseq::libpq::command_error When the catalog command fails.
[[nodiscard]] std::vector<reference_entity> load_extension_function_entities(
    const std::string& connection_info,
    const std::string& extension_name,
    const std::vector<std::string>& included_schemas) {
    std::vector<reference_entity> entities;
    bioseq::libpq::client_connection connection(connection_info);
    const std::string extension_literal =
        connection.escape_literal(extension_name);
    // Set search_path to pg_catalog so format_type() includes the schema for
    // extension types, such as bioseq.seq. Built-in names, such as text, stay
    // unqualified. Read argument types directly because
    // pg_get_function_identity_arguments() also includes parameter names,
    // which the manifest identities ommt.
    (void)connection.execute("set search_path = pg_catalog");
    for (const std::string& schema : included_schemas) {
        const std::string schema_literal = connection.escape_literal(schema);
        // Select exact extension members through pg_depend. Build the argument
        // list from types, without parameter names or defaults, to match the manifest.
        const std::string query =
            "select p.proname, "
            "coalesce((select string_agg(format_type(t, null), ',' order by ord) "
            "          from unnest(p.proargtypes) with ordinality as u(t, ord)), '') "
            "  as identity_types, "
            "pg_get_function_result(p.oid), obj_description(p.oid, 'pg_proc') "
            "from pg_proc p join pg_namespace n on n.oid = p.pronamespace "
            "join pg_depend d on d.classid = 'pg_proc'::regclass and d.objid = p.oid "
            "  and d.refclassid = 'pg_extension'::regclass and d.deptype = 'e' "
            "join pg_extension e on e.oid = d.refobjid and e.extname = "
            + extension_literal + " where n.nspname = " + schema_literal
            + " order by p.proname, p.oid";
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
