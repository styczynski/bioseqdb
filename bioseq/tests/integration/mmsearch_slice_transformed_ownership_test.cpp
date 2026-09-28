/**
 * @file mmsearch_slice_transformed_ownership_test.cpp
 * @brief Tests mmsearch on slices and sequences built from SQL expressions.
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

namespace {
/** Recognize GPU seed and chain dispatches with no GPU backtracking or extension */
[[nodiscard]] bool is_completed_vulkan_dispatch_line(
        const std::string_view line) {
    const auto has_positive_counter = [line](const std::string_view marker) {
        const std::size_t marker_offset = line.find(marker);
        if (marker_offset == std::string_view::npos) {
            return false;
        }
        const std::size_t value_offset = marker_offset + marker.size();
        return value_offset < line.size()
            && line[value_offset] >= '1' && line[value_offset] <= '9';
    };
    return has_positive_counter("* GPU dispatch [seed=")
        && has_positive_counter(" chain=")
        && line.contains(
            " chain_backtrack=0 coordinate_vote=0 extend=0 "
            "backend_declines=0 backend_fallbacks=0]");
}

/** Check GPU dispatch and execution labels for the expected custom scan. */
void require_completed_vulkan_host_suffix(
        const std::span<const std::string> plan,
        const std::string_view expected_owner) {
    auto dispatch = plan.end();
    auto owner = plan.end();
    for (auto candidate = plan.begin(); candidate != plan.end(); ++candidate) {
        if (!is_completed_vulkan_dispatch_line(*candidate)) {
            continue;
        }
        auto candidate_owner = plan.end();
        for (auto line = candidate; line != plan.begin();) {
            --line;
            if (line->contains("Custom Scan (")) {
                candidate_owner = line;
                break;
            }
        }
        if (candidate_owner != plan.end()
                && candidate_owner->contains(expected_owner)) {
            dispatch = candidate;
            owner = candidate_owner;
            break;
        }
    }
    REQUIRE(dispatch != plan.end());
    REQUIRE(owner != plan.end());

    auto execution_identity = plan.end();
    for (auto line = dispatch; line != owner;) {
        --line;
        if (line->contains("* Execution statistics [")) {
            execution_identity = line;
            break;
        }
    }
    REQUIRE(execution_identity != plan.end());
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    REQUIRE(execution_identity->contains(
        "requested_backend=hybrid_cpu_vulkan "
        "actual_backend=hybrid_cpu_vulkan"));
    REQUIRE(execution_identity->contains(
        "attempted=1 fully_attested=1 unattested=0"));
#else
    REQUIRE(execution_identity->contains(
        "requested_backend=mixed_invalid actual_backend=mixed_invalid"));
    REQUIRE(execution_identity->contains(
        "attempted=1 fully_attested=0 unattested=1"));
#endif
    REQUIRE(plan_contains(plan, "| chain_stage: Vulkan tiled DP"));
    REQUIRE(plan_contains(plan, "| align_stage: Block Aligner"));
    REQUIRE(plan_contains(
        plan, "| mapq_stage: Compute mapping quality (mm_set_mapq2)"));
    REQUIRE_FALSE(plan_contains(plan, "GPU CIGAR"));
    REQUIRE_FALSE(plan_contains(plan, "GPU MAPQ"));
}
}  // namespace

TEST_CASE("target base mmsearch keeps ordinary parameterized scan ownership",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    /* The target has both a base filter  and a join search. Keep BioseqScan
     * responsible for both. Query 0 cannot match, so it must be removed even
     * though the target passes the base filter. */
    auto ctx = fresh_context({k_batch_join_target});
    seed_batch_join_target(ctx);
    load_batch_join_queries(ctx, 2, /*only_last_can_match=*/true);

    const std::string sql = batch_join_sql(1, "q.id, t.pk")
        + fmt::format(
            " WHERE mmsearch(t.seq0, {})",
            batch_join_target_literal());
    const auto plan = explain_text<false>(ctx, sql);
    REQUIRE_FALSE(plan_contains(plan, "Custom Scan (BioseqBatchJoin)"));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqScan)"));

    const auto rows = query_rows(ctx, sql + " ORDER BY q.id, t.pk;");
    REQUIRE(rows.size() == 1);
    REQUIRE(rows.front().size() == 2);
    REQUIRE(rows.front()[0].has_value());
    REQUIRE(rows.front()[1].has_value());
    REQUIRE(std::stoll(*rows.front()[0]) == 1);
    REQUIRE(std::stoll(*rows.front()[1]) == 1);
}

TEST_CASE("BatchJoin cursor publication waits for portal finish and stays partial",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_batch_join_target});
    seed_batch_join_target(ctx);
    load_batch_join_queries(ctx, 4);

    (void)query_rows(ctx, index_requirement_sql(k_batch_join_target, 1));
    const long long prior_id = stats_i64(last_alignment_stats(ctx).front(), 0);

    exec_sql(ctx, "BEGIN;");
    exec_sql(
        ctx,
        "DECLARE batch_join_cursor NO SCROLL CURSOR FOR "
        + batch_join_sql(1, "q.id, t.pk") + " ORDER BY q.id, t.pk;");
    REQUIRE(query_rows(ctx, "FETCH 1 FROM batch_join_cursor;").size() == 1);
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
    REQUIRE(stats_i64(last_alignment_stats(ctx).front(), 0) == prior_id);
    exec_sql(ctx, "CLOSE batch_join_cursor;");
    exec_sql(ctx, "COMMIT;");

    const auto stats = last_alignment_stats(ctx);
    REQUIRE(stats.size() == 20);
    REQUIRE(stats_i64(stats.front(), 0) == prior_id + 1);
    REQUIRE_FALSE(stats_bool(stats.front(), 13));
    REQUIRE(stats_bool(stats.front(), 14));
    REQUIRE_FALSE(stats_bool(stats.front(), 15));
}

TEST_CASE("nested SPI BatchJoin attaches once to its executor root",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_batch_join_target});
    seed_batch_join_target(ctx);
    load_batch_join_queries(ctx, 2);
    exec_sql(
        ctx,
        "CREATE OR REPLACE FUNCTION pg_temp.run_batch_join_spi() "
        "RETURNS bigint LANGUAGE plpgsql AS $$ "
        "DECLARE n bigint; BEGIN "
        "SELECT count(*)::bigint INTO n FROM mmsearch_batch_join_target_int t "
        "JOIN mmsearch_batch_join_queries_int q "
        "ON mmsearch(t.seq0, q.seq, "
        "aligner_options(index_read_requirement => 1)); "
        "RETURN n; END $$;");
    REQUIRE(query_scalar_int(
        ctx, "SELECT pg_temp.run_batch_join_spi();") > 0);

    const auto stats = last_alignment_stats(ctx);
    REQUIRE(stats.size() == 20);
    REQUIRE(stats_i64(stats.front(), 11) == 1);
    REQUIRE(stats_i64(stats.front(), 12) == 1);
    REQUIRE(stats_i64(stats.front(), 32) == 1);
    REQUIRE(stats_i64(stats.front(), 33) == 2);
    REQUIRE(stats_i64(stats.front(), 45) == 0);
    REQUIRE(stats_i64(stats.front(), 46) == 0);
    REQUIRE(stats_i64(stats.front(), 47) == 1);
    REQUIRE(stats_bool(stats.front(), 13));
}

TEST_CASE("GPU target outcomes count logical rows with identical sequence content",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_duplicate_targets});
    const std::string identical_target =
        hoe_seed_rows().front().sequences.front();
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string{k_duplicate_targets},
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 1,
            .using_bioseq = true,
        },
        {
            seq_row{.pk = 1, .sequences = {identical_target}},
            seq_row{.pk = 2, .sequences = {identical_target}},
        });
    exec_sql(ctx, "SET bioseq.index.sequence_read.enabled = off;");
    exec_sql(ctx, "SET bioseq.aligner.base_alignment.algorithm = 'block_aligner';");
    exec_sql(ctx, "SET bioseq.aligner.chaining.algorithm = 'vulkan_tiled_dynamic_programming';");
    exec_sql(ctx, "SET bioseq.aligner.minimizer.algorithm = 'vulkan_minimap_integer';");
    exec_sql(ctx, "SET bioseq.aligner.vulkan.enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan.seed_lookup.enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan.index_cache.enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan.fused_frontend.enabled = off;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan.index_build.enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan.alignment_extension.enabled = off;");
    exec_sql(ctx, "SET bioseq.aligner.chaining.minimum_score = 20;");
    exec_sql(ctx, "SET bioseq.aligner.chaining.minimum_seed_count = 1;");
    const std::string sql = fmt::format(
        "SELECT pk, bioseq.cigar(pk), bioseq.has_cigar(pk), bioseq.mapq(pk) "
        "FROM {} WHERE mmsearch(seq0, {}) ORDER BY pk;",
        k_duplicate_targets,
        hoe_probe_literal());
    if (selected_vulkan_runtime_untrusted(ctx)) {
        const long long reserved_before = query_scalar_int(
            ctx,
            "SELECT index_memory_reserved_bytes "
            "FROM bioseq.scheduler_stats();");
        const auto err = sql_error(ctx, sql);
        REQUIRE(err.sqlstate == "XX000");
        REQUIRE(err.contains("Vulkan aligner execution failed"));
        REQUIRE(query_scalar_int(
            ctx,
            "SELECT index_memory_reserved_bytes "
            "FROM bioseq.scheduler_stats();") == reserved_before);
        return;
    }

    const auto plan = explain_text<true>(ctx, sql);
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    REQUIRE(plan_contains(
        plan, "attempted=2 fully_attested=2 unattested=0"));
#else
    // This build reports fully_attested=0 for hybrid execution. Check the
    // GPU dispatch counters below.
    REQUIRE(plan_contains(
        plan, "attempted=2 fully_attested=0 unattested=2"));
#endif
    REQUIRE(plan_contains(plan, "| chain_stage: Vulkan tiled DP"));
    REQUIRE(plan_contains(plan, "| align_stage: Block Aligner"));
    REQUIRE(plan_contains(
        plan, "| mapq_stage: Compute mapping quality (mm_set_mapq2)"));
    REQUIRE_FALSE(plan_contains(plan, "GPU CIGAR"));
    REQUIRE_FALSE(plan_contains(plan, "GPU MAPQ"));
    REQUIRE(plan_contains(plan, "Index outcomes [targets=2"));
    REQUIRE(plan_contains(plan, "recomputed=2]"));
    REQUIRE(plan_contains(plan, "GPU dispatch [seed="));
    REQUIRE_FALSE(plan_contains(plan, "GPU dispatch [seed=0 "));
    REQUIRE_FALSE(plan_contains(plan, " chain=0 chain_backtrack="));
    REQUIRE(plan_contains(plan, " chain_backtrack=0 "));
    const auto tiled_chain_dispatches =
        find_explain_property(plan, "GPU Chain Dispatches");
    const auto tiled_backtrack_dispatches =
        find_explain_property(plan, "GPU Chain Backtrack Dispatches");
    const auto coordinate_vote_dispatches =
        find_explain_property(plan, "GPU Coordinate Vote Dispatches");
    REQUIRE(tiled_chain_dispatches.has_value());
    REQUIRE(std::stoull(*tiled_chain_dispatches) > 0u);
    REQUIRE(tiled_backtrack_dispatches.has_value());
    REQUIRE(std::stoull(*tiled_backtrack_dispatches) == 0u);
    REQUIRE(coordinate_vote_dispatches.has_value());
    REQUIRE(std::stoull(*coordinate_vote_dispatches) == 0u);
    REQUIRE(plan_contains(plan, "extend=0"));
    REQUIRE(plan_contains(
        plan, "backend_declines=0 backend_fallbacks=0"));
}

TEST_CASE("USING bioseq table without mmsearch qual scans all rows",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_hoe});
    seed_hoe_fixture(ctx, k_hoe);

    // A, scan without mmsearch must return all five fixture rows.
    REQUIRE(query_scalar_int(
                ctx, fmt::format("SELECT COUNT(*)::bigint FROM {};", k_hoe)) ==
            5);
}

TEST_CASE(
    "mmsearch result set is byte-identical across the full GUC parity matrix",
    "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_hoe});
    seed_hoe_fixture(ctx, k_hoe);

    const std::string sql = fmt::format(
        "SELECT pk, "
        "       bioseq.query_start(pk)  AS qs, "
        "       bioseq.query_end(pk)    AS qe, "
        "       bioseq.target_start(pk) AS ts, "
        "       bioseq.target_end(pk)   AS te, "
        "       bioseq.cigar(pk)        AS cg "
        "FROM {} "
        "WHERE mmsearch(seq0, {}) "
        "ORDER BY pk;",
        k_hoe,
        hoe_probe_literal());

    const auto reference = expect_query_invariant_under_options(
        ctx, sql,
        {
            {"bioseq.vectorization.enabled",     {"on", "off"}},
            {"bioseq.column_cache.enabled",      {"on", "off"}},
            {"bioseq.planner.index_scan.enabled", {"on", "off"}},
            {"bioseq.aligner.profiling.enabled", {"on", "off"}},
        });

    // Check the syntax of esch nonempty CIGAR string.
    for (const auto& row : reference) {
        REQUIRE(row.size() == 6);
        if (row[5].has_value() && !row[5].value().empty()) {
            REQUIRE(is_valid_cigar(row[5].value()));
        }
    }
}

TEST_CASE("Sliced concat reverse mmsearch preserves coordinates across GUCs",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_hoe, k_hoe_lazy_no_insert});
    seed_hoe_fixture(ctx, k_hoe);
    exec_sql(ctx, fmt::format(
        "CREATE TEMP TABLE {} "
        "(id int PRIMARY KEY, seq bioseq.seq('dna4',32));",
        k_lazy_queries));
    exec_sql(ctx, fmt::format(
        "INSERT INTO {} VALUES (1, {});",
        k_lazy_queries, hoe_probe_literal()));
    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string{k_hoe_lazy_no_insert},
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 2,
            .using_bioseq = true,
        },
        hoe_seed_rows());

    for (const std::string_view table :
         std::array{k_hoe, k_hoe_lazy_no_insert}) {
        /* Recompute the flat-target baseline so a stored index cannot change
         * its minimizer settings. Compare that result with the sequence  built
         * from slices, concatenation, and two reversals. */
        const std::string plain = eager_mmsearch_coordinate_sql(table);
        const std::string transformed = transformed_mmsearch_sql(table);
        const auto expected = query_rows(ctx, plain);
        const auto plan = explain_text<false>(ctx, transformed);
        REQUIRE(plan_contains(plan, "Custom Scan (BioseqBatchJoin)"));
        REQUIRE(plan_contains(
            plan, "Bioseq Mmsearch Ownership: per-row recipe"));
        REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
        REQUIRE_FALSE(plan_contains(plan, "Join Filter: mmsearch"));

        const auto actual = expect_query_invariant_under_options(
            ctx, transformed,
            {
                {"bioseq.vectorization.enabled", {"on", "off"}},
                {"bioseq.column_cache.enabled", {"on", "off"}},
                {"bioseq.index.sequence_read.enabled", {"on", "off"}},
                {"bioseq.index.async_persistence.enabled", {"on", "off"}},
                {"bioseq.index.recompute_async_persistence.enabled", {"on", "off"}},
            });
        REQUIRE(actual == expected);
        REQUIRE_FALSE(actual.empty());
        for (const auto& row : actual) {
            REQUIRE(row.size() == 6);
            REQUIRE(row[1].has_value());
            REQUIRE(row[2].has_value());
            REQUIRE(row[3].has_value());
            REQUIRE(row[4].has_value());
            REQUIRE(std::stoll(*row[1]) <= std::stoll(*row[2]));
            REQUIRE(std::stoll(*row[3]) <= std::stoll(*row[4]));
            if (row[5].has_value() && !row[5]->empty()) {
                REQUIRE(is_valid_cigar(*row[5]));
            }
        }
    }
}

TEST_CASE("Base-local sliced mmsearch matches a flat logical oracle",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context(
        {k_transform_target, k_oracle_target});
    const std::string source = high_entropy_sequence(384u);
    const std::string logical_target = source.substr(1u);
    constexpr std::size_t query_source_offset = 97u;
    constexpr std::size_t query_length = 96u;
    const std::string query = source.substr(
        query_source_offset, query_length);
    const std::string query_literal = format_literal(
        query, seq_typmod{.alphabet = "dna4", .width = 32});

    exec_sql(ctx,
        "SET max_parallel_workers_per_gather=0;"
        "SET bioseq.index.insert_maintenance.enabled=off;"
        "SET bioseq.index.sequence_read.enabled=on;"
        "SET bioseq.aligner.chaining.minimum_score=20;");
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string{k_transform_target},
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 1,
            .using_bioseq = true,
        },
        {seq_row{.pk = 1, .sequences = {source}}});
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string{k_oracle_target},
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 1,
            .using_bioseq = true,
        },
        {seq_row{.pk = 1, .sequences = {logical_target}}});

    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT bioseq.update_index('{}'::regclass,"
        "'minimizers_per_fragment');",
        k_transform_target)) == 1);
    exec_sql(ctx, fmt::format(
        "SELECT bioseq.set_active_read_index('{}'::regclass,"
        "'minimizers_per_fragment');",
        k_transform_target));
    REQUIRE(index_table_row_count(
        ctx, "minimizers_per_fragment", k_transform_target) > 0);
    const std::string raw_control = fmt::format(
        "SELECT t.pk FROM {} AS t WHERE mmsearch(t.seq0, {}, "
        "aligner_options(index_read_requirement=>0));",
        k_transform_target, query_literal);
    const auto raw_plan = explain_text<true>(ctx, raw_control);
    REQUIRE(plan_contains(
        raw_plan,
        "requested_index_source=persisted actual_index_source=persisted"));
    REQUIRE(plan_contains(
        raw_plan, "Index outcomes [targets=1 persisted=1"));
    const auto raw_stats = last_alignment_stats(ctx);
    REQUIRE(raw_stats.size() == 20u);
    for (const auto& stage : raw_stats) {
        REQUIRE(stage[18].value_or("") == "persisted");
        REQUIRE(stage[19].value_or("") == "persisted");
    }
    REQUIRE(stats_i64(raw_stats.front(), 45) == 1);
    REQUIRE(stats_i64(raw_stats.front(), 46) == 0);
    REQUIRE(stats_i64(raw_stats.front(), 47) == 0);

    const long long minimizer_fragment_rows_before = index_table_row_count(
        ctx, "minimizers_per_fragment", k_transform_target);
    const long long minimizer_sequence_rows_before = index_table_row_count(
        ctx, "minimizers_per_sequence", k_transform_target);
    const rowset_t age_log_before = query_rows(ctx, fmt::format(
        "SELECT index_name,last_updated::text "
        "FROM bioseq_internal.metadata_age_log "
        "WHERE user_relid='{}'::regclass AND column_attno=2 "
        "ORDER BY index_name;",
        k_transform_target));
    const long long async_waiting_before = query_scalar_int(
        ctx,
        "SELECT async_persist_jobs_waiting FROM bioseq.scheduler_stats();");

    const std::string transformed = base_alignment_sql(
        k_transform_target, "t.seq0[1:]", query_literal);
    const std::string oracle = base_alignment_sql(
        k_oracle_target, "t.seq0", query_literal);
    const auto plan = explain_text<true>(ctx, transformed);
    REQUIRE(std::count_if(
        plan.begin(), plan.end(), [](const std::string& line) {
            return line.find("Custom Scan (BioseqScan)")
                != std::string::npos;
        }) == 1);
    REQUIRE(plan_contains(
        plan,
        "Bioseq Match Operand Evaluation: base-local ExprState "
        "(target then query; persisted target index absent)"));
    REQUIRE(plan_contains(
        plan,
        "Bioseq Recomputed: minimizers 100% "
        "(evaluated logical view requires index absence)"));
    REQUIRE_FALSE(plan_contains(plan, "Filter: mmsearch"));
    REQUIRE_FALSE(plan_contains(plan, "One-Time Filter: mmsearch"));
    REQUIRE_FALSE(plan_contains(plan, "Fetched From Sequence Index"));
    REQUIRE(plan_contains(
        plan,
        "requested_index_source=recomputed actual_index_source=recomputed"));
    REQUIRE(plan_contains(plan, "Index outcomes [targets=1 persisted=0"));
    REQUIRE(plan_contains(plan, "missing_persisted=0 recomputed=1]"));
    const auto transformed_stats = last_alignment_stats(ctx);
    REQUIRE(transformed_stats.size() == 20u);
    for (const auto& stage : transformed_stats) {
        REQUIRE(stage[18].value_or("") == "recomputed");
        REQUIRE(stage[19].value_or("") == "recomputed");
        REQUIRE(stats_i64(stage, 45) == 0);
        REQUIRE(stats_i64(stage, 46) == 0);
        REQUIRE(stats_i64(stage, 47) == 1);
    }

    const rowset_t expected = query_rows(ctx, oracle);
    const rowset_t actual = expect_query_invariant_under_options(
        ctx, transformed,
        {
            {"bioseq.vectorization.enabled", {"on", "off"}},
            {"bioseq.column_cache.enabled", {"on", "off"}},
            {"bioseq.index.sequence_read.enabled", {"on", "off"}},
            {"bioseq.index.async_persistence.enabled", {"on", "off"}},
            {"bioseq.index.recompute_async_persistence.enabled", {"on", "off"}},
        });
    REQUIRE(actual == expected);
    REQUIRE_FALSE(actual.empty());
    bool found_exact_logical_hit = false;
    for (const auto& row : actual) {
        REQUIRE(row.size() == 8u);
        for (const auto& value : row) REQUIRE(value.has_value());
        REQUIRE(is_valid_cigar(*row[6]));
        REQUIRE(*row[7] == "t");
        found_exact_logical_hit = found_exact_logical_hit
            || (std::stoull(*row[1]) == 0u
                && std::stoull(*row[2]) == query_length
                && std::stoull(*row[3]) == query_source_offset - 1u
                && std::stoull(*row[4])
                    == query_source_offset - 1u + query_length
                && *row[5] == "f");
    }
    REQUIRE(found_exact_logical_hit);

    const std::string compound_target =
        "bioseq.reverse(bioseq.reverse("
        "t.seq0[1:193] OPERATOR(bioseq.||) t.seq0[193:]))";
    const std::string compound_query =
        "bioseq.reverse_complement(bioseq.reverse_complement("
        "t.seq0[97:193]))";
    const std::string compound = base_alignment_sql(
        k_transform_target, compound_target, compound_query);
    const auto compound_plan = explain_text<false>(ctx, compound);
    REQUIRE(plan_contains(compound_plan, "Custom Scan (BioseqScan)"));
    REQUIRE(plan_contains(
        compound_plan,
        "Bioseq Match Operand Evaluation: base-local ExprState "
        "(target then query; persisted target index absent)"));
    REQUIRE_FALSE(plan_contains(compound_plan, "Filter: mmsearch"));
    const rowset_t compound_rows = query_rows(ctx, compound);
    REQUIRE(compound_rows == expected);
    REQUIRE_FALSE(compound_rows.empty());

    for (int attempt = 0; attempt < 200; ++attempt) {
        if (query_scalar_int(
                ctx,
                "SELECT async_persist_jobs_waiting "
                "FROM bioseq.scheduler_stats();")
                == async_waiting_before) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{25});
    }
    REQUIRE(query_scalar_int(
        ctx,
        "SELECT async_persist_jobs_waiting FROM bioseq.scheduler_stats();")
        == async_waiting_before);
    REQUIRE(index_table_row_count(
        ctx, "minimizers_per_fragment", k_transform_target)
        == minimizer_fragment_rows_before);
    REQUIRE(index_table_row_count(
        ctx, "minimizers_per_sequence", k_transform_target)
        == minimizer_sequence_rows_before);
    REQUIRE(query_rows(ctx, fmt::format(
        "SELECT index_name,last_updated::text "
        "FROM bioseq_internal.metadata_age_log "
        "WHERE user_relid='{}'::regclass AND column_attno=2 "
        "ORDER BY index_name;",
        k_transform_target)) == age_log_before);
}

TEST_CASE("Base-local sliced mmsearch preserves algorithm-route parity",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context(
        {k_transform_target, k_oracle_target});
    const std::string source = high_entropy_sequence(384u);
    const std::string logical_target = source.substr(1u);
    const std::string query = source.substr(97u, 96u);
    const std::string query_literal = format_literal(
        query, seq_typmod{.alphabet = "dna4", .width = 32});
    exec_sql(ctx,
        "SET max_parallel_workers_per_gather=0;"
        "SET bioseq.index.insert_maintenance.enabled=off;"
        "SET bioseq.index.sequence_read.enabled=off;"
        "SET bioseq.index.async_persistence.enabled=off;"
        "SET bioseq.index.recompute_async_persistence.enabled=off;"
        "SET bioseq.aligner.chaining.minimum_score=20;");
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string{k_transform_target},
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 1,
            .using_bioseq = true,
        },
        {seq_row{.pk = 1, .sequences = {source}}});
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string{k_oracle_target},
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 1,
            .using_bioseq = true,
        },
        {seq_row{.pk = 1, .sequences = {logical_target}}});
    const auto route_alignment_sql = [](
            const std::string_view table,
            const std::string_view target_expression,
            const std::string_view query_expression) {
        return fmt::format(
            "SELECT t.pk, bioseq.query_start({2}), bioseq.query_end({2}), "
            "bioseq.target_start({2}), bioseq.target_end({2}), "
            "bioseq.is_reverse({2}), bioseq.cigar({2}), "
            "bioseq.has_cigar({2}), bioseq.mapq({2}) "
            "FROM {0} AS t WHERE mmsearch({2}, {1}, "
            "aligner_options(index_read_requirement => 1)) "
            "ORDER BY 2, 3, 4, 5, 6, 7;",
            table, query_expression, target_expression);
    };
    const std::string transformed = route_alignment_sql(
        k_transform_target, "t.seq0[1:]", query_literal);
    const std::string oracle = route_alignment_sql(
        k_oracle_target, "t.seq0", query_literal);

    struct algorithm_route {
        std::string_view name;
        std::string_view minimizer;
        std::string_view chaining;
        std::string_view base_alignment;
        std::string_view requested_backend;
        std::string_view actual_backend;
        std::size_t execution_column = 0u;
        bool fully_attested = true;
        bool vulkan = false;
    };
    std::vector<algorithm_route> routes{
        {"scalar", "scalar", "scalar_minimap2", "ksw2",
         "scalar_cpu", "scalar_cpu", 26u, true, false},
        {"Exact SIMD", "two_stack_nthash", "simd_minimap2",
         "block_aligner", "simd_cpu", "simd_cpu", 27u, true, false},
        {"bounded ntHash SIMD", "bounded_nthash_simd", "simd_minimap2",
         "block_aligner", "simd_cpu", "simd_cpu", 27u, true, false},
    };
    const bool vulkan_untrusted = selected_vulkan_runtime_untrusted(ctx);
    if (!vulkan_untrusted) {
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        routes.push_back({
            "physical Vulkan", "vulkan_minimap_integer", "vulkan_tiled_dynamic_programming",
            "block_aligner", "hybrid_cpu_vulkan", "hybrid_cpu_vulkan", 28u,
            true, true});
#else
        // This build reports hybrid work as mixed_invalid. GPU dispatch
        // counters below still check which operations ran.
        routes.push_back({
            "physical Vulkan", "vulkan_minimap_integer", "vulkan_tiled_dynamic_programming",
            "block_aligner", "mixed_invalid", "mixed_invalid", 28u,
            false, true});
#endif
    }

    for (const auto& route : routes) {
        DYNAMIC_SECTION(route.name) {
            exec_sql(ctx, fmt::format(
                "SET bioseq.aligner.minimizer.algorithm='{}';"
                "SET bioseq.aligner.chaining.algorithm='{}';"
                "SET bioseq.aligner.base_alignment.algorithm='{}';"
                "SET bioseq.aligner.vulkan.enabled={};",
                route.minimizer, route.chaining, route.base_alignment,
                route.vulkan ? "on" : "off"));
            if (route.vulkan) {
                exec_sql(ctx,
                    "SET bioseq.aligner.vulkan.seed_lookup.enabled=on;"
                    "SET bioseq.aligner.vulkan.index_cache.enabled=on;"
                    "SET bioseq.aligner.vulkan.fused_frontend.enabled=off;"
                    "SET bioseq.aligner.vulkan.index_build.enabled=on;"
                    "SET bioseq.aligner.vulkan.alignment_extension.enabled=off;");
            }
            const rowset_t route_expected = query_rows(ctx, oracle);
            const rowset_t route_actual = query_rows(ctx, transformed);
            REQUIRE(route_actual == route_expected);
            REQUIRE_FALSE(route_actual.empty());

            if (!route.vulkan) {
                const auto stats = last_alignment_stats(ctx);
                REQUIRE(stats.size() == 20u);
                for (const auto& stage : stats) {
                    REQUIRE(stage.size() == 151u);
                    REQUIRE(stage[16].value_or("")
                        == route.requested_backend);
                    REQUIRE(stage[17].value_or("") == route.actual_backend);
                    REQUIRE(stats_bool(stage, 13));
                    REQUIRE_FALSE(stats_bool(stage, 14));
                    REQUIRE(stats_bool(stage, 15));
                    REQUIRE(stats_i64(stage, 33) > 0);
                    if (route.fully_attested) {
                        REQUIRE(stats_i64(stage, 34)
                            == stats_i64(stage, 33));
                        REQUIRE(stats_i64(stage, 35) == 0);
                    } else {
                        REQUIRE(stats_i64(stage, 34) == 0);
                        REQUIRE(stats_i64(stage, 35)
                            == stats_i64(stage, 33));
                    }
                    REQUIRE(stats_i64(stage, 29) == 0);
                }
                for (const std::size_t stage_no : {
                         std::size_t{0}, std::size_t{1},
                         std::size_t{7}, std::size_t{8}}) {
                    REQUIRE(stats_i64(stats[stage_no], 24) > 0);
                    REQUIRE(stats_i64(
                        stats[stage_no], route.execution_column)
                        == stats_i64(stats[stage_no], 24));
                    for (const std::size_t execution_column : {
                             std::size_t{26}, std::size_t{27},
                             std::size_t{28}}) {
                        if (execution_column != route.execution_column) {
                            REQUIRE(stats_i64(
                                stats[stage_no], execution_column) == 0);
                        }
                    }
                    REQUIRE(stats_i64(stats[stage_no], 25) == 0);
                    REQUIRE(stats_i64(stats[stage_no], 29) == 0);
                }
            } else {
                const auto route_plan = explain_text<true>(ctx, transformed);
                require_completed_vulkan_host_suffix(
                    route_plan, "Custom Scan (BioseqScan)");
                for (const auto& row : route_actual) {
                    REQUIRE(row.size() == 9u);
                    REQUIRE(row[6].has_value());
                    REQUIRE(is_valid_cigar(*row[6]));
                    REQUIRE(row[7].has_value());
                    REQUIRE(*row[7] == "t");
                    REQUIRE(row[8].has_value());
                    const auto mapq = std::stoll(*row[8]);
                    REQUIRE(mapq >= 0);
                    REQUIRE(mapq <= 60);
                }
            }
        }
    }

    if (vulkan_untrusted) {
        exec_sql(ctx,
            "SET bioseq.aligner.minimizer.algorithm='vulkan_minimap_integer';"
            "SET bioseq.aligner.chaining.algorithm='vulkan_tiled_dynamic_programming';"
            "SET bioseq.aligner.base_alignment.algorithm='block_aligner';"
            "SET bioseq.aligner.vulkan.enabled=on;"
            "SET bioseq.aligner.vulkan.seed_lookup.enabled=on;"
            "SET bioseq.aligner.vulkan.index_cache.enabled=on;"
            "SET bioseq.aligner.vulkan.fused_frontend.enabled=off;"
            "SET bioseq.aligner.vulkan.index_build.enabled=on;"
            "SET bioseq.aligner.vulkan.alignment_extension.enabled=off;");
        const auto error = sql_error(ctx, transformed);
        REQUIRE(error.sqlstate == "XX000");
        REQUIRE(error.contains("Vulkan aligner execution failed"));
        REQUIRE((error.contains("software/CPU Vulkan device")
                 || error.contains("usable Vulkan runtime")));
        REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
    }
}

TEST_CASE("Base-local operands preserve roots, effects, and control flow",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_transform_target});
    const std::string source = high_entropy_sequence(384u);
    const std::string residual_source = source.substr(8u, 32u);
    const std::string rejected_residual(residual_source.size(), 'A');
    exec_sql(ctx,
        "SET max_parallel_workers_per_gather=0;"
        "SET bioseq.index.insert_maintenance.enabled=off;"
        "SET bioseq.index.sequence_read.enabled=off;"
        "SET bioseq.aligner.chaining.minimum_score=20;"
        "SET bioseq.aligner.vulkan.enabled=off;");
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string{k_transform_target},
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 2,
            .using_bioseq = true,
        },
        {
            seq_row{.pk = 1, .sequences = {source, residual_source}},
            seq_row{.pk = 2, .sequences = {source, rejected_residual}},
        });
    const std::string target = "t.seq0[1:]";
    const std::string sql = fmt::format(
        "SELECT t.pk, bioseq.query_start({0}), bioseq.query_end({0}), "
        "bioseq.target_start({0}), bioseq.target_end({0}), "
        "bioseq.cigar({0}), t.seq0::text "
        "FROM {1} AS t "
        "WHERE t.seq1::text='{2}'::text "
        "AND mmsearch({0}, t.seq0[97:193], "
        "aligner_options(index_read_requirement=>1)) "
        "ORDER BY 2, 3, 4, 5, 6;",
        target, k_transform_target, residual_source);
    const auto plan = explain_text<false>(ctx, sql);
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqScan)"));
    REQUIRE(plan_contains(
        plan,
        "Bioseq Match Operand Evaluation: base-local ExprState "
        "(target then query; persisted target index absent)"));
    REQUIRE_FALSE(plan_contains(plan, "Filter: mmsearch"));

    const auto rows = query_rows(ctx, sql);
    REQUIRE_FALSE(rows.empty());
    bool found_exact_logical_hit = false;
    for (const auto& row : rows) {
        REQUIRE(row.size() == 7u);
        for (const auto& value : row) REQUIRE(value.has_value());
        REQUIRE(*row[0] == "1");
        REQUIRE(is_valid_cigar(*row[5]));
        REQUIRE(*row[6] == source);
        found_exact_logical_hit = found_exact_logical_hit
            || (std::stoull(*row[1]) == 0u
                && std::stoull(*row[2]) == 96u
                && std::stoull(*row[3]) == 96u
                && std::stoull(*row[4]) == 192u);
    }
    REQUIRE(found_exact_logical_hit);

    const std::string query_only = base_alignment_sql(
        k_transform_target, "t.seq0", "t.seq0[97:193]",
        "t.pk=1");
    const auto query_only_plan = explain_text<false>(ctx, query_only);
    REQUIRE(plan_contains(query_only_plan, "Custom Scan (BioseqScan)"));
    REQUIRE(plan_contains(
        query_only_plan,
        "Bioseq Match Operand Evaluation: base-local ExprState "
        "(target then query; persisted target index absent)"));
    const rowset_t query_only_rows = query_rows(ctx, query_only);
    REQUIRE_FALSE(query_only_rows.empty());
    REQUIRE(std::any_of(
        query_only_rows.begin(), query_only_rows.end(),
        [](const row_t& row) {
            return row.size() == 8u
                && row[1] == std::optional<std::string>{"0"}
                && row[2] == std::optional<std::string>{"96"}
                && row[3] == std::optional<std::string>{"97"}
                && row[4] == std::optional<std::string>{"193"};
        }));

    exec_sql(ctx,
        "CREATE TEMP TABLE base_operand_effect_log ("
        "ordinal bigserial PRIMARY KEY, label text NOT NULL);"
        "CREATE FUNCTION pg_temp.mark_operand_bound("
        "label text, value bigint) RETURNS bigint "
        "LANGUAGE plpgsql VOLATILE AS $$"
        "BEGIN "
        "  INSERT INTO base_operand_effect_log(label) VALUES(label); "
        "  RETURN value; "
        "END$$;"
        "CREATE FUNCTION pg_temp.fail_operand_bound(label text) "
        "RETURNS bigint LANGUAGE plpgsql VOLATILE AS $$"
        "BEGIN RAISE EXCEPTION 'active % operand', label; END$$;");

    const std::string ordered = fmt::format(
        "SELECT t.pk FROM {} AS t WHERE t.pk=1 AND mmsearch("
        "t.seq0[pg_temp.mark_operand_bound('target',1):], "
        "t.seq0[pg_temp.mark_operand_bound('query',97):193], "
        "aligner_options(index_read_requirement=>1));",
        k_transform_target);
    const auto ordered_plan = explain_text<false>(ctx, ordered);
    REQUIRE(plan_contains(ordered_plan, "Custom Scan (BioseqScan)"));
    REQUIRE(plan_contains(
        ordered_plan,
        "Bioseq Match Operand Evaluation: base-local ExprState "
        "(target then query; persisted target index absent)"));
    REQUIRE(query_scalar_int(
        ctx, "SELECT count(*) FROM base_operand_effect_log;") == 0);
    REQUIRE_FALSE(query_rows(ctx, ordered).empty());
    REQUIRE(query_scalar(
        ctx,
        "SELECT string_agg(label,',' ORDER BY ordinal) "
        "FROM base_operand_effect_log;") == "target,query");

    const auto require_no_alignment_jobs = [&ctx](const long long prior_id) {
        const auto stats = last_alignment_stats(ctx);
        REQUIRE(stats.size() == 20u);
        REQUIRE((stats_i64(stats.front(), 0) == prior_id
                 || stats_i64(stats.front(), 33) == 0));
    };

    exec_sql(ctx, "TRUNCATE base_operand_effect_log RESTART IDENTITY;");
    long long prior_id = stats_i64(last_alignment_stats(ctx).front(), 0);
    const std::string global_null_query = fmt::format(
        "SELECT t.pk FROM {} AS t WHERE t.pk=1 AND mmsearch("
        "t.seq0[pg_temp.mark_operand_bound("
        "'target-before-global-null',1):], "
        "NULL::bioseq.seq('dna4',32), "
        "aligner_options(index_read_requirement=>1));",
        k_transform_target);
    const auto global_null_plan = explain_text<false>(
        ctx, global_null_query);
    REQUIRE(plan_contains(global_null_plan, "Custom Scan (BioseqScan)"));
    REQUIRE(plan_contains(
        global_null_plan,
        "Bioseq Match Operand Evaluation: base-local ExprState "
        "(target then query; persisted target index absent)"));
    REQUIRE(query_rows(ctx, global_null_query).empty());
    REQUIRE(query_scalar(
        ctx,
        "SELECT string_agg(label,',' ORDER BY ordinal) "
        "FROM base_operand_effect_log;") == "target-before-global-null");
    require_no_alignment_jobs(prior_id);

    exec_sql(ctx, "TRUNCATE base_operand_effect_log RESTART IDENTITY;");
    prior_id = stats_i64(last_alignment_stats(ctx).front(), 0);
    const std::string null_target = fmt::format(
        "SELECT t.pk FROM {} AS t WHERE t.pk=1 AND mmsearch("
        "CASE WHEN t.pk=1 THEN NULL::bioseq.seq('dna4',32) "
        "ELSE t.seq0[1:] END, "
        "t.seq0[pg_temp.mark_operand_bound('query-after-null-target',97):193], "
        "aligner_options(index_read_requirement=>1));",
        k_transform_target);
    REQUIRE(query_rows(ctx, null_target).empty());
    REQUIRE(query_scalar(
        ctx,
        "SELECT string_agg(label,',' ORDER BY ordinal) "
        "FROM base_operand_effect_log;") == "query-after-null-target");
    require_no_alignment_jobs(prior_id);

    exec_sql(ctx, "TRUNCATE base_operand_effect_log RESTART IDENTITY;");
    prior_id = stats_i64(last_alignment_stats(ctx).front(), 0);
    const std::string null_query = fmt::format(
        "SELECT t.pk FROM {} AS t WHERE t.pk=1 AND mmsearch("
        "t.seq0[pg_temp.mark_operand_bound('target-before-null-query',1):], "
        "CASE WHEN t.pk=1 THEN NULL::bioseq.seq('dna4',32) "
        "ELSE t.seq0[97:193] END, "
        "aligner_options(index_read_requirement=>1));",
        k_transform_target);
    REQUIRE(query_rows(ctx, null_query).empty());
    REQUIRE(query_scalar(
        ctx,
        "SELECT string_agg(label,',' ORDER BY ordinal) "
        "FROM base_operand_effect_log;") == "target-before-null-query");
    require_no_alignment_jobs(prior_id);

    const std::string dead_arms = fmt::format(
        "SELECT t.pk FROM {} AS t WHERE t.pk=1 AND mmsearch("
        "CASE WHEN t.pk=1 THEN t.seq0[1:] "
        "ELSE t.seq0[pg_temp.fail_operand_bound('target'):] END, "
        "COALESCE(NULL::bioseq.seq('dna4',32), t.seq0[97:193], "
        "t.seq0[pg_temp.fail_operand_bound('query'):]), "
        "aligner_options(index_read_requirement=>1));",
        k_transform_target);
    const auto dead_arm_plan = explain_text<false>(ctx, dead_arms);
    REQUIRE(plan_contains(dead_arm_plan, "Custom Scan (BioseqScan)"));
    REQUIRE_FALSE(query_rows(ctx, dead_arms).empty());

    const auto target_error = sql_error(ctx, fmt::format(
        "SELECT t.pk FROM {} AS t WHERE t.pk=1 AND mmsearch("
        "CASE WHEN t.pk=0 THEN t.seq0[1:] "
        "ELSE t.seq0[pg_temp.fail_operand_bound('target'):] END, "
        "t.seq0[97:193], aligner_options(index_read_requirement=>1));",
        k_transform_target));
    REQUIRE(target_error.sqlstate == "P0001");
    REQUIRE(target_error.contains("active target operand"));
    const auto query_error = sql_error(ctx, fmt::format(
        "SELECT t.pk FROM {} AS t WHERE t.pk=1 "
        "AND mmsearch(t.seq0[1:], "
        "COALESCE(NULL::bioseq.seq('dna4',32), "
        "CASE WHEN t.pk=0 THEN t.seq0[97:193] "
        "ELSE NULL::bioseq.seq('dna4',32) END, "
        "t.seq0[pg_temp.fail_operand_bound('query'):]), "
        "aligner_options(index_read_requirement=>1));",
        k_transform_target));
    REQUIRE(query_error.sqlstate == "P0001");
    REQUIRE(query_error.contains("active query operand"));
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE("Transformed INNER mmsearch matches a flat logical oracle",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context(
        {k_transform_target, k_oracle_target});
    const std::string source = high_entropy_sequence(320u);
    const auto cases = transform_cases(source);
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string{k_transform_target},
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 1,
            .using_bioseq = true,
        },
        {seq_row{.pk = 1, .sequences = {source}}});
    std::vector<seq_row> oracle_rows;
    oracle_rows.reserve(cases.size());
    for (std::size_t index = 0u; index < cases.size(); ++index) {
        oracle_rows.push_back(seq_row{
            .pk = static_cast<int>(index + 1u),
            .sequences = {cases[index].logical_target},
        });
    }
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string{k_oracle_target},
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 1,
            .using_bioseq = true,
        },
        oracle_rows);
    exec_sql(ctx, fmt::format(
        "CREATE TEMP TABLE {} ("
        "id int PRIMARY KEY, source_seq bioseq.seq('dna4',32), "
        "oracle_seq bioseq.seq('dna4',32));",
        k_oracle_queries));
    const seq_typmod dna4{.alphabet = "dna4", .width = 32};
    for (std::size_t index = 0u; index < cases.size(); ++index) {
        exec_sql(ctx, fmt::format(
            "INSERT INTO {} VALUES ({}, {}, {});",
            k_oracle_queries, index + 1u,
            format_literal(cases[index].stored_query, dna4),
            format_literal(cases[index].logical_query, dna4)));
    }

    exec_sql(ctx, "SET max_parallel_workers_per_gather = 0;");
    exec_sql(ctx, "SET bioseq.index.sequence_read.enabled = off;");
    exec_sql(ctx, "SET bioseq.aligner.chaining.minimum_score = 20;");

    struct algorithm_route {
        std::string_view name;
        std::string_view minimizer;
        std::string_view chaining;
        std::string_view base_alignment;
        std::string_view requested_backend;
        std::string_view actual_backend;
        std::size_t execution_column = 0u;
        bool fully_attested = true;
        bool vulkan = false;
    };
    std::vector<algorithm_route> routes{
        {"scalar", "scalar", "scalar_minimap2", "ksw2",
         "scalar_cpu", "scalar_cpu", 26u, true, false},
        {"exact SIMD", "two_stack_nthash", "simd_minimap2", "block_aligner",
         "simd_cpu", "simd_cpu", 27u, true, false},
        {"bounded ntHash SIMD", "bounded_nthash_simd", "simd_minimap2",
         "block_aligner", "simd_cpu", "simd_cpu", 27u, true, false},
    };
    if (!selected_vulkan_runtime_untrusted(ctx)) {
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        routes.push_back({
            "Vulkan", "vulkan_minimap_integer", "vulkan_tiled_dynamic_programming",
            "block_aligner", "hybrid_cpu_vulkan", "hybrid_cpu_vulkan", 28u,
            true, true});
#else
        // This build reports hybrid work as mixed_invalid. Check its GPU
        // stages and dispatch counters below.
        routes.push_back({
            "Vulkan", "vulkan_minimap_integer", "vulkan_tiled_dynamic_programming",
            "block_aligner", "mixed_invalid", "mixed_invalid", 28u,
            false, true});
#endif
    }

    for (const auto& route : routes) {
        DYNAMIC_SECTION(route.name) {
            exec_sql(ctx, fmt::format(
                "SET bioseq.aligner.minimizer.algorithm = '{}';",
                route.minimizer));
            exec_sql(ctx, fmt::format(
                "SET bioseq.aligner.chaining.algorithm = '{}';",
                route.chaining));
            exec_sql(ctx, fmt::format(
                "SET bioseq.aligner.base_alignment.algorithm = '{}';",
                route.base_alignment));
            exec_sql(ctx, fmt::format(
                "SET bioseq.aligner.vulkan.enabled = {};",
                route.vulkan ? "on" : "off"));
            if (route.vulkan) {
                exec_sql(ctx,
                    "SET bioseq.aligner.vulkan.seed_lookup.enabled = on;");
                exec_sql(ctx,
                    "SET bioseq.aligner.vulkan.index_cache.enabled = on;");
                exec_sql(ctx,
                    "SET bioseq.aligner.vulkan.fused_frontend.enabled = off;");
                exec_sql(ctx,
                    "SET bioseq.aligner.vulkan.index_build.enabled = on;");
                exec_sql(ctx,
                    "SET bioseq.aligner.vulkan.alignment_extension.enabled = off;");
            }
            for (std::size_t index = 0u; index < cases.size(); ++index) {
                const auto& test_case = cases[index];
                DYNAMIC_SECTION(test_case.name) {
                    const std::size_t case_id = index + 1u;
                    const std::string transformed = fmt::format(
                        "SELECT {}, bioseq.mapq(t.pk) "
                        "FROM {} AS t INNER JOIN {} AS q "
                        "ON q.id = {} AND mmsearch({}, {}) "
                        "ORDER BY 2, 3, 4, 5, 6, 7;",
                        alignment_projection("t", "pk"),
                        k_transform_target, k_oracle_queries,
                        case_id, test_case.target_expression,
                        test_case.query_expression);
                    const std::string oracle = fmt::format(
                        "SELECT {}, bioseq.mapq(o.pk) "
                        "FROM {} AS o INNER JOIN {} AS q "
                        "ON o.pk = q.id AND mmsearch(o.seq0, q.oracle_seq) "
                        "WHERE q.id = {} ORDER BY 2, 3, 4, 5, 6, 7;",
                        alignment_projection("o", "pk"),
                        k_oracle_target, k_oracle_queries,
                        case_id);
                    const auto plan = explain_text<false>(ctx, transformed);
                    REQUIRE(plan_contains(
                        plan, "Custom Scan (BioseqBatchJoin)"));
                    REQUIRE(plan_contains(
                        plan, "Bioseq Mmsearch Ownership: per-row recipe"));
                    REQUIRE(std::count_if(
                        plan.begin(), plan.end(),
                        [](const std::string& line) {
                            return line.find(
                                "Bioseq Mmsearch Ownership: per-row recipe")
                                != std::string::npos;
                        }) == 1);
                    /* Keep one BatchJoin responsible for the transformed search while
                     * BioseqScan supplies rows and SliceProject evaluates the
                     * sequence expressions. */
                    REQUIRE(plan_contains(
                        plan, "Custom Scan (BioseqScan)"));
                    REQUIRE(plan_contains(
                        plan, "Custom Scan (BioseqSliceProject)"));
                    REQUIRE_FALSE(plan_contains(plan, "Join Filter: mmsearch"));

                    const rowset_t expected = query_rows(ctx, oracle);
                    const rowset_t actual = query_rows(ctx, transformed);
                    REQUIRE(actual == expected);
                    REQUIRE_FALSE(actual.empty());
                    if (!route.vulkan) {
                        const auto execution_stats = last_alignment_stats(ctx);
                        REQUIRE(execution_stats.size() == 20u);
                        for (const auto& stage : execution_stats) {
                            REQUIRE(stage.size() == 151u);
                            REQUIRE(stage[16].value_or("")
                                    == route.requested_backend);
                            REQUIRE(stage[17].value_or("")
                                    == route.actual_backend);
                            REQUIRE(stats_bool(stage, 13));
                            REQUIRE_FALSE(stats_bool(stage, 14));
                            REQUIRE(stats_bool(stage, 15));
                            REQUIRE(stats_i64(stage, 33) > 0);
                            if (route.fully_attested) {
                                REQUIRE(stats_i64(stage, 34)
                                        == stats_i64(stage, 33));
                                REQUIRE(stats_i64(stage, 35) == 0);
                            } else {
                                REQUIRE(stats_i64(stage, 34) == 0);
                                REQUIRE(stats_i64(stage, 35)
                                        == stats_i64(stage, 33));
                            }
                            REQUIRE(stats_i64(stage, 29) == 0);
                        }
                        REQUIRE_FALSE(stats_bool(
                            execution_stats.front(), 141));
                        REQUIRE(stats_i64(
                            execution_stats.front(), 139) == 0);
                        REQUIRE(stats_i64(
                            execution_stats.front(), 140) == 0);
                        /* Confirm that each stage ran the selected CPU implementation before
                         * accepting the overall backend label. */
                        for (const std::size_t stage_no : {
                                 std::size_t{0}, std::size_t{1},
                                 std::size_t{7}, std::size_t{8}}) {
                            const auto& stage = execution_stats[stage_no];
                            REQUIRE(stats_i64(stage, 24) > 0);
                            REQUIRE(stats_i64(stage, route.execution_column)
                                    == stats_i64(stage, 24));
                            for (const std::size_t execution_column : {
                                     std::size_t{26}, std::size_t{27},
                                     std::size_t{28}}) {
                                if (execution_column
                                        != route.execution_column) {
                                    REQUIRE(stats_i64(
                                        stage, execution_column) == 0);
                                }
                            }
                            REQUIRE(stats_i64(stage, 25) == 0);
                            REQUIRE(stats_i64(stage, 29) == 0);
                        }
                    } else {
                        const auto execution_plan =
                            explain_text<true>(ctx, transformed);
                        require_completed_vulkan_host_suffix(
                            execution_plan,
                            "Custom Scan (BioseqBatchJoin)");
                    }
                    bool found_exact_logical_hit = false;
                    for (const auto& row : actual) {
                        REQUIRE(row.size() == 9u);
                        for (std::size_t column = 0u;
                             column < row.size(); ++column) {
                            REQUIRE(row[column].has_value());
                        }
                        REQUIRE(is_valid_cigar(*row[6]));
                        REQUIRE(*row[6] != "!UNEXPECTED!");
                        REQUIRE(*row[7] == "t");
                        const auto mapq = std::stoll(*row[8]);
                        REQUIRE(mapq >= 0);
                        REQUIRE(mapq <= 60);
                        const bool exact_coordinates =
                            std::stoull(*row[1]) == 0u
                            && std::stoull(*row[2])
                                == test_case.logical_query.size()
                            && std::stoull(*row[3])
                                == test_case.expected_target_start
                            && std::stoull(*row[4])
                                == test_case.expected_target_start
                                    + test_case.logical_query.size()
                            && (*row[5] == (test_case.expected_reverse
                                ? "t" : "f"));
                        found_exact_logical_hit =
                            found_exact_logical_hit || exact_coordinates;
                    }
                    REQUIRE(found_exact_logical_hit);
                }
            }
        }
    }
}

TEST_CASE("Transformed ownership reconstructs INNER join algorithms",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_hoe});
    seed_hoe_fixture(ctx, k_hoe);
    exec_sql(ctx,
        "CREATE TEMP TABLE join_shape_queries "
        "(id int PRIMARY KEY, seq bioseq.seq('dna4',32));");
    exec_sql(ctx, fmt::format(
        "INSERT INTO join_shape_queries VALUES (1, {});",
        batch_join_target_literal()));
    exec_sql(ctx,
        "SET max_parallel_workers_per_gather=0;"
        "SET bioseq.index.sequence_read.enabled=off;"
        "SET bioseq.aligner.chaining.minimum_score=20;");

    struct join_route {
        std::string_view name;
        std::string_view expected_plan;
        std::string_view settings;
    };
    constexpr std::array<join_route, 3u> routes{{
        {"Nested Loop", "Nested Loop",
         "SET enable_nestloop=on; SET enable_hashjoin=off; "
         "SET enable_mergejoin=off;"},
        {"Hash Join", "Hash Join",
         "SET enable_nestloop=off; SET enable_hashjoin=on; "
         "SET enable_mergejoin=off;"},
        {"Merge Join", "Merge Join",
         "SET enable_nestloop=off; SET enable_hashjoin=off; "
         "SET enable_mergejoin=on;"},
    }};
    constexpr std::array<std::string_view, 2u> options_suffixes{
        "", ", NULL::bioseq_aligner_options"};
    for (const auto& route : routes) {
        DYNAMIC_SECTION(route.name) {
            exec_sql(ctx, route.settings);
            for (const auto options_suffix : options_suffixes) {
                DYNAMIC_SECTION((options_suffix.empty()
                        ? "two-argument OID"
                        : "three-argument OID")) {
                    const std::string sql = fmt::format(
                        "SELECT t.pk, q.id FROM {} AS t INNER JOIN "
                        "join_shape_queries AS q "
                        "ON t.pk=q.id AND mmsearch("
                        "t.seq0[0:], q.seq[0:]{} ) ORDER BY t.pk, q.id;",
                        k_hoe, options_suffix);
                    const auto plan = explain_text<false>(ctx, sql);
                    REQUIRE(plan_contains(
                        plan, "Custom Scan (BioseqBatchJoin)"));
                    REQUIRE(plan_contains(
                        plan, "Custom Scan (BioseqSliceProject)"));
                    REQUIRE(plan_contains(plan, route.expected_plan));
                    REQUIRE_FALSE(plan_contains(plan, "Join Filter: mmsearch"));
                    REQUIRE_FALSE(plan_contains(plan, "Filter: mmsearch"));
                    REQUIRE_FALSE(plan_contains(
                        plan, "One-Time Filter: mmsearch"));

                    const auto rows = query_rows(ctx, sql);
                    REQUIRE_FALSE(rows.empty());
                    for (const auto& row : rows) {
                        REQUIRE(row.size() == 2u);
                        REQUIRE(row[0] == std::optional<std::string>{"1"});
                        REQUIRE(row[1] == std::optional<std::string>{"1"});
                    }
                }
            }
        }
    }
}

TEST_CASE("Transformed mmsearch owns cross-table concat",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_transform_target});
    const std::string source = high_entropy_sequence(320u);
    const std::string prefix = high_entropy_sequence(96u);
    /* Build a 96-base query with 48 bases from each side of the  concatenation.
     * Its expected match crosses the join at target position 64. */
    const std::string needle = source.substr(16u, 48u)
        + prefix.substr(0u, 48u);
    const seq_typmod dna4{.alphabet = "dna4", .width = 32};
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string{k_transform_target},
            .typmod = dna4,
            .seq_columns = 1,
            .using_bioseq = true,
        },
        {seq_row{.pk = 1, .sequences = {source}}});
    exec_sql(ctx,
        "CREATE TEMP TABLE cross_concat_queries ("
        "id int PRIMARY KEY, prefix bioseq.seq('dna4',32), "
        "needle bioseq.seq('dna4',32));");
    exec_sql(ctx, fmt::format(
        "INSERT INTO cross_concat_queries VALUES (1, {}, {});",
        format_literal(prefix, dna4), format_literal(needle, dna4)));
    exec_sql(ctx,
        "SET max_parallel_workers_per_gather=0;"
        "SET bioseq.index.sequence_read.enabled=off;"
        "SET bioseq.aligner.vulkan.enabled=off;"
        "SET bioseq.aligner.minimizer.algorithm='scalar';"
        "SET bioseq.aligner.chaining.algorithm='scalar_minimap2';"
        "SET bioseq.aligner.chaining.minimum_score=10;");

    const std::string sql = fmt::format(
        "SELECT t.pk, q.id, bioseq.target_start(t.pk), "
        "bioseq.target_end(t.pk), bioseq.has_cigar(t.pk) "
        "FROM {} AS t INNER JOIN "
        "cross_concat_queries AS q "
        "ON t.pk=q.id AND mmsearch("
        "t.seq0[0:64] OPERATOR(bioseq.||) q.prefix[0:64], "
        "q.needle[0:]) "
        "ORDER BY 3, 4;",
        k_transform_target);
    const auto plan = explain_text<false>(ctx, sql);
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqBatchJoin)"));
    REQUIRE(plan_contains(plan, "Bioseq Mmsearch Ownership: per-row recipe"));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE_FALSE(plan_contains(plan, "Join Filter: mmsearch"));
    REQUIRE_FALSE(plan_contains(plan, "Filter: mmsearch"));

    const auto rows = query_rows(ctx, sql);
    REQUIRE_FALSE(rows.empty());
    bool found_boundary_hit = false;
    for (const auto& row : rows) {
        REQUIRE(row.size() == 5u);
        for (const auto& value : row) REQUIRE(value.has_value());
        REQUIRE(*row[4] == "t");
        found_boundary_hit = found_boundary_hit
            || (std::stoull(*row[2]) == 16u
                && std::stoull(*row[3]) == 112u);
    }
    REQUIRE(found_boundary_hit);
}

TEST_CASE("Outer-join match quals fail closed",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_hoe});
    seed_hoe_fixture(ctx, k_hoe);
    exec_sql(ctx,
        "CREATE TEMP TABLE outer_match_queries "
        "(id int PRIMARY KEY, seq bioseq.seq('dna4',32));");
    exec_sql(ctx, fmt::format(
        "INSERT INTO outer_match_queries VALUES (1, {});",
        batch_join_target_literal()));

    constexpr std::array<std::string_view, 3u> join_types{
        "LEFT", "RIGHT", "FULL"};
    for (const auto join_type : join_types) {
        const std::string sql = fmt::format(
            /* Select columns from both sides so PostgreSQL keeps the outer join
             * and its unsupported mmsearch condition */
            "SELECT t.pk, q.id FROM {} AS t {} JOIN "
            "outer_match_queries AS q "
            "ON t.pk=q.id AND mmsearch(t.seq0[0:], q.seq[0:]);",
            k_hoe, join_type);
        const auto error = sql_error(ctx, sql);
        REQUIRE(error.sqlstate == "0A000");
        REQUIRE((error.contains("not owned")
                 || error.contains("no Bioseq executor owner")));
        REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
    }
}

TEST_CASE("Projection-only slicing is legal above outer joins",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({"outer_projection_target"});
    const seq_typmod dna4{.alphabet = "dna4", .width = 32};
    exec_sql(ctx,
        "CREATE TABLE outer_projection_target "
        "(id int PRIMARY KEY, seq bioseq.seq('dna4',32)) USING bioseq;"
        "CREATE TEMP TABLE outer_projection_query "
        "(id int PRIMARY KEY, seq bioseq.seq('dna4',32));");
    exec_sql(ctx, fmt::format(
        "INSERT INTO outer_projection_target VALUES "
        "(1, {}), (2, {});"
        "INSERT INTO outer_projection_query VALUES "
        "(1, {}), (3, {});",
        format_literal("ACGTACGT", dna4),
        format_literal("TTTTCCCC", dna4),
        format_literal("GGGGAAAA", dna4),
        format_literal("CCCCGGGG", dna4)));

    struct outer_projection_case {
        std::string_view join_type;
        std::string_view expected;
    };
    constexpr std::array<outer_projection_case, 3u> cases{{
        {"LEFT", "1:8,2:NULL"},
        {"RIGHT", "1:8,3:NULL"},
        {"FULL", "1:8,2:NULL,3:NULL"},
    }};
    for (const auto& test_case : cases) {
        const std::string inner = fmt::format(
            "SELECT COALESCE(t.id,q.id) AS id, "
            "bioseq.len(t.seq[0:4] OPERATOR(bioseq.||) q.seq[0:4]) AS n "
            "FROM outer_projection_target AS t {} JOIN "
            "outer_projection_query AS q ON t.id=q.id",
            test_case.join_type);
        const auto plan = explain_text<false>(ctx, inner);
        REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
        REQUIRE(query_scalar(ctx, fmt::format(
            "SELECT string_agg(id::text || ':' || COALESCE(n::text,'NULL'), "
            "',' ORDER BY id) FROM ({} ) AS projected;",
            inner)) == test_case.expected);
    }
}
