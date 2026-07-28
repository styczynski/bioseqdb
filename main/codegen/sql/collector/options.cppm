module;

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <exception>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.codegen.sql.collector:options;

import :model;

export namespace bioseq::codegen::sql {

namespace detail {

[[nodiscard]] inline bool collector_ascii_space(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\n'
        || value == '\r' || value == '\f' || value == '\v';
}

[[nodiscard]] inline std::string_view collector_trim(
    std::string_view text) noexcept {
    while (!text.empty() && collector_ascii_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && collector_ascii_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] inline bool cpp_digit_separator(
    std::string_view text,
    std::size_t index) noexcept {
    if (index == 0 || index + 1 >= text.size()
        || text[index] != '\'') {
        return false;
    }
    return std::isalnum(
               static_cast<unsigned char>(text[index - 1]))
        && std::isalnum(
               static_cast<unsigned char>(text[index + 1]));
}

[[nodiscard]] inline bool option_boundary_after_comma(
    std::string_view text,
    std::size_t index) noexcept {
    while (index < text.size()
           && collector_ascii_space(text[index])) {
        ++index;
    }
    if (index == text.size()) return true;
    if (text[index] != '.') return false;
    ++index;
    if (index == text.size()
        || !(std::isalpha(
                 static_cast<unsigned char>(text[index]))
             || text[index] == '_')) {
        return false;
    }
    while (index < text.size()
           && (std::isalnum(
                   static_cast<unsigned char>(text[index]))
               || text[index] == '_')) {
        ++index;
    }
    while (index < text.size()
           && collector_ascii_space(text[index])) {
        ++index;
    }
    return index < text.size() && text[index] == '=';
}

[[nodiscard]] inline bool encloses_entire_expression(
    std::string_view text,
    char open,
    char close) {
    text = collector_trim(text);
    if (text.size() < 2 || text.front() != open || text.back() != close) {
        return false;
    }
    int depth = 0;
    bool quoted = false;
    char quote = '\0';
    bool escaped = false;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char value = text[index];
        if (quoted) {
            if (escaped) escaped = false;
            else if (value == '\\') escaped = true;
            else if (value == quote) quoted = false;
            continue;
        }
        if ((value == '\'' && !cpp_digit_separator(text, index))
            || value == '"') {
            quoted = true;
            quote = value;
        } else if (value == open) {
            ++depth;
        } else if (value == close) {
            --depth;
            if (depth == 0 && index + 1 != text.size()) return false;
        }
    }
    return depth == 0 && !quoted;
}

[[nodiscard]] inline std::vector<std::string> split_top_level(
    std::string_view text) {
    text = collector_trim(text);
    if (encloses_entire_expression(text, '{', '}')) {
        text.remove_prefix(1);
        text.remove_suffix(1);
    }
    std::vector<std::string> result;
    std::size_t begin = 0;
    int parentheses = 0;
    int braces = 0;
    int brackets = 0;
    int angles = 0;
    bool quoted = false;
    char quote = '\0';
    bool escaped = false;
    for (std::size_t index = 0; index <= text.size(); ++index) {
        const bool at_end = index == text.size();
        const char value = at_end ? '\0' : text[index];
        if (!at_end && quoted) {
            if (escaped) escaped = false;
            else if (value == '\\') escaped = true;
            else if (value == quote) quoted = false;
            continue;
        }
        if (!at_end
            && ((value == '\''
                 && !cpp_digit_separator(text, index))
                || value == '"')) {
            quoted = true;
            quote = value;
            continue;
        }
        if (!at_end) {
            if (value == '(') ++parentheses;
            else if (value == ')') --parentheses;
            else if (value == '{') ++braces;
            else if (value == '}') --braces;
            else if (value == '[') ++brackets;
            else if (value == ']') --brackets;
            else if (value == '<') ++angles;
            else if (value == '>' && angles > 0) --angles;
        }
        if (at_end
            || (value == ',' && parentheses == 0 && braces == 0
                && brackets == 0
                && (angles == 0
                    || option_boundary_after_comma(
                        text, index + 1)))) {
            const std::string_view item =
                collector_trim(text.substr(begin, index - begin));
            if (!item.empty()) result.emplace_back(item);
            begin = index + 1;
        }
    }
    if (quoted || parentheses != 0 || braces != 0 || brackets != 0) {
        throw std::runtime_error(
            "unbalanced SQL annotation option expression");
    }
    return result;
}

[[nodiscard]] inline std::size_t find_top_level_equals(
    std::string_view text) {
    int parentheses = 0;
    int braces = 0;
    int brackets = 0;
    int angles = 0;
    bool quoted = false;
    char quote = '\0';
    bool escaped = false;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char value = text[index];
        if (quoted) {
            if (escaped) escaped = false;
            else if (value == '\\') escaped = true;
            else if (value == quote) quoted = false;
            continue;
        }
        if ((value == '\''
             && !cpp_digit_separator(text, index))
            || value == '"') {
            quoted = true;
            quote = value;
        } else if (value == '(') ++parentheses;
        else if (value == ')') --parentheses;
        else if (value == '{') ++braces;
        else if (value == '}') --braces;
        else if (value == '[') ++brackets;
        else if (value == ']') --brackets;
        else if (value == '<') ++angles;
        else if (value == '>' && angles > 0) --angles;
        else if (value == '=' && parentheses == 0 && braces == 0
                 && brackets == 0 && angles == 0) {
            return index;
        }
    }
    return std::string_view::npos;
}

[[nodiscard]] inline std::string canonical_expression(
    std::string_view text) {
    text = collector_trim(text);
    std::string result;
    result.reserve(text.size());
    bool pending_space = false;
    bool quoted = false;
    char quote = '\0';
    bool escaped = false;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char value = text[index];
        if (quoted) {
            result.push_back(value);
            if (escaped) escaped = false;
            else if (value == '\\') escaped = true;
            else if (value == quote) quoted = false;
            continue;
        }
        if ((value == '\''
             && !cpp_digit_separator(text, index))
            || value == '"') {
            if (pending_space && !result.empty()) result.push_back(' ');
            pending_space = false;
            result.push_back(value);
            quoted = true;
            quote = value;
        } else if (collector_ascii_space(value)) {
            pending_space = true;
        } else {
            const bool punctuation = value == ',' || value == '{'
                || value == '}' || value == '(' || value == ')'
                || value == '=' || value == ':';
            if (pending_space && !result.empty() && !punctuation
                && result.back() != ':' && result.back() != '{'
                && result.back() != '(') {
                result.push_back(' ');
            }
            pending_space = false;
            if (punctuation && !result.empty() && result.back() == ' ') {
                result.pop_back();
            }
            result.push_back(value);
        }
    }
    return result;
}

[[nodiscard]] inline std::string unquoted_literal(std::string_view text) {
    text = collector_trim(text);
    if (text.size() >= 2
        && ((text.front() == '"' && text.back() == '"')
            || (text.front() == '\'' && text.back() == '\''))) {
        return std::string{text.substr(1, text.size() - 2)};
    }
    return std::string{text};
}

[[nodiscard]] inline std::string decoded_cpp_literal(
        std::string_view text) {
    text = collector_trim(text);
    const std::size_t raw_prefix = text.find("R\"");
    if (raw_prefix != std::string_view::npos) {
        const std::size_t open = text.find('(', raw_prefix + 2);
        if (open == std::string_view::npos) {
            throw std::runtime_error(
                "malformed C++ raw string literal");
        }
        const std::string_view delimiter =
            text.substr(raw_prefix + 2, open - raw_prefix - 2);
        const std::string close =
            ")" + std::string{delimiter} + "\"";
        const std::size_t close_offset = text.rfind(close);
        if (close_offset == std::string_view::npos
            || close_offset < open + 1) {
            throw std::runtime_error(
                "unterminated C++ raw string literal");
        }
        return std::string{
            text.substr(open + 1, close_offset - open - 1)};
    }
    if (text.size() < 2
        || (text.front() != '"' && text.front() != '\'')
        || text.back() != text.front()) {
        return std::string{text};
    }

    const char quote = text.front();
    std::string result;
    result.reserve(text.size() - 2);
    for (std::size_t index = 1; index + 1 < text.size(); ++index) {
        const char value = text[index];
        if (value != '\\') {
            result.push_back(value);
            continue;
        }
        ++index;
        if (index >= text.size() - 1) {
            throw std::runtime_error(
                "unterminated C++ escape sequence");
        }
        const char escaped = text[index];
        switch (escaped) {
            case '\'': result.push_back('\''); break;
            case '"': result.push_back('"'); break;
            case '?': result.push_back('?'); break;
            case '\\': result.push_back('\\'); break;
            case 'a': result.push_back('\a'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'v': result.push_back('\v'); break;
            case '\n': break;
            case '\r':
                if (index + 1 < text.size()
                    && text[index + 1] == '\n') {
                    ++index;
                }
                break;
            default:
                if (escaped < '0' || escaped > '7') {
                    throw std::runtime_error(
                        "unsupported C++ escape sequence");
                }
                unsigned int byte =
                    static_cast<unsigned int>(escaped - '0');
                for (int digit = 1;
                     digit < 3 && index + 1 < text.size() - 1
                     && text[index + 1] >= '0'
                     && text[index + 1] <= '7';
                     ++digit) {
                    byte = byte * 8U
                        + static_cast<unsigned int>(
                            text[++index] - '0');
                }
                if (byte > 0xffU) {
                    throw std::runtime_error(
                        "C++ octal escape is outside one byte");
                }
                result.push_back(static_cast<char>(byte));
                break;
        }
    }
    if (quote == '\'' && result.empty()) {
        throw std::runtime_error("empty C++ character literal");
    }
    return result;
}

[[nodiscard]] inline std::string leaf_identifier(std::string_view text) {
    text = collector_trim(text);
    const std::size_t template_open = text.find('<');
    if (template_open != std::string_view::npos
        && (text.substr(0, template_open).ends_with("fixed_schema")
            || text.substr(0, template_open).ends_with("custom_type"))) {
        const std::size_t quote = text.find('"', template_open);
        const std::size_t close = quote == std::string_view::npos
            ? std::string_view::npos
            : text.find('"', quote + 1);
        if (close != std::string_view::npos) {
            return std::string{text.substr(quote + 1, close - quote - 1)};
        }
    }
    while (text.ends_with('}')) text.remove_suffix(1);
    const std::size_t separator = text.rfind("::");
    return std::string{
        separator == std::string_view::npos
            ? text
            : text.substr(separator + 2)};
}

}  // namespace detail

[[nodiscard]] inline std::vector<option_value> parse_options(
    const clang_structural::attribute_record& attribute) {
    std::vector<option_value> result;
    if (detail::collector_trim(attribute.argument_text).empty()) {
        return result;
    }
    std::vector<std::string> items;
    try {
        items = detail::split_top_level(attribute.argument_text);
    } catch (const std::exception& error) {
        throw std::runtime_error(
            std::string{error.what()} + " in "
            + attribute.qualified_name + " with expression '"
            + attribute.argument_text + "'");
    }
    std::map<std::string, std::string, std::less<>> seen;
    for (const std::string& item : items) {
        const std::string_view view = detail::collector_trim(item);
        const std::size_t equals = detail::find_top_level_equals(view);
        std::string name = "value";
        std::string expression;
        if (view.starts_with('.') && equals != std::string_view::npos) {
            name = std::string{detail::collector_trim(
                view.substr(1, equals - 1))};
            expression = detail::canonical_expression(view.substr(equals + 1));
        } else {
            expression = detail::canonical_expression(view);
        }
        if (name.empty() || expression.empty()) {
            throw std::runtime_error(
                "empty SQL annotation option in "
                + attribute.qualified_name);
        }
        const auto [position, inserted] = seen.emplace(name, expression);
        if (!inserted) {
            throw std::runtime_error(
                "duplicate SQL annotation option '." + name + "' in "
                + attribute.qualified_name);
        }
        result.push_back(option_value{
            .name = std::move(name),
            .expression = std::move(expression),
            .range = attribute.range,
        });
    }
    if (attribute.name == "guc_value") {
        for (const option_value& option : result) {
            if (option.name != "aliases"
                || !option.expression.contains("guc_aliases")) {
                continue;
            }
            const std::size_t open = option.expression.find('{');
            const std::size_t close = option.expression.rfind('}');
            if (open == std::string::npos
                || close == std::string::npos
                || close <= open) {
                continue;
            }
            bool positional = false;
            bool designated = false;
            for (const std::string& alias_option :
                 detail::split_top_level(option.expression.substr(
                     open + 1, close - open - 1))) {
                if (detail::collector_trim(alias_option).starts_with('.')) {
                    designated = true;
                } else {
                    positional = true;
                }
            }
            if (positional && designated) {
                throw std::runtime_error(
                    "[BIOSEQ_SQL_E_MIXED_INITIALIZER] "
                    "guc_aliases cannot mix positional and designated "
                    "initializers");
            }
        }
    }
    return result;
}

[[nodiscard]] inline std::vector<dependency_reference> parse_dependencies(
    const clang_structural::attribute_record& attribute) {
    std::vector<dependency_reference> result;
    for (const std::string& expression :
         detail::split_top_level(attribute.argument_text)) {
        result.push_back(dependency_reference{
            .expression = detail::canonical_expression(expression),
            .range = attribute.range,
        });
    }
    if (result.empty()) {
        throw std::runtime_error(
            "bioseq_sql::depends requires at least one declaration reference");
    }
    return result;
}

[[nodiscard]] inline std::string derive_schema(
    const std::vector<option_value>& options) {
    for (const option_value& option : options) {
        if (option.name != "schema") continue;
        if (option.expression.contains("extension_schema")) {
            return "$extension";
        }
        if (option.expression.contains("pg_catalog")) {
            return "pg_catalog";
        }
        return detail::leaf_identifier(option.expression);
    }
    return "$extension";
}

[[nodiscard]] inline std::string option_literal(
    const collected_entity& entity,
    std::string_view name,
    std::string_view fallback = {}) {
    if (const option_value* option = find_option(entity, name)) {
        return detail::unquoted_literal(option->expression);
    }
    return std::string{fallback};
}

}  // namespace bioseq::codegen::sql
