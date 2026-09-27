/**
 * @file mmsearch_edit_options_policy_vulkan_test.cpp
 * @brief Tests edit filters, aligner options, index policies, and Vulkan execution.
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
inline constexpr std::string_view k_ordinary_vulkan_scan =
    "mmsearch_ordinary_vulkan_scan_int";

void check_single_guc_parity(std::string_view guc) {
    auto ctx = fresh_context({k_hoe});
    seed_hoe_fixture(ctx, k_hoe);

    const std::string sql = fmt::format(
        "SELECT pk FROM {} WHERE mmsearch(seq0, {}) ORDER BY pk;",
        k_hoe,
        hoe_probe_literal());
    expect_invariant_under_guc_toggle(ctx, sql, guc);
}
}  // namespace

#if defined(BIOSEQ_TEST_INDEXES)
TEST_CASE(
    "ordinary Vulkan seeding preserves per-target offsets and the selected host suffix",
    "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    struct route_case {
        std::string_view index_name;
        std::string_view base_alignment;
    };
    constexpr std::array routes{
        route_case{"minimizers_per_fragment", "ksw2"},
        route_case{"minimizers_per_fragment", "block_aligner"},
        route_case{"minimizers_per_sequence", "ksw2"},
        route_case{"minimizers_per_sequence", "block_aligner"},
    };
    const std::string source = high_entropy_sequence(1400u);
    const std::string query = source.substr(500u, 160u);
    const std::string target_one =
        source.substr(0u, 200u) + query + source.substr(850u, 240u);
    const std::string target_two =
        source.substr(250u, 73u) + query + source.substr(1100u, 211u);
    const std::string query_literal = format_literal(
        query, seq_typmod{.alphabet = "dna4", .width = 32});

    for (const auto& route : routes) {
        DYNAMIC_SECTION(route.index_name << "/" << route.base_alignment) {
            auto ctx = fresh_context({k_ordinary_vulkan_scan});
            exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
            create_seq_table(
                ctx,
                seq_table_spec{
                    .name = std::string{k_ordinary_vulkan_scan},
                    .typmod = {.alphabet = "dna4", .width = 32},
                    .seq_columns = 1,
                    .using_bioseq = true,
                },
                {
                    seq_row{.pk = 1, .sequences = {target_one}},
                    seq_row{.pk = 2, .sequences = {target_two}},
                });
            exec_sqlf(ctx,
                "SELECT bioseq.set_active_read_index('{}'::regclass, '{}');",
                k_ordinary_vulkan_scan,
                route.index_name);
            REQUIRE(index_table_row_count(
                        ctx, route.index_name, k_ordinary_vulkan_scan) == 0);
            exec_sql(ctx, fmt::format(
                "SET max_parallel_workers_per_gather = 0;"
                "SET bioseq.index.sequence_read.enabled = off;"
                "SET bioseq.aligner.minimizer.algorithm = 'vulkan_minimap_integer';"
                "SET bioseq.aligner.chaining.algorithm = 'scalar_minimap2';"
                "SET bioseq.aligner.base_alignment.algorithm = '{}';"
                "SET bioseq.aligner.vulkan.enabled = on;"
                "SET bioseq.aligner.vulkan.seed_lookup.enabled = on;"
                "SET bioseq.aligner.vulkan.index_cache.enabled = on;"
                "SET bioseq.aligner.vulkan.fused_frontend.enabled = off;"
                "SET bioseq.aligner.vulkan.index_build.enabled = on;"
                "SET bioseq.aligner.vulkan.alignment_extension.enabled = off;",
                route.base_alignment));

            const std::string sql = fmt::format(
                "SELECT pk, bioseq.query_start(pk), bioseq.query_end(pk), "
                "       bioseq.target_start(pk), bioseq.target_end(pk), "
                "       bioseq.is_reverse(pk), bioseq.mapq(pk), "
                "       bioseq.cigar(pk), bioseq.has_cigar(pk) "
                "FROM {} WHERE mmsearch(seq0, {}, "
                " aligner_options(kmer_size=>15, window_size=>10, "
                "                 min_chain_score=>10, best_n=>1, "
                "                 index_read_requirement=>1)) "
                "ORDER BY pk;",
                k_ordinary_vulkan_scan,
                query_literal);
            const auto plan = explain_text<false>(ctx, sql);
            REQUIRE(plan_contains(plan, "Custom Scan (BioseqScan)"));
            REQUIRE(plan_contains(plan, "| chain_stage: Minimap2 Scalar"));
            REQUIRE_FALSE(plan_contains(plan, "| chain_stage: Vulkan tiled DP"));
            REQUIRE_FALSE(plan_contains(plan, "GPU CIGAR"));
            REQUIRE_FALSE(plan_contains(plan, "GPU MAPQ"));

            if (selected_vulkan_runtime_untrusted(ctx)) {
                const auto error = sql_error(ctx, sql);
                REQUIRE(error.sqlstate == "XX000");
                REQUIRE(error.contains("Vulkan aligner execution failed"));
                REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
                continue;
            }

            reset_mmsearch_source_counters(ctx);
            const auto rows = query_rows(ctx, sql);
            REQUIRE(rows.size() == 2u);
            constexpr std::array<std::uint64_t, 2u> expected_target_starts{200u, 73u};
            for (std::size_t index = 0u; index < rows.size(); ++index) {
                const auto& row = rows[index];
                REQUIRE(row.size() == 9u);
                for (const auto& value : row) {
                    REQUIRE(value.has_value());
                }
                REQUIRE(std::stoull(*row[0]) == index + 1u);
                REQUIRE(std::stoull(*row[1]) == 0u);
                REQUIRE(std::stoull(*row[2]) == query.size());
                REQUIRE(std::stoull(*row[3]) == expected_target_starts[index]);
                REQUIRE(std::stoull(*row[4]) ==
                        expected_target_starts[index] + query.size());
                REQUIRE(*row[5] == "f");
                const auto mapq = std::stoll(*row[6]);
                REQUIRE(mapq >= 0);
                REQUIRE(mapq <= 60);
                REQUIRE(is_valid_cigar(*row[7]));
                REQUIRE(*row[8] == "t");
            }
            require_mmsearch_source_resources_balanced(ctx);
        }
    }
}
#endif

TEST_CASE("mmsearch parity: bioseq.vectorization.enabled on/off agree",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    check_single_guc_parity("bioseq.vectorization.enabled");
}

TEST_CASE("mmsearch parity: bioseq.column_cache.enabled on/off agree",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    check_single_guc_parity("bioseq.column_cache.enabled");
}

TEST_CASE("mmsearch parity: bioseq.planner.index_scan.enabled on/off agree",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    check_single_guc_parity("bioseq.planner.index_scan.enabled");
}

TEST_CASE("mmsearch parity: bioseq.aligner.profiling.enabled on/off agree",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    check_single_guc_parity("bioseq.aligner.profiling.enabled");
}

TEST_CASE("bioseq.edit strict predicate drives SneakySnake and exact post-filter",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_sneaky_snake_edits});
    std::string query(320, 'A');
    std::uint32_t state = 0x9e3779b9u;
    constexpr std::string_view alphabet = "ACGT";
    for (char& base : query) {
        state ^= state << 13u;
        state ^= state >> 17u;
        state ^= state << 5u;
        base = alphabet[state & 3u];
    }
    auto with_substitutions = [&query, alphabet](const std::size_t count) {
        std::string sequence = query;
        for (std::size_t index = 0; index < count; ++index) {
            const std::size_t position = 32u + index * 27u;
            const std::size_t rank = alphabet.find(sequence[position]);
            sequence[position] = alphabet[(rank + 1u) & 3u];
        }
        return sequence;
    };
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string{k_sneaky_snake_edits},
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 2,
            .using_bioseq = true,
        },
        {
            seq_row{.pk = 1, .sequences = {query, query}},
            seq_row{.pk = 2, .sequences = {
                with_substitutions(9), query}},
            seq_row{.pk = 3, .sequences = {
                with_substitutions(10), query}},
        });
    exec_sql(ctx, "SET bioseq.aligner.base_alignment.algorithm = 'ksw2';");
    exec_sql(ctx, "SET bioseq.aligner.chaining.minimum_score = 10;");
    exec_sql(ctx, "SET bioseq.vectorization.enabled = on;");

    const std::string query_literal = format_literal(
        query, seq_typmod{.alphabet = "dna4", .width = 32});
    const std::string exact_projection_sql = fmt::format(
        "SELECT pk, bioseq.edit(seq0), bioseq.query_start(seq0), "
        "       bioseq.cigar(seq0) "
        "FROM {} WHERE mmsearch(seq0, {}) ORDER BY pk;",
        k_sneaky_snake_edits,
        query_literal);
    const auto exact_rows = query_rows(ctx, exact_projection_sql);
    REQUIRE(exact_rows.size() == 3);
    REQUIRE(exact_rows[0][0] == std::optional<std::string>{"1"});
    REQUIRE(exact_rows[0][1] == std::optional<std::string>{"0"});
    REQUIRE(exact_rows[1][0] == std::optional<std::string>{"2"});
    REQUIRE(exact_rows[1][1] == std::optional<std::string>{"9"});
    REQUIRE(exact_rows[2][0] == std::optional<std::string>{"3"});
    REQUIRE(exact_rows[2][1] == std::optional<std::string>{"10"});

    const std::string sql = fmt::format(
        "SELECT pk, bioseq.edit(seq0), bioseq.query_start(seq0), "
        "       bioseq.cigar(seq0) "
        "FROM {} "
        "WHERE mmsearch(seq0, {}) AND bioseq.edit(seq0) < 10 "
        "ORDER BY pk;",
        k_sneaky_snake_edits,
        query_literal);
    const auto vectorized = query_rows(ctx, sql);
    exec_sql(ctx, "SET bioseq.vectorization.enabled = off;");
    const auto scalar = query_rows(ctx, sql);
    REQUIRE(vectorized == scalar);
    REQUIRE(vectorized.size() == 2);
    REQUIRE(vectorized[0][0] == std::optional<std::string>{"1"});
    REQUIRE(vectorized[0][1] == std::optional<std::string>{"0"});
    REQUIRE(vectorized[1][0] == std::optional<std::string>{"2"});
    REQUIRE(vectorized[1][1] == std::optional<std::string>{"9"});
    for (const auto& row : vectorized) {
        REQUIRE(row.size() == 4);
        REQUIRE(row[2].has_value());
        REQUIRE(row[3].has_value());
        REQUIRE(is_valid_cigar(*row[3]));
    }

    const auto plan = explain_text<true>(ctx, sql);
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqScan)"));
    REQUIRE(plan_contains(plan, "prefilter_stage"));
    REQUIRE(plan_contains(plan, "Rank edit-distance prefilter"));
    REQUIRE(plan_contains(plan, "max_edits: 9"));
    REQUIRE(plan_contains(plan, "exact_edit_filter_stage"));
    REQUIRE(plan_contains(plan, "exclusive_upper_bound: 10"));

    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT count(*) FROM {} "
        "WHERE mmsearch(seq0, {}) AND bioseq.edit(seq0) < 1;",
        k_sneaky_snake_edits,
        query_literal)) == 1);
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT count(*) FROM {} "
        "WHERE mmsearch(seq0, {}) AND bioseq.edit(seq0) < 0;",
        k_sneaky_snake_edits,
        query_literal)) == 0);

    exec_sql(ctx, "SET bioseq.vectorization.enabled = on;");
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT count(*) FROM {} "
        "WHERE mmsearch(seq0, {}) AND bioseq.edit(seq0) < 10;",
        k_sneaky_snake_edits,
        query_literal)) == 2);
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT count(*) FROM {} "
        "WHERE mmsearch(seq0, {}) AND bioseq.edit(seq0) < 10::bigint;",
        k_sneaky_snake_edits,
        query_literal)) == 2);
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT count(*) FROM {} "
        "WHERE mmsearch(seq0, {}) AND bioseq.edit(seq0) < 10::smallint;",
        k_sneaky_snake_edits,
        query_literal)) == 2);

    const auto wrong_column = sql_error(ctx, fmt::format(
        "SELECT pk FROM {} "
        "WHERE mmsearch(seq0, {}) AND bioseq.edit(seq1) < 10;",
        k_sneaky_snake_edits,
        query_literal));
    REQUIRE(wrong_column.sqlstate == "0A000");
    REQUIRE(wrong_column.contains(
        "bioseq.edit predicate must reference the mmsearch target column"));

    const auto wrong_projection = sql_error(ctx, fmt::format(
        "SELECT bioseq.edit(seq1) FROM {} WHERE mmsearch(seq0, {});",
        k_sneaky_snake_edits,
        query_literal));
    REQUIRE(wrong_projection.sqlstate == "0A000");
    REQUIRE(wrong_projection.contains(
        "bioseq.edit projection must reference the mmsearch target column"));

    const auto wrapped_wrong_projection = sql_error(ctx, fmt::format(
        "SELECT bioseq.edit(COALESCE(seq1, seq1)) FROM {} "
        "WHERE mmsearch(seq0, {});",
        k_sneaky_snake_edits,
        query_literal));
    REQUIRE(wrapped_wrong_projection.sqlstate == "0A000");
    REQUIRE(wrapped_wrong_projection.contains(
        "bioseq.edit requires a bioseq alignment custom scan"));
}

TEST_CASE("bioseq.edit drives exact GateKeeper Vulkan prefilter attestation",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_gatekeeper_edits});
    constexpr std::string_view query =
        "CCCCAAGAAACAAAGTGTTGAGAATAACTAGCTACCAGGTATGCAATCATAAAGAGGACCGCAAGGTGATCAACA";
    constexpr std::string_view four_edits =
        "CCCCAAGAAACAAAGTGTTGAGAATAACAAGCTAGCAGGAATGCATTCATAAAGAGGACCGCAAGGTGATCAACA";
    constexpr std::string_view thirteen_edits =
        "CCCCAAGAAACAAAGTGTTGAGAATTAGTTGGTTCGACGAAAGGATTGAAAAAGAGGACCGCAAGGTGATCAACA";
    static_assert(query.size() == 75u);
    static_assert(four_edits.size() == 75u);
    static_assert(thirteen_edits.size() == 75u);

    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string{k_gatekeeper_edits},
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 1,
            .using_bioseq = true,
        },
        {
            seq_row{.pk = 1, .sequences = {std::string{query}}},
            seq_row{.pk = 2, .sequences = {std::string{four_edits}}},
            seq_row{.pk = 3, .sequences = {std::string{thirteen_edits}}},
        });

    exec_sql(ctx, "SET bioseq.index.sequence_read.enabled = off;");
    exec_sql(ctx, "SET bioseq.aligner.base_alignment.algorithm = 'ksw2';");
    exec_sql(ctx, "SET bioseq.aligner.minimizer.algorithm = 'simd_minimap2_reference';");
    exec_sql(ctx, "SET bioseq.aligner.vulkan.enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.chaining.minimum_score = 10;");
    // Matching bases at both ends provide seed anchors around the edited
    // middle. Allow a two-seed chain so this fixture reaches the edit filter.
    exec_sql(ctx, "SET bioseq.aligner.chaining.minimum_seed_count = 2;");

    const std::string query_literal = format_literal(
        query, seq_typmod{.alphabet = "dna4", .width = 32});
    const std::string active_sql = fmt::format(
        "SELECT pk, bioseq.edit(seq0), bioseq.query_start(seq0), "
        "       bioseq.cigar(seq0) "
        "FROM {} "
        "WHERE mmsearch(seq0, {}, "
        "      aligner_options(kmer_size => 15, window_size => 10, "
        "                      min_chain_score => 1, best_n => 0, "
        "                      vulkan_enabled => true)) "
        "  AND bioseq.edit(seq0) < 5 "
        "ORDER BY pk;",
        k_gatekeeper_edits,
        query_literal);
    const std::string baseline_sql = fmt::format(
        "WITH candidates AS MATERIALIZED ("
        "  SELECT pk, bioseq.edit(seq0) AS ed, "
        "         bioseq.query_start(seq0) AS qs, "
        "         bioseq.cigar(seq0) AS cigar "
        "  FROM {} "
        "  WHERE mmsearch(seq0, {}, "
        "        aligner_options(kmer_size => 15, window_size => 10, "
        "                        min_chain_score => 1, best_n => 0, "
        "                        vulkan_enabled => true))"
        ") "
        "SELECT pk, ed, qs, cigar "
        "FROM candidates WHERE ed < 5 ORDER BY pk;",
        k_gatekeeper_edits,
        query_literal);

    const std::string implicit_vulkan_capability_sql = fmt::format(
        "SELECT pk, bioseq.edit(seq0) FROM {} "
        "WHERE mmsearch(seq0, {}, "
        "      aligner_options(kmer_size => 15, window_size => 10, "
        "                      min_chain_score => 1, best_n => 0)) "
        "  AND bioseq.edit(seq0) < 5 ORDER BY pk;",
        k_gatekeeper_edits,
        query_literal);
    const std::string explicit_false_sql = fmt::format(
        "SELECT pk, bioseq.edit(seq0) FROM {} "
        "WHERE mmsearch(seq0, {}, "
        "      aligner_options(kmer_size => 15, window_size => 10, "
        "                      min_chain_score => 1, best_n => 0, "
        "                      vulkan_enabled => false)) "
        "  AND bioseq.edit(seq0) < 5 ORDER BY pk;",
        k_gatekeeper_edits,
        query_literal);

    exec_sql(ctx, "SET bioseq.vectorization.enabled = on;");
    const auto selected_plan = explain_text<false>(ctx, active_sql);
    require_gatekeeper_explain_selection(selected_plan);
    require_cpu_prefilter_explain_selection(
        explain_text<false>(ctx, implicit_vulkan_capability_sql));
    require_cpu_prefilter_explain_selection(
        explain_text<false>(ctx, explicit_false_sql));
    require_no_prefilter_explain_selection(
        explain_text<false>(ctx, baseline_sql));

    exec_sql(ctx, "SET bioseq.aligner.vulkan.enabled = off;");
    const std::array<std::string, 2u> stacked_explicit_sqls{
        fmt::format(
            "SELECT pk, bioseq.edit(seq0) FROM {} WHERE "
            "mmsearch(seq0, {}, aligner_options(vulkan_enabled => true)) "
            "AND mmsearch(seq0, {}, aligner_options()) "
            "AND bioseq.edit(seq0) < 5 ORDER BY pk;",
            k_gatekeeper_edits, query_literal, query_literal),
        fmt::format(
            "SELECT pk, bioseq.edit(seq0) FROM {} WHERE "
            "mmsearch(seq0, {}, aligner_options()) "
            "AND mmsearch(seq0, {}, aligner_options(vulkan_enabled => true)) "
            "AND bioseq.edit(seq0) < 5 ORDER BY pk;",
            k_gatekeeper_edits, query_literal, query_literal),
    };
    for (const auto& stacked_sql : stacked_explicit_sqls) {
        const auto stacked_error = sql_error(ctx, stacked_sql);
        REQUIRE(stacked_error.sqlstate == "0A000");
        REQUIRE(stacked_error.contains("stacked mmsearch"));
    }
    const auto conflicting_vulkan_options = sql_error(ctx, fmt::format(
        "SELECT pk, bioseq.edit(seq0) FROM {} WHERE "
        "mmsearch(seq0, {}, aligner_options(vulkan_enabled => true)) "
        "AND mmsearch(seq0, {}, aligner_options(vulkan_enabled => false)) "
        "AND bioseq.edit(seq0) < 5 ORDER BY pk;",
        k_gatekeeper_edits, query_literal, query_literal));
    REQUIRE(conflicting_vulkan_options.sqlstate == "0A000");
    REQUIRE(conflicting_vulkan_options.contains("stacked mmsearch"));
    exec_sql(ctx, "SET bioseq.aligner.vulkan.enabled = on;");

    if (selected_vulkan_runtime_untrusted(ctx)) {
        const auto error = sql_error(ctx, active_sql);
        REQUIRE(error.sqlstate == "XX000");
        REQUIRE(error.contains("Vulkan aligner execution failed"));
        REQUIRE((
            error.contains("requires a usable Vulkan runtime")
            || error.contains("requires a hardware GPU")));
        REQUIRE_FALSE(error.contains("complete-tuple admission"));
        REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
        SKIP("hardware Vulkan SQL execution is unavailable; route selection "
             "and fail-closed detail were checked");
    }

    rowset_t reference_active;
    rowset_t reference_baseline;
    bool reference_recorded = false;
    for (const std::string_view vectorization : {"on", "off"}) {
        CAPTURE(vectorization);
        exec_sql(ctx, fmt::format(
            "SET bioseq.vectorization.enabled = {};", vectorization));

        const auto active_rows = query_rows(ctx, active_sql);
        require_gatekeeper_edit_rows(active_rows);
        const auto active_plan = explain_text<true>(ctx, active_sql);
        require_gatekeeper_explain_attestation(active_plan);

        const auto baseline_rows = query_rows(ctx, baseline_sql);
        require_gatekeeper_edit_rows(baseline_rows);
        REQUIRE(active_rows == baseline_rows);
        const auto baseline_plan = explain_text<true>(ctx, baseline_sql);
        require_no_prefilter_explain_selection(baseline_plan);

        if (!reference_recorded) {
            reference_active = active_rows;
            reference_baseline = baseline_rows;
            reference_recorded = true;
        } else {
            REQUIRE(active_rows == reference_active);
            REQUIRE(baseline_rows == reference_baseline);
        }
    }
}

TEST_CASE("bioseq.edit rejects unsupported predicate shapes",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_hoe});
    seed_hoe_fixture(ctx, k_hoe);
    const std::string sql = fmt::format(
        "SELECT pk FROM {} WHERE mmsearch(seq0, {}) "
        "AND bioseq.edit(seq0) <= 10;",
        k_hoe,
        hoe_probe_literal());
    const auto error = sql_error(ctx, sql);
    REQUIRE(error.sqlstate == "0A000");
    REQUIRE(error.contains("unsupported bioseq.edit predicate"));
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);

    const auto direct_error = sql_error(ctx, "SELECT bioseq.edit(1);");
    REQUIRE(direct_error.sqlstate == "0A000");
    REQUIRE(direct_error.contains(
        "bioseq.edit requires a bioseq alignment custom scan"));
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);

    const auto scan_without_alignment = sql_error(ctx, fmt::format(
        "SELECT bioseq.edit(seq0) FROM {} LIMIT 1;", k_hoe));
    REQUIRE(scan_without_alignment.sqlstate == "0A000");
    REQUIRE(scan_without_alignment.contains(
        "bioseq.edit requires mmsearch in the same scan"));
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE("mmsearch with all-NULL aligner_options matches the GUC defaults",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_hoe});
    seed_hoe_fixture(ctx, k_hoe);

    const std::string base_sql = fmt::format(
        "SELECT pk FROM {} WHERE mmsearch(seq0, {}) ORDER BY pk;",
        k_hoe,
        hoe_probe_literal());

    const std::string overridden_sql = fmt::format(
        "SELECT pk FROM {} WHERE mmsearch(seq0, {}, aligner_options()) "
        "ORDER BY pk;",
        k_hoe,
        hoe_probe_literal());

    const std::string null_options_sql = fmt::format(
        "SELECT pk FROM {} WHERE mmsearch(seq0, {}, "
        "NULL::bioseq_aligner_options) ORDER BY pk;",
        k_hoe,
        hoe_probe_literal());

    REQUIRE(query_rows(ctx, base_sql) == query_rows(ctx, overridden_sql));
    REQUIRE(query_rows(ctx, base_sql) == query_rows(ctx, null_options_sql));
}

TEST_CASE("Raw and folded Const aligner options retain ownership",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_hoe});
    seed_hoe_fixture(ctx, k_hoe);

    // Test both a composite constant and options folded to a constant from
    // CASE. Each sets kmer_size to 15 and leaves the other 35 fields NULL.
    std::string raw_options = "(15";
    raw_options.append(35u, ',');
    raw_options.push_back(')');
    const std::string raw_sql = fmt::format(
        "SELECT pk FROM {} WHERE mmsearch(seq0, {}, "
        "'{}'::bioseq_aligner_options) ORDER BY pk;",
        k_hoe, hoe_probe_literal(), raw_options);
    const std::string folded_sql = fmt::format(
        "SELECT pk FROM {} WHERE mmsearch(seq0, {}, "
        "CASE WHEN true THEN aligner_options(kmer_size => 15) "
        "ELSE NULL::bioseq_aligner_options END) ORDER BY pk;",
        k_hoe, hoe_probe_literal());

    for (const auto& sql : {raw_sql, folded_sql}) {
        const auto plan = explain_text<false>(ctx, sql);
        REQUIRE(plan_contains(plan, "Custom Scan (BioseqScan)"));
        REQUIRE_FALSE(plan_contains(plan, "Filter: mmsearch"));
        REQUIRE_FALSE(plan_contains(plan, "One-Time Filter: mmsearch"));
    }
    REQUIRE(query_rows(ctx, raw_sql) == query_rows(ctx, folded_sql));
}

TEST_CASE("NULL target and query bypass both mmsearch arities",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_hoe});
    seed_hoe_fixture(ctx, k_hoe);
    exec_sql(ctx, "SET max_parallel_workers_per_gather = 0;");
    exec_sql(ctx, "SET bioseq.index.sequence_read.enabled = off;");

    REQUIRE_FALSE(query_rows(ctx, index_requirement_sql(k_hoe, 1)).empty());
    const long long prior_execution = stats_i64(
        last_alignment_stats(ctx).front(), 0);
    const std::array<std::string, 4u> null_calls{
        fmt::format(
            "mmsearch(NULL::bioseq.seq('dna4',32), {})",
            hoe_probe_literal()),
        "mmsearch(seq0, NULL::bioseq.seq('dna4',32))",
        fmt::format(
            "mmsearch(NULL::bioseq.seq('dna4',32), {}, "
            "NULL::bioseq_aligner_options)",
            hoe_probe_literal()),
        "mmsearch(seq0, NULL::bioseq.seq('dna4',32), "
        "NULL::bioseq_aligner_options)",
    };
    for (const auto& call : null_calls) {
        REQUIRE(query_scalar_int(ctx, fmt::format(
            "SELECT count(*) FROM {} WHERE {};", k_hoe, call)) == 0);
        REQUIRE(stats_i64(last_alignment_stats(ctx).front(), 0)
            == prior_execution);
    }
}

TEST_CASE("aligner_options rejects an invalid index read requirement ordinal",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_hoe});
    seed_hoe_fixture(ctx, k_hoe);
    for (const int invalid_ordinal : {-1, 3}) {
        INFO("invalid ordinal=" << invalid_ordinal);
        const std::string sql = fmt::format(
            "SELECT pk FROM {} WHERE mmsearch(seq0, {}, "
            "aligner_options(index_read_requirement => {})) ORDER BY pk;",
            k_hoe,
            hoe_probe_literal(),
            invalid_ordinal);
        const auto err = sql_error(ctx, sql);
        REQUIRE(err.sqlstate == "22023");
        REQUIRE(err.contains("index_read_requirement out of range"));
    }
}

TEST_CASE("aligner_options exposes the appended index requirement field",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    connection_test_context ctx;
    REQUIRE(query_scalar(
        ctx,
        "SELECT ((aligner_options()).index_read_requirement IS NULL)::text;")
        == "true");
    REQUIRE(query_scalar_int(
        ctx,
        "SELECT (aligner_options(index_read_requirement => 0))."
        "index_read_requirement;") == 0);
    REQUIRE(query_scalar(
        ctx,
        "SELECT string_agg(attname || ':' || atttypid::text, ',' "
        "ORDER BY attnum) FROM pg_attribute "
        "WHERE attrelid='bioseq_aligner_options'::regclass AND attnum > 0;")
        == "kmer_size:23,window_size:23,max_gap:23,max_gap_ref:23,"
           "bandwidth:23,bandwidth_long:23,max_chain_skip:23,"
           "max_chain_iter:23,min_seed_count:23,min_chain_score:23,"
           "chain_gap_scale:701,chain_skip_scale:701,mask_level:701,"
           "mask_len:23,pri_ratio:701,best_n:23,match_score:23,"
           "mismatch_pen:23,gap_open1:23,gap_ext1:23,gap_open2:23,"
           "gap_ext2:23,sc_ambi:23,zdrop:23,end_bonus:23,max_occ:23,"
           "mid_occ:23,block_min_size:23,block_max_size:23,"
           "chaining_algorithm:23,base_alignment_algorithm:23,"
           "minimizer_algorithm:23,vulkan_enabled:16,"
           "vulkan_seed_hits_enabled:16,vulkan_index_cache_enabled:16,"
           "index_read_requirement:23");
}

TEST_CASE("mmsearch rejects a non-Const aligner_options third argument",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_hoe});
    seed_hoe_fixture(ctx, k_hoe);
    exec_sql(ctx, "SET plan_cache_mode = force_generic_plan;");
    exec_sql(ctx, fmt::format(
        "PREPARE mmsearch_nonconst_options(bioseq_aligner_options) AS "
        "SELECT pk FROM {} WHERE mmsearch(seq0, {}, $1) ORDER BY pk;",
        k_hoe,
        hoe_probe_literal()));
    const auto err = sql_error(
        ctx,
        "EXECUTE mmsearch_nonconst_options("
        "aligner_options(index_read_requirement => 1));");
    REQUIRE(err.sqlstate == "0A000");
    REQUIRE((err.contains("not owned")
             || err.contains("no Bioseq executor owner")));
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE("Var subquery and volatile aligner options fail closed",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_hoe});
    seed_hoe_fixture(ctx, k_hoe);
    exec_sql(ctx,
        "CREATE TEMP TABLE options_shape "
        "(id int PRIMARY KEY, value bioseq_aligner_options);"
        "INSERT INTO options_shape VALUES "
        "(1, aligner_options(kmer_size => 15));"
        "CREATE FUNCTION pg_temp.volatile_aligner_options() "
        "RETURNS bioseq_aligner_options LANGUAGE sql VOLATILE "
        "AS $$ SELECT aligner_options(kmer_size => 15) $$;");

    const std::array<std::string, 3u> unsupported_options{
        "o.value",
        "(SELECT value FROM options_shape WHERE id=1)",
        "pg_temp.volatile_aligner_options()",
    };
    for (std::size_t index = 0u; index < unsupported_options.size(); ++index) {
        const bool var_shape = index == 0u;
        const std::string sql = fmt::format(
            "SELECT t.pk FROM {} AS t {} WHERE mmsearch(t.seq0, {}, {}) "
            "ORDER BY t.pk;",
            k_hoe,
            var_shape
                ? "CROSS JOIN options_shape AS o"
                : "",
            hoe_probe_literal(), unsupported_options[index]);
        const auto error = sql_error(ctx, sql);
        REQUIRE(error.sqlstate == "0A000");
        REQUIRE((error.contains("not owned")
                 || error.contains("no Bioseq executor owner")));
        REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
    }
}

TEST_CASE("index read requirement allow ordinal preserves production policy",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_hoe});
    seed_hoe_fixture(ctx, k_hoe);
    const std::string sql = index_requirement_sql(k_hoe, 0);

    exec_sql(ctx, "SET bioseq.index.sequence_read.enabled = off;");
    const auto disabled_plan = explain_text<true>(ctx, sql);
    REQUIRE(plan_contains(
        disabled_plan,
        "requested_index_source=recomputed actual_index_source=recomputed"));
    REQUIRE(plan_contains(disabled_plan, "persisted=0"));

    exec_sql(ctx, "SET bioseq.index.sequence_read.enabled = on;");
    const auto enabled_plan = explain_text<true>(ctx, sql);
    REQUIRE(plan_contains(
        enabled_plan,
        "requested_index_source=persisted actual_index_source=persisted"));
    REQUIRE(plan_contains(enabled_plan, "Index outcomes [targets=5 persisted=5"));
}

TEST_CASE("stacked mmsearch rejects multiple index read requirements",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_hoe});
    seed_hoe_fixture(ctx, k_hoe);
    const std::string second_probe = format_literal(
        "CTAGTCACTGTAGCCTGATCGTAGCTAGC",
        seq_typmod{.alphabet = "dna4", .width = 32});
    const std::string sql = fmt::format(
        "SELECT pk FROM {} WHERE "
        "mmsearch(seq0, {}, aligner_options(index_read_requirement => 1)) "
        "AND mmsearch(seq0, {}, aligner_options(index_read_requirement => 2)) "
        "ORDER BY pk;",
        k_hoe,
        hoe_probe_literal(),
        second_probe);
    const auto err = sql_error(ctx, sql);
    REQUIRE(err.sqlstate == "0A000");
    REQUIRE(err.contains("stacked mmsearch"));
}

TEST_CASE("nested mmsearch Boolean shape fails closed",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_hoe});
    seed_hoe_fixture(ctx, k_hoe);
    const auto error = sql_error(ctx, fmt::format(
        "SELECT pk FROM {} WHERE NOT mmsearch(seq0, {});",
        k_hoe, hoe_probe_literal()));
    REQUIRE(error.sqlstate == "0A000");
    REQUIRE(error.contains("nested mmsearch qualifier shape is unsupported"));
}

TEST_CASE("owned mmsearch is strict for NULL target and query",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_hoe});
    seed_hoe_fixture(ctx, k_hoe);
    exec_sql(ctx, fmt::format("UPDATE {} SET seq0 = NULL WHERE pk = 1;", k_hoe));
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT count(*) FROM {} WHERE seq0 IS NULL AND mmsearch(seq0, {});",
        k_hoe, hoe_probe_literal())) == 0);
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT count(*) FROM {} WHERE mmsearch("
        "seq0, NULL::bioseq.seq('dna4', 32));",
        k_hoe)) == 0);
}

TEST_CASE("stacked mmsearch fails closed independent of order",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_hoe});
    seed_hoe_fixture(ctx, k_hoe);
    exec_sql(ctx, "SET bioseq.index.sequence_read.enabled = on;");
    const std::string second_probe = format_literal(
        "CTAGTCACTGTAGCCTGATCGTAGCTAGC",
        seq_typmod{.alphabet = "dna4", .width = 32});
    const std::array<std::string, 2> sqls{
        fmt::format(
            "SELECT pk FROM {} WHERE "
            "mmsearch(seq0, {}, aligner_options()) AND "
            "mmsearch(seq0, {}, aligner_options(index_read_requirement => 1)) "
            "ORDER BY pk;",
            k_hoe,
            hoe_probe_literal(),
            second_probe),
        fmt::format(
            "SELECT pk FROM {} WHERE "
            "mmsearch(seq0, {}, aligner_options(index_read_requirement => 1)) "
            "AND mmsearch(seq0, {}, aligner_options()) ORDER BY pk;",
            k_hoe,
            hoe_probe_literal(),
            second_probe),
    };
    for (const auto& sql : sqls) {
        const auto error = sql_error(ctx, sql);
        REQUIRE(error.sqlstate == "0A000");
        REQUIRE(error.contains("stacked mmsearch"));
    }
}

TEST_CASE("require_absent bypasses a prewarmed persisted assembled cache",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_hoe});
    seed_hoe_fixture(ctx, k_hoe);
    exec_sql(ctx, "SET bioseq.index.sequence_read.enabled = on;");
    const auto warm_plan = explain_text<true>(
        ctx, index_requirement_sql(k_hoe, 0));
    REQUIRE(plan_contains(warm_plan, "Index outcomes [targets=5 persisted=5"));

    const std::string sql = index_requirement_sql(k_hoe, 1);
    const auto plan = explain_text<true>(ctx, sql);
    REQUIRE(plan_contains(
        plan,
        "requested_index_source=recomputed actual_index_source=recomputed"));
    REQUIRE(plan_contains(plan, "Index outcomes [targets=5 persisted=0"));
    REQUIRE(plan_contains(plan, "missing_persisted=0 recomputed=5]"));
}

TEST_CASE("require_compatible_persisted rejects a complete minimizers_per_sequence index",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_hoe});
    seed_hoe_fixture(ctx, k_hoe);
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT bioseq.update_index('{}'::regclass, "
        "'minimizers_per_sequence');",
        k_hoe)) > 0);
    exec_sql(ctx, fmt::format(
        "SELECT bioseq.set_active_read_index('{}'::regclass, "
        "'minimizers_per_sequence');",
        k_hoe));
    REQUIRE(index_table_row_count(
        ctx, "minimizers_per_sequence", k_hoe) > 0);
    require_no_current_compatible_persisted_index(
        ctx, index_requirement_sql(k_hoe, 2));
}

TEST_CASE("active-read policy preserves configured versus authoritative no-row source",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_hoe});
    seed_hoe_fixture(ctx, k_hoe);
    exec_sql(ctx, "SET bioseq.index.sequence_read.enabled = on;");
    const std::string search = index_requirement_sql(k_hoe, 0);

    const auto policy_debug = [&ctx] {
        const auto rows = query_rows(
            ctx, "SELECT * FROM bioseq_internal.active_read_policy_debug();");
        REQUIRE(rows.size() == 1);
        REQUIRE(rows.front().size() == 9);
        REQUIRE(rows.front()[7].value_or("") == "0");
        REQUIRE(rows.front()[8].value_or("") == "0");
        return rows.front();
    };
    const auto value = [](const row_t& row, const std::size_t column) {
        REQUIRE(column < row.size());
        REQUIRE(row[column].has_value());
        return std::stoll(*row[column]);
    };

    // With no active policy row, use the per-fragment default and record
    // that the choice came from the missing row.
    exec_sqlf(ctx,
        "UPDATE bioseq_internal.index_policy SET is_active_read = false "
        "WHERE user_relid = '{}'::regclass;",
        k_hoe);
    exec_sql(ctx,
        "SELECT bioseq_internal.reset_active_read_policy_debug();");
    (void)query_rows(ctx, search);
    const row_t authoritative = policy_debug();
    REQUIRE(value(authoritative, 0) == 0);
    REQUIRE(value(authoritative, 1) > 0);
    REQUIRE(value(authoritative, 2) == 0);
    REQUIRE(value(authoritative, 3) == 0);
    REQUIRE(value(authoritative, 4) > 0);
    REQUIRE(value(authoritative, 5) == 0);
    REQUIRE(authoritative[6].value_or("")
        == "authoritative_no_row_default");

    // Explicitly selecting the same index must report configured_row, so
    // diagnostics distinguish the user setting from the default.
    exec_sqlf(ctx,
        "SELECT bioseq.set_active_read_index('{}'::regclass, "
        "'minimizers_per_fragment');",
        k_hoe);
    exec_sql(ctx,
        "SELECT bioseq_internal.reset_active_read_policy_debug();");
    (void)query_rows(ctx, search);
    const row_t configured = policy_debug();
    REQUIRE(value(configured, 0) > 0);
    REQUIRE(value(configured, 1) == 0);
    REQUIRE(value(configured, 2) == 0);
    REQUIRE(value(configured, 3) > 0);
    REQUIRE(value(configured, 4) == 0);
    REQUIRE(value(configured, 5) == 0);
    REQUIRE(configured[6].value_or("") == "configured_row");
}

TEST_CASE("require_compatible_persisted errors when target chunks are missing",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_missing_index});
    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string{k_missing_index},
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 1,
            .using_bioseq = true,
        },
        {
            seq_row{
                .pk = 1,
                .sequences = {hoe_seed_rows().front().sequences.front()}},
        });
    exec_sql(ctx, "SET bioseq.index.sequence_read.enabled = off;");
    REQUIRE(index_table_row_count(
        ctx, "minimizers_per_fragment", k_missing_index) == 0);
    require_no_current_compatible_persisted_index(
        ctx, index_requirement_sql(k_missing_index, 2));
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE("require_compatible_persisted rejects before alternate scanner and GPU paths",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_missing_index});
    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string{k_missing_index},
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 1,
            .using_bioseq = true,
        },
        {
            seq_row{.pk = 1, .sequences = {"ACGTACGTACGTACGT"}},
            seq_row{.pk = 2, .sequences = {"TGCATGCATGCATGCA"}},
        });
    const std::string sql = index_requirement_sql(k_missing_index, 2);

    INFO("small inline-eligible target path");
    require_no_current_compatible_persisted_index(ctx, sql);

    INFO("forced in-memory path");
    exec_sql(ctx, "SET bioseq.testing.in_memory_scanner.enabled = on;");
    require_no_current_compatible_persisted_index(ctx, sql);
    exec_sql(ctx, "SET bioseq.testing.in_memory_scanner.enabled = off;");

    INFO("forced chunked path");
    exec_sql(ctx, "SET bioseq.testing.chunked_scanner.enabled = on;");
    require_no_current_compatible_persisted_index(ctx, sql);
    exec_sql(ctx, "SET bioseq.testing.chunked_scanner.enabled = off;");

    INFO("batched selected-Vulkan path");
    exec_sql(ctx, "SET bioseq.aligner.base_alignment.algorithm = 'block_aligner';");
    exec_sql(ctx, "SET bioseq.aligner.chaining.algorithm = 'vulkan_tiled_dynamic_programming';");
    exec_sql(ctx, "SET bioseq.aligner.minimizer.algorithm = 'vulkan_minimap_integer';");
    exec_sql(ctx, "SET bioseq.aligner.vulkan.enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan.seed_lookup.enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan.index_cache.enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan.fused_frontend.enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan.index_build.enabled = on;");
    require_no_current_compatible_persisted_index(ctx, sql);
}

TEST_CASE("require_absent preserves on-insert policy and prohibits external async files",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_missing_index});
    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string{k_missing_index},
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 1,
            .using_bioseq = true,
        });
    exec_sql(ctx, fmt::format(
        "INSERT INTO {} VALUES "
        "(1, repeat('ACGT', 2000)::bioseq.seq('dna4', 32));",
        k_missing_index));
    REQUIRE(index_table_row_count(
        ctx, "minimizers_per_fragment", k_missing_index) == 0);

    exec_sql(ctx, "SET bioseq.index.sequence_read.enabled = on;");
    exec_sql(ctx, "SET bioseq.index.async_persistence.enabled = on;");
    exec_sql(ctx, "SET bioseq.index.recompute_async_persistence.enabled = on;");
    const auto async_runtime = query_one(
        ctx,
        "SELECT current_setting('shared_preload_libraries', true), "
        "is_supervisor::text FROM bioseq.scheduler_stats();");
    REQUIRE(async_runtime.size() == 2);
    REQUIRE(async_runtime[0].has_value());
    REQUIRE(async_runtime[1].has_value());
    INFO("async persistence requires bioseq in shared_preload_libraries and "
         "a live scheduler supervisor; observed shared_preload_libraries='"
         << *async_runtime[0] << "', supervisor=" << *async_runtime[1]);
    REQUIRE(*async_runtime[1] == "true");
    const auto absent_plan = explain_text<true>(
        ctx, index_requirement_sql(k_missing_index, 1));
    REQUIRE(plan_contains(absent_plan, "persisted=0"));
    REQUIRE(plan_contains(absent_plan, "recomputed=1"));
    REQUIRE(query_scalar(ctx, "SHOW bioseq.index.async_persistence.enabled;") == "on");
    REQUIRE(query_scalar(ctx, "SHOW bioseq.index.recompute_async_persistence.enabled;") == "on");
    REQUIRE(query_scalar(ctx, "SHOW bioseq.index.insert_maintenance.enabled;") == "off");

    // Recomputing for this query must leave the persisted index empty and
    // never enter the external-file constructor or writer.
    REQUIRE(index_table_row_count(
        ctx, "minimizers_per_fragment", k_missing_index) == 0);
    const auto producer = query_one(
        ctx,
        "SELECT prohibited_file_constructor_entries, "
        "prohibited_file_writer_entries "
        "FROM bioseq_internal.active_read_policy_debug();");
    REQUIRE(producer.size() == 2);
    REQUIRE(producer[0].value_or("") == "0");
    REQUIRE(producer[1].value_or("") == "0");

    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = on;");
    exec_sql(ctx, fmt::format(
        "INSERT INTO {} VALUES "
        "(2, repeat('TGCA', 2000)::bioseq.seq('dna4', 32));",
        k_missing_index));
    REQUIRE(query_scalar(ctx, "SHOW bioseq.index.insert_maintenance.enabled;") == "on");
    REQUIRE(index_table_row_count(
        ctx, "minimizers_per_fragment", k_missing_index) > 0);
}
