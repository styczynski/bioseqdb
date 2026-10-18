module;

#include <boost/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <map>
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
import bioseq.codegen.sql.signature;

// Writes website JSON with Boost.JSON. Callers insert object keys in a fixed
// order Artifact serialization adds a final newline and escapes characters
// that could interfere with embedding JSON in HTML.
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

/// @brief Serializes JSON with escapes suitable for embedding in HTML.
/// @param value JSON value with keys already in the desired schema order.
/// @return Serialized JSON ending in one newline.
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

/// @brief Documentation for a public SQL entity from the generated manifest.
///
/// stable_identity uses $extension for  the configurable schema.
/// installed_identity replaces it with the schema used by this installation,
/// so catalog checks and reference pages can find the installed object.
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
    // Source-owned examples, setting values, return documentation and SQL aliases.
    std::vector<content_node> additional_documentation;
    std::string result_sql_type;
    std::vector<parameter_description> parameters;
    std::vector<parameter_description> fields;
    std::map<std::string, std::string, std::less<>> attributes;
    std::string catalog_identity;
    std::string qualified_name;
    bool public_documentation{};
};

[[nodiscard]] content_node reference_heading(std::string text, std::string anchor) {
    return content_node{.kind = content_node_kind::heading,
        .attributes = {{"level", "2"}, {"anchor", std::move(anchor)}},
        .children = {content_node{.kind = content_node_kind::text, .text = std::move(text)}}};
}

[[nodiscard]] content_node reference_paragraph(std::string text) {
    return content_node{.kind = content_node_kind::paragraph,
        .children = {content_node{.kind = content_node_kind::text, .text = std::move(text)}}};
}

void append_documentation_examples(std::vector<content_node>& nodes,
                                  const boost::json::value* examples) {
    if (!examples || !examples->is_array() || examples->as_array().empty()) return;
    nodes.push_back(reference_heading("Examples", "examples"));
    for (const auto& example : examples->as_array()) {
        const auto& item = example.as_object();
        nodes.push_back(reference_paragraph(std::string(item.at("description").as_string())));
        nodes.push_back(content_node{.kind = content_node_kind::code_block,
            .text = std::string(item.at("sql").as_string()),
            .attributes = {{"language", "sql"}}});
    }
}

/// @brief SQL manifest data needed for catalog checks and website documentation.
struct sql_manifest_public_projection {
    std::string extension_name;
    std::string extension_schema;
    std::string script_sha256;
    std::vector<std::string> stable_identities;
    std::vector<std::string> installed_identities;
    std::vector<sql_manifest_public_entity> entities;
    std::vector<std::string> catalog_schemas;
    std::vector<std::string> restricted_sequence_names;
    // Documentation discovery never changes the original executor ACL surface.
    std::vector<sql_manifest_public_entity> documentation_entities;
    std::vector<std::string> documentation_schemas;
};

/// @brief Reads source documentation and the separate runnable ACL projection.
///
/// Requires the annotated-C++ manifest format and checks identity uniqueness
/// and schema substitution. Public types and functions must have a brief.
/// Documentation includes all SQL declarations; execution grants retain the public allowlist.
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
    std::string extension_name = "bioseq";
    if (const boost::json::value* value =
            root.if_contains("extension_name"); value != nullptr) {
        if (!value->is_string()) {
            fail("field 'extension_name' must be a string");
        }
        const boost::json::string& raw = value->as_string();
        extension_name.assign(raw.data(), raw.size());
        if (extension_name.empty()) {
            fail("extension_name must not be empty");
        }
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
    std::set<std::string, std::less<>> documentation_schemas;
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
    const auto identity_kind_for_manifest_kind =
        [](std::string_view kind) noexcept -> std::string_view {
            // The identity uses guc-value where the manifest kind uses guc_value.
            return kind == "guc_value" ? "guc-value" : kind;
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
            identity_body(
                installed_identity,
                identity_kind_for_manifest_kind(kind));
        const std::string_view installed_name =
            qualified_name(installed_body);
        if (kind == "sequence"
            && schema_name(installed_name) == "bioseq_internal") {
            restricted_sequence_names.emplace(installed_name);
        }
        const bool documentable = kind == "type" || kind == "function"
            || kind == "sql_function" || kind == "procedure"
            || kind == "aggregate" || kind == "operator" || kind == "cast"
            || kind == "schema" || kind == "table" || kind == "view"
            || kind == "sequence" || kind == "opclass"
            || kind == "access_method" || kind == "guc" || kind == "index";
        if (!documentable && !public_documentation) {
            continue;
        }
        const auto optional_string = [&entity](std::string_view key) {
            const auto* value = entity.if_contains(key);
            return value && value->is_string() ? std::string(value->as_string()) : std::string{};
        };
        std::string brief = public_documentation ? require_string(entity, "brief") : optional_string("brief");
        if (public_documentation && brief.empty()) {
            fail("website-public entity '" + stable_identity
                 + "' has no Doxygen brief");
        }
        if (public_documentation && kind != "type" && kind != "function"
            && kind != "sql_function") {
            fail("website-public entity '" + stable_identity
                 + "' has unsupported website kind '" + kind + "'");
        }
        if (public_documentation) {
            catalog_schemas.emplace(schema_name(installed_name));
        }
        if (kind == "schema") {
            documentation_schemas.emplace(installed_name);
        } else if (installed_name.find('.') != std::string_view::npos) {
            documentation_schemas.emplace(schema_name(installed_name));
        }
        std::vector<parameter_description> parameters;
        const boost::json::value empty_parameters = boost::json::array{};
        const boost::json::value& parameters_value = entity.if_contains("parameters")
            ? entity.at("parameters") : empty_parameters;
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
            if (const auto* text = parameter.if_contains("documentation");
                text && text->is_string() && !text->as_string().empty()) {
                documentation.description.push_back(content_node{
                    .kind = content_node_kind::text,
                    .text = std::string(text->as_string()),
                });
            }
            parameters.push_back(std::move(documentation));
        }
        sql_manifest_public_entity documented{
            .stable_identity = stable_identity,
            .installed_identity = installed_identity,
            .kind = kind,
            .name = optional_string("name"),
            .source_module = optional_string("source_module"),
            .source_path = optional_string("source_path"),
            .source_line = entity.if_contains("source_line") ? require_size(entity, "source_line") : 0,
            .brief = brief,
            .details = optional_string("details"),
            .result_sql_type = optional_string("result_sql_type"),
            .parameters = std::move(parameters),
        };
        documented.public_documentation = public_documentation;
        documented.catalog_identity = installed_identity;
        if (kind == "sql_function") {
            documented.catalog_identity = "sql:function:" + std::string(installed_body);
        }
        documented.qualified_name = std::string(installed_name);
        if (kind == "access_method") {
            documented.qualified_name = documented.name;
            documented.catalog_identity = "sql:access_method:" + documented.name;
        }
        if (kind == "index") {
            // Index annotations inherit a C++ owner's spelling in stable IDs;
            // their installed SQL schema is the owning table's schema.
            if (const auto* dependencies = entity.if_contains("dependencies");
                dependencies && dependencies->is_array()) {
                for (const auto& dependency : dependencies->as_array()) {
                    const auto value = resolve_extension_identity(std::string(dependency.as_string()));
                    if (!value.starts_with("sql:table:")) continue;
                    const auto table_name = std::string_view(value).substr(10);
                    documented.qualified_name = std::string(schema_name(table_name)) + "." + documented.name;
                    documented.catalog_identity = "sql:index:" + documented.qualified_name;
                    break;
                }
            }
        }
        // Aggregate manifest identities contain the C++ transition binding.
        // Resolve that declaration to its emitted SQL argument types.
        if (kind == "aggregate") {
            const auto* options = entity.if_contains("options");
            const auto* transition = options && options->is_object()
                ? options->as_object().if_contains("transition") : nullptr;
            for (const auto& candidate_value : entities_value.as_array()) {
                const auto& candidate = candidate_value.as_object();
                const auto* declaration = candidate.if_contains("cpp_declaration");
                if (!transition || !transition->is_string() || !declaration
                    || *declaration != *transition) continue;
                // The installed identity has already rendered annotation aliases
                // such as quoted_any to the actual PostgreSQL spelling ("any").
                const std::string transition_identity = require_string(candidate, "installed_identity");
                const auto argument_begin = transition_identity.find('(');
                if (argument_begin == std::string::npos || !transition_identity.ends_with(')')) continue;
                const auto arguments = bioseq::codegen::sql::parse_routine_argument_declarations(
                    std::string_view(transition_identity).substr(argument_begin + 1,
                        transition_identity.size() - argument_begin - 2));
                std::string signature = documented.qualified_name.substr(
                    0, documented.qualified_name.find(':')) + "(";
                for (std::size_t index = 1; index < arguments.size(); ++index) {
                    if (index != 1) signature += ',';
                    signature += arguments[index].sql_type;
                }
                signature += ')';
                documented.catalog_identity = "sql:aggregate:" + signature;
                documented.qualified_name = signature.substr(0, signature.find('('));
                break;
            }
        }
        for (const auto key : {"options", "routine_properties"}) {
            if (kind == "guc") continue; // Show typed setting metadata, not C++ option expressions.
            const auto* options = entity.if_contains(key);
            if (!options || !options->is_object()) continue;
            for (const auto& option : options->as_object()) {
                if (option.value().is_string() && !option.value().as_string().empty()) {
                    documented.attributes.emplace(std::string(option.key()),
                        std::string(option.value().as_string()));
                }
            }
        }
        if (const auto* condition = entity.if_contains("active_condition");
            condition && condition->is_string() && !condition->as_string().empty()) {
            documented.attributes["active_condition"] = std::string(condition->as_string());
        }
        if (const auto* columns = entity.if_contains("columns"); columns && columns->is_array()) {
            for (const auto& column_id : columns->as_array()) {
                for (const auto& column_value : entities_value.as_array()) {
                    const auto& column = column_value.as_object();
                    if (column.at("identity") != column_id) continue;
                    parameter_description field{.name = require_string(column, "name")};
                    const std::string description = require_string(column, "brief");
                    if (!description.empty()) field.description.push_back(content_node{
                        .kind = content_node_kind::text, .text = description});
                    documented.fields.push_back(std::move(field));
                    break;
                }
            }
        }
        if (const auto* result = entity.if_contains("result"); result && result->is_object()) {
            const auto* description = result->as_object().if_contains("documentation");
            if (description && description->is_string() && !description->as_string().empty()) {
                documented.additional_documentation.push_back(reference_heading("Return value", "return-value"));
                documented.additional_documentation.push_back(reference_paragraph(std::string(description->as_string())));
            }
        }
        if (const auto* fields = entity.if_contains("documentation_fields"); fields && fields->is_array()) {
            for (const auto& field : fields->as_array()) {
                const auto& item = field.as_object();
                parameter_description described{.name = require_string(item, "name"),
                    .description = {content_node{.kind = content_node_kind::text,
                        .text = require_string(item, "description")}}};
                bool replaced = false;
                for (auto& existing : documented.fields) if (existing.name == described.name) {
                    existing.description = described.description;
                    replaced = true;
                    break;
                }
                if (!replaced) documented.fields.push_back(std::move(described));
            }
        }
        const auto append_operator = [&](const boost::json::object& binding, bool implementation) {
            const auto get = [&](std::string_view key) { return require_string(binding, key); };
            documented.additional_documentation.push_back(content_node{
                .kind = content_node_kind::code_block,
                .text = get("left_type") + " " + get("symbol") + " " + get("right_type"),
                .attributes = {{"language", "sql"}}});
            const auto id = get(implementation ? "function_identity" : "operator_identity");
            const auto label = resolve_extension_identity(id.substr(id.find(':', 4) + 1));
            documented.additional_documentation.push_back(content_node{
                .kind = content_node_kind::paragraph,
                .children = {
                    content_node{.kind = content_node_kind::text,
                        .text = implementation ? "Implemented by " : "SQL operator alias: "},
                    content_node{.kind = content_node_kind::entity_link,
                        .text = label, .target = id, .attributes = {{"entity_id", id}}},
                    content_node{.kind = content_node_kind::text,
                        .text = implementation ? ". The operator and this function invoke the same implementation."
                            : ". The function and this operator invoke the same implementation."}
                }});
        };
        if (const auto* binding = entity.if_contains("operator_binding"); binding && binding->is_object()) {
            documented.additional_documentation.push_back(reference_heading("Operator", "operator"));
            append_operator(binding->as_object(), true);
        }
        if (const auto* aliases = entity.if_contains("operator_aliases"); aliases && aliases->is_array()
            && !aliases->as_array().empty()) {
            documented.additional_documentation.push_back(reference_heading("Operator aliases", "operator-aliases"));
            for (const auto& alias : aliases->as_array()) append_operator(alias.as_object(), false);
        }
        if (kind != "guc") append_documentation_examples(documented.additional_documentation,
                                      entity.if_contains("documentation_examples"));
        if (kind == "guc") {
            const auto* generation = root.if_contains("guc_generation");
            const auto* registrations = generation && generation->is_object()
                ? generation->as_object().if_contains("registrations") : nullptr;
            if (registrations && registrations->is_array()) for (const auto& item : registrations->as_array()) {
                const auto& registration = item.as_object();
                if (require_string(registration, "name") != documented.qualified_name) continue;
                const auto& authority = registration.at("generated_live_authority").as_object();
                const auto property = [&](std::string_view key, std::string_view title) {
                    const auto* value = authority.if_contains(key);
                    if (value && value->is_string()) documented.attributes[std::string(title)] = std::string(value->as_string());
                };
                property("vartype", "Type");
                property("boot_value", "Default");
                property("minimum", "Minimum");
                property("maximum", "Maximum");
                property("unit", "Unit");
                property("context", "Context");
                if (documented.attributes["Type"] == "bool") documented.attributes["Possible values"] = "on, off";
                if (const auto* values = authority.if_contains("visible_enum_values"); values && values->is_array()) {
                    std::string choices;
                    for (const auto& value : values->as_array()) {
                        if (!choices.empty()) choices += ", ";
                        choices += std::string(value.as_string());
                    }
                    if (!choices.empty()) documented.attributes["Possible values"] = std::move(choices);
                }
                if (!documented.attributes.contains("Minimum")) documented.attributes["Minimum"] = "Not applicable";
                if (!documented.attributes.contains("Maximum")) documented.attributes["Maximum"] = "Not applicable";
                documented.attributes["Visible in SHOW ALL"] = authority.at("visible_in_show_all").as_bool() ? "Yes" : "No";
                if (const auto* documentation = registration.if_contains("documentation"); documentation && documentation->is_object()) {
                    const auto& doc = documentation->as_object();
                    if (!documented.attributes.contains("Unit")) {
                        if (const auto* unit = doc.if_contains("display_unit"); unit && unit->is_string() && !unit->as_string().empty())
                            documented.attributes["Unit"] = std::string(unit->as_string());
                    }
                    if (const auto* description = doc.if_contains("description"); description && description->is_string()) {
                        const std::string configuration_description(description->as_string());
                        if (!documented.details.empty() && documented.details != configuration_description)
                            documented.additional_documentation.insert(documented.additional_documentation.begin(),
                                reference_paragraph(documented.details));
                        documented.details = configuration_description;
                    }
                    if (const auto* values = doc.if_contains("possible_values"); values && values->is_array() && !values->as_array().empty()) {
                        documented.additional_documentation.push_back(reference_heading("Possible values", "possible-values"));
                        content_node table{.kind = content_node_kind::table,
                            .attributes = {{"class", "reference-values-table"}}};
                        table.children.push_back(content_node{.kind = content_node_kind::table_row,
                            .children = {
                                content_node{.kind = content_node_kind::table_cell, .attributes = {{"scope", "col"}}, .children = {content_node{.kind = content_node_kind::text, .text = "Value"}}},
                                content_node{.kind = content_node_kind::table_cell, .attributes = {{"scope", "col"}}, .children = {content_node{.kind = content_node_kind::text, .text = "Meaning"}}}}});
                        for (const auto& value : values->as_array()) {
                            const auto& row = value.as_object();
                            std::string description = require_string(row, "description");
                            if (const auto* compiled = row.if_contains("compiled"); compiled && compiled->is_bool() && !compiled->as_bool())
                                description += " Not available in this build.";
                            if (const auto* hidden = row.if_contains("hidden"); hidden && hidden->is_bool() && hidden->as_bool())
                                description += " Hidden value.";
                            table.children.push_back(content_node{.kind = content_node_kind::table_row,
                                .children = {
                                    content_node{.kind = content_node_kind::table_cell, .children = {content_node{.kind = content_node_kind::code_span, .text = require_string(row, "value")}}},
                                    content_node{.kind = content_node_kind::table_cell, .children = {content_node{.kind = content_node_kind::text, .text = std::move(description)}}}}});
                        }
                        documented.additional_documentation.push_back(std::move(table));
                    }
                    append_documentation_examples(documented.additional_documentation, doc.if_contains("examples"));
                }
                break;
            }
        }
        // Keep the security projection byte-for-byte equivalent in membership.
        if (public_documentation) projection.entities.push_back(documented);
        projection.documentation_entities.push_back(std::move(documented));
    }
    if (projection.documentation_entities.empty()) {
        fail("no SQL entity is available for documentation");
    }
    projection.catalog_schemas.assign(
        catalog_schemas.begin(), catalog_schemas.end());
    projection.documentation_schemas.assign(
        documentation_schemas.begin(), documentation_schemas.end());
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
    case content_node_kind::raw_html: return "raw_html";
    case content_node_kind::math_inline: return "math_inline";
    case content_node_kind::math_display: return "math_display";
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
    case entity_kind::sql_object: return "sql_object";
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
    object["publication_date"] = item.metadata.publication_date;
    object["category"] = item.metadata.category;
    object["author"] = item.metadata.author;
    object["cover"] = item.metadata.cover;
    object["image_alt"] = item.metadata.image_alt;
    object["reading_minutes"] = item.metadata.reading_minutes;
    object["social_image"] = item.metadata.social_image
        ? boost::json::value(*item.metadata.social_image) : boost::json::value(nullptr);
    object["public_markdown"] = item.public_markdown;
    object["search_text"] = item.search_text;
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

/// @brief Serializes a website plan after checking its format versions.
/// @param plan Parsed tutorials and build inputs.
/// @return JSON with HTML-safe escapes and  a final newline.
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
    boost::json::array blog_posts;
    for (const tutorial& item : plan.blog_posts) {
        blog_posts.push_back(serialize_tutorial(item));
    }
    root["blog_posts"] = std::move(blog_posts);
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
    boost::json::array fields;
    for (const parameter_description& field : entity.fields) {
        boost::json::object item;
        item["name"] = field.name;
        item["type"] = field.type;
        item["default"] = field.default_value
            ? boost::json::value(*field.default_value) : boost::json::value(nullptr);
        boost::json::array description;
        for (const content_node& node : field.description) {
            description.push_back(serialize_node(node));
        }
        item["description"] = std::move(description);
        fields.push_back(std::move(item));
    }
    object["fields"] = std::move(fields);
    boost::json::array enum_values;
    for (const std::string& value : entity.enum_values) enum_values.push_back(boost::json::value(value));
    object["enum_values"] = std::move(enum_values);
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

/// @brief Serializes C++ declarations and their documentation.
/// @param entities Public entities with, structural and Doxygen data merged.
/// @param clang_version Clang version used for the structural scan.
/// @return JSON with HTML-safe escapes and a final newline.
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

/// @brief Serializes SQL entity records.
/// @param entities SQL entities for a reference index or catalog inventory.
/// @return JSON with HTML-safe escapes and a final newline.
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

// Escape text before binding it to a template. Jinja2Cpp does not escape
// values automatically, so angle brackets, ampersands, and quotes must become
// HTML entities before insertion into text or an attribute
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

/// @brief Encodes a string as JSON with escapes for embedding in HTML.
///
/// The decoded string keeps its original characters. Use  this for data or
/// already-rendered HTML. Use json_text when a template will display plain text.
/// @param value String to encode.
/// @return A double-quoted JSON string literal.
[[nodiscard]] std::string json_quote(std::string_view value) {
    return apply_html_safe_escapes(boost::json::serialize(boost::json::value(value)));
}

/// @brief HTML-escapes plain text, then encodes it as a JSON string.
///
/// Use this for text and attrhbute values that templates insert directly,
/// including titles, names, and URLs. It escapes quotes but does not check URLs.
/// @param value Plain text to display.
/// @return A double-quoted JSON string containing the HTML-escaped value.
[[nodiscard]] std::string json_text(std::string_view value) {
    return json_quote(html_escape(value));
}

[[nodiscard]] bool reference_attribute_visible(std::string_view name) {
    return name != "refid" && name != "stable_identity" && name != "installed_identity"
        && name != "documentation_authority" && name != "sql_manifest_script_sha256"
        && name != "documentation_owner"
        && name != "public_documentation" && name != "catalog_comment"
        && name != "declaration" && name != "sql_kind";
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

// Build the entity data used directly by templates. Escape text and route
// attributes here because Jinja2Cpp inserts them as supplied. Saved entity
// JSON uses serialize_entity and keeps the original text.
[[nodiscard]] boost::json::value serialize_entity_view(const reference_entity& entity) {
    boost::json::object object;
    object["canonical_id"] = html_escape(entity.canonical_id);
    object["domain"] = entity.domain == entity_domain::sql ? "sql" : "cpp";
    std::string display_kind(entity_kind_name(entity.kind));
    if (entity.domain == entity_domain::sql) {
        if (const auto kind = entity.attributes.find("sql_kind"); kind != entity.attributes.end())
            display_kind = kind->second == "guc" ? "setting" : kind->second;
        else if (display_kind.starts_with("sql_")) display_kind.erase(0,4);
    }
    object["kind"] = html_escape(display_kind);
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
    boost::json::array fields;
    for (const parameter_description& field : entity.fields) {
        boost::json::object item;
        item["name"] = html_escape(field.name);
        item["type"] = html_escape(field.type);
        item["default"] = field.default_value
            ? boost::json::value(html_escape(*field.default_value))
            : boost::json::value(nullptr);
        item["description"] = html_escape(content_plain_text(field.description));
        fields.push_back(std::move(item));
    }
    object["fields"] = std::move(fields);
    boost::json::array enum_values;
    for (const std::string& value : entity.enum_values) enum_values.push_back(boost::json::value(html_escape(value)));
    object["enum_values"] = std::move(enum_values);
    // Emit SQL attributes and C++ qualifiers in map order.
    if (!entity.attributes.empty()) {
        boost::json::array attributes;
        for (const auto& [name, value] : entity.attributes) {
            if (!reference_attribute_visible(name)) continue;
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

/// @brief Serializes an entity for use  in a page template.
/// @param entity Documented entity.
/// @return A JSON object with its text and attribute values HTML-escaped.
[[nodiscard]] std::string serialize_reference_entity(const reference_entity& entity) {
    return apply_html_safe_escapes(boost::json::serialize(serialize_entity_view(entity)));
}

/// @brief Serializes entities for an index page template.
/// @param entities Documented entities in display order.
/// @return A JSON array with text and attribute values HTML-escaped.
[[nodiscard]] std::string serialize_reference_entity_array(
    const std::vector<reference_entity>& entities) {
    boost::json::array items;
    for (const reference_entity& entity : entities) {
        items.push_back(serialize_entity_view(entity));
    }
    return apply_html_safe_escapes(
        boost::json::serialize(boost::json::value(std::move(items))));
}

/// @brief Builds the examples.<slot> data used by landing-page templates.
///
/// Each slot contains visible_sql and a table result, or null when no table
/// was captured. Columns include display types, and each cell keeps its NULL
/// flag and, text. Escape the text here because templates insert it directly
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
    if (name == "sql_object") return entity_kind::sql_object;
    return entity_kind::overload_group;
}

[[nodiscard]] std::optional<std::string> as_optional_string(const boost::json::value& value) {
    if (value.is_string()) {
        return std::string(value.get_string());
    }
    return std::nullopt;
}

// Read a node-kind name from saved JSON. Reject unknown names so an
// unsupported node cannot silently turn into another  kind of content.
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
    if (name == "raw_html") return kind::raw_html;
    if (name == "math_inline") return kind::math_inline;
    if (name == "math_display") return kind::math_display;
    throw std::runtime_error("site_model: unknown document node kind '"
                             + std::string(name) + "'");
}

[[nodiscard]] entity_query_domain domain_from_name(std::string_view name) {
    if (name == "cpp") return entity_query_domain::cpp;
    if (name == "sql") return entity_query_domain::sql;
    if (name == "canonical_id") return entity_query_domain::canonical_id;
    return entity_query_domain::any;
}

// Restore a saveb content node. Missing optional fields stay empty, matching
// the fields serialize_node omits when writing it.
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

// Read an example from a plan or site model. Plans contain setup and teardown.
// Execution clears those fields before saving the site model.
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

// Restore tutorial content, examples, and navigation links so rendering can
// use the saved model without a database.
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
    const auto optional_text = [&object](std::string_view key) {
        const auto* value = object.if_contains(key);
        return value && value->is_string() ? std::string(value->as_string()) : std::string{};
    };
    item.metadata.publication_date = optional_text("publication_date");
    if (const auto* category = object.if_contains("category")) {
        item.metadata.category = std::string(category->as_string());
        if (item.metadata.category != "bioseqdb" && item.metadata.category != "others")
            throw std::runtime_error("serialized blog category must be bioseqdb or others");
    }
    item.metadata.author = optional_text("author");
    item.metadata.cover = optional_text("cover");
    item.metadata.image_alt = optional_text("image_alt");
    item.public_markdown = optional_text("public_markdown");
    item.search_text = optional_text("search_text");
    if (const auto* minutes = object.if_contains("reading_minutes"))
        item.metadata.reading_minutes = minutes->to_number<std::uint32_t>();
    if (const auto* social = object.if_contains("social_image"))
        item.metadata.social_image = as_optional_string(*social);
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

// Restore the  entity fields written by serialize_entity.
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
    if (const boost::json::value* field_items = object.if_contains("fields");
        field_items != nullptr && field_items->is_array()) {
        for (const boost::json::value& field_item : field_items->as_array()) {
            const boost::json::object& fields = field_item.as_object();
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
            entity.fields.push_back(std::move(documented));
        }
    }
    if (const auto* values = object.if_contains("enum_values"); values && values->is_array()) {
        for (const auto& value : values->as_array()) entity.enum_values.emplace_back(value.as_string());
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

// Save a landing slot with its original result text. HTML escaping happens
// later in serialize_landing_examples when rendering the page.
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

/// @brief Reads a saved C++ or SQL entity index.
///
/// Restores documentation, parameter descriptions, and SQL-to-C++ links after
/// checking the artifact schema and revision.
/// @param text Entity-index JSON.
/// @return Entities in their stored order.
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

/// @brief Saves the site data needed to render without PostgreSQL.
///
/// Stores resolved tutorials and landing results with reference entities,
/// coverage, and build inputs. The caller must clear setup and  teardown SQL
/// before saving. Routes remain independent of the deployment base path.
/// @param model Resolved site model with execution-only data nemoved
/// @return JSON with HTML-safe escapes and a final newline.
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
    boost::json::array blog_posts;
    for (const tutorial& item : model.blog_posts) {
        blog_posts.push_back(serialize_tutorial(item));
    }
    root["blog_posts"] = std::move(blog_posts);
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

/// @brief Reads a site, model and checks its schema and processing versions.
/// @param text Saved site-model JSON.
/// @return The model used to render pages without a database.
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
         "blog_posts",
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
    if (const auto* posts = object.if_contains("blog_posts")) {
        for (const auto& item : posts->as_array())
            model.blog_posts.push_back(deserialize_tutorial(item));
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

/// @brief Serializes modules reachable through exported imports.
/// @param clang_version Clang version used for the scan.
/// @param roots Primary modules from which the scan starts.
/// @param closure Reachable modules, sorted by module name.
/// @return JSON with HTML-safe escapes and a final newline.
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

/// @brief Reads output routes from a manifest with  the expected schema.
/// @param text Manifest JSON.
/// @param expected_schema Required schema name.
/// @param expected_revision Required schema revision.
/// @return Output route keys in their stored order.
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

/// @brief Serializes the manifest for a site containing several versions.
/// @param primary_version Primary version label.
/// @param ordered_versions Version labels, newest first.
/// @param base_path Normalized base path for version routes.
/// @param input_manifest_hashes SHA-256 of each input site-manifest.json.
/// @param input_success_hashes SHA-256 of each input `<root>.success` marker.
/// @param outputs Output routes and hashes, excluding this manifest.
/// @return JSON with HTML-safe escapes and a final newline.
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

/// @brief Combines site fields and page data for a template.
///
/// Adds site, nav, and page objects, plus an optional named collection.
/// Callers prepare text escaping for template bindings before this step.
/// @param site Site labels and URLs, with  the base path applied to internal links.
/// @param page_object JSON object for the page binding.
/// @param collection_key Extra top-level key, or empty to omit it.
/// @param collection_json JSON value for that key
/// @return JSON context for render_page_template.
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

    // Metadata shares the same page inputs as visible content. PageMaps are
    // consumed by Programmable Search; JSON-LD describes the public page to
    // general search engines. Neither format invents publication dates.
    boost::json::value page_value = parse_json_value(page_object);
    if (page_value.is_object()) {
        boost::json::object& page_object_ref = page_value.as_object();
        // Template fields have been HTML-escaped by json_text. Decode only
        // that encoder's entities, once, while preserving raw structured_*
        // fields (a title can legitimately contain a literal "&lt;").
        const auto template_text = [](std::string_view text) {
            std::string result;
            for (std::size_t offset = 0; offset < text.size();) {
                bool decoded = false;
                for (const auto& [entity, replacement] :
                     std::initializer_list<std::pair<std::string_view, char>>{
                         {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'},
                         {"&quot;", '"'}, {"&#39;", '\''}}) {
                    if (text.substr(offset).starts_with(entity)) {
                        result.push_back(replacement);
                        offset += entity.size();
                        decoded = true;
                        break;
                    }
                }
                if (!decoded) result.push_back(text[offset++]);
            }
            return result;
        };
        const auto text = [&page_object_ref](std::string_view key) {
            const auto* value = page_object_ref.if_contains(key);
            return value != nullptr && value->is_string()
                ? std::string(value->as_string()) : std::string{};
        };
        const auto original = [&text, &template_text](
                                  std::string_view raw_key, std::string_view escaped_key) {
            const std::string raw = text(raw_key);
            return raw.empty() ? template_text(text(escaped_key)) : raw;
        };
        const std::string hint = text("page_type");
        const std::string title = original("structured_title", "title");
        const std::string description = original("structured_description", "description");
        const std::string canonical = template_text(text("canonical"));
        const auto origin_end = canonical.find('/', canonical.find("://") + 3);
        const std::string origin = canonical.substr(0, origin_end);
        const std::string home = origin + (site.base_path.empty() ? "/" : site.base_path);
        const auto absolute_url = [&origin, &home](std::string value) {
            if (value.starts_with("https://") || value.starts_with("http://")) return value;
            if (value.starts_with('/')) return origin + value;
            if (!value.empty()) return home + value;
            return std::string{};
        };
        std::string author = original("structured_author", "author");
        if (author.empty()) {
            author = site.copyright_holder;
            if (author.starts_with("BioseqDB ")) author.erase(0, 9);
            if (author.empty()) author = "Piotr Styczyński";
        }
        std::string category = template_text(text("category"));
        if (category.empty()) category = hint.empty() ? "documentation" : hint;
        const std::string date = text("date");
        const std::string modified_date = text("modified_date");
        std::string thumbnail = absolute_url(template_text(text("thumbnail_url")));
        if (thumbnail.empty()) thumbnail = absolute_url(template_text(text("social_image_url")));
        if (thumbnail.empty()) thumbnail = absolute_url(site.social_image_url);
        const bool is_article = hint == "blog" || hint == "tutorial" || hint == "entity";
        const std::string page_type = hint == "landing" ? "WebSite"
            : hint == "blog" ? "BlogPosting"
            : is_article ? "TechArticle"
            : hint == "qa" || hint == "faq" ? "FAQPage" : "WebPage";
        const std::string organization_id = home + "#organization";
        const std::string website_id = home + "#website";
        boost::json::object organization{
            {"@type", "Organization"}, {"@id", organization_id},
            {"name", "BioseqDB"}, {"url", home},
            {"description", "Independent open-source project developing a PostgreSQL extension for biological sequence storage and alignment. Published benchmarks report speedups of up to 120× for selected workloads and baselines."},
            {"logo", boost::json::object{{"@type", "ImageObject"},
                {"url", home + "bioseq-mark.svg"}}},
            {"sameAs", boost::json::array{
                site.repository_url, "https://www.linkedin.com/company/bioseqdb/",
                "https://www.crunchbase.com/organization/bioseq-db"}},
            {"subjectOf", boost::json::object{{"@type", "WebPage"},
                {"url", home + "benchmarks/"}, {"name", "BioseqDB benchmarks"}}}};
        boost::json::object website{
            {"@type", "WebSite"}, {"@id", website_id}, {"url", home},
            {"name", site.title}, {"inLanguage", "en"},
            {"publisher", boost::json::object{{"@id", organization_id}}}};
        boost::json::object primary;
        primary["@type"] = page_type;
        primary["@id"] = hint == "landing" ? website_id
            : canonical + (is_article ? "#article" : "#webpage");
        primary["name"] = title;
        primary["description"] = description;
        primary["url"] = canonical;
        primary["inLanguage"] = "en";
        primary["publisher"] = boost::json::object{{"@id", organization_id}};
        if (hint != "landing") primary["isPartOf"] = boost::json::object{{"@id", website_id}};
        if (!thumbnail.empty()) {
            primary["image"] = thumbnail;
            primary["thumbnailUrl"] = thumbnail;
        }
        if (!date.empty()) primary["datePublished"] = date;
        if (!modified_date.empty()) primary["dateModified"] = modified_date;
        if (is_article) {
            primary["headline"] = title;
            primary["mainEntityOfPage"] = boost::json::object{
                {"@type", "WebPage"}, {"@id", canonical}};
            primary["author"] = boost::json::object{
                {"@type", "Person"}, {"name", author}};
            if (author == "Piotr Styczyński")
                primary["author"].as_object()["url"] = "https://github.com/styczynski";
            if (const auto* keywords = page_object_ref.if_contains("structured_keywords"))
                primary["keywords"] = *keywords;
            primary["articleSection"] = category;
            if (const auto* minutes = page_object_ref.if_contains("reading_minutes"))
                primary["timeRequired"] = "PT" + std::to_string(
                    minutes->to_number<std::uint32_t>()) + "M";
        }
        if (page_type == "FAQPage") {
            // This is an editorial collection of answers, not a single
            // community question accepting user answers (QAPage).
            boost::json::array questions;
            if (const auto* entries = page_object_ref.if_contains("structured_questions")) {
                for (const auto& entry : entries->as_array()) {
                    const auto& question = entry.as_object();
                    boost::json::object answer{
                        {"@type", "Question"}, {"name", question.at("question")},
                        {"acceptedAnswer", boost::json::object{
                            {"@type", "Answer"}, {"text", question.at("answer")}}}};
                    if (const auto* href = question.if_contains("url"))
                        answer["url"] = absolute_url(std::string(href->as_string()));
                    questions.push_back(std::move(answer));
                }
            }
            primary["mainEntity"] = std::move(questions);
        }
        boost::json::array graph;
        graph.push_back(std::move(primary));
        if (hint != "landing") graph.push_back(std::move(website));
        graph.push_back(std::move(organization));
        boost::json::array crumb_items;
        const auto append_crumb = [&crumb_items](const std::string& name, const std::string& href) {
            crumb_items.push_back(boost::json::object{
                {"@type", "ListItem"},
                {"position", static_cast<std::int64_t>(crumb_items.size() + 1)},
                {"name", name}, {"item", href}});
        };
        if (page_object_ref.contains("breadcrumb") && page_object_ref.at("breadcrumb").is_array()
            && !page_object_ref.at("breadcrumb").as_array().empty()) {
            for (const boost::json::value& crumb : page_object_ref.at("breadcrumb").as_array()) {
                if (!crumb.is_object()) continue;
                const auto* name = crumb.as_object().if_contains("name");
                const auto* href = crumb.as_object().if_contains("url");
                if (name == nullptr || !name->is_string()) continue;
                append_crumb(template_text(std::string(name->as_string())),
                    href != nullptr && href->is_string()
                        ? absolute_url(template_text(std::string(href->as_string()))) : canonical);
            }
        }
        if (crumb_items.size() < 2 && canonical != home && !canonical.empty()) {
            crumb_items.clear();
            append_crumb("Home", home);
            append_crumb(title, canonical);
        }
        // A homepage has no ancestor. Do not invent a two-item trail to meet
        // Google's minimum of two breadcrumb items.
        if (crumb_items.size() >= 2)
            graph.push_back(boost::json::object{{"@type", "BreadcrumbList"},
                {"@id", canonical + "#breadcrumbs"}, {"itemListElement", std::move(crumb_items)}});
        boost::json::object json_ld{{"@context", "https://schema.org"}, {"@graph", std::move(graph)}};
        std::string structured_json = apply_html_safe_escapes(boost::json::serialize(json_ld));
        // These opening delimiters cannot occur in JSON grammar, only in
        // string values. Preserve literal editorial template examples while
        // preventing the HTML validator from mistaking them for leftovers.
        for (const std::string_view marker : {"{{", "{%", "{#"}) {
            std::size_t offset = 0;
            while ((offset = structured_json.find(marker, offset)) != std::string::npos) {
                structured_json.replace(offset, 1, "\\u007b");
                offset += 6;
            }
        }
        page_object_ref["jsonld"] = std::move(structured_json);

        // A PageMap is XML inside an HTML comment. Escaping '-' and '{' as
        // well as XML characters prevents editorial text from ending the
        // comment or resembling an unrendered template. Character references
        // restore the original XML value.
        const auto xml_comment_text = [](std::string_view value) {
            std::string escaped;
            for (const char ch : html_escape(value)) {
                if (ch == '-') escaped += "&#45;";
                else if (ch == '{') escaped += "&#123;";
                else escaped.push_back(ch);
            }
            return escaped;
        };
        std::string pagemap = "<!--\n<PageMap>\n  <DataObject type=\"document\">\n";
        const auto attribute = [&pagemap, &xml_comment_text](std::string_view name, std::string_view value) {
            pagemap += "    <Attribute name=\"" + std::string(name) + "\">"
                + xml_comment_text(value) + "</Attribute>\n";
        };
        attribute("title", title);
        attribute("description", description);
        attribute("page_count", "1"); // One HTML document; not an estimated print length.
        attribute("last_update", modified_date);
        attribute("category", category);
        attribute("author", author);
        attribute("date", date);
        pagemap += "  </DataObject>\n";
        if (!thumbnail.empty()) {
            pagemap += "  <DataObject type=\"thumbnail\">\n";
            attribute("src", thumbnail);
            // The screenshot pipeline writes every page thumbnail at this size.
            if (!text("thumbnail_url").empty()) {
                attribute("width", "1200");
                attribute("height", "630");
            }
            pagemap += "  </DataObject>\n";
        }
        pagemap += "</PageMap>\n-->";
        page_object_ref["pagemap_html"] = std::move(pagemap);
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

/// @brief Serializes entity details for link tooltips.
///
/// Each record contains a summary, signature, and route, with  parameter and
/// return information. The renderer names this file by its SHA-256 so browsers
/// can cache it until its contents change.
/// @param entities Documented entities for this, version.
/// @return JSON with HTML-safe escapes and a final newline.
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
        if (entity.canonical_id.starts_with("sql:guc:")) {
            record["kind"] = "setting";
            boost::json::object setting;
            for (const auto& key : {"Type", "Default", "Minimum", "Maximum", "Unit", "Context", "Possible values"}) {
                if (const auto field = entity.attributes.find(key); field != entity.attributes.end())
                    setting[key] = field->second;
            }
            record["setting"] = std::move(setting);
        }
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
        boost::json::array fields;
        for (const parameter_description& field : entity.fields) {
            boost::json::object item;
            item["name"] = field.name;
            item["type"] = field.type;
            item["description"] = content_plain_text(field.description);
            fields.push_back(std::move(item));
        }
        record["fields"] = std::move(fields);
        boost::json::array values;
        for (const auto& value : entity.enum_values) values.push_back(boost::json::value(value));
        record["enum_values"] = std::move(values);
        records[entity.canonical_id] = std::move(record);
    }
    root["entities"] = std::move(records);
    return serialize_canonical(root);
}

/// @brief Serializes version links for the header selector.
/// @param version_labels Version labels, newest first.
/// @param base_path Normalized base path for each version route.
/// @return JSON with HTML-safe escapes and a final newline.
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

/// @brief Converts entity IDs into a JSON array for coverage reports.
/// @param ids Entity identities in report order.
/// @return A JSON array of strings.
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
