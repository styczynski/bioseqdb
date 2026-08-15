/**
 * @file
 * @brief Collects and validates annotated build-configuration declarations.
 */
module;

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

export module bioseq.codegen.build_config:validator;

import :model;
import bioseq.codegen.structural_analysis;
import bioseq.codegen.support;

export namespace bioseq::codegen::build_config {

namespace detail {

[[nodiscard]] inline bool ascii_space(const char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == '\f' ||
           value == '\v';
}

[[nodiscard]] inline std::string_view trim(std::string_view value) noexcept {
    while (!value.empty() && ascii_space(value.front()))
        value.remove_prefix(1U);
    while (!value.empty() && ascii_space(value.back()))
        value.remove_suffix(1U);
    return value;
}

[[nodiscard]] inline std::string parse_string_literal(const std::string_view input,
                                                      std::size_t& cursor) {
    if (cursor >= input.size() || input[cursor] != '"') {
        throw std::runtime_error("build_config arguments must be ordinary narrow string literals");
    }
    ++cursor;
    std::string result;
    while (cursor < input.size()) {
        const char value = input[cursor++];
        if (value == '"')
            return result;
        if (value == '\0' || value == '\n' || value == '\r') {
            throw std::runtime_error("build_config string arguments cannot contain line breaks");
        }
        if (value != '\\') {
            result.push_back(value);
            continue;
        }
        if (cursor == input.size()) {
            throw std::runtime_error("build_config string argument ends in an escape");
        }
        const char escaped = input[cursor++];
        if (escaped != '\\' && escaped != '"' && escaped != '\'') {
            throw std::runtime_error("build_config string arguments accept only printable escapes");
        }
        result.push_back(escaped);
    }
    throw std::runtime_error("unterminated build_config string argument");
}

[[nodiscard]] inline std::vector<std::string> parse_arguments(std::string_view input) {
    input = trim(input);
    std::vector<std::string> result;
    std::size_t cursor = 0U;
    while (cursor < input.size()) {
        while (cursor < input.size() && ascii_space(input[cursor]))
            ++cursor;
        result.push_back(parse_string_literal(input, cursor));
        while (cursor < input.size() && ascii_space(input[cursor]))
            ++cursor;
        if (cursor == input.size())
            break;
        if (input[cursor++] != ',') {
            throw std::runtime_error("build_config arguments must be comma-separated");
        }
        std::size_t next = cursor;
        while (next < input.size() && ascii_space(input[next]))
            ++next;
        if (next == input.size()) {
            throw std::runtime_error("build_config attribute cannot end with a comma");
        }
    }
    if (result.size() != 3U) {
        throw std::runtime_error("build_config requires macro, unit, and domain string arguments");
    }
    return result;
}

[[nodiscard]] inline bool lower_snake_identifier(const std::string_view value) noexcept {
    if (value.empty() || value.front() < 'a' || value.front() > 'z' || value.back() == '_' ||
        value.contains("__")) {
        return false;
    }
    return std::ranges::all_of(value, [](const char character) {
        return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
               character == '_';
    });
}

[[nodiscard]] inline std::string expected_macro_name(const std::string_view variable_name) {
    constexpr std::string_view prefix = "bioseq_";
    if (!variable_name.starts_with(prefix))
        return {};
    std::string result{"BIOSEQ_BUILD_CONFIG_"};
    for (const char character : variable_name.substr(prefix.size())) {
        result.push_back(character >= 'a' && character <= 'z'
                             ? static_cast<char>(character - 'a' + 'A')
                             : character);
    }
    return result;
}

[[nodiscard]] inline std::int64_t parse_decimal(std::string_view expression,
                                                const bool allow_unsigned_suffix) {
    expression = trim(expression);
    if (allow_unsigned_suffix && !expression.empty() &&
        (expression.back() == 'U' || expression.back() == 'u')) {
        expression.remove_suffix(1U);
    }
    if (expression.empty() || expression.front() == '+') {
        throw std::runtime_error("build_config integer values must be canonical decimal literals");
    }
    std::int64_t value{};
    const auto parsed =
        std::from_chars(expression.data(), expression.data() + expression.size(), value, 10);
    if (parsed.ec != std::errc{} || parsed.ptr != expression.data() + expression.size()) {
        throw std::runtime_error("build_config integer values must be canonical decimal literals");
    }
    return value;
}

[[nodiscard]] inline std::int64_t
parse_value(const value_kind kind, const std::string_view expression, const bool authored_default) {
    const std::string_view value = trim(expression);
    if (kind == value_kind::boolean) {
        if (value == "false" || value == "0")
            return 0;
        if (value == "true" || value == "1")
            return 1;
        throw std::runtime_error("boolean build_config values must be false, true, 0, or 1");
    }
    const std::int64_t parsed =
        parse_decimal(value, authored_default && kind == value_kind::unsigned_integer);
    if (kind == value_kind::unsigned_integer && parsed < 0) {
        throw std::runtime_error("unsigned build_config values cannot be negative");
    }
    return parsed;
}

[[nodiscard]] inline value_kind parse_type(const std::string_view cpp_type) {
    const std::string_view type = trim(cpp_type);
    if (type == "bool")
        return value_kind::boolean;
    if (type == "int")
        return value_kind::signed_integer;
    if (type == "unsigned int")
        return value_kind::unsigned_integer;
    throw std::runtime_error("build_config supports only bool, int, and unsigned int declarations");
}

[[nodiscard]] inline value_domain parse_domain(const value_kind kind,
                                               const std::string_view spelling) {
    if (kind == value_kind::boolean) {
        if (spelling != "false|true") {
            throw std::runtime_error("boolean build_config domain must be exactly false|true");
        }
        return value_domain{
            .kind = domain_kind::boolean,
            .minimum = 0,
            .maximum = 1,
            .values = {0, 1},
            .spelling = std::string{spelling},
        };
    }

    const std::size_t range_separator = spelling.find("..");
    if (range_separator != std::string_view::npos) {
        if (spelling.find("..", range_separator + 2U) != std::string_view::npos) {
            throw std::runtime_error("build_config range domains require one '..' separator");
        }
        const std::int64_t minimum = parse_decimal(spelling.substr(0U, range_separator), false);
        const std::int64_t maximum = parse_decimal(spelling.substr(range_separator + 2U), false);
        if (minimum > maximum || (kind == value_kind::unsigned_integer && minimum < 0)) {
            throw std::runtime_error("build_config range domain is invalid");
        }
        return value_domain{
            .kind = domain_kind::closed_range,
            .minimum = minimum,
            .maximum = maximum,
            .spelling = std::string{spelling},
        };
    }

    value_domain result{
        .kind = domain_kind::enumeration,
        .spelling = std::string{spelling},
    };
    std::size_t begin = 0U;
    while (begin <= spelling.size()) {
        const std::size_t separator = spelling.find('|', begin);
        const std::string_view item = spelling.substr(
            begin,
            separator == std::string_view::npos ? spelling.size() - begin : separator - begin);
        result.values.push_back(parse_decimal(item, false));
        if (separator == std::string_view::npos)
            break;
        begin = separator + 1U;
    }
    std::ranges::sort(result.values);
    if (result.values.size() < 2U ||
        std::ranges::adjacent_find(result.values) != result.values.end() ||
        (kind == value_kind::unsigned_integer && result.values.front() < 0)) {
        throw std::runtime_error("build_config enumeration domains require distinct valid values");
    }
    result.minimum = result.values.front();
    result.maximum = result.values.back();
    return result;
}

inline void validate_type_bounds(const value_kind kind, const value_domain& domain) {
    if (kind == value_kind::signed_integer &&
        (domain.minimum < (std::numeric_limits<int>::min)() ||
         domain.maximum > (std::numeric_limits<int>::max)())) {
        throw std::runtime_error("build_config domain exceeds int");
    }
    if (kind == value_kind::unsigned_integer &&
        static_cast<std::uint64_t>(domain.maximum) > (std::numeric_limits<unsigned int>::max)()) {
        throw std::runtime_error("build_config domain exceeds unsigned int");
    }
}

inline void validate_in_domain(const config_entry& entry,
                               const std::int64_t value,
                               const std::string_view role) {
    const bool accepted = entry.domain.kind == domain_kind::enumeration
                              ? std::ranges::contains(entry.domain.values, value)
                              : value >= entry.domain.minimum && value <= entry.domain.maximum;
    if (!accepted) {
        throw std::runtime_error(std::string{role} + " for " + entry.macro_name +
                                 " is outside domain " + entry.domain.spelling);
    }
}

[[nodiscard]] inline bool
has_required_documentation(const std::string_view source,
                           const structural_analysis::declaration_record& declaration,
                           const std::string_view unit,
                           const std::string_view domain,
                           const std::string_view default_expression) {
    const std::size_t declaration_begin = declaration.declaration_range.begin.byte_offset;
    const std::size_t close = source.rfind("*/", declaration_begin);
    if (close == std::string_view::npos ||
        !trim(source.substr(close + 2U, declaration_begin - close - 2U)).empty()) {
        return false;
    }
    const std::size_t open = source.rfind("/**", close);
    if (open == std::string_view::npos)
        return false;
    const std::string_view documentation = source.substr(open, close + 2U - open);
    const std::string expected_unit = "Unit: " + std::string{unit} + ".";
    const std::string expected_domain = "Domain: " + std::string{domain} + ".";
    const std::string expected_default = "Default: " + std::string{default_expression} + ".";
    return documentation.contains("@brief") && documentation.contains(expected_unit) &&
           documentation.contains(expected_domain) && documentation.contains(expected_default);
}

[[nodiscard]] inline const structural_analysis::attribute_record&
config_attribute(const structural_analysis::declaration_record& declaration) {
    const auto first = std::ranges::find(declaration.attributes,
                                         std::string_view{"bioseq::build_config"},
                                         &structural_analysis::attribute_record::qualified_name);
    if (first == declaration.attributes.end()) {
        throw std::runtime_error("public build-config declaration '" + declaration.name +
                                 "' lacks [[bioseq::build_config]]");
    }
    if (std::ranges::find(std::next(first),
                          declaration.attributes.end(),
                          std::string_view{"bioseq::build_config"},
                          &structural_analysis::attribute_record::qualified_name) !=
        declaration.attributes.end()) {
        throw std::runtime_error("build-config declaration '" + declaration.name +
                                 "' has duplicate attributes");
    }
    return *first;
}

[[nodiscard]] inline config_entry
make_entry(const std::string_view source,
           const structural_analysis::declaration_record& declaration) {
    if (declaration.kind != structural_analysis::declaration_kind::variable ||
        declaration.enclosing_qualified_name != "bioseq::build_config" ||
        !declaration.specifiers.direct_namespace_member || !declaration.specifiers.is_constexpr ||
        !declaration.specifiers.is_inline || declaration.specifiers.is_static ||
        declaration.specifiers.is_extern || declaration.specifiers.is_thread_local ||
        !declaration.specifiers.has_explicit_type || declaration.specifiers.has_deduced_type ||
        declaration.specifiers.multiple_declarators) {
        throw std::runtime_error("build_config targets must be direct inline constexpr variables "
                                 "with one explicit supported type");
    }
    if (!lower_snake_identifier(declaration.name) || !declaration.name.starts_with("bioseq_")) {
        throw std::runtime_error(
            "public build-config variable names must start with bioseq_ and use lower_snake_case");
    }
    if (declaration.initializer_text.empty() ||
        declaration.body_or_initializer_range.end.byte_offset <=
            declaration.body_or_initializer_range.begin.byte_offset) {
        throw std::runtime_error("build-config declaration '" + declaration.name +
                                 "' requires one literal default initializer");
    }
    const structural_analysis::attribute_record& attribute = config_attribute(declaration);
    const std::vector<std::string> arguments = parse_arguments(attribute.argument_text);
    const std::string expected_macro = expected_macro_name(declaration.name);
    if (arguments[0] != expected_macro) {
        throw std::runtime_error("build-config macro for '" + declaration.name +
                                 "' must be exactly " + expected_macro);
    }
    if (arguments[1].empty() || !std::ranges::all_of(arguments[1], [](const char character) {
            return (character >= 'a' && character <= 'z') ||
                   (character >= '0' && character <= '9') || character == '_';
        })) {
        throw std::runtime_error("build_config units must use a non-empty lower_snake token");
    }
    if (!has_required_documentation(
            source, declaration, arguments[1], arguments[2], trim(declaration.initializer_text))) {
        throw std::runtime_error("build-config declaration '" + declaration.name +
                                 "' must exactly document its unit, domain, and default");
    }

    const value_kind kind = parse_type(declaration.explicit_declared_type);
    const value_domain domain = parse_domain(kind, arguments[2]);
    validate_type_bounds(kind, domain);
    config_entry result{
        .name = declaration.name,
        .macro_name = arguments[0],
        .cpp_type = std::string{trim(declaration.explicit_declared_type)},
        .unit = arguments[1],
        .default_expression = std::string{trim(declaration.initializer_text)},
        .default_value = parse_value(kind, declaration.initializer_text, true),
        .kind = kind,
        .domain = domain,
        .attribute_begin = attribute.removal_range.begin.byte_offset,
        .attribute_end = attribute.removal_range.end.byte_offset,
        .initializer_begin = declaration.body_or_initializer_range.begin.byte_offset,
        .initializer_end = declaration.body_or_initializer_range.end.byte_offset,
    };
    validate_in_domain(result, result.default_value, "authored default");
    return result;
}

[[nodiscard]] inline const resolved_entry* find_resolved(const std::vector<resolved_entry>& entries,
                                                         const std::string_view name) noexcept {
    const auto found = std::ranges::find_if(
        entries, [name](const resolved_entry& candidate) { return candidate.entry.name == name; });
    return found == entries.end() ? nullptr : &*found;
}

inline void validate_cross_policy(const std::vector<resolved_entry>& entries) {
    const resolved_entry* profile = find_resolved(entries, "bioseq_template_flatten_profile");
    const resolved_entry* all_alphabets =
        find_resolved(entries, "bioseq_template_flatten_has_all_alphabets_uint64");
    const resolved_entry* all_coordinates =
        find_resolved(entries, "bioseq_template_flatten_has_all_coordinates");
    const resolved_entry* adaptive =
        find_resolved(entries, "bioseq_template_flatten_has_adaptive_lengths");
    const resolved_entry* extended =
        find_resolved(entries, "bioseq_template_flatten_has_extended_kmer_coordinates");
    const resolved_entry* full =
        find_resolved(entries, "bioseq_template_flatten_has_full_specialization");
    const resolved_entry* heavy = find_resolved(entries, "bioseq_scheduler_typed_heavy_unit_count");
    if (profile && all_alphabets && all_coordinates && adaptive && extended && full && heavy) {
        const std::int64_t ordinal = profile->value;
        const std::int64_t expected_heavy = ordinal == 0 ? 7 : ordinal == 1 ? 14 : 56;
        const bool consistent =
            all_alphabets->value == (ordinal >= 1) && all_coordinates->value == (ordinal >= 2) &&
            adaptive->value == (ordinal >= 3) && extended->value == (ordinal >= 4) &&
            full->value == (ordinal >= 5) && heavy->value == expected_heavy;
        if (!consistent) {
            throw std::runtime_error(
                "template-flatten capability overrides do not match the selected profile");
        }
    }

    const resolved_entry* wait =
        find_resolved(entries, "bioseq_vacuum_truncate_lock_wait_interval_milliseconds");
    const resolved_entry* timeout =
        find_resolved(entries, "bioseq_vacuum_truncate_lock_timeout_milliseconds");
    if (wait && timeout && wait->value > timeout->value) {
        throw std::runtime_error("vacuum truncate wait interval cannot exceed its timeout");
    }
}

} // namespace detail

[[nodiscard]] inline config_model collect_config_model(const std::filesystem::path& source_path,
                                                       const std::string_view source) {
    const structural_analysis::translation_unit unit =
        structural_analysis::scan_token_slice(source_path, source);
    if (unit.source_module.provided_module != "bioseq.build_config" ||
        !unit.source_module.is_interface || unit.source_module.is_partition) {
        throw std::runtime_error(
            "canonical build config must provide primary interface module bioseq.build_config");
    }
    if (!unit.preprocessor_directives.empty()) {
        throw std::runtime_error("authored build config cannot contain preprocessor directives");
    }
    const auto config_namespace =
        std::ranges::find(unit.namespaces,
                          std::string_view{"bioseq::build_config"},
                          &structural_analysis::namespace_record::qualified_name);
    if (config_namespace == unit.namespaces.end() || !config_namespace->has_explicit_export ||
        config_namespace->is_anonymous || config_namespace->is_inline) {
        throw std::runtime_error(
            "canonical build config requires one exported bioseq::build_config namespace");
    }

    config_model result{
        .source_path = source_path,
        .source = std::string{source},
        .source_sha256 = unit.source_sha256,
        .module_name = unit.source_module.provided_module,
        .module_declaration_begin = unit.source_module.declaration_range.begin.byte_offset,
    };
    std::set<std::string, std::less<>> names;
    std::set<std::string, std::less<>> macros;
    for (const structural_analysis::declaration_record& declaration : unit.declarations) {
        if (declaration.enclosing_qualified_name != "bioseq::build_config") {
            continue;
        }
        config_entry entry = detail::make_entry(source, declaration);
        if (!names.insert(entry.name).second) {
            throw std::runtime_error("duplicate build-config variable '" + entry.name + "'");
        }
        if (!macros.insert(entry.macro_name).second) {
            throw std::runtime_error("duplicate build-config macro '" + entry.macro_name + "'");
        }
        result.entries.push_back(std::move(entry));
    }
    if (result.entries.empty()) {
        throw std::runtime_error("canonical build config has no annotated variables");
    }
    std::ranges::sort(result.entries, {}, &config_entry::initializer_begin);
    return result;
}

[[nodiscard]] inline config_model collect_config_model(const std::filesystem::path& source_path) {
    const std::string source = bioseq::codegen::read_artifact_bytes(source_path);
    return collect_config_model(source_path, source);
}

[[nodiscard]] inline std::vector<resolved_entry>
resolve_config(const config_model& model, const std::vector<config_override>& overrides) {
    std::map<std::string, std::string, std::less<>> requested;
    for (const config_override& override : overrides) {
        if (!requested.emplace(override.macro_name, override.expression).second) {
            throw std::runtime_error("duplicate build-config override '" + override.macro_name +
                                     "'");
        }
    }

    std::vector<resolved_entry> result;
    result.reserve(model.entries.size());
    for (const config_entry& entry : model.entries) {
        const auto found = requested.find(entry.macro_name);
        const std::int64_t value = found == requested.end()
                                       ? entry.default_value
                                       : detail::parse_value(entry.kind, found->second, false);
        detail::validate_in_domain(entry, value, "override");
        result.push_back(resolved_entry{
            .entry = entry,
            .expression =
                entry.kind == value_kind::boolean ? value == 0 ? "0" : "1" : std::to_string(value),
            .value = value,
        });
        requested.erase(entry.macro_name);
    }
    if (!requested.empty()) {
        throw std::runtime_error("unknown build-config override '" + requested.begin()->first +
                                 "'");
    }
    detail::validate_cross_policy(result);
    return result;
}

} // namespace bioseq::codegen::build_config
