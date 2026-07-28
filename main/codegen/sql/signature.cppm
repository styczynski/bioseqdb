module;

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

export module bioseq.codegen.sql.signature;

export namespace bioseq::codegen::sql {

enum class routine_argument_mode {
    in,
    out,
    inout,
    variadic,
};

struct parsed_routine_argument final {
    routine_argument_mode mode{routine_argument_mode::in};
    std::string name;
    std::string sql_type;
    std::string default_expression;
    bool has_default{};

    [[nodiscard]] bool input_bearing() const noexcept {
        return mode != routine_argument_mode::out;
    }

    [[nodiscard]] bool output_bearing() const noexcept {
        return mode == routine_argument_mode::out
            || mode == routine_argument_mode::inout;
    }
};

namespace signature_detail {

[[nodiscard]] inline bool ascii_space(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\n'
        || value == '\r' || value == '\f' || value == '\v';
}

[[nodiscard]] inline std::string_view trim(
        std::string_view value) noexcept {
    while (!value.empty() && ascii_space(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && ascii_space(value.back())) {
        value.remove_suffix(1);
    }
    return value;
}

[[nodiscard]] inline char lower(char value) noexcept {
    return value >= 'A' && value <= 'Z'
        ? static_cast<char>(value - 'A' + 'a')
        : value;
}

[[nodiscard]] inline std::string lower_copy(
        std::string_view value) {
    std::string result{value};
    std::ranges::transform(result, result.begin(), lower);
    return result;
}

[[nodiscard]] inline bool word_boundary(
        std::string_view text,
        std::size_t offset) noexcept {
    if (offset >= text.size()) return true;
    const unsigned char value =
        static_cast<unsigned char>(text[offset]);
    return !std::isalnum(value) && value != '_';
}

[[nodiscard]] inline std::size_t find_top_level_keyword(
        std::string_view value,
        std::string_view keyword) {
    int parentheses = 0;
    int brackets = 0;
    char quote = '\0';
    for (std::size_t index = 0;
         index + keyword.size() <= value.size();
         ++index) {
        const char character = value[index];
        if (quote != '\0') {
            if (character == quote) {
                if (index + 1 < value.size()
                    && value[index + 1] == quote) {
                    ++index;
                } else {
                    quote = '\0';
                }
            }
            continue;
        }
        if (character == '\'' || character == '"') {
            quote = character;
            continue;
        }
        if (character == '(') {
            ++parentheses;
            continue;
        }
        if (character == ')') {
            --parentheses;
            continue;
        }
        if (character == '[') {
            ++brackets;
            continue;
        }
        if (character == ']') {
            --brackets;
            continue;
        }
        if (parentheses != 0 || brackets != 0) continue;
        if (index != 0 && !word_boundary(value, index - 1)) {
            continue;
        }
        if (!word_boundary(value, index + keyword.size())) {
            continue;
        }
        bool matches = true;
        for (std::size_t offset = 0;
             offset < keyword.size();
             ++offset) {
            if (lower(value[index + offset])
                != lower(keyword[offset])) {
                matches = false;
                break;
            }
        }
        if (matches) return index;
    }
    return std::string_view::npos;
}

[[nodiscard]] inline std::size_t find_top_level_equals(
        std::string_view value) {
    int parentheses = 0;
    int brackets = 0;
    char quote = '\0';
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char character = value[index];
        if (quote != '\0') {
            if (character == quote) {
                if (index + 1 < value.size()
                    && value[index + 1] == quote) {
                    ++index;
                } else {
                    quote = '\0';
                }
            }
            continue;
        }
        if (character == '\'' || character == '"') {
            quote = character;
        } else if (character == '(') {
            ++parentheses;
        } else if (character == ')') {
            --parentheses;
        } else if (character == '[') {
            ++brackets;
        } else if (character == ']') {
            --brackets;
        } else if (character == '='
                   && parentheses == 0 && brackets == 0) {
            return index;
        }
    }
    return std::string_view::npos;
}

[[nodiscard]] inline std::vector<std::size_t>
top_level_word_begins(std::string_view value) {
    std::vector<std::size_t> result;
    int parentheses = 0;
    int brackets = 0;
    char quote = '\0';
    bool in_word = false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char character = value[index];
        if (quote != '\0') {
            if (character == quote) {
                if (index + 1 < value.size()
                    && value[index + 1] == quote) {
                    ++index;
                } else {
                    quote = '\0';
                }
            }
            continue;
        }
        if (character == '\'' || character == '"') {
            if (!in_word) {
                result.push_back(index);
                in_word = true;
            }
            quote = character;
            continue;
        }
        if (character == '(') ++parentheses;
        else if (character == ')') --parentheses;
        else if (character == '[') ++brackets;
        else if (character == ']') --brackets;

        if (parentheses == 0 && brackets == 0
            && ascii_space(character)) {
            in_word = false;
        } else if (!in_word) {
            result.push_back(index);
            in_word = true;
        }
    }
    return result;
}

[[nodiscard]] inline std::string normalized_type(
        std::string_view value) {
    value = trim(value);
    std::string result;
    result.reserve(value.size());
    bool pending_space = false;
    char quote = '\0';
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char character = value[index];
        if (quote != '\0') {
            result.push_back(character);
            if (character == quote) {
                if (index + 1 < value.size()
                    && value[index + 1] == quote) {
                    result.push_back(value[++index]);
                } else {
                    quote = '\0';
                }
            }
            continue;
        }
        if (character == '"') {
            if (pending_space && !result.empty()) {
                result.push_back(' ');
            }
            pending_space = false;
            quote = character;
            result.push_back(character);
        } else if (ascii_space(character)) {
            pending_space = true;
        } else {
            const bool punctuation =
                character == '.' || character == ','
                || character == '(' || character == ')'
                || character == '[' || character == ']';
            if (pending_space && !result.empty() && !punctuation
                && result.back() != '.' && result.back() != '('
                && result.back() != '[') {
                result.push_back(' ');
            }
            pending_space = false;
            if (punctuation && !result.empty()
                && result.back() == ' ') {
                result.pop_back();
            }
            result.push_back(lower(character));
        }
    }
    return result;
}

[[nodiscard]] inline std::string base_type_words(
        std::string_view type) {
    std::string value = normalized_type(type);
    while (value.ends_with("[]")) {
        value.erase(value.size() - 2);
    }
    std::string result;
    result.reserve(value.size());
    int parentheses = 0;
    char quote = '\0';
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char character = value[index];
        if (quote != '\0') {
            result.push_back(character);
            if (character == quote) quote = '\0';
        } else if (character == '"') {
            quote = character;
            result.push_back(character);
        } else if (character == '(') {
            ++parentheses;
        } else if (character == ')') {
            --parentheses;
        } else if (parentheses == 0) {
            result.push_back(character);
        }
    }
    return std::string{trim(result)};
}

[[nodiscard]] inline bool plausible_type(
        std::string_view type) {
    const std::string normalized = normalized_type(type);
    if (normalized.empty()) return false;
    const std::vector<std::size_t> words =
        top_level_word_begins(normalized);
    if (words.empty()) return false;
    if (words.size() == 1) return true;

    const std::string base = base_type_words(normalized);
    static constexpr std::string_view multiword_types[] = {
        "bit varying",
        "character varying",
        "double precision",
        "time with time zone",
        "time without time zone",
        "timestamp with time zone",
        "timestamp without time zone",
    };
    return std::ranges::contains(multiword_types, base);
}

[[nodiscard]] inline routine_argument_mode parse_mode(
        std::string_view& declaration) {
    const std::vector<std::size_t> words =
        top_level_word_begins(declaration);
    if (words.empty()) {
        throw std::runtime_error(
            "SQL routine argument declaration is empty");
    }
    std::size_t end = declaration.find_first_of(
        " \t\n\r\f\v", words.front());
    if (end == std::string_view::npos) end = declaration.size();
    const std::string first = lower_copy(
        declaration.substr(words.front(), end - words.front()));
    routine_argument_mode result = routine_argument_mode::in;
    bool explicit_mode = true;
    if (first == "in") result = routine_argument_mode::in;
    else if (first == "out") result = routine_argument_mode::out;
    else if (first == "inout") result = routine_argument_mode::inout;
    else if (first == "variadic") {
        result = routine_argument_mode::variadic;
    } else {
        explicit_mode = false;
    }
    if (explicit_mode) {
        declaration = trim(declaration.substr(end));
    }
    return result;
}

[[nodiscard]] inline parsed_routine_argument parse_argument(
        std::string_view declaration) {
    declaration = trim(declaration);
    parsed_routine_argument result;
    result.mode = parse_mode(declaration);

    std::size_t default_offset =
        find_top_level_keyword(declaration, "default");
    const std::size_t equals_offset =
        find_top_level_equals(declaration);
    if (equals_offset != std::string_view::npos
        && (default_offset == std::string_view::npos
            || equals_offset < default_offset)) {
        default_offset = equals_offset;
    }
    if (default_offset != std::string_view::npos) {
        const bool keyword =
            default_offset + 7 <= declaration.size()
            && lower_copy(declaration.substr(
                   default_offset, 7)) == "default";
        const std::size_t value_begin =
            default_offset + (keyword ? 7U : 1U);
        result.default_expression =
            std::string{trim(declaration.substr(value_begin))};
        if (result.default_expression.empty()) {
            throw std::runtime_error(
                "SQL routine argument DEFAULT expression is empty");
        }
        result.has_default = true;
        declaration = trim(declaration.substr(0, default_offset));
    }

    const std::vector<std::size_t> words =
        top_level_word_begins(declaration);
    if (words.empty()) {
        throw std::runtime_error(
            "SQL routine argument has no type");
    }
    std::size_t type_begin = std::string_view::npos;
    for (const std::size_t candidate : words) {
        if (plausible_type(declaration.substr(candidate))) {
            type_begin = candidate;
            break;
        }
    }
    if (type_begin == std::string_view::npos) {
        throw std::runtime_error(
            "cannot identify SQL routine argument type in '"
            + std::string{declaration} + "'");
    }
    result.name = std::string{
        trim(declaration.substr(0, type_begin))};
    if (!result.name.empty()
        && top_level_word_begins(result.name).size() != 1) {
        throw std::runtime_error(
            "SQL routine argument has an invalid name prefix '"
            + result.name + "'");
    }
    result.sql_type = normalized_type(
        declaration.substr(type_begin));
    return result;
}

}  // namespace signature_detail

[[nodiscard]] inline std::string normalize_routine_sql_type(
        std::string_view value) {
    return signature_detail::normalized_type(value);
}

[[nodiscard]] inline std::string canonical_routine_sql_type(
        std::string_view value) {
    std::string result =
        signature_detail::normalized_type(value);
    std::string compact;
    compact.reserve(result.size());
    char quote = '\0';
    for (const char character : result) {
        if (character == '"') {
            quote = quote == '\0' ? '"' : '\0';
            compact.push_back(character);
        } else if (quote == '\0'
                   && signature_detail::ascii_space(character)) {
            continue;
        } else {
            compact.push_back(character);
        }
    }
    if (compact == "bool") return "boolean";
    if (compact == "int2") return "smallint";
    if (compact == "int4" || compact == "int") return "integer";
    if (compact == "int8") return "bigint";
    if (compact == "float4") return "real";
    if (compact == "float8") return "doubleprecision";
    return compact;
}

[[nodiscard]] inline std::vector<parsed_routine_argument>
parse_routine_argument_declarations(
        std::string_view declarations) {
    declarations = signature_detail::trim(declarations);
    if (declarations.empty()
        || signature_detail::lower_copy(declarations) == "none") {
        return {};
    }
    std::vector<parsed_routine_argument> result;
    std::size_t begin = 0;
    int parentheses = 0;
    int brackets = 0;
    char quote = '\0';
    for (std::size_t index = 0;
         index <= declarations.size();
         ++index) {
        const bool at_end = index == declarations.size();
        const char character = at_end ? ',' : declarations[index];
        if (!at_end && quote != '\0') {
            if (character == quote) {
                if (index + 1 < declarations.size()
                    && declarations[index + 1] == quote) {
                    ++index;
                } else {
                    quote = '\0';
                }
            }
            continue;
        }
        if (!at_end && (character == '\'' || character == '"')) {
            quote = character;
        } else if (!at_end && character == '(') {
            ++parentheses;
        } else if (!at_end && character == ')') {
            --parentheses;
        } else if (!at_end && character == '[') {
            ++brackets;
        } else if (!at_end && character == ']') {
            --brackets;
        } else if (character == ',' && parentheses == 0
                   && brackets == 0) {
            const std::string_view declaration =
                signature_detail::trim(
                    declarations.substr(begin, index - begin));
            if (!declaration.empty()) {
                result.push_back(
                    signature_detail::parse_argument(declaration));
            }
            begin = index + 1;
        }
    }
    if (quote != '\0' || parentheses != 0 || brackets != 0) {
        throw std::runtime_error(
            "unbalanced SQL routine argument declarations");
    }
    return result;
}

/// Derives a catalog routine identity from a normalized routine name and its
/// authored argument declarations. PostgreSQL routine identity includes only
/// input-bearing SQL types: argument names, OUT-only arguments, modes, and
/// default expressions do not participate.
[[nodiscard]] inline std::string
routine_identity_from_argument_declarations(
        std::string_view routine_name,
        std::string_view declarations) {
    routine_name = signature_detail::trim(routine_name);
    if (routine_name.empty()) {
        throw std::runtime_error(
            "SQL routine identity requires a routine name");
    }
    std::string result{routine_name};
    result.push_back('(');
    bool first = true;
    for (const parsed_routine_argument& argument :
         parse_routine_argument_declarations(declarations)) {
        if (!argument.input_bearing()) continue;
        if (!first) result.push_back(',');
        result += argument.sql_type;
        first = false;
    }
    result.push_back(')');
    return result;
}

[[nodiscard]] inline std::vector<parsed_routine_argument>
parse_returns_table_columns(std::string_view result_type) {
    result_type = signature_detail::trim(result_type);
    const std::string lowered =
        signature_detail::lower_copy(result_type);
    if (!lowered.starts_with("table")) return {};
    if (result_type.size() > 5
        && !signature_detail::ascii_space(result_type[5])
        && result_type[5] != '(') {
        return {};
    }
    const std::size_t open = result_type.find('(');
    const std::size_t close = result_type.rfind(')');
    if (open == std::string_view::npos
        || close == std::string_view::npos || close <= open
        || !signature_detail::trim(
               result_type.substr(close + 1)).empty()) {
        throw std::runtime_error(
            "RETURNS TABLE declaration is malformed");
    }
    std::vector<parsed_routine_argument> result =
        parse_routine_argument_declarations(
            result_type.substr(open + 1, close - open - 1));
    for (parsed_routine_argument& column : result) {
        if (column.mode != routine_argument_mode::in
            || column.name.empty() || column.has_default) {
            throw std::runtime_error(
                "RETURNS TABLE columns require a name and type only");
        }
        column.mode = routine_argument_mode::out;
    }
    return result;
}

}  // namespace bioseq::codegen::sql
