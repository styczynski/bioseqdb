/**
 * @file index_mixed_test.cpp
 * @brief Verifies a scan containing indexed and unindexed rows.
 *
 * Indexed rows use stored assembly while unindexed rows use recomputation.
 */
#include <algorithm>
#include <set>
#include <string>
#include <tests/support/postgres.hpp>
import bioseq.tests.integration.support;

using namespace bioseq_int_tests;

namespace {
/** @brief Generates deterministic ACGT-only DNA with the fixture's xorshift stream. */
inline std::string generate_dna4(std::size_t length, std::uint64_t seed) {
    std::string sequence;
    sequence.reserve(length);
    std::uint64_t state = seed * 2862933555777941757ull + 3037000493ull;
    for (std::size_t index = 0; index < length; ++index) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        sequence.push_back("ACGT"[(state >> 11) & 3]);
    }
    return sequence;
}

/** @brief Inserts one deterministic target and its exact substring query. */
inline void insert_pair(const connection_test_context& ctx,
                        std::string_view table_name,
                        int primary_key,
                        std::uint64_t seed) {
    const std::string target = generate_dna4(40000, seed); // ~2 fragments
    const std::string query = target.substr(12000, 1500);
    exec_sqlf(ctx,
              "INSERT INTO {} VALUES ({}, '{}'::bioseq.seq('dna4',32), "
              "'{}'::bioseq.seq('dna4',32));",
              table_name,
              primary_key,
              target,
              query);
}

} // namespace

TEST_CASE("mixed indexed + unindexed rows in one scan",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({"mixed_index_rows"});
    exec_sql(ctx,
             "CREATE TABLE mixed_index_rows (pk int, target bioseq.seq('dna4',32), "
             "query bioseq.seq('dna4',32)) USING bioseq;");

    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = on;");
    insert_pair(ctx, "mixed_index_rows", 1, 11); // indexed on insert
    insert_pair(ctx, "mixed_index_rows", 2, 22); // indexed on insert
    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
    insert_pair(ctx, "mixed_index_rows", 3, 33); // Unindexed.
    insert_pair(ctx, "mixed_index_rows", 4, 44); // Unindexed.

    // Index-table row counts reflect only indexed rows.
    auto row_number_for = [&ctx](int primary_key) {
        return query_scalar_int(
            ctx,
            fmt::format("SELECT bioseq.column_row_number('mixed_index_rows'::regclass, ctid) FROM "
                        "mixed_index_rows WHERE pk={}",
                        primary_key));
    };
    REQUIRE(index_distinct_sub_keys(
                ctx, "minimizers_per_fragment", "mixed_index_rows", row_number_for(1)) >= 1);
    REQUIRE(index_distinct_sub_keys(
                ctx, "minimizers_per_fragment", "mixed_index_rows", row_number_for(2)) >= 1);
    REQUIRE(index_distinct_sub_keys(
                ctx, "minimizers_per_fragment", "mixed_index_rows", row_number_for(3)) == 0);
    REQUIRE(index_distinct_sub_keys(
                ctx, "minimizers_per_fragment", "mixed_index_rows", row_number_for(4)) == 0);

    // With sequence-index reads enabled, mmsearch returns matches for both
    // indexed (assemble) and unindexed (recompute) rows.
    auto find_primary_keys = [&ctx](const char* sequence_read_value) {
        guc_scope sequence_read_scope(
            ctx, "bioseq.index.sequence_read.enabled", sequence_read_value);
        const auto matching_rows = query_rows(
            ctx, "SELECT pk FROM mixed_index_rows WHERE mmsearch(target, query) ORDER BY pk");
        std::set<long long> primary_keys;
        for (const auto& row : matching_rows) {
            if (row[0]) {
                primary_keys.insert(std::stoll(*row[0]));
            }
        }
        return primary_keys;
    };
    const auto with_index_reads = find_primary_keys("on");
    const auto without_index_reads = find_primary_keys("off");
    REQUIRE(with_index_reads.size() == 4); // all 4 rows found (mixed assemble + recompute)
    REQUIRE(
        std::includes(with_index_reads.begin(),
                      with_index_reads.end(),
                      without_index_reads.begin(),
                      without_index_reads.end())); // Enabled reads include disabled-read results.
}
