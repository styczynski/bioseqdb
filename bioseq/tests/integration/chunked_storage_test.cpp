// Per-fragment index storage and assembly integration.
// Covers: on-insert index build, exact-rowno chunk placement, assemble==recompute
// (single-segment dna4 byte-identical), forced-scanner disables assembly, ROLLBACK
// leaves no index rows, and the reserved-sentinel / missing-manifest corruption hooks.
#include <tests/support/postgres.hpp>
#include <set>
#include <string_view>
#include <tuple>
import bioseq.tests.integration.support;

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
        const connection_test_context& ctx, std::string_view table_name) {
    auto rows = query_rows(ctx, fmt::format(
        "SELECT id, bioseq.target_start(id), bioseq.target_end(id) FROM {} "
        "WHERE mmsearch(target, query) ORDER BY id", table_name));
    std::set<std::tuple<long long, long long, long long>> out;
    for (auto& r : rows)
        if (r[0] && r[1] && r[2])
            out.emplace(std::stoll(*r[0]), std::stoll(*r[1]), std::stoll(*r[2]));
    return out;
}
}  // namespace

TEST_CASE("per-fragment storage is created on insert with the right query_keys",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({"chunked_storage_insert"});
    seed_small_bioseq_column(ctx, "chunked_storage_insert");   // 3 dna4/32 rows; insert maintenance on
    REQUIRE(index_table_row_count(ctx, "minimizers_per_fragment", "chunked_storage_insert") > 0);
    REQUIRE(index_table_row_count(ctx, "symbol_counts", "chunked_storage_insert") == 3);

    // Exact-rowno chunk placement: every indexed columnar rowno has >=1 sub_key.
    auto rownos = query_int_column(ctx,
        "SELECT bioseq.column_row_number('chunked_storage_insert'::regclass, ctid) "
        "FROM chunked_storage_insert ORDER BY 1");
    REQUIRE(rownos.size() == 3);
    for (long long sequence_rowno : rownos)
        REQUIRE(index_distinct_sub_keys(
            ctx, "minimizers_per_fragment", "chunked_storage_insert",
            sequence_rowno) >= 1);
}

TEST_CASE("mmsearch per-fragment assemble == recompute (single-segment dna4)",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({"chunked_storage_recompute"});
    seed_sample(ctx, "chunked_storage_recompute", 3);
    exec_sqlf(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
    REQUIRE(query_scalar_int(ctx,
        "SELECT bioseq.update_index('chunked_storage_recompute'::regclass, 'minimizers_per_fragment')") == 3);

    guc_scope sequence_read_on(
        ctx, "bioseq.index.sequence_read.enabled", "on");
    const auto assembled = mmsearch_set(ctx, "chunked_storage_recompute");
    REQUIRE(assembled.size() == 3);                              // non-empty result

    exec_sqlf(ctx, "SET bioseq.index.sequence_read.enabled = off;");
    const auto recomputed = mmsearch_set(ctx, "chunked_storage_recompute");
    REQUIRE(assembled == recomputed);                            // strict byte-identical
}

TEST_CASE("forced-scanner GUC disables stored assembly but results stay correct",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({"chunked_storage_forced_scanner"});
    seed_sample(ctx, "chunked_storage_forced_scanner", 3);
    exec_sqlf(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
    exec_sqlf(ctx, "SELECT bioseq.update_index('chunked_storage_forced_scanner'::regclass, 'minimizers_per_fragment');");
    guc_scope sequence_read_on(
        ctx, "bioseq.index.sequence_read.enabled", "on");
    guc_scope chunked(ctx, "bioseq.testing.chunked_scanner.enabled", "on");  // not the production scanner_pg path
    REQUIRE(mmsearch_set(ctx, "chunked_storage_forced_scanner").size() == 3);
}

TEST_CASE("index rows do not survive a ROLLBACK", "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({"chunked_storage_rollback"});
    seed_small_bioseq_column(ctx, "chunked_storage_rollback");
    const long long before = index_table_row_count(ctx, "minimizers_per_fragment", "chunked_storage_rollback");
    exec_sql(ctx, "BEGIN;");
    exec_sql(ctx, "INSERT INTO chunked_storage_rollback VALUES (99, 'GGGGCCCCAAAATTTT'::bioseq.seq('dna4',32));");
    exec_sql(ctx, "ROLLBACK;");
    REQUIRE(query_scalar_int(ctx, "SELECT count(*) FROM chunked_storage_rollback WHERE pk=99") == 0);
    REQUIRE(index_table_row_count(ctx, "minimizers_per_fragment", "chunked_storage_rollback") == before);
}

#ifdef BIOSEQ_TEST_INDEXES
TEST_CASE("missing manifest forces recompute; reserved sentinels are ignored",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({"chunked_storage_missing_manifest"});
    seed_sample(ctx, "chunked_storage_missing_manifest", 1);
    exec_sqlf(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
    exec_sqlf(ctx, "SELECT bioseq.update_index('chunked_storage_missing_manifest'::regclass, 'minimizers_per_fragment');");
    const long long sequence_rowno = query_scalar_int(ctx,
        "SELECT bioseq.column_row_number('chunked_storage_missing_manifest'::regclass, ctid) FROM chunked_storage_missing_manifest LIMIT 1");

    // A reserved-band tuple (sub_key in the TEMP_RUN reserved range) must NOT be counted as a data fragment.
    const long long distinct_before = index_distinct_sub_keys(
        ctx, "minimizers_per_fragment", "chunked_storage_missing_manifest",
        sequence_rowno);
    exec_sqlf(ctx,
        "SELECT bioseq_internal.write_index_chunk('chunked_storage_missing_manifest'::regclass, 'minimizers_per_fragment', "
        "{}, (4611686018427387904)::bigint, 0, '\\x00'::bytea);",
        sequence_rowno); // sub_key = 1<<62 (TEMP_RUN_BASE)
    REQUIRE(index_distinct_sub_keys(
        ctx, "minimizers_per_fragment", "chunked_storage_missing_manifest",
        sequence_rowno) == distinct_before);

    // Deleting the completeness manifest makes the row read as "absent" -> recompute (still correct, non-empty).
    exec_sqlf(ctx,
        "SELECT bioseq_internal.delete_index_manifest('chunked_storage_missing_manifest'::regclass, 'minimizers_per_fragment', {});",
        sequence_rowno);
    guc_scope sequence_read_on(
        ctx, "bioseq.index.sequence_read.enabled", "on");
    REQUIRE(mmsearch_set(ctx, "chunked_storage_missing_manifest").size() == 1);   // recompute fallback still finds the match
}
#endif
