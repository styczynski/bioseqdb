/**
 * @file
 * @brief Exercises Highway dispatch metadata in Bioseq EXPLAIN output.
 */
#include <algorithm>
#include <array>
#include <cstdint>
#include <libpq-fe.h>
#include <limits>
#include <mutex>
#include <tests/support/postgres.hpp>

import bioseq.tests.integration.support;
import bioseq.tests.integration.explain_test_support;

using namespace bioseq_int_tests;
using namespace bioseq_int_tests::explain_test_support;

namespace {

#ifdef BIOSEQ_TEST_INDEXES
std::mutex highway_target_force_mutex;

/** Restore the process-local Highway target mask after a test scope. */
class highway_target_force_reset final {
  public:
    explicit highway_target_force_reset(PGconn* connection) noexcept : connection_{connection} {}

    highway_target_force_reset(const highway_target_force_reset&) = delete;
    highway_target_force_reset& operator=(const highway_target_force_reset&) = delete;

    ~highway_target_force_reset() noexcept {
        if (connection_ == nullptr)
            return;
        PGresult* result =
            PQexec(connection_, "SELECT bioseq_internal.set_highway_target_for_test(0);");
        const bool restored = result != nullptr && PQresultStatus(result) == PGRES_TUPLES_OK;
        if (result != nullptr)
            PQclear(result);
        if (!restored) {
            // Terminating/replacing the backend also discards Highway's
            // process-local target mask if SQL cleanup cannot run.
            PQreset(connection_);
        }
    }

  private:
    PGconn* connection_ = nullptr;
};
#endif

} // namespace

TEST_CASE(
    "plain EXPLAIN reports the complete Highway dispatch plan",
    "[component:bioseq][layer:integration][feature:planner][resource:postgres][resource:highway]") {
    auto ctx = fresh_context({k_table});
    seed_small_bioseq_column(ctx, k_table);
    configure_highway_simd_pipeline(ctx);

    const auto plan = explain_text<false>(ctx, explain_highway_target_sql());
    const auto planned = find_explain_property(plan, "Planned Architecture");
    const auto supported = find_explain_property(plan, "CPU-Supported Targets");
    const auto compiled = find_explain_property(plan, "Highway Compiled Targets");
    const auto runnable = find_explain_property(plan, "Runnable Targets");
    const auto executed = find_explain_property(plan, "* Executed Architecture");

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

    const std::string planned_annotation = fmt::format("[planned architecture: {}]", *planned);
    std::size_t highway_functions = 0;
    bool query_sketch_found = false;
    bool chaining_found = false;
    bool alignment_found = false;
    for (const std::string& line : plan) {
        if (line.find("Highway Function:") == std::string::npos)
            continue;
        ++highway_functions;
        REQUIRE(line.find(planned_annotation) != std::string::npos);
        query_sketch_found =
            query_sketch_found || line.find("minimizer::two_stack_nthash") != std::string::npos;
        chaining_found =
            chaining_found || line.find("chaining::chain_anchors_simd") != std::string::npos;
        alignment_found =
            alignment_found || line.find("alignment::block_aligner") != std::string::npos;
    }
    REQUIRE(highway_functions >= 3u);
    REQUIRE(query_sketch_found);
    REQUIRE(chaining_found);
    REQUIRE(alignment_found);
}

TEST_CASE(
    "EXPLAIN ANALYZE reports the executed Highway binding",
    "[component:bioseq][layer:integration][feature:planner][resource:postgres][resource:highway]") {
    auto ctx = fresh_context({k_table});
    seed_small_bioseq_column(ctx, k_table);
    configure_highway_simd_pipeline(ctx);

    const auto plan = explain_text<true>(ctx, explain_highway_target_sql());
    const auto planned = find_explain_property(plan, "Planned Architecture");
    const auto executed = find_explain_property(plan, "* Executed Architecture");

    REQUIRE(planned.has_value());
    REQUIRE(executed.has_value());
    REQUIRE(*executed != "none");
    REQUIRE(*executed != "mixed");
    REQUIRE(*executed == *planned);
}

TEST_CASE(
    "BatchJoin and PerRow EXPLAIN own every selected Highway stage",
    "[component:bioseq][layer:integration][feature:planner][resource:postgres][resource:highway]") {
    constexpr std::string_view target = "explain_highway_join_target";
    auto ctx = fresh_context({target});
    seed_small_bioseq_column(ctx, target);
    configure_highway_simd_pipeline(ctx);
    exec_sql(ctx, "SET join_collapse_limit = 1;");
    exec_sql(ctx, "SET from_collapse_limit = 1;");
    exec_sql(ctx, "SET geqo = off;");
    exec_sql(ctx,
             "CREATE TEMP TABLE explain_highway_join_query "
             "(id int PRIMARY KEY, seq bioseq.seq('dna4',32));");
    exec_sql(ctx,
             "INSERT INTO explain_highway_join_query VALUES "
             "(1, 'TTTTAAAAGGGGCCCC'::bioseq.seq('dna4',32));");

    const std::string batch_join_sql =
        fmt::format("SELECT q.id, t.pk, bioseq.edit(t.pk) AS edit_distance "
                    "FROM {} AS t "
                    "JOIN explain_highway_join_query AS q "
                    "ON mmsearch(t.seq0, q.seq);",
                    target);
    const auto batch_join = explain_with_options(ctx, "VERBOSE, COSTS OFF", batch_join_sql);
    REQUIRE(plan_contains(batch_join, "Custom Scan (BioseqBatchJoin)"));
    REQUIRE(plan_contains(batch_join, "Bioseq Batch ABI Version: 3"));
    REQUIRE(plan_contains(batch_join, "Bioseq Match Placeholder Sites: 1"));
    REQUIRE_FALSE(plan_contains(batch_join, "Bioseq Mmsearch Ownership: per-row recipe"));
    require_complete_generic_highway_pipeline(batch_join);

    const std::string per_row_sql =
        fmt::format("SELECT q.id, t.pk, bioseq.edit(t.seq0[0:]) AS edit_distance "
                    "FROM {} AS t "
                    "JOIN explain_highway_join_query AS q "
                    "ON mmsearch(t.seq0[0:], q.seq[0:]);",
                    target);
    const auto per_row = explain_with_options(ctx, "VERBOSE, COSTS OFF", per_row_sql);
    REQUIRE(plan_contains(per_row, "Custom Scan (BioseqBatchJoin)"));
    /* PostgreSQL may expose the same helper as a whole PHV or bare FuncExpr;
     * both carrier sites are distinct but resolve to one MatchResult owner. */
    REQUIRE(plan_contains(per_row, "Bioseq Match Placeholder Sites: 2"));
    REQUIRE(plan_contains(per_row, "Bioseq Mmsearch Ownership: per-row recipe"));
    require_complete_generic_highway_pipeline(per_row);
}

#ifdef BIOSEQ_TEST_INDEXES
TEST_CASE(
    "every runnable Highway target preserves public query results",
    "[component:bioseq][layer:integration][feature:planner][resource:postgres][resource:highway]") {
    const std::scoped_lock serialized{highway_target_force_mutex};
    auto ctx = fresh_context({k_table});
    highway_target_force_reset restore{ctx.raw()};
    (void)query_scalar(ctx, "SELECT bioseq_internal.set_highway_target_for_test(0);");
    seed_small_bioseq_column(ctx, k_table);
    configure_highway_simd_pipeline(ctx);

    const auto baseline_mask = static_cast<std::int64_t>(
        query_scalar_int(ctx,
                         "SELECT bioseq_internal."
                         "highway_baseline_runnable_targets_for_test();"));
    REQUIRE(baseline_mask > 0);
    REQUIRE(sql_fails(ctx, "SELECT bioseq_internal.set_highway_target_for_test(-1);"));

    const std::uint64_t baseline_bits = static_cast<std::uint64_t>(baseline_mask);
    if ((baseline_bits & (baseline_bits - std::uint64_t{1})) != 0u) {
        REQUIRE(sql_fails(
            ctx,
            fmt::format("SELECT bioseq_internal.set_highway_target_for_test({});", baseline_mask)));
    }
    const std::uint64_t unsupported_positive_bits =
        ~baseline_bits & static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    REQUIRE(unsupported_positive_bits != 0u);
    const std::uint64_t unsupported_target_bit =
        unsupported_positive_bits & (~unsupported_positive_bits + std::uint64_t{1});
    REQUIRE(sql_fails(ctx,
                      fmt::format("SELECT bioseq_internal.set_highway_target_for_test({});",
                                  static_cast<std::int64_t>(unsupported_target_bit))));

    const std::string public_sql =
        fmt::format("SELECT pk, seq0, bioseq.cigar(pk) AS cigar, "
                    "bioseq.edit(pk) AS edit_distance FROM {} WHERE mmsearch(seq0, "
                    "'TTTTAAAAGGGGCCCC'::bioseq.seq('dna4', 32)) ORDER BY pk;",
                    k_table);
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
        const std::uint64_t target_bit = remaining & (~remaining + std::uint64_t{1});
        const auto target = static_cast<std::int64_t>(target_bit);
        const std::string target_name = query_scalar(
            ctx, fmt::format("SELECT bioseq_internal.set_highway_target_for_test({});", target));
        CAPTURE(target, target_name);

        REQUIRE(query_rows(ctx, public_sql) == baseline_rows);
        const auto plan = explain_text<true>(ctx, explain_highway_target_sql());
        const auto planned = find_explain_property(plan, "Planned Architecture");
        const auto executed = find_explain_property(plan, "* Executed Architecture");
        REQUIRE(planned.has_value());
        REQUIRE(executed.has_value());
        REQUIRE(*planned == target_name);
        REQUIRE(*executed == target_name);

        remaining &= remaining - std::uint64_t{1};
    }
}

TEST_CASE(
    "KSW2 boundary receipts match EMU128 on every runnable Highway target",
    "[component:bioseq][layer:integration][feature:planner][resource:postgres][resource:highway]") {
    const std::scoped_lock serialized{highway_target_force_mutex};
    auto ctx = fresh_context();
    highway_target_force_reset restore{ctx.raw()};
    (void)query_scalar(ctx, "SELECT bioseq_internal.set_highway_target_for_test(0);");

    const auto baseline_mask = static_cast<std::int64_t>(
        query_scalar_int(ctx,
                         "SELECT bioseq_internal."
                         "highway_baseline_runnable_targets_for_test();"));
    REQUIRE(baseline_mask > 0);
    const std::uint64_t baseline_bits = static_cast<std::uint64_t>(baseline_mask);

    std::string emu128_receipt;
    std::uint64_t remaining = baseline_bits;
    while (remaining != 0u) {
        const std::uint64_t target_bit = remaining & (~remaining + std::uint64_t{1});
        const auto target = static_cast<std::int64_t>(target_bit);
        const std::string target_name = query_scalar(
            ctx, fmt::format("SELECT bioseq_internal.set_highway_target_for_test({});", target));
        if (target_name == "EMU128") {
            emu128_receipt = query_scalar(ctx,
                                          "SELECT bioseq_internal."
                                          "highway_ksw_boundary_receipt_for_test();");
            break;
        }
        remaining &= remaining - std::uint64_t{1};
    }
    REQUIRE_FALSE(emu128_receipt.empty());

    remaining = baseline_bits;
    while (remaining != 0u) {
        const std::uint64_t target_bit = remaining & (~remaining + std::uint64_t{1});
        const auto target = static_cast<std::int64_t>(target_bit);
        const std::string target_name = query_scalar(
            ctx, fmt::format("SELECT bioseq_internal.set_highway_target_for_test({});", target));
        CAPTURE(target, target_name);
        const std::string receipt = query_scalar(ctx,
                                                 "SELECT bioseq_internal."
                                                 "highway_ksw_boundary_receipt_for_test();");
        REQUIRE_FALSE(receipt.empty());
        REQUIRE(receipt == emu128_receipt);
        remaining &= remaining - std::uint64_t{1};
    }
}
#endif

TEST_CASE(
    "EXPLAIN FORMAT JSON owns one complete Highway dispatch object",
    "[component:bioseq][layer:integration][feature:planner][resource:postgres][resource:highway]") {
    auto ctx = fresh_context({k_table});
    seed_small_bioseq_column(ctx, k_table);
    configure_highway_simd_pipeline(ctx);

    const std::string json = explain_json(ctx, explain_highway_target_sql());

    REQUIRE_FALSE(json.empty());
    // BioseqScan and the Custom-Scan node-type string must both appear.
    REQUIRE(json.find("BioseqScan") != std::string::npos);
    REQUIRE(json.find("Custom Scan") != std::string::npos);
    REQUIRE(count_occurrences(json, "\"Highway SIMD Dispatch\"") == 1u);
    const auto highway_dispatch = json_object_member(json, "Highway SIMD Dispatch");
    REQUIRE(highway_dispatch.has_value());
    for (const std::string_view property : {
             "\"Planned Architecture\"",
             "\"CPU-Supported Targets\"",
             "\"Highway Compiled Targets\"",
             "\"Runnable Targets\"",
             "\"Executed Architecture\"",
         }) {
        CAPTURE(property);
        REQUIRE(count_occurrences(*highway_dispatch, property) == 1u);
    }
    REQUIRE(json.find("\"Highway Functions\"") != std::string::npos);
    REQUIRE(json.find("minimizer::two_stack_nthash") != std::string::npos);
    REQUIRE(json.find("chaining::chain_anchors_simd") != std::string::npos);
    REQUIRE(json.find("alignment::block_aligner") != std::string::npos);
}

TEST_CASE("structured plan omits an execution target before ANALYZE",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    seed_small_bioseq_column(ctx, k_table);

    const auto rows =
        explain_with_options(ctx, "FORMAT JSON, VERBOSE, COSTS OFF", explain_highway_target_sql());
    const std::string json = plan_document(rows);

    REQUIRE(count_occurrences(json, "\"Highway SIMD Dispatch\"") == 1u);
    const auto highway_dispatch = json_object_member(json, "Highway SIMD Dispatch");
    REQUIRE(highway_dispatch.has_value());
    REQUIRE(highway_dispatch->find("\"Planned Architecture\"") != std::string_view::npos);
    REQUIRE(highway_dispatch->find("\"CPU-Supported Targets\"") != std::string_view::npos);
    REQUIRE(highway_dispatch->find("\"Highway Compiled Targets\"") != std::string_view::npos);
    REQUIRE(highway_dispatch->find("\"Runnable Targets\"") != std::string_view::npos);
    REQUIRE(highway_dispatch->find("\"Executed Architecture\"") == std::string_view::npos);
}
