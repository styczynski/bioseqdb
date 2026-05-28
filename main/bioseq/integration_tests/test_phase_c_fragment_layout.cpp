// DOC0264 Phase C — verify the per-fragment row layout via sub-table size.
// USING bioseq tables emit zero rows for SELECT-without-mmsearch (the
// columnar custom scan only fires on mmsearch). Sub-tables are also
// USING bioseq, so `SELECT COUNT(*) FROM <sub_table>` returns 0 by
// design. We verify via `pg_relation_size(sub_table_relid)` — a larger
// number of fragment rows produces a larger relation size.

#include "common_tests_integration.h"
import integration_tests.common;

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view kTable = "phase_c_fragment_layout_t";

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

TEST_CASE("DOC0264 — multi-fragment vs single-fragment relation size scales monotonically",
          "[integration][phase-c][fragment-layout]") {
    auto ctx_small = fresh_context({kTable});
    // 1 sequence × 1024 bp = 1 fragment row.
    exec_sqlf(ctx_small,
              "SELECT bioseq.create_sample_table('{}', 1, 1024, 'dna4', 42);",
              kTable);
    const long long size_small = sub_table_relation_size(ctx_small, kTable);
    REQUIRE(size_small > 0);

    auto ctx_large = fresh_context({kTable});
    // 1 sequence × 100_000 bp = 4 fragments (3 full + 1 partial).
    exec_sqlf(ctx_large,
              "SELECT bioseq.create_sample_table('{}', 1, 100000, 'dna4', 7);",
              kTable);
    const long long size_large = sub_table_relation_size(ctx_large, kTable);
    REQUIRE(size_large > 0);
    REQUIRE(size_large > size_small);
}

TEST_CASE("DOC0264 — multiple sequences in the same bucket land in one shard",
          "[integration][phase-c][fragment-layout]") {
    auto ctx = fresh_context({kTable});
    // 4 sequences × 65000 bp each = 4 × 2 fragments. Same bucket, same shard.
    exec_sqlf(ctx,
              "SELECT bioseq.create_sample_table('{}', 4, 65000, 'dna4', 42);",
              kTable);
    // One catalog row (one shard).
    REQUIRE(query_scalar_int(ctx,
                fmt::format("SELECT COUNT(*)::bigint FROM bioseq_internal.seq_sub_table s "
                            "  WHERE s.main_relfilenode = (SELECT relfilenode FROM pg_class "
                            "                              WHERE relname = '{}');",
                            kTable)) == 1);
    REQUIRE(sub_table_relation_size(ctx, kTable) > 0);
}

TEST_CASE("DOC0264 — non-aligned final-fragment sequences (off-by-one boundary)",
          "[integration][phase-c][fragment-layout]") {
    constexpr long long kFragBp = 32768;
    auto ctx = fresh_context({kTable});
    for (long long bp : {kFragBp - 1, kFragBp, kFragBp + 1,
                          static_cast<long long>(123456ULL)}) {
        exec_sqlf(ctx, "DROP TABLE IF EXISTS {} CASCADE;", kTable);
        exec_sqlf(ctx,
                  "SELECT bioseq.create_sample_table('{}', 1, {}, 'dna4', 42);",
                  kTable, bp);
        // Each non-empty sequence must produce a non-zero-byte sub-table.
        REQUIRE(sub_table_relation_size(ctx, kTable) > 0);
    }
}
