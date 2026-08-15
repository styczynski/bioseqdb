// One batch contains both indexed and unindexed rows. Indexed rows use the
// assembly path while unindexed rows
// fall back to recompute, in the same scan.
#include <tests/support/postgres.hpp>
#include <algorithm>
#include <set>
#include <string>
import bioseq.tests.integration.support;

using namespace bioseq_int_tests;

namespace {
inline std::string gen_dna4(std::size_t n, std::uint64_t seed) {
    std::string s;
    s.reserve(n);
    std::uint64_t x = seed * 2862933555777941757ull + 3037000493ull;
    for (std::size_t i = 0; i < n; ++i) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        s.push_back("ACGT"[(x >> 11) & 3]);
    }
    return s;
}
inline void insert_pair(
    const connection_test_context& ctx,
    std::string_view table_name,
    int pk, std::uint64_t seed) {
    const std::string target = gen_dna4(40000, seed); // ~2 fragments
    const std::string query = target.substr(12000, 1500);
    exec_sqlf(ctx, "INSERT INTO {} VALUES ({}, '{}'::bioseq.seq('dna4',32), "
                   "'{}'::bioseq.seq('dna4',32));",
              table_name, pk, target, query);
}

}  // namespace

TEST_CASE("mixed indexed + unindexed rows in one scan",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({"mixed_index_rows"});
    exec_sql(ctx, "CREATE TABLE mixed_index_rows (pk int, target bioseq.seq('dna4',32), "
                  "query bioseq.seq('dna4',32)) USING bioseq;");

    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = on;");
    insert_pair(ctx, "mixed_index_rows", 1, 11);    // indexed on insert
    insert_pair(ctx, "mixed_index_rows", 2, 22);    // indexed on insert
    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
    insert_pair(ctx, "mixed_index_rows", 3, 33);    // NOT indexed
    insert_pair(ctx, "mixed_index_rows", 4, 44);    // NOT indexed

    // Index-table row counts reflect only indexed rows.
    auto rowno_of = [&ctx](int pk) {
        return query_scalar_int(ctx, fmt::format(
            "SELECT bioseq.column_row_number('mixed_index_rows'::regclass, ctid) FROM mixed_index_rows WHERE pk={}", pk));
    };
    REQUIRE(index_distinct_sub_keys(ctx, "minimizers_per_fragment", "mixed_index_rows", rowno_of(1)) >= 1);
    REQUIRE(index_distinct_sub_keys(ctx, "minimizers_per_fragment", "mixed_index_rows", rowno_of(2)) >= 1);
    REQUIRE(index_distinct_sub_keys(ctx, "minimizers_per_fragment", "mixed_index_rows", rowno_of(3)) == 0);
    REQUIRE(index_distinct_sub_keys(ctx, "minimizers_per_fragment", "mixed_index_rows", rowno_of(4)) == 0);

    // With sequence-index reads enabled, mmsearch returns matches for both
    // indexed (assemble) and unindexed (recompute) rows.
    auto found_pks = [&ctx](const char* sequence_read_value) {
        guc_scope sequence_read_scope(
            ctx, "bioseq.index.sequence_read.enabled", sequence_read_value);
        auto rows = query_rows(ctx,
            "SELECT pk FROM mixed_index_rows WHERE mmsearch(target, query) ORDER BY pk");
        std::set<long long> out;
        for (auto& r : rows) if (r[0]) out.insert(std::stoll(*r[0]));
        return out;
    };
    const auto on  = found_pks("on");
    const auto off = found_pks("off");
    REQUIRE(on.size() == 4);                 // all 4 rows found (mixed assemble + recompute)
    REQUIRE(std::includes(on.begin(), on.end(), off.begin(), off.end()));  // on superset of off
}
