module;

#include <boost/json.hpp>

#include <cstddef>
#include <cstdint>
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

}  // namespace bioseq::codegen::website
