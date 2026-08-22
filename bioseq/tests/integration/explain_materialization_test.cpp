/**
 * @file
 * @brief Exercises Bioseq materialization and slice EXPLAIN metrics.
 */
#include <libpq-fe.h>
#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.tests.integration.support;
import bioseq.tests.integration.explain_test_support;

using namespace bioseq_int_tests;
using namespace bioseq_int_tests::explain_test_support;

TEST_CASE("plain EXPLAIN reports only materialization boundaries",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    seed_small_bioseq_column(ctx, k_table);

    const auto plan = explain_with_options(
        ctx, "VERBOSE, COSTS OFF", fmt::format("SELECT seq0[0:4] FROM {};", k_table));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(plan_contains(plan, "Slice Materialization:"));
    REQUIRE(plan_contains(plan, "Mechanism: text"));
    REQUIRE(plan_contains(plan, "Mechanism: binary"));
    REQUIRE(plan_contains(plan, "Reason: client output"));
    REQUIRE_FALSE(plan_contains(plan, "Actual Calls:"));
    REQUIRE_FALSE(plan_contains(plan, "Estimated Symbols:"));
    REQUIRE_FALSE(plan_contains(plan, "Operation: slice"));
}

TEST_CASE("metadata-only projection emits no false allocation boundary",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    seed_small_bioseq_column(ctx, k_table);

    const auto plan =
        explain_with_options(ctx,
                             "ANALYZE, TIMING OFF, COSTS OFF",
                             fmt::format("SELECT bioseq.len(seq0[0:4]) FROM {};", k_table));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(plan_contains(plan, "Slice Materialization:"));
    REQUIRE(plan_contains(plan, "none"));
    REQUIRE_FALSE(plan_contains(plan, "Mechanism: packed full"));
    REQUIRE_FALSE(plan_contains(plan, "Mechanism: text"));
}

TEST_CASE("EXPLAIN SERIALIZE records only the observed client boundary",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    seed_small_bioseq_column(ctx, k_table);
    const auto sql = fmt::format("SELECT seq0[0:4] FROM {} WHERE pk = 1;", k_table);

    const auto none = explain_with_options(ctx, "ANALYZE, TIMING OFF, COSTS OFF", sql);
    REQUIRE(boundary_actual_calls(none, "text", "client output") == 0u);
    REQUIRE(boundary_actual_calls(none, "binary", "client output") == 0u);
    REQUIRE(boundary_actual_bytes(none, "text", "client output") == 0u);
    REQUIRE(boundary_actual_bytes(none, "binary", "client output") == 0u);
    REQUIRE_FALSE(plan_contains(none, "Actual Time:"));

    const auto text =
        explain_with_options(ctx, "ANALYZE, TIMING OFF, COSTS OFF, SERIALIZE TEXT", sql);
    REQUIRE(boundary_actual_calls(text, "text", "client output") == 1u);
    REQUIRE(boundary_actual_bytes(text, "text", "client output") == 5u);
    REQUIRE(boundary_actual_calls(text, "binary", "client output") == 0u);
    REQUIRE(boundary_actual_bytes(text, "binary", "client output") == 0u);

    const auto binary =
        explain_with_options(ctx, "ANALYZE, TIMING OFF, COSTS OFF, SERIALIZE BINARY", sql);
    REQUIRE(boundary_actual_calls(binary, "text", "client output") == 0u);
    REQUIRE(boundary_actual_bytes(binary, "text", "client output") == 0u);
    REQUIRE(boundary_actual_calls(binary, "binary", "client output") == 1u);
    REQUIRE(boundary_actual_bytes(binary, "binary", "client output") == 20u);
}

TEST_CASE("SQL materialization calls and bytes match allocations",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    seed_small_bioseq_column(ctx, k_table);

    const auto plan =
        explain_with_options(ctx,
                             "ANALYZE, TIMING OFF, COSTS OFF",
                             fmt::format("SELECT bioseq.to_text(seq0[0:4]) FROM {};", k_table));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(boundary_actual_calls(plan, "text", "SQL conversion") == 3u);
    REQUIRE(boundary_actual_metric(plan, "text", "SQL conversion", "Actual Input Symbols") == 12u);
    REQUIRE(boundary_actual_metric(plan, "text", "SQL conversion", "Actual Output Symbols") == 12u);
    /* Three 4-symbol text varlenas: VARHDRSZ + 4 bytes each. */
    REQUIRE(boundary_actual_bytes(plan, "text", "SQL conversion") == 24u);
    REQUIRE_FALSE(plan_contains(plan, "Mechanism: packed full"));
}

TEST_CASE("rescans retain every materialization counter delta",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    seed_small_bioseq_column(ctx, k_table);

    /* The correlated scalar SubPlan rescans its one-row SliceProject for each
     * VALUES row.  This is the executor shape used by a parallel worker under
     * a nested loop as well; counters must remain cumulative across rearms. */
    const auto plan =
        explain_with_options(ctx,
                             "ANALYZE, TIMING OFF, COSTS OFF",
                             fmt::format("SELECT (SELECT bioseq.to_text(source.seq0[0:4]) "
                                         "FROM {} AS source "
                                         "WHERE source.pk = driver.pk) "
                                         "FROM (VALUES (1), (2), (3)) AS driver(pk);",
                                         k_table));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(boundary_actual_calls(plan, "text", "SQL conversion") == 3u);
    REQUIRE(boundary_actual_metric(plan, "text", "SQL conversion", "Actual Input Symbols") == 12u);
    REQUIRE(boundary_actual_metric(plan, "text", "SQL conversion", "Actual Output Symbols") == 12u);
    /* Three 4-symbol text varlenas: VARHDRSZ + 4 bytes each. */
    REQUIRE(boundary_actual_bytes(plan, "text", "SQL conversion") == 24u);
}

TEST_CASE("structured EXPLAIN preserves numeric counter fields",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    seed_small_bioseq_column(ctx, k_table);

    const auto json = explain_json(ctx, fmt::format("SELECT seq0[0:4] FROM {};", k_table));
    REQUIRE(json.find("Slice Materialization") != std::string::npos);
    REQUIRE(json.find("\"Mechanism\": \"text\"") != std::string::npos);
    REQUIRE(json.find("\"Reason\": \"client output\"") != std::string::npos);
    REQUIRE(json.find("\"Actual Calls\": 0") != std::string::npos);
    REQUIRE(json.find("\"Actual Bytes\": 0") != std::string::npos);
    REQUIRE(json.find("\"Depends On\": []") != std::string::npos);
    REQUIRE(json.find("\"Counter Overflow\": false") != std::string::npos);

    const auto materializations = json_array_member(json, "Slice Materialization");
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

    const auto parsed = query_one(
        ctx,
        fmt::format("WITH document AS (SELECT {}::jsonb AS value), "
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

TEST_CASE("INSERT plan declares a heap-tuple boundary",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    auto ctx =
        fresh_context({k_table, "explain_flat_insert_target", "explain_slice_insert_target"});
    seed_small_bioseq_column(ctx, k_table);
    exec_sql(ctx,
             "CREATE TABLE explain_flat_insert_target "
             "(pk int, seq0 bioseq.seq('dna4',32)) USING bioseq;"
             "CREATE TABLE explain_slice_insert_target "
             "(pk int, seq0 bioseq.seq('dna4',32)) USING bioseq;");

    /* Control the ModifyTable/EXPLAIN contract independently of the lazy
     * wrapper.  A flat Var projection must execute and persist the same three
     * source rows before comparison with the sliced path. */
    const auto flat_plan =
        explain_with_options(ctx,
                             "ANALYZE, TIMING OFF, COSTS OFF",
                             fmt::format("INSERT INTO explain_flat_insert_target "
                                         "SELECT pk, seq0 FROM {};",
                                         k_table));
    REQUIRE_FALSE(plan_contains(flat_plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(query_int_column(ctx, "SELECT pk FROM explain_flat_insert_target ORDER BY pk;") ==
            std::vector<long long>{1, 2, 3});

#ifdef BIOSEQ_TEST_INDEXES
    exec_sql(ctx, "SELECT bioseq_internal.slice_payload_allocation_probe(0);");
#endif
    const auto plan = explain_with_options(ctx,
                                           "ANALYZE, TIMING OFF, COSTS OFF",
                                           fmt::format("INSERT INTO explain_slice_insert_target "
                                                       "SELECT pk, seq0[0:4] FROM {};",
                                                       k_table));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(plan_contains(plan, "Mechanism: packed full"));
    REQUIRE(plan_contains(plan, "Reason: heap tuple"));
    REQUIRE(boundary_actual_calls(plan, "packed full", "heap tuple") == 3u);
#ifdef BIOSEQ_TEST_INDEXES
    REQUIRE(query_scalar_int(ctx, "SELECT bioseq_internal.slice_payload_allocation_probe(1);") ==
            3);
#endif
    REQUIRE(query_int_column(ctx, "SELECT pk FROM explain_slice_insert_target ORDER BY pk;") ==
            std::vector<long long>{1, 2, 3});
    REQUIRE(query_int_column(ctx,
                             "SELECT bioseq.len(seq0) "
                             "FROM explain_slice_insert_target ORDER BY pk;") ==
            std::vector<long long>{4, 4, 4});
}

TEST_CASE("ordinary INSERT restores the page-cache GUC",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    constexpr std::string_view table = "page_cache_insert";
    auto ctx = fresh_context({table});
    exec_sql(ctx,
             "DROP PUBLICATION IF EXISTS page_cache_insert_publication;"
             "CREATE TABLE page_cache_insert "
             " (id int PRIMARY KEY, s bioseq.seq('dna4',32)) USING bioseq;"
             "SET bioseq.column_cache.enabled=on;");
    REQUIRE(query_scalar(ctx, "SHOW bioseq.column_cache.enabled;") == "on");

    exec_sql(ctx,
             "INSERT INTO page_cache_insert VALUES "
             " (1, repeat('ACGT',1024)::bioseq.seq('dna4',32));");
    REQUIRE(query_scalar(ctx, "SHOW bioseq.column_cache.enabled;") == "on");
    REQUIRE(query_int_column(ctx, "SELECT id FROM page_cache_insert ORDER BY id;") ==
            std::vector<long long>{1});

    exec_sql(ctx,
             "CREATE PUBLICATION page_cache_insert_publication "
             "FOR TABLE page_cache_insert;");
    const auto failure = sql_error(ctx,
                                   "INSERT INTO page_cache_insert VALUES "
                                   " (2, repeat('TGCA',1024)::bioseq.seq('dna4',32));");
    const auto cache_after_error = query_scalar(ctx, "SHOW bioseq.column_cache.enabled;");
    const auto rows_after_error =
        query_int_column(ctx, "SELECT id FROM page_cache_insert ORDER BY id;");
    exec_sql(ctx,
             "DROP PUBLICATION page_cache_insert_publication;"
             "INSERT INTO page_cache_insert VALUES "
             " (3, repeat('TGCA',1024)::bioseq.seq('dna4',32));");

    REQUIRE(failure.sqlstate == "0A000");
    REQUIRE(failure.contains("cannot insert into columnar table that is a part of a publication"));
    REQUIRE(cache_after_error == "on");
    REQUIRE(rows_after_error == std::vector<long long>{1});
    REQUIRE(query_scalar(ctx, "SHOW bioseq.column_cache.enabled;") == "on");
    REQUIRE(query_int_column(ctx, "SELECT id FROM page_cache_insert ORDER BY id;") ==
            std::vector<long long>{1, 3});
}

TEST_CASE("planned slice recipe is stable between plain and ANALYZE",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    seed_small_bioseq_column(ctx, k_table);
    const auto sql =
        fmt::format("SELECT seq0[0:8], (seq0[0:8])[0:4] FROM {} WHERE pk = 1;", k_table);

    const auto plain = explain_with_options(ctx, "VERBOSE, COSTS OFF", sql);
    const auto analyzed = explain_with_options(ctx, "ANALYZE, VERBOSE, TIMING OFF, COSTS OFF", sql);
    REQUIRE(planned_slice_signature(plain) == planned_slice_signature(analyzed));
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

TEST_CASE("LIMIT and skipped CASE arms retain zero boundary calls",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    seed_small_bioseq_column(ctx, k_table);

    const auto limited =
        explain_with_options(ctx,
                             "ANALYZE, TIMING OFF, COSTS OFF, SERIALIZE TEXT",
                             fmt::format("SELECT seq0[0:4] FROM {} LIMIT 0;", k_table));
    REQUIRE(boundary_actual_calls(limited, "text", "client output") == 0u);
    REQUIRE(boundary_actual_calls(limited, "binary", "client output") == 0u);

    const auto skipped = explain_with_options(
        ctx,
        "ANALYZE, TIMING OFF, COSTS OFF, SERIALIZE TEXT",
        fmt::format("SELECT CASE WHEN seq0 IS NULL THEN seq0[0:4] "
                    "ELSE 'ACGT'::bioseq.seq('dna4',32) END FROM {} WHERE pk = 1;",
                    k_table));
    REQUIRE(plan_contains(skipped, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(boundary_actual_calls(skipped, "text", "client output") == 0u);
    REQUIRE(boundary_actual_calls(skipped, "binary", "client output") == 0u);
}

TEST_CASE("lazy-only slice concat reverse has no allocation boundary",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    seed_small_bioseq_column(ctx, k_table);

    const auto plan =
        explain_with_options(ctx,
                             "ANALYZE, VERBOSE, TIMING OFF, COSTS OFF",
                             fmt::format("SELECT bioseq.len(bioseq.reverse(seq0[0:4]) "
                                         "OPERATOR(bioseq.||) seq0[4:8]) "
                                         "FROM {};",
                                         k_table));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(plan_contains(plan, "Slice Materialization:"));
    REQUIRE(plan_contains(plan, "none"));
    REQUIRE_FALSE(plan_contains(plan, "Mechanism: packed full"));
    REQUIRE_FALSE(plan_contains(plan, "Reason: executor copy"));
}

#ifdef BIOSEQ_TEST_INDEXES
TEST_CASE("unsliced EXPLAIN remains zero-payload around a sliced heap boundary",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    constexpr std::string_view materialized_table = "explain_slice_materialization_target";
    auto ctx = fresh_context({k_table, materialized_table});
    seed_small_bioseq_column(ctx, k_table);
    exec_sql(ctx,
             fmt::format("SET max_parallel_workers_per_gather=0; "
                         "SET bioseq.index.insert_maintenance.enabled=off; "
                         "TRUNCATE {}; "
                         "INSERT INTO {} VALUES "
                         "(1, repeat('ACGT',262144)::bioseq.seq('dna4',32)), "
                         "(2, repeat('TGCA',262144)::bioseq.seq('dna4',32)), "
                         "(3, repeat('AAAA',262144)::bioseq.seq('dna4',32)); "
                         "CREATE TABLE {} "
                         "(pk int, seq0 bioseq.seq('dna4',32)) USING bioseq;",
                         k_table,
                         k_table,
                         materialized_table));

    const auto reset_allocation_probe = [&ctx] {
        exec_sql(ctx, "SELECT bioseq_internal.slice_payload_allocation_probe(0);");
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
    const auto unsliced_sql = fmt::format("SELECT seq0 FROM {};", k_table);
    const auto run_unsliced_control =
        [&ctx, &reset_allocation_probe, &allocation_signature, &unsliced_sql] {
            reset_allocation_probe();
            const auto plan =
                explain_with_options(ctx, "ANALYZE, TIMING OFF, COSTS OFF", unsliced_sql);
            REQUIRE(plan_contains(plan, "Custom Scan (BioseqScan)"));
            REQUIRE_FALSE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
            REQUIRE_FALSE(plan_contains(plan, "Slice Materialization:"));
            REQUIRE(allocation_signature() == "0,0,0,0");
        };

    /* Bracket one explicit sliced heap boundary with the same unsliced plan.
     * len(slice/concat/reverse) is intentionally metadata-only and cannot
     * allocate payload, so it cannot be the materializing arm. INSERT gives the
     * sliced value a planned, executed packed-full boundary.  Assert that one
     * relevant allocation per row, without coupling this control to unrelated
     * executor-copy or SliceProject-publication counters. */
    run_unsliced_control();

    const auto expression = fmt::format("bioseq.reverse(seq0[0:4]) OPERATOR(bioseq.||) seq0[4:8]");
    reset_allocation_probe();
    const auto sliced_plan = explain_with_options(
        ctx,
        "ANALYZE, TIMING OFF, COSTS OFF",
        fmt::format(
            "INSERT INTO {} SELECT pk, {} FROM {};", materialized_table, expression, k_table));
    REQUIRE(plan_contains(sliced_plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(plan_contains(sliced_plan, "Slice Materialization:"));
    REQUIRE(plan_contains(sliced_plan, "Mechanism: packed full"));
    REQUIRE(plan_contains(sliced_plan, "Reason: heap tuple"));
    REQUIRE(boundary_actual_calls(sliced_plan, "packed full", "heap tuple") == 3u);
    REQUIRE(query_scalar_int(ctx, "SELECT bioseq_internal.slice_payload_allocation_probe(1);") ==
            3);
    REQUIRE(query_int_column(ctx,
                             fmt::format("SELECT bioseq.len(seq0) FROM {} ORDER BY pk;",
                                         materialized_table)) == std::vector<long long>{8, 8, 8});

    run_unsliced_control();
}

TEST_CASE("SliceProject shallow publication allocates no payload",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    seed_small_bioseq_column(ctx, k_table);
    /* Source-backed slice intermediates remain lazy through reverse
     * and concat.  Their global allocation totals are not a contract of this
     * case; measure only SliceProject's memcpy + RO publication seam with the
     * wrapper-local deltas. */
    exec_sql(ctx,
             fmt::format("SET max_parallel_workers_per_gather=0; "
                         "SET bioseq.index.insert_maintenance.enabled=off; "
                         "TRUNCATE {}; "
                         "INSERT INTO {} VALUES "
                         "(1, repeat('ACGT',262144)::bioseq.seq('dna4',32)), "
                         "(2, repeat('TGCA',262144)::bioseq.seq('dna4',32)), "
                         "(3, repeat('AAAA',262144)::bioseq.seq('dna4',32));",
                         k_table,
                         k_table));
    const auto expression = fmt::format("bioseq.reverse(seq0[0:4]) OPERATOR(bioseq.||) seq0[4:8]");
    const auto plan =
        explain_with_options(ctx,
                             "VERBOSE, COSTS OFF",
                             fmt::format("SELECT bioseq.len({}) FROM {};", expression, k_table));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));

    exec_sql(ctx, "SELECT bioseq_internal.slice_payload_allocation_probe(0);");
    REQUIRE(query_scalar(
                ctx,
                fmt::format("SELECT bool_and(bioseq.len({}) = 8) FROM {};", expression, k_table)) ==
            "t");
    const auto counters = query_one(ctx,
                                    "SELECT bioseq_internal.slice_payload_allocation_probe(3), "
                                    "       bioseq_internal.slice_payload_allocation_probe(4);");
    REQUIRE(counters.size() == 2u);
    REQUIRE(counters[0].value_or("") == "0");
    REQUIRE(counters[1].value_or("") == "0");
}
#endif

TEST_CASE("timing and summary options control only actual metadata",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    seed_small_bioseq_column(ctx, k_table);
    const auto sql = fmt::format("SELECT seq0[0:4] FROM {} WHERE pk = 1;", k_table);

    const auto with_timing =
        explain_with_options(ctx, "ANALYZE, TIMING ON, SUMMARY ON, COSTS OFF, SERIALIZE TEXT", sql);
    REQUIRE(plan_contains(with_timing, "Actual Time:"));
    REQUIRE(plan_contains(with_timing, "Execution Time:"));

    const auto without_timing = explain_with_options(
        ctx, "ANALYZE, TIMING OFF, SUMMARY OFF, COSTS OFF, SERIALIZE TEXT", sql);
    REQUIRE_FALSE(plan_contains(without_timing, "Actual Time:"));
    REQUIRE_FALSE(plan_contains(without_timing, "Execution Time:"));
}

TEST_CASE("YAML and XML preserve structured slice fields",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    seed_small_bioseq_column(ctx, k_table);
    const auto sql = fmt::format("SELECT seq0[0:4] FROM {} WHERE pk = 1;", k_table);

    const auto yaml = explain_with_options(ctx, "ANALYZE, TIMING OFF, COSTS OFF, FORMAT YAML", sql);
    REQUIRE(plan_contains(yaml, "BioseqSliceProject"));
    REQUIRE(plan_contains(yaml, "Slice Materialization"));
    REQUIRE(plan_contains(yaml, "Mechanism: \"text\""));
    REQUIRE(plan_contains(yaml, "Actual Calls: 0"));
    const auto yaml_document = plan_document(yaml);
    const auto yaml_materializations = yaml_property_block(yaml_document, "Slice Materialization");
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
    REQUIRE(yaml_materializations->find("var_1 =") == std::string_view::npos);

    const auto xml = explain_with_options(ctx, "ANALYZE, TIMING OFF, COSTS OFF, FORMAT XML", sql);
    REQUIRE(plan_contains(xml, "BioseqSliceProject"));
    REQUIRE(plan_contains(xml, "Slice-Materialization"));
    REQUIRE(plan_contains(xml, "<Mechanism>text</Mechanism>"));
    REQUIRE(plan_contains(xml, "<Actual-Calls>0</Actual-Calls>"));
    const auto xml_document = plan_document(xml);
    const auto xml_materializations = xml_element_block(xml_document, "Slice-Materialization");
    REQUIRE(xml_materializations.has_value());
    REQUIRE(count_occurrences(*xml_materializations, "<Materialization>") == 2u);
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
    REQUIRE(xml_materializations->find("var_1 =") == std::string_view::npos);
    const auto parsed_xml =
        query_one(ctx,
                  fmt::format("SELECT xml_is_well_formed_document({}::text), "
                              "       cardinality(xpath("
                              "         '//*[local-name()=\"Slice-Materialization\"]"
                              "/*[local-name()=\"Materialization\"]', {}::xml))::text;",
                              sql_quoted_literal(ctx.raw(), xml_document),
                              sql_quoted_literal(ctx.raw(), xml_document)));
    REQUIRE(parsed_xml.size() == 2u);
    REQUIRE(parsed_xml[0].value_or("") == "t");
    REQUIRE(parsed_xml[1].value_or("") == "2");
}
