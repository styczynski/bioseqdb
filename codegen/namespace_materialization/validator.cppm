module;

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.codegen.namespace_materialization:validator;

import :model;
import bioseq.codegen.structural_analysis;
import bioseq.codegen.support;

export namespace bioseq::codegen::namespace_materialization {

namespace detail {

inline constexpr std::string_view flatten_attribute =
    "bioseq::template_flatten";
inline constexpr std::string_view when_attribute =
    "bioseq::template_flatten_when";
inline constexpr std::string_view entry_attribute =
    "bioseq::template_flatten_entry";

[[nodiscard]] inline bool ascii_space(const char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n'
        || value == '\f' || value == '\v';
}

[[nodiscard]] inline bool lowercase_label(std::string_view value) noexcept {
    if (value.empty() || value.front() < 'a' || value.front() > 'z') {
        return false;
    }
    return std::ranges::all_of(value, [](const char character) {
        return (character >= 'a' && character <= 'z')
            || (character >= '0' && character <= '9') || character == '_';
    });
}

[[nodiscard]] inline bool identifier(std::string_view value) noexcept {
    if (value.empty()
        || !((value.front() >= 'a' && value.front() <= 'z')
             || (value.front() >= 'A' && value.front() <= 'Z')
             || value.front() == '_')) {
        return false;
    }
    return std::ranges::all_of(value, [](const char character) {
        return (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9') || character == '_';
    });
}

[[nodiscard]] inline bool module_name(std::string_view value) noexcept {
    if (value.empty() || value.front() == '.' || value.back() == '.'
        || value.contains("..") || value.starts_with(':')) {
        return false;
    }
    std::size_t begin = 0;
    bool saw_partition = false;
    while (begin < value.size()) {
        const std::size_t separator = value.find_first_of(".:", begin);
        const std::string_view component = value.substr(
            begin,
            separator == std::string_view::npos
                ? value.size() - begin
                : separator - begin);
        if (!identifier(component)) return false;
        if (separator == std::string_view::npos) break;
        if (value[separator] == ':') {
            if (saw_partition || separator + 1 == value.size()) return false;
            saw_partition = true;
        } else if (saw_partition) {
            return false;
        }
        begin = separator + 1;
    }
    return true;
}

[[nodiscard]] inline bool lowercase_hex_digest(std::string_view value) noexcept {
    return value.size() == 64u
        && std::ranges::all_of(value, [](const char character) {
               return (character >= '0' && character <= '9')
                   || (character >= 'a' && character <= 'f');
           });
}

[[nodiscard]] inline bool nonempty_range(
    const structural_analysis::source_range& range) noexcept {
    return range.end.byte_offset > range.begin.byte_offset;
}

[[nodiscard]] inline bool contains(
    const structural_analysis::source_range& outer,
    const structural_analysis::source_range& inner) noexcept {
    return outer.begin.byte_offset <= inner.begin.byte_offset
        && inner.end.byte_offset <= outer.end.byte_offset;
}

[[nodiscard]] inline bool intersects(
    const structural_analysis::source_range& left,
    const structural_analysis::source_range& right) noexcept {
    return left.begin.byte_offset < right.end.byte_offset
        && right.begin.byte_offset < left.end.byte_offset;
}

[[nodiscard]] inline const structural_analysis::source_range& declaration_range(
    const structural_analysis::declaration_record& declaration) noexcept {
    return nonempty_range(declaration.declaration_range)
        ? declaration.declaration_range
        : declaration.range;
}

[[nodiscard]] inline const structural_analysis::source_range& attribute_range(
    const structural_analysis::attribute_record& attribute) noexcept {
    return nonempty_range(attribute.removal_range)
        ? attribute.removal_range
        : attribute.spelling_range;
}

[[nodiscard]] inline std::string location(
    const std::filesystem::path& path,
    const structural_analysis::source_range& range) {
    return path.string() + ':' + std::to_string(range.begin.line) + ':'
        + std::to_string(range.begin.column);
}

[[noreturn]] inline void reject(
    const std::filesystem::path& path,
    const structural_analysis::source_range& range,
    std::string message) {
    throw std::runtime_error(location(path, range) + ": " + std::move(message));
}

inline constexpr std::string_view scheduler_output_module =
    "bioseq.postgres.scheduler.dispatch_processor";

[[nodiscard]] inline bool forbidden_scheduler_highway_provider(
    std::string_view module,
    std::string_view output_module) {
    return module.starts_with("bioseq.highway.generated.")
        || module
            == std::string{output_module} + ":highway_registry";
}

[[nodiscard]] inline unsigned hex_value(const char character) noexcept {
    if (character >= '0' && character <= '9') {
        return static_cast<unsigned>(character - '0');
    }
    if (character >= 'a' && character <= 'f') {
        return static_cast<unsigned>(character - 'a' + 10);
    }
    return static_cast<unsigned>(character - 'A' + 10);
}

[[nodiscard]] inline bool hex_digit(const char character) noexcept {
    return (character >= '0' && character <= '9')
        || (character >= 'a' && character <= 'f')
        || (character >= 'A' && character <= 'F');
}

inline void append_utf8(std::string& output, const std::uint32_t codepoint) {
    if (codepoint == 0u || codepoint > 0x10ffffu
        || (codepoint >= 0xd800u && codepoint <= 0xdfffu)) {
        throw std::runtime_error(
            "template_flatten string literal contains an invalid universal character");
    }
    if (codepoint <= 0x7fu) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ffu) {
        output.push_back(static_cast<char>(0xc0u | (codepoint >> 6u)));
        output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
    } else if (codepoint <= 0xffffu) {
        output.push_back(static_cast<char>(0xe0u | (codepoint >> 12u)));
        output.push_back(
            static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
    } else {
        output.push_back(static_cast<char>(0xf0u | (codepoint >> 18u)));
        output.push_back(
            static_cast<char>(0x80u | ((codepoint >> 12u) & 0x3fu)));
        output.push_back(
            static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
    }
}

[[nodiscard]] inline std::string parse_one_string_literal(
    const std::string_view input,
    std::size_t& cursor) {
    if (cursor >= input.size() || input[cursor] != '"') {
        throw std::runtime_error(
            "every template_flatten argument must be one ordinary narrow string-literal token");
    }
    ++cursor;
    std::string result;
    while (cursor < input.size()) {
        const unsigned char character =
            static_cast<unsigned char>(input[cursor++]);
        if (character == '"') return result;
        if (character == 0u || character == '\n' || character == '\r') {
            throw std::runtime_error(
                "template_flatten string literal contains a NUL or newline");
        }
        if (character != '\\') {
            result.push_back(static_cast<char>(character));
            continue;
        }
        if (cursor >= input.size()) {
            throw std::runtime_error(
                "template_flatten string literal ends in an escape");
        }
        const char escaped = input[cursor++];
        switch (escaped) {
            case '\\': result.push_back('\\'); break;
            case '\'': result.push_back('\''); break;
            case '"': result.push_back('"'); break;
            case '?': result.push_back('?'); break;
            case 'a': result.push_back('\a'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'v': result.push_back('\v'); break;
            case 'u':
            case 'U': {
                const std::size_t digits = escaped == 'u' ? 4u : 8u;
                if (cursor + digits > input.size()) {
                    throw std::runtime_error(
                        "truncated universal character in template_flatten argument");
                }
                std::uint32_t codepoint = 0;
                for (std::size_t index = 0; index < digits; ++index) {
                    if (!hex_digit(input[cursor])) {
                        throw std::runtime_error(
                            "invalid universal character in template_flatten argument");
                    }
                    codepoint = (codepoint << 4u) | hex_value(input[cursor++]);
                }
                append_utf8(result, codepoint);
                break;
            }
            case 'x': {
                if (cursor >= input.size() || !hex_digit(input[cursor])) {
                    throw std::runtime_error(
                        "empty hexadecimal escape in template_flatten argument");
                }
                std::uint32_t value = 0;
                while (cursor < input.size() && hex_digit(input[cursor])) {
                    if (value > 0x0fffffffu) {
                        throw std::runtime_error(
                            "oversized hexadecimal escape in template_flatten argument");
                    }
                    value = (value << 4u) | hex_value(input[cursor++]);
                }
                if (value == 0u || value > 0xffu) {
                    throw std::runtime_error(
                        "template_flatten hexadecimal escape is not one non-NUL byte");
                }
                result.push_back(static_cast<char>(value));
                break;
            }
            default:
                if (escaped < '0' || escaped > '7') {
                    throw std::runtime_error(
                        "unknown escape in template_flatten string literal");
                }
                std::uint32_t value = static_cast<std::uint32_t>(escaped - '0');
                for (std::size_t count = 1;
                     count < 3u && cursor < input.size()
                     && input[cursor] >= '0' && input[cursor] <= '7';
                     ++count) {
                    value = value * 8u
                        + static_cast<std::uint32_t>(input[cursor++] - '0');
                }
                if (value == 0u || value > 0xffu) {
                    throw std::runtime_error(
                        "template_flatten octal escape is not one non-NUL byte");
                }
                result.push_back(static_cast<char>(value));
                break;
        }
    }
    throw std::runtime_error("unterminated template_flatten string literal");
}

[[nodiscard]] inline std::string trim_ascii(std::string_view value) {
    while (!value.empty() && ascii_space(value.front())) value.remove_prefix(1);
    while (!value.empty() && ascii_space(value.back())) value.remove_suffix(1);
    return std::string{value};
}

[[nodiscard]] inline std::vector<std::string> cpp_token_sequence(
    std::string_view spelling) {
    const std::string wrapper =
        "export module bioseq.codegen.probes.token_normalization;\n"
        "using bioseq_token_normalization = "
        + std::string{spelling} + ";\n";
    const structural_analysis::translation_unit unit =
        structural_analysis::scan_token_slice(
            "template_flatten_token_normalization.cppm", wrapper);
    std::vector<std::string> result;
    bool in_replacement = false;
    bool terminated = false;
    for (const structural_analysis::lexical_token& token : unit.tokens) {
        if (!in_replacement) {
            if (token.spelling == "=") in_replacement = true;
            continue;
        }
        if (token.spelling == ";") {
            terminated = true;
            break;
        }
        result.push_back(token.spelling);
    }
    if (!in_replacement || !terminated || result.empty()) {
        throw std::runtime_error(
            "cannot tokenize template_flatten replacement spelling");
    }
    return result;
}

[[nodiscard]] inline std::string slice(
    std::string_view source,
    const structural_analysis::source_range& range,
    std::string_view role) {
    if (range.begin.byte_offset > range.end.byte_offset
        || range.end.byte_offset > source.size()) {
        throw std::runtime_error(
            "structural " + std::string{role} + " range is outside the source");
    }
    return std::string{source.substr(
        range.begin.byte_offset,
        range.end.byte_offset - range.begin.byte_offset)};
}

inline void validate_replacement_spelling(
    const replacement_case& replacement,
    std::string_view group) {
    const std::string spelling = trim_ascii(replacement.spelling);
    if (spelling.empty()) {
        throw std::runtime_error(
            "template_flatten group '" + std::string{group} + "' case '"
            + replacement.label + "' has an empty replacement spelling");
    }
    if (spelling.contains('\0') || spelling.contains('\n')
        || spelling.contains('\r') || spelling.contains(';')
        || spelling.contains("//") || spelling.contains("/*")
        || spelling.contains("*/") || spelling.contains('#')) {
        throw std::runtime_error(
            "template_flatten group '" + std::string{group} + "' case '"
            + replacement.label
            + "' replacement contains a forbidden directive/comment/terminator");
    }
    std::vector<char> delimiters;
    bool in_string = false;
    bool in_character = false;
    bool escaped = false;
    for (const char character : spelling) {
        if (escaped) {
            escaped = false;
            continue;
        }
        if ((in_string || in_character) && character == '\\') {
            escaped = true;
            continue;
        }
        if (!in_character && character == '"') {
            in_string = !in_string;
            continue;
        }
        if (!in_string && character == '\'') {
            in_character = !in_character;
            continue;
        }
        if (in_string || in_character) continue;
        if (character == '(' || character == '[' || character == '{') {
            delimiters.push_back(character);
        } else if (character == ')' || character == ']' || character == '}') {
            const char expected = character == ')' ? '(' : character == ']' ? '[' : '{';
            if (delimiters.empty() || delimiters.back() != expected) {
                throw std::runtime_error(
                    "template_flatten replacement has unbalanced delimiters");
            }
            delimiters.pop_back();
        }
    }
    if (escaped || in_string || in_character || !delimiters.empty()) {
        throw std::runtime_error(
            "template_flatten replacement has an unterminated lexical construct");
    }
}

[[nodiscard]] inline std::vector<const structural_analysis::attribute_record*>
attributes_named(
    const structural_analysis::declaration_record& declaration,
    std::string_view qualified_name) {
    std::vector<const structural_analysis::attribute_record*> result;
    for (const structural_analysis::attribute_record& attribute
         : declaration.attributes) {
        if (attribute.qualified_name == qualified_name) {
            result.push_back(&attribute);
        }
    }
    return result;
}

[[nodiscard]] inline const replacement_case& selected_case(
    const binding_record& binding,
    const materialization_coordinate& coordinate) {
    const std::vector<axis_case_assignment>::const_iterator assignment =
        std::ranges::find(
        coordinate.assignments, binding.group, &axis_case_assignment::group);
    if (assignment == coordinate.assignments.end()) {
        throw std::runtime_error(
            "coordinate has no assignment for group '" + binding.group + "'");
    }
    const std::vector<replacement_case>::const_iterator replacement =
        std::ranges::find(
        binding.cases, assignment->case_label, &replacement_case::label);
    if (replacement == binding.cases.end()) {
        throw std::runtime_error(
            "coordinate selects unknown case '" + assignment->case_label
            + "' for group '" + binding.group + "'");
    }
    return *replacement;
}

[[nodiscard]] inline std::string identity_key(
    const materialization_coordinate& coordinate) {
    std::ostringstream result;
    for (const axis_case_assignment& assignment : coordinate.assignments) {
        if (assignment.role == axis_role::identity) {
            result << assignment.group << '=' << assignment.case_label << '\n';
        }
    }
    return result.str();
}

[[nodiscard]] inline std::string identity_namespace(
    std::string_view boundary,
    const std::vector<axis_case_assignment>& assignments) {
    std::string result{boundary};
    result += "::bioseq_flattened";
    for (const axis_case_assignment& assignment : assignments) {
        if (assignment.role == axis_role::identity) {
            result += "::" + assignment.group + '_' + assignment.case_label;
        }
    }
    return result;
}

[[nodiscard]] inline std::string canonical_topology_input(
    std::string_view output_module,
    const materialization_region& region,
    const std::vector<axis_group>& groups,
    const std::vector<materialization_coordinate>& coordinates) {
    std::ostringstream output;
    output << "bioseq-template-flatten-topology\n"
              "revision=1\n";
    output << "output-module\t" << output_module << '\n';
    for (std::size_t group_index = 0; group_index < groups.size(); ++group_index) {
        const axis_group& group = groups[group_index];
        output << "group\t" << group_index << '\t'
               << (group.role == axis_role::identity ? "identity" : "shard")
               << '\t' << group.name << '\n';
        for (std::size_t binding_ordinal = 0;
             binding_ordinal < group.binding_indices.size();
             ++binding_ordinal) {
            const binding_record& binding =
                region.bindings.at(group.binding_indices[binding_ordinal]);
            output << "binding\t" << group_index << '\t' << binding_ordinal
                   << '\t' << binding.declaration.name << '\n';
            for (const replacement_case& replacement : binding.cases) {
                output << "case\t" << group_index << '\t' << binding_ordinal
                       << '\t' << replacement.ordinal << '\t'
                       << replacement.label << '\t' << replacement.spelling
                       << '\n';
            }
        }
    }
    for (const materialization_coordinate& coordinate : coordinates) {
        output << "coordinate\t" << coordinate.ordinal;
        for (const axis_case_assignment& assignment : coordinate.assignments) {
            output << '\t' << assignment.group << '=' << assignment.case_label;
        }
        output << '\n';
    }
    return output.str();
}

}  // namespace detail

[[nodiscard]] inline std::vector<std::string> parse_closed_string_arguments(
    std::string_view argument_text) {
    while (!argument_text.empty() && detail::ascii_space(argument_text.front())) {
        argument_text.remove_prefix(1);
    }
    while (!argument_text.empty() && detail::ascii_space(argument_text.back())) {
        argument_text.remove_suffix(1);
    }
    std::vector<std::string> result;
    std::size_t cursor = 0;
    while (cursor < argument_text.size()) {
        while (cursor < argument_text.size()
               && detail::ascii_space(argument_text[cursor])) {
            ++cursor;
        }
        if (cursor == argument_text.size()) break;
        result.push_back(detail::parse_one_string_literal(argument_text, cursor));
        while (cursor < argument_text.size()
               && detail::ascii_space(argument_text[cursor])) {
            ++cursor;
        }
        if (cursor == argument_text.size()) break;
        if (argument_text[cursor++] != ',') {
            throw std::runtime_error(
                "template_flatten arguments must be comma-separated string literals");
        }
        std::size_t next = cursor;
        while (next < argument_text.size()
               && detail::ascii_space(argument_text[next])) {
            ++next;
        }
        if (next == argument_text.size()) {
            throw std::runtime_error(
                "template_flatten attribute has a trailing comma");
        }
    }
    if (result.empty()) {
        throw std::runtime_error(
            "template_flatten attribute requires string-literal arguments");
    }
    return result;
}

[[nodiscard]] inline axis_role parse_axis_role(std::string_view spelling) {
    if (spelling == "identity") return axis_role::identity;
    if (spelling == "shard") return axis_role::shard;
    throw std::runtime_error(
        "template_flatten role must be exactly 'identity' or 'shard'");
}

[[nodiscard]] inline binding_record parse_binding_attribute(
    const structural_analysis::declaration_record& declaration,
    const structural_analysis::attribute_record& attribute) {
    const std::vector<std::string> arguments =
        parse_closed_string_arguments(attribute.argument_text);
    if (arguments.size() < 4u || arguments.size() % 2u != 0u) {
        throw std::runtime_error(
            "template_flatten requires role, group, and one or more label/replacement pairs");
    }
    binding_record result{
        .declaration = declaration,
        .role = parse_axis_role(arguments[0]),
        .group = arguments[1],
    };
    if (!detail::lowercase_label(result.group)) {
        throw std::runtime_error(
            "template_flatten group '" + result.group
            + "' must match [a-z][a-z0-9_]*");
    }
    std::set<std::string, std::less<>> labels;
    for (std::size_t index = 2u; index < arguments.size(); index += 2u) {
        replacement_case replacement{
            .label = arguments[index],
            .spelling = arguments[index + 1u],
            .ordinal = static_cast<std::uint32_t>((index - 2u) / 2u),
        };
        if (!detail::lowercase_label(replacement.label)) {
            throw std::runtime_error(
                "template_flatten case label '" + replacement.label
                + "' must match [a-z][a-z0-9_]*");
        }
        if (!labels.insert(replacement.label).second) {
            throw std::runtime_error(
                "template_flatten group '" + result.group
                + "' repeats case label '" + replacement.label + "'");
        }
        detail::validate_replacement_spelling(replacement, result.group);
        result.cases.push_back(std::move(replacement));
    }
    return result;
}

[[nodiscard]] inline selection_clause parse_selection_attribute(
    const structural_analysis::attribute_record& attribute) {
    const std::vector<std::string> arguments =
        parse_closed_string_arguments(attribute.argument_text);
    if (arguments.size() < 2u || !detail::lowercase_label(arguments.front())) {
        throw std::runtime_error(
            "template_flatten_when requires a group and one or more case labels");
    }
    selection_clause result{.group = arguments.front()};
    std::set<std::string, std::less<>> labels;
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        if (!detail::lowercase_label(arguments[index])
            || !labels.insert(arguments[index]).second) {
            throw std::runtime_error(
                "template_flatten_when case labels must be unique and match "
                "[a-z][a-z0-9_]*");
        }
        result.accepted_case_labels.push_back(arguments[index]);
    }
    return result;
}

[[nodiscard]] inline std::string parse_entry_attribute(
    const structural_analysis::attribute_record& attribute) {
    const std::vector<std::string> arguments =
        parse_closed_string_arguments(attribute.argument_text);
    if (arguments.size() != 1u || !detail::lowercase_label(arguments.front())) {
        throw std::runtime_error(
            "template_flatten_entry requires exactly one role label matching "
            "[a-z][a-z0-9_]*");
    }
    return arguments.front();
}

void validate_coordinate_axis_domain(
    const materialization_coordinate& coordinate) {
    const std::size_t external_axes = std::ranges::count_if(
        coordinate.assignments,
        [](const axis_case_assignment& assignment) {
            return assignment.role == axis_role::external;
        });
    const std::size_t explicit_axes =
        coordinate.assignments.size() - external_axes;
    if ((external_axes == 0u && explicit_axes == 0u)
        || (external_axes != 0u && explicit_axes != 0u)
        || external_axes > 1u) {
        throw std::runtime_error(
            "a materialization coordinate must carry either explicit "
            "identity/shard axes or exactly one external axis");
    }
    if (external_axes == 1u) {
        const axis_case_assignment& external =
            *std::ranges::find(
                coordinate.assignments,
                axis_role::external,
                &axis_case_assignment::role);
        if (external.group.empty() || external.case_label.empty()
            || external.case_ordinal != coordinate.ordinal) {
            throw std::runtime_error(
                "an external materialization axis must name its provider, "
                "case, and ordered coordinate");
        }
    }
}

/// Returns whether a coordinate satisfies every selection clause.
[[nodiscard]] inline bool coordinate_matches(
    const materialization_coordinate& coordinate,
    const std::vector<selection_clause>& clauses) {
    for (const selection_clause& clause : clauses) {
        const std::vector<axis_case_assignment>::const_iterator assignment =
            std::ranges::find(
            coordinate.assignments, clause.group, &axis_case_assignment::group);
        if (assignment == coordinate.assignments.end()
            || std::ranges::find(
                   clause.accepted_case_labels, assignment->case_label)
                == clause.accepted_case_labels.end()) {
            return false;
        }
    }
    return true;
}

/// Resolves a binding's replacement at one validated coordinate.
[[nodiscard]] inline const replacement_case& replacement_for(
    const binding_record& binding,
    const materialization_coordinate& coordinate) {
    return detail::selected_case(binding, coordinate);
}

/// Builds source-ordered, zipped axis groups from extracted bindings.
[[nodiscard]] inline std::vector<axis_group> build_axis_groups(
    const materialization_region& region) {
    std::vector<axis_group> groups;
    std::map<std::string, std::size_t, std::less<>> by_name;
    for (std::size_t binding_index = 0;
         binding_index < region.bindings.size();
         ++binding_index) {
        const binding_record& binding = region.bindings[binding_index];
        const std::pair<
            std::map<std::string, std::size_t, std::less<>>::iterator,
            bool> insertion =
            by_name.emplace(binding.group, groups.size());
        if (insertion.second) {
            groups.push_back(axis_group{
                .name = binding.group,
                .role = binding.role,
                .cases = binding.cases,
                .binding_indices = {binding_index},
            });
            continue;
        }
        axis_group& group = groups[insertion.first->second];
        if (group.role != binding.role) {
            throw std::runtime_error(
                "zipped template_flatten group '" + binding.group
                + "' is declared with both identity and shard roles");
        }
        std::vector<std::string> expected;
        std::vector<std::string> actual;
        for (const replacement_case& replacement : group.cases) {
            expected.push_back(replacement.label);
        }
        for (const replacement_case& replacement : binding.cases) {
            actual.push_back(replacement.label);
        }
        if (expected != actual) {
            std::ostringstream message;
            message << "zipped template_flatten group '" << binding.group
                    << "' has unequal ordered labels; first=[";
            for (std::size_t index = 0; index < expected.size(); ++index) {
                if (index != 0u) message << ',';
                message << expected[index];
            }
            message << "], conflicting=[";
            for (std::size_t index = 0; index < actual.size(); ++index) {
                if (index != 0u) message << ',';
                message << actual[index];
            }
            message << ']';
            throw std::runtime_error(message.str());
        }
        group.binding_indices.push_back(binding_index);
    }
    if (groups.empty()) {
        throw std::runtime_error(
            "materialization region contains no template_flatten axes");
    }
    return groups;
}

/// Expands a validated region into its closed coordinate product.
[[nodiscard]] inline topology_plan build_topology_plan(
    materialization_region region,
    const std::size_t maximum_coordinates = 4096u) {
    topology_plan plan{.region = std::move(region)};
    if (!detail::module_name(plan.region.configured_output_module_name)
        || plan.region.configured_output_module_name.contains(':')) {
        throw std::runtime_error(
            "configured template_flatten output must be a primary module name");
    }
    plan.groups = build_axis_groups(plan.region);
    std::size_t product = 1u;
    for (const axis_group& group : plan.groups) {
        if (group.cases.empty()
            || product > maximum_coordinates / group.cases.size()) {
            throw std::runtime_error(
                "template_flatten coordinate product exceeds configured ceiling "
                + std::to_string(maximum_coordinates));
        }
        product *= group.cases.size();
    }
    plan.coordinates.reserve(product);
    for (std::size_t ordinal = 0; ordinal < product; ++ordinal) {
        std::size_t remainder = ordinal;
        std::vector<std::size_t> case_ordinals(plan.groups.size());
        for (std::size_t reverse = plan.groups.size(); reverse != 0u; --reverse) {
            const std::size_t group_index = reverse - 1u;
            case_ordinals[group_index] =
                remainder % plan.groups[group_index].cases.size();
            remainder /= plan.groups[group_index].cases.size();
        }
        materialization_coordinate coordinate{
            .ordinal = static_cast<std::uint32_t>(ordinal),
            .module_name = plan.region.configured_output_module_name,
            .relative_path =
                "instance_" + std::to_string(ordinal) + ".cppm",
        };
        for (std::size_t group_index = 0;
             group_index < plan.groups.size();
             ++group_index) {
            const axis_group& group = plan.groups[group_index];
            const replacement_case& replacement =
                group.cases[case_ordinals[group_index]];
            coordinate.assignments.push_back(axis_case_assignment{
                .group = group.name,
                .role = group.role,
                .case_label = replacement.label,
                .case_ordinal = replacement.ordinal,
            });
        }
        coordinate.identity_namespace = detail::identity_namespace(
            plan.region.boundary.qualified_name, coordinate.assignments);
        validate_coordinate_axis_domain(coordinate);
        plan.coordinates.push_back(std::move(coordinate));
    }
    plan.canonical_digest_input = detail::canonical_topology_input(
        plan.region.configured_output_module_name,
        plan.region,
        plan.groups,
        plan.coordinates);
    plan.topology_digest = bioseq::common::sha256_hex(plan.canonical_digest_input);
    for (materialization_coordinate& coordinate : plan.coordinates) {
        coordinate.topology_digest = plan.topology_digest;
    }
    std::set<std::string, std::less<>> identities;
    for (const materialization_coordinate& coordinate : plan.coordinates) {
        if (identities.insert(detail::identity_key(coordinate)).second) {
            plan.identity_representative_ordinals.push_back(coordinate.ordinal);
        }
    }
    return plan;
}

/// Closed semantic template-flatten profile vocabulary.
inline constexpr std::array<std::string_view, 6u>
    template_flatten_profiles{
        "dna4_uint64",
        "all_alphabets_uint64",
        "all_coordinates",
        "all_coordinates_adaptive_lengths",
        "extended_kmer_coordinates",
        "full_specialization",
    };

/// Returns whether a semantic template-flatten profile is supported.
[[nodiscard]] inline bool template_flatten_profile_supported(
    const std::string_view profile) noexcept {
    return std::ranges::find(template_flatten_profiles, profile)
        != template_flatten_profiles.end();
}

/// Returns whether a semantic profile includes a coordinate.
[[nodiscard]] inline bool coordinate_enabled_for_profile(
    const materialization_coordinate& coordinate,
    std::string_view profile) {
    if (!template_flatten_profile_supported(profile)) {
        throw std::runtime_error(
            "template_flatten profile is not in the closed semantic vocabulary");
    }
    const std::vector<axis_case_assignment>::const_iterator alphabet =
        std::ranges::find(
        coordinate.assignments, std::string_view{"alphabet"},
        &axis_case_assignment::group);
    const std::vector<axis_case_assignment>::const_iterator width =
        std::ranges::find(
        coordinate.assignments, std::string_view{"width"},
        &axis_case_assignment::group);
    if (alphabet == coordinate.assignments.end()
        || width == coordinate.assignments.end()) {
        return true;
    }
    if (profile == "dna4_uint64") {
        return alphabet->case_label == "dna4" && width->case_label == "w8";
    }
    if (profile == "all_alphabets_uint64") {
        return (alphabet->case_label == "dna4"
                || alphabet->case_label == "dna5")
            && width->case_label == "w8";
    }
    return true;
}

/// Returns all supported profiles that include a coordinate.
[[nodiscard]] inline std::vector<std::string> enabled_profiles_for(
    const materialization_coordinate& coordinate) {
    std::vector<std::string> result;
    for (const std::string_view profile : template_flatten_profiles) {
        if (coordinate_enabled_for_profile(coordinate, profile)) {
            result.emplace_back(profile);
        }
    }
    return result;
}

/// Shared configure/structural handshake used by production generation and
/// mutation contracts. A topology change always requires CMake to rebuild its
/// source graph; generation never chooses one side of a mismatch.
inline void validate_configure_topology_digest(
    const topology_plan& topology,
    std::string_view configured_digest) {
    const bool canonical_sha256 = configured_digest.size() == 64u
        && std::ranges::all_of(configured_digest, [](const char value) {
               return (value >= '0' && value <= '9')
                   || (value >= 'a' && value <= 'f');
           });
    if (!canonical_sha256
        || configured_digest != topology.topology_digest) {
        throw std::runtime_error(
            "configure-time and structural template_flatten topology "
            "digests disagree; configured=" + std::string{configured_digest}
            + ", structural=" + topology.topology_digest
            + "; reconfigure the build tree");
    }
}

/// Verifies the exact enabled-coordinate list for a profile.
inline void validate_enabled_ordinals(
    const topology_plan& topology,
    std::string_view profile,
    const std::vector<std::uint32_t>& enabled_ordinals) {
    std::vector<std::uint32_t> expected;
    for (const materialization_coordinate& coordinate : topology.coordinates) {
        if (coordinate_enabled_for_profile(coordinate, profile)) {
            expected.push_back(coordinate.ordinal);
        }
    }
    if (enabled_ordinals != expected) {
        std::ostringstream message;
        message << "profile " << profile
                << " enabled ordinal list disagrees with structural topology; expected=";
        for (std::size_t index = 0; index < expected.size(); ++index) {
            if (index != 0u) message << ',';
            message << expected[index];
        }
        message << ", configured=";
        for (std::size_t index = 0; index < enabled_ordinals.size(); ++index) {
            if (index != 0u) message << ',';
            message << enabled_ordinals[index];
        }
        throw std::runtime_error(message.str());
    }
}

/// Verifies unique generated entry ownership for every identity.
inline void validate_entry_ownership(const topology_plan& topology) {
    std::set<std::string, std::less<>> roles;
    for (const owned_declaration& declaration
         : topology.region.controlled_declarations) {
        if (declaration.generated_api_entry
            && !roles.insert(declaration.entry_role).second) {
            throw std::runtime_error(
                "template_flatten entry roles must be globally unique; duplicate role '"
                + declaration.entry_role + "'");
        }
    }
    if (roles.empty()) {
        throw std::runtime_error(
            "template_flatten region declares no generated API entries");
    }
    if (topology.region.configured_output_module_name
        == detail::scheduler_output_module) {
        const std::set<std::string, std::less<>> required{
            "root",
            "scanner_pg",
            "inmemory",
            "inmemory_standard",
            "inmemory_segmented",
            "inmemory_chunked",
            "inmemory_chunked_segmented",
        };
        if (roles != required) {
            throw std::runtime_error(
                "scheduler template_flatten topology must declare exactly the seven normative entry roles");
        }
    }
    const std::vector<owned_declaration>::const_iterator root =
        std::ranges::find_if(
        topology.region.controlled_declarations,
        [](const owned_declaration& declaration) {
            return declaration.generated_api_entry
                && declaration.entry_role == "root";
        });
    if (root == topology.region.controlled_declarations.end()) {
        throw std::runtime_error(
            "template_flatten topology requires one root selector entry");
    }
    std::set<std::string, std::less<>> root_parameters;
    for (const structural_analysis::parameter_record& parameter :
         root->declaration.parameters) {
        if (parameter.name.empty()
            || !root_parameters.insert(parameter.name).second) {
            throw std::runtime_error(
                "root selector entry requires unique source-spelled parameter names");
        }
    }
    for (const axis_group& group : topology.groups) {
        if (group.role != axis_role::identity) continue;
        const std::string selector_parameter = group.name == "alphabet"
            ? "alphabet_code"
            : group.name == "width"
            ? "storage_unit_byte_width"
            : group.name + "_code";
        if (!root_parameters.contains(selector_parameter)) {
            throw std::runtime_error(
                "root selector entry omits parameter '" + selector_parameter
                + "' required by identity group '" + group.name + "'");
        }
    }
    for (const std::uint32_t representative
         : topology.identity_representative_ordinals) {
        const materialization_coordinate& identity =
            topology.coordinates.at(representative);
        for (const std::string& role : roles) {
            std::vector<std::uint32_t> owners;
            for (const materialization_coordinate& coordinate
                 : topology.coordinates) {
                if (detail::identity_key(coordinate)
                    != detail::identity_key(identity)) {
                    continue;
                }
                for (const owned_declaration& declaration
                     : topology.region.controlled_declarations) {
                    if (declaration.generated_api_entry
                        && declaration.entry_role == role
                        && coordinate_matches(coordinate, declaration.clauses)) {
                        owners.push_back(coordinate.ordinal);
                    }
                }
            }
            if (owners.size() != 1u) {
                std::ostringstream message;
                message << "identity coordinate " << identity.ordinal
                        << " must own entry role '" << role
                        << "' in exactly one shard; owners=";
                if (owners.empty()) {
                    message << "none";
                } else {
                    for (std::size_t index = 0; index < owners.size(); ++index) {
                        if (index != 0u) message << ',';
                        message << owners[index];
                    }
                }
                throw std::runtime_error(message.str());
            }
        }
        for (const owned_declaration& declaration :
             topology.region.controlled_declarations) {
            if (declaration.generated_api_entry
                || !declaration.declaration.specifiers.is_definition
                || declaration.declaration.linkage
                    == structural_analysis::entity_linkage::internal) {
                continue;
            }
            const structural_analysis::source_range& range =
                declaration.declaration.declaration_range;
            if (topology.region.support_island.has_value()
                && detail::contains(
                    topology.region.support_island->body_range, range)) {
                continue;
            }
            std::vector<std::uint32_t> owners;
            for (const materialization_coordinate& coordinate :
                 topology.coordinates) {
                if (detail::identity_key(coordinate)
                        == detail::identity_key(identity)
                    && coordinate_matches(
                        coordinate, declaration.clauses)) {
                    owners.push_back(coordinate.ordinal);
                }
            }
            if (owners.size() != 1u) {
                throw std::runtime_error(
                    "non-TU-local controlled definition '"
                    + declaration.declaration.qualified_name
                    + "' must be selected into exactly one shard of identity coordinate "
                    + std::to_string(identity.ordinal));
            }
        }
    }
}

/// Verifies that body-free probes cover the logical topology exactly once.
inline void validate_declaration_probes(
    const topology_plan& topology,
    const std::vector<structural_analysis::translation_unit>& probes) {
    if (probes.size() != topology.identity_representative_ordinals.size()) {
        throw std::runtime_error(
            "declaration-probe action count disagrees with identity topology");
    }
    std::set<std::string, std::less<>> expected_occurrences;
    std::map<std::string, const binding_record*, std::less<>> bindings;
    std::map<std::string, const owned_declaration*, std::less<>> entries;
    for (const binding_record& binding : topology.region.bindings) {
        if (binding.declaration.source_identity.empty()
            || !bindings.emplace(
                    binding.declaration.source_identity, &binding).second) {
            throw std::runtime_error(
                "materialization bindings require unique source provenance IDs");
        }
        for (const materialization_coordinate& coordinate :
             topology.coordinates) {
            expected_occurrences.insert(
                binding.declaration.source_identity + "\n"
                + std::to_string(coordinate.ordinal));
        }
    }
    for (const owned_declaration& declaration
         : topology.region.controlled_declarations) {
        if (!declaration.generated_api_entry) continue;
        if (declaration.declaration.source_identity.empty()
            || bindings.contains(declaration.declaration.source_identity)
            || !entries.emplace(
                    declaration.declaration.source_identity,
                    &declaration).second) {
            throw std::runtime_error(
                "materialization entries require unique source provenance IDs");
        }
        for (const materialization_coordinate& coordinate :
             topology.coordinates) {
            if (coordinate_matches(coordinate, declaration.clauses)) {
                expected_occurrences.insert(
                    declaration.declaration.source_identity + "\n"
                    + std::to_string(coordinate.ordinal));
            }
        }
    }
    std::set<std::string, std::less<>> actual_occurrences;
    std::set<std::string, std::less<>> semantic_identities;
    std::set<std::string, std::less<>> probed_identity_keys;
    for (const structural_analysis::translation_unit& probe : probes) {
        if (probe.mode != structural_analysis::collection_mode::declaration_probe) {
            throw std::runtime_error(
                probe.source_path.string()
                + ": body-free validation did not use declaration_probe mode");
        }
        std::optional<std::string> probe_identity_key;
        std::set<std::uint32_t> probe_coordinates;
        for (const structural_analysis::declaration_record& declaration
             : probe.declarations) {
            if (declaration.source_identity.empty()) continue;
            if (!bindings.contains(declaration.source_identity)
                && !entries.contains(declaration.source_identity)) {
                throw std::runtime_error(
                    probe.source_path.string()
                    + ": declaration probe carries unauthenticated provenance '"
                    + declaration.source_identity + "'");
            }
            std::uint32_t coordinate_ordinal{};
            const std::from_chars_result parsed_coordinate = std::from_chars(
                declaration.source_coordinate.data(),
                declaration.source_coordinate.data()
                    + declaration.source_coordinate.size(),
                coordinate_ordinal);
            if (declaration.source_coordinate.empty()
                || parsed_coordinate.ec != std::errc{}
                || parsed_coordinate.ptr
                    != declaration.source_coordinate.data()
                        + declaration.source_coordinate.size()
                || coordinate_ordinal >= topology.coordinates.size()
                || declaration.source_coordinate
                    != std::to_string(coordinate_ordinal)) {
                throw std::runtime_error(
                    probe.source_path.string()
                    + ": declaration probe has invalid coordinate provenance for '"
                    + declaration.source_identity + "'");
            }
            const materialization_coordinate& coordinate =
                topology.coordinates[coordinate_ordinal];
            const std::string coordinate_identity =
                detail::identity_key(coordinate);
            if (probe_identity_key.has_value()
                && *probe_identity_key != coordinate_identity) {
                throw std::runtime_error(
                    probe.source_path.string()
                    + ": one declaration probe spans multiple identity coordinates");
            }
            probe_identity_key = coordinate_identity;
            probe_coordinates.insert(coordinate_ordinal);
            const std::string expected_namespace =
                coordinate.identity_namespace + "::bioseq_flatten_probe_"
                + std::to_string(coordinate_ordinal) + "::";
            if (!declaration.qualified_name.starts_with(expected_namespace)) {
                throw std::runtime_error(
                    probe.source_path.string()
                    + ": declaration probe namespace disagrees with coordinate provenance '"
                    + declaration.source_coordinate + "'");
            }
            const std::string occurrence = declaration.source_identity + "\n"
                + declaration.source_coordinate;
            if (!expected_occurrences.contains(occurrence)
                || !actual_occurrences.insert(occurrence).second) {
                throw std::runtime_error(
                    probe.source_path.string()
                    + ": declaration probe duplicates or misassigns provenance '"
                    + declaration.source_identity + "' at coordinate "
                    + declaration.source_coordinate);
            }
            if (declaration.stable_usr.empty()
                || declaration.attachment
                    != structural_analysis::module_attachment::named_module) {
                throw std::runtime_error(
                    probe.source_path.string()
                    + ": probe declaration lost its semantic USR/module attachment for provenance '"
                    + declaration.source_identity + "'");
            }
            if (!semantic_identities.insert(
                    declaration.stable_usr + "#"
                    + declaration.qualified_name).second) {
                throw std::runtime_error(
                    probe.source_path.string()
                    + ": duplicate semantic declaration identity in body-free probe");
            }
            if (const std::map<
                    std::string,
                    const binding_record*,
                    std::less<>>::const_iterator binding =
                    bindings.find(declaration.source_identity);
                binding != bindings.end()) {
                const bool expected_alias = binding->second->declaration.kind
                    == structural_analysis::declaration_kind::alias;
                if ((expected_alias
                     && declaration.kind
                         != structural_analysis::declaration_kind::alias)
                    || (!expected_alias
                        && declaration.kind
                            != structural_analysis::declaration_kind::variable)
                    || (!expected_alias
                        && (!declaration.specifiers.is_static
                            || !declaration.specifiers.is_constexpr
                            || !declaration.specifiers.has_explicit_type
                            || declaration.specifiers.has_deduced_type))) {
                    throw std::runtime_error(
                        probe.source_path.string()
                        + ": replacement changed the declaration kind/specifiers for binding provenance '"
                        + declaration.source_identity + "'");
                }
            } else if (const std::map<
                           std::string,
                           const owned_declaration*,
                           std::less<>>::const_iterator entry =
                           entries.find(declaration.source_identity);
                       entry != entries.end()) {
                if (declaration.kind
                        != structural_analysis::declaration_kind::function
                    || declaration.specifiers.is_definition
                    || declaration.linkage
                        == structural_analysis::entity_linkage::internal
                    || declaration.linkage
                        == structural_analysis::entity_linkage::none
                    || declaration.specifiers.is_template
                    || declaration.specifiers.is_overloaded
                    || declaration.specifiers.has_default_arguments
                    || declaration.specifiers.has_deduced_type) {
                    throw std::runtime_error(
                        probe.source_path.string()
                        + ": body-free entry probe failed module-safe declaration semantics for role '"
                        + entry->second->entry_role + "'");
                }
            }
        }
        if (!probe_identity_key.has_value()
            || !probed_identity_keys.insert(*probe_identity_key).second) {
            throw std::runtime_error(
                probe.source_path.string()
                + ": declaration probe has no unique identity-coordinate ownership");
        }
        for (const materialization_coordinate& coordinate :
             topology.coordinates) {
            if (detail::identity_key(coordinate) == *probe_identity_key
                && !probe_coordinates.contains(coordinate.ordinal)) {
                throw std::runtime_error(
                    probe.source_path.string()
                    + ": declaration probe omits logical coordinate "
                    + std::to_string(coordinate.ordinal));
            }
        }
    }
    if (actual_occurrences != expected_occurrences
        || probed_identity_keys.size()
            != topology.identity_representative_ordinals.size()) {
        throw std::runtime_error(
            "declaration-probe provenance does not cover the exact logical topology");
    }
}

/// Verifies the authored boundary and all controlled declarations.
inline void validate_materialization_region(
    const materialization_region& region,
    std::string_view source_text) {
    const std::filesystem::path& path = region.source_module.source_path;
    if (!region.source_module.is_interface || region.source_module.is_partition
        || !detail::module_name(region.source_module.provided_module)
        || region.source_module.provided_module
            == region.configured_output_module_name) {
        throw std::runtime_error(
            path.string()
            + ": template_flatten input must be a unique primary scan-only interface module");
    }
    if (region.boundary.qualified_name.empty() || region.boundary.is_inline
        || region.boundary.is_anonymous || region.boundary.is_reopened
        || region.boundary.is_nested_namespace_definition
        || region.boundary.has_explicit_export
        || region.boundary.is_in_export_context
        || region.boundary.qualified_name.contains("<anonymous>")) {
        detail::reject(
            path,
            region.boundary.declaration_range,
            "materialization boundary must be one named, non-exported, non-inline, non-reopened, non-shorthand namespace");
    }
    std::size_t output_primary_imports = 0u;
    for (const std::string& imported : region.source_module.imported_modules) {
        if (imported == region.configured_output_module_name) {
            ++output_primary_imports;
            continue;
        }
        if (imported.contains(':')) {
            throw std::runtime_error(
                path.string()
                + ": module-partition imports are forbidden in template_flatten");
        }
        if (!detail::module_name(imported)) {
            throw std::runtime_error(
                path.string() + ": invalid imported module '" + imported + "'");
        }
    }
    if (output_primary_imports != 1u) {
        throw std::runtime_error(
            path.string()
            + ": template_flatten input must import its configured output primary exactly once");
    }
    for (const binding_record& binding : region.bindings) {
        const structural_analysis::declaration_record& declaration =
            binding.declaration;
        const structural_analysis::declaration_specifier_state& facts =
            declaration.specifiers;
        if (!facts.direct_namespace_member
            || declaration.enclosing_qualified_name
                != region.boundary.qualified_name) {
            detail::reject(
                path,
                detail::declaration_range(declaration),
                "template_flatten binding must be a direct member of the materialization namespace");
        }
        if (declaration.kind == structural_analysis::declaration_kind::alias) {
            if (binding.role == axis_role::shard || !facts.is_using_alias
                || facts.is_template || facts.is_static
                || facts.is_constexpr || facts.is_inline || facts.is_extern
                || facts.is_thread_local || facts.multiple_declarators
                || declaration.exported) {
                detail::reject(
                    path,
                    detail::declaration_range(declaration),
                    binding.role == axis_role::shard
                        ? "a shard binding may not be an alias"
                        : "an identity alias binding must be exactly one unexported using-alias declaration");
            }
        } else if (declaration.kind
                   == structural_analysis::declaration_kind::variable) {
            if (!facts.is_static || !facts.is_constexpr || facts.is_inline
                || facts.is_extern || facts.is_thread_local
                || !facts.has_explicit_type || facts.has_deduced_type
                || facts.multiple_declarators || facts.is_template
                || declaration.exported) {
                detail::reject(
                    path,
                    detail::declaration_range(declaration),
                    "a value binding must be exactly one explicit-type static constexpr variable");
            }
        } else {
            detail::reject(
                path,
                detail::declaration_range(declaration),
                "template_flatten may annotate only an alias or static constexpr variable");
        }
        if (facts.range_contains_macro_expansion
            || !detail::nonempty_range(declaration.body_or_initializer_range)) {
            detail::reject(
                path,
                detail::declaration_range(declaration),
                "template_flatten binding requires a source-spelled RHS/initializer range");
        }
        const std::string default_spelling = declaration.initializer_text.empty()
            ? detail::slice(
                  source_text,
                  declaration.body_or_initializer_range,
                  "binding initializer")
            : declaration.initializer_text;
        if (detail::cpp_token_sequence(default_spelling)
            != detail::cpp_token_sequence(binding.cases.front().spelling)) {
            detail::reject(
                path,
                declaration.body_or_initializer_range,
                "authored RHS/initializer must token-normalize to the first replacement of group '"
                    + binding.group + "'");
        }
        const std::vector<const structural_analysis::attribute_record*> flatten =
            detail::attributes_named(declaration, detail::flatten_attribute);
        if (flatten.size() != 1u
            || detail::attribute_range(*flatten.front()).begin.byte_offset
                < declaration.name_range.end.byte_offset
            || detail::attribute_range(*flatten.front()).end.byte_offset
                > declaration.body_or_initializer_range.begin.byte_offset) {
            detail::reject(
                path,
                detail::declaration_range(declaration),
                "template_flatten attribute must occur exactly once after the declared name");
        }
    }

    std::map<std::string, axis_role, std::less<>> group_roles;
    std::map<std::string, std::set<std::string, std::less<>>, std::less<>>
        group_labels;
    std::set<std::string, std::less<>> binding_provenance;
    std::set<std::string, std::less<>> value_binding_provenance;
    std::size_t final_binding_end = 0u;
    for (const binding_record& binding : region.bindings) {
        group_roles.emplace(binding.group, binding.role);
        binding_provenance.insert(binding.declaration.source_identity);
        for (const replacement_case& replacement : binding.cases) {
            group_labels[binding.group].insert(replacement.label);
        }
        if (binding.declaration.kind
            == structural_analysis::declaration_kind::variable) {
            value_binding_provenance.insert(
                binding.declaration.source_identity);
        }
        final_binding_end = std::max(
            final_binding_end,
            detail::declaration_range(binding.declaration).end.byte_offset);
    }
    std::set<std::string, std::less<>> entry_names;
    std::set<std::string, std::less<>> controlled_noninternal_names;
    for (const owned_declaration& owned : region.controlled_declarations) {
        const structural_analysis::declaration_record& declaration =
            owned.declaration;
        if (!declaration.specifiers.direct_namespace_member
            || !detail::contains(
                region.boundary.body_range,
                detail::declaration_range(declaration))) {
            detail::reject(
                path,
                detail::declaration_range(declaration),
                "template_flatten_when may only select a direct namespace child");
        }
        if (detail::declaration_range(declaration).begin.byte_offset
            < final_binding_end) {
            detail::reject(
                path,
                detail::declaration_range(declaration),
                "all flatten bindings must precede declarations that use them");
        }
        for (const selection_clause& clause : owned.clauses) {
            const std::map<
                std::string,
                axis_role,
                std::less<>>::const_iterator role =
                group_roles.find(clause.group);
            if (role == group_roles.end()) {
                detail::reject(
                    path,
                    detail::declaration_range(declaration),
                    "template_flatten_when names unknown group '" + clause.group + "'");
            }
            for (const std::string& label : clause.accepted_case_labels) {
                if (!group_labels[clause.group].contains(label)) {
                    detail::reject(
                        path,
                        detail::declaration_range(declaration),
                        "template_flatten_when names unknown case '" + label
                            + "' of group '" + clause.group + "'");
                }
            }
        }
        const bool inside_support = region.support_island.has_value()
            && detail::contains(
                region.support_island->body_range,
                detail::declaration_range(declaration));
        if (!owned.generated_api_entry
            && declaration.specifiers.is_definition
            && declaration.linkage
                != structural_analysis::entity_linkage::internal
            && !inside_support
            && (declaration.specifiers.is_overloaded
                || declaration.qualified_name.empty()
                || !controlled_noninternal_names.insert(
                        declaration.qualified_name).second)) {
            detail::reject(
                path,
                detail::declaration_range(declaration),
                "non-TU-local controlled definitions must have unique non-overloaded names; move overloads to an authored module");
        }
        if (!owned.generated_api_entry) continue;
        const structural_analysis::declaration_specifier_state& facts =
            declaration.specifiers;
        if (declaration.enclosing_qualified_name
            != region.boundary.qualified_name) {
            detail::reject(
                path,
                detail::declaration_range(declaration),
                "template_flatten_entry must be a direct child of the materialization boundary");
        }
        if (declaration.kind != structural_analysis::declaration_kind::function
            || !facts.is_definition
            || facts.is_static || facts.is_inline || facts.is_constexpr
            || facts.is_consteval || facts.has_deduced_type
            || facts.has_default_arguments || facts.is_template
            || facts.is_overloaded || facts.has_c_language_linkage
            || facts.is_operator
            || facts.range_contains_macro_expansion
            || declaration.name.starts_with("operator")
            || declaration.return_type.empty()
            || !detail::nonempty_range(declaration.declaration_head_range)
            || !detail::nonempty_range(declaration.body_or_initializer_range)) {
            detail::reject(
                path,
                detail::declaration_range(declaration),
                "template_flatten_entry requires one non-template, non-overloaded, body-bearing function with an explicit return type");
        }
        if (declaration.linkage == structural_analysis::entity_linkage::internal
            || declaration.attachment
                != structural_analysis::module_attachment::named_module) {
            detail::reject(
                path,
                detail::declaration_range(declaration),
                "template_flatten_entry must have module-safe non-internal linkage and named-module attachment");
        }
        if (owned.clauses.empty()) {
            detail::reject(
                path,
                detail::declaration_range(declaration),
                "template_flatten_entry must have explicit shard ownership clauses");
        }
        if (!entry_names.insert(declaration.qualified_name).second) {
            detail::reject(
                path,
                detail::declaration_range(declaration),
                "template_flatten_entry overload/name is not unique");
        }
        for (const std::string& reference
             : declaration.referenced_declaration_source_identities) {
            if (value_binding_provenance.contains(reference)) {
                detail::reject(
                    path,
                    declaration.declaration_head_range,
                    "generated API entry declaration head references a TU-local value binding");
            }
            if (owned.entry_role == "root"
                && binding_provenance.contains(reference)) {
                detail::reject(
                    path,
                    declaration.declaration_head_range,
                    "root entry declaration head may not reference an identity-local binding because the selector is rendered in the parent namespace");
            }
        }
    }
}

/// The explicit-axis scheduler may consume the ordinary target-neutral
/// dispatch contract, but it must never become a Highway target source or
/// import the generated target graph. Exact token records keep this check
/// independent of comments and substrings in unrelated identifiers.
inline void validate_scheduler_highway_source_separation(
    const structural_analysis::translation_unit& unit,
    std::string_view output_module) {
    if (output_module != detail::scheduler_output_module) return;

    for (const std::string& imported : unit.source_module.imported_modules) {
        if (!detail::forbidden_scheduler_highway_provider(
                imported, output_module)) {
            continue;
        }
        throw std::runtime_error(
            unit.source_path.string()
            + ": scheduler materialization source imports forbidden Highway provider '"
            + imported + "'");
    }

    static constexpr std::array<std::string_view, 3u> forbidden_tokens{
        "HWY_TARGET",
        "HWY_EXPORT",
        "HWY_DYNAMIC_DISPATCH",
    };
    for (const structural_analysis::lexical_token& token : unit.tokens) {
        if (!std::ranges::contains(forbidden_tokens, token.spelling)) continue;
        detail::reject(
            unit.source_path,
            token.range,
            "scheduler materialization source contains forbidden Highway token '"
                + token.spelling + "'");
    }
    if (!unit.marked_namespaces.empty()) {
        detail::reject(
            unit.source_path,
            unit.marked_namespaces.front().marker.removal_range,
            "scheduler materialization source contains forbidden bioseq::simd namespace marker");
    }
}

/// Recheck the same boundary against the authenticated module identities from
/// PREPARE so an indirect generated provider cannot be laundered through an
/// otherwise neutral direct import.
inline void validate_scheduler_highway_prepared_closure(
    std::string_view output_module,
    const structural_analysis::dependency_compilation_validation& prepared) {
    if (output_module != detail::scheduler_output_module) return;

    for (const structural_analysis::dependency_module_identity& module
         : prepared.modules) {
        if (!detail::forbidden_scheduler_highway_provider(
                module.module_name, output_module)) {
            continue;
        }
        throw std::runtime_error(
            module.source_path.string()
            + ": authenticated scheduler PREPARE closure contains forbidden Highway provider '"
            + module.module_name + "'");
    }
}

/// Extracts and validates one materialization region from a token slice.
[[nodiscard]] inline materialization_region extract_materialization_region(
    const structural_analysis::translation_unit& unit,
    std::string configured_output_module_name,
    std::string_view source_text) {
    if (unit.mode != structural_analysis::collection_mode::token_slice) {
        throw std::runtime_error(
            unit.source_path.string()
            + ": namespace materialization requires the non-Sema token_slice collector");
    }
    validate_scheduler_highway_source_separation(
        unit, configured_output_module_name);
    materialization_region region{
        .source_module = unit.source_module,
        .configured_output_module_name =
            std::move(configured_output_module_name),
    };
    std::set<std::string, std::less<>> boundary_names;
    for (const structural_analysis::declaration_record& declaration
         : unit.declarations) {
        const std::vector<const structural_analysis::attribute_record*> flatten =
            detail::attributes_named(declaration, detail::flatten_attribute);
        if (flatten.empty()) continue;
        if (flatten.size() != 1u) {
            detail::reject(
                unit.source_path,
                detail::declaration_range(declaration),
                "a declaration may carry template_flatten exactly once");
        }
        if (!detail::attributes_named(
                 declaration, detail::when_attribute).empty()
            || !detail::attributes_named(
                    declaration, detail::entry_attribute).empty()) {
            detail::reject(
                unit.source_path,
                detail::declaration_range(declaration),
                "a template_flatten binding may not also carry a selection or entry marker");
        }
        boundary_names.insert(declaration.enclosing_qualified_name);
        try {
            region.bindings.push_back(
                parse_binding_attribute(declaration, *flatten.front()));
        } catch (const std::exception& error) {
            detail::reject(
                unit.source_path,
                detail::attribute_range(*flatten.front()),
                error.what());
        }
    }
    if (region.bindings.empty() || boundary_names.size() != 1u
        || boundary_names.begin()->empty()) {
        throw std::runtime_error(
            unit.source_path.string()
            + ": template_flatten requires exactly one materialization namespace");
    }
    std::vector<const structural_analysis::namespace_record*> boundaries;
    for (const structural_analysis::namespace_record& candidate : unit.namespaces) {
        if (candidate.qualified_name == *boundary_names.begin()) {
            boundaries.push_back(&candidate);
        }
    }
    if (boundaries.size() != 1u) {
        throw std::runtime_error(
            unit.source_path.string()
            + ": materialization namespace is missing or reopened");
    }
    region.boundary = *boundaries.front();

    for (const structural_analysis::lexical_token& token : unit.tokens) {
        if (token.spelling == "export"
            && detail::contains(region.boundary.body_range, token.range)) {
            detail::reject(
                unit.source_path,
                token.range,
                "export declarations and export blocks are forbidden inside a template_flatten region");
        }
    }

    for (const structural_analysis::namespace_record& candidate : unit.namespaces) {
        if (candidate.source_identity == region.boundary.source_identity
            || !detail::contains(
                region.boundary.body_range, candidate.declaration_range)) {
            continue;
        }
        if (!candidate.is_anonymous || region.support_island.has_value()) {
            detail::reject(
                unit.source_path,
                candidate.declaration_range,
                "template_flatten permits exactly one direct anonymous support namespace and no named nested namespace");
        }
        region.support_island = candidate;
    }

    for (const structural_analysis::preprocessor_directive_record& directive
         : unit.preprocessor_directives) {
        if (detail::intersects(
                region.boundary.body_range, directive.complete_range)) {
            detail::reject(
                unit.source_path,
                directive.complete_range,
                "preprocessor directives are forbidden inside a template_flatten region");
        }
    }

    for (const structural_analysis::declaration_record& declaration
         : unit.declarations) {
        const std::vector<const structural_analysis::attribute_record*> whens =
            detail::attributes_named(declaration, detail::when_attribute);
        const std::vector<const structural_analysis::attribute_record*> entries =
            detail::attributes_named(declaration, detail::entry_attribute);
        if (whens.empty() && entries.empty()) continue;
        if (entries.size() > 1u) {
            detail::reject(
                unit.source_path,
                detail::declaration_range(declaration),
                "a declaration may carry template_flatten_entry exactly once");
        }
        owned_declaration owned{
            .declaration = declaration,
            .generated_api_entry = !entries.empty(),
        };
        try {
            for (const structural_analysis::attribute_record* attribute : whens) {
                owned.clauses.push_back(parse_selection_attribute(*attribute));
            }
            if (!entries.empty()) {
                owned.entry_role = parse_entry_attribute(*entries.front());
            }
        } catch (const std::exception& error) {
            detail::reject(
                unit.source_path,
                detail::declaration_range(declaration),
                error.what());
        }
        region.controlled_declarations.push_back(std::move(owned));
    }

    std::set<std::string, std::less<>> policy_owned;
    for (const binding_record& binding : region.bindings) {
        policy_owned.insert(binding.declaration.source_identity);
    }
    for (const owned_declaration& declaration
         : region.controlled_declarations) {
        policy_owned.insert(declaration.declaration.source_identity);
        if (declaration.declaration.kind
            == structural_analysis::declaration_kind::namespace_declaration) {
            detail::reject(
                unit.source_path,
                detail::declaration_range(declaration.declaration),
                "selection markers may not own the support namespace itself");
        }
    }
    for (const structural_analysis::declaration_record& declaration
         : unit.declarations) {
        const structural_analysis::source_range& range =
            detail::declaration_range(declaration);
        if (!detail::contains(region.boundary.body_range, range)) {
            continue;
        }
        if (declaration.exported) {
            detail::reject(
                unit.source_path,
                range,
                "export declarations are forbidden inside a template_flatten materialization body");
        }
        if (policy_owned.contains(declaration.source_identity)) continue;
        if (region.support_island.has_value()
            && (range.begin.byte_offset
                    == region.support_island->declaration_range.begin.byte_offset
                || detail::contains(region.support_island->body_range, range))) {
            continue;
        }
        if (declaration.kind
            == structural_analysis::declaration_kind::namespace_declaration) {
            detail::reject(
                unit.source_path,
                range,
                "named or additional namespace definitions are forbidden in a template_flatten region");
        }
        if (!declaration.specifiers.is_definition) continue;
        const bool proven_internal = declaration.specifiers.is_static
            || declaration.linkage
                == structural_analysis::entity_linkage::internal;
        if (!proven_internal) {
            detail::reject(
                unit.source_path,
                range,
                "an unowned non-TU-local definition would be cloned across named-module units");
        }
    }
    validate_materialization_region(region, source_text);
    return region;
}

}  // namespace bioseq::codegen::namespace_materialization
