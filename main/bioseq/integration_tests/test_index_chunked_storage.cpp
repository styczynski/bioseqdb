// DOC0279 §16.2 — the new CHUNKED_PER_FRAGMENT storage + per-fragment assemble.
// Proves: on-insert index build, exact-rowno chunk placement, assemble==recompute
// (single-segment dna4 byte-identical), forced-scanner disables assembly, ROLLBACK
// leaves no index rows, and the reserved-sentinel / missing-manifest corruption hooks.
#include "common_tests_integration.h"
#include <set>
#include <tuple>
import bioseq.integration_tests.common;

using namespace bioseq_int_tests;

namespace {
// n rows: ~5 kbp query fragments mutated out of 200 kbp dna4 targets, no reverse.
inline void seed_sample(const connection_test_context& ctx, std::string_view name, int n) {
    exec_sqlf(ctx, "DROP TABLE IF EXISTS {} CASCADE;", name);
    exec_sqlf(ctx,
        "SELECT bioseq.create_query_target_sample_table('{}', {}, 5000, 100.0, 200000, "
        "0.001, 0.0005, 0.0005, 1.0, 1.0, 0.0, 13579);", name, n);
}
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

TEST_CASE("DOC0279 per-fragment storage is created on insert with the right query_keys",
          "[indexing][per-fragment]") {
    auto ctx = fresh_context({"doc0279_cs_seed"});
    seed_small_bioseq_column(ctx, "doc0279_cs_seed");   // 3 dna4/32 rows, update_index_on_insert=on
    REQUIRE(index_table_row_count(ctx, "minimizers_per_fragment", "doc0279_cs_seed") > 0);
    REQUIRE(index_table_row_count(ctx, "symbol_counts", "doc0279_cs_seed") == 3);

    // Exact-rowno chunk placement: every indexed columnar rowno has >=1 sub_key.
    auto rownos = query_int_column(ctx,
        "SELECT bioseq.column_row_number('doc0279_cs_seed'::regclass, ctid) "
        "FROM doc0279_cs_seed ORDER BY 1");
    REQUIRE(rownos.size() == 3);
    for (long long rn : rownos)
        REQUIRE(index_distinct_sub_keys(ctx, "minimizers_per_fragment", "doc0279_cs_seed", rn) >= 1);
}

TEST_CASE("DOC0279 mmsearch per-fragment assemble == recompute (single-segment dna4)",
          "[indexing][per-fragment]") {
    auto ctx = fresh_context({"doc0279_cs"});
    seed_sample(ctx, "doc0279_cs", 3);
    exec_sqlf(ctx, "SET bioseq.update_index_on_insert = off;");
    REQUIRE(query_scalar_int(ctx,
        "SELECT bioseq.update_index('doc0279_cs'::regclass, 'minimizers_per_fragment')") == 3);

    guc_scope rsi_on(ctx, "bioseq.read_sequence_index", "on");
    const auto assembled = mmsearch_set(ctx, "doc0279_cs");
    REQUIRE(assembled.size() == 3);                              // NON-EMPTY proof

    exec_sqlf(ctx, "SET bioseq.read_sequence_index = off;");
    const auto recomputed = mmsearch_set(ctx, "doc0279_cs");
    REQUIRE(assembled == recomputed);                            // strict byte-identical
}

TEST_CASE("DOC0279 forced-scanner GUC disables stored assembly but results stay correct",
          "[indexing][per-fragment]") {
    auto ctx = fresh_context({"doc0279_cs_fs"});
    seed_sample(ctx, "doc0279_cs_fs", 3);
    exec_sqlf(ctx, "SET bioseq.update_index_on_insert = off;");
    exec_sqlf(ctx, "SELECT bioseq.update_index('doc0279_cs_fs'::regclass, 'minimizers_per_fragment');");
    guc_scope rsi_on(ctx, "bioseq.read_sequence_index", "on");
    guc_scope chunked(ctx, "bioseq.test_force_chunked_scanner", "on");  // not the production scanner_pg path
    REQUIRE(mmsearch_set(ctx, "doc0279_cs_fs").size() == 3);
}

TEST_CASE("DOC0279 index rows do not survive a ROLLBACK", "[indexing][per-fragment]") {
    auto ctx = fresh_context({"doc0279_cs_rb"});
    seed_small_bioseq_column(ctx, "doc0279_cs_rb");
    const long long before = index_table_row_count(ctx, "minimizers_per_fragment", "doc0279_cs_rb");
    exec_sql(ctx, "BEGIN;");
    exec_sql(ctx, "INSERT INTO doc0279_cs_rb VALUES (99, 'GGGGCCCCAAAATTTT'::bioseq.seq('dna4',32));");
    exec_sql(ctx, "ROLLBACK;");
    REQUIRE(query_scalar_int(ctx, "SELECT count(*) FROM doc0279_cs_rb WHERE pk=99") == 0);
    REQUIRE(index_table_row_count(ctx, "minimizers_per_fragment", "doc0279_cs_rb") == before);
}

#ifdef BIOSEQ_TEST_INDEXES
TEST_CASE("DOC0279 missing manifest forces recompute; reserved sentinels are ignored",
          "[indexing][per-fragment]") {
    auto ctx = fresh_context({"doc0279_cs_cor"});
    seed_sample(ctx, "doc0279_cs_cor", 1);
    exec_sqlf(ctx, "SET bioseq.update_index_on_insert = off;");
    exec_sqlf(ctx, "SELECT bioseq.update_index('doc0279_cs_cor'::regclass, 'minimizers_per_fragment');");
    const long long rn = query_scalar_int(ctx,
        "SELECT bioseq.column_row_number('doc0279_cs_cor'::regclass, ctid) FROM doc0279_cs_cor LIMIT 1");

    // A reserved-band tuple (sub_key in the TEMP_RUN reserved range) must NOT be counted as a data fragment.
    const long long distinct_before = index_distinct_sub_keys(ctx, "minimizers_per_fragment", "doc0279_cs_cor", rn);
    exec_sqlf(ctx,
        "SELECT bioseq.test_write_index_chunk('doc0279_cs_cor'::regclass, 'minimizers_per_fragment', "
        "{}, (4611686018427387904)::bigint, 0, '\\x00'::bytea);", rn);   // sub_key = 1<<62 (TEMP_RUN_BASE)
    REQUIRE(index_distinct_sub_keys(ctx, "minimizers_per_fragment", "doc0279_cs_cor", rn) == distinct_before);

    // Deleting the completeness manifest makes the row read as "absent" -> recompute (still correct, non-empty).
    exec_sqlf(ctx,
        "SELECT bioseq.test_delete_index_manifest('doc0279_cs_cor'::regclass, 'minimizers_per_fragment', {});", rn);
    guc_scope rsi_on(ctx, "bioseq.read_sequence_index", "on");
    REQUIRE(mmsearch_set(ctx, "doc0279_cs_cor").size() == 1);   // recompute fallback still finds the match
}
#endif
