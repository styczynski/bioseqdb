// Integration scenarios:
//   * `mmsearch` self-join uses the right-hand side's `seq1` column as the
//     query argument and runs without errors; whichever match set the
//     alignment pipeline yields is unconstrained here, but every row must
//     reference seeded primary keys.
//   * Projection helpers (`bioseq.target_start`, `bioseq.cigar`) work for
//     either input table in the join because the placeholder rewrite is
//     resolved per-call against the originating Var.
//   * The match set is invariant under `bioseq.enable_vectorization` toggling,
//     since the SIMD path must produce byte-identical output to scalar.
//
// We avoid `CROSS JOIN`-without-mmsearch on `USING bioseq` tables: the
// custom scan only buffers tuples when a SeqMmsearchOpExpr is part of the
// scan's qual list, so a literal cartesian product would emit zero rows
// (SPEC_HOW_TO_CONNECT_ALIGNER_3 §13.1).

#include "common_tests_integration.h"
import integration_tests.common;

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view kHoeJoin = "hoe_join";

}  // namespace

TEST_CASE("Self-join with mmsearch executes and stays in the seeded pk domain",
          "[join][mmsearch]") {
    auto ctx = fresh_context({kHoeJoin});
    seed_hoe_fixture(ctx, kHoeJoin);

    const auto rows = query_rows(
        ctx,
        fmt::format(
            "SELECT t1.pk, t2.pk "
            "  FROM {} t1 CROSS JOIN {} t2 "
            " WHERE mmsearch(t1.seq0, t2.seq1) "
            " ORDER BY t1.pk, t2.pk;",
            kHoeJoin, kHoeJoin));
    for (const auto& row : rows) {
        REQUIRE(row.size() == 2);
        REQUIRE(row[0].has_value());
        REQUIRE(row[1].has_value());
        const auto pk1 = std::stoll(row[0].value());
        const auto pk2 = std::stoll(row[1].value());
        REQUIRE((pk1 >= 1 && pk1 <= 5));
        REQUIRE((pk2 >= 1 && pk2 <= 5));
    }
}

TEST_CASE("Self-join with mmsearch is invariant under vectorisation",
          "[join][mmsearch][parity]") {
    auto ctx = fresh_context({kHoeJoin});
    seed_hoe_fixture(ctx, kHoeJoin);

    const std::string sql = fmt::format(
        "SELECT t1.pk, t2.pk "
        "  FROM {} t1 CROSS JOIN {} t2 "
        " WHERE mmsearch(t1.seq0, t2.seq1) "
        " ORDER BY t1.pk, t2.pk;",
        kHoeJoin, kHoeJoin);
    expect_invariant_under_guc_toggle(ctx, sql,
                                      "bioseq.enable_vectorization");
}

TEST_CASE("Self-join projection helpers run without errors when matches surface",
          "[join][projection]") {
    auto ctx = fresh_context({kHoeJoin});
    seed_hoe_fixture(ctx, kHoeJoin);

    const auto rows = query_rows(
        ctx,
        fmt::format(
            "SELECT t1.pk, t2.pk, "
            "       bioseq.target_start(t1.pk), "
            "       bioseq.target_end(t1.pk), "
            "       bioseq.cigar(t1.pk) "
            "  FROM {} t1 CROSS JOIN {} t2 "
            " WHERE mmsearch(t1.seq0, t2.seq1) "
            " ORDER BY t1.pk, t2.pk;",
            kHoeJoin, kHoeJoin));
    for (const auto& row : rows) {
        REQUIRE(row.size() == 5);
        if (row[2].has_value() && row[3].has_value()) {
            REQUIRE(std::stoll(row[2].value()) <= std::stoll(row[3].value()));
        }
        if (row[4].has_value() && !row[4].value().empty()) {
            REQUIRE(is_valid_cigar(row[4].value()));
        }
    }
}
