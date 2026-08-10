// Verify the per-fragment row layout through sub-table size.
// USING bioseq tables emit zero rows for SELECT-without-mmsearch (the
// columnar custom scan only fires on mmsearch). Sub-tables are also
// USING bioseq, so `SELECT COUNT(*) FROM <sub_table>` returns 0 by
// design. We verify via `pg_relation_size(sub_table_relid)` — a larger
// number of fragment rows produces a larger relation size.

#include <tests/support/postgres.hpp>
import bioseq.tests.integration.support;

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view k_table = "fragment_storage_layout";

[[nodiscard]] long long sub_table_relation_size(
    const connection_test_context& ctx, std::string_view parent_table) {
    return query_scalar_int(ctx,
        fmt::format("SELECT COALESCE(SUM(pg_relation_size(s.sub_table_relid)), 0)::bigint "
                    "  FROM bioseq_internal.seq_sub_table s "
                    " WHERE s.main_relfilenode = (SELECT relfilenode FROM pg_class "
                    "                              WHERE relname = '{}');",
                    parent_table));
}

}  // namespace

TEST_CASE("multi-fragment vs single-fragment relation size scales monotonically",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    auto ctx_small = fresh_context({k_table});
    // 1 sequence × 1024 bp = 1 fragment row.
    exec_sqlf(ctx_small,
              "SELECT bioseq.create_sample_table('{}', 1, 1024, 'dna4', 42);",
              k_table);
    const long long size_small = sub_table_relation_size(ctx_small, k_table);
    REQUIRE(size_small > 0);

    auto ctx_large = fresh_context({k_table});
    // 1 sequence × 100_000 bp = 4 fragments (3 full + 1 partial).
    exec_sqlf(ctx_large,
              "SELECT bioseq.create_sample_table('{}', 1, 100000, 'dna4', 7);",
              k_table);
    const long long size_large = sub_table_relation_size(ctx_large, k_table);
    REQUIRE(size_large > 0);
    REQUIRE(size_large > size_small);
}

TEST_CASE("multiple sequences in the same bucket land in one shard",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    // 4 sequences × 65000 bp each = 4 × 2 fragments. Same bucket, same shard.
    exec_sqlf(ctx,
              "SELECT bioseq.create_sample_table('{}', 4, 65000, 'dna4', 42);",
              k_table);
    // One catalog row (one shard).
    REQUIRE(query_scalar_int(ctx,
                fmt::format("SELECT COUNT(*)::bigint FROM bioseq_internal.seq_sub_table s "
                            "  WHERE s.main_relfilenode = (SELECT relfilenode FROM pg_class "
                            "                              WHERE relname = '{}');",
                            k_table)) == 1);
    REQUIRE(sub_table_relation_size(ctx, k_table) > 0);
}

TEST_CASE("non-aligned final-fragment sequences cover the off-by-one boundary",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    constexpr long long k_fragment_bp = 32768;
    auto ctx = fresh_context({k_table});
    for (long long bp : {k_fragment_bp - 1, k_fragment_bp, k_fragment_bp + 1,
                          static_cast<long long>(123456ULL)}) {
        exec_sqlf(ctx, "DROP TABLE IF EXISTS {} CASCADE;", k_table);
        exec_sqlf(ctx,
                  "SELECT bioseq.create_sample_table('{}', 1, {}, 'dna4', 42);",
                  k_table, bp);
        // Each non-empty sequence must produce a non-zero-byte sub-table.
        REQUIRE(sub_table_relation_size(ctx, k_table) > 0);
    }
}
