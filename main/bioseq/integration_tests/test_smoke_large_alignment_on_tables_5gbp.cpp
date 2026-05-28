// DOC0274 — smoke test for Phase C alignment correctness on per-fragment
// storage. Generates a (query, target) sample table via
// `bioseq.create_query_target_sample_table(...)`, then runs the canonical
// `SELECT ... WHERE mmsearch(target, query, ...)` once with the production
// scanner_pg path (`bioseq.test_force_inmemory_scanner = off`) and once
// with the Phase B in-memory baseline (`bioseq.test_force_inmemory_scanner
// = on`). Both must produce ideal matches whose `target_start` /
// `target_end` equal the ground-truth bounds `[truth_target_start,
// truth_target_end)` the generator recorded.
//
// `OUR_TEST_BASED_ON_XXL_HIFI` parameters mirror the bench v10 config
// `XXL_HIFI` cell minus the bench-only `concurrency` knob. The reference
// is `kReferenceBp` bp (large enough to fragment into many per-fragment
// sub-table rows + exercise the 28-bit sidecar-skip threshold) and the
// query fragments are `kFragmentBp` bp with HiFi-style ~0.2 % error.

#include "common_tests_integration.h"
import integration_tests.common;

using namespace bioseq_int_tests;

namespace {

// "OUR_TEST_BASED_ON_XXL_HIFI" parameters. The bench config calls for
// reference_length=5e9 bp which holds ~5 GB of letters in RAM during
// generation. Practical CI machines may not have that much headroom, so
// the smoke variant uses a smaller-but-still-fragmenting reference. The
// test will exercise the same per-fragment Phase C storage code path as
// the full 5 Gbp run.
constexpr long long kReferenceBp        = 100'000'000;   // 100 Mbp
constexpr long long kFragmentBp         = 5'000'000;     // 5 Mbp
constexpr long long kFragmentStddev     = 100'000;       // 100 kbp
constexpr int       kFragmentCount      = 3;
constexpr double    kSubstitutionRate   = 0.001;
constexpr double    kInsertionRate      = 0.0005;
constexpr double    kDeletionRate       = 0.0005;
constexpr double    kInsertionLenMean   = 1.0;
constexpr double    kDeletionLenMean    = 1.0;
constexpr double    kReverseRate        = 0.5;
constexpr long long kSeed               = 3219876;

constexpr std::string_view kTable = "phase_c_xxl_hifi_sample";

inline void seed_sample_table(const connection_test_context& ctx) {
    exec_sqlf(ctx,
        "SELECT bioseq.create_query_target_sample_table("
        "  '{}', {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {});",
        kTable,
        kFragmentCount,
        kFragmentBp,
        kFragmentStddev,
        kReferenceBp,
        kSubstitutionRate,
        kInsertionRate,
        kDeletionRate,
        kInsertionLenMean,
        kDeletionLenMean,
        kReverseRate,
        kSeed);
}

struct truth_row {
    long long id              = 0;
    long long truth_start     = 0;
    long long truth_end       = 0;
    bool      is_reverse      = false;
    long long query_length    = 0;
};

inline std::vector<truth_row> read_truth(const connection_test_context& ctx) {
    auto rows = query_rows(ctx,
        fmt::format("SELECT id, truth_target_start, truth_target_end, "
                    "is_reverse, original_query_length FROM {} ORDER BY id;",
                    kTable));
    std::vector<truth_row> out;
    out.reserve(rows.size());
    for (auto& r : rows) {
        truth_row tr;
        tr.id           = std::stoll(*r[0]);
        tr.truth_start  = std::stoll(*r[1]);
        tr.truth_end    = std::stoll(*r[2]);
        tr.is_reverse   = (*r[3] == "t");
        tr.query_length = std::stoll(*r[4]);
        out.push_back(tr);
    }
    return out;
}

// Result row from the mmsearch SELECT. The aligner returns a chained
// alignment whose `target_start`/`target_end` should equal the truth bounds
// when the error rate is low (~0.2%) and the chain is correctly placed.
struct mmsearch_row {
    long long id            = 0;
    long long target_start  = 0;
    long long target_end    = 0;
};

inline std::vector<mmsearch_row> run_mmsearch(
    const connection_test_context& ctx)
{
    // Per-row mmsearch: arg0 is `target` column, arg1 is `query` column —
    // both are Vars of the same relation, so the dispatch_buffer takes the
    // per-row branch (DOC0274). The aligner's reported target_start /
    // target_end should equal the truth bounds within tolerance.
    auto rows = query_rows(ctx,
        fmt::format("SELECT id, "
                    "       bioseq.target_start(id)::bigint, "
                    "       bioseq.target_end(id)::bigint "
                    "  FROM {} "
                    " WHERE mmsearch(target, query) "
                    " ORDER BY id;",
                    kTable));
    std::vector<mmsearch_row> out;
    out.reserve(rows.size());
    for (auto& r : rows) {
        if (!r[0].has_value() || !r[1].has_value() || !r[2].has_value()) {
            continue;
        }
        mmsearch_row m;
        m.id           = std::stoll(*r[0]);
        m.target_start = std::stoll(*r[1]);
        m.target_end   = std::stoll(*r[2]);
        out.push_back(m);
    }
    return out;
}

// Tolerance for the aligner's reported start/end vs the truth bounds. With
// HiFi-style 0.2 % error rate and ~5 Mbp queries, the chain endpoint is
// typically within ~10 kbp of truth (the trailing low-quality region of
// the alignment is trimmed by the band).
constexpr long long kPositionTolerance = 50'000;

inline void require_ideal_match(
    const truth_row& truth, const mmsearch_row& observed,
    std::string_view scanner_label)
{
    const long long start_delta =
        std::abs(observed.target_start - truth.truth_start);
    const long long end_delta =
        std::abs(observed.target_end - truth.truth_end);
    JustLog("[test] scanner={} id={} truth=[{},{}) observed=[{},{}) "
            "start_delta={} end_delta={} is_reverse={}",
            scanner_label, truth.id, truth.truth_start, truth.truth_end,
            observed.target_start, observed.target_end,
            start_delta, end_delta, truth.is_reverse);
    REQUIRE(start_delta <= kPositionTolerance);
    REQUIRE(end_delta   <= kPositionTolerance);
}

}  // namespace

TEST_CASE("DOC0274 — XXL_HIFI sample table: scanner_pg (test_force_inmemory_scanner=off) "
          "and Phase B baseline (=on) both produce ideal matches",
          "[integration][phase-c][xxl-hifi][scanner-pg]") {
    auto ctx = fresh_context({kTable});
    seed_sample_table(ctx);
    const auto truth = read_truth(ctx);
    REQUIRE(truth.size() == static_cast<std::size_t>(kFragmentCount));

    // Both paths must produce at least one match per truth row. The
    // alignment should land within `kPositionTolerance` of the truth bounds
    // — the bp-level offset comes from the residual error rate and the
    // chain endpoint heuristic.
    //
    // (A) Phase B baseline: test_force_inmemory_scanner = on. The dispatch
    // buffer hydrates each stub via `fetch_payload` and uses
    // `with_lazy_in_memory` over the materialised varlena. Only valid for
    // total_bp <= MaxAllocSize, but the chosen smoke parameters all fit.
    {
        guc_scope inmemory_scope(ctx, "bioseq.test_force_inmemory_scanner", "on");
        const auto inmemory_results = run_mmsearch(ctx);
        REQUIRE(inmemory_results.size() >= truth.size());
        // Match each truth row against at least one observed match.
        for (const auto& tr : truth) {
            bool matched = false;
            for (const auto& obs : inmemory_results) {
                if (obs.id != tr.id) continue;
                const long long start_delta =
                    std::abs(obs.target_start - tr.truth_start);
                const long long end_delta =
                    std::abs(obs.target_end - tr.truth_end);
                if (start_delta <= kPositionTolerance
                    && end_delta <= kPositionTolerance) {
                    matched = true;
                    require_ideal_match(tr, obs, "in_memory");
                    break;
                }
            }
            REQUIRE(matched);
        }
    }

    // (B) Production scanner_pg path: test_force_inmemory_scanner = off.
    // The dispatch buffer constructs `sequence_scanner_pg<S>` in PG-backed
    // mode for the per-row target column (decodes the locator, reads
    // fragment-by-fragment via Phase A's columnar reader) and in-memory
    // mode for the per-row query column. Both share the same Scanner
    // template type so the minimap_port pipeline only takes ONE
    // instantiation per (alphabet, width).
    {
        guc_scope inmemory_scope(ctx, "bioseq.test_force_inmemory_scanner", "off");
        const auto pg_results = run_mmsearch(ctx);
        REQUIRE(pg_results.size() >= truth.size());
        for (const auto& tr : truth) {
            bool matched = false;
            for (const auto& obs : pg_results) {
                if (obs.id != tr.id) continue;
                const long long start_delta =
                    std::abs(obs.target_start - tr.truth_start);
                const long long end_delta =
                    std::abs(obs.target_end - tr.truth_end);
                if (start_delta <= kPositionTolerance
                    && end_delta <= kPositionTolerance) {
                    matched = true;
                    require_ideal_match(tr, obs, "scanner_pg");
                    break;
                }
            }
            REQUIRE(matched);
        }
    }
}
