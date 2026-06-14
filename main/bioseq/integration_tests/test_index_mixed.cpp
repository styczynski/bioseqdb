// DOC0279 §16.2 — one batch with BOTH indexed and unindexed rows (the query_key/rowno
// silent-failure class, §15). Indexed rows go through the assemble path, unindexed rows
// fall back to recompute, in the same scan.
#include "common_tests_integration.h"
#include <algorithm>
#include <set>
#include <string>
#include <tuple>
import integration_tests.common;

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
inline void insert_pair(const connection_test_context& ctx, std::string_view tbl,
                        int pk, std::uint64_t seed) {
    const std::string tgt = gen_dna4(40000, seed);          // ~2 fragments
    const std::string qry = tgt.substr(12000, 1500);
    exec_sqlf(ctx, "INSERT INTO {} VALUES ({}, '{}'::bioseq.seq('dna4',32), "
                   "'{}'::bioseq.seq('dna4',32));", tbl, pk, tgt, qry);
}
}  // namespace

TEST_CASE("DOC0279 mixed indexed + unindexed rows in one scan",
          "[indexing][mixed]") {
    auto ctx = fresh_context({"doc0279_mx"});
    exec_sql(ctx, "CREATE TABLE doc0279_mx (pk int, target bioseq.seq('dna4',32), "
                  "query bioseq.seq('dna4',32)) USING bioseq;");

    exec_sql(ctx, "SET bioseq.update_index_on_insert = on;");
    insert_pair(ctx, "doc0279_mx", 1, 11);    // indexed on insert
    insert_pair(ctx, "doc0279_mx", 2, 22);    // indexed on insert
    exec_sql(ctx, "SET bioseq.update_index_on_insert = off;");
    insert_pair(ctx, "doc0279_mx", 3, 33);    // NOT indexed
    insert_pair(ctx, "doc0279_mx", 4, 44);    // NOT indexed

    // (a) index_table_row_count reflects only the indexed rows.
    auto rowno_of = [&](int pk) {
        return query_scalar_int(ctx, fmt::format(
            "SELECT bioseq.column_row_number('doc0279_mx'::regclass, ctid) FROM doc0279_mx WHERE pk={}", pk));
    };
    REQUIRE(index_distinct_sub_keys(ctx, "minimizers_per_fragment", "doc0279_mx", rowno_of(1)) >= 1);
    REQUIRE(index_distinct_sub_keys(ctx, "minimizers_per_fragment", "doc0279_mx", rowno_of(2)) >= 1);
    REQUIRE(index_distinct_sub_keys(ctx, "minimizers_per_fragment", "doc0279_mx", rowno_of(3)) == 0);
    REQUIRE(index_distinct_sub_keys(ctx, "minimizers_per_fragment", "doc0279_mx", rowno_of(4)) == 0);

    // (b) mmsearch with read_sequence_index=on returns matches for BOTH indexed (assemble)
    //     and unindexed (recompute) rows.
    auto found_pks = [&](const char* rsi) {
        guc_scope g(ctx, "bioseq.read_sequence_index", rsi);
        auto rows = query_rows(ctx,
            "SELECT pk FROM doc0279_mx WHERE mmsearch(target, query) ORDER BY pk");
        std::set<long long> out;
        for (auto& r : rows) if (r[0]) out.insert(std::stoll(*r[0]));
        return out;
    };
    const auto on  = found_pks("on");
    const auto off = found_pks("off");
    REQUIRE(on.size() == 4);                 // all 4 rows found (mixed assemble + recompute)
    REQUIRE(std::includes(on.begin(), on.end(), off.begin(), off.end()));  // on superset of off
}
