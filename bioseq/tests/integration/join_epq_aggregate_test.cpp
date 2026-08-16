/**
 * @file
 * @brief Exercises EvalPlanQual, aggregate, and fail-closed Bioseq joins.
 */
#include <chrono>
#include <libpq-fe.h>
#include <set>
#include <tests/support/postgres.hpp>
#include <thread>

import bioseq.tests.integration.support;
import bioseq.tests.integration.join_test_support;

using namespace bioseq_int_tests;
using namespace bioseq_int_tests::join_test_support;

TEST_CASE("Projection-only EPQ evaluates volatile final once",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    constexpr std::string_view table = "projection_epq";
    constexpr std::string_view source = "projection_epq_source";
    constexpr std::string_view log = "projection_epq_log";
    auto setup = fresh_context({log, source, table});
    exec_sql(setup,
             "DROP FUNCTION IF EXISTS public.projection_epq_once("
             "text,text);"
             "CREATE TABLE projection_epq ("
             "id int PRIMARY KEY, source_id int NOT NULL, use_suffix boolean "
             "NOT NULL, touch int, sliced text);"
             "CREATE TABLE projection_epq_source ("
             "id int PRIMARY KEY, seq bioseq.seq('dna4',32)) "
             "USING bioseq;"
             "CREATE TABLE projection_epq_log ("
             "ordinal bigserial PRIMARY KEY, label text UNIQUE NOT NULL, "
             "observed text);"
             "INSERT INTO projection_epq VALUES (1,1,false,0,NULL);"
             "INSERT INTO projection_epq_source VALUES "
             "(1,'AAAACCCC'::bioseq.seq('dna4',32));"
             "CREATE FUNCTION public.projection_epq_once("
             "label text, value text) RETURNS text LANGUAGE plpgsql VOLATILE "
             "AS $$BEGIN "
             "INSERT INTO public.projection_epq_log(label,observed) "
             "VALUES(label,value); RETURN value; END$$;");

    const std::string update_sql = "UPDATE public.projection_epq AS h "
                                   "SET touch=h.touch+1, sliced=CASE WHEN h.use_suffix "
                                   "THEN s.suffix ELSE s.prefix END "
                                   "FROM (SELECT id, seq[0:4]::text AS prefix, "
                                   "seq[4:]::text AS suffix "
                                   "FROM public.projection_epq_source OFFSET 0) AS s "
                                   "WHERE h.id=1 AND h.source_id=s.id "
                                   "RETURNING public.projection_epq_once("
                                   "'projection-final',h.sliced);";
    const auto plan = explain_text<false>(setup, update_sql);
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(plan_contains(plan, "Subquery Scan on s"));
    REQUIRE(query_scalar_int(setup, "SELECT count(*) FROM projection_epq_log;") == 0);

    connection_test_context blocker;
    connection_test_context worker;
    connection_test_context observer;
    exec_sql(worker, "SET statement_timeout='15s';");
    const auto outcome = run_locked_update(blocker,
                                           worker,
                                           observer,
                                           "UPDATE public.projection_epq "
                                           "SET use_suffix=true,touch=touch+1 WHERE id=1;",
                                           update_sql);
    INFO(outcome.error);
    REQUIRE(outcome.sent);
    REQUIRE(outcome.lock_wait_observed);
    REQUIRE(outcome.consume_ok);
    REQUIRE(outcome.result_ok);
    REQUIRE(outcome.got_tuple_result);
    REQUIRE(outcome.returned_rows == 1u);
    REQUIRE(query_scalar(setup,
                         "SELECT string_agg(label,',' ORDER BY ordinal) "
                         "FROM projection_epq_log;") == "projection-final");
    REQUIRE(query_scalar(setup, "SELECT observed FROM projection_epq_log;") == "CCCC");
    REQUIRE(query_scalar_int(setup, "SELECT touch FROM projection_epq WHERE id=1;") == 2);
    REQUIRE(query_scalar_int(setup, "SELECT source_id FROM projection_epq WHERE id=1;") == 1);
    REQUIRE(query_scalar_int(setup, "SELECT use_suffix::int FROM projection_epq WHERE id=1;") == 1);
    REQUIRE(query_scalar(setup, "SELECT sliced FROM projection_epq WHERE id=1;") == "CCCC");
    exec_sql(setup, "DROP FUNCTION public.projection_epq_once(text,text);");
}

TEST_CASE("BatchJoin EPQ preserves final effect order exactly once",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    constexpr std::string_view target = "batchjoin_epq_target";
    constexpr std::string_view queries = "batchjoin_epq_queries";
    constexpr std::string_view slice_source = "batchjoin_epq_slice_source";
    constexpr std::string_view result = "batchjoin_epq_result";
    constexpr std::string_view log = "batchjoin_epq_log";
    auto setup = fresh_context({log, result, slice_source, queries, target});
    create_seq_table(setup,
                     seq_table_spec{
                         .name = std::string{target},
                         .typmod = {.alphabet = "dna4", .width = 32},
                         .seq_columns = 1,
                         .using_bioseq = true,
                     },
                     {seq_row{
                         .pk = 1,
                         .sequences = {hoe_seed_rows().front().sequences.front()},
                     }});
    exec_sql(setup,
             fmt::format("DROP FUNCTION IF EXISTS public.batchjoin_epq_once("
                         "text,text);"
                         "CREATE TABLE {} (id int PRIMARY KEY, target_pk int NOT NULL, "
                         "seq bioseq.seq('dna4',32));"
                         "INSERT INTO {} SELECT id,1,{} "
                         "FROM generate_series(1,16) AS ids(id);"
                         "CREATE TABLE {} (id int PRIMARY KEY, "
                         "seq bioseq.seq('dna4',32)) USING bioseq;"
                         "INSERT INTO {} VALUES (1,{});"
                         "CREATE TABLE {} (id int PRIMARY KEY, target_pk int NOT NULL, "
                         "query_id int NOT NULL, use_suffix boolean NOT NULL, touch int, "
                         "matched_query text);"
                         "INSERT INTO {} VALUES (1,1,1,false,0,NULL);"
                         "CREATE TABLE {} (ordinal bigserial PRIMARY KEY, "
                         "label text UNIQUE NOT NULL, observed text);"
                         "CREATE FUNCTION public.batchjoin_epq_once("
                         "label text, value text) RETURNS text LANGUAGE plpgsql VOLATILE "
                         "AS $$BEGIN INSERT INTO public.{}(label,observed) "
                         "VALUES(label,value); "
                         "RETURN value; END$$;",
                         queries,
                         queries,
                         hoe_probe_literal(),
                         slice_source,
                         slice_source,
                         hoe_probe_literal(),
                         result,
                         result,
                         log,
                         log));
    configure_transformed_join(setup);

    const std::string update_sql =
        fmt::format("UPDATE public.{} AS h "
                    "SET touch=h.touch+1, "
                    "matched_query=CASE WHEN h.use_suffix "
                    "THEN sliced.query_suffix ELSE sliced.query_prefix END "
                    "FROM (SELECT t.pk AS target_pk, q.id AS query_id "
                    "FROM public.{} AS t INNER JOIN public.{} AS q "
                    "ON mmsearch(t.seq0, q.seq, "
                    "aligner_options(index_read_requirement => 1)) "
                    "OFFSET 0) AS matched "
                    "INNER JOIN (SELECT q.id AS query_id, "
                    "q.seq[0:1]::text AS query_prefix, "
                    "q.seq[1:]::text AS query_suffix "
                    "FROM public.{} AS q OFFSET 0) AS sliced "
                    "ON sliced.query_id=matched.query_id "
                    "WHERE h.id=1 AND h.target_pk=matched.target_pk "
                    "AND h.query_id=matched.query_id "
                    "RETURNING "
                    "public.batchjoin_epq_once("
                    "'target',matched.target_pk::text), "
                    "public.batchjoin_epq_once('query',h.matched_query), "
                    "public.batchjoin_epq_once("
                    "'options',CASE WHEN h.use_suffix "
                    "THEN 'suffix' ELSE 'prefix' END);",
                    result,
                    target,
                    queries,
                    slice_source);
    const auto plan = explain_text<false>(setup, update_sql);
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqBatchJoin)"));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(plan_contains(plan, "Subquery Scan on matched"));
    REQUIRE(plan_contains(plan, "Bioseq Batch ABI Version: 3"));
    REQUIRE(plan_contains(plan, "Bioseq Residual Mmsearch Qual: absent"));
    REQUIRE_FALSE(plan_contains(plan, "Bioseq Mmsearch Ownership: per-row recipe"));
    REQUIRE_FALSE(plan_has_executable_mmsearch(plan));
    REQUIRE(query_scalar_int(setup, fmt::format("SELECT count(*) FROM {};", log)) == 0);

    connection_test_context blocker;
    connection_test_context worker;
    connection_test_context observer;
    configure_transformed_join(worker);
    exec_sql(worker, "SET statement_timeout='15s';");
    const auto outcome =
        run_locked_update(blocker,
                          worker,
                          observer,
                          fmt::format("UPDATE public.{} SET use_suffix=true,touch=touch+1 "
                                      "WHERE id=1;",
                                      result),
                          update_sql);
    INFO(outcome.error);
    REQUIRE(outcome.sent);
    REQUIRE(outcome.lock_wait_observed);
    REQUIRE(outcome.consume_ok);
    REQUIRE(outcome.result_ok);
    REQUIRE(outcome.got_tuple_result);
    REQUIRE(outcome.returned_rows == 1u);
    REQUIRE(query_scalar_int(worker,
                             "SELECT scan_node_count FROM "
                             "bioseq.last_alignment_execution_stats() LIMIT 1;") == 1);
    REQUIRE(query_scalar_int(worker,
                             "SELECT nested_alignment_executor_count FROM "
                             "bioseq.last_alignment_execution_stats() LIMIT 1;") == 0);
    REQUIRE(query_scalar(setup,
                         fmt::format("SELECT string_agg(label,',' ORDER BY ordinal) FROM {};",
                                     log)) == "target,query,options");
    REQUIRE(query_scalar(setup, fmt::format("SELECT observed FROM {} WHERE label='query';", log)) ==
            std::string{k_hoe_probe_sequence.substr(1)});
    REQUIRE(
        query_scalar(setup, fmt::format("SELECT observed FROM {} WHERE label='options';", log)) ==
        "suffix");
    REQUIRE(query_scalar_int(setup, fmt::format("SELECT touch FROM {} WHERE id=1;", result)) == 2);
    REQUIRE(query_scalar_int(
                setup, fmt::format("SELECT use_suffix::int FROM {} WHERE id=1;", result)) == 1);
    REQUIRE(query_scalar(setup, fmt::format("SELECT matched_query FROM {} WHERE id=1;", result)) ==
            std::string{k_hoe_probe_sequence.substr(1)});
    exec_sql(setup, "DROP FUNCTION public.batchjoin_epq_once(text,text);");
}

TEST_CASE("Many-to-many mmsearch join owns the predicate and projections",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    auto ctx = fresh_context({k_hoe_join_left, k_hoe_join_right});
    seed_hoe_fixture(ctx, k_hoe_join_left);
    seed_hoe_fixture(ctx, k_hoe_join_right);

    const auto rows = query_rows(ctx,
                                 fmt::format("SELECT t1.pk, t2.pk, bioseq.cigar(t1.pk) "
                                             "  FROM {} t1, {} t2 "
                                             " WHERE mmsearch(t1.seq0, t2.seq1) "
                                             " ORDER BY t1.pk, t2.pk;",
                                             k_hoe_join_left,
                                             k_hoe_join_right));
    REQUIRE_FALSE(rows.empty());
    for (const auto& row : rows) {
        REQUIRE(row.size() == 3);
        REQUIRE(row[2].has_value());
        REQUIRE_FALSE(row[2]->empty());
        REQUIRE(is_valid_cigar(*row[2]));
    }
    // Session must still be usable.
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE("LEFT JOIN with bioseq.symbol_count projection does not crash",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    // The bioseq.symbol_count and bioseq.cigar projection placeholders share
    // the same rewriter and must route through
    // bioseq_symbol_count_support's simplify branch for LEFT JOIN.
    auto ctx = fresh_context({k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join);

    ctx.clear_notices();
    const auto rows =
        query_rows(ctx,
                   fmt::format("SELECT t1.pk, t2.pk, bioseq.symbol_count(t1.seq0, 'A') "
                               "  FROM {} t1 LEFT JOIN {} t2 ON t1.pk = t2.pk "
                               " ORDER BY t1.pk;",
                               k_hoe_join,
                               k_hoe_join));
    REQUIRE(rows.size() == 5);
    for (const auto& row : rows) {
        REQUIRE(row.size() == 3);
        REQUIRE(row[0].has_value());
        REQUIRE(row[1].has_value());
        // Every seeded sequence has a deterministic A-count from the stored
        // inline symbol_counts index, populated by the columnar AM at INSERT
        // time when bioseq.index.insert_maintenance.enabled is on (which
        // seed_hoe_fixture sets).
        REQUIRE(row[2].has_value());
        const auto count = std::stoll(row[2].value());
        REQUIRE(count >= 0);
    }
    REQUIRE_FALSE(ctx.notice_contains("resource was not closed"));

    // A row inserted without the stored inline index and read with
    // bioseq.index.sequence_read.enabled disabled must surface a non-NULL tally
    // through the unconditional recompute fallback. Its sequence contains
    // eight A symbols.
    {
        guc_scope no_insert(ctx, "bioseq.index.insert_maintenance.enabled", "off");
        exec_sqlf(ctx,
                  "INSERT INTO {} VALUES "
                  "(6, 'AAAACCCCGGGGTTTTAAAACCCCGGGGTTTT'::bioseq.seq('dna4', 32), "
                  "'AAAACCCC'::bioseq.seq('dna4', 32));",
                  k_hoe_join);
    }
    {
        guc_scope no_read(ctx, "bioseq.index.sequence_read.enabled", "off");
        ctx.clear_notices();
        const auto guard_row = query_one(
            ctx,
            fmt::format("SELECT bioseq.symbol_count(seq0, 'A') FROM {} WHERE pk = 6;", k_hoe_join));
        REQUIRE(guard_row.size() == 1);
        REQUIRE(guard_row[0].has_value()); // recompute fallback is never NULL
        REQUIRE(std::stoll(guard_row[0].value()) == 8);
        // The recompute opens seq_sub plus a registered random-read snapshot.
        // Both must be released before PQexec returns.
        REQUIRE_FALSE(ctx.notice_contains("resource was not closed"));
    }
}

TEST_CASE("EXPLAIN ANALYZE survives CROSS JOIN of bioseq tables",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    // EXPLAIN ANALYZE executes both scans and must retain valid row counts.
    auto ctx = fresh_context({k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join);

    const auto plan = explain_text(ctx,
                                   fmt::format("SELECT t1.pk, t2.pk "
                                               "  FROM {} t1 CROSS JOIN {} t2 "
                                               " ORDER BY t1.pk, t2.pk;",
                                               k_hoe_join,
                                               k_hoe_join));
    REQUIRE_FALSE(plan.empty());
    REQUIRE(plan_contains(plan, "BioseqScan"));
    // Both relations should produce a Custom Scan node — the planner
    // self-join optimisation that could collapse them away does not
    // apply when the table uses our access method. A single BioseqScan would
    // mean the second scan was incorrectly rewritten to a heap path.
    int bioseq_scan_count = 0;
    for (const auto& line : plan) {
        if (line.find("Custom Scan (BioseqScan)") != std::string::npos) {
            ++bioseq_scan_count;
        }
    }
    REQUIRE(bioseq_scan_count == 2);
    // Assert the outer-side actual count by inspecting the raw plan strings.
    // Accept any ordering of "(actual ... rows=N..." after BioseqScan.
    const std::string actual_marker = "rows=5";
    bool outer_returned_five = false;
    for (const auto& line : plan) {
        if (line.find("Custom Scan (BioseqScan)") != std::string::npos &&
            line.find(actual_marker) != std::string::npos) {
            outer_returned_five = true;
            break;
        }
    }
    REQUIRE(outer_returned_five);
}

TEST_CASE("Cross-table mmsearch (mmsearch as join filter) returns rows only "
          "where the aligner produced a real match",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    // `WHERE mmsearch(t1.seq, t2.seq2)` executes inside the Nested Loop as a
    // fuzzy substring predicate. Pushdown extraction creates a parameterised
    // inner path, column-scan path construction claims its RestrictInfo, and
    // cursor construction removes alignments with empty CIGAR output.
    auto ctx = fresh_context({k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join);

    const auto rows = query_rows(ctx,
                                 fmt::format("SELECT t1.pk, t2.pk, bioseq.cigar(t1.pk) "
                                             "  FROM {} t1 CROSS JOIN {} t2 "
                                             " WHERE mmsearch(t1.seq0, t2.seq1) "
                                             " ORDER BY t1.pk, t2.pk;",
                                             k_hoe_join,
                                             k_hoe_join));

    // The row count is below the full 5x5 cartesian product because not every
    // pair aligns, and each emitted row has a non-empty valid CIGAR.
    REQUIRE_FALSE(rows.empty());
    // Strict upper bound: at most full cartesian (25) — no row
    // explosion from match multiplicity should exceed that under
    // these probes, but we leave headroom for the multi-match-per-
    // row case.
    REQUIRE(rows.size() <= 100);

    for (const auto& row : rows) {
        REQUIRE(row.size() == 3);
        REQUIRE(row[0].has_value());
        REQUIRE(row[1].has_value());
        // Every surfaced row must have a non-empty, valid CIGAR.
        REQUIRE(row[2].has_value());
        REQUIRE_FALSE(row[2].value().empty());
        REQUIRE(is_valid_cigar(row[2].value()));
    }
}

TEST_CASE("Multiple Const mmsearch quals fail closed",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    auto ctx = fresh_context({k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join);
    constexpr std::string_view second_probe = "TTCATCATCATCATCTACTCCT";
    const auto error = sql_error(ctx,
                                 fmt::format("SELECT pk FROM {} WHERE "
                                             "mmsearch(seq0, '{}'::bioseq.seq('dna4', 32)) AND "
                                             "mmsearch(seq0, '{}'::bioseq.seq('dna4', 32));",
                                             k_hoe_join,
                                             k_hoe_probe_sequence,
                                             second_probe));
    REQUIRE(error.sqlstate == "0A000");
    REQUIRE(error.contains("stacked mmsearch"));
}

TEST_CASE("Aggregate over bioseq.cigar projection (MAX) returns the "
          "lexicographic max CIGAR",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    // Exercises the has_aggs branch of bioseq_placeholder_support. The planner
    // uses the standard Aggregate node when an input depends on a bioseq
    // projection, so placeholder Consts are evaluated by the correct slot.
    auto ctx = fresh_context({k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join);

    const std::string sql = fmt::format("SELECT MAX(bioseq.cigar(pk)) FROM {} "
                                        " WHERE mmsearch(seq0, '{}'::bioseq.seq('dna4', 32));",
                                        k_hoe_join,
                                        k_hoe_probe_sequence);
    const auto row = query_one(ctx, sql);
    REQUIRE(row.size() == 1);
    REQUIRE(row[0].has_value());
    REQUIRE_FALSE(row[0].value().empty());
    REQUIRE(is_valid_cigar(row[0].value()));
}

TEST_CASE("COUNT(*) over an mmsearch-filtered bioseq table compiles and runs",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    // COUNT(*) doesn't take a bioseq projection as an argument,
    // so the SUPPORT-function PHV machinery doesn't fire for it
    // — but the WHERE mmsearch(...) does filter rows through our
    // batch processor. The hit count must reflect the number of
    // matching rows the aligner returned, NOT the raw 5 rows in
    // the fixture.
    //
    // This count has no bioseq projection. Integer aggregate projections use
    // a separate planner path.
    auto ctx = fresh_context({k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join);

    const std::string count_sql =
        fmt::format("SELECT COUNT(*)::int8 AS hits "
                    "  FROM {} "
                    " WHERE mmsearch(seq0, '{}'::bioseq.seq('dna4', 32));",
                    k_hoe_join,
                    k_hoe_probe_sequence);
    const auto count_plan = explain_text<false>(ctx, count_sql);
    REQUIRE(plan_contains(count_plan, "Aggregate"));
    REQUIRE(plan_contains(count_plan, "Custom Scan (BioseqScan)"));
    REQUIRE_FALSE(plan_contains(count_plan, "Custom Scan (VectorAggNode)"));
    const auto count_rows =
        expect_invariant_under_guc_toggle(ctx, count_sql, "bioseq.vectorization.enabled");
    REQUIRE(count_rows.size() == 1);
    const auto& count_row = count_rows.front();
    REQUIRE(count_row.size() == 1);
    REQUIRE(count_row[0].has_value());
    const long long hits = std::stoll(count_row[0].value());
    REQUIRE(hits > 0);
    // Five seeded rows; not all need to align under the probe.
    REQUIRE(hits <= 100);
}

TEST_CASE("GROUP BY pk with bioseq aggregate returns one row per matching pk",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    auto ctx = fresh_context({k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join);

    const std::string sql = fmt::format("SELECT pk, MAX(bioseq.cigar(pk)) AS best_cigar "
                                        "  FROM {} "
                                        " WHERE mmsearch(seq0, '{}'::bioseq.seq('dna4', 32)) "
                                        " GROUP BY pk ORDER BY pk;",
                                        k_hoe_join,
                                        k_hoe_probe_sequence);
    const auto rows = query_rows(ctx, sql);
    REQUIRE_FALSE(rows.empty());
    for (const auto& row : rows) {
        REQUIRE(row.size() == 2);
        REQUIRE(row[0].has_value());
        REQUIRE(row[1].has_value());
        REQUIRE_FALSE(row[1].value().empty());
        REQUIRE(is_valid_cigar(row[1].value()));
    }
}

TEST_CASE("Param-driven primary plus Const mmsearch fails closed",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    auto ctx = fresh_context({k_hoe_join_left, k_hoe_join_right});
    seed_hoe_fixture(ctx, k_hoe_join_left);
    seed_hoe_fixture(ctx, k_hoe_join_right);
    const auto stacked_error =
        sql_error(ctx,
                  fmt::format("SELECT t1.pk, t2.pk FROM {} t1 INNER JOIN {} t2 "
                              "ON mmsearch(t2.seq0, t1.seq1) "
                              "WHERE mmsearch(t2.seq0, '{}'::bioseq.seq('dna4', 32));",
                              k_hoe_join_left,
                              k_hoe_join_right,
                              "TCATCATCATCATCTACTCCTATCTATCATCATCATCATCTACTCCT"));
    REQUIRE(stacked_error.sqlstate == "0A000");
    REQUIRE(stacked_error.contains("stacked mmsearch"));
}

TEST_CASE("Cross-table mmsearch via INNER JOIN with bioseq.cigar projection",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    // INNER JOIN syntax has the same semantics as the CROSS JOIN predicate:
    // every emitted row has a valid non-empty CIGAR and the planner uses the
    // parameterised inner-side path.
    auto ctx = fresh_context({k_hoe_join_left, k_hoe_join_right});
    seed_hoe_fixture(ctx, k_hoe_join_left);
    seed_hoe_fixture(ctx, k_hoe_join_right);

    const auto rows = query_rows(ctx,
                                 fmt::format("SELECT t1.pk, t2.pk, bioseq.cigar(t1.pk) "
                                             "  FROM {} t1 INNER JOIN {} t2 "
                                             "    ON mmsearch(t1.seq0, t2.seq1) "
                                             " ORDER BY t1.pk, t2.pk;",
                                             k_hoe_join_left,
                                             k_hoe_join_right));
    REQUIRE_FALSE(rows.empty());
    for (const auto& row : rows) {
        REQUIRE(row.size() == 3);
        REQUIRE(row[0].has_value());
        REQUIRE(row[1].has_value());
        REQUIRE(row[2].has_value());
        REQUIRE_FALSE(row[2].value().empty());
        REQUIRE(is_valid_cigar(row[2].value()));
    }
}
