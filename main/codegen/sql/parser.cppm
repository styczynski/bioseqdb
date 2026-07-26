module;

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.codegen.sql:parser;

import :model;
import bioseq.codegen.support;
import bioseq.codegen.sql.signature;

export namespace bioseq::codegen::sql {

namespace detail {

[[nodiscard]] inline bool ascii_space(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\n'
        || value == '\r' || value == '\f' || value == '\v';
}

[[nodiscard]] inline char ascii_lower(char value) noexcept {
    if (value >= 'A' && value <= 'Z') {
        return static_cast<char>(value - 'A' + 'a');
    }
    return value;
}

[[nodiscard]] inline std::string lower_copy(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const char value : text) {
        result.push_back(ascii_lower(value));
    }
    return result;
}

[[nodiscard]] inline std::string_view trim_view(std::string_view text) noexcept {
    while (!text.empty() && ascii_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && ascii_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] inline bool starts_with_word(
    std::string_view text,
    std::string_view prefix) noexcept {
    if (!text.starts_with(prefix)) {
        return false;
    }
    return text.size() == prefix.size()
        || ascii_space(text[prefix.size()])
        || text[prefix.size()] == '(';
}

[[nodiscard]] inline std::size_t line_number_at(
    std::string_view text,
    std::size_t byte_offset) {
    return 1 + static_cast<std::size_t>(
        std::ranges::count(text.substr(0, byte_offset), '\n'));
}

[[nodiscard]] inline std::optional<std::string>
read_dollar_delimiter(std::string_view text, std::size_t offset) {
    if (offset >= text.size() || text[offset] != '$') {
        return std::nullopt;
    }
    std::size_t cursor = offset + 1;
    while (cursor < text.size()) {
        const char value = text[cursor];
        if (value == '$') {
            return std::string{text.substr(offset, cursor - offset + 1)};
        }
        const bool valid = (value >= 'A' && value <= 'Z')
            || (value >= 'a' && value <= 'z')
            || (value >= '0' && value <= '9')
            || value == '_';
        if (!valid) {
            return std::nullopt;
        }
        ++cursor;
    }
    return std::nullopt;
}

[[nodiscard]] inline std::string normalize_sql(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    bool pending_space = false;
    bool in_single_quote = false;
    bool in_double_quote = false;
    bool in_line_comment = false;
    int block_comment_depth = 0;
    std::optional<std::string> dollar_delimiter;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char value = text[index];
        const char next =
            index + 1 < text.size() ? text[index + 1] : '\0';
        if (in_line_comment) {
            if (value == '\n') {
                in_line_comment = false;
                pending_space = true;
            }
            continue;
        }
        if (block_comment_depth > 0) {
            if (value == '/' && next == '*') {
                ++block_comment_depth;
                ++index;
            } else if (value == '*' && next == '/') {
                --block_comment_depth;
                ++index;
                pending_space = true;
            }
            continue;
        }
        if (dollar_delimiter) {
            if (text.substr(index).starts_with(*dollar_delimiter)) {
                result += *dollar_delimiter;
                index += dollar_delimiter->size() - 1;
                dollar_delimiter.reset();
            } else {
                result.push_back(value);
            }
            continue;
        }
        if (in_single_quote) {
            result.push_back(value);
            if (value == '\'' && index + 1 < text.size()
                && text[index + 1] == '\'') {
                result.push_back(text[++index]);
            } else if (value == '\'') {
                in_single_quote = false;
            }
            continue;
        }
        if (in_double_quote) {
            result.push_back(value);
            if (value == '"' && index + 1 < text.size()
                && text[index + 1] == '"') {
                result.push_back(text[++index]);
            } else if (value == '"') {
                in_double_quote = false;
            }
            continue;
        }
        if (value == '-' && next == '-') {
            in_line_comment = true;
            ++index;
            continue;
        }
        if (value == '/' && next == '*') {
            block_comment_depth = 1;
            ++index;
            continue;
        }
        if (value == '\'') {
            if (pending_space && !result.empty()) {
                result.push_back(' ');
            }
            pending_space = false;
            result.push_back(value);
            in_single_quote = true;
            continue;
        }
        if (value == '"') {
            if (pending_space && !result.empty()) {
                result.push_back(' ');
            }
            pending_space = false;
            result.push_back(value);
            in_double_quote = true;
            continue;
        }
        if (value == '$') {
            if (const auto delimiter = read_dollar_delimiter(text, index)) {
                if (pending_space && !result.empty()) {
                    result.push_back(' ');
                }
                pending_space = false;
                result += *delimiter;
                index += delimiter->size() - 1;
                dollar_delimiter = delimiter;
                continue;
            }
        }
        if (ascii_space(value)) {
            pending_space = true;
            continue;
        }
        if (pending_space && !result.empty()) {
            result.push_back(' ');
        }
        pending_space = false;
        result.push_back(ascii_lower(value));
    }
    return std::string{trim_view(result)};
}

[[nodiscard]] inline statement_kind classify_statement(
    std::string_view normalized) noexcept {
    if (starts_with_word(normalized, "alter")) return statement_kind::alter;
    if (starts_with_word(normalized, "comment")) return statement_kind::comment;
    if (normalized.starts_with("create access method"))
        return statement_kind::create_access_method;
    if (normalized.starts_with("create aggregate"))
        return statement_kind::create_aggregate;
    if (normalized.starts_with("create cast"))
        return statement_kind::create_cast;
    if (normalized.starts_with("create function")
        || normalized.starts_with("create or replace function"))
        return statement_kind::create_function;
    if (normalized.starts_with("create index")
        || normalized.starts_with("create unique index"))
        return statement_kind::create_index;
    if (normalized.starts_with("create operator class"))
        return statement_kind::create_operator_class;
    if (normalized.starts_with("create operator"))
        return statement_kind::create_operator;
    if (normalized.starts_with("create procedure")
        || normalized.starts_with("create or replace procedure"))
        return statement_kind::create_procedure;
    if (normalized.starts_with("create schema"))
        return statement_kind::create_schema;
    if (normalized.starts_with("create sequence"))
        return statement_kind::create_sequence;
    if (normalized.starts_with("create table")
        || normalized.starts_with("create unlogged table")
        || normalized.starts_with("create temporary table"))
        return statement_kind::create_table;
    if (normalized.starts_with("create type"))
        return statement_kind::create_type;
    if (normalized.starts_with("create view")
        || normalized.starts_with("create or replace view"))
        return statement_kind::create_view;
    if (starts_with_word(normalized, "grant")) return statement_kind::grant;
    if (starts_with_word(normalized, "insert")) return statement_kind::insert;
    if (starts_with_word(normalized, "reset")) return statement_kind::reset;
    if (starts_with_word(normalized, "revoke")) return statement_kind::revoke;
    if (starts_with_word(normalized, "select")) return statement_kind::select;
    if (starts_with_word(normalized, "set")) return statement_kind::set;
    return statement_kind::unknown;
}

[[nodiscard]] inline std::optional<std::string> routine_identity(
    statement_kind kind,
    std::string_view normalized) {
    if (kind != statement_kind::create_function
        && kind != statement_kind::create_procedure) {
        return std::nullopt;
    }
    const std::string_view marker =
        kind == statement_kind::create_function ? "function" : "procedure";
    const std::size_t marker_offset = normalized.find(marker);
    if (marker_offset == std::string_view::npos) {
        return std::nullopt;
    }
    std::size_t cursor = marker_offset + marker.size();
    while (cursor < normalized.size() && ascii_space(normalized[cursor])) {
        ++cursor;
    }
    const std::size_t name_begin = cursor;
    std::size_t arguments_begin = std::string_view::npos;
    int parenthesis_depth = 0;
    bool in_double_quote = false;
    for (; cursor < normalized.size(); ++cursor) {
        const char value = normalized[cursor];
        if (value == '"') {
            in_double_quote = !in_double_quote;
            continue;
        }
        if (in_double_quote) {
            continue;
        }
        if (value == '(') {
            ++parenthesis_depth;
            if (parenthesis_depth == 1) {
                arguments_begin = cursor + 1;
            }
        } else if (value == ')') {
            --parenthesis_depth;
            if (arguments_begin != std::string_view::npos
                && parenthesis_depth == 0) {
                const std::string name{
                    trim_view(normalized.substr(
                        name_begin, arguments_begin - name_begin - 1))};
                const std::string_view arguments = normalized.substr(
                    arguments_begin, cursor - arguments_begin);
                return routine_identity_from_argument_declarations(
                    name, arguments);
            }
        }
    }
    return std::nullopt;
}

[[nodiscard]] inline bool sql_word_boundary(
    std::string_view text,
    std::size_t offset) noexcept {
    if (offset >= text.size()) return true;
    const char value = text[offset];
    return !((value >= 'a' && value <= 'z')
        || (value >= 'A' && value <= 'Z')
        || (value >= '0' && value <= '9') || value == '_');
}

[[nodiscard]] inline std::size_t find_sql_keyword(
    std::string_view text,
    std::string_view keyword,
    std::size_t begin = 0,
    bool top_level_parentheses_only = false) {
    bool in_single_quote = false;
    bool in_double_quote = false;
    int parenthesis_depth = 0;
    std::optional<std::string> dollar_delimiter;
    for (std::size_t index = begin; index < text.size(); ++index) {
        const char value = text[index];
        const char next =
            index + 1 < text.size() ? text[index + 1] : '\0';
        if (dollar_delimiter) {
            if (text.substr(index).starts_with(*dollar_delimiter)) {
                index += dollar_delimiter->size() - 1;
                dollar_delimiter.reset();
            }
            continue;
        }
        if (in_single_quote) {
            if (value == '\'' && next == '\'') {
                ++index;
            } else if (value == '\'') {
                in_single_quote = false;
            }
            continue;
        }
        if (in_double_quote) {
            if (value == '"' && next == '"') {
                ++index;
            } else if (value == '"') {
                in_double_quote = false;
            }
            continue;
        }
        if (value == '\'') {
            in_single_quote = true;
            continue;
        }
        if (value == '"') {
            in_double_quote = true;
            continue;
        }
        if (value == '$') {
            if (const auto delimiter = read_dollar_delimiter(text, index)) {
                dollar_delimiter = delimiter;
                index += delimiter->size() - 1;
                continue;
            }
        }
        if (value == '(') {
            ++parenthesis_depth;
            continue;
        }
        if (value == ')') {
            --parenthesis_depth;
            continue;
        }
        if ((!top_level_parentheses_only || parenthesis_depth == 0)
            && index + keyword.size() <= text.size()
            && text.substr(index, keyword.size()) == keyword
            && (index == 0 || sql_word_boundary(text, index - 1))
            && sql_word_boundary(text, index + keyword.size())) {
            return index;
        }
    }
    return std::string_view::npos;
}

struct routine_head {
    std::string name;
    std::string arguments;
    std::size_t close_parenthesis{};
};

[[nodiscard]] inline std::optional<routine_head> parse_routine_head(
    statement_kind kind,
    std::string_view normalized) {
    if (kind != statement_kind::create_function
        && kind != statement_kind::create_procedure) {
        return std::nullopt;
    }
    const std::string_view marker =
        kind == statement_kind::create_function ? "function" : "procedure";
    const std::size_t marker_offset =
        find_sql_keyword(normalized, marker);
    if (marker_offset == std::string_view::npos) return std::nullopt;
    std::size_t cursor = marker_offset + marker.size();
    while (cursor < normalized.size() && ascii_space(normalized[cursor])) {
        ++cursor;
    }
    const std::size_t name_begin = cursor;
    bool in_double_quote = false;
    for (; cursor < normalized.size(); ++cursor) {
        const char value = normalized[cursor];
        if (value == '"') {
            in_double_quote = !in_double_quote;
        } else if (!in_double_quote && value == '(') {
            break;
        }
    }
    if (cursor == normalized.size()) return std::nullopt;
    const std::size_t arguments_begin = cursor + 1;
    int depth = 1;
    bool in_single_quote = false;
    in_double_quote = false;
    std::optional<std::string> dollar_delimiter;
    for (++cursor; cursor < normalized.size(); ++cursor) {
        const char value = normalized[cursor];
        const char next = cursor + 1 < normalized.size()
            ? normalized[cursor + 1]
            : '\0';
        if (dollar_delimiter) {
            if (normalized.substr(cursor).starts_with(*dollar_delimiter)) {
                cursor += dollar_delimiter->size() - 1;
                dollar_delimiter.reset();
            }
            continue;
        }
        if (in_single_quote) {
            if (value == '\'' && next == '\'') {
                ++cursor;
            } else if (value == '\'') {
                in_single_quote = false;
            }
            continue;
        }
        if (in_double_quote) {
            if (value == '"' && next == '"') {
                ++cursor;
            } else if (value == '"') {
                in_double_quote = false;
            }
            continue;
        }
        if (value == '\'') {
            in_single_quote = true;
        } else if (value == '"') {
            in_double_quote = true;
        } else if (value == '$') {
            if (const auto delimiter =
                    read_dollar_delimiter(normalized, cursor)) {
                dollar_delimiter = delimiter;
                cursor += delimiter->size() - 1;
            }
        } else if (value == '(') {
            ++depth;
        } else if (value == ')' && --depth == 0) {
            return routine_head{
                .name = std::string{trim_view(normalized.substr(
                    name_begin, arguments_begin - name_begin - 1))},
                .arguments = std::string{trim_view(normalized.substr(
                    arguments_begin, cursor - arguments_begin))},
                .close_parenthesis = cursor,
            };
        }
    }
    return std::nullopt;
}

[[nodiscard]] inline std::size_t next_routine_property(
    std::string_view normalized,
    std::size_t begin) {
    static constexpr std::string_view properties[] = {
        "as", "language", "transform", "window", "immutable", "stable",
        "volatile", "leakproof", "strict",
        "returns null on null input", "called on null input", "security",
        "parallel", "cost", "rows", "support", "set",
    };
    std::size_t result = std::string_view::npos;
    for (const std::string_view property : properties) {
        const std::size_t candidate =
            find_sql_keyword(normalized, property, begin, true);
        if (candidate != std::string_view::npos
            && (result == std::string_view::npos || candidate < result)) {
            result = candidate;
        }
    }
    return result;
}

[[nodiscard]] inline std::optional<std::string> quoted_sql_value(
    std::string_view text,
    std::size_t& cursor) {
    while (cursor < text.size()
           && (ascii_space(text[cursor]) || text[cursor] == ',')) {
        ++cursor;
    }
    if (cursor >= text.size()) return std::nullopt;
    if (text[cursor] == '\'') {
        std::string value;
        for (++cursor; cursor < text.size(); ++cursor) {
            if (text[cursor] == '\''
                && cursor + 1 < text.size()
                && text[cursor + 1] == '\'') {
                value.push_back('\'');
                ++cursor;
            } else if (text[cursor] == '\'') {
                ++cursor;
                return value;
            } else {
                value.push_back(text[cursor]);
            }
        }
        return std::nullopt;
    }
    if (text[cursor] == '$') {
        const auto delimiter = read_dollar_delimiter(text, cursor);
        if (!delimiter) return std::nullopt;
        const std::size_t body_begin = cursor + delimiter->size();
        const std::size_t close = text.find(*delimiter, body_begin);
        if (close == std::string_view::npos) return std::nullopt;
        cursor = close + delimiter->size();
        return std::string{text.substr(body_begin, close - body_begin)};
    }
    return std::nullopt;
}

[[nodiscard]] inline std::string routine_property_value(
    std::string_view normalized,
    std::string_view property,
    std::size_t begin) {
    const std::size_t offset =
        find_sql_keyword(normalized, property, begin, true);
    if (offset == std::string_view::npos) return "none";
    const std::size_t value_begin = offset + property.size();
    std::size_t value_end =
        next_routine_property(normalized, value_begin);
    if (value_end == std::string_view::npos) {
        value_end = normalized.rfind(';');
        if (value_end == std::string_view::npos) {
            value_end = normalized.size();
        }
    }
    const std::string_view value =
        trim_view(normalized.substr(value_begin, value_end - value_begin));
    return value.empty() ? "none" : std::string{value};
}

[[nodiscard]] inline std::optional<routine_metadata>
parse_routine_metadata(
    statement_kind kind,
    std::string_view normalized) {
    const std::optional<routine_head> head =
        parse_routine_head(kind, normalized);
    const std::optional<std::string> identity =
        routine_identity(kind, normalized);
    if (!head || !identity) return std::nullopt;

    const std::size_t properties_begin = head->close_parenthesis + 1;
    std::string returns = "void";
    if (kind == statement_kind::create_function) {
        const std::size_t returns_offset =
            find_sql_keyword(normalized, "returns", properties_begin, true);
        if (returns_offset == std::string_view::npos) {
            return std::nullopt;
        }
        const std::size_t value_begin =
            returns_offset + std::string_view{"returns"}.size();
        std::size_t value_end =
            next_routine_property(normalized, value_begin);
        if (value_end == std::string_view::npos) {
            return std::nullopt;
        }
        returns = std::string{trim_view(
            normalized.substr(value_begin, value_end - value_begin))};
    }

    const std::size_t language_offset =
        find_sql_keyword(normalized, "language", properties_begin, true);
    if (language_offset == std::string_view::npos) return std::nullopt;
    std::string_view language_tail = trim_view(normalized.substr(
        language_offset + std::string_view{"language"}.size()));
    const std::size_t language_end =
        language_tail.find_first_of(" \t\n\r;");
    const std::string language{language_tail.substr(0, language_end)};

    std::string c_symbol = "none";
    if (language == "c") {
        const std::size_t as_offset =
            find_sql_keyword(normalized, "as", properties_begin, true);
        if (as_offset == std::string_view::npos) return std::nullopt;
        std::size_t literal_cursor =
            as_offset + std::string_view{"as"}.size();
        const std::optional<std::string> module_path =
            quoted_sql_value(normalized, literal_cursor);
        if (!module_path
            || lower_copy(*module_path) != "module_pathname") {
            return std::nullopt;
        }
        if (const std::optional<std::string> explicit_symbol =
                quoted_sql_value(normalized, literal_cursor)) {
            c_symbol = *explicit_symbol;
        } else {
            std::string_view routine_name = head->name;
            const std::size_t separator = routine_name.rfind('.');
            if (separator != std::string_view::npos) {
                routine_name.remove_prefix(separator + 1);
            }
            c_symbol = std::string{routine_name};
            if (c_symbol.size() >= 2 && c_symbol.front() == '"'
                && c_symbol.back() == '"') {
                c_symbol =
                    c_symbol.substr(1, c_symbol.size() - 2);
            }
        }
    }

    const auto has = [normalized, properties_begin](
                         std::string_view property) {
        return find_sql_keyword(
            normalized, property, properties_begin, true)
            != std::string_view::npos;
    };
    const std::string volatility = has("immutable")
        ? "immutable"
        : has("stable") ? "stable" : "volatile";
    const std::string null_input =
        has("strict") || has("returns null on null input")
        ? "strict"
        : "called";
    const std::string security =
        has("security definer") ? "definer" : "invoker";
    const std::string parallel = has("parallel safe")
        ? "safe"
        : has("parallel restricted") ? "restricted" : "unsafe";

    std::string configuration = "none";
    const std::size_t set_offset =
        find_sql_keyword(normalized, "set", properties_begin, true);
    if (set_offset != std::string_view::npos) {
        std::size_t set_end = normalized.rfind(';');
        if (set_end == std::string_view::npos) set_end = normalized.size();
        configuration = std::string{trim_view(
            normalized.substr(set_offset, set_end - set_offset))};
    }
    return routine_metadata{
        .identity = *identity,
        .argument_declarations =
            head->arguments.empty() ? "none" : head->arguments,
        .returns = std::move(returns),
        .language = language,
        .c_symbol = std::move(c_symbol),
        .volatility = volatility,
        .null_input = null_input,
        .security = security,
        .parallel = parallel,
        .configuration = std::move(configuration),
        .cost = routine_property_value(
            normalized, "cost", properties_begin),
        .rows = routine_property_value(
            normalized, "rows", properties_begin),
        .support = routine_property_value(
            normalized, "support", properties_begin),
    };
}

}  // namespace detail

/// Parses semicolon-terminated top-level PostgreSQL statements while preserving
/// their exact source bytes.
[[nodiscard]] inline install_script parse_install_script(
    std::string_view source) {
    if (source.empty()) {
        throw std::runtime_error("install SQL is empty");
    }
    if (source.find('\0') != std::string_view::npos) {
        throw std::runtime_error("install SQL contains a NUL byte");
    }
    if (source.find('\r') != std::string_view::npos) {
        throw std::runtime_error("install SQL must use LF newlines");
    }
    const std::size_t guard_end = source.find('\n', source.find("\\echo"));
    if (!source.starts_with("-- complain if script is sourced in psql")
        || source.find("\\echo Use \"CREATE EXTENSION bioseq\"") ==
            std::string_view::npos
        || guard_end == std::string_view::npos) {
        throw std::runtime_error(
            "install SQL is missing the canonical psql direct-load guard");
    }

    install_script result{
        .preamble = std::string{source.substr(0, guard_end + 1)}};
    std::size_t statement_begin = guard_end + 1;
    bool in_single_quote = false;
    bool in_double_quote = false;
    bool in_line_comment = false;
    int block_comment_depth = 0;
    std::optional<std::string> dollar_delimiter;
    for (std::size_t index = statement_begin; index < source.size(); ++index) {
        const char value = source[index];
        const char next = index + 1 < source.size() ? source[index + 1] : '\0';
        if (in_line_comment) {
            if (value == '\n') {
                in_line_comment = false;
            }
            continue;
        }
        if (block_comment_depth > 0) {
            if (value == '/' && next == '*') {
                ++block_comment_depth;
                ++index;
            } else if (value == '*' && next == '/') {
                --block_comment_depth;
                ++index;
            }
            continue;
        }
        if (dollar_delimiter) {
            if (source.substr(index).starts_with(*dollar_delimiter)) {
                index += dollar_delimiter->size() - 1;
                dollar_delimiter.reset();
            }
            continue;
        }
        if (in_single_quote) {
            if (value == '\'' && next == '\'') {
                ++index;
            } else if (value == '\'') {
                in_single_quote = false;
            }
            continue;
        }
        if (in_double_quote) {
            if (value == '"' && next == '"') {
                ++index;
            } else if (value == '"') {
                in_double_quote = false;
            }
            continue;
        }
        if (value == '-' && next == '-') {
            in_line_comment = true;
            ++index;
            continue;
        }
        if (value == '/' && next == '*') {
            block_comment_depth = 1;
            ++index;
            continue;
        }
        if (value == '\'') {
            in_single_quote = true;
            continue;
        }
        if (value == '"') {
            in_double_quote = true;
            continue;
        }
        if (value == '$') {
            if (const auto delimiter =
                    detail::read_dollar_delimiter(source, index)) {
                dollar_delimiter = delimiter;
                index += delimiter->size() - 1;
                continue;
            }
        }
        if (value != ';') {
            continue;
        }

        const std::size_t statement_end = index + 1;
        const std::string_view exact =
            source.substr(statement_begin, statement_end - statement_begin);
        const std::string normalized = detail::normalize_sql(exact);
        if (!normalized.empty()) {
            const statement_kind kind =
                detail::classify_statement(normalized);
            result.statements.push_back(sql_statement{
                .physical_ordinal = result.statements.size() + 1,
                .kind = kind,
                .span = {
                    .byte_begin = statement_begin,
                    .byte_end = statement_end,
                    .line_begin =
                        detail::line_number_at(source, statement_begin),
                    .line_end =
                        detail::line_number_at(source, statement_end),
                },
                .text = std::string{exact},
                .normalized_text = normalized,
                .normalized_hash = sha256_hex(normalized),
                .routine_identity =
                    detail::routine_identity(kind, normalized),
                .routine =
                    detail::parse_routine_metadata(kind, normalized),
                .create_or_replace =
                    normalized.starts_with("create or replace function")
                    || normalized.starts_with(
                        "create or replace procedure")
                    || normalized.starts_with(
                        "create or replace view"),
            });
        }
        statement_begin = statement_end;
    }
    if (in_single_quote || in_double_quote || dollar_delimiter
        || block_comment_depth != 0) {
        throw std::runtime_error(
            "install SQL ends inside a quoted string or comment");
    }
    result.trailing_text = std::string{source.substr(statement_begin)};
    if (!detail::trim_view(result.trailing_text).empty()) {
        throw std::runtime_error(
            "install SQL contains a non-comment fragment after its last semicolon");
    }
    return result;
}

/// Validates the release install-script invariants enforced before rendering.
inline void validate_install_script(install_script& script) {
    if (script.statements.empty()) {
        throw std::runtime_error("install SQL contains no statements");
    }
    std::map<std::string, std::size_t, std::less<>> routine_owners;
    for (std::size_t index = 0; index < script.statements.size(); ++index) {
        sql_statement& statement = script.statements[index];
        if (!statement.routine_identity) continue;
        const auto previous = routine_owners.find(
            *statement.routine_identity);
        if (previous == routine_owners.end()) {
            routine_owners.emplace(*statement.routine_identity, index);
            continue;
        }
        if (!statement.create_or_replace) {
            throw std::runtime_error(
                "duplicate routine owner '" + *statement.routine_identity
                + "' at physical statements "
                + std::to_string(
                    script.statements[previous->second].physical_ordinal)
                + " and "
                + std::to_string(statement.physical_ordinal)
                + "; only a later CREATE OR REPLACE may supersede a "
                  "baseline definition");
        }
        sql_statement& superseded = script.statements[previous->second];
        superseded.disposition =
            statement_disposition::superseded_by_later_create_or_replace;
        superseded.superseded_by_physical_ordinal =
            statement.physical_ordinal;
        previous->second = index;
    }
    for (const sql_statement& statement : script.statements) {
        if (statement.disposition != statement_disposition::retained) {
            continue;
        }
        if ((statement.kind == statement_kind::create_function
             || statement.kind == statement_kind::create_procedure)
            && !statement.routine) {
            throw std::runtime_error(
                "cannot derive complete typed routine metadata at statement "
                + std::to_string(statement.physical_ordinal));
        }
        const std::string& normalized = statement.normalized_text;
        if (normalized.find("language plpgsql") != std::string::npos) {
            throw std::runtime_error(
                "production PL/pgSQL is forbidden at statement "
                + std::to_string(statement.physical_ordinal));
        }
        if (normalized.starts_with("do $")
            || normalized.starts_with("do\n$")) {
            throw std::runtime_error(
                "anonymous procedural SQL is forbidden at statement "
                + std::to_string(statement.physical_ordinal));
        }
        if (statement.routine
            && statement.routine->language != "c"
            && statement.routine->language != "sql") {
            throw std::runtime_error(
                "unsupported routine language '"
                + statement.routine->language + "' at statement "
                + std::to_string(statement.physical_ordinal));
        }
        if (statement.routine
            && statement.routine->language == "c"
            && statement.routine->c_symbol == "none") {
            throw std::runtime_error(
                "C routine is missing its exact link symbol at statement "
                + std::to_string(statement.physical_ordinal));
        }
        if (statement.kind == statement_kind::unknown) {
            throw std::runtime_error(
                "unsupported top-level SQL statement at ordinal "
                + std::to_string(statement.physical_ordinal));
        }
    }
}

}  // namespace bioseq::codegen::sql
