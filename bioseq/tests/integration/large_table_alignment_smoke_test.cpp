/**
 * @file
 * @brief Compares alignment results for large fragmented references.
 *
 * The 4 Gbp case compares streaming and in-memory scans. The 6 Gbp case
 * uses streaming only. Both check query placement and 64-bit target lengths.
 */

#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.tests.integration.support;
import bioseq.common.logging;

using namespace bioseq_int_tests;

namespace {

// A 4 Gbp DNA4 reference needs abou 1 GB of packed bases. It fits the
// in-memory baseline while its length exceeds signed 32-bit range. This lets
// both scanners run on the same data and checks that SQL target lengths use
// BIGINT. Query positions also test that range when their generated placement
// falls beyond INT32_MAX.
constexpr long long k_reference_bp =
    4'000'000'000; // 4 Gbp for, both scanner paths
                   // fits the packed in-memory baseline
constexpr long long k_six_gbp_reference_bp =
    6'000'000'000; // 6 Gbp for  the streaming scanner
                   // exceeds the flat sequence length limit
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

// The reference length always exceeds INT32_MAX, regardless of where
// the generated queries fall. Checking target_len therefore tests 64-bit SQL output.
constexpr long long k_int32_maximum = 2'147'483'647; // INT32_MAX

// The 6 Gbp reference also exceeds unsigned 32-bit range. Reading it
// fragment by fragment avoids the flat sequence format and whole-reference
// allocation limits. The test checks its full length through BIGINT output.
constexpr long long k_uint32_maximum = 4'294'967'295; // UINT32_MAX
constexpr std::uint64_t k_minimum_disk_bytes_for_six_gbp_index =
    120ull * 1024ull * 1024ull * 1024ull;

constexpr std::string_view k_hifi_table = "large_hifi_alignment_sample";
constexpr std::string_view k_six_gbp_table = "large_alignment_sample_6gbp";

/** Skips the 6 Gbp case when the working filesystem has less than 120 GiB free. */
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

/** Generates query/target pairs using this fixture's error rates and seed. */
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

/** Reads known  query positions and the table's sequence row numbers. */
inline std::vector<truth_row> read_truth(const connection_test_context& ctx,
                                         std::string_view table) {
    // Select ctid as its own column so it is carried through the orered query
    // for column_row_number to read The extra column is skipped when building truth_row.
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

/** Row ID, target coordinates, and target length returned by mmsearch. */
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

/** Runs mmsearch for each row's target and query, keeping complete alignment fields. */
inline std::vector<mmsearch_row> run_mmsearch(const connection_test_context& ctx,
                                              std::string_view table) {
    // Read the alignment helpers' native BIGINT results without casts.
    // The checks below compare these coordinates and lengths with generated truth.
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

// Allow endpoint differences for the mutations and gaps in generated queries.
constexpr long long k_position_tolerance = 50'000;

/** Checks endpoint tolerance and the exact reference  length. */
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
    // target_len must preserve the full reference length, including values above 32-bit range.
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

    // The fixed reference length guarantees a BIGINT check. Log query positions
    // to show which placements also exceed INT32_MAX.
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

    // Force the in-memory baseline, which packs the 4 Gbp target into one allocation.
    // Its matches must agree with the generated positions and the streaming run below.
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

    // Read the target from PostgreSQL fragments with the streaming scanner.
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

    // Sort by row ID and both,  target coordinates before comparing scanners.
    // A query can return several alignments for one row.
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
        // Both scanners must report the full 4 Gbp target length.
        REQUIRE(a.target_len == b.target_len);
        REQUIRE(a.target_len == k_reference_bp);
    }
}

TEST_CASE("6 Gbp sample table aligns within tolerance and target_len "
          "round-trips past UINT32_MAX with the PostgreSQL scanner",
          "[component:bioseq][layer:integration][feature:alignment][resource:postgres][resource:"
          "filesystem][resource:large_memory]") {
    // A 6 Gbp target exceeds the flat format's length and allocation limits.
    // Use the streaming scanner and check the full target length toether with
    // alignments near the generated query positions.
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
