// DOC0279 §16.2 — the per-sequence merged (external-sort) index + set_active_read_index
// routing + the multi-run k-way merge (forced via the tiny index_run_buffer_bytes GUC).
#include "common_tests_integration.h"
#include <set>
#include <tuple>
import integration_tests.common;

using namespace bioseq_int_tests;

namespace {
inline std::set<std::tuple<long long, long long, long long>> mmsearch_set(
        const connection_test_context& ctx, std::string_view tbl) {
    auto rows = query_rows(ctx, fmt::format(
        "SELECT id, bioseq.target_start(id), bioseq.target_end(id) FROM {} "
        "WHERE mmsearch(target, query) ORDER BY id", tbl));
    std::set<std::tuple<long long, long long, long long>> out;
    for (auto& r : rows)
        if (r[0] && r[1] && r[2])
            out.emplace(std::stoll(*r[0]), std::stoll(*r[1]), std::stoll(*r[2]));
    return out;
}
}  // namespace

TEST_CASE("DOC0279 minimizers_per_sequence: multi-run merge + active-read routing",
          "[indexing][per-sequence]") {
    auto ctx = fresh_context({"doc0279_ps"});
    exec_sql(ctx, "SET bioseq.update_index_on_insert = off;");
    exec_sql(ctx,
        "SELECT bioseq.create_query_target_sample_table('doc0279_ps', 3, 5000, 100.0, 200000, "
        "0.001, 0.0005, 0.0005, 1.0, 1.0, 0.0, 271828);");

    // Force the external sort to spill >=2 runs per sequence via a tiny run buffer.
    exec_sql(ctx, "SET bioseq.index_run_buffer_bytes = 4096;");
    REQUIRE(query_scalar_int(ctx,
        "SELECT bioseq.update_index('doc0279_ps'::regclass, 'minimizers_per_sequence')") == 3);
    REQUIRE(last_index_merge_run_count(ctx) >= 2);          // the multi-cursor k-way merge ran

    // The per-fragment recompute baseline (default active index, before set_active_read_index).
    const auto recomputed = [&] {
        guc_scope off(ctx, "bioseq.read_sequence_index", "off");
        return mmsearch_set(ctx, "doc0279_ps");
    }();
    REQUIRE(recomputed.size() == 3);

    // Route the read path to the per-sequence assemble.
    exec_sql(ctx, "SELECT bioseq.set_active_read_index('doc0279_ps'::regclass, 'minimizers_per_sequence');");
    guc_scope on(ctx, "bioseq.read_sequence_index", "on");
    const auto assembled = mmsearch_set(ctx, "doc0279_ps");
    REQUIRE(assembled.size() == 3);                        // NON-EMPTY via per-sequence assemble

    // assemble's claimed-match id set is a superset of recompute's (per-segment INIT artifacts allowed).
    for (const auto& m : recomputed) {
        const long long id = std::get<0>(m);
        bool found = false;
        for (const auto& a : assembled) if (std::get<0>(a) == id) { found = true; break; }
        REQUIRE(found);
    }
}
