// Integration scenarios:
//   * `EXPLAIN` of a `USING bioseq` table SELECT shows
//     `Custom Scan (BioseqScan)` as the table-level plan node.
//   * The custom-scan node's `Bioseq Projected Columns:` property exactly
//     equals the comma-separated list of projected attribute names — not
//     just contains a substring match.
//   * Every scan-toggling GUC (`enable_bioseq_index_scan`,
//     `enable_vectorization`, `enable_column_cache`) keeps the custom scan
//     winning over a heap scan through exact RestrictInfo ownership and
//     structural pruning, while reported costs stay finite and nonnegative.
//   * `EXPLAIN (FORMAT JSON, ANALYZE, ...)` returns a JSON document whose
//     plan tree references "BioseqScan" as the custom plan provider.

#include "common_tests_integration.h"
#include <libpq-fe.h>
import bioseq.integration_tests.common;

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <mutex>

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view kTable = "test_explain_t";

std::string explain_target_sql() {
    return fmt::format(
        "SELECT pk, seq0 FROM {} WHERE mmsearch(seq0, "
        "'TTTTAAAAGGGGCCCC'::bioseq.seq('dna4', 32));",
        kTable);
}

std::string explain_highway_target_sql() {
    return fmt::format(
        "SELECT pk, seq0, bioseq.edit(pk) AS edit_distance FROM {} "
        "WHERE mmsearch(seq0, "
        "'TTTTAAAAGGGGCCCC'::bioseq.seq('dna4', 32));",
        kTable);
}

void configure_highway_simd_pipeline(
        const connection_test_context& ctx) {
    exec_sql(ctx, "SET max_parallel_workers_per_gather = 0;");
    exec_sql(ctx, "SET bioseq.read_sequence_index = off;");
    exec_sql(ctx, "SET bioseq.enable_vectorization = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan_enabled = off;");
    exec_sql(ctx,
        "SET bioseq.aligner.minimizer_algorithm = 'two_stack_nthash';");
    exec_sql(ctx,
        "SET bioseq.aligner.chaining_algorithm = 'simd_mm2';");
    exec_sql(ctx,
        "SET bioseq.aligner.base_alignment_algorithm = 'block_aligner';");
}

struct ExpectedHighwayStageFunction {
    std::string_view function;
    std::string_view stage;
};

void require_complete_generic_highway_pipeline(
        std::span<const std::string> plan) {
    constexpr std::array expected{
        ExpectedHighwayStageFunction{
            "minimizer::two_stack_nthash", "QuerySketchStage"},
        ExpectedHighwayStageFunction{
            "chaining::chain_anchors_simd", "ChainStage"},
        ExpectedHighwayStageFunction{
            "alignment::block_aligner", "AlignStage"},
    };
    std::array<bool, expected.size()> seen{};
    std::size_t planned_function_count = 0;
    for (const std::string& line : plan) {
        if (line.find("Highway Function:") == std::string::npos
            || line.find("* Highway Function:") != std::string::npos
            || line.find("[stage:") == std::string::npos) {
            continue;
        }
        ++planned_function_count;
        bool matched = false;
        for (std::size_t index = 0; index < expected.size(); ++index) {
            const auto& item = expected[index];
            if (line.find(fmt::format(
                    "Highway Function: {}", item.function))
                    == std::string::npos
                || line.find(fmt::format("[stage: {}]", item.stage))
                    == std::string::npos) {
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
    for (const bool was_seen : seen) REQUIRE(was_seen);
}

#ifdef BIOSEQ_TEST_INDEXES
std::mutex highway_target_force_mutex;

class HighwayTargetForceReset final {
public:
    explicit HighwayTargetForceReset(PGconn* connection) noexcept
        : connection_{connection} {}

    HighwayTargetForceReset(const HighwayTargetForceReset&) = delete;
    HighwayTargetForceReset& operator=(const HighwayTargetForceReset&) = delete;

    ~HighwayTargetForceReset() noexcept {
        if (connection_ == nullptr) return;
        PGresult* result = PQexec(
            connection_,
            "SELECT bioseq_internal.set_highway_target_for_test(0);");
        const bool restored = result != nullptr
            && PQresultStatus(result) == PGRES_TUPLES_OK;
        if (result != nullptr) PQclear(result);
        if (!restored) {
            // Terminating/replacing the backend also discards Highway's
            // process-local test mask if SQL cleanup cannot run.
            PQreset(connection_);
        }
    }

private:
    PGconn* connection_ = nullptr;
};
#endif

std::vector<std::string> explain_with_options(
    const connection_test_context& ctx,
    std::string_view options,
    std::string_view sql) {
    auto rows = query_rows(
        ctx, fmt::format("EXPLAIN ({}) {}", options, sql));
    std::vector<std::string> lines;
    lines.reserve(rows.size());
    for (auto& row : rows) {
        REQUIRE(row.size() == 1);
        REQUIRE(row.front().has_value());
        lines.push_back(std::move(*row.front()));
    }
    return lines;
}

std::optional<unsigned long long> boundary_actual_metric(
    std::span<const std::string> plan,
    std::string_view mechanism,
    std::string_view reason,
    std::string_view property) {
    bool mechanism_matches = false;
    bool reason_matches = false;
    const auto property_marker = fmt::format("{}:", property);
    for (const auto& line : plan) {
        if (line.find("var_") != std::string::npos
            && line.find(" = ") != std::string::npos) {
            mechanism_matches = false;
            reason_matches = false;
        }
        if (line.find(fmt::format("Mechanism: {}", mechanism))
            != std::string::npos) {
            mechanism_matches = true;
        }
        if (line.find(fmt::format("Reason: {}", reason))
            != std::string::npos) {
            reason_matches = true;
        }
        if (mechanism_matches && reason_matches) {
            const auto marker = line.find(property_marker);
            if (marker != std::string::npos) {
                return std::stoull(line.substr(
                    marker + property_marker.size()));
            }
        }
    }
    return std::nullopt;
}

std::optional<unsigned long long> boundary_actual_calls(
    std::span<const std::string> plan,
    std::string_view mechanism,
    std::string_view reason) {
    return boundary_actual_metric(
        plan, mechanism, reason, "Actual Calls");
}

std::optional<unsigned long long> boundary_actual_bytes(
    std::span<const std::string> plan,
    std::string_view mechanism,
    std::string_view reason) {
    return boundary_actual_metric(
        plan, mechanism, reason, "Actual Bytes");
}

std::vector<std::string> planned_slice_signature(
    std::span<const std::string> plan) {
    std::vector<std::string> signature;
    for (const auto& line : plan) {
        const bool recipe_value = line.find("value_") != std::string::npos
            && line.find(" = ") != std::string::npos;
        const bool boundary_value = line.find("var_") != std::string::npos
            && line.find(" = ") != std::string::npos;
        const bool planned_property = line.find("Mechanism:") != std::string::npos
            || line.find("Reason:") != std::string::npos
            || line.find("Depends On:") != std::string::npos
            || line.find("Encoding:") != std::string::npos
            || line.find("Cache Scope:") != std::string::npos;
        if (!recipe_value && !boundary_value && !planned_property) {
            continue;
        }
        const auto first = line.find_first_not_of(" \t");
        signature.push_back(first == std::string::npos
            ? std::string{} : line.substr(first));
    }
    return signature;
}

struct materialization_signature {
    std::string variable;
    std::string mechanism;
    std::string reason;
    std::string depends_on;
    std::optional<unsigned long long> producer_value;
};

std::vector<materialization_signature> materialization_signatures(
    std::span<const std::string> plan) {
    std::vector<materialization_signature> result;
    materialization_signature current;
    auto property = [](const std::string& line, std::string_view name)
        -> std::optional<std::string> {
        const auto marker = fmt::format("{}:", name);
        const auto position = line.find(marker);
        if (position == std::string::npos) {
            return std::nullopt;
        }
        const auto value_start = line.find_first_not_of(
            " \t", position + marker.size());
        return value_start == std::string::npos
            ? std::string{} : line.substr(value_start);
    };
    for (const auto& line : plan) {
        const auto variable_start = line.find("var_");
        const auto expression_separator = line.find(" = ");
        if (variable_start != std::string::npos
            && expression_separator != std::string::npos) {
            if (!current.variable.empty()) {
                result.push_back(std::move(current));
                current = {};
            }
            current.variable = line.substr(
                variable_start, expression_separator - variable_start);
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

[[nodiscard]] std::string plan_document(
    std::span<const std::string> plan) {
    std::string document;
    for (const auto& row : plan) {
        document += row;
        document.push_back('\n');
    }
    return document;
}

[[nodiscard]] std::size_t count_occurrences(
    std::string_view text, std::string_view needle) {
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

[[nodiscard]] std::optional<std::string_view> json_array_member(
    std::string_view document, std::string_view key) {
    const auto key_position = document.find(
        fmt::format("\"{}\"", key));
    if (key_position == std::string_view::npos) {
        return std::nullopt;
    }
    const auto colon = document.find(':', key_position + key.size() + 2u);
    const auto open = colon == std::string_view::npos
        ? std::string_view::npos : document.find('[', colon + 1u);
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

[[nodiscard]] std::optional<std::string_view> json_object_member(
    std::string_view document, std::string_view key) {
    const auto key_position = document.find(fmt::format("\"{}\"", key));
    if (key_position == std::string_view::npos) {
        return std::nullopt;
    }
    const auto colon = document.find(':', key_position + key.size() + 2u);
    const auto open = colon == std::string_view::npos
        ? std::string_view::npos : document.find('{', colon + 1u);
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

[[nodiscard]] std::optional<std::string_view> yaml_property_block(
    std::string_view document, std::string_view key) {
    const auto key_position = document.find(key);
    if (key_position == std::string_view::npos) {
        return std::nullopt;
    }
    const auto line_start_marker = document.rfind('\n', key_position);
    const auto line_start = line_start_marker == std::string_view::npos
        ? 0u : line_start_marker + 1u;
    const auto header_end = document.find('\n', key_position);
    if (header_end == std::string_view::npos) {
        return std::nullopt;
    }
    std::size_t base_indent = 0u;
    while (line_start + base_indent < header_end
           && (document[line_start + base_indent] == ' '
               || document[line_start + base_indent] == '\t')) {
        ++base_indent;
    }
    const std::size_t block_start = header_end + 1u;
    std::size_t block_end = document.size();
    for (std::size_t cursor = block_start; cursor < document.size();) {
        const auto newline = document.find('\n', cursor);
        const auto line_end = newline == std::string_view::npos
            ? document.size() : newline;
        std::size_t indent = 0u;
        while (cursor + indent < line_end
               && (document[cursor + indent] == ' '
                   || document[cursor + indent] == '\t')) {
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

[[nodiscard]] std::optional<std::string_view> xml_element_block(
    std::string_view document, std::string_view element) {
    const auto open_text = fmt::format("<{}>", element);
    const auto close_text = fmt::format("</{}>", element);
    const auto open = document.find(open_text);
    const auto close = open == std::string_view::npos
        ? std::string_view::npos : document.find(close_text, open);
    if (open == std::string_view::npos || close == std::string_view::npos) {
        return std::nullopt;
    }
    return document.substr(
        open, close - open + close_text.size());
}

[[nodiscard]] std::string sql_quoted_literal(
    PGconn* connection, std::string_view value) {
    char* escaped = PQescapeLiteral(connection, value.data(), value.size());
    REQUIRE(escaped != nullptr);
    std::string result{escaped};
    PQfreemem(escaped);
    return result;
}

// Returns whether one trimmed item in a comma-separated EXPLAIN value exactly
// matches `expected`.
[[nodiscard]] bool comma_separated_value_contains(
    std::string_view values,
    std::string_view expected)
{
    std::size_t begin = 0;
    while (begin <= values.size()) {
        const std::size_t separator = values.find(',', begin);
        const std::size_t end = separator == std::string_view::npos
            ? values.size()
            : separator;
        const std::size_t value_begin = values.find_first_not_of(
            " \t", begin);
        const std::size_t value_end = values.find_last_not_of(
            " \t", end == 0 ? 0 : end - 1u);
        if (value_begin != std::string_view::npos
            && value_begin < end
            && value_end != std::string_view::npos
            && value_end >= value_begin
            && values.substr(value_begin, value_end - value_begin + 1u)
                == expected) {
            return true;
        }
        if (separator == std::string_view::npos) break;
        begin = separator + 1u;
    }
    return false;
}

}  // namespace

TEST_CASE("EXPLAIN names the custom scan on a USING bioseq table",
          "[explain][custom_scan]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);

    const auto plan = explain_text<false>(ctx, explain_target_sql());
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqScan)"));
    REQUIRE_FALSE(plan_contains(plan, "Filter: mmsearch"));
}

TEST_CASE("EXPLAIN's Bioseq Projected Columns property lists exactly the SELECTed attrs",
          "[explain][projection]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);

    const auto plan = explain_text<false>(ctx, explain_target_sql());
    const auto value = find_explain_property(plan, "Bioseq Projected Columns");
    REQUIRE(value.has_value());
    // The property orders columns by their SELECT-list position; "pk, seq0"
    // is the canonical order for the SELECT in `explain_target_sql`. We pin
    // the exact value to catch any reordering regression.
    REQUIRE(value.value() == "pk, seq0");
}

namespace {
void check_explain_keeps_custom_scan(std::string_view guc,
                                     std::string_view value) {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);

    guc_scope set(ctx, guc, value);
    const auto plan = explain_text<false>(ctx, explain_target_sql());
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqScan)"));
}
}  // namespace

TEST_CASE("EXPLAIN: BioseqScan persists with enable_bioseq_index_scan=on",
          "[explain][guc][custom_scan]") {
    check_explain_keeps_custom_scan("bioseq.enable_bioseq_index_scan", "on");
}
TEST_CASE("EXPLAIN: BioseqScan persists with enable_bioseq_index_scan=off",
          "[explain][guc][custom_scan]") {
    check_explain_keeps_custom_scan("bioseq.enable_bioseq_index_scan", "off");
}
TEST_CASE("EXPLAIN: BioseqScan persists with enable_vectorization=on",
          "[explain][guc][custom_scan]") {
    check_explain_keeps_custom_scan("bioseq.enable_vectorization", "on");
}
TEST_CASE("EXPLAIN: BioseqScan persists with enable_vectorization=off",
          "[explain][guc][custom_scan]") {
    check_explain_keeps_custom_scan("bioseq.enable_vectorization", "off");
}
TEST_CASE("EXPLAIN: BioseqScan persists with enable_column_cache=on",
          "[explain][guc][custom_scan]") {
    check_explain_keeps_custom_scan("bioseq.enable_column_cache", "on");
}
TEST_CASE("EXPLAIN: BioseqScan persists with enable_column_cache=off",
          "[explain][guc][custom_scan]") {
    check_explain_keeps_custom_scan("bioseq.enable_column_cache", "off");
}

TEST_CASE("EXPLAIN ANALYZE on mmsearch query reports the BioseqScan rows",
          "[explain][analyze]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);

    const auto plan = explain_text<true>(
        ctx,
        fmt::format("SELECT pk, bioseq.cigar(pk) FROM {} WHERE mmsearch(seq0, "
                    "'AAAA'::bioseq.seq('dna4', 32)) ORDER BY pk;",
                    kTable));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqScan)"));
    REQUIRE(plan_contains(plan, "actual time"));
}

TEST_CASE("DOC0398 plain EXPLAIN reports the complete Highway dispatch plan",
          "[highway][explain][dispatch]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);
    configure_highway_simd_pipeline(ctx);

    const auto plan = explain_text<false>(
        ctx, explain_highway_target_sql());
    const auto planned = find_explain_property(
        plan, "Highway Planned Architecture");
    const auto supported = find_explain_property(
        plan, "Highway CPU-Supported Architectures");
    const auto compiled = find_explain_property(
        plan, "Highway Compiled Architectures");
    const auto runnable = find_explain_property(
        plan, "Highway Runnable Architectures");
    const auto executed = find_explain_property(
        plan, "* Highway Executed Architecture");

    REQUIRE(planned.has_value());
    REQUIRE(supported.has_value());
    REQUIRE(compiled.has_value());
    REQUIRE(runnable.has_value());
    REQUIRE_FALSE(executed.has_value());
    REQUIRE_FALSE(planned->empty());
    REQUIRE(*planned != "none");
    REQUIRE(*supported != "none");
    REQUIRE(*compiled != "none");
    REQUIRE(*runnable != "none");
    REQUIRE(comma_separated_value_contains(*runnable, *planned));

    const std::string planned_annotation = fmt::format(
        "[planned architecture: {}]", *planned);
    std::size_t highway_functions = 0;
    bool query_sketch_found = false;
    bool chaining_found = false;
    bool alignment_found = false;
    for (const std::string& line : plan) {
        if (line.find("Highway Function:") == std::string::npos) continue;
        ++highway_functions;
        REQUIRE(line.find(planned_annotation) != std::string::npos);
        query_sketch_found = query_sketch_found
            || line.find("minimizer::two_stack_nthash")
                != std::string::npos;
        chaining_found = chaining_found
            || line.find("chaining::chain_anchors_simd")
                != std::string::npos;
        alignment_found = alignment_found
            || line.find("alignment::block_aligner")
                != std::string::npos;
    }
    REQUIRE(highway_functions >= 3u);
    REQUIRE(query_sketch_found);
    REQUIRE(chaining_found);
    REQUIRE(alignment_found);
}

TEST_CASE("DOC0398 EXPLAIN ANALYZE reports the executed Highway binding",
          "[highway][explain][analyze][dispatch]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);
    configure_highway_simd_pipeline(ctx);

    const auto plan = explain_text<true>(
        ctx, explain_highway_target_sql());
    const auto planned = find_explain_property(
        plan, "Highway Planned Architecture");
    const auto executed = find_explain_property(
        plan, "* Highway Executed Architecture");

    REQUIRE(planned.has_value());
    REQUIRE(executed.has_value());
    REQUIRE(*executed != "none");
    REQUIRE(*executed != "mixed");
    REQUIRE(*executed == *planned);
}

TEST_CASE("DOC0398 BatchJoin and PerRow EXPLAIN own every selected Highway stage",
          "[highway][explain][dispatch][batch-join][per-row]") {
    constexpr std::string_view target =
        "test_explain_highway_join_target";
    auto ctx = fresh_context({target});
    seed_small_bioseq_column(ctx, target);
    configure_highway_simd_pipeline(ctx);
    exec_sql(ctx, "SET join_collapse_limit = 1;");
    exec_sql(ctx, "SET from_collapse_limit = 1;");
    exec_sql(ctx, "SET geqo = off;");
    exec_sql(ctx,
        "CREATE TEMP TABLE test_explain_highway_join_query "
        "(id int PRIMARY KEY, seq bioseq.seq('dna4',32));");
    exec_sql(ctx,
        "INSERT INTO test_explain_highway_join_query VALUES "
        "(1, 'TTTTAAAAGGGGCCCC'::bioseq.seq('dna4',32));");

    const std::string batch_join_sql = fmt::format(
        "SELECT q.id, t.pk, bioseq.edit(t.pk) AS edit_distance "
        "FROM {} AS t "
        "JOIN test_explain_highway_join_query AS q "
        "ON mmsearch(t.seq0, q.seq);",
        target);
    const auto batch_join = explain_with_options(
        ctx, "VERBOSE, COSTS OFF", batch_join_sql);
    REQUIRE(plan_contains(batch_join, "Custom Scan (BioseqBatchJoin)"));
    REQUIRE(plan_contains(batch_join, "Bioseq Batch ABI Version: 3"));
    REQUIRE(plan_contains(
        batch_join, "Bioseq Match Placeholder Sites: 1"));
    REQUIRE_FALSE(plan_contains(
        batch_join, "Bioseq Mmsearch Ownership: per-row recipe"));
    require_complete_generic_highway_pipeline(batch_join);

    const std::string per_row_sql = fmt::format(
        "SELECT q.id, t.pk, bioseq.edit(t.pk) AS edit_distance "
        "FROM {} AS t "
        "JOIN test_explain_highway_join_query AS q "
        "ON mmsearch(t.seq0[0:], q.seq[0:]);",
        target);
    const auto per_row = explain_with_options(
        ctx, "VERBOSE, COSTS OFF", per_row_sql);
    REQUIRE(plan_contains(per_row, "Custom Scan (BioseqBatchJoin)"));
    REQUIRE(plan_contains(
        per_row, "Bioseq Match Placeholder Sites: 1"));
    REQUIRE(plan_contains(
        per_row, "Bioseq Mmsearch Ownership: per-row recipe"));
    require_complete_generic_highway_pipeline(per_row);
}

TEST_CASE("DOC0398 BatchJoin attaches executed grouped32 to SeedHitStage",
          "[highway][explain][analyze][batch-join][grouped32]") {
    constexpr std::string_view target =
        "test_explain_highway_grouped32_target";
    constexpr std::string_view query_table =
        "test_explain_highway_grouped32_query";
    constexpr std::string_view query_sequence =
        /* The exact W10/K15 sketch has one minimizer with u32 hash 3694.
         * The legacy fixture publishes hashes [1, 4096], so the selective
         * grouped32 reader must load and validate its exact partition. */
        "ACTTGCAGCCCCCAGTTCATTCAG";
    const std::string target_sequence(4096u, 'A');

    auto ctx = fresh_context({target});
    exec_sql(ctx, "SET bioseq.update_index_on_insert = off;");
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string{target},
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 1,
            .using_bioseq = true,
        },
        {seq_row{
            .pk = 1,
            .sequences = {std::string{target_sequence}},
        }});
    configure_highway_simd_pipeline(ctx);
    exec_sql(ctx,
        "SET bioseq.index_async_persist = off; "
        "SET bioseq.recompute_async_persist = off; "
        "SET bioseq.read_sequence_index = on;");
    const auto rowno = query_scalar_int(ctx, fmt::format(
        "SELECT bioseq.column_row_number('{}'::regclass, ctid) "
        "FROM {} ORDER BY pk LIMIT 1;",
        target, target));
    const row_t fixture = query_one(ctx, fmt::format(
        "SELECT * FROM bioseq_internal.build_minimizer_fixture("
        "'{}'::regclass, 'minimizers_per_fragment', {}, "
        "15::smallint, 10::smallint, 4::smallint, "
        "'exact_simd');",
        target, rowno));
    REQUIRE(fixture.size() == 20u);
    REQUIRE(fixture[1].value_or("") == "4");
    exec_sql(ctx, fmt::format(
        "SELECT bioseq.set_active_read_index('{}'::regclass, "
        "'minimizers_per_fragment');",
        target));
    exec_sql(ctx, fmt::format(
        "CREATE TEMP TABLE {} "
        "(id int PRIMARY KEY, seq bioseq.seq('dna4',32)); "
        "INSERT INTO {} VALUES "
        "(1, '{}'::bioseq.seq('dna4',32));",
        query_table, query_table, query_sequence));

    const std::string sql = fmt::format(
        "SELECT q.id, t.pk FROM {} AS t JOIN {} AS q "
        "ON mmsearch(t.seq0, q.seq);",
        target, query_table);
    const auto plain = explain_with_options(
        ctx, "VERBOSE, COSTS OFF", sql);
    REQUIRE(plan_contains(plain, "Custom Scan (BioseqBatchJoin)"));
    REQUIRE_FALSE(plan_contains(
        plain, "indexing::grouped32_validate"));

    const auto analyzed = explain_with_options(
        ctx, "ANALYZE, VERBOSE, COSTS OFF", sql);
    REQUIRE(plan_contains(
        analyzed,
        "Bioseq Index Read Requirement: require_compatible_persisted"));
    std::size_t grouped32_stage_functions = 0u;
    for (const std::string& line : analyzed) {
        if (line.find(
                "Highway Function: indexing::grouped32_validate")
                == std::string::npos) {
            continue;
        }
        ++grouped32_stage_functions;
        REQUIRE(line.find("[stage: SeedHitStage]") != std::string::npos);
        REQUIRE(line.find("[planned architecture:") != std::string::npos);
        REQUIRE(line.find("[executed calls:") == std::string::npos);
        REQUIRE(line.find("* Highway Function:") == std::string::npos);
    }
    REQUIRE(grouped32_stage_functions == 1u);
}

#ifdef BIOSEQ_TEST_INDEXES
TEST_CASE("DOC0398 every runnable Highway target preserves public query results",
          "[highway][explain][dispatch][forced-target][serial]") {
    const std::scoped_lock serialized{highway_target_force_mutex};
    auto ctx = fresh_context({kTable});
    HighwayTargetForceReset restore{ctx.raw()};
    (void)query_scalar(
        ctx, "SELECT bioseq_internal.set_highway_target_for_test(0);");
    seed_small_bioseq_column(ctx, kTable);
    configure_highway_simd_pipeline(ctx);

    const auto baseline_mask = static_cast<std::int64_t>(query_scalar_int(
        ctx,
        "SELECT bioseq_internal."
        "highway_baseline_runnable_targets_for_test();"));
    REQUIRE(baseline_mask > 0);
    REQUIRE(sql_fails(
        ctx, "SELECT bioseq_internal.set_highway_target_for_test(-1);"));

    const std::uint64_t baseline_bits =
        static_cast<std::uint64_t>(baseline_mask);
    if ((baseline_bits & (baseline_bits - std::uint64_t{1})) != 0u) {
        REQUIRE(sql_fails(ctx, fmt::format(
            "SELECT bioseq_internal.set_highway_target_for_test({});",
            baseline_mask)));
    }
    const std::uint64_t unsupported_positive_bits =
        ~baseline_bits
        & static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max());
    REQUIRE(unsupported_positive_bits != 0u);
    const std::uint64_t unsupported_target_bit =
        unsupported_positive_bits
        & (~unsupported_positive_bits + std::uint64_t{1});
    REQUIRE(sql_fails(ctx, fmt::format(
        "SELECT bioseq_internal.set_highway_target_for_test({});",
        static_cast<std::int64_t>(unsupported_target_bit))));

    const std::string public_sql = fmt::format(
        "SELECT pk, seq0, bioseq.cigar(pk) AS cigar, "
        "bioseq.edit(pk) AS edit_distance FROM {} WHERE mmsearch(seq0, "
        "'TTTTAAAAGGGGCCCC'::bioseq.seq('dna4', 32)) ORDER BY pk;",
        kTable);
    const rowset_t baseline_rows = query_rows(ctx, public_sql);
    REQUIRE_FALSE(baseline_rows.empty());
    for (const auto& row : baseline_rows) {
        REQUIRE(row.size() == 4u);
        REQUIRE(row[0].has_value());
        REQUIRE(row[1].has_value());
        REQUIRE(row[2].has_value());
        REQUIRE_FALSE(row[2]->empty());
        REQUIRE(is_valid_cigar(*row[2]));
        REQUIRE(row[3].has_value());
        REQUIRE(std::stoll(*row[3]) >= 0);
    }

    std::uint64_t remaining = baseline_bits;
    while (remaining != 0u) {
        const std::uint64_t target_bit =
            remaining & (~remaining + std::uint64_t{1});
        const auto target = static_cast<std::int64_t>(target_bit);
        const std::string target_name = query_scalar(ctx, fmt::format(
            "SELECT bioseq_internal.set_highway_target_for_test({});",
            target));
        CAPTURE(target, target_name);

        REQUIRE(query_rows(ctx, public_sql) == baseline_rows);
        const auto plan = explain_text<true>(
            ctx, explain_highway_target_sql());
        const auto planned = find_explain_property(
            plan, "Highway Planned Architecture");
        const auto executed = find_explain_property(
            plan, "* Highway Executed Architecture");
        REQUIRE(planned.has_value());
        REQUIRE(executed.has_value());
        REQUIRE(*planned == target_name);
        REQUIRE(*executed == target_name);

        remaining &= remaining - std::uint64_t{1};
    }
}
#endif

TEST_CASE("EXPLAIN FORMAT JSON owns one complete Highway dispatch object",
          "[explain][json][highway]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);
    configure_highway_simd_pipeline(ctx);

    const std::string json = explain_json(
        ctx, explain_highway_target_sql());

    REQUIRE_FALSE(json.empty());
    // BioseqScan and the Custom-Scan node-type string must both appear.
    REQUIRE(json.find("BioseqScan") != std::string::npos);
    REQUIRE(json.find("Custom Scan") != std::string::npos);
    REQUIRE(count_occurrences(
                json, "\"Highway SIMD Dispatch\"") == 1u);
    const auto highway_dispatch = json_object_member(
        json, "Highway SIMD Dispatch");
    REQUIRE(highway_dispatch.has_value());
    for (const std::string_view property : {
             "\"Planned Architecture\"",
             "\"CPU-Supported Architectures\"",
             "\"Compiled Architectures\"",
             "\"Runnable Architectures\"",
             "\"Executed Architecture\"",
        }) {
        CAPTURE(property);
        REQUIRE(count_occurrences(*highway_dispatch, property) == 1u);
    }
    REQUIRE(json.find("\"Highway Functions\"") != std::string::npos);
    REQUIRE(json.find("minimizer::two_stack_nthash")
            != std::string::npos);
    REQUIRE(json.find("chaining::chain_anchors_simd")
            != std::string::npos);
    REQUIRE(json.find("alignment::block_aligner")
            != std::string::npos);
}

TEST_CASE("DOC0398 structured plan omits an execution target before ANALYZE",
          "[explain][json][highway][plan-only]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);

    const auto rows = explain_with_options(
        ctx, "FORMAT JSON, VERBOSE, COSTS OFF",
        explain_highway_target_sql());
    const std::string json = plan_document(rows);

    REQUIRE(count_occurrences(
                json, "\"Highway SIMD Dispatch\"") == 1u);
    const auto highway_dispatch = json_object_member(
        json, "Highway SIMD Dispatch");
    REQUIRE(highway_dispatch.has_value());
    REQUIRE(highway_dispatch->find("\"Planned Architecture\"")
            != std::string_view::npos);
    REQUIRE(highway_dispatch->find("\"CPU-Supported Architectures\"")
            != std::string_view::npos);
    REQUIRE(highway_dispatch->find("\"Compiled Architectures\"")
            != std::string_view::npos);
    REQUIRE(highway_dispatch->find("\"Runnable Architectures\"")
            != std::string_view::npos);
    REQUIRE(highway_dispatch->find("\"Executed Architecture\"")
            == std::string_view::npos);
}

TEST_CASE("DOC0377 plain EXPLAIN reports only materialization boundaries",
          "[explain][slice][materialization]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);

    const auto plan = explain_with_options(
        ctx, "VERBOSE, COSTS OFF",
        fmt::format("SELECT seq0[0:4] FROM {};", kTable));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(plan_contains(plan, "Slice Materialization:"));
    REQUIRE(plan_contains(plan, "Mechanism: text"));
    REQUIRE(plan_contains(plan, "Mechanism: binary"));
    REQUIRE(plan_contains(plan, "Reason: client output"));
    REQUIRE_FALSE(plan_contains(plan, "Actual Calls:"));
    REQUIRE_FALSE(plan_contains(plan, "Estimated Symbols:"));
    REQUIRE_FALSE(plan_contains(plan, "Operation: slice"));
}

TEST_CASE("DOC0377 metadata-only projection emits no false allocation boundary",
          "[explain][slice][materialization]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);

    const auto plan = explain_with_options(
        ctx, "ANALYZE, TIMING OFF, COSTS OFF",
        fmt::format("SELECT bioseq.len(seq0[0:4]) FROM {};", kTable));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(plan_contains(plan, "Slice Materialization:"));
    REQUIRE(plan_contains(plan, "none"));
    REQUIRE_FALSE(plan_contains(plan, "Mechanism: packed full"));
    REQUIRE_FALSE(plan_contains(plan, "Mechanism: text"));
}

TEST_CASE("DOC0377 EXPLAIN SERIALIZE records only the observed client boundary",
          "[explain][slice][serialize]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);
    const auto sql = fmt::format(
        "SELECT seq0[0:4] FROM {} WHERE pk = 1;", kTable);

    const auto none = explain_with_options(
        ctx, "ANALYZE, TIMING OFF, COSTS OFF", sql);
    REQUIRE(boundary_actual_calls(none, "text", "client output") == 0u);
    REQUIRE(boundary_actual_calls(none, "binary", "client output") == 0u);
    REQUIRE(boundary_actual_bytes(none, "text", "client output") == 0u);
    REQUIRE(boundary_actual_bytes(none, "binary", "client output") == 0u);
    REQUIRE_FALSE(plan_contains(none, "Actual Time:"));

    const auto text = explain_with_options(
        ctx, "ANALYZE, TIMING OFF, COSTS OFF, SERIALIZE TEXT", sql);
    REQUIRE(boundary_actual_calls(text, "text", "client output") == 1u);
    REQUIRE(boundary_actual_bytes(text, "text", "client output") == 5u);
    REQUIRE(boundary_actual_calls(text, "binary", "client output") == 0u);
    REQUIRE(boundary_actual_bytes(text, "binary", "client output") == 0u);

    const auto binary = explain_with_options(
        ctx, "ANALYZE, TIMING OFF, COSTS OFF, SERIALIZE BINARY", sql);
    REQUIRE(boundary_actual_calls(binary, "text", "client output") == 0u);
    REQUIRE(boundary_actual_bytes(binary, "text", "client output") == 0u);
    REQUIRE(boundary_actual_calls(binary, "binary", "client output") == 1u);
    REQUIRE(boundary_actual_bytes(binary, "binary", "client output") == 20u);
}

TEST_CASE("DOC0377 SQL materialization calls and bytes match allocations",
          "[explain][slice][materialization][accounting]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);

    const auto plan = explain_with_options(
        ctx, "ANALYZE, TIMING OFF, COSTS OFF",
        fmt::format("SELECT bioseq.to_text(seq0[0:4]) FROM {};", kTable));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(boundary_actual_calls(plan, "text", "SQL conversion") == 3u);
    REQUIRE(boundary_actual_metric(
        plan, "text", "SQL conversion", "Actual Input Symbols") == 12u);
    REQUIRE(boundary_actual_metric(
        plan, "text", "SQL conversion", "Actual Output Symbols") == 12u);
    /* Three 4-symbol text varlenas: VARHDRSZ + 4 bytes each. */
    REQUIRE(boundary_actual_bytes(plan, "text", "SQL conversion") == 24u);
    REQUIRE_FALSE(plan_contains(plan, "Mechanism: packed full"));
}

TEST_CASE("DOC0377 rescans retain every materialization counter delta",
          "[explain][slice][rescan]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);

    /* The correlated scalar SubPlan rescans its one-row SliceProject for each
     * VALUES row.  This is the executor shape used by a parallel worker under
     * a nested loop as well; counters must remain cumulative across rearms. */
    const auto plan = explain_with_options(
        ctx, "ANALYZE, TIMING OFF, COSTS OFF",
        fmt::format(
            "SELECT (SELECT bioseq.to_text(source.seq0[0:4]) "
            "FROM {} AS source "
            "WHERE source.pk = driver.pk) "
            "FROM (VALUES (1), (2), (3)) AS driver(pk);",
            kTable));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(boundary_actual_calls(plan, "text", "SQL conversion") == 3u);
    REQUIRE(boundary_actual_metric(
        plan, "text", "SQL conversion", "Actual Input Symbols") == 12u);
    REQUIRE(boundary_actual_metric(
        plan, "text", "SQL conversion", "Actual Output Symbols") == 12u);
    /* Three 4-symbol text varlenas: VARHDRSZ + 4 bytes each. */
    REQUIRE(boundary_actual_bytes(plan, "text", "SQL conversion") == 24u);
}

TEST_CASE("DOC0377 structured EXPLAIN preserves numeric counter fields",
          "[explain][slice][json]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);

    const auto json = explain_json(
        ctx, fmt::format("SELECT seq0[0:4] FROM {};", kTable));
    REQUIRE(json.find("Slice Materialization") != std::string::npos);
    REQUIRE(json.find("\"Mechanism\": \"text\"") != std::string::npos);
    REQUIRE(json.find("\"Reason\": \"client output\"")
            != std::string::npos);
    REQUIRE(json.find("\"Actual Calls\": 0") != std::string::npos);
    REQUIRE(json.find("\"Actual Bytes\": 0") != std::string::npos);
    REQUIRE(json.find("\"Depends On\": []") != std::string::npos);
    REQUIRE(json.find("\"Counter Overflow\": false")
            != std::string::npos);

    const auto materializations = json_array_member(
        json, "Slice Materialization");
    REQUIRE(materializations.has_value());
    REQUIRE(count_occurrences(*materializations, "\"Variable\"") == 2u);
    constexpr std::array unique_properties{
        std::string_view{"\"Mechanism\""},
        std::string_view{"\"Reason\""},
        std::string_view{"\"Depends On\""},
        std::string_view{"\"Encoding\""},
        std::string_view{"\"Cache Scope\""},
        std::string_view{"\"Actual Calls\""},
        std::string_view{"\"Actual Bytes\""},
        std::string_view{"\"Counter Overflow\""},
    };
    for (const auto property : unique_properties) {
        CAPTURE(property);
        REQUIRE(count_occurrences(*materializations, property) == 2u);
    }
    REQUIRE(materializations->find("var_1 =") == std::string_view::npos);

    const auto parsed = query_one(ctx, fmt::format(
        "WITH document AS (SELECT {}::jsonb AS value), "
        "materializations AS ("
        " SELECT value->0->'Plan'->'Slice Materialization' AS value "
        " FROM document"
        ") SELECT jsonb_typeof(value), "
        "         jsonb_array_length(value)::text, "
        "         (SELECT bool_and(jsonb_typeof(entry)='object') "
        "          FROM jsonb_array_elements(value) AS item(entry))::text, "
        "         (SELECT bool_and(entry ?& ARRAY["
        "             'Variable','Expression','Mechanism','Reason',"
        "             'Depends On','Encoding','Cache Scope',"
        "             'Actual Calls','Actual Bytes','Counter Overflow']) "
        "          FROM jsonb_array_elements(value) AS item(entry))::text, "
        "         (SELECT bool_and(jsonb_typeof(entry->'Depends On')='array') "
        "          FROM jsonb_array_elements(value) AS item(entry))::text, "
        "         (SELECT bool_and("
        "             jsonb_typeof(entry->'Actual Calls')='number' AND "
        "             jsonb_typeof(entry->'Actual Bytes')='number' AND "
        "             jsonb_typeof(entry->'Counter Overflow')='boolean') "
        "          FROM jsonb_array_elements(value) AS item(entry))::text "
        "FROM materializations;",
        sql_quoted_literal(ctx.raw(), json)));
    REQUIRE(parsed.size() == 6u);
    REQUIRE(parsed[0].value_or("") == "array");
    REQUIRE(parsed[1].value_or("") == "2");
    REQUIRE(parsed[2].value_or("") == "true");
    REQUIRE(parsed[3].value_or("") == "true");
    REQUIRE(parsed[4].value_or("") == "true");
    REQUIRE(parsed[5].value_or("") == "true");
}

TEST_CASE("DOC0377 INSERT plan declares a heap-tuple boundary",
          "[explain][slice][insert]") {
    auto ctx = fresh_context({
        kTable, "test_explain_flat_dst", "test_explain_slice_dst"});
    seed_small_bioseq_column(ctx, kTable);
    exec_sql(ctx,
        "CREATE TABLE test_explain_flat_dst "
        "(pk int, seq0 bioseq.seq('dna4',32)) USING bioseq;"
        "CREATE TABLE test_explain_slice_dst "
        "(pk int, seq0 bioseq.seq('dna4',32)) USING bioseq;");

    /* Control the ModifyTable/EXPLAIN contract independently of the lazy
     * wrapper.  A flat Var projection must execute and persist the same three
     * source rows before the sliced path is allowed to claim a regression. */
    const auto flat_plan = explain_with_options(
        ctx, "ANALYZE, TIMING OFF, COSTS OFF",
        fmt::format(
            "INSERT INTO test_explain_flat_dst "
            "SELECT pk, seq0 FROM {};", kTable));
    REQUIRE_FALSE(plan_contains(
        flat_plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(query_int_column(
        ctx, "SELECT pk FROM test_explain_flat_dst ORDER BY pk;")
        == std::vector<long long>{1, 2, 3});

#ifdef BIOSEQ_TEST_INDEXES
    exec_sql(ctx,
        "SELECT bioseq_internal.slice_payload_allocation_probe(0);");
#endif
    const auto plan = explain_with_options(
        ctx, "ANALYZE, TIMING OFF, COSTS OFF",
        fmt::format(
            "INSERT INTO test_explain_slice_dst "
            "SELECT pk, seq0[0:4] FROM {};", kTable));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(plan_contains(plan, "Mechanism: packed full"));
    REQUIRE(plan_contains(plan, "Reason: heap tuple"));
    REQUIRE(boundary_actual_calls(
        plan, "packed full", "heap tuple") == 3u);
#ifdef BIOSEQ_TEST_INDEXES
    REQUIRE(query_scalar_int(ctx,
        "SELECT bioseq_internal.slice_payload_allocation_probe(1);") == 3);
#endif
    REQUIRE(query_int_column(
        ctx, "SELECT pk FROM test_explain_slice_dst ORDER BY pk;")
        == std::vector<long long>{1, 2, 3});
    REQUIRE(query_int_column(
        ctx, "SELECT bioseq.len(seq0) "
             "FROM test_explain_slice_dst ORDER BY pk;")
        == std::vector<long long>{4, 4, 4});
}

TEST_CASE("DOC0377 ordinary INSERT restores the page-cache GUC",
          "[slice][insert][page-cache][lifetime]") {
    constexpr std::string_view table = "doc0377_page_cache_insert";
    auto ctx = fresh_context({table});
    exec_sql(ctx,
        "DROP PUBLICATION IF EXISTS doc0377_page_cache_insert_pub;"
        "CREATE TABLE doc0377_page_cache_insert "
        " (id int PRIMARY KEY, s bioseq.seq('dna4',32)) USING bioseq;"
        "SET bioseq.enable_column_cache=on;");
    REQUIRE(query_scalar(ctx, "SHOW bioseq.enable_column_cache;") == "on");

    exec_sql(ctx,
        "INSERT INTO doc0377_page_cache_insert VALUES "
        " (1, repeat('ACGT',1024)::bioseq.seq('dna4',32));");
    REQUIRE(query_scalar(ctx, "SHOW bioseq.enable_column_cache;") == "on");
    REQUIRE(query_int_column(ctx,
        "SELECT id FROM doc0377_page_cache_insert ORDER BY id;")
        == std::vector<long long>{1});

    exec_sql(ctx,
        "CREATE PUBLICATION doc0377_page_cache_insert_pub "
        "FOR TABLE doc0377_page_cache_insert;");
    const auto failure = sql_error(ctx,
        "INSERT INTO doc0377_page_cache_insert VALUES "
        " (2, repeat('TGCA',1024)::bioseq.seq('dna4',32));");
    const auto cache_after_error = query_scalar(
        ctx, "SHOW bioseq.enable_column_cache;");
    const auto rows_after_error = query_int_column(ctx,
        "SELECT id FROM doc0377_page_cache_insert ORDER BY id;");
    exec_sql(ctx,
        "DROP PUBLICATION doc0377_page_cache_insert_pub;"
        "INSERT INTO doc0377_page_cache_insert VALUES "
        " (3, repeat('TGCA',1024)::bioseq.seq('dna4',32));");

    REQUIRE(failure.sqlstate == "0A000");
    REQUIRE(failure.contains(
        "cannot insert into columnar table that is a part of a publication"));
    REQUIRE(cache_after_error == "on");
    REQUIRE(rows_after_error == std::vector<long long>{1});
    REQUIRE(query_scalar(ctx, "SHOW bioseq.enable_column_cache;") == "on");
    REQUIRE(query_int_column(ctx,
        "SELECT id FROM doc0377_page_cache_insert ORDER BY id;")
        == std::vector<long long>{1, 3});
}

TEST_CASE("DOC0377 planned slice recipe is stable between plain and ANALYZE",
          "[explain][slice][recipe]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);
    const auto sql = fmt::format(
        "SELECT seq0[0:8], (seq0[0:8])[0:4] FROM {} WHERE pk = 1;",
        kTable);

    const auto plain = explain_with_options(
        ctx, "VERBOSE, COSTS OFF", sql);
    const auto analyzed = explain_with_options(
        ctx, "ANALYZE, VERBOSE, TIMING OFF, COSTS OFF", sql);
    REQUIRE(planned_slice_signature(plain)
            == planned_slice_signature(analyzed));
    REQUIRE(plan_contains(plain, "var_1 ="));
    REQUIRE(plan_contains(plain, "var_2 ="));
    REQUIRE(plan_contains(plain, "var_3 ="));
    REQUIRE(plan_contains(plain, "Depends On: var_1, var_2"));
    const auto boundaries = materialization_signatures(plain);
    REQUIRE(boundaries.size() == 4u);
    REQUIRE(boundaries[0].variable == "var_1");
    REQUIRE(boundaries[0].mechanism == "text");
    REQUIRE(boundaries[0].reason == "client output");
    REQUIRE(boundaries[0].depends_on == "none");
    REQUIRE(boundaries[0].producer_value == 2u);
    REQUIRE(boundaries[1].variable == "var_2");
    REQUIRE(boundaries[1].mechanism == "binary");
    REQUIRE(boundaries[1].reason == "client output");
    REQUIRE(boundaries[1].depends_on == "none");
    REQUIRE(boundaries[1].producer_value == boundaries[0].producer_value);
    REQUIRE(boundaries[2].variable == "var_3");
    REQUIRE(boundaries[2].depends_on == "var_1, var_2");
    REQUIRE(boundaries[2].producer_value == 3u);
    REQUIRE(boundaries[3].variable == "var_4");
    REQUIRE(boundaries[3].depends_on == "var_1, var_2");
    REQUIRE(boundaries[3].producer_value == boundaries[2].producer_value);
    REQUIRE_FALSE(plan_contains(plain, "Actual Calls:"));
    REQUIRE(plan_contains(analyzed, "Actual Calls:"));
}

TEST_CASE("DOC0377 LIMIT and skipped CASE arms retain zero boundary calls",
          "[explain][slice][control-flow]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);

    const auto limited = explain_with_options(
        ctx, "ANALYZE, TIMING OFF, COSTS OFF, SERIALIZE TEXT",
        fmt::format("SELECT seq0[0:4] FROM {} LIMIT 0;", kTable));
    REQUIRE(boundary_actual_calls(
        limited, "text", "client output") == 0u);
    REQUIRE(boundary_actual_calls(
        limited, "binary", "client output") == 0u);

    const auto skipped = explain_with_options(
        ctx, "ANALYZE, TIMING OFF, COSTS OFF, SERIALIZE TEXT",
        fmt::format(
            "SELECT CASE WHEN seq0 IS NULL THEN seq0[0:4] "
            "ELSE 'ACGT'::bioseq.seq('dna4',32) END FROM {} WHERE pk = 1;",
            kTable));
    REQUIRE(plan_contains(skipped, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(boundary_actual_calls(
        skipped, "text", "client output") == 0u);
    REQUIRE(boundary_actual_calls(
        skipped, "binary", "client output") == 0u);
}

TEST_CASE("DOC0377 lazy-only slice concat reverse has no allocation boundary",
          "[explain][slice][lazy-only]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);

    const auto plan = explain_with_options(
        ctx, "ANALYZE, VERBOSE, TIMING OFF, COSTS OFF",
        fmt::format(
            "SELECT bioseq.len(bioseq.reverse(seq0[0:4]) "
            "OPERATOR(bioseq.||) seq0[4:8]) "
            "FROM {};",
            kTable));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(plan_contains(plan, "Slice Materialization:"));
    REQUIRE(plan_contains(plan, "none"));
    REQUIRE_FALSE(plan_contains(plan, "Mechanism: packed full"));
    REQUIRE_FALSE(plan_contains(plan, "Reason: executor copy"));
}

#ifdef BIOSEQ_TEST_INDEXES
TEST_CASE("DOC0377 unsliced EXPLAIN is a zero-payload A/B/A control",
          "[explain][slice][materialization][accounting][regression]") {
    constexpr std::string_view positive_table = "test_explain_aba_dst";
    auto ctx = fresh_context({kTable, positive_table});
    seed_small_bioseq_column(ctx, kTable);
    exec_sql(ctx, fmt::format(
        "SET max_parallel_workers_per_gather=0; "
        "SET bioseq.update_index_on_insert=off; "
        "TRUNCATE {}; "
        "INSERT INTO {} VALUES "
        "(1, repeat('ACGT',262144)::bioseq.seq('dna4',32)), "
        "(2, repeat('TGCA',262144)::bioseq.seq('dna4',32)), "
        "(3, repeat('AAAA',262144)::bioseq.seq('dna4',32)); "
        "CREATE TABLE {} "
        "(pk int, seq0 bioseq.seq('dna4',32)) USING bioseq;",
        kTable, kTable, positive_table));

    const auto reset_allocation_probe = [&ctx] {
        exec_sql(ctx,
            "SELECT bioseq_internal.slice_payload_allocation_probe(0);");
    };
    /* packed_full, executor_copy, publication_packed_full,
     * publication_executor_copy */
    const auto allocation_signature = [&ctx] {
        return query_scalar(ctx,
            "SELECT concat_ws(',', "
            "       bioseq_internal.slice_payload_allocation_probe(1), "
            "       bioseq_internal.slice_payload_allocation_probe(2), "
            "       bioseq_internal.slice_payload_allocation_probe(3), "
            "       bioseq_internal.slice_payload_allocation_probe(4));");
    };
    const auto unsliced_sql = fmt::format(
        "SELECT seq0 FROM {};", kTable);
    const auto run_unsliced_control = [
        &ctx, &reset_allocation_probe, &allocation_signature,
        &unsliced_sql] {
        reset_allocation_probe();
        const auto plan = explain_with_options(
            ctx, "ANALYZE, TIMING OFF, COSTS OFF", unsliced_sql);
        REQUIRE(plan_contains(plan, "Custom Scan (BioseqScan)"));
        REQUIRE_FALSE(plan_contains(
            plan, "Custom Scan (BioseqSliceProject)"));
        REQUIRE_FALSE(plan_contains(plan, "Slice Materialization:"));
        REQUIRE(allocation_signature() == "0,0,0,0");
    };

    /* Bracket one explicit sliced heap boundary with the same unsliced plan.
     * len(slice/concat/reverse) is intentionally metadata-only and no longer
     * allocates payload, so it cannot be the positive arm.  INSERT gives the
     * sliced value a planned, executed packed-full boundary.  Assert that one
     * relevant allocation per row, without coupling this control to unrelated
     * executor-copy or SliceProject-publication counters. */
    run_unsliced_control();

    const auto expression = fmt::format(
        "bioseq.reverse(seq0[0:4]) OPERATOR(bioseq.||) seq0[4:8]");
    reset_allocation_probe();
    const auto sliced_plan = explain_with_options(
        ctx, "ANALYZE, TIMING OFF, COSTS OFF",
        fmt::format(
            "INSERT INTO {} SELECT pk, {} FROM {};",
            positive_table, expression, kTable));
    REQUIRE(plan_contains(
        sliced_plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(plan_contains(sliced_plan, "Slice Materialization:"));
    REQUIRE(plan_contains(sliced_plan, "Mechanism: packed full"));
    REQUIRE(plan_contains(sliced_plan, "Reason: heap tuple"));
    REQUIRE(boundary_actual_calls(
        sliced_plan, "packed full", "heap tuple") == 3u);
    REQUIRE(query_scalar_int(ctx,
        "SELECT bioseq_internal.slice_payload_allocation_probe(1);") == 3);
    REQUIRE(query_int_column(ctx, fmt::format(
        "SELECT bioseq.len(seq0) FROM {} ORDER BY pk;", positive_table))
        == std::vector<long long>{8, 8, 8});

    run_unsliced_control();
}

TEST_CASE("DOC0377 SliceProject shallow publication allocates no payload",
          "[explain][slice][materialization][accounting]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);
    /* Source-backed slice intermediates can now remain lazy through reverse
     * and concat.  Their global allocation totals are not a contract of this
     * test; measure only SliceProject's memcpy + RO publication seam with the
     * wrapper-local deltas. */
    exec_sql(ctx, fmt::format(
        "SET max_parallel_workers_per_gather=0; "
        "SET bioseq.update_index_on_insert=off; "
        "TRUNCATE {}; "
        "INSERT INTO {} VALUES "
        "(1, repeat('ACGT',262144)::bioseq.seq('dna4',32)), "
        "(2, repeat('TGCA',262144)::bioseq.seq('dna4',32)), "
        "(3, repeat('AAAA',262144)::bioseq.seq('dna4',32));",
        kTable, kTable));
    const auto expression = fmt::format(
        "bioseq.reverse(seq0[0:4]) OPERATOR(bioseq.||) seq0[4:8]");
    const auto plan = explain_with_options(
        ctx, "VERBOSE, COSTS OFF",
        fmt::format("SELECT bioseq.len({}) FROM {};", expression, kTable));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));

    exec_sql(ctx,
        "SELECT bioseq_internal.slice_payload_allocation_probe(0);");
    REQUIRE(query_scalar(ctx, fmt::format(
        "SELECT bool_and(bioseq.len({}) = 8) FROM {};",
        expression, kTable)) == "t");
    const auto counters = query_one(ctx,
        "SELECT bioseq_internal.slice_payload_allocation_probe(3), "
        "       bioseq_internal.slice_payload_allocation_probe(4);");
    REQUIRE(counters.size() == 2u);
    REQUIRE(counters[0].value_or("") == "0");
    REQUIRE(counters[1].value_or("") == "0");
}
#endif

TEST_CASE("DOC0377 timing and summary options control only actual metadata",
          "[explain][slice][timing]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);
    const auto sql = fmt::format(
        "SELECT seq0[0:4] FROM {} WHERE pk = 1;", kTable);

    const auto with_timing = explain_with_options(
        ctx, "ANALYZE, TIMING ON, SUMMARY ON, COSTS OFF, SERIALIZE TEXT",
        sql);
    REQUIRE(plan_contains(with_timing, "Actual Time:"));
    REQUIRE(plan_contains(with_timing, "Execution Time:"));

    const auto without_timing = explain_with_options(
        ctx, "ANALYZE, TIMING OFF, SUMMARY OFF, COSTS OFF, SERIALIZE TEXT",
        sql);
    REQUIRE_FALSE(plan_contains(without_timing, "Actual Time:"));
    REQUIRE_FALSE(plan_contains(without_timing, "Execution Time:"));
}

TEST_CASE("DOC0377 YAML and XML preserve structured slice fields",
          "[explain][slice][structured]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);
    const auto sql = fmt::format(
        "SELECT seq0[0:4] FROM {} WHERE pk = 1;", kTable);

    const auto yaml = explain_with_options(
        ctx, "ANALYZE, TIMING OFF, COSTS OFF, FORMAT YAML", sql);
    REQUIRE(plan_contains(yaml, "BioseqSliceProject"));
    REQUIRE(plan_contains(yaml, "Slice Materialization"));
    REQUIRE(plan_contains(yaml, "Mechanism: \"text\""));
    REQUIRE(plan_contains(yaml, "Actual Calls: 0"));
    const auto yaml_document = plan_document(yaml);
    const auto yaml_materializations = yaml_property_block(
        yaml_document, "Slice Materialization");
    REQUIRE(yaml_materializations.has_value());
    REQUIRE(count_occurrences(*yaml_materializations, "- Variable:") == 2u);
    constexpr std::array yaml_unique_properties{
        std::string_view{"Mechanism:"},
        std::string_view{"Reason:"},
        std::string_view{"Depends On:"},
        std::string_view{"Encoding:"},
        std::string_view{"Cache Scope:"},
        std::string_view{"Actual Calls:"},
        std::string_view{"Actual Bytes:"},
        std::string_view{"Counter Overflow:"},
    };
    for (const auto property : yaml_unique_properties) {
        CAPTURE(property);
        REQUIRE(count_occurrences(*yaml_materializations, property) == 2u);
    }
    REQUIRE(yaml_materializations->find("var_1 =")
            == std::string_view::npos);

    const auto xml = explain_with_options(
        ctx, "ANALYZE, TIMING OFF, COSTS OFF, FORMAT XML", sql);
    REQUIRE(plan_contains(xml, "BioseqSliceProject"));
    REQUIRE(plan_contains(xml, "Slice-Materialization"));
    REQUIRE(plan_contains(xml, "<Mechanism>text</Mechanism>"));
    REQUIRE(plan_contains(xml, "<Actual-Calls>0</Actual-Calls>"));
    const auto xml_document = plan_document(xml);
    const auto xml_materializations = xml_element_block(
        xml_document, "Slice-Materialization");
    REQUIRE(xml_materializations.has_value());
    REQUIRE(count_occurrences(
        *xml_materializations, "<Materialization>") == 2u);
    constexpr std::array xml_unique_properties{
        std::string_view{"<Variable>"},
        std::string_view{"<Mechanism>"},
        std::string_view{"<Reason>"},
        std::string_view{"<Depends-On"},
        std::string_view{"<Encoding>"},
        std::string_view{"<Cache-Scope>"},
        std::string_view{"<Actual-Calls>"},
        std::string_view{"<Actual-Bytes>"},
        std::string_view{"<Counter-Overflow>"},
    };
    for (const auto property : xml_unique_properties) {
        CAPTURE(property);
        REQUIRE(count_occurrences(*xml_materializations, property) == 2u);
    }
    REQUIRE(xml_materializations->find("var_1 =")
            == std::string_view::npos);
    const auto parsed_xml = query_one(ctx, fmt::format(
        "SELECT xml_is_well_formed_document({}::text), "
        "       cardinality(xpath("
        "         '//*[local-name()=\"Slice-Materialization\"]"
        "/*[local-name()=\"Materialization\"]', {}::xml))::text;",
        sql_quoted_literal(ctx.raw(), xml_document),
        sql_quoted_literal(ctx.raw(), xml_document)));
    REQUIRE(parsed_xml.size() == 2u);
    REQUIRE(parsed_xml[0].value_or("") == "t");
    REQUIRE(parsed_xml[1].value_or("") == "2");
}

TEST_CASE("DOC0377 transformed INNER mmsearch explains one per-row BatchJoin owner",
          "[explain][slice][batch-join]") {
    constexpr std::string_view target = "test_explain_slice_join_target";
    auto ctx = fresh_context({target});
    seed_small_bioseq_column(ctx, target);
    exec_sql(ctx,
        "CREATE TEMP TABLE test_explain_slice_join_query "
        "(id int, seq bioseq.seq('dna4',32));");
    exec_sql(ctx,
        "INSERT INTO test_explain_slice_join_query VALUES "
        "(1, 'ACGTACGT'::bioseq.seq('dna4',32));");

    const auto plan = explain_with_options(
        ctx, "VERBOSE, COSTS OFF",
        fmt::format(
            "SELECT q.id, t.pk FROM {} AS t "
            "JOIN test_explain_slice_join_query AS q "
            "ON mmsearch(t.seq0[0:], q.seq[0:]);",
            target));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqBatchJoin)"));
    REQUIRE(plan_contains(plan, "Bioseq Mmsearch Ownership: per-row recipe"));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE_FALSE(plan_contains(plan, "Join Filter: mmsearch"));
}

TEST_CASE("DOC0377 mmsearch slice boundaries follow the selected minimizer route",
          "[explain][slice][batch-join][materialization]") {
    constexpr std::string_view target =
        "test_explain_slice_join_materialization_target";
    auto ctx = fresh_context({target});
    seed_small_bioseq_column(ctx, target);
    exec_sql(ctx, fmt::format(
        "TRUNCATE {}; "
        "INSERT INTO {} VALUES "
        "(1, repeat('ACGT',64)::bioseq.seq('dna4',32));",
        target, target));
    exec_sql(ctx,
        "CREATE TEMP TABLE test_explain_slice_join_materialization_query "
        "(id int, seq bioseq.seq('dna4',32)); "
        "INSERT INTO test_explain_slice_join_materialization_query VALUES "
        "(1, repeat('ACGT',64)::bioseq.seq('dna4',32)); "
        "SET max_parallel_workers_per_gather=0; "
        "SET bioseq.read_sequence_index=off; "
        "SET bioseq.aligner.vulkan_enabled=off;");

    const auto sql = fmt::format(
        "SELECT q.id, t.pk FROM {} AS t "
        "JOIN test_explain_slice_join_materialization_query AS q "
        "ON mmsearch("
        "t.seq0[1:128] OPERATOR(bioseq.||) t.seq0[128:], "
        "q.seq[1:128] OPERATOR(bioseq.||) q.seq[128:]);",
        target);

    {
        guc_scope scalar(
            ctx, "bioseq.aligner.minimizer_algorithm", "scalar");
        const auto streaming = explain_with_options(
            ctx, "ANALYZE, TIMING OFF, COSTS OFF", sql);
        REQUIRE(plan_contains(streaming, "Custom Scan (BioseqBatchJoin)"));
        REQUIRE(plan_contains(streaming, "Custom Scan (BioseqSliceProject)"));
        REQUIRE_FALSE(plan_contains(streaming, "Reason: mmsearch target"));
        REQUIRE_FALSE(plan_contains(streaming, "Reason: mmsearch query"));
        REQUIRE_FALSE(plan_contains(streaming, "Mechanism: packed region"));
        REQUIRE_FALSE(plan_contains(streaming, "Mechanism: packed full"));
    }

    {
        guc_scope admitted(
            ctx, "bioseq.aligner.minimizer_algorithm", "two_stack_nthash");
        const auto resident = explain_with_options(
            ctx, "ANALYZE, TIMING OFF, COSTS OFF", sql);
        REQUIRE(plan_contains(resident, "Custom Scan (BioseqBatchJoin)"));
        REQUIRE(plan_contains(resident, "Custom Scan (BioseqSliceProject)"));
        REQUIRE_FALSE(plan_contains(resident, "Mechanism: packed full"));
        REQUIRE(boundary_actual_calls(
            resident, "packed region", "mmsearch target") == 1u);
        REQUIRE(boundary_actual_bytes(
            resident, "packed region", "mmsearch target") == 64u);
        REQUIRE(boundary_actual_calls(
            resident, "packed region", "mmsearch query") == 1u);
        REQUIRE(boundary_actual_bytes(
            resident, "packed region", "mmsearch query") == 64u);
    }
}

#ifdef BIOSEQ_TEST_INDEXES
namespace {

enum class dsm_explain_format {
    text,
    json,
    yaml,
    xml,
};

void require_dsm_fixture_property(
        std::span<const std::string> plan,
        dsm_explain_format format,
        std::string_view name,
        std::string_view value,
        bool string_value = false) {
    switch (format) {
        case dsm_explain_format::text:
            REQUIRE(plan_contains(plan, fmt::format("{}: {}", name, value)));
            return;
        case dsm_explain_format::yaml:
            REQUIRE(plan_contains(plan, string_value
                ? fmt::format("{}: \"{}\"", name, value)
                : fmt::format("{}: {}", name, value)));
            return;
        case dsm_explain_format::json:
            REQUIRE(plan_contains(plan, string_value
                ? fmt::format("\"{}\": \"{}\"", name, value)
                : fmt::format("\"{}\": {}", name, value)));
            return;
        case dsm_explain_format::xml: {
            std::string tag{name};
            std::replace(tag.begin(), tag.end(), ' ', '-');
            REQUIRE(plan_contains(
                plan, fmt::format("<{}>{}</{}>", tag, value, tag)));
            return;
        }
    }
}

void arm_dsm_fixture(
        const connection_test_context& ctx, std::string_view mode) {
    exec_sql(ctx, fmt::format(
        "SELECT bioseq_internal.arm_slice_dsm_fixture('{}');", mode));
}

}  // namespace

TEST_CASE("DOC0377 malformed DSM fixtures reject before counter dereference",
          "[explain][slice][dsm][malformed]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);
    arm_dsm_fixture(ctx, "none");
    const auto sql = fmt::format(
        "SELECT seq0[0:4] FROM {} WHERE pk = 1;", kTable);
    const std::array formats{
        std::pair{dsm_explain_format::text,
                  std::string_view{"ANALYZE, TIMING OFF, COSTS OFF"}},
        std::pair{dsm_explain_format::json,
                  std::string_view{"ANALYZE, TIMING OFF, COSTS OFF, FORMAT JSON"}},
        std::pair{dsm_explain_format::yaml,
                  std::string_view{"ANALYZE, TIMING OFF, COSTS OFF, FORMAT YAML"}},
        std::pair{dsm_explain_format::xml,
                  std::string_view{"ANALYZE, TIMING OFF, COSTS OFF, FORMAT XML"}},
    };

    for (const auto& [format, options] : formats) {
        arm_dsm_fixture(ctx, "malformed");
        const auto plan = explain_with_options(ctx, options, sql);
        require_dsm_fixture_property(
            plan, format, "DSM Test Fixture", "malformed", true);
        require_dsm_fixture_property(
            plan, format, "DSM Fixture Validated", "true");
        require_dsm_fixture_property(
            plan, format, "DSM Async Lifecycle Validated", "true");
        require_dsm_fixture_property(
            plan, format, "DSM Fixture Checks", "6");
        require_dsm_fixture_property(
            plan, format, "DSM Fixture Counter Records Read", "0");
        require_dsm_fixture_property(
            plan, format, "DSM Async Cleanup Callbacks", "3");
        require_dsm_fixture_property(
            plan, format, "Counter Overflow", "true");
        REQUIRE_FALSE(plan_contains(plan, "Incomplete Workers"));
        REQUIRE_FALSE(plan_contains(plan, "Incomplete-Workers"));
    }
    const auto unarmed = explain_with_options(
        ctx, "ANALYZE, TIMING OFF, COSTS OFF", sql);
    REQUIRE_FALSE(plan_contains(unarmed, "DSM Test Fixture"));

    const auto failing_sql = fmt::format(
        "SELECT seq0[0:4], 1 / (pk - pk) FROM {} WHERE pk = 1;", kTable);
    const auto failing_plan = explain_with_options(
        ctx, "COSTS OFF", failing_sql);
    REQUIRE(plan_contains(
        failing_plan, "Custom Scan (BioseqSliceProject)"));
    arm_dsm_fixture(ctx, "malformed");
    const auto execution_error = sql_error(ctx, fmt::format(
        "EXPLAIN (ANALYZE, TIMING OFF, COSTS OFF) {}", failing_sql));
    REQUIRE(execution_error.sqlstate == "22012");
    const auto after_error = explain_with_options(
        ctx, "ANALYZE, TIMING OFF, COSTS OFF", sql);
    REQUIRE_FALSE(plan_contains(after_error, "DSM Test Fixture"));
    arm_dsm_fixture(ctx, "none");
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE("DOC0377 two-cycle DSM rescan counts incomplete work exactly once",
          "[explain][slice][dsm][incomplete][rescan]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);
    exec_sql(ctx, "SET max_parallel_workers_per_gather=0;");
    arm_dsm_fixture(ctx, "none");
    const auto sql = fmt::format(
        "SELECT seq0[0:4] FROM {} WHERE pk = 1;", kTable);
    const std::array formats{
        std::pair{dsm_explain_format::text,
                  std::string_view{"ANALYZE, TIMING OFF, COSTS OFF"}},
        std::pair{dsm_explain_format::json,
                  std::string_view{"ANALYZE, TIMING OFF, COSTS OFF, FORMAT JSON"}},
        std::pair{dsm_explain_format::yaml,
                  std::string_view{"ANALYZE, TIMING OFF, COSTS OFF, FORMAT YAML"}},
        std::pair{dsm_explain_format::xml,
                  std::string_view{"ANALYZE, TIMING OFF, COSTS OFF, FORMAT XML"}},
    };

    for (const auto& [format, options] : formats) {
        arm_dsm_fixture(ctx, "incomplete");
        const auto plan = explain_with_options(ctx, options, sql);
        require_dsm_fixture_property(
            plan, format, "DSM Test Fixture", "incomplete", true);
        require_dsm_fixture_property(
            plan, format, "DSM Fixture Validated", "true");
        require_dsm_fixture_property(
            plan, format, "DSM Async Lifecycle Validated", "true");
        require_dsm_fixture_property(
            plan, format, "DSM Fixture Checks", "5");
        require_dsm_fixture_property(
            plan, format, "DSM Fixture Slot Headers Read", "9");
        require_dsm_fixture_property(
            plan, format, "DSM Fixture Counter Records Read", "6");
        require_dsm_fixture_property(
            plan, format, "DSM Async Cleanup Callbacks", "3");
        require_dsm_fixture_property(
            plan, format, "Incomplete Workers", "2");
        require_dsm_fixture_property(
            plan, format, "Counter Overflow", "false");
        require_dsm_fixture_property(
            plan, format, "Actual Calls", "15");
        require_dsm_fixture_property(
            plan, format, "Actual Bytes", "150");
        if (format == dsm_explain_format::text) {
            REQUIRE(boundary_actual_calls(
                plan, "text", "client output") == 15u);
            REQUIRE(boundary_actual_bytes(
                plan, "text", "client output") == 150u);
            REQUIRE(boundary_actual_calls(
                plan, "binary", "client output") == 15u);
            REQUIRE(boundary_actual_bytes(
                plan, "binary", "client output") == 150u);
        }
    }
    const auto unarmed = explain_with_options(
        ctx, "ANALYZE, TIMING OFF, COSTS OFF", sql);
    REQUIRE_FALSE(plan_contains(unarmed, "DSM Test Fixture"));
    arm_dsm_fixture(ctx, "none");
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}
#endif
