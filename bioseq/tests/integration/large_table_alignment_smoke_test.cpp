/**
 * @file
 * @brief Large-reference alignment parity across streaming and in-memory scanners.
 *
 * Generated HiFi-like samples exercise fragmented storage, 64-bit projected
 * coordinates, exact match placement, and the streaming-only 6 Gbp route.
 */

#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.tests.integration.support;
import bioseq.common.logging;

using namespace bioseq_int_tests;

namespace {

// The large HiFi reference is 4 Gbp, chosen
// to push fragment positions PAST `INT32_MAX` (~2.15 Gbp) so execution
// exercises the BIGINT projection contract. Two hard ceilings constrain the
// largest size both dispatch paths can exercise faithfully, and 4 Gbp clears
// both:
//
//   1. `MaxAllocSize` (`utils/memutils.h`, ~1.07 GiB). The in-memory scanner
//      (`bioseq.testing.in_memory_scanner.enabled=on`) hydrates every fragment
//      via `SM::fetch_payload_bulk` → `fetch_payload` and assembles them
//      into ONE contiguous `palloc`'d `bioseq::pg_types::seq` before passing the
//      buffer through `sequence_scanner_none` (the no-fragment wrapper)
//      to `minimap_port`. The hydrated varlena MUST fit in `MaxAllocSize`;
//      at dna4/uint64 the bp ceiling is ~4.29 Gbp (4 Gbp packs to ~1.0e9
//      bytes, ~70 MiB under the cap).
//
//   2. `bioseq::pg_types::seq::bp_size` is `uint32_t` (`sequences.cppm:1980` declares
//      the template default). Any reference > `UINT32_MAX` bp (~4.29 Gbp)
//      silently truncates the `bp_size` field, after which
//      `compact_sequence_range::bp_size()` returns the wrong total and
//      the aligner sees an incorrect target length. This case stays below
//      `UINT32_MAX`, so no truncation occurs here.
//
// `bioseq.target_start` and the other `sequence_size_t` projections return
// BIGINT and use `Int64GetDatum`, so positions up to the project's 1 Tbp
// pessimistic ceiling round-trip untruncated. The 4 Gbp case ensures at least
// one fragment lands beyond `INT32_MAX`; exact-match parity and the
// truth-vs-observed tolerance check exercise that contract.
//
// The OFF path (`sequence_scanner_pg<S>`) doesn't care about (1) or (2)
// — it streams fragments and uses the locator's `uint64` `total_bp_size`,
// working for any size up to the 1 Tbp ceiling. The exact-match comparison
// below verifies that the two paths agree byte-for-byte on the same data at this
// size; the production scanner_pg path is the only one that scales past these
// two caps end-to-end.
constexpr long long k_reference_bp =
    4'000'000'000; // 4 Gbp (pushes positions past INT32_MAX; clears the ~4.29 Gbp MaxAllocSize /
                   // bp_size caps)
constexpr long long k_six_gbp_reference_bp =
    6'000'000'000; // 6 Gbp (pushes positions past UINT32_MAX — scanner_pg-only, see
                   // k_uint32_maximum note)
constexpr long long k_fragment_bp = 5'000'000;               // 5 Mbp
constexpr long long k_fragment_standard_deviation = 100'000; // 100 kbp
constexpr int k_fragment_count = 3;
constexpr double k_substitution_rate = 0.001;
constexpr double k_insertion_rate = 0.0005;
constexpr double k_deletion_rate = 0.0005;
constexpr double k_insertion_length_mean = 1.0;
constexpr double k_deletion_length_mean = 1.0;
constexpr double k_reverse_rate = 0.5;
constexpr long long k_seed = 3219876;

// `k_reference_bp` (the per-row target_len) exceeds `INT32_MAX`
// deterministically, so the run always exercises BIGINT projection instead of
// passing trivially in the < 2.15 Gbp regime; the
// per-fragment truth bounds exceed it too whenever the seed places them high
// (logged as bonus coverage).
constexpr long long k_int32_maximum = 2'147'483'647; // INT32_MAX

// The 6 Gbp reference passes `UINT32_MAX` (~4.29
// Gbp). This is strictly larger than the int8-projection boundary
// (INT32_MAX): it also clears the columnar `bioseq::pg_types::seq::bp_size` uint32 cap and
// the `MaxAllocSize` hydration cap, so ONLY the production streaming path
// (`bioseq.testing.in_memory_scanner.enabled = off`, `sequence_scanner_pg<S>`)
// can serve it — the in-memory baseline would truncate `bp_size` and/or
// exceed `MaxAllocSize`. The aligner carries reference positions as
// `sequence_size_t` (uint64) end-to-end and the radix-sort key
// `Anchor::ref_bits()` keeps the full 63-bit position, so a
// 6 Gbp target sorts/chains/aligns without the >2^32 wrap that a uint32 key
// would introduce.
constexpr long long k_uint32_maximum = 4'294'967'295; // UINT32_MAX
constexpr std::uint64_t k_minimum_disk_bytes_for_six_gbp_index =
    120ull * 1024ull * 1024ull * 1024ull;

constexpr std::string_view k_hifi_table = "large_hifi_alignment_sample";
constexpr std::string_view k_six_gbp_table = "large_alignment_sample_6gbp";

/** Skip the 6 Gbp scenario when the local filesystem cannot hold its index. */
inline void skip_6gbp_if_disk_is_too_low() {
    std::error_code ec;
    const auto info = std::filesystem::space(std::filesystem::current_path(), ec);
    if (ec) {
        WARN(fmt::format("could not preflight disk space for 6 Gbp alignment: {}", ec.message()));
        return;
    }
    if (info.available < k_minimum_disk_bytes_for_six_gbp_index) {
        SKIP(fmt::format(
            "6 Gbp minimizers_per_fragment scenario requires >=120 GiB free; available={:.1f} GiB",
            static_cast<double>(info.available) / (1024.0 * 1024.0 * 1024.0)));
    }
}

/** Generate one query-target sample table with the canonical error model. */
inline void seed_sample_table(const connection_test_context& ctx,
                              std::string_view table,
                              long long reference_bp) {
    exec_sqlf(ctx,
              "SELECT bioseq.create_query_target_sample_table("
              "  '{}', {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {});",
              table,
              k_fragment_count,
              k_fragment_bp,
              k_fragment_standard_deviation,
              reference_bp,
              k_substitution_rate,
              k_insertion_rate,
              k_deletion_rate,
              k_insertion_length_mean,
              k_deletion_length_mean,
              k_reverse_rate,
              k_seed);
}

/** Ground-truth placement and source row identity for one generated query. */
struct truth_row {
    long long id = 0;
    long long truth_start = 0;
    long long truth_end = 0;
    bool is_reverse = false;
    long long query_length = 0;
    long long rowno = 0;
};

/** Read generated truth coordinates and stable physical row numbers. */
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
    // is unaffected — this is purely a SQL query-shape requirement.)
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
        tr.id = std::stoll(*r[0]);
        tr.truth_start = std::stoll(*r[1]);
        tr.truth_end = std::stoll(*r[2]);
        tr.is_reverse = (*r[3] == "t");
        tr.query_length = std::stoll(*r[4]);
        tr.rowno = std::stoll(*r[6]); // r[5] is the explicit ctid column
        out.push_back(tr);
    }
    return out;
}

/** Alignment projections returned by the canonical mmsearch statement. */
struct mmsearch_row {
    long long id = 0;
    long long target_start = 0;
    long long target_end = 0;
    long long target_len = 0;
};

/** Return the unique generated IDs represented by alignment output rows. */
[[nodiscard]] std::set<long long> result_id_set(std::span<const mmsearch_row> rows) {
    std::set<long long> out;
    for (const auto& row : rows)
        out.insert(row.id);
    return out;
}

/** Execute canonical per-row mmsearch and retain complete projection rows. */
inline std::vector<mmsearch_row> run_mmsearch(const connection_test_context& ctx,
                                              std::string_view table) {
    // Per-row mmsearch: arg0 is `target` column, arg1 is `query` column —
    // both are Vars of the same relation, so the dispatch_buffer takes the
    // per-row branch. The aligner's reported target_start /
    // target_end should equal the truth bounds within tolerance.
    //
    // No `::bigint` cast: `bioseq.target_start` /
    // `bioseq.target_end` RETURN BIGINT natively, so the value read here is
    // the untruncated int64 coordinate, including positions past INT32_MAX.
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
        if (!r[0].has_value() || !r[1].has_value() || !r[2].has_value() || !r[3].has_value()) {
            continue;
        }
        mmsearch_row m;
        m.id = std::stoll(*r[0]);
        m.target_start = std::stoll(*r[1]);
        m.target_end = std::stoll(*r[2]);
        m.target_len = std::stoll(*r[3]);
        out.push_back(m);
    }
    return out;
}

// Tolerance for the aligner's reported start/end vs the truth bounds. With
// HiFi-style 0.2 % error rate and ~5 Mbp queries, the chain endpoint is
// typically within ~10 kbp of truth (the trailing low-quality region of
// the alignment is trimmed by the band).
constexpr long long k_position_tolerance = 50'000;

/** Require observed alignment bounds and target length to match generated truth. */
inline void require_ideal_match(const truth_row& truth,
                                const mmsearch_row& observed,
                                std::string_view scanner_label,
                                long long reference_bp) {
    const long long start_delta = std::abs(observed.target_start - truth.truth_start);
    const long long end_delta = std::abs(observed.target_end - truth.truth_end);
    bioseq_log<logging_level::debug>(
        "[large-alignment] scanner={} id={} truth=[{},{}) observed=[{},{}) "
        "target_len={} start_delta={} end_delta={} is_reverse={}",
        scanner_label,
        truth.id,
        truth.truth_start,
        truth.truth_end,
        observed.target_start,
        observed.target_end,
        observed.target_len,
        start_delta,
        end_delta,
        truth.is_reverse);
    REQUIRE(start_delta <= k_position_tolerance);
    REQUIRE(end_delta <= k_position_tolerance);
    // The seed-independent int8 projection contract uses the full target_len:
    // `reference_bp` reference length (> INT32_MAX, and for the 6 Gbp case
    // > UINT32_MAX), and the int8 projection returns it exactly.
    REQUIRE(observed.target_len == reference_bp);
}

} // namespace

TEST_CASE("large HiFi sample table produces identical ideal matches with the "
          "PostgreSQL and in-memory scanners",
          "[component:bioseq][layer:integration][feature:alignment][resource:postgres][resource:"
          "filesystem][resource:large_memory]") {
    auto ctx = fresh_context({k_hifi_table});
    seed_sample_table(ctx, k_hifi_table, k_reference_bp);
    const auto truth = read_truth(ctx, k_hifi_table);
    REQUIRE(truth.size() == static_cast<std::size_t>(k_fragment_count));

    // The BIGINT projection contract is exercised by coordinates above INT32_MAX.
    // `bioseq.target_len(id)` is seed-independent: every
    // row's target is the same `k_reference_bp` (4 Gbp) reference, so target_len
    // is always 4e9 > INT32_MAX and is asserted equal to `k_reference_bp` in the
    // per-row checks below. `static_assert` keeps the reference above the
    // projection boundary. Fragment start/end also cover values past INT32_MAX
    // when their deterministic placement reaches that range.
    static_assert(k_reference_bp > k_int32_maximum,
                  "4 Gbp reference must exceed INT32_MAX for BIGINT projection coverage");
    for (const auto& tr : truth) {
        bioseq_log<logging_level::debug>("[large-alignment] truth id={} [{},{}) "
                                         "(start_past_int32={} end_past_int32={})",
                                         tr.id,
                                         tr.truth_start,
                                         tr.truth_end,
                                         tr.truth_start > k_int32_maximum,
                                         tr.truth_end > k_int32_maximum);
    }

    // The in-memory scanner uses `bioseq.testing.in_memory_scanner.enabled = on`. The
    // dispatch buffer hydrates the stub via `fetch_payload` and dispatches
    // through `with_lazy_in_memory` (`sequence_scanner_none`) over ONE
    // contiguous materialised varlena. At 4 Gbp the dna4/uint64 packing is
    // ~1.0e9 bytes — under `MaxAllocSize` (~1.07 GiB) — so hydration
    // succeeds. (Past ~4.29 Gbp the assembled varlena would exceed
    // `MaxAllocSize`; in that regime this baseline path raises an explicit
    // `bioseq_log<ERR, PG>( …)` rather than silently falling through to
    // scanner_pg, so the GUC toggle keeps its force-baseline meaning.
    // That regime is out of scope here.) Both paths therefore run the same
    // alignment over the same bytes and are expected to produce
    // bit-identical observed bounds.
    std::vector<mmsearch_row> inmemory_results;
    {
        guc_scope inmemory_scope(ctx, "bioseq.testing.in_memory_scanner.enabled", "on");
        inmemory_results = run_mmsearch(ctx, k_hifi_table);
    }
    REQUIRE(inmemory_results.size() >= truth.size());
    for (const auto& tr : truth) {
        bool matched = false;
        for (const auto& obs : inmemory_results) {
            if (obs.id != tr.id)
                continue;
            const long long start_delta = std::abs(obs.target_start - tr.truth_start);
            const long long end_delta = std::abs(obs.target_end - tr.truth_end);
            if (start_delta <= k_position_tolerance && end_delta <= k_position_tolerance) {
                matched = true;
                require_ideal_match(tr, obs, "in_memory", k_reference_bp);
                break;
            }
        }
        REQUIRE(matched);
    }

    // The PostgreSQL scanner uses `bioseq.testing.in_memory_scanner.enabled =
    // off`. The dispatch buffer constructs `sequence_scanner_pg<S>` in
    // PG-backed mode for the per-row target column (decodes the locator,
    // reads fragment-by-fragment through the columnar reader) and
    // in-memory mode for the per-row query column. Both share the same
    // Scanner template type so the minimap_port pipeline only takes ONE
    // instantiation per (alphabet, width).
    std::vector<mmsearch_row> pg_results;
    {
        guc_scope inmemory_scope(ctx, "bioseq.testing.in_memory_scanner.enabled", "off");
        pg_results = run_mmsearch(ctx, k_hifi_table);
    }
    REQUIRE(pg_results.size() >= truth.size());
    for (const auto& tr : truth) {
        bool matched = false;
        for (const auto& obs : pg_results) {
            if (obs.id != tr.id)
                continue;
            const long long start_delta = std::abs(obs.target_start - tr.truth_start);
            const long long end_delta = std::abs(obs.target_end - tr.truth_end);
            if (start_delta <= k_position_tolerance && end_delta <= k_position_tolerance) {
                matched = true;
                require_ideal_match(tr, obs, "scanner_pg", k_reference_bp);
                break;
            }
        }
        REQUIRE(matched);
    }

    // Scanner parity requires that changing the Scanner type does not
    // affect the aligner's observed start/end — the algorithm is identical
    // across the two paths, only the byte source differs. We sort each
    // result vector by `id` so cross-path comparison is well-defined when
    // mmsearch returns multiple secondary alignments per row (the order
    // inside one row is deterministic per path because both paths use the
    // same SQL `ORDER BY id`).
    auto by_id = [](const mmsearch_row& a, const mmsearch_row& b) {
        return std::tie(a.id, a.target_start, a.target_end) <
               std::tie(b.id, b.target_start, b.target_end);
    };
    std::sort(inmemory_results.begin(), inmemory_results.end(), by_id);
    std::sort(pg_results.begin(), pg_results.end(), by_id);
    REQUIRE(inmemory_results.size() == pg_results.size());
    for (std::size_t i = 0; i < inmemory_results.size(); ++i) {
        const auto& a = inmemory_results[i];
        const auto& b = pg_results[i];
        bioseq_log<logging_level::debug>("[large-alignment] parity row {}: in_memory id={} [{},{}) "
                                         "len={} vs scanner_pg id={} [{},{}) len={}",
                                         i,
                                         a.id,
                                         a.target_start,
                                         a.target_end,
                                         a.target_len,
                                         b.id,
                                         b.target_start,
                                         b.target_end,
                                         b.target_len);
        REQUIRE(a.id == b.id);
        REQUIRE(a.target_start == b.target_start);
        REQUIRE(a.target_end == b.target_end);
        // Parity on the > INT32_MAX target_len: fragmented scanner_pg and the
        // in-memory none_scanner must agree byte-for-byte, AND both must equal
        // the untruncated 4 Gbp reference length required by the int8 projection.
        REQUIRE(a.target_len == b.target_len);
        REQUIRE(a.target_len == k_reference_bp);
    }
}

TEST_CASE("6 Gbp sample table aligns within tolerance and target_len "
          "round-trips past UINT32_MAX with the PostgreSQL scanner",
          "[component:bioseq][layer:integration][feature:alignment][resource:postgres][resource:"
          "filesystem][resource:large_memory]") {
    // 6 Gbp is past `UINT32_MAX` (~4.29 Gbp), so — unlike the 4 Gbp case — the
    // in-memory baseline (`bioseq.testing.in_memory_scanner.enabled = on`) CANNOT serve it:
    // assembling the whole target into one varlena would exceed `MaxAllocSize`
    // and `bioseq::pg_types::seq::bp_size` (uint32) would truncate the reported length. We
    // therefore exercise ONLY the production streaming path
    // (`bioseq.testing.in_memory_scanner.enabled = off`, `sequence_scanner_pg<S>`), which
    // uses the locator's uint64 `total_bp_size` and reads fragment-by-fragment.
    // Positions within tolerance require the columnar
    // metadata read, full-64-bit aligner positions past 2^32, and BIGINT
    // projection at a size strictly beyond every 32-bit boundary in the stack.
    static_assert(k_six_gbp_reference_bp > k_uint32_maximum,
                  "6 Gbp reference must exceed UINT32_MAX to exercise the >2^32 position path");
    skip_6gbp_if_disk_is_too_low();

    auto ctx = fresh_context({k_six_gbp_table});
    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
    seed_sample_table(ctx, k_six_gbp_table, k_six_gbp_reference_bp);
    const auto truth = read_truth(ctx, k_six_gbp_table);
    REQUIRE(truth.size() == static_cast<std::size_t>(k_fragment_count));
    for (const auto& tr : truth) {
        bioseq_log<logging_level::debug>("[large-alignment] 6gbp truth id={} rowno={} [{},{}) "
                                         "(start_past_uint32={} end_past_uint32={})",
                                         tr.id,
                                         tr.rowno,
                                         tr.truth_start,
                                         tr.truth_end,
                                         tr.truth_start > k_uint32_maximum,
                                         tr.truth_end > k_uint32_maximum);
    }

    const long long updated_rows = query_scalar_int(
        ctx,
        fmt::format("SELECT bioseq.update_index('{}'::regclass, 'minimizers_per_fragment')",
                    k_six_gbp_table));
    REQUIRE(updated_rows == static_cast<long long>(truth.size()));

    constexpr long long k_fragment_bp = 1024LL * 32LL;
    const long long expected_target_fragments =
        (k_six_gbp_reference_bp + k_fragment_bp - 1) / k_fragment_bp;
    for (const auto& tr : truth) {
        const long long distinct_sub_keys =
            index_distinct_sub_keys(ctx, "minimizers_per_fragment", k_six_gbp_table, tr.rowno);
        bioseq_log<logging_level::debug>("[large-alignment] 6gbp row id={} rowno={} "
                                         "distinct_sub_keys={} expected_target_fragments={}",
                                         tr.id,
                                         tr.rowno,
                                         distinct_sub_keys,
                                         expected_target_fragments);
        REQUIRE(distinct_sub_keys >= expected_target_fragments);
    }

    std::vector<mmsearch_row> recompute_results;
    {
        guc_scope inmemory_scope(ctx, "bioseq.testing.in_memory_scanner.enabled", "off");
        guc_scope read_scope(ctx, "bioseq.index.sequence_read.enabled", "off");
        recompute_results = run_mmsearch(ctx, k_six_gbp_table);
    }
    REQUIRE(recompute_results.size() >= truth.size());
    for (const auto& tr : truth) {
        bool matched = false;
        for (const auto& obs : recompute_results) {
            if (obs.id != tr.id)
                continue;
            const long long start_delta = std::abs(obs.target_start - tr.truth_start);
            const long long end_delta = std::abs(obs.target_end - tr.truth_end);
            if (start_delta <= k_position_tolerance && end_delta <= k_position_tolerance) {
                matched = true;
                require_ideal_match(tr, obs, "scanner_pg_6gbp_recompute", k_six_gbp_reference_bp);
                break;
            }
        }
        REQUIRE(matched);
    }

    exec_sqlf(ctx,
              "SELECT bioseq.set_active_read_index('{}'::regclass, 'minimizers_per_fragment');",
              k_six_gbp_table);
    std::vector<mmsearch_row> assemble_results;
    {
        guc_scope inmemory_scope(ctx, "bioseq.testing.in_memory_scanner.enabled", "off");
        guc_scope read_scope(ctx, "bioseq.index.sequence_read.enabled", "on");
        assemble_results = run_mmsearch(ctx, k_six_gbp_table);
    }
    REQUIRE(assemble_results.size() >= truth.size());
    for (const auto& tr : truth) {
        bool matched = false;
        for (const auto& obs : assemble_results) {
            if (obs.id != tr.id)
                continue;
            const long long start_delta = std::abs(obs.target_start - tr.truth_start);
            const long long end_delta = std::abs(obs.target_end - tr.truth_end);
            if (start_delta <= k_position_tolerance && end_delta <= k_position_tolerance) {
                matched = true;
                require_ideal_match(tr, obs, "scanner_pg_6gbp_assemble", k_six_gbp_reference_bp);
                break;
            }
        }
        REQUIRE(matched);
    }

    const auto recompute_ids = result_id_set(recompute_results);
    const auto assemble_ids = result_id_set(assemble_results);
    REQUIRE(std::includes(
        assemble_ids.begin(), assemble_ids.end(), recompute_ids.begin(), recompute_ids.end()));
}
