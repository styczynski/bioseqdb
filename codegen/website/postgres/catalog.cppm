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

namespace catalog_detail {

std::string catalog_text(const bioseq::libpq::command_result& rows,
                         std::size_t row, std::size_t column) {
    return rows.cell_is_null(row, column) ? std::string{}
        : std::string(rows.text_value(row, column));
}

void catalog_fields(bioseq::libpq::client_connection& connection,
                    reference_entity& entity, const std::string& relation_oid) {
    if (relation_oid.empty() || relation_oid == "0") return;
    const auto rows = connection.execute(
        "select a.attname, format_type(a.atttypid,a.atttypmod), "
        "col_description(a.attrelid,a.attnum), pg_get_expr(d.adbin,d.adrelid), "
        "a.attnotnull::text from pg_attribute a left join pg_attrdef d "
        "on d.adrelid=a.attrelid and d.adnum=a.attnum where a.attrelid="
        + relation_oid + " and a.attnum>0 and not a.attisdropped order by a.attnum");
    for (std::size_t row = 0; row < rows.row_count(); ++row) {
        parameter_description field{.name = catalog_text(rows,row,0),
                                    .type = catalog_text(rows,row,1)};
        const std::string comment = catalog_text(rows,row,2);
        if (!comment.empty()) field.description.push_back(content_node{
            .kind = content_node_kind::text, .text = comment});
        if (!rows.cell_is_null(row,3)) field.default_value = catalog_text(rows,row,3);
        if (catalog_text(rows,row,4) == "true") field.description.push_back(content_node{
            .kind = content_node_kind::text, .text = " NOT NULL."});
        entity.fields.push_back(std::move(field));
    }
    const auto constraints = connection.execute(
        "select conname,pg_get_constraintdef(oid) from pg_constraint where conrelid="
        + relation_oid + " order by conname");
    for (std::size_t row = 0; row < constraints.row_count(); ++row)
        entity.attributes["constraint " + catalog_text(constraints,row,0)] = catalog_text(constraints,row,1);
}

void catalog_identity(reference_entity& entity, const std::string& extension_name,
                      const std::string& schema) {
    entity.attributes["extension"] = extension_name;
    entity.attributes["schema"] = schema;
    entity.attributes["documentation_authority"] = "PostgreSQL extension catalog";
    if (schema == "bioseq_internal") entity.attributes["visibility"] = "Internal";
}

} // namespace catalog_detail

using catalog_detail::catalog_text;
using catalog_detail::catalog_fields;
using catalog_detail::catalog_identity;

/// @brief Reads every extension-owned SQL type, including relation row types.
/// Array types synthesized by PostgreSQL are represented by their element type.
[[nodiscard]] std::vector<reference_entity> load_extension_type_entities(
    const std::string& connection_info,
    const std::string& extension_name,
    const std::vector<std::string>& included_schemas) {
    std::vector<reference_entity> entities;
    bioseq::libpq::client_connection connection(connection_info);
    (void)connection.execute("set search_path = pg_catalog");
    const std::string extension_literal = connection.escape_literal(extension_name);
    for (const std::string& schema : included_schemas) {
        const auto rows = connection.execute(
            "select distinct t.typname,obj_description(t.oid,'pg_type'),t.oid::text,"
            "t.typtype::text,t.typrelid::text,format_type(t.typbasetype,t.typtypmod),"
            "t.typlen::text,t.typbyval::text,t.typinput::regproc::text,"
            "t.typoutput::regproc::text,t.typalign::text,t.typstorage::text,t.typdefault "
            "from pg_type t join pg_namespace n on n.oid=t.typnamespace "
            "join pg_depend d on ((d.classid='pg_type'::regclass and d.objid=t.oid) "
            "or (t.typrelid<>0 and d.classid='pg_class'::regclass and d.objid=t.typrelid)) "
            "and d.refclassid='pg_extension'::regclass and d.deptype='e' "
            "join pg_extension e on e.oid=d.refobjid where e.extname="
            + extension_literal + " and n.nspname=" + connection.escape_literal(schema)
            + " and t.typtype in ('b','c','d','e','r','m') and t.typcategory<>'A' order by t.typname");
        for (std::size_t row = 0; row < rows.row_count(); ++row) {
            reference_entity entity;
            entity.domain = entity_domain::sql;
            entity.kind = entity_kind::sql_type;
            entity.display_name = catalog_text(rows,row,0);
            entity.qualified_name = schema + "." + entity.display_name;
            entity.normalized_signature = entity.qualified_name;
            entity.canonical_id = make_sql_entity_id(schema,entity.display_name,"type");
            entity.summary_text = catalog_text(rows,row,1);
            if (!entity.summary_text.empty()) entity.attributes["catalog_comment"] = entity.summary_text;
            const std::string type = catalog_text(rows,row,3);
            const std::string category = type == "c" ? "Composite" : type == "d" ? "Domain"
                : type == "e" ? "Enum" : type == "r" ? "Range" : type == "m" ? "Multirange" : "Base";
            entity.attributes["type_kind"] = category;
            if (entity.summary_text.empty()) entity.summary_text = category + " SQL type " + entity.qualified_name + ".";
            catalog_identity(entity,extension_name,schema);
            for (const auto& [name,column] : std::vector<std::pair<std::string,std::size_t>>{
                    {"length",6},{"passed_by_value",7},{"input_function",8},
                    {"output_function",9},{"alignment",10},{"storage",11},{"default",12}}) {
                const auto value = catalog_text(rows,row,column);
                if (!value.empty() && value != "-") entity.attributes[name] = value;
            }
            if (type == "d") entity.attributes["base_type"] = catalog_text(rows,row,5);
            catalog_fields(connection,entity,catalog_text(rows,row,4));
            const std::string oid = catalog_text(rows,row,2);
            if (type == "e") {
                const auto values = connection.execute("select enumlabel from pg_enum where enumtypid=" + oid + " order by enumsortorder");
                for (std::size_t i = 0; i < values.row_count(); ++i) entity.enum_values.push_back(catalog_text(values,i,0));
            }
            if (type == "d") {
                const auto constraints = connection.execute("select conname,pg_get_constraintdef(oid) from pg_constraint where contypid=" + oid + " order by conname");
                for (std::size_t i = 0; i < constraints.row_count(); ++i) entity.attributes["constraint " + catalog_text(constraints,i,0)] = catalog_text(constraints,i,1);
            }
            if (type == "r") {
                const auto range = connection.execute("select format_type(rngsubtype,null),rngcanonical::regproc::text,rngsubdiff::regproc::text from pg_range where rngtypid=" + oid);
                if (range.row_count()) {
                    entity.attributes["subtype"] = catalog_text(range,0,0);
                    entity.attributes["canonical_function"] = catalog_text(range,0,1);
                    entity.attributes["subtype_difference"] = catalog_text(range,0,2);
                }
            }
            entities.push_back(std::move(entity));
        }
    }
    return entities;
}

/// @brief Reads all extension routines with exact input identities and catalog properties.
[[nodiscard]] std::vector<reference_entity> load_extension_function_entities(
    const std::string& connection_info,
    const std::string& extension_name,
    const std::vector<std::string>& included_schemas) {
    std::vector<reference_entity> entities;
    bioseq::libpq::client_connection connection(connection_info);
    const std::string extension_literal = connection.escape_literal(extension_name);
    (void)connection.execute("set search_path = pg_catalog");
    for (const std::string& schema : included_schemas) {
        const auto rows = connection.execute(
            "select p.proname,coalesce((select string_agg(format_type(t,null),',' order by ord) "
            "from unnest(p.proargtypes) with ordinality as u(t,ord)),''),"
            "pg_get_function_result(p.oid),obj_description(p.oid,'pg_proc'),"
            "p.prokind::text,pg_get_function_arguments(p.oid),l.lanname,"
            "p.provolatile::text,p.proisstrict::text,p.prosecdef::text,p.proparallel::text,p.oid::text "
            "from pg_proc p join pg_namespace n on n.oid=p.pronamespace "
            "join pg_language l on l.oid=p.prolang "
            "join pg_depend d on d.classid='pg_proc'::regclass and d.objid=p.oid "
            "and d.refclassid='pg_extension'::regclass and d.deptype='e' "
            "join pg_extension e on e.oid=d.refobjid where e.extname="
            + extension_literal + " and n.nspname=" + connection.escape_literal(schema)
            + " order by p.proname,p.oid");
        for (std::size_t row = 0; row < rows.row_count(); ++row) {
            reference_entity entity;
            entity.domain = entity_domain::sql;
            const std::string routine_kind = catalog_text(rows,row,4);
            const std::string kind = routine_kind == "a" ? "aggregate" : routine_kind == "p" ? "procedure" : "function";
            entity.kind = kind == "function" ? entity_kind::sql_function : entity_kind::sql_object;
            entity.display_name = catalog_text(rows,row,0);
            entity.qualified_name = schema + "." + entity.display_name;
            entity.normalized_signature = entity.qualified_name + "(" + catalog_text(rows,row,1) + ")";
            entity.canonical_id = "sql:" + kind + ":" + entity.normalized_signature;
            if (!rows.cell_is_null(row,2)) entity.return_type = catalog_text(rows,row,2);
            entity.summary_text = catalog_text(rows,row,3);
            if (!entity.summary_text.empty()) entity.attributes["catalog_comment"] = entity.summary_text;
            if (entity.summary_text.empty()) entity.summary_text = "SQL " + kind + " " + entity.normalized_signature
                + (entity.return_type ? " returns " + *entity.return_type : std::string{}) + ".";
            catalog_identity(entity,extension_name,schema);
            entity.attributes["sql_kind"] = kind;
            entity.attributes["arguments"] = catalog_text(rows,row,5);
            entity.attributes["language"] = catalog_text(rows,row,6);
            entity.attributes["volatility"] = catalog_text(rows,row,7) == "i" ? "immutable" : catalog_text(rows,row,7) == "s" ? "stable" : "volatile";
            entity.attributes["null_input"] = catalog_text(rows,row,8) == "true" ? "returns null on null input" : "called on null input";
            entity.attributes["security"] = catalog_text(rows,row,9) == "true" ? "definer" : "invoker";
            entity.attributes["parallel"] = catalog_text(rows,row,10) == "s" ? "safe" : catalog_text(rows,row,10) == "r" ? "restricted" : "unsafe";
            const auto args = connection.execute("select coalesce(p.proargnames[ord], '$'||ord::text),format_type(t,null) from pg_proc p,unnest(p.proargtypes) with ordinality u(t,ord) where p.oid=" + catalog_text(rows,row,11) + " order by ord");
            for (std::size_t i = 0; i < args.row_count(); ++i) entity.parameters.push_back(parameter_description{
                .name = catalog_text(args,i,0), .type = catalog_text(args,i,1)});
            entities.push_back(std::move(entity));
        }
    }
    return entities;
}

/// @brief Reads extension tables/views/sequences and their actual column types.
[[nodiscard]] std::vector<reference_entity> load_extension_relation_entities(
    const std::string& connection_info, const std::string& extension_name,
    const std::vector<std::string>& included_schemas) {
    std::vector<reference_entity> entities;
    bioseq::libpq::client_connection connection(connection_info);
    (void)connection.execute("set search_path = pg_catalog");
    for (const std::string& schema : included_schemas) {
        const auto rows = connection.execute("select distinct c.relname,c.relkind::text,c.oid::text,obj_description(c.oid,'pg_class') from pg_class c join pg_namespace n on n.oid=c.relnamespace join pg_depend d on d.classid='pg_class'::regclass and (d.objid=c.oid or (c.relkind='i' and d.objid=(select indrelid from pg_index where indexrelid=c.oid))) and d.refclassid='pg_extension'::regclass and d.deptype='e' join pg_extension e on e.oid=d.refobjid where e.extname=" + connection.escape_literal(extension_name) + " and n.nspname=" + connection.escape_literal(schema) + " and c.relkind in ('r','p','v','m','S','i') order by c.relname");
        for (std::size_t row = 0; row < rows.row_count(); ++row) {
            const auto relkind = catalog_text(rows,row,1);
            const std::string kind = relkind == "S" ? "sequence" : relkind == "i" ? "index" : relkind == "v" || relkind == "m" ? "view" : "table";
            reference_entity entity;
            entity.domain = entity_domain::sql;
            entity.kind = entity_kind::sql_object;
            entity.display_name = catalog_text(rows,row,0);
            entity.qualified_name = schema + "." + entity.display_name;
            entity.normalized_signature = entity.qualified_name;
            entity.canonical_id = "sql:" + kind + ":" + entity.qualified_name;
            entity.summary_text = catalog_text(rows,row,3);
            if (!entity.summary_text.empty()) entity.attributes["catalog_comment"] = entity.summary_text;
            if (entity.summary_text.empty()) entity.summary_text = "SQL " + kind + " " + entity.qualified_name + ".";
            catalog_identity(entity,extension_name,schema);
            entity.attributes["sql_kind"] = kind;
            catalog_fields(connection,entity,catalog_text(rows,row,2));
            if (kind == "index") {
                const auto index = connection.execute(
                    "select pg_get_indexdef(indexrelid),indrelid::regclass::text,indisunique::text "
                    "from pg_index where indexrelid=" + catalog_text(rows,row,2));
                if (index.row_count()) {
                    entity.attributes["definition"] = catalog_text(index,0,0);
                    entity.attributes["table"] = catalog_text(index,0,1);
                    entity.attributes["unique"] = catalog_text(index,0,2);
                }
            }
            entities.push_back(std::move(entity));
        }
    }
    return entities;
}

}  // namespace bioseq::codegen::website
