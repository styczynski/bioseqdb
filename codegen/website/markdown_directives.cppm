module;

#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module bioseq.codegen.website:markdown_directives;

import bioseq.codegen.website.model;
import bioseq.codegen.support;

// Bioseq Markdown extensions over MD4C's typed events: admonition
// classification, entity-reference parsing (both `[[sql:...]]` wikilinks and
// `entity:` links), and the runnable-SQL fence grammar. Source-neutral helpers
// shared by the markdown driver and the tutorial planner.
namespace bioseq::codegen::website {

/// @brief Maps an admonition marker keyword to its rendered severity class.
///
/// Accepts the keyword in either spelling the two producers use: the marker as
/// authored (`> [!WARNING]`) and the already-normalized lowercase tag MD4C
/// reports in MD_BLOCK_ADMONITION_DETAIL::type ("warning"). Matching only one
/// case silently classifies the other as unknown, which renders every severity
/// as the "note" fallback, so the comparison is deliberately case-insensitive.
/// @param keyword Marker keyword in any case.
/// @return The severity class, or empty for a keyword outside the five known
///         severities.
[[nodiscard]] std::string admonition_class(std::string_view keyword) {
    std::string normalized(keyword);
    for (char& character : normalized) {
        character = static_cast<char>(
            std::tolower(static_cast<unsigned char>(character)));
    }
    if (normalized == "note") return "note";
    if (normalized == "tip") return "tip";
    if (normalized == "important") return "important";
    if (normalized == "warning") return "warning";
    if (normalized == "caution") return "caution";
    return {};
}

/// @brief Parses an entity-reference target into a typed query.
///
/// Accepts an optional `entity:` prefix (link form) and a bare domain prefix
/// (`sql:`/`cpp:`/`id:`) as produced by `[[sql:...]]` wikilinks. A trailing
/// `|label` is stripped and returned as the query label.
/// @param spelling Raw target such as `sql:bioseq.seq|the sequence type`.
/// @param span Source span for diagnostics.
/// @return The typed query, or nullopt when the target is an ordinary link.
[[nodiscard]] std::optional<unresolved_entity_query> parse_entity_query(
    std::string_view spelling,
    source_span span) {
    if (spelling.starts_with("entity:")) {
        spelling.remove_prefix(std::string_view{"entity:"}.size());
    }
    unresolved_entity_query query;
    query.source = std::move(span);
    if (spelling.starts_with("sql:")) {
        query.domain = entity_query_domain::sql;
        spelling.remove_prefix(4);
    } else if (spelling.starts_with("cpp:")) {
        query.domain = entity_query_domain::cpp;
        spelling.remove_prefix(4);
    } else if (spelling.starts_with("id:")) {
        query.domain = entity_query_domain::canonical_id;
        spelling.remove_prefix(3);
    } else {
        return std::nullopt;  // ordinary link/wikilink, not an entity reference
    }
    const std::size_t bar = spelling.find('|');
    if (bar != std::string_view::npos) {
        query.label = std::string(trim_ascii_whitespace(spelling.substr(bar + 1)));
        spelling = spelling.substr(0, bar);
    }
    query.spelling = std::string(trim_ascii_whitespace(spelling));
    if (query.spelling.empty()) {
        return std::nullopt;
    }
    return query;
}

/// @brief Parsed attributes of a runnable SQL fence info string.
struct runnable_fence_attributes {
    std::string local_id;
    std::chrono::milliseconds timeout{15'000};
    sql_expectation expectation{sql_expectation::success};
    std::optional<std::string> expected_sqlstate;
};

namespace {

[[nodiscard]] std::chrono::milliseconds parse_timeout(std::string_view value) {
    // Unsigned decimal integer followed by ms or s; 100ms-60s.
    std::size_t digits = 0;
    while (digits < value.size() && value[digits] >= '0' && value[digits] <= '9') {
        ++digits;
    }
    if (digits == 0 || digits > 9) {
        throw std::runtime_error("runnable timeout must be <number>ms|s: "
                                 + std::string(value));
    }
    const std::string_view unit = value.substr(digits);
    unsigned long long magnitude = 0;
    for (std::size_t index = 0; index < digits; ++index) {
        magnitude = magnitude * 10 + static_cast<unsigned>(value[index] - '0');
    }
    unsigned long long millis = 0;
    if (unit == "ms") {
        millis = magnitude;
    } else if (unit == "s") {
        millis = magnitude * 1000;
    } else {
        throw std::runtime_error("runnable timeout unit must be ms or s: "
                                 + std::string(value));
    }
    if (millis < 100 || millis > 60'000) {
        throw std::runtime_error("runnable timeout must be 100ms-60s: "
                                 + std::string(value));
    }
    return std::chrono::milliseconds{static_cast<long long>(millis)};
}

[[nodiscard]] bool is_valid_sqlstate(std::string_view value) {
    if (value.size() != 5 || value == "00000") {
        return false;
    }
    for (const char character : value) {
        const bool ok = (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9');
        if (!ok) {
            return false;
        }
    }
    return true;
}

// One token of a fenced info string: the head language/kind word, a `key=value`
// attribute, or a bare boolean flag.
struct info_token {
    std::string key;                   // head word, attribute key, or flag name
    std::optional<std::string> value;  // present only for key=value
    bool head = false;                 // true for the first (language/kind) token
};

// Tokenizes a fenced info string honoring double-quoted values with \\ \" \t \n
// escapes; bare values end at ASCII whitespace. A quote may only open a
// value (immediately after `=`); a stray or unterminated quote is fatal, so a
// signature containing commas/spaces can be carried verbatim as one value.
[[nodiscard]] std::vector<info_token> tokenize_info(std::string_view info) {
    std::vector<info_token> tokens;
    std::size_t index = 0;
    const auto skip_ws = [info, &index]() -> void {
        while (index < info.size() && (info[index] == ' ' || info[index] == '\t')) {
            ++index;
        }
    };
    skip_ws();
    bool first = true;
    while (index < info.size()) {
        std::string key;
        while (index < info.size() && info[index] != ' ' && info[index] != '\t'
               && info[index] != '=' && info[index] != '"') {
            key.push_back(info[index]);
            ++index;
        }
        if (index < info.size() && info[index] == '"') {
            throw std::runtime_error(
                "fence info: a '\"' may only follow an attribute '='");
        }
        info_token token;
        token.head = first;
        token.key = std::move(key);
        if (index < info.size() && info[index] == '=') {
            ++index;  // consume '='
            std::string value;
            if (index < info.size() && info[index] == '"') {
                ++index;  // opening quote
                bool closed = false;
                while (index < info.size()) {
                    const char character = info[index++];
                    if (character == '"') {
                        closed = true;
                        break;
                    }
                    if (character == '\\') {
                        if (index >= info.size()) {
                            break;
                        }
                        const char escaped = info[index++];
                        switch (escaped) {
                        case '\\': value.push_back('\\'); break;
                        case '"': value.push_back('"'); break;
                        case 't': value.push_back('\t'); break;
                        case 'n': value.push_back('\n'); break;
                        default:
                            throw std::runtime_error("fence info: invalid escape \\"
                                                     + std::string(1, escaped));
                        }
                    } else {
                        value.push_back(character);
                    }
                }
                if (!closed) {
                    throw std::runtime_error("fence info: unterminated quoted value");
                }
            } else {
                while (index < info.size() && info[index] != ' ' && info[index] != '\t') {
                    value.push_back(info[index]);
                    ++index;
                }
            }
            token.value = std::move(value);
        }
        tokens.push_back(std::move(token));
        first = false;
        skip_ws();
    }
    return tokens;
}

}  // namespace

/// @brief Closed classification of a fenced code block.
enum class fence_kind { ordinary, annotation, runnable };

/// @brief Parsed, validated attributes of any fenced block's info string.
struct fence_descriptor {
    fence_kind kind{fence_kind::ordinary};
    std::string language;               // head word (ordinary/runnable)
    std::string id;                     // optional id (ordinary/runnable)
    bool annotated{false};
    std::vector<std::string> entities;  // repeatable entity="<canonical-id>"
    std::chrono::milliseconds timeout{15'000};
    sql_expectation expectation{sql_expectation::success};
    std::optional<std::string> expected_sqlstate;
    std::string target;                 // annotation directive
    std::string match;
    std::string title;
    std::optional<std::size_t> occurrence;
};

/// @brief Parses any fenced info string against the closed attribute table.
///
/// Duplicate scalar attributes and unknown attributes/flags are fatal; `entity=` is
/// the one repeatable attribute; quoted values may carry commas/spaces (an overload
/// id or a C++ signature) verbatim. `runnable` is legal only on `sql`; `sqlstate`
/// only with `expect=error`. The head word selects the branch: `annotation` is the
/// directive, any other word is a display language (runnable when a `runnable` flag
/// is present).
[[nodiscard]] fence_descriptor parse_fence_info(std::string_view info) {
    const std::vector<info_token> tokens = tokenize_info(info);
    if (tokens.empty() || tokens.front().value || tokens.front().key.empty()) {
        throw std::runtime_error("fenced block requires a leading language or directive");
    }
    fence_descriptor descriptor;
    std::vector<std::string_view> seen;
    const auto once = [&seen](std::string_view key) -> void {
        for (const std::string_view existing : seen) {
            if (existing == key) {
                throw std::runtime_error("duplicate fence attribute '"
                                         + std::string(key) + "'");
            }
        }
        seen.push_back(key);
    };
    const std::string& head = tokens.front().key;

    if (head == "annotation") {
        descriptor.kind = fence_kind::annotation;
        for (std::size_t index = 1; index < tokens.size(); ++index) {
            const info_token& token = tokens[index];
            if (!token.value) {
                throw std::runtime_error("annotation directive takes no flag '"
                                         + token.key + "'");
            }
            once(token.key);
            if (token.key == "target") {
                descriptor.target = *token.value;
            } else if (token.key == "match") {
                descriptor.match = *token.value;
            } else if (token.key == "title") {
                descriptor.title = *token.value;
            } else if (token.key == "occurrence") {
                const std::string& text = *token.value;
                std::size_t value = 0;
                if (text.empty() || text.size() > 6) {
                    throw std::runtime_error("annotation occurrence must be a positive integer");
                }
                for (const char character : text) {
                    if (character < '0' || character > '9') {
                        throw std::runtime_error("annotation occurrence must be a positive integer");
                    }
                    value = value * 10 + static_cast<std::size_t>(character - '0');
                }
                if (value == 0) {
                    throw std::runtime_error("annotation occurrence is 1-based (>= 1)");
                }
                descriptor.occurrence = value;
            } else {
                throw std::runtime_error("annotation directive: unknown attribute '"
                                         + token.key + "'");
            }
        }
        if (descriptor.target.empty() || descriptor.match.empty()
            || descriptor.title.empty()) {
            throw std::runtime_error("annotation directive requires target, match, and title");
        }
        return descriptor;
    }

    descriptor.language = head;
    bool runnable = false;
    for (std::size_t index = 1; index < tokens.size(); ++index) {
        if (!tokens[index].value && tokens[index].key == "runnable") {
            runnable = true;
        }
    }
    bool has_expect = false;
    bool has_sqlstate = false;
    for (std::size_t index = 1; index < tokens.size(); ++index) {
        const info_token& token = tokens[index];
        if (token.key == "entity") {  // the one repeatable attribute
            if (!token.value || token.value->empty()) {
                throw std::runtime_error("entity=\"<canonical-id>\" requires a quoted id");
            }
            descriptor.entities.push_back(*token.value);
            continue;
        }
        once(token.key);
        if (token.value) {
            if (token.key == "id") {
                descriptor.id = *token.value;
            } else if (runnable && token.key == "timeout") {
                descriptor.timeout = parse_timeout(*token.value);
            } else if (runnable && token.key == "expect") {
                if (*token.value == "success") {
                    descriptor.expectation = sql_expectation::success;
                } else if (*token.value == "error") {
                    descriptor.expectation = sql_expectation::error;
                } else {
                    throw std::runtime_error("runnable expect must be success or error");
                }
                has_expect = true;
            } else if (runnable && token.key == "sqlstate") {
                if (!is_valid_sqlstate(*token.value)) {
                    throw std::runtime_error("runnable sqlstate must be 5 uppercase "
                                             "alphanumerics other than 00000");
                }
                descriptor.expected_sqlstate = *token.value;
                has_sqlstate = true;
            } else {
                throw std::runtime_error("unknown fence attribute '" + token.key + "'");
            }
        } else {
            if (token.key == "runnable") {
                descriptor.kind = fence_kind::runnable;
            } else if (token.key == "annotated") {
                descriptor.annotated = true;
            } else {
                throw std::runtime_error("unknown fence flag '" + token.key + "'");
            }
        }
    }
    if (runnable) {
        descriptor.kind = fence_kind::runnable;
        if (descriptor.language != "sql") {
            throw std::runtime_error("runnable is only valid on a sql block");
        }
        if (descriptor.id.empty()) {
            throw std::runtime_error("runnable fence requires a non-empty id");
        }
        const bool expects_error =
            has_expect && descriptor.expectation == sql_expectation::error;
        if (expects_error && !has_sqlstate) {
            throw std::runtime_error("expect=error requires a sqlstate attribute");
        }
        if (!expects_error && has_sqlstate) {
            throw std::runtime_error("sqlstate is only valid with expect=error");
        }
    }
    return descriptor;
}

/// @brief Parses a runnable-SQL fence info string, or nullopt when not runnable.
/// @param info Full fence info such as `sql runnable id=x timeout=10s`.
/// @return Validated attributes, or nullopt for a non-runnable code fence.
[[nodiscard]] std::optional<runnable_fence_attributes> parse_runnable_sql_fence(
    std::string_view info) {
    const fence_descriptor descriptor = parse_fence_info(info);
    if (descriptor.kind != fence_kind::runnable) {
        return std::nullopt;
    }
    return runnable_fence_attributes{
        .local_id = descriptor.id,
        .timeout = descriptor.timeout,
        .expectation = descriptor.expectation,
        .expected_sqlstate = descriptor.expected_sqlstate};
}

/// @brief The setup/visible/teardown text of a runnable example.
struct sql_example_sections {
    std::string setup;
    std::string visible;
    std::string teardown;
};

/// @brief Splits fenced SQL into sections on the exact marker lines.
///
/// Each of `-- bioseq:setup`, `-- bioseq:visible`, `-- bioseq:teardown` must
/// occur exactly once, alone on a line, in order. Markers are removed and each
/// section is trimmed of surrounding blank lines.
/// @param code Raw fenced SQL body.
/// @return The three SQL sections.
[[nodiscard]] sql_example_sections parse_sql_example_sections(std::string_view code) {
    constexpr auto markers = std::to_array<std::string_view>({
        "-- bioseq:setup", "-- bioseq:visible", "-- bioseq:teardown"});
    int active_section = -1;
    std::array<std::string, 3> sections;
    std::array seen{false, false, false};
    std::size_t position = 0;
    while (position <= code.size()) {
        std::size_t newline = code.find('\n', position);
        const std::string_view line = code.substr(
            position, (newline == std::string_view::npos ? code.size() : newline) - position);
        std::string_view trimmed = line;
        while (!trimmed.empty() && (trimmed.back() == '\r' || trimmed.back() == ' '
                                    || trimmed.back() == '\t')) {
            trimmed.remove_suffix(1);
        }
        int matched = -1;
        for (int index = 0; index < 3; ++index) {
            if (trimmed == markers[index]) {
                matched = index;
                break;
            }
        }
        if (matched >= 0) {
            if (seen[matched] || matched != active_section + 1) {
                throw std::runtime_error(
                    "runnable SQL requires setup/visible/teardown markers exactly "
                    "once in order");
            }
            seen[matched] = true;
            active_section = matched;
        } else if (active_section >= 0) {
            sections[active_section].append(line);
            sections[active_section].push_back('\n');
        }
        if (newline == std::string_view::npos) {
            break;
        }
        position = newline + 1;
    }
    if (!seen[0] || !seen[1] || !seen[2]) {
        throw std::runtime_error(
            "runnable SQL requires all three bioseq markers");
    }
    return sql_example_sections{
        .setup = trim_ascii_whitespace(sections[0]),
        .visible = trim_ascii_whitespace(sections[1]),
        .teardown = trim_ascii_whitespace(sections[2])};
}

}  // namespace bioseq::codegen::website
