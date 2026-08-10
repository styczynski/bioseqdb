module;

#include <boost/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module bioseq.codegen.website:serialization;

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
[[nodiscard]] boost::json::value parse_json_value(std::string_view text) {
    boost::json::parse_options options;
    options.allow_comments = false;
    options.allow_trailing_commas = false;
    return boost::json::parse(text, {}, options);
}

namespace {

void require_exact_members(
    const boost::json::object& object,
    std::initializer_list<std::string_view> expected_members,
    std::string_view context) {
    if (object.size() != expected_members.size()) {
        throw std::runtime_error(
            std::string(context) + ": unexpected top-level member set");
    }
    for (const std::string_view member : expected_members) {
        if (object.if_contains(member) == nullptr) {
            throw std::runtime_error(
                std::string(context) + ": missing top-level member '"
                + std::string(member) + "'");
        }
    }
}

void require_known_generated_sql_manifest_members(
    const boost::json::object& object,
    std::string_view context) {
    static const std::set<std::string, std::less<>> members{
        "schema_revision",
        "authority_mode",
        "compiler_profile",
        "clang_version",
        "compilation_signature",
        "extension_name",
        "extension_schema",
        "generated_module_prefix",
        "fmgr_boundary_module",
        "script_sha256",
        "statement_count",
        "statements",
        "entity_count",
        "public_entity_count",
        "public_stable_identities",
        "public_installed_identities",
        "routine_entity_count",
        "c_routine_identity_count",
        "c_link_symbol_inventory_count",
        "c_finfo_symbol_inventory_count",
        "c_wrapper_entity_count",
        "generated_wrapper_symbol_count",
        "handwritten_fmgr_export_symbol_count",
        "handwritten_fmgr_export_external_symbol_count",
        "handwritten_fmgr_export_owner_count",
        "c_symbol_sharing_group_count",
        "generated_cpp_profile_invariant",
        "entity_handle_count",
        "entity_handle_module_count",
        "entity_handle_modules",
        "row_codec_count",
        "row_codec_module_count",
        "row_codec_modules",
        "routine_boundary_contract_count",
        "routine_boundary_contracts",
        "result_shape_contract_count",
        "result_shape_contracts",
        "row_codec_contract_count",
        "row_codec_contracts",
        "c_routine_identities",
        "c_link_symbols",
        "c_finfo_symbols",
        "generated_wrapper_symbols",
        "handwritten_fmgr_export_symbols",
        "handwritten_fmgr_export_external_symbols",
        "fmgr_entrypoint_module_count",
        "fmgr_entrypoint_modules",
        "handwritten_fmgr_export_owners",
        "c_symbol_owners",
        "entity_counts_by_kind",
        "entities",
        "guc_generation",
    };
    for (const auto& member : object) {
        const auto raw = member.key();
        const std::string key{raw.data(), raw.size()};
        if (!members.contains(key)) {
            throw std::runtime_error(
                std::string(context) + ": unknown top-level member '"
                + key + "'");
        }
    }
}

void require_current_schema(
    const boost::json::object& object,
    std::string_view expected_schema,
    std::uint32_t expected_revision,
    std::string_view context) {
    const boost::json::value* const schema = object.if_contains("schema");
    const boost::json::value* const revision =
        object.if_contains("schema_revision");
    const bool revision_matches = revision != nullptr
        && ((revision->is_int64()
             && revision->as_int64()
                    == static_cast<std::int64_t>(expected_revision))
            || (revision->is_uint64()
                && revision->as_uint64()
                       == static_cast<std::uint64_t>(expected_revision)));
    if (schema == nullptr || !schema->is_string()
        || std::string_view{
               schema->as_string().data(), schema->as_string().size()}
               != expected_schema
        || !revision_matches) {
        throw std::runtime_error(
            std::string(context) + ": expected current schema '"
            + std::string(expected_schema) + "' revision "
            + std::to_string(expected_revision));
    }
}

[[nodiscard]] std::uint32_t require_current_revision_member(
    const boost::json::object& object,
    std::string_view member,
    std::uint32_t expected_revision,
    std::string_view context) {
    const boost::json::value* const revision = object.if_contains(member);
    const bool revision_matches = revision != nullptr
        && ((revision->is_int64()
             && revision->as_int64()
                    == static_cast<std::int64_t>(expected_revision))
            || (revision->is_uint64()
                && revision->as_uint64()
                       == static_cast<std::uint64_t>(expected_revision)));
    if (!revision_matches) {
        throw std::runtime_error(
            std::string(context) + ": expected current "
            + std::string(member) + " "
            + std::to_string(expected_revision));
    }
    return expected_revision;
}

void require_processing_contract(
    std::string_view schema,
    std::uint32_t schema_revision,
    std::string_view expected_schema,
    std::string_view parser_identity,
    std::uint32_t parser_revision,
    std::string_view binding_schema,
    std::uint32_t binding_schema_revision,
    std::string_view context) {
    if (schema != expected_schema || schema_revision != 1
        || parser_identity != "bioseq.website.parser"
        || parser_revision != 1
        || binding_schema != "bioseq.website.bindings"
        || binding_schema_revision != 1) {
        throw std::runtime_error(
            std::string(context) + ": non-current processing contract");
    }
}

void require_manifest_contract(
    const boost::json::object& object,
    std::string_view expected_schema,
    std::uint32_t expected_revision) {
    require_current_schema(
        object, expected_schema, expected_revision, "website manifest");
    if (expected_schema == "bioseq.website.site_manifest") {
        require_exact_members(
            object,
            {"schema",
             "schema_revision",
             "hash_algorithm",
             "version",
             "source_revision",
             "base_path",
             "generator_signature",
             "parser_identity",
             "parser_revision",
             "binding_schema",
             "binding_schema_revision",
             "asset_bundle_sha256",
             "postgres_server_version",
             "extension_version",
             "staged_extension_sha256",
             "inputs",
             "outputs"},
            "site manifest");
        require_processing_contract(
            std::string(object.at("schema").as_string()),
            expected_revision,
            expected_schema,
            std::string(object.at("parser_identity").as_string()),
            require_current_revision_member(
                object, "parser_revision", 1, "site manifest"),
            std::string(object.at("binding_schema").as_string()),
            require_current_revision_member(
                object, "binding_schema_revision", 1, "site manifest"),
            "site manifest");
    } else if (expected_schema == "bioseq.website.prepublish") {
        require_exact_members(
            object,
            {"schema",
             "schema_revision",
             "operation",
             "nonce",
             "staging",
             "new_manifest_sha256",
             "validation_stamps"},
            "prepublish descriptor");
    }
}

}  // namespace

void require_artifact_schema(
    std::string_view text,
    std::string_view expected_schema,
    std::uint32_t expected_revision,
    std::string_view context) {
    const boost::json::value document = parse_json_value(text);
    const boost::json::object& object = document.as_object();
    require_current_schema(object, expected_schema, expected_revision, context);
    if (expected_schema == "bioseq.website.owner") {
        require_exact_members(
            object,
            {"schema", "schema_revision", "nonce", "staging"},
            context);
    }
}

/// @brief One website-public SQL entity projected from the generated SQL
/// manifest.
///
/// `stable_identity` remains relocation-stable for provenance and cross-schema
/// tests. `installed_identity` is the exact live-catalog/page identity for the
/// selected extension schema.
struct sql_manifest_public_entity {
    std::string stable_identity;
    std::string installed_identity;
    std::string kind;
    std::string name;
    std::string source_module;
    std::string source_path;
    std::size_t source_line{};
    std::string brief;
    std::string details;
    std::string result_sql_type;
    std::vector<parameter_description> parameters;
};

/// @brief Website-facing projection of the generated SQL manifest.
struct sql_manifest_public_projection {
    std::string extension_name;
    std::string extension_schema;
    std::string script_sha256;
    std::vector<std::string> stable_identities;
    std::vector<std::string> installed_identities;
    std::vector<sql_manifest_public_entity> entities;
    std::vector<std::string> catalog_schemas;
    std::vector<std::string> restricted_sequence_names;
};

/// @brief Reads the website-public set from a generated SQL manifest.
///
/// Visibility comes only from `public_documentation`. Transitional install
/// owners are rejected if they claim visibility: only natural annotated
/// declarations may publish SQL documentation.
[[nodiscard]] sql_manifest_public_projection
parse_sql_manifest_public_projection(
        std::string_view text,
        std::string_view path) {
    const auto fail = [path](std::string_view detail) -> void {
        throw std::runtime_error(
            "invalid SQL manifest " + std::string(path) + ": "
            + std::string(detail));
    };
    const auto require_member =
        [&fail](const boost::json::object& object,
                std::string_view key) -> const boost::json::value& {
            const boost::json::value* value = object.if_contains(key);
            if (value == nullptr) {
                fail("missing field '" + std::string(key) + "'");
            }
            return *value;
        };
    const auto require_string =
        [&fail, &require_member](const boost::json::object& object,
                                std::string_view key) -> std::string {
            const boost::json::value& value = require_member(object, key);
            if (!value.is_string()) {
                fail("field '" + std::string(key) + "' must be a string");
            }
            const boost::json::string& raw = value.as_string();
            return std::string(raw.data(), raw.size());
        };
    const auto require_bool =
        [&fail, &require_member](const boost::json::object& object,
                                std::string_view key) -> bool {
            const boost::json::value& value = require_member(object, key);
            if (!value.is_bool()) {
                fail("field '" + std::string(key) + "' must be a boolean");
            }
            return value.as_bool();
        };
    const auto require_size =
        [&fail, &require_member](const boost::json::object& object,
                                std::string_view key) -> std::size_t {
            const boost::json::value& value = require_member(object, key);
            if (!value.is_int64() || value.as_int64() <= 0) {
                fail("field '" + std::string(key)
                     + "' must be a positive integer");
            }
            return static_cast<std::size_t>(value.as_int64());
        };

    const boost::json::value document = parse_json_value(text);
    if (!document.is_object()) {
        fail("root must be an object");
    }
    const boost::json::object& root = document.as_object();
    require_known_generated_sql_manifest_members(root, "SQL manifest");
    require_current_revision_member(
        root, "schema_revision", 1, "SQL manifest");
    const std::string authority_mode =
        require_string(root, "authority_mode");
    if (authority_mode != "annotated_cpp") {
        fail("authority_mode must be 'annotated_cpp'");
    }
    const std::string extension_name =
        require_string(root, "extension_name");
    if (extension_name.empty()) {
        fail("extension_name must not be empty");
    }
    const std::string extension_schema =
        require_string(root, "extension_schema");
    if (extension_schema.empty()) {
        fail("extension_schema must not be empty");
    }
    const std::string script_sha256 =
        require_string(root, "script_sha256");
    if (script_sha256.size() != 64) {
        fail("script_sha256 must contain 64 hexadecimal characters");
    }
    for (const char value : script_sha256) {
        const bool hexadecimal =
            (value >= '0' && value <= '9')
            || (value >= 'a' && value <= 'f');
        if (!hexadecimal) {
            fail("script_sha256 must be lowercase hexadecimal");
        }
    }
    const boost::json::value& entities_value =
        require_member(root, "entities");
    if (!entities_value.is_array()) {
        fail("entities must be an array");
    }

    sql_manifest_public_projection projection{
        .extension_name = extension_name,
        .extension_schema = extension_schema,
        .script_sha256 = script_sha256,
    };
    std::set<std::string, std::less<>> stable_identities;
    std::set<std::string, std::less<>> installed_identities;
    std::set<std::string, std::less<>> catalog_schemas;
    std::set<std::string, std::less<>> restricted_sequence_names;
    const auto resolve_extension_identity =
        [&extension_schema](std::string identity) {
            constexpr std::string_view marker = "$extension";
            std::size_t position = 0;
            while ((position = identity.find(marker, position))
                   != std::string::npos) {
                identity.replace(position, marker.size(), extension_schema);
                position += extension_schema.size();
            }
            return identity;
        };
    const auto identity_body =
        [&fail](std::string_view identity,
                std::string_view expected_kind) -> std::string_view {
            const std::string prefix =
                "sql:" + std::string(expected_kind) + ":";
            if (!identity.starts_with(prefix)) {
                fail("identity '" + std::string(identity)
                     + "' does not match kind '" + std::string(expected_kind)
                     + "'");
            }
            return identity.substr(prefix.size());
        };
    const auto qualified_name =
        [](std::string_view body) -> std::string_view {
            const std::size_t signature = body.find('(');
            return body.substr(0, signature);
        };
    const auto schema_name =
        [&fail](std::string_view name) -> std::string_view {
            const std::size_t dot = name.find('.');
            if (dot == std::string_view::npos || dot == 0) {
                fail("installed identity has no qualified schema: "
                     + std::string(name));
            }
            return name.substr(0, dot);
        };
    for (const boost::json::value& value : entities_value.as_array()) {
        if (!value.is_object()) {
            fail("every entities entry must be an object");
        }
        const boost::json::object& entity = value.as_object();
        std::string stable_identity = require_string(entity, "identity");
        std::string installed_identity =
            require_string(entity, "installed_identity");
        if (stable_identity.empty() || installed_identity.empty()) {
            fail("entity identities must not be empty");
        }
        if (!stable_identities.insert(stable_identity).second) {
            fail("duplicate stable identity '" + stable_identity + "'");
        }
        if (!installed_identities.insert(installed_identity).second) {
            fail("duplicate installed identity '" + installed_identity + "'");
        }
        if (resolve_extension_identity(stable_identity)
            != installed_identity) {
            fail("installed identity '" + installed_identity
                 + "' is not the extension-schema resolution of '"
                 + stable_identity + "'");
        }

        const bool public_documentation =
            require_bool(entity, "public_documentation");
        const std::string kind = require_string(entity, "kind");
        const std::string_view installed_body =
            identity_body(installed_identity, kind);
        const std::string_view installed_name =
            qualified_name(installed_body);
        if (kind == "sequence"
            && schema_name(installed_name) == "bioseq_internal") {
            restricted_sequence_names.emplace(installed_name);
        }
        if (!public_documentation) {
            continue;
        }
        std::string brief = require_string(entity, "brief");
        if (brief.empty()) {
            fail("website-public entity '" + stable_identity
                 + "' has no Doxygen brief");
        }
        if (kind != "type" && kind != "function"
            && kind != "sql_function") {
            fail("website-public entity '" + stable_identity
                 + "' has unsupported website kind '" + kind + "'");
        }
        catalog_schemas.emplace(schema_name(installed_name));
        std::vector<parameter_description> parameters;
        const boost::json::value& parameters_value =
            require_member(entity, "parameters");
        if (!parameters_value.is_array()) {
            fail("parameters must be an array for '" + stable_identity + "'");
        }
        for (const boost::json::value& parameter_value :
             parameters_value.as_array()) {
            if (!parameter_value.is_object()) {
                fail("parameter entries must be objects for '"
                     + stable_identity + "'");
            }
            const boost::json::object& parameter =
                parameter_value.as_object();
            parameter_description documentation{
                .name = require_string(parameter, "name"),
                .type = require_string(parameter, "sql_type"),
            };
            (void)require_bool(parameter, "has_default");
            parameters.push_back(std::move(documentation));
        }
        projection.entities.push_back(sql_manifest_public_entity{
            .stable_identity = std::move(stable_identity),
            .installed_identity = std::move(installed_identity),
            .kind = kind,
            .name = require_string(entity, "name"),
            .source_module = require_string(entity, "source_module"),
            .source_path = require_string(entity, "source_path"),
            .source_line = require_size(entity, "source_line"),
            .brief = std::move(brief),
            .details = require_string(entity, "details"),
            .result_sql_type = require_string(entity, "result_sql_type"),
            .parameters = std::move(parameters),
        });
    }
    if (projection.entities.empty()) {
        fail("no entity is marked public_documentation");
    }
    projection.catalog_schemas.assign(
        catalog_schemas.begin(), catalog_schemas.end());
    projection.stable_identities.assign(
        stable_identities.begin(), stable_identities.end());
    projection.installed_identities.assign(
        installed_identities.begin(), installed_identities.end());
    projection.restricted_sequence_names.assign(
        restricted_sequence_names.begin(),
        restricted_sequence_names.end());
    return projection;
}

namespace {

[[nodiscard]] std::string_view node_kind_name(content_node_kind kind) {
    switch (kind) {
    case content_node_kind::root: return "document";
    case content_node_kind::paragraph: return "paragraph";
    case content_node_kind::text: return "text";
    case content_node_kind::heading: return "heading";
    case content_node_kind::emphasis: return "emphasis";
    case content_node_kind::strong_text: return "strong";
    case content_node_kind::strikethrough: return "strikethrough";
    case content_node_kind::highlight: return "highlight";
    case content_node_kind::link: return "link";
    case content_node_kind::image: return "image";
    case content_node_kind::entity_link: return "entity_link";
    case content_node_kind::code_span: return "code_span";
    case content_node_kind::code_block: return "code_block";
    case content_node_kind::admonition: return "admonition";
    case content_node_kind::block_quote: return "block_quote";
    case content_node_kind::ordered_list: return "ordered_list";
    case content_node_kind::unordered_list: return "unordered_list";
    case content_node_kind::list_item: return "list_item";
    case content_node_kind::table: return "table";
    case content_node_kind::table_row: return "table_row";
    case content_node_kind::table_cell: return "table_cell";
    case content_node_kind::footnote: return "footnote";
    case content_node_kind::line_break: return "line_break";
    case content_node_kind::code_annotation: return "code_annotation";
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

[[nodiscard]] boost::json::value serialize_node(const content_node& node) {
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
        for (const content_node& child : node.children) {
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
    for (const content_node& node : item.content) {
        content.push_back(serialize_node(node));
    }
    object["content"] = std::move(content);
    return object;
}

}  // namespace

/// @brief Serializes the current website plan artifact canonically.
/// @param plan Fully-built plan aggregate.
/// @return Canonical HTML-safe JSON ending in one newline.
[[nodiscard]] std::string serialize_site_plan(const site_plan& plan) {
    require_processing_contract(
        plan.schema,
        plan.schema_revision,
        "bioseq.website.plan",
        plan.parser_identity,
        plan.parser_revision,
        plan.binding_schema,
        plan.binding_schema_revision,
        "site plan");
    boost::json::object root;
    root["schema"] = plan.schema;
    root["schema_revision"] = plan.schema_revision;
    root["parser_identity"] = plan.parser_identity;
    root["parser_revision"] = plan.parser_revision;
    root["binding_schema"] = plan.binding_schema;
    root["binding_schema_revision"] = plan.binding_schema_revision;
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
    for (const source_input& input : plan.inputs) {
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

[[nodiscard]] boost::json::value serialize_entity(const reference_entity& entity) {
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
    boost::json::array documentation;
    for (const content_node& node : entity.detail_content) {
        documentation.push_back(serialize_node(node));
    }
    object["documentation"] = std::move(documentation);
    if (entity.declaration) {
        boost::json::object declaration;
        declaration["path"] = entity.declaration->path.generic_string();
        declaration["line"] = static_cast<std::int64_t>(entity.declaration->first_line);
        object["declaration"] = std::move(declaration);
    } else {
        object["declaration"] = boost::json::value(nullptr);
    }
    boost::json::array parameters;
    for (const parameter_description& parameter : entity.parameters) {
        boost::json::object item;
        item["name"] = parameter.name;
        item["type"] = parameter.type;
        item["default"] = parameter.default_value
            ? boost::json::value(*parameter.default_value) : boost::json::value(nullptr);
        boost::json::array description;
        for (const content_node& node : parameter.description) {
            description.push_back(serialize_node(node));
        }
        item["description"] = std::move(description);
        parameters.push_back(std::move(item));
    }
    object["parameters"] = std::move(parameters);
    object["implementation_entity_id"] = entity.implementation_entity_id
        ? boost::json::value(*entity.implementation_entity_id)
        : boost::json::value(nullptr);
    boost::json::array overload_ids;
    for (const std::string& id : entity.overload_ids) {
        overload_ids.push_back(boost::json::value(id));
    }
    object["overload_ids"] = std::move(overload_ids);
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
/// @return Canonical HTML-safe JSON ending in one newline.
[[nodiscard]] std::string serialize_cpp_reference_entities(
    const std::vector<reference_entity>& entities, std::string_view clang_version) {
    boost::json::object root;
    root["schema"] = "bioseq.website.cpp_entities";
    root["schema_revision"] = 1;
    root["clang_version"] = std::string(clang_version);
    boost::json::array items;
    for (const reference_entity& entity : entities) {
        items.push_back(serialize_entity(entity));
    }
    root["entities"] = std::move(items);
    return serialize_canonical(root);
}

/// @brief Serializes the SQL catalog entity index (§12.2).
/// @param entities Documented SQL entities captured from the live catalog.
/// @return Canonical HTML-safe JSON ending in one newline.
[[nodiscard]] std::string serialize_sql_entities(
    const std::vector<reference_entity>& entities) {
    boost::json::object root;
    root["schema"] = "bioseq.website.sql_entities";
    root["schema_revision"] = 1;
    boost::json::array items;
    for (const reference_entity& entity : entities) {
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

[[nodiscard]] std::string content_plain_text(const content_node& node) {
    std::string result = node.text;
    for (const content_node& child : node.children) {
        result += content_plain_text(child);
    }
    return result;
}

[[nodiscard]] std::string content_plain_text(
    const std::vector<content_node>& nodes) {
    std::string result;
    for (const content_node& node : nodes) {
        const std::string fragment = content_plain_text(node);
        if (!result.empty() && !fragment.empty()) {
            result.push_back(' ');
        }
        result += fragment;
    }
    return result;
}

// Projects an entity onto a template view whose plain-text fields are already
// HTML-escaped (§14). Distinct from serialize_entity, which keeps raw values for
// the on-disk data artifact; this view is bound straight into a page context and
// so must be safe for verbatim template emission. route_path is a same-origin
// path built by the generator and is attribute-escaped for safety.
[[nodiscard]] boost::json::value serialize_entity_view(const reference_entity& entity) {
    boost::json::object object;
    object["canonical_id"] = html_escape(entity.canonical_id);
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
    for (const parameter_description& parameter : entity.parameters) {
        boost::json::object item;
        item["name"] = html_escape(parameter.name);
        item["type"] = html_escape(parameter.type);
        item["default"] = parameter.default_value
            ? boost::json::value(html_escape(*parameter.default_value))
            : boost::json::value(nullptr);
        item["description"] = html_escape(content_plain_text(parameter.description));
        parameters.push_back(std::move(item));
    }
    object["parameters"] = std::move(parameters);
    // SQL attributes / C++ qualifiers (§12.3), sorted for deterministic output.
    if (!entity.attributes.empty()) {
        boost::json::array attributes;
        for (const auto& [name, value] : entity.attributes) {
            if (name == "refid") {
                continue;  // internal Doxygen routing alias, never page content
            }
            boost::json::object item;
            item["name"] = html_escape(name);
            item["value"] = html_escape(value);
            attributes.push_back(std::move(item));
        }
        if (!attributes.empty()) {
            object["attributes"] = std::move(attributes);
        }
    }
    if (!entity.overload_members.empty()) {
        boost::json::array overloads;
        for (const auto& member : entity.overload_members) {
            boost::json::object item;
            item["signature"] = html_escape(member.first);
            item["url"] = html_escape(member.second);
            overloads.push_back(std::move(item));
        }
        object["overloads"] = std::move(overloads);
    }
    if (!entity.child_members.empty()) {
        boost::json::array members;
        for (const auto& member : entity.child_members) {
            boost::json::object item;
            item["name"] = html_escape(member.first);
            item["url"] = html_escape(member.second);
            members.push_back(std::move(item));
        }
        object["members"] = std::move(members);
    }
    if (!entity.implemented_by_route.empty()) {
        object["implemented_by"] = html_escape(entity.implemented_by_route);
    }
    return object;
}

}  // namespace

/// @brief Serializes one documented entity view object (HTML-escaped) for a page.
/// @param entity Canonical documented entity.
/// @return The HTML-safe JSON object text for a single-entity page context.
[[nodiscard]] std::string serialize_reference_entity(const reference_entity& entity) {
    return apply_html_safe_escapes(boost::json::serialize(serialize_entity_view(entity)));
}

/// @brief Serializes a documented-entity view array (HTML-escaped) for an index.
/// @param entities Canonical documented entities.
/// @return The HTML-safe JSON array text for an entity-index page context.
[[nodiscard]] std::string serialize_reference_entity_array(
    const std::vector<reference_entity>& entities) {
    boost::json::array items;
    for (const reference_entity& entity : entities) {
        items.push_back(serialize_entity_view(entity));
    }
    return apply_html_safe_escapes(
        boost::json::serialize(boost::json::value(std::move(items))));
}

/// @brief Serializes executed landing slots to the `examples.<slot>` context.
///
/// Shape consumed by the landing partials: each slot has `visible_sql` and a
/// nullable `result` with typed `columns[]` and `rows[][].{is_null,text}`. Every
/// plain-text value is HTML-escaped at the source; there is no autoescape (§14).
[[nodiscard]] std::string serialize_landing_examples(
    const std::vector<landing_example_view>& slots) {
    boost::json::object root;
    for (const landing_example_view& slot : slots) {
        boost::json::object entry;
        entry["visible_sql"] = html_escape(slot.visible_sql);
        if (slot.has_result && slot.result.shape == result_shape::rows) {
            boost::json::object result;
            boost::json::array columns;
            for (const sql_column& column : slot.result.columns) {
                boost::json::object header;
                header["name"] = html_escape(column.name);
                header["type_name"] = html_escape(column.type_name);
                columns.push_back(std::move(header));
            }
            result["columns"] = std::move(columns);
            boost::json::array rows;
            for (const std::vector<sql_cell>& source_row : slot.result.rows) {
                boost::json::array cells;
                for (const sql_cell& cell : source_row) {
                    boost::json::object rendered;
                    rendered["is_null"] = cell.is_null;
                    rendered["text"] = html_escape(cell.text);
                    cells.push_back(std::move(rendered));
                }
                rows.push_back(std::move(cells));
            }
            result["rows"] = std::move(rows);
            entry["result"] = std::move(result);
        } else {
            entry["result"] = nullptr;
        }
        root[slot.slot] = std::move(entry);
    }
    return apply_html_safe_escapes(boost::json::serialize(boost::json::value(std::move(root))));
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

// Inverse of node_kind_name; unknown spellings are rejected so a stale/tampered
// artifact cannot silently degrade to an unexpected node kind (§5.1).
[[nodiscard]] content_node_kind node_kind_from_name(std::string_view name) {
    using kind = content_node_kind;
    if (name == "document") return kind::root;
    if (name == "paragraph") return kind::paragraph;
    if (name == "text") return kind::text;
    if (name == "heading") return kind::heading;
    if (name == "emphasis") return kind::emphasis;
    if (name == "strong") return kind::strong_text;
    if (name == "strikethrough") return kind::strikethrough;
    if (name == "highlight") return kind::highlight;
    if (name == "link") return kind::link;
    if (name == "image") return kind::image;
    if (name == "entity_link") return kind::entity_link;
    if (name == "code_span") return kind::code_span;
    if (name == "code_block") return kind::code_block;
    if (name == "admonition") return kind::admonition;
    if (name == "block_quote") return kind::block_quote;
    if (name == "ordered_list") return kind::ordered_list;
    if (name == "unordered_list") return kind::unordered_list;
    if (name == "list_item") return kind::list_item;
    if (name == "table") return kind::table;
    if (name == "table_row") return kind::table_row;
    if (name == "table_cell") return kind::table_cell;
    if (name == "footnote") return kind::footnote;
    if (name == "line_break") return kind::line_break;
    if (name == "code_annotation") return kind::code_annotation;
    throw std::runtime_error("site_model: unknown document node kind '"
                             + std::string(name) + "'");
}

[[nodiscard]] entity_query_domain domain_from_name(std::string_view name) {
    if (name == "cpp") return entity_query_domain::cpp;
    if (name == "sql") return entity_query_domain::sql;
    if (name == "canonical_id") return entity_query_domain::canonical_id;
    return entity_query_domain::any;
}

// Inverse of serialize_node; optional keys default to empty just as the serializer
// omits them when empty, so the round-trip is exact.
[[nodiscard]] content_node deserialize_node(const boost::json::value& value) {
    const boost::json::object& object = value.as_object();
    content_node node;
    node.kind = node_kind_from_name(std::string(object.at("kind").as_string()));
    if (const boost::json::value* text = object.if_contains("text")) {
        node.text = std::string(text->as_string());
    }
    if (const boost::json::value* target = object.if_contains("target")) {
        node.target = std::string(target->as_string());
    }
    if (const boost::json::value* attributes = object.if_contains("attributes")) {
        for (const auto& field : attributes->as_object()) {
            node.attributes.emplace(std::string(field.key()),
                                    std::string(field.value().as_string()));
        }
    }
    if (const boost::json::value* children = object.if_contains("children")) {
        for (const boost::json::value& child : children->as_array()) {
            node.children.push_back(deserialize_node(child));
        }
    }
    return node;
}

// Inverse of serialize_example. setup/teardown are read when present so the plan
// round-trips; the site_model projection stores examples with those fields cleared.
[[nodiscard]] sql_example deserialize_example(const boost::json::value& value) {
    const boost::json::object& object = value.as_object();
    sql_example example;
    example.local_id = std::string(object.at("local_id").as_string());
    example.canonical_id = std::string(object.at("canonical_id").as_string());
    example.expectation = std::string(object.at("expectation").as_string()) == "error"
        ? sql_expectation::error : sql_expectation::success;
    example.timeout = std::chrono::milliseconds(
        object.at("timeout_ms").to_number<std::int64_t>());
    example.expected_sqlstate = object.if_contains("expected_sqlstate")
        ? as_optional_string(object.at("expected_sqlstate")) : std::nullopt;
    if (const boost::json::value* setup = object.if_contains("setup_sql")) {
        example.setup_sql = std::string(setup->as_string());
    }
    if (const boost::json::value* visible = object.if_contains("visible_sql")) {
        example.visible_sql = std::string(visible->as_string());
    }
    if (const boost::json::value* teardown = object.if_contains("teardown_sql")) {
        example.teardown_sql = std::string(teardown->as_string());
    }
    return example;
}

[[nodiscard]] unresolved_entity_query deserialize_query(const boost::json::value& value) {
    const boost::json::object& object = value.as_object();
    unresolved_entity_query query;
    query.domain = domain_from_name(std::string(object.at("domain").as_string()));
    query.spelling = std::string(object.at("spelling").as_string());
    query.label = object.if_contains("label")
        ? as_optional_string(object.at("label")) : std::nullopt;
    return query;
}

// Inverse of serialize_tutorial. Rebuilds the resolved content forest, examples,
// entity queries, and previous/next links the render stage binds without a DB.
[[nodiscard]] tutorial deserialize_tutorial(const boost::json::value& value) {
    const boost::json::object& object = value.as_object();
    tutorial item;
    item.metadata.slug = std::string(object.at("slug").as_string());
    item.metadata.order =
        static_cast<std::uint32_t>(object.at("order").to_number<std::int64_t>());
    item.metadata.title = std::string(object.at("title").as_string());
    item.metadata.description = std::string(object.at("description").as_string());
    for (const boost::json::value& keyword : object.at("keywords").as_array()) {
        item.metadata.keywords.push_back(std::string(keyword.as_string()));
    }
    item.metadata.draft = object.at("draft").as_bool();
    item.source_path = std::string(object.at("source_path").as_string());
    item.previous_slug = as_optional_string(object.at("previous_slug"));
    item.next_slug = as_optional_string(object.at("next_slug"));
    for (const boost::json::value& example : object.at("sql_examples").as_array()) {
        item.sql_examples.push_back(deserialize_example(example));
    }
    for (const boost::json::value& query : object.at("entity_queries").as_array()) {
        item.entity_queries.push_back(deserialize_query(query));
    }
    for (const boost::json::value& node : object.at("content").as_array()) {
        item.content.push_back(deserialize_node(node));
    }
    return item;
}

// Inverse of serialize_entity (the full reference_entity projection, §11.3).
[[nodiscard]] reference_entity deserialize_one_entity(const boost::json::object& object) {
    reference_entity entity;
    entity.canonical_id = std::string(object.at("canonical_id").as_string());
    entity.domain = std::string(object.at("domain").as_string()) == "sql"
        ? entity_domain::sql : entity_domain::cpp;
    entity.kind = entity_kind_from_name(std::string(object.at("kind").as_string()));
    entity.qualified_name = std::string(object.at("qualified_name").as_string());
    entity.display_name = std::string(object.at("display_name").as_string());
    entity.normalized_signature =
        std::string(object.at("normalized_signature").as_string());
    entity.summary_text = std::string(object.at("summary").as_string());
    entity.route_path = std::string(object.at("route_path").as_string());
    entity.module_name = as_optional_string(object.at("module"));
    entity.return_type = as_optional_string(object.at("return_type"));
    if (const boost::json::value* documentation = object.if_contains("documentation");
        documentation != nullptr && documentation->is_array()) {
        for (const boost::json::value& node : documentation->as_array()) {
            entity.detail_content.push_back(deserialize_node(node));
        }
    }
    if (const boost::json::value& declaration = object.at("declaration");
        declaration.is_object()) {
        source_span span;
        span.path = std::string(declaration.at("path").as_string());
        span.first_line =
            static_cast<std::size_t>(declaration.at("line").to_number<std::int64_t>());
        entity.declaration = span;
    }
    if (const boost::json::value* parameters = object.if_contains("parameters");
        parameters != nullptr && parameters->is_array()) {
        for (const boost::json::value& parameter : parameters->as_array()) {
            const boost::json::object& fields = parameter.as_object();
            parameter_description documented;
            documented.name = std::string(fields.at("name").as_string());
            documented.type = std::string(fields.at("type").as_string());
            documented.default_value = as_optional_string(fields.at("default"));
            if (const boost::json::value* description = fields.if_contains("description");
                description != nullptr && description->is_array()) {
                for (const boost::json::value& node : description->as_array()) {
                    documented.description.push_back(deserialize_node(node));
                }
            }
            entity.parameters.push_back(std::move(documented));
        }
    }
    entity.implementation_entity_id = object.if_contains("implementation_entity_id")
        ? as_optional_string(object.at("implementation_entity_id")) : std::nullopt;
    if (const boost::json::value* overload_ids = object.if_contains("overload_ids");
        overload_ids != nullptr && overload_ids->is_array()) {
        for (const boost::json::value& id : overload_ids->as_array()) {
            entity.overload_ids.push_back(std::string(id.as_string()));
        }
    }
    if (const boost::json::value& attributes = object.at("attributes");
        attributes.is_object()) {
        for (const auto& field : attributes.as_object()) {
            entity.attributes.emplace(std::string(field.key()),
                                      std::string(field.value().as_string()));
        }
    }
    return entity;
}

// Semantic (non-HTML-escaped) projection of an executed landing slot for the
// private site_model. The render stage re-escapes via serialize_landing_examples,
// so these stored values are the raw result text (§180 semantic projection).
[[nodiscard]] boost::json::value serialize_landing_view(const landing_example_view& slot) {
    boost::json::object object;
    object["slot"] = slot.slot;
    object["visible_sql"] = slot.visible_sql;
    object["has_result"] = slot.has_result;
    if (slot.has_result && slot.result.shape == result_shape::rows) {
        boost::json::object result;
        boost::json::array columns;
        for (const sql_column& column : slot.result.columns) {
            boost::json::object header;
            header["name"] = column.name;
            header["type_name"] = column.type_name;
            columns.push_back(std::move(header));
        }
        result["columns"] = std::move(columns);
        boost::json::array rows;
        for (const std::vector<sql_cell>& source_row : slot.result.rows) {
            boost::json::array cells;
            for (const sql_cell& cell : source_row) {
                boost::json::object rendered;
                rendered["is_null"] = cell.is_null;
                rendered["text"] = cell.text;
                cells.push_back(std::move(rendered));
            }
            rows.push_back(std::move(cells));
        }
        result["rows"] = std::move(rows);
        object["result"] = std::move(result);
    } else {
        object["result"] = boost::json::value(nullptr);
    }
    return object;
}

[[nodiscard]] landing_example_view deserialize_landing_view(const boost::json::value& value) {
    const boost::json::object& object = value.as_object();
    landing_example_view slot;
    slot.slot = std::string(object.at("slot").as_string());
    slot.visible_sql = std::string(object.at("visible_sql").as_string());
    slot.has_result = object.at("has_result").as_bool();
    if (const boost::json::value& result = object.at("result"); result.is_object()) {
        slot.result.shape = result_shape::rows;
        for (const boost::json::value& column : result.at("columns").as_array()) {
            sql_column header;
            header.name = std::string(column.as_object().at("name").as_string());
            header.type_name = std::string(
                column.as_object().at("type_name").as_string());
            slot.result.columns.push_back(std::move(header));
        }
        for (const boost::json::value& source_row : result.at("rows").as_array()) {
            std::vector<sql_cell> cells;
            for (const boost::json::value& cell : source_row.as_array()) {
                sql_cell rendered;
                rendered.is_null = cell.as_object().at("is_null").as_bool();
                rendered.text = std::string(cell.as_object().at("text").as_string());
                cells.push_back(std::move(rendered));
            }
            slot.result.rows.push_back(std::move(cells));
        }
    }
    return slot;
}

}  // namespace

/// @brief Parses a cpp_entities/sql_entities artifact back into the model (§11.3).
///
/// Reconstructs the complete persisted entity projection, including detailed and
/// per-parameter documentation plus the SQL-to-C++ implementation relationship.
/// @param text Canonical entity-index JSON text.
/// @return The documented entities in the array's stored order.
[[nodiscard]] std::vector<reference_entity> deserialize_reference_entities(std::string_view text) {
    const boost::json::value root = parse_json_value(text);
    const boost::json::object& object = root.as_object();
    const boost::json::value* const schema = object.if_contains("schema");
    if (schema == nullptr || !schema->is_string()) {
        throw std::runtime_error(
            "reference entities: missing current schema identity");
    }
    if (schema->as_string() == "bioseq.website.cpp_entities") {
        require_exact_members(
            object,
            {"schema", "schema_revision", "clang_version", "entities"},
            "C++ entities");
        require_current_schema(
            object, "bioseq.website.cpp_entities", 1, "C++ entities");
    } else if (schema->as_string() == "bioseq.website.sql_entities") {
        require_exact_members(
            object,
            {"schema", "schema_revision", "entities"},
            "SQL entities");
        require_current_schema(
            object, "bioseq.website.sql_entities", 1, "SQL entities");
    } else {
        throw std::runtime_error(
            "reference entities: unexpected schema identity");
    }
    std::vector<reference_entity> entities;
    const boost::json::array& items = object.at("entities").as_array();
    entities.reserve(items.size());
    for (const boost::json::value& item : items) {
        entities.push_back(deserialize_one_entity(item.as_object()));
    }
    return entities;
}

/// @brief Serializes the resolved, database-independent site model (§180).
///
/// Schema bioseq.website.site_model revision 1: the private artifact `website execute`
/// writes (mode 0600) and `website render` consumes without a cluster. It carries
/// only normalized semantic projections — resolved tutorial/landing trees, executed
/// visible-result models, the entity graph with base-independent routes, coverage,
/// and server/extension provenance — never setup/teardown SQL, credentials, OIDs,
/// timings, or machine paths, so the same model renders at two base paths.
/// @param model Fully resolved site model.
/// @return Canonical JSON ending in one newline.
[[nodiscard]] std::string serialize_site_model(const site_model& model) {
    require_processing_contract(
        model.schema,
        model.schema_revision,
        "bioseq.website.site_model",
        model.parser_identity,
        model.parser_revision,
        model.binding_schema,
        model.binding_schema_revision,
        "site model");
    boost::json::object root;
    root["schema"] = model.schema;
    root["schema_revision"] = model.schema_revision;
    root["parser_identity"] = model.parser_identity;
    root["parser_revision"] = model.parser_revision;
    root["binding_schema"] = model.binding_schema;
    root["binding_schema_revision"] = model.binding_schema_revision;
    boost::json::object version;
    version["label"] = model.version.path_component;
    boost::json::array parts;
    for (const std::uint32_t part : model.version.parts) {
        parts.push_back(static_cast<std::int64_t>(part));
    }
    version["parts"] = std::move(parts);
    root["version"] = std::move(version);
    root["site_title"] = model.site_title;
    root["source_revision"] = model.source_revision;
    boost::json::array tutorials;
    for (const tutorial& item : model.tutorials) {
        tutorials.push_back(serialize_tutorial(item));
    }
    root["tutorials"] = std::move(tutorials);
    boost::json::array landing;
    for (const landing_example_view& slot : model.landing_examples) {
        landing.push_back(serialize_landing_view(slot));
    }
    root["landing_examples"] = std::move(landing);
    boost::json::array cpp_entities;
    for (const reference_entity& entity : model.cpp_entities) {
        cpp_entities.push_back(serialize_entity(entity));
    }
    root["cpp_entities"] = std::move(cpp_entities);
    boost::json::array sql_entities;
    for (const reference_entity& entity : model.sql_entities) {
        sql_entities.push_back(serialize_entity(entity));
    }
    root["sql_entities"] = std::move(sql_entities);
    boost::json::array omitted_sql_entities;
    for (const reference_entity& entity : model.omitted_sql_entities) {
        omitted_sql_entities.push_back(serialize_entity(entity));
    }
    root["omitted_sql_entities"] = std::move(omitted_sql_entities);
    boost::json::object coverage;
    coverage["public_total"] = static_cast<std::int64_t>(model.coverage_public_total);
    coverage["documented_total"] =
        static_cast<std::int64_t>(model.coverage_documented_total);
    boost::json::array missing;
    for (const std::string& entry : model.coverage_missing) {
        missing.push_back(boost::json::value(entry));
    }
    coverage["missing"] = std::move(missing);
    boost::json::array missing_parameters;
    for (const std::string& entry : model.coverage_missing_parameters) {
        missing_parameters.push_back(boost::json::value(entry));
    }
    coverage["missing_parameters"] = std::move(missing_parameters);
    boost::json::array missing_returns;
    for (const std::string& entry : model.coverage_missing_returns) {
        missing_returns.push_back(boost::json::value(entry));
    }
    coverage["missing_returns"] = std::move(missing_returns);
    boost::json::array fatal_missing;
    for (const std::string& entry : model.coverage_fatal_missing) {
        fatal_missing.push_back(boost::json::value(entry));
    }
    coverage["fatal_missing"] = std::move(fatal_missing);
    boost::json::array groups;
    for (const coverage_group_view& group : model.coverage_groups) {
        boost::json::object entry;
        entry["group"] = group.group;
        entry["public_total"] = static_cast<std::int64_t>(group.public_total);
        entry["documented_total"] = static_cast<std::int64_t>(group.documented_total);
        groups.push_back(std::move(entry));
    }
    coverage["groups"] = std::move(groups);
    root["coverage"] = std::move(coverage);
    boost::json::object provenance;
    provenance["postgres_server_version"] = model.postgres_server_version;
    provenance["extension_version"] = model.extension_version;
    provenance["staged_extension_sha256"] = model.staged_extension_sha256;
    root["provenance"] = std::move(provenance);
    boost::json::array inputs;
    for (const source_input& input : model.inputs) {
        boost::json::object object;
        object["path"] = input.path;
        object["content_hash"] = input.content_hash;
        inputs.push_back(std::move(object));
    }
    root["inputs"] = std::move(inputs);
    boost::json::object signatures;
    signatures["plan"] = model.plan_signature;
    signatures["cpp"] = model.cpp_signature;
    root["signatures"] = std::move(signatures);
    return serialize_canonical(root);
}

/// @brief Parses the current site-model artifact, rejecting a wrong
/// schema so a stale/tampered artifact cannot silently render (§5.1, §184).
/// @param text Canonical site_model JSON text.
/// @return The resolved site model.
[[nodiscard]] site_model deserialize_site_model(std::string_view text) {
    const boost::json::value document = parse_json_value(text);
    const boost::json::object& object = document.as_object();
    site_model model;
    require_exact_members(
        object,
        {"schema",
         "schema_revision",
         "parser_identity",
         "parser_revision",
         "binding_schema",
         "binding_schema_revision",
         "version",
         "site_title",
         "source_revision",
         "tutorials",
         "landing_examples",
         "cpp_entities",
         "sql_entities",
         "omitted_sql_entities",
         "coverage",
         "provenance",
         "inputs",
         "signatures"},
        "site model");
    require_current_schema(
        object, "bioseq.website.site_model", 1, "site model");
    model.schema = std::string(object.at("schema").as_string());
    model.schema_revision = 1;
    model.parser_identity =
        std::string(object.at("parser_identity").as_string());
    model.parser_revision = require_current_revision_member(
        object, "parser_revision", 1, "site model");
    model.binding_schema =
        std::string(object.at("binding_schema").as_string());
    model.binding_schema_revision = require_current_revision_member(
        object, "binding_schema_revision", 1, "site model");
    require_processing_contract(
        model.schema,
        model.schema_revision,
        "bioseq.website.site_model",
        model.parser_identity,
        model.parser_revision,
        model.binding_schema,
        model.binding_schema_revision,
        "site model");
    const boost::json::object& version = object.at("version").as_object();
    model.version.path_component = std::string(version.at("label").as_string());
    for (const boost::json::value& part : version.at("parts").as_array()) {
        model.version.parts.push_back(
            static_cast<std::uint32_t>(part.to_number<std::int64_t>()));
    }
    model.site_title = std::string(object.at("site_title").as_string());
    model.source_revision = std::string(object.at("source_revision").as_string());
    for (const boost::json::value& item : object.at("tutorials").as_array()) {
        model.tutorials.push_back(deserialize_tutorial(item));
    }
    for (const boost::json::value& slot : object.at("landing_examples").as_array()) {
        model.landing_examples.push_back(deserialize_landing_view(slot));
    }
    for (const boost::json::value& entity : object.at("cpp_entities").as_array()) {
        model.cpp_entities.push_back(deserialize_one_entity(entity.as_object()));
    }
    for (const boost::json::value& entity : object.at("sql_entities").as_array()) {
        model.sql_entities.push_back(deserialize_one_entity(entity.as_object()));
    }
    if (const auto* omitted = object.if_contains("omitted_sql_entities")) {
        for (const boost::json::value& entity : omitted->as_array()) {
            model.omitted_sql_entities.push_back(
                deserialize_one_entity(entity.as_object()));
        }
    }
    const boost::json::object& coverage = object.at("coverage").as_object();
    model.coverage_public_total = static_cast<std::uint32_t>(
        coverage.at("public_total").to_number<std::int64_t>());
    model.coverage_documented_total = static_cast<std::uint32_t>(
        coverage.at("documented_total").to_number<std::int64_t>());
    for (const boost::json::value& entry : coverage.at("missing").as_array()) {
        model.coverage_missing.push_back(std::string(entry.as_string()));
    }
    if (const auto* parameters = coverage.if_contains("missing_parameters")) {
        for (const boost::json::value& entry : parameters->as_array()) {
            model.coverage_missing_parameters.push_back(std::string(entry.as_string()));
        }
    }
    if (const auto* returns = coverage.if_contains("missing_returns")) {
        for (const boost::json::value& entry : returns->as_array()) {
            model.coverage_missing_returns.push_back(std::string(entry.as_string()));
        }
    }
    if (const auto* groups = coverage.if_contains("groups")) {
        for (const boost::json::value& entry : groups->as_array()) {
            const boost::json::object& fields = entry.as_object();
            coverage_group_view view;
            view.group = std::string(fields.at("group").as_string());
            view.public_total = static_cast<std::uint32_t>(
                fields.at("public_total").to_number<std::int64_t>());
            view.documented_total = static_cast<std::uint32_t>(
                fields.at("documented_total").to_number<std::int64_t>());
            model.coverage_groups.push_back(std::move(view));
        }
    }
    for (const boost::json::value& entry : coverage.at("fatal_missing").as_array()) {
        model.coverage_fatal_missing.push_back(std::string(entry.as_string()));
    }
    const boost::json::object& provenance = object.at("provenance").as_object();
    model.postgres_server_version =
        std::string(provenance.at("postgres_server_version").as_string());
    model.extension_version =
        std::string(provenance.at("extension_version").as_string());
    model.staged_extension_sha256 =
        std::string(provenance.at("staged_extension_sha256").as_string());
    for (const boost::json::value& input : object.at("inputs").as_array()) {
        source_input parsed;
        parsed.path = std::string(input.as_object().at("path").as_string());
        parsed.content_hash =
            std::string(input.as_object().at("content_hash").as_string());
        model.inputs.push_back(std::move(parsed));
    }
    const boost::json::object& signatures = object.at("signatures").as_object();
    model.plan_signature = std::string(signatures.at("plan").as_string());
    model.cpp_signature = std::string(signatures.at("cpp").as_string());
    return model;
}

/// @brief Serializes the current Clang source-graph closure (§11.3).
/// @param clang_version The Clang version the scan was linked against.
/// @param roots Configured primary module names (the closure seeds).
/// @param closure Reachable Bioseq-owned units, sorted by module name.
/// @return Canonical JSON ending in one newline.
[[nodiscard]] std::string serialize_source_graph(
    std::string_view clang_version,
    const std::vector<std::string>& roots,
    const std::vector<module_graph_unit>& closure) {
    boost::json::object root;
    root["schema"] = "bioseq.website.source_graph";
    root["schema_revision"] = 1;
    root["clang_version"] = std::string(clang_version);
    boost::json::array root_array;
    for (const std::string& name : roots) {
        root_array.push_back(boost::json::value(name));
    }
    root["roots"] = std::move(root_array);
    boost::json::array reachable;
    boost::json::array edges;
    for (const module_graph_unit& unit : closure) {
        boost::json::object node;
        node["module"] = unit.provided_module;
        node["source"] = unit.source_path;
        boost::json::array exported;
        for (const std::string& name : unit.exported_imports) {
            exported.push_back(boost::json::value(name));
            boost::json::object edge;
            edge["from"] = unit.provided_module;
            edge["to"] = name;
            edges.push_back(std::move(edge));
        }
        node["exported_imports"] = std::move(exported);
        reachable.push_back(std::move(node));
    }
    root["reachable"] = std::move(reachable);
    root["edges"] = std::move(edges);
    return serialize_canonical(root);
}

/// @brief Reads a required top-level string from a current website artifact.
/// @param text Artifact JSON text.
/// @param key Required field name.
/// @param expected_schema Exact current schema identity.
/// @param expected_revision Exact current numeric schema revision.
/// @return The required string value.
[[nodiscard]] std::string manifest_string_field(
    std::string_view text,
    std::string_view key,
    std::string_view expected_schema,
    std::uint32_t expected_revision) {
    const boost::json::value root = parse_json_value(text);
    const boost::json::object& object = root.as_object();
    require_manifest_contract(object, expected_schema, expected_revision);
    return std::string(object.at(key).as_string());
}

/// @brief Reads the owned output route set from a current manifest (§16).
/// @param text Manifest JSON text.
/// @param expected_schema Exact current schema identity.
/// @param expected_revision Exact current numeric schema revision.
/// @return The manifest's declared output routes (leading-slash), in stored order.
[[nodiscard]] std::vector<std::string> manifest_output_routes(
    std::string_view text,
    std::string_view expected_schema,
    std::uint32_t expected_revision) {
    const boost::json::value root = parse_json_value(text);
    require_manifest_contract(
        root.as_object(), expected_schema, expected_revision);
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
/// @return Canonical JSON ending in one newline.
[[nodiscard]] std::string serialize_release_manifest(
    std::string_view primary_version,
    const std::vector<std::string>& ordered_versions,
    std::string_view base_path,
    const std::vector<std::string>& input_manifest_hashes,
    const std::vector<std::string>& input_success_hashes,
    const std::vector<std::pair<std::string, std::string>>& outputs) {
    boost::json::object root;
    root["schema"] = "bioseq.website.assembled_manifest";
    root["schema_revision"] = 1;
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
/// (built by the caller from json_quote / serialize_reference_entity_array), so no raw
/// string is ever concatenated unescaped into the context (§14).
/// @param site Resolved site-wide fields (base-path-aware URLs and labels).
/// @param page_object JSON object text for the per-page `page` binding.
/// @param collection_key Optional extra top-level key (for example "entities").
/// @param collection_json JSON value text for that key; ignored when key empty.
/// @return The serialized page context ready for render_page_template.
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
    site_object["assets_base_url"] = site.assets_base_url;
    site_object["css_url"] = site.css_url;
    site_object["js_url"] = site.js_url;
    site_object["favicon_url"] = site.favicon_url;
    site_object["prism_js_url"] = site.prism_js_url;
    site_object["prism_css_url"] = site.prism_css_url;
    site_object["social_image_url"] = site.social_image_url;
    site_object["search_index_url"] = site.search_index_url;
    site_object["versions_url"] = site.versions_url;
    site_object["versions_page_url"] = site.versions_page_url;
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
    // datePublished/dateModified is included (no deterministic source metadata). The
    // primary @type follows the page_type hint (WebSite/TechArticle/WebPage); a
    // BreadcrumbList is added under @graph when the page carries a breadcrumb trail.
    boost::json::value page_value = parse_json_value(page_object);
    if (page_value.is_object()) {
        boost::json::object& page_object_ref = page_value.as_object();
        std::string page_type = "WebPage";
        if (page_object_ref.contains("page_type") && page_object_ref.at("page_type").is_string()) {
            const std::string hint(page_object_ref.at("page_type").as_string().c_str());
            if (hint == "landing") {
                page_type = "WebSite";
            } else if (hint == "tutorial" || hint == "entity") {
                page_type = "TechArticle";
            }
        }
        boost::json::object primary;
        primary["@type"] = page_type;
        primary["name"] = page_object_ref.contains("title")
            ? page_object_ref.at("title") : boost::json::value("");
        primary["description"] = page_object_ref.contains("description")
            ? page_object_ref.at("description") : boost::json::value("");
        primary["url"] = page_object_ref.contains("canonical")
            ? page_object_ref.at("canonical") : boost::json::value("");
        primary["isPartOf"] = site.title;
        if (!site.social_image_url.empty()) {
            primary["image"] = site.social_image_url;
        }
        boost::json::object json_ld;
        json_ld["@context"] = "https://schema.org";
        if (page_object_ref.contains("breadcrumb") && page_object_ref.at("breadcrumb").is_array()
            && !page_object_ref.at("breadcrumb").as_array().empty()) {
            boost::json::array crumb_items;
            std::int64_t position = 1;
            for (const boost::json::value& crumb : page_object_ref.at("breadcrumb").as_array()) {
                if (!crumb.is_object()) {
                    continue;
                }
                boost::json::object list_item;
                list_item["@type"] = "ListItem";
                list_item["position"] = position++;
                list_item["name"] = crumb.as_object().contains("name")
                    ? crumb.as_object().at("name") : boost::json::value("");
                if (crumb.as_object().contains("url")
                    && crumb.as_object().at("url").is_string()) {
                    list_item["item"] = crumb.as_object().at("url");
                }
                crumb_items.push_back(std::move(list_item));
            }
            boost::json::object breadcrumb_list;
            breadcrumb_list["@type"] = "BreadcrumbList";
            breadcrumb_list["itemListElement"] = std::move(crumb_items);
            boost::json::array graph;
            graph.push_back(std::move(primary));
            graph.push_back(std::move(breadcrumb_list));
            json_ld["@graph"] = std::move(graph);
        } else {
            for (const auto& field : primary) {
                json_ld[field.key()] = field.value();
            }
        }
        page_object_ref["jsonld"] = boost::json::serialize(json_ld);
    }

    boost::json::object root;
    root["site"] = std::move(site_object);
    root["nav"] = std::move(nav_object);
    root["page"] = std::move(page_value);
    if (!collection_key.empty()) {
        root[std::string(collection_key)] = parse_json_value(collection_json);
    }
    return apply_html_safe_escapes(boost::json::serialize(root));
}

/// @brief Serializes the content-addressed per-version entity tooltip set (§12.4).
///
/// Richer than the search index: each record carries the normalized signature and
/// route so a hovered entity link can show a summary/signature card without a
/// second request. The file is named by its own SHA-256 (§16), so its bytes are
/// immutable and long-cacheable.
/// @param entities Canonical documented entities for the version.
/// @return Canonical HTML-safe JSON ending in one newline.
[[nodiscard]] std::string serialize_reference_tooltips(
    const std::vector<reference_entity>& entities) {
    boost::json::object root;
    root["schema"] = "bioseq.website.entity_tooltips";
    root["schema_revision"] = 1;
    boost::json::object records;
    for (const reference_entity& entity : entities) {
        boost::json::object record;
        record["title"] = entity.display_name;
        record["qualified_name"] = entity.qualified_name;
        record["kind"] = std::string(entity_kind_name(entity.kind));
        record["signature"] = entity.normalized_signature;
        record["summary"] = entity.summary_text;
        record["route"] = entity.route_path;
        record["return_type"] = entity.return_type
            ? boost::json::value(*entity.return_type) : boost::json::value(nullptr);
        boost::json::array parameters;
        for (const parameter_description& parameter : entity.parameters) {
            boost::json::object item;
            item["name"] = parameter.name;
            item["type"] = parameter.type;
            item["description"] = content_plain_text(parameter.description);
            parameters.push_back(std::move(item));
        }
        record["parameters"] = std::move(parameters);
        records[entity.canonical_id] = std::move(record);
    }
    root["entities"] = std::move(records);
    return serialize_canonical(root);
}

/// @brief Serializes the versions manifest consumed by the header selector (§16).
/// @param version_labels Version path components, newest first.
/// @param base_path Normalized base path used to build each version route.
/// @return Canonical HTML-safe JSON ending in one newline.
[[nodiscard]] std::string serialize_release_catalog(
    const std::vector<std::string>& version_labels, std::string_view base_path) {
    boost::json::object root;
    root["schema"] = "bioseq.website.versions";
    root["schema_revision"] = 1;
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
/// @return Canonical JSON ending in one newline.
[[nodiscard]] boost::json::array serialize_id_array(
    const std::vector<std::string>& ids) {
    boost::json::array array;
    for (const std::string& id : ids) {
        array.push_back(boost::json::value(id));
    }
    return array;
}

[[nodiscard]] std::string serialize_coverage(
    std::size_t public_total, std::size_t documented_total,
    const std::vector<std::string>& missing_summary,
    const std::vector<std::string>& missing_parameters,
    const std::vector<std::string>& missing_returns,
    const std::vector<std::string>& fatal_missing,
    const std::vector<coverage_group_view>& groups) {
    boost::json::object root;
    root["schema"] = "bioseq.website.documentation_coverage";
    root["schema_revision"] = 1;
    root["public_total"] = static_cast<std::int64_t>(public_total);
    root["documented_total"] = static_cast<std::int64_t>(documented_total);
    boost::json::array group_array;
    for (const coverage_group_view& group : groups) {
        boost::json::object entry;
        entry["group"] = group.group;
        entry["public_total"] = static_cast<std::int64_t>(group.public_total);
        entry["documented_total"] = static_cast<std::int64_t>(group.documented_total);
        group_array.push_back(std::move(entry));
    }
    root["groups"] = std::move(group_array);
    root["missing_summary"] = serialize_id_array(missing_summary);
    root["missing_parameters"] = serialize_id_array(missing_parameters);
    root["missing_returns"] = serialize_id_array(missing_returns);
    root["fatal_missing"] = serialize_id_array(fatal_missing);
    return serialize_canonical(root);
}

}  // namespace bioseq::codegen::website
