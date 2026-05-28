// DOC0264 Phase C — mmsearch end-to-end through the fragment-stitching
// hydration path. With Phase C, INSERT INTO bioseq tables writes per-
// fragment sub-table rows AND a stub in the parent. mmsearch dispatch
// hydrates the stub via fetch_payload (which now stream-stitches the
// per-fragment rows back into a contiguous varlena, bounded by
// MaxAllocSize) and dispatches through `with_lazy_in_memory`.

#include "common_tests_integration.h"
import integration_tests.common;

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view kHoe = "phase_c_mmsearch_hoe";

}  // namespace

TEST_CASE("DOC0264 — mmsearch on hoe fixture returns the canonical probe matches",
          "[integration][phase-c][mmsearch][hoe-fixture]") {
    auto ctx = fresh_context({kHoe});
    seed_hoe_fixture(ctx, kHoe);
    // The canonical hoe probe matches seq0 of rows 1, 2 (which contain the
    // exact substring "CTAGTCACTGTAGCCTGATCGTAGCTAGT") plus other rows
    // where the chainer finds a fuzzy hit. Verify at least one match — the
    // important Phase C bar is that the dispatch_buffer → fetch_payload
    // (fragment-stitching) → with_lazy_in_memory pipeline produces a
    // non-empty result set against per-fragment storage.
    auto hits = query_rows(ctx,
        fmt::format("SELECT pk FROM {} "
                    "WHERE mmsearch(seq0, "
                    "  'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4',32)) "
                    "ORDER BY pk;", kHoe));
    REQUIRE(!hits.empty());
    // At least pk = 1 and pk = 2 (exact substring matches).
    bool has_1 = false;
    bool has_2 = false;
    for (const auto& row : hits) {
        const long long pk = std::stoll(*row[0]);
        if (pk == 1) has_1 = true;
        if (pk == 2) has_2 = true;
    }
    REQUIRE(has_1);
    REQUIRE(has_2);
}

TEST_CASE("DOC0264 — mmsearch self-join on hoe fixture (Phase C fragment storage)",
          "[integration][phase-c][mmsearch][self-join]") {
    auto ctx = fresh_context({kHoe});
    seed_hoe_fixture(ctx, kHoe);
    // Self-join via mmsearch exercises the dispatch buffer's bulk-fetch +
    // with_lazy_in_memory pipeline against Phase C per-fragment storage.
    auto rows = query_rows(ctx,
        fmt::format("SELECT t1.pk, t2.pk FROM {0} t1 JOIN {0} t2 "
                    "  ON mmsearch(t1.seq0, t2.seq0) ORDER BY 1, 2;",
                    kHoe));
    // Self-join must yield >0 matches.
    REQUIRE(!rows.empty());
}
