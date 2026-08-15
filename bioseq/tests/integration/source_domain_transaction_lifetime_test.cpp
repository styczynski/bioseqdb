/**
 * @file source_domain_transaction_lifetime_test.cpp
 * @brief Exercises cancellation, transactions, transformed ownership, and error cleanup.
 * @note CMake compiles this source only when BIOSEQ_TEST_INDEXES is enabled.
 */

#include <tests/support/postgres.hpp>
#include <bioseq/pg_build_profile.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <libpq-fe.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import bioseq.tests.integration.support;
import bioseq.tests.integration.source_domain_lifetime_test_support;

using namespace bioseq_int_tests;
using namespace bioseq_int_tests::source_domain_lifetime_test_support;

TEST_CASE("LIMIT Portal cancellation and xact release paths quiesce exactly",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    constexpr std::string_view table = "source_lifetime_source_control_flow";
    auto setup = fresh_context({table});
    exec_sql(setup,
        "CREATE TABLE source_lifetime_source_control_flow "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;"
        "INSERT INTO source_lifetime_source_control_flow "
        "SELECT g, repeat('ACGT',2048)::bioseq.seq('dna4',32) "
        "FROM generate_series(1,64) AS g;");

    reset_source_counters(setup);
    const auto limited = query_rows(setup,
        "SELECT id, bioseq.to_text(s[101:109]) "
        "FROM source_lifetime_source_control_flow ORDER BY id LIMIT 2;");
    REQUIRE(limited.size() == 2);
    REQUIRE(source_counter(setup, "domains_created") == 1);
    REQUIRE(source_counter(setup, "cleanup_commit") == 1);
    REQUIRE(source_counter(setup, "release_before_locks") == 1);
    REQUIRE(source_counter(setup, "release_locks") == 1);
    REQUIRE(source_counter(setup, "release_after_locks") == 1);
    require_source_resources_balanced(setup);

    reset_source_counters(setup);
    exec_sql(setup,
        "BEGIN;"
        "DECLARE source_lifetime_source_portal NO SCROLL CURSOR FOR "
        " SELECT id, bioseq.to_text(s[0:8]) "
        " FROM source_lifetime_source_control_flow ORDER BY id;");
    REQUIRE(query_rows(setup,
        "FETCH 3 FROM source_lifetime_source_portal;").size() == 3);
    REQUIRE(source_counter(setup, "live_registries") == 1);
    REQUIRE(source_counter(setup, "live_providers") == 1);
    exec_sql(setup, "CLOSE source_lifetime_source_portal; COMMIT;");
    REQUIRE(source_counter(setup, "release_before_locks")
            == source_counter(setup, "domains_created"));
    REQUIRE(source_counter(setup, "release_locks")
            == source_counter(setup, "domains_created"));
    REQUIRE(source_counter(setup, "release_after_locks")
            == source_counter(setup, "domains_created"));
    require_source_resources_balanced(setup);

    reset_source_counters(setup);
    exec_sql(setup,
        "BEGIN;"
        "DECLARE source_lifetime_source_abort_portal NO SCROLL CURSOR FOR "
        " SELECT id, bioseq.to_text(s[0:8]) "
        " FROM source_lifetime_source_control_flow ORDER BY id;");
    REQUIRE(query_rows(setup,
        "FETCH 1 FROM source_lifetime_source_abort_portal;").size() == 1);
    REQUIRE(source_counter(setup, "live_registries") == 1);
    exec_sql(setup, "ROLLBACK;");
    REQUIRE(source_counter(setup, "cleanup_abort")
            == source_counter(setup, "domains_created"));
    REQUIRE(source_counter(setup, "release_before_locks")
            == source_counter(setup, "domains_created"));
    REQUIRE(source_counter(setup, "release_locks")
            == source_counter(setup, "domains_created"));
    REQUIRE(source_counter(setup, "release_after_locks")
            == source_counter(setup, "domains_created"));
    require_source_resources_balanced(setup);

    connection_test_context worker;
    connection_test_context controller;
    exec_sql(worker,
        "CREATE FUNCTION pg_temp.source_lifetime_hold_source(value bioseq.seq) "
        "RETURNS bigint LANGUAGE plpgsql VOLATILE STRICT AS $$ "
        "DECLARE held bioseq.seq; BEGIN "
        " held := value; "
        " PERFORM pg_sleep(0.25); RETURN bioseq.len(held); END $$;");
    constexpr std::string_view cancel_sql =
        "/* source_lifetime source cancel */ "
        "SELECT pg_temp.source_lifetime_hold_source(("
        " SELECT t.s[0:64] "
        " FROM source_lifetime_source_control_flow AS t "
        " WHERE t.id=1 LIMIT 1"
        "));";
    const auto cancel_plan = explain_text<false>(worker, cancel_sql);
    REQUIRE(plan_contains(cancel_plan, "Custom Scan (BioseqSliceProject)"));
    reset_source_counters(worker);
    const auto cancelled = cancel_sleeping_query(
        worker, controller, cancel_sql);
    REQUIRE(cancelled.sent);
    REQUIRE(cancelled.wait_observed);
    REQUIRE(cancelled.cancel_requested);
    REQUIRE(cancelled.drained);
    REQUIRE(cancelled.sqlstate == "57014");
    REQUIRE(source_counter(worker, "domains_created") == 1);
    REQUIRE(source_counter(worker, "cleanup_abort")
            == source_counter(worker, "domains_created"));
    REQUIRE(source_counter(worker, "release_before_locks")
            == source_counter(worker, "domains_created"));
    REQUIRE(source_counter(worker, "release_locks")
            == source_counter(worker, "domains_created"));
    REQUIRE(source_counter(worker, "release_after_locks")
            == source_counter(worker, "domains_created"));
    REQUIRE(source_counter(worker, "wrong_thread_rejections") == 0);
    require_source_resources_balanced(worker);
    REQUIRE(query_scalar_int(worker, "SELECT 1;") == 1);
}

TEST_CASE("retained source control fails closed after transaction end",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    auto ctx = fresh_context();
    reset_source_counters(ctx);

    /* The existing native probe retains a real source-domain control across
     * the production PRE_COMMIT cleanup callback, then accepts success only
     * after the production read guard reports SQLSTATE 55000 and the exact
     * transaction-ended diagnostic. */
    REQUIRE(query_scalar(ctx,
        "SELECT bioseq_internal.sequence_source_fault_contract_probe();")
        == "t");
    REQUIRE(source_counter(ctx, "refcount_faults") == 1);
    REQUIRE(source_counter(ctx, "lease_faults") == 1);
    REQUIRE(source_counter(ctx, "registries_created") == 1);
    REQUIRE(source_counter(ctx, "registries_destroyed") == 1);
    REQUIRE(source_counter(ctx, "domains_created") == 1);
    REQUIRE(source_counter(ctx, "domains_destroyed") == 1);
    REQUIRE(source_counter(ctx, "cleanup_commit") == 1);
    REQUIRE(source_counter(ctx, "cleanup_abort") == 0);
    REQUIRE(source_counter(ctx, "release_before_locks") == 1);
    REQUIRE(source_counter(ctx, "release_locks") == 1);
    REQUIRE(source_counter(ctx, "release_after_locks") == 1);
    REQUIRE(source_counter(ctx, "wrong_thread_rejections") == 0);
    require_source_resources_balanced(ctx);
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE("nested BatchJoin SliceProject shares and releases source routes",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    constexpr std::string_view target = "source_lifetime_source_batch_target";
    constexpr std::string_view query = "source_lifetime_source_batch_query";
    auto ctx = fresh_context({target, query});
    exec_sql(ctx,
        "SET bioseq.index.insert_maintenance.enabled=off;"
        "SET bioseq.index.sequence_read.enabled=off;"
        "SET bioseq.index.async_persistence.enabled=off;"
        "SET bioseq.index.recompute_async_persistence.enabled=off;"
        "SET bioseq.vectorization.enabled=off;"
        "SET bioseq.aligner.minimizer.algorithm='scalar';"
        "SET bioseq.aligner.chaining.algorithm='scalar_minimap2';"
        "SET bioseq.aligner.base_alignment.algorithm='ksw2';"
        "SET bioseq.aligner.vulkan.enabled=off;"
        "CREATE TABLE source_lifetime_source_batch_target "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;"
        "CREATE TABLE source_lifetime_source_batch_query "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;");

    const std::string target_sequence =
        source_domain_high_entropy_dna4(16384u);
    const std::string query_sequence = target_sequence.substr(4096u, 4096u);
    exec_sqlf(ctx,
        "INSERT INTO source_lifetime_source_batch_target VALUES "
        " (1, '{}'::bioseq.seq('dna4',32));"
        "INSERT INTO source_lifetime_source_batch_query VALUES "
        " (1, '{}'::bioseq.seq('dna4',32));",
        target_sequence, query_sequence);

    REQUIRE(query_scalar_int(ctx,
        "SELECT count(*) "
        "FROM source_lifetime_source_batch_target AS t "
        "JOIN source_lifetime_source_batch_query AS q "
        "ON mmsearch(t.s, q.s, "
        " aligner_options(index_read_requirement => 1));") == 1);
    require_source_resources_balanced(ctx);

    constexpr auto sql =
        "SELECT bioseq.len((t.s[0:16] OPERATOR(bioseq.||) q.s[0:16])[4:28]) "
        "FROM source_lifetime_source_batch_target AS t "
        "JOIN source_lifetime_source_batch_query AS q "
        "ON mmsearch(t.s[0:], q.s[0:], "
        " aligner_options(index_read_requirement => 1)) "
        "LIMIT 1;";
    const auto plan = explain_text<false>(ctx, sql);
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqBatchJoin)"));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));

    reset_source_counters(ctx);
    REQUIRE(query_scalar_int(ctx, sql) == 24);
    REQUIRE(source_counter(ctx, "registries_created") >= 2);
    REQUIRE(source_counter(ctx, "domains_created") >= 2);
    REQUIRE(source_counter(ctx, "leases_acquired") >= 2);
    REQUIRE(source_counter(ctx, "provider_closed_with_upper_leases") >= 1);
    REQUIRE(source_counter(ctx, "wrong_thread_rejections") == 0);
    require_source_resources_balanced(ctx);
}

TEST_CASE("materialized spill aggregate COPY generated and parallel routes are safe",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    constexpr std::string_view source = "source_lifetime_source_boundaries";
    constexpr std::string_view generated = "source_lifetime_source_generated";
    auto ctx = fresh_context({generated, source});
    exec_sql(ctx,
        "SET work_mem='64kB';"
        "CREATE TABLE source_lifetime_source_boundaries "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;"
        "INSERT INTO source_lifetime_source_boundaries "
        "SELECT g, repeat('ACGT',2048)::bioseq.seq('dna4',32) "
        "FROM generate_series(1,96) AS g;");

    reset_source_counters(ctx);
    REQUIRE(query_scalar_int(ctx,
        "SELECT sum(bioseq.len(s[101:133])) "
        "FROM source_lifetime_source_boundaries;") == 3072);
    require_source_resources_balanced(ctx);

    reset_source_counters(ctx);
    constexpr auto spill_sql =
        "WITH fragments AS MATERIALIZED ("
        " SELECT id, s[0:4096] AS fragment "
        " FROM source_lifetime_source_boundaries"
        ") SELECT sum(bioseq.len(fragment)) FROM fragments;";
    const auto spill_plan = explain_text<true>(ctx, spill_sql);
    REQUIRE((plan_contains(spill_plan, "temp written=")
             || plan_contains(spill_plan, "temp read=")));
    require_source_resources_balanced(ctx);

    reset_source_counters(ctx);
    REQUIRE(query_scalar_int(ctx, spill_sql) == 393216);
    REQUIRE(source_counter(ctx, "domains_created") == 1);
    require_source_resources_balanced(ctx);

    constexpr auto ordered_aggregate_sql =
        "SELECT sum(bioseq.len(fragment)) "
        "FROM ("
        " SELECT unnest(array_agg(s[0:128] ORDER BY id)) AS fragment "
        " FROM source_lifetime_source_boundaries"
        ") AS aggregate_state;";
    const auto ordered_aggregate_plan =
        explain_text<false>(ctx, ordered_aggregate_sql);
    REQUIRE(plan_line_count(ordered_aggregate_plan,
        "Custom Scan (BioseqSliceProject)") == 1u);
    const auto ordered_owner = plan_line_index(
        ordered_aggregate_plan, "Custom Scan (BioseqSliceProject)");
    const auto ordered_sort = plan_line_index(
        ordered_aggregate_plan, "Sort");
    REQUIRE(ordered_owner.has_value());
    REQUIRE(ordered_sort.has_value());
    REQUIRE(*ordered_owner < *ordered_sort);
    REQUIRE(plan_contains(ordered_aggregate_plan,
        "Sort Key: source_lifetime_source_boundaries.id"));
    REQUIRE_FALSE(plan_contains(
        ordered_aggregate_plan, "Custom Scan (VectorAggNode)"));

    reset_source_counters(ctx);
    REQUIRE(query_scalar_int(ctx, ordered_aggregate_sql) == 12288);
    REQUIRE(source_counter(ctx, "domains_created") == 1);
    require_source_resources_balanced(ctx);

    reset_source_counters(ctx);
    const auto copied = run_copy_out(ctx,
        "COPY (SELECT id, bioseq.to_text(s[0:8]) "
        "      FROM source_lifetime_source_boundaries "
        "      WHERE id<=3 ORDER BY id) TO STDOUT");
    REQUIRE(copied.started);
    REQUIRE(copied.stream_ok);
    REQUIRE(copied.command_ok);
    REQUIRE(copied.bytes ==
        "1\tACGTACGT\n2\tACGTACGT\n3\tACGTACGT\n");
    REQUIRE(source_counter(ctx, "domains_created") == 1);
    require_source_resources_balanced(ctx);

    reset_source_counters(ctx);
    const auto copied_binary = run_copy_out(ctx,
        "COPY (SELECT id, s[0:64] "
        "      FROM source_lifetime_source_boundaries "
        "      WHERE id<=3 ORDER BY id) TO STDOUT (FORMAT binary)");
    REQUIRE(copied_binary.started);
    REQUIRE(copied_binary.stream_ok);
    REQUIRE(copied_binary.command_ok);
    REQUIRE(copied_binary.bytes.size() > 21);
    REQUIRE(copied_binary.bytes.compare(
        0, 11, std::string("PGCOPY\n\xff\r\n\0", 11)) == 0);
    REQUIRE(copied_binary.bytes.compare(
        copied_binary.bytes.size() - 2, 2,
        std::string("\xff\xff", 2)) == 0);
    REQUIRE(source_counter(ctx, "domains_created") == 1);
    require_source_resources_balanced(ctx);

    reset_source_counters(ctx);
    exec_sql(ctx,
        "CREATE TABLE source_lifetime_source_generated "
        " (id int, s bioseq.seq('dna4',32), "
        "  prefix text GENERATED ALWAYS AS "
        "    (bioseq.to_text(s[0:8])) STORED) USING bioseq;"
        "INSERT INTO source_lifetime_source_generated(id,s) "
        "SELECT id, s[0:] FROM source_lifetime_source_boundaries WHERE id<=2;");
    REQUIRE(query_scalar(ctx,
        "SELECT string_agg(prefix,',' ORDER BY id) "
        "FROM source_lifetime_source_generated;") == "ACGTACGT,ACGTACGT");
    REQUIRE(source_counter(ctx, "domains_created") >= 1);
    require_source_resources_balanced(ctx);

    reset_source_counters(ctx);
    exec_sql(ctx,
        "CREATE MATERIALIZED VIEW source_lifetime_source_mv AS "
        "SELECT id, s[0:128]::bioseq.seq('dna4',32) AS fragment "
        "FROM source_lifetime_source_boundaries WHERE id<=4;");
    REQUIRE(query_scalar_int(ctx,
        "SELECT sum(bioseq.len(fragment)) FROM source_lifetime_source_mv;") == 512);
    REQUIRE(source_counter(ctx, "domains_created") >= 1);
    require_source_resources_balanced(ctx);

    const long long max_workers = query_scalar_int(ctx,
        "SELECT current_setting('max_parallel_workers')::int;");
    if (max_workers > 0) {
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
        constexpr auto parallel_sql =
            "SELECT sum(bioseq.len(s[0:64])) "
            "FROM source_lifetime_source_boundaries;";
        const auto parallel_plan = explain_text<false>(ctx, parallel_sql);
        REQUIRE(plan_contains(parallel_plan, "Gather"));

        /* Leader participation is disabled, so these actual counters can
         * reach the leader only through SliceProject's DSM worker slots.  A
         * full 96-call aggregate is process-shared evidence that the workers
         * collectively executed every source-backed slice and conversion. */
        constexpr auto worker_materialization_sql =
            "SELECT bioseq.to_text(s[0:64]) "
            "FROM source_lifetime_source_boundaries;";
        reset_source_counters(ctx);
        const auto worker_plan = explain_text<true>(
            ctx, worker_materialization_sql);
        REQUIRE(plan_contains(worker_plan, "Gather"));
        REQUIRE(plan_contains(
            worker_plan, "Custom Scan (BioseqSliceProject)"));
        REQUIRE(plan_line_index(worker_plan, "Gather")
                < plan_line_index(
                    worker_plan, "Custom Scan (BioseqSliceProject)"));
        REQUIRE(source_boundary_actual_calls(
            worker_plan, "text", "SQL conversion") == 96u);
        REQUIRE_FALSE(plan_contains(worker_plan, "Incomplete Workers"));
        REQUIRE_FALSE(plan_contains(worker_plan, "Counter Overflow: true"));
        REQUIRE(source_counter(ctx, "wrong_thread_rejections") == 0);
        require_source_resources_balanced(ctx);

        reset_source_counters(ctx);
        REQUIRE(query_scalar_int(ctx, parallel_sql) == 6144);
        REQUIRE(source_counter(ctx, "wrong_thread_rejections") == 0);
        require_source_resources_balanced(ctx);
    }
}

TEST_CASE("ordinary CTAS owns transformed source values",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    constexpr std::string_view source = "source_lifetime_source_ctas_input";
    constexpr std::string_view destination = "source_lifetime_source_ctas_output";
    auto ctx = fresh_context({destination, source});
    exec_sql(ctx,
        "SET max_parallel_workers_per_gather=0;"
        "SET bioseq.index.insert_maintenance.enabled=off;"
        "CREATE TABLE source_lifetime_source_ctas_input "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;"
        "INSERT INTO source_lifetime_source_ctas_input VALUES "
        " (7, repeat('ACGT',4096)::bioseq.seq('dna4',32));");

    constexpr auto ctas_sql =
        "CREATE TABLE source_lifetime_source_ctas_output USING heap AS "
        "SELECT id, "
        " (s[4093:4101] OPERATOR(bioseq.||) "
        "  bioseq.reverse(s[8190:8194]))::bioseq.seq('dna4',32) "
        "  AS fragment "
        "FROM source_lifetime_source_ctas_input WHERE id=7;";
    reset_source_counters(ctx);
    exec_sql(ctx,
        "SELECT bioseq_internal.slice_payload_allocation_probe(0);");
    const auto ctas_plan = explain_text<true>(ctx, ctas_sql);
    REQUIRE(plan_contains(
        ctas_plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(plan_contains(ctas_plan, "Slice Materialization:"));
    /* CTAS is planned as CMD_SELECT, so its declared result-datum boundaries
     * are the two client serialization formats.  The CTAS receiver invokes
     * neither; core heap ownership performs exactly one packed-full executor
     * copy instead.  Keep both halves exact so a client-output detour or a
     * second payload copy cannot masquerade as durable ownership. */
    REQUIRE(plan_line_count(ctas_plan, "Mechanism: text") == 1u);
    REQUIRE(plan_line_count(ctas_plan, "Mechanism: binary") == 1u);
    REQUIRE(plan_line_count(ctas_plan, "Reason: client output") == 2u);
    REQUIRE(source_boundary_actual_calls(
        ctas_plan, "text", "client output") == 0u);
    REQUIRE(source_boundary_actual_calls(
        ctas_plan, "binary", "client output") == 0u);
    REQUIRE(query_scalar_int(ctx,
        "SELECT bioseq_internal.slice_payload_allocation_probe(1);") == 1);
    REQUIRE(query_scalar_int(ctx,
        "SELECT bioseq_internal.slice_payload_allocation_probe(2);") == 1);
    REQUIRE(query_scalar_int(ctx,
        "SELECT bioseq_internal.slice_payload_allocation_probe(3);") == 0);
    REQUIRE(query_scalar_int(ctx,
        "SELECT bioseq_internal.slice_payload_allocation_probe(4);") == 0);
    REQUIRE(source_counter(ctx, "domains_created") >= 1);
    REQUIRE(source_counter(ctx, "wrong_thread_rejections") == 0);
    require_source_resources_balanced(ctx);

    /* The durable heap tuple must no longer depend on the provider, relation,
     * statement registry, or snapshot that produced the lazy expression. */
    exec_sql(ctx, "DROP TABLE source_lifetime_source_ctas_input;");
    reset_source_counters(ctx);
    const auto durable = query_one(ctx,
        "SELECT bioseq.len(fragment)::text, "
        "       bioseq.to_text(fragment), "
        "       encode(bioseq.seq_send(fragment),'hex'), "
        "       encode(bioseq.seq_send("
        "         'CGTACGTACATG'::bioseq.seq('dna4',32)),'hex') "
        "FROM source_lifetime_source_ctas_output WHERE id=7;");
    REQUIRE(durable.size() == 4u);
    REQUIRE(durable[0] == "12");
    REQUIRE(durable[1] == "CGTACGTACATG");
    REQUIRE(durable[2].has_value());
    REQUIRE(durable[2] == durable[3]);
    REQUIRE(source_counter(ctx, "domains_created") == 0);
    REQUIRE(source_counter(ctx, "wrong_thread_rejections") == 0);
    require_source_resources_balanced(ctx);
}

TEST_CASE("caught late binding errors release sliced source ownership",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    constexpr std::string_view table = "source_lifetime_source_error_atomic";
    auto ctx = fresh_context({table});
    exec_sql(ctx,
        "SET max_parallel_workers_per_gather=0;"
        "SET bioseq.index.insert_maintenance.enabled=off;"
        "CREATE TABLE source_lifetime_source_error_atomic "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;"
        "INSERT INTO source_lifetime_source_error_atomic VALUES "
        " (1, repeat('ACGT',4096)::bioseq.seq('dna4',32));"
        "CREATE FUNCTION pg_temp.source_lifetime_source_runtime_text(value text) "
        "RETURNS text LANGUAGE plpgsql VOLATILE STRICT AS $$ "
        "BEGIN RETURN value; END $$;");

    /* Expression evaluation opens the source-backed left edge before the
     * volatile RHS conversion.  The level-0 public profile's runtime input
     * parser rejects N while constructing the intermediate unbound sequence.
     * Richer profiles admit DNA5, then resolve_concat_type rejects the bound
     * DNA4/DNA5 pair before literal-ledger construction.  The exact
     * source-domain counts below confirm that neither error happened before the
     * provider-backed left edge was opened. */
#if !defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
    constexpr std::string_view late_rhs =
        "pg_temp.source_lifetime_source_runtime_text('N')"
        "::bioseq.seq('dna4',32)";
    constexpr std::string_view expected_sqlstate = "22P02";
    constexpr std::string_view expected_message =
        "invalid symbol at byte 0 in unbound bioseq.seq";
    constexpr std::string_view expected_condition =
        "invalid_text_representation";
#else
    constexpr std::string_view late_rhs =
        "pg_temp.source_lifetime_source_runtime_text('N')"
        "::bioseq.seq('dna5',32)";
    constexpr std::string_view expected_sqlstate = "42804";
    constexpr std::string_view expected_message =
        "cannot concatenate bioseq alphabets dna4 and dna5";
    constexpr std::string_view expected_condition = "datatype_mismatch";
#endif
    const auto late_error_query = fmt::format(
        "SELECT sliced OPERATOR(bioseq.||) "
        "       {} "
        "FROM ("
        " SELECT s[17:49] AS sliced "
        " FROM source_lifetime_source_error_atomic WHERE id=1 OFFSET 0"
        ") AS source_row;",
        late_rhs);
    const auto planned = explain_text<false>(ctx, late_error_query);
    REQUIRE(plan_contains(
        planned, "Custom Scan (BioseqSliceProject)"));

    reset_source_counters(ctx);
    const auto preflight = sql_error(ctx, late_error_query);
    REQUIRE(preflight.sqlstate == expected_sqlstate);
    REQUIRE(preflight.contains(expected_message));
    REQUIRE(source_counter(ctx, "domains_created") == 1);
    REQUIRE(source_counter(ctx, "domains_destroyed") == 1);
    require_source_resources_balanced(ctx);

    const auto admission_baseline = query_scalar_int(ctx,
        "SELECT index_memory_reserved_bytes "
        "FROM bioseq.scheduler_stats();");
    reset_source_counters(ctx);
    exec_sql(ctx, fmt::format(
        "DO $source_lifetime$ "
        "DECLARE iteration integer; "
        "BEGIN "
        "  FOR iteration IN 1..32 LOOP "
        "    BEGIN "
        "      PERFORM sliced OPERATOR(bioseq.||) "
        "              {} "
        "      FROM ("
        "       SELECT s[17:49] AS sliced "
        "       FROM source_lifetime_source_error_atomic WHERE id=1 OFFSET 0"
        "      ) AS source_row; "
        "      RAISE EXCEPTION 'expected late sequence binding error'; "
        "    EXCEPTION WHEN {} THEN "
        "      NULL; "
        "    END; "
        "  END LOOP; "
        "END $source_lifetime$;",
        late_rhs, expected_condition));

    REQUIRE(source_counter(ctx, "domains_created") == 32);
    REQUIRE(source_counter(ctx, "domains_destroyed") == 32);
    REQUIRE(source_counter(ctx, "refcount_faults") == 0);
    REQUIRE(source_counter(ctx, "lease_faults") == 0);
    REQUIRE(source_counter(ctx, "wrong_thread_rejections") == 0);
    require_source_resources_balanced(ctx);
    REQUIRE(query_scalar_int(ctx,
        "SELECT index_memory_reserved_bytes "
        "FROM bioseq.scheduler_stats();") == admission_baseline);
}
