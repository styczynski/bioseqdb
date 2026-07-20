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

#ifdef BIOSEQ_TEST_INDEXES
TEST_CASE("DOC0377 expression index CREATE REINDEX and VACUUM FULL hydrate locator sources",
          "[indexing][expression][slice][source-backed][rewrite][doc0377]") {
    constexpr std::string_view table = "doc0377_expr_rewrite";
    constexpr std::string_view index = "doc0377_expr_rewrite_slice_idx";
    auto ctx = fresh_context({table});
    exec_sql(ctx,
        "SET bioseq.update_index_on_insert=off;"
        "CREATE TABLE doc0377_expr_rewrite ("
        " id int PRIMARY KEY, s bioseq.seq('dna4',32)) USING bioseq;"
        "INSERT INTO doc0377_expr_rewrite VALUES "
        " (1,repeat('A',40000)::bioseq.seq('dna4',32)),"
        " (2,repeat('C',40000)::bioseq.seq('dna4',32));");

    // A duplicate-key ERROR occurs inside the TableAM's index tuple callback.
    // The next trace self-test requires default ambient TLS, proving the
    // index_tuple classification scope was restored by the PG_CATCH path.
    const auto duplicate = sql_error(ctx,
        "CREATE UNIQUE INDEX doc0377_expr_rewrite_failed_idx "
        "ON doc0377_expr_rewrite ((s[0:0]));");
    REQUIRE(duplicate.sqlstate == "23505");
    REQUIRE(query_scalar(ctx,
        "SELECT bioseq_internal.slice_trace_self_test();") == "t");

    exec_sql(ctx,
        "SELECT bioseq_internal.reset_sequence_source_counters();");
    exec_sql(ctx,
        "CREATE INDEX doc0377_expr_rewrite_slice_idx "
        "ON doc0377_expr_rewrite "
        "((bioseq.reverse(s[4:36]) OPERATOR(bioseq.||) "
        "s[16380:16412]));");
    REQUIRE(query_scalar_int(ctx,
        "SELECT (bioseq_internal.sequence_source_counters()"
        "->>'standalone_providers_created')::bigint;") == 1);
    REQUIRE(query_scalar_int(ctx,
        "SELECT (bioseq_internal.sequence_source_counters()"
        "->>'domains_created')::bigint;") == 1);
    REQUIRE(query_scalar_int(ctx,
        "SELECT (bioseq_internal.sequence_source_counters()"
        "->>'domains_reused')::bigint;") >= 1);

    const std::string indexed_query =
        "SELECT id FROM doc0377_expr_rewrite WHERE "
        "(bioseq.reverse(s[4:36]) OPERATOR(bioseq.||) s[16380:16412]) "
        "OPERATOR(bioseq.=) "
        "repeat('A',64)::bioseq.seq('dna4',32) ORDER BY id";
    exec_sql(ctx, "SET enable_seqscan=off;");
    const auto initial_plan = explain_text<false>(ctx, indexed_query);
    REQUIRE(plan_contains(initial_plan, std::string(index)));
    auto ids = query_int_column(ctx, indexed_query);
    REQUIRE(ids.size() == 1u);
    REQUIRE(ids.front() == 1);

    exec_sql(ctx,
        "SELECT bioseq_internal.reset_sequence_source_counters();");
    exec_sql(ctx, "REINDEX INDEX doc0377_expr_rewrite_slice_idx;");
    REQUIRE(query_scalar_int(ctx,
        "SELECT (bioseq_internal.sequence_source_counters()"
        "->>'standalone_providers_created')::bigint;") == 1);
    ids = query_int_column(ctx, indexed_query);
    REQUIRE(ids.size() == 1u);
    REQUIRE(ids.front() == 1);

    // DOC0377 requires safe VACUUM/table-rewrite interaction, not indexed
    // CLUSTER support. Keep the access method's existing rejection explicit
    // and prove the backend remains usable before exercising the shared
    // relation_copy_for_cluster callback through VACUUM FULL.
    const auto cluster_error = sql_error(ctx,
        "CLUSTER doc0377_expr_rewrite USING "
        "doc0377_expr_rewrite_pkey;");
    REQUIRE(cluster_error.sqlstate == "0A000");
    REQUIRE(cluster_error.contains(
        "clustering columnar tables using indexes is not supported"));
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);

    exec_sql(ctx, "VACUUM FULL doc0377_expr_rewrite;");
    ids = query_int_column(ctx, indexed_query);
    REQUIRE(ids.size() == 1u);
    REQUIRE(ids.front() == 1);
    REQUIRE(query_scalar(ctx,
        "SELECT bioseq.to_text(s[16380:16412]) "
        "FROM doc0377_expr_rewrite WHERE id=1;")
        == std::string(32u, 'A'));
    REQUIRE(query_scalar_int(ctx,
        "SELECT count(*) FROM bioseq_internal.sequence_source_version v "
        "JOIN pg_class c ON c.oid=v.user_relid "
        "WHERE v.user_relid='doc0377_expr_rewrite'::regclass "
        "AND v.state=1 AND v.main_relfilenode=c.relfilenode "
        "AND v.num_fragments=3;") == 2);

    // The direct helper pins zero/aligned/overflow and exact MaxAlloc edges
    // in the same storage/index boundary suite.
    REQUIRE(query_scalar(ctx,
        "SELECT bioseq_internal.materialization_size_self_test();") == "t");
}
#endif
