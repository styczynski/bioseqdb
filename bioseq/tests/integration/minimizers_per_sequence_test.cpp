/**
 * @file
 * @brief Tests merging minimizer sort runs and reading the per-sequence index.
 */
#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.tests.integration.support;

using namespace bioseq_int_tests;

namespace {
/** Return the distinct row IDs and target bounds reported by mmsearch. */
inline std::set<std::tuple<long long, long long, long long>>
mmsearch_set(const connection_test_context& ctx, std::string_view table_name) {
    auto rows =
        query_rows(ctx,
                   fmt::format("SELECT id, bioseq.target_start(id), bioseq.target_end(id) FROM {} "
                               "WHERE mmsearch(target, query) ORDER BY id",
                               table_name));
    std::set<std::tuple<long long, long long, long long>> out;
    for (auto& r : rows)
        if (r[0] && r[1] && r[2])
            out.emplace(std::stoll(*r[0]), std::stoll(*r[1]), std::stoll(*r[2]));
    return out;
}
} // namespace

TEST_CASE("minimizers_per_sequence: multi-run merge + active-read routing",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({"minimizer_sequence_merge"});
    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
    exec_sql(ctx,
             "SELECT bioseq.create_query_target_sample_table('minimizer_sequence_merge', 3, 5000, "
             "100.0, 200000, "
             "0.001, 0.0005, 0.0005, 1.0, 1.0, 0.0, 271828);");

    // Use a small sort buffer so index creation has several runs to merge.
    exec_sql(ctx, "SET bioseq.index_build.run_buffer_bytes = 4096;");
    REQUIRE(query_scalar_int(ctx,
                             "SELECT bioseq.update_index('minimizer_sequence_merge'::regclass, "
                             "'minimizers_per_sequence')") == 3);
    REQUIRE(last_index_merge_run_count(ctx) >= 2); // at least two sorted runs were merged

    // Compute a baseline with stored sequence-index reads disabled.
    const auto recomputed = [&ctx] {
        guc_scope off(ctx, "bioseq.index.sequence_read.enabled", "off");
        return mmsearch_set(ctx, "minimizer_sequence_merge");
    }();
    REQUIRE(recomputed.size() == 3);

    // Search using the stored per-sequence index.
    exec_sql(ctx,
             "SELECT bioseq.set_active_read_index('minimizer_sequence_merge'::regclass, "
             "'minimizers_per_sequence');");
    guc_scope on(ctx, "bioseq.index.sequence_read.enabled", "on");
    const auto assembled = mmsearch_set(ctx, "minimizer_sequence_merge");
    REQUIRE(assembled.size() == 3); // three distinct result tuples

    // Every row matched by the baseline must also match  through the index.
    // This comparison checks row IDs only.
    for (const auto& m : recomputed) {
        const long long id = std::get<0>(m);
        bool found = false;
        for (const auto& a : assembled)
            if (std::get<0>(a) == id) {
                found = true;
                break;
            }
        REQUIRE(found);
    }
}
