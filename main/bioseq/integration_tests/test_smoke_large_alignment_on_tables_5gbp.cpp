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

#include <algorithm>
#include <filesystem>
#include <set>
#include <span>
#include <system_error>
#include <tuple>

import bioseq.integration_tests.common;
import bioseq.common.mlogging;

using namespace bioseq_int_tests;

namespace {

// "OUR_TEST_BASED_ON_XXL_HIFI" parameters — `bench_v10_configuration.json`'s
// `XXL_HIFI` cell minus `concurrency`. The reference is **4 Gbp** — chosen
// to push fragment positions PAST `INT32_MAX` (~2.15 Gbp) so the test
// exercises (and proves) the DOC0274 §1.8 int8 projection fix. Two hard
// ceilings still constrain the largest size BOTH dispatch paths can exercise
// faithfully, and 4 Gbp clears both:
//
//   1. `MaxAllocSize` (`utils/memutils.h`, ~1.07 GiB). Phase B baseline
//      (`bioseq.test_force_inmemory_scanner=on`) hydrates every fragment
//      via `SM::fetch_payload_bulk` → `fetch_payload` and assembles them
//      into ONE contiguous `palloc`'d `pgseq_t` before passing the
//      buffer through `sequence_scanner_none` (the no-fragment wrapper)
//      to `minimap_port`. The hydrated varlena MUST fit in `MaxAllocSize`;
//      at dna4/uint64 the bp ceiling is ~4.29 Gbp (4 Gbp packs to ~1.0e9
//      bytes, ~70 MiB under the cap).
//
//   2. `pgseq_t::bp_size` is `uint32_t` (`sequences.cppm:1980` declares
//      the template default). Any reference > `UINT32_MAX` bp (~4.29 Gbp)
//      silently truncates the `bp_size` field, after which
//      `compact_sequence_range::bp_size()` returns the wrong total and
//      the aligner sees a target that ends ~700 Mbp in. Bumping
//      `bp_size_t` to `uint64_t` is an on-disk format change deferred to
//      Phase D D-VARLENA. 4 Gbp < `UINT32_MAX`, so no truncation here.
//
// FORMER cap #3 (now RESOLVED by DOC0274 §1.8): `bioseq.target_start` &c.
// used to return `INTEGER` (PG's `INT4`) and the custom-scan placeholder
// filler narrowed positions via `Int32GetDatum`, truncating anything >
// `INT32_MAX` (~2.15 Gbp). The eight `sequence_size_t` projections
// (query_len … block_length) now RETURN BIGINT and are filled via
// `Int64GetDatum` (`columnar_customscan.cppm` `set_bigint`), so positions
// up to the project's 1 Tbp pessimistic ceiling round-trip untruncated.
// That is exactly why this test now uses 4 Gbp: at least one fragment lands
// beyond `INT32_MAX` (asserted below via `kInt32Max`), so the §C
// exact-match parity AND the truth-vs-observed tolerance check would BOTH
// spurious-fail under the old INT4 projection — they only hold because the
// projection is now int8.
//
// The OFF path (`sequence_scanner_pg<S>`) doesn't care about (1) or (2)
// — it streams fragments and uses the locator's `uint64` `total_bp_size`,
// working for any size up to the 1 Tbp ceiling. The §C exact-match
// assertion below proves the two paths agree byte-for-byte on the same
// data at this size; the production scanner_pg path is the only one
// that scales past these two caps end-to-end.
constexpr long long kReferenceBp        = 4'000'000'000;  // 4 Gbp (pushes positions past INT32_MAX; clears the ~4.29 Gbp MaxAllocSize / bp_size caps)
constexpr long long kReferenceBp6       = 6'000'000'000;  // 6 Gbp (pushes positions past UINT32_MAX — scanner_pg-only, see kUint32Max note)
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

// Coordinates strictly above this truncate under the OLD `Int32GetDatum`
// projection (PG `INT4`). `kReferenceBp` (the per-row target_len) exceeds it
// deterministically, so the run always exercises the DOC0274 §1.8 int8
// projection fix instead of passing trivially in the < 2.15 Gbp regime; the
// per-fragment truth bounds exceed it too whenever the seed places them high
// (logged as bonus coverage).
constexpr long long kInt32Max = 2'147'483'647;  // INT32_MAX

// DOC0275: the second case pushes the reference to 6 Gbp — PAST `UINT32_MAX`
// (~4.29 Gbp). This is strictly larger than the int8-projection boundary
// (INT32_MAX): it also clears the columnar `pgseq_t::bp_size` uint32 cap and
// the `MaxAllocSize` hydration cap, so ONLY the production streaming path
// (`bioseq.test_force_inmemory_scanner = off`, `sequence_scanner_pg<S>`)
// can serve it — the in-memory baseline would truncate `bp_size` and/or
// exceed `MaxAllocSize`. The aligner carries reference positions as
// `sequence_size_t` (uint64) end-to-end and the radix-sort key
// `Anchor::ref_bits()` now keeps the full 63-bit position (DOC0275), so a
// 6 Gbp target sorts/chains/aligns without the >2^32 wrap that a uint32 key
// would introduce.
constexpr long long kUint32Max = 4'294'967'295;  // UINT32_MAX
constexpr std::uint64_t kMinDiskBytesFor6GbpIndex =
    120ull * 1024ull * 1024ull * 1024ull;

constexpr std::string_view kTable = "phase_c_xxl_hifi_sample";
constexpr std::string_view kTable6 = "phase_c_xxl_hifi_sample_6gbp";

inline void skip_6gbp_if_disk_is_too_low() {
    std::error_code ec;
    const auto info = std::filesystem::space(std::filesystem::current_path(), ec);
    if (ec) {
        WARN(fmt::format("could not preflight disk space for 6 Gbp test: {}", ec.message()));
        return;
    }
    if (info.available < kMinDiskBytesFor6GbpIndex) {
        SKIP(fmt::format(
            "6 Gbp minimizers_per_fragment integration test requires >=120 GiB free; available={:.1f} GiB",
            static_cast<double>(info.available) / (1024.0 * 1024.0 * 1024.0)));
    }
}

inline void seed_sample_table(const connection_test_context& ctx,
                              std::string_view table, long long reference_bp) {
    exec_sqlf(ctx,
        "SELECT bioseq.create_query_target_sample_table("
        "  '{}', {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {});",
        table,
        kFragmentCount,
        kFragmentBp,
        kFragmentStddev,
        reference_bp,
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
    long long rowno           = 0;
};

inline std::vector<truth_row> read_truth(const connection_test_context& ctx,
                                         std::string_view table) {
    // `ctid` is selected as an explicit target-list column (column 6) BEFORE it is
    // also passed to bioseq.column_row_number(...). This is required for the
    // bioseq custom scan: a `ctid` system column referenced ONLY inside an upper
    // expression above a Sort (ORDER BY id) is not carried through the sort, so the
    // function would receive an unprojected/garbage ctid. Selecting ctid as a bare
    // Var materialises it as a real data column that survives the sort. (The
    // production query_key path resolves rowno from the live scan slot's tts_tid in
    // dispatch_buffer/access_method, never through a SQL function above a Sort, so it
    // is unaffected — this is purely a test-harness query-shape requirement.)
    auto rows = query_rows(ctx,
        fmt::format("SELECT id, truth_target_start, truth_target_end, "
                    "is_reverse, original_query_length, ctid, "
                    "bioseq.column_row_number('{}'::regclass, ctid) "
                    "FROM {} ORDER BY id;",
                    table,
                    table));
    std::vector<truth_row> out;
    out.reserve(rows.size());
    for (auto& r : rows) {
        truth_row tr;
        tr.id           = std::stoll(*r[0]);
        tr.truth_start  = std::stoll(*r[1]);
        tr.truth_end    = std::stoll(*r[2]);
        tr.is_reverse   = (*r[3] == "t");
        tr.query_length = std::stoll(*r[4]);
        tr.rowno        = std::stoll(*r[6]);   // r[5] is the explicit ctid column
        out.push_back(tr);
    }
    return out;
}

// Result row from the mmsearch SELECT. The aligner returns a chained
// alignment whose `target_start`/`target_end` should equal the truth bounds
// when the error rate is low (~0.2%) and the chain is correctly placed.
// `target_len` is the FULL reference length (PAF col 7) — the generator
// stores the same 4 Gbp reference as every row's target, so this field is
// deterministically `kReferenceBp` (> INT32_MAX) and is the seed-independent
// witness that the int8 projection works (under INT4 it truncated to a
// negative value).
struct mmsearch_row {
    long long id            = 0;
    long long target_start  = 0;
    long long target_end    = 0;
    long long target_len    = 0;
};

[[nodiscard]] std::set<long long> result_id_set(std::span<const mmsearch_row> rows) {
    std::set<long long> out;
    for (const auto& row : rows) out.insert(row.id);
    return out;
}

inline std::vector<mmsearch_row> run_mmsearch(
    const connection_test_context& ctx, std::string_view table)
{
    // Per-row mmsearch: arg0 is `target` column, arg1 is `query` column —
    // both are Vars of the same relation, so the dispatch_buffer takes the
    // per-row branch (DOC0274). The aligner's reported target_start /
    // target_end should equal the truth bounds within tolerance.
    //
    // No `::bigint` cast: post-DOC0274-§1.8 `bioseq.target_start` /
    // `bioseq.target_end` RETURN BIGINT natively, so the value read here is
    // the untruncated int64 coordinate. (Under the old INT4 projection a
    // position past INT32_MAX would arrive already truncated — no SQL cast
    // could recover it — which is what this 4 Gbp test now rules out.)
    auto rows = query_rows(ctx,
        fmt::format("SELECT id, "
                    "       bioseq.target_start(id), "
                    "       bioseq.target_end(id), "
                    "       bioseq.target_len(id) "
                    "  FROM {} "
                    " WHERE mmsearch(target, query) "
                    " ORDER BY id;",
                    table));
    std::vector<mmsearch_row> out;
    out.reserve(rows.size());
    for (auto& r : rows) {
        if (!r[0].has_value() || !r[1].has_value()
            || !r[2].has_value() || !r[3].has_value()) {
            continue;
        }
        mmsearch_row m;
        m.id           = std::stoll(*r[0]);
        m.target_start = std::stoll(*r[1]);
        m.target_end   = std::stoll(*r[2]);
        m.target_len   = std::stoll(*r[3]);
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
    std::string_view scanner_label, long long reference_bp)
{
    const long long start_delta =
        std::abs(observed.target_start - truth.truth_start);
    const long long end_delta =
        std::abs(observed.target_end - truth.truth_end);
    bioseq_log<DBG>("[test] scanner={} id={} truth=[{},{}) observed=[{},{}) "
            "target_len={} start_delta={} end_delta={} is_reverse={}",
            scanner_label, truth.id, truth.truth_start, truth.truth_end,
            observed.target_start, observed.target_end,
            observed.target_len, start_delta, end_delta, truth.is_reverse);
    REQUIRE(start_delta <= kPositionTolerance);
    REQUIRE(end_delta   <= kPositionTolerance);
    // Seed-INDEPENDENT int8-projection proof: target_len is the full
    // `reference_bp` reference length (> INT32_MAX, and for the 6 Gbp case
    // > UINT32_MAX). The old INT4 projection truncated it to a negative
    // value; the int8 projection returns it exactly.
    REQUIRE(observed.target_len == reference_bp);
}

}  // namespace

TEST_CASE("DOC0274 — XXL_HIFI sample table: scanner_pg (test_force_inmemory_scanner=off) "
          "and Phase B baseline (=on) both produce ideal matches AND identical bounds",
          "[integration][phase-c][xxl-hifi][scanner-pg]") {
    auto ctx = fresh_context({kTable});
    seed_sample_table(ctx, kTable, kReferenceBp);
    const auto truth = read_truth(ctx, kTable);
    REQUIRE(truth.size() == static_cast<std::size_t>(kFragmentCount));

    // This test only PROVES the DOC0274 §1.8 int8 projection fix if a
    // projected coordinate exceeds INT32_MAX — otherwise the int4 and int8
    // projections are byte-identical and the run is vacuous on the truncation
    // question. The seed-INDEPENDENT witness is `bioseq.target_len(id)`: every
    // row's target is the same `kReferenceBp` (4 Gbp) reference, so target_len
    // is always 4e9 > INT32_MAX and is asserted equal to `kReferenceBp` in the
    // per-row checks below (under the old INT4 projection it truncated to a
    // negative value). `static_assert` pins the invariant at compile time so a
    // future shrink of `kReferenceBp` can't silently make this a sub-2.15-Gbp
    // test. Fragment start/end ALSO exercise the fix whenever they land past
    // INT32_MAX — logged here as bonus coverage (their placement is
    // seed-dependent, so it is informative, not asserted).
    static_assert(kReferenceBp > kInt32Max,
                  "4 Gbp reference must exceed INT32_MAX to exercise the int8 projection fix");
    for (const auto& tr : truth) {
        bioseq_log<DBG>("[test] truth id={} [{},{}) (start_past_int32={} end_past_int32={})",
                tr.id, tr.truth_start, tr.truth_end,
                tr.truth_start > kInt32Max, tr.truth_end > kInt32Max);
    }

    // (A) Phase B baseline: `bioseq.test_force_inmemory_scanner = on`. The
    // dispatch buffer hydrates the stub via `fetch_payload` and dispatches
    // through `with_lazy_in_memory` (`sequence_scanner_none`) over ONE
    // contiguous materialised varlena. At 4 Gbp the dna4/uint64 packing is
    // ~1.0e9 bytes — under `MaxAllocSize` (~1.07 GiB) — so hydration
    // succeeds. (Past ~4.29 Gbp the assembled varlena would exceed
    // `MaxAllocSize`; in that regime this baseline path raises an explicit
    // `bioseq_log<ERR, PG>( …)` rather than silently falling through to scanner_pg
    // — DOC0274 §1.4.2 — so the GUC toggle keeps its force-baseline meaning.
    // That regime is out of scope here.) Both paths therefore run the same
    // alignment over the same bytes and are expected to produce
    // bit-identical observed bounds.
    std::vector<mmsearch_row> inmemory_results;
    {
        guc_scope inmemory_scope(ctx, "bioseq.test_force_inmemory_scanner", "on");
        inmemory_results = run_mmsearch(ctx, kTable);
    }
    REQUIRE(inmemory_results.size() >= truth.size());
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
                require_ideal_match(tr, obs, "in_memory", kReferenceBp);
                break;
            }
        }
        REQUIRE(matched);
    }

    // (B) Production scanner_pg path: `bioseq.test_force_inmemory_scanner =
    // off`. The dispatch buffer constructs `sequence_scanner_pg<S>` in
    // PG-backed mode for the per-row target column (decodes the locator,
    // reads fragment-by-fragment via Phase A's columnar reader) and
    // in-memory mode for the per-row query column. Both share the same
    // Scanner template type so the minimap_port pipeline only takes ONE
    // instantiation per (alphabet, width).
    std::vector<mmsearch_row> pg_results;
    {
        guc_scope inmemory_scope(ctx, "bioseq.test_force_inmemory_scanner", "off");
        pg_results = run_mmsearch(ctx, kTable);
    }
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
                require_ideal_match(tr, obs, "scanner_pg", kReferenceBp);
                break;
            }
        }
        REQUIRE(matched);
    }

    // (C) EXACT-MATCH parity: enabling a different Scanner type MUST NOT
    // affect the aligner's observed start/end — the algorithm is identical
    // across the two paths, only the byte source differs. We sort each
    // result vector by `id` so cross-path comparison is well-defined when
    // mmsearch returns multiple secondary alignments per row (the order
    // inside one row is deterministic per path because both paths use the
    // same SQL `ORDER BY id`).
    auto by_id = [](const mmsearch_row& a, const mmsearch_row& b) {
        return std::tie(a.id, a.target_start, a.target_end)
             < std::tie(b.id, b.target_start, b.target_end);
    };
    std::sort(inmemory_results.begin(), inmemory_results.end(), by_id);
    std::sort(pg_results.begin(), pg_results.end(), by_id);
    REQUIRE(inmemory_results.size() == pg_results.size());
    for (std::size_t i = 0; i < inmemory_results.size(); ++i) {
        const auto& a = inmemory_results[i];
        const auto& b = pg_results[i];
        bioseq_log<DBG>("[test] parity row {}: in_memory id={} [{},{}) len={} vs scanner_pg id={} [{},{}) len={}",
                i, a.id, a.target_start, a.target_end, a.target_len,
                b.id, b.target_start, b.target_end, b.target_len);
        REQUIRE(a.id           == b.id);
        REQUIRE(a.target_start == b.target_start);
        REQUIRE(a.target_end   == b.target_end);
        // Parity on the > INT32_MAX target_len: fragmented scanner_pg and the
        // in-memory none_scanner must agree byte-for-byte, AND both must equal
        // the untruncated 4 Gbp reference length — the core int8-projection claim.
        REQUIRE(a.target_len   == b.target_len);
        REQUIRE(a.target_len   == kReferenceBp);
    }
}

TEST_CASE("DOC0275 — 6 Gbp sample table: scanner_pg (test_force_inmemory_scanner=off) "
          "aligns within tolerance AND target_len round-trips past UINT32_MAX",
          "[integration][phase-c][xxl-hifi][scanner-pg][6gbp]") {
    // 6 Gbp is past `UINT32_MAX` (~4.29 Gbp), so — unlike the 4 Gbp case — the
    // in-memory baseline (`test_force_inmemory_scanner = on`) CANNOT serve it:
    // assembling the whole target into one varlena would exceed `MaxAllocSize`
    // and `pgseq_t::bp_size` (uint32) would truncate the reported length. We
    // therefore exercise ONLY the production streaming path
    // (`test_force_inmemory_scanner = off`, `sequence_scanner_pg<S>`), which
    // uses the locator's uint64 `total_bp_size` and reads fragment-by-fragment.
    // This is the single, lenient "positions within tolerance" case the
    // DOC0275 follow-up asked for: it proves the columnar metadata read
    // (DOC0275 chunk_group `deleted_rows` fix), the aligner's full-64-bit
    // position handling past 2^32, and the int8 projection all hold at a size
    // strictly beyond every 32-bit boundary in the stack.
    static_assert(kReferenceBp6 > kUint32Max,
                  "6 Gbp reference must exceed UINT32_MAX to exercise the >2^32 position path");
    skip_6gbp_if_disk_is_too_low();

    auto ctx = fresh_context({kTable6});
    exec_sql(ctx, "SET bioseq.update_index_on_insert = off;");
    seed_sample_table(ctx, kTable6, kReferenceBp6);
    const auto truth = read_truth(ctx, kTable6);
    REQUIRE(truth.size() == static_cast<std::size_t>(kFragmentCount));
    for (const auto& tr : truth) {
        bioseq_log<DBG>("[test] 6gbp truth id={} rowno={} [{},{}) (start_past_uint32={} end_past_uint32={})",
                tr.id, tr.rowno, tr.truth_start, tr.truth_end,
                tr.truth_start > kUint32Max, tr.truth_end > kUint32Max);
    }

    const long long updated_rows = query_scalar_int(ctx,
        fmt::format("SELECT bioseq.update_index('{}'::regclass, 'minimizers_per_fragment')", kTable6));
    REQUIRE(updated_rows == static_cast<long long>(truth.size()));

    constexpr long long kFragBp = 1024LL * 32LL;
    const long long expected_target_fragments = (kReferenceBp6 + kFragBp - 1) / kFragBp;
    for (const auto& tr : truth) {
        const long long distinct_sub_keys =
            index_distinct_sub_keys(ctx, "minimizers_per_fragment", kTable6, tr.rowno);
        bioseq_log<DBG>("[test] 6gbp row id={} rowno={} distinct_sub_keys={} expected_target_fragments={}",
                tr.id, tr.rowno, distinct_sub_keys, expected_target_fragments);
        REQUIRE(distinct_sub_keys >= expected_target_fragments);
    }

    std::vector<mmsearch_row> recompute_results;
    {
        guc_scope inmemory_scope(ctx, "bioseq.test_force_inmemory_scanner", "off");
        guc_scope read_scope(ctx, "bioseq.read_sequence_index", "off");
        recompute_results = run_mmsearch(ctx, kTable6);
    }
    REQUIRE(recompute_results.size() >= truth.size());
    for (const auto& tr : truth) {
        bool matched = false;
        for (const auto& obs : recompute_results) {
            if (obs.id != tr.id) continue;
            const long long start_delta =
                std::abs(obs.target_start - tr.truth_start);
            const long long end_delta =
                std::abs(obs.target_end - tr.truth_end);
            if (start_delta <= kPositionTolerance
                && end_delta <= kPositionTolerance) {
                matched = true;
                require_ideal_match(tr, obs, "scanner_pg_6gbp_recompute", kReferenceBp6);
                break;
            }
        }
        REQUIRE(matched);
    }

    exec_sqlf(ctx,
        "SELECT bioseq.set_active_read_index('{}'::regclass, 'minimizers_per_fragment');",
        kTable6);
    std::vector<mmsearch_row> assemble_results;
    {
        guc_scope inmemory_scope(ctx, "bioseq.test_force_inmemory_scanner", "off");
        guc_scope read_scope(ctx, "bioseq.read_sequence_index", "on");
        assemble_results = run_mmsearch(ctx, kTable6);
    }
    REQUIRE(assemble_results.size() >= truth.size());
    for (const auto& tr : truth) {
        bool matched = false;
        for (const auto& obs : assemble_results) {
            if (obs.id != tr.id) continue;
            const long long start_delta =
                std::abs(obs.target_start - tr.truth_start);
            const long long end_delta =
                std::abs(obs.target_end - tr.truth_end);
            if (start_delta <= kPositionTolerance
                && end_delta <= kPositionTolerance) {
                matched = true;
                require_ideal_match(tr, obs, "scanner_pg_6gbp_assemble", kReferenceBp6);
                break;
            }
        }
        REQUIRE(matched);
    }

    const auto recompute_ids = result_id_set(recompute_results);
    const auto assemble_ids = result_id_set(assemble_results);
    REQUIRE(std::includes(assemble_ids.begin(), assemble_ids.end(),
                          recompute_ids.begin(), recompute_ids.end()));
}
