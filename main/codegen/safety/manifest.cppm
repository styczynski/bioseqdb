module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.codegen.safety:manifest;

import :json;

export namespace bioseq::codegen::safety {

struct safety_manifest_overload {
    std::string sql_identity;
    std::string cpp_declaration;
    std::string source_path;
    std::size_t source_line{};
    std::vector<std::string> argument_converter_keys;
    std::string result_converter_key;
    bool injected_call_context{};
};

struct safety_manifest_owner {
    std::string public_symbol;
    std::string finfo_symbol;
    std::string generated_wrapper_module;
    std::vector<std::string> sql_identities;
    std::vector<std::string> implementation_declarations;
    std::vector<safety_manifest_overload> overloads;
};

struct safety_manifest {
    std::uint32_t manifest_version{1};
    std::string authority_mode;
    std::vector<std::string> c_routine_identities;
    std::vector<std::string> c_link_symbols;
    std::vector<std::string> c_finfo_symbols;
    std::vector<safety_manifest_owner> owners;
};

struct manifest_closure_policy {
    std::size_t expected_parent_count{270};
    std::size_t expected_symbol_count{268};
    std::set<std::string, std::less<>> shared_symbols{
        "from_array",
        "mmsearch",
    };
    bool require_complete_codegen{true};
};

namespace manifest_detail {

[[nodiscard]] inline std::size_t nonnegative_size(
    const json_value& input,
    std::string_view context) {
    const std::int64_t value = json_integer(input, context);
    if (value < 0) {
        throw std::runtime_error(
            std::string{context} + " cannot be negative");
    }
    return static_cast<std::size_t>(value);
}

[[nodiscard]] inline std::vector<std::string> string_array(
    const json_value& input,
    std::string_view context,
    bool allow_empty = true,
    bool require_unique = true) {
    std::vector<std::string> result;
    std::set<std::string, std::less<>> seen;
    const json_value::array& values = json_array(input, context);
    if (!allow_empty && values.empty()) {
        throw std::runtime_error(
            std::string{context} + " cannot be empty");
    }
    result.reserve(values.size());
    for (std::size_t index = 0; index < values.size(); ++index) {
        const std::string item{
            json_string(
                values[index],
                std::string{context} + "["
                    + std::to_string(index) + "]")};
        if (item.empty()) {
            throw std::runtime_error(
                std::string{context}
                + " contains an empty string");
        }
        if (require_unique && !seen.insert(item).second) {
            throw std::runtime_error(
                std::string{context}
                + " contains a duplicate string");
        }
        result.push_back(item);
    }
    return result;
}

[[nodiscard]] inline std::string routine_leaf(
    std::string_view identity) {
    const std::size_t arguments = identity.find('(');
    const std::string_view qualified =
        arguments == std::string_view::npos
        ? identity
        : identity.substr(0, arguments);
    const std::size_t separator = qualified.rfind('.');
    return std::string{
        separator == std::string_view::npos
            ? qualified
            : qualified.substr(separator + 1)};
}

inline void validate_runtime_sql_identity(
        std::string_view identity,
        std::string_view context) {
    if (identity.starts_with("sql:")
        || identity.contains("$extension.")) {
        throw std::runtime_error(
            std::string{context}
            + " must not retain a sql: kind prefix or $extension. token");
    }
    const std::size_t arguments = identity.find('(');
    if (arguments == std::string_view::npos
        || arguments == 0
        || !identity.ends_with(')')) {
        throw std::runtime_error(
            std::string{context}
            + " must be a regprocedure-like name(type,...) identity");
    }
}

[[nodiscard]] inline const json_value::object& field_object(
    const json_value::object& parent,
    std::string_view field,
    std::string_view context) {
    return json_object(
        json_field(parent, field, context),
        std::string{context} + "." + std::string{field});
}

[[nodiscard]] inline std::string field_string(
    const json_value::object& parent,
    std::string_view field,
    std::string_view context) {
    std::string result{
        json_string(
            json_field(parent, field, context),
            std::string{context} + "." + std::string{field})};
    if (result.empty()) {
        throw std::runtime_error(
            std::string{context} + "." + std::string{field}
            + " must not be empty");
    }
    return result;
}

[[nodiscard]] inline bool field_boolean(
    const json_value::object& parent,
    std::string_view field,
    std::string_view context) {
    return json_boolean(
        json_field(parent, field, context),
        std::string{context} + "." + std::string{field});
}

}  // namespace manifest_detail

[[nodiscard]] inline safety_manifest parse_safety_manifest(
    std::string_view text,
    const manifest_closure_policy& policy = {}) {
    const json_value document = parse_json(text);
    const json_value::object& root =
        json_object(document, "SQL manifest");
    const std::size_t version = manifest_detail::nonnegative_size(
        json_field(root, "manifest_version", "SQL manifest"),
        "SQL manifest.manifest_version");
    if (version != 1) {
        throw std::runtime_error(
            "SQL manifest has an unsupported version");
    }
    safety_manifest result{
        .manifest_version = static_cast<std::uint32_t>(version),
        .authority_mode = manifest_detail::field_string(
            root, "authority_mode", "SQL manifest"),
        .c_routine_identities = manifest_detail::string_array(
            json_field(
                root, "c_routine_identities", "SQL manifest"),
            "SQL manifest.c_routine_identities",
            false),
        .c_link_symbols = manifest_detail::string_array(
            json_field(root, "c_link_symbols", "SQL manifest"),
            "SQL manifest.c_link_symbols",
            false),
        .c_finfo_symbols = manifest_detail::string_array(
            json_field(root, "c_finfo_symbols", "SQL manifest"),
            "SQL manifest.c_finfo_symbols",
            false),
    };
    if (result.authority_mode != "annotated_cpp") {
        throw std::runtime_error(
            "SQL safety analysis requires annotated_cpp authority");
    }
    for (std::size_t index = 0;
         index < result.c_routine_identities.size();
         ++index) {
        manifest_detail::validate_runtime_sql_identity(
            result.c_routine_identities[index],
            "SQL manifest.c_routine_identities["
                + std::to_string(index) + "]");
    }
    const std::size_t declared_parent_count =
        manifest_detail::nonnegative_size(
            json_field(
                root, "c_routine_identity_count", "SQL manifest"),
            "SQL manifest.c_routine_identity_count");
    const std::size_t declared_symbol_count =
        manifest_detail::nonnegative_size(
            json_field(
                root,
                "c_link_symbol_inventory_count",
                "SQL manifest"),
            "SQL manifest.c_link_symbol_inventory_count");
    const std::size_t declared_finfo_count =
        manifest_detail::nonnegative_size(
            json_field(
                root,
                "c_finfo_symbol_inventory_count",
                "SQL manifest"),
            "SQL manifest.c_finfo_symbol_inventory_count");
    if (declared_parent_count != policy.expected_parent_count
        || result.c_routine_identities.size()
            != policy.expected_parent_count) {
        throw std::runtime_error(
            "SQL manifest does not contain exactly "
            + std::to_string(policy.expected_parent_count)
            + " LANGUAGE C parent identities");
    }
    if (declared_symbol_count != policy.expected_symbol_count
        || result.c_link_symbols.size()
            != policy.expected_symbol_count
        || declared_finfo_count != policy.expected_symbol_count
        || result.c_finfo_symbols.size()
            != policy.expected_symbol_count) {
        throw std::runtime_error(
            "SQL manifest does not contain exactly "
            + std::to_string(policy.expected_symbol_count)
            + " C/finfo symbol owners");
    }

    std::set<std::string, std::less<>> expected_finfo;
    for (const std::string& symbol : result.c_link_symbols) {
        expected_finfo.insert("pg_finfo_" + symbol);
    }
    if (expected_finfo
        != std::set<std::string, std::less<>>{
            result.c_finfo_symbols.begin(),
            result.c_finfo_symbols.end()}) {
        throw std::runtime_error(
            "SQL manifest finfo inventory differs from public symbols");
    }

    std::map<
        std::string,
        std::vector<safety_manifest_overload>,
        std::less<>>
        overloads_by_symbol;
    std::map<std::string, std::string, std::less<>>
        module_by_symbol;
    std::set<std::string, std::less<>> overload_identities;
    const json_value::array& modules = json_array(
        json_field(
            root, "fmgr_entrypoint_modules", "SQL manifest"),
        "SQL manifest.fmgr_entrypoint_modules");
    for (std::size_t module_index = 0;
         module_index < modules.size();
         ++module_index) {
        const std::string module_context =
            "SQL manifest.fmgr_entrypoint_modules["
            + std::to_string(module_index) + "]";
        const json_value::object& module =
            json_object(modules[module_index], module_context);
        const std::string generated_module =
            manifest_detail::field_string(
                module, "module_name", module_context);
        const json_value::array& wrappers = json_array(
            json_field(module, "wrappers", module_context),
            module_context + ".wrappers");
        for (std::size_t wrapper_index = 0;
             wrapper_index < wrappers.size();
             ++wrapper_index) {
            const std::string wrapper_context =
                module_context + ".wrappers["
                + std::to_string(wrapper_index) + "]";
            const json_value::object& wrapper =
                json_object(wrappers[wrapper_index], wrapper_context);
            const std::string symbol =
                manifest_detail::field_string(
                    wrapper, "symbol", wrapper_context);
            const std::string finfo =
                manifest_detail::field_string(
                    wrapper, "finfo_symbol", wrapper_context);
            if (finfo != "pg_finfo_" + symbol) {
                throw std::runtime_error(
                    wrapper_context
                    + " has an inconsistent finfo symbol");
            }
            const auto [module, inserted] =
                module_by_symbol.emplace(symbol, generated_module);
            if (!inserted && module->second != generated_module) {
                throw std::runtime_error(
                    wrapper_context
                    + " assigns one public symbol to multiple "
                      "generated modules");
            }
            const json_value::array& overloads = json_array(
                json_field(wrapper, "overloads", wrapper_context),
                wrapper_context + ".overloads");
            for (std::size_t overload_index = 0;
                 overload_index < overloads.size();
                 ++overload_index) {
                const std::string overload_context =
                    wrapper_context + ".overloads["
                    + std::to_string(overload_index) + "]";
                const json_value::object& overload = json_object(
                    overloads[overload_index], overload_context);
                safety_manifest_overload parsed{
                    .sql_identity = manifest_detail::field_string(
                        overload, "sql_identity", overload_context),
                    .cpp_declaration =
                        manifest_detail::field_string(
                            overload,
                            "cpp_declaration",
                            overload_context),
                    .source_path = manifest_detail::field_string(
                        overload, "source_path", overload_context),
                    .source_line =
                        manifest_detail::nonnegative_size(
                            json_field(
                                overload,
                                "source_line",
                                overload_context),
                            overload_context + ".source_line"),
                    .argument_converter_keys =
                        manifest_detail::string_array(
                            json_field(
                                overload,
                                "argument_converter_keys",
                                overload_context),
                            overload_context
                                + ".argument_converter_keys",
                            true,
                            false),
                    .result_converter_key =
                        manifest_detail::field_string(
                            overload,
                            "result_converter_key",
                            overload_context),
                    .injected_call_context =
                        manifest_detail::field_boolean(
                            overload,
                            "injected_call_context",
                            overload_context),
                };
                manifest_detail::validate_runtime_sql_identity(
                    parsed.sql_identity,
                    overload_context + ".sql_identity");
                if (parsed.source_line == 0
                    || !overload_identities.insert(
                            parsed.sql_identity).second) {
                    throw std::runtime_error(
                        overload_context
                        + " has a zero line or duplicate SQL identity");
                }
                overloads_by_symbol[symbol].push_back(
                    std::move(parsed));
            }
            auto& symbol_overloads = overloads_by_symbol[symbol];
            if (symbol_overloads.empty()) {
                throw std::runtime_error(
                    wrapper_context + " has no SQL overloads");
            }
        }
    }

    const json_value::array& owners = json_array(
        json_field(root, "c_symbol_owners", "SQL manifest"),
        "SQL manifest.c_symbol_owners");
    if (owners.size() != policy.expected_symbol_count) {
        throw std::runtime_error(
            "SQL manifest C symbol owners are incomplete");
    }
    std::set<std::string, std::less<>> owner_symbols;
    std::set<std::string, std::less<>> owner_identities;
    std::set<std::string, std::less<>> shared_symbols;
    result.owners.reserve(owners.size());
    for (std::size_t index = 0; index < owners.size(); ++index) {
        const std::string context =
            "SQL manifest.c_symbol_owners["
            + std::to_string(index) + "]";
        const json_value::object& owner =
            json_object(owners[index], context);
        safety_manifest_owner parsed{
            .public_symbol = manifest_detail::field_string(
                owner, "symbol", context),
            .finfo_symbol = manifest_detail::field_string(
                owner, "finfo_symbol", context),
            .sql_identities = manifest_detail::string_array(
                json_field(owner, "routine_identities", context),
                context + ".routine_identities",
                false),
            .implementation_declarations =
                manifest_detail::string_array(
                    json_field(
                        owner,
                        "implementation_declarations",
                        context),
                    context + ".implementation_declarations",
                    false),
        };
        for (std::size_t identity_index = 0;
             identity_index < parsed.sql_identities.size();
             ++identity_index) {
            manifest_detail::validate_runtime_sql_identity(
                parsed.sql_identities[identity_index],
                context + ".routine_identities["
                    + std::to_string(identity_index) + "]");
        }
        const auto generated_module =
            module_by_symbol.find(parsed.public_symbol);
        if (generated_module == module_by_symbol.end()) {
            throw std::runtime_error(
                context + " has no generated module owner");
        }
        parsed.generated_wrapper_module =
            generated_module->second;
        if (!owner_symbols.insert(parsed.public_symbol).second
            || parsed.finfo_symbol
                != "pg_finfo_" + parsed.public_symbol) {
            throw std::runtime_error(
                context + " is duplicated or has the wrong finfo");
        }
        const bool generated = manifest_detail::field_boolean(
            owner, "generated_wrapper", context);
        if (policy.require_complete_codegen && !generated) {
            throw std::runtime_error(
                context + " is not owned by generated boundary code");
        }
        const auto overloads =
            overloads_by_symbol.find(parsed.public_symbol);
        if (overloads == overloads_by_symbol.end()) {
            throw std::runtime_error(
                context + " has no generated wrapper overloads");
        }
        parsed.overloads = overloads->second;
        std::set<std::string, std::less<>>
            overload_declarations;
        for (const safety_manifest_overload& overload :
             parsed.overloads) {
            overload_declarations.insert(
                overload.cpp_declaration);
        }
        if (overload_declarations
            != std::set<std::string, std::less<>>{
                parsed.implementation_declarations.begin(),
                parsed.implementation_declarations.end()}) {
            throw std::runtime_error(
                context
                + " implementation declarations differ from "
                  "generated wrapper overloads");
        }
        if (parsed.sql_identities.size() > 1) {
            shared_symbols.insert(parsed.public_symbol);
        }
        std::set<std::string, std::less<>> overload_identity_set;
        for (const safety_manifest_overload& overload :
             parsed.overloads) {
            overload_identity_set.insert(overload.sql_identity);
        }
        if (overload_identity_set
            != std::set<std::string, std::less<>>{
                parsed.sql_identities.begin(),
                parsed.sql_identities.end()}) {
            throw std::runtime_error(
                context
                + " wrapper overloads differ from owner identities");
        }
        for (const std::string& identity :
             parsed.sql_identities) {
            if (!owner_identities.insert(identity).second
                || manifest_detail::routine_leaf(identity)
                    != parsed.public_symbol) {
                throw std::runtime_error(
                    context
                    + " shares a C symbol across different SQL names "
                      "or duplicates an identity");
            }
        }
        result.owners.push_back(std::move(parsed));
    }
    if (owner_symbols
            != std::set<std::string, std::less<>>{
                result.c_link_symbols.begin(),
                result.c_link_symbols.end()}
        || owner_identities
            != std::set<std::string, std::less<>>{
                result.c_routine_identities.begin(),
                result.c_routine_identities.end()}
        || overload_identities != owner_identities
        || shared_symbols != policy.shared_symbols) {
        throw std::runtime_error(
            "SQL manifest does not close exactly over the reviewed "
            "identity/symbol ownership groups");
    }
    return result;
}

}  // namespace bioseq::codegen::safety
