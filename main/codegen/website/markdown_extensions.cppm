module;

#include <chrono>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module bioseq.codegen.website:markdown_extensions;

import bioseq.codegen.website.model;
import bioseq.codegen.support;

// Bioseq Markdown extensions over MD4C's typed events (§10.3-10.7): admonition
// classification, entity-reference parsing (both `[[sql:...]]` wikilinks and
// `entity:` links), and the runnable-SQL fence grammar. Source-neutral helpers
// shared by the markdown driver and the tutorial planner.
namespace bioseq::codegen::website {

/// @brief Maps an admonition marker keyword to its rendered severity class.
[[nodiscard]] std::string admonition_class(std::string_view keyword) {
    if (keyword == "NOTE") return "note";
    if (keyword == "TIP") return "tip";
    if (keyword == "IMPORTANT") return "important";
    if (keyword == "WARNING") return "warning";
    if (keyword == "CAUTION") return "caution";
    return {};
}

/// @brief Parses an entity-reference target into a typed query (§10.4).
///
/// Accepts an optional `entity:` prefix (link form) and a bare domain prefix
/// (`sql:`/`cpp:`/`id:`) as produced by `[[sql:...]]` wikilinks. A trailing
/// `|label` is stripped and returned as the query label.
/// @param spelling Raw target such as `sql:bioseq.seq|the sequence type`.
/// @param span Source span for diagnostics.
/// @return The typed query, or nullopt when the target is an ordinary link.
[[nodiscard]] std::optional<unresolved_entity_query> parse_entity_reference(
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
        query.label = std::string(trim(spelling.substr(bar + 1)));
        spelling = spelling.substr(0, bar);
    }
    query.spelling = std::string(trim(spelling));
    if (query.spelling.empty()) {
        return std::nullopt;
    }
    return query;
}

/// @brief Parsed attributes of a runnable SQL fence info string (§10.7).
struct runnable_fence_attributes {
    std::string local_id;
    std::chrono::milliseconds timeout{15'000};
    sql_expectation expectation{sql_expectation::success};
    std::optional<std::string> expected_sqlstate;
};

namespace {

[[nodiscard]] std::vector<std::string_view> split_whitespace(std::string_view text) {
    std::vector<std::string_view> tokens;
    std::size_t index = 0;
    while (index < text.size()) {
        while (index < text.size()
               && (text[index] == ' ' || text[index] == '\t')) {
            ++index;
        }
        const std::size_t begin = index;
        while (index < text.size()
               && text[index] != ' ' && text[index] != '\t') {
            ++index;
        }
        if (index > begin) {
            tokens.push_back(text.substr(begin, index - begin));
        }
    }
    return tokens;
}

[[nodiscard]] std::chrono::milliseconds parse_timeout(std::string_view value) {
    // Unsigned decimal integer followed by ms or s; 100ms-60s (§10.7).
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

}  // namespace

/// @brief Parses a runnable-SQL fence info string, or nullopt when not runnable.
/// @param info Full fence info such as `sql runnable id=x timeout=10s`.
/// @return Validated attributes, or nullopt for a non-runnable code fence.
[[nodiscard]] std::optional<runnable_fence_attributes> parse_runnable_fence(
    std::string_view info) {
    const std::vector<std::string_view> tokens = split_whitespace(info);
    if (tokens.size() < 2 || tokens[0] != "sql" || tokens[1] != "runnable") {
        return std::nullopt;
    }
    runnable_fence_attributes attributes;
    bool has_id = false;
    bool has_expect = false;
    bool has_sqlstate = false;
    for (std::size_t index = 2; index < tokens.size(); ++index) {
        const std::string_view token = tokens[index];
        const std::size_t equals = token.find('=');
        if (equals == std::string_view::npos) {
            throw std::runtime_error("runnable fence attribute must be key=value: "
                                     + std::string(token));
        }
        const std::string_view key = token.substr(0, equals);
        const std::string_view value = token.substr(equals + 1);
        if (key == "id") {
            attributes.local_id = std::string(value);
            has_id = true;
        } else if (key == "timeout") {
            attributes.timeout = parse_timeout(value);
        } else if (key == "expect") {
            if (value == "success") {
                attributes.expectation = sql_expectation::success;
            } else if (value == "error") {
                attributes.expectation = sql_expectation::error;
            } else {
                throw std::runtime_error("runnable expect must be success or error");
            }
            has_expect = true;
        } else if (key == "sqlstate") {
            if (!is_valid_sqlstate(value)) {
                throw std::runtime_error("runnable sqlstate must be 5 uppercase "
                                         "alphanumerics other than 00000");
            }
            attributes.expected_sqlstate = std::string(value);
            has_sqlstate = true;
        } else {
            throw std::runtime_error("unknown runnable fence attribute: "
                                     + std::string(key));
        }
    }
    if (!has_id || attributes.local_id.empty()) {
        throw std::runtime_error("runnable fence requires a non-empty id");
    }
    const bool expects_error =
        has_expect && attributes.expectation == sql_expectation::error;
    if (expects_error && !has_sqlstate) {
        throw std::runtime_error("expect=error requires a sqlstate attribute");
    }
    if (!expects_error && has_sqlstate) {
        throw std::runtime_error("sqlstate is only valid with expect=error");
    }
    return attributes;
}

/// @brief The setup/visible/teardown text of a runnable example (§10.7).
struct sql_phase_bodies {
    std::string setup;
    std::string visible;
    std::string teardown;
};

/// @brief Splits fenced SQL into phases on the exact marker lines (§10.7).
///
/// Each of `-- bioseq:setup`, `-- bioseq:visible`, `-- bioseq:teardown` must
/// occur exactly once, alone on a line, in order. Markers are removed and each
/// phase is trimmed of surrounding blank lines.
/// @param code Raw fenced SQL body.
/// @return The three phase bodies.
[[nodiscard]] sql_phase_bodies split_sql_phases(std::string_view code) {
    constexpr std::string_view markers[] = {
        "-- bioseq:setup", "-- bioseq:visible", "-- bioseq:teardown"};
    int phase = -1;
    std::string bodies[3];
    bool seen[3] = {false, false, false};
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
            if (seen[matched] || matched != phase + 1) {
                throw std::runtime_error(
                    "runnable SQL requires setup/visible/teardown markers exactly "
                    "once in order");
            }
            seen[matched] = true;
            phase = matched;
        } else if (phase >= 0) {
            bodies[phase].append(line);
            bodies[phase].push_back('\n');
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
    return sql_phase_bodies{
        .setup = trim(bodies[0]),
        .visible = trim(bodies[1]),
        .teardown = trim(bodies[2])};
}

}  // namespace bioseq::codegen::website
