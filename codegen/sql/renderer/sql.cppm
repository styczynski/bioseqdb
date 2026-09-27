module;

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

export module bioseq.codegen.sql.renderer:sql;

import :model;
import bioseq.codegen.structural_analysis;
import bioseq.codegen.sql.collector;
import bioseq.codegen.sql.graph;

export namespace bioseq::codegen::sql {

namespace detail {

[[nodiscard]] inline std::string qualified_sql_name(
    const normalized_entity& entity,
    const sql_graph& graph) {
    static_cast<void>(graph);
    if (entity.source.kind == entity_kind::schema
        || entity.source.kind == entity_kind::access_method
        || entity.source.kind == entity_kind::comment
        || entity.source.kind == entity_kind::guc
        || entity.source.kind == entity_kind::guc_value
        || entity.source.kind == entity_kind::phase) {
        return entity.source.name;
    }
    if (entity.source.schema == "$extension") {
        /*
         * PostgreSQL runs a relocatable extension script with the target
         * schema first in search_path.  Leaving extension-owned names
         * unqualified is therefore what makes CREATE EXTENSION ... SCHEMA
         * select their installation namespace.  graph.extension_schema is
         * retained for installed manifest identities; it is not SQL text.
         */
        return entity.source.name;
    }
    return entity.source.schema + "." + entity.source.name;
}

[[nodiscard]] inline std::string operator_reference_name(
    const normalized_entity& entity,
    const sql_graph& graph) {
    if (entity.source.schema == "$extension") {
        return entity.source.name;
    }
    return "OPERATOR(" + qualified_sql_name(entity, graph) + ")";
}

[[nodiscard]] inline std::string enum_leaf(
    std::string_view expression) {
    const std::size_t separator = expression.rfind("::");
    std::string result{
        separator == std::string_view::npos
            ? expression
            : expression.substr(separator + 2)};
    while (!result.empty()
           && (result.back() == '}' || result.back() == ')'
               || result.back() == ' ')) {
        result.pop_back();
    }
    return result;
}

[[nodiscard]] inline bool option_is(
    const collected_entity& entity,
    std::string_view name,
    std::string_view enum_value) {
    const option_value* option = find_option(entity, name);
    return option && enum_leaf(option->expression) == enum_value;
}

[[nodiscard]] inline std::string create_clause(
    const collected_entity& entity,
    bool supports_replace,
    bool supports_if_not_exists) {
    if (option_is(entity, "create", "or_replace")) {
        if (!supports_replace) {
            throw std::runtime_error(
                "CREATE OR REPLACE is not supported for generated "
                + std::string{kind_name(entity.kind)});
        }
        return "CREATE OR REPLACE ";
    }
    if (option_is(entity, "create", "if_not_exists")) {
        if (!supports_if_not_exists) {
            throw std::runtime_error(
                "IF NOT EXISTS is not supported for generated "
                + std::string{kind_name(entity.kind)});
        }
        return "CREATE ";
    }
    return "CREATE ";
}

[[nodiscard]] inline std::string if_not_exists(
    const collected_entity& entity) {
    return option_is(entity, "create", "if_not_exists")
        ? "IF NOT EXISTS "
        : "";
}

inline void trim_checked_sql_edge_newlines(std::string& value) {
    while (!value.empty() && value.front() == '\n') {
        value.erase(value.begin());
    }
    while (!value.empty() && value.back() == '\n') {
        value.pop_back();
    }
}

[[nodiscard]] inline int checked_sql_hex_digit(char value) noexcept {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

[[nodiscard]] inline bool starts_checked_sql_string_literal(
    std::string_view value) noexcept {
    while (!value.empty()
           && (value.front() == ' ' || value.front() == '\t'
               || value.front() == '\n' || value.front() == '\r')) {
        value.remove_prefix(1);
    }
    static constexpr auto prefixes = std::to_array<std::string_view>({
        "\"", "R\"", "u8\"", "u8R\"", "u\"", "uR\"",
        "U\"", "UR\"", "L\"", "LR\""});
    for (const std::string_view prefix : prefixes) {
        if (value.starts_with(prefix)) return true;
    }
    return false;
}

[[nodiscard]] inline std::string decode_checked_sql_string(
    std::string_view expression,
    std::size_t quote,
    bool trim_edge_newlines) {
    std::string result;
    std::size_t cursor = quote + 1;
    bool closed = false;
    while (cursor < expression.size()) {
        const char value = expression[cursor++];
        if (value == '"') {
            closed = true;
            break;
        }
        if (value != '\\') {
            result.push_back(value);
            continue;
        }
        if (cursor == expression.size()) {
            throw std::runtime_error(
                "unterminated checked_sql string escape");
        }
        const char escape = expression[cursor++];
        switch (escape) {
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
                if (cursor < expression.size()
                    && expression[cursor] == '\n') {
                    ++cursor;
                }
                break;
            case 'x': {
                unsigned int decoded = 0;
                std::size_t digits = 0;
                while (cursor < expression.size()) {
                    const int digit =
                        checked_sql_hex_digit(expression[cursor]);
                    if (digit < 0) break;
                    decoded = decoded * 16U
                        + static_cast<unsigned int>(digit);
                    ++cursor;
                    ++digits;
                    if (decoded > 0xffU) {
                        throw std::runtime_error(
                            "checked_sql hexadecimal escape exceeds "
                            "one byte");
                    }
                }
                if (digits == 0) {
                    throw std::runtime_error(
                        "checked_sql hexadecimal escape has no digits");
                }
                result.push_back(static_cast<char>(decoded));
                break;
            }
            default:
                if (escape >= '0' && escape <= '7') {
                    unsigned int decoded =
                        static_cast<unsigned int>(escape - '0');
                    std::size_t digits = 1;
                    while (digits < 3 && cursor < expression.size()
                           && expression[cursor] >= '0'
                           && expression[cursor] <= '7') {
                        decoded = decoded * 8U
                            + static_cast<unsigned int>(
                                expression[cursor] - '0');
                        ++cursor;
                        ++digits;
                    }
                    if (decoded > 0xffU) {
                        throw std::runtime_error(
                            "checked_sql octal escape exceeds one byte");
                    }
                    result.push_back(static_cast<char>(decoded));
                    break;
                }
                throw std::runtime_error(
                    "unsupported checked_sql string escape");
        }
    }
    if (!closed) {
        throw std::runtime_error(
            "unterminated checked_sql string literal");
    }
    if (starts_checked_sql_string_literal(expression.substr(cursor))) {
        throw std::runtime_error(
            "concatenated checked_sql string literals are unsupported");
    }
    if (trim_edge_newlines) {
        trim_checked_sql_edge_newlines(result);
    }
    return result;
}

[[nodiscard]] inline std::string extract_checked_sql(
    std::string_view expression,
    bool trim_edge_newlines = true) {
    std::size_t search_begin = 0;
    if (const std::size_t checked_sql =
            expression.find("checked_sql");
        checked_sql != std::string_view::npos) {
        search_begin =
            checked_sql + std::string_view{"checked_sql"}.size();
    }
    const std::size_t quote = expression.find('"', search_begin);
    const std::size_t raw =
        quote != std::string_view::npos && quote > search_begin
            && expression[quote - 1] == 'R'
        ? quote - 1
        : std::string_view::npos;
    if (raw != std::string_view::npos) {
        const std::size_t delimiter_end = expression.find('(', raw + 2);
        if (delimiter_end == std::string_view::npos) {
            throw std::runtime_error("malformed checked_sql raw string");
        }
        const std::string delimiter{expression.substr(
            raw + 2, delimiter_end - raw - 2)};
        const std::string close = ")" + delimiter + "\"";
        const std::size_t body_end =
            expression.find(close, delimiter_end + 1);
        if (body_end == std::string_view::npos
            || body_end < delimiter_end + 1) {
            throw std::runtime_error("unterminated checked_sql raw string");
        }
        std::string result{expression.substr(
            delimiter_end + 1, body_end - delimiter_end - 1)};
        if (starts_checked_sql_string_literal(
                expression.substr(body_end + close.size()))) {
            throw std::runtime_error(
                "concatenated checked_sql string literals are unsupported");
        }
        if (trim_edge_newlines) {
            trim_checked_sql_edge_newlines(result);
        }
        return result;
    }
    if (quote != std::string_view::npos) {
        return decode_checked_sql_string(
            expression, quote, trim_edge_newlines);
    }
    throw std::runtime_error(
        "checked SQL authority does not contain a string literal");
}

[[nodiscard]] inline bool sql_identifier_start(
    char value) noexcept {
    const unsigned char code = static_cast<unsigned char>(value);
    return value == '_' || std::isalpha(code) != 0;
}

[[nodiscard]] inline bool sql_identifier_continue(
    char value) noexcept {
    const unsigned char code = static_cast<unsigned char>(value);
    return value == '_' || std::isalnum(code) != 0;
}

[[nodiscard]] inline bool sql_token_identifier_continue(
    char value) noexcept {
    return value == '$'
        || sql_identifier_continue(value);
}

[[nodiscard]] inline std::optional<std::string_view>
sql_dollar_delimiter(
    std::string_view sql,
    std::size_t begin) noexcept {
    if (begin >= sql.size() || sql[begin] != '$'
        || begin + 1 >= sql.size()
        || (begin > 0
            && sql_token_identifier_continue(
                sql[begin - 1]))) {
        return std::nullopt;
    }
    if (sql[begin + 1] == '$') {
        return sql.substr(begin, 2);
    }
    if (!sql_identifier_start(sql[begin + 1])) {
        return std::nullopt;
    }
    std::size_t cursor = begin + 2;
    while (cursor < sql.size()
           && sql_identifier_continue(sql[cursor])) {
        ++cursor;
    }
    if (cursor >= sql.size() || sql[cursor] != '$') {
        return std::nullopt;
    }
    return sql.substr(begin, cursor - begin + 1);
}

[[nodiscard]] inline std::size_t top_level_statement_count(
    std::string_view sql) {
    std::size_t count = 0;
    std::size_t cursor = 0;
    std::size_t parentheses = 0;
    bool has_tokens = false;
    while (cursor < sql.size()) {
        if (sql.substr(cursor).starts_with("--")) {
            cursor = sql.find_first_of("\r\n", cursor + 2);
            if (cursor == std::string_view::npos) break;
            continue;
        }
        if (sql.substr(cursor).starts_with("/*")) {
            std::size_t depth = 1;
            cursor += 2;
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
                    "unterminated SQL block comment while counting "
                    "generated statements");
            }
            continue;
        }
        if (sql[cursor] == '\'') {
            has_tokens = true;
            const bool backslash_escapes =
                cursor > 0
                && (sql[cursor - 1] == 'E'
                    || sql[cursor - 1] == 'e');
            ++cursor;
            bool closed = false;
            while (cursor < sql.size()) {
                if (backslash_escapes && sql[cursor] == '\\'
                    && cursor + 1 < sql.size()) {
                    cursor += 2;
                    continue;
                }
                if (sql[cursor++] != '\'') continue;
                if (cursor < sql.size() && sql[cursor] == '\'') {
                    ++cursor;
                    continue;
                }
                closed = true;
                break;
            }
            if (!closed) {
                throw std::runtime_error(
                    "unterminated SQL string while counting generated "
                    "statements");
            }
            continue;
        }
        if (sql[cursor] == '"') {
            has_tokens = true;
            ++cursor;
            bool closed = false;
            while (cursor < sql.size()) {
                if (sql[cursor++] != '"') continue;
                if (cursor < sql.size() && sql[cursor] == '"') {
                    ++cursor;
                    continue;
                }
                closed = true;
                break;
            }
            if (!closed) {
                throw std::runtime_error(
                    "unterminated SQL identifier while counting generated "
                    "statements");
            }
            continue;
        }
        if (const std::optional<std::string_view> delimiter =
                sql_dollar_delimiter(sql, cursor)) {
            has_tokens = true;
            const std::size_t body_begin =
                cursor + delimiter->size();
            const std::size_t body_end =
                sql.find(*delimiter, body_begin);
            if (body_end == std::string_view::npos) {
                throw std::runtime_error(
                    "unterminated SQL dollar quote while counting "
                    "generated statements");
            }
            cursor = body_end + delimiter->size();
            continue;
        }
        const char value = sql[cursor++];
        if (value == '(') {
            ++parentheses;
            has_tokens = true;
        } else if (value == ')') {
            if (parentheses == 0) {
                throw std::runtime_error(
                    "unbalanced SQL parentheses while counting generated "
                    "statements");
            }
            --parentheses;
            has_tokens = true;
        } else if (value == ';' && parentheses == 0) {
            if (has_tokens) {
                ++count;
                has_tokens = false;
            }
        } else if (std::isspace(
                       static_cast<unsigned char>(value))
                   == 0) {
            has_tokens = true;
        }
    }
    if (parentheses != 0) {
        throw std::runtime_error(
            "unbalanced SQL parentheses while counting generated statements");
    }
    if (has_tokens) ++count;
    return count;
}

[[nodiscard]] inline std::string sql_default(
    const routine_parameter& parameter) {
    if (!parameter.has_default) return {};
    std::string expression = parameter.default_expression;
    if (expression == "std::nullopt") return "NULL";
    if (expression == "true") return "TRUE";
    if (expression == "false") return "FALSE";
    if (expression.starts_with("bioseq::sql::checked_sql")) {
        return extract_checked_sql(expression);
    }
    return expression;
}

[[nodiscard]] inline std::string column_initializer_sql(
    const collected_entity& column) {
    if (column.initializer_expression == "std::string{}"
        || column.initializer_expression == "std::string()") {
        return "text ''";
    }
    if (column.initializer_expression == "true") return "TRUE";
    if (column.initializer_expression == "false") return "FALSE";
    return column.initializer_expression;
}

[[nodiscard]] inline std::string render_sql_type(
    std::string_view sql_type) {
    return sql_type == "quoted_any" ? "\"any\"" : std::string{sql_type};
}

[[nodiscard]] inline std::string render_parameter_name(
    std::string_view name) {
    return name == "array" ? "\"array\"" : std::string{name};
}

[[nodiscard]] inline std::string render_parameters(
    const collected_entity& entity) {
    std::string result;
    bool saw_default = false;
    const std::vector<const routine_parameter*> parameters =
        sql_parameters(entity);
    for (std::size_t index = 0; index < parameters.size(); ++index) {
        const routine_parameter& parameter = *parameters[index];
        if (parameter.name.empty()) {
            throw std::runtime_error(
                "SQL routine input parameters must be named");
        }
        if (index != 0) result += ", ";
        result += render_parameter_name(parameter.name) + " "
            + render_sql_type(parameter.sql_type);
        if (parameter.has_default) {
            saw_default = true;
            result += " DEFAULT " + sql_default(parameter);
        } else if (saw_default) {
            throw std::runtime_error(
                "SQL routine parameter without a default follows one with a "
                "default in " + entity.name);
        }
    }
    return result;
}

[[nodiscard]] inline std::string routine_properties(
    const normalized_entity& normalized,
    const sql_graph& graph) {
    const collected_entity& entity = normalized.source;
    std::string result;
    if (option_is(entity, "volatility_value", "immutable")
        || option_is(entity, "volatility", "immutable")) {
        result += "\nIMMUTABLE";
    } else if (option_is(entity, "volatility_value", "stable")
               || option_is(entity, "volatility", "stable")) {
        result += "\nSTABLE";
    } else {
        result += "\nVOLATILE";
    }
    if (option_is(entity, "parallel", "safe")) {
        result += "\nPARALLEL SAFE";
    } else if (option_is(entity, "parallel", "restricted")) {
        result += "\nPARALLEL RESTRICTED";
    } else {
        result += "\nPARALLEL UNSAFE";
    }
    if (option_is(entity, "security", "definer")) {
        result += "\nSECURITY DEFINER";
    } else {
        result += "\nSECURITY INVOKER";
    }
    const option_value* null_input =
        find_option(entity, "null_input");
    bool strict = false;
    bool called = false;
    if (!null_input || option_is(entity, "null_input", "derived")) {
        const std::vector<const routine_parameter*> parameters =
            sql_parameters(entity);
        strict = !parameters.empty()
            && std::ranges::none_of(
                parameters,
                [](const routine_parameter* parameter) {
                    return parameter->nullable;
                });
    } else if (option_is(entity, "null_input", "strict")) {
        strict = true;
    } else if (option_is(entity, "null_input", "called")) {
        called = true;
    } else {
        throw std::runtime_error(
            "function null_input must be derived, strict, or called");
    }
    if (strict) result += "\nSTRICT";
    else if (called) result += "\nCALLED ON NULL INPUT";
    if (option_is(entity, "leakproof", "true")) {
        result += "\nLEAKPROOF";
    }
    if (const option_value* cost = find_option(entity, "cost");
        cost != nullptr && option_is(entity, "has_cost", "true")) {
        result += "\nCOST " + cost->expression;
    }
    if (const option_value* rows = find_option(entity, "rows");
        rows != nullptr && option_is(entity, "has_rows", "true")) {
        result += "\nROWS " + rows->expression;
    }
    if (const option_value* support = find_option(entity, "support");
        support != nullptr
        && option_literal(entity, "support", "none") != "none") {
        const normalized_entity* provider = nullptr;
        for (const normalized_entity& candidate : graph.entities) {
            if (candidate.stable_identity
                == normalized.planner_support_identity) {
                provider = &candidate;
                break;
            }
        }
        if (provider == nullptr
            || provider->source.kind != entity_kind::function) {
            throw std::runtime_error(
                "function '" + normalized.stable_identity
                + "' has no resolved planner support provider");
        }
        result += "\nSUPPORT "
            + qualified_sql_name(*provider, graph);
    }
    if (const option_value* configuration =
            find_option(entity, "configuration")) {
        result += "\n" + extract_checked_sql(
            configuration->expression, false);
    }
    return result;
}

[[nodiscard]] inline std::string remark_prefix(
    const collected_entity& entity) {
    std::string result;
    for (const std::string& remark : entity.documentation.remarks) {
        if (remark.empty()) {
            result += "--\n";
        } else {
            result += "-- " + remark + "\n";
        }
    }
    return result;
}

[[nodiscard]] inline const normalized_entity* referenced_routine(
    const normalized_entity& owner,
    const sql_graph& graph,
    std::string_view option_name,
    bool required = false);

[[nodiscard]] inline std::optional<std::string> comment_statement(
    const normalized_entity& entity,
    const sql_graph& graph) {
    if (!entity.source.documentation.has_explicit_brief
        || entity.source.documentation.brief.empty()) {
        return std::nullopt;
    }
    std::string object_kind;
    switch (entity.source.kind) {
        case entity_kind::schema: object_kind = "SCHEMA"; break;
        case entity_kind::sequence: object_kind = "SEQUENCE"; break;
        case entity_kind::table: object_kind = "TABLE"; break;
        case entity_kind::view: object_kind = "VIEW"; break;
        case entity_kind::function:
        case entity_kind::sql_function: object_kind = "FUNCTION"; break;
        case entity_kind::procedure: object_kind = "PROCEDURE"; break;
        case entity_kind::type: object_kind = "TYPE"; break;
        case entity_kind::aggregate: object_kind = "AGGREGATE"; break;
        case entity_kind::access_method: object_kind = "ACCESS METHOD"; break;
        default: return std::nullopt;
    }
    std::string target = qualified_sql_name(entity, graph);
    if (entity.source.kind == entity_kind::function
        || entity.source.kind == entity_kind::sql_function
        || entity.source.kind == entity_kind::procedure) {
        target += "(";
        const std::vector<const routine_parameter*> parameters =
            sql_parameters(entity.source);
        for (std::size_t index = 0; index < parameters.size(); ++index) {
            if (index != 0) target += ", ";
            target += render_sql_type(parameters[index]->sql_type);
        }
        target += ")";
    } else if (entity.source.kind == entity_kind::aggregate) {
        const normalized_entity* transition =
            referenced_routine(entity, graph, "transition", true);
        const std::vector<const routine_parameter*> parameters =
            sql_parameters(transition->source);
        if (parameters.empty()) {
            throw std::runtime_error(
                "aggregate comment target has no state parameter");
        }
        target += "(";
        if (parameters.size() == 1) {
            target += "*";
        } else {
            for (std::size_t index = 1; index < parameters.size();
                 ++index) {
                if (index != 1) target += ", ";
                target += render_sql_type(parameters[index]->sql_type);
            }
        }
        target += ")";
    }
    std::string escaped;
    for (const char value : entity.source.documentation.brief) {
        escaped += value == '\'' ? "''" : std::string(1, value);
    }
    return "COMMENT ON " + object_kind + " " + target
        + " IS '" + escaped + "';";
}

[[nodiscard]] inline const normalized_entity* find_entity(
    const sql_graph& graph,
    std::string_view identity) {
    for (const normalized_entity& entity : graph.entities) {
        if (entity.stable_identity == identity) return &entity;
    }
    return nullptr;
}

[[nodiscard]] inline std::string cpp_leaf(std::string_view name) {
    const std::size_t separator = name.rfind("::");
    return std::string{
        separator == std::string_view::npos
            ? name
            : name.substr(separator + 2)};
}

[[nodiscard]] inline bool expression_mentions_declaration(
    std::string_view expression,
    const collected_entity& declaration) {
    return std::ranges::any_of(
        declaration_reference_tokens(expression),
        [&declaration](std::string_view token) {
            return declaration_reference_matches(
                declaration.cpp_qualified_name, token);
        });
}

template<class p_predicate>
[[nodiscard]] inline const normalized_entity* referenced_entity(
    const normalized_entity& owner,
    const sql_graph& graph,
    std::string_view option_name,
    p_predicate predicate,
    bool required = false) {
    const option_value* option = find_option(owner.source, option_name);
    if (!option) {
        if (required) {
            throw std::runtime_error(
                std::string{kind_name(owner.source.kind)} + " '"
                + owner.source.name + "' requires ." + std::string{option_name});
        }
        return nullptr;
    }
    const normalized_entity* result = nullptr;
    for (const std::string& identity : owner.dependency_identities) {
        const normalized_entity* candidate = find_entity(graph, identity);
        if (!candidate || !predicate(candidate->source.kind)
            || !expression_mentions_declaration(
                option->expression, candidate->source)) {
            continue;
        }
        if (result
            && result->source.cpp_qualified_name
                != candidate->source.cpp_qualified_name) {
            throw std::runtime_error(
                "ambiguous declaration reference in ."
                + std::string{option_name} + " for "
                + owner.stable_identity);
        }
        result = candidate;
    }
    if (!result && required) {
        throw std::runtime_error(
            "unresolved declaration reference in ."
            + std::string{option_name} + " for "
            + owner.stable_identity);
    }
    return result;
}

[[nodiscard]] inline bool routine_entity_kind(entity_kind kind) noexcept {
    return kind == entity_kind::function
        || kind == entity_kind::sql_function
        || kind == entity_kind::procedure;
}

[[nodiscard]] inline const normalized_entity* referenced_routine(
    const normalized_entity& owner,
    const sql_graph& graph,
    std::string_view option_name,
    bool required) {
    return referenced_entity(
        owner,
        graph,
        option_name,
        [](entity_kind kind) { return routine_entity_kind(kind); },
        required);
}

[[nodiscard]] inline std::string quoted_sql_literal(
    std::string_view value) {
    std::string result{"'"};
    for (const char item : value) {
        result += item == '\'' ? "''" : std::string(1, item);
    }
    result += '\'';
    return result;
}

[[nodiscard]] inline std::string external_catalog_reference(
    std::string_view expression) {
    if (!expression.contains("pg_catalog")) return {};
    while (!expression.empty()
           && (expression.back() == '}' || expression.back() == ')'
               || expression.back() == ' ')) {
        expression.remove_suffix(1);
    }
    const std::size_t separator = expression.rfind("::");
    if (separator == std::string_view::npos) return {};
    return "pg_catalog."
        + std::string{expression.substr(separator + 2)};
}

[[nodiscard]] inline std::string routine_reference_sql(
    const normalized_entity& owner,
    const sql_graph& graph,
    std::string_view option_name,
    bool required = false) {
    if (const normalized_entity* routine =
            referenced_routine(owner, graph, option_name)) {
        return qualified_sql_name(*routine, graph);
    }
    if (const option_value* option =
            find_option(owner.source, option_name)) {
        const std::string external =
            external_catalog_reference(option->expression);
        if (!external.empty()) return external;
    }
    if (required) {
        throw std::runtime_error(
            "unresolved routine reference in ."
            + std::string{option_name} + " for "
            + owner.stable_identity);
    }
    return {};
}

struct type_role_binding {
    const normalized_entity* role{};
    const normalized_entity* function{};
    const normalized_entity* type{};
};

[[nodiscard]] inline std::vector<type_role_binding> type_role_bindings(
    const sql_graph& graph) {
    std::vector<type_role_binding> result;
    for (const normalized_entity& role : graph.entities) {
        if (role.source.kind != entity_kind::type_role) continue;
        const normalized_entity* function = nullptr;
        const normalized_entity* type = find_entity(
            graph, role.type_role_owner_identity);
        for (const std::string& identity : role.dependency_identities) {
            const normalized_entity* candidate = find_entity(graph, identity);
            if (!candidate) continue;
            if (candidate->source.kind == entity_kind::function
                && candidate->source.cpp_qualified_name
                    == role.source.cpp_qualified_name) {
                function = candidate;
            }
        }
        if (!function || !type
            || type->source.kind != entity_kind::type) {
            throw std::runtime_error(
                "type callback '" + role.source.name
                + "' must resolve one function and one owning type");
        }
        result.push_back({
            .role = &role,
            .function = function,
            .type = type,
        });
    }
    return result;
}

[[nodiscard]] inline std::vector<type_role_binding> type_roles_for(
    const normalized_entity& type,
    const sql_graph& graph) {
    std::vector<type_role_binding> result;
    for (const type_role_binding& binding : type_role_bindings(graph)) {
        if (binding.type == &type
            || binding.type->stable_identity == type.stable_identity) {
            result.push_back(binding);
        }
    }
    static constexpr auto order = std::to_array<std::string_view>({
        "type_input",
        "type_output",
        "type_receive",
        "type_send",
        "type_typmod_in",
        "type_typmod_out",
        "type_analyze",
        "type_subscript",
    });
    std::ranges::sort(
        result,
        [](const type_role_binding& left,
           const type_role_binding& right) {
            const auto rank = [](std::string_view name) {
                const auto found = std::ranges::find(order, name);
                return static_cast<std::size_t>(
                    std::ranges::distance(order.begin(), found));
            };
            return rank(left.role->source.annotation_name)
                < rank(right.role->source.annotation_name);
        });
    std::set<std::string, std::less<>> seen;
    for (const type_role_binding& binding : result) {
        if (!seen.insert(binding.role->source.annotation_name).second) {
            throw std::runtime_error(
                "base type '" + type.source.name
                + "' has duplicate "
                + binding.role->source.annotation_name + " callbacks");
        }
    }
    return result;
}

[[nodiscard]] inline bool function_is_type_callback(
    const normalized_entity& function,
    const sql_graph& graph) {
    return std::ranges::any_of(
        type_role_bindings(graph),
        [&function](const type_role_binding& binding) {
            return binding.function->stable_identity
                == function.stable_identity;
        });
}

[[nodiscard]] inline const collected_result_row& routine_result_row(
    const collected_entity& entity,
    const sql_graph& graph) {
    const collected_result_row* result = nullptr;
    for (const collected_result_row& candidate : graph.result_rows) {
        if (candidate.cpp_qualified_name
            != entity.result.row_cpp_name) {
            continue;
        }
        if (result != nullptr) {
            throw std::runtime_error(
                "SQL routine '" + entity.cpp_qualified_name
                + "' resolves duplicate result Row metadata for '"
                + entity.result.row_cpp_name + "'");
        }
        result = &candidate;
    }
    if (result == nullptr) {
        throw std::runtime_error(
            "SQL routine '" + entity.cpp_qualified_name
            + "' cannot resolve result Row metadata for '"
            + entity.result.row_cpp_name + "'");
    }
    return *result;
}

[[nodiscard]] inline std::string render_result_columns(
    const collected_result_row& row) {
    std::string result;
    for (std::size_t index = 0; index < row.fields.size(); ++index) {
        if (index != 0) result += ", ";
        result += row.fields[index].name + " "
            + row.fields[index].sql_type;
    }
    return result;
}

[[nodiscard]] inline std::string render_c_routine_parameters(
    const collected_entity& entity,
    const sql_graph& graph) {
    std::string result = render_parameters(entity);
    if (entity.result.form != routine_result_form::named_output
        && entity.result.form
            != routine_result_form::named_output_set) {
        return result;
    }
    const collected_result_row& row =
        routine_result_row(entity, graph);
    for (const collected_result_field& field : row.fields) {
        if (!result.empty()) result += ", ";
        result += "OUT " + field.name + " " + field.sql_type;
    }
    return result;
}

[[nodiscard]] inline std::string render_c_routine_result(
    const collected_entity& entity,
    const sql_graph& graph) {
    if (entity.result.form == routine_result_form::table_rows) {
        return "TABLE ("
            + render_result_columns(routine_result_row(entity, graph))
            + ")";
    }
    if (entity.result.form
        == routine_result_form::named_output_set) {
        return "SETOF record";
    }
    if (entity.result.form == routine_result_form::named_output) {
        return "record";
    }
    return entity.result_sql_type;
}

[[nodiscard]] inline std::string c_routine_statement(
    const normalized_entity& entity,
    const sql_graph& graph) {
    const collected_entity& source = entity.source;
    const std::string qualified = qualified_sql_name(entity, graph);
    const bool procedure = source.kind == entity_kind::procedure;
    std::string result = create_clause(source, true, false)
        + (procedure ? "PROCEDURE " : "FUNCTION ") + qualified + "("
        + render_c_routine_parameters(source, graph) + ")";
    if (!procedure) {
        result += "\nRETURNS "
            + render_c_routine_result(source, graph);
    }
    result += "\nAS 'MODULE_PATHNAME', '" + source.name
        + "'\nLANGUAGE C";
    if (procedure) {
        result += option_is(source, "security", "definer")
            ? "\nSECURITY DEFINER"
            : "\nSECURITY INVOKER";
    } else {
        result += routine_properties(entity, graph);
    }
    result += ";";
    return result;
}

[[nodiscard]] inline const normalized_entity* primary_parent(
    const normalized_entity& entity,
    const sql_graph& graph) {
    const normalized_entity* same_declaration = nullptr;
    const normalized_entity* enclosing_declaration = nullptr;
    std::size_t same_declaration_count = 0;
    std::size_t enclosing_declaration_count = 0;
    for (const std::string& identity : entity.dependency_identities) {
        const normalized_entity* dependency = find_entity(graph, identity);
        if (!dependency || dependency->source.companion) continue;
        const bool same = dependency->source.cpp_qualified_name
            == entity.source.cpp_qualified_name;
        const bool enclosing = !same
            && dependency->source.cpp_qualified_name
                == entity.source.enclosing_cpp_name;
        if (!same && !enclosing) continue;
        const normalized_entity*& result = same
            ? same_declaration
            : enclosing_declaration;
        std::size_t& count = same
            ? same_declaration_count
            : enclosing_declaration_count;
        result = dependency;
        ++count;
    }
    const bool enclosing_owned =
        entity.source.kind == entity_kind::column
        || entity.source.kind == entity_kind::primary_key
        || entity.source.kind == entity_kind::unique
        || entity.source.kind == entity_kind::foreign_key
        || entity.source.kind == entity_kind::check
        || entity.source.kind == entity_kind::type_role
        || entity.source.kind == entity_kind::guc_value;
    const bool use_enclosing =
        enclosing_owned && enclosing_declaration != nullptr;
    if ((use_enclosing
             ? enclosing_declaration_count
             : same_declaration_count) > 1) {
        throw std::runtime_error(
            "companion SQL entity '" + entity.stable_identity
            + "' has more than one natural parent");
    }
    return use_enclosing
        ? enclosing_declaration
        : same_declaration;
}

[[nodiscard]] inline std::string leaf_cpp_name(std::string_view name) {
    const std::size_t separator = name.rfind("::");
    return std::string{
        separator == std::string_view::npos
            ? name
            : name.substr(separator + 2)};
}

[[nodiscard]] inline std::vector<std::string> expression_identifiers(
    std::string_view expression) {
    std::vector<std::string> result;
    std::string current;
    const auto flush = [&current, &result] {
        if (current.empty()) return;
        static constexpr auto ignored = std::to_array<std::string_view>({
            "bioseq", "entities", "sql", "type_of"});
        if (!std::ranges::contains(ignored, current)) {
            result.push_back(leaf_cpp_name(current));
        }
        current.clear();
    };
    for (std::size_t index = 0; index < expression.size(); ++index) {
        const char value = expression[index];
        const bool identifier = (value >= 'a' && value <= 'z')
            || (value >= 'A' && value <= 'Z')
            || (value >= '0' && value <= '9') || value == '_';
        if (identifier) {
            current.push_back(value);
        } else if (value == ':' && index + 1 < expression.size()
                   && expression[index + 1] == ':') {
            current += "::";
            ++index;
        } else {
            flush();
        }
    }
    flush();
    return result;
}

[[nodiscard]] inline std::string column_list(
    const collected_entity& entity) {
    if (const option_value* columns = find_option(entity, "columns")) {
        const std::vector<std::string> names =
            expression_identifiers(columns->expression);
        std::string result;
        for (std::size_t index = 0; index < names.size(); ++index) {
            if (index != 0) result += ", ";
            result += names[index];
        }
        return result;
    }
    if (entity.declaration_kind
        == structural_analysis::declaration_kind::field) {
        return entity.name;
    }
    return {};
}

[[nodiscard]] inline std::string constraint_fragment(
    const normalized_entity& constraint) {
    const collected_entity& source = constraint.source;
    const std::string columns = column_list(source);
    const std::string prefix =
        find_option(source, "name")
        ? "CONSTRAINT " + source.name + " "
        : "";
    switch (source.kind) {
        case entity_kind::primary_key:
            if (columns.empty()) {
                throw std::runtime_error(
                    "primary_key '" + source.name
                    + "' requires typed .columns references");
            }
            return prefix + "PRIMARY KEY (" + columns + ")";
        case entity_kind::unique:
            if (columns.empty()) {
                throw std::runtime_error(
                    "unique constraint '" + source.name
                    + "' requires typed .columns references");
            }
            return prefix + "UNIQUE (" + columns + ")";
        case entity_kind::check: {
            const option_value* expression =
                find_option(source, "expression");
            if (!expression) {
                throw std::runtime_error(
                    "check constraint '" + source.name
                    + "' requires .expression");
            }
            return prefix + "CHECK ("
                + extract_checked_sql(expression->expression) + ")";
        }
        case entity_kind::foreign_key: {
            const option_value* references =
                find_option(source, "references");
            if (columns.empty() || !references) {
                throw std::runtime_error(
                    "foreign_key '" + source.name
                    + "' requires .columns and .references");
            }
            return prefix + "FOREIGN KEY (" + columns + ") REFERENCES "
                + references->expression;
        }
        default:
            throw std::runtime_error(
                "non-constraint passed to constraint renderer");
    }
}

[[nodiscard]] inline std::string privilege_list(
    const collected_entity& entity) {
    const option_value* privileges = find_option(entity, "privileges");
    if (!privileges) {
        return entity.kind == entity_kind::grant
            || entity.kind == entity_kind::revoke
            ? "EXECUTE"
            : "";
    }
    static constexpr auto values = std::to_array<
        std::pair<std::string_view, std::string_view>>({
        {"select", "SELECT"},
        {"insert", "INSERT"},
        {"update", "UPDATE"},
        {"delete_", "DELETE"},
        {"truncate", "TRUNCATE"},
        {"references", "REFERENCES"},
        {"trigger", "TRIGGER"},
        {"usage", "USAGE"},
        {"execute", "EXECUTE"},
        {"create", "CREATE"},
        {"all", "ALL"},
    });
    std::string result;
    for (const auto [token, spelling] : values) {
        if (privileges->expression.contains(token)) {
            if (!result.empty()) result += ", ";
            result += spelling;
        }
    }
    if (result.empty()) {
        throw std::runtime_error(
            "ACL privileges must use bioseq::sql::privilege values");
    }
    return result;
}

[[nodiscard]] inline std::string acl_object(
    const normalized_entity& parent,
    const sql_graph& graph) {
    std::string kind;
    switch (parent.source.kind) {
        case entity_kind::function:
        case entity_kind::sql_function: kind = "FUNCTION "; break;
        case entity_kind::procedure: kind = "PROCEDURE "; break;
        case entity_kind::schema: kind = "SCHEMA "; break;
        case entity_kind::sequence: kind = "SEQUENCE "; break;
        case entity_kind::table: kind = "TABLE "; break;
        case entity_kind::view: kind = "TABLE "; break;
        case entity_kind::type: kind = "TYPE "; break;
        default:
            throw std::runtime_error(
                "ACL annotation has an unsupported natural parent kind");
    }
    std::string result = kind + qualified_sql_name(parent, graph);
    if (parent.source.kind == entity_kind::function
        || parent.source.kind == entity_kind::sql_function
        || parent.source.kind == entity_kind::procedure) {
        result += "(";
        const std::vector<const routine_parameter*> parameters =
            sql_parameters(parent.source);
        for (std::size_t index = 0; index < parameters.size(); ++index) {
            if (index != 0) result += ", ";
            result += parameters[index]->sql_type;
        }
        result += ")";
    }
    return result;
}

[[nodiscard]] inline std::string type_role_sql_keyword(
    std::string_view annotation_name) {
    if (annotation_name == "type_input") return "INPUT";
    if (annotation_name == "type_output") return "OUTPUT";
    if (annotation_name == "type_receive") return "RECEIVE";
    if (annotation_name == "type_send") return "SEND";
    if (annotation_name == "type_typmod_in") return "TYPMOD_IN";
    if (annotation_name == "type_typmod_out") return "TYPMOD_OUT";
    if (annotation_name == "type_analyze") return "ANALYZE";
    if (annotation_name == "type_subscript") return "SUBSCRIPT";
    throw std::runtime_error(
        "unsupported base-type callback role '"
        + std::string{annotation_name} + "'");
}

struct natural_type_statement {
    std::string text;
};

[[nodiscard]] inline std::vector<natural_type_statement>
natural_type_statements(
    const normalized_entity& entity,
    const sql_graph& graph,
    const std::vector<const normalized_entity*>& children) {
    const collected_entity& source = entity.source;
    const std::string qualified = qualified_sql_name(entity, graph);
    const std::vector<type_role_binding> roles =
        type_roles_for(entity, graph);
    const option_value* internal_length =
        find_option(source, "internal_length");
    const option_value* variable_length =
        find_option(source, "variable_length");
    static constexpr auto base_option_names = std::to_array<std::string_view>({
        "internal_length",
        "variable_length",
        "passed_by_value",
        "alignment",
        "storage",
        "category",
        "preferred",
        "delimiter",
        "collatable",
        "element",
        "default_value",
    });
    const bool base_type = !roles.empty()
        || std::ranges::any_of(
            base_option_names,
            [&source](std::string_view name) {
                return find_option(source, name) != nullptr;
            });
    if (!base_type) {
        std::vector<const normalized_entity*> columns;
        for (const normalized_entity* child : children) {
            if (child->source.kind == entity_kind::column) {
                columns.push_back(child);
            }
        }
        std::ranges::sort(
            columns,
            {},
            [](const normalized_entity* column) {
                return column->source.source_range.begin.byte_offset;
            });
        if (columns.empty()) {
            return {{
                .text = "CREATE TYPE " + qualified + ";",
            }};
        }
        std::string result = "CREATE TYPE " + qualified + " AS (\n";
        for (std::size_t index = 0; index < columns.size(); ++index) {
            const collected_entity& column = columns[index]->source;
            if (index != 0) result += ",\n";
            std::string sql_type;
            if (!column.resolved_sql_type.empty()) {
                sql_type = column.resolved_sql_type;
            } else if (find_option(column, "sql_type")) {
                sql_type = option_literal(column, "sql_type", "");
                if (sql_type.contains("type_of<")) {
                    const std::size_t open = sql_type.find('<');
                    const std::size_t close = sql_type.rfind('>');
                    sql_type = cpp_leaf(
                        sql_type.substr(open + 1, close - open - 1));
                } else {
                    sql_type = enum_leaf(sql_type);
                }
            } else {
                sql_type =
                    infer_sql_type_from_cpp(column.cpp_return_type);
            }
            result += "    " + column.name + " " + sql_type;
            if (const option_value* collation =
                    find_option(column, "collation")) {
                result += " COLLATE "
                    + enum_leaf(collation->expression);
            }
        }
        return {{
            .text = result + "\n);",
        }};
    }
    const bool variable =
        variable_length && option_is(source, "variable_length", "true");
    if (variable_length && !variable) {
        throw std::runtime_error(
            "base type '" + source.name
            + "' requires .variable_length to be true when specified");
    }
    if (internal_length && variable) {
        throw std::runtime_error(
            "base type '" + source.name
            + "' cannot specify both .internal_length and .variable_length");
    }
    if (!internal_length && !variable) {
        throw std::runtime_error(
            "base type '" + source.name
            + "' requires .internal_length or .variable_length");
    }
    std::map<std::string, const type_role_binding*, std::less<>>
        role_by_name;
    for (const type_role_binding& role : roles) {
        role_by_name.emplace(role.role->source.annotation_name, &role);
    }
    if (!role_by_name.contains("type_input")
        || !role_by_name.contains("type_output")) {
        throw std::runtime_error(
            "base type '" + source.name
            + "' requires exactly one input and output callback");
    }

    std::string result = "CREATE TYPE " + qualified + " (\n";
    bool first = true;
    auto append = [&first, &result](std::string clause) {
        if (!first) result += ",\n";
        first = false;
        result += "    " + std::move(clause);
    };
    for (const type_role_binding& role : roles) {
        append(
            type_role_sql_keyword(role.role->source.annotation_name)
            + " = " + qualified_sql_name(*role.function, graph));
    }
    std::string length = variable
        ? std::string{"VARIABLE"}
        : internal_length->expression;
    if (length.contains("variable_length") || length == "-1") {
        length = "VARIABLE";
    }
    append("INTERNALLENGTH = " + length);
    if (option_is(source, "passed_by_value", "true")) {
        append("PASSEDBYVALUE");
    }
    if (const option_value* alignment = find_option(source, "alignment")) {
        std::string value = enum_leaf(alignment->expression);
        if (value == "character") value = "char";
        if (value == "double_") value = "double";
        append("ALIGNMENT = " + value);
    }
    if (const option_value* storage = find_option(source, "storage")) {
        append("STORAGE = " + enum_leaf(storage->expression));
    }
    if (const option_value* category = find_option(source, "category")) {
        append(
            "CATEGORY = "
            + quoted_sql_literal(
                option_literal(source, "category")));
    }
    if (option_is(source, "preferred", "true")) {
        append("PREFERRED = true");
    }
    if (const option_value* delimiter = find_option(source, "delimiter")) {
        static_cast<void>(delimiter);
        std::string value = option_literal(source, "delimiter");
        if (value.size() >= 2
            && ((value.front() == '\'' && value.back() == '\'')
                || (value.front() == '"' && value.back() == '"'))) {
            value = value.substr(1, value.size() - 2);
        }
        append("DELIMITER = " + quoted_sql_literal(value));
    }
    if (option_is(source, "collatable", "true")) {
        append("COLLATABLE = true");
    }
    for (const std::string_view option_name : {"element"}) {
        if (const option_value* value =
                find_option(source, option_name)) {
            const std::string& identity = entity.type_element_identity;
            std::string type_name;
            if (!identity.empty()) {
                const normalized_entity* referenced =
                    find_entity(graph, identity);
                if (!referenced
                    || referenced->source.kind != entity_kind::type) {
                    throw std::runtime_error(
                        "base type '" + source.name
                        + "' lost its resolved ."
                        + std::string{option_name} + " type");
                }
                type_name = qualified_sql_name(*referenced, graph);
            } else if (value->expression.contains("custom_type<")) {
                type_name = leaf_identifier(value->expression);
            } else if (value->expression.contains("type_of<")) {
                throw std::runtime_error(
                    "base type '" + source.name
                    + "' has an unresolved ."
                    + std::string{option_name} + " type");
            } else {
                type_name = enum_leaf(value->expression);
            }
            append(
                std::string{"ELEMENT = "}
                + type_name);
        }
    }
    if (const option_value* default_value =
            find_option(source, "default_value")) {
        append(
            "DEFAULT = "
            + quoted_sql_literal(
                extract_checked_sql(default_value->expression)));
    }
    result += "\n);";
    return {
        natural_type_statement{
            .text = "CREATE TYPE " + qualified + ";",
        },
        natural_type_statement{
            .text = std::move(result),
        },
    };
}

[[nodiscard]] inline std::string aggregate_initial_condition_sql(
    const option_value& option) {
    const std::string_view expression = option.expression;
    if (expression.contains("pg_infinity::negative")) {
        return quoted_sql_literal("-infinity");
    }
    if (expression.contains("pg_infinity::positive")) {
        return quoted_sql_literal("infinity");
    }
    if (const std::size_t input =
            expression.find("pg_input_text");
        input != std::string_view::npos) {
        const std::size_t quote = expression.find('"', input);
        const std::size_t close = quote == std::string_view::npos
            ? std::string_view::npos
            : expression.find('"', quote + 1);
        if (close == std::string_view::npos) {
            throw std::runtime_error(
                "aggregate pg_input_text has no string literal");
        }
        return quoted_sql_literal(
            expression.substr(quote + 1, close - quote - 1));
    }
    std::string_view value = expression;
    if (const std::size_t open = value.find('{');
        open != std::string_view::npos) {
        const std::size_t close = value.rfind('}');
        if (close == std::string_view::npos || close <= open) {
            throw std::runtime_error(
                "aggregate integer initial condition has malformed braces");
        }
        value = value.substr(open + 1, close - open - 1);
    }
    std::string compact;
    for (const char character : value) {
        if ((character >= '0' && character <= '9')
            || character == '+' || character == '-') {
            compact.push_back(character);
        } else if (!std::isspace(
                       static_cast<unsigned char>(character))
                   && character != 'l' && character != 'L'
                   && character != 'u' && character != 'U') {
            throw std::runtime_error(
                "aggregate integer initial condition contains an "
                "unsupported expression");
        }
    }
    if (compact.empty()) {
        throw std::runtime_error(
            "aggregate initial condition is not a closed supported value");
    }
    std::size_t cursor =
        compact.front() == '-' || compact.front() == '+' ? 1 : 0;
    while (cursor < compact.size()
           && compact[cursor] >= '0' && compact[cursor] <= '9') {
        ++cursor;
    }
    std::int64_t result{};
    const auto [first_end, first_error] = std::from_chars(
        compact.data(), compact.data() + cursor, result);
    if (first_error != std::errc{}
        || first_end != compact.data() + cursor) {
        throw std::runtime_error(
            "aggregate integer initial condition is outside int64");
    }
    while (cursor < compact.size()) {
        const char operation = compact[cursor++];
        const std::size_t begin = cursor;
        while (cursor < compact.size()
               && compact[cursor] >= '0'
               && compact[cursor] <= '9') {
            ++cursor;
        }
        std::int64_t operand{};
        const auto [end, error] = std::from_chars(
            compact.data() + begin, compact.data() + cursor, operand);
        if (begin == cursor || error != std::errc{}
            || end != compact.data() + cursor) {
            throw std::runtime_error(
                "aggregate integer initial condition is malformed");
        }
        if (operation == '+') {
            if (result
                > std::numeric_limits<std::int64_t>::max() - operand) {
                throw std::runtime_error(
                    "aggregate integer initial condition overflows int64");
            }
            result += operand;
        } else {
            if (result
                < std::numeric_limits<std::int64_t>::min() + operand) {
                throw std::runtime_error(
                    "aggregate integer initial condition overflows int64");
            }
            result -= operand;
        }
    }
    return quoted_sql_literal(std::to_string(result));
}

[[nodiscard]] inline std::string aggregate_modify_sql(
    const collected_entity& source,
    std::string_view option_name) {
    if (option_is(source, option_name, "shareable")) return "SHAREABLE";
    if (option_is(source, option_name, "read_write")) return "READ_WRITE";
    return "READ_ONLY";
}

[[nodiscard]] inline std::string aggregate_statement(
    const normalized_entity& entity,
    const sql_graph& graph) {
    const collected_entity& source = entity.source;
    const normalized_entity* transition =
        referenced_routine(entity, graph, "transition", true);
    const std::vector<const routine_parameter*> transition_parameters =
        transition == nullptr
        ? std::vector<const routine_parameter*>{}
        : sql_parameters(transition->source);
    if (!transition
        || transition->source.kind != entity_kind::function
        || transition_parameters.empty()
        || transition_parameters.front()->sql_type
            != transition->source.result_sql_type) {
        throw std::runtime_error(
            "aggregate '" + source.name
            + "' transition function must return its first state input type");
    }
    std::string arguments;
    for (std::size_t index = 1;
         index < transition_parameters.size();
         ++index) {
        if (!arguments.empty()) arguments += ", ";
        arguments += render_sql_type(
            transition_parameters[index]->sql_type);
    }
    if (arguments.empty()) arguments = "*";

    std::vector<std::string> clauses;
    clauses.push_back(
        "SFUNC = " + qualified_sql_name(*transition, graph));
    clauses.push_back(
        "STYPE = "
        + render_sql_type(transition_parameters.front()->sql_type));
    const auto add_routine =
        [&clauses, &entity, &graph](std::string_view option_name,
            std::string_view sql_name) {
            const std::string reference = routine_reference_sql(
                entity, graph, option_name);
            if (!reference.empty()) {
                clauses.push_back(
                    std::string{sql_name} + " = " + reference);
            }
        };
    add_routine("final", "FINALFUNC");
    if (const option_value* initial =
            find_option(source, "initial_condition")) {
        clauses.push_back(
            "INITCOND = "
            + aggregate_initial_condition_sql(*initial));
    }
    add_routine("serial", "SERIALFUNC");
    add_routine("deserial", "DESERIALFUNC");
    if (const option_value* state_space =
            find_option(source, "state_space")) {
        clauses.push_back("SSPACE = " + state_space->expression);
    }
    add_routine("combine", "COMBINEFUNC");
    if (option_is(source, "final_function_extra", "true")) {
        clauses.push_back("FINALFUNC_EXTRA");
    }
    if (find_option(source, "final_function_modify")) {
        clauses.push_back(
            "FINALFUNC_MODIFY = "
            + aggregate_modify_sql(source, "final_function_modify"));
    }

    const normalized_entity* moving_transition =
        referenced_routine(entity, graph, "moving_transition");
    if (moving_transition) {
        const std::vector<const routine_parameter*>
            moving_transition_parameters =
                sql_parameters(moving_transition->source);
        if (moving_transition_parameters.empty()
            || moving_transition_parameters.front()->sql_type
                != moving_transition->source.result_sql_type) {
            throw std::runtime_error(
                "aggregate '" + source.name
                + "' moving transition must return its state input type");
        }
        std::string moving_arguments;
        for (std::size_t index = 1;
             index < moving_transition_parameters.size();
             ++index) {
            if (!moving_arguments.empty()) moving_arguments += ", ";
            moving_arguments +=
                render_sql_type(
                    moving_transition_parameters[index]->sql_type);
        }
        if (moving_arguments != (arguments == "*" ? "" : arguments)) {
            throw std::runtime_error(
                "aggregate '" + source.name
                + "' moving transition input signature differs from SFUNC");
        }
        clauses.push_back(
            "MSFUNC = "
            + qualified_sql_name(*moving_transition, graph));
        clauses.push_back(
            "MSTYPE = "
            + moving_transition_parameters.front()->sql_type);
    }
    add_routine("moving_inverse", "MINVFUNC");
    add_routine("moving_final", "MFINALFUNC");
    if (const option_value* moving_space =
            find_option(source, "moving_state_space")) {
        clauses.push_back("MSSPACE = " + moving_space->expression);
    }
    if (const option_value* moving_initial =
            find_option(source, "moving_initial_condition")) {
        clauses.push_back(
            "MINITCOND = "
            + aggregate_initial_condition_sql(*moving_initial));
    }
    if (option_is(source, "moving_final_function_extra", "true")) {
        clauses.push_back("MFINALFUNC_EXTRA");
    }
    if (find_option(source, "moving_final_function_modify")) {
        clauses.push_back(
            "MFINALFUNC_MODIFY = "
            + aggregate_modify_sql(
                source, "moving_final_function_modify"));
    }
    if (const option_value* sort_operator =
            find_option(source, "sort_operator")) {
        const normalized_entity* op = referenced_entity(
            entity,
            graph,
            "sort_operator",
            [](entity_kind kind) {
                return kind == entity_kind::operator_;
            });
        if (!op) {
            throw std::runtime_error(
                "aggregate sort operator reference is unresolved: "
                + sort_operator->expression);
        }
        clauses.push_back(
            "SORTOP = "
            + operator_reference_name(*op, graph));
    }
    if (find_option(source, "parallel")) {
        std::string parallel = enum_leaf(
            find_option(source, "parallel")->expression);
        std::ranges::transform(
            parallel,
            parallel.begin(),
            [](unsigned char value) {
                return static_cast<char>(std::toupper(value));
            });
        clauses.push_back("PARALLEL = " + parallel);
    }

    std::string result = "CREATE AGGREGATE "
        + qualified_sql_name(entity, graph)
        + "(" + arguments + ") (\n";
    for (std::size_t index = 0; index < clauses.size(); ++index) {
        result += "    " + clauses[index]
            + (index + 1 == clauses.size() ? "\n" : ",\n");
    }
    return result + ");";
}

[[nodiscard]] inline const normalized_entity* routine_from_expression(
    const normalized_entity& owner,
    const sql_graph& graph,
    std::string_view expression) {
    const normalized_entity* result = nullptr;
    for (const std::string& identity : owner.dependency_identities) {
        const normalized_entity* candidate = find_entity(graph, identity);
        if (!candidate
            || !routine_entity_kind(candidate->source.kind)
            || !expression_mentions_declaration(
                expression, candidate->source)) {
            continue;
        }
        if (result
            && result->source.cpp_qualified_name
                != candidate->source.cpp_qualified_name) {
            throw std::runtime_error(
                "ambiguous operator-class routine reference '"
                + std::string{expression} + "'");
        }
        result = candidate;
    }
    if (!result) {
        throw std::runtime_error(
            "unresolved operator-class routine reference '"
            + std::string{expression} + "'");
    }
    return result;
}

[[nodiscard]] inline const normalized_entity* operator_for_function(
    const normalized_entity& function,
    const sql_graph& graph) {
    const normalized_entity* result = nullptr;
    for (const normalized_entity& candidate : graph.entities) {
        if (candidate.source.kind == entity_kind::operator_
            && candidate.source.cpp_qualified_name
                == function.source.cpp_qualified_name) {
            if (result) {
                throw std::runtime_error(
                    "function '" + function.source.cpp_qualified_name
                    + "' owns more than one operator annotation");
            }
            result = &candidate;
        }
    }
    return result;
}

[[nodiscard]] inline std::string operator_reference_sql(
    const normalized_entity& owner,
    const sql_graph& graph,
    std::string_view option_name) {
    const normalized_entity* referenced_operator = referenced_entity(
        owner,
        graph,
        option_name,
        [](entity_kind kind) {
            return kind == entity_kind::operator_;
        });
    if (referenced_operator == nullptr) {
        if (const normalized_entity* routine =
                referenced_routine(owner, graph, option_name)) {
            referenced_operator =
                operator_for_function(*routine, graph);
        }
    }
    if (referenced_operator == nullptr) {
        throw std::runtime_error(
            "unresolved operator reference in ."
            + std::string{option_name} + " for "
            + owner.stable_identity);
    }
    return operator_reference_name(*referenced_operator, graph);
}

[[nodiscard]] inline std::string opclass_statement(
    const normalized_entity& entity,
    const sql_graph& graph) {
    const collected_entity& source = entity.source;
    const option_value* operators = find_option(source, "operators");
    const option_value* functions = find_option(source, "functions");
    if (!operators || !functions) {
        throw std::runtime_error(
            "opclass '" + source.name
            + "' requires operator and support-function entries");
    }
    const std::vector<numbered_reference> operator_entries =
        parse_numbered_references(
            operators->expression, "opclass_operator");
    const std::vector<numbered_reference> function_entries =
        parse_numbered_references(
            functions->expression, "opclass_function");
    std::string indexed_type;
    std::vector<std::string> clauses;
    for (const numbered_reference& entry : operator_entries) {
        const normalized_entity* routine =
            routine_from_expression(entity, graph, entry.expression);
        const normalized_entity* op =
            operator_for_function(*routine, graph);
        const std::vector<const routine_parameter*> parameters =
            sql_parameters(routine->source);
        if (!op || parameters.empty() || parameters.size() > 2) {
            throw std::runtime_error(
                "operator-class strategy " + std::to_string(entry.number)
                + " does not reference an annotated unary/binary operator");
        }
        for (const routine_parameter* parameter : parameters) {
            if (indexed_type.empty()) {
                indexed_type = parameter->sql_type;
            } else if (parameter->sql_type != indexed_type) {
                throw std::runtime_error(
                    "opclass '" + source.name
                    + "' has mixed indexed operand types");
            }
        }
        std::string signature;
        for (std::size_t index = 0; index < parameters.size(); ++index) {
            if (index != 0) signature += ", ";
            signature += parameters[index]->sql_type;
        }
        clauses.push_back(
            "OPERATOR " + std::to_string(entry.number) + " "
            + qualified_sql_name(*op, graph)
            + " (" + signature + ")");
    }
    for (const numbered_reference& entry : function_entries) {
        const normalized_entity* routine =
            routine_from_expression(entity, graph, entry.expression);
        const std::vector<const routine_parameter*> parameters =
            sql_parameters(routine->source);
        if (parameters.empty()
            || (!indexed_type.empty()
                && parameters.front()->sql_type
                    != indexed_type)) {
            throw std::runtime_error(
                "opclass '" + source.name
                + "' support function does not accept the indexed type");
        }
        std::string signature;
        for (std::size_t index = 0; index < parameters.size(); ++index) {
            if (index != 0) signature += ", ";
            signature += parameters[index]->sql_type;
        }
        clauses.push_back(
            "FUNCTION " + std::to_string(entry.number) + " "
            + qualified_sql_name(*routine, graph)
            + "(" + signature + ")");
    }
    if (indexed_type.empty()) {
        throw std::runtime_error(
            "opclass '" + source.name
            + "' has no inferred indexed type");
    }
    std::string access_method =
        option_literal(source, "access_method", "btree");
    access_method = enum_leaf(access_method);
    std::string result = "CREATE OPERATOR CLASS "
        + qualified_sql_name(entity, graph);
    if (option_is(source, "is_default", "true")) result += " DEFAULT";
    result += "\n    FOR TYPE " + indexed_type
        + " USING " + access_method;
    if (const option_value* family = find_option(source, "family")) {
        result += " FAMILY " + enum_leaf(family->expression);
    }
    result += " AS\n";
    for (std::size_t index = 0; index < clauses.size(); ++index) {
        result += "    " + clauses[index] + ",\n";
    }
    if (const option_value* storage = find_option(source, "storage")) {
        std::string type_name;
        if (!entity.opclass_storage_identity.empty()) {
            const normalized_entity* referenced =
                find_entity(graph, entity.opclass_storage_identity);
            if (!referenced
                || referenced->source.kind != entity_kind::type) {
                throw std::runtime_error(
                    "operator class '" + source.name
                    + "' lost its resolved storage type");
            }
            type_name = qualified_sql_name(*referenced, graph);
        } else if (storage->expression.contains("type_of<")) {
            throw std::runtime_error(
                "operator class '" + source.name
                + "' has an unresolved storage type");
        } else {
            type_name = leaf_identifier(storage->expression);
        }
        result += "    STORAGE " + type_name + "\n";
    } else {
        result.erase(result.size() - 2);
        result += "\n";
    }
    return result + ";";
}

[[nodiscard]] inline std::string catalog_dependencies_statement(
    const normalized_entity& entity,
    const sql_graph& graph) {
    const normalized_entity* access_method = nullptr;
    for (const normalized_entity& candidate : graph.entities) {
        if (candidate.source.kind == entity_kind::access_method
            && candidate.source.cpp_qualified_name
                == entity.source.cpp_qualified_name) {
            if (access_method) {
                throw std::runtime_error(
                    "AM catalog dependency action has multiple access methods");
            }
            access_method = &candidate;
        }
    }
    if (!access_method) {
        throw std::runtime_error(
            "AM catalog dependency action has no annotated access method");
    }
    std::vector<std::pair<std::string, std::string>> relations;
    for (const std::string& identity : entity.dependency_identities) {
        const normalized_entity* root = find_entity(graph, identity);
        if (!root
            || (root->source.kind != entity_kind::table
                && root->source.kind != entity_kind::sequence)) {
            continue;
        }
        const std::string root_schema = root->source.schema;
        relations.emplace_back(root_schema, root->source.name);
        for (const normalized_entity& candidate : graph.entities) {
            const bool child =
                candidate.source.kind == entity_kind::primary_key
                    || candidate.source.kind == entity_kind::unique
                    || candidate.source.kind == entity_kind::index;
            if (child
                && std::ranges::contains(
                    candidate.dependency_identities,
                    root->stable_identity)) {
                std::string relation_name;
                if (find_option(candidate.source, "name")) {
                    relation_name = candidate.source.name;
                } else if (
                    candidate.source.kind == entity_kind::primary_key) {
                    constexpr std::string_view suffix{"_pkey"};
                    if (root->source.name.size() + suffix.size() > 63) {
                        throw std::runtime_error(
                            "derived primary-key relation name for '"
                            + root_schema + "." + root->source.name
                            + "' exceeds PostgreSQL's 63-byte identifier "
                              "limit; specify an explicit .name");
                    }
                    relation_name = root->source.name + std::string{suffix};
                } else {
                    throw std::runtime_error(
                        "natural "
                        + std::string{kind_name(candidate.source.kind)}
                        + " child for '" + root_schema + "."
                        + root->source.name
                        + "' requires an explicit .name before it can be "
                          "used as a catalog dependency");
                }
                relations.emplace_back(
                    root_schema,
                    std::move(relation_name));
            }
        }
    }
    std::ranges::sort(relations);
    relations.erase(
        std::unique(relations.begin(), relations.end()),
        relations.end());
    if (relations.empty()) {
        throw std::runtime_error(
            "AM catalog dependency action resolves no relation roots");
    }
    const std::string& schema = relations.front().first;
    if (std::ranges::any_of(
            relations,
            [&schema](const auto& relation) {
                return relation.first != schema;
            })) {
        throw std::runtime_error(
            "AM catalog dependency roots must share one schema");
    }
    std::string result =
        "INSERT INTO pg_catalog.pg_depend (\n"
        "    classid, objid, objsubid,\n"
        "    refclassid, refobjid, refobjsubid, deptype)\n"
        "WITH desired(relid) AS (\n"
        "    SELECT c.oid\n"
        "      FROM pg_catalog.pg_class AS c\n"
        "      JOIN pg_catalog.pg_namespace AS n\n"
        "        ON n.oid = c.relnamespace\n"
        "     WHERE n.nspname = "
        + (schema == "$extension"
               ? std::string{"pg_catalog.current_schema()"}
               : quoted_sql_literal(schema))
        + "\n"
          "       AND c.relname IN (\n";
    for (std::size_t index = 0; index < relations.size(); ++index) {
        result += "           "
            + quoted_sql_literal(relations[index].second)
            + (index + 1 == relations.size() ? ")\n" : ",\n");
    }
    result +=
        "),\n"
        "edges AS (\n"
        "    SELECT 'pg_catalog.pg_am'::regclass::oid,\n"
        "           am.oid,\n"
        "           0,\n"
        "           'pg_catalog.pg_class'::regclass::oid,\n"
        "           desired.relid,\n"
        "           0,\n"
        "           'n'::\"char\"\n"
        "      FROM desired\n"
        "      CROSS JOIN pg_catalog.pg_am AS am\n"
        "     WHERE am.amname = "
        + quoted_sql_literal(access_method->source.name)
        + "\n"
          ")\n"
          "SELECT * FROM edges\n"
          "EXCEPT\n"
          "SELECT classid, objid, objsubid,\n"
          "       refclassid, refobjid, refobjsubid, deptype\n"
          "  FROM pg_catalog.pg_depend;";
    return result;
}

[[nodiscard]] inline std::string sql_function_body_delimiter(
        std::string_view body) {
    constexpr std::string_view preferred{"$sql$"};
    if (!body.contains(preferred)) {
        return std::string{preferred};
    }
    constexpr std::string_view fallback{"$bioseq_sql$"};
    if (!body.contains(fallback)) {
        return std::string{fallback};
    }
    for (std::size_t suffix = 1;; ++suffix) {
        const std::string candidate =
            "$bioseq_sql_" + std::to_string(suffix) + "$";
        if (!body.contains(candidate)) {
            return candidate;
        }
    }
}

[[nodiscard]] inline std::string render_entity_statement(
    const normalized_entity& entity,
    const sql_graph& graph,
    const std::vector<const normalized_entity*>& children) {
    const collected_entity& source = entity.source;
    const std::string qualified = qualified_sql_name(entity, graph);
    switch (source.kind) {
        case entity_kind::comment:
            throw std::runtime_error(
                "comment is an internally synthesized SQL statement kind, "
                "not an authored entity");
        case entity_kind::schema:
            return create_clause(source, false, true) + "SCHEMA "
                + if_not_exists(source) + source.name + ";";
        case entity_kind::session_setting: {
            std::string value = option_literal(source, "value");
            if (value.empty()) {
                throw std::runtime_error(
                    "session_setting requires a typed .value option");
            }
            if (value == "false") value = "off";
            else if (value == "true") value = "on";
            return "SET " + source.name + " = " + value + ";";
        }
        case entity_kind::sequence: {
            std::string result = create_clause(source, false, true)
                + "SEQUENCE " + if_not_exists(source) + qualified;
            const std::string sequence_type =
                infer_sql_type_from_cpp(source.cpp_return_type);
            if (sequence_type != "smallint"
                && sequence_type != "integer"
                && sequence_type != "bigint") {
                throw std::runtime_error(
                    "sequence '" + qualified
                    + "' requires a signed integral C++ declaration");
            }
            if (source.initializer_expression.empty()) {
                throw std::runtime_error(
                    "sequence '" + qualified
                    + "' requires a C++ initializer that supplies START");
            }
            result += "\n    AS " + sequence_type;
            if (const option_value* value = find_option(source, "increment")) {
                result += "\n    INCREMENT BY " + value->expression;
            }
            if (const option_value* value = find_option(source, "minimum")) {
                result += "\n    MINVALUE " + value->expression;
            }
            if (const option_value* value = find_option(source, "maximum")) {
                result += "\n    MAXVALUE " + value->expression;
            }
            result += "\n    START " + source.initializer_expression;
            if (const option_value* value = find_option(source, "cache")) {
                result += "\n    CACHE " + value->expression;
            }
            if (const option_value* value = find_option(source, "cycle")) {
                result += option_is(source, "cycle", "true")
                    ? "\n    CYCLE"
                    : "\n    NO CYCLE";
            }
            result += ";";
            return result;
        }
        case entity_kind::table: {
            std::string qualifier;
            if (option_is(source, "persistence", "temporary")) {
                qualifier = "TEMPORARY ";
            } else if (option_is(
                           source, "persistence", "unlogged")) {
                qualifier = "UNLOGGED ";
            }
            std::string result = "CREATE " + qualifier + "TABLE "
                + if_not_exists(source) + qualified + " (\n";
            std::vector<const normalized_entity*> table_children;
            for (const normalized_entity* child : children) {
                if ((child->source.kind == entity_kind::column
                     || child->source.kind
                         == entity_kind::primary_key
                     || child->source.kind == entity_kind::unique
                     || child->source.kind
                         == entity_kind::foreign_key
                     || child->source.kind == entity_kind::check)
                    && !option_is(
                        child->source,
                        "placement",
                        "alter_table")) {
                    table_children.push_back(child);
                }
            }
            std::ranges::sort(
                table_children,
                [](const normalized_entity* left,
                   const normalized_entity* right) {
                    const bool left_column =
                        left->source.kind == entity_kind::column;
                    const bool right_column =
                        right->source.kind == entity_kind::column;
                    if (left_column != right_column) {
                        return left_column;
                    }
                    const std::size_t left_offset =
                        left->source.source_range.begin.byte_offset;
                    const std::size_t right_offset =
                        right->source.source_range.begin.byte_offset;
                    if (left_offset != right_offset) {
                        return left_offset < right_offset;
                    }
                    return left->stable_identity
                        < right->stable_identity;
                });
            bool first = true;
            for (const normalized_entity* child : table_children) {
                if (!first) result += ",\n";
                first = false;
                if (child->source.kind == entity_kind::column) {
                    const std::string sql_type =
                        child->source.resolved_sql_type.empty()
                        ? option_literal(
                            child->source,
                            "sql_type",
                            infer_sql_type_from_cpp(
                                child->source.cpp_return_type))
                        : child->source.resolved_sql_type;
                    const std::string rendered_sql_type =
                        sql_type.contains("custom_type<")
                        ? leaf_identifier(sql_type)
                        : (sql_type.contains("::")
                               ? detail::enum_leaf(sql_type)
                               : sql_type);
                    result += "    " + child->source.name + " "
                        + (sql_type.empty()
                               ? "text"
                               : rendered_sql_type);
                    if (!child->source.cpp_return_type.contains(
                            "optional<")) {
                        result += " NOT NULL";
                    }
                    if (const option_value* default_value =
                            find_option(
                                child->source, "default_value")) {
                        result += " DEFAULT "
                            + extract_checked_sql(
                                default_value->expression);
                    } else if (!child->source
                                    .initializer_expression.empty()) {
                        result += " DEFAULT "
                            + column_initializer_sql(child->source);
                    }
                } else {
                    result += "    " + constraint_fragment(*child);
                }
            }
            if (first) {
                throw std::runtime_error(
                    "annotated table '" + source.name
                    + "' has no annotated columns");
            }
            result += "\n)";
            if (const std::string access_method =
                    option_literal(
                        source, "access_method", "default");
                access_method != "default") {
                result += " USING " + access_method;
            }
            std::vector<std::string> relation_options;
            if (option_is(
                    source, "user_catalog_table", "true")) {
                relation_options.emplace_back(
                    "user_catalog_table = true");
            }
            if (const option_value* authored_options =
                    find_option(source, "relation_options")) {
                relation_options.push_back(extract_checked_sql(
                    authored_options->expression));
            }
            if (!relation_options.empty()) {
                result += " WITH (";
                for (std::size_t index = 0;
                     index < relation_options.size(); ++index) {
                    if (index != 0) result += ", ";
                    result += relation_options[index];
                }
                result += ")";
            }
            result += ";";
            return result;
        }
        case entity_kind::view: {
            const std::string body =
                extract_checked_sql(source.initializer_expression);
            std::string result = create_clause(source, true, false);
            if (option_is(source, "temporary", "true")) {
                result += "TEMPORARY ";
            }
            if (option_is(source, "recursive", "true")) {
                result += "RECURSIVE ";
            }
            result += "VIEW " + qualified;
            std::vector<std::string> options;
            if (option_is(source, "security_barrier", "true")) {
                options.emplace_back("security_barrier = true");
            }
            if (option_is(source, "security_invoker", "true")) {
                options.emplace_back("security_invoker = true");
            }
            if (!options.empty()) {
                result += " WITH (";
                for (std::size_t index = 0;
                     index < options.size(); ++index) {
                    if (index != 0) result += ", ";
                    result += options[index];
                }
                result += ")";
            }
            return result + " AS\n" + body + ";";
        }
        case entity_kind::function: {
            return c_routine_statement(entity, graph);
        }
        case entity_kind::procedure: {
            return c_routine_statement(entity, graph);
        }
        case entity_kind::sql_function: {
            const std::string body =
                extract_checked_sql(source.initializer_expression);
            const std::string delimiter =
                sql_function_body_delimiter(body);
            std::string result = create_clause(source, true, false)
                + "FUNCTION " + qualified + "("
                + render_parameters(source) + ")\nRETURNS "
                + source.result_sql_type + "\nLANGUAGE SQL\nAS "
                + delimiter + "\n" + body + "\n" + delimiter;
            result += routine_properties(entity, graph);
            result += ";";
            return result;
        }
        case entity_kind::type: {
            std::vector<natural_type_statement> statements =
                natural_type_statements(entity, graph, children);
            if (statements.size() != 1) {
                throw std::runtime_error(
                    "base type '" + source.name
                    + "' requires separate shell and definition "
                      "generated statements");
            }
            return std::move(statements.front().text);
        }
        case entity_kind::index: {
            const normalized_entity* parent =
                primary_parent(entity, graph);
            if (!parent || parent->source.kind != entity_kind::table) {
                throw std::runtime_error(
                    "index '" + source.name
                    + "' has no annotated table parent");
            }
            const option_value* expression_option =
                find_option(source, "expression");
            const std::string expression =
                expression_option == nullptr
                ? std::string{}
                : extract_checked_sql(
                    expression_option->expression);
            std::string result{
                option_is(source, "unique", "true")
                    ? "CREATE UNIQUE INDEX "
                    : "CREATE INDEX "};
            result += if_not_exists(source) + source.name + " ON "
                + qualified_sql_name(*parent, graph);
            if (const std::string access_method =
                    option_literal(
                        source, "access_method", "btree");
                access_method != "btree") {
                result += " USING " + access_method;
            }
            if (!expression.empty()) {
                result += " (" + expression + ")";
            }
            if (const option_value* predicate =
                    find_option(source, "predicate")) {
                result += " WHERE "
                    + extract_checked_sql(predicate->expression);
            }
            return result + ";";
        }
        case entity_kind::primary_key:
        case entity_kind::unique:
        case entity_kind::foreign_key:
        case entity_kind::check: {
            if (!option_is(source, "placement", "alter_table")) {
                throw std::runtime_error(
                    "inline constraint '" + source.name
                    + "' reached the top-level renderer without its table");
            }
            const normalized_entity* parent =
                primary_parent(entity, graph);
            if (!parent || parent->source.kind != entity_kind::table) {
                throw std::runtime_error(
                    "constraint '" + source.name
                    + "' has no annotated table parent");
            }
            return "ALTER TABLE " + qualified_sql_name(*parent, graph)
                + "\n    ADD " + constraint_fragment(entity) + ";";
        }
        case entity_kind::operator_: {
            const normalized_entity* function =
                primary_parent(entity, graph);
            const std::vector<const routine_parameter*> parameters =
                function == nullptr
                ? std::vector<const routine_parameter*>{}
                : sql_parameters(function->source);
            if (!function
                || (function->source.kind != entity_kind::function
                    && function->source.kind
                        != entity_kind::sql_function)
                || parameters.size() != 2) {
                throw std::runtime_error(
                    "operator '" + source.name
                    + "' requires one annotated binary function parent");
            }
            std::string result = "CREATE OPERATOR "
                + qualified_sql_name(entity, graph)
                + " (\n    FUNCTION = "
                + qualified_sql_name(*function, graph)
                + ",\n    LEFTARG = "
                + parameters[0]->sql_type
                + ",\n    RIGHTARG = "
                + parameters[1]->sql_type;
            if (find_option(source, "commutator")) {
                result += ",\n    COMMUTATOR = "
                    + operator_reference_sql(
                        entity, graph, "commutator");
            }
            if (find_option(source, "negator")) {
                result += ",\n    NEGATOR = "
                    + operator_reference_sql(
                        entity, graph, "negator");
            }
            if (find_option(source, "restrict")) {
                result += ",\n    RESTRICT = "
                    + routine_reference_sql(
                        entity, graph, "restrict", true);
            }
            if (find_option(source, "join")) {
                result += ",\n    JOIN = "
                    + routine_reference_sql(
                        entity, graph, "join", true);
            }
            if (option_is(source, "hashes", "true")) {
                result += ",\n    HASHES";
            }
            if (option_is(source, "merges", "true")) {
                result += ",\n    MERGES";
            }
            result += "\n);";
            return result;
        }
        case entity_kind::cast: {
            const normalized_entity* function =
                primary_parent(entity, graph);
            const std::vector<const routine_parameter*> parameters =
                function == nullptr
                ? std::vector<const routine_parameter*>{}
                : sql_parameters(function->source);
            if (!function || parameters.empty()
                || parameters.size() > 3) {
                throw std::runtime_error(
                    "cast requires an annotated function parent with one "
                    "to three SQL arguments");
            }
            if (parameters.size() >= 2
                && parameters[1]->sql_type != "integer") {
                throw std::runtime_error(
                    "cast function second argument must be integer typmod");
            }
            if (parameters.size() == 3
                && parameters[2]->sql_type != "boolean") {
                throw std::runtime_error(
                    "cast function third argument must be boolean "
                    "explicitness");
            }
            std::string result = "CREATE CAST ("
                + parameters[0]->sql_type + " AS "
                + function->source.result_sql_type + ")\n    WITH FUNCTION "
                + qualified_sql_name(*function, graph) + "(";
            for (std::size_t index = 0; index < parameters.size();
                 ++index) {
                if (index != 0) result += ", ";
                result += parameters[index]->sql_type;
            }
            result += ")";
            if (option_is(source, "assignment", "true")) {
                result += "\n    AS ASSIGNMENT";
            } else if (option_is(source, "implicit", "true")) {
                result += "\n    AS IMPLICIT";
            }
            result += ";";
            return result;
        }
        case entity_kind::access_method: {
            const normalized_entity* handler =
                primary_parent(entity, graph);
            if (!handler || handler->source.kind != entity_kind::function) {
                throw std::runtime_error(
                    "access method requires its annotated handler function");
            }
            const std::string kind = option_is(source, "kind", "index")
                ? "INDEX"
                : "TABLE";
            return "CREATE ACCESS METHOD " + source.name + "\n    TYPE "
                + kind + "\n    HANDLER "
                + qualified_sql_name(*handler, graph) + ";";
        }
        case entity_kind::grant:
        case entity_kind::revoke: {
            const normalized_entity* parent =
                primary_parent(entity, graph);
            if (!parent) {
                throw std::runtime_error(
                    std::string{kind_name(source.kind)}
                    + " has no annotated natural parent");
            }
            const bool revoke = source.kind == entity_kind::revoke;
            const std::string role = option_literal(
                source, revoke ? "from" : "to", "public");
            std::string result;
            if (revoke) {
                result = "REVOKE ";
                if (option_is(source, "grant_option", "true")) {
                    result += "GRANT OPTION FOR ";
                }
            } else {
                result = "GRANT ";
            }
            std::string object = acl_object(*parent, graph);
            if (const option_value* scope = find_option(source, "scope")) {
                if (parent->source.kind != entity_kind::schema) {
                    throw std::runtime_error(
                        "schema-wide ACL scope requires a schema parent");
                }
                const std::string leaf = enum_leaf(scope->expression);
                if (leaf == "all_tables_in_schema") {
                    object = "ALL TABLES IN SCHEMA " + parent->source.name;
                } else if (leaf == "all_sequences_in_schema") {
                    object =
                        "ALL SEQUENCES IN SCHEMA " + parent->source.name;
                } else if (leaf == "all_functions_in_schema") {
                    object =
                        "ALL FUNCTIONS IN SCHEMA " + parent->source.name;
                } else if (leaf != "object") {
                    throw std::runtime_error(
                        "unsupported typed ACL scope '" + leaf + "'");
                }
            }
            result += privilege_list(source) + " ON " + object
                + (revoke ? " FROM " : " TO ")
                + (role == "bioseq::sql::public_role"
                       || role == "public_role"
                       ? "PUBLIC"
                       : enum_leaf(role));
            if (!revoke && option_is(source, "grant_option", "true")) {
                result += " WITH GRANT OPTION";
            }
            result += ";";
            return result;
        }
        case entity_kind::config_dump: {
            const normalized_entity* table =
                primary_parent(entity, graph);
            if (!table || table->source.kind != entity_kind::table) {
                throw std::runtime_error(
                    "config_dump must attach to an annotated table");
            }
            return "SELECT pg_catalog.pg_extension_config_dump('"
                + qualified_sql_name(*table, graph)
                + "', 'WHERE FALSE');";
        }
        case entity_kind::aggregate: {
            return aggregate_statement(entity, graph);
        }
        case entity_kind::opclass:
            return opclass_statement(entity, graph);
        case entity_kind::catalog_dependencies:
            return catalog_dependencies_statement(entity, graph);
        case entity_kind::guc:
            throw std::runtime_error(
                "GUC '" + source.name
                + "' requires its generated C++ registration facet");
        case entity_kind::column:
            throw std::runtime_error(
                "orphan column '" + source.name
                + "' was not consumed by an annotated table");
        case entity_kind::type_role:
            throw std::runtime_error(
                "orphan type-role callback '" + source.name
                + "' was not consumed by an annotated base type");
        case entity_kind::guc_value:
            throw std::runtime_error(
                "orphan GUC value '" + source.name
                + "' was not consumed by an annotated GUC");
        case entity_kind::phase:
            throw std::runtime_error(
                "phase marker reached statement rendering");
    }
    throw std::runtime_error(
        "unhandled SQL entity kind in deterministic renderer");
}

struct statement_candidate {
    const normalized_entity* owner{};
    entity_kind statement_kind{entity_kind::phase};
    std::string prefix;
    std::string text;
};

inline void append_entity_statement_candidates(
    std::vector<statement_candidate>& result,
    const normalized_entity& entity,
    const sql_graph& graph,
    const std::vector<const normalized_entity*>& children,
    std::string prefix) {
    std::vector<statement_candidate> local;
    if (entity.source.kind == entity_kind::type) {
        std::vector<natural_type_statement> statements =
            natural_type_statements(entity, graph, children);
        if (statements.empty() || statements.size() > 2) {
            throw std::runtime_error(
                "annotated type '" + entity.stable_identity
                + "' rendered an unsupported statement shape");
        }
        local.reserve(statements.size() + 16);
        local.push_back(statement_candidate{
            .owner = &entity,
            .statement_kind = entity_kind::type,
            .text = std::move(statements.front().text),
        });
        if (statements.size() == 2) {
            std::set<std::string, std::less<>> emitted_callbacks;
            for (const type_role_binding& role :
                 type_roles_for(entity, graph)) {
                if (!emitted_callbacks.insert(
                        role.function->stable_identity).second) {
                    continue;
                }
                for (const std::string& dependency :
                     role.function->dependency_identities) {
                    if (dependency == entity.stable_identity) continue;
                    const auto dependency_position = std::ranges::find(
                        graph.entities,
                        dependency,
                        &normalized_entity::stable_identity);
                    const auto type_position = std::ranges::find(
                        graph.entities,
                        entity.stable_identity,
                        &normalized_entity::stable_identity);
                    if (dependency_position == graph.entities.end()
                        || type_position == graph.entities.end()
                        || dependency_position >= type_position) {
                        throw std::runtime_error(
                            "base-type callback '"
                            + role.function->stable_identity
                            + "' has an unsatisfied prerequisite '"
                            + dependency + "'");
                    }
                }
                local.push_back(statement_candidate{
                    .owner = role.function,
                    .statement_kind = role.function->source.kind,
                    .prefix = remark_prefix(role.function->source),
                    .text = render_entity_statement(
                        *role.function,
                        graph,
                        {}),
                });
                if (const std::optional<std::string> comment =
                        comment_statement(*role.function, graph)) {
                    local.push_back(statement_candidate{
                        .owner = role.function,
                        .statement_kind = entity_kind::comment,
                        .text = *comment,
                    });
                }
            }
            local.push_back(statement_candidate{
                .owner = &entity,
                .statement_kind = entity_kind::type,
                .text = std::move(statements.back().text),
            });
        }
    } else {
        local.push_back(statement_candidate{
            .owner = &entity,
            .statement_kind = entity.source.kind,
            .text = render_entity_statement(entity, graph, children),
        });
    }

    if (!entity.source.companion) {
        if (const std::optional<std::string> comment =
                comment_statement(entity, graph)) {
            local.push_back(statement_candidate{
                .owner = &entity,
                .statement_kind = entity_kind::comment,
                .text = *comment,
            });
        }
    }
    local.front().prefix = std::move(prefix);
    result.insert(
        result.end(),
        std::make_move_iterator(local.begin()),
        std::make_move_iterator(local.end()));
}

inline void validate_statement_candidates(
    const std::vector<statement_candidate>& candidates) {
    for (const statement_candidate& candidate : candidates) {
        if (top_level_statement_count(candidate.text) != 1) {
            throw std::runtime_error(
                "generated SQL owner '" + candidate.owner->stable_identity
                + "' must render exactly one top-level statement per "
                  "statement candidate");
        }
    }
}

[[nodiscard]] inline std::string statement_id(std::size_t ordinal) {
    std::ostringstream output;
    output << 'S' << std::setw(3) << std::setfill('0') << ordinal;
    return output.str();
}

}  // namespace detail

[[nodiscard]] inline rendered_sql_artifact render_sql_graph(
    const sql_graph& graph) {
    rendered_sql_artifact artifact;
    std::map<std::string, std::vector<const normalized_entity*>, std::less<>>
        children;
    for (const normalized_entity& entity : graph.entities) {
        if (!entity.source.companion) continue;
        if (const normalized_entity* parent =
                detail::primary_parent(entity, graph)) {
            children[parent->stable_identity].push_back(&entity);
        }
    }
    std::vector<detail::statement_candidate> candidates;
    std::string pending_prefix;
    for (std::size_t graph_ordinal = 0;
         graph_ordinal < graph.entities.size();
         ++graph_ordinal) {
        const normalized_entity& entity =
            graph.entities[graph_ordinal];
        if (entity.source.kind == entity_kind::guc
            || entity.source.kind == entity_kind::guc_value) {
            continue;
        }
        if (entity.source.kind == entity_kind::function
            && detail::function_is_type_callback(entity, graph)) {
            continue;
        }
        const bool inline_companion =
            entity.source.kind == entity_kind::column
            || entity.source.kind == entity_kind::type_role
            || ((entity.source.kind == entity_kind::primary_key
                 || entity.source.kind == entity_kind::unique
                 || entity.source.kind == entity_kind::foreign_key
                 || entity.source.kind == entity_kind::check)
                && !detail::option_is(
                    entity.source, "placement", "alter_table"));
        if (inline_companion) {
            bool valid_parent = false;
            if (entity.source.kind == entity_kind::type_role) {
                valid_parent = std::ranges::any_of(
                    detail::type_role_bindings(graph),
                    [&entity](const detail::type_role_binding& binding) {
                        return binding.role->stable_identity
                            == entity.stable_identity;
                    });
            } else {
                const normalized_entity* parent =
                    detail::primary_parent(entity, graph);
                valid_parent =
                    parent
                    && ((entity.source.kind == entity_kind::column
                         && (parent->source.kind == entity_kind::table
                             || parent->source.kind == entity_kind::type))
                        || ((entity.source.kind
                                 == entity_kind::primary_key
                             || entity.source.kind == entity_kind::unique
                             || entity.source.kind
                                 == entity_kind::foreign_key
                             || entity.source.kind == entity_kind::check)
                            && parent->source.kind
                                == entity_kind::table));
            }
            if (!valid_parent) {
                throw std::runtime_error(
                    "dependency-only SQL entity '"
                    + entity.stable_identity
                    + "' has no compatible natural parent");
            }
            continue;
        }
        const std::string remarks = detail::remark_prefix(entity.source);
        if (entity.source.kind == entity_kind::phase) {
            if (!remarks.empty()) {
                pending_prefix += remarks;
                if (!pending_prefix.ends_with("\n\n")) {
                    pending_prefix += '\n';
                }
            }
            continue;
        }
        if (entity.source.kind == entity_kind::schema
            && detail::option_is(
                entity.source, "provision", "extension_control")) {
            pending_prefix += remarks;
            continue;
        }
        detail::append_entity_statement_candidates(
            candidates,
            entity,
            graph,
            children[entity.stable_identity],
            std::exchange(pending_prefix, {}) + remarks);
    }
    detail::validate_statement_candidates(candidates);

    artifact.sql =
        "-- complain if script is sourced in psql, rather than via CREATE EXTENSION\n"
        "\\echo Use \"CREATE EXTENSION " + graph.extension_name
        + "\" to load this file. \\quit\n\n";
    artifact.statements.reserve(candidates.size());
    for (detail::statement_candidate& candidate : candidates) {
        const std::string id =
            detail::statement_id(artifact.statements.size() + 1);
        artifact.sql += candidate.prefix;
        artifact.sql += candidate.text;
        artifact.sql += "\n\n";
        artifact.statements.push_back(rendered_statement{
            .id = id,
            .entity_identity =
                candidate.owner->stable_identity,
            .text = std::move(candidate.text),
            .statement_kind = candidate.statement_kind,
        });
    }
    artifact.sql += pending_prefix;
    while (artifact.sql.ends_with("\n\n")) {
        artifact.sql.pop_back();
    }
    if (!artifact.sql.ends_with('\n')) artifact.sql.push_back('\n');
    return artifact;
}

}  // namespace bioseq::codegen::sql
