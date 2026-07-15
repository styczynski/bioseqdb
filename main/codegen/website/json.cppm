module;

#include <boost/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

module bioseq.codegen.website:json;

import bioseq.codegen.website.model;
import bioseq.codegen.support;

// Canonical JSON emission (§5.1). Uses the Boost.JSON already built for the
// website-enabled Jinja2Cpp component for every plan/manifest/search/tooltip/
// coverage artifact. Object keys are inserted in a fixed schema order by the
// caller; this unit guarantees the terminal newline and the HTML-context
// safety post-pass for <, >, &, U+2028, and U+2029.
namespace bioseq::codegen::website {

/// @brief Escapes JSON text that may be embedded in an HTML context.
/// @param serialized Canonical JSON text.
/// @return The text with <, >, &, U+2028, and U+2029 replaced by \uXXXX.
[[nodiscard]] std::string apply_html_safe_escapes(std::string_view serialized) {
    std::string result;
    result.reserve(serialized.size());
    for (std::size_t index = 0; index < serialized.size(); ++index) {
        const unsigned char character = static_cast<unsigned char>(serialized[index]);
        if (character == '<') {
            result += "\\u003c";
        } else if (character == '>') {
            result += "\\u003e";
        } else if (character == '&') {
            result += "\\u0026";
        } else if (character == 0xE2 && index + 2 < serialized.size()
                   && static_cast<unsigned char>(serialized[index + 1]) == 0x80
                   && (static_cast<unsigned char>(serialized[index + 2]) == 0xA8
                       || static_cast<unsigned char>(serialized[index + 2]) == 0xA9)) {
            result += static_cast<unsigned char>(serialized[index + 2]) == 0xA8
                ? "\\u2028"
                : "\\u2029";
            index += 2;
        } else {
            result.push_back(serialized[index]);
        }
    }
    return result;
}

/// @brief Serializes a Boost.JSON value into canonical HTML-safe artifact text.
/// @param value Fully-built JSON value with keys already in schema order.
/// @return Canonical serialization ending in exactly one newline.
[[nodiscard]] std::string serialize_canonical(const boost::json::value& value) {
    std::string serialized = boost::json::serialize(value);
    std::string safe = apply_html_safe_escapes(serialized);
    safe.push_back('\n');
    return safe;
}

/// @brief Parses artifact JSON text, rejecting trailing garbage.
/// @param text Canonical JSON text.
/// @return The parsed value.
[[nodiscard]] boost::json::value parse_json(std::string_view text) {
    boost::json::parse_options options;
    options.allow_comments = false;
    options.allow_trailing_commas = false;
    return boost::json::parse(text, {}, options);
}

namespace {

[[nodiscard]] std::string_view node_kind_name(document_node_kind kind) {
    switch (kind) {
    case document_node_kind::document: return "document";
    case document_node_kind::paragraph: return "paragraph";
    case document_node_kind::text: return "text";
    case document_node_kind::heading: return "heading";
    case document_node_kind::emphasis: return "emphasis";
    case document_node_kind::strong_text: return "strong";
    case document_node_kind::strikethrough: return "strikethrough";
    case document_node_kind::highlight: return "highlight";
    case document_node_kind::link: return "link";
    case document_node_kind::image: return "image";
    case document_node_kind::entity_link: return "entity_link";
    case document_node_kind::code_span: return "code_span";
    case document_node_kind::code_block: return "code_block";
    case document_node_kind::admonition: return "admonition";
    case document_node_kind::block_quote: return "block_quote";
    case document_node_kind::ordered_list: return "ordered_list";
    case document_node_kind::unordered_list: return "unordered_list";
    case document_node_kind::list_item: return "list_item";
    case document_node_kind::table: return "table";
    case document_node_kind::table_row: return "table_row";
    case document_node_kind::table_cell: return "table_cell";
    case document_node_kind::footnote: return "footnote";
    case document_node_kind::line_break: return "line_break";
    case document_node_kind::code_annotation: return "code_annotation";
    }
    return "unknown";
}

[[nodiscard]] std::string_view entity_kind_name(entity_kind kind) {
    switch (kind) {
    case entity_kind::namespace_: return "namespace";
    case entity_kind::module: return "module";
    case entity_kind::class_: return "class";
    case entity_kind::struct_: return "struct";
    case entity_kind::union_: return "union";
    case entity_kind::enum_: return "enum";
    case entity_kind::enum_value: return "enum_value";
    case entity_kind::concept_: return "concept";
    case entity_kind::type_alias: return "type_alias";
    case entity_kind::function: return "function";
    case entity_kind::variable: return "variable";
    case entity_kind::sql_schema: return "sql_schema";
    case entity_kind::sql_type: return "sql_type";
    case entity_kind::sql_function: return "sql_function";
    case entity_kind::overload_group: return "overload_group";
    }
    return "unknown";
}

[[nodiscard]] std::string_view domain_name(entity_query_domain domain) {
    switch (domain) {
    case entity_query_domain::any: return "any";
    case entity_query_domain::cpp: return "cpp";
    case entity_query_domain::sql: return "sql";
    case entity_query_domain::canonical_id: return "canonical_id";
    }
    return "any";
}

[[nodiscard]] boost::json::value serialize_node(const document_node& node) {
    boost::json::object object;
    object["kind"] = std::string(node_kind_name(node.kind));
    if (!node.text.empty()) {
        object["text"] = node.text;
    }
    if (!node.target.empty()) {
        object["target"] = node.target;
    }
    if (!node.attributes.empty()) {
        boost::json::object attributes;
        for (const auto& [key, value] : node.attributes) {
            attributes[key] = value;
        }
        object["attributes"] = std::move(attributes);
    }
    if (!node.children.empty()) {
        boost::json::array children;
        for (const document_node& child : node.children) {
            children.push_back(serialize_node(child));
        }
        object["children"] = std::move(children);
    }
    return object;
}

[[nodiscard]] boost::json::value serialize_example(const sql_example& example) {
    boost::json::object object;
    object["local_id"] = example.local_id;
    object["canonical_id"] = example.canonical_id;
    object["expectation"] =
        example.expectation == sql_expectation::error ? "error" : "success";
    object["timeout_ms"] = static_cast<std::int64_t>(example.timeout.count());
    if (example.expected_sqlstate) {
        object["expected_sqlstate"] = *example.expected_sqlstate;
    }
    object["setup_sql"] = example.setup_sql;
    object["visible_sql"] = example.visible_sql;
    object["teardown_sql"] = example.teardown_sql;
    return object;
}

[[nodiscard]] boost::json::value serialize_query(const unresolved_entity_query& query) {
    boost::json::object object;
    object["domain"] = std::string(domain_name(query.domain));
    object["spelling"] = query.spelling;
    if (query.label) {
        object["label"] = *query.label;
    }
    return object;
}

[[nodiscard]] boost::json::value serialize_tutorial(const tutorial& item) {
    boost::json::object object;
    object["slug"] = item.metadata.slug;
    object["order"] = static_cast<std::int64_t>(item.metadata.order);
    object["title"] = item.metadata.title;
    object["description"] = item.metadata.description;
    boost::json::array keywords;
    for (const std::string& keyword : item.metadata.keywords) {
        keywords.push_back(boost::json::value(keyword));
    }
    object["keywords"] = std::move(keywords);
    object["draft"] = item.metadata.draft;
    object["source_path"] = item.source_path;
    object["previous_slug"] =
        item.previous_slug ? boost::json::value(*item.previous_slug) : boost::json::value(nullptr);
    object["next_slug"] =
        item.next_slug ? boost::json::value(*item.next_slug) : boost::json::value(nullptr);
    boost::json::array examples;
    for (const sql_example& example : item.sql_examples) {
        examples.push_back(serialize_example(example));
    }
    object["sql_examples"] = std::move(examples);
    boost::json::array queries;
    for (const unresolved_entity_query& query : item.entity_queries) {
        queries.push_back(serialize_query(query));
    }
    object["entity_queries"] = std::move(queries);
    boost::json::array content;
    for (const document_node& node : item.content) {
        content.push_back(serialize_node(node));
    }
    object["content"] = std::move(content);
    return object;
}

}  // namespace

/// @brief Serializes a website plan artifact canonically (schema plan/v1).
/// @param plan Fully-built plan aggregate.
/// @return Canonical HTML-safe JSON ending in one newline.
[[nodiscard]] std::string serialize_plan(const website_plan& plan) {
    boost::json::object root;
    root["schema"] = plan.schema_version;
    root["parser_version"] = plan.parser_version;
    root["binding_schema"] = plan.binding_schema_version;
    boost::json::object version;
    version["label"] = plan.version.path_component;
    boost::json::array parts;
    for (const std::uint32_t part : plan.version.parts) {
        parts.push_back(static_cast<std::int64_t>(part));
    }
    version["parts"] = std::move(parts);
    root["version"] = std::move(version);
    root["base_path"] = plan.base_path;
    root["source_revision"] = plan.source_revision;
    root["site_title"] = plan.site_title;
    boost::json::array tutorials;
    for (const tutorial& item : plan.tutorials) {
        tutorials.push_back(serialize_tutorial(item));
    }
    root["tutorials"] = std::move(tutorials);
    boost::json::array inputs;
    for (const plan_input& input : plan.inputs) {
        boost::json::object object;
        object["path"] = input.path;
        object["content_hash"] = input.content_hash;
        inputs.push_back(std::move(object));
    }
    root["inputs"] = std::move(inputs);
    root["signature"] = plan.signature;
    return serialize_canonical(root);
}

namespace {

[[nodiscard]] boost::json::value serialize_entity(const documented_entity& entity) {
    boost::json::object object;
    object["canonical_id"] = entity.canonical_id;
    object["domain"] = entity.domain == entity_domain::sql ? "sql" : "cpp";
    object["kind"] = std::string(entity_kind_name(entity.kind));
    object["qualified_name"] = entity.qualified_name;
    object["display_name"] = entity.display_name;
    object["normalized_signature"] = entity.normalized_signature;
    object["summary"] = entity.summary_text;
    object["route_path"] = entity.route_path;
    object["module"] = entity.module_name ? boost::json::value(*entity.module_name)
                                          : boost::json::value(nullptr);
    object["return_type"] = entity.return_type ? boost::json::value(*entity.return_type)
                                               : boost::json::value(nullptr);
    if (entity.declaration) {
        boost::json::object declaration;
        declaration["path"] = entity.declaration->path.generic_string();
        declaration["line"] = static_cast<std::int64_t>(entity.declaration->first_line);
        object["declaration"] = std::move(declaration);
    } else {
        object["declaration"] = boost::json::value(nullptr);
    }
    boost::json::array parameters;
    for (const parameter_documentation& parameter : entity.parameters) {
        boost::json::object item;
        item["name"] = parameter.name;
        item["type"] = parameter.type;
        item["default"] = parameter.default_value
            ? boost::json::value(*parameter.default_value) : boost::json::value(nullptr);
        parameters.push_back(std::move(item));
    }
    object["parameters"] = std::move(parameters);
    boost::json::object attributes;
    for (const auto& [key, value] : entity.attributes) {
        attributes[key] = value;
    }
    object["attributes"] = std::move(attributes);
    return object;
}

}  // namespace

/// @brief Serializes the C++ structural/documentation entity index (§11.3).
/// @param entities Canonical merged public entities.
/// @param clang_version The Clang version the structural pass was linked against.
/// @return Canonical HTML-safe JSON ending in one newline (schema cpp_entities/v1).
[[nodiscard]] std::string serialize_cpp_entities(
    const std::vector<documented_entity>& entities, std::string_view clang_version) {
    boost::json::object root;
    root["schema"] = "bioseq.website.cpp_entities/v1";
    root["clang_version"] = std::string(clang_version);
    boost::json::array items;
    for (const documented_entity& entity : entities) {
        items.push_back(serialize_entity(entity));
    }
    root["entities"] = std::move(items);
    return serialize_canonical(root);
}

/// @brief Serializes the documentation-coverage report (§11.4).
/// @param public_total Number of public entities considered.
/// @param documented_total Number with a non-empty summary.
/// @param missing_summary Public entities missing docs (non-fatal warnings).
/// @param fatal_missing Required/tutorial-referenced entities missing docs.
/// @return Canonical JSON ending in one newline (schema documentation_coverage/v1).
[[nodiscard]] std::string serialize_coverage(
    std::size_t public_total, std::size_t documented_total,
    const std::vector<std::string>& missing_summary,
    const std::vector<std::string>& fatal_missing) {
    boost::json::object root;
    root["schema"] = "bioseq.website.documentation_coverage/v1";
    root["public_total"] = static_cast<std::int64_t>(public_total);
    root["documented_total"] = static_cast<std::int64_t>(documented_total);
    boost::json::array warnings;
    for (const std::string& id : missing_summary) {
        warnings.push_back(boost::json::value(id));
    }
    root["missing_summary"] = std::move(warnings);
    boost::json::array fatal;
    for (const std::string& id : fatal_missing) {
        fatal.push_back(boost::json::value(id));
    }
    root["fatal_missing"] = std::move(fatal);
    return serialize_canonical(root);
}

}  // namespace bioseq::codegen::website
