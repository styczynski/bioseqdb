/**
 * @file source_domain_parallel_policy_test.cpp
 * @brief Exercises parallel source routes, planner metadata, and security policy.
 * @note CMake compiles this source only when BIOSEQ_TEST_INDEXES is enabled.
 */

#include <libpq-fe.h>

#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.tests.integration.support;
import bioseq.tests.integration.source_domain_lifetime_test_support;

using namespace bioseq_int_tests;
using namespace bioseq_int_tests::source_domain_lifetime_test_support;

TEST_CASE("BatchSort keeps child resnos distinct from physical attnos",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    constexpr std::string_view table = "source_lifetime_batch_sort_attno_namespace";
    auto ctx = fresh_context({table});
    exec_sql(ctx,
        "SET bioseq.index.insert_maintenance.enabled=off;"
        "CREATE TABLE source_lifetime_batch_sort_attno_namespace ("
        " pad bioseq.seq('dna4',32),"
        " id int,"
        " wanted bioseq.seq('dna4',32)"
        ") USING bioseq;"
        "INSERT INTO source_lifetime_batch_sort_attno_namespace VALUES ("
        " repeat('A',4096)::bioseq.seq('dna4',32),"
        " 7,"
        " repeat('TGCA',1024)::bioseq.seq('dna4',32)"
        ");");

    /* The scan projects only wanted: child resno 1 therefore names physical
     * attribute 3, while physical attribute 1 is a different SEQ locator.
     * Confusing those namespaces returns the pad slice (AAAAAAAA). */
    constexpr auto sql =
        "SELECT bioseq.to_text(wanted[0:8]) "
        "FROM source_lifetime_batch_sort_attno_namespace;";
    const auto planned = explain_text<false>(ctx, sql);
    REQUIRE(plan_contains(planned, "Custom Scan (BioseqSliceProject)"));

    reset_source_counters(ctx);
    REQUIRE(query_scalar(ctx, sql) == "TGCATGCA");
    REQUIRE(source_counter(ctx, "wrong_thread_rejections") == 0);
    require_source_resources_balanced(ctx);

    reset_source_counters(ctx);
    const auto analyzed = explain_text<true>(ctx, sql);
    REQUIRE(plan_contains(analyzed, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(source_boundary_actual_calls(
        analyzed, "text", "SQL conversion") == 1u);
    REQUIRE_FALSE(plan_contains(analyzed, "Counter Overflow: true"));
    REQUIRE(source_counter(ctx, "wrong_thread_rejections") == 0);
    require_source_resources_balanced(ctx);
}

TEST_CASE("parallel lazy providers adopt the flushed scan snapshot",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    constexpr std::string_view table = "source_lifetime_parallel_source_snapshot";
    auto ctx = fresh_context({table});
    REQUIRE(query_scalar_int(ctx,
        "SELECT current_setting('max_parallel_workers')::int;") > 0);
    exec_sql(ctx,
        "SET bioseq.index.insert_maintenance.enabled=off;"
        "SET bioseq.execution.parallel.enabled=on;"
        "SET bioseq.execution.parallel.minimum_processes=2;"
        "SET max_parallel_workers_per_gather=2;"
        "SET min_parallel_table_scan_size=0;"
        "SET parallel_setup_cost=0;"
        "SET parallel_tuple_cost=0;"
        "SET parallel_leader_participation=off;"
        "DO $$ BEGIN "
        " IF EXISTS (SELECT 1 FROM pg_settings "
        "            WHERE name='debug_parallel_query') THEN "
        "   PERFORM set_config('debug_parallel_query','on',false);"
        " ELSIF EXISTS (SELECT 1 FROM pg_settings "
        "               WHERE name='force_parallel_mode') THEN "
        "   PERFORM set_config('force_parallel_mode','on',false);"
        " END IF; END $$;");
    exec_sql(ctx,
        "BEGIN;"
        "CREATE TABLE source_lifetime_parallel_source_snapshot "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq "
        " WITH (autovacuum_enabled=false);"
        "INSERT INTO source_lifetime_parallel_source_snapshot "
        "SELECT g, repeat('TGCA',2048)::bioseq.seq('dna4',32) "
        "FROM generate_series(1,64) AS g;"
        "COMMIT;");
    exec_sql(ctx,
        "BEGIN;"
        "INSERT INTO source_lifetime_parallel_source_snapshot "
        "SELECT 1000+g, repeat('ACGT',2048)::bioseq.seq('dna4',32) "
        "FROM generate_series(1,64) AS g;");

    constexpr auto sql =
        "SELECT id,bioseq.to_text(s[0:8]) "
        "FROM source_lifetime_parallel_source_snapshot;";
    const auto planned = explain_text<false>(ctx, sql);
    REQUIRE(plan_contains(planned, "Gather"));
    REQUIRE(plan_contains(planned, "Workers Planned: 2"));
    REQUIRE_FALSE(plan_contains(planned, "Single Copy: true"));
    REQUIRE(plan_line_count(
        planned, "Custom Scan (BioseqSliceProject)") == 1u);
    const auto gather = plan_line_index(planned, "Gather");
    const auto owner = plan_line_index(
        planned, "Custom Scan (BioseqSliceProject)");
    REQUIRE(gather.has_value());
    REQUIRE(owner.has_value());
    REQUIRE(*gather < *owner);

    rowset_t expected;
    expected.reserve(128u);
    for (int id = 1; id <= 64; ++id) {
        expected.push_back({
            std::optional<std::string>{std::to_string(id)},
            std::optional<std::string>{"TGCATGCA"},
        });
    }
    for (int id = 1001; id <= 1064; ++id) {
        expected.push_back({
            std::optional<std::string>{std::to_string(id)},
            std::optional<std::string>{"ACGTACGT"},
        });
    }
    std::sort(expected.begin(), expected.end());
    reset_source_counters(ctx);
    auto actual = query_rows(ctx, sql);
    std::sort(actual.begin(), actual.end());
    REQUIRE(actual == expected);
    REQUIRE(source_counter(ctx, "wrong_thread_rejections") == 0);
    require_source_resources_balanced(ctx);
    exec_sql(ctx, "COMMIT;");
}

TEST_CASE("parallel stripe claiming skips aborted same-relation gaps",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    constexpr std::string_view table = "source_lifetime_parallel_stripe_gaps";
    auto ctx = fresh_context({table});
    exec_sql(ctx,
        "SET bioseq.index.insert_maintenance.enabled=off;"
        "SET bioseq.storage.stripe.row_limit=1000;"
        "SET bioseq.storage.chunk_group.row_limit=1000;"
        "CREATE TABLE source_lifetime_parallel_stripe_gaps "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;");

    /* Stripe ids are relation-local.  A first row reserves the next id in the
     * relation metapage, which is not rolled back with its MVCC metadata row.
     * Burn ids 2 and 4 in aborted transactions so the reader must claim
     * visible stripes 1, 3 and 5 without duplicating a gap successor. */
    exec_sql(ctx,
        "INSERT INTO source_lifetime_parallel_stripe_gaps "
        "SELECT g, repeat('ACGT',16)::bioseq.seq('dna4',32) "
        "FROM generate_series(1,1000) AS g;");
    exec_sql(ctx,
        "BEGIN;"
        "INSERT INTO source_lifetime_parallel_stripe_gaps VALUES (-1,NULL);"
        "ROLLBACK;");
    exec_sql(ctx,
        "INSERT INTO source_lifetime_parallel_stripe_gaps "
        "SELECT g, repeat('TGCA',16)::bioseq.seq('dna4',32) "
        "FROM generate_series(1001,2000) AS g;");
    exec_sql(ctx,
        "BEGIN;"
        "INSERT INTO source_lifetime_parallel_stripe_gaps VALUES (-2,NULL);"
        "ROLLBACK;");
    exec_sql(ctx,
        "INSERT INTO source_lifetime_parallel_stripe_gaps "
        "SELECT g, repeat('GATT',16)::bioseq.seq('dna4',32) "
        "FROM generate_series(2001,3000) AS g;");

    REQUIRE(query_int_column(ctx,
        "SELECT stripe_num FROM bioseq.stripe "
        "WHERE relation='source_lifetime_parallel_stripe_gaps'::regclass "
        "ORDER BY stripe_num;") == std::vector<long long>{1, 3, 5});
    REQUIRE(query_scalar_int(ctx,
        "SELECT current_setting('max_parallel_workers')::int;") > 0);
    exec_sql(ctx,
        "SET bioseq.execution.parallel.enabled=on;"
        "SET max_parallel_workers_per_gather=2;"
        "SET min_parallel_table_scan_size=0;"
        "SET parallel_setup_cost=0;"
        "SET parallel_tuple_cost=0;"
        "SET parallel_leader_participation=off;"
        "DO $$ BEGIN "
        " IF EXISTS (SELECT 1 FROM pg_settings "
        "            WHERE name='debug_parallel_query') THEN "
        "   PERFORM set_config('debug_parallel_query','on',false);"
        " ELSIF EXISTS (SELECT 1 FROM pg_settings "
        "               WHERE name='force_parallel_mode') THEN "
        "   PERFORM set_config('force_parallel_mode','on',false);"
        " END IF; END $$;");

    constexpr auto exact_sql =
        "SELECT id,bioseq.to_text(s[0:8]) "
        "FROM source_lifetime_parallel_stripe_gaps ORDER BY id;";
    const auto planned = explain_text<false>(ctx, exact_sql);
    REQUIRE(plan_contains(planned, "Gather"));
    REQUIRE(plan_contains(planned, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(plan_line_index(planned, "Gather")
            < plan_line_index(
                planned, "Custom Scan (BioseqSliceProject)"));

    rowset_t expected;
    expected.reserve(3000u);
    for (long long id = 1; id <= 3000; ++id) {
        const std::string prefix = id <= 1000
            ? "ACGTACGT" : id <= 2000 ? "TGCATGCA" : "GATTGATT";
        expected.push_back({
            std::optional<std::string>{std::to_string(id)},
            std::optional<std::string>{prefix},
        });
    }

    reset_source_counters(ctx);
    const auto analyzed = explain_text<true>(ctx,
        "SELECT bioseq.to_text(s[0:8]) "
        "FROM source_lifetime_parallel_stripe_gaps;");
    REQUIRE(plan_contains(analyzed, "Gather"));
    REQUIRE(plan_contains(
        analyzed, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(source_boundary_actual_calls(
        analyzed, "text", "SQL conversion") == 3000u);
    REQUIRE_FALSE(plan_contains(analyzed, "Incomplete Workers"));
    REQUIRE_FALSE(plan_contains(analyzed, "Counter Overflow: true"));
    REQUIRE(source_counter(ctx, "wrong_thread_rejections") == 0);
    require_source_resources_balanced(ctx);

    for (int execution = 0; execution < 2; ++execution) {
        reset_source_counters(ctx);
        REQUIRE(query_rows(ctx, exact_sql) == expected);
        REQUIRE(source_counter(ctx, "wrong_thread_rejections") == 0);
        require_source_resources_balanced(ctx);
    }
}

TEST_CASE("parallel worker ERROR preserves DSM cleanup and original error",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    constexpr std::string_view source = "source_lifetime_slice_worker_error";
    auto ctx = fresh_context({source});
    exec_sql(ctx,
        "CREATE TABLE source_lifetime_slice_worker_error "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;"
        "INSERT INTO source_lifetime_slice_worker_error "
        "SELECT g, repeat('ACGT',2048)::bioseq.seq('dna4',32) "
        "FROM generate_series(1,256) AS g;");
    REQUIRE(query_scalar_int(ctx,
        "SELECT current_setting('max_parallel_workers')::int;") > 0);
    exec_sql(ctx,
        "SET max_parallel_workers_per_gather=2;"
        "SET min_parallel_table_scan_size=0;"
        "SET parallel_setup_cost=0;"
        "SET parallel_tuple_cost=0;"
        "SET parallel_leader_participation=off;"
        "DO $$ BEGIN "
        " IF EXISTS (SELECT 1 FROM pg_settings "
        "            WHERE name='debug_parallel_query') THEN "
        "   PERFORM set_config('debug_parallel_query','on',false);"
        " ELSIF EXISTS (SELECT 1 FROM pg_settings "
        "               WHERE name='force_parallel_mode') THEN "
        "   PERFORM set_config('force_parallel_mode','on',false);"
        " END IF; END $$;");

    constexpr auto error_sql =
        "SELECT bioseq.to_text(s[0:64]) "
        "FROM source_lifetime_slice_worker_error "
        "WHERE bioseq_internal.slice_trace_worker_failure_probe(id);";
    const auto planned = explain_text<false>(ctx, error_sql);
    REQUIRE(plan_contains(planned, "Gather"));
    REQUIRE(plan_contains(
        planned, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(plan_contains(
        planned, "slice_trace_worker_failure_probe"));

    reset_source_counters(ctx);
    const auto injected = sql_error(ctx, error_sql);
    REQUIRE(injected.sqlstate == "XX000");
    REQUIRE(injected.contains(
        "injected compact-sequence parallel worker failure"));
    require_source_resources_balanced(ctx);
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);

    /* A fresh forced-parallel cycle must attach, publish and detach cleanly
     * after the failed cycle.  With leader participation disabled, all 256
     * successful conversion deltas must arrive through worker DSM slots. */
    reset_source_counters(ctx);
    const auto recovered = explain_text<true>(ctx,
        "SELECT bioseq.to_text(s[0:64]) "
        "FROM source_lifetime_slice_worker_error;");
    REQUIRE(plan_contains(recovered, "Gather"));
    REQUIRE(plan_contains(
        recovered, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(source_boundary_actual_calls(
        recovered, "text", "SQL conversion") == 256u);
    REQUIRE_FALSE(plan_contains(recovered, "Incomplete Workers"));
    REQUIRE_FALSE(plan_contains(recovered, "Counter Overflow: true"));
    REQUIRE(source_counter(ctx, "wrong_thread_rejections") == 0);
    require_source_resources_balanced(ctx);
}

TEST_CASE("production source routes never invoke PostgreSQL off backend",
          "[component:bioseq][layer:integration][feature:scheduler][resource:postgres]") {
    constexpr std::string_view table = "source_lifetime_source_routes";
    auto ctx = fresh_context({table});
    exec_sql(ctx,
        "SET bioseq.index.insert_maintenance.enabled=off;"
        "SET bioseq.index.sequence_read.enabled=off;"
        "SET bioseq.index.async_persistence.enabled=off;"
        "SET bioseq.index.recompute_async_persistence.enabled=off;"
        "SET bioseq.aligner.minimizer.algorithm='scalar';"
        "SET bioseq.aligner.chaining.algorithm='scalar_minimap2';"
        "SET bioseq.aligner.base_alignment.algorithm='ksw2';"
        "SET bioseq.aligner.vulkan.enabled=off;"
        "SET bioseq.scheduler.pipeline.mode='task_parallel';"
        "CREATE TABLE source_lifetime_source_routes "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;");
    const std::string route_target_sequence =
        source_domain_high_entropy_dna4(65536u);
    const std::string route_query_sequence =
        route_target_sequence.substr(32768u, 128u);
    exec_sqlf(ctx,
        "INSERT INTO source_lifetime_source_routes VALUES "
        " (1, '{}'::bioseq.seq('dna4',32)),"
        " (2, repeat('T',65536)::bioseq.seq('dna4',32));",
        route_target_sequence);

    reset_source_counters(ctx);
    REQUIRE(query_scalar(ctx,
        "SELECT bioseq.to_text(s[32765:32773]) "
        "FROM source_lifetime_source_routes WHERE id=1;")
        == route_target_sequence.substr(32765u, 8u));
    REQUIRE(source_counter(ctx, "scan_providers_created") == 1);
    REQUIRE(source_counter(ctx, "wrong_thread_rejections") == 0);
    require_source_resources_balanced(ctx);

    /* Exercise all four lazy-facade cursor orientations on a non-centered,
     * non-palindromic subrange.  The planner evidence and source counters pin the
     * diagnostic to a real provider-backed expanded value; the probe flattens
     * only a separate compact oracle. */
    constexpr auto orientation_sql =
        "SELECT bioseq_internal.sequence_facade_orientation_probe("
        "s[32749:32786]) FROM source_lifetime_source_routes WHERE id=1;";
    const auto orientation_plan = explain_text<false>(ctx, orientation_sql);
    REQUIRE(plan_contains(
        orientation_plan, "Custom Scan (BioseqSliceProject)"));
    reset_source_counters(ctx);
    REQUIRE(query_scalar(ctx, orientation_sql) == "t");
    REQUIRE(source_counter(ctx, "scan_providers_created") == 1);
    REQUIRE(source_counter(ctx, "domains_created") >= 1);
    REQUIRE(source_counter(ctx, "scanner_entries_created") >= 1);
    REQUIRE(source_counter(ctx, "facade_source_stream_callbacks") > 0);
    REQUIRE(source_counter(ctx, "wrong_thread_rejections") == 0);
    require_source_resources_balanced(ctx);

    exec_sql(ctx, "SET bioseq.planner.sequence_sort_scan.enabled=on;");
    const auto sort_plan = explain_text<false>(ctx,
        "SELECT id FROM source_lifetime_source_routes ORDER BY s;");
    REQUIRE(plan_contains(sort_plan, "BioseqBatchSortScan"));
    reset_source_counters(ctx);
    REQUIRE(query_int_column(ctx,
        "SELECT id FROM source_lifetime_source_routes ORDER BY s;")
        == std::vector<long long>{1, 2});
    REQUIRE(source_counter(ctx, "wrong_thread_rejections") == 0);
    require_source_resources_balanced(ctx);

    reset_source_counters(ctx);
    REQUIRE(query_scalar_int(ctx,
        "SELECT bioseq.update_index('source_lifetime_source_routes'::regclass, "
        " 'minimizers_per_fragment');") == 2);
    REQUIRE(source_counter(ctx, "standalone_providers_created") >= 1);
    REQUIRE(source_counter(ctx, "domains_created") >= 1);
    REQUIRE(source_counter(ctx, "scanner_entries_created") >= 1);
    REQUIRE(source_counter(ctx, "facade_source_stream_callbacks") > 0);
    REQUIRE(source_counter(ctx, "wrong_thread_rejections") == 0);
    require_source_resources_balanced(ctx);

    reset_source_counters(ctx);
    const auto matches = query_int_column(ctx, fmt::format(
        "SELECT id FROM source_lifetime_source_routes "
        "WHERE mmsearch(s, "
        " '{}'::bioseq.seq('dna4',32), "
        " aligner_options(index_read_requirement => 1)) "
        "ORDER BY id;",
        route_query_sequence));
    REQUIRE(matches == std::vector<long long>{1});
    REQUIRE(source_counter(ctx, "domains_created") >= 1);
    REQUIRE(source_counter(ctx, "scanner_entries_created") >= 1);
    REQUIRE(source_counter(ctx, "facade_source_stream_callbacks") > 0);

    /* The source callbacks above are backend-thread-only.  This immutable
     * execution snapshot confirms the selected scalar tuple completed without
     * fallback or unattested work while the task-parallel pool exposed at
     * least two CPU slots.  The cross-thread return-value probe below is the
     * authoritative worker-thread tripwire evidence; the backend-local TLS
     * counter is not used as a substitute for it. */
    const auto task_evidence = query_one(ctx,
        "SELECT effective_cpu_workers::text, requested_backend, "
        "       actual_backend, is_completed::text, is_partial::text, "
        "       is_fully_drained::text, attempted_alignment_jobs::text, "
        "       fully_attested_alignment_jobs::text, "
        "       unattested_alignment_jobs::text, eligible_jobs::text, "
        "       executed_scalar_cpu_jobs::text, "
        "       executed_simd_cpu_jobs::text, "
        "       executed_vulkan_gpu_jobs::text, fallback_jobs::text, "
        "       backend_declines::text, backend_fallbacks::text "
        "FROM bioseq.last_alignment_execution_stats() "
        "WHERE stage='query_sketch';");
    REQUIRE(task_evidence.size() == 16u);
    for (const auto& value : task_evidence) REQUIRE(value.has_value());
    REQUIRE(std::stoll(*task_evidence[0]) >= 2);
    REQUIRE(*task_evidence[1] == "scalar_cpu");
    REQUIRE(*task_evidence[2] == "scalar_cpu");
    REQUIRE(*task_evidence[3] == "true");
    REQUIRE(*task_evidence[4] == "false");
    REQUIRE(*task_evidence[5] == "true");
    REQUIRE(std::stoll(*task_evidence[6]) > 0);
    REQUIRE(*task_evidence[7] == *task_evidence[6]);
    REQUIRE(*task_evidence[8] == "0");
    REQUIRE(std::stoll(*task_evidence[9]) > 0);
    REQUIRE(*task_evidence[10] == *task_evidence[9]);
    for (const std::size_t zero : {11u, 12u, 13u, 14u, 15u}) {
        REQUIRE(*task_evidence[zero] == "0");
    }
    REQUIRE(source_counter(ctx, "wrong_thread_rejections") == 0);
    require_source_resources_balanced(ctx);

    reset_source_counters(ctx);
    /* The SQL boolean carries the status written by the spawned std::thread
     * after join.  The TLS counter increment is deliberately replayed on the
     * backend thread and remains only a local diagnostic assertion. */
    REQUIRE(query_scalar(
        ctx, "SELECT bioseq_internal.sequence_source_wrong_thread_probe();")
        == "t");
    REQUIRE(source_counter(ctx, "wrong_thread_rejections") == 1);
    REQUIRE(source_counter(ctx, "fault_injections") == 0);
    REQUIRE(source_counter(ctx, "cache_sets_created") == 0);
    REQUIRE(source_counter(ctx, "snapshots_registered") == 0);
    require_source_quiescent(ctx);
}

TEST_CASE("locator columns never trust physical minmax metadata",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    constexpr std::string_view table = "source_lifetime_locator_minmax";
    auto ctx = fresh_context({table});
    exec_sql(ctx,
        "CREATE TABLE source_lifetime_locator_minmax "
        " (id int, label text, s bioseq.seq('dna4',32)) USING bioseq;"
        "INSERT INTO source_lifetime_locator_minmax VALUES "
        " (1,'a',repeat('A',8192)::bioseq.seq('dna4',32)),"
        " (2,'c',repeat('C',8192)::bioseq.seq('dna4',32)),"
        " (3,'g1',repeat('G',8192)::bioseq.seq('dna4',32)),"
        " (4,'t',repeat('T',8192)::bioseq.seq('dna4',32)),"
        " (5,'a2',repeat('A',8192)::bioseq.seq('dna4',32)),"
        " (6,'g2',repeat('G',8192)::bioseq.seq('dna4',32));");

    const long long sequence_chunks = query_scalar_int(ctx,
        "SELECT count(*) FROM bioseq.chunk "
        "WHERE relation='source_lifetime_locator_minmax'::regclass "
        "AND attr_num=3;");
    REQUIRE(sequence_chunks > 0);
    REQUIRE(query_scalar_int(ctx,
        "SELECT count(*) FROM bioseq.chunk "
        "WHERE relation='source_lifetime_locator_minmax'::regclass "
        "AND attr_num=3 "
        "AND (minimum_value IS NOT NULL OR maximum_value IS NOT NULL);") == 0);
    REQUIRE(query_scalar_int(ctx,
        "SELECT count(*) FROM bioseq.chunk "
        "WHERE relation='source_lifetime_locator_minmax'::regclass "
        "AND attr_num=2 "
        "AND minimum_value IS NOT NULL AND maximum_value IS NOT NULL;") > 0);

    const std::string adenine = "repeat('A',8192)::bioseq.seq('dna4',32)";
    const std::string cytosine = "repeat('C',8192)::bioseq.seq('dna4',32)";
    const std::string guanine = "repeat('G',8192)::bioseq.seq('dna4',32)";
    REQUIRE(query_int_column(ctx, fmt::format(
        "SELECT id FROM {} WHERE s OPERATOR(bioseq.<) {} ORDER BY id;",
        table, cytosine)) == std::vector<long long>{1, 5});
    REQUIRE(query_int_column(ctx, fmt::format(
        "SELECT id FROM {} WHERE s OPERATOR(bioseq.=) {} ORDER BY id;",
        table, guanine)) == std::vector<long long>{3, 6});
    REQUIRE(query_int_column(ctx, fmt::format(
        "SELECT id FROM {} WHERE s OPERATOR(bioseq.>) {} ORDER BY id;",
        table, guanine)) == std::vector<long long>{4});
    const rowset_t expected_groups{{"A"}, {"C"}, {"G"}, {"T"}};
    REQUIRE(query_rows(ctx,
        "SELECT bioseq.to_text(s[0:1]) "
        "FROM source_lifetime_locator_minmax GROUP BY s ORDER BY s;")
        == expected_groups);
    REQUIRE(query_rows(ctx,
        "SELECT bioseq.to_text(s[0:1]) FROM ("
        " SELECT DISTINCT s FROM source_lifetime_locator_minmax"
        ") AS distinct_sequences ORDER BY 1;") == expected_groups);

    exec_sql(ctx,
        "SET enable_nestloop=off; SET enable_mergejoin=off; "
        "SET enable_hashjoin=on;");
    const auto hash_plan = explain_text<false>(ctx,
        "SELECT l.id, r.id FROM source_lifetime_locator_minmax AS l "
        "JOIN source_lifetime_locator_minmax AS r "
        "ON l.s OPERATOR(bioseq.=) r.s;");
    REQUIRE(plan_contains(hash_plan, "Hash Join"));
    REQUIRE(query_int_column(ctx,
        "SELECT l.id * 10 + r.id FROM source_lifetime_locator_minmax AS l "
        "JOIN source_lifetime_locator_minmax AS r "
        "ON l.s OPERATOR(bioseq.=) r.s ORDER BY 1;")
        == std::vector<long long>{11, 15, 22, 33, 36, 44, 51, 55, 63, 66});

    /* Forge hasMinMax=true with structurally valid varlena bytes borrowed from
     * the text column. The seq reader must ignore them before comparison. The
     * transaction-local catalog mutation is rolled back after the read. */
    exec_sql(ctx,
        "BEGIN;"
        "WITH donor AS ("
        " SELECT storage_id, stripe_num, chunk_group_num, "
        "        minimum_value, maximum_value "
        " FROM bioseq_internal.chunk WHERE attr_num=2"
        ") UPDATE bioseq_internal.chunk AS sequence_chunk "
        "SET minimum_value=donor.minimum_value, "
        "    maximum_value=donor.maximum_value "
        "FROM donor "
        "WHERE sequence_chunk.storage_id=donor.storage_id "
        "AND sequence_chunk.stripe_num=donor.stripe_num "
        "AND sequence_chunk.chunk_group_num=donor.chunk_group_num "
        "AND sequence_chunk.attr_num=3 "
        "AND sequence_chunk.storage_id="
        "    bioseq.get_storage_id('source_lifetime_locator_minmax'::regclass);"
        "SET LOCAL enable_indexscan=off;"
        "SET LOCAL enable_bitmapscan=off;");
    REQUIRE(query_scalar_int(ctx,
        "SELECT count(*) FROM bioseq.chunk "
        "WHERE relation='source_lifetime_locator_minmax'::regclass "
        "AND attr_num=3 "
        "AND minimum_value IS NOT NULL AND maximum_value IS NOT NULL;")
        == sequence_chunks);
    REQUIRE(query_int_column(ctx, fmt::format(
        "SELECT id FROM {} WHERE s OPERATOR(bioseq.>) {} ORDER BY id;",
        table, cytosine)) == std::vector<long long>{3, 4, 6});
    exec_sql(ctx, "ROLLBACK;");
    REQUIRE(query_scalar_int(ctx,
        "SELECT count(*) FROM bioseq.chunk "
        "WHERE relation='source_lifetime_locator_minmax'::regclass "
        "AND attr_num=3 "
        "AND (minimum_value IS NOT NULL OR maximum_value IS NOT NULL);") == 0);

    exec_sql(ctx,
        "RESET enable_nestloop; RESET enable_mergejoin; RESET enable_hashjoin;"
        "CREATE INDEX source_lifetime_locator_minmax_btree "
        "ON source_lifetime_locator_minmax USING btree(s);"
        "SET enable_seqscan=off;");
    REQUIRE(query_int_column(ctx, fmt::format(
        "SELECT id FROM {} WHERE s OPERATOR(bioseq.=) {} ORDER BY id;",
        table, adenine)) == std::vector<long long>{1, 5});
}

TEST_CASE("mmsearch ownership respects RLS barriers and effectful siblings",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    constexpr std::string_view table = "source_lifetime_source_security";
    auto ctx = fresh_context({table});
    const auto canonical_rows = hoe_seed_rows();
    const seq_typmod dna4{.alphabet = "dna4", .width = 32};
    const std::string positive_target = format_literal(
        canonical_rows.front().sequences.front(), dna4);
    const std::string alternate_target = format_literal(
        canonical_rows[1].sequences.front(), dna4);
    exec_sql(ctx, fmt::format(
        "SET bioseq.index.insert_maintenance.enabled=on;"
        "SET bioseq.index.sequence_read.enabled=off;"
        "SET bioseq.index.async_persistence.enabled=off;"
        "SET bioseq.index.recompute_async_persistence.enabled=off;"
        "SET bioseq.vectorization.enabled=off;"
        "SET bioseq.aligner.minimizer.algorithm='scalar';"
        "SET bioseq.aligner.chaining.algorithm='scalar_minimap2';"
        "SET bioseq.aligner.base_alignment.algorithm='ksw2';"
        "SET bioseq.aligner.chaining.minimum_score=20;"
        "SET bioseq.aligner.vulkan.enabled=off;"
        "SET bioseq.scheduler.pipeline.mode='task_parallel';"
        "CREATE TABLE source_lifetime_source_security "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;"
        "INSERT INTO source_lifetime_source_security VALUES "
        " (1,{}),(2,{}),(3,{});"
        "CREATE FUNCTION pg_temp.source_lifetime_nonleaky(i int) "
        "RETURNS bool LANGUAGE sql IMMUTABLE LEAKPROOF "
        "AS 'SELECT i > 0';"
        "CREATE FUNCTION pg_temp.source_lifetime_leaky(i int) "
        "RETURNS bool LANGUAGE plpgsql STABLE "
        "AS $$ BEGIN RETURN i > 0; END $$;"
        "CREATE FUNCTION pg_temp.source_lifetime_volatile(i int) "
        "RETURNS bool LANGUAGE plpgsql VOLATILE "
        "AS $$ BEGIN RETURN i > 0; END $$;"
        "CREATE TEMP VIEW source_lifetime_source_security_barrier "
        "WITH (security_barrier=true) AS "
        "SELECT id,s FROM source_lifetime_source_security "
        "WHERE pg_temp.source_lifetime_leaky(id);",
        positive_target, alternate_target, positive_target));

    const std::string mmsearch_call = fmt::format(
        "mmsearch(s, {}, aligner_options(index_read_requirement => 1))",
        positive_target);
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT count(*) FROM {} WHERE pg_temp.source_lifetime_nonleaky(id) "
        "AND {};", table, mmsearch_call)) >= 1);
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT count(*) FROM {} WHERE pg_temp.source_lifetime_leaky(id) "
        "AND {};", table, mmsearch_call)) >= 1);

    const auto volatile_error = sql_error(ctx, fmt::format(
        "SELECT id FROM {} WHERE {} "
        "AND pg_temp.source_lifetime_volatile(id);", table, mmsearch_call));
    REQUIRE(volatile_error.sqlstate == "0A000");
    REQUIRE((volatile_error.contains("not owned")
             || volatile_error.contains("no Bioseq executor owner")));

    REQUIRE(query_int_column(ctx,
        "SELECT id FROM source_lifetime_source_security_barrier "
        "WHERE bioseq.len(s[0:8])=8 ORDER BY id;")
        == std::vector<long long>{1, 2, 3});
    const auto barrier_error = sql_error(ctx, fmt::format(
        "SELECT id FROM source_lifetime_source_security_barrier WHERE {};",
        mmsearch_call));
    REQUIRE(barrier_error.sqlstate == "0A000");

    exec_sql(ctx,
        "BEGIN;"
        "CREATE ROLE source_lifetime_source_rls_role NOLOGIN;"
        "GRANT USAGE ON SCHEMA bioseq TO source_lifetime_source_rls_role;"
        "GRANT SELECT ON source_lifetime_source_security "
        "TO source_lifetime_source_rls_role;"
        "ALTER TABLE source_lifetime_source_security ENABLE ROW LEVEL SECURITY;"
        "ALTER TABLE source_lifetime_source_security FORCE ROW LEVEL SECURITY;"
        "CREATE POLICY source_lifetime_source_odd_rows "
        "ON source_lifetime_source_security FOR SELECT "
        "TO source_lifetime_source_rls_role USING ((id % 2)=1);"
        "SET LOCAL ROLE source_lifetime_source_rls_role;");
    REQUIRE(query_int_column(ctx,
        "SELECT id FROM source_lifetime_source_security "
        "WHERE bioseq.len(s[0:8])=8 ORDER BY id;")
        == std::vector<long long>{1, 3});
    const auto rls_plan = explain_text<false>(ctx,
        "SELECT bioseq.to_text(s[0:8]) "
        "FROM source_lifetime_source_security ORDER BY id;");
    REQUIRE_FALSE(plan_contains(rls_plan, "BioseqSliceProject"));
    exec_sql(ctx, "SAVEPOINT source_lifetime_rls_mmsearch;");
    const auto rls_error = sql_error(ctx, fmt::format(
        "SELECT id FROM source_lifetime_source_security WHERE {};",
        mmsearch_call));
    REQUIRE(rls_error.sqlstate == "0A000");
    exec_sql(ctx,
        "ROLLBACK TO SAVEPOINT source_lifetime_rls_mmsearch;"
        "RELEASE SAVEPOINT source_lifetime_rls_mmsearch;"
        "RESET ROLE; ROLLBACK;");
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}
