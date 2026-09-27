/**
 * @file mmsearch_aligner_options_backend_test.cpp
 * @brief Tests per-call aligner settings and Vulkan backend selection.
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

// Run the same execution check for each named aligner option.
namespace {
void check_aligner_options_partial_override(std::string_view named_arg_sql) {
    auto ctx = fresh_context({k_hoe});
    seed_hoe_fixture(ctx, k_hoe);

    const std::string overridden_sql = fmt::format(
        "SELECT pk FROM {} "
        "WHERE mmsearch(seq0, {}, aligner_options({})) "
        "ORDER BY pk;",
        k_hoe, hoe_probe_literal(), named_arg_sql);

    // An override may change the matches. The query must succeed and return
    // only keys present in the fixture.
    const auto pks = query_int_column(ctx, overridden_sql);
    const std::set<long long> seeded{1, 2, 3, 4, 5};
    for (const auto pk : pks) {
        REQUIRE(seeded.contains(pk));
    }
}
}  // namespace

TEST_CASE("aligner_options(kmer_size => 11) executes without errors",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    check_aligner_options_partial_override("kmer_size => 11");
}

// A per-call kmer_size setting must return the same rows as the matching
// bioseq.aligner.minimizer.kmer_size session setting.
TEST_CASE("aligner_options(kmer_size => K) matches SET bioseq.aligner.minimizer.kmer_size = K",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_hoe});
    seed_hoe_fixture(ctx, k_hoe);

    constexpr int k_kmer_size = 11;

    rowset_t via_options;
    rowset_t via_guc;
    {
        // Set kmer_size for this call.
        const std::string sql = fmt::format(
            "SELECT pk FROM {} "
            "WHERE mmsearch(seq0, {}, aligner_options(kmer_size => {})) "
            "ORDER BY pk;",
            k_hoe, hoe_probe_literal(), k_kmer_size);
        via_options = query_rows(ctx, sql);
    }
    {
        guc_scope guc(ctx, "bioseq.aligner.minimizer.kmer_size",
                      std::to_string(k_kmer_size));
        const std::string sql = fmt::format(
            "SELECT pk FROM {} WHERE mmsearch(seq0, {}) ORDER BY pk;",
            k_hoe, hoe_probe_literal());
        via_guc = query_rows(ctx, sql);
    }

    REQUIRE(via_options == via_guc);
}

TEST_CASE("aligner_options(window_size => 5) executes without errors",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    check_aligner_options_partial_override("window_size => 5");
}

TEST_CASE("aligner_options(max_occ => 69) executes without errors",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    check_aligner_options_partial_override("max_occ => 69");
}

TEST_CASE("aligner_options(chaining_algorithm => 0) executes without errors",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    check_aligner_options_partial_override("chaining_algorithm => 0");
}

TEST_CASE("PostgreSQL finalizes SIMD chaining policy after the last override",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_hoe});
    seed_hoe_fixture(ctx, k_hoe);

    exec_sql(ctx, "SET bioseq.index.sequence_read.enabled = off;");
    exec_sql(ctx, "SET bioseq.aligner.minimizer.algorithm = 'two_stack_nthash';");
    exec_sql(ctx, "SET bioseq.aligner.base_alignment.algorithm = 'block_aligner';");

    SECTION("a per-call SIMD override selects bounded after a scalar GUC") {
        exec_sql(ctx, "SET bioseq.aligner.chaining.algorithm = 'scalar_minimap2';");
        const std::string sql = fmt::format(
            "SELECT pk FROM {} WHERE mmsearch(seq0, {}, "
            "aligner_options(chaining_algorithm => 1)) ORDER BY pk;",
            k_hoe,
            hoe_probe_literal());
        const auto plan = explain_text<true>(ctx, sql);
        REQUIRE(plan_contains(plan, "| chain_stage: Minimap2 SIMD"));
        REQUIRE(plan_contains(
            plan,
            "simd_chaining_policy: bounded_batch_approximate"));
        REQUIRE(plan_contains(plan, "backend_fallbacks=0"));
    }

    SECTION("a per-call scalar override clears bounded after a SIMD GUC") {
        exec_sql(ctx, "SET bioseq.aligner.chaining.algorithm = 'simd_minimap2';");
        const std::string sql = fmt::format(
            "SELECT pk FROM {} WHERE mmsearch(seq0, {}, "
            "aligner_options(chaining_algorithm => 0)) ORDER BY pk;",
            k_hoe,
            hoe_probe_literal());
        const auto plan = explain_text<true>(ctx, sql);
        REQUIRE(plan_contains(plan, "| chain_stage: Minimap2 Scalar"));
        REQUIRE_FALSE(plan_contains(
            plan,
            "simd_chaining_policy: bounded_batch_approximate"));
        REQUIRE(plan_contains(plan, "backend_fallbacks=0"));
    }

    SECTION("RMQ and Vulkan overrides never carry the bounded SIMD policy") {
        exec_sql(ctx, "SET bioseq.aligner.chaining.algorithm = 'simd_minimap2';");
        exec_sql(ctx, "SET bioseq.aligner.vulkan.enabled = off;");
        for (const int ordinal : {2, 3}) {
            CAPTURE(ordinal);
            guc_scope runtime_enabled(
                ctx, "bioseq.aligner.vulkan.enabled", ordinal == 3 ? "on" : "off");
            const std::string sql = fmt::format(
                "SELECT pk FROM {} WHERE mmsearch(seq0, {}, "
                "aligner_options(chaining_algorithm => {})) ORDER BY pk;",
                k_hoe,
                hoe_probe_literal(),
                ordinal);
            const auto plan = explain_text<false>(ctx, sql);
            REQUIRE_FALSE(plan_contains(
                plan,
                "simd_chaining_policy: bounded_batch_approximate"));
            if (ordinal == 3) {
                REQUIRE(plan_contains(plan, "| chain_stage: Vulkan tiled DP"));
            }
        }
    }
}

TEST_CASE("selected Vulkan minimizer reports a Postgres error when Vulkan is disabled",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_hoe});
    seed_hoe_fixture(ctx, k_hoe);

    exec_sql(ctx, "SET bioseq.aligner.minimizer.algorithm = 'vulkan_minimap_integer';");
    exec_sql(ctx, "SET bioseq.aligner.vulkan.enabled = off;");

    const std::string sql = fmt::format(
        "SELECT pk FROM {} WHERE mmsearch(seq0, {}) ORDER BY pk;",
        k_hoe,
        hoe_probe_literal());
    const auto err = sql_error(ctx, sql);
    REQUIRE(err.sqlstate == "XX000");
    REQUIRE(err.contains("BioseqColumnScan Begin raised a C++ exception"));
    REQUIRE(err.contains("selected Vulkan seeding is disabled"));
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE("experimental GPU CIGAR and MAPQ are unreachable for campaign routes",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_hoe});
    seed_hoe_fixture(ctx, k_hoe);

    exec_sql(ctx, "SET bioseq.index.sequence_read.enabled = off;");
    exec_sql(ctx, "SET bioseq.aligner.base_alignment.algorithm = 'block_aligner';");
    exec_sql(ctx, "SET bioseq.aligner.chaining.algorithm = 'vulkan_tiled_dynamic_programming';");
    exec_sql(ctx, "SET bioseq.aligner.minimizer.algorithm = 'vulkan_minimap_integer';");
    exec_sql(ctx, "SET bioseq.aligner.vulkan.enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan.seed_lookup.enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan.index_cache.enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan.fused_frontend.enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan.index_build.enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan.alignment_extension.enabled = on;");
    const bool untrusted_runtime = selected_vulkan_runtime_untrusted(ctx);

    const std::string sql = fmt::format(
        "SELECT pk, bioseq.cigar(pk), bioseq.mapq(pk) "
        "FROM {} "
        "WHERE mmsearch(seq0, {}, aligner_options(index_read_requirement => 1)) "
        "ORDER BY pk;",
        k_hoe,
        hoe_probe_literal());
    const auto err = sql_error(ctx, sql);
    REQUIRE(err.sqlstate == "XX000");
    REQUIRE(err.contains("Vulkan aligner execution failed"));
    if (untrusted_runtime) {
        REQUIRE((err.contains("software/CPU Vulkan device")
                 || err.contains("usable Vulkan runtime")));
    } else {
        REQUIRE(err.contains("Vulkan canonical seed route"));
        REQUIRE(err.contains("MPF Vulkan seed route has no matching statement target owner"));
    }
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE("selected Vulkan batch rejects forced chunked routing before generic biological work",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_hoe});
    seed_hoe_fixture(ctx, k_hoe);
    const bool untrusted_runtime = selected_vulkan_runtime_untrusted(ctx);

    exec_sql(ctx, "SET bioseq.index.sequence_read.enabled = off;");
    exec_sql(ctx, "SET bioseq.testing.chunked_scanner.enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.base_alignment.algorithm = 'block_aligner';");
    exec_sql(ctx, "SET bioseq.aligner.chaining.algorithm = 'vulkan_tiled_dynamic_programming';");
    exec_sql(ctx, "SET bioseq.aligner.minimizer.algorithm = 'vulkan_minimap_integer';");
    exec_sql(ctx, "SET bioseq.aligner.vulkan.enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan.seed_lookup.enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan.index_cache.enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan.fused_frontend.enabled = off;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan.index_build.enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan.alignment_extension.enabled = off;");

    const std::string sql = fmt::format(
        "SELECT pk FROM {} WHERE mmsearch(seq0, {}) ORDER BY pk;",
        k_hoe,
        hoe_probe_literal());
    const auto err = sql_error(ctx, sql);
    REQUIRE(err.sqlstate == "XX000");
    REQUIRE(err.contains("Vulkan aligner execution failed"));
    if (!untrusted_runtime) {
        REQUIRE(err.contains("forced chunked test scanner"));
        REQUIRE(err.contains("host biological fallback is forbidden"));
    }
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE("selected Vulkan seed and chain use the selected host CIGAR and MAPQ suffix",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_hoe});
    seed_hoe_fixture(ctx, k_hoe);

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
    // Set a threshold that lets the 29-base probe produce a tiled-DP match.
    exec_sql(ctx, "SET bioseq.aligner.chaining.minimum_score = 20;");

    const std::string sql = fmt::format(
        "SELECT pk, bioseq.cigar(pk), bioseq.has_cigar(pk), bioseq.mapq(pk) "
        "FROM {} "
        "WHERE mmsearch(seq0, {}, aligner_options(index_read_requirement => 1)) "
        "ORDER BY pk;",
        k_hoe,
        hoe_probe_literal());
    if (selected_vulkan_runtime_untrusted(ctx)) {
        const auto err = sql_error(ctx, sql);
        REQUIRE(err.sqlstate == "XX000");
        REQUIRE(err.contains("Vulkan aligner execution failed"));
        REQUIRE((err.contains("software/CPU Vulkan device")
                 || err.contains("usable Vulkan runtime")));
        REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
        return;
    }

    const auto rows = query_rows(ctx, sql);
    REQUIRE_FALSE(rows.empty());
    for (const auto& row : rows) {
        REQUIRE(row.size() == 4);
        REQUIRE(row[1].has_value());
        REQUIRE_FALSE(row[1].value().empty());
        REQUIRE(is_valid_cigar(row[1].value()));
        REQUIRE(row[2].has_value());
        REQUIRE(row[2].value() == "t");
        REQUIRE(row[3].has_value());
        const auto mapq = std::stoll(row[3].value());
        REQUIRE(mapq >= 0);
        REQUIRE(mapq <= 60);
    }

    const auto plan = explain_text<true>(ctx, sql);
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqScan)"));
    REQUIRE(plan_contains(plan, "| chain_stage: Vulkan tiled DP"));
    REQUIRE(plan_contains(plan, "| align_stage: Block Aligner"));
    REQUIRE(plan_contains(plan, "| mapq_stage: Compute mapping quality (mm_set_mapq2)"));
    REQUIRE_FALSE(plan_contains(plan, "GPU CIGAR"));
    REQUIRE_FALSE(plan_contains(plan, "GPU MAPQ"));
    REQUIRE(plan_contains(plan, "GPU dispatch [seed="));
    REQUIRE_FALSE(plan_contains(plan, "GPU dispatch [seed=0 "));
    REQUIRE_FALSE(plan_contains(plan, " chain=0 chain_backtrack="));
    // Tiled DP runs on Vulkan, then the CPU follows the predecessors to
    // recover each chain. There should be no GPU backtracking dispatch.
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

TEST_CASE(
    "aligner_options(base_alignment_algorithm => 0) executes without errors",
    "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    check_aligner_options_partial_override("base_alignment_algorithm => 0");
}
