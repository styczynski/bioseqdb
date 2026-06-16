// DOC0279 §16.2 — bioseq.update_index over STUB-backed rows reindexes via the
// scanner_pg fragmented walk (§11.4). Pins (a) query_key == columnar rowno on the
// reindex path (each row's distinct sub_keys == its fragment count, filed under the
// rowno the query path resolves), and (b) strict assemble==recompute on the genuinely
// new scanner_pg fragmented-globalisation path (dna4, single segment).
#include "common_tests_integration.h"
#include <set>
#include <tuple>
import bioseq.integration_tests.common;

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

TEST_CASE("DOC0279 update_index reindexes stub-backed rows via scanner_pg (query_key==rowno)",
          "[indexing][per-fragment][scanner-pg]") {
    auto ctx = fresh_context({"doc0279_rx"});
    exec_sql(ctx, "SET bioseq.update_index_on_insert = off;");   // no on-insert index; update_index only
    exec_sql(ctx,
        "SELECT bioseq.create_query_target_sample_table('doc0279_rx', 3, 5000, 100.0, 200000, "
        "0.001, 0.0005, 0.0005, 1.0, 1.0, 0.0, 90210);");

    REQUIRE(query_scalar_int(ctx,
        "SELECT bioseq.update_index('doc0279_rx'::regclass, 'minimizers_per_fragment')") == 3);

    // (a) query_key == rowno: every resolved columnar rowno has its full fragment count
    // (~7 for a 200 kbp target), filed under that rowno.
    auto rownos = query_int_column(ctx,
        "SELECT bioseq.column_row_number('doc0279_rx'::regclass, ctid) FROM doc0279_rx ORDER BY 1");
    REQUIRE(rownos.size() == 3);
    for (long long rn : rownos)
        REQUIRE(index_distinct_sub_keys(ctx, "minimizers_per_fragment", "doc0279_rx", rn) >= 7);

    // (b) strict assemble == recompute on the scanner_pg fragmented path.
    guc_scope on(ctx, "bioseq.read_sequence_index", "on");
    const auto assembled = mmsearch_set(ctx, "doc0279_rx");
    REQUIRE(assembled.size() == 3);
    exec_sql(ctx, "SET bioseq.read_sequence_index = off;");
    const auto recomputed = mmsearch_set(ctx, "doc0279_rx");
    REQUIRE(assembled == recomputed);
}
