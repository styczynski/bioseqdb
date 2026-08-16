/**
 * @file
 * @brief Exercises transformed-owner and parameterized Bioseq joins.
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

TEST_CASE("PARAM_EXEC rescan clears NULL and restores the next primary query",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    /* OFFSET 0 keeps the LATERAL child as a parameterized Nested Loop inner
     * scan, so p.probe reaches BioseqScan as PARAM_EXEC.  VALUES supplies the
     * load-bearing non-NULL -> NULL -> non-NULL rescan sequence in one
     * executor: the NULL iteration must not replay stale matches, and the
     * third iteration must reconfigure the same scan state successfully. */
    auto ctx = fresh_context({k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join);

    const std::string sql = fmt::format("SELECT p.ord, sub.pk "
                                        "FROM (VALUES (1, {}), "
                                        "             (2, NULL::bioseq.seq('dna4', 32)), "
                                        "             (3, {})) AS p(ord, probe) "
                                        "LEFT JOIN LATERAL ("
                                        "  SELECT t.pk FROM {} t "
                                        "  WHERE mmsearch(t.seq0, p.probe) "
                                        "  ORDER BY t.pk OFFSET 0"
                                        ") sub ON true "
                                        "ORDER BY p.ord, sub.pk",
                                        hoe_probe_literal(),
                                        hoe_probe_literal(),
                                        k_hoe_join);
    const auto plan = explain_text<false>(ctx, sql);
    REQUIRE(plan_contains(plan, "Nested Loop"));
    REQUIRE(plan_contains(plan, "Values Scan"));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqScan)"));

    const auto rows = query_rows(ctx, sql + ";");
    std::vector<std::string> first_matches;
    std::vector<std::string> restored_matches;
    std::size_t null_outer_rows = 0;
    for (const auto& row : rows) {
        REQUIRE(row.size() == 2);
        REQUIRE(row[0].has_value());
        const long long ordinal = std::stoll(*row[0]);
        if (ordinal == 1) {
            REQUIRE(row[1].has_value());
            first_matches.push_back(*row[1]);
        } else if (ordinal == 2) {
            REQUIRE_FALSE(row[1].has_value());
            ++null_outer_rows;
        } else if (ordinal == 3) {
            REQUIRE(row[1].has_value());
            restored_matches.push_back(*row[1]);
        } else {
            FAIL("unexpected PARAM_EXEC probe ordinal");
        }
    }
    REQUIRE_FALSE(first_matches.empty());
    REQUIRE(null_outer_rows == 1);
    REQUIRE(restored_matches == first_matches);
}

TEST_CASE("Transformed LATERAL owner survives PARAM_EXEC rescans",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    auto ctx = fresh_context({k_transformed_join_target});
    seed_transformed_join_fixture(ctx);

    constexpr std::array<std::string_view, 2u> option_suffixes{"",
                                                               ", NULL::bioseq_aligner_options"};
    for (const auto option_suffix : option_suffixes) {
        DYNAMIC_SECTION((option_suffix.empty() ? "two-argument mmsearch"
                                               : "three-argument mmsearch with default options")) {
            /* OFFSET 0 is the pull-up fence.  q.id=d.ord makes the child join
             * genuinely parameterized; the same BioseqBatchJoin is rescanned
             * over present -> NULL -> present sequence payloads. */
            const std::string sql =
                fmt::format("SELECT d.ord, matched.pk "
                            "FROM (VALUES (1), (2), (3)) AS d(ord) "
                            "LEFT JOIN LATERAL ("
                            "  SELECT t.pk FROM {} AS t INNER JOIN {} AS q "
                            "    ON t.pk=q.target_pk AND t.pk>0 AND q.target_pk>0 "
                            "   AND mmsearch(t.seq0[0:], q.seq[0:]{} ) "
                            "  WHERE q.id=d.ord "
                            "  ORDER BY t.pk LIMIT 1 OFFSET 0"
                            ") AS matched ON true "
                            "ORDER BY d.ord, matched.pk;",
                            k_transformed_join_target,
                            k_transformed_join_queries,
                            option_suffix);
            const auto plan = explain_text<false>(ctx, sql);
            REQUIRE(plan_contains(plan, "Nested Loop"));
            REQUIRE(plan_contains(plan, "Custom Scan (BioseqBatchJoin)"));
            REQUIRE(plan_contains(plan, "Bioseq Mmsearch Ownership: per-row recipe"));
            REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
            require_owned_qual_audit(plan);

            const auto rows = query_rows(ctx, sql);
            REQUIRE(rows.size() == 3u);
            REQUIRE((rows[0] ==
                     row_t{std::optional<std::string>{"1"}, std::optional<std::string>{"1"}}));
            REQUIRE((rows[1] == row_t{std::optional<std::string>{"2"}, std::nullopt}));
            REQUIRE((rows[2] ==
                     row_t{std::optional<std::string>{"3"}, std::optional<std::string>{"1"}}));
        }
    }
}

TEST_CASE("Transformed owner remains inside CTE and subquery fences",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    auto ctx = fresh_context({k_transformed_join_target});
    seed_transformed_join_fixture(ctx);

    constexpr std::array<std::string_view, 2u> option_suffixes{"",
                                                               ", NULL::bioseq_aligner_options"};
    for (const auto option_suffix : option_suffixes) {
        const std::string owned_body = fmt::format("SELECT t.pk, q.id, bioseq.cigar(t.pk) AS cigar "
                                                   "FROM {} AS t INNER JOIN {} AS q "
                                                   "ON q.id=1 AND t.pk=q.target_pk "
                                                   "AND mmsearch(t.seq0[0:], q.seq[0:]{} )",
                                                   k_transformed_join_target,
                                                   k_transformed_join_queries,
                                                   option_suffix);
        const std::string flat_sql = owned_body + " ORDER BY 1, 2, 3;";
        const std::string cte_sql = fmt::format("WITH owned AS MATERIALIZED ({}) "
                                                "SELECT pk, id, cigar FROM owned ORDER BY 1, 2, 3;",
                                                owned_body);
        const std::string subquery_sql =
            fmt::format("SELECT pk, id, cigar FROM ({} OFFSET 0) AS owned "
                        "ORDER BY 1, 2, 3;",
                        owned_body);

        const auto expected = query_rows(ctx, flat_sql);
        REQUIRE_FALSE(expected.empty());
        for (const auto& sql : {cte_sql, subquery_sql}) {
            const auto plan = explain_text<false>(ctx, sql);
            REQUIRE(plan_contains(plan, "Custom Scan (BioseqBatchJoin)"));
            REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
            require_owned_qual_audit(plan);
            REQUIRE(query_rows(ctx, sql) == expected);
        }
        const auto cte_plan = explain_text<false>(ctx, cte_sql);
        REQUIRE(plan_contains(cte_plan, "CTE Scan"));
    }
}

TEST_CASE("Partition child reparameterization retains one transformed owner",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    auto ctx = fresh_context({k_transformed_join_target});
    seed_hoe_fixture(ctx, k_transformed_join_target);
    exec_sql(ctx,
             fmt::format("CREATE TEMP TABLE transformed_partition_queries ("
                         "id int, bucket int, target_pk int NOT NULL, "
                         "seq bioseq.seq('dna4',32)) PARTITION BY RANGE(bucket);"
                         "CREATE TEMP TABLE transformed_partition_queries_lo "
                         "PARTITION OF transformed_partition_queries "
                         "FOR VALUES FROM (0) TO (10);"
                         "CREATE TEMP TABLE transformed_partition_queries_hi "
                         "PARTITION OF transformed_partition_queries "
                         "FOR VALUES FROM (10) TO (20);"
                         "INSERT INTO transformed_partition_queries VALUES "
                         "(1, 5, 1, {}), "
                         "(2, 15, 1, NULL::bioseq.seq('dna4',32)), "
                         "(3, 5, 1, {});",
                         hoe_probe_literal(),
                         hoe_probe_literal()));
    configure_transformed_join(ctx);
    exec_sql(ctx,
             "SET enable_partition_pruning = off;"
             "SET enable_hashjoin = off;"
             "SET enable_mergejoin = off;"
             "SET enable_nestloop = on;");

    const std::string sql = fmt::format("SELECT d.ord, matched.pk "
                                        "FROM (VALUES (1,5), (2,15), (3,5)) AS d(ord,bucket) "
                                        "LEFT JOIN LATERAL ("
                                        "  SELECT t.pk FROM {} AS t "
                                        "  INNER JOIN transformed_partition_queries AS q "
                                        "    ON t.pk=q.target_pk "
                                        "   AND mmsearch(t.seq0[0:], q.seq[0:]) "
                                        "  WHERE q.id=d.ord AND q.bucket=d.bucket "
                                        "  ORDER BY t.pk LIMIT 1 OFFSET 0"
                                        ") AS matched ON true ORDER BY d.ord;",
                                        k_transformed_join_target);
    const auto plan = explain_text<false>(ctx, sql);
    REQUIRE(plan_contains(plan, "Append"));
    REQUIRE(plan_contains(plan, "transformed_partition_queries_lo"));
    REQUIRE(plan_contains(plan, "transformed_partition_queries_hi"));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqBatchJoin)"));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
    require_owned_qual_audit(plan);

    const auto rows = query_rows(ctx, sql);
    REQUIRE(rows.size() == 3u);
    REQUIRE(rows[0][0] == "1");
    REQUIRE(rows[0][1] == "1");
    REQUIRE(rows[1][0] == "2");
    REQUIRE_FALSE(rows[1][1].has_value());
    REQUIRE(rows[2][0] == "3");
    REQUIRE(rows[2][1] == "1");
}

TEST_CASE("Dominating SliceProject spans Append branches across rescans",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    auto ctx = fresh_context({k_transformed_join_target});
    seed_hoe_fixture(ctx, k_transformed_join_target);
    exec_sql(ctx,
             fmt::format("CREATE TEMP TABLE transformed_owner_append_queries ("
                         "id int, driver int, bucket int, target_pk int NOT NULL, "
                         "seq bioseq.seq('dna4',32)) PARTITION BY RANGE(bucket);"
                         "CREATE TEMP TABLE transformed_owner_append_queries_lo "
                         "PARTITION OF transformed_owner_append_queries "
                         "FOR VALUES FROM (0) TO (10);"
                         "CREATE TEMP TABLE transformed_owner_append_queries_hi "
                         "PARTITION OF transformed_owner_append_queries "
                         "FOR VALUES FROM (10) TO (20);"
                         "INSERT INTO transformed_owner_append_queries "
                         "SELECT d.driver * 100 + branch.row_delta, d.driver, "
                         "       branch.bucket, 1, {} "
                         "FROM (VALUES (1), (2), (3)) AS d(driver) "
                         "CROSS JOIN (VALUES (1,5), (2,5), (3,15), (4,15)) "
                         "AS branch(row_delta,bucket);",
                         hoe_probe_literal()));
    configure_transformed_join(ctx);
    exec_sql(ctx,
             "SET enable_partition_pruning = off;"
             "SET enable_hashjoin = off;"
             "SET enable_mergejoin = off;"
             "SET enable_nestloop = on;"
             "SET enable_memoize = off;");

    const std::string sql =
        fmt::format("SELECT d.pass_no, d.driver, matched.ids, matched.hit_count "
                    "FROM (VALUES (1,1), (2,2), (3,3), "
                    "             (4,1), (5,2), (6,3)) AS d(pass_no,driver) "
                    "CROSS JOIN LATERAL ("
                    "  SELECT string_agg(hit.id::text, ',' ORDER BY hit.id) AS ids, "
                    "         count(*)::bigint AS hit_count "
                    "  FROM ("
                    "    SELECT DISTINCT q.id, t.pk "
                    "    FROM {} AS t "
                    "    INNER JOIN transformed_owner_append_queries AS q "
                    "      ON t.pk=q.target_pk "
                    "     AND mmsearch("
                    "       t.seq0[0:16] OPERATOR(bioseq.||) t.seq0[16:], "
                    "       q.seq[0:]) "
                    "    WHERE q.driver=d.driver "
                    "    OFFSET 0"
                    "  ) AS hit"
                    ") AS matched "
                    "ORDER BY d.pass_no;",
                    k_transformed_join_target);
    const auto plan = explain_text<false>(ctx, sql);
    REQUIRE(plan_contains(plan, "Nested Loop"));
    REQUIRE(plan_contains(plan, "Values Scan"));
    REQUIRE(plan_contains(plan, "Append"));
    REQUIRE(plan_contains(plan, "transformed_owner_append_queries_lo"));
    REQUIRE(plan_contains(plan, "transformed_owner_append_queries_hi"));
    REQUIRE(plan_contains(plan, "Bioseq Mmsearch Ownership: per-row recipe"));
    require_owned_qual_audit(plan);

    std::size_t batch_join_count = 0u;
    std::size_t slice_project_count = 0u;
    std::optional<std::size_t> batch_join_line;
    std::optional<std::size_t> slice_project_line;
    std::optional<std::size_t> append_line;
    for (std::size_t index = 0u; index < plan.size(); ++index) {
        if (plan[index].find("Custom Scan (BioseqBatchJoin)") != std::string::npos) {
            ++batch_join_count;
            if (!batch_join_line.has_value())
                batch_join_line = index;
        }
        if (plan[index].find("Custom Scan (BioseqSliceProject)") != std::string::npos) {
            ++slice_project_count;
            if (!slice_project_line.has_value())
                slice_project_line = index;
        }
        if (!append_line.has_value() && plan[index].find("Append") != std::string::npos) {
            append_line = index;
        }
    }
    REQUIRE(batch_join_count == 1u);
    REQUIRE(slice_project_count == 1u);
    REQUIRE(batch_join_line.has_value());
    REQUIRE(slice_project_line.has_value());
    REQUIRE(append_line.has_value());
    REQUIRE(*batch_join_line < *slice_project_line);
    REQUIRE(*slice_project_line < *append_line);

    const auto batch_capacity = find_explain_property(plan, "Bioseq Batch Capacity");
    REQUIRE(batch_capacity.has_value());
    const long long capacity = std::stoll(*batch_capacity);
    REQUIRE(capacity == 8192);
    REQUIRE(query_scalar_int(ctx,
                             "SELECT max(branch_rows) FROM ("
                             " SELECT count(*) AS branch_rows "
                             " FROM transformed_owner_append_queries_lo "
                             " UNION ALL "
                             " SELECT count(*) AS branch_rows "
                             " FROM transformed_owner_append_queries_hi"
                             ") AS branch_counts;") < capacity);
    REQUIRE(query_scalar_int(ctx,
                             "SELECT max(driver_rows) FROM ("
                             " SELECT driver, count(*) AS driver_rows "
                             " FROM transformed_owner_append_queries "
                             " GROUP BY driver"
                             ") AS driver_counts;") == 4);

#ifdef BIOSEQ_TEST_INDEXES
    exec_sql(ctx, "SELECT bioseq_internal.reset_sequence_source_counters();");
#endif
    const rowset_t rows = query_rows(ctx, sql);
    const rowset_t expected{
        {"1", "1", "101,102,103,104", "4"},
        {"2", "2", "201,202,203,204", "4"},
        {"3", "3", "301,302,303,304", "4"},
        {"4", "1", "101,102,103,104", "4"},
        {"5", "2", "201,202,203,204", "4"},
        {"6", "3", "301,302,303,304", "4"},
    };
    REQUIRE(rows == expected);

#ifdef BIOSEQ_TEST_INDEXES
    const auto source_counter = [&ctx](const std::string_view name) {
        return query_scalar_int(ctx,
                                fmt::format("SELECT (bioseq_internal.sequence_source_counters()"
                                            "        ->>'{}')::bigint;",
                                            name));
    };
    REQUIRE(source_counter("registries_created") == 1);
    REQUIRE(source_counter("domains_created") == 1);
    REQUIRE(source_counter("domains_reused") >= 5);
    REQUIRE(source_counter("registries_created") == source_counter("registries_destroyed"));
    REQUIRE(source_counter("domains_created") == source_counter("domains_destroyed"));
    REQUIRE(source_counter("leases_acquired") == source_counter("leases_released"));
    REQUIRE(source_counter("live_registries") == 0);
    REQUIRE(source_counter("live_domains") == 0);
    REQUIRE(source_counter("live_leases") == 0);
#endif
}

TEST_CASE("Generic and custom plans retain transformed ownership",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    auto ctx = fresh_context({k_transformed_join_target});
    seed_transformed_join_fixture(ctx);

    constexpr std::array<std::string_view, 2u> plan_modes{"force_generic_plan",
                                                          "force_custom_plan"};
    constexpr std::array<std::string_view, 2u> option_suffixes{"",
                                                               ", NULL::bioseq_aligner_options"};
    for (std::size_t mode_index = 0u; mode_index < plan_modes.size(); ++mode_index) {
        exec_sql(ctx, fmt::format("SET plan_cache_mode='{}';", plan_modes[mode_index]));
        for (std::size_t oid_index = 0u; oid_index < option_suffixes.size(); ++oid_index) {
            const std::string statement =
                fmt::format("transformed_owned_{}_{}", mode_index, oid_index);
            exec_sql(ctx,
                     fmt::format("PREPARE {}(int) AS "
                                 "SELECT t.pk, q.id FROM {} AS t INNER JOIN {} AS q "
                                 "ON t.pk=q.target_pk AND t.pk>0 AND q.target_pk>0 "
                                 "AND q.id=$1 AND mmsearch(t.seq0[0:], q.seq[0:]{} ) "
                                 "ORDER BY t.pk LIMIT 1;",
                                 statement,
                                 k_transformed_join_target,
                                 k_transformed_join_queries,
                                 option_suffixes[oid_index]));
            const auto plan = explain_text<false>(ctx, fmt::format("EXECUTE {}(1);", statement));
            REQUIRE(plan_contains(plan, "Custom Scan (BioseqBatchJoin)"));
            REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
            require_owned_qual_audit(plan);

            const auto first = query_rows(ctx, fmt::format("EXECUTE {}(1);", statement));
            const auto null_query = query_rows(ctx, fmt::format("EXECUTE {}(2);", statement));
            const auto restored = query_rows(ctx, fmt::format("EXECUTE {}(3);", statement));
            const auto repeated = query_rows(ctx, fmt::format("EXECUTE {}(1);", statement));
            const auto null_param = query_rows(ctx, fmt::format("EXECUTE {}(NULL);", statement));
            REQUIRE(first.size() == 1u);
            REQUIRE((first.front() ==
                     row_t{std::optional<std::string>{"1"}, std::optional<std::string>{"1"}}));
            REQUIRE(restored.size() == 1u);
            REQUIRE((restored.front() ==
                     row_t{std::optional<std::string>{"1"}, std::optional<std::string>{"3"}}));
            REQUIRE(repeated == first);
            REQUIRE(null_query.empty());
            REQUIRE(null_param.empty());
            exec_sql(ctx, fmt::format("DEALLOCATE {};", statement));
        }
    }
}

TEST_CASE("Final target, query, and options order is exact across rescans",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    auto ctx = fresh_context({k_transformed_join_target});
    seed_transformed_join_fixture(ctx);
    exec_sql(ctx,
             "CREATE TEMP TABLE transformed_effect_log ("
             "ordinal bigserial PRIMARY KEY, label text UNIQUE NOT NULL);"
             "CREATE FUNCTION pg_temp.transformed_mark_once(label text, value text) "
             "RETURNS text LANGUAGE plpgsql VOLATILE AS $$"
             "BEGIN "
             "  INSERT INTO transformed_effect_log(label) VALUES(label); "
             "  RETURN value; "
             "END$$;");

    const std::string sql =
        fmt::format("SELECT d.ord, "
                    "CASE WHEN matched.pk IS NULL THEN NULL ELSE "
                    " pg_temp.transformed_mark_once('target-'||d.ord, "
                    " matched.target_value) END, "
                    "CASE WHEN matched.pk IS NULL THEN NULL ELSE "
                    " pg_temp.transformed_mark_once('query-'||d.ord, "
                    " matched.query_value) END, "
                    "CASE WHEN matched.pk IS NULL THEN NULL ELSE "
                    " pg_temp.transformed_mark_once('options-'||d.ord, 'default') END "
                    "FROM (VALUES (1), (2), (3)) AS d(ord) "
                    "LEFT JOIN LATERAL ("
                    "  SELECT t.pk, t.seq0[0:]::text AS target_value, "
                    "         q.seq[0:]::text AS query_value "
                    "  FROM {} AS t INNER JOIN {} AS q "
                    "    ON t.pk=q.target_pk "
                    "   AND mmsearch(t.seq0[0:], q.seq[0:], "
                    "       NULL::bioseq_aligner_options) "
                    "  WHERE q.id=d.ord ORDER BY t.pk LIMIT 1 OFFSET 0"
                    ") AS matched ON true ORDER BY d.ord;",
                    k_transformed_join_target,
                    k_transformed_join_queries);
    const auto plan = explain_text<false>(ctx, sql);
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqBatchJoin)"));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
    require_owned_qual_audit(plan);
    REQUIRE(query_scalar_int(ctx, "SELECT count(*) FROM transformed_effect_log;") == 0);

    constexpr std::string_view expected_order =
        "target-1,query-1,options-1,target-3,query-3,options-3";
    for (unsigned execution = 0; execution < 2u; ++execution) {
        exec_sql(ctx, "TRUNCATE transformed_effect_log RESTART IDENTITY;");
        const auto rows = query_rows(ctx, sql);
        REQUIRE(rows.size() == 3u);
        REQUIRE(rows[0][1].has_value());
        REQUIRE(rows[0][2].has_value());
        REQUIRE(rows[0][3] == "default");
        REQUIRE_FALSE(rows[1][1].has_value());
        REQUIRE_FALSE(rows[1][2].has_value());
        REQUIRE_FALSE(rows[1][3].has_value());
        REQUIRE(rows[2][1].has_value());
        REQUIRE(rows[2][2].has_value());
        REQUIRE(rows[2][3] == "default");
        REQUIRE(query_scalar(ctx,
                             "SELECT string_agg(label,',' ORDER BY ordinal) "
                             "FROM transformed_effect_log;") == expected_order);
    }
}

TEST_CASE("Ineligible mmsearch arguments keep target, query, and options order",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    connection_test_context ctx;
    exec_sql(ctx,
             fmt::format("CREATE TEMP TABLE argument_effect_targets ("
                         "id int PRIMARY KEY, seq bioseq.seq('dna4',32));"
                         "CREATE TEMP TABLE argument_effect_queries ("
                         "id int PRIMARY KEY, seq bioseq.seq('dna4',32));"
                         "INSERT INTO argument_effect_targets VALUES (1,{});"
                         "INSERT INTO argument_effect_queries VALUES (1,{});"
                         "CREATE TEMP SEQUENCE argument_effect_sequence;"
                         "CREATE FUNCTION pg_temp.argument_effect_target("
                         "value bioseq.seq) RETURNS bioseq.seq LANGUAGE plpgsql VOLATILE "
                         "AS $$DECLARE observed bigint; BEGIN "
                         "observed := nextval('pg_temp.argument_effect_sequence'); "
                         "IF observed <> 1 THEN RAISE EXCEPTION "
                         "'target order/duplication: %', observed; END IF; "
                         "RETURN value; END$$;"
                         "CREATE FUNCTION pg_temp.argument_effect_query("
                         "value bioseq.seq) RETURNS bioseq.seq LANGUAGE plpgsql VOLATILE "
                         "AS $$DECLARE observed bigint; BEGIN "
                         "observed := nextval('pg_temp.argument_effect_sequence'); "
                         "IF observed <> 2 THEN RAISE EXCEPTION "
                         "'query order/duplication: %', observed; END IF; "
                         "RETURN value; END$$;"
                         "CREATE FUNCTION pg_temp.argument_effect_options("
                         "value bioseq_aligner_options) RETURNS bioseq_aligner_options "
                         "LANGUAGE plpgsql VOLATILE AS $$DECLARE observed bigint; BEGIN "
                         "observed := nextval('pg_temp.argument_effect_sequence'); "
                         "IF observed <> 3 THEN RAISE EXCEPTION "
                         "'options order/duplication: %', observed; END IF; "
                         "RETURN value; END$$;",
                         hoe_probe_literal(),
                         hoe_probe_literal()));

    constexpr std::array<std::string_view, 2u> plan_modes{"force_generic_plan",
                                                          "force_custom_plan"};
    for (std::size_t index = 0u; index < plan_modes.size(); ++index) {
        exec_sql(ctx, fmt::format("SET plan_cache_mode='{}';", plan_modes[index]));
        const std::string statement = fmt::format("argument_effects_{}", index);
        exec_sql(ctx,
                 fmt::format("PREPARE {}(int) AS "
                             "SELECT t.id FROM argument_effect_targets AS t "
                             "INNER JOIN argument_effect_queries AS q "
                             "ON t.id=q.id AND q.id=$1 "
                             "AND mmsearch("
                             "pg_temp.argument_effect_target(t.seq[0:]), "
                             "pg_temp.argument_effect_query(q.seq[0:]), "
                             "pg_temp.argument_effect_options("
                             "NULL::bioseq_aligner_options));",
                             statement));

        /* EXPLAIN/PREPARE must not evaluate any volatile argument. The
         * functions intentionally make the candidate ineligible; eligible
         * options remain raw/folded Consts by contract. */
        exec_sql(ctx,
                 "SELECT setval('pg_temp.argument_effect_sequence',"
                 "1,false);");
        const auto plan = explain_text<false>(ctx, fmt::format("EXECUTE {}(1);", statement));
        REQUIRE_FALSE(plan.empty());
        REQUIRE(query_scalar(ctx,
                             "SELECT is_called::text FROM "
                             "pg_temp.argument_effect_sequence;") == "false");

        for (unsigned execution = 0; execution < 2u; ++execution) {
            exec_sql(ctx,
                     "SELECT setval("
                     "'pg_temp.argument_effect_sequence',1,false);");
            const auto error = sql_error(ctx, fmt::format("EXECUTE {}(1);", statement));
            REQUIRE(error.sqlstate == "0A000");
            REQUIRE((error.contains("not owned") || error.contains("no Bioseq executor owner")));
            REQUIRE(query_scalar_int(ctx,
                                     "SELECT last_value FROM "
                                     "pg_temp.argument_effect_sequence;") == 3);
            REQUIRE(query_scalar(ctx,
                                 "SELECT is_called::text FROM "
                                 "pg_temp.argument_effect_sequence;") == "true");
        }
        exec_sql(ctx, fmt::format("DEALLOCATE {};", statement));
    }
}
