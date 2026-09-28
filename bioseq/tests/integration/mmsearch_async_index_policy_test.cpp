/**
 * @file mmsearch_async_index_policy_test.cpp
 * @brief Tests recovery after errors while queuing and building async indexes.
 */

#include <cctype>
#include <libpq-fe.h>

#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.tests.integration.support;
import bioseq.tests.integration.mmsearch_test_support;

using namespace bioseq_int_tests;
using namespace bioseq_int_tests::mmsearch_test_support;

TEST_CASE(
    "Asynchronous minimizers_per_sequence staging and worker PostgreSQL leaves recover exactly",
    "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_async_minimizer_sequence_worker_leaves});
    exec_sqlf(ctx,
        "CREATE TABLE {} (pk int, seq0 bioseq.seq('dna4', 32)) USING bioseq;",
        k_async_minimizer_sequence_worker_leaves);
    exec_sqlf(ctx,
        "SELECT bioseq.set_index_policy('{}'::regclass, "
        "'minimizers_per_fragment', 'UPDATE_MANUALLY');",
        k_async_minimizer_sequence_worker_leaves);
    exec_sqlf(ctx,
        "SELECT bioseq.set_index_policy('{}'::regclass, "
        "'minimizers_per_sequence', 'UPDATE_ON_INSERT');",
        k_async_minimizer_sequence_worker_leaves);
    exec_sqlf(ctx,
        "SELECT bioseq.set_active_read_index('{}'::regclass, "
        "'minimizers_per_sequence');",
        k_async_minimizer_sequence_worker_leaves);
    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
    exec_sqlf(ctx,
        "INSERT INTO {} VALUES "
        "(1, repeat('ACGT', 10000)::bioseq.seq('dna4', 32));",
        k_async_minimizer_sequence_worker_leaves);
    exec_sql(ctx, "SET bioseq.index.sequence_read.enabled = off;");
    exec_sql(ctx, "SET bioseq.index.async_persistence.enabled = on;");
    exec_sql(ctx, "SET bioseq.index.recompute_async_persistence.enabled = on;");
    exec_sql(ctx,
        "SET bioseq.aligner.minimizer.algorithm = 'two_stack_nthash';");
    // Keep this combination of minimizer, chaining, and alignment settings
    // explicit It must reach the injected staging error.
    exec_sql(ctx,
        "SET bioseq.aligner.chaining.algorithm = 'scalar_minimap2';");
    exec_sql(ctx,
        "SET bioseq.aligner.base_alignment.algorithm = 'block_aligner';");
    exec_sql(ctx,
        "SET bioseq.scheduler.pipeline.mode = 'task_parallel';");
    exec_sql(ctx, "SET bioseq.index.memory_cap_mb = 256;");
    const auto supervisor = query_scalar(
        ctx, "SELECT is_supervisor::text FROM bioseq.scheduler_stats();");
    if (supervisor != "true") {
        SKIP("asynchronous minimizers_per_sequence worker integration requires "
            "shared_preload_libraries='bioseq'");
    }
    const auto runnable_targets = static_cast<std::uint64_t>(query_scalar_int(
        ctx,
        "SELECT bioseq_internal."
        "highway_baseline_runnable_targets_for_test();"));
    REQUIRE(runnable_targets != 0u);
    const std::uint64_t forced_target =
        runnable_targets & (~runnable_targets + std::uint64_t{1});
    (void)query_scalar(ctx, fmt::format(
        "SELECT bioseq_internal.set_highway_target_for_test({});",
        static_cast<std::int64_t>(forced_target)));
    exec_sql(ctx,
        "SELECT bioseq_internal.disarm_async_minimizer_sequence_worker_failure();");

    const std::string trigger_sql = fmt::format(
        "SELECT pk FROM {} WHERE mmsearch(seq0, {}, "
        "aligner_options(index_read_requirement => 1));",
        k_async_minimizer_sequence_worker_leaves, hoe_probe_literal());
    // Fail at each step of staging the job in the client backend. Cleanup
    // must leave the connection usable so the next statement can try again.
    constexpr std::array<std::string_view, 4> stage_failure_points{
        "stage_register_xact_callback", "stage_dsm_create",
        "stage_dsm_pin", "stage_dsm_detach"};
    for (const std::string_view point : stage_failure_points) {
        REQUIRE(query_scalar(ctx, fmt::format(
            "SELECT bioseq_internal.arm_async_minimizer_sequence_worker_failure('{}')::text;",
            point)) == "true");
        const auto error = sql_error(ctx, trigger_sql);
        REQUIRE(error.sqlstate == "XX000");
        REQUIRE(error.contains(
            "injected asynchronous minimizer-sequence staging PostgreSQL leaf failure"));
        REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
    }
    const auto boundary = [&ctx] {
        const auto row = query_one(ctx,
            "SELECT * FROM "
            "bioseq_internal.async_minimizer_sequence_worker_boundary_stats();");
        REQUIRE(row.size() == 14);
        return row;
    };
    const auto count_at = [](const row_t& row, const std::size_t column) {
        REQUIRE(column < row.size());
        REQUIRE(row[column].has_value());
        return std::stoll(*row[column]);
    };
    const auto async_jobs_waiting = [&ctx] {
        return query_scalar_int(ctx,
            "SELECT async_persist_jobs_waiting "
            "FROM bioseq.scheduler_stats();");
    };
    const auto wait_for_settled = [&async_jobs_waiting, &boundary, &count_at](
            const row_t& before,
            const bool materialize,
            const long long waiting_before) {
        row_t observed;
        for (int attempt = 0; attempt < 400; ++attempt) {
            observed = boundary();
            const bool failure_settled =
                count_at(observed, 4) == count_at(before, 4) + 1
                && count_at(observed, 5) == count_at(before, 5) + 1
                && observed[11].value_or("") == "none";
            const bool admission_settled = !materialize
                || (count_at(observed, 6) > count_at(before, 6)
                    && count_at(observed, 7) == count_at(before, 7) + 1
                    && count_at(observed, 8) == count_at(before, 8) + 1
                    && count_at(observed, 9) == count_at(before, 9));
            if (failure_settled && admission_settled
                && async_jobs_waiting() == waiting_before) {
                return observed;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{25});
        }
        FAIL("asynchronous minimizer-sequence worker failure did not fully unwind");
        return observed;
    };

    constexpr std::array<std::string_view, 10> failure_points{
        "relation_open", "relation_identity", "relation_close",
        "spi_connect", "spi_execute", "spi_read", "spi_finish",
        "materialize_error_log", "materialize_interrupt",
        "materialize_scanner_read"};
    const long long initial_admission_mismatches = count_at(boundary(), 9);
    for (const std::string_view point : failure_points) {
        const row_t before = boundary();
        const long long waiting_before = async_jobs_waiting();
        REQUIRE(query_scalar(ctx, fmt::format(
            "SELECT bioseq_internal.arm_async_minimizer_sequence_worker_failure('{}')::text;",
            point)) == "true");
        (void)query_rows(ctx, trigger_sql);
        const bool materialize = point.starts_with("materialize_");
        const row_t after = wait_for_settled(
            before, materialize, waiting_before);
        REQUIRE(count_at(after, 4) > count_at(before, 4));
        REQUIRE(count_at(after, 5) == count_at(before, 5) + 1);
        REQUIRE(after[10].value_or("") == point);
        REQUIRE(after[11].value_or("") == "none");
        REQUIRE(after[12].value_or("") == "XX000");
        REQUIRE(after[13].value_or("")
            == "injected asynchronous minimizer-sequence worker PostgreSQL leaf failure");

        if (materialize) {
            REQUIRE(count_at(after, 6) > count_at(before, 6));
            REQUIRE(count_at(after, 7) == count_at(before, 7) + 1);
            REQUIRE(count_at(after, 8) == count_at(before, 8) + 1);
            REQUIRE(count_at(after, 9) == count_at(before, 9));
        }
        // The client must remain usable after each worker failure.
        REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
    }

    // Disable fault injection and  require a successful index build after
    // the ten failed jobs.
    exec_sql(ctx,
        "SELECT bioseq_internal.disarm_async_minimizer_sequence_worker_failure();");
    const long long generation_before = query_scalar_int(
        ctx, "SELECT generation FROM bioseq.scheduler_minimizer_sequence_build_stats();");
    (void)query_rows(ctx, trigger_sql);
    long long generation_after = generation_before;
    for (int attempt = 0;
         attempt < 400 && generation_after <= generation_before; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{25});
        generation_after = query_scalar_int(
            ctx, "SELECT generation FROM bioseq.scheduler_minimizer_sequence_build_stats();");
    }
    REQUIRE(generation_after > generation_before);
    long long persisted_rows = 0;
    for (int attempt = 0; attempt < 400 && persisted_rows == 0; ++attempt) {
        persisted_rows = index_table_row_count(
            ctx, "minimizers_per_sequence", k_async_minimizer_sequence_worker_leaves);
        if (persisted_rows == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds{25});
        }
    }
    REQUIRE(persisted_rows > 0);
    const row_t recovered = boundary();
    REQUIRE(recovered[11].value_or("") == "none");
    REQUIRE(count_at(recovered, 9) == initial_admission_mismatches);
    (void)query_scalar(
        ctx, "SELECT bioseq_internal.set_highway_target_for_test(0);");
}
