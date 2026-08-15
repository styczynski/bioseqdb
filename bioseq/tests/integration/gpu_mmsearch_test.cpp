/**
 * @file gpu_mmsearch_test.cpp
 * @brief Verifies mmsearch through fragment hydration and in-memory dispatch.
 *
 * Inserts write fragment rows and a parent stub. The scan stitches those rows
 * into a bounded varlena before dispatch.
 */

#include <tests/support/postgres.hpp>
import bioseq.tests.integration.support;

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view k_alignment_table = "fragment_hydration_alignment";

} // namespace

TEST_CASE("mmsearch on the canonical alignment dataset returns the expected matches",
          "[component:bioseq][layer:integration][feature:alignment][resource:postgres]") {
    auto ctx = fresh_context({k_alignment_table});
    seed_hoe_fixture(ctx, k_alignment_table);
    // The canonical probe matches seq0 of rows 1 and 2, which contain the
    // exact substring "CTAGTCACTGTAGCCTGATCGTAGCTAGT") plus other rows
    // where the chainer finds a fuzzy hit. Verify at least one match: the
    // dispatch_buffer -> fetch_payload -> with_lazy_in_memory pipeline
    // must produce a non-empty result set against per-fragment storage.
    const auto hits =
        query_rows(ctx,
                   fmt::format("SELECT pk FROM {} "
                               "WHERE mmsearch(seq0, "
                               "  'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4',32)) "
                               "ORDER BY pk;",
                               k_alignment_table));
    REQUIRE(!hits.empty());
    // At least pk = 1 and pk = 2 (exact substring matches).
    bool has_first_exact_match = false;
    bool has_second_exact_match = false;
    for (const auto& row : hits) {
        const long long pk = std::stoll(*row[0]);
        if (pk == 1) {
            has_first_exact_match = true;
        }
        if (pk == 2) {
            has_second_exact_match = true;
        }
    }
    REQUIRE(has_first_exact_match);
    REQUIRE(has_second_exact_match);
}

TEST_CASE("mmsearch self-join reads fragment storage through the in-memory pipeline",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    auto ctx = fresh_context({k_alignment_table});
    seed_hoe_fixture(ctx, k_alignment_table);
    // Self-join via mmsearch exercises the dispatch buffer's bulk-fetch +
    // with_lazy_in_memory pipeline against per-fragment storage.
    const auto rows = query_rows(ctx,
                                 fmt::format("SELECT t1.pk, t2.pk FROM {0} t1 JOIN {0} t2 "
                                             "  ON mmsearch(t1.seq0, t2.seq0) ORDER BY 1, 2;",
                                             k_alignment_table));
    // Self-join must yield >0 matches.
    REQUIRE(!rows.empty());
}
