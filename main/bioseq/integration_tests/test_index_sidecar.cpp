// Integration scenarios:
//   * `bioseq.update_index_on_insert = on` populates `bioseq_internal.
//     index_sidecar` and `bioseq_internal.meta_sidecar` rows for every newly
//     inserted tuple of a `USING bioseq` table.
//   * Flipping the GUC off blocks BOTH sidecars on subsequent inserts (the
//     setup.sql comment about meta_sidecar continuing to populate is stale).
//   * Toggling the GUC back on restores per-insert sidecar population.
//   * `meta_sidecar.symbol_counts` is a 4-element array for dna4 with the
//     POC-hardcoded value `kSymbolCountPocFor_A` in the rank-0 slot.
//   * The serialized `target_index` blob carries the 0xB1000001 magic
//     header (little-endian: 01 00 00 B1).
//   * A `BEGIN; INSERT; ROLLBACK;` does not leave sidecar rows behind, even
//     though the sidecars are heap tables.

#include "common_tests_integration.h"
import integration_tests.common;

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view kTable = "test_sidecar_t";

}  // namespace

TEST_CASE("Sidecar tables are populated for every insert when the GUC is on",
          "[sidecar][on]") {
    auto ctx = fresh_context({kTable});
    seed_small_columnar(ctx, kTable);

    REQUIRE(count_sidecar(ctx, "index_sidecar", kTable) == 3);
    REQUIRE(count_sidecar(ctx, "meta_sidecar", kTable) == 3);
}

TEST_CASE("update_index_on_insert OFF blocks both sidecars on new inserts",
          "[sidecar][toggle]") {
    auto ctx = fresh_context({kTable});
    seed_small_columnar(ctx, kTable);

    const long long before_index = count_sidecar(ctx, "index_sidecar", kTable);
    const long long before_meta = count_sidecar(ctx, "meta_sidecar", kTable);

    {
        guc_scope off(ctx, "bioseq.update_index_on_insert", "off");
        exec_sqlf(ctx,
                  "INSERT INTO {} VALUES (4, 'GGGGAAAATTTTCCCC');",
                  kTable);
    }

    REQUIRE(count_sidecar(ctx, "index_sidecar", kTable) == before_index);
    REQUIRE(count_sidecar(ctx, "meta_sidecar", kTable) == before_meta);
}

TEST_CASE("Flipping update_index_on_insert back ON resumes index inserts",
          "[sidecar][toggle]") {
    auto ctx = fresh_context({kTable});
    seed_small_columnar(ctx, kTable);

    {
        guc_scope off(ctx, "bioseq.update_index_on_insert", "off");
        exec_sqlf(ctx,
                  "INSERT INTO {} VALUES (4, 'GGGGAAAATTTTCCCC');",
                  kTable);
    }
    const long long index_after_off =
        count_sidecar(ctx, "index_sidecar", kTable);

    // Default GUC is on; INSERTs after the scope ends populate the index.
    exec_sqlf(ctx,
              "INSERT INTO {} VALUES (5, 'CCCCGGGGAAAATTTT');",
              kTable);

    REQUIRE(count_sidecar(ctx, "index_sidecar", kTable) ==
            index_after_off + 1);
}

TEST_CASE("Sidecar rows do not survive a ROLLBACK",
          "[sidecar][transactional]") {
    auto ctx = fresh_context({kTable});
    seed_small_columnar(ctx, kTable);

    const long long before_index = count_sidecar(ctx, "index_sidecar", kTable);
    const long long before_meta = count_sidecar(ctx, "meta_sidecar", kTable);

    exec_sql(ctx, "BEGIN;");
    exec_sqlf(ctx,
              "INSERT INTO {} VALUES (42, 'GGGGAAAATTTTCCCC');",
              kTable);
    exec_sql(ctx, "ROLLBACK;");

    // Both the user table and the sidecar inserts must be undone.
    REQUIRE(query_scalar_int(
                ctx, fmt::format("SELECT COUNT(*)::bigint FROM {} "
                                 "WHERE pk = 42;",
                                 kTable)) == 0);
    REQUIRE(count_sidecar(ctx, "index_sidecar", kTable) == before_index);
    REQUIRE(count_sidecar(ctx, "meta_sidecar", kTable) == before_meta);
}

TEST_CASE("meta_sidecar carries a 4-element symbol_counts array with the POC value at rank 0",
          "[sidecar][meta][symbol_counts]") {
    auto ctx = fresh_context({kTable});
    seed_small_columnar(ctx, kTable);

    const auto rows = query_rows(
        ctx,
        fmt::format("SELECT sequence_rowno, alphabet_code, "
                    "       array_length(symbol_counts, 1) AS arr_len, "
                    "       symbol_counts[1] AS rank0 "
                    "  FROM bioseq_internal.meta_sidecar "
                    " WHERE user_relid = '{}'::regclass "
                    " ORDER BY sequence_rowno;",
                    kTable));
    REQUIRE(rows.size() == 3);
    for (const auto& row : rows) {
        REQUIRE(row.size() == 4);
        REQUIRE(row[1].value() == "0");        // alphabet_code 0 == dna4
        REQUIRE(row[2].value() == "4");        // 4 ranks
        REQUIRE(std::stoll(row[3].value()) == kSymbolCountPocFor_A);
    }
}

TEST_CASE("index_sidecar blob carries the 0xB1000001 magic header",
          "[sidecar][index][magic]") {
    auto ctx = fresh_context({kTable});
    seed_small_columnar(ctx, kTable);

    // Magic bytes are stored little-endian: 01 00 00 B1.
    const auto rows = query_rows(
        ctx,
        fmt::format(
            "SELECT encode(substring(target_index, 1, 4), 'hex') AS magic_hex, "
            "       length(target_index) AS blob_bytes "
            "  FROM bioseq_internal.index_sidecar "
            " WHERE user_relid = '{}'::regclass "
            " ORDER BY sequence_rowno;",
            kTable));
    REQUIRE(rows.size() == 3);
    for (const auto& row : rows) {
        REQUIRE(row.size() == 2);
        REQUIRE(row[0].value() == "010000b1");
        REQUIRE(std::stoll(row[1].value()) > 4);
    }
}
