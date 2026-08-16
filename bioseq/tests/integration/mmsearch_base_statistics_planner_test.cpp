/** @file Base, statistics, and planner mmsearch integration scenarios. */

#include <tests/support/postgres.hpp>
#include <bioseq/pg_build_profile.h>

#include <array>
#include <bit>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <libpq-fe.h>
#include <set>
#include <span>
#include <string_view>
#include <thread>

import bioseq.tests.integration.support;
import bioseq.tests.integration.mmsearch_test_support;

using namespace bioseq_int_tests;
using namespace bioseq_int_tests::mmsearch_test_support;

TEST_CASE("mmsearch on columnar table runs without errors and stays in domain",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_hoe});
    seed_hoe_fixture(ctx, k_hoe);

    const std::string base_sql = fmt::format(
        "SELECT pk FROM {} WHERE mmsearch(seq0, {}) ORDER BY pk;",
        k_hoe,
        hoe_probe_literal());

    const auto pks = query_int_column(ctx, base_sql);
    INFO("mmsearch returned " << pks.size() << " pks");
    const std::set<long long> seeded{1, 2, 3, 4, 5};
    for (const auto pk : pks) {
        REQUIRE(seeded.contains(pk));
    }
}

TEST_CASE("scalar mmsearch count retains its privately owned target column",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_hoe});
    seed_hoe_fixture(ctx, k_hoe);
    exec_sql(ctx,
        "SET max_parallel_workers_per_gather=0;"
        "SET bioseq.index.sequence_read.enabled=off;"
        "SET bioseq.index.async_persistence.enabled=off;"
        "SET bioseq.index.recompute_async_persistence.enabled=off;"
        "SET bioseq.vectorization.enabled=off;"
        "SET bioseq.aligner.minimizer.algorithm='scalar';"
        "SET bioseq.aligner.chaining.algorithm='scalar_minimap2';"
        "SET bioseq.aligner.base_alignment.algorithm='ksw2';"
        "SET bioseq.aligner.chaining.minimum_score=20;"
        "SET bioseq.aligner.vulkan.enabled=off;");

    const std::string hidden_dependency_sql = fmt::format(
        "SELECT count(*) FROM {} WHERE mmsearch(seq0, {}, "
        "aligner_options(index_read_requirement => 1));",
        k_hoe, hoe_probe_literal());
    const std::string projected_dependency_sql = fmt::format(
        "SELECT count(seq0) FROM {} WHERE mmsearch(seq0, {}, "
        "aligner_options(index_read_requirement => 1));",
        k_hoe, hoe_probe_literal());

    const auto plan = explain_text<false>(ctx, hidden_dependency_sql);
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqScan)"));
    REQUIRE_FALSE(plan_contains(plan, "Filter: mmsearch"));
    REQUIRE_FALSE(plan_contains(plan, "One-Time Filter: mmsearch"));

    const long long projected_count = query_scalar_int(
        ctx, projected_dependency_sql);
    REQUIRE(projected_count > 0);
    REQUIRE(query_scalar_int(ctx, hidden_dependency_sql) == projected_count);
}

#ifdef BIOSEQ_TEST_INDEXES
TEST_CASE("Locator output survives every multi-match context reset",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_output_restore});
    exec_sql(ctx,
        "SET max_parallel_workers_per_gather=0;"
        "SET bioseq.index.insert_maintenance.enabled=off;"
        "SET bioseq.index.sequence_read.enabled=off;"
        "SET bioseq.index.async_persistence.enabled=off;"
        "SET bioseq.index.recompute_async_persistence.enabled=off;"
        "SET bioseq.vectorization.enabled=off;"
        "SET bioseq.aligner.minimizer.algorithm='scalar';"
        "SET bioseq.aligner.chaining.algorithm='scalar_minimap2';"
        "SET bioseq.aligner.base_alignment.algorithm='ksw2';"
        "SET bioseq.aligner.chaining.minimum_score=20;"
        "SET bioseq.aligner.secondary.maximum_count=5;"
        "SET bioseq.aligner.vulkan.enabled=off;");

    /* Row five produces three MatchResult values. The homogeneous suffix
     * cannot add the diverse 29-base probe, but makes
     * the stored value unambiguously locator-backed. */
    const auto canonical_rows = hoe_seed_rows();
    std::string target = canonical_rows.back().sequences.front();
    target.append(16384u, 'A');
    const std::string projected_source(16384u, 'C');
    const std::string projected_slice = projected_source.substr(7u, 16u);
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string{k_output_restore},
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 3,
            .using_bioseq = true,
        },
        {seq_row{.pk = 5, .sequences = {
            target, projected_source, std::string{k_hoe_probe_sequence}}}});

    /* Coordinate-only plans retain chain-only candidates, whose absent base
     * alignment may carry the documented 0/0 sentinel. Request has_cigar so
     * only the three usable alignments reach output promotion; the dispatch
     * cursor rejects every degenerate candidate first. */
    const std::string output_sql = fmt::format(
        "SELECT pk, bioseq.target_start(seq0), "
        "       bioseq.target_end(seq0), seq0::text, "
        "       (seq1[7:23])::text, "
        "       seq2::text, "
        "       bioseq.has_cigar(seq0) "
        "FROM {} "
        "WHERE mmsearch(seq0, {}, "
        " aligner_options(best_n=>5,index_read_requirement=>1)) "
        "ORDER BY bioseq.target_start(seq0), bioseq.target_end(seq0);",
        k_output_restore, hoe_probe_literal());
    const std::string evaluated_output_sql = fmt::format(
        "SELECT pk, bioseq.target_start(seq0[0:]), "
        "       bioseq.target_end(seq0[0:]), (seq0[0:32])::text, "
        "       (seq1[7:23])::text, "
        "       seq2::text, "
        "       bioseq.has_cigar(seq0[0:]) "
        "FROM {} "
        "WHERE mmsearch(seq0[0:], {}, "
        " aligner_options(best_n=>5,index_read_requirement=>1)) "
        "ORDER BY bioseq.target_start(seq0[0:]), "
        "         bioseq.target_end(seq0[0:]);",
        k_output_restore, hoe_probe_literal());
    const std::string raw_var_pair_output_sql = fmt::format(
        "SELECT pk, bioseq.target_start(seq0), "
        "       bioseq.target_end(seq0), seq0::text, "
        "       (seq1[7:23])::text, "
        "       seq2::text, "
        "       bioseq.has_cigar(seq0) "
        "FROM {} "
        "WHERE mmsearch(seq0, seq2, "
        " aligner_options(best_n=>5,index_read_requirement=>1)) "
        "ORDER BY bioseq.target_start(seq0), bioseq.target_end(seq0);",
        k_output_restore);
    const std::string transformed_target_raw_query_sql = fmt::format(
        "SELECT pk, bioseq.target_start(seq0[0:]), "
        "       bioseq.target_end(seq0[0:]), (seq0[0:32])::text, "
        "       (seq1[7:23])::text, "
        "       seq2::text, "
        "       bioseq.has_cigar(seq0[0:]) "
        "FROM {} "
        "WHERE mmsearch(seq0[0:], seq2, "
        " aligner_options(best_n=>5,index_read_requirement=>1)) "
        "ORDER BY bioseq.target_start(seq0[0:]), "
        "         bioseq.target_end(seq0[0:]);",
        k_output_restore);
    const std::string raw_target_transformed_query_sql = fmt::format(
        "SELECT pk, bioseq.target_start(seq0), "
        "       bioseq.target_end(seq0), (seq0[0:32])::text, "
        "       (seq1[7:23])::text, "
        "       seq2::text, "
        "       bioseq.has_cigar(seq0) "
        "FROM {} "
        "WHERE mmsearch(seq0, seq2[0:], "
        " aligner_options(best_n=>5,index_read_requirement=>1)) "
        "ORDER BY bioseq.target_start(seq0), bioseq.target_end(seq0);",
        k_output_restore);
    const auto plan = explain_text<false>(ctx, output_sql);
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqScan)"));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE_FALSE(plan_contains(plan, "Filter: mmsearch"));
    const auto evaluated_plan = explain_text<false>(ctx, evaluated_output_sql);
    REQUIRE(plan_contains(evaluated_plan, "Custom Scan (BioseqScan)"));
    REQUIRE(plan_contains(
        evaluated_plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE_FALSE(plan_contains(evaluated_plan, "Filter: mmsearch"));
    for (const auto& mixed_operand_sql : {
            raw_var_pair_output_sql,
            transformed_target_raw_query_sql,
            raw_target_transformed_query_sql}) {
        const auto mixed_plan = explain_text<false>(ctx, mixed_operand_sql);
        REQUIRE(plan_contains(mixed_plan, "Custom Scan (BioseqScan)"));
        REQUIRE(plan_contains(
            mixed_plan, "Custom Scan (BioseqSliceProject)"));
        REQUIRE_FALSE(plan_contains(mixed_plan, "Filter: mmsearch"));
    }

    const auto require_exact_outputs = [&projected_slice](
            const rowset_t& outputs,
            const std::string& expected_target_projection) {
        REQUIRE(outputs.size() >= 3u);
        std::set<long long> starts;
        for (const auto& row : outputs) {
            REQUIRE(row.size() == 7u);
            REQUIRE(row[0] == std::optional<std::string>{"5"});
            REQUIRE(row[1].has_value());
            REQUIRE(row[2].has_value());
            REQUIRE(row[3]
                == std::optional<std::string>{expected_target_projection});
            REQUIRE(row[4] == std::optional<std::string>{projected_slice});
            REQUIRE(row[5]
                == std::optional<std::string>{std::string{k_hoe_probe_sequence}});
            REQUIRE(row[6] == std::optional<std::string>{"t"});
            const long long start = std::stoll(*row[1]);
            const long long end = std::stoll(*row[2]);
            REQUIRE(start >= 0);
            REQUIRE(start < end);
            starts.insert(start);
        }
        REQUIRE(starts.size() >= 3u);
    };

    reset_mmsearch_source_counters(ctx);
    const rowset_t baseline = query_rows(ctx, output_sql);
    require_exact_outputs(baseline, target);
    REQUIRE(mmsearch_source_counter(
        ctx, "mmsearch_output_context_resets")
        == static_cast<std::int64_t>(baseline.size()));
    REQUIRE(mmsearch_source_counter(
        ctx, "mmsearch_output_locator_promotions")
        == static_cast<std::int64_t>(baseline.size()));
    REQUIRE(mmsearch_source_counter(
        ctx, "mmsearch_output_pg_error_restores") == 0);
    REQUIRE(mmsearch_source_counter(
        ctx, "mmsearch_output_restore_mismatches") == 0);
    REQUIRE(mmsearch_source_counter(ctx, "scan_providers_created") == 1);
    /* Source domains are keyed by physical attribute.  The three projected
     * sequence columns therefore establish three reusable domains under the
     * one scan provider. */
    REQUIRE(mmsearch_source_counter(ctx, "domains_created") == 3);
    REQUIRE(mmsearch_source_counter(ctx, "domains_reused")
        >= static_cast<std::int64_t>(baseline.size() - 1u));
    require_mmsearch_source_resources_balanced(ctx);

    /* Raw target, raw per-row query, and an unrelated BSS3 output occupy
     * three distinct physical columns.  Their locator snapshots form one
     * duplicate-free output union and every multi-match row sees the same
     * stable values after per-output restoration. */
    reset_mmsearch_source_counters(ctx);
    const rowset_t raw_var_pair = query_rows(ctx, raw_var_pair_output_sql);
    REQUIRE(raw_var_pair == baseline);
    require_exact_outputs(raw_var_pair, target);
    REQUIRE(mmsearch_source_counter(
        ctx, "mmsearch_output_context_resets")
        == static_cast<std::int64_t>(raw_var_pair.size()));
    REQUIRE(mmsearch_source_counter(
        ctx, "mmsearch_output_locator_promotions")
        == static_cast<std::int64_t>(raw_var_pair.size()));
    REQUIRE(mmsearch_source_counter(
        ctx, "mmsearch_output_restore_mismatches") == 0);
    require_mmsearch_source_resources_balanced(ctx);

    /* The transformed target consumes seq0 before submission while seq1 is
     * output-only.  The same seq0 locator is also needed by an independent
     * output slice.  Both physical columns must be restored into the buffered
     * row and promoted under each MatchResult's fresh output context. */
    reset_mmsearch_source_counters(ctx);
    const rowset_t evaluated = query_rows(ctx, evaluated_output_sql);
    REQUIRE(evaluated.size() == baseline.size());
    require_exact_outputs(evaluated, target.substr(0u, 32u));
    REQUIRE(mmsearch_source_counter(
        ctx, "mmsearch_output_context_resets")
        == static_cast<std::int64_t>(evaluated.size()));
    REQUIRE(mmsearch_source_counter(
        ctx, "mmsearch_output_locator_promotions")
        == static_cast<std::int64_t>(evaluated.size()));
    REQUIRE(mmsearch_source_counter(
        ctx, "mmsearch_output_pg_error_restores") == 0);
    REQUIRE(mmsearch_source_counter(
        ctx, "mmsearch_output_restore_mismatches") == 0);
    require_mmsearch_source_resources_balanced(ctx);

    /* Evaluated submission executes ExprStates for both operands.  Exercise
     * each asymmetric shape so the raw-Var side is promoted too, then restored
     * with the transformed side before the stable locator row is buffered. */
    for (const auto& mixed_operand_sql : {
            transformed_target_raw_query_sql,
            raw_target_transformed_query_sql}) {
        reset_mmsearch_source_counters(ctx);
        const rowset_t mixed_rows = query_rows(ctx, mixed_operand_sql);
        REQUIRE(mixed_rows.size() == baseline.size());
        require_exact_outputs(mixed_rows, target.substr(0u, 32u));
        REQUIRE(mmsearch_source_counter(
            ctx, "mmsearch_output_context_resets")
            == static_cast<std::int64_t>(mixed_rows.size()));
        REQUIRE(mmsearch_source_counter(
            ctx, "mmsearch_output_locator_promotions")
            == static_cast<std::int64_t>(mixed_rows.size()));
        REQUIRE(mmsearch_source_counter(
            ctx, "mmsearch_output_restore_mismatches") == 0);
        require_mmsearch_source_resources_balanced(ctx);
    }

    /* This one-shot ERROR occurs after the locator Datum was replaced by an
     * EOH and before ExecProject.  The PG_CATCH path must put the exact
     * MinimalTuple Datum back before ordinary statement-error cleanup. */
    reset_mmsearch_source_counters(ctx);
    exec_sql(ctx,
        "SELECT bioseq_internal.arm_sequence_source_failure("
        " 'mmsearch_output_after_promotion');");
    const auto injected = sql_error(ctx, raw_var_pair_output_sql);
    REQUIRE(injected.sqlstate == "XX000");
    REQUIRE(injected.contains(
        "injected PostgreSQL sequence source-domain failure"));
    REQUIRE(mmsearch_source_counter(
        ctx, "mmsearch_output_context_resets") == 1);
    REQUIRE(mmsearch_source_counter(
        ctx, "mmsearch_output_locator_promotions") == 1);
    REQUIRE(mmsearch_source_counter(
        ctx, "mmsearch_output_pg_error_restores") == 1);
    REQUIRE(mmsearch_source_counter(
        ctx, "mmsearch_output_restore_mismatches") == 0);
    REQUIRE(mmsearch_source_counter(ctx, "fault_injections") == 1);
    require_mmsearch_source_resources_balanced(ctx);
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);

    /* Keep the failure injection armed through this replay so it is consumed
     * before raising and cannot poison a later scan in the backend. */
    const rowset_t replay = query_rows(ctx, raw_var_pair_output_sql);
    REQUIRE(replay == raw_var_pair);
    require_exact_outputs(replay, target);
    REQUIRE(mmsearch_source_counter(
        ctx, "mmsearch_output_context_resets")
        == 1 + static_cast<std::int64_t>(replay.size()));
    REQUIRE(mmsearch_source_counter(
        ctx, "mmsearch_output_locator_promotions")
        == 1 + static_cast<std::int64_t>(replay.size()));
    REQUIRE(mmsearch_source_counter(
        ctx, "mmsearch_output_pg_error_restores") == 1);
    REQUIRE(mmsearch_source_counter(
        ctx, "mmsearch_output_restore_mismatches") == 0);
    REQUIRE(mmsearch_source_counter(ctx, "fault_injections") == 1);
    require_mmsearch_source_resources_balanced(ctx);
    exec_sql(ctx,
        "SELECT bioseq_internal.disarm_sequence_source_failure();");
}
#endif

TEST_CASE("alignment execution SRF is empty before the first published root",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    connection_test_context ctx;
    REQUIRE(last_alignment_stats(ctx).empty());
    REQUIRE(query_scalar_int(ctx,
        "SELECT (bioseq."
        "last_alignment_persisted_gpu_operation_stats() IS NULL)::int;")
        == 1);
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
    REQUIRE(last_alignment_stats(ctx).empty());
    REQUIRE(query_scalar_int(ctx,
        "SELECT (bioseq."
        "last_alignment_persisted_gpu_operation_stats() IS NULL)::int;")
        == 1);
}

TEST_CASE("alignment execution SRF publishes one exact stage row set",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_hoe});
    seed_hoe_fixture(ctx, k_hoe);
    exec_sql(ctx, "SET max_parallel_workers_per_gather = 0;");
    exec_sql(ctx, "SET bioseq.index.sequence_read.enabled = off;");

    const auto result = query_rows(ctx, index_requirement_sql(k_hoe, 1));
    REQUIRE_FALSE(result.empty());
    const auto rows = last_alignment_stats(ctx);
    REQUIRE(rows.size() == 20);

    const std::array<std::string_view, 20> expected_stages{
        "query_sketch", "target_sketch", "target_index_build",
        "persisted_index_decode", "partition_selection",
        "device_index_upload", "seed_lookup", "chain_score",
        "chain_backtrack", "region_generation",
        "primary_secondary_reduction", "divergence_estimation", "mapq",
        "target_window_plan", "target_window_io", "target_window_upload",
        "cigar_job_generation", "left_extension", "gap_extension",
        "right_extension",
    };
    const long long execution_id = stats_i64(rows.front(), 0);
    REQUIRE(execution_id > 0);
    REQUIRE(stats_i64(rows.front(), 1)
            == query_scalar_int(ctx, "SELECT pg_backend_pid();"));
    REQUIRE(stats_i64(rows.front(), 2)
            == query_scalar_int(
                ctx,
                "SELECT oid::bigint FROM pg_database "
                "WHERE datname=current_database();"));
    for (std::size_t stage_no = 0; stage_no < rows.size(); ++stage_no) {
        const auto& row = rows[stage_no];
        REQUIRE(row.size() == 151);
        REQUIRE(stats_i64(row, 0) == execution_id);
        REQUIRE(stats_i64(row, 11) == 1);
        REQUIRE(stats_i64(row, 12) == 0);
        REQUIRE(stats_bool(row, 13));
        REQUIRE_FALSE(stats_bool(row, 14));
        REQUIRE(stats_bool(row, 15));
        REQUIRE(row[18].value_or("") == "recomputed");
        REQUIRE(row[19].value_or("") == "recomputed");
        REQUIRE(row[21].value_or("") == "0000000000000000");
        REQUIRE(row[23].value_or("") == expected_stages[stage_no]);
        REQUIRE(stats_i64(row, 32) == 5);
        REQUIRE(stats_i64(row, 33) == 5);
        REQUIRE(stats_i64(row, 34) == 0);
        REQUIRE(stats_i64(row, 35) == 5);
        REQUIRE_FALSE(stats_bool(row, 141));
        for (std::size_t gpu_column = 4; gpu_column <= 10; ++gpu_column) {
            REQUIRE_FALSE(row[gpu_column].has_value());
        }
    }
}

TEST_CASE("persisted GPU SQL statistics are exact-empty and immutable for completed CPU work",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_hoe});
    seed_hoe_fixture(ctx, k_hoe);
    exec_sql(ctx, "SET max_parallel_workers_per_gather = 0;");
    exec_sql(ctx, "SET bioseq.index.sequence_read.enabled = off;");
    REQUIRE_FALSE(query_rows(ctx, index_requirement_sql(k_hoe, 1)).empty());

    const std::string first = query_scalar(ctx,
        "SELECT bioseq."
        "last_alignment_persisted_gpu_operation_stats()::text;");
    const std::string second = query_scalar(ctx,
        "SELECT bioseq."
        "last_alignment_persisted_gpu_operation_stats()::text;");
    REQUIRE(second == first);

    const row_t diagnostics = query_one(ctx,
        "WITH gpu_stats AS MATERIALIZED ("
        " SELECT bioseq."
        "last_alignment_persisted_gpu_operation_stats() AS value"
        "), stats AS ("
        " SELECT execution_id, backend_pid, database_oid, is_completed, is_partial,"
        "        is_fully_drained"
        " FROM bioseq.last_alignment_execution_stats()"
        " WHERE stage='query_sketch'"
        ") SELECT "
        "jsonb_typeof(value)='object', "
        "(SELECT count(*) FROM jsonb_object_keys(value))=19 "
        "AND value ?& ARRAY["
        " 'schema','revision','execution_id','backend_pid','database_oid',"
        " 'is_completed','is_partial','is_fully_drained','is_consistent','is_complete',"
        " 'is_saturated','dropped_operation_count','operation_count',"
        " 'accepted_operation_count','complete_operation_count','job_count',"
        " 'accepted_job_count','cache_validation','operations']::text[], "
        "value->>'schema'='bioseq.persisted_gpu_operation_stats' "
        "AND (value->>'revision')::bigint=2 "
        "AND jsonb_typeof(value->'schema')='string' "
        "AND jsonb_typeof(value->'revision')='number', "
        "(value->>'execution_id')::bigint=stats.execution_id "
        "AND (value->>'backend_pid')::bigint=stats.backend_pid "
        "AND (value->>'database_oid')::bigint=stats.database_oid, "
        "(value->>'is_completed')::boolean=stats.is_completed "
        "AND (value->>'is_partial')::boolean=stats.is_partial "
        "AND (value->>'is_fully_drained')::boolean=stats.is_fully_drained "
        "AND stats.is_completed AND NOT stats.is_partial "
        "AND stats.is_fully_drained, "
        "NOT (value->>'is_consistent')::boolean "
        "AND (value->>'is_complete')::boolean "
        "AND NOT (value->>'is_saturated')::boolean, "
        "(value->>'dropped_operation_count')::bigint=0 "
        "AND (value->>'operation_count')::bigint=0 "
        "AND (value->>'accepted_operation_count')::bigint=0 "
        "AND (value->>'complete_operation_count')::bigint=0 "
        "AND (value->>'job_count')::bigint=0 "
        "AND (value->>'accepted_job_count')::bigint=0, "
        "jsonb_typeof(value->'operations')='array' "
        "AND jsonb_array_length(value->'operations')=0, "
        "jsonb_typeof(value->'cache_validation')='null', "
        "NOT EXISTS ("
        " SELECT 1 FROM jsonb_each(value) AS entry"
        " WHERE entry.key IN ("
        "  'revision','execution_id','backend_pid','database_oid',"
        "  'dropped_operation_count','operation_count',"
        "  'accepted_operation_count','complete_operation_count',"
        "  'job_count','accepted_job_count'"
        " ) AND jsonb_typeof(entry.value)<>'number'"
        "), "
        "NOT EXISTS ("
        " SELECT 1 FROM jsonb_each(value) AS entry"
        " WHERE entry.key IN ("
        "  'is_completed','is_partial','is_fully_drained','is_consistent',"
        "  'is_complete','is_saturated'"
        " ) AND jsonb_typeof(entry.value)<>'boolean'"
        "), "
        "(SELECT provolatile='v' AND proparallel='r' "
        " FROM pg_proc WHERE oid='bioseq."
        "last_alignment_persisted_gpu_operation_stats()'::regprocedure) "
        "FROM gpu_stats CROSS JOIN stats;");
    REQUIRE(diagnostics.size() == 12u);
    for (const auto& diagnostic_result : diagnostics) {
        REQUIRE(diagnostic_result.value_or("") == "t");
    }
}

TEST_CASE("alignment execution id is fresh and errors do not publish",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_hoe});
    seed_hoe_fixture(ctx, k_hoe);
    exec_sql(ctx, "SET max_parallel_workers_per_gather = 0;");
    exec_sql(ctx, "SET bioseq.index.sequence_read.enabled = off;");

    (void)query_rows(ctx, index_requirement_sql(k_hoe, 1));
    const long long first_id = stats_i64(last_alignment_stats(ctx).front(), 0);
    const std::string first_evidence = query_scalar(ctx,
        "SELECT bioseq."
        "last_alignment_persisted_gpu_operation_stats()::text;");
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
    REQUIRE(stats_i64(last_alignment_stats(ctx).front(), 0) == first_id);

    require_no_current_compatible_persisted_index(
        ctx, index_requirement_sql(k_hoe, 2));
    REQUIRE(stats_i64(last_alignment_stats(ctx).front(), 0) == first_id);
    REQUIRE(query_scalar(ctx,
        "SELECT bioseq."
        "last_alignment_persisted_gpu_operation_stats()::text;")
        == first_evidence);

    (void)query_rows(ctx, index_requirement_sql(k_hoe, 1));
    REQUIRE(stats_i64(last_alignment_stats(ctx).front(), 0) == first_id + 1);
    REQUIRE(query_scalar_int(ctx,
        "SELECT (bioseq."
        "last_alignment_persisted_gpu_operation_stats()->>"
        "'execution_id')::bigint;") == first_id + 1);
}

TEST_CASE("alignment LIMIT publishes a partial statement snapshot",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_hoe});
    seed_hoe_fixture(ctx, k_hoe);
    exec_sql(ctx, "SET max_parallel_workers_per_gather = 0;");
    exec_sql(ctx, "SET bioseq.index.sequence_read.enabled = off;");
    REQUIRE_FALSE(query_rows(ctx, index_requirement_sql(k_hoe, 1)).empty());
    const long long completed_id = stats_i64(
        last_alignment_stats(ctx).front(), 0);
    const std::string completed_evidence = query_scalar(ctx,
        "SELECT bioseq."
        "last_alignment_persisted_gpu_operation_stats()::text;");
    const std::string limited_sql = fmt::format(
        "SELECT pk FROM {} WHERE mmsearch(seq0, {}, "
        "aligner_options(index_read_requirement => 1)) LIMIT 1;",
        k_hoe,
        hoe_probe_literal());
    REQUIRE_FALSE(query_rows(ctx, limited_sql).empty());
    const auto rows = last_alignment_stats(ctx);
    REQUIRE(rows.size() == 20);
    REQUIRE(stats_i64(rows.front(), 0) == completed_id + 1);
    REQUIRE_FALSE(stats_bool(rows.front(), 13));
    REQUIRE(stats_bool(rows.front(), 14));
    REQUIRE_FALSE(stats_bool(rows.front(), 15));
    REQUIRE(rows.front()[16].value_or("") == "mixed_invalid");
    REQUIRE(rows.front()[17].value_or("") == "mixed_invalid");
    REQUIRE(stats_i64(rows.front(), 33) > 0);
    REQUIRE(stats_i64(rows.front(), 34) == 0);
    REQUIRE(stats_i64(rows.front(), 35) == stats_i64(rows.front(), 33));
    REQUIRE(query_scalar(ctx,
        "SELECT bioseq."
        "last_alignment_persisted_gpu_operation_stats()::text;")
        == completed_evidence);
    REQUIRE(query_scalar_int(ctx,
        "SELECT (bioseq."
        "last_alignment_persisted_gpu_operation_stats()->>"
        "'execution_id')::bigint;") == completed_id);
}

TEST_CASE("one executor root aggregates two invoked alignment nodes",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_hoe});
    seed_hoe_fixture(ctx, k_hoe);
    exec_sql(ctx, "SET max_parallel_workers_per_gather = 0;");
    exec_sql(ctx, "SET bioseq.index.sequence_read.enabled = off;");
    const std::string sql = fmt::format(
        "SELECT "
        "(SELECT count(*) FROM {} a WHERE mmsearch(a.seq0, {}, "
        " aligner_options(index_read_requirement => 1))), "
        "(SELECT count(*) FROM {} b WHERE mmsearch(b.seq0, {}, "
        " aligner_options(index_read_requirement => 1)));",
        k_hoe,
        hoe_probe_literal(),
        k_hoe,
        hoe_probe_literal());
    REQUIRE(query_rows(ctx, sql).size() == 1);
    const auto rows = last_alignment_stats(ctx);
    REQUIRE(rows.size() == 20);
    REQUIRE(stats_i64(rows.front(), 11) == 2);
    REQUIRE(stats_i64(rows.front(), 12) == 0);
    REQUIRE(stats_bool(rows.front(), 13));
    REQUIRE(stats_i64(rows.front(), 32) == 10);
    REQUIRE(stats_i64(rows.front(), 33) == 10);
}

TEST_CASE("multiple broadcast queries fail closed",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_hoe});
    seed_hoe_fixture(ctx, k_hoe);
    exec_sql(ctx, "SET bioseq.index.sequence_read.enabled = off;");
    const std::string distinct_probe = format_literal(
        "CTAGTCACTGTAGCCTGATCGTAGCTAGC",
        seq_typmod{.alphabet = "dna4", .width = 32});

    const std::string sql = fmt::format(
        "SELECT pk FROM {} "
        "WHERE mmsearch(seq0, {}) AND mmsearch(seq0, {}) "
        "ORDER BY pk;",
        k_hoe,
        hoe_probe_literal(),
        distinct_probe);
    const auto error = sql_error(ctx, sql);
    REQUIRE(error.sqlstate == "0A000");
    REQUIRE(error.contains("stacked mmsearch"));
}

TEST_CASE("BioseqBatchJoin plans one physical target and rewrites compound helpers",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_batch_join_target});
    seed_batch_join_target(ctx);
    load_batch_join_queries(ctx, 2);

    const std::string sql = batch_join_sql(
        1,
        "q.id, t.pk, "
        "bioseq.score(t.pk) + 1 AS adjusted_score, "
        "bioseq.query_end(t.pk) - bioseq.query_start(t.pk) AS query_span, "
        "bioseq.has_cigar(t.pk) AS has_cigar, "
        "bioseq.edit(t.pk) AS edit_distance");
    const auto plan = explain_text<false>(ctx, sql);
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqBatchJoin)"));
    REQUIRE(plan_contains(plan, "Bioseq Residual Mmsearch Qual: absent"));
    REQUIRE(plan_contains(plan, "Bioseq Batch ABI Version: 3"));
    REQUIRE(plan_contains(plan, "Bioseq Match Placeholder Sites: 5"));
    REQUIRE_FALSE(plan_contains(plan, "Join Filter: mmsearch"));

    const auto rows = query_rows(ctx, sql + " ORDER BY q.id, t.pk;");
    REQUIRE_FALSE(rows.empty());
    for (const auto& row : rows) {
        REQUIRE(row.size() == 6);
        REQUIRE(row[0].has_value());
        REQUIRE(row[1].has_value());
        REQUIRE(row[2].has_value());
        REQUIRE(row[3].has_value());
        REQUIRE(row[4].has_value());
        REQUIRE(row[5].has_value());
        /* Alignment scores are legitimately zero for this tiny exact fixture, so
         * adjusted_score=1 is valid.  The direct-call score stub returns -1 and
         * would instead produce the excluded sentinel value zero. */
        REQUIRE(std::stoll(*row[2]) >= 1);
        /* query_end() and query_start() have the same BIGINT result type but
         * must occupy distinct executor-owned helper slots.  Aliasing helpers
         * by result type (or leaving the SQL stubs to execute) collapses this
         * positive aligned span to zero. */
        REQUIRE(std::stoll(*row[3]) > 0);
        /* has_cigar is both an unprunable Block Aligner consumer and a separate
         * no-stub guard: the SQL rendezvous stub returns false. */
        REQUIRE(*row[4] == "t");
        /* Identical query/target fixtures require BatchJoin to request and
         * publish the exact edit count instead of executing the SQL stub. */
        REQUIRE(std::stoll(*row[5]) == 0);
    }
    require_batch_join_stats(ctx, 1, 2);

    const auto wrapped_sequence_helper = sql_error(
        ctx,
        batch_join_sql(
            1,
            "bioseq.edit(COALESCE(t.seq0, t.seq0)) AS edit_distance"));
    REQUIRE(wrapped_sequence_helper.sqlstate == "0A000");
    REQUIRE(wrapped_sequence_helper.contains(
        "bioseq.edit projection must reference the mmsearch target column"));
}

TEST_CASE("BioseqBatchJoin base ProjectionPath guard is exact and fail closed",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context();

    /* SQL normally moves an unrelated target expression to an upper
     * projection, so an EXPLAIN-only negative does not exercise the dangerous
     * base ProjectionPath shape. This seam constructs PostgreSQL's real
     * PathTarget/PlaceHolderVar nodes and calls the production finder. */
    const auto guard_diagnostics = query_one(
        ctx,
        "SELECT accepts_exact_helper::text, "
        "rejects_unrelated_phv::text, "
        "rejects_volatile_mixed_expression::text, "
        "preserves_wrapper_cost_owner::text, "
        "accepts_plain_target_var::text, "
        "rejects_plain_target_sequence_var::text, "
        "rejects_whole_row_var::text, "
        "rejects_system_var::text, "
        "rejects_nulling_var::text "
        "FROM bioseq_internal.batch_join_projection_guard_probe();");
    REQUIRE(guard_diagnostics.size() == 9);
    for (const auto& field : guard_diagnostics) {
        REQUIRE(field.has_value());
        REQUIRE(*field == "true");
    }
}

TEST_CASE("BioseqBatchJoin headline metrics are one target crossed with 16 queries",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_batch_join_target});
    seed_batch_join_target(ctx);
    load_batch_join_queries(ctx, 16);

    const std::string zero_placeholder_sql =
        batch_join_sql(1, "count(*)::bigint");
    const auto zero_placeholder_plan =
        explain_text<false>(ctx, zero_placeholder_sql);
    REQUIRE(plan_contains(
        zero_placeholder_plan, "Custom Scan (BioseqBatchJoin)"));
    REQUIRE(plan_contains(
        zero_placeholder_plan, "Bioseq Batch ABI Version: 3"));
        /* count(*) has no match-result helper. NIL is the encoded empty placeholder
         * list. This shape elides base alignment, so only plan structure is checked. */
    REQUIRE(plan_contains(
        zero_placeholder_plan, "Bioseq Match Placeholder Sites: 0"));

    const std::string sql = batch_join_sql(
        1, "q.id, bioseq.has_cigar(t.pk) AS has_cigar");
    const auto plan = explain_text<false>(ctx, sql);
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqBatchJoin)"));
    REQUIRE(plan_contains(plan, "Bioseq Batch ABI Version: 3"));
    REQUIRE(plan_contains(plan, "Bioseq Match Placeholder Sites: 1"));
    /* Ownership is structural; all exposed cost metrics remain finite and
     * nonnegative. */
    REQUIRE(plan_contains(plan, "Bioseq Honest Startup Cost"));
    REQUIRE(plan_contains(plan, "Bioseq Honest Total Cost"));
    REQUIRE(plan_contains(plan, "Bioseq Incumbent Cost Floor"));
    const auto incumbent_floor = find_explain_property(
        plan, "Bioseq Incumbent Cost Floor");
    REQUIRE(incumbent_floor.has_value());
    REQUIRE(std::stod(*incumbent_floor) >= 0.0);
    require_batch_join_aligned_query_rows(ctx, sql, 16, "t");
    require_batch_join_stats(ctx, 1, 16);
}

#ifdef BIOSEQ_TEST_INDEXES
TEST_CASE("Ordinary BatchJoin retains an upper-tagged lazy query child",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({
        k_batch_join_target,
        k_batch_recipe_queries_32,
        k_batch_recipe_queries_64,
        k_batch_recipe_oracle_32,
        k_batch_recipe_oracle_64,
    });
    exec_sql(ctx,
        "SET bioseq.index.insert_maintenance.enabled=off;"
        "SET bioseq.index.async_persistence.enabled=off;"
        "SET bioseq.index.recompute_async_persistence.enabled=off;"
        "SET max_parallel_workers_per_gather=0;"
        "SET bioseq.aligner.chaining.minimum_score=20;"
        "SET bioseq.aligner.vulkan.enabled=off;");

    const std::string target = high_entropy_sequence(320u);
    const std::string logical_query = target.substr(80u, 96u);
    seed_batch_join_target(ctx, target);
    /* Build exactly the grouped-SoA minimizers_per_fragment payload used by
     * scalar and Exact-SIMD prepared-target routes. Query-source indexes remain
     * disabled. */
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT bioseq.update_index('{}'::regclass,"
        "'minimizers_per_fragment');", k_batch_join_target)) == 1);
    const std::int64_t target_rowno = query_scalar_int(ctx, fmt::format(
        "SELECT bioseq.column_row_number('{}'::regclass,ctid) "
        "FROM {} WHERE pk=1;", k_batch_join_target, k_batch_join_target));
    REQUIRE(target_rowno >= 0);
    const std::vector<seq_row> query_rows_fixture{
        seq_row{.pk = 1, .sequences = {logical_query}},
        seq_row{.pk = 2, .sequences = {logical_query}},
        seq_row{.pk = 3, .sequences = {logical_query}},
    };

    struct width_case {
        std::string_view name;
        std::string_view source_table;
        std::string_view oracle_table;
        std::uint8_t width = 0u;
    };
    constexpr std::array<width_case, 2u> width_cases{{
        {"query width 32", k_batch_recipe_queries_32,
         k_batch_recipe_oracle_32, 32u},
        {"query width 64", k_batch_recipe_queries_64,
         k_batch_recipe_oracle_64, 64u},
    }};
    for (const width_case& width_configuration : width_cases) {
        create_seq_table(
            ctx,
            seq_table_spec{
                .name = std::string{width_configuration.source_table},
                .typmod = {.alphabet = "dna4", .width = width_configuration.width},
                .seq_columns = 1,
                .using_bioseq = true,
            },
            query_rows_fixture);
        create_seq_table(
            ctx,
            seq_table_spec{
                .name = std::string{width_configuration.oracle_table},
                .typmod = {.alphabet = "dna4", .width = width_configuration.width},
                .seq_columns = 1,
                .using_bioseq = false,
            },
            query_rows_fixture);
    }

    struct algorithm_route {
        std::string_view name;
        std::string_view minimizer;
        std::string_view chaining;
        std::string_view base_alignment;
        std::string_view backend;
        bool consumes_persisted_prepared_target = false;
    };
    constexpr std::array<algorithm_route, 3u> routes{{
        {"scalar", "scalar", "scalar_minimap2", "ksw2", "scalar_cpu", true},
        {"Exact SIMD", "two_stack_nthash", "simd_minimap2",
         "block_aligner", "simd_cpu", true},
        {"bounded ntHash SIMD", "bounded_nthash_simd", "simd_minimap2",
         "block_aligner", "simd_cpu", false},
    }};
    struct scanner_mode {
        std::string_view name;
        bool force_inmemory = false;
        bool force_chunked = false;
    };
    constexpr std::array<scanner_mode, 3u> scanner_modes{{
        {"production", false, false},
        {"forced in-memory baseline", true, false},
        {"forced chunked baseline", false, true},
    }};

    const auto transformed_sql = [target_table = k_batch_join_target](
            const std::string_view query_table,
            const std::string_view limit_expression) {
        constexpr std::string_view recipe =
            "bioseq.reverse(bioseq.reverse("
            "q.seq0[0:48] OPERATOR(bioseq.||) q.seq0[48:96]))";
        return fmt::format(
            "SELECT q.pk, bioseq.len({0}), "
            "       {0} OPERATOR(bioseq.=) q.seq0, "
            "       bioseq.query_start(t.pk), bioseq.query_end(t.pk), "
            "       bioseq.target_start(t.pk), bioseq.target_end(t.pk), "
            "       bioseq.has_cigar(t.pk) "
            "FROM {1} AS t JOIN {2} AS q "
            "ON mmsearch(t.seq0, q.seq0, "
            " aligner_options(index_read_requirement=>0)) "
            "WHERE q.pk <= {3} ORDER BY q.pk, 4, 5, 6, 7",
            recipe, target_table, query_table, limit_expression);
    };
    const auto oracle_sql = [target_table = k_batch_join_target](
            const std::string_view query_table,
            const std::string_view limit_expression) {
        return fmt::format(
            "SELECT q.pk, bioseq.len(q.seq0), "
            "       q.seq0 OPERATOR(bioseq.=) q.seq0, "
            "       bioseq.query_start(t.pk), bioseq.query_end(t.pk), "
            "       bioseq.target_start(t.pk), bioseq.target_end(t.pk), "
            "       bioseq.has_cigar(t.pk) "
            "FROM {0} AS t JOIN {1} AS q "
            "ON mmsearch(t.seq0, q.seq0, "
            " aligner_options(index_read_requirement=>0)) "
            "WHERE q.pk <= {2} ORDER BY q.pk, 4, 5, 6, 7",
            target_table, query_table, limit_expression);
    };
    const auto require_ordinary_tagged_query_child = [](
            const std::span<const std::string> plan) {
        std::size_t batch_line = plan.size();
        std::size_t batch_indent = 0u;
        std::size_t slice_line = plan.size();
        std::size_t slice_indent = 0u;
        std::size_t slice_count = 0u;
        for (std::size_t index = 0u; index < plan.size(); ++index) {
            const std::string& line = plan[index];
            std::size_t indent = 0u;
            while (indent < line.size()
                   && std::isspace(static_cast<unsigned char>(line[indent]))) {
                ++indent;
            }
            if (line.find("Custom Scan (BioseqBatchJoin)")
                    != std::string::npos
                && batch_line == plan.size()) {
                batch_line = index;
                batch_indent = indent;
            }
            if (line.find("Custom Scan (BioseqSliceProject)")
                    != std::string::npos) {
                ++slice_count;
                if (slice_line == plan.size()) {
                    slice_line = index;
                    slice_indent = indent;
                }
            }
        }
        REQUIRE(batch_line < plan.size());
        REQUIRE(slice_count == 1u);
        /* A two-baserel PlannerInfo cannot install a base-local SliceProject.
         * The final-output recipe therefore has one upper owner; its private
         * path walk tags q's BioseqScan so BatchJoin receives and retains the
         * source-backed expanded Datum before the upper recipe is evaluated. */
        REQUIRE(slice_line < batch_line);
        REQUIRE(slice_indent < batch_indent);
        REQUIRE(plan_contains(plan, "Bioseq Batch ABI Version: 3"));
        REQUIRE_FALSE(plan_contains(
            plan, "Bioseq Mmsearch Ownership: per-row recipe"));
        REQUIRE_FALSE(plan_contains(plan, "Join Filter: mmsearch"));
    };
    const auto reset_payload_probe = [&ctx] {
        exec_sql(ctx,
            "SELECT bioseq_internal.slice_payload_allocation_probe(0);");
    };
    const auto require_streaming_query_child = [&ctx] {
        REQUIRE(mmsearch_source_counter(
            ctx, "scan_providers_created") == 2);
        REQUIRE(mmsearch_source_counter(
            ctx, "facade_source_stream_callbacks") > 0);
        REQUIRE(mmsearch_source_counter(
            ctx, "facade_declared_materializations") == 0);
        REQUIRE(mmsearch_source_counter(
            ctx, "facade_completed_materializations") == 0);
        REQUIRE(query_scalar_int(ctx,
            "SELECT bioseq_internal.slice_payload_allocation_probe(1);")
            == 0);
        REQUIRE(query_scalar_int(ctx,
            "SELECT bioseq_internal.slice_payload_allocation_probe(2);")
            == 0);
        require_mmsearch_source_resources_balanced(ctx);
    };
    const auto prepared_diagnostics = [&ctx, target_rowno](
            const algorithm_route& route,
            const long long expected_jobs) {
        const row_t diagnostics = query_one(ctx,
            "SELECT cpu_prepared_index_builds::text, "
            "       cpu_prepared_index_reuses::text, "
            "       cpu_prepared_unique_keys::text, "
            "       cpu_prepared_duplicate_build_attempts::text, "
            "       cpu_prepared_key_set_checksum, "
            "       cpu_prepared_build_set_checksum, "
            "       requested_backend, actual_backend, "
            "       attempted_alignment_jobs::text, "
            "       fully_attested_alignment_jobs::text, "
            "       unattested_alignment_jobs::text, "
            "       requested_target_index_source, "
            "       actual_target_index_source, "
            "       persisted_generation_id::text, "
            "       persisted_index_rows::text, "
            "       missing_persisted_index_rows::text, "
            "       recomputed_target_index_rows::text, "
            "       first_unattested_target_row::text, "
            "       first_unattested_query_ordinal::text "
            "FROM bioseq.last_alignment_execution_stats() "
            "WHERE stage='query_sketch';");
        REQUIRE(diagnostics.size() == 19u);
        constexpr std::array<std::string_view, 19u> diagnostic_fields{{
            "cpu_prepared_index_builds",
            "cpu_prepared_index_reuses",
            "cpu_prepared_unique_keys",
            "cpu_prepared_duplicate_build_attempts",
            "cpu_prepared_key_set_checksum",
            "cpu_prepared_build_set_checksum",
            "requested_backend",
            "actual_backend",
            "attempted_alignment_jobs",
            "fully_attested_alignment_jobs",
            "unattested_alignment_jobs",
            "requested_target_index_source",
            "actual_target_index_source",
            "persisted_generation_id",
            "persisted_index_rows",
            "missing_persisted_index_rows",
            "recomputed_target_index_rows",
            "first_unattested_target_row",
            "first_unattested_query_ordinal",
        }};
        for (std::size_t field = 0u; field < diagnostics.size(); ++field) {
            INFO(fmt::format(
                "prepared diagnostic field {} ({})={}",
                field, diagnostic_fields[field], diagnostics[field].value_or("NULL")));
            if (field != 5u) REQUIRE(diagnostics[field].has_value());
        }
        REQUIRE(std::stoll(*diagnostics[2]) > 0);
        REQUIRE(diagnostics[3] == std::optional<std::string>{"0"});
        /* The build-set identity is published only after every prepared
         * parameter group has built a target index.  Bounded ntHash rejects the
         * incompatible minimizers_per_fragment payload, so its query-union identity remains
         * valid while target-build activity is intentionally absent. */
        if (route.consumes_persisted_prepared_target) {
            REQUIRE(diagnostics[0] == std::optional<std::string>{"1"});
            REQUIRE(std::stoll(*diagnostics[1]) == expected_jobs - 1);
            REQUIRE(diagnostics[5].has_value());
            REQUIRE(diagnostics[5] == diagnostics[4]);
        } else {
            REQUIRE(diagnostics[0] == std::optional<std::string>{"0"});
            REQUIRE(diagnostics[1] == std::optional<std::string>{"0"});
            REQUIRE_FALSE(diagnostics[5].has_value());
        }
        REQUIRE(diagnostics[6] == std::optional<std::string>{
            std::string{route.backend}});
        REQUIRE(diagnostics[7] == std::optional<std::string>{
            std::string{route.backend}});
        REQUIRE(std::stoll(*diagnostics[8]) == expected_jobs);
        REQUIRE(diagnostics[9] == std::optional<std::string>{"0"});
        REQUIRE(std::stoll(*diagnostics[10]) == expected_jobs);
        REQUIRE(std::stoll(*diagnostics[9]) + std::stoll(*diagnostics[10])
                == std::stoll(*diagnostics[8]));
        REQUIRE(diagnostics[11] == std::optional<std::string>{"persisted"});
        REQUIRE(diagnostics[13] == std::optional<std::string>{"0"});
        if (route.consumes_persisted_prepared_target) {
            /* The persisted reader records real prepared build/reuse work but
             * has no READY-generation/source identity, so job attestation fails
             * closed even when every selected CPU stage was accepted. */
            REQUIRE(diagnostics[12] == std::optional<std::string>{"persisted"});
            REQUIRE(diagnostics[14] == std::optional<std::string>{"1"});
            REQUIRE(diagnostics[15] == std::optional<std::string>{"0"});
            REQUIRE(diagnostics[16] == std::optional<std::string>{"0"});
        } else {
            /* Bounded ntHash cannot consume the incompatible
             * minimizers_per_fragment payload. The
             * allow/read-on request is persisted while execution recomputes,
             * and the requested/actual source mismatch is intentionally
             * unattested rather than laundered as a prepared build. */
            REQUIRE(diagnostics[12] == std::optional<std::string>{"recomputed"});
            REQUIRE(diagnostics[14] == std::optional<std::string>{"0"});
            REQUIRE(diagnostics[15] == std::optional<std::string>{"1"});
            REQUIRE(diagnostics[16] == std::optional<std::string>{"1"});
        }
        REQUIRE(std::stoll(*diagnostics[17]) == target_rowno);
        REQUIRE(diagnostics[18] == std::optional<std::string>{"0"});
        return std::pair{*diagnostics[4], diagnostics[5]};
    };

    exec_sql(ctx,
        "SET bioseq.index.sequence_read.enabled=on;"
        "SET plan_cache_mode=force_generic_plan;");
    for (const width_case& width_configuration : width_cases) {
        INFO(fmt::format("query width: {}", width_configuration.name));
        for (const algorithm_route& route : routes) {
            INFO(fmt::format("algorithm route: {}", route.name));
            exec_sql(ctx, fmt::format(
                "SET bioseq.aligner.minimizer.algorithm='{}';"
                "SET bioseq.aligner.chaining.algorithm='{}';"
                "SET bioseq.aligner.base_alignment.algorithm='{}';",
                route.minimizer, route.chaining,
                route.base_alignment));
            const rowset_t expected_two = query_rows(
                ctx, oracle_sql(width_configuration.oracle_table, "2"));
            const rowset_t expected_three = query_rows(
                ctx, oracle_sql(width_configuration.oracle_table, "3"));
            REQUIRE_FALSE(expected_two.empty());
            REQUIRE_FALSE(expected_three.empty());

            for (const scanner_mode& active_scanner_mode : scanner_modes) {
                INFO(fmt::format(
                    "scanner mode: {}", active_scanner_mode.name));
                exec_sql(ctx, fmt::format(
                    "SET bioseq.testing.in_memory_scanner.enabled={};"
                    "SET bioseq.testing.chunked_scanner.enabled={};",
                    active_scanner_mode.force_inmemory ? "on" : "off",
                    active_scanner_mode.force_chunked ? "on" : "off"));
                reset_mmsearch_source_counters(ctx);
                reset_payload_probe();
                const rowset_t actual = query_rows(
                    ctx,
                    transformed_sql(width_configuration.source_table, "3"));
                REQUIRE(actual == expected_three);
                require_streaming_query_child();
            }

            exec_sql(ctx,
                "SET bioseq.testing.in_memory_scanner.enabled=off;"
                "SET bioseq.testing.chunked_scanner.enabled=off;"
                "DEALLOCATE ALL;");
            constexpr std::string_view statement_name =
                "batch_recipe_plan";
            exec_sql(ctx, fmt::format(
                "PREPARE {} (int) AS {};",
                statement_name,
                transformed_sql(width_configuration.source_table, "$1")));
            const auto plan = explain_text<false>(ctx, fmt::format(
                "EXECUTE {}(3)", statement_name));
            require_ordinary_tagged_query_child(plan);

            reset_mmsearch_source_counters(ctx);
            reset_payload_probe();
            const rowset_t first = query_rows(ctx, fmt::format(
                "EXECUTE {}(2);", statement_name));
            REQUIRE(first == expected_two);
            const auto first_checksums = prepared_diagnostics(route, 2);
            require_streaming_query_child();

            reset_mmsearch_source_counters(ctx);
            reset_payload_probe();
            const rowset_t expanded = query_rows(ctx, fmt::format(
                "EXECUTE {}(3);", statement_name));
            REQUIRE(expanded == expected_three);
            const auto expanded_checksums = prepared_diagnostics(route, 3);
            REQUIRE(expanded_checksums == first_checksums);
            require_streaming_query_child();

            reset_mmsearch_source_counters(ctx);
            reset_payload_probe();
            const rowset_t replay = query_rows(ctx, fmt::format(
                "EXECUTE {}(2);", statement_name));
            REQUIRE(replay == first);
            REQUIRE(prepared_diagnostics(route, 2) == first_checksums);
            require_streaming_query_child();
            exec_sql(ctx, "DEALLOCATE ALL;");
        }
    }

    /* One exact-SIMD, cross-width execution forces ERROR during the query
     * recipe's first source callback.  The prepared plan, read session,
     * retained expanded attributes, permits, and provider leases must unwind
     * together, and the same generic plan must be reusable immediately. */
    exec_sql(ctx,
        "SET bioseq.aligner.minimizer.algorithm='two_stack_nthash';"
        "SET bioseq.aligner.chaining.algorithm='simd_minimap2';"
        "SET bioseq.aligner.base_alignment.algorithm='block_aligner';"
        "SET bioseq.testing.in_memory_scanner.enabled=off;"
        "SET bioseq.testing.chunked_scanner.enabled=off;"
        "DEALLOCATE ALL;");
    constexpr std::string_view failure_statement =
        "batch_recipe_failure";
    exec_sql(ctx, fmt::format(
        "PREPARE {} (int) AS {};",
        failure_statement,
        transformed_sql(k_batch_recipe_queries_64, "$1")));
    reset_mmsearch_source_counters(ctx);
    reset_payload_probe();
    exec_sql(ctx,
        "SELECT bioseq_internal.arm_sequence_source_failure("
        " 'read_before_scanner');");
    const auto injected = sql_error(ctx, fmt::format(
        "EXECUTE {}(3);", failure_statement));
    REQUIRE(injected.sqlstate == "XX000");
    REQUIRE(injected.contains(
        "injected PostgreSQL sequence source-domain failure"));
    REQUIRE(mmsearch_source_counter(ctx, "fault_injections") == 1);
    REQUIRE(mmsearch_source_counter(ctx, "read_fault_injections") == 1);
    REQUIRE(query_scalar_int(ctx,
        "SELECT bioseq_internal.slice_payload_allocation_probe(1);") == 0);
    REQUIRE(query_scalar_int(ctx,
        "SELECT bioseq_internal.slice_payload_allocation_probe(2);") == 0);
    require_mmsearch_source_resources_balanced(ctx);
    exec_sql(ctx,
        "SELECT bioseq_internal.disarm_sequence_source_failure();");
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);

    const rowset_t recovered_oracle = query_rows(ctx, oracle_sql(
        k_batch_recipe_oracle_64, "3"));
    reset_mmsearch_source_counters(ctx);
    reset_payload_probe();
    const rowset_t recovered = query_rows(ctx, fmt::format(
        "EXECUTE {}(3);", failure_statement));
    REQUIRE(recovered == recovered_oracle);
    const auto recovered_checksums = prepared_diagnostics(routes[1], 3);
    REQUIRE(recovered_checksums.first == recovered_checksums.second);
    require_streaming_query_child();
    exec_sql(ctx, "DEALLOCATE ALL;");
}
#endif

TEST_CASE("BioseqBatchJoin dispatches exactly one full 8192-query wave",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_batch_join_target});
    seed_batch_join_target(ctx);
    load_batch_join_queries(ctx, 8192);

    const std::string sql = batch_join_sql(
        1, "q.id, bioseq.has_cigar(t.pk) AS has_cigar");
    require_batch_join_aligned_query_rows(ctx, sql, 8192, "t");
    require_batch_join_stats(ctx, 1, 8192);
}

TEST_CASE("cpu_prepared_target drops oversized capacity before same-object reprepare",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_batch_join_target});
    REQUIRE(query_scalar(
        ctx,
        "SELECT bioseq_internal."
        "cpu_prepared_target_same_object_reprepare_probe();") == "t");
}

TEST_CASE("BioseqBatchJoin resumes after 8192 and merges the one-row tail",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_batch_join_target});
    seed_batch_join_target(ctx);
    load_batch_join_queries(ctx, 8193);
    const auto reserved_bytes = [&ctx] {
        return query_scalar_int(ctx,
            "SELECT index_memory_reserved_bytes FROM bioseq.scheduler_stats();");
    };
    const long long admission_baseline = reserved_bytes();

    const std::string sql = batch_join_sql(
        1, "q.id, bioseq.has_cigar(t.pk) AS has_cigar");
    require_batch_join_aligned_query_rows(ctx, sql, 8193, "t");
    require_batch_join_stats(ctx, 1, 8193);
    REQUIRE(reserved_bytes() == admission_baseline);

    const auto analyzed = explain_text<true>(ctx, sql);
    REQUIRE(plan_contains(analyzed, "Custom Scan (BioseqBatchJoin)"));
    REQUIRE(plan_contains(analyzed, "Bioseq Query Batches: 2"));
    REQUIRE(plan_contains(
        analyzed, "attempted=8193 fully_attested=8193 unattested=0"));

    // A later much smaller preparation in the same backend must neither retain
    // nor inherit the 8193-query union's outer/inner vector capacities.
    exec_sqlf(ctx, "DROP TABLE {};", k_batch_join_queries);
    load_batch_join_queries(ctx, 1);
    require_batch_join_aligned_query_rows(
        ctx,
        batch_join_sql(1, "q.id, bioseq.has_cigar(t.pk) AS has_cigar"),
        1,
        "t");
    REQUIRE(reserved_bytes() == admission_baseline);
}

TEST_CASE("BioseqBatchJoin emits a match after a zero-match full first wave",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_batch_join_target});
    seed_batch_join_target(ctx);
    load_batch_join_queries(ctx, 8193, /*only_last_can_match=*/true);

    const auto rows = query_rows(
        ctx,
        batch_join_sql(
            1,
            "DISTINCT q.id, bioseq.has_cigar(t.pk) AS has_cigar")
            + " ORDER BY q.id;");
    REQUIRE_FALSE(rows.empty());
    for (const auto& row : rows) {
        REQUIRE(row.size() == 2);
        REQUIRE(row[0].has_value());
        REQUIRE(row[1].has_value());
        REQUIRE(std::stoll(*row[0]) == 8192);
        REQUIRE(*row[1] == "t");
    }

    /* The first wave deliberately uses 10-bp queries, shorter than k=15. They
     * cannot produce the non-empty sketch/chain/Block activity required for a
     * simd_cpu attestation, so the aggregate must stay diagnostic rather than
     * laundering those 8192 jobs. The final eligible query still has to carry
     * positive end-to-end SIMD counters and zero fallback counters. */
    const auto stats = last_alignment_stats(ctx);
    REQUIRE(stats.size() == 20);
    REQUIRE(stats.front()[16].value_or("") == "simd_cpu");
    REQUIRE(stats.front()[17].value_or("") == "mixed_invalid");
    REQUIRE(stats.front()[18].value_or("") == "recomputed");
    REQUIRE(stats.front()[19].value_or("") == "recomputed");
    REQUIRE(stats_i64(stats.front(), 32) == 1);
    REQUIRE(stats_i64(stats.front(), 33) == 8193);
    REQUIRE(stats_i64(stats.front(), 34) == 0);
    REQUIRE(stats_i64(stats.front(), 35) == 8193);
    REQUIRE(stats_i64(stats.front(), 37) == 0);
    REQUIRE(stats_i64(stats.front(), 45) == 0);
    REQUIRE(stats_i64(stats.front(), 46) == 0);
    REQUIRE(stats_i64(stats.front(), 47) == 1);
    REQUIRE(stats_i64(stats.front(), 94) > 0);
    REQUIRE(stats_i64(stats.front(), 95) == 0);
    REQUIRE(stats_i64(stats.front(), 96) == 0);
    REQUIRE(stats_i64(stats.front(), 98) > 0);
    REQUIRE(stats_i64(stats.front(), 99) > 0);
    REQUIRE(stats_i64(stats.front(), 100) == 0);
    REQUIRE(stats_i64(stats.front(), 101) == 0);
    for (std::size_t gpu_counter = 103; gpu_counter <= 138; ++gpu_counter) {
        REQUIRE(stats_i64(stats.front(), gpu_counter) == 0);
    }
    REQUIRE(stats_i64(stats.front(), 139) == 0);
    REQUIRE(stats_i64(stats.front(), 140) == 0);
    REQUIRE_FALSE(stats_bool(stats.front(), 141));
    for (const std::size_t stage_no : {std::size_t{0}, std::size_t{1},
             std::size_t{7}, std::size_t{8}}) {
        REQUIRE(stats_i64(stats[stage_no], 24) == 1);
        REQUIRE(stats_i64(stats[stage_no], 25) == 0);
        REQUIRE(stats_i64(stats[stage_no], 26) == 0);
        REQUIRE(stats_i64(stats[stage_no], 27) == 1);
        REQUIRE(stats_i64(stats[stage_no], 28) == 0);
        REQUIRE(stats_i64(stats[stage_no], 29) == 0);
    }
    long long tail_block_simd_executions = 0;
    for (std::size_t stage_no = 17; stage_no <= 19; ++stage_no) {
        REQUIRE(stats_i64(stats[stage_no], 26) == 0);
        REQUIRE(stats_i64(stats[stage_no], 27)
            == stats_i64(stats[stage_no], 24));
        REQUIRE(stats_i64(stats[stage_no], 28) == 0);
        REQUIRE(stats_i64(stats[stage_no], 29) == 0);
        tail_block_simd_executions += stats_i64(stats[stage_no], 27);
    }
    REQUIRE(tail_block_simd_executions >= 1);
}

TEST_CASE("plain EXPLAIN exposes live CPU just-index BatchJoin before missing-generation failure",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_batch_join_target});
    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
    seed_batch_join_target(ctx);
    load_batch_join_queries(ctx, 2);
    const std::string sql = batch_join_sql(2, "q.id, t.pk");

    const auto plan = explain_text<false>(ctx, sql);
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqBatchJoin)"));
    REQUIRE(plan_contains(plan, "Bioseq Residual Mmsearch Qual: absent"));
    REQUIRE(plan_contains(
        plan,
        "Bioseq Index Read Requirement: require_compatible_persisted"));
    REQUIRE(plan_contains(plan, "Bioseq Requirement Executable: true"));
    require_no_current_compatible_persisted_index(ctx, sql + ";");
    require_no_current_compatible_persisted_index(
        ctx, "EXPLAIN (ANALYZE, COSTS OFF) " + sql + ";");
}
