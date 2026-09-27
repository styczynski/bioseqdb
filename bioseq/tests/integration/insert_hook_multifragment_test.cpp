/**
 * @file insert_hook_multifragment_test.cpp
 * @brief Checks insert-time indexing for sequences spanning several fragments.
 *
 * One insert must populate fragment minimizers and symbol counts. Searching
 * with stored minimizers must return the same coordinates as recomputing them.
 */
#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.tests.integration.support;

using namespace bioseq_int_tests;

namespace {
/** @brief Generates deterministic ACGT-only DNA with the fixture's xorshift stream. */
[[nodiscard]] std::string generate_dna4(std::size_t length, std::uint64_t seed) {
    std::string sequence;
    sequence.reserve(length);
    std::uint64_t state = seed * 2862933555777941757ull + 3037000493ull;
    for (std::size_t index = 0; index < length; ++index) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        sequence.push_back("ACGT"[(state >> 9) & 3]);
    }
    return sequence;
}

/** @brief Returns the unique target-coordinate pairs emitted by mmsearch. */
[[nodiscard]] std::set<std::tuple<long long, long long>>
match_set(const connection_test_context& ctx, std::string_view table_name) {
    const auto rows = query_rows(
        ctx,
        fmt::format("SELECT bioseq.target_start(target) ts, bioseq.target_end(target) te FROM {} "
                    "WHERE mmsearch(target, query) ORDER BY 1,2",
                    table_name));
    std::set<std::tuple<long long, long long>> matches;
    for (const auto& row : rows) {
        if (row[0] && row[1]) {
            matches.emplace(std::stoll(*row[0]), std::stoll(*row[1]));
        }
    }
    return matches;
}
} // namespace

TEST_CASE("INSERT hook builds a multi-fragment index + both on-insert indexes",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({"insert_hook_multifragment"});
    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = on;");
    exec_sql(ctx,
             "CREATE TABLE insert_hook_multifragment (pk int, target bioseq.seq('dna4',32), "
             "query bioseq.seq('dna4',32)) USING bioseq;");
    const std::string target = generate_dna4(200000, 1234567); // spans seven fragments
    const std::string query = target.substr(60000, 2000);      // an exact 2 kbp substring
    exec_sqlf(ctx,
              "INSERT INTO insert_hook_multifragment VALUES (1, '{}'::bioseq.seq('dna4',32), "
              "'{}'::bioseq.seq('dna4',32));",
              target,
              query);

    const long long sequence_row_number =
        query_scalar_int(ctx,
                         "SELECT bioseq.column_row_number('insert_hook_multifragment'::regclass, "
                         "ctid) FROM insert_hook_multifragment LIMIT 1");
    // The 200,000-base target needs seven fragment sub-keys at this fragment size.
    REQUIRE(index_distinct_sub_keys(
                ctx, "minimizers_per_fragment", "insert_hook_multifragment", sequence_row_number) >=
            7);
    // This insert must populate both indexes with an UPDATE_ON_INSERT policy.
    REQUIRE(index_table_row_count(ctx, "minimizers_per_fragment", "insert_hook_multifragment") > 0);
    REQUIRE(index_table_row_count(ctx, "symbol_counts", "insert_hook_multifragment") > 0);

    // Stored and recomputed minimizers must produce the same target coordinates.
    guc_scope sequence_read_on(ctx, "bioseq.index.sequence_read.enabled", "on");
    const auto assembled = match_set(ctx, "insert_hook_multifragment");
    REQUIRE(!assembled.empty());
    exec_sql(ctx, "SET bioseq.index.sequence_read.enabled = off;");
    const auto recomputed = match_set(ctx, "insert_hook_multifragment");
    REQUIRE(assembled == recomputed);
}

TEST_CASE("a manual symbol_counts policy still gives the correct fallback tally",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({"insert_hook_manual_symbol_counts"});
    exec_sql(ctx,
             "CREATE TABLE insert_hook_manual_symbol_counts (pk int, seq0 bioseq.seq('dna4',32)) "
             "USING bioseq;");
    exec_sql(ctx,
             "SELECT bioseq.set_index_policy('insert_hook_manual_symbol_counts'::regclass, "
             "'symbol_counts', 'UPDATE_MANUALLY');");
    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = on;");
    exec_sql(ctx,
             "INSERT INTO insert_hook_manual_symbol_counts VALUES (1, "
             "'AAAACCCCGGGGTTTT'::bioseq.seq('dna4',32));");
    // Fragment minimizers are stored on insert. Symbol counts stay absent under the manual policy.
    REQUIRE(index_table_row_count(
                ctx, "minimizers_per_fragment", "insert_hook_manual_symbol_counts") > 0);
    REQUIRE(index_table_row_count(ctx, "symbol_counts", "insert_hook_manual_symbol_counts") == 0);
    // Check that both scalar and vector scans return the row before checking its
    // computed symbol counts. EXPLAIN ANALYZE should report no removed chunk groups.
    guc_scope sequence_read_off(ctx, "bioseq.index.sequence_read.enabled", "off");
    ctx.clear_notices();
    constexpr std::string_view row_count_query =
        "SELECT count(*) FROM insert_hook_manual_symbol_counts WHERE pk=1";
    {
        guc_scope vectorization_off(ctx, "bioseq.vectorization.enabled", "off");
        REQUIRE(query_scalar_int(ctx, row_count_query) == 1);
    }

    {
        guc_scope vectorization_on(ctx, "bioseq.vectorization.enabled", "on");
        const auto plan = explain_text(ctx, row_count_query);
        std::string plan_text;
        for (const auto& line : plan) {
            plan_text += line;
            plan_text += '\n';
        }
        INFO(plan_text);
        REQUIRE(plan_contains(plan, "BioseqScan"));
        REQUIRE(plan_contains(plan, "Bioseq Vectorized Filter"));
        REQUIRE(find_explain_property(plan, "Bioseq Chunk Groups Removed by Filter") == "0");
        REQUIRE(query_scalar_int(ctx, row_count_query) == 1);
    }

    REQUIRE(query_scalar_int(ctx,
                             "SELECT bioseq.symbol_count(seq0,'A') FROM "
                             "insert_hook_manual_symbol_counts WHERE pk=1") == 4);
    REQUIRE(query_scalar_int(ctx,
                             "SELECT bioseq.symbol_count(seq0,'T') FROM "
                             "insert_hook_manual_symbol_counts WHERE pk=1") == 4);
    // Repeated symbol-count calls must release their PostgreSQL resources cleanly.
    REQUIRE_FALSE(ctx.notice_contains("resource was not closed"));
}
