module;

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.codegen.sql.collector:gucs;

import :model;
import :options;
import bioseq.codegen.structural_analysis;
import bioseq.codegen.support;

export namespace bioseq::codegen::sql {

enum class guc_profile : std::uint8_t {
    production,
    test_indexes,
};

enum class guc_kind : std::uint8_t {
    boolean,
    integer,
    real,
    string,
    enumeration,
};

struct collected_guc_enum_value {
    std::string name;
    std::string cpp_name;
    std::string wire_expression;
    std::string active_condition;
    std::optional<std::string> alias_of;
    bool hidden{};
    bool compiled{};
};

struct collected_guc_live_authority {
    std::string vartype;
    std::string context;
    std::optional<std::string> unit;
    std::string boot_value;
    std::string reset_value;
    std::string current_value;
    std::optional<std::string> minimum;
    std::optional<std::string> maximum;
    std::vector<std::string> visible_enum_values;
    std::string short_description;
    std::optional<std::string> extra_description;
    std::int64_t raw_flags{};
    bool visible_in_show_all{true};
    bool included_in_sample{true};
    std::array<bool, 3> hook_presence{};
};

struct collected_guc {
    std::size_t registration_ordinal{};
    std::string name;
    std::string owner_key;
    std::string member;
    guc_kind kind{guc_kind::boolean};
    std::string cpp_qualified_name;
    std::string source_module;
    std::filesystem::path source_path;
    std::string source_sha256;
    std::string active_condition;
    std::string default_expression;
    std::optional<std::string> minimum_expression;
    std::optional<std::string> maximum_expression;
    std::string context;
    std::vector<std::string> flags;
    std::optional<std::string> unit;
    std::string short_description;
    std::string long_description;
    std::string enum_cpp_name;
    std::string enum_default_name;
    std::vector<collected_guc_enum_value> enum_values;
    std::string storage_kind;
    std::string feature_predicate;
    std::array<std::optional<std::string>, 3> hooks;
    bool reset_plan_cache{};
    std::optional<std::string> authored_assign_hook;
    collected_guc_live_authority generated_live_authority;
};

struct guc_inventory {
    guc_profile profile{guc_profile::production};
    std::vector<collected_guc> gucs;
};

[[nodiscard]] inline constexpr std::string_view guc_profile_name(
    guc_profile profile) noexcept {
    switch (profile) {
        case guc_profile::production: return "production";
        case guc_profile::test_indexes: return "test_indexes";
    }
    return "unknown";
}

[[nodiscard]] inline constexpr std::string_view guc_kind_name(
    guc_kind kind) noexcept {
    switch (kind) {
        case guc_kind::boolean: return "bool";
        case guc_kind::integer: return "integer";
        case guc_kind::real: return "real";
        case guc_kind::string: return "string";
        case guc_kind::enumeration: return "enum";
    }
    return "unknown";
}

namespace guc_detail {

[[nodiscard]] inline std::string lower_ascii(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char item : value) {
        result.push_back(
            item >= 'A' && item <= 'Z'
            ? static_cast<char>(item - 'A' + 'a')
            : item);
    }
    return result;
}

[[nodiscard]] inline std::string compact_annotation_expression(
    std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char item : value) {
        if (item == ' ' || item == '\t' || item == '\n'
            || item == '\r') {
            continue;
        }
        result.push_back(
            item >= 'A' && item <= 'Z'
            ? static_cast<char>(item - 'A' + 'a')
            : item);
    }
    return result;
}

[[nodiscard]] inline std::string natural_guc_name(
    std::string_view cpp_qualified_name) {
    std::string path{cpp_qualified_name};
    std::size_t offset = 0;
    while ((offset = path.find("::", offset)) != std::string::npos) {
        path.replace(offset, 2, ".");
        ++offset;
    }
    const std::string marker = ".settings.";
    const std::size_t settings = path.find(marker);
    if (settings == std::string::npos
        || path.find(marker, settings + marker.size())
            != std::string::npos) {
        throw std::runtime_error(
            "annotated GUC '" + std::string{cpp_qualified_name}
            + "' must have exactly one natural settings owner");
    }
    path.erase(settings, marker.size() - 1);
    if (!path.starts_with("bioseq.") || path.ends_with('.')) {
        throw std::runtime_error(
            "annotated GUC '" + std::string{cpp_qualified_name}
            + "' does not produce a Bioseq GUC name");
    }
    std::size_t component_begin = 0;
    while (component_begin < path.size()) {
        const std::size_t separator = path.find('.', component_begin);
        const std::string_view component{path.data() + component_begin,
            (separator == std::string::npos ? path.size() : separator)
                - component_begin};
        if (component.empty()
            || !((component.front() >= 'a'
                    && component.front() <= 'z')
                 || component.front() == '_')
            || !std::ranges::all_of(
                component.substr(1),
                [](const char value) {
                    return (value >= 'a' && value <= 'z')
                        || (value >= '0' && value <= '9')
                        || value == '_';
                })) {
            throw std::runtime_error(
                "annotated GUC '" + std::string{cpp_qualified_name}
                + "' has a non-lower-snake natural name component");
        }
        if (separator == std::string::npos) break;
        component_begin = separator + 1;
    }
    return path;
}

[[nodiscard]] inline const structural_analysis::translation_unit&
registered_guc_source(
    const collected_program& program,
    const collected_entity& entity) {
    const auto source = std::ranges::find(
        program.translation_units,
        entity.source_path,
        &structural_analysis::translation_unit::source_path);
    if (source == program.translation_units.end()) {
        throw std::runtime_error(
            "annotated GUC '" + entity.cpp_qualified_name
            + "' does not belong to a registered SQL source");
    }
    if (source->module_name != entity.source_module
        || source->source_sha256 != entity.source_sha256) {
        throw std::runtime_error(
            "annotated GUC '" + entity.cpp_qualified_name
            + "' disagrees with its registered-source provenance");
    }
    const auto declaration = std::ranges::find_if(
        source->declarations,
        [&entity](
            const structural_analysis::declaration_record& candidate) {
            return candidate.kind == entity.declaration_kind
                && candidate.qualified_name
                    == entity.cpp_qualified_name;
        });
    if (declaration == source->declarations.end()) {
        throw std::runtime_error(
            "annotated GUC '" + entity.cpp_qualified_name
            + "' is absent from its registered SQL source");
    }
    const std::string source_path =
        source->source_path.generic_string();
    if (source_path.contains("/backend/pg/")
        && !source_path.contains("/backend/pg/configuration/")) {
        throw std::runtime_error(
            "annotated GUC '" + entity.cpp_qualified_name
            + "' is owned by an unsupported PostgreSQL adapter path");
    }
    return *source;
}

[[nodiscard]] inline std::string guc_owner_key(
    const structural_analysis::translation_unit& source) {
    static constexpr std::string_view prefix =
        "bioseq.postgres.configuration.";
    if (!source.module_name.starts_with(prefix)) {
        throw std::runtime_error(
            source.source_path.string()
            + ": registered GUC source module must begin with '"
            + std::string{prefix} + "'");
    }
    const std::string_view suffix =
        std::string_view{source.module_name}.substr(prefix.size());
    if (suffix == "core") return "bioseq";
    if (suffix == "aligner" || suffix == "scheduler"
        || suffix == "storage") {
        return std::string{suffix};
    }
    throw std::runtime_error(
        source.source_path.string()
        + ": registered GUC source has unsupported configuration owner '"
        + std::string{suffix} + "'");
}

[[nodiscard]] inline std::string registration_member(
    std::string_view natural_name,
    std::string_view owner_key) {
    const std::string prefix = owner_key == "bioseq"
        ? "bioseq."
        : "bioseq." + std::string{owner_key} + ".";
    if (!natural_name.starts_with(prefix)
        || natural_name.size() == prefix.size()) {
        throw std::runtime_error(
            "natural GUC name '" + std::string{natural_name}
            + "' does not belong to registered owner '"
            + std::string{owner_key} + "'");
    }
    std::string result{natural_name.substr(prefix.size())};
    std::ranges::replace(result, '.', '_');
    return result;
}

[[nodiscard]] inline std::string compact_cpp_type(
    std::string_view spelling) {
    std::string compact;
    compact.reserve(spelling.size());
    for (const char value : spelling) {
        if (value == ' ' || value == '\t' || value == '\n'
            || value == '\r' || value == '_') {
            continue;
        }
        compact.push_back(
            value >= 'A' && value <= 'Z'
            ? static_cast<char>(value - 'A' + 'a')
            : value);
    }
    return compact;
}

[[nodiscard]] inline std::string storage_kind_from_entity(
    const collected_entity& entity) {
    const std::string compact = compact_cpp_type(
        entity.cpp_return_type);
    if (compact == "bool") return "bool";
    if (compact == "int" || compact == "int32t"
        || compact == "std::int32t") {
        return "int32";
    }
    if (compact == "double") return "double";
    if (compact.contains("postgresenumstorage<")) {
        return "int32_enum";
    }
    if (compact.contains("char*") || compact.contains("string")) {
        return "string";
    }
    throw std::runtime_error(
        "unsupported C++ storage ABI for GUC '" + entity.name
        + "': " + entity.cpp_return_type);
}

[[nodiscard]] inline guc_kind kind_from_entity(
    const collected_entity& entity) {
    const std::string compact = compact_cpp_type(
        entity.cpp_return_type);
    if (compact.contains("postgresenumstorage<")) {
        return guc_kind::enumeration;
    }
    if (compact == "bool") return guc_kind::boolean;
    if (compact == "int" || compact == "int32t") {
        return guc_kind::integer;
    }
    if (compact == "double") return guc_kind::real;
    if (compact.contains("char*") || compact.contains("string")) {
        return guc_kind::string;
    }
    throw std::runtime_error(
        "unsupported C++ storage type for GUC '" + entity.name
        + "': " + entity.cpp_return_type);
}

[[nodiscard]] inline std::string option_leaf(
    const collected_entity& entity,
    std::string_view name,
    std::string_view fallback) {
    const option_value* option = find_option(entity, name);
    if (!option) return std::string{fallback};
    const std::string& expression = option->expression;
    const std::size_t separator = expression.rfind("::");
    return separator == std::string::npos
        ? expression
        : expression.substr(separator + 2);
}

[[nodiscard]] inline std::vector<std::string> guc_flags(
    const collected_entity& entity) {
    const option_value* option = find_option(entity, "flags");
    if (!option) return {};
    static constexpr std::string_view names[] = {
        "no_show_all",
        "not_in_sample",
        "unit_kb",
        "unit_mb",
        "unit_blocks",
        "unit_ms",
        "unit_s",
        "unit_min",
    };
    std::vector<std::string> result;
    for (const std::string_view name : names) {
        if (option->expression.contains(name)
            && !name.starts_with("unit_")) {
            result.emplace_back(name);
        }
    }
    return result;
}

[[nodiscard]] inline std::optional<std::string> guc_unit(
    const collected_entity& entity) {
    const option_value* option = find_option(entity, "flags");
    if (!option) return std::nullopt;
    static constexpr std::pair<std::string_view, std::string_view>
        units[] = {
            {"unit_kb", "kb"},
            {"unit_mb", "mb"},
            {"unit_blocks", "blocks"},
            {"unit_ms", "ms"},
            {"unit_s", "s"},
            {"unit_min", "min"},
        };
    for (const auto& [token, spelling] : units) {
        if (option->expression.contains(token)) {
            return std::string{spelling};
        }
    }
    return std::nullopt;
}

[[nodiscard]] inline std::string context_name(
        const collected_entity& entity) {
    const std::string value = option_leaf(entity, "context", "user");
    if (value == "user") return "user_set";
    if (value == "superuser") return "superuser_set";
    return value;
}

[[nodiscard]] inline bool resets_plan_cache(
        const collected_entity& entity) {
    const std::string value =
        option_leaf(entity, "plan_cache", "unchanged");
    if (value == "unchanged") return false;
    if (value == "reset") return true;
    throw std::runtime_error(
        "GUC '" + entity.cpp_qualified_name
        + "' plan_cache policy must be unchanged or reset");
}

[[nodiscard]] inline std::optional<std::string> range_option(
    const collected_entity& entity,
    std::string_view name) {
    if (const option_value* option = find_option(entity, name)) {
        return option->expression;
    }
    return std::nullopt;
}

[[nodiscard]] inline std::string enum_type_leaf(
    std::string_view cpp_type) {
    const std::size_t marker = cpp_type.find("postgres_enum_storage");
    if (marker == std::string_view::npos) return {};
    const std::size_t open = cpp_type.find('<', marker);
    if (open == std::string_view::npos) return {};
    int depth = 0;
    for (std::size_t index = open; index < cpp_type.size(); ++index) {
        if (cpp_type[index] == '<') {
            ++depth;
        } else if (cpp_type[index] == '>') {
            --depth;
            if (depth == 0) {
                std::string value{cpp_type.substr(
                    open + 1, index - open - 1)};
                const std::size_t separator = value.rfind("::");
                return separator == std::string::npos
                    ? value
                    : value.substr(separator + 2);
            }
        }
    }
    return {};
}


[[nodiscard]] inline bool condition_enabled_for_profile(
    std::string_view condition,
    guc_profile profile) {
    if (condition.empty() || condition == "true") return true;
    if (condition.contains("BIOSEQ_TEST_INDEXES")) {
        return profile == guc_profile::test_indexes;
    }
    return true;
}

[[nodiscard]] inline std::string canonical_feature_predicate(
    std::string_view condition) {
    const auto trim = [](std::string_view value) noexcept {
        while (!value.empty()
               && (value.front() == ' ' || value.front() == '\t'
                   || value.front() == '\n'
                   || value.front() == '\r')) {
            value.remove_prefix(1);
        }
        while (!value.empty()
               && (value.back() == ' ' || value.back() == '\t'
                   || value.back() == '\n'
                   || value.back() == '\r')) {
            value.remove_suffix(1);
        }
        return value;
    };
    if (condition.empty()) return "true";
    condition = trim(condition);
    if (condition.empty()) {
        throw std::runtime_error(
            "GUC feature predicate is whitespace-only");
    }
    bool removed_wrapper = false;
    while (condition.size() >= 2
           && condition.front() == '('
           && condition.back() == ')') {
        int depth = 0;
        bool whole_expression_wrapped = true;
        for (std::size_t index = 0; index < condition.size(); ++index) {
            if (condition[index] == '(') {
                ++depth;
            } else if (condition[index] == ')') {
                --depth;
                if (depth < 0
                    || (depth == 0
                        && index + 1 != condition.size())) {
                    whole_expression_wrapped = false;
                    break;
                }
            }
        }
        if (!whole_expression_wrapped || depth != 0) break;
        condition = trim(condition.substr(1, condition.size() - 2));
        removed_wrapper = true;
    }
    if (condition.empty() && removed_wrapper) {
        throw std::runtime_error(
            "GUC feature predicate has an empty parenthesized "
            "expression");
    }
    return std::string{condition};
}

[[nodiscard]] inline std::string condition_at(
    std::string_view source,
    std::size_t offset) {
    const auto trim = [](std::string_view value) noexcept {
        while (!value.empty()
               && (value.front() == ' ' || value.front() == '\t'
                   || value.front() == '\n'
                   || value.front() == '\r')) {
            value.remove_prefix(1);
        }
        while (!value.empty()
               && (value.back() == ' ' || value.back() == '\t'
                   || value.back() == '\n'
                   || value.back() == '\r')) {
            value.remove_suffix(1);
        }
        return value;
    };
    std::vector<std::string> stack;
    std::size_t line_begin = 0;
    while (line_begin < offset) {
        const std::size_t newline = source.find('\n', line_begin);
        const std::size_t line_end = std::min(
            newline == std::string_view::npos ? source.size() : newline,
            offset);
        const std::string_view line = trim(
            source.substr(line_begin, line_end - line_begin));
        if (line.starts_with("#if ")) {
            stack.emplace_back(trim(line.substr(4)));
        } else if (line.starts_with("#ifdef ")) {
            stack.emplace_back(
                "defined(" + std::string{trim(line.substr(7))} + ")");
        } else if (line.starts_with("#ifndef ")) {
            stack.emplace_back(
                "!defined(" + std::string{trim(line.substr(8))} + ")");
        } else if (line.starts_with("#elif ") && !stack.empty()) {
            stack.back() = std::string{trim(line.substr(6))};
        } else if (line == "#else" && !stack.empty()) {
            stack.back() = "!(" + stack.back() + ")";
        } else if (line == "#endif" && !stack.empty()) {
            stack.pop_back();
        }
        if (newline == std::string_view::npos) break;
        line_begin = newline + 1;
    }
    std::string result;
    for (const std::string& condition : stack) {
        if (!result.empty()) result += " && ";
        result += "(" + condition + ")";
    }
    return result;
}

[[nodiscard]] inline bool identifier_token(
    std::string_view token) noexcept {
    if (token.empty()
        || !((token.front() >= 'a' && token.front() <= 'z')
             || (token.front() >= 'A' && token.front() <= 'Z')
             || token.front() == '_')) {
        return false;
    }
    return std::ranges::all_of(
        token.substr(1),
        [](char value) {
            return (value >= 'a' && value <= 'z')
                || (value >= 'A' && value <= 'Z')
                || (value >= '0' && value <= '9') || value == '_';
        });
}

struct parsed_enumerator {
    std::string name;
    std::string qualified_name;
    std::string initializer_text;
    std::string active_condition;
};

struct parsed_enum {
    std::string qualified_name;
    std::map<std::string, parsed_enumerator, std::less<>> values;
    std::vector<std::string> declaration_order;
};

[[nodiscard]] inline parsed_enum parse_enum(
    const collected_program& program,
    const collected_entity& entity,
    std::string_view leaf) {
    const auto unit = std::ranges::find(
        program.translation_units,
        entity.source_path,
        &structural_analysis::translation_unit::source_path);
    if (unit == program.translation_units.end()) {
        throw std::runtime_error(
            "cannot find translation unit for enum GUC '" + entity.name
            + "'");
    }
    const std::string source = read_artifact_bytes(unit->source_path);
    const auto& tokens = unit->tokens;
    std::size_t enum_name = tokens.size();
    std::size_t open = tokens.size();
    std::size_t close = tokens.size();
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        if (tokens[index].spelling != "enum") continue;
        std::size_t cursor = index + 1;
        if (cursor < tokens.size()
            && (tokens[cursor].spelling == "class"
                || tokens[cursor].spelling == "struct")) {
            ++cursor;
        }
        if (cursor >= tokens.size()
            || tokens[cursor].spelling != leaf) {
            continue;
        }
        enum_name = cursor;
        while (cursor < tokens.size()
               && tokens[cursor].spelling != "{"
               && tokens[cursor].spelling != ";") {
            ++cursor;
        }
        if (cursor >= tokens.size()
            || tokens[cursor].spelling != "{") {
            continue;
        }
        open = cursor;
        int depth = 1;
        while (++cursor < tokens.size()) {
            if (tokens[cursor].spelling == "{") {
                ++depth;
            } else if (tokens[cursor].spelling == "}") {
                --depth;
                if (depth == 0) {
                    close = cursor;
                    break;
                }
            }
        }
        if (close != tokens.size()) break;
    }
    if (enum_name == tokens.size() || close == tokens.size()) {
        throw std::runtime_error(
            "cannot resolve enum type '" + std::string{leaf}
            + "' used by GUC '" + entity.name + "'");
    }

    const std::string settings_marker = "::settings::";
    const std::size_t settings =
        entity.cpp_qualified_name.find(settings_marker);
    if (settings == std::string::npos) {
        throw std::runtime_error(
            "enum GUC '" + entity.name
            + "' is not nested in a natural settings owner");
    }
    parsed_enum result{
        .qualified_name =
            entity.cpp_qualified_name.substr(0, settings)
            + "::" + std::string{leaf},
    };
    const auto parse_segment = [
        &result,
        &source,
        &tokens](std::size_t begin, std::size_t end) {
        while (begin < end
               && (tokens[begin].spelling == ","
                   || tokens[begin].spelling == ";")) {
            ++begin;
        }
        std::size_t equals = end;
        std::size_t name_token = end;
        int parentheses = 0;
        int braces = 0;
        int brackets = 0;
        for (std::size_t index = begin; index < end; ++index) {
            const std::string& spelling = tokens[index].spelling;
            if (spelling == "(") ++parentheses;
            else if (spelling == ")") --parentheses;
            else if (spelling == "{") ++braces;
            else if (spelling == "}") --braces;
            else if (spelling == "[") ++brackets;
            else if (spelling == "]") --brackets;
            else if (spelling == "=" && parentheses == 0
                     && braces == 0 && brackets == 0) {
                equals = index;
                break;
            } else if (parentheses == 0 && braces == 0
                       && brackets == 0
                       && identifier_token(spelling)) {
                name_token = index;
            }
        }
        if (name_token == end) return;
        const std::string name = tokens[name_token].spelling;
        std::string initializer;
        if (equals != end && equals + 1 < end) {
            const std::size_t byte_begin =
                tokens[equals + 1].range.begin.byte_offset;
            const std::size_t byte_end =
                tokens[end - 1].range.end.byte_offset;
            initializer = source.substr(
                byte_begin, byte_end - byte_begin);
        }
        const std::size_t offset =
            tokens[name_token].range.begin.byte_offset;
        const auto [value, inserted] = result.values.emplace(
            name,
            parsed_enumerator{
                .name = name,
                .qualified_name = result.qualified_name + "::" + name,
                .initializer_text = std::move(initializer),
                .active_condition = condition_at(source, offset),
            });
        static_cast<void>(value);
        if (!inserted) {
            throw std::runtime_error(
                "enum type '" + result.qualified_name
                + "' duplicates value '" + name + "'");
        }
        result.declaration_order.push_back(name);
    };

    std::size_t segment_begin = open + 1;
    int parentheses = 0;
    int braces = 0;
    int brackets = 0;
    for (std::size_t index = segment_begin; index < close; ++index) {
        const std::string& spelling = tokens[index].spelling;
        if (spelling == "(") ++parentheses;
        else if (spelling == ")") --parentheses;
        else if (spelling == "{") ++braces;
        else if (spelling == "}") --braces;
        else if (spelling == "[") ++brackets;
        else if (spelling == "]") --brackets;
        else if (spelling == "," && parentheses == 0
                 && braces == 0 && brackets == 0) {
            parse_segment(segment_begin, index);
            segment_begin = index + 1;
        }
    }
    parse_segment(segment_begin, close);
    if (result.values.empty()) {
        throw std::runtime_error(
            "enum type '" + result.qualified_name
            + "' has no values");
    }
    return result;
}

struct value_exception {
    bool omit{};
    std::optional<std::string> alias;
    bool alias_hidden{};
};

[[nodiscard]] inline std::map<
    std::string,
    value_exception,
    std::less<>>
guc_value_exceptions(
    const collected_program& program,
    std::string_view enum_cpp_name) {
    std::map<std::string, value_exception, std::less<>> result;
    for (const collected_entity& entity : program.entities) {
        if (entity.kind != entity_kind::guc_value
            || entity.enclosing_cpp_name != enum_cpp_name) {
            continue;
        }
        value_exception exception;
        if (const option_value* omit = find_option(entity, "omit")) {
            exception.omit =
                compact_annotation_expression(omit->expression)
                    == "true";
        }
        if (const option_value* aliases = find_option(entity, "aliases")) {
            const std::string& expression = aliases->expression;
            const std::size_t spelling = expression.find(".spelling=");
            if (spelling != std::string::npos) {
                const std::size_t quote = expression.find('"', spelling);
                const std::size_t close = quote == std::string::npos
                    ? std::string::npos
                    : expression.find('"', quote + 1);
                if (close != std::string::npos) {
                    exception.alias = expression.substr(
                        quote + 1, close - quote - 1);
                }
            }
            exception.alias_hidden =
                compact_annotation_expression(expression).contains(
                    ".hidden=true");
        }
        result.emplace(entity.name, std::move(exception));
    }
    return result;
}

inline void validate_documentation(const collected_entity& entity) {
    if (!entity.documentation.has_explicit_brief
        || entity.documentation.brief.empty()) {
        throw std::runtime_error(
            "GUC '" + natural_guc_name(entity.cpp_qualified_name)
            + "' requires an explicit Doxygen @brief");
    }
    const std::string documentation = lower_ascii(
        entity.documentation.brief + " "
        + entity.documentation.details);
    static constexpr std::string_view forbidden[] = {
        "doc0",
        "prototype",
        " phase ",
        " stage ",
    };
    for (const std::string_view token : forbidden) {
        if (documentation.contains(token)) {
            throw std::runtime_error(
                "GUC '" + natural_guc_name(entity.cpp_qualified_name)
                + "' documentation describes project history instead of "
                  "current behavior");
        }
    }
}

[[nodiscard]] inline bool compiled_enumerator(
    const collected_program& program,
    const parsed_enumerator& value) {
    if (program.compiler_profile.empty()) {
        return canonical_feature_predicate(value.active_condition)
            == "true";
    }
    return std::ranges::any_of(
        program.translation_units,
        [&value](const structural_analysis::translation_unit& unit) {
            return std::ranges::any_of(
                unit.declarations,
                [&value](
                    const structural_analysis::declaration_record&
                        declaration) {
                    return declaration.kind
                            == structural_analysis::declaration_kind::
                                enumerator
                        && declaration.qualified_name
                            == value.qualified_name;
                });
        });
}

[[nodiscard]] inline std::vector<collected_guc_enum_value>
collect_enum_values(
    const collected_program& program,
    const collected_entity& entity,
    const parsed_enum& parsed) {
    const auto exceptions =
        guc_value_exceptions(program, parsed.qualified_name);
    for (const auto& [name, exception] : exceptions) {
        static_cast<void>(exception);
        if (!parsed.values.contains(name)) {
            throw std::runtime_error(
                "enum GUC '" + natural_guc_name(
                    entity.cpp_qualified_name)
                + "' has a guc_value annotation for unknown value '"
                + name + "'");
        }
    }

    std::set<std::string, std::less<>> emitted_names;
    std::vector<collected_guc_enum_value> result;
    result.reserve(parsed.declaration_order.size());
    for (const std::string& name : parsed.declaration_order) {
        const parsed_enumerator& declaration =
            parsed.values.at(name);
        const auto exception = exceptions.find(name);
        const bool omitted =
            exception != exceptions.end() && exception->second.omit;
        if (omitted && exception->second.alias) {
            throw std::runtime_error(
                "enum GUC '" + natural_guc_name(
                    entity.cpp_qualified_name)
                + "' value '" + name
                + "' cannot be both omitted and aliased");
        }
        if (omitted) continue;
        if (!emitted_names.emplace(name).second) {
            throw std::runtime_error(
                "enum GUC '" + natural_guc_name(
                    entity.cpp_qualified_name)
                + "' duplicates emitted value '" + name + "'");
        }
        result.push_back(collected_guc_enum_value{
            .name = name,
            .cpp_name = declaration.qualified_name,
            .wire_expression = declaration.initializer_text,
            .active_condition = declaration.active_condition,
            .hidden = false,
            .compiled = compiled_enumerator(program, declaration),
        });
        if (exception == exceptions.end()
            || !exception->second.alias) {
            continue;
        }
        const std::string& alias = *exception->second.alias;
        if (alias.empty()
            || !((alias.front() >= 'a' && alias.front() <= 'z')
                 || alias.front() == '_')
            || !std::ranges::all_of(
                std::string_view{alias}.substr(1),
                [](const char value) {
                    return (value >= 'a' && value <= 'z')
                        || (value >= '0' && value <= '9')
                        || value == '_';
                })
            || !emitted_names.emplace(alias).second) {
            throw std::runtime_error(
                "enum GUC '" + natural_guc_name(
                    entity.cpp_qualified_name)
                + "' has invalid or duplicate alias '" + alias + "'");
        }
        result.push_back(collected_guc_enum_value{
            .name = alias,
            .cpp_name = declaration.qualified_name,
            .wire_expression = declaration.initializer_text,
            .active_condition = declaration.active_condition,
            .alias_of = name,
            .hidden = exception->second.alias_hidden,
            .compiled = compiled_enumerator(program, declaration),
        });
    }
    if (result.empty()) {
        throw std::runtime_error(
            "enum GUC '" + natural_guc_name(
                entity.cpp_qualified_name)
            + "' has no registered values");
    }
    return result;
}

[[nodiscard]] inline std::string_view trim_enum_expression(
        std::string_view value) noexcept {
    while (!value.empty()
           && (value.front() == ' ' || value.front() == '\t'
               || value.front() == '\n' || value.front() == '\r')) {
        value.remove_prefix(1);
    }
    while (!value.empty()
           && (value.back() == ' ' || value.back() == '\t'
               || value.back() == '\n' || value.back() == '\r')) {
        value.remove_suffix(1);
    }
    return value;
}

[[nodiscard]] inline std::string enum_initializer_argument(
    std::string_view initializer) {
    initializer = trim_enum_expression(initializer);
    if (initializer.ends_with('}')) {
        const std::size_t open = initializer.rfind('{');
        if (open == std::string_view::npos) {
            throw std::runtime_error(
                "malformed enum GUC storage initializer");
        }
        initializer = trim_enum_expression(initializer.substr(
            open + 1, initializer.size() - open - 2));
    }
    if (initializer.empty()) {
        throw std::runtime_error(
            "enum GUC storage initializer has no default expression");
    }
    return std::string{initializer};
}

[[nodiscard]] inline std::string qualified_identifier_leaf(
    std::string_view expression) {
    expression = trim_enum_expression(expression);
    if (expression.starts_with("::")) expression.remove_prefix(2);
    std::size_t component_begin = 0;
    while (component_begin < expression.size()) {
        const std::size_t separator =
            expression.find("::", component_begin);
        const std::string_view component = expression.substr(
            component_begin,
            (separator == std::string_view::npos
                 ? expression.size()
                 : separator)
                - component_begin);
        if (component.empty()
            || !((component.front() >= 'a'
                    && component.front() <= 'z')
                 || (component.front() >= 'A'
                     && component.front() <= 'Z')
                 || component.front() == '_')
            || !std::ranges::all_of(
                component.substr(1),
                [](const char value) {
                    return (value >= 'a' && value <= 'z')
                        || (value >= 'A' && value <= 'Z')
                        || (value >= '0' && value <= '9')
                        || value == '_';
                })) {
            return {};
        }
        if (separator == std::string_view::npos) {
            return std::string{component};
        }
        component_begin = separator + 2;
    }
    return {};
}

struct resolved_enum_default {
    std::string cpp_expression;
    std::string value_name;
};

[[nodiscard]] inline resolved_enum_default resolve_enum_default(
    const collected_program& program,
    const collected_entity& entity,
    const parsed_enum& parsed,
    const std::vector<collected_guc_enum_value>& values) {
    const std::string authored =
        enum_initializer_argument(entity.initializer_expression);
    const std::string leaf = qualified_identifier_leaf(authored);
    if (leaf.empty()) {
        throw std::runtime_error(
            "enum GUC '" + natural_guc_name(
                entity.cpp_qualified_name)
            + "' has a non-identifier default expression '"
            + authored + "'");
    }

    std::string cpp_expression;
    std::string value_name;
    if (const auto direct = parsed.values.find(leaf);
        direct != parsed.values.end()) {
        cpp_expression = direct->second.qualified_name;
        value_name = leaf;
    } else {
        const structural_analysis::translation_unit& source =
            registered_guc_source(program, entity);
        std::vector<
            const structural_analysis::declaration_record*>
            constants;
        for (const structural_analysis::declaration_record& candidate :
             source.declarations) {
            if (candidate.kind
                    != structural_analysis::declaration_kind::variable
                || candidate.name != leaf
                || candidate.initializer_text.empty()
                || !candidate.signature_text.contains("constexpr")) {
                continue;
            }
            std::string requested = authored;
            if (requested.starts_with("::")) requested.erase(0, 2);
            if (authored.contains("::")
                && candidate.qualified_name != requested) {
                continue;
            }
            constants.push_back(&candidate);
        }
        if (constants.empty()) {
            throw std::runtime_error(
                "enum GUC '" + natural_guc_name(
                    entity.cpp_qualified_name)
                + "' cannot resolve constexpr default '" + authored
                + "' in its registered source");
        }
        cpp_expression = constants.front()->qualified_name;
        std::set<std::string, std::less<>> resolved_values;
        for (const auto* constant : constants) {
            if (constant->qualified_name != cpp_expression) {
                throw std::runtime_error(
                    "enum GUC '" + natural_guc_name(
                        entity.cpp_qualified_name)
                    + "' has ambiguous constexpr default '" + authored
                    + "'");
            }
            const std::string resolved_name =
                qualified_identifier_leaf(
                    constant->initializer_text);
            if (parsed.values.contains(resolved_name)) {
                resolved_values.emplace(resolved_name);
            }
        }
        if (resolved_values.size() == 1) {
            value_name = *resolved_values.begin();
        } else if (program.compiler_profile.empty()) {
            for (const collected_guc_enum_value& value : values) {
                if (value.compiled && !value.hidden
                    && !value.alias_of) {
                    value_name = value.name;
                }
            }
        }
        if (value_name.empty()) {
            throw std::runtime_error(
                "enum GUC '" + natural_guc_name(
                    entity.cpp_qualified_name)
                + "' cannot resolve constexpr default '" + authored
                + "' to one enum value");
        }
    }
    const auto registered = std::ranges::find_if(
        values,
        [&value_name](const collected_guc_enum_value& value) {
            return value.name == value_name && value.compiled
                && !value.hidden && !value.alias_of;
        });
    if (registered == values.end()) {
        throw std::runtime_error(
            "enum GUC '" + natural_guc_name(
                entity.cpp_qualified_name)
            + "' default '" + value_name
            + "' is not a compiled registered value");
    }
    return {
        .cpp_expression = std::move(cpp_expression),
        .value_name = std::move(value_name),
    };
}
[[nodiscard]] inline std::string_view trim_expression(
        std::string_view value) noexcept {
    while (!value.empty()
           && (value.front() == ' ' || value.front() == '\t'
               || value.front() == '\n' || value.front() == '\r')) {
        value.remove_prefix(1);
    }
    while (!value.empty()
           && (value.back() == ' ' || value.back() == '\t'
               || value.back() == '\n' || value.back() == '\r')) {
        value.remove_suffix(1);
    }
    return value;
}

class integer_expression_parser final {
public:
    explicit integer_expression_parser(std::string_view input)
        : input_{input} {}

    [[nodiscard]] std::int64_t parse() {
        const std::int64_t result = parse_bitwise_or();
        skip_space();
        if (cursor_ != input_.size()) {
            fail("trailing token");
        }
        return result;
    }

private:
    [[noreturn]] void fail(std::string_view reason) const {
        throw std::runtime_error(
            "cannot resolve generated integer GUC expression '"
            + std::string{input_} + "' at byte "
            + std::to_string(cursor_) + ": " + std::string{reason});
    }

    void skip_space() noexcept {
        while (cursor_ < input_.size()
               && (input_[cursor_] == ' '
                   || input_[cursor_] == '\t'
                   || input_[cursor_] == '\n'
                   || input_[cursor_] == '\r')) {
            ++cursor_;
        }
    }

    [[nodiscard]] bool consume(std::string_view token) noexcept {
        skip_space();
        if (!input_.substr(cursor_).starts_with(token)) {
            return false;
        }
        cursor_ += token.size();
        return true;
    }

    [[nodiscard]] std::int64_t parse_primary() {
        skip_space();
        if (consume("(")) {
            const std::int64_t value = parse_bitwise_or();
            if (!consume(")")) fail("missing closing parenthesis");
            return value;
        }
        const std::size_t begin = cursor_;
        while (cursor_ < input_.size()
               && ((input_[cursor_] >= '0'
                    && input_[cursor_] <= '9')
                   || input_[cursor_] == '\'')) {
            ++cursor_;
        }
        if (begin == cursor_) fail("expected an integer literal");
        std::string digits;
        digits.reserve(cursor_ - begin);
        for (const char item : input_.substr(begin, cursor_ - begin)) {
            if (item != '\'') digits.push_back(item);
        }
        while (cursor_ < input_.size()
               && (input_[cursor_] == 'u'
                   || input_[cursor_] == 'U'
                   || input_[cursor_] == 'l'
                   || input_[cursor_] == 'L')) {
            ++cursor_;
        }
        std::int64_t value = 0;
        const auto parsed = std::from_chars(
            digits.data(), digits.data() + digits.size(), value);
        if (parsed.ec != std::errc{}
            || parsed.ptr != digits.data() + digits.size()) {
            fail("invalid or overflowing integer literal");
        }
        return value;
    }

    [[nodiscard]] std::int64_t parse_unary() {
        if (consume("+")) return parse_unary();
        if (consume("-")) return -parse_unary();
        if (consume("~")) return ~parse_unary();
        return parse_primary();
    }

    [[nodiscard]] std::int64_t parse_multiplicative() {
        std::int64_t result = parse_unary();
        while (true) {
            if (consume("*")) {
                result *= parse_unary();
            } else if (consume("/")) {
                const std::int64_t divisor = parse_unary();
                if (divisor == 0) fail("division by zero");
                result /= divisor;
            } else if (consume("%")) {
                const std::int64_t divisor = parse_unary();
                if (divisor == 0) fail("remainder by zero");
                result %= divisor;
            } else {
                return result;
            }
        }
    }

    [[nodiscard]] std::int64_t parse_additive() {
        std::int64_t result = parse_multiplicative();
        while (true) {
            if (consume("+")) result += parse_multiplicative();
            else if (consume("-")) result -= parse_multiplicative();
            else return result;
        }
    }

    [[nodiscard]] std::int64_t parse_shift() {
        std::int64_t result = parse_additive();
        while (true) {
            if (consume("<<")) {
                const std::int64_t amount = parse_additive();
                if (amount < 0 || amount >= 63) {
                    fail("invalid left-shift amount");
                }
                result <<= amount;
            } else if (consume(">>")) {
                const std::int64_t amount = parse_additive();
                if (amount < 0 || amount >= 63) {
                    fail("invalid right-shift amount");
                }
                result >>= amount;
            } else {
                return result;
            }
        }
    }

    [[nodiscard]] std::int64_t parse_bitwise_and() {
        std::int64_t result = parse_shift();
        while (consume("&")) result &= parse_shift();
        return result;
    }

    [[nodiscard]] std::int64_t parse_bitwise_xor() {
        std::int64_t result = parse_bitwise_and();
        while (consume("^")) result ^= parse_bitwise_and();
        return result;
    }

    [[nodiscard]] std::int64_t parse_bitwise_or() {
        std::int64_t result = parse_bitwise_xor();
        while (consume("|")) result |= parse_bitwise_xor();
        return result;
    }

    std::string_view input_;
    std::size_t cursor_ = 0;
};

[[nodiscard]] inline bool bare_numeric_identifier(
        std::string_view value) noexcept {
    value = trim_expression(value);
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

[[nodiscard]] inline std::optional<std::string>
source_only_integer_initializer(
    const structural_analysis::translation_unit& unit,
    std::string_view identifier) {
    const std::string source = read_artifact_bytes(unit.source_path);
    const auto& tokens = unit.tokens;
    std::optional<std::string> result;
    for (std::size_t index = 0; index + 2 < tokens.size(); ++index) {
        if (tokens[index].spelling != identifier
            || tokens[index + 1].spelling != "="
            || canonical_feature_predicate(condition_at(
                   source,
                   tokens[index].range.begin.byte_offset))
                != "true") {
            continue;
        }

        bool constexpr_declaration = false;
        for (std::size_t cursor = index; cursor > 0; --cursor) {
            const std::string& prior = tokens[cursor - 1].spelling;
            if (prior == ";" || prior == "{"
                || prior == "}") {
                break;
            }
            if (prior == "constexpr") {
                constexpr_declaration = true;
            }
        }
        if (!constexpr_declaration) continue;

        std::size_t terminator = tokens.size();
        int parentheses = 0;
        int brackets = 0;
        int braces = 0;
        for (std::size_t cursor = index + 2;
             cursor < tokens.size();
             ++cursor) {
            const std::string& spelling = tokens[cursor].spelling;
            if (spelling == "(") ++parentheses;
            else if (spelling == ")") --parentheses;
            else if (spelling == "[") ++brackets;
            else if (spelling == "]") --brackets;
            else if (spelling == "{") ++braces;
            else if (spelling == "}") --braces;
            else if (spelling == ";" && parentheses == 0
                     && brackets == 0 && braces == 0) {
                terminator = cursor;
                break;
            }
            if (parentheses < 0 || brackets < 0 || braces < 0) {
                throw std::runtime_error(
                    "malformed source-only integer GUC constant '"
                    + std::string{identifier} + "'");
            }
        }
        if (terminator == tokens.size()
            || terminator == index + 2) {
            throw std::runtime_error(
                "cannot resolve source-only integer GUC constant '"
                + std::string{identifier} + "'");
        }
        const std::size_t begin =
            tokens[index + 2].range.begin.byte_offset;
        const std::size_t end =
            tokens[terminator - 1].range.end.byte_offset;
        const std::string initializer =
            source.substr(begin, end - begin);
        if (result) {
            throw std::runtime_error(
                "ambiguous source-only integer GUC constant '"
                + std::string{identifier} + "'");
        }
        result = initializer;
    }
    return result;
}

[[nodiscard]] inline std::string resolve_integer_expression(
    const collected_program& program,
    const collected_entity& owner,
    std::string_view expression,
    std::size_t depth = 0) {
    if (depth > 16) {
        throw std::runtime_error(
            "generated integer GUC expression recursion is too deep");
    }
    expression = trim_expression(expression);
    if (expression == "INT_MAX") {
        return std::to_string(std::numeric_limits<int>::max());
    }
    if (expression == "INT_MIN") {
        return std::to_string(std::numeric_limits<int>::min());
    }
    if (bare_numeric_identifier(expression)) {
        const auto unit = std::ranges::find(
            program.translation_units,
            owner.source_path,
            &structural_analysis::translation_unit::source_path);
        if (unit == program.translation_units.end()) {
            throw std::runtime_error(
                "cannot resolve generated integer GUC owner source");
        }
        const auto declaration = std::ranges::find_if(
            unit->declarations,
            [expression](
                const structural_analysis::declaration_record& candidate) {
                return candidate.kind
                        == structural_analysis::declaration_kind::variable
                    && candidate.name == expression
                    && !candidate.initializer_text.empty();
            });
        if (declaration == unit->declarations.end()) {
            if (program.compiler_profile.empty()) {
                if (const std::optional<std::string> initializer =
                        source_only_integer_initializer(
                            *unit, expression)) {
                    return resolve_integer_expression(
                        program,
                        owner,
                        *initializer,
                        depth + 1);
                }
            }
            throw std::runtime_error(
                "cannot resolve generated integer GUC identifier '"
                + std::string{expression} + "' in "
                + owner.source_path.string());
        }
        return resolve_integer_expression(
            program,
            owner,
            declaration->initializer_text,
            depth + 1);
    }
    return std::to_string(
        integer_expression_parser{expression}.parse());
}

[[nodiscard]] inline std::string resolve_real_expression(
        std::string_view expression) {
    expression = trim_expression(expression);
    std::string normalized;
    normalized.reserve(expression.size());
    for (const char item : expression) {
        if (item != '\'') normalized.push_back(item);
    }
    char* end = nullptr;
    const double value = std::strtod(normalized.c_str(), &end);
    if (end == normalized.c_str() || *end != '\0') {
        throw std::runtime_error(
            "cannot resolve generated real GUC expression '"
            + normalized + "'");
    }
    std::array<char, 128> buffer{};
    const auto rendered = std::to_chars(
        buffer.data(),
        buffer.data() + buffer.size(),
        value,
        std::chars_format::general);
    if (rendered.ec != std::errc{}) {
        throw std::runtime_error(
            "cannot render generated real GUC expression '"
            + normalized + "'");
    }
    return std::string{
        buffer.data(),
        static_cast<std::size_t>(rendered.ptr - buffer.data())};
}

[[nodiscard]] inline std::int64_t raw_guc_flags(
        const collected_guc& guc) {
    std::int64_t result = 0;
    for (const std::string& flag : guc.flags) {
        if (flag == "no_show_all") result |= 0x000004;
        else if (flag == "not_in_sample") result |= 0x000080;
        else {
            throw std::runtime_error(
                "cannot resolve generated raw GUC flag '" + flag + "'");
        }
    }
    if (guc.unit) {
        if (*guc.unit == "kb") result |= 0x01000000;
        else if (*guc.unit == "blocks") result |= 0x02000000;
        else if (*guc.unit == "mb") result |= 0x04000000;
        else if (*guc.unit == "ms") result |= 0x10000000;
        else if (*guc.unit == "s") result |= 0x20000000;
        else if (*guc.unit == "min") result |= 0x30000000;
        else {
            throw std::runtime_error(
                "cannot resolve generated raw GUC unit '" + *guc.unit
                + "'");
        }
    }
    return result;
}

[[nodiscard]] inline collected_guc_live_authority
make_generated_live_authority(
    const collected_program& program,
    const collected_entity& entity,
    const collected_guc& guc) {
    collected_guc_live_authority result{
        .vartype = std::string{guc_kind_name(guc.kind)},
        .context =
            guc.context == "sighup"
            ? "sighup"
            : guc.context == "superuser_set"
            ? "superuser"
            : guc.context == "user_set" ? "user" : std::string{},
        .unit =
            guc.unit
            ? std::optional<std::string>{
                  *guc.unit == "mb"
                      ? std::string{"MB"}
                      : *guc.unit}
            : std::nullopt,
        .short_description = guc.short_description,
        .extra_description =
            guc.long_description.empty()
            ? std::nullopt
            : std::optional<std::string>{guc.long_description},
        .raw_flags = raw_guc_flags(guc),
        .visible_in_show_all =
            !std::ranges::contains(guc.flags, "no_show_all"),
        .included_in_sample =
            !std::ranges::contains(guc.flags, "not_in_sample"),
        .hook_presence = {
            guc.hooks[0].has_value(),
            guc.hooks[1].has_value(),
            guc.hooks[2].has_value(),
        },
    };
    if (result.context.empty()) {
        throw std::runtime_error(
            "cannot resolve generated live context for GUC '"
            + guc.name + "'");
    }
    switch (guc.kind) {
        case guc_kind::boolean:
            if (guc.default_expression == "true") {
                result.boot_value = "on";
            } else if (guc.default_expression == "false") {
                result.boot_value = "off";
            } else {
                throw std::runtime_error(
                    "cannot resolve generated boolean boot value for GUC '"
                    + guc.name + "'");
            }
            break;
        case guc_kind::integer:
            result.boot_value = resolve_integer_expression(
                program, entity, guc.default_expression);
            result.minimum = resolve_integer_expression(
                program, entity, *guc.minimum_expression);
            result.maximum = resolve_integer_expression(
                program, entity, *guc.maximum_expression);
            break;
        case guc_kind::real:
            result.boot_value =
                resolve_real_expression(guc.default_expression);
            result.minimum =
                resolve_real_expression(*guc.minimum_expression);
            result.maximum =
                resolve_real_expression(*guc.maximum_expression);
            break;
        case guc_kind::string: {
            std::string_view expression =
                trim_expression(guc.default_expression);
            if (expression == "nullptr" || expression == "NULL") {
                result.boot_value.clear();
            } else if (
                expression.size() >= 2
                && expression.front() == '"'
                && expression.back() == '"') {
                result.boot_value = std::string{
                    expression.substr(1, expression.size() - 2)};
            } else {
                throw std::runtime_error(
                    "cannot resolve generated string boot value for GUC '"
                    + guc.name + "'");
            }
            break;
        }
        case guc_kind::enumeration:
            for (const collected_guc_enum_value& value :
                 guc.enum_values) {
                if (value.compiled && !value.hidden) {
                    result.visible_enum_values.push_back(value.name);
                }
            }
            if (result.visible_enum_values.empty()
                || guc.enum_default_name.empty()) {
                throw std::runtime_error(
                    "cannot resolve generated enum values for GUC '"
                    + guc.name + "'");
            }
            result.boot_value = guc.enum_default_name;
            if (!std::ranges::contains(
                    result.visible_enum_values,
                    result.boot_value)) {
                throw std::runtime_error(
                    "generated enum boot value is not visible for GUC '"
                    + guc.name + "'");
            }
            break;
    }
    result.reset_value = result.boot_value;
    result.current_value = result.boot_value;
    return result;
}

}  // namespace guc_detail

[[nodiscard]] inline guc_inventory collect_guc_inventory(
    const collected_program& program,
    guc_profile profile) {
    const std::string profile_spelling{guc_profile_name(profile)};
    if (!program.compiler_profile.empty()
        && program.compiler_profile != profile_spelling) {
        throw std::runtime_error(
            "annotated GUC inventory profile '" + profile_spelling
            + "' disagrees with registered compiler profile '"
            + program.compiler_profile + "'");
    }

    std::vector<const collected_entity*> selected;
    std::set<std::string, std::less<>> natural_names;
    for (const collected_entity& entity : program.entities) {
        if (entity.kind != entity_kind::guc) continue;
        static_cast<void>(
            guc_detail::registered_guc_source(program, entity));
        const std::string name =
            guc_detail::natural_guc_name(entity.cpp_qualified_name);
        const std::string predicate =
            guc_detail::canonical_feature_predicate(
                entity.active_condition);
        if (predicate != "true"
            && !predicate.contains("BIOSEQ_TEST_INDEXES")) {
            throw std::runtime_error(
                "annotated GUC '" + name
                + "' has unsupported profile predicate '"
                + predicate + "'");
        }
        if (!guc_detail::condition_enabled_for_profile(
                entity.active_condition, profile)) {
            continue;
        }
        if (!natural_names.emplace(name).second) {
            throw std::runtime_error(
                "duplicate annotated GUC '" + name
                + "' in profile '" + profile_spelling + "'");
        }
        selected.push_back(&entity);
    }
    if (selected.empty()) {
        throw std::runtime_error(
            "annotated GUC profile '" + profile_spelling
            + "' has no registrations");
    }

    guc_inventory result{.profile = profile};
    result.gucs.reserve(selected.size());
    std::map<
        std::string,
        std::set<std::string, std::less<>>,
        std::less<>>
        members_by_owner;
    for (const collected_entity* selected_entity : selected) {
        const collected_entity& entity = *selected_entity;
        guc_detail::validate_documentation(entity);
        const structural_analysis::translation_unit& source =
            guc_detail::registered_guc_source(program, entity);
        const std::string name =
            guc_detail::natural_guc_name(entity.cpp_qualified_name);
        const std::string owner_key =
            guc_detail::guc_owner_key(source);
        const std::string member =
            guc_detail::registration_member(name, owner_key);
        if (!members_by_owner[owner_key].emplace(member).second) {
            throw std::runtime_error(
                "annotated GUC owner '" + owner_key
                + "' has duplicate generated member '" + member + "'");
        }

        const guc_kind kind =
            guc_detail::kind_from_entity(entity);
        std::optional<std::string> minimum =
            guc_detail::range_option(entity, "minimum");
        std::optional<std::string> maximum =
            guc_detail::range_option(entity, "maximum");
        if ((kind == guc_kind::integer
                || kind == guc_kind::real)
            && (!minimum || !maximum)) {
            throw std::runtime_error(
                "numeric GUC '" + name
                + "' requires explicit annotation bounds");
        }
        if (kind != guc_kind::integer
            && kind != guc_kind::real
            && (minimum || maximum)) {
            throw std::runtime_error(
                "non-numeric GUC '" + name
                + "' cannot declare numeric bounds");
        }

        collected_guc guc{
            .registration_ordinal = result.gucs.size(),
            .name = name,
            .owner_key = owner_key,
            .member = member,
            .kind = kind,
            .cpp_qualified_name = entity.cpp_qualified_name,
            .source_module = entity.source_module,
            .source_path = entity.source_path,
            .source_sha256 = entity.source_sha256,
            .active_condition = entity.active_condition,
            .default_expression = entity.initializer_expression,
            .minimum_expression = std::move(minimum),
            .maximum_expression = std::move(maximum),
            .context = guc_detail::context_name(entity),
            .flags = guc_detail::guc_flags(entity),
            .unit = guc_detail::guc_unit(entity),
            .short_description = entity.documentation.brief,
            .long_description = entity.documentation.details,
            .storage_kind =
                guc_detail::storage_kind_from_entity(entity),
            .feature_predicate =
                guc_detail::canonical_feature_predicate(
                    entity.active_condition),
            .reset_plan_cache =
                guc_detail::resets_plan_cache(entity),
        };
        if (guc.reset_plan_cache) {
            guc.hooks[1] =
                "assign_" + guc.member
                + "_and_reset_plan_cache";
        }
        if (kind == guc_kind::enumeration) {
            const std::string enum_leaf =
                guc_detail::enum_type_leaf(
                    entity.cpp_return_type);
            if (enum_leaf.empty()) {
                throw std::runtime_error(
                    "enum GUC '" + name
                    + "' has no postgres_enum_storage type argument");
            }
            const guc_detail::parsed_enum parsed =
                guc_detail::parse_enum(
                    program, entity, enum_leaf);
            guc.enum_cpp_name = parsed.qualified_name;
            guc.enum_values =
                guc_detail::collect_enum_values(
                    program, entity, parsed);
            guc_detail::resolved_enum_default resolved_default =
                guc_detail::resolve_enum_default(
                    program, entity, parsed, guc.enum_values);
            guc.default_expression =
                std::move(resolved_default.cpp_expression);
            guc.enum_default_name =
                std::move(resolved_default.value_name);
        }
        guc.generated_live_authority =
            guc_detail::make_generated_live_authority(
                program, entity, guc);
        result.gucs.push_back(std::move(guc));
    }
    return result;
}
}  // namespace bioseq::codegen::sql
