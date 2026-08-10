// The chunked per-table row-count index is
// compiled only under -DBIOSEQ_TEST_INDEXES. Gives table_index_storage::for_each_sequence +
// write_to live coverage: update_index streams every row and persists the row count as
// one int64 chunk. It also exercises the table-wide-never-UPDATE_ON_INSERT guard.
#include <tests/support/postgres.hpp>
#include <string>
import bioseq.tests.integration.support;

using namespace bioseq_int_tests;

#ifdef BIOSEQ_TEST_INDEXES
TEST_CASE("per-table chunked indexing streams every row and persists the count",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({"chunked_table_index"});
    REQUIRE(query_scalar(ctx,
        "SELECT to_regprocedure('bioseq.update_index(regclass,text)') IS NOT NULL") == "t");

    seed_small_bioseq_column(ctx, "chunked_table_index");   // 3 rows
    exec_sql(ctx, "SELECT bioseq.update_index('chunked_table_index'::regclass, 'test_table_rowcount');");

    // The per-table shard exists and holds the row count (one int64 LE chunk; count<256 => byte 0).
    auto names = query_rows(ctx,
        "SELECT c.relname FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace "
        "WHERE n.nspname='bioseq_internal' AND c.relkind='r' "
        "AND c.relname LIKE 'seqidxtab\\_ttr\\_%' AND c.relname NOT LIKE '%pkey' LIMIT 1");
    REQUIRE(names.size() == 1);
    const std::string shard = *names[0][0];
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT get_byte(blob,0) FROM bioseq_internal.{} ORDER BY chunk_no LIMIT 1", shard)) == 3);

    // The table-wide-never runtime guard: UPDATE_ON_INSERT on a CHUNKED_PER_TABLE index errors.
    REQUIRE(sql_fails(ctx,
        "SELECT bioseq.set_index_policy('chunked_table_index'::regclass, 'test_table_rowcount', 'UPDATE_ON_INSERT')"));
}
#endif
