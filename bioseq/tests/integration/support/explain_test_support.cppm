/**
 * @file
 * @brief Provides shared EXPLAIN fixtures, parsers, and plan assertions.
 */
module;
#include <libpq-fe.h>
#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

export module bioseq.tests.integration.explain_test_support;

import std;
import bioseq.tests.integration.support;

export namespace bioseq_int_tests::explain_test_support {

/** Table used by the shared EXPLAIN fixtures. */
inline constexpr std::string_view k_table = "explain_sequences";

/** Return the canonical sequence-search statement used by planner checks. */
std::string explain_target_sql() {
    return fmt::format("SELECT pk, seq0 FROM {} WHERE mmsearch(seq0, "
                       "'TTTTAAAAGGGGCCCC'::bioseq.seq('dna4', 32));",
                       k_table);
}

/** Return the canonical statement that exposes every Highway stage. */
std::string explain_highway_target_sql() {
    return fmt::format("SELECT pk, seq0, bioseq.edit(pk) AS edit_distance FROM {} "
                       "WHERE mmsearch(seq0, "
                       "'TTTTAAAAGGGGCCCC'::bioseq.seq('dna4', 32));",
                       k_table);
}

/** Select deterministic Highway algorithms without Vulkan execution. */
void configure_highway_simd_pipeline(const connection_test_context& ctx) {
    exec_sql(ctx, "SET max_parallel_workers_per_gather = 0;");
    exec_sql(ctx, "SET bioseq.index.sequence_read.enabled = off;");
    exec_sql(ctx, "SET bioseq.vectorization.enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan.enabled = off;");
    exec_sql(ctx, "SET bioseq.aligner.minimizer.algorithm = 'two_stack_nthash';");
    exec_sql(ctx, "SET bioseq.aligner.chaining.algorithm = 'simd_minimap2';");
    exec_sql(ctx, "SET bioseq.aligner.base_alignment.algorithm = 'block_aligner';");
    // k=15,w=10 requires at least 24 bp to emit a minimizer. Callers that
    // require a nonempty result seed a qualifying exact probe; admit its real
    // chain while keeping the statement parameters frozen.
    exec_sql(ctx, "SET bioseq.aligner.chaining.minimum_score = 10;");
    exec_sql(ctx, "SET bioseq.aligner.chaining.minimum_seed_count = 1;");
}

/** Expected function and stage pair in structured Highway plan text. */
struct expected_highway_stage_function {
    /** Planned function identity. */
    std::string_view function;
    /** Pipeline stage identity. */
    std::string_view stage;
};

/** Require exactly one planned function for every canonical Highway stage. */
void require_complete_generic_highway_pipeline(std::span<const std::string> plan) {
    constexpr std::array expected{
        expected_highway_stage_function{"minimizer::two_stack_nthash", "QuerySketchStage"},
        expected_highway_stage_function{"chaining::chain_anchors_simd", "chain_stage"},
        expected_highway_stage_function{"alignment::block_aligner", "align_stage"},
    };
    std::array<bool, expected.size()> seen{};
    std::size_t planned_function_count = 0;
    for (const std::string& line : plan) {
        if (line.find("Highway Function:") == std::string::npos ||
            line.find("* Highway Function:") != std::string::npos ||
            line.find("[stage:") == std::string::npos) {
            continue;
        }
        ++planned_function_count;
        bool matched = false;
        for (std::size_t index = 0; index < expected.size(); ++index) {
            const auto& item = expected[index];
            if (line.find(fmt::format("Highway Function: {}", item.function)) ==
                    std::string::npos ||
                line.find(fmt::format("[stage: {}]", item.stage)) == std::string::npos) {
                continue;
            }
            REQUIRE_FALSE(seen[index]);
            seen[index] = true;
            matched = true;
            break;
        }
        REQUIRE(matched);
        REQUIRE(line.find("[planned architecture:") != std::string::npos);
    }
    REQUIRE(planned_function_count == expected.size());
    for (const bool was_seen : seen)
        REQUIRE(was_seen);
}

/** Execute EXPLAIN with caller-provided options and return its textual rows. */
std::vector<std::string> explain_with_options(const connection_test_context& ctx,
                                              std::string_view options,
                                              std::string_view sql) {
    auto rows = query_rows(ctx, fmt::format("EXPLAIN ({}) {}", options, sql));
    std::vector<std::string> lines;
    lines.reserve(rows.size());
    for (auto& row : rows) {
        REQUIRE(row.size() == 1);
        REQUIRE(row.front().has_value());
        lines.push_back(std::move(*row.front()));
    }
    return lines;
}

/** Read one actual counter from a matching materialization boundary. */
std::optional<unsigned long long> boundary_actual_metric(std::span<const std::string> plan,
                                                         std::string_view mechanism,
                                                         std::string_view reason,
                                                         std::string_view property) {
    bool mechanism_matches = false;
    bool reason_matches = false;
    const auto property_marker = fmt::format("{}:", property);
    for (const auto& line : plan) {
        if (line.find("var_") != std::string::npos && line.find(" = ") != std::string::npos) {
            mechanism_matches = false;
            reason_matches = false;
        }
        if (line.find(fmt::format("Mechanism: {}", mechanism)) != std::string::npos) {
            mechanism_matches = true;
        }
        if (line.find(fmt::format("Reason: {}", reason)) != std::string::npos) {
            reason_matches = true;
        }
        if (mechanism_matches && reason_matches) {
            const auto marker = line.find(property_marker);
            if (marker != std::string::npos) {
                return std::stoull(line.substr(marker + property_marker.size()));
            }
        }
    }
    return std::nullopt;
}

/** Read actual calls from a matching materialization boundary. */
std::optional<unsigned long long> boundary_actual_calls(std::span<const std::string> plan,
                                                        std::string_view mechanism,
                                                        std::string_view reason) {
    return boundary_actual_metric(plan, mechanism, reason, "Actual Calls");
}

/** Read actual bytes from a matching materialization boundary. */
std::optional<unsigned long long> boundary_actual_bytes(std::span<const std::string> plan,
                                                        std::string_view mechanism,
                                                        std::string_view reason) {
    return boundary_actual_metric(plan, mechanism, reason, "Actual Bytes");
}

/** Extract stable slice-recipe and boundary properties from plan text. */
std::vector<std::string> planned_slice_signature(std::span<const std::string> plan) {
    std::vector<std::string> signature;
    for (const auto& line : plan) {
        const bool recipe_value =
            line.find("value_") != std::string::npos && line.find(" = ") != std::string::npos;
        const bool boundary_value =
            line.find("var_") != std::string::npos && line.find(" = ") != std::string::npos;
        const bool planned_property = line.find("Mechanism:") != std::string::npos ||
                                      line.find("Reason:") != std::string::npos ||
                                      line.find("Depends On:") != std::string::npos ||
                                      line.find("Encoding:") != std::string::npos ||
                                      line.find("Cache Scope:") != std::string::npos;
        if (!recipe_value && !boundary_value && !planned_property) {
            continue;
        }
        const auto first = line.find_first_not_of(" \t");
        signature.push_back(first == std::string::npos ? std::string{} : line.substr(first));
    }
    return signature;
}

/** Structured identity of one planned materialization boundary. */
struct materialization_signature {
    /** Planner variable assigned to the boundary. */
    std::string variable;
    /** Storage mechanism reported by EXPLAIN. */
    std::string mechanism;
    /** Boundary reason reported by EXPLAIN. */
    std::string reason;
    /** Planner variables consumed by the boundary. */
    std::string depends_on;
    /** Optional producer identifier. */
    std::optional<unsigned long long> producer_value;
};

/** Parse every planned materialization boundary from EXPLAIN text. */
std::vector<materialization_signature>
materialization_signatures(std::span<const std::string> plan) {
    std::vector<materialization_signature> result;
    materialization_signature current;
    auto property = [](const std::string& line,
                       std::string_view name) -> std::optional<std::string> {
        const auto marker = fmt::format("{}:", name);
        const auto position = line.find(marker);
        if (position == std::string::npos) {
            return std::nullopt;
        }
        const auto value_start = line.find_first_not_of(" \t", position + marker.size());
        return value_start == std::string::npos ? std::string{} : line.substr(value_start);
    };
    for (const auto& line : plan) {
        const auto variable_start = line.find("var_");
        const auto expression_separator = line.find(" = ");
        if (variable_start != std::string::npos && expression_separator != std::string::npos) {
            if (!current.variable.empty()) {
                result.push_back(std::move(current));
                current = {};
            }
            current.variable = line.substr(variable_start, expression_separator - variable_start);
            continue;
        }
        if (current.variable.empty()) {
            continue;
        }
        if (auto value = property(line, "Mechanism")) {
            current.mechanism = std::move(*value);
        } else if (auto value = property(line, "Reason")) {
            current.reason = std::move(*value);
        } else if (auto value = property(line, "Depends On")) {
            current.depends_on = std::move(*value);
        } else if (auto value = property(line, "Producer Value")) {
            current.producer_value = std::stoull(*value);
        }
    }
    if (!current.variable.empty()) {
        result.push_back(std::move(current));
    }
    return result;
}

/** Join EXPLAIN rows into one document while preserving line boundaries. */
[[nodiscard]] std::string plan_document(std::span<const std::string> plan) {
    std::string document;
    for (const auto& row : plan) {
        document += row;
        document.push_back('\n');
    }
    return document;
}

/** Count non-overlapping occurrences of a marker in a document. */
[[nodiscard]] std::size_t count_occurrences(std::string_view text, std::string_view needle) {
    if (needle.empty()) {
        return 0u;
    }
    std::size_t count = 0u;
    for (std::size_t position = 0u;
         (position = text.find(needle, position)) != std::string_view::npos;
         position += needle.size()) {
        ++count;
    }
    return count;
}

/** Return the balanced JSON array assigned to an object key. */
[[nodiscard]] std::optional<std::string_view> json_array_member(std::string_view document,
                                                                std::string_view key) {
    const auto key_position = document.find(fmt::format("\"{}\"", key));
    if (key_position == std::string_view::npos) {
        return std::nullopt;
    }
    const auto colon = document.find(':', key_position + key.size() + 2u);
    const auto open =
        colon == std::string_view::npos ? std::string_view::npos : document.find('[', colon + 1u);
    if (open == std::string_view::npos) {
        return std::nullopt;
    }
    std::size_t depth = 0u;
    bool in_string = false;
    bool escaped = false;
    for (std::size_t position = open; position < document.size(); ++position) {
        const char value = document[position];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (value == '\\') {
                escaped = true;
            } else if (value == '"') {
                in_string = false;
            }
            continue;
        }
        if (value == '"') {
            in_string = true;
        } else if (value == '[') {
            ++depth;
        } else if (value == ']') {
            if (depth == 0u) {
                return std::nullopt;
            }
            --depth;
            if (depth == 0u) {
                return document.substr(open, position - open + 1u);
            }
        }
    }
    return std::nullopt;
}

/** Return the balanced JSON object assigned to an object key. */
[[nodiscard]] std::optional<std::string_view> json_object_member(std::string_view document,
                                                                 std::string_view key) {
    const auto key_position = document.find(fmt::format("\"{}\"", key));
    if (key_position == std::string_view::npos) {
        return std::nullopt;
    }
    const auto colon = document.find(':', key_position + key.size() + 2u);
    const auto open =
        colon == std::string_view::npos ? std::string_view::npos : document.find('{', colon + 1u);
    if (open == std::string_view::npos) {
        return std::nullopt;
    }
    std::size_t depth = 0u;
    bool in_string = false;
    bool escaped = false;
    for (std::size_t position = open; position < document.size(); ++position) {
        const char value = document[position];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (value == '\\') {
                escaped = true;
            } else if (value == '"') {
                in_string = false;
            }
            continue;
        }
        if (value == '"') {
            in_string = true;
        } else if (value == '{') {
            ++depth;
        } else if (value == '}') {
            if (depth == 0u) {
                return std::nullopt;
            }
            --depth;
            if (depth == 0u) {
                return document.substr(open, position - open + 1u);
            }
        }
    }
    return std::nullopt;
}

/** Return the indentation-delimited YAML block assigned to a key. */
[[nodiscard]] std::optional<std::string_view> yaml_property_block(std::string_view document,
                                                                  std::string_view key) {
    const auto key_position = document.find(key);
    if (key_position == std::string_view::npos) {
        return std::nullopt;
    }
    const auto line_start_marker = document.rfind('\n', key_position);
    const auto line_start =
        line_start_marker == std::string_view::npos ? 0u : line_start_marker + 1u;
    const auto header_end = document.find('\n', key_position);
    if (header_end == std::string_view::npos) {
        return std::nullopt;
    }
    std::size_t base_indent = 0u;
    while (line_start + base_indent < header_end && (document[line_start + base_indent] == ' ' ||
                                                     document[line_start + base_indent] == '\t')) {
        ++base_indent;
    }
    const std::size_t block_start = header_end + 1u;
    std::size_t block_end = document.size();
    for (std::size_t cursor = block_start; cursor < document.size();) {
        const auto newline = document.find('\n', cursor);
        const auto line_end = newline == std::string_view::npos ? document.size() : newline;
        std::size_t indent = 0u;
        while (cursor + indent < line_end &&
               (document[cursor + indent] == ' ' || document[cursor + indent] == '\t')) {
            ++indent;
        }
        if (cursor + indent < line_end && indent <= base_indent) {
            block_end = cursor;
            break;
        }
        if (newline == std::string_view::npos) {
            break;
        }
        cursor = newline + 1u;
    }
    return document.substr(block_start, block_end - block_start);
}

/** Return one complete XML element by its exact name. */
[[nodiscard]] std::optional<std::string_view> xml_element_block(std::string_view document,
                                                                std::string_view element) {
    const auto open_text = fmt::format("<{}>", element);
    const auto close_text = fmt::format("</{}>", element);
    const auto open = document.find(open_text);
    const auto close =
        open == std::string_view::npos ? std::string_view::npos : document.find(close_text, open);
    if (open == std::string_view::npos || close == std::string_view::npos) {
        return std::nullopt;
    }
    return document.substr(open, close - open + close_text.size());
}

/** Quote arbitrary test data as a PostgreSQL SQL literal. */
[[nodiscard]] std::string sql_quoted_literal(PGconn* connection, std::string_view value) {
    char* escaped = PQescapeLiteral(connection, value.data(), value.size());
    REQUIRE(escaped != nullptr);
    std::string result{escaped};
    PQfreemem(escaped);
    return result;
}

/** Return whether a comma-separated EXPLAIN value contains one exact item. */
[[nodiscard]] bool comma_separated_value_contains(std::string_view values,
                                                  std::string_view expected) {
    std::size_t begin = 0;
    while (begin <= values.size()) {
        const std::size_t separator = values.find(',', begin);
        const std::size_t end = separator == std::string_view::npos ? values.size() : separator;
        const std::size_t value_begin = values.find_first_not_of(" \t", begin);
        const std::size_t value_end = values.find_last_not_of(" \t", end == 0 ? 0 : end - 1u);
        if (value_begin != std::string_view::npos && value_begin < end &&
            value_end != std::string_view::npos && value_end >= value_begin &&
            values.substr(value_begin, value_end - value_begin + 1u) == expected) {
            return true;
        }
        if (separator == std::string_view::npos)
            break;
        begin = separator + 1u;
    }
    return false;
}

} // namespace bioseq_int_tests::explain_test_support
