module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.codegen.sql.renderer:manifest;

import :model;
import :cpp_facets;
import :fmgr;
import :sql;
import bioseq.codegen.sql.collector;
import bioseq.codegen.sql.graph;
import bioseq.codegen.support;

export namespace bioseq::codegen::sql {

namespace detail {

[[nodiscard]] inline std::string manifest_json_escape(
    std::string_view text) {
    std::string result;
    result.reserve(text.size() + 8);
    constexpr std::string_view digits = "0123456789abcdef";
    for (const unsigned char value : text) {
        switch (value) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (value < 0x20U) {
                    result += "\\u00";
                    result.push_back(digits[(value >> 4U) & 0x0fU]);
                    result.push_back(digits[value & 0x0fU]);
                } else {
                    result.push_back(static_cast<char>(value));
                }
        }
    }
    return result;
}

inline void write_json_string(
    std::ostringstream& output,
    std::string_view value) {
    output << '"' << manifest_json_escape(value) << '"';
}

[[nodiscard]] inline std::string manifest_repository_path(
    const std::filesystem::path& path,
    std::string_view field) {
    static constexpr std::array<std::string_view, 16>
        repository_source_roots{
            "bench",
            "bench_framework",
            "bioseq",
            "bioseq_caching",
            "build_mcp",
            "cmake",
            "codegen",
            "common",
            "config",
            "data",
            "DOCS",
            "infra",
            "overlays",
            "research",
            "vendor",
            "website",
        };
    if (path.empty()) {
        throw std::runtime_error(
            "manifest repository path '" + std::string{field}
            + "' is empty");
    }
    const std::filesystem::path normalized = path.lexically_normal();
    if (!path.is_absolute()) {
        const std::string raw = path.generic_string();
        const std::string result = normalized.generic_string();
        if (raw != result || result.empty() || result == "."
            || std::ranges::any_of(
                normalized,
                [](const std::filesystem::path& component) {
                    return component == ".." || component == ".";
                })) {
            throw std::runtime_error(
                "manifest repository path '" + std::string{field}
                + "' is not an already-normalized relative path: "
                + raw);
        }
        const std::string first =
            normalized.begin()->generic_string();
        if (first == "main") {
            throw std::runtime_error(
                "manifest repository path '" + std::string{field}
                + "' uses the forbidden main source-root prefix: "
                + raw);
        }
        return result;
    }

    if (const char* reviewed_root_raw =
            std::getenv("BIOSEQ_REVIEWED_REPOSITORY_ROOT");
        reviewed_root_raw != nullptr && reviewed_root_raw[0] != '\0') {
        const std::filesystem::path configured_root{reviewed_root_raw};
        std::error_code root_error;
        const std::filesystem::path reviewed_root =
            std::filesystem::weakly_canonical(
                configured_root, root_error);
        std::error_code directory_error;
        if (root_error
            || !std::filesystem::is_directory(
                reviewed_root, directory_error)
            || directory_error) {
            throw std::runtime_error(
                "BIOSEQ_REVIEWED_REPOSITORY_ROOT is not a directory: "
                + std::string{reviewed_root_raw});
        }

        std::error_code path_error;
        const std::filesystem::path reviewed_path =
            std::filesystem::weakly_canonical(normalized, path_error);
        const std::filesystem::path relative =
            path_error
            ? std::filesystem::path{}
            : reviewed_path.lexically_relative(reviewed_root);
        if (!relative.empty() && relative != "." && !relative.is_absolute()
            && std::ranges::none_of(
                relative,
                [](const std::filesystem::path& component) {
                    return component == ".." || component == ".";
                })) {
            const std::string first =
                relative.begin()->generic_string();
            if (std::ranges::contains(
                    repository_source_roots,
                    std::string_view{first})) {
                return relative.generic_string();
            }
        }
        throw std::runtime_error(
            "absolute manifest repository path '"
            + std::string{field}
            + "' is outside the reviewed repository source closure: "
            + path.generic_string());
    }

    std::vector<std::string> components;
    for (const std::filesystem::path& component : normalized) {
        const std::string spelling = component.generic_string();
        if (!spelling.empty() && spelling != "/") {
            components.push_back(spelling);
        }
    }
    std::optional<std::size_t> repository_source_root;
    for (std::size_t index = 0; index < components.size(); ++index) {
        if (std::ranges::contains(
                repository_source_roots,
                std::string_view{components[index]})) {
            if (index != 0 && components[index - 1] == "main") {
                throw std::runtime_error(
                    "absolute manifest repository path '"
                    + std::string{field}
                    + "' uses the forbidden main source-root prefix: "
                    + path.generic_string());
            }
            repository_source_root = index;
            break;
        }
    }
    if (!repository_source_root) {
        throw std::runtime_error(
            "absolute manifest repository path '"
            + std::string{field}
            + "' cannot be reduced to the repository source tree: "
            + path.generic_string());
    }
    std::string result;
    for (std::size_t index = *repository_source_root;
         index < components.size();
         ++index) {
        if (!result.empty()) result.push_back('/');
        result += components[index];
    }
    return result;
}

[[nodiscard]] inline const option_value* manifest_option(
    const collected_entity& entity,
    std::string_view name) noexcept {
    for (const option_value& option : entity.options) {
        if (option.name == name) return &option;
    }
    return nullptr;
}

[[nodiscard]] inline std::string manifest_leaf(
    std::string_view expression) {
    if (starts_checked_sql_string_literal(expression)) {
        return extract_checked_sql(expression, false);
    }
    while (!expression.empty()
           && (expression.back() == '}' || expression.back() == ')'
               || expression.back() == ' ')) {
        expression.remove_suffix(1);
    }
    const std::size_t separator = expression.rfind("::");
    std::string result{
        separator == std::string_view::npos
            ? expression
            : expression.substr(separator + 2)};
    if (result.size() >= 2
        && ((result.front() == '"' && result.back() == '"')
            || (result.front() == '\'' && result.back() == '\''))) {
        result = result.substr(1, result.size() - 2);
    }
    return result;
}

[[nodiscard]] inline std::string manifest_option_leaf(
    const collected_entity& entity,
    std::string_view name,
    std::string_view fallback = {}) {
    const option_value* option = manifest_option(entity, name);
    return option
        ? manifest_leaf(option->expression)
        : std::string{fallback};
}

[[nodiscard]] inline std::string manifest_enum_option(
    const collected_entity& entity,
    std::string_view name,
    std::string_view fallback = {}) {
    std::string result = manifest_option_leaf(entity, name, fallback);
    if (result == "volatile_") result = "volatile";
    return result;
}

[[nodiscard]] inline bool manifest_create_or_replace(
    const collected_entity& entity) {
    if (const option_value* option =
            manifest_option(entity, "create_or_replace")) {
        return manifest_leaf(option->expression) == "true";
    }
    return manifest_option_leaf(entity, "create") == "or_replace";
}

[[nodiscard]] inline std::string_view manifest_result_form(
    routine_result_form form) noexcept {
    switch (form) {
        case routine_result_form::scalar: return "scalar";
        case routine_result_form::void_: return "void";
        case routine_result_form::set_of: return "set_of";
        case routine_result_form::table_rows: return "table_rows";
        case routine_result_form::named_output: return "named_output";
        case routine_result_form::named_output_set:
            return "named_output_set";
    }
    return "scalar";
}

[[nodiscard]] inline std::string manifest_row_codec_converter_key(
    const collected_entity& field,
    std::size_t attribute_number) {
    const row_codec_field codec =
        make_row_codec_field(field, attribute_number);
    std::string key{"row_codec."};
    switch (codec.storage) {
        case row_codec_field::storage_kind::scalar:
            key += codec.underlying_cpp_type;
            break;
        case row_codec_field::storage_kind::oid: key += "oid"; break;
        case row_codec_field::storage_kind::timestamp_tz:
            key += "timestamptz";
            break;
        case row_codec_field::storage_kind::xid8: key += "xid8"; break;
        case row_codec_field::storage_kind::name: key += "name"; break;
        case row_codec_field::storage_kind::bytea_pointer:
            key += "bytea_pointer";
            break;
        case row_codec_field::storage_kind::text_pointer:
            key += "text_pointer";
            break;
        case row_codec_field::storage_kind::text_string:
            key += "text_string";
            break;
    }
    key += codec.nullable ? ".nullable" : ".required";
    return key;
}

[[nodiscard]] inline std::string manifest_schema_mode(
    const collected_entity& entity) {
    if (entity.kind == entity_kind::schema) return "fixed";
    if (entity.kind == entity_kind::comment
        || entity.kind == entity_kind::session_setting
        || entity.kind == entity_kind::guc
        || entity.kind == entity_kind::guc_value
        || entity.kind == entity_kind::phase) {
        return "none";
    }
    if (entity.companion) {
        return "enclosing";
    }
    if (entity.schema == "$extension") return "extension";
    if (entity.schema == "pg_catalog") return "pg_catalog";
    return "fixed";
}

[[nodiscard]] inline bool manifest_routine_kind(
    entity_kind kind) noexcept {
    return kind == entity_kind::function
        || kind == entity_kind::sql_function
        || kind == entity_kind::procedure;
}

[[nodiscard]] inline std::string manifest_display_spelling(
    const normalized_entity& entity) {
    const std::string prefix =
        "sql:" + std::string{kind_name(entity.source.kind)} + ":";
    return entity.installed_identity.starts_with(prefix)
        ? entity.installed_identity.substr(prefix.size())
        : entity.installed_identity;
}

[[nodiscard]] inline std::string manifest_routine_identity(
    const normalized_entity& entity) {
    return fmgr_detail::relocatable_routine_identity(entity);
}

[[nodiscard]] inline std::string manifest_routine_language(
    const collected_entity& entity) {
    return entity.kind == entity_kind::sql_function ? "sql" : "c";
}

[[nodiscard]] inline std::string manifest_c_symbol(
    const collected_entity& entity) {
    return manifest_option_leaf(entity, "c_symbol", entity.name);
}

[[nodiscard]] inline bool handwritten_fmgr_export_symbol(
    std::string_view symbol) {
    return symbol == "bioseq_external_handler";
}

[[nodiscard]] inline bool reviewed_handwritten_fmgr_export_routine(
    const normalized_entity& entity,
    std::string_view routine_identity,
    std::string_view symbol) {
    if (!handwritten_fmgr_export_symbol(symbol)) return false;
    if (!fmgr_detail::reviewed_handwritten_fmgr_export_routine(entity)
        || routine_identity
            != "bioseq.bioseq_external_handler(internal)") {
        throw std::runtime_error(
            "the reviewed external fmgr symbol '" + std::string{symbol}
            + "' is owned by an unexpected natural SQL routine");
    }
    return true;
}

[[nodiscard]] inline std::string routine_identity_leaf(
    std::string_view identity) {
    const std::size_t arguments = identity.find('(');
    const std::string_view name =
        arguments == std::string_view::npos
        ? identity
        : identity.substr(0, arguments);
    const std::size_t separator = name.rfind('.');
    return std::string{
        separator == std::string_view::npos
            ? name
            : name.substr(separator + 1)};
}

struct c_symbol_inventory_entry {
    std::string symbol;
    std::string finfo_symbol;
    std::vector<std::string> routine_identities;
    std::vector<std::string> implementation_declarations;
    bool generated_wrapper{};
};

struct routine_inventory {
    std::vector<std::string> c_routine_identities;
    std::vector<std::string> link_symbols;
    std::vector<std::string> finfo_symbols;
    std::vector<c_symbol_inventory_entry> symbol_entries;
    std::map<std::string, std::vector<std::string>, std::less<>>
        routine_identities_by_symbol;
    std::size_t natural_wrapper_symbol_count{};
    std::size_t sharing_group_count{};
};

struct fmgr_ownership_inventory {
    std::vector<std::string> generated_wrapper_symbols;
    std::vector<std::string> handwritten_fmgr_export_symbols;
    std::vector<std::string>
        handwritten_fmgr_export_external_symbols;
};

[[nodiscard]] inline routine_inventory build_routine_inventory(
    const sql_graph& graph) {
    struct owner_record {
        const normalized_entity* entity{};
        std::string routine_identity;
    };
    std::map<std::string, std::vector<owner_record>, std::less<>>
        owners_by_symbol;
    std::set<std::string, std::less<>> all_routine_identities;
    for (const normalized_entity& entity : graph.entities) {
        if (!manifest_routine_kind(entity.source.kind)) continue;
        const std::string routine_identity =
            manifest_routine_identity(entity);
        if (routine_identity.empty()
            || !all_routine_identities.insert(routine_identity).second) {
            throw std::runtime_error(
                routine_identity.empty()
                    ? "SQL routine manifest identity is empty"
                    : "duplicate SQL routine manifest identity '"
                          + routine_identity + "'");
        }
        if (manifest_routine_language(entity.source) != "c") continue;
        const std::string symbol = manifest_c_symbol(entity.source);
        if (symbol.empty() || symbol == "none") {
            throw std::runtime_error(
                "C-language SQL routine '" + routine_identity
                + "' has no linked C symbol");
        }
        if (symbol != entity.source.name
            || routine_identity_leaf(routine_identity)
                != entity.source.name) {
            throw std::runtime_error(
                "natural C routine '" + routine_identity
                + "' must derive its SQL leaf and C symbol from '"
                + entity.source.name + "'");
        }
        // The external table access method uses its own handwritten POSTGRES_FN
        // wrapper. Leave it out of the main extension's generated-wrapper inventory.
        if (reviewed_handwritten_fmgr_export_routine(
                entity, routine_identity, symbol)) {
            continue;
        }
        owners_by_symbol[symbol].push_back({
            .entity = &entity,
            .routine_identity = routine_identity,
        });
    }

    routine_inventory result;
    for (auto& [symbol, owners] : owners_by_symbol) {
        c_symbol_inventory_entry entry{
            .symbol = symbol,
            .finfo_symbol = "pg_finfo_" + symbol,
        };
        std::set<std::string, std::less<>> natural_implementations;
        for (const owner_record& owner : owners) {
            entry.routine_identities.push_back(owner.routine_identity);
            entry.implementation_declarations.push_back(
                owner.entity->source.cpp_qualified_name);
            entry.generated_wrapper = true;
            natural_implementations.insert(
                owner.entity->source.cpp_qualified_name);
        }
        if (natural_implementations.size() > 1) {
            throw std::runtime_error(
                "linked C symbol '" + symbol
                + "' has more than one natural implementation declaration");
        }
        std::ranges::sort(entry.routine_identities);
        std::ranges::sort(entry.implementation_declarations);
        entry.implementation_declarations.erase(
            std::unique(
                entry.implementation_declarations.begin(),
                entry.implementation_declarations.end()),
            entry.implementation_declarations.end());
        result.c_routine_identities.insert(
            result.c_routine_identities.end(),
            entry.routine_identities.begin(),
            entry.routine_identities.end());
        result.link_symbols.push_back(symbol);
        result.finfo_symbols.push_back(entry.finfo_symbol);
        result.routine_identities_by_symbol.emplace(
            symbol, entry.routine_identities);
        if (entry.generated_wrapper) {
            ++result.natural_wrapper_symbol_count;
        }
        if (entry.routine_identities.size() > 1) {
            ++result.sharing_group_count;
        }
        result.symbol_entries.push_back(std::move(entry));
    }
    std::ranges::sort(result.c_routine_identities);
    return result;
}

[[nodiscard]] inline fmgr_ownership_inventory
build_fmgr_ownership_inventory(
    const sql_graph& graph,
    const routine_inventory& routines,
    const std::vector<rendered_fmgr_entrypoint_module>&
        fmgr_modules,
    bool validate_reviewed_target) {
    const std::set<std::string, std::less<>> target_symbols{
        routines.link_symbols.begin(), routines.link_symbols.end()};
    std::set<std::string, std::less<>> expected_generated_symbols;
    for (const c_symbol_inventory_entry& entry :
         routines.symbol_entries) {
        if (entry.generated_wrapper) {
            expected_generated_symbols.insert(entry.symbol);
        }
    }
    const std::vector<std::string> emitted_symbols =
        emitted_fmgr_wrapper_symbols(fmgr_modules);
    const std::set<std::string, std::less<>> generated_symbols{
        emitted_symbols.begin(), emitted_symbols.end()};
    if (generated_symbols != expected_generated_symbols) {
        throw std::runtime_error(
            "emitted fmgr wrapper artifacts do not exactly match "
            "the normalized natural C routine owners");
    }

    std::set<std::string, std::less<>>
        handwritten_fmgr_export_symbols;
    std::set<std::string, std::less<>>
        handwritten_fmgr_export_external_symbols;
    for (const handwritten_fmgr_export_owner& owner :
         graph.handwritten_fmgr_export_owners) {
        if (target_symbols.contains(owner.symbol)) {
            handwritten_fmgr_export_symbols.insert(owner.symbol);
        } else {
            handwritten_fmgr_export_external_symbols.insert(owner.symbol);
        }
    }
    const std::set<std::string, std::less<>> expected_external_symbols{
        "bioseq_external_handler"};
    if (validate_reviewed_target
        && handwritten_fmgr_export_external_symbols
            != expected_external_symbols) {
        throw std::runtime_error(
            "production handwritten fmgr export scan must contain exactly "
            "the reviewed bioseq_external handler outside the main "
            "extension");
    }

    std::vector<std::string> overlapping_symbols;
    std::ranges::set_intersection(
        generated_symbols,
        handwritten_fmgr_export_symbols,
        std::back_inserter(overlapping_symbols));
    if (!overlapping_symbols.empty()) {
        throw std::runtime_error(
            "generated and handwritten fmgr export ownership overlap at "
            "symbol '"
            + overlapping_symbols.front() + "'");
    }
    std::set<std::string, std::less<>> owned_main_symbols =
        generated_symbols;
    owned_main_symbols.insert(
        handwritten_fmgr_export_symbols.begin(),
        handwritten_fmgr_export_symbols.end());
    if (owned_main_symbols != target_symbols) {
        throw std::runtime_error(
            "generated plus handwritten fmgr export ownership does not "
            "exactly cover "
            + std::string{
                validate_reviewed_target
                    ? "the reviewed profile-specific main-extension closure"
                    : "the normalized C-routine closure"});
    }

    return {
        .generated_wrapper_symbols = {
            generated_symbols.begin(), generated_symbols.end()},
        .handwritten_fmgr_export_symbols = {
            handwritten_fmgr_export_symbols.begin(),
            handwritten_fmgr_export_symbols.end()},
        .handwritten_fmgr_export_external_symbols = {
            handwritten_fmgr_export_external_symbols.begin(),
            handwritten_fmgr_export_external_symbols.end()},
    };
}

[[nodiscard]] inline std::string manifest_null_input(
    const collected_entity& entity) {
    if (const option_value* option =
            manifest_option(entity, "null_input")) {
        const std::string policy =
            manifest_enum_option(entity, "null_input");
        if (policy == "strict" || policy == "called") {
            return policy;
        }
        if (policy != "derived") {
            throw std::runtime_error(
                "function null_input must be derived, strict, or called");
        }
    }
    const std::vector<const routine_parameter*> parameters =
        sql_parameters(entity);
    const bool strict = !parameters.empty()
        && std::ranges::none_of(
            parameters,
            [](const routine_parameter* parameter) {
                return parameter->nullable;
            });
    return strict ? "strict" : "called";
}

[[nodiscard]] inline std::string documented_parameter_text(
    const collected_entity& entity,
    std::string_view parameter_name) {
    for (const auto& parameter : entity.documentation.parameters) {
        if (parameter.name == parameter_name) return parameter.text;
    }
    return {};
}

[[nodiscard]] inline bool constraint_kind(entity_kind kind) noexcept {
    return kind == entity_kind::primary_key
        || kind == entity_kind::unique
        || kind == entity_kind::foreign_key
        || kind == entity_kind::check;
}

[[nodiscard]] inline std::vector<std::string> child_identities(
    const sql_graph& graph,
    const normalized_entity& owner,
    bool columns) {
    std::vector<std::string> result;
    for (const normalized_entity& candidate : graph.entities) {
        const bool selected = columns
            ? candidate.source.kind == entity_kind::column
            : constraint_kind(candidate.source.kind);
        if (selected
            && candidate.source.enclosing_cpp_name
                == owner.source.cpp_qualified_name) {
            result.push_back(candidate.stable_identity);
        }
    }
    std::ranges::sort(result);
    return result;
}

[[nodiscard]] inline std::vector<std::string> relationship_identities(
    const sql_graph& graph,
    const normalized_entity& owner) {
    std::vector<std::string> result = owner.dependency_identities;
    for (const normalized_entity& candidate : graph.entities) {
        if (candidate.stable_identity == owner.stable_identity) continue;
        const bool depends_on_owner = std::ranges::contains(
            candidate.dependency_identities,
            owner.stable_identity);
        const bool same_cpp_owner =
            candidate.source.enclosing_cpp_name
                == owner.source.cpp_qualified_name
            || owner.source.enclosing_cpp_name
                == candidate.source.cpp_qualified_name;
        if (depends_on_owner || same_cpp_owner) {
            result.push_back(candidate.stable_identity);
        }
    }
    std::ranges::sort(result);
    result.erase(
        std::unique(result.begin(), result.end()),
        result.end());
    return result;
}

struct manifest_documentation_example {
    std::string description;
    std::string sql;
};

[[nodiscard]] inline manifest_documentation_example documentation_example(
    std::string_view source) {
    manifest_documentation_example result;
    bool description = true;
    std::istringstream input{std::string{source}};
    for (std::string line; std::getline(input, line);) {
        const std::size_t begin = line.find_first_not_of(" \t\r");
        const std::string_view trimmed = begin == std::string::npos
            ? std::string_view{} : std::string_view{line}.substr(begin);
        if (description && trimmed.starts_with("--")) {
            std::string_view sentence = trimmed.substr(2);
            const std::size_t first = sentence.find_first_not_of(" \t");
            if (first != std::string_view::npos) {
                if (!result.description.empty()) result.description += ' ';
                result.description += sentence.substr(first);
            }
        } else if (!trimmed.empty() || !description) {
            description = false;
            if (!result.sql.empty()) result.sql += '\n';
            result.sql += line;
        }
    }
    return result;
}

[[nodiscard]] inline const normalized_entity* operator_implementation(
    const sql_graph& graph, const normalized_entity& operation) {
    const auto operands = sql_parameters(operation.source);
    const normalized_entity* implementation = nullptr;
    for (const normalized_entity& candidate : graph.entities) {
        if (candidate.source.kind != entity_kind::function
            || candidate.source.cpp_qualified_name
                != operation.source.cpp_qualified_name) continue;
        const auto parameters = sql_parameters(candidate.source);
        if (parameters.size() != operands.size()) continue;
        bool same_signature = true;
        for (std::size_t index = 0; index < parameters.size(); ++index) {
            same_signature = same_signature
                && parameters[index]->sql_type == operands[index]->sql_type;
        }
        if (!same_signature) continue;
        if (implementation != nullptr) {
            throw std::runtime_error("ambiguous operator implementation for "
                + operation.stable_identity);
        }
        implementation = &candidate;
    }
    return implementation;
}

inline void write_operator_binding(std::ostringstream& output,
    const normalized_entity& operation,
    const normalized_entity& implementation) {
    const auto operands = sql_parameters(operation.source);
    output << "{\"function_identity\": ";
    write_json_string(output, implementation.stable_identity);
    output << ", \"operator_identity\": ";
    write_json_string(output, operation.stable_identity);
    output << ", \"symbol\": ";
    write_json_string(output, operation.source.name);
    output << ", \"left_type\": ";
    write_json_string(output, operands.size() == 2 ? operands[0]->sql_type : "");
    output << ", \"right_type\": ";
    write_json_string(output, operands.empty() ? "" : operands.back()->sql_type);
    output << '}';
}

inline void write_string_array(
    std::ostringstream& output,
    const std::vector<std::string>& values,
    std::string_view indentation) {
    output << '[';
    for (std::size_t index = 0; index < values.size(); ++index) {
        output << (index == 0 ? "\n" : ",\n") << indentation;
        write_json_string(output, values[index]);
    }
    if (!values.empty()) {
        output << '\n'
               << indentation.substr(
                      0, indentation.size() >= 2
                          ? indentation.size() - 2
                          : 0);
    }
    output << ']';
}

[[nodiscard]] inline const normalized_entity&
manifest_statement_owner(
    const sql_graph& graph,
    const rendered_statement& statement) {
    const auto owner = std::ranges::find(
        graph.entities,
        statement.entity_identity,
        &normalized_entity::stable_identity);
    if (owner == graph.entities.end()) {
        throw std::runtime_error(
            "rendered statement '" + statement.id
            + "' references unknown graph owner '"
            + statement.entity_identity + "'");
    }
    return *owner;
}

}  // namespace detail

[[nodiscard]] inline std::string render_graph_manifest(
    const sql_graph& graph,
    const rendered_sql_artifact& artifact,
    const std::vector<rendered_fmgr_entrypoint_module>&
        fmgr_modules,
    const std::vector<std::pair<std::string, std::string>>&
        entity_handle_owners,
    const std::vector<std::pair<std::string, std::string>>&
        row_codec_owners,
    bool validate_reviewed_fmgr_target = false) {
    std::size_t routine_entity_count = 0;
    std::map<std::string, std::size_t, std::less<>> entity_counts_by_kind;
    for (const normalized_entity& entity : graph.entities) {
        const std::string_view kind = kind_name(entity.source.kind);
        ++entity_counts_by_kind[std::string{kind}];
        if (kind == "function" || kind == "sql_function"
            || kind == "procedure") {
            ++routine_entity_count;
        }
    }
    const detail::routine_inventory routine_inventory =
        detail::build_routine_inventory(graph);
    const detail::fmgr_ownership_inventory fmgr_ownership_inventory =
        detail::build_fmgr_ownership_inventory(
            graph,
            routine_inventory,
            fmgr_modules,
            validate_reviewed_fmgr_target);
    const std::set<std::string, std::less<>>
        actual_generated_symbols{
            fmgr_ownership_inventory.generated_wrapper_symbols.begin(),
            fmgr_ownership_inventory.generated_wrapper_symbols.end()};
    std::set<std::string, std::less<>>
        actual_generated_identities;
    std::map<
        std::string,
        const rendered_fmgr_overload*,
        std::less<>>
        generated_overload_by_identity;
    for (const rendered_fmgr_entrypoint_module& module : fmgr_modules) {
        for (const rendered_fmgr_wrapper& wrapper : module.wrappers) {
            for (const rendered_fmgr_overload& overload :
                 wrapper.overloads) {
                actual_generated_identities.insert(
                    overload.sql_identity);
                const auto [position, inserted] =
                    generated_overload_by_identity.emplace(
                        overload.sql_identity, &overload);
                if (!inserted) {
                    throw std::runtime_error(
                        "generated fmgr overload identity is repeated in "
                        "the structured manifest projection: "
                        + overload.sql_identity);
                }
            }
        }
    }
    const std::vector<rendered_entity_handle_module>
        entity_handle_modules = render_entity_handle_modules(
            graph, entity_handle_owners);
    std::size_t entity_handle_count = 0;
    std::map<
        std::string,
        std::pair<
            const rendered_entity_handle_module*,
            const rendered_entity_handle*>,
        std::less<>>
        entity_handles_by_identity;
    for (const rendered_entity_handle_module& module :
         entity_handle_modules) {
        entity_handle_count += module.handles.size();
        for (const rendered_entity_handle& handle : module.handles) {
            entity_handles_by_identity.emplace(
                handle.entity_identity,
                std::pair{&module, &handle});
        }
    }
    const std::vector<rendered_row_codec_module>
        row_codec_modules = render_row_codec_modules(
            graph, row_codec_owners);
    std::size_t row_codec_count = 0;
    std::map<
        std::string,
        std::pair<
            const rendered_row_codec_module*,
            std::string_view>,
        std::less<>>
        row_codecs_by_identity;
    std::map<std::string, std::string, std::less<>>
        row_identity_by_cpp_name;
    for (const normalized_entity& entity : graph.entities) {
        if (entity.source.kind == entity_kind::table
            || entity.source.kind == entity_kind::type) {
            row_identity_by_cpp_name.emplace(
                entity.source.cpp_qualified_name,
                entity.stable_identity);
        }
    }
    for (const rendered_row_codec_module& module :
         row_codec_modules) {
        row_codec_count += module.row_cpp_names.size();
        for (const std::string& row_cpp_name :
             module.row_cpp_names) {
            const auto identity =
                row_identity_by_cpp_name.find(row_cpp_name);
            if (identity == row_identity_by_cpp_name.end()) {
                throw std::runtime_error(
                    "generated row codec '" + row_cpp_name
                    + "' has no normalized row identity");
            }
            row_codecs_by_identity.emplace(
                identity->second,
                std::pair{&module, std::string_view{row_cpp_name}});
        }
    }
    std::map<std::string, std::vector<std::string>, std::less<>>
        statements_by_entity;
    for (const rendered_statement& statement : artifact.statements) {
        statements_by_entity[statement.entity_identity].push_back(
            statement.id);
    }
    std::vector<std::string> public_stable_identities;
    std::vector<std::string> public_installed_identities;
    for (const normalized_entity& entity : graph.entities) {
        if (!entity.source.public_documentation) continue;
        public_stable_identities.push_back(entity.stable_identity);
        public_installed_identities.push_back(entity.installed_identity);
    }
    std::ranges::sort(public_stable_identities);
    std::ranges::sort(public_installed_identities);
    std::ostringstream output;
    output << "{\n"
           << "  \"schema_revision\": 1,\n"
           << "  \"authority_mode\": \"annotated_cpp\",\n"
           << "  \"compiler_profile\": ";
    detail::write_json_string(output, graph.compiler_profile);
    output << ",\n  \"clang_version\": ";
    detail::write_json_string(output, graph.clang_release);
    output << ",\n  \"compilation_signature\": ";
    detail::write_json_string(
        output, graph.compilation_signature);
    if (graph.extension_name != "bioseq"
        || graph.generated_module_prefix != "bioseq.postgres.sql.generated"
        || graph.fmgr_boundary_module
            != "bioseq.postgres.support.sql_boundary") {
        output << ",\n"
               << "  \"extension_name\": ";
        detail::write_json_string(output, graph.extension_name);
    }
    output << ",\n"
           << "  \"extension_schema\": ";
    detail::write_json_string(output, graph.extension_schema);
    if (graph.extension_name != "bioseq"
        || graph.generated_module_prefix != "bioseq.postgres.sql.generated"
        || graph.fmgr_boundary_module
            != "bioseq.postgres.support.sql_boundary") {
        output << ",\n  \"generated_module_prefix\": ";
        detail::write_json_string(
            output, graph.generated_module_prefix);
        output << ",\n  \"fmgr_boundary_module\": ";
        detail::write_json_string(
            output, graph.fmgr_boundary_module);
    }
    output << ",\n  \"script_sha256\": ";
    detail::write_json_string(output, bioseq::common::sha256_hex(artifact.sql));
    output << ",\n  \"statement_count\": " << artifact.statements.size()
           << ",\n  \"statements\": [";
    for (std::size_t index = 0;
         index < artifact.statements.size();
         ++index) {
        const rendered_statement& statement =
            artifact.statements[index];
        const normalized_entity& owner =
            detail::manifest_statement_owner(graph, statement);
        output << (index == 0 ? "\n" : ",\n")
               << "    {\"statement_id\": ";
        detail::write_json_string(output, statement.id);
        output << ", \"kind\": ";
        detail::write_json_string(
            output, kind_name(statement.statement_kind));
        output << ", \"owner_identity\": ";
        detail::write_json_string(
            output, statement.entity_identity);
        output << ", \"text_sha256\": ";
        detail::write_json_string(
            output, bioseq::common::sha256_hex(statement.text));
        output << ", \"source_module\": ";
        detail::write_json_string(output, owner.source.source_module);
        output << ", \"source_path\": ";
        detail::write_json_string(
            output,
            detail::manifest_repository_path(
                owner.source.source_path, "statements.source_path"));
        output << ", \"source_sha256\": ";
        detail::write_json_string(output, owner.source.source_sha256);
        output << ", \"source_line\": "
               << owner.source.source_range.begin.line << "}";
    }
    if (!artifact.statements.empty()) output << '\n' << "  ";
    output << "]"
           << ",\n  \"entity_count\": " << graph.entities.size()
           << ",\n  \"public_entity_count\": "
           << public_stable_identities.size()
           << ",\n  \"public_stable_identities\": ";
    detail::write_string_array(
        output, public_stable_identities, "    ");
    output << ",\n  \"public_installed_identities\": ";
    detail::write_string_array(
        output, public_installed_identities, "    ");
    output
           << ",\n  \"routine_entity_count\": " << routine_entity_count
           << ",\n  \"c_routine_identity_count\": "
           << routine_inventory.c_routine_identities.size()
           << ",\n  \"c_link_symbol_inventory_count\": "
           << routine_inventory.link_symbols.size()
           << ",\n  \"c_finfo_symbol_inventory_count\": "
           << routine_inventory.finfo_symbols.size()
           << ",\n  \"c_wrapper_entity_count\": "
           << routine_inventory.natural_wrapper_symbol_count
           << ",\n  \"generated_wrapper_symbol_count\": "
           << fmgr_ownership_inventory
                  .generated_wrapper_symbols.size()
           << ",\n  \"handwritten_fmgr_export_symbol_count\": "
           << fmgr_ownership_inventory
                  .handwritten_fmgr_export_symbols.size()
           << ",\n  \"handwritten_fmgr_export_external_symbol_count\": "
           << fmgr_ownership_inventory
                  .handwritten_fmgr_export_external_symbols.size()
           << ",\n  \"handwritten_fmgr_export_owner_count\": "
           << graph.handwritten_fmgr_export_owners.size()
           << ",\n  \"c_symbol_sharing_group_count\": "
           << routine_inventory.sharing_group_count
           << ",\n  \"generated_cpp_profile_invariant\": true"
           << ",\n  \"entity_handle_count\": "
           << entity_handle_count
           << ",\n  \"entity_handle_module_count\": "
           << entity_handle_modules.size()
           << ",\n  \"entity_handle_modules\": [";
    for (std::size_t index = 0;
         index < entity_handle_modules.size();
         ++index) {
        const rendered_entity_handle_module& module =
            entity_handle_modules[index];
        output << (index == 0 ? "\n" : ",\n")
               << "    {\"owner_key\": ";
        detail::write_json_string(output, module.owner_key);
        output << ", \"source_module\": ";
        detail::write_json_string(output, module.source_module);
        output << ", \"module_name\": ";
        detail::write_json_string(output, module.module_name);
        output << ", \"cpp_namespace\": ";
        detail::write_json_string(output, module.cpp_namespace);
        output << ", \"profile_invariant\": "
               << (module.profile_invariant ? "true" : "false")
               << ", \"entity_identities\": [";
        for (std::size_t handle = 0;
             handle < module.handles.size();
             ++handle) {
            if (handle != 0) output << ", ";
            detail::write_json_string(
                output, module.handles[handle].entity_identity);
        }
        output << "]}";
    }
    if (!entity_handle_modules.empty()) output << '\n' << "  ";
    output << "]"
           << ",\n  \"row_codec_count\": "
           << row_codec_count
           << ",\n  \"row_codec_module_count\": "
           << row_codec_modules.size()
           << ",\n  \"row_codec_modules\": [";
    for (std::size_t index = 0;
         index < row_codec_modules.size();
         ++index) {
        const rendered_row_codec_module& module =
            row_codec_modules[index];
        output << (index == 0 ? "\n" : ",\n")
               << "    {\"owner_key\": ";
        detail::write_json_string(output, module.owner_key);
        output << ", \"source_module\": ";
        detail::write_json_string(output, module.source_module);
        output << ", \"module_name\": ";
        detail::write_json_string(output, module.module_name);
        output << ", \"cpp_namespace\": ";
        detail::write_json_string(output, module.cpp_namespace);
        output << ", \"profile_invariant\": "
               << (module.profile_invariant ? "true" : "false")
               << ", \"descriptor_sha256\": ";
        detail::write_json_string(
            output, bioseq::common::sha256_hex(module.content));
        output << ", \"row_cpp_names\": ";
        detail::write_string_array(
            output, module.row_cpp_names, "      ");
        std::vector<std::string> row_entity_identities;
        row_entity_identities.reserve(module.row_cpp_names.size());
        for (const std::string& row_cpp_name :
             module.row_cpp_names) {
            row_entity_identities.push_back(
                row_identity_by_cpp_name.at(row_cpp_name));
        }
        output << ", \"row_entity_identities\": ";
        detail::write_string_array(
            output, row_entity_identities, "      ");
        output << "}";
    }
    if (!row_codec_modules.empty()) output << '\n' << "  ";
    std::size_t routine_boundary_contract_count = 0;
    for (const normalized_entity& entity : graph.entities) {
        if (!detail::manifest_routine_kind(entity.source.kind)
            || detail::manifest_routine_language(entity.source) != "c") {
            continue;
        }
        const std::string identity =
            detail::manifest_routine_identity(entity);
        if (generated_overload_by_identity.contains(identity)
            || detail::reviewed_handwritten_fmgr_export_routine(
                entity,
                identity,
                detail::manifest_c_symbol(entity.source))) {
            ++routine_boundary_contract_count;
        }
    }
    output << "]"
           << ",\n  \"routine_boundary_contract_count\": "
           << routine_boundary_contract_count
           << ",\n  \"routine_boundary_contracts\": [";
    std::size_t boundary_contract_index = 0;
    for (const normalized_entity& entity : graph.entities) {
        if (!detail::manifest_routine_kind(entity.source.kind)
            || detail::manifest_routine_language(entity.source) != "c") {
            continue;
        }
        const std::string identity =
            detail::manifest_routine_identity(entity);
        const auto overload =
            generated_overload_by_identity.find(identity);
        const bool generated =
            overload != generated_overload_by_identity.end();
        if (!generated
            && !detail::reviewed_handwritten_fmgr_export_routine(
                entity,
                identity,
                detail::manifest_c_symbol(entity.source))) {
            throw std::runtime_error(
                "C routine has no generated or reviewed external boundary "
                "contract: " + identity);
        }
        output << (boundary_contract_index++ == 0 ? "\n" : ",\n")
               << "    {\"routine_identity\": ";
        detail::write_json_string(output, identity);
        output << ", \"boundary_owner\": ";
        detail::write_json_string(
            output,
            generated ? "generated" : "handwritten_fmgr_export");
        output << ", \"cpp_declaration\": ";
        detail::write_json_string(
            output, entity.source.cpp_qualified_name);
        output << ", \"source_path\": ";
        detail::write_json_string(
            output,
            detail::manifest_repository_path(
                entity.source.source_path,
                "routine_boundary_contracts.source_path"));
        output << ", \"source_line\": "
               << entity.source.source_range.begin.line
               << ", \"parameters\": [";
        std::size_t sql_ordinal = 0;
        for (std::size_t ordinal = 0;
             ordinal < entity.source.parameters.size();
             ++ordinal) {
            const routine_parameter& parameter =
                entity.source.parameters[ordinal];
            output << (ordinal == 0 ? "\n" : ",\n")
                   << "      {\"cpp_ordinal\": " << ordinal
                   << ", \"cpp_name\": ";
            detail::write_json_string(output, parameter.name);
            output << ", \"cpp_type\": ";
            detail::write_json_string(output, parameter.cpp_type);
            output << ", \"injected_call_context\": "
                   << (parameter.injected_call_context
                           ? "true"
                           : "false")
                   << ", \"sql_ordinal\": ";
            if (parameter.injected_call_context) {
                output << "null";
            } else {
                output << sql_ordinal;
            }
            output << ", \"sql_name\": ";
            if (parameter.injected_call_context) {
                output << "null";
            } else {
                detail::write_json_string(output, parameter.name);
            }
            output << ", \"sql_type\": ";
            if (parameter.injected_call_context) {
                output << "null";
            } else {
                detail::write_json_string(output, parameter.sql_type);
            }
            output << ", \"nullable\": "
                   << (parameter.nullable ? "true" : "false")
                   << ", \"has_default\": "
                   << (parameter.has_default ? "true" : "false")
                   << ", \"converter_key\": ";
            if (parameter.injected_call_context) {
                detail::write_json_string(
                    output, "injected:function_call_context");
            } else if (!generated) {
                detail::write_json_string(
                    output, "handwritten_fmgr_export");
                ++sql_ordinal;
            } else {
                if (sql_ordinal
                    >= overload->second->argument_converter_keys.size()) {
                    throw std::runtime_error(
                        "generated fmgr overload has too few converter "
                        "keys for " + identity);
                }
                detail::write_json_string(
                    output,
                    overload->second
                        ->argument_converter_keys[sql_ordinal]);
                ++sql_ordinal;
            }
            output << "}";
        }
        if (!entity.source.parameters.empty()) {
            output << '\n' << "    ";
        }
        if (generated
            && sql_ordinal
                != overload->second->argument_converter_keys.size()) {
            throw std::runtime_error(
                "generated fmgr overload converter arity disagrees with "
                "the structured boundary contract for " + identity);
        }
        output << "], \"result\": {\"form\": ";
        detail::write_json_string(
            output,
            detail::manifest_result_form(entity.source.result.form));
        output << ", \"cpp_type\": ";
        detail::write_json_string(
            output, entity.source.result.cpp_type);
        output << ", \"sql_type\": ";
        detail::write_json_string(
            output, entity.source.result_sql_type);
        output << ", \"nullable\": "
               << (entity.source.cpp_return_type.contains("optional<")
                       ? "true"
                       : "false")
               << ", \"converter_key\": ";
        if (generated) {
            detail::write_json_string(
                output, overload->second->result_converter_key);
        } else {
            detail::write_json_string(
                output, "handwritten_fmgr_export");
        }
        output << ", \"row_cpp_name\": ";
        if (entity.source.result.row_cpp_name.empty()) {
            output << "null";
        } else {
            detail::write_json_string(
                output, entity.source.result.row_cpp_name);
        }
        output << "}}";
    }
    if (boundary_contract_index != routine_boundary_contract_count) {
        throw std::runtime_error(
            "structured routine boundary projection count disagrees with "
            "the normalized C routine inventory");
    }
    if (boundary_contract_index != 0) output << '\n' << "  ";
    output << "]"
           << ",\n  \"result_shape_contract_count\": ";
    std::size_t result_shape_count = 0;
    for (const normalized_entity& entity : graph.entities) {
        if (detail::manifest_routine_kind(entity.source.kind)
            && !entity.source.result.row_cpp_name.empty()) {
            ++result_shape_count;
        }
    }
    output << result_shape_count
           << ",\n  \"result_shape_contracts\": [";
    std::size_t result_shape_index = 0;
    for (const normalized_entity& entity : graph.entities) {
        if (!detail::manifest_routine_kind(entity.source.kind)
            || entity.source.result.row_cpp_name.empty()) {
            continue;
        }
        const auto row = std::ranges::find(
            graph.result_rows,
            entity.source.result.row_cpp_name,
            &collected_result_row::cpp_qualified_name);
        if (row == graph.result_rows.end()) {
            throw std::runtime_error(
                "structured result contract has no Row aggregate for "
                + entity.source.result.row_cpp_name);
        }
        output << (result_shape_index++ == 0 ? "\n" : ",\n")
               << "    {\"routine_identity\": ";
        detail::write_json_string(
            output, detail::manifest_routine_identity(entity));
        output << ", \"form\": ";
        detail::write_json_string(
            output,
            detail::manifest_result_form(entity.source.result.form));
        output << ", \"row_cpp_name\": ";
        detail::write_json_string(output, row->cpp_qualified_name);
        output << ", \"source_module\": ";
        detail::write_json_string(output, row->source_module);
        output << ", \"fields\": [";
        for (std::size_t ordinal = 0;
             ordinal < row->fields.size();
             ++ordinal) {
            const collected_result_field& field = row->fields[ordinal];
            output << (ordinal == 0 ? "\n" : ",\n")
                   << "      {\"ordinal\": " << ordinal
                   << ", \"cpp_name\": ";
            detail::write_json_string(output, field.name);
            output << ", \"sql_name\": ";
            detail::write_json_string(output, field.name);
            output << ", \"cpp_type\": ";
            detail::write_json_string(output, field.cpp_type);
            output << ", \"sql_type\": ";
            detail::write_json_string(output, field.sql_type);
            output << ", \"nullable\": "
                   << (field.nullable ? "true" : "false")
                   << ", \"converter_key\": ";
            detail::write_json_string(
                output,
                fmgr_detail::scalar_converter(
                    field.cpp_type, field.sql_type, true)
                    .key);
            output << "}";
        }
        if (!row->fields.empty()) output << '\n' << "    ";
        output << "]}";
    }
    if (result_shape_index != 0) output << '\n' << "  ";
    output << "]"
           << ",\n  \"row_codec_contract_count\": "
           << row_codec_count
           << ",\n  \"row_codec_contracts\": [";
    std::size_t row_contract_index = 0;
    for (const normalized_entity& row : graph.entities) {
        const auto codec =
            row_codecs_by_identity.find(row.stable_identity);
        if (codec == row_codecs_by_identity.end()) continue;
        std::vector<const normalized_entity*> fields;
        for (const normalized_entity& candidate : graph.entities) {
            if (candidate.source.kind == entity_kind::column
                && candidate.source.enclosing_cpp_name
                    == row.source.cpp_qualified_name) {
                fields.push_back(&candidate);
            }
        }
        std::ranges::sort(
            fields,
            {},
            [](const normalized_entity* field) {
                return field->source.source_range.begin.byte_offset;
            });
        output << (row_contract_index++ == 0 ? "\n" : ",\n")
               << "    {\"row_identity\": ";
        detail::write_json_string(output, row.stable_identity);
        output << ", \"row_cpp_name\": ";
        detail::write_json_string(output, row.source.cpp_qualified_name);
        output << ", \"module_name\": ";
        detail::write_json_string(
            output, codec->second.first->module_name);
        output << ", \"fields\": [";
        for (std::size_t ordinal = 0;
             ordinal < fields.size();
             ++ordinal) {
            const collected_entity& field = fields[ordinal]->source;
            std::string sql_type =
                detail::row_column_sql_type(field);
            if (sql_type.empty()) {
                sql_type =
                    infer_sql_type_from_cpp(field.cpp_return_type);
            }
            output << (ordinal == 0 ? "\n" : ",\n")
                   << "      {\"ordinal\": " << ordinal
                   << ", \"cpp_name\": ";
            const std::size_t separator =
                field.cpp_qualified_name.rfind("::");
            detail::write_json_string(
                output,
                separator == std::string::npos
                    ? field.cpp_qualified_name
                    : field.cpp_qualified_name.substr(separator + 2));
            output << ", \"sql_name\": ";
            detail::write_json_string(output, field.name);
            output << ", \"cpp_type\": ";
            detail::write_json_string(output, field.cpp_return_type);
            output << ", \"sql_type\": ";
            detail::write_json_string(output, sql_type);
            output << ", \"nullable\": "
                   << (field.cpp_return_type.contains("optional<")
                           ? "true"
                           : "false")
                   << ", \"converter_key\": ";
            detail::write_json_string(
                output,
                detail::manifest_row_codec_converter_key(
                    field, ordinal + 1));
            output
                   << "}";
        }
        if (!fields.empty()) output << '\n' << "    ";
        output << "]}";
    }
    if (row_contract_index != 0) output << '\n' << "  ";
    output << "]"
           << ",\n  \"c_routine_identities\": ";
    detail::write_string_array(
        output, routine_inventory.c_routine_identities, "    ");
    output << ",\n  \"c_link_symbols\": ";
    detail::write_string_array(
        output, routine_inventory.link_symbols, "    ");
    output << ",\n  \"c_finfo_symbols\": ";
    detail::write_string_array(
        output, routine_inventory.finfo_symbols, "    ");
    output << ",\n  \"generated_wrapper_symbols\": ";
    detail::write_string_array(
        output,
        fmgr_ownership_inventory.generated_wrapper_symbols,
        "    ");
    output << ",\n  \"handwritten_fmgr_export_symbols\": ";
    detail::write_string_array(
        output,
        fmgr_ownership_inventory.handwritten_fmgr_export_symbols,
        "    ");
    output << ",\n  \"handwritten_fmgr_export_external_symbols\": ";
    detail::write_string_array(
        output,
        fmgr_ownership_inventory
            .handwritten_fmgr_export_external_symbols,
        "    ");
    output << ",\n  \"fmgr_entrypoint_module_count\": "
           << fmgr_modules.size()
           << ",\n  \"fmgr_entrypoint_modules\": [";
    for (std::size_t module_index = 0;
         module_index < fmgr_modules.size();
         ++module_index) {
        const rendered_fmgr_entrypoint_module& module =
            fmgr_modules[module_index];
        output << (module_index == 0 ? "\n" : ",\n")
               << "    {\"owner_key\": ";
        detail::write_json_string(output, module.owner_key);
        output << ", \"owner_module\": ";
        detail::write_json_string(output, module.source_module);
        output << ", \"module_name\": ";
        detail::write_json_string(output, module.module_name);
        output << ", \"content_sha256\": ";
        detail::write_json_string(output, module.content_sha256);
        output << ", \"wrappers\": [";
        for (std::size_t wrapper_index = 0;
             wrapper_index < module.wrappers.size();
             ++wrapper_index) {
            const rendered_fmgr_wrapper& wrapper =
                module.wrappers[wrapper_index];
            output << (wrapper_index == 0 ? "\n" : ",\n")
                   << "      {\"symbol\": ";
            detail::write_json_string(
                output, wrapper.public_symbol);
            output << ", \"finfo_symbol\": ";
            detail::write_json_string(
                output, wrapper.finfo_symbol);
            output << ", \"overloads\": [";
            for (std::size_t overload_index = 0;
                 overload_index < wrapper.overloads.size();
                 ++overload_index) {
                const rendered_fmgr_overload& overload =
                    wrapper.overloads[overload_index];
                output << (overload_index == 0 ? "\n" : ",\n")
                       << "        {\"sql_identity\": ";
                detail::write_json_string(
                    output, overload.sql_identity);
                output << ", \"cpp_declaration\": ";
                detail::write_json_string(
                    output, overload.cpp_declaration);
                output << ", \"source_path\": ";
                detail::write_json_string(
                    output,
                    detail::manifest_repository_path(
                        overload.source_path,
                        "fmgr_entrypoint_modules.overloads.source_path"));
                output << ", \"source_line\": "
                       << overload.source_line
                       << ", \"sql_arity\": "
                       << overload.sql_arity
                       << ", \"argument_converter_keys\": ";
                detail::write_string_array(
                    output,
                    overload.argument_converter_keys,
                    "          ");
                output << ", \"result_converter_key\": ";
                detail::write_json_string(
                    output, overload.result_converter_key);
                output << ", \"injected_call_context\": "
                       << (overload.injected_call_context
                               ? "true"
                               : "false")
                       << "}";
            }
            if (!wrapper.overloads.empty()) {
                output << '\n' << "      ";
            }
            output << "]}";
        }
        if (!module.wrappers.empty()) output << '\n' << "    ";
        output << "]}";
    }
    if (!fmgr_modules.empty()) output << '\n' << "  ";
    output << "]"
           << ",\n  \"handwritten_fmgr_export_owners\": [";
    for (std::size_t index = 0;
         index < graph.handwritten_fmgr_export_owners.size();
         ++index) {
        const handwritten_fmgr_export_owner& owner =
            graph.handwritten_fmgr_export_owners[index];
        output << (index == 0 ? "\n" : ",\n")
               << "    {\"symbol\": ";
        detail::write_json_string(output, owner.symbol);
        output << ", \"macro\": ";
        detail::write_json_string(output, owner.macro_name);
        output << ", \"source_module\": ";
        detail::write_json_string(output, owner.source_module);
        output << ", \"source_path\": ";
        detail::write_json_string(
            output,
            detail::manifest_repository_path(
                owner.source_path,
                "handwritten_fmgr_export_owners.source_path"));
        output << ", \"source_line\": " << owner.source_line
               << "}";
    }
    if (!graph.handwritten_fmgr_export_owners.empty()) {
        output << '\n' << "  ";
    }
    output << "]"
           << ",\n  \"c_symbol_owners\": [";
    for (std::size_t index = 0;
         index < routine_inventory.symbol_entries.size();
         ++index) {
        const detail::c_symbol_inventory_entry& entry =
            routine_inventory.symbol_entries[index];
        output << (index == 0 ? "\n" : ",\n")
               << "    {\"symbol\": ";
        detail::write_json_string(output, entry.symbol);
        output << ", \"finfo_symbol\": ";
        detail::write_json_string(output, entry.finfo_symbol);
        output << ", \"generated_wrapper\": "
               << (actual_generated_symbols.contains(entry.symbol)
                       ? "true"
                       : "false")
               << ", \"routine_identities\": ";
        detail::write_string_array(
            output, entry.routine_identities, "      ");
        output << ", \"implementation_declarations\": ";
        detail::write_string_array(
            output, entry.implementation_declarations, "      ");
        output << "}";
    }
    if (!routine_inventory.symbol_entries.empty()) {
        output << '\n' << "  ";
    }
    output << "]"
           << ",\n  \"entity_counts_by_kind\": {";
    std::size_t kind_index = 0;
    for (const auto& [kind, count] : entity_counts_by_kind) {
        output << (kind_index++ == 0 ? "\n" : ",\n") << "    ";
        detail::write_json_string(output, kind);
        output << ": " << count;
    }
    if (!entity_counts_by_kind.empty()) output << '\n' << "  ";
    output << "}"
           << ",\n  \"entities\": [\n";
    for (std::size_t index = 0; index < graph.entities.size(); ++index) {
        const normalized_entity& entity = graph.entities[index];
        const std::vector<std::string> columns =
            detail::child_identities(graph, entity, true);
        const std::vector<std::string> constraints =
            detail::child_identities(graph, entity, false);
        const std::vector<std::string> relationships =
            detail::relationship_identities(graph, entity);
        const bool routine =
            detail::manifest_routine_kind(entity.source.kind);
        const std::string routine_identity = routine
            ? detail::manifest_routine_identity(entity)
            : std::string{};
        const std::string routine_language = routine
            ? detail::manifest_routine_language(entity.source)
            : std::string{};
        const std::string c_symbol = routine_language == "c"
            ? detail::manifest_c_symbol(entity.source)
            : std::string{};
        const auto shared_routines =
            routine_inventory.routine_identities_by_symbol.find(c_symbol);
        const auto entity_handle =
            entity_handles_by_identity.find(entity.stable_identity);
        const auto row_codec =
            row_codecs_by_identity.find(entity.stable_identity);
        output << "    {\n      \"identity\": ";
        detail::write_json_string(output, entity.stable_identity);
        output << ",\n      \"stable_identity\": ";
        detail::write_json_string(output, entity.stable_identity);
        output << ",\n      \"installed_identity\": ";
        detail::write_json_string(output, entity.installed_identity);
        output << ",\n      \"kind\": ";
        detail::write_json_string(output, kind_name(entity.source.kind));
        output << ",\n      \"schema\": ";
        detail::write_json_string(output, entity.source.schema);
        output << ",\n      \"schema_mode\": ";
        detail::write_json_string(
            output, detail::manifest_schema_mode(entity.source));
        if (graph.extension_name != "bioseq") {
            output << ",\n      \"schema_provision\": ";
            if (entity.source.kind == entity_kind::schema) {
                detail::write_json_string(
                    output,
                    detail::manifest_enum_option(
                        entity.source, "provision", "install_sql"));
            } else {
                output << "null";
            }
        }
        output << ",\n      \"name\": ";
        detail::write_json_string(output, entity.source.name);
        output << ",\n      \"routine_identity\": ";
        if (routine) {
            detail::write_json_string(output, routine_identity);
        } else {
            output << "null";
        }
        output << ",\n      \"display_spelling\": ";
        detail::write_json_string(
            output, detail::manifest_display_spelling(entity));
        output << ",\n      \"cpp_declaration\": ";
        detail::write_json_string(
            output, entity.source.cpp_qualified_name);
        output << ",\n      \"source_module\": ";
        detail::write_json_string(output, entity.source.source_module);
        output << ",\n      \"source_path\": ";
        detail::write_json_string(
            output,
            detail::manifest_repository_path(
                entity.source.source_path,
                "entities.source_path"));
        output << ",\n      \"source_sha256\": ";
        detail::write_json_string(
            output, entity.source.source_sha256);
        output << ",\n      \"source_line\": "
               << entity.source.source_range.begin.line
               << ",\n      \"source_span\": {"
               << "\"byte_begin\": "
               << entity.source.source_range.begin.byte_offset
               << ", \"byte_end\": "
               << entity.source.source_range.end.byte_offset
               << ", \"line_begin\": "
               << entity.source.source_range.begin.line
               << ", \"line_end\": "
               << entity.source.source_range.end.line
               << ", \"column_begin\": "
               << entity.source.source_range.begin.column
               << ", \"column_end\": "
               << entity.source.source_range.end.column << "}"
               << ",\n      \"active_condition\": ";
        detail::write_json_string(output, entity.source.active_condition);
        output << ",\n      \"public_documentation\": "
               << (entity.source.public_documentation ? "true" : "false")
               << ",\n      \"brief\": ";
        detail::write_json_string(
            output, entity.source.documentation.brief);
        output << ",\n      \"details\": ";
        detail::write_json_string(
            output, entity.source.documentation.details);
        output << ",\n      \"parameters\": [";
        const std::vector<const routine_parameter*> parameters =
            sql_parameters(entity.source);
        for (std::size_t parameter = 0;
             parameter < parameters.size();
             ++parameter) {
            const routine_parameter& value = *parameters[parameter];
            output << (parameter == 0 ? "\n" : ",\n")
                   << "        {\"name\": ";
            detail::write_json_string(output, value.name);
            output << ", \"cpp_type\": ";
            detail::write_json_string(output, value.cpp_type);
            output << ", \"sql_type\": ";
            detail::write_json_string(output, value.sql_type);
            output << ", \"nullable\": "
                   << (value.nullable ? "true" : "false")
                   << ", \"has_default\": "
                   << (value.has_default ? "true" : "false")
                   << ", \"documentation\": ";
            detail::write_json_string(
                output,
                detail::documented_parameter_text(
                    entity.source, value.name));
            output << "}";
        }
        if (!parameters.empty()) output << '\n' << "      ";
        output << "],\n      \"result_sql_type\": ";
        const std::string result_sql_type =
            entity.source.result_sql_type.empty()
            ? detail::manifest_option_leaf(entity.source, "returns")
            : entity.source.result_sql_type;
        detail::write_json_string(
            output, result_sql_type);
        output << ",\n      \"result\": {\"sql_type\": ";
        detail::write_json_string(
            output, result_sql_type);
        output << ", \"documentation\": ";
        detail::write_json_string(
            output, entity.source.documentation.returns);
        output << "}";
        output << ",\n      \"overload_signature\": [";
        for (std::size_t parameter = 0;
             parameter < parameters.size();
             ++parameter) {
            if (parameter != 0) output << ", ";
            detail::write_json_string(
                output, parameters[parameter]->sql_type);
        }
        output << "]";
        output << ",\n      \"routine_properties\": ";
        if (!detail::manifest_routine_kind(entity.source.kind)) {
            output << "null";
        } else {
            const bool c_binding = routine_language == "c";
            const bool create_or_replace =
                detail::manifest_create_or_replace(entity.source);
            output << "{\"language\": ";
            detail::write_json_string(
                output, routine_language);
            output << ", \"c_symbol\": ";
            detail::write_json_string(output, c_symbol);
            output << ", \"generated_wrapper\": "
                   << (c_binding
                           && actual_generated_identities.contains(
                               routine_identity)
                           ? "true"
                           : "false")
                   << ", \"create_or_replace\": "
                   << (create_or_replace ? "true" : "false")
                   << ", \"create_mode\": ";
            detail::write_json_string(
                output,
                create_or_replace ? "or_replace" : "create");
            output << ", \"volatility\": ";
            detail::write_json_string(
                output,
                detail::manifest_enum_option(
                    entity.source,
                    "volatility_value",
                    detail::manifest_enum_option(
                        entity.source, "volatility", "volatile")));
            output << ", \"null_input\": ";
            detail::write_json_string(
                output, detail::manifest_null_input(entity.source));
            output << ", \"security\": ";
            detail::write_json_string(
                output,
                detail::manifest_enum_option(
                    entity.source, "security", "invoker"));
            output << ", \"parallel\": ";
            detail::write_json_string(
                output,
                detail::manifest_enum_option(
                    entity.source, "parallel", "unsafe"));
            output << ", \"configuration\": ";
            detail::write_json_string(
                output,
                detail::manifest_option_leaf(
                    entity.source, "configuration"));
            output << ", \"cost\": ";
            detail::write_json_string(
                output,
                detail::manifest_option_leaf(entity.source, "cost"));
            output << ", \"rows\": ";
            detail::write_json_string(
                output,
                detail::manifest_option_leaf(entity.source, "rows"));
            output << ", \"support\": ";
            detail::write_json_string(
                output,
                detail::manifest_option_leaf(entity.source, "support"));
            output << "}";
        }
        output << ",\n      \"c_binding\": ";
        if (routine_language == "c") {
            output << "{\"symbol\": ";
            detail::write_json_string(output, c_symbol);
            output << ", \"finfo_symbol\": ";
            detail::write_json_string(
                output, "pg_finfo_" + c_symbol);
            output << ", \"routine_identity\": ";
            detail::write_json_string(output, routine_identity);
            output << ", \"implementation_declaration\": ";
            detail::write_json_string(
                output, entity.source.cpp_qualified_name);
            output << ", \"same_symbol_routine_identities\": ";
            if (shared_routines
                != routine_inventory.routine_identities_by_symbol.end()) {
                detail::write_string_array(
                    output, shared_routines->second, "        ");
            } else {
                output << "[]";
            }
            output << ", \"generated_wrapper\": "
                   << (actual_generated_identities.contains(
                           routine_identity)
                           ? "true"
                           : "false")
                   << "}";
        } else {
            output << "null";
        }
        output << ",\n      \"generated_cpp\": ";
        if (entity_handle == entity_handles_by_identity.end()
            && row_codec == row_codecs_by_identity.end()) {
            output << "null";
        } else if (entity_handle
                   != entity_handles_by_identity.end()) {
            const rendered_entity_handle_module& module =
                *entity_handle->second.first;
            const rendered_entity_handle& handle =
                *entity_handle->second.second;
            output << "{\"facet\": \"entity_handle\", \"owner_key\": ";
            detail::write_json_string(output, module.owner_key);
            output << ", \"module_name\": ";
            detail::write_json_string(output, module.module_name);
            output << ", \"cpp_namespace\": ";
            detail::write_json_string(output, module.cpp_namespace);
            output << ", \"variable_name\": ";
            detail::write_json_string(output, handle.variable_name);
            output << ", \"expected_relkind\": ";
            detail::write_json_string(output, handle.expected_relkind);
            output << ", \"schema_binding\": ";
            detail::write_json_string(output, handle.schema_binding);
            output << ", \"schema_name\": ";
            detail::write_json_string(output, handle.schema_name);
            output << ", \"profile_invariant\": true}";
        } else {
            const rendered_row_codec_module& module =
                *row_codec->second.first;
            output << "{\"facet\": \"row_codec\", \"owner_key\": ";
            detail::write_json_string(output, module.owner_key);
            output << ", \"source_module\": ";
            detail::write_json_string(output, module.source_module);
            output << ", \"module_name\": ";
            detail::write_json_string(output, module.module_name);
            output << ", \"cpp_namespace\": ";
            detail::write_json_string(output, module.cpp_namespace);
            output << ", \"row_cpp_name\": ";
            detail::write_json_string(output, row_codec->second.second);
            output << ", \"descriptor_sha256\": ";
            detail::write_json_string(
                output, bioseq::common::sha256_hex(module.content));
            output << ", \"profile_invariant\": true}";
        }
        output << ",\n      \"columns\": ";
        detail::write_string_array(output, columns, "        ");
        output << ",\n      \"constraints\": ";
        detail::write_string_array(output, constraints, "        ");
        output << ",\n      \"dependencies\": [";
        for (std::size_t dependency = 0;
             dependency < entity.dependency_identities.size();
             ++dependency) {
            if (dependency != 0) output << ", ";
            detail::write_json_string(
                output, entity.dependency_identities[dependency]);
        }
        output << "],\n      \"relationships\": ";
        detail::write_string_array(output, relationships, "        ");
        output << ",\n      \"entity_links\": ";
        detail::write_string_array(output, relationships, "        ");
        output << ",\n      \"acl_summary\": [";
        bool first_acl = true;
        for (const normalized_entity& candidate : graph.entities) {
            if (candidate.source.kind != entity_kind::grant
                && candidate.source.kind != entity_kind::revoke) {
                continue;
            }
            if (!std::ranges::contains(
                    candidate.dependency_identities,
                    entity.stable_identity)) {
                continue;
            }
            if (!first_acl) output << ", ";
            first_acl = false;
            detail::write_json_string(
                output, candidate.stable_identity);
        }
        output << "],\n      \"runnable_examples\": [";
        for (std::size_t example = 0;
             example < entity.source.documentation.runnable_examples.size();
             ++example) {
            if (example != 0) output << ", ";
            detail::write_json_string(
                output,
                entity.source.documentation.runnable_examples[example]);
        }
        output << "],\n      \"documentation_examples\": [";
        bool first_example = true;
        for (const std::string& source_example :
             entity.source.documentation.runnable_examples) {
            const auto example = detail::documentation_example(source_example);
            if (example.sql.empty()) continue;
            if (!first_example) output << ", ";
            first_example = false;
            output << "{\"description\": ";
            detail::write_json_string(output, example.description);
            output << ", \"sql\": ";
            detail::write_json_string(output, example.sql);
            output << '}';
        }
        output << "],\n      \"documentation_fields\": [";
        bool first_field_documentation = true;
        for (const std::string& remark : entity.source.documentation.remarks) {
            constexpr std::string_view prefix = "website.field ";
            if (!std::string_view{remark}.starts_with(prefix)) continue;
            const std::string_view field = std::string_view{remark}.substr(prefix.size());
            const std::size_t separator = field.find(':');
            if (separator == std::string_view::npos || separator == 0) {
                throw std::runtime_error("malformed website.field documentation for "
                    + entity.stable_identity);
            }
            const std::string_view description = field.substr(separator + 1);
            const std::size_t first = description.find_first_not_of(" \t");
            if (first == std::string_view::npos) {
                throw std::runtime_error("empty website.field description for "
                    + entity.stable_identity);
            }
            if (!first_field_documentation) output << ", ";
            first_field_documentation = false;
            output << "{\"name\": ";
            detail::write_json_string(output, field.substr(0, separator));
            output << ", \"description\": ";
            detail::write_json_string(output, description.substr(first));
            output << '}';
        }
        output << "],\n      \"operator_binding\": ";
        if (entity.source.kind == entity_kind::operator_) {
            const auto* implementation = detail::operator_implementation(graph, entity);
            if (implementation == nullptr) {
                throw std::runtime_error("operator has no implementation: "
                    + entity.stable_identity);
            }
            detail::write_operator_binding(output, entity, *implementation);
        } else {
            output << "null";
        }
        output << ",\n      \"operator_aliases\": [";
        bool first_operator = true;
        if (entity.source.kind == entity_kind::function) {
            for (const normalized_entity& operation : graph.entities) {
                if (operation.source.kind != entity_kind::operator_
                    || detail::operator_implementation(graph, operation) != &entity) continue;
                if (!first_operator) output << ", ";
                first_operator = false;
                detail::write_operator_binding(output, operation, entity);
            }
        }
        output << "],\n      \"options\": {";
        for (std::size_t option = 0;
             option < entity.source.options.size();
             ++option) {
            output << (option == 0 ? "\n" : ",\n") << "        ";
            detail::write_json_string(
                output, entity.source.options[option].name);
            output << ": ";
            detail::write_json_string(
                output, entity.source.options[option].expression);
        }
        if (!entity.source.options.empty()) output << '\n' << "      ";
        output << "},\n      \"statement_ids\": [";
        const auto statement_position =
            statements_by_entity.find(entity.stable_identity);
        if (statement_position != statements_by_entity.end()) {
            for (std::size_t statement = 0;
                 statement < statement_position->second.size();
                 ++statement) {
                if (statement != 0) output << ", ";
                detail::write_json_string(
                    output, statement_position->second[statement]);
            }
        }
        output << "]\n    }"
               << (index + 1 == graph.entities.size() ? "\n" : ",\n");
    }
    output << "  ]\n}\n";
    return output.str();
}

}  // namespace bioseq::codegen::sql
