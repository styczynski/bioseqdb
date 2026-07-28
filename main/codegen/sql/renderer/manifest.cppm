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
import bioseq.codegen.clang_structural;
import bioseq.codegen.safety;
import bioseq.codegen.sql.collector;
import bioseq.codegen.sql.graph;
import bioseq.codegen.support;

export namespace bioseq::codegen::sql {

namespace detail {

[[nodiscard]] inline std::string manifest_json_escape(
    std::string_view text) {
    std::string result;
    result.reserve(text.size() + 8);
    constexpr char digits[] = "0123456789abcdef";
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
    static constexpr std::array<std::string_view, 17>
        repository_main_roots{
            "bench",
            "bench_framework",
            "bioseq",
            "bioseq_caching",
            "cmake",
            "codegen",
            "common",
            "config",
            "data",
            "DOCS",
            "infra",
            "main",
            "overlays",
            "papers",
            "reporting",
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
        return result;
    }

    if (const char* reviewed_root_raw =
            std::getenv("BIOSEQ_REVIEWED_MAIN_SOURCE_ROOT");
        reviewed_root_raw != nullptr && reviewed_root_raw[0] != '\0') {
        std::error_code root_error;
        const std::filesystem::path reviewed_root =
            std::filesystem::weakly_canonical(
                std::filesystem::path{reviewed_root_raw}, root_error);
        std::error_code directory_error;
        if (root_error
            || !std::filesystem::is_directory(
                reviewed_root, directory_error)
            || directory_error) {
            throw std::runtime_error(
                "BIOSEQ_REVIEWED_MAIN_SOURCE_ROOT is not a directory: "
                + std::string{reviewed_root_raw});
        }

        std::error_code path_error;
        const std::filesystem::path reviewed_path =
            std::filesystem::weakly_canonical(normalized, path_error);
        if (!path_error) {
            const std::filesystem::path relative =
                reviewed_path.lexically_relative(reviewed_root);
            if (relative == ".") return "main";
            if (!relative.empty()
                && std::ranges::none_of(
                    relative,
                    [](const std::filesystem::path& component) {
                        return component == ".." || component == ".";
                    })) {
                const std::string first =
                    relative.begin()->generic_string();
                if (!std::ranges::contains(
                        repository_main_roots,
                        std::string_view{first})) {
                    throw std::runtime_error(
                        "absolute manifest repository path '"
                        + std::string{field}
                        + "' is outside the reviewed main source closure: "
                        + path.generic_string());
                }
                return "main/" + relative.generic_string();
            }
        }
    }

    std::vector<std::string> components;
    for (const std::filesystem::path& component : normalized) {
        const std::string spelling = component.generic_string();
        if (!spelling.empty() && spelling != "/") {
            components.push_back(spelling);
        }
    }
    std::optional<std::size_t> repository_main;
    for (std::size_t index = 0;
         index + 1 < components.size();
         ++index) {
        if (components[index] == "main"
            && std::ranges::contains(
                repository_main_roots,
                std::string_view{components[index + 1]})) {
            repository_main = index;
        }
    }
    if (!repository_main) {
        throw std::runtime_error(
            "absolute manifest repository path '"
            + std::string{field}
            + "' cannot be reduced to the reviewed main source tree: "
            + path.generic_string());
    }
    std::string result;
    for (std::size_t index = *repository_main;
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
        case routine_result_form::setof: return "setof";
        case routine_result_form::returns_table: return "returns_table";
        case routine_result_form::named_out: return "named_out";
        case routine_result_form::setof_named_out:
            return "setof_named_out";
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
    if (entity.kind == entity_kind::alter
        || entity.kind == entity_kind::comment
        || entity.kind == entity_kind::session_setting
        || entity.kind == entity_kind::guc
        || entity.kind == entity_kind::guc_value
        || entity.kind == entity_kind::phase) {
        return "none";
    }
    if (entity.companion && !entity.typed_install_owner) {
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
    if (entity.source.typed_install_owner) {
        const std::string identity = manifest_option_leaf(
            entity.source, "sql_identity");
        if (!identity.empty() && identity != "unknown") return identity;
    }
    return fmgr_detail::relocatable_routine_identity(entity);
}

[[nodiscard]] inline std::string manifest_routine_language(
    const collected_entity& entity) {
    if (entity.typed_install_owner) {
        const std::string language =
            manifest_enum_option(entity, "language");
        if (!language.empty() && language != "none") return language;
    }
    return entity.kind == entity_kind::sql_function ? "sql" : "c";
}

[[nodiscard]] inline std::string manifest_c_symbol(
    const collected_entity& entity) {
    return manifest_option_leaf(entity, "c_symbol", entity.name);
}

[[nodiscard]] inline bool external_legacy_fmgr_symbol(
    std::string_view symbol) {
    return std::ranges::contains(
        safety::doc0388_external_fmgr_symbols, symbol);
}

[[nodiscard]] inline bool reviewed_external_legacy_fmgr_routine(
    const normalized_entity& entity,
    std::string_view routine_identity,
    std::string_view symbol) {
    if (!external_legacy_fmgr_symbol(symbol)) return false;
    if (!fmgr_detail::reviewed_handwritten_external_routine(entity)
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
    std::vector<std::string> legacy_macro_symbols;
    std::vector<std::string> external_legacy_macro_symbols;
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
        if (!entity.source.typed_install_owner
            && (symbol != entity.source.name
                || routine_identity_leaf(routine_identity)
                    != entity.source.name)) {
            throw std::runtime_error(
                "natural C routine '" + routine_identity
                + "' must derive its SQL leaf and C symbol from '"
                + entity.source.name + "'");
        }
        // The external table-AM handler deliberately retains its handwritten
        // POSTGRES_FN boundary. It is a natural SQL owner, but remains outside
        // the generated main-extension wrapper/DSO closure.
        if (reviewed_external_legacy_fmgr_routine(
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
            if (!owner.entity->source.typed_install_owner) {
                entry.generated_wrapper = true;
                natural_implementations.insert(
                    owner.entity->source.cpp_qualified_name);
            }
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
    if (validate_reviewed_target) {
        safety::validate_doc0388_fmgr_target(
            routines.c_routine_identities,
            routines.link_symbols);
    }

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

    std::set<std::string, std::less<>> legacy_symbols;
    std::set<std::string, std::less<>> external_symbols;
    for (const legacy_fmgr_macro_owner& owner :
         graph.legacy_fmgr_macro_owners) {
        if (target_symbols.contains(owner.symbol)) {
            legacy_symbols.insert(owner.symbol);
        } else {
            external_symbols.insert(owner.symbol);
        }
    }
    const std::set<std::string, std::less<>>
        expected_external_symbols{
            safety::doc0388_external_fmgr_symbols.begin(),
            safety::doc0388_external_fmgr_symbols.end()};
    if (validate_reviewed_target
        && external_symbols != expected_external_symbols) {
        throw std::runtime_error(
            "production fmgr macro scan must contain exactly the reviewed "
            "bioseq_external handler outside the main extension");
    }

    std::vector<std::string> overlapping_symbols;
    std::ranges::set_intersection(
        generated_symbols,
        legacy_symbols,
        std::back_inserter(overlapping_symbols));
    if (!overlapping_symbols.empty()) {
        throw std::runtime_error(
            "generated and legacy fmgr ownership overlap at symbol '"
            + overlapping_symbols.front() + "'");
    }
    std::set<std::string, std::less<>> owned_main_symbols =
        generated_symbols;
    owned_main_symbols.insert(
        legacy_symbols.begin(), legacy_symbols.end());
    if (owned_main_symbols != target_symbols) {
        throw std::runtime_error(
            "generated plus legacy fmgr ownership does not exactly cover "
            + std::string{
                validate_reviewed_target
                    ? "the reviewed 268-symbol main-extension closure"
                    : "the normalized C-routine closure"});
    }

    return {
        .generated_wrapper_symbols = {
            generated_symbols.begin(), generated_symbols.end()},
        .legacy_macro_symbols = {
            legacy_symbols.begin(), legacy_symbols.end()},
        .external_legacy_macro_symbols = {
            external_symbols.begin(), external_symbols.end()},
    };
}

[[nodiscard]] inline std::string manifest_null_input(
    const collected_entity& entity) {
    if (const option_value* option =
            manifest_option(entity, "null_input")) {
        return manifest_enum_option(entity, "null_input");
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

[[nodiscard]] inline std::optional<physical_statement_binding>
manifest_statement_binding(
    const normalized_entity& owner,
    const rendered_statement& statement) {
    std::vector<physical_statement_binding> bindings =
        effective_physical_statement_bindings(owner.source);
    if (statement.baseline_physical_id.empty()) {
        if (bindings.size() == 1
            && bindings.front().baseline_physical_id.empty()
            && bindings.front().facet == statement.facet) {
            return bindings.front();
        }
        return std::nullopt;
    }
    const auto binding = std::ranges::find(
        bindings,
        statement.baseline_physical_id,
        &physical_statement_binding::baseline_physical_id);
    if (binding == bindings.end()) {
        throw std::runtime_error(
            "rendered statement '" + statement.id
            + "' has baseline physical ID '"
            + statement.baseline_physical_id
            + "' that is absent from owner '"
            + owner.stable_identity + "'");
    }
    return *binding;
}

}  // namespace detail

[[nodiscard]] inline std::string render_graph_manifest(
    const sql_graph& graph,
    const rendered_sql_artifact& artifact,
    const std::vector<rendered_fmgr_entrypoint_module>&
        fmgr_modules,
    bool validate_reviewed_fmgr_target = false) {
    std::size_t migration_action_count = 0;
    std::size_t routine_entity_count = 0;
    std::size_t install_owner_entity_count = 0;
    std::map<std::string, std::size_t, std::less<>> entity_counts_by_kind;
    for (const normalized_entity& entity : graph.entities) {
        const std::string_view kind = kind_name(entity.source.kind);
        ++entity_counts_by_kind[std::string{kind}];
        if (entity.source.typed_install_owner) {
            ++install_owner_entity_count;
        }
        if (kind == "install_action"
            && entity.source.annotation_name == "install_action") {
            ++migration_action_count;
        }
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
    const std::vector<std::string> retired_c_symbols =
        safety::doc0388_retired_c_symbols();
    const std::vector<rendered_entity_handle_module>
        entity_handle_modules = render_entity_handle_modules(graph);
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
        row_codec_modules = render_row_codec_modules(graph);
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
           << "  \"manifest_version\": 1,\n"
           << "  \"authority_mode\": \"annotated_cpp\",\n"
           << "  \"compiler_profile\": ";
    detail::write_json_string(output, graph.compiler_profile);
    output << ",\n  \"clang_version\": ";
    detail::write_json_string(output, graph.clang_version);
    output << ",\n  \"compilation_signature\": ";
    detail::write_json_string(
        output, graph.compilation_signature);
    output << ",\n"
           << "  \"extension_schema\": ";
    detail::write_json_string(output, graph.extension_schema);
    output << ",\n  \"script_sha256\": ";
    detail::write_json_string(output, sha256_hex(artifact.sql));
    output << ",\n  \"statement_count\": " << artifact.statements.size()
           << ",\n  \"physical_statement_count\": "
           << artifact.statements.size()
           << ",\n  \"baseline_physical_statement_count\": "
           << graph.expected_baseline_physical_statements.size()
           << ",\n  \"physical_statements\": [";
    for (std::size_t index = 0;
         index < artifact.statements.size();
         ++index) {
        const rendered_statement& statement =
            artifact.statements[index];
        const normalized_entity& owner =
            detail::manifest_statement_owner(graph, statement);
        const std::optional<physical_statement_binding> binding =
            detail::manifest_statement_binding(owner, statement);
        const std::string source_module =
            binding ? binding->source_module
                    : owner.source.source_module;
        const std::filesystem::path source_path =
            binding ? binding->source_path
                    : owner.source.source_path;
        const std::string source_sha256 =
            binding ? binding->source_sha256
                    : owner.source.source_sha256;
        const clang_structural::source_range source_range =
            binding ? binding->source_range
                    : owner.source.source_range;
        output << (index == 0 ? "\n" : ",\n")
               << "    {\"statement_id\": ";
        detail::write_json_string(output, statement.id);
        output << ", \"baseline_physical_id\": ";
        if (statement.baseline_physical_id.empty()) {
            output << "null";
        } else {
            detail::write_json_string(
                output, statement.baseline_physical_id);
        }
        output << ", \"baseline_ordinal\": ";
        if (statement.baseline_physical_id.empty()) {
            output << "null";
        } else {
            output << parse_baseline_physical_ordinal(
                statement.baseline_physical_id);
        }
        output << ", \"kind\": ";
        detail::write_json_string(
            output, kind_name(statement.statement_kind));
        output << ", \"facet\": ";
        detail::write_json_string(
            output,
            physical_statement_facet_name(statement.facet));
        output << ", \"owner_identity\": ";
        detail::write_json_string(
            output, statement.entity_identity);
        output << ", \"render_source\": ";
        detail::write_json_string(
            output,
            physical_render_source_name(
                statement.render_source));
        output << ", \"text_sha256\": ";
        detail::write_json_string(
            output, sha256_hex(statement.text));
        output << ", \"baseline_text_sha256\": ";
        if (!binding
            || binding->compatibility_sql_initializer.empty()) {
            output << "null";
        } else {
            detail::write_json_string(
                output,
                sha256_hex(
                    detail::sanitize_compatibility_sql(
                        detail::extract_checked_sql(
                            binding->compatibility_sql_initializer,
                            false))));
        }
        output << ", \"order_identity\": ";
        detail::write_json_string(
            output, binding ? binding->order_identity : std::string{});
        output << ", \"source_module\": ";
        detail::write_json_string(output, source_module);
        output << ", \"source_path\": ";
        detail::write_json_string(
            output,
            detail::manifest_repository_path(
                source_path, "physical_statements.source_path"));
        output << ", \"source_sha256\": ";
        detail::write_json_string(output, source_sha256);
        output << ", \"source_line\": "
               << source_range.begin.line << "}";
    }
    if (!artifact.statements.empty()) output << '\n' << "  ";
    output << "]"
           << ",\n  \"entity_count\": " << graph.entities.size()
           << ",\n  \"typed_entity_count\": "
           << graph.entities.size() - migration_action_count
           << ",\n  \"transitional_install_owner_entity_count\": "
           << install_owner_entity_count
           << ",\n  \"natural_owner_entity_count\": "
           << graph.entities.size() - install_owner_entity_count
           << ",\n  \"natural_authority_complete\": "
           << (install_owner_entity_count == 0 ? "true" : "false")
           << ",\n  \"public_entity_count\": "
           << public_stable_identities.size()
           << ",\n  \"public_stable_identities\": ";
    detail::write_string_array(
        output, public_stable_identities, "    ");
    output << ",\n  \"public_installed_identities\": ";
    detail::write_string_array(
        output, public_installed_identities, "    ");
    output
           << ",\n  \"migration_install_action_count\": "
           << migration_action_count
           << ",\n  \"routine_entity_count\": " << routine_entity_count
           << ",\n  \"c_routine_identity_count\": "
           << routine_inventory.c_routine_identities.size()
           << ",\n  \"c_link_symbol_inventory_count\": "
           << routine_inventory.link_symbols.size()
           << ",\n  \"c_finfo_symbol_inventory_count\": "
           << routine_inventory.finfo_symbols.size()
           << ",\n  \"c_wrapper_entity_count\": "
           << routine_inventory.natural_wrapper_symbol_count
           << ",\n  \"retired_c_symbol_count\": "
           << retired_c_symbols.size()
           << ",\n  \"generated_wrapper_symbol_count\": "
           << fmgr_ownership_inventory
                  .generated_wrapper_symbols.size()
           << ",\n  \"legacy_macro_symbol_count\": "
           << fmgr_ownership_inventory.legacy_macro_symbols.size()
           << ",\n  \"external_legacy_macro_symbol_count\": "
           << fmgr_ownership_inventory
                  .external_legacy_macro_symbols.size()
           << ",\n  \"legacy_fmgr_macro_owner_count\": "
           << graph.legacy_fmgr_macro_owners.size()
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
            output, sha256_hex(module.content));
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
            || detail::reviewed_external_legacy_fmgr_routine(
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
            && !detail::reviewed_external_legacy_fmgr_routine(
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
            generated ? "generated" : "external_legacy");
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
                    output, "injected:sql_call_context");
            } else if (!generated) {
                detail::write_json_string(
                    output, "external_legacy:handwritten");
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
                output, "external_legacy:handwritten");
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
    output << ",\n  \"retired_c_symbols\": ";
    detail::write_string_array(
        output, retired_c_symbols, "    ");
    output << ",\n  \"generated_wrapper_symbols\": ";
    detail::write_string_array(
        output,
        fmgr_ownership_inventory.generated_wrapper_symbols,
        "    ");
    output << ",\n  \"legacy_macro_symbols\": ";
    detail::write_string_array(
        output,
        fmgr_ownership_inventory.legacy_macro_symbols,
        "    ");
    output << ",\n  \"external_legacy_macro_symbols\": ";
    detail::write_string_array(
        output,
        fmgr_ownership_inventory.external_legacy_macro_symbols,
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
           << ",\n  \"legacy_fmgr_macro_owners\": [";
    for (std::size_t index = 0;
         index < graph.legacy_fmgr_macro_owners.size();
         ++index) {
        const legacy_fmgr_macro_owner& owner =
            graph.legacy_fmgr_macro_owners[index];
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
                "legacy_fmgr_macro_owners.source_path"));
        output << ", \"source_line\": " << owner.source_line
               << "}";
    }
    if (!graph.legacy_fmgr_macro_owners.empty()) {
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
        output << ",\n      \"typed_install_owner\": "
               << (entity.source.typed_install_owner ? "true" : "false");
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
        output << ",\n      \"argument_declarations\": ";
        detail::write_json_string(
            output,
            entity.source.typed_install_owner
                ? detail::manifest_option_leaf(
                      entity.source, "argument_declarations")
                : std::string{});
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
                output, sha256_hex(module.content));
            output << ", \"profile_invariant\": true}";
        }
        output << ",\n      \"compatibility_sql\": ";
        if (entity.source.compatibility_sql_initializer.empty()) {
            output << "null";
        } else {
            output << "{\"descriptor_equal\": true, "
                      "\"baseline_physical_id\": ";
            detail::write_json_string(
                output,
                entity.source.compatibility_baseline_physical_id);
            output << ", \"source_module\": ";
            detail::write_json_string(
                output, entity.source.compatibility_source_module);
            output << ", \"source_path\": ";
            detail::write_json_string(
                output,
                detail::manifest_repository_path(
                    entity.source.compatibility_source_path,
                    "entities.compatibility_sql.source_path"));
            output << ", \"source_sha256\": ";
            detail::write_json_string(
                output, entity.source.compatibility_source_sha256);
            output << ", \"source_line\": "
                   << entity.source.compatibility_source_range.begin.line
                   << "}";
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
