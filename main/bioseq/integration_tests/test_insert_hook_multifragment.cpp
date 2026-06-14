// DOC0279 §16.2 — the INSERT-time in-RAM build hook on a MULTI-fragment value.
// A normal INSERT of a ~200 kbp dna4 sequence (~7 fragments at 32768 bp) must build
// one sub_key per fragment (the /D11 geometric-nfrag fix, NOT "first fragment only"),
// build BOTH default on-insert indexes (minimizers_per_fragment + symbol_counts) from
// the single insert, and produce mmsearch results byte-identical to the recompute
// path (dna4, single whole-sequence segment => exact multiplicity).
#include "common_tests_integration.h"
#include <set>
#include <string>
#include <tuple>
import integration_tests.common;

using namespace bioseq_int_tests;

namespace {
// Deterministic pseudo-random dna4 string (xorshift; ACGT only, no N).
inline std::string gen_dna4(std::size_t n, std::uint64_t seed) {
    std::string s;
    s.reserve(n);
    std::uint64_t x = seed * 2862933555777941757ull + 3037000493ull;
    for (std::size_t i = 0; i < n; ++i) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        s.push_back("ACGT"[(x >> 9) & 3]);
    }
    return s;
}
inline std::set<std::tuple<long long, long long>> match_set(
        const connection_test_context& ctx, std::string_view tbl) {
    auto rows = query_rows(ctx, fmt::format(
        "SELECT bioseq.target_start(target) ts, bioseq.target_end(target) te FROM {} "
        "WHERE mmsearch(target, query) ORDER BY 1,2", tbl));
    std::set<std::tuple<long long, long long>> out;
    for (auto& r : rows)
        if (r[0] && r[1]) out.emplace(std::stoll(*r[0]), std::stoll(*r[1]));
    return out;
}
}  // namespace

TEST_CASE("DOC0279 INSERT hook builds a multi-fragment index + both on-insert indexes",
          "[indexing][insert]") {
    auto ctx = fresh_context({"doc0279_ih"});
    exec_sql(ctx, "SET bioseq.update_index_on_insert = on;");
    exec_sql(ctx,
        "CREATE TABLE doc0279_ih (pk int, target bioseq.seq('dna4',32), "
        "query bioseq.seq('dna4',32)) USING bioseq;");
    const std::string tgt = gen_dna4(200000, 1234567);   // ~7 fragments @ 32768 bp
    const std::string qry = tgt.substr(60000, 2000);      // an exact 2 kbp substring
    exec_sqlf(ctx, "INSERT INTO doc0279_ih VALUES (1, '{}'::bioseq.seq('dna4',32), "
                   "'{}'::bioseq.seq('dna4',32));", tgt, qry);

    const long long rn = query_scalar_int(ctx,
        "SELECT bioseq.column_row_number('doc0279_ih'::regclass, ctid) FROM doc0279_ih LIMIT 1");
    // /D11: one sub_key per fragment, NOT just the first. ceil(200000/32768) = 7.
    REQUIRE(index_distinct_sub_keys(ctx, "minimizers_per_fragment", "doc0279_ih", rn) >= 7);
    // BOTH default on-insert indexes materialise from the single insert.
    REQUIRE(index_table_row_count(ctx, "minimizers_per_fragment", "doc0279_ih") > 0);
    REQUIRE(index_table_row_count(ctx, "symbol_counts", "doc0279_ih") > 0);

    // dna4, single whole-sequence segment => assemble == recompute exactly.
    guc_scope on(ctx, "bioseq.read_sequence_index", "on");
    const auto assembled = match_set(ctx, "doc0279_ih");
    REQUIRE(!assembled.empty());
    exec_sql(ctx, "SET bioseq.read_sequence_index = off;");
    const auto recomputed = match_set(ctx, "doc0279_ih");
    REQUIRE(assembled == recomputed);
}

TEST_CASE("DOC0279 a manual symbol_counts policy still gives the correct fallback tally",
          "[indexing][insert]") {
    auto ctx = fresh_context({"doc0279_ih2"});
    exec_sql(ctx, "CREATE TABLE doc0279_ih2 (pk int, seq0 bioseq.seq('dna4',32)) USING bioseq;");
    exec_sql(ctx, "SELECT bioseq.set_index_policy('doc0279_ih2'::regclass, 'symbol_counts', 'UPDATE_MANUALLY');");
    exec_sql(ctx, "SET bioseq.update_index_on_insert = on;");
    exec_sql(ctx, "INSERT INTO doc0279_ih2 VALUES (1, 'AAAACCCCGGGGTTTT'::bioseq.seq('dna4',32));");
    // per-fragment chunks materialise; symbol_counts does NOT (manual policy).
    REQUIRE(index_table_row_count(ctx, "minimizers_per_fragment", "doc0279_ih2") > 0);
    REQUIRE(index_table_row_count(ctx, "symbol_counts", "doc0279_ih2") == 0);
    // symbol_count still returns the correct real tally via the unconditional recompute fallback.
    guc_scope off(ctx, "bioseq.read_sequence_index", "off");
    REQUIRE(query_scalar_int(ctx,
        "SELECT bioseq.symbol_count(seq0,'A') FROM doc0279_ih2 WHERE pk=1") == 4);
    REQUIRE(query_scalar_int(ctx,
        "SELECT bioseq.symbol_count(seq0,'T') FROM doc0279_ih2 WHERE pk=1") == 4);
}
