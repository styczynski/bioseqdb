module;

#include <boost/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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

/// @brief Serializes the SQL catalog entity index (§12.2).
/// @param entities Documented SQL entities captured from the live catalog.
/// @return Canonical HTML-safe JSON ending in one newline (schema sql_entities/v1).
[[nodiscard]] std::string serialize_sql_entities(
    const std::vector<documented_entity>& entities) {
    boost::json::object root;
    root["schema"] = "bioseq.website.sql_entities/v1";
    boost::json::array items;
    for (const documented_entity& entity : entities) {
        items.push_back(serialize_entity(entity));
    }
    root["entities"] = std::move(items);
    return serialize_canonical(root);
}

namespace {

// HTML/attribute-escapes a plain-text value (§14 plain_text projection). Because
// Jinja2Cpp performs no automatic escaping, plain-text bindings must already carry
// HTML entities so a value containing <, >, &, or a quote cannot break out of its
// element/attribute context when the template emits it verbatim.
[[nodiscard]] std::string html_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const char character : value) {
        switch (character) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out.push_back(character); break;
        }
    }
    return out;
}

}  // namespace

/// @brief Quotes a JSON string literal safe to embed in an HTML-context artifact.
///
/// Applies only the JSON-in-HTML escaping for <, >, &, U+2028, U+2029. The decoded
/// value is emitted verbatim, so this is for trusted fragments (already-sanitized
/// HTML) and raw data artifacts — never for a plain-text template binding.
/// @param value Text to encode.
/// @return A double-quoted JSON string literal.
[[nodiscard]] std::string json_quote(std::string_view value) {
    return apply_html_safe_escapes(boost::json::serialize(boost::json::value(value)));
}

/// @brief Quotes a JSON string literal whose decoded value is HTML-safe (§14).
///
/// HTML/attribute-escapes the plain text first, so the value a template emits
/// verbatim already carries entities. Use for every untrusted plain-text/URL
/// binding (titles, descriptions, names, hrefs); use json_quote only for trusted
/// HTML fragments.
/// @param value Plain (untrusted) text.
/// @return A double-quoted JSON string literal of the HTML-escaped value.
[[nodiscard]] std::string json_text(std::string_view value) {
    return json_quote(html_escape(value));
}

namespace {

// Projects an entity onto a template view whose plain-text fields are already
// HTML-escaped (§14). Distinct from serialize_entity, which keeps raw values for
// the on-disk data artifact; this view is bound straight into a page context and
// so must be safe for verbatim template emission. route_path is a same-origin
// path built by the generator and is attribute-escaped for safety.
[[nodiscard]] boost::json::value serialize_entity_view(const documented_entity& entity) {
    boost::json::object object;
    object["canonical_id"] = entity.canonical_id;
    object["domain"] = entity.domain == entity_domain::sql ? "sql" : "cpp";
    object["kind"] = html_escape(entity_kind_name(entity.kind));
    object["qualified_name"] = html_escape(entity.qualified_name);
    object["display_name"] = html_escape(entity.display_name);
    object["normalized_signature"] = html_escape(entity.normalized_signature);
    object["summary"] = html_escape(entity.summary_text);
    object["route_path"] = html_escape(entity.route_path);
    object["return_type"] = entity.return_type
        ? boost::json::value(html_escape(*entity.return_type)) : boost::json::value(nullptr);
    boost::json::array parameters;
    for (const parameter_documentation& parameter : entity.parameters) {
        boost::json::object item;
        item["name"] = html_escape(parameter.name);
        item["type"] = html_escape(parameter.type);
        parameters.push_back(std::move(item));
    }
    object["parameters"] = std::move(parameters);
    return object;
}

}  // namespace

/// @brief Serializes one documented entity view object (HTML-escaped) for a page.
/// @param entity Canonical documented entity.
/// @return The HTML-safe JSON object text for a single-entity page context.
[[nodiscard]] std::string serialize_single_entity(const documented_entity& entity) {
    return apply_html_safe_escapes(boost::json::serialize(serialize_entity_view(entity)));
}

/// @brief Serializes a documented-entity view array (HTML-escaped) for an index.
/// @param entities Canonical documented entities.
/// @return The HTML-safe JSON array text for an entity-index page context.
[[nodiscard]] std::string serialize_entity_array(
    const std::vector<documented_entity>& entities) {
    boost::json::array items;
    for (const documented_entity& entity : entities) {
        items.push_back(serialize_entity_view(entity));
    }
    return apply_html_safe_escapes(
        boost::json::serialize(boost::json::value(std::move(items))));
}

namespace {

[[nodiscard]] entity_kind entity_kind_from_name(std::string_view name) {
    if (name == "namespace") return entity_kind::namespace_;
    if (name == "module") return entity_kind::module;
    if (name == "class") return entity_kind::class_;
    if (name == "struct") return entity_kind::struct_;
    if (name == "union") return entity_kind::union_;
    if (name == "enum") return entity_kind::enum_;
    if (name == "enum_value") return entity_kind::enum_value;
    if (name == "concept") return entity_kind::concept_;
    if (name == "type_alias") return entity_kind::type_alias;
    if (name == "function") return entity_kind::function;
    if (name == "variable") return entity_kind::variable;
    if (name == "sql_schema") return entity_kind::sql_schema;
    if (name == "sql_type") return entity_kind::sql_type;
    if (name == "sql_function") return entity_kind::sql_function;
    return entity_kind::overload_group;
}

[[nodiscard]] std::optional<std::string> as_optional_string(const boost::json::value& value) {
    if (value.is_string()) {
        return std::string(value.get_string());
    }
    return std::nullopt;
}

}  // namespace

/// @brief Parses a cpp_entities/sql_entities artifact back into the model (§11.3).
///
/// Reconstructs the scalar entity fields that reference/search pages bind to
/// (identity, kind, names, signature, parameters, declaration); the documentation
/// node forest is not needed for the current entity page shape and is skipped.
/// @param text Canonical entity-index JSON text.
/// @return The documented entities in the array's stored order.
[[nodiscard]] std::vector<documented_entity> deserialize_entities(std::string_view text) {
    const boost::json::value root = parse_json(text);
    std::vector<documented_entity> entities;
    const boost::json::array& items = root.at("entities").as_array();
    entities.reserve(items.size());
    for (const boost::json::value& item : items) {
        const boost::json::object& object = item.as_object();
        documented_entity entity;
        entity.canonical_id = std::string(object.at("canonical_id").as_string());
        entity.domain = std::string(object.at("domain").as_string()) == "sql"
            ? entity_domain::sql : entity_domain::cpp;
        entity.kind =
            entity_kind_from_name(std::string(object.at("kind").as_string()));
        entity.qualified_name = std::string(object.at("qualified_name").as_string());
        entity.display_name = std::string(object.at("display_name").as_string());
        entity.normalized_signature =
            std::string(object.at("normalized_signature").as_string());
        entity.summary_text = std::string(object.at("summary").as_string());
        entity.route_path = std::string(object.at("route_path").as_string());
        entity.module_name = as_optional_string(object.at("module"));
        entity.return_type = as_optional_string(object.at("return_type"));
        if (const boost::json::value& declaration = object.at("declaration");
            declaration.is_object()) {
            source_span span;
            span.path = std::string(declaration.at("path").as_string());
            span.first_line =
                static_cast<std::size_t>(declaration.at("line").to_number<std::int64_t>());
            entity.declaration = span;
        }
        for (const boost::json::value& parameter : object.at("parameters").as_array()) {
            const boost::json::object& fields = parameter.as_object();
            parameter_documentation documented;
            documented.name = std::string(fields.at("name").as_string());
            documented.type = std::string(fields.at("type").as_string());
            documented.default_value = as_optional_string(fields.at("default"));
            entity.parameters.push_back(std::move(documented));
        }
        if (const boost::json::value& attributes = object.at("attributes");
            attributes.is_object()) {
            for (const auto& field : attributes.as_object()) {
                entity.attributes.emplace(std::string(field.key()),
                                          std::string(field.value().as_string()));
            }
        }
        entities.push_back(std::move(entity));
    }
    return entities;
}

/// @brief Reads a top-level string field from a site manifest (or "" if absent).
/// @param text site-manifest.json text.
/// @param key Field name (for example "version" or "base_path").
/// @return The string value, or empty when missing/non-string.
[[nodiscard]] std::string manifest_string_field(
    std::string_view text, std::string_view key) {
    const boost::json::value root = parse_json(text);
    if (!root.is_object()) {
        return {};
    }
    const boost::json::object& object = root.as_object();
    const auto found = object.find(key);
    if (found == object.end() || !found->value().is_string()) {
        return {};
    }
    return std::string(found->value().as_string());
}

/// @brief Reads the owned output route set from a site manifest (§16).
/// @param text site-manifest.json text.
/// @return The manifest's declared output routes (leading-slash), in stored order.
[[nodiscard]] std::vector<std::string> manifest_output_routes(std::string_view text) {
    const boost::json::value root = parse_json(text);
    std::vector<std::string> routes;
    const boost::json::object& outputs = root.at("outputs").as_object();
    routes.reserve(outputs.size());
    for (const auto& entry : outputs) {
        routes.emplace_back(entry.key());
    }
    return routes;
}

/// @brief Serializes the immutable multi-version assembled manifest (§16).
/// @param primary_version The primary (newest) version label.
/// @param ordered_versions All version labels, newest first.
/// @param base_path Normalized base path used to build version routes.
/// @param input_manifest_hashes SHA-256 of each input site-manifest.json.
/// @param input_success_hashes SHA-256 of each input `<root>.success` marker.
/// @param outputs Every assembled output route and its SHA-256, except itself.
/// @return Canonical JSON ending in one newline (schema assembled_manifest/v1).
[[nodiscard]] std::string serialize_assembled_manifest(
    std::string_view primary_version,
    const std::vector<std::string>& ordered_versions,
    std::string_view base_path,
    const std::vector<std::string>& input_manifest_hashes,
    const std::vector<std::string>& input_success_hashes,
    const std::vector<std::pair<std::string, std::string>>& outputs) {
    boost::json::object root;
    root["schema"] = "bioseq.website.assembled_manifest/v1";
    root["hash_algorithm"] = "sha256";
    root["primary_version"] = std::string(primary_version);
    root["base_path"] = std::string(base_path);
    boost::json::array versions;
    for (const std::string& version : ordered_versions) {
        versions.push_back(boost::json::value(version));
    }
    root["versions"] = std::move(versions);
    boost::json::array manifest_hashes;
    for (const std::string& hash : input_manifest_hashes) {
        manifest_hashes.push_back(boost::json::value(hash));
    }
    root["input_manifest_sha256"] = std::move(manifest_hashes);
    boost::json::array success_hashes;
    for (const std::string& hash : input_success_hashes) {
        success_hashes.push_back(boost::json::value(hash));
    }
    root["input_success_sha256"] = std::move(success_hashes);
    boost::json::object output_map;
    for (const auto& [route, hash] : outputs) {
        output_map[route] = hash;
    }
    root["outputs"] = std::move(output_map);
    return serialize_canonical(root);
}

/// @brief Builds a full page render context from the site view and page fields.
///
/// Produces {"site":..,"nav":..,"page":..[,<collection_key>:..]} as one HTML-safe
/// JSON object. `page_object` and `collection_json` are pre-serialized JSON text
/// (built by the caller from json_quote / serialize_entity_array), so no raw
/// string is ever concatenated unescaped into the context (§14).
/// @param site Resolved site-wide fields (base-path-aware URLs and labels).
/// @param page_object JSON object text for the per-page `page` binding.
/// @param collection_key Optional extra top-level key (for example "entities").
/// @param collection_json JSON value text for that key; ignored when key empty.
/// @return The serialized page context ready for render_page.
[[nodiscard]] std::string build_page_context(
    const site_view& site,
    std::string_view page_object,
    std::string_view collection_key,
    std::string_view collection_json) {
    boost::json::object site_object;
    site_object["title"] = site.title;
    site_object["base_path"] = site.base_path;
    site_object["repository_url"] = site.repository_url;
    site_object["copyright_holder"] = site.copyright_holder;
    site_object["copyright_year"] = static_cast<std::int64_t>(site.copyright_year);
    site_object["status_label"] = site.status_label
        ? boost::json::value(*site.status_label) : boost::json::value(nullptr);
    site_object["version"] = site.version;
    site_object["css_url"] = site.css_url;
    site_object["js_url"] = site.js_url;
    site_object["favicon_url"] = site.favicon_url;
    boost::json::object nav_labels;
    nav_labels["documentation"] = site.nav_documentation;
    nav_labels["tutorials"] = site.nav_tutorials;
    nav_labels["reference"] = site.nav_reference;
    site_object["nav"] = std::move(nav_labels);

    boost::json::object nav_object;
    nav_object["documentation"] = site.documentation_url;
    nav_object["tutorials"] = site.tutorials_url;
    nav_object["reference"] = site.reference_url;

    // Synthesize the inert SEO JSON-LD (§15.5) from the page's own title/
    // description/canonical. It is a serialized-JSON string bound as page.jsonld so
    // the single application/ld+json block emits data, never executable script; no
    // datePublished/dateModified is included (no deterministic source metadata).
    boost::json::value page_value = parse_json(page_object);
    if (page_value.is_object()) {
        boost::json::object& page_object_ref = page_value.as_object();
        boost::json::object json_ld;
        json_ld["@context"] = "https://schema.org";
        json_ld["@type"] = "WebPage";
        json_ld["name"] = page_object_ref.contains("title")
            ? page_object_ref.at("title") : boost::json::value("");
        json_ld["description"] = page_object_ref.contains("description")
            ? page_object_ref.at("description") : boost::json::value("");
        json_ld["url"] = page_object_ref.contains("canonical")
            ? page_object_ref.at("canonical") : boost::json::value("");
        json_ld["isPartOf"] = site.title;
        page_object_ref["jsonld"] = boost::json::serialize(json_ld);
    }

    boost::json::object root;
    root["site"] = std::move(site_object);
    root["nav"] = std::move(nav_object);
    root["page"] = std::move(page_value);
    if (!collection_key.empty()) {
        root[std::string(collection_key)] = parse_json(collection_json);
    }
    return apply_html_safe_escapes(boost::json::serialize(root));
}

/// @brief Serializes the versions manifest consumed by the header selector (§16).
/// @param version_labels Version path components, newest first.
/// @param base_path Normalized base path used to build each version route.
/// @return Canonical HTML-safe JSON ending in one newline (schema versions/v1).
[[nodiscard]] std::string serialize_versions(
    const std::vector<std::string>& version_labels, std::string_view base_path) {
    boost::json::object root;
    root["schema"] = "bioseq.website.versions/v1";
    root["primary"] = version_labels.empty()
        ? boost::json::value(nullptr) : boost::json::value(version_labels.front());
    boost::json::array versions;
    for (const std::string& label : version_labels) {
        boost::json::object object;
        object["label"] = label;
        object["route"] = std::string(base_path) + label + "/";
        versions.push_back(std::move(object));
    }
    root["versions"] = std::move(versions);
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
