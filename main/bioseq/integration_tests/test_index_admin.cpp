// Integration scenarios for the DOC0279 metadata-admin SQL surface
// (bioseq.list_metadata + bioseq.update_index + bioseq.set_index_policy):
//   * list_metadata enumerates the three PRODUCTION index kinds
//     (minimizers_per_fragment, symbol_counts, minimizers_per_sequence) for a
//     freshly-created USING bioseq table, NOT gated on data: every index
//     appears with size='0 bytes' and a NULL age, carrying its registered
//     (index_type, update_policy).
//   * update_index('minimizers_per_fragment') (re)builds the per-fragment
//     chunk index for every bioseq.seq column of every row and returns the
//     count of sequences (re)indexed.
//   * update_index rejects an unknown index name, and rejects non-bioseq-AM
//     (heap) tables.
//   * update_index accepts the table argument as quoted text or as an
//     explicit ::regclass cast.
//   * set_index_policy toggles a kind's update_policy (visible through
//     list_metadata), including opt-in UPDATE_ON_INSERT for the heavier
//     minimizers_per_sequence kind.
//   * update_index is idempotent at the content level (stable chunk row
//     count across repeated calls) and returns 0 on an empty table while
//     list_metadata still lists the kinds.
//
// The legacy index_sidecar / meta_sidecar tables and count_sidecar() helper
// are DELETED; row-count assertions go through the new
// bioseq.index_table_row_count() libpq wrapper (index_table_row_count()).

#include "common_tests_integration.h"
import bioseq.integration_tests.common;

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view kTable = "test_index_admin_t";

}  // namespace

TEST_CASE("list_metadata reports all production index kinds for a fresh bioseq table",
          "[index_admin][list]") {
    auto ctx = fresh_context({kTable});
    // No inserts: list_metadata is NOT gated on data, so every registered
    // index must already appear for the fresh table.
    exec_sqlf(ctx,
              "CREATE TABLE {} (pk int, seq0 bioseq.seq('dna4', 32)) "
              "USING bioseq;",
              kTable);

    // Filter out the test-only CHUNKED_PER_TABLE 'test_table_rowcount' index
    // (only present under -DBIOSEQ_TEST_INDEXES) so the assertion pins exactly
    // the three production kinds regardless of how the dylib was built.
    const auto rows = query_rows(
        ctx,
        fmt::format("SELECT name, index_type, update_policy, size, (age IS NULL) "
                    "  FROM bioseq.list_metadata() "
                    " WHERE table_name = '{}'::regclass "
                    "   AND name <> 'test_table_rowcount' "
                    " ORDER BY name;",
                    kTable));

    REQUIRE(rows.size() == 3);

    // Sorted by name: minimizers_per_fragment, minimizers_per_sequence,
    // symbol_counts.
    REQUIRE(rows[0].size() == 5);
    REQUIRE(rows[0][0].value() == "minimizers_per_fragment");
    REQUIRE(rows[0][1].value() == "CHUNKED_PER_FRAGMENT");
    REQUIRE(rows[0][2].value() == "UPDATE_ON_INSERT");
    REQUIRE(rows[0][3].value() == "0 bytes");
    REQUIRE(rows[0][4].value() == "t");  // age IS NULL

    REQUIRE(rows[1].size() == 5);
    REQUIRE(rows[1][0].value() == "minimizers_per_sequence");
    REQUIRE(rows[1][1].value() == "CHUNKED_PER_FRAGMENT");
    REQUIRE(rows[1][2].value() == "UPDATE_MANUALLY");
    REQUIRE(rows[1][3].value() == "0 bytes");
    REQUIRE(rows[1][4].value() == "t");

    REQUIRE(rows[2].size() == 5);
    REQUIRE(rows[2][0].value() == "symbol_counts");
    REQUIRE(rows[2][1].value() == "INLINE_PER_SEQUENCE");
    REQUIRE(rows[2][2].value() == "UPDATE_ON_INSERT");
    REQUIRE(rows[2][3].value() == "0 bytes");
    REQUIRE(rows[2][4].value() == "t");
}

TEST_CASE("update_index('minimizers_per_fragment') builds the per-fragment index",
          "[index_admin][update][backfill]") {
    auto ctx = fresh_context({kTable});
    // seed_small_bioseq_column sets bioseq.update_index_on_insert = on, so the two
    // default UPDATE_ON_INSERT indexes (minimizers_per_fragment, symbol_counts)
    // are populated incrementally at INSERT time.
    seed_small_bioseq_column(ctx, kTable);

    REQUIRE(index_table_row_count(ctx, "minimizers_per_fragment", kTable) > 0);
    REQUIRE(index_table_row_count(ctx, "symbol_counts", kTable) > 0);

    // Explicit rebuild of the per-fragment index returns the count of
    // sequences (re)indexed — three seeded rows.
    REQUIRE(query_scalar_int(
                ctx,
                fmt::format("SELECT bioseq.update_index('{}'::regclass, "
                            "'minimizers_per_fragment');",
                            kTable)) == 3);
}

TEST_CASE("update_index rejects an unknown index name",
          "[index_admin][update][error]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);

    REQUIRE(sql_fails(
        ctx,
        fmt::format("SELECT bioseq.update_index('{}'::regclass, 'NOT_A_KIND');",
                    kTable)));
}

TEST_CASE("update_index accepts text and explicit regclass for the table arg",
          "[index_admin][update][regclass]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);

    // Quoted text form — PG implicitly casts text -> regclass via regclassin.
    REQUIRE(query_scalar_int(
                ctx,
                fmt::format("SELECT bioseq.update_index('{}', "
                            "'minimizers_per_fragment');",
                            kTable)) == 3);

    // Explicit ::regclass cast form.
    REQUIRE(query_scalar_int(
                ctx,
                fmt::format("SELECT bioseq.update_index('{}'::regclass, "
                            "'minimizers_per_fragment');",
                            kTable)) == 3);
}

TEST_CASE("update_index rejects non-bioseq-AM tables",
          "[index_admin][update][error][heap]") {
    constexpr std::string_view kHeapTable = "test_index_admin_heap_t";
    auto ctx = fresh_context({kHeapTable});

    // Plain heap-AM table with a bioseq.seq column. The chunked-storage
    // reindex path is only reachable from the bioseq columnar AM, so
    // update_index must refuse a heap relation.
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string(kHeapTable),
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 1,
            .using_bioseq = false,
        },
        {seq_row{.pk = 1, .sequences = {"ACGTACGTACGTACGT"}}});

    REQUIRE(sql_fails(
        ctx,
        fmt::format("SELECT bioseq.update_index('{}'::regclass, "
                    "'minimizers_per_fragment');",
                    kHeapTable)));
}

TEST_CASE("set_index_policy toggles update_policy and supports per-sequence on-insert",
          "[index_admin][policy]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);

    // Helper: read the current update_policy of one index from list_metadata.
    auto policy_of = [&](std::string_view index_name) {
        return query_scalar(
            ctx,
            fmt::format("SELECT update_policy FROM bioseq.list_metadata() "
                        " WHERE table_name = '{}'::regclass "
                        "   AND name = '{}' LIMIT 1;",
                        kTable, index_name));
    };

    // symbol_counts defaults to UPDATE_ON_INSERT; flip it to UPDATE_MANUALLY.
    exec_sqlf(ctx,
              "SELECT bioseq.set_index_policy('{}'::regclass, 'symbol_counts', "
              "'UPDATE_MANUALLY');",
              kTable);
    REQUIRE(policy_of("symbol_counts") == "UPDATE_MANUALLY");

    // Flip it back to UPDATE_ON_INSERT.
    exec_sqlf(ctx,
              "SELECT bioseq.set_index_policy('{}'::regclass, 'symbol_counts', "
              "'UPDATE_ON_INSERT');",
              kTable);
    REQUIRE(policy_of("symbol_counts") == "UPDATE_ON_INSERT");

    // minimizers_per_sequence is opt-in UPDATE_ON_INSERT. This must be
    // accepted so recompute_async_persist and batch load can honor the table
    // policy instead of hard-wiring minimizers_per_fragment.
    exec_sqlf(ctx,
              "SELECT bioseq.set_index_policy('{}'::regclass, "
              "'minimizers_per_fragment', 'UPDATE_MANUALLY');",
              kTable);
    exec_sqlf(ctx,
              "SELECT bioseq.set_index_policy('{}'::regclass, "
              "'minimizers_per_sequence', 'UPDATE_ON_INSERT');",
              kTable);
    REQUIRE(policy_of("minimizers_per_fragment") == "UPDATE_MANUALLY");
    REQUIRE(policy_of("minimizers_per_sequence") == "UPDATE_ON_INSERT");
}

TEST_CASE("UPDATE_ON_INSERT can build minimizers_per_sequence without per-fragment fallback",
          "[index_admin][policy][insert_hook]") {
    auto ctx = fresh_context({"test_index_admin_mps"});
    exec_sql(ctx, "CREATE TABLE test_index_admin_mps "
                  "(header text, seq bioseq.seq('dna4', 64)) USING bioseq;");
    exec_sql(ctx, "SELECT bioseq.set_index_policy("
                  "'test_index_admin_mps'::regclass, "
                  "'minimizers_per_fragment', 'UPDATE_MANUALLY');");
    exec_sql(ctx, "SELECT bioseq.set_index_policy("
                  "'test_index_admin_mps'::regclass, "
                  "'minimizers_per_sequence', 'UPDATE_ON_INSERT');");
    exec_sql(ctx, "SET bioseq.update_index_on_insert = on;");
    exec_sql(ctx, "INSERT INTO test_index_admin_mps VALUES "
                  "('mps', repeat('ACGT', 2000)::bioseq.seq('dna4', 64));");

    REQUIRE(index_table_row_count(ctx, "minimizers_per_fragment",
                                  "test_index_admin_mps") == 0);
    REQUIRE(index_table_row_count(ctx, "minimizers_per_sequence",
                                  "test_index_admin_mps") > 0);
}

TEST_CASE("update_index('minimizers_per_fragment') is idempotent at the content level",
          "[index_admin][update][idempotent]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);

    // Run the rebuild twice; the per-fragment chunk row count must be stable
    // (the rebuild clears the index then repopulates from identical input).
    REQUIRE(query_scalar_int(
                ctx,
                fmt::format("SELECT bioseq.update_index('{}'::regclass, "
                            "'minimizers_per_fragment');",
                            kTable)) == 3);
    const auto after_first =
        index_table_row_count(ctx, "minimizers_per_fragment", kTable);

    REQUIRE(query_scalar_int(
                ctx,
                fmt::format("SELECT bioseq.update_index('{}'::regclass, "
                            "'minimizers_per_fragment');",
                            kTable)) == 3);
    const auto after_second =
        index_table_row_count(ctx, "minimizers_per_fragment", kTable);

    REQUIRE(after_first > 0);
    REQUIRE(after_first == after_second);
}

TEST_CASE("update_index on an empty bioseq table returns 0 and list_metadata "
          "still lists kinds",
          "[index_admin][update][empty]") {
    auto ctx = fresh_context({kTable});
    // Create an empty USING bioseq table (no rows).
    exec_sqlf(ctx,
              "CREATE TABLE {} (pk int, seq0 bioseq.seq('dna4', 32)) "
              "USING bioseq;",
              kTable);

    // No rows -> no sequences (re)indexed -> 0.
    REQUIRE(query_scalar_int(
                ctx,
                fmt::format("SELECT bioseq.update_index('{}'::regclass, "
                            "'minimizers_per_fragment');",
                            kTable)) == 0);

    // list_metadata still lists the three production kinds with size '0 bytes'.
    const auto rows = query_rows(
        ctx,
        fmt::format("SELECT name, size FROM bioseq.list_metadata() "
                    " WHERE table_name = '{}'::regclass "
                    "   AND name <> 'test_table_rowcount' "
                    " ORDER BY name;",
                    kTable));
    REQUIRE(rows.size() == 3);
    REQUIRE(rows[0][0].value() == "minimizers_per_fragment");
    REQUIRE(rows[1][0].value() == "minimizers_per_sequence");
    REQUIRE(rows[2][0].value() == "symbol_counts");
    for (const auto& row : rows) {
        REQUIRE(row.size() == 2);
        REQUIRE(row[1].value() == "0 bytes");
    }
}
