module;

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.codegen.sql.graph:fmgr;

import :compatibility;
import bioseq.codegen.sql.collector;
import bioseq.codegen.sql.signature;

export namespace bioseq::codegen::sql {

namespace fmgr_reconciliation_detail {

struct routine_identity final {
    std::string schema;
    std::string name;
    std::vector<std::string> argument_types;
};

[[nodiscard]] inline std::string_view trim(
        std::string_view value) noexcept {
    while (!value.empty()
           && std::isspace(
               static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty()
           && std::isspace(
               static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return value;
}

[[nodiscard]] inline std::string canonical_sql_type(
        std::string_view value) {
    value = trim(value);
    std::string result;
    result.reserve(value.size());
    bool quoted = false;
    for (const unsigned char character : value) {
        if (character == '"') {
            quoted = !quoted;
            result.push_back('"');
        } else if (!std::isspace(character)) {
            result.push_back(
                quoted
                ? static_cast<char>(character)
                : static_cast<char>(std::tolower(character)));
        }
    }
    if (result == "bool") return "boolean";
    if (result == "int2") return "smallint";
    if (result == "int4" || result == "int") return "integer";
    if (result == "int8") return "bigint";
    if (result == "float4") return "real";
    if (result == "float8") return "doubleprecision";
    if (result == "char" || result == "bpchar"
        || result == "charactervarying") {
        return result == "charactervarying"
            ? "varchar"
            : "character";
    }
    if (result == "quoted_any") return "\"any\"";
    return result;
}

[[nodiscard]] inline std::vector<std::string> split_arguments(
        std::string_view arguments) {
    std::vector<std::string> result;
    std::size_t begin = 0;
    int parentheses = 0;
    bool quoted = false;
    for (std::size_t index = 0; index <= arguments.size(); ++index) {
        const char character =
            index == arguments.size() ? ',' : arguments[index];
        if (character == '"') {
            quoted = !quoted;
        } else if (!quoted && character == '(') {
            ++parentheses;
        } else if (!quoted && character == ')') {
            --parentheses;
        } else if (!quoted && parentheses == 0
                   && character == ',') {
            const std::string_view argument =
                trim(arguments.substr(begin, index - begin));
            if (!argument.empty()) {
                result.push_back(canonical_sql_type(argument));
            }
            begin = index + 1;
        }
    }
    return result;
}

[[nodiscard]] inline routine_identity parse_routine_identity(
        std::string_view value) {
    value = trim(value);
    const std::size_t open = value.find('(');
    const std::size_t close = value.rfind(')');
    if (open == std::string_view::npos
        || close == std::string_view::npos || close < open
        || !trim(value.substr(close + 1)).empty()) {
        throw std::runtime_error(
            "typed install-owner routine identity is malformed: "
            + std::string{value});
    }
    const std::string_view qualified = trim(value.substr(0, open));
    const std::size_t separator = qualified.rfind('.');
    routine_identity result;
    if (separator == std::string_view::npos) {
        result.name = std::string{qualified};
    } else {
        result.schema = std::string{
            trim(qualified.substr(0, separator))};
        result.name = std::string{
            trim(qualified.substr(separator + 1))};
    }
    result.argument_types = split_arguments(
        value.substr(open + 1, close - open - 1));
    return result;
}

[[nodiscard]] inline std::optional<std::string>
install_routine_identity(const collected_entity& entity) {
    if (!entity.typed_install_owner
        || (entity.kind != entity_kind::function
            && entity.kind != entity_kind::procedure
            && entity.kind != entity_kind::sql_function)) {
        return std::nullopt;
    }
    const option_value* identity = find_option(entity, "sql_identity");
    if (identity == nullptr) return std::nullopt;
    return detail::decoded_cpp_literal(identity->expression);
}

[[nodiscard]] inline routine_identity natural_identity(
        const collected_entity& entity) {
    routine_identity result{
        .schema = entity.schema == "$extension"
            ? std::string{}
            : entity.schema,
        .name = entity.name,
    };
    for (const routine_parameter& parameter : entity.parameters) {
        if (parameter.injected_call_context) {
            continue;
        }
        result.argument_types.push_back(
            canonical_sql_type(parameter.sql_type));
    }
    return result;
}

[[nodiscard]] inline bool same_identity(
        const routine_identity& natural,
        const routine_identity& install) {
    return natural.name == install.name
        && natural.argument_types == install.argument_types
        && (natural.schema == install.schema
            || (natural.schema.empty() && install.schema.empty()));
}

[[nodiscard]] inline std::string option_text(
    const collected_entity& entity,
    std::string_view name,
    std::string_view fallback = {});

inline void validate_install_identity_declarations(
    const collected_entity& install,
    const routine_identity& identity,
    std::string_view identity_text) {
    const std::string qualified_name = identity.schema.empty()
        ? identity.name
        : identity.schema + "." + identity.name;
    const std::string derived_text =
        routine_identity_from_argument_declarations(
            qualified_name,
            option_text(
                install, "argument_declarations", "none"));
    const routine_identity derived =
        parse_routine_identity(derived_text);
    if (!same_identity(derived, identity)) {
        throw std::runtime_error(
            "typed routine install owner '" + std::string{identity_text}
            + "' has sql_identity inconsistent with "
              "argument_declarations; derived identity is '"
            + derived_text + "'");
    }
}

[[nodiscard]] inline std::string unquote_cpp_literal(
    std::string_view value) {
    value = trim(value);
    const std::size_t raw_prefix = value.find("R\"");
    if (raw_prefix != std::string_view::npos) {
        const std::size_t open = value.find('(', raw_prefix + 2);
        if (open != std::string_view::npos) {
            const std::string_view delimiter =
                value.substr(
                    raw_prefix + 2,
                    open - (raw_prefix + 2));
            const std::string close =
                ")" + std::string{delimiter} + "\"";
            const std::size_t close_offset =
                value.find(close, open + 1);
            if (close_offset != std::string_view::npos
                && close_offset >= open + 1) {
                return std::string{
                    value.substr(
                        open + 1, close_offset - open - 1)};
            }
        }
    }
    const std::size_t first_quote = value.find('"');
    const std::size_t last_quote = value.rfind('"');
    if (first_quote != std::string_view::npos
        && last_quote > first_quote) {
        return detail::decoded_cpp_literal(
            value.substr(
                first_quote, last_quote - first_quote + 1));
    }
    return std::string{value};
}

[[nodiscard]] inline std::string option_text(
        const collected_entity& entity,
        std::string_view name,
        std::string_view fallback) {
    const option_value* option = find_option(entity, name);
    return option == nullptr
        ? std::string{fallback}
        : unquote_cpp_literal(option->expression);
}

[[nodiscard]] inline std::string enum_leaf(
        std::string_view value) {
    value = trim(value);
    while (value.ends_with('}')) value.remove_suffix(1);
    const std::size_t separator = value.rfind("::");
    std::string result{
        separator == std::string_view::npos
            ? value
            : value.substr(separator + 2)};
    std::ranges::transform(
        result,
        result.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    if (result == "volatile_") result = "volatile";
    return result;
}

[[nodiscard]] inline std::string option_enum(
        const collected_entity& entity,
        std::string_view name,
        std::string_view fallback) {
    const option_value* option = find_option(entity, name);
    return option == nullptr
        ? std::string{fallback}
        : enum_leaf(option->expression);
}

[[nodiscard]] inline bool option_bool(
        const collected_entity& entity,
        std::string_view name,
        bool fallback = false) {
    const option_value* option = find_option(entity, name);
    if (option == nullptr) return fallback;
    const std::string value = enum_leaf(option->expression);
    if (value == "true") return true;
    if (value == "false") return false;
    throw std::runtime_error(
        "routine option ." + std::string{name}
        + " is not a closed Boolean");
}

[[nodiscard]] inline std::string canonical_sql_fragment(
        std::string_view value) {
    value = trim(value);
    std::string result;
    result.reserve(value.size());
    char quote = '\0';
    for (const unsigned char character : value) {
        if (quote == '\0'
            && (character == '\'' || character == '"')) {
            quote = static_cast<char>(character);
            result.push_back(static_cast<char>(character));
        } else if (quote != '\0' && character == quote) {
            result.push_back(static_cast<char>(character));
            quote = '\0';
        } else if (quote == '\0' && std::isspace(character)) {
            continue;
        } else {
            result.push_back(
                quote == '\0'
                ? static_cast<char>(std::tolower(character))
                : static_cast<char>(character));
        }
    }
    return result;
}

[[nodiscard]] inline bool sql_identifier_character(
        const unsigned char value) noexcept {
    return std::isalnum(value) || value == '_' || value == '$';
}

[[nodiscard]] inline bool sql_keyword_at(
        std::string_view sql,
        std::size_t begin,
        std::string_view keyword) noexcept {
    if (begin + keyword.size() > sql.size()
        || (begin != 0
            && sql_identifier_character(
                static_cast<unsigned char>(sql[begin - 1])))
        || (begin + keyword.size() != sql.size()
            && sql_identifier_character(static_cast<unsigned char>(
                sql[begin + keyword.size()])))) {
        return false;
    }
    for (std::size_t index = 0; index < keyword.size(); ++index) {
        if (std::tolower(
                static_cast<unsigned char>(sql[begin + index]))
            != std::tolower(
                static_cast<unsigned char>(keyword[index]))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool sql_escape_string_quote(
        std::string_view sql,
        std::size_t quote) noexcept {
    if (quote == 0
        || (sql[quote - 1] != 'e' && sql[quote - 1] != 'E')) {
        return false;
    }
    return quote == 1
        || !sql_identifier_character(
            static_cast<unsigned char>(sql[quote - 2]));
}

[[nodiscard]] inline std::size_t sql_quoted_token_end(
        std::string_view sql,
        std::size_t begin,
        std::string_view context) {
    if (begin >= sql.size()
        || (sql[begin] != '\'' && sql[begin] != '"')) {
        throw std::runtime_error(
            std::string{context} + " does not begin with a quoted token");
    }
    const char quote = sql[begin];
    const bool escape_string =
        quote == '\'' && sql_escape_string_quote(sql, begin);
    std::size_t cursor = begin + 1;
    while (cursor < sql.size()) {
        const char value = sql[cursor++];
        if (escape_string && value == '\\') {
            if (cursor >= sql.size()) {
                break;
            }
            ++cursor;
            continue;
        }
        if (value != quote) continue;
        if (cursor < sql.size() && sql[cursor] == quote) {
            ++cursor;
            continue;
        }
        return cursor;
    }
    throw std::runtime_error(
        std::string{context} + " contains an unterminated quoted token");
}

[[nodiscard]] inline std::size_t sql_block_comment_end(
        std::string_view sql,
        std::size_t begin,
        std::string_view context) {
    if (!sql.substr(begin).starts_with("/*")) {
        throw std::runtime_error(
            std::string{context} + " does not begin with a block comment");
    }
    int depth = 1;
    std::size_t cursor = begin + 2;
    while (cursor < sql.size() && depth != 0) {
        if (sql.substr(cursor).starts_with("/*")) {
            ++depth;
            cursor += 2;
        } else if (sql.substr(cursor).starts_with("*/")) {
            --depth;
            cursor += 2;
        } else {
            ++cursor;
        }
    }
    if (depth != 0) {
        throw std::runtime_error(
            std::string{context}
            + " contains an unterminated block comment");
    }
    return cursor;
}

[[nodiscard]] inline std::optional<std::string_view>
sql_dollar_delimiter(
        std::string_view sql,
        std::size_t begin) noexcept {
    if (begin >= sql.size() || sql[begin] != '$'
        || (begin != 0
            && sql_identifier_character(
                static_cast<unsigned char>(sql[begin - 1])))) {
        return std::nullopt;
    }
    std::size_t cursor = begin + 1;
    if (cursor >= sql.size()) return std::nullopt;
    if (sql[cursor] == '$') {
        return sql.substr(begin, 2);
    }
    const unsigned char first =
        static_cast<unsigned char>(sql[cursor]);
    if (!std::isalpha(first) && sql[cursor] != '_') {
        return std::nullopt;
    }
    while (++cursor < sql.size()) {
        const unsigned char value =
            static_cast<unsigned char>(sql[cursor]);
        if (sql[cursor] == '$') {
            return sql.substr(begin, cursor - begin + 1);
        }
        if (!std::isalnum(value) && sql[cursor] != '_') {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

[[nodiscard]] inline std::string typed_sql_function_body(
        const collected_entity& install,
        std::string_view identity) {
    const std::string sql =
        unquote_cpp_literal(install.initializer_expression);
    const std::string context =
        "typed SQL function install owner for '"
        + std::string{identity} + "'";
    int parentheses = 0;
    std::optional<std::size_t> as_offset;
    for (std::size_t cursor = 0; cursor < sql.size();) {
        if (sql.substr(cursor).starts_with("--")) {
            cursor = sql.find_first_of("\r\n", cursor + 2);
            if (cursor == std::string::npos) break;
            continue;
        }
        if (sql.substr(cursor).starts_with("/*")) {
            cursor = sql_block_comment_end(sql, cursor, context);
            continue;
        }
        if (sql[cursor] == '\'' || sql[cursor] == '"') {
            cursor = sql_quoted_token_end(
                sql, cursor, context);
            continue;
        }
        if (const std::optional<std::string_view> delimiter =
                sql_dollar_delimiter(sql, cursor)) {
            const std::size_t literal_begin =
                cursor + delimiter->size();
            const std::size_t literal_end =
                sql.find(*delimiter, literal_begin);
            if (literal_end == std::string::npos) {
                throw std::runtime_error(
                    context
                    + " contains an unterminated dollar-quoted token");
            }
            cursor = literal_end + delimiter->size();
            continue;
        }
        if (sql[cursor] == '(') {
            ++parentheses;
            ++cursor;
            continue;
        }
        if (sql[cursor] == ')') {
            if (parentheses == 0) {
                throw std::runtime_error(
                    context + " contains an unmatched closing parenthesis");
            }
            --parentheses;
            ++cursor;
            continue;
        }
        if (parentheses == 0 && sql_keyword_at(sql, cursor, "as")) {
            as_offset = cursor;
            break;
        }
        ++cursor;
    }
    if (!as_offset) {
        throw std::runtime_error(
            context + " has no top-level AS body clause");
    }

    std::size_t cursor = *as_offset + std::string_view{"as"}.size();
    while (cursor < sql.size()) {
        if (std::isspace(
                static_cast<unsigned char>(sql[cursor]))) {
            ++cursor;
            continue;
        }
        if (sql.substr(cursor).starts_with("--")) {
            cursor = sql.find_first_of("\r\n", cursor + 2);
            if (cursor == std::string::npos) break;
            continue;
        }
        if (sql.substr(cursor).starts_with("/*")) {
            cursor = sql_block_comment_end(sql, cursor, context);
            continue;
        }
        break;
    }
    const std::optional<std::string_view> delimiter =
        sql_dollar_delimiter(sql, cursor);
    if (!delimiter) {
        throw std::runtime_error(
            context
            + " does not use a dollar-quoted body after its AS clause");
    }
    const std::size_t body_begin = cursor + delimiter->size();
    const std::size_t body_end = sql.find(*delimiter, body_begin);
    if (body_end == std::string::npos) {
        throw std::runtime_error(
            context + " contains an unterminated dollar-quoted body");
    }
    return sql.substr(body_begin, body_end - body_begin);
}

[[nodiscard]] inline std::vector<std::string>
canonical_sql_body_tokens(std::string_view sql) {
    std::vector<std::string> result;
    std::string current;
    bool current_is_word = false;
    const auto flush = [&current, &result, &current_is_word] {
        if (!current.empty()) {
            result.push_back(std::move(current));
            current.clear();
        }
        current_is_word = false;
    };
    for (std::size_t cursor = 0; cursor < sql.size();) {
        const char value = sql[cursor];
        if (std::isspace(static_cast<unsigned char>(value))) {
            flush();
            ++cursor;
            continue;
        }
        if (sql.substr(cursor).starts_with("--")) {
            flush();
            cursor = sql.find_first_of("\r\n", cursor + 2);
            if (cursor == std::string_view::npos) break;
            continue;
        }
        if (sql.substr(cursor).starts_with("/*")) {
            flush();
            cursor = sql_block_comment_end(
                sql, cursor, "SQL function body");
            continue;
        }
        if (value == '\'' || value == '"') {
            flush();
            const std::size_t end = sql_quoted_token_end(
                sql, cursor, "SQL function body");
            result.emplace_back(sql.substr(cursor, end - cursor));
            cursor = end;
            continue;
        }
        if (const std::optional<std::string_view> delimiter =
                sql_dollar_delimiter(sql, cursor)) {
            flush();
            const std::size_t body_begin = cursor + delimiter->size();
            const std::size_t body_end =
                sql.find(*delimiter, body_begin);
            if (body_end == std::string_view::npos) {
                throw std::runtime_error(
                    "unterminated dollar-quoted token in SQL function body");
            }
            result.emplace_back(
                sql.substr(
                    cursor,
                    body_end + delimiter->size() - cursor));
            cursor = body_end + delimiter->size();
            continue;
        }
        if (value == '(' || value == ')' || value == ','
            || value == '.' || value == ';') {
            flush();
            result.emplace_back(1, value);
            ++cursor;
            continue;
        }
        const unsigned char code =
            static_cast<unsigned char>(value);
        const bool is_word =
            std::isalnum(code) || value == '_' || value == '$';
        if (!current.empty() && current_is_word != is_word) {
            flush();
        }
        if (current.empty()) current_is_word = is_word;
        current.push_back(static_cast<char>(std::tolower(code)));
        ++cursor;
    }
    flush();
    if (!result.empty() && result.back() == ";") {
        result.pop_back();
    }
    return result;
}

[[nodiscard]] inline std::string sql_token_fragment(
        const std::vector<std::string>& tokens) {
    std::string result;
    for (const std::string& token : tokens) {
        if (!result.empty()) result += ' ';
        result += token;
    }
    return result;
}

[[nodiscard]] inline std::string canonical_number(
        std::string_view value) {
    std::string result = canonical_sql_fragment(
        unquote_cpp_literal(value));
    if (result == "none") return result;
    const std::size_t exponent = result.find_first_of("eE");
    const std::size_t decimal = result.find('.');
    if (decimal != std::string::npos
        && (exponent == std::string::npos || decimal < exponent)) {
        const std::size_t fraction_end =
            exponent == std::string::npos ? result.size() : exponent;
        std::size_t keep = fraction_end;
        while (keep > decimal + 1 && result[keep - 1] == '0') {
            --keep;
        }
        if (keep == decimal + 1) --keep;
        result.erase(keep, fraction_end - keep);
    }
    return result;
}

[[nodiscard]] inline std::string canonical_default(
        std::string_view value) {
    value = trim(value);
    if (value == "std::nullopt" || value == "std::nullopt_t{}") {
        return "null";
    }
    if (value.contains("checked_sql")) {
        return canonical_sql_fragment(unquote_cpp_literal(value));
    }
    return canonical_sql_fragment(value);
}

[[nodiscard]] inline std::size_t find_keyword(
        std::string_view value,
        std::string_view keyword) {
    const auto identifier_character = [](const unsigned char character) {
        return std::isalnum(character)
            || character == '_' || character == '$';
    };
    int parentheses = 0;
    char quote = '\0';
    for (std::size_t index = 0; index + keyword.size() <= value.size();
         ++index) {
        const char character = value[index];
        if (quote == '\0'
            && (character == '\'' || character == '"')) {
            quote = character;
            continue;
        }
        if (quote != '\0') {
            if (character == quote) quote = '\0';
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
        if (parentheses != 0) continue;
        const bool left_boundary =
            index == 0
            || !identifier_character(
                static_cast<unsigned char>(value[index - 1]));
        const bool right_boundary =
            index + keyword.size() == value.size()
            || !identifier_character(static_cast<unsigned char>(
                value[index + keyword.size()]));
        if (!left_boundary || !right_boundary) continue;
        bool match = true;
        for (std::size_t offset = 0; offset < keyword.size(); ++offset) {
            if (std::tolower(
                    static_cast<unsigned char>(value[index + offset]))
                != std::tolower(
                    static_cast<unsigned char>(keyword[offset]))) {
                match = false;
                break;
            }
        }
        if (match) return index;
    }
    return std::string_view::npos;
}

[[nodiscard]] inline std::vector<std::string>
split_argument_declarations(std::string_view declarations) {
    declarations = trim(declarations);
    if (declarations.empty()
        || canonical_sql_fragment(declarations) == "none") {
        return {};
    }
    std::vector<std::string> result;
    std::size_t begin = 0;
    int parentheses = 0;
    char quote = '\0';
    for (std::size_t index = 0; index <= declarations.size(); ++index) {
        const char character =
            index == declarations.size() ? ',' : declarations[index];
        if (quote == '\0'
            && (character == '\'' || character == '"')) {
            quote = character;
        } else if (quote != '\0' && character == quote) {
            quote = '\0';
        } else if (quote == '\0' && character == '(') {
            ++parentheses;
        } else if (quote == '\0' && character == ')') {
            --parentheses;
        } else if (quote == '\0' && parentheses == 0
                   && character == ',') {
            const std::string_view declaration =
                trim(declarations.substr(begin, index - begin));
            if (!declaration.empty()) {
                result.emplace_back(declaration);
            }
            begin = index + 1;
        }
    }
    return result;
}

struct parameter_contract final {
    std::string name;
    std::string sql_type;
    std::string mode{"in"};
    std::string default_value;
    bool has_default{};
};

[[nodiscard]] inline parameter_contract parse_parameter_contract(
        std::string_view declaration,
        std::string_view identity_type) {
    parameter_contract result;
    declaration = trim(declaration);
    const std::size_t default_offset =
        find_keyword(declaration, "default");
    if (default_offset != std::string_view::npos) {
        result.has_default = true;
        result.default_value = canonical_default(
            declaration.substr(
                default_offset
                + std::string_view{"default"}.size()));
        declaration = trim(declaration.substr(0, default_offset));
    }

    const std::size_t first_space =
        declaration.find_first_of(" \t\r\n");
    const std::string first = enum_leaf(
        declaration.substr(0, first_space));
    if (first == "in" || first == "out" || first == "inout"
        || first == "variadic") {
        result.mode = first;
        declaration = first_space == std::string_view::npos
            ? std::string_view{}
            : trim(declaration.substr(first_space + 1));
    }

    const std::string expected_type =
        canonical_sql_type(identity_type);
    std::size_t type_begin = std::string_view::npos;
    for (std::size_t index = 0; index < declaration.size(); ++index) {
        if (index != 0
            && !std::isspace(
                static_cast<unsigned char>(declaration[index - 1]))) {
            continue;
        }
        if (canonical_sql_type(declaration.substr(index))
            == expected_type) {
            type_begin = index;
            break;
        }
    }
    if (type_begin == std::string_view::npos) {
        throw std::runtime_error(
            "typed routine argument declaration '"
            + std::string{declaration}
            + "' does not end in identity type '"
            + std::string{identity_type} + "'");
    }
    result.name = std::string{trim(declaration.substr(0, type_begin))};
    result.sql_type = expected_type;
    return result;
}

[[nodiscard]] inline std::string parameter_mode(
        std::string_view declaration) {
    declaration = trim(declaration);
    const std::size_t first_space =
        declaration.find_first_of(" \t\r\n");
    const std::string first = enum_leaf(
        declaration.substr(0, first_space));
    if (first == "in" || first == "out" || first == "inout"
        || first == "variadic") {
        return first;
    }
    return "in";
}

[[noreturn]] inline void contract_mismatch(
        std::string_view identity,
        std::string_view field,
        std::string_view install_value,
        std::string_view natural_value) {
    throw std::runtime_error(
        "natural C++ routine owner for '" + std::string{identity}
        + "' disagrees with its typed install owner for "
        + std::string{field} + ": install='"
        + std::string{install_value} + "', natural='"
        + std::string{natural_value} + "'");
}

inline void require_contract_value(
        std::string_view identity,
        std::string_view field,
        std::string_view install_value,
        std::string_view natural_value) {
    if (install_value != natural_value) {
        contract_mismatch(
            identity, field, install_value, natural_value);
    }
}

[[nodiscard]] inline const collected_result_row* result_row_for(
        const collected_entity& entity,
        const std::vector<collected_result_row>& result_rows) {
    if (entity.result.row_cpp_name.empty()) return nullptr;
    const collected_result_row* result = nullptr;
    for (const collected_result_row& candidate : result_rows) {
        if (candidate.cpp_qualified_name
            != entity.result.row_cpp_name) {
            continue;
        }
        if (result != nullptr) {
            throw std::runtime_error(
                "natural C++ routine owner '" + entity.cpp_qualified_name
                + "' resolves more than one result Row '"
                + entity.result.row_cpp_name + "'");
        }
        result = &candidate;
    }
    if (result == nullptr) {
        throw std::runtime_error(
            "natural C++ routine owner '" + entity.cpp_qualified_name
            + "' cannot resolve result Row '"
            + entity.result.row_cpp_name + "'");
    }
    return result;
}

inline void validate_returns_table_contract(
        std::string_view identity,
        std::string_view install_returns,
        const collected_result_row& row) {
    install_returns = trim(install_returns);
    const std::size_t open = install_returns.find('(');
    const std::size_t close = install_returns.rfind(')');
    if (open == std::string_view::npos
        || close == std::string_view::npos || close < open
        || canonical_sql_fragment(install_returns.substr(0, open))
            != "table"
        || !trim(install_returns.substr(close + 1)).empty()) {
        contract_mismatch(
            identity,
            "result form",
            canonical_sql_fragment(install_returns),
            "returns_table");
    }
    const std::vector<std::string> columns =
        split_argument_declarations(
            install_returns.substr(open + 1, close - open - 1));
    if (columns.size() != row.fields.size()) {
        contract_mismatch(
            identity,
            "result column count",
            std::to_string(columns.size()),
            std::to_string(row.fields.size()));
    }
    for (std::size_t index = 0; index < row.fields.size(); ++index) {
        const parameter_contract install_column =
            parse_parameter_contract(
                columns[index], row.fields[index].sql_type);
        require_contract_value(
            identity,
            "result column mode " + std::to_string(index),
            install_column.mode,
            "in");
        require_contract_value(
            identity,
            "result column name " + std::to_string(index),
            install_column.name,
            row.fields[index].name);
        require_contract_value(
            identity,
            "result column type " + std::to_string(index),
            install_column.sql_type,
            canonical_sql_type(row.fields[index].sql_type));
        require_contract_value(
            identity,
            "result column default presence "
                + std::to_string(index),
            install_column.has_default ? "true" : "false",
            "false");
    }
}

inline void validate_full_contract(
        const collected_entity& natural,
        const collected_entity& install,
        const routine_identity& install_identity,
        std::string_view identity_text,
        const std::vector<collected_result_row>& result_rows) {
    const auto implementation_kind =
        [](const entity_kind kind) -> std::string_view {
            if (kind == entity_kind::procedure) return "procedure";
            if (kind == entity_kind::sql_function) {
                return "sql_function";
            }
            return "function";
        };
    require_contract_value(
        identity_text,
        "routine kind",
        implementation_kind(install.kind),
        implementation_kind(natural.kind));

    const std::string language =
        option_enum(install, "language", "none");
    const bool natural_sql =
        natural.kind == entity_kind::sql_function;
    require_contract_value(
        identity_text,
        "language",
        language,
        natural_sql ? "sql" : "c");
    require_contract_value(
        identity_text,
        "C symbol",
        option_text(install, "c_symbol", "none"),
        natural_sql ? "none" : natural.name);

    const bool install_or_replace =
        option_bool(install, "create_or_replace");
    const std::string natural_create =
        option_enum(natural, "create", "create");
    const bool natural_or_replace = natural_create == "or_replace";
    if (natural_create != "create" && !natural_or_replace) {
        contract_mismatch(
            identity_text,
            "create mode",
            install_or_replace ? "or_replace" : "create",
            natural_create);
    }
    require_contract_value(
        identity_text,
        "create mode",
        install_or_replace ? "or_replace" : "create",
        natural_or_replace ? "or_replace" : "create");

    require_contract_value(
        identity_text,
        "volatility",
        option_enum(install, "volatility_value", "volatile"),
        option_enum(natural, "volatility", "volatile"));
    require_contract_value(
        identity_text,
        "parallel safety",
        option_enum(install, "parallel", "unsafe"),
        option_enum(natural, "parallel", "unsafe"));
    require_contract_value(
        identity_text,
        "security",
        option_enum(install, "security", "invoker"),
        option_enum(natural, "security", "invoker"));
    require_contract_value(
        identity_text,
        "leakproof",
        option_bool(install, "leakproof") ? "true" : "false",
        option_bool(natural, "leakproof") ? "true" : "false");

    const std::string install_cost =
        canonical_number(option_text(install, "cost", "none"));
    const bool natural_has_cost =
        option_bool(natural, "has_cost");
    const std::string natural_cost = natural_has_cost
        ? canonical_number(option_text(natural, "cost"))
        : "none";
    require_contract_value(
        identity_text, "cost", install_cost, natural_cost);

    const std::string install_rows =
        canonical_number(option_text(install, "rows", "none"));
    const bool natural_has_rows =
        option_bool(natural, "has_rows");
    const std::string natural_rows = natural_has_rows
        ? canonical_number(option_text(natural, "rows"))
        : "none";
    require_contract_value(
        identity_text, "rows", install_rows, natural_rows);

    require_contract_value(
        identity_text,
        "support function",
        canonical_sql_fragment(
            option_text(install, "support", "none")),
        canonical_sql_fragment(
            option_text(natural, "support", "none")));

    std::string natural_configuration{"none"};
    if (const option_value* configuration =
            find_option(natural, "configuration")) {
        natural_configuration = canonical_sql_fragment(
            unquote_cpp_literal(configuration->expression));
        if (natural_configuration.empty()) {
            natural_configuration = "none";
        }
    }
    require_contract_value(
        identity_text,
        "SET configuration",
        canonical_sql_fragment(
            option_text(install, "configuration", "none")),
        natural_configuration);

    const std::vector<const routine_parameter*> parameters =
        sql_parameters(natural);
    const std::vector<std::string> declarations =
        split_argument_declarations(
            option_text(install, "argument_declarations", "none"));
    if (parameters.size() != install_identity.argument_types.size()) {
        contract_mismatch(
            identity_text,
            "input parameter count",
            std::to_string(install_identity.argument_types.size()),
            std::to_string(parameters.size()));
    }
    const collected_result_row* result_row =
        result_row_for(natural, result_rows);
    const bool named_out_result =
        natural.result.form == routine_result_form::named_out
        || natural.result.form
            == routine_result_form::setof_named_out;
    std::size_t input_index = 0;
    std::size_t output_index = 0;
    for (const std::string& declaration : declarations) {
        const std::string mode = parameter_mode(declaration);
        if (mode == "out") {
            if (!named_out_result || result_row == nullptr
                || output_index >= result_row->fields.size()) {
                contract_mismatch(
                    identity_text,
                    "result OUT parameter count",
                    std::to_string(output_index + 1),
                    result_row == nullptr
                        ? "0"
                        : std::to_string(result_row->fields.size()));
            }
            const collected_result_field& field =
                result_row->fields[output_index];
            const parameter_contract expected =
                parse_parameter_contract(declaration, field.sql_type);
            require_contract_value(
                identity_text,
                "result OUT mode " + std::to_string(output_index),
                expected.mode,
                "out");
            require_contract_value(
                identity_text,
                "result OUT name " + std::to_string(output_index),
                expected.name,
                field.name);
            require_contract_value(
                identity_text,
                "result OUT type " + std::to_string(output_index),
                expected.sql_type,
                canonical_sql_type(field.sql_type));
            require_contract_value(
                identity_text,
                "result OUT default presence "
                    + std::to_string(output_index),
                expected.has_default ? "true" : "false",
                "false");
            ++output_index;
            continue;
        }
        if (mode != "in") {
            contract_mismatch(
                identity_text,
                "parameter mode " + std::to_string(input_index),
                mode,
                "in");
        }
        if (input_index >= parameters.size()
            || input_index >= install_identity.argument_types.size()) {
            contract_mismatch(
                identity_text,
                "input parameter count",
                std::to_string(input_index + 1),
                std::to_string(parameters.size()));
        }
        const parameter_contract expected =
            parse_parameter_contract(
                declaration,
                install_identity.argument_types[input_index]);
        const routine_parameter& actual = *parameters[input_index];
        require_contract_value(
            identity_text,
            "parameter mode " + std::to_string(input_index),
            expected.mode,
            "in");
        if (!expected.name.empty()) {
            require_contract_value(
                identity_text,
                "parameter name " + std::to_string(input_index),
                expected.name,
                actual.name);
        }
        require_contract_value(
            identity_text,
            "parameter type " + std::to_string(input_index),
            expected.sql_type,
            canonical_sql_type(actual.sql_type));
        require_contract_value(
            identity_text,
            "parameter default presence "
                + std::to_string(input_index),
            expected.has_default ? "true" : "false",
            actual.has_default ? "true" : "false");
        if (expected.has_default && actual.has_default) {
            require_contract_value(
                identity_text,
                "parameter default " + std::to_string(input_index),
                expected.default_value,
                canonical_default(actual.default_expression));
        }
        ++input_index;
    }
    if (input_index != parameters.size()) {
        contract_mismatch(
            identity_text,
            "input parameter count",
            std::to_string(input_index),
            std::to_string(parameters.size()));
    }
    if (named_out_result) {
        if (result_row == nullptr
            || output_index != result_row->fields.size()) {
            contract_mismatch(
                identity_text,
                "result OUT parameter count",
                std::to_string(output_index),
                result_row == nullptr
                    ? "0"
                    : std::to_string(result_row->fields.size()));
        }
    } else if (output_index != 0) {
        contract_mismatch(
            identity_text,
            "result OUT parameter count",
            std::to_string(output_index),
            "0");
    }

    const std::string install_null_input =
        option_enum(install, "null_input", "called");
    const bool natural_strict = !parameters.empty()
        && std::ranges::none_of(
            parameters,
            [](const routine_parameter* parameter) {
                return parameter->nullable;
            });
    const std::string natural_null_input =
        natural_strict ? "strict" : "called";
    require_contract_value(
        identity_text,
        "STRICT/CALLED semantics",
        install_null_input,
        natural_null_input);
    const bool expected_nullable = install_null_input == "called";
    for (std::size_t index = 0; index < parameters.size(); ++index) {
        require_contract_value(
            identity_text,
            "parameter nullability " + std::to_string(index),
            expected_nullable ? "nullable" : "non-nullable",
            parameters[index]->nullable
                ? "nullable"
                : "non-nullable");
    }

    const std::string install_result_text =
        option_text(install, "returns", "none");
    if (natural.result.form
        == routine_result_form::returns_table) {
        if (result_row == nullptr) {
            contract_mismatch(
                identity_text,
                "result Row",
                "none",
                natural.result.row_cpp_name);
        }
        validate_returns_table_contract(
            identity_text, install_result_text, *result_row);
    } else if (natural.result.form
               == routine_result_form::named_out) {
        require_contract_value(
            identity_text,
            "result form/type",
            canonical_sql_type(install_result_text),
            "record");
    } else if (natural.result.form
               == routine_result_form::setof_named_out) {
        require_contract_value(
            identity_text,
            "result form/type",
            canonical_sql_type(install_result_text),
            "setofrecord");
    } else {
        const std::string install_result =
            canonical_sql_type(install_result_text);
        const std::string natural_result =
            canonical_sql_type(natural.result_sql_type);
        require_contract_value(
            identity_text,
            "result form/type",
            install_result,
            natural_result);
    }

    if (natural_sql) {
        const std::vector<std::string> install_body =
            canonical_sql_body_tokens(
                typed_sql_function_body(install, identity_text));
        const std::vector<std::string> natural_body =
            canonical_sql_body_tokens(
                unquote_cpp_literal(
                    natural.initializer_expression));
        require_contract_value(
            identity_text,
            "SQL body",
            sql_token_fragment(install_body),
            sql_token_fragment(natural_body));
    }
}

}  // namespace fmgr_reconciliation_detail

/**
 * Replace a typed migration descriptor with the matching annotated C++ owner
 * while retaining the checked statement bytes and physical ordering.
 */
inline void reconcile_natural_routine_owners(
        collected_program& program) {
    using fmgr_reconciliation_detail::routine_identity;
    std::map<std::string, std::vector<std::size_t>, std::less<>>
        natural_by_leaf;
    for (std::size_t index = 0; index < program.entities.size(); ++index) {
        const collected_entity& entity = program.entities[index];
        if (!entity.typed_install_owner
            && (entity.kind == entity_kind::function
                || entity.kind == entity_kind::procedure
                || entity.kind == entity_kind::sql_function)) {
            natural_by_leaf[entity.name].push_back(index);
        }
    }

    std::set<std::size_t> merged_install_owners;
    std::set<std::size_t> matched_natural_owners;
    for (std::size_t install_index = 0;
         install_index < program.entities.size();
         ++install_index) {
        const collected_entity& install =
            program.entities[install_index];
        const std::optional<std::string> identity_text =
            fmgr_reconciliation_detail::install_routine_identity(
                install);
        if (!identity_text.has_value()) continue;
        const routine_identity install_identity =
            fmgr_reconciliation_detail::parse_routine_identity(
                *identity_text);
        fmgr_reconciliation_detail::
            validate_install_identity_declarations(
                install, install_identity, *identity_text);
        const auto candidates =
            natural_by_leaf.find(install_identity.name);
        if (candidates == natural_by_leaf.end()) continue;

        std::optional<std::size_t> match;
        for (const std::size_t natural_index : candidates->second) {
            const collected_entity& natural =
                program.entities[natural_index];
            if (!fmgr_reconciliation_detail::same_identity(
                    fmgr_reconciliation_detail::natural_identity(
                        natural),
                    install_identity)) {
                continue;
            }
            if (match.has_value()) {
                throw std::runtime_error(
                    "typed routine install owner '" + *identity_text
                    + "' matches more than one natural C++ owner");
            }
            match = natural_index;
        }
        if (!match.has_value()) continue;
        if (!matched_natural_owners.insert(*match).second) {
            throw std::runtime_error(
                "natural C++ routine owner for '" + *identity_text
                + "' matches more than one typed install descriptor");
        }
        fmgr_reconciliation_detail::validate_full_contract(
            program.entities[*match],
            install,
            install_identity,
            *identity_text,
            program.result_rows);
        adopt_typed_install_statement(
            program.entities[*match],
            install,
            physical_statement_facet::primary,
            physical_render_source::natural_projection);
        merged_install_owners.insert(install_index);
    }

    if (merged_install_owners.empty()) return;
    std::vector<collected_entity> reconciled;
    reconciled.reserve(
        program.entities.size() - merged_install_owners.size());
    for (std::size_t index = 0; index < program.entities.size(); ++index) {
        if (!merged_install_owners.contains(index)) {
            reconciled.push_back(std::move(program.entities[index]));
        }
    }
    program.entities = std::move(reconciled);
}

}  // namespace bioseq::codegen::sql
