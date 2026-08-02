module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.codegen.sql.renderer:gucs;

import bioseq.codegen.sql.collector;
import bioseq.codegen.support;
import :manifest;

export namespace bioseq::codegen::sql {

struct rendered_guc_owner_module {
    std::string owner_key;
    std::string source_module;
    std::string module_name;
    std::string content;
    std::vector<std::string> guc_names;
};

struct rendered_guc_registry_module {
    std::string module_name;
    std::string content;
    std::vector<std::string> guc_names;
};

struct rendered_guc_package {
    guc_profile profile{guc_profile::production};
    std::vector<rendered_guc_owner_module> owners;
    rendered_guc_registry_module registry;
};

struct guc_profile_delta_proof {
    std::vector<std::string> added_in_test_indexes;
    std::vector<std::string> removed_from_test_indexes;
    std::vector<std::string> changed_in_test_indexes;
    std::map<std::string, bool, std::less<>>
        owner_descriptor_profile_invariant;
};

struct guc_profile_projection_row {
    std::size_t registration_ordinal{};
    std::string name;
    std::string owner_key;
    std::string source_module;
    std::string source_sha256;
    std::string descriptor_sha256;

    [[nodiscard]] bool operator==(
        const guc_profile_projection_row&) const = default;
};

struct guc_profile_projection {
    guc_profile profile{guc_profile::production};
    std::string ledger_sha256;
    std::string registry_content_sha256;
    std::map<std::string, std::string, std::less<>>
        owner_content_sha256;
    std::vector<guc_profile_projection_row> registrations;

    [[nodiscard]] bool operator==(
        const guc_profile_projection&) const = default;
};

struct guc_profile_delta_evidence {
    guc_profile_delta_proof proof;
    std::string production_registry_content_sha256;
    std::string test_indexes_registry_content_sha256;
    std::map<
        std::string,
        std::pair<std::string, std::string>,
        std::less<>>
        owner_content_sha256;
};

namespace guc_render_detail {

[[nodiscard]] inline std::string cpp_string(std::string_view value) {
    std::string result{"\""};
    result.reserve(value.size() + 2);
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char item : value) {
        switch (item) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (item < 0x20U) {
                    result += "\\x";
                    result.push_back(hex[(item >> 4U) & 0x0fU]);
                    result.push_back(hex[item & 0x0fU]);
                } else {
                    result.push_back(static_cast<char>(item));
                }
        }
    }
    result.push_back('"');
    return result;
}

[[nodiscard]] inline std::string json_string(std::string_view value) {
    return cpp_string(value);
}

inline void render_json_optional_string(
    std::ostringstream& output,
    const std::optional<std::string>& value) {
    if (value) output << json_string(*value);
    else output << "null";
}

inline void render_json_string_array(
    std::ostringstream& output,
    const std::vector<std::string>& values) {
    output << "[";
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) output << ", ";
        output << json_string(values[index]);
    }
    output << "]";
}

inline void render_generated_live_authority(
    std::ostringstream& output,
    const collected_guc& guc) {
    const collected_guc_live_authority& live =
        guc.generated_live_authority;
    static constexpr std::array<std::string_view, 3> hook_names{
        "check", "assign", "show"};
    static constexpr std::array<std::string_view, 3> hook_behaviors{
        "custom_check", "custom_assign", "custom_show"};
    output << "{\"vartype\": " << json_string(live.vartype)
           << ", \"context\": " << json_string(live.context)
           << ", \"unit\": ";
    render_json_optional_string(output, live.unit);
    output << ", \"boot_value\": " << json_string(live.boot_value)
           << ", \"reset_value\": " << json_string(live.reset_value)
           << ", \"current_value\": " << json_string(live.current_value)
           << ", \"minimum\": ";
    render_json_optional_string(output, live.minimum);
    output << ", \"maximum\": ";
    render_json_optional_string(output, live.maximum);
    output << ", \"visible_enum_values\": ";
    render_json_string_array(output, live.visible_enum_values);
    output << ", \"short_description\": "
           << json_string(live.short_description)
           << ", \"extra_description\": ";
    render_json_optional_string(output, live.extra_description);
    output << ", \"raw_flags\": " << live.raw_flags
           << ", \"visible_in_show_all\": "
           << (live.visible_in_show_all ? "true" : "false")
           << ", \"included_in_sample\": "
           << (live.included_in_sample ? "true" : "false")
           << ", \"hooks\": {";
    for (std::size_t index = 0; index < hook_names.size(); ++index) {
        const bool present = live.hook_presence[index];
        if (present != guc.authority.hooks[index].has_value()) {
            throw std::runtime_error(
                "generated live hook presence differs from GUC "
                "authority for '" + guc.name + "'");
        }
        if (index != 0) output << ", ";
        output << json_string(hook_names[index])
               << ": {\"symbol\": ";
        render_json_optional_string(
            output, guc.authority.hooks[index]);
        output << ", \"present\": "
               << (present ? "true" : "false")
               << ", \"behavior\": "
               << json_string(
                    present ? hook_behaviors[index] : "none")
               << "}";
    }
    output << "}}";
}

[[nodiscard]] inline std::string global_cpp_name(
    std::string_view value) {
    return value.starts_with("::")
        ? std::string{value}
        : "::" + std::string{value};
}

[[nodiscard]] inline std::string owner_namespace(
    const collected_guc& guc) {
    const std::string marker = "::settings::";
    const std::size_t offset =
        guc.cpp_qualified_name.find(marker);
    if (offset == std::string::npos) {
        throw std::runtime_error(
            "GUC '" + guc.name
            + "' has no natural settings owner");
    }
    return guc.cpp_qualified_name.substr(0, offset);
}

[[nodiscard]] inline bool bare_identifier(
    std::string_view value) noexcept {
    if (value.empty()
        || !((value.front() >= 'a' && value.front() <= 'z')
             || (value.front() >= 'A' && value.front() <= 'Z')
             || value.front() == '_')) {
        return false;
    }
    return std::ranges::all_of(
        value.substr(1),
        [](char item) {
            return (item >= 'a' && item <= 'z')
                || (item >= 'A' && item <= 'Z')
                || (item >= '0' && item <= '9') || item == '_';
        });
}

[[nodiscard]] inline std::string owner_expression(
    const collected_guc& guc,
    std::string_view expression) {
    if (!bare_identifier(expression)
        || expression == "true"
        || expression == "false"
        || expression == "nullptr"
        || expression == "NULL"
        || expression == "INT_MAX"
        || expression == "INT_MIN") {
        return std::string{expression};
    }
    return global_cpp_name(owner_namespace(guc))
        + "::" + std::string{expression};
}

[[nodiscard]] inline std::string pg_context(
    std::string_view context) {
    if (context == "internal") return "PGC_INTERNAL";
    if (context == "postmaster") return "PGC_POSTMASTER";
    if (context == "sighup") return "PGC_SIGHUP";
    if (context == "superuser_backend") return "PGC_SU_BACKEND";
    if (context == "backend") return "PGC_BACKEND";
    if (context == "superuser_set") return "PGC_SUSET";
    if (context == "user_set") return "PGC_USERSET";
    throw std::runtime_error(
        "unsupported generated GUC context '"
        + std::string{context} + "'");
}

[[nodiscard]] inline std::string pg_flags(
    const collected_guc& guc) {
    std::vector<std::string> values;
    for (const std::string& flag : guc.flags) {
        if (flag == "no_show_all") values.emplace_back("GUC_NO_SHOW_ALL");
        else if (flag == "not_in_sample")
            values.emplace_back("GUC_NOT_IN_SAMPLE");
        else {
            throw std::runtime_error(
                "unsupported generated GUC flag '" + flag + "'");
        }
    }
    if (guc.unit) {
        if (*guc.unit == "kb") values.emplace_back("GUC_UNIT_KB");
        else if (*guc.unit == "mb") values.emplace_back("GUC_UNIT_MB");
        else if (*guc.unit == "blocks")
            values.emplace_back("GUC_UNIT_BLOCKS");
        else if (*guc.unit == "ms") values.emplace_back("GUC_UNIT_MS");
        else if (*guc.unit == "s") values.emplace_back("GUC_UNIT_S");
        else if (*guc.unit == "min") values.emplace_back("GUC_UNIT_MIN");
        else {
            throw std::runtime_error(
                "unsupported generated GUC unit '" + *guc.unit + "'");
        }
    }
    if (values.empty()) return "0";
    std::string result;
    for (const std::string& value : values) {
        if (!result.empty()) result += " | ";
        result += value;
    }
    return result;
}

[[nodiscard]] inline std::string description_argument(
    std::string_view value) {
    return value.empty() ? "nullptr" : cpp_string(value);
}

[[nodiscard]] inline std::string hook_argument(
    const std::optional<std::string>& hook) {
    return hook ? global_cpp_name(*hook) : "nullptr";
}

[[nodiscard]] inline std::string plan_cache_assign_hook_name(
        const collected_guc& guc) {
    return "assign_" + guc.member + "_and_reset_plan_cache";
}

[[nodiscard]] inline std::string assign_hook_argument(
        const collected_guc& guc) {
    return guc.reset_plan_cache
        ? plan_cache_assign_hook_name(guc)
        : hook_argument(guc.authority.hooks[1]);
}

inline void render_hook_arguments(
    std::ostringstream& output,
    const collected_guc& guc) {
    output << hook_argument(guc.authority.hooks[0]) << ", "
           << assign_hook_argument(guc) << ", "
           << hook_argument(guc.authority.hooks[2]);
}

[[nodiscard]] inline std::string registration_function_name(
    const collected_guc& guc) {
    return "register_" + guc.member;
}

[[nodiscard]] inline std::string owner_facet_namespace(
    guc_profile profile,
    std::string_view owner_key) {
    return "::bioseq::sql::generated::"
        + std::string{guc_profile_name(profile)}
        + "::" + std::string{owner_key} + "_gucs";
}

[[nodiscard]] inline std::string owner_module_name(
    guc_profile profile,
    std::string_view owner_key,
    std::string_view generated_module_prefix =
        "bioseq.sql.generated") {
    return std::string{generated_module_prefix} + "."
        + std::string{guc_profile_name(profile)}
        + "." + std::string{owner_key} + ".gucs";
}

inline void render_condition_begin(
    std::ostringstream& output,
    std::string_view condition) {
    if (!condition.empty()) {
        output << "#if " << condition << "\n";
    }
}

inline void render_condition_end(
    std::ostringstream& output,
    std::string_view condition) {
    if (!condition.empty()) {
        output << "#endif\n";
    }
}

inline void render_enum_table(
    std::ostringstream& output,
    const collected_guc& guc) {
    output << "const config_enum_entry " << guc.member
           << "_values[] = {\n";
    for (const collected_guc_enum_value& value : guc.enum_values) {
        render_condition_begin(output, value.active_condition);
        output << "    {" << cpp_string(value.name)
               << ", static_cast<int>("
               << global_cpp_name(value.cpp_name)
               << "), " << (value.hidden ? "true" : "false")
               << "},\n";
        render_condition_end(output, value.active_condition);
    }
    output << "    {nullptr, 0, false},\n"
           << "};\n\n";
}

[[nodiscard]] inline std::string assign_value_type(
        const collected_guc& guc) {
    switch (guc.kind) {
        case guc_kind::boolean: return "bool";
        case guc_kind::integer:
        case guc_kind::enumeration:
            return "int";
        case guc_kind::real: return "double";
        case guc_kind::string: return "const char*";
    }
    throw std::runtime_error(
        "unsupported plan-cache GUC kind for '" + guc.name + "'");
}

inline void render_plan_cache_assign_hook(
    std::ostringstream& output,
    const collected_guc& guc) {
    if (!guc.reset_plan_cache) return;
    const std::string cell =
        global_cpp_name(guc.cpp_qualified_name);
    output << "inline void "
           << plan_cache_assign_hook_name(guc) << "("
           << assign_value_type(guc)
           << " new_value, void* extra) {\n"
           << "    const bool changed = ";
    switch (guc.kind) {
        case guc_kind::boolean:
        case guc_kind::integer:
        case guc_kind::real:
            output << cell << " != new_value;\n";
            break;
        case guc_kind::enumeration:
            output << "static_cast<int>(" << cell
                   << ".value()) != new_value;\n";
            break;
        case guc_kind::string:
            output
                << "(" << cell << " == nullptr) != "
                   "(new_value == nullptr)\n"
                << "        || (" << cell
                << " != nullptr && new_value != nullptr\n"
                << "            && std::strcmp(" << cell
                << ", new_value) != 0);\n";
            break;
    }
    if (guc.authored_assign_hook) {
        output << "    "
               << global_cpp_name(*guc.authored_assign_hook)
               << "(new_value, extra);\n";
    } else {
        output << "    static_cast<void>(extra);\n";
    }
    output << "    if (changed) ResetPlanCache();\n"
           << "}\n\n";
}

inline void render_planner_assertions(
    std::ostringstream& output,
    const collected_guc& guc) {
    if (guc.name != "bioseq.planner_debug_level") return;
    static constexpr std::pair<std::string_view, std::string_view>
        levels[] = {
            {"debug5", "DEBUG5"},
            {"debug4", "DEBUG4"},
            {"debug3", "DEBUG3"},
            {"debug2", "DEBUG2"},
            {"debug1", "DEBUG1"},
            {"log", "LOG"},
            {"info", "INFO"},
            {"notice", "NOTICE"},
            {"warning", "WARNING"},
        };
    for (const auto& [value, macro] : levels) {
        output << "static_assert(static_cast<int>("
               << global_cpp_name(guc.enum_cpp_name) << "::" << value
               << ") == " << macro
               << ", \"PostgreSQL log-level wire value changed\");\n";
    }
    output << "\n";
}

inline void render_primitive_registration(
    std::ostringstream& output,
    const collected_guc& guc) {
    const std::string cell =
        global_cpp_name(guc.cpp_qualified_name);
    const std::string boot_value =
        owner_expression(guc, guc.default_expression);
    const std::string common =
        cpp_string(guc.name) + ", "
        + cpp_string(guc.short_description) + ", "
        + description_argument(guc.long_description) + ", &"
        + cell + ", "
        + boot_value;
    output << "inline void "
           << registration_function_name(guc) << "() {\n"
           << "    static bool complete = false;\n"
           << "    if (complete) return;\n"
           << "    auto define_variable = [] {\n        ";
    switch (guc.kind) {
        case guc_kind::boolean:
            output << "DefineCustomBoolVariable(\n"
                   << "            " << common << ",\n"
                   << "            " << pg_context(guc.context) << ", "
                   << pg_flags(guc) << ", ";
            render_hook_arguments(output, guc);
            output << ");\n";
            break;
        case guc_kind::integer:
            if (!guc.minimum_expression || !guc.maximum_expression) {
                throw std::runtime_error(
                    "integer GUC '" + guc.name + "' has no bounds");
            }
            output << "DefineCustomIntVariable(\n"
                   << "            " << common << ",\n"
                   << "            "
                   << owner_expression(guc, *guc.minimum_expression)
                   << ", "
                   << owner_expression(guc, *guc.maximum_expression)
                   << ", " << pg_context(guc.context) << ", "
                   << pg_flags(guc) << ", ";
            render_hook_arguments(output, guc);
            output << ");\n";
            break;
        case guc_kind::real:
            if (!guc.minimum_expression || !guc.maximum_expression) {
                throw std::runtime_error(
                    "real GUC '" + guc.name + "' has no bounds");
            }
            output << "DefineCustomRealVariable(\n"
                   << "            " << common << ",\n"
                   << "            "
                   << owner_expression(guc, *guc.minimum_expression)
                   << ", "
                   << owner_expression(guc, *guc.maximum_expression)
                   << ", " << pg_context(guc.context) << ", "
                   << pg_flags(guc) << ", ";
            render_hook_arguments(output, guc);
            output << ");\n";
            break;
        case guc_kind::string:
            output << "DefineCustomStringVariable(\n"
                   << "            " << cpp_string(guc.name) << ", "
                   << cpp_string(guc.short_description) << ", "
                   << description_argument(guc.long_description) << ", &"
                   << cell << ", " << boot_value << ", "
                   << pg_context(guc.context) << ", "
                   << pg_flags(guc) << ", ";
            render_hook_arguments(output, guc);
            output << ");\n";
            break;
        case guc_kind::enumeration:
            throw std::runtime_error(
                "enum GUC reached primitive registration renderer");
    }
    output << "    };\n"
           << "    static_assert(std::is_trivially_destructible_v<\n"
           << "        decltype(define_variable)>);\n"
           << "    ::bioseq::pg_utils::invoke_pg_as_cpp(define_variable);\n"
           << "    complete = true;\n"
           << "}\n\n";
}

inline void render_enum_registration(
    std::ostringstream& output,
    const collected_guc& guc) {
    render_planner_assertions(output, guc);
    output << "inline void "
           << registration_function_name(guc) << "() {\n"
           << "    using enum_type = "
           << global_cpp_name(guc.enum_cpp_name) << ";\n"
           << "    static const "
           << "::bioseq::pg_utils::postgres_enum_registration<enum_type>\n"
           << "        registration{\n"
           << "            .name = " << cpp_string(guc.name) << ",\n"
           << "            .short_description = "
           << cpp_string(guc.short_description) << ",\n"
           << "            .long_description = "
           << description_argument(guc.long_description) << ",\n"
           << "            .boot_value = "
           << global_cpp_name(guc.default_expression) << ",\n"
           << "            .entries = " << guc.member << "_values,\n"
           << "            .context = " << pg_context(guc.context) << ",\n"
           << "            .flags = " << pg_flags(guc) << ",\n"
           << "            .check_hook = "
           << hook_argument(guc.authority.hooks[0]) << ",\n"
           << "            .assign_hook = "
           << assign_hook_argument(guc) << ",\n"
           << "            .show_hook = "
           << hook_argument(guc.authority.hooks[2]) << ",\n"
           << "        };\n"
           << "    ::bioseq::pg_utils::guc_registration_access::init(\n"
           << "        " << global_cpp_name(guc.cpp_qualified_name)
           << ", registration);\n"
           << "}\n\n";
}

[[nodiscard]] inline rendered_guc_owner_module render_owner(
    const guc_inventory& inventory,
    std::string_view owner_key,
    const std::vector<const collected_guc*>& gucs,
    std::string_view generated_module_prefix,
    std::string_view boundary_module) {
    if (gucs.empty()) {
        throw std::runtime_error(
            "configured GUC owner '" + std::string{owner_key}
            + "' has no registrations");
    }
    const std::string source_module = gucs.front()->source_module;
    for (const collected_guc* guc : gucs) {
        if (guc->source_module != source_module) {
            throw std::runtime_error(
                "GUC owner '" + std::string{owner_key}
                + "' spans more than one authored module");
        }
    }
    const std::string module_name =
        owner_module_name(
            inventory.profile, owner_key, generated_module_prefix);
    const std::string cpp_namespace =
        owner_facet_namespace(inventory.profile, owner_key);
    std::ostringstream output;
    output
        << "// Generated by bioseq_codegen. Do not edit.\n"
        << "module;\n\n"
        << "#include \"common.h\"\n\n"
        << "#include \"utils/plancache.h\"\n\n"
        << "#include <cstring>\n"
        << "#include <type_traits>\n\n"
        << "export module " << module_name << ";\n\n"
        << "import " << source_module << ";\n"
        << "import " << boundary_module << ";\n\n"
        << "namespace {\n\n";
    for (const collected_guc* guc : gucs) {
        render_condition_begin(output, guc->active_condition);
        if (guc->kind == guc_kind::enumeration) {
            render_enum_table(output, *guc);
        }
        render_plan_cache_assign_hook(output, *guc);
        render_condition_end(output, guc->active_condition);
    }
    output << "}  // namespace\n\n"
           << "export namespace "
           << cpp_namespace.substr(2) << " {\n\n"
           << "inline constexpr unsigned registration_count = "
           << gucs.size() << "U;\n\n";
    std::vector<std::string> names;
    names.reserve(gucs.size());
    for (const collected_guc* guc : gucs) {
        render_condition_begin(output, guc->active_condition);
        if (guc->kind == guc_kind::enumeration) {
            render_enum_registration(output, *guc);
        } else {
            render_primitive_registration(output, *guc);
        }
        render_condition_end(output, guc->active_condition);
        names.push_back(guc->name);
    }
    output << "}  // namespace "
           << cpp_namespace.substr(2) << "\n";
    return {
        .owner_key = std::string{owner_key},
        .source_module = source_module,
        .module_name = module_name,
        .content = std::move(output).str(),
        .guc_names = std::move(names),
    };
}

[[nodiscard]] inline rendered_guc_registry_module render_registry(
    const guc_inventory& inventory,
    const std::vector<rendered_guc_owner_module>& owners,
    std::string_view generated_module_prefix) {
    const std::string profile{guc_profile_name(inventory.profile)};
    const std::string module_name =
        std::string{generated_module_prefix}
        + "." + profile + ".guc_registry";
    std::ostringstream output;
    output << "// Generated by bioseq_codegen. Do not edit.\n"
           << "export module " << module_name << ";\n\n";
    for (const rendered_guc_owner_module& owner : owners) {
        output << "import " << owner.module_name << ";\n";
    }
    output << "\nexport namespace bioseq::sql::generated::"
           << profile << " {\n\n"
           << "inline constexpr unsigned registration_count = "
           << inventory.gucs.size() << "U;\n"
           << "inline constexpr bool test_indexes_profile = "
           << (inventory.profile == guc_profile::test_indexes
                   ? "true"
                   : "false")
           << ";\n\n"
           << "inline void register_gucs() {\n"
           << "    static bool complete = false;\n"
           << "    if (complete) return;\n";
    std::vector<std::string> names;
    names.reserve(inventory.gucs.size());
    for (const collected_guc& guc : inventory.gucs) {
        render_condition_begin(output, guc.active_condition);
        output << "    "
               << owner_facet_namespace(
                    inventory.profile, guc.owner_key)
               << "::" << registration_function_name(guc) << "();\n";
        render_condition_end(output, guc.active_condition);
        names.push_back(guc.name);
    }
    output << "    complete = true;\n"
           << "}\n\n"
           << "}  // namespace bioseq::sql::generated::"
           << profile << "\n";
    return {
        .module_name = module_name,
        .content = std::move(output).str(),
        .guc_names = std::move(names),
    };
}

[[nodiscard]] inline std::string descriptor_signature(
    const collected_guc& guc) {
    std::ostringstream output;
    output << guc.name << '\0'
           << guc.owner_key << '\0'
           << guc.member << '\0'
           << guc_kind_name(guc.kind) << '\0'
           << guc.cpp_qualified_name << '\0'
           << guc.active_condition << '\0'
           << guc.default_expression << '\0'
           << guc.minimum_expression.value_or("") << '\0'
           << guc.maximum_expression.value_or("") << '\0'
           << guc.context << '\0'
           << guc.unit.value_or("") << '\0'
           << guc.short_description << '\0'
           << guc.long_description << '\0'
           << guc.enum_cpp_name << '\0';
    output << guc.reset_plan_cache << '\0'
           << guc.authored_assign_hook.value_or("") << '\0';
    for (const std::optional<std::string>& hook :
         guc.authority.hooks) {
        output << hook.value_or("") << '\0';
    }
    for (const std::string& flag : guc.flags) {
        output << flag << '\0';
    }
    for (const collected_guc_enum_value& value : guc.enum_values) {
        output << value.name << '\0'
               << value.cpp_name << '\0'
               << value.wire_expression << '\0'
               << value.active_condition << '\0'
               << value.alias_of.value_or("") << '\0'
               << value.hidden << '\0'
               << value.compiled << '\0';
    }
    const collected_guc_live_authority& live =
        guc.generated_live_authority;
    output << live.vartype << '\0'
           << live.context << '\0'
           << live.unit.value_or("") << '\0'
           << live.boot_value << '\0'
           << live.reset_value << '\0'
           << live.current_value << '\0'
           << live.minimum.value_or("") << '\0'
           << live.maximum.value_or("") << '\0'
           << live.short_description << '\0'
           << live.extra_description.value_or("") << '\0'
           << live.raw_flags << '\0'
           << live.visible_in_show_all << '\0'
           << live.included_in_sample << '\0';
    for (const std::string& value : live.visible_enum_values) {
        output << value << '\0';
    }
    for (const bool present : live.hook_presence) {
        output << present << '\0';
    }
    return std::move(output).str();
}

}  // namespace guc_render_detail

[[nodiscard]] inline std::string
render_primitive_guc_registration_contract_probe(
        const collected_guc& guc) {
    std::ostringstream output;
    guc_render_detail::render_plan_cache_assign_hook(output, guc);
    guc_render_detail::render_primitive_registration(output, guc);
    return std::move(output).str();
}

[[nodiscard]] inline rendered_guc_package render_guc_package(
    const guc_inventory& inventory,
    std::string_view generated_module_prefix =
        "bioseq.sql.generated",
    std::string_view boundary_module =
        "bioseq.pg_utils.sql_boundary") {
    static constexpr std::string_view owner_order[] = {
        "bioseq",
        "scheduler",
        "storage",
        "aligner",
    };
    std::map<
        std::string,
        std::vector<const collected_guc*>,
        std::less<>>
        by_owner;
    for (const collected_guc& guc : inventory.gucs) {
        by_owner[guc.owner_key].push_back(&guc);
    }
    if (by_owner.size() != std::size(owner_order)) {
        throw std::runtime_error(
            "GUC generation requires exactly four natural owners");
    }
    rendered_guc_package result{.profile = inventory.profile};
    for (const std::string_view owner_key : owner_order) {
        const auto found = by_owner.find(owner_key);
        if (found == by_owner.end()) {
            throw std::runtime_error(
                "GUC generation is missing natural owner '"
                + std::string{owner_key} + "'");
        }
        result.owners.push_back(
            guc_render_detail::render_owner(
                inventory,
                owner_key,
                found->second,
                generated_module_prefix,
                boundary_module));
    }
    result.registry =
        guc_render_detail::render_registry(
            inventory, result.owners, generated_module_prefix);
    if (result.registry.guc_names.size() != inventory.gucs.size()) {
        throw std::runtime_error(
            "generated GUC registry lost registration rows");
    }
    return result;
}

[[nodiscard]] inline guc_profile_projection
make_guc_profile_projection(
    const guc_inventory& inventory,
    const rendered_guc_package& package) {
    if (inventory.profile != package.profile
        || package.registry.guc_names.size()
            != inventory.gucs.size()) {
        throw std::runtime_error(
            "GUC profile projection received mismatched inventory and "
            "package profiles");
    }
    guc_profile_projection projection{
        .profile = inventory.profile,
        .ledger_sha256 = inventory.ledger_sha256,
        .registry_content_sha256 =
            sha256_hex(package.registry.content),
    };
    for (const rendered_guc_owner_module& owner :
         package.owners) {
        if (!projection.owner_content_sha256.emplace(
                owner.owner_key,
                sha256_hex(owner.content)).second) {
            throw std::runtime_error(
                "GUC profile projection duplicates owner '"
                + owner.owner_key + "'");
        }
    }
    projection.registrations.reserve(inventory.gucs.size());
    std::set<std::string, std::less<>> names;
    for (const collected_guc& guc : inventory.gucs) {
        if (!names.insert(guc.name).second) {
            throw std::runtime_error(
                "GUC profile projection duplicates registration '"
                + guc.name + "'");
        }
        projection.registrations.push_back({
            .registration_ordinal = guc.registration_ordinal,
            .name = guc.name,
            .owner_key = guc.owner_key,
            .source_module = guc.source_module,
            .source_sha256 = guc.source_sha256,
            .descriptor_sha256 = sha256_hex(
                guc_render_detail::descriptor_signature(guc)),
        });
    }
    return projection;
}

[[nodiscard]] inline std::string guc_profile_projection_row_signature(
    const guc_profile_projection_row& row) {
    std::string result =
        std::to_string(row.registration_ordinal);
    const auto append =
        [&result](std::string_view value) {
            result.push_back('\0');
            result.append(value);
        };
    append(row.name);
    append(row.owner_key);
    append(row.source_module);
    append(row.source_sha256);
    append(row.descriptor_sha256);
    return result;
}

[[nodiscard]] inline guc_profile_delta_proof prove_guc_profile_delta(
    const guc_profile_projection& production,
    const guc_profile_projection& test_indexes) {
    if (production.profile != guc_profile::production
        || test_indexes.profile != guc_profile::test_indexes) {
        throw std::runtime_error(
            "GUC profile projection delta requires production then "
            "test_indexes");
    }
    if (production.ledger_sha256.size() != 64
        || production.ledger_sha256
            != test_indexes.ledger_sha256) {
        throw std::runtime_error(
            "GUC profile projection delta compared different parity "
            "ledgers");
    }
    std::map<
        std::string,
        const guc_profile_projection_row*,
        std::less<>>
        production_by_name;
    std::map<
        std::string,
        const guc_profile_projection_row*,
        std::less<>>
        test_by_name;
    const auto index =
        [](const std::vector<guc_profile_projection_row>& rows,
           auto& by_name,
           std::string_view role) {
            std::set<std::size_t> ordinals;
            for (const guc_profile_projection_row& row : rows) {
                if (row.name.empty() || row.owner_key.empty()
                    || row.source_module.empty()
                    || row.source_sha256.size() != 64
                    || row.descriptor_sha256.size() != 64
                    || !ordinals.insert(
                            row.registration_ordinal).second
                    || !by_name.emplace(row.name, &row).second) {
                    throw std::runtime_error(
                        "GUC " + std::string{role}
                        + " profile projection is malformed or "
                          "duplicated");
                }
            }
        };
    index(
        production.registrations,
        production_by_name,
        "production");
    index(
        test_indexes.registrations,
        test_by_name,
        "test_indexes");
    guc_profile_delta_proof proof;
    for (const auto& [name, row] : test_by_name) {
        const auto baseline = production_by_name.find(name);
        if (baseline == production_by_name.end()) {
            proof.added_in_test_indexes.push_back(name);
        } else if (
            guc_profile_projection_row_signature(*row)
            != guc_profile_projection_row_signature(
                *baseline->second)) {
            proof.changed_in_test_indexes.push_back(name);
        }
    }
    for (const auto& [name, row] : production_by_name) {
        static_cast<void>(row);
        if (!test_by_name.contains(name)) {
            proof.removed_from_test_indexes.push_back(name);
        }
    }
    static constexpr std::string_view owners[] = {
        "bioseq",
        "scheduler",
        "storage",
        "aligner",
    };
    for (const std::string_view owner : owners) {
        bool invariant = true;
        for (const auto& [name, row] : production_by_name) {
            if (row->owner_key != owner) continue;
            const auto test = test_by_name.find(name);
            if (test == test_by_name.end()
                || guc_profile_projection_row_signature(*row)
                    != guc_profile_projection_row_signature(
                        *test->second)) {
                invariant = false;
                break;
            }
        }
        if (invariant) {
            for (const auto& [name, row] : test_by_name) {
                if (row->owner_key == owner
                    && !production_by_name.contains(name)) {
                    invariant = false;
                    break;
                }
            }
        }
        proof.owner_descriptor_profile_invariant.emplace(
            std::string{owner}, invariant);
    }
    if (proof.added_in_test_indexes
            != std::vector<std::string>{
                "bioseq.test_force_paper_index_failure"}
        || !proof.removed_from_test_indexes.empty()
        || !proof.changed_in_test_indexes.empty()
        || proof.owner_descriptor_profile_invariant.at("bioseq")
        || !proof.owner_descriptor_profile_invariant.at("scheduler")
        || !proof.owner_descriptor_profile_invariant.at("storage")
        || !proof.owner_descriptor_profile_invariant.at("aligner")) {
        throw std::runtime_error(
            "GUC production/test_indexes projection delta is not the "
            "single documented test-only setting");
    }
    return proof;
}

[[nodiscard]] inline guc_profile_delta_evidence
make_guc_profile_delta_evidence(
    const guc_profile_projection& production,
    const guc_profile_projection& test_indexes) {
    if (production.owner_content_sha256.size()
            != test_indexes.owner_content_sha256.size()
        || production.owner_content_sha256.size() != 4) {
        throw std::runtime_error(
            "GUC profile projections do not seal the same four owners");
    }
    guc_profile_delta_evidence evidence{
        .proof = prove_guc_profile_delta(
            production, test_indexes),
        .production_registry_content_sha256 =
            production.registry_content_sha256,
        .test_indexes_registry_content_sha256 =
            test_indexes.registry_content_sha256,
    };
    for (const auto& [owner, production_sha256] :
         production.owner_content_sha256) {
        const auto test =
            test_indexes.owner_content_sha256.find(owner);
        if (test == test_indexes.owner_content_sha256.end()
            || production_sha256.size() != 64
            || test->second.size() != 64) {
            throw std::runtime_error(
                "GUC profile projections have mismatched owner "
                "artifact seals");
        }
        evidence.owner_content_sha256.emplace(
            owner,
            std::pair{production_sha256, test->second});
    }
    if (evidence.production_registry_content_sha256.size() != 64
        || evidence.test_indexes_registry_content_sha256.size()
            != 64) {
        throw std::runtime_error(
            "GUC profile projection registry seal is malformed");
    }
    return evidence;
}

[[nodiscard]] inline guc_profile_delta_proof prove_guc_profile_delta(
    const guc_inventory& production,
    const guc_inventory& test_indexes) {
    if (production.profile != guc_profile::production
        || test_indexes.profile != guc_profile::test_indexes) {
        throw std::runtime_error(
            "GUC profile delta proof requires production then test_indexes");
    }
    if (production.ledger_sha256 != test_indexes.ledger_sha256) {
        throw std::runtime_error(
            "GUC profile delta proof compared different parity ledgers");
    }
    std::map<std::string, const collected_guc*, std::less<>>
        production_by_name;
    std::map<std::string, const collected_guc*, std::less<>>
        test_by_name;
    for (const collected_guc& guc : production.gucs) {
        production_by_name.emplace(guc.name, &guc);
    }
    for (const collected_guc& guc : test_indexes.gucs) {
        test_by_name.emplace(guc.name, &guc);
    }
    guc_profile_delta_proof proof;
    for (const auto& [name, guc] : test_by_name) {
        const auto baseline = production_by_name.find(name);
        if (baseline == production_by_name.end()) {
            proof.added_in_test_indexes.push_back(name);
        } else if (
            guc_render_detail::descriptor_signature(*guc)
            != guc_render_detail::descriptor_signature(
                *baseline->second)) {
            proof.changed_in_test_indexes.push_back(name);
        }
    }
    for (const auto& [name, guc] : production_by_name) {
        static_cast<void>(guc);
        if (!test_by_name.contains(name)) {
            proof.removed_from_test_indexes.push_back(name);
        }
    }
    static constexpr std::string_view owners[] = {
        "bioseq",
        "scheduler",
        "storage",
        "aligner",
    };
    for (const std::string_view owner : owners) {
        bool invariant = true;
        for (const auto& [name, guc] : production_by_name) {
            if (guc->owner_key != owner) continue;
            const auto test = test_by_name.find(name);
            if (test == test_by_name.end()
                || guc_render_detail::descriptor_signature(*guc)
                    != guc_render_detail::descriptor_signature(
                        *test->second)) {
                invariant = false;
                break;
            }
        }
        if (invariant) {
            for (const auto& [name, guc] : test_by_name) {
                if (guc->owner_key == owner
                    && !production_by_name.contains(name)) {
                    invariant = false;
                    break;
                }
            }
        }
        proof.owner_descriptor_profile_invariant.emplace(
            std::string{owner}, invariant);
    }
    if (proof.added_in_test_indexes
            != std::vector<std::string>{
                "bioseq.test_force_paper_index_failure"}
        || !proof.removed_from_test_indexes.empty()
        || !proof.changed_in_test_indexes.empty()
        || proof.owner_descriptor_profile_invariant.at("bioseq")
        || !proof.owner_descriptor_profile_invariant.at("scheduler")
        || !proof.owner_descriptor_profile_invariant.at("storage")
        || !proof.owner_descriptor_profile_invariant.at("aligner")) {
        throw std::runtime_error(
            "GUC production/test_indexes delta is not the single "
            "documented test-only setting");
    }
    return proof;
}

[[nodiscard]] inline std::string merge_guc_manifest(
    std::string_view graph_manifest,
    const guc_inventory& inventory,
    const rendered_guc_package& package,
    const guc_inventory* production_inventory = nullptr,
    const rendered_guc_package* production_package = nullptr,
    const guc_inventory* test_inventory = nullptr,
    const rendered_guc_package* test_package = nullptr,
    const guc_profile_delta_evidence* sealed_delta = nullptr) {
    std::string result{graph_manifest};
    while (!result.empty()
           && (result.back() == '\n' || result.back() == '\r'
               || result.back() == ' ' || result.back() == '\t')) {
        result.pop_back();
    }
    if (result.empty() || result.back() != '}') {
        throw std::runtime_error(
            "cannot append GUC provenance to malformed graph manifest");
    }
    result.pop_back();
    std::ostringstream output;
    output << result
           << ",\n  \"guc_generation\": {\n"
           << "    \"profile\": "
           << guc_render_detail::json_string(guc_profile_name(
                inventory.profile)) << ",\n"
           << "    \"description_authority\": "
           << "\"cleaned_doxygen_only\",\n"
           << "    \"parity_ledger\": {\n"
           << "      \"path\": "
           << guc_render_detail::json_string(
                detail::manifest_repository_path(
                    inventory.ledger_path,
                    "guc_generation.parity_ledger.path")) << ",\n"
           << "      \"sha256\": "
           << guc_render_detail::json_string(inventory.ledger_sha256)
           << "\n    },\n"
           << "    \"registration_count\": "
           << inventory.gucs.size() << ",\n"
           << "    \"registry_module\": "
           << guc_render_detail::json_string(package.registry.module_name)
           << ",\n"
           << "    \"registry_content_sha256\": "
           << guc_render_detail::json_string(
                sha256_hex(package.registry.content)) << ",\n"
           << "    \"owner_facets\": [\n";
    for (std::size_t index = 0; index < package.owners.size(); ++index) {
        const rendered_guc_owner_module& owner = package.owners[index];
        output << "      {\"owner\": "
               << guc_render_detail::json_string(owner.owner_key)
               << ", \"source_module\": "
               << guc_render_detail::json_string(owner.source_module)
               << ", \"generated_module\": "
               << guc_render_detail::json_string(owner.module_name)
               << ", \"content_sha256\": "
               << guc_render_detail::json_string(
                    sha256_hex(owner.content))
               << ", \"registration_count\": "
               << owner.guc_names.size() << "}"
               << (index + 1 == package.owners.size() ? "\n" : ",\n");
    }
    output << "    ],\n";
    const std::size_t delta_pair_count =
        static_cast<std::size_t>(production_inventory != nullptr)
        + static_cast<std::size_t>(production_package != nullptr)
        + static_cast<std::size_t>(test_inventory != nullptr)
        + static_cast<std::size_t>(test_package != nullptr);
    if (delta_pair_count != 0 && delta_pair_count != 4) {
        throw std::runtime_error(
            "GUC profile delta rendering received an incomplete "
            "inventory/package pair");
    }
    if (delta_pair_count == 4 && sealed_delta != nullptr) {
        throw std::runtime_error(
            "GUC profile delta rendering received two authorities");
    }
    std::optional<guc_profile_delta_evidence> derived_delta;
    if (delta_pair_count == 4) {
        derived_delta.emplace(
            make_guc_profile_delta_evidence(
                make_guc_profile_projection(
                    *production_inventory, *production_package),
                make_guc_profile_projection(
                    *test_inventory, *test_package)));
    }
    const guc_profile_delta_evidence* delta =
        sealed_delta != nullptr
        ? sealed_delta
        : derived_delta
        ? &*derived_delta
        : nullptr;
    if (delta != nullptr) {
        const guc_profile_delta_proof& proof = delta->proof;
        output
            << "    \"profile_delta_proof\": {\n"
            << "      \"production_registry_content_sha256\": "
            << guc_render_detail::json_string(
                delta->production_registry_content_sha256)
            << ",\n"
            << "      \"test_indexes_registry_content_sha256\": "
            << guc_render_detail::json_string(
                delta->test_indexes_registry_content_sha256)
            << ",\n"
            << "      \"added_in_test_indexes\": [";
        for (std::size_t index = 0;
             index < proof.added_in_test_indexes.size();
             ++index) {
            if (index != 0) output << ", ";
            output << guc_render_detail::json_string(
                proof.added_in_test_indexes[index]);
        }
        output << "],\n"
               << "      \"removed_from_test_indexes\": [],\n"
               << "      \"changed_in_test_indexes\": [],\n"
               << "      \"owner_descriptor_profile_invariant\": {\n";
        std::size_t owner_index = 0;
        for (const auto& [owner, invariant] :
             proof.owner_descriptor_profile_invariant) {
            output << "        "
                   << guc_render_detail::json_string(owner)
                   << ": " << (invariant ? "true" : "false")
                   << (++owner_index
                               == proof.owner_descriptor_profile_invariant
                                      .size()
                           ? "\n"
                           : ",\n");
        }
        output << "      },\n"
               << "      \"owner_artifacts\": {\n";
        std::size_t artifact_index = 0;
        for (const auto& [owner, hashes] :
             delta->owner_content_sha256) {
            output << "        "
                   << guc_render_detail::json_string(owner)
                   << ": {\"production_content_sha256\": "
                   << guc_render_detail::json_string(hashes.first)
                   << ", \"test_indexes_content_sha256\": "
                   << guc_render_detail::json_string(hashes.second)
                   << "}"
                   << (++artifact_index
                               == delta->owner_content_sha256.size()
                           ? "\n"
                           : ",\n");
        }
        output << "      }\n"
               << "    },\n";
    }
    output
           << "    \"registrations\": [\n";
    for (std::size_t index = 0; index < inventory.gucs.size(); ++index) {
        const collected_guc& guc = inventory.gucs[index];
        const collected_guc_authority& authority = guc.authority;
        output << "      {\"authority\": {"
               << "\"registration_ordinal\": "
               << guc.registration_ordinal
               << ", \"source_owner\": "
               << guc_render_detail::json_string(
                    authority.source_owner)
               << ", \"name\": "
               << guc_render_detail::json_string(guc.name)
               << ", \"group\": "
               << guc_render_detail::json_string(authority.group)
               << ", \"member\": "
               << guc_render_detail::json_string(guc.member)
               << ", \"kind\": "
               << guc_render_detail::json_string(
                    guc_kind_name(guc.kind))
               << ", \"storage_kind\": "
               << guc_render_detail::json_string(
                    authority.storage_kind)
               << ", \"storage\": "
               << guc_render_detail::json_string(
                    authority.legacy_storage)
               << ", \"default_expression\": "
               << guc_render_detail::json_string(
                    authority.default_expression)
               << ", \"minimum_expression\": ";
        guc_render_detail::render_json_optional_string(
            output, authority.minimum_expression);
        output << ", \"maximum_expression\": ";
        guc_render_detail::render_json_optional_string(
            output, authority.maximum_expression);
        output << ", \"context\": "
               << guc_render_detail::json_string(authority.context)
               << ", \"flags\": ";
        guc_render_detail::render_json_string_array(
            output, authority.flags);
        output << ", \"unit\": ";
        guc_render_detail::render_json_optional_string(
            output, authority.unit);
        output << ", \"hooks\": [";
        for (std::size_t hook_index = 0;
             hook_index < authority.hooks.size();
             ++hook_index) {
            if (hook_index != 0) output << ", ";
            guc_render_detail::render_json_optional_string(
                output, authority.hooks[hook_index]);
        }
        output << "], \"feature_predicate\": "
               << guc_render_detail::json_string(
                    authority.feature_predicate)
               << ", \"profiles\": ";
        guc_render_detail::render_json_string_array(
            output, authority.profiles);
        output << ", \"default_semantic\": ";
        guc_render_detail::render_json_optional_string(
            output, authority.default_semantic);
        output << ", "
               << "\"enum_rows_name_wire_hidden_feature_alias_of\": [";
        for (std::size_t value_index = 0;
             value_index < authority.enum_values.size();
             ++value_index) {
            const collected_guc_authority_enum_value& value =
                authority.enum_values[value_index];
            if (value_index != 0) output << ", ";
            output << "["
                   << guc_render_detail::json_string(value.name)
                   << ", "
                   << guc_render_detail::json_string(
                        value.wire_expression)
                   << ", " << (value.hidden ? "1" : "0")
                   << ", "
                   << guc_render_detail::json_string(
                        value.feature_predicate)
                   << ", ";
            guc_render_detail::render_json_optional_string(
                output, value.alias_of);
            output << "]";
        }
        output << "], \"enum_omissions_name_wire\": [";
        for (std::size_t omission_index = 0;
             omission_index < authority.omissions.size();
             ++omission_index) {
            const collected_guc_authority_omission& omission =
                authority.omissions[omission_index];
            if (omission_index != 0) output << ", ";
            output << "["
                   << guc_render_detail::json_string(omission.name)
                   << ", "
                   << guc_render_detail::json_string(
                        omission.wire_expression)
                   << "]";
        }
        output << "]}, \"generated_live_authority\": ";
        guc_render_detail::render_generated_live_authority(
            output, guc);
        output << ", \"candidate\": {\"owner\": "
               << guc_render_detail::json_string(authority.group)
               << ", \"cpp_storage\": "
               << guc_render_detail::json_string(
                    guc.cpp_qualified_name)
               << ", \"storage_kind\": "
               << guc_render_detail::json_string(
                    authority.storage_kind)
               << ", \"source_module\": "
               << guc_render_detail::json_string(guc.source_module)
               << ", \"source_sha256\": "
               << guc_render_detail::json_string(guc.source_sha256)
               << "}}"
               << (index + 1 == inventory.gucs.size() ? "\n" : ",\n");
    }
    output << "    ]\n"
           << "  }\n"
           << "}\n";
    return std::move(output).str();
}

}  // namespace bioseq::codegen::sql
