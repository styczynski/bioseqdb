/** @file Shared source-domain ownership and lifetime integration coverage. */

#include <tests/support/postgres.hpp>

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

#ifdef BIOSEQ_TEST_INDEXES
TEST_CASE("thousands of locator rows share one binding domain",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    auto ctx = fresh_context({"source_lifetime_source_many"});
    exec_sql(ctx,
        "CREATE TABLE source_lifetime_source_many "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;"
        "INSERT INTO source_lifetime_source_many "
        "SELECT g, repeat('ACGT',1024)::bioseq.seq('dna4',32) "
        "FROM generate_series(1,1500) AS g;");

    constexpr auto aggregate_sql =
        "SELECT sum(bioseq.len(s[101:133])) "
        "FROM source_lifetime_source_many;";
    const auto aggregate_plan = explain_text<false>(ctx, aggregate_sql);
    REQUIRE(plan_contains(
        aggregate_plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE_FALSE(plan_contains(
        aggregate_plan, "Custom Scan (VectorAggNode)"));

    reset_source_counters(ctx);
    REQUIRE(query_scalar_int(ctx, aggregate_sql) == 48000);

    REQUIRE(source_counter(ctx, "registries_created") == 1);
    REQUIRE(source_counter(ctx, "scan_providers_created") == 1);
    REQUIRE(source_counter(ctx, "domains_created") == 1);
    REQUIRE(source_counter(ctx, "domains_reused") >= 1499);
    REQUIRE(source_counter(ctx, "cache_sets_created") == 1);
    REQUIRE(source_counter(ctx, "snapshots_registered") == 1);
    REQUIRE(source_counter(ctx, "cache_sets_destroyed") == 1);
    REQUIRE(source_counter(ctx, "snapshots_unregistered") == 1);
    REQUIRE(source_counter(ctx, "live_registries") == 0);
    REQUIRE(source_counter(ctx, "live_domains") == 0);
    REQUIRE(source_counter(ctx, "live_cache_sets") == 0);
    REQUIRE(source_counter(ctx, "live_snapshots") == 0);
}

TEST_CASE("BatchSort retains exact source routes without flattening tuples",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    auto ctx = fresh_context({"source_lifetime_source_batch_sort"});
    exec_sql(ctx,
        "CREATE TABLE source_lifetime_source_batch_sort "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;"
        "INSERT INTO source_lifetime_source_batch_sort VALUES "
        " (1, repeat('T',4096)::bioseq.seq('dna4',32)),"
        " (2, repeat('A',4096)::bioseq.seq('dna4',32)),"
        " (3, repeat('AC',2048)::bioseq.seq('dna4',32)),"
        " (4, NULL);"
        "SET bioseq.planner.sequence_sort_scan.enabled = on;"
        "SET bioseq.aligner.vulkan.enabled = off;");

    const auto plan = explain_text<false>(ctx,
        "SELECT id FROM source_lifetime_source_batch_sort "
        "ORDER BY s NULLS LAST;");
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqBatchSortScan)"));

    reset_source_counters(ctx);
    const auto ids = query_int_column(ctx,
        "SELECT id FROM source_lifetime_source_batch_sort "
        "ORDER BY s NULLS LAST;");
    REQUIRE(ids == std::vector<long long>{2, 3, 1, 4});
    REQUIRE(source_counter(ctx, "registries_created") == 1);
    REQUIRE(source_counter(ctx, "providers_created") == 1);
    REQUIRE(source_counter(ctx, "scan_providers_created") == 1);
    REQUIRE(source_counter(ctx, "domains_created") == 1);
    REQUIRE(source_counter(ctx, "domains_reused") == 2);
    REQUIRE(source_counter(ctx, "cache_sets_created") == 1);
    REQUIRE(source_counter(ctx, "snapshots_registered") == 1);
    REQUIRE(source_counter(ctx, "scanner_entries_created") == 3);
    REQUIRE(source_counter(ctx, "leases_acquired")
            == source_counter(ctx, "leases_released"));
    REQUIRE(source_counter(ctx, "provider_closed_with_upper_leases") >= 1);
    REQUIRE(source_counter(ctx, "last_upper_lease_after_provider_end") >= 1);
    require_source_resources_balanced(ctx);
}

TEST_CASE("repeated source leaves reuse one callback scanner entry",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    auto ctx = fresh_context({"source_lifetime_source_repeat"});
    exec_sql(ctx,
        "CREATE TABLE source_lifetime_source_repeat "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;"
        "INSERT INTO source_lifetime_source_repeat VALUES "
        " (1, repeat('ACGT',4096)::bioseq.seq('dna4',32));");

    reset_source_counters(ctx);
    REQUIRE(query_scalar(ctx,
        "SELECT bioseq.to_text("
        " s[0:16] OPERATOR(bioseq.||) bioseq.reverse(s[0:16]) "
        " OPERATOR(bioseq.||) s[0:16]) "
        "FROM source_lifetime_source_repeat;")
        == "ACGTACGTACGTACGTTGCATGCATGCATGCAACGTACGTACGTACGT");
    REQUIRE(source_counter(ctx, "domains_created") == 1);
    REQUIRE(source_counter(ctx, "scanner_entries_created") == 1);
    REQUIRE(source_counter(ctx, "scanner_entry_hits") >= 2);
}

TEST_CASE("partial final fragment is reusable through one callback scanner",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    auto ctx = fresh_context({"source_lifetime_partial_fragment_repeat"});
    const std::string sequence = source_domain_high_entropy_dna4(32'792u);
    exec_sql(ctx, fmt::format(
        "CREATE TABLE source_lifetime_partial_fragment_repeat "
        " (id int, s bioseq.seq('dna4',64)) USING bioseq;"
        "INSERT INTO source_lifetime_partial_fragment_repeat VALUES "
        " (1, '{}'::bioseq.seq('dna4',64));",
        sequence));

    reset_source_counters(ctx);
    const auto result = query_scalar(ctx,
        "SELECT bioseq.to_text("
        " s[32768:32792] OPERATOR(bioseq.||) "
        " bioseq.reverse(s[32768:32792]) OPERATOR(bioseq.||) "
        " s[32768:32792]) "
        "FROM source_lifetime_partial_fragment_repeat;");
    const std::string tail = sequence.substr(32'768u);
    const std::string expected =
        tail + std::string(tail.rbegin(), tail.rend()) + tail;
    REQUIRE(result == expected);
    REQUIRE(source_counter(ctx, "scanner_entries_created") == 1);
    REQUIRE(source_counter(ctx, "scanner_entry_hits") >= 2);
}

TEST_CASE("cached readers survive serial per-tuple resets",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    constexpr std::string_view table = "source_lifetime_source_reader_context";
    auto ctx = fresh_context({table});
    exec_sql(ctx,
        "SET max_parallel_workers_per_gather=0;"
        "SET bioseq.index.insert_maintenance.enabled=off;"
        "CREATE TABLE source_lifetime_source_reader_context "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;"
        "INSERT INTO source_lifetime_source_reader_context "
        "SELECT g, repeat('ACGT',2048)::bioseq.seq('dna4',32) "
        "FROM generate_series(1,8) AS g;");

    constexpr auto sql =
        "SELECT bioseq.to_text(s[0:64]) "
        "FROM source_lifetime_source_reader_context;";
    const auto planned = explain_text<false>(ctx, sql);
    REQUIRE(plan_contains(planned, "Custom Scan (BioseqSliceProject)"));
    REQUIRE_FALSE(plan_contains(planned, "Gather"));

    rowset_t expected;
    expected.reserve(8u);
    for (int row = 0; row < 8; ++row) {
        expected.push_back({
            std::optional<std::string>{
                "ACGTACGTACGTACGTACGTACGTACGTACGT"
                "ACGTACGTACGTACGTACGTACGTACGTACGT"},
        });
    }
    for (int execution = 0; execution < 2; ++execution) {
        reset_source_counters(ctx);
        REQUIRE(query_rows(ctx, sql) == expected);
        REQUIRE(source_counter(ctx, "domains_created") == 1);
        REQUIRE(source_counter(ctx, "wrong_thread_rejections") == 0);
        require_source_resources_balanced(ctx);
    }
}

TEST_CASE("SliceProject keeps one lowest owner across ordering and Gather",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    constexpr std::string_view table = "source_lifetime_slice_ordered_owner";
    constexpr std::string_view owner =
        "Custom Scan (BioseqSliceProject)";
    auto ctx = fresh_context({table});
    REQUIRE(query_scalar_int(ctx,
        "SELECT current_setting('max_parallel_workers')::int;") > 0);
    exec_sql(ctx,
        "SET bioseq.index.insert_maintenance.enabled=off;"
        "SET max_parallel_workers_per_gather=0;"
        "CREATE TABLE source_lifetime_slice_ordered_owner ("
        " pk int, s bioseq.seq('dna4',32)"
        ") USING bioseq WITH (autovacuum_enabled=false);"
        "INSERT INTO source_lifetime_slice_ordered_owner "
        "SELECT g, repeat('A',4161-g)::bioseq.seq('dna4',32) "
        "FROM generate_series(64,1,-1) AS g;");

    std::vector<long long> lengths_by_pk;
    rowset_t length_pk_rows;
    lengths_by_pk.reserve(64u);
    length_pk_rows.reserve(64u);
    for (long long pk = 1; pk <= 64; ++pk) {
        const long long length = 4161 - pk;
        lengths_by_pk.push_back(length);
        length_pk_rows.push_back({
            std::optional<std::string>{std::to_string(length)},
            std::optional<std::string>{std::to_string(pk)},
        });
    }
    auto lengths_multiset = lengths_by_pk;
    std::sort(lengths_multiset.begin(), lengths_multiset.end());

    const auto require_single_owner = [owner](const auto& plan) {
        REQUIRE(plan_line_count(plan, owner) == 1u);
        const auto index = plan_line_index(plan, owner);
        REQUIRE(index.has_value());
        return *index;
    };
    const auto require_exact_multiset = [&ctx, &lengths_multiset](
            std::string_view sql) {
        auto actual = query_int_column(ctx, sql);
        std::sort(actual.begin(), actual.end());
        REQUIRE(actual == lengths_multiset);
    };

    constexpr auto unordered_sql =
        "SELECT bioseq.len(s) FROM source_lifetime_slice_ordered_owner;";
    const auto unordered_plan = explain_text<false>(ctx, unordered_sql);
    (void)require_single_owner(unordered_plan);
    REQUIRE_FALSE(plan_contains(unordered_plan, "Sort"));
    REQUIRE_FALSE(plan_contains(unordered_plan, "Gather"));
    require_exact_multiset(unordered_sql);

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
    const auto parallel_plan = explain_text<false>(ctx, unordered_sql);
    const std::size_t parallel_owner = require_single_owner(parallel_plan);
    const auto gather = plan_line_index(parallel_plan, "Gather");
    REQUIRE(gather.has_value());
    REQUIRE(*gather < parallel_owner);
    REQUIRE_FALSE(plan_contains(parallel_plan, "Single Copy: true"));
    REQUIRE_FALSE(plan_contains(parallel_plan, "Sort"));
    require_exact_multiset(unordered_sql);

    exec_sql(ctx,
        "SET max_parallel_workers_per_gather=0;"
        "DO $$ BEGIN "
        " IF EXISTS (SELECT 1 FROM pg_settings "
        "            WHERE name='debug_parallel_query') THEN "
        "   PERFORM set_config('debug_parallel_query','off',false);"
        " ELSIF EXISTS (SELECT 1 FROM pg_settings "
        "               WHERE name='force_parallel_mode') THEN "
        "   PERFORM set_config('force_parallel_mode','off',false);"
        " END IF; END $$;");

    constexpr auto length_order_sql =
        "SELECT bioseq.len(s) FROM source_lifetime_slice_ordered_owner ORDER BY 1;";
    const auto length_order_plan = explain_text<false>(ctx, length_order_sql);
    const std::size_t length_order_owner =
        require_single_owner(length_order_plan);
    const auto length_sort = plan_line_index(length_order_plan, "Sort");
    REQUIRE(length_sort.has_value());
    REQUIRE(*length_sort < length_order_owner);
    REQUIRE_FALSE(plan_contains(length_order_plan, "Gather"));
    REQUIRE(query_int_column(ctx, length_order_sql) == lengths_multiset);

    constexpr auto length_pk_sql =
        "SELECT bioseq.len(s),pk FROM source_lifetime_slice_ordered_owner "
        "ORDER BY pk;";
    const auto length_pk_plan = explain_text<false>(ctx, length_pk_sql);
    const std::size_t length_pk_owner = require_single_owner(length_pk_plan);
    const auto length_pk_sort = plan_line_index(length_pk_plan, "Sort");
    REQUIRE(length_pk_sort.has_value());
    REQUIRE(*length_pk_sort < length_pk_owner);
    REQUIRE_FALSE(plan_contains(length_pk_plan, "Gather"));
    REQUIRE(query_rows(ctx, length_pk_sql) == length_pk_rows);

    constexpr auto resjunk_sql =
        "SELECT bioseq.len(s) FROM source_lifetime_slice_ordered_owner ORDER BY pk;";
    const auto resjunk_plan = explain_text<false>(ctx, resjunk_sql);
    const std::size_t resjunk_owner = require_single_owner(resjunk_plan);
    const auto resjunk_sort = plan_line_index(resjunk_plan, "Sort");
    REQUIRE(resjunk_sort.has_value());
    REQUIRE(*resjunk_sort < resjunk_owner);
    REQUIRE_FALSE(plan_contains(resjunk_plan, "Gather"));
    REQUIRE(query_int_column(ctx, resjunk_sql) == lengths_by_pk);
}

TEST_CASE("nested two-provider projection preserves upper leases",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    auto ctx = fresh_context(
        {"source_lifetime_source_left", "source_lifetime_source_right"});
    exec_sql(ctx,
        "CREATE TABLE source_lifetime_source_left "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;"
        "CREATE TABLE source_lifetime_source_right "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;"
        "INSERT INTO source_lifetime_source_left VALUES "
        " (1, repeat('ACGT',2048)::bioseq.seq('dna4',32));"
        "INSERT INTO source_lifetime_source_right VALUES "
        " (1, repeat('TGCA',2048)::bioseq.seq('dna4',32));");
    const auto plan = explain_text<false>(ctx,
        "SELECT (l.s[0:16] OPERATOR(bioseq.||) r.s[0:16])[2:30] "
        "FROM source_lifetime_source_left l "
        "JOIN source_lifetime_source_right r USING (id);");
    REQUIRE(plan_contains(plan, "BioseqSliceProject"));

    reset_source_counters(ctx);
    REQUIRE(query_scalar(ctx,
        "SELECT bioseq.to_text("
        " (l.s[0:16] OPERATOR(bioseq.||) r.s[0:16])[2:30]) "
        "FROM source_lifetime_source_left l "
        "JOIN source_lifetime_source_right r USING (id);")
        == "GTACGTACGTACGTTGCATGCATGCATG");
    REQUIRE(source_counter(ctx, "registries_created") == 2);
    REQUIRE(source_counter(ctx, "scan_providers_created") == 2);
    REQUIRE(source_counter(ctx, "domains_created") == 2);
    REQUIRE(source_counter(ctx, "cache_sets_created") == 2);
    REQUIRE(source_counter(ctx, "snapshots_registered") == 2);
    REQUIRE(source_counter(ctx, "leases_acquired") >= 4);
    REQUIRE(source_counter(ctx, "leases_acquired")
            == source_counter(ctx, "leases_released"));
    REQUIRE(source_counter(ctx, "provider_closed_with_upper_leases") >= 2);
    REQUIRE(source_counter(ctx, "last_upper_lease_after_provider_end") >= 2);
    REQUIRE(source_counter(ctx, "live_registries") == 0);
    REQUIRE(source_counter(ctx, "live_leases") == 0);
}

TEST_CASE("three-source concat retains arbitrary upper routes",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    auto ctx = fresh_context({
        "source_lifetime_source_three_left",
        "source_lifetime_source_three_middle",
        "source_lifetime_source_three_right",
    });
    exec_sql(ctx,
        "CREATE TABLE source_lifetime_source_three_left "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;"
        "CREATE TABLE source_lifetime_source_three_middle "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;"
        "CREATE TABLE source_lifetime_source_three_right "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;"
        "INSERT INTO source_lifetime_source_three_left VALUES "
        " (1, repeat('ACGT',2048)::bioseq.seq('dna4',32));"
        "INSERT INTO source_lifetime_source_three_middle VALUES "
        " (1, repeat('TGCA',2048)::bioseq.seq('dna4',32));"
        "INSERT INTO source_lifetime_source_three_right VALUES "
        " (1, repeat('GATC',2048)::bioseq.seq('dna4',32));");

    constexpr const char* three_source_expression =
        "((l.s[0:16] OPERATOR(bioseq.||) m.s[0:16]) "
        " OPERATOR(bioseq.||) r.s[0:16])";
    const auto plan = explain_text<false>(ctx, fmt::format(
        "SELECT {} FROM source_lifetime_source_three_left AS l "
        "JOIN source_lifetime_source_three_middle AS m USING (id) "
        "JOIN source_lifetime_source_three_right AS r USING (id);",
        three_source_expression));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(plan_line_count(plan, "Custom Scan (BioseqScan)") == 3u);

    reset_source_counters(ctx);
    REQUIRE(query_scalar(ctx, fmt::format(
        "SELECT bioseq.to_text({}) "
        "FROM source_lifetime_source_three_left AS l "
        "JOIN source_lifetime_source_three_middle AS m USING (id) "
        "JOIN source_lifetime_source_three_right AS r USING (id);",
        three_source_expression))
        == "ACGTACGTACGTACGTTGCATGCATGCATGCAGATCGATCGATCGATC");
    REQUIRE(source_counter(ctx, "registries_created") == 3);
    REQUIRE(source_counter(ctx, "scan_providers_created") == 3);
    REQUIRE(source_counter(ctx, "domains_created") == 3);
    REQUIRE(source_counter(ctx, "leases_acquired") >= 6);
    REQUIRE(source_counter(ctx, "provider_closed_with_upper_leases") >= 3);
    REQUIRE(source_counter(ctx, "last_upper_lease_after_provider_end") >= 3);
    require_source_resources_balanced(ctx);

    reset_source_counters(ctx);
    exec_sql(ctx,
        "SELECT bioseq_internal.arm_sequence_source_failure("
        " 'read_before_scanner');");
    const auto injected = sql_error(ctx, fmt::format(
        "SELECT bioseq.to_text({}) "
        "FROM source_lifetime_source_three_left AS l "
        "JOIN source_lifetime_source_three_middle AS m USING (id) "
        "JOIN source_lifetime_source_three_right AS r USING (id);",
        three_source_expression));
    REQUIRE(injected.sqlstate == "XX000");
    REQUIRE(injected.contains(
        "injected PostgreSQL sequence source-domain failure"));
    exec_sql(ctx,
        "SELECT bioseq_internal.disarm_sequence_source_failure();");
    REQUIRE(source_counter(ctx, "fault_injections") == 1);
    REQUIRE(source_counter(ctx, "read_fault_injections") == 1);
    REQUIRE(source_counter(ctx, "scan_providers_created") == 3);
    require_source_resources_balanced(ctx);
}

TEST_CASE("expression index build uses one standalone source owner",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({"source_lifetime_source_index"});
    exec_sql(ctx,
        "CREATE TABLE source_lifetime_source_index "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;"
        "INSERT INTO source_lifetime_source_index "
        "SELECT g, repeat('ACGT',1024)::bioseq.seq('dna4',32) "
        "FROM generate_series(1,128) AS g;");

    reset_source_counters(ctx);
    exec_sql(ctx,
        "CREATE INDEX source_lifetime_source_index_expr "
        "ON source_lifetime_source_index ((s[4:20]));");
    REQUIRE(source_counter(ctx, "standalone_providers_created") == 1);
    REQUIRE(source_counter(ctx, "domains_created") == 1);
    REQUIRE(source_counter(ctx, "domains_reused") >= 127);
    REQUIRE(source_counter(ctx, "cache_sets_created") == 1);
    REQUIRE(source_counter(ctx, "snapshots_registered") == 1);
    REQUIRE(source_counter(ctx, "live_registries") == 0);
}

TEST_CASE("cursor-held source registries promote or abort with subxact",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    auto ctx = fresh_context({"source_lifetime_source_subxact"});
    exec_sql(ctx,
        "CREATE TABLE source_lifetime_source_subxact "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;"
        "INSERT INTO source_lifetime_source_subxact VALUES "
        " (1, repeat('ACGT',2048)::bioseq.seq('dna4',32));");
    // Commit setup writes before entering the subtransaction lifecycle under test.
    exec_sql(ctx, "BEGIN;");

    reset_source_counters(ctx);
    exec_sql(ctx,
        "SAVEPOINT source_commit;"
        "DECLARE source_commit_cursor NO SCROLL CURSOR FOR "
        " SELECT bioseq.to_text(s[0:8]) "
        " FROM source_lifetime_source_subxact;"
        "FETCH 1 FROM source_commit_cursor;"
        "RELEASE SAVEPOINT source_commit;");
    REQUIRE(source_counter(ctx, "subxact_promotions") >= 2);
    REQUIRE(source_counter(ctx, "live_registries") == 1);
    exec_sql(ctx, "CLOSE source_commit_cursor;");
    REQUIRE(source_counter(ctx, "live_registries") == 0);

    reset_source_counters(ctx);
    exec_sql(ctx,
        "SAVEPOINT source_abort;"
        "DECLARE source_abort_cursor NO SCROLL CURSOR FOR "
        " SELECT bioseq.to_text(s[0:8]) "
        " FROM source_lifetime_source_subxact;"
        "FETCH 1 FROM source_abort_cursor;"
        "ROLLBACK TO SAVEPOINT source_abort;");
    REQUIRE(source_counter(ctx, "subxact_aborts") >= 1);
    REQUIRE(source_counter(ctx, "cleanup_abort") >= 1);
    REQUIRE(source_counter(ctx, "live_registries") == 0);
    REQUIRE(source_counter(ctx, "live_domains") == 0);
    exec_sql(ctx, "RELEASE SAVEPOINT source_abort; COMMIT;");
}

TEST_CASE("empty source registry reaches real prepared commit",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    auto owner = fresh_context({"source_lifetime_source_prepared_marker"});
    connection_test_context resolver;

    const auto max_prepared = query_scalar_int(
        owner,
        "SELECT current_setting('max_prepared_transactions')::bigint;");
    INFO("prepared-transaction acceptance requires "
         "max_prepared_transactions > 0");
    REQUIRE(max_prepared > 0);

    exec_sql(owner,
        "CREATE TABLE source_lifetime_source_prepared_marker "
        " (id bigint PRIMARY KEY);");
    const auto owner_pid = query_scalar_int(owner, "SELECT pg_backend_pid();");
    const auto nonce = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    const auto gid = fmt::format(
        "source_lifetime_empty_registry_{}_{}", owner_pid, nonce);
    const auto prepare_sql = fmt::format(
        "PREPARE TRANSACTION '{}';", gid);
    const auto commit_prepared_sql = fmt::format(
        "COMMIT PREPARED '{}';", gid);
    const auto prepared_count_sql = fmt::format(
        "SELECT count(*) FROM pg_catalog.pg_prepared_xacts "
        "WHERE gid='{}' AND database=current_database();", gid);
    prepared_transaction_cleanup_guard cleanup{resolver.raw(), gid};

    reset_source_counters(owner);
    exec_sql(owner, "BEGIN;");
    exec_sql(owner,
        "INSERT INTO source_lifetime_source_prepared_marker VALUES (377);");
    exec_sql(owner,
        "DO $$ BEGIN "
        "  PERFORM bioseq_internal.sequence_source_pre_prepare_probe(); "
        "  RAISE EXCEPTION 'PRE_PREPARE probe did not reject'; "
        "EXCEPTION WHEN feature_not_supported THEN "
        "  IF SQLERRM NOT LIKE "
        "    'cannot PREPARE TRANSACTION with live bioseq source domains%' "
        "  THEN RAISE; END IF; "
        "END $$;");
    REQUIRE(source_counter(owner, "pre_prepare_rejections") == 1);
    require_source_quiescent(owner);

    // PREPARE and COMMIT PREPARED must each be their own top-level command.
    exec_sql(owner, prepare_sql);
    cleanup.arm();
    REQUIRE(PQtransactionStatus(owner.raw()) == PQTRANS_IDLE);
    require_source_quiescent(owner);
    REQUIRE(query_scalar_int(resolver, prepared_count_sql) == 1);
    REQUIRE(query_scalar_int(
        resolver,
        "SELECT count(*) FROM source_lifetime_source_prepared_marker;") == 0);
    REQUIRE(PQtransactionStatus(resolver.raw()) == PQTRANS_IDLE);

    exec_sql(resolver, commit_prepared_sql);
    cleanup.release();
    REQUIRE(PQtransactionStatus(resolver.raw()) == PQTRANS_IDLE);
    REQUIRE(query_scalar_int(resolver, prepared_count_sql) == 0);
    REQUIRE(query_scalar_int(
        resolver,
        "SELECT count(*) FROM source_lifetime_source_prepared_marker "
        "WHERE id=377;") == 1);
    require_source_quiescent(owner);
    exec_sql(resolver, "DROP TABLE source_lifetime_source_prepared_marker;");
}

TEST_CASE("source bindings obey MVCC epoch rewrite and relcache invalidation",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    constexpr std::string_view table = "source_lifetime_source_mvcc";
    auto setup = fresh_context({table});
    exec_sql(setup,
        "CREATE TABLE source_lifetime_source_mvcc "
        " (id int PRIMARY KEY, s bioseq.seq('dna4',32)) USING bioseq;"
        "INSERT INTO source_lifetime_source_mvcc VALUES "
        " (1, repeat('ACGT',4096)::bioseq.seq('dna4',32));");

    connection_test_context reader;
    connection_test_context writer;
    reset_source_counters(reader);
    exec_sql(reader, "BEGIN ISOLATION LEVEL REPEATABLE READ;");
    REQUIRE(query_scalar(reader,
        "SELECT bioseq.to_text(s[0:8]) "
        "FROM source_lifetime_source_mvcc WHERE id=1;") == "ACGTACGT");

    exec_sql(writer,
        "UPDATE source_lifetime_source_mvcc "
        "SET s=repeat('TGCA',4096)::bioseq.seq('dna4',32) "
        "WHERE id=1;");
    REQUIRE(query_scalar(reader,
        "SELECT bioseq.to_text(s[0:8]) "
        "FROM source_lifetime_source_mvcc WHERE id=1;") == "ACGTACGT");
    exec_sql(reader, "COMMIT;");
    REQUIRE(query_scalar(reader,
        "SELECT bioseq.to_text(s[0:8]) "
        "FROM source_lifetime_source_mvcc WHERE id=1;") == "TGCATGCA");

    exec_sql(reader,
        "PREPARE source_lifetime_source_read AS "
        "SELECT bioseq.to_text(s[0:8]) "
        "FROM source_lifetime_source_mvcc WHERE id=1;");
    REQUIRE(query_scalar(reader, "EXECUTE source_lifetime_source_read;")
            == "TGCATGCA");
    exec_sql(writer,
        "ALTER TABLE source_lifetime_source_mvcc "
        "ADD COLUMN invalidation_marker int DEFAULT 7;");
    REQUIRE(query_scalar(reader, "EXECUTE source_lifetime_source_read;")
            == "TGCATGCA");
    exec_sql(writer, "VACUUM FULL source_lifetime_source_mvcc;");
    REQUIRE(query_scalar(reader, "EXECUTE source_lifetime_source_read;")
            == "TGCATGCA");
    exec_sql(reader, "DEALLOCATE source_lifetime_source_read;");

    REQUIRE(source_counter(reader, "domains_created") >= 6);
    REQUIRE(source_counter(reader, "wrong_thread_rejections") == 0);
    require_source_resources_balanced(reader);
}

TEST_CASE("attribute snapshot and callback session keys remain distinct",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    auto ctx = fresh_context({"source_lifetime_source_binding"});
    exec_sql(ctx,
        "CREATE TABLE source_lifetime_source_binding "
        " (id int, left_seq bioseq.seq('dna4',32), "
        "  right_seq bioseq.seq('dna4',32)) USING bioseq;"
        "INSERT INTO source_lifetime_source_binding "
        "SELECT g, repeat('ACGT',2048)::bioseq.seq('dna4',32), "
        "          repeat('TGCA',2048)::bioseq.seq('dna4',32) "
        "FROM generate_series(1,4) AS g;");

    reset_source_counters(ctx);
    REQUIRE(query_scalar(ctx,
        "SELECT bioseq.to_text(left_seq[0:8] "
        "       OPERATOR(bioseq.||) right_seq[0:8]) "
        "FROM source_lifetime_source_binding WHERE id=1;")
        == "ACGTACGTTGCATGCA");
    REQUIRE(source_counter(ctx, "registries_created") == 1);
    REQUIRE(source_counter(ctx, "domains_created") == 2);
    REQUIRE(source_counter(ctx, "cache_sets_created") == 2);
    REQUIRE(source_counter(ctx, "snapshots_registered") == 2);
    REQUIRE(source_counter(ctx, "scanner_entries_created") == 2);
    require_source_resources_balanced(ctx);

    exec_sql(ctx,
        "CREATE FUNCTION pg_temp.source_lifetime_read_source_text(value bioseq.seq) "
        "RETURNS text LANGUAGE plpgsql VOLATILE STRICT AS $$ "
        "BEGIN RETURN bioseq.to_text(value); END $$;");
    constexpr std::string_view callback_sql =
        "SELECT g, pg_temp.source_lifetime_read_source_text(("
        " SELECT s.left_seq[0:8] "
        " FROM source_lifetime_source_binding AS s "
        " WHERE s.id=g LIMIT 1"
        ")) AS fragment FROM generate_series(1,4) AS g;";
    const auto callback_plan = explain_text<false>(ctx, callback_sql);
    REQUIRE(plan_contains(callback_plan, "SubPlan"));
    REQUIRE(plan_contains(
        callback_plan, "Custom Scan (BioseqSliceProject)"));

    reset_source_counters(ctx);
    const auto rows = query_rows(ctx, callback_sql);
    REQUIRE(rows.size() == 4);
    for (const auto& row : rows) {
        REQUIRE(row.size() == 2);
        REQUIRE(row[1].value_or("") == "ACGTACGT");
    }
    REQUIRE(source_counter(ctx, "registries_created") == 1);
    REQUIRE(source_counter(ctx, "domains_created") == 1);
    REQUIRE(source_counter(ctx, "domains_reused") >= 3);
    REQUIRE(source_counter(ctx, "cache_sets_created") == 1);
    REQUIRE(source_counter(ctx, "snapshots_registered") == 1);
    REQUIRE(source_counter(ctx, "scanner_entries_created") == 4);
    REQUIRE(source_counter(ctx, "facade_source_stream_callbacks") == 4);
    REQUIRE(source_counter(ctx, "wrong_thread_rejections") == 0);
    require_source_resources_balanced(ctx);
}
#endif
