/** @file Deterministic randomized properties for lazy sequence expressions. */

#include <libpq-fe.h>

#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.tests.integration.support;

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view k_expression_table =
    "lazy_expression_property";
constexpr std::uint64_t k_property_root_seed = 249'767'362'635'374'630ULL;
constexpr std::size_t k_property_case_count = 96u;
constexpr std::uint32_t k_no_child =
    (std::numeric_limits<std::uint32_t>::max)();

/** A library-independent deterministic word stream with stable output. */
class deterministic_words {
public:
    explicit deterministic_words(std::uint64_t seed) noexcept : state_(seed) {}

    /** Return the next SplitMix64 word. */
    [[nodiscard]] std::uint64_t next() noexcept {
        state_ += 0x9e37'79b9'7f4a'7c15ULL;
        std::uint64_t word = state_;
        word = (word ^ (word >> 30u)) * 0xbf58'476d'1ce4'e5b9ULL;
        word = (word ^ (word >> 27u)) * 0x94d0'49bb'1331'11ebULL;
        return word ^ (word >> 31u);
    }

    /** Return a stable value in [0, exclusive). */
    [[nodiscard]] std::size_t below(std::size_t exclusive) noexcept {
        return exclusive == 0u
            ? 0u
            : static_cast<std::size_t>(next() % exclusive);
    }

    /** Return true exactly one out of each denominator-sized word buckets. */
    [[nodiscard]] bool one_in(std::size_t denominator) noexcept {
        return denominator != 0u && below(denominator) == 0u;
    }

private:
    std::uint64_t state_;
};

/** Operations represented by the independently evaluated expression tree. */
enum class expression_kind : std::uint8_t {
    source_a,
    source_b,
    null_source,
    literal,
    concat,
    slice,
    reversed,
    complemented,
    reverse_complement,
};

/** SQL slice-bound forms emitted by the property generator. */
enum class bound_kind : std::uint8_t {
    omitted,
    value,
    null_value,
};

/** One generated slice bound and its optional numeric value. */
struct slice_bound {
    bound_kind kind = bound_kind::omitted;
    std::int64_t value = 0;
};

/** Arena node carrying both SQL structure and its eager expected value. */
struct expression_node {
    expression_kind kind = expression_kind::literal;
    std::uint32_t left = k_no_child;
    std::uint32_t right = k_no_child;
    std::string literal;
    slice_bound lower;
    slice_bound upper;
    std::optional<std::string> eager_symbols;
    std::size_t node_count = 1u;
};

/** Generated expression arena with its root and operation coverage mask. */
struct expression_tree {
    std::vector<expression_node> nodes;
    std::uint32_t root = k_no_child;
    std::uint32_t seen_kind_mask = 0u;
};

/** Complement one canonical DNA4 symbol without using production traits. */
[[nodiscard]] char eager_complement(char symbol) {
    switch (symbol) {
        case 'A': return 'T';
        case 'C': return 'G';
        case 'G': return 'C';
        case 'T': return 'A';
        default:
            FAIL(fmt::format("invalid eager DNA4 symbol `{}`", symbol));
    }
}

/** Complement a string in logical order. */
[[nodiscard]] std::string eager_complemented(std::string value) {
    std::transform(
        value.begin(), value.end(), value.begin(), eager_complement);
    return value;
}

/** Reverse-complement a string without calling production sequence code. */
[[nodiscard]] std::string eager_reverse_complement(std::string value) {
    std::reverse(value.begin(), value.end());
    return eager_complemented(std::move(value));
}

/** Normalize a Python slice bound independently and without signed overflow. */
[[nodiscard]] std::size_t eager_slice_bound(
        std::int64_t bound,
        std::size_t length) noexcept {
    if (bound >= 0) {
        const auto positive = static_cast<std::uint64_t>(bound);
        return positive >= length
            ? length
            : static_cast<std::size_t>(positive);
    }
    const std::uint64_t magnitude =
        static_cast<std::uint64_t>(-(bound + 1)) + 1u;
    return magnitude >= length
        ? 0u
        : length - static_cast<std::size_t>(magnitude);
}

/** Normalize a Python element index, returning nullopt when it is outside. */
[[nodiscard]] std::optional<std::size_t> eager_element_index(
        std::int64_t index,
        std::size_t length) noexcept {
    if (index >= 0) {
        const auto positive = static_cast<std::uint64_t>(index);
        if (positive >= length) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(positive);
    }
    const std::uint64_t magnitude =
        static_cast<std::uint64_t>(-(index + 1)) + 1u;
    if (magnitude > length) {
        return std::nullopt;
    }
    return length - static_cast<std::size_t>(magnitude);
}

/** Apply the normative half-open Python slice rules to an eager value. */
[[nodiscard]] std::optional<std::string> eager_slice(
        const std::optional<std::string>& value,
        slice_bound lower,
        slice_bound upper) {
    if (!value.has_value()
        || lower.kind == bound_kind::null_value
        || upper.kind == bound_kind::null_value) {
        return std::nullopt;
    }
    const std::size_t length = value->size();
    const std::size_t from = lower.kind == bound_kind::omitted
        ? 0u
        : eager_slice_bound(lower.value, length);
    const std::size_t to = upper.kind == bound_kind::omitted
        ? length
        : eager_slice_bound(upper.value, length);
    if (to <= from) {
        return std::string{};
    }
    return value->substr(from, to - from);
}

/** Apply one sequence operation in the independent eager evaluator. */
[[nodiscard]] std::optional<std::string> eager_unary(
        expression_kind kind,
        const std::optional<std::string>& input) {
    if (!input.has_value()) {
        return std::nullopt;
    }
    std::string value = *input;
    switch (kind) {
        case expression_kind::reversed:
            std::reverse(value.begin(), value.end());
            return value;
        case expression_kind::complemented:
            return eager_complemented(std::move(value));
        case expression_kind::reverse_complement:
            return eager_reverse_complement(std::move(value));
        default:
            FAIL("eager_unary received a non-unary operation");
    }
}

/** Build stable DNA symbols for source and literal leaves. */
[[nodiscard]] std::string make_symbols(
        std::size_t length,
        std::uint64_t seed) {
    deterministic_words words(seed);
    constexpr std::array symbols{'A', 'C', 'G', 'T'};
    std::string result(length, 'A');
    for (char& symbol : result) {
        symbol = symbols[words.below(symbols.size())];
    }
    return result;
}

/** Generate signed bounds around both edges, with rare exact int64 limits. */
[[nodiscard]] slice_bound make_random_bound(
        deterministic_words& words,
        std::size_t length,
        bool allow_null) noexcept {
    const std::size_t selector = words.below(24u);
    if (selector < 3u) {
        return slice_bound{.kind = bound_kind::omitted};
    }
    if (allow_null && selector == 3u) {
        return slice_bound{.kind = bound_kind::null_value};
    }
    if (selector == 4u) {
        return slice_bound{
            .kind = bound_kind::value,
            .value = (std::numeric_limits<std::int64_t>::min)(),
        };
    }
    if (selector == 5u) {
        return slice_bound{
            .kind = bound_kind::value,
            .value = (std::numeric_limits<std::int64_t>::max)(),
        };
    }
    const std::size_t radius = (std::min)(length, std::size_t{2048u}) + 8u;
    const std::size_t offset = words.below(radius * 2u + 1u);
    return slice_bound{
        .kind = bound_kind::value,
        .value = static_cast<std::int64_t>(offset)
            - static_cast<std::int64_t>(radius),
    };
}

/** Generate one tree bottom-up while computing its eager value independently. */
class expression_generator {
public:
    expression_generator(
        std::uint64_t seed,
        std::string source_a,
        std::string source_b)
        : words_(seed),
          source_a_(std::move(source_a)),
          source_b_(std::move(source_b)) {}

    /** Generate a tree whose root kind can be forced for coverage. */
    [[nodiscard]] expression_tree generate(
            std::size_t depth,
            expression_kind root_kind) {
        expression_tree tree;
        tree.nodes.reserve((std::size_t{1u} << (depth + 1u)) - 1u);
        tree.root = build(tree, depth, root_kind, true);
        return tree;
    }

private:
    /** Append a completed node and return its stable arena index. */
    [[nodiscard]] std::uint32_t append(
            expression_tree& tree,
            expression_node node) {
        REQUIRE(tree.nodes.size() < k_no_child);
        const auto index = static_cast<std::uint32_t>(tree.nodes.size());
        tree.seen_kind_mask |=
            std::uint32_t{1u} << static_cast<std::uint8_t>(node.kind);
        tree.nodes.push_back(std::move(node));
        return index;
    }

    /** Generate a source, NULL source, or bound literal leaf. */
    [[nodiscard]] std::uint32_t build_leaf(expression_tree& tree) {
        switch (words_.below(6u)) {
            case 0u:
                return append(tree, expression_node{
                    .kind = expression_kind::source_a,
                    .eager_symbols = source_a_,
                });
            case 1u:
                return append(tree, expression_node{
                    .kind = expression_kind::source_b,
                    .eager_symbols = source_b_,
                });
            case 2u:
                return append(tree, expression_node{
                    .kind = expression_kind::null_source,
                    .eager_symbols = std::nullopt,
                });
            default: {
                const std::string literal = make_symbols(
                    words_.below(25u), words_.next());
                return append(tree, expression_node{
                    .kind = expression_kind::literal,
                    .literal = literal,
                    .eager_symbols = literal,
                });
            }
        }
    }

    /** Recursively generate one operation or terminate at a leaf. */
    [[nodiscard]] std::uint32_t build(
            expression_tree& tree,
            std::size_t depth,
            expression_kind requested_kind,
            bool force_kind) {
        if (depth == 0u || (!force_kind && words_.one_in(5u))) {
            return build_leaf(tree);
        }
        constexpr std::array operation_kinds{
            expression_kind::concat,
            expression_kind::slice,
            expression_kind::reversed,
            expression_kind::complemented,
            expression_kind::reverse_complement,
        };
        const expression_kind kind = force_kind
            ? requested_kind
            : operation_kinds[words_.below(operation_kinds.size())];
        if (kind == expression_kind::concat) {
            const std::uint32_t left = build(
                tree, depth - 1u, expression_kind::literal, false);
            const std::uint32_t right = build(
                tree, depth - 1u, expression_kind::literal, false);
            const auto& lhs = tree.nodes[left].eager_symbols;
            const auto& rhs = tree.nodes[right].eager_symbols;
            std::optional<std::string> eager;
            if (lhs.has_value() && rhs.has_value()) {
                eager = *lhs + *rhs;
            }
            return append(tree, expression_node{
                .kind = kind,
                .left = left,
                .right = right,
                .eager_symbols = std::move(eager),
                .node_count = tree.nodes[left].node_count
                    + tree.nodes[right].node_count + 1u,
            });
        }

        const std::uint32_t child = build(
            tree, depth - 1u, expression_kind::literal, false);
        if (kind == expression_kind::slice) {
            const std::size_t length =
                tree.nodes[child].eager_symbols.has_value()
                ? tree.nodes[child].eager_symbols->size()
                : 0u;
            const slice_bound lower = make_random_bound(words_, length, true);
            const slice_bound upper = make_random_bound(words_, length, true);
            return append(tree, expression_node{
                .kind = kind,
                .left = child,
                .lower = lower,
                .upper = upper,
                .eager_symbols = eager_slice(
                    tree.nodes[child].eager_symbols, lower, upper),
                .node_count = tree.nodes[child].node_count + 1u,
            });
        }
        return append(tree, expression_node{
            .kind = kind,
            .left = child,
            .eager_symbols = eager_unary(
                kind, tree.nodes[child].eager_symbols),
            .node_count = tree.nodes[child].node_count + 1u,
        });
    }

    deterministic_words words_;
    std::string source_a_;
    std::string source_b_;
};

/** Render a bound expression leaf or lazy operation as SQL. */
[[nodiscard]] std::string render_bound(slice_bound bound) {
    switch (bound.kind) {
        case bound_kind::omitted: return {};
        case bound_kind::null_value: return "NULL::bigint";
        case bound_kind::value: return fmt::format("{}", bound.value);
    }
}

/** Render any generated subtree as a standalone bioseq.seq SQL expression. */
[[nodiscard]] std::string render_expression(
        const expression_tree& tree,
        std::uint32_t node_index) {
    const expression_node& node = tree.nodes[node_index];
    switch (node.kind) {
        case expression_kind::source_a: return "source_a";
        case expression_kind::source_b: return "source_b";
        case expression_kind::null_source: return "source_null";
        case expression_kind::literal:
            return format_literal(
                node.literal,
                seq_typmod{.alphabet = "dna4", .width = 32});
        case expression_kind::concat:
            return fmt::format(
                "({} OPERATOR(bioseq.||) {})",
                render_expression(tree, node.left),
                render_expression(tree, node.right));
        case expression_kind::slice:
            return fmt::format(
                "({})[{}:{}]",
                render_expression(tree, node.left),
                render_bound(node.lower),
                render_bound(node.upper));
        case expression_kind::reversed:
            return fmt::format(
                "bioseq.reverse({})",
                render_expression(tree, node.left));
        case expression_kind::complemented:
            // SQL intentionally has no standalone complement function. This
            // composition is the public route to the internal complement bit.
            return fmt::format(
                "bioseq.reverse(bioseq.reverse_complement({}))",
                render_expression(tree, node.left));
        case expression_kind::reverse_complement:
            return fmt::format(
                "bioseq.reverse_complement({})",
                render_expression(tree, node.left));
    }
}

/** Edge-heavy element and slice coordinates for one expression result. */
struct coordinate_probes {
    std::array<std::int64_t, 6u> elements;
    std::array<std::pair<slice_bound, slice_bound>, 6u> slices;
};

/** Build edge-heavy coordinate probes plus two case-specific random windows. */
[[nodiscard]] coordinate_probes make_coordinate_probes(
        std::size_t length,
        std::uint64_t seed) noexcept {
    deterministic_words words(seed);
    const std::size_t capped_length = (std::min)(
        length,
        static_cast<std::size_t>(
            (std::numeric_limits<std::int64_t>::max)() - 16));
    const auto signed_length = static_cast<std::int64_t>(capped_length);
    coordinate_probes probes{
        .elements = {
            0,
            -1,
            signed_length,
            signed_length == (std::numeric_limits<std::int64_t>::max)()
                ? (std::numeric_limits<std::int64_t>::min)()
                : -signed_length - 1,
            static_cast<std::int64_t>(words.below(capped_length + 9u)) - 4,
            (std::numeric_limits<std::int64_t>::min)(),
        },
        .slices = {
            std::pair{slice_bound{.kind = bound_kind::omitted},
                      slice_bound{.kind = bound_kind::omitted}},
            std::pair{slice_bound{.kind = bound_kind::value, .value = 1},
                      slice_bound{.kind = bound_kind::value, .value = -1}},
            std::pair{slice_bound{.kind = bound_kind::value, .value = -3},
                      slice_bound{.kind = bound_kind::value,
                                  .value = signed_length + 3}},
            std::pair{
                slice_bound{.kind = bound_kind::value,
                            .value = (std::numeric_limits<std::int64_t>::min)()},
                slice_bound{.kind = bound_kind::value,
                            .value = (std::numeric_limits<std::int64_t>::max)()}},
            std::pair{make_random_bound(words, length, false),
                      make_random_bound(words, length, false)},
            std::pair{slice_bound{.kind = bound_kind::null_value},
                      slice_bound{.kind = bound_kind::value, .value = 1}},
        },
    };
    return probes;
}

/** Return the eager element text or SQL NULL for one coordinate probe. */
[[nodiscard]] std::optional<std::string> eager_element(
        const std::optional<std::string>& value,
        std::int64_t index) {
    if (!value.has_value()) {
        return std::nullopt;
    }
    const auto normalized = eager_element_index(index, value->size());
    if (!normalized.has_value()) {
        return std::nullopt;
    }
    return std::string(1u, (*value)[*normalized]);
}

/** Rendered property query paired with its independently computed row. */
struct property_query {
    std::string sql;
    std::vector<std::optional<std::string>> expected_row;
};

/** Build one SQL query that checks every required logical consumer at once. */
[[nodiscard]] property_query make_property_query(
        const expression_tree& tree,
        std::uint32_t node_index,
        std::uint64_t probe_seed) {
    const expression_node& node = tree.nodes[node_index];
    const std::size_t length = node.eager_symbols.has_value()
        ? node.eager_symbols->size()
        : 0u;
    const coordinate_probes probes = make_coordinate_probes(length, probe_seed);
    const std::string expected_sql = node.eager_symbols.has_value()
        ? format_literal(
            *node.eager_symbols,
            seq_typmod{.alphabet = "dna4", .width = 32})
        : "NULL::bioseq.seq('dna4',32)";

    property_query query;
    query.sql = fmt::format(
        "WITH actual(value) AS NOT MATERIALIZED ("
        " SELECT {} FROM {} WHERE id=1), "
        "expected(value) AS (VALUES ({})) "
        "SELECT (actual.value IS NULL), actual.value::text, "
        "       bioseq.len(actual.value)::text, "
        "       (bioseq.seq_cmp(actual.value, expected.value)=0), "
        "       (bioseq.seq_hash(actual.value)="
        "          bioseq.seq_hash(expected.value)), "
        "       (bioseq.seq_hash_extended(actual.value,77)="
        "          bioseq.seq_hash_extended(expected.value,77)), "
        "       (actual.value OPERATOR(bioseq.=) expected.value)",
        render_expression(tree, node_index), k_expression_table, expected_sql);

    query.expected_row.reserve(
        7u + probes.elements.size() + probes.slices.size());
    query.expected_row.push_back(node.eager_symbols.has_value() ? "f" : "t");
    query.expected_row.push_back(node.eager_symbols);
    query.expected_row.push_back(node.eager_symbols.has_value()
        ? std::optional<std::string>{fmt::format("{}", length)}
        : std::nullopt);
    for (std::size_t index = 0u; index < 4u; ++index) {
        query.expected_row.push_back(node.eager_symbols.has_value()
            ? std::optional<std::string>{"t"}
            : std::nullopt);
    }

    for (const std::int64_t index : probes.elements) {
        query.sql += fmt::format(
            ", ((actual.value)[{}])::text", index);
        query.expected_row.push_back(eager_element(node.eager_symbols, index));
    }
    for (const auto& [lower, upper] : probes.slices) {
        query.sql += fmt::format(
            ", ((actual.value)[{}:{}])::text",
            render_bound(lower), render_bound(upper));
        query.expected_row.push_back(eager_slice(
            node.eager_symbols, lower, upper));
    }
    query.sql += " FROM actual CROSS JOIN expected;";
    return query;
}

/** SQL execution outcome used to report and minimize property failures. */
struct sql_observation {
    bool succeeded = false;
    std::string sqlstate;
    std::string message;
    std::vector<std::optional<std::string>> row;
};

/** Execute SQL without assertions so failing properties can be shrunk. */
[[nodiscard]] sql_observation observe_sql(
        const connection_test_context& ctx,
        const std::string& sql) {
    sql_observation observation;
    PGresult* result = PQexec(ctx.raw(), sql.c_str());
    if (result == nullptr) {
        observation.message = PQerrorMessage(ctx.raw());
        return observation;
    }
    const ExecStatusType status = PQresultStatus(result);
    if (status != PGRES_TUPLES_OK) {
        const char* state = PQresultErrorField(result, PG_DIAG_SQLSTATE);
        const char* message = PQresultErrorMessage(result);
        observation.sqlstate = state == nullptr ? "" : state;
        observation.message = message == nullptr ? "" : message;
        PQclear(result);
        return observation;
    }
    observation.succeeded = true;
    if (PQntuples(result) != 1) {
        observation.message = fmt::format(
            "expected one row, observed {}", PQntuples(result));
        observation.succeeded = false;
        PQclear(result);
        return observation;
    }
    const int column_count = PQnfields(result);
    observation.row.reserve(static_cast<std::size_t>(column_count));
    for (int column = 0; column < column_count; ++column) {
        if (PQgetisnull(result, 0, column)) {
            observation.row.emplace_back(std::nullopt);
        } else {
            observation.row.emplace_back(
                std::string{PQgetvalue(result, 0, column)});
        }
    }
    PQclear(result);
    return observation;
}

/** Explain the first mismatch between a SQL observation and eager row. */
[[nodiscard]] std::optional<std::string> property_mismatch(
        const sql_observation& observation,
        const property_query& query) {
    if (!observation.succeeded) {
        return fmt::format(
            "SQLSTATE={} error={}",
            observation.sqlstate, observation.message);
    }
    if (observation.row.size() != query.expected_row.size()) {
        return fmt::format(
            "column count expected={} observed={}",
            query.expected_row.size(), observation.row.size());
    }
    for (std::size_t column = 0u;
         column < query.expected_row.size(); ++column) {
        if (observation.row[column] != query.expected_row[column]) {
            return fmt::format(
                "column={} expected={} observed={}",
                column,
                query.expected_row[column].value_or("<NULL>"),
                observation.row[column].value_or("<NULL>"));
        }
    }
    return std::nullopt;
}

/** Smallest generated subtree that still reproduces a property mismatch. */
struct minimized_failure {
    std::uint32_t node_index;
    property_query query;
    std::string mismatch;
};

/** Search all generated subtrees from smallest to largest for a reproducer. */
[[nodiscard]] minimized_failure minimize_failure(
        const connection_test_context& ctx,
        const expression_tree& tree,
        std::uint32_t original_node,
        std::uint64_t probe_seed) {
    std::vector<std::uint32_t> candidates(tree.nodes.size());
    for (std::size_t index = 0u; index < candidates.size(); ++index) {
        candidates[index] = static_cast<std::uint32_t>(index);
    }
    std::sort(
        candidates.begin(), candidates.end(),
        [&tree](std::uint32_t left, std::uint32_t right) {
            return tree.nodes[left].node_count < tree.nodes[right].node_count;
        });
    for (const std::uint32_t candidate : candidates) {
        property_query query = make_property_query(
            tree, candidate, probe_seed ^ candidate);
        const sql_observation observation = observe_sql(ctx, query.sql);
        if (auto mismatch = property_mismatch(observation, query)) {
            return minimized_failure{
                .node_index = candidate,
                .query = std::move(query),
                .mismatch = std::move(*mismatch),
            };
        }
    }
    property_query query = make_property_query(
        tree, original_node, probe_seed);
    const sql_observation observation = observe_sql(ctx, query.sql);
    std::string mismatch = property_mismatch(observation, query).value_or(
        "original failure did not reproduce during shrinking");
    return minimized_failure{
        .node_index = original_node,
        .query = std::move(query),
        .mismatch = std::move(mismatch),
    };
}

/** Render a random expression that deliberately remains alphabet-unbound. */
[[nodiscard]] std::string make_unbound_expression(
        deterministic_words& words,
        std::size_t depth) {
    if (depth == 0u || words.one_in(4u)) {
        return fmt::format(
            "'{}'::bioseq.seq",
            make_symbols(words.below(17u) + 1u, words.next()));
    }
    switch (words.below(3u)) {
        case 0u:
            return fmt::format(
                "({} OPERATOR(bioseq.||) {})",
                make_unbound_expression(words, depth - 1u),
                make_unbound_expression(words, depth - 1u));
        case 1u: {
            const slice_bound lower = make_random_bound(words, 32u, false);
            const slice_bound upper = make_random_bound(words, 32u, false);
            return fmt::format(
                "({})[{}:{}]",
                make_unbound_expression(words, depth - 1u),
                render_bound(lower), render_bound(upper));
        }
        default:
            return fmt::format(
                "bioseq.reverse({})",
                make_unbound_expression(words, depth - 1u));
    }
}

}  // namespace

TEST_CASE("randomized lazy expression trees match an eager oracle",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
    const std::string source_a = make_symbols(521u, 0xa11c'e5a1'0000'0001ULL);
    const std::string source_b = make_symbols(389u, 0xb22c'e5b2'0000'0002ULL);
    auto ctx = fresh_context({k_expression_table});
    exec_sql(ctx, fmt::format(
        "CREATE TABLE {} ("
        " id int PRIMARY KEY,"
        " source_a bioseq.seq('dna4',32),"
        " source_b bioseq.seq('dna4',32),"
        " source_null bioseq.seq('dna4',32)) USING bioseq;"
        "INSERT INTO {} VALUES (1, {}, {}, NULL);",
        k_expression_table, k_expression_table,
        format_literal(
            source_a, seq_typmod{.alphabet = "dna4", .width = 32}),
        format_literal(
            source_b, seq_typmod{.alphabet = "dna4", .width = 32})));

    constexpr std::array forced_roots{
        expression_kind::concat,
        expression_kind::slice,
        expression_kind::reversed,
        expression_kind::complemented,
        expression_kind::reverse_complement,
    };
    std::uint32_t seen_kind_mask = 0u;
    deterministic_words case_words(k_property_root_seed);
    for (std::size_t case_index = 0u;
         case_index < k_property_case_count; ++case_index) {
        const std::uint64_t case_seed = case_words.next();
        expression_generator generator(case_seed, source_a, source_b);
        expression_tree tree = generator.generate(
            3u + case_index % 5u,
            forced_roots[case_index % forced_roots.size()]);
        seen_kind_mask |= tree.seen_kind_mask;
        const std::uint64_t probe_seed =
            case_seed ^ 0x5a17'c00d'1eaf'beefULL;
        property_query query = make_property_query(
            tree, tree.root, probe_seed);
        const sql_observation observation = observe_sql(ctx, query.sql);
        if (auto mismatch = property_mismatch(observation, query)) {
            const minimized_failure minimal = minimize_failure(
                ctx, tree, tree.root, probe_seed);
            FAIL(fmt::format(
                "randomized lazy-expression property failed\n"
                "root_seed=0x{:016x} case={} case_seed=0x{:016x}\n"
                "original_nodes={} original_mismatch={}\n"
                "minimal_node={} minimal_nodes={} minimal_mismatch={}\n"
                "replay_sql={}",
                k_property_root_seed, case_index, case_seed,
                tree.nodes[tree.root].node_count, *mismatch,
                minimal.node_index,
                tree.nodes[minimal.node_index].node_count,
                minimal.mismatch, minimal.query.sql));
        }
    }

    constexpr std::array required_kinds{
        expression_kind::source_a,
        expression_kind::source_b,
        expression_kind::null_source,
        expression_kind::literal,
        expression_kind::concat,
        expression_kind::slice,
        expression_kind::reversed,
        expression_kind::complemented,
        expression_kind::reverse_complement,
    };
    for (const expression_kind kind : required_kinds) {
        INFO(fmt::format(
            "root_seed=0x{:016x} missing_kind={}",
            k_property_root_seed, static_cast<unsigned>(kind)));
        REQUIRE((seen_kind_mask
                 & (std::uint32_t{1u} << static_cast<std::uint8_t>(kind)))
                != 0u);
    }
}

TEST_CASE("randomized unbound transform trees preserve error outcome",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
    auto ctx = fresh_context();
    deterministic_words case_words(
        k_property_root_seed ^ 0xe220'4280'4000'0001ULL);
    for (std::size_t case_index = 0u; case_index < 24u; ++case_index) {
        const std::uint64_t case_seed = case_words.next();
        deterministic_words expression_words(case_seed);
        const std::string input = make_unbound_expression(
            expression_words, 2u + case_index % 5u);
        const std::string expression = case_index % 2u == 0u
            ? fmt::format("bioseq.reverse_complement({})", input)
            : fmt::format(
                "bioseq.reverse(bioseq.reverse_complement({}))", input);
        const std::string sql = fmt::format(
            "SELECT ({})::text;", expression);
        const sql_observation observation = observe_sql(ctx, sql);
        if (observation.succeeded || observation.sqlstate != "42804") {
            FAIL(fmt::format(
                "unbound transform property failed\n"
                "root_seed=0x{:016x} case={} case_seed=0x{:016x}\n"
                "expected_sqlstate=42804 observed_sqlstate={} message={}\n"
                "replay_sql={}",
                k_property_root_seed, case_index, case_seed,
                observation.sqlstate, observation.message, sql));
        }
        REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
    }
}

TEST_CASE("effectful expression leaves execute once and skipped leaves never",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
    auto ctx = fresh_context();
    exec_sql(ctx,
        "CREATE TEMP SEQUENCE expression_effect_counter START 1;"
        "CREATE FUNCTION pg_temp.property_effect(value text) "
        "RETURNS bioseq.seq LANGUAGE plpgsql VOLATILE AS $$ "
        "BEGIN "
        "  PERFORM nextval('pg_temp.expression_effect_counter'); "
        "  RETURN value::bioseq.seq('dna4',32); "
        "END $$;"
        "CREATE FUNCTION pg_temp.property_throw() "
        "RETURNS bioseq.seq LANGUAGE plpgsql VOLATILE AS $$ "
        "BEGIN RAISE EXCEPTION 'unselected property leaf executed'; END $$;");

    REQUIRE(query_scalar(
        ctx,
        "SELECT bioseq.reverse("
        "  (pg_temp.property_effect('ACGT'))[:3] "
        "  OPERATOR(bioseq.||) "
        "  bioseq.reverse(bioseq.reverse_complement("
        "    pg_temp.property_effect('TGCA'))))::text;")
        == "TGCAGCA");
    REQUIRE(query_scalar_int(
        ctx, "SELECT last_value FROM expression_effect_counter;") == 2);
    REQUIRE(query_scalar(
        ctx,
        "SELECT (CASE WHEN true "
        "  THEN ('AC'::bioseq.seq('dna4',32))[0:] "
        "  ELSE pg_temp.property_throw() END)::text;") == "AC");
    REQUIRE(query_scalar_int(
        ctx, "SELECT last_value FROM expression_effect_counter;") == 2);
}
