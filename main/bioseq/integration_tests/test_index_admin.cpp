// Integration scenarios for the metadata-admin SQL surface
// (bioseq.list_metadata + bioseq.update_index):
//   * list_metadata enumerates the two default kinds (minimizers,
//     symbol_counts) for a freshly-seeded USING bioseq table, with the
//     expected (mode, insert_support) shape.
//   * size and age columns are populated for kinds that have rows.
//   * update_index('minimizers') backfills index_sidecar after a bulk load
//     done with bioseq.update_index_on_insert = off.
//   * update_index returns the count of entries written.
//   * update_index is idempotent: running it twice produces the same row
//     count and the same blob bytes.
//   * update_index('NOT_A_KIND') raises (caught via sql_fails — the
//     server-side message is "unknown metadata kind").
//   * update_index updates bioseq_internal.metadata_age_log.
//   * list_metadata returns no rows for a relation whose sidecars have
//     been emptied.

#include "common_tests_integration.h"
import integration_tests.common;

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view kTable = "test_index_admin_t";

}  // namespace

TEST_CASE("list_metadata enumerates default kinds for a seeded table",
          "[index_admin][list]") {
    auto ctx = fresh_context({kTable});
    seed_small_columnar(ctx, kTable);

    const auto rows = query_rows(
        ctx,
        fmt::format("SELECT name, mode, insert_support "
                    "  FROM bioseq.list_metadata() "
                    " WHERE table_name = '{}'::regclass "
                    " ORDER BY name;",
                    kTable));
    REQUIRE(rows.size() == 2);
    REQUIRE(rows[0].size() == 3);
    REQUIRE(rows[0][0].value() == "minimizers");
    REQUIRE(rows[0][1].value() == "PER_ROW");
    REQUIRE(rows[0][2].value() == "t");
    REQUIRE(rows[1].size() == 3);
    REQUIRE(rows[1][0].value() == "symbol_counts");
    REQUIRE(rows[1][1].value() == "PER_ROW");
    REQUIRE(rows[1][2].value() == "t");
}

TEST_CASE("list_metadata size and age columns are populated for seeded kinds",
          "[index_admin][list]") {
    auto ctx = fresh_context({kTable});
    seed_small_columnar(ctx, kTable);

    // The age-log throttle in sidecar_after_insert means the first INSERT
    // in a 60-second window writes the row eagerly. seed_small_columnar
    // performs three INSERTs through that path, so by the time we read
    // back, both kinds have an age-log row.
    const auto rows = query_rows(
        ctx,
        fmt::format("SELECT name, "
                    "       size IS NOT NULL AS has_size, "
                    "       age IS NOT NULL AS has_age "
                    "  FROM bioseq.list_metadata() "
                    " WHERE table_name = '{}'::regclass "
                    " ORDER BY name;",
                    kTable));
    REQUIRE(rows.size() == 2);
    for (const auto& row : rows) {
        REQUIRE(row.size() == 3);
        REQUIRE(row[1].value() == "t");   // size populated
        REQUIRE(row[2].value() == "t");   // age populated
    }
}

TEST_CASE("update_index('minimizers') backfills after GUC=off bulk load",
          "[index_admin][update][backfill]") {
    auto ctx = fresh_context({kTable});

    // Bulk load with the GUC off — neither sidecar populates incrementally.
    // We can't reuse seed_small_columnar here because that helper sets
    // bioseq.update_index_on_insert = on internally, which would defeat
    // the scope. Build the same shape (3 dna4/32 rows) by hand.
    exec_sqlf(ctx, "SET bioseq.update_index_on_insert = off;");
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string(kTable),
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 1,
            .using_bioseq = true,
        },
        {
            seq_row{.pk = 1, .sequences = {"ACGTACGTACGTACGT"}},
            seq_row{.pk = 2, .sequences = {"TTTTAAAAGGGGCCCC"}},
            seq_row{.pk = 3, .sequences = {"AAAAAAAAAAAAAAAA"}},
        });
    exec_sqlf(ctx, "RESET bioseq.update_index_on_insert;");

    REQUIRE(count_sidecar(ctx, "index_sidecar", kTable) == 0);
    REQUIRE(count_sidecar(ctx, "meta_sidecar",  kTable) == 0);

    const auto written = query_scalar_int(
        ctx,
        fmt::format("SELECT bioseq.update_index('{}'::regclass, 'minimizers');",
                    kTable));
    REQUIRE(written == 3);
    REQUIRE(count_sidecar(ctx, "index_sidecar", kTable) == 3);
    // Updating one kind must not touch the other kind's sidecar.
    REQUIRE(count_sidecar(ctx, "meta_sidecar", kTable) == 0);

    // Symmetric backfill for symbol_counts.
    const auto written_meta = query_scalar_int(
        ctx,
        fmt::format("SELECT bioseq.update_index('{}'::regclass, 'symbol_counts');",
                    kTable));
    REQUIRE(written_meta == 3);
    REQUIRE(count_sidecar(ctx, "meta_sidecar", kTable) == 3);
}

TEST_CASE("update_index is idempotent: row count and blob bytes stable",
          "[index_admin][update][idempotent]") {
    auto ctx = fresh_context({kTable});
    seed_small_columnar(ctx, kTable);

    const auto first = query_scalar_int(
        ctx,
        fmt::format("SELECT bioseq.update_index('{}'::regclass, 'minimizers');",
                    kTable));
    const auto second = query_scalar_int(
        ctx,
        fmt::format("SELECT bioseq.update_index('{}'::regclass, 'minimizers');",
                    kTable));
    REQUIRE(first == second);
    REQUIRE(first == 3);
    REQUIRE(count_sidecar(ctx, "index_sidecar", kTable) == first);

    // Blob bytes are bit-identical across the two recomputes. The
    // serialiser walks std::unordered_map<uint64_t, ...> in bucket
    // order, which is implementation-defined but stable for two
    // identical insertion sequences against the same libstdc++ build —
    // exactly the situation here where collect_minimizers is replayed
    // on the same input. If a future libstdc++ adds hash randomisation
    // for uint64_t keys, this assertion will need to compare a sorted
    // (key, positions) projection instead of the raw blob.
    const auto blob_hashes_a = query_rows(
        ctx,
        fmt::format("SELECT md5(target_index) "
                    "  FROM bioseq_internal.index_sidecar "
                    " WHERE user_relid = '{}'::regclass "
                    " ORDER BY sequence_rowno;",
                    kTable));
    (void)query_scalar_int(
        ctx,
        fmt::format("SELECT bioseq.update_index('{}'::regclass, 'minimizers');",
                    kTable));
    const auto blob_hashes_b = query_rows(
        ctx,
        fmt::format("SELECT md5(target_index) "
                    "  FROM bioseq_internal.index_sidecar "
                    " WHERE user_relid = '{}'::regclass "
                    " ORDER BY sequence_rowno;",
                    kTable));
    REQUIRE(blob_hashes_a == blob_hashes_b);
}

TEST_CASE("update_index rejects an unknown metadata kind",
          "[index_admin][update][error]") {
    auto ctx = fresh_context({kTable});
    seed_small_columnar(ctx, kTable);

    REQUIRE(sql_fails(
        ctx,
        fmt::format("SELECT bioseq.update_index('{}'::regclass, 'NOT_A_KIND');",
                    kTable)));
    // No backfill side effect from the failed call.
    REQUIRE(count_sidecar(ctx, "index_sidecar", kTable) == 3);
}

TEST_CASE("update_index updates the metadata_age_log row for the kind",
          "[index_admin][update][age_log]") {
    auto ctx = fresh_context({kTable});
    seed_small_columnar(ctx, kTable);

    // Wipe the age log so we can prove update_index writes a fresh row.
    exec_sql(ctx, "DELETE FROM bioseq_internal.metadata_age_log;");
    REQUIRE(query_scalar_int(
                ctx,
                "SELECT COUNT(*)::bigint "
                "  FROM bioseq_internal.metadata_age_log;") == 0);

    (void)query_scalar_int(
        ctx,
        fmt::format("SELECT bioseq.update_index('{}'::regclass, 'minimizers');",
                    kTable));

    REQUIRE(query_scalar_int(
                ctx,
                fmt::format("SELECT COUNT(*)::bigint "
                            "  FROM bioseq_internal.metadata_age_log "
                            " WHERE user_relid = '{}'::regclass "
                            "   AND kind = 'minimizers';",
                            kTable)) == 1);
}

TEST_CASE("list_metadata returns no rows for a relation with no sidecar entries",
          "[index_admin][list][empty]") {
    auto ctx = fresh_context({kTable});
    seed_small_columnar(ctx, kTable);
    exec_sqlf(ctx,
              "DELETE FROM bioseq_internal.index_sidecar "
              " WHERE user_relid = '{}'::regclass;",
              kTable);
    exec_sqlf(ctx,
              "DELETE FROM bioseq_internal.meta_sidecar "
              " WHERE user_relid = '{}'::regclass;",
              kTable);

    const auto rows = query_rows(
        ctx,
        fmt::format("SELECT name FROM bioseq.list_metadata() "
                    " WHERE table_name = '{}'::regclass;",
                    kTable));
    REQUIRE(rows.empty());
}

TEST_CASE("update_index accepts text and explicit regclass for the table arg",
          "[index_admin][update][regclass]") {
    auto ctx = fresh_context({kTable});
    seed_small_columnar(ctx, kTable);

    // Quoted text form — PG implicitly casts text → regclass via
    // regclassin. This is the form most callers will use.
    const auto written_text = query_scalar_int(
        ctx,
        fmt::format("SELECT bioseq.update_index('{}', 'minimizers');", kTable));
    REQUIRE(written_text == 3);

    // Explicit ::regclass cast form.
    const auto written_cast = query_scalar_int(
        ctx,
        fmt::format("SELECT bioseq.update_index('{}'::regclass, 'minimizers');",
                    kTable));
    REQUIRE(written_cast == 3);
}

TEST_CASE("update_index rejects non-bioseq-AM tables",
          "[index_admin][update][error][heap]") {
    constexpr std::string_view kHeapTable = "test_index_admin_heap_t";
    auto ctx = fresh_context({kHeapTable});

    // Plain heap-AM table with a bioseq.seq column. The recompute path
    // would not be reachable from the bioseq columnar custom scan, so
    // update_index must refuse to write a sidecar that nothing reads.
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
        fmt::format("SELECT bioseq.update_index('{}'::regclass, 'minimizers');",
                    kHeapTable)));
}
