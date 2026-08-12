// The insert-time in-memory build hook handles multi-fragment values.
// A normal INSERT of a ~200 kbp dna4 sequence (~7 fragments at 32768 bp) must build
// one sub_key per fragment rather than indexing only the first fragment,
// build BOTH default on-insert indexes (minimizers_per_fragment + symbol_counts) from
// the single insert, and produce mmsearch results byte-identical to the recompute
// path (dna4, single whole-sequence segment => exact multiplicity).
#include <tests/support/postgres.hpp>
#include <set>
#include <string>
#include <tuple>
import bioseq.tests.integration.support;

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
        const connection_test_context& ctx, std::string_view table_name) {
    auto rows = query_rows(ctx, fmt::format(
        "SELECT bioseq.target_start(target) ts, bioseq.target_end(target) te FROM {} "
        "WHERE mmsearch(target, query) ORDER BY 1,2", table_name));
    std::set<std::tuple<long long, long long>> out;
    for (auto& r : rows)
        if (r[0] && r[1]) out.emplace(std::stoll(*r[0]), std::stoll(*r[1]));
    return out;
}
}  // namespace

TEST_CASE("INSERT hook builds a multi-fragment index + both on-insert indexes",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({"insert_hook_multifragment"});
    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = on;");
    exec_sql(ctx,
        "CREATE TABLE insert_hook_multifragment (pk int, target bioseq.seq('dna4',32), "
        "query bioseq.seq('dna4',32)) USING bioseq;");
    const std::string target = gen_dna4(200000, 1234567); // ~7 fragments @ 32768 bp
    const std::string query = target.substr(60000, 2000); // an exact 2 kbp substring
    exec_sqlf(ctx, "INSERT INTO insert_hook_multifragment VALUES (1, '{}'::bioseq.seq('dna4',32), "
                   "'{}'::bioseq.seq('dna4',32));", target, query);

    const long long sequence_rowno = query_scalar_int(ctx,
        "SELECT bioseq.column_row_number('insert_hook_multifragment'::regclass, ctid) FROM insert_hook_multifragment LIMIT 1");
    // One sub_key is written per fragment: ceil(200000/32768) = 7.
    REQUIRE(index_distinct_sub_keys(
        ctx, "minimizers_per_fragment", "insert_hook_multifragment",
        sequence_rowno) >= 7);
    // BOTH default on-insert indexes materialise from the single insert.
    REQUIRE(index_table_row_count(ctx, "minimizers_per_fragment", "insert_hook_multifragment") > 0);
    REQUIRE(index_table_row_count(ctx, "symbol_counts", "insert_hook_multifragment") > 0);

    // dna4, single whole-sequence segment => assemble == recompute exactly.
    guc_scope on(ctx, "bioseq.index.sequence_read.enabled", "on");
    const auto assembled = match_set(ctx, "insert_hook_multifragment");
    REQUIRE(!assembled.empty());
    exec_sql(ctx, "SET bioseq.index.sequence_read.enabled = off;");
    const auto recomputed = match_set(ctx, "insert_hook_multifragment");
    REQUIRE(assembled == recomputed);
}

TEST_CASE("a manual symbol_counts policy still gives the correct fallback tally",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({"insert_hook_manual_symbol_counts"});
    exec_sql(ctx, "CREATE TABLE insert_hook_manual_symbol_counts (pk int, seq0 bioseq.seq('dna4',32)) USING bioseq;");
    exec_sql(ctx, "SELECT bioseq.set_index_policy('insert_hook_manual_symbol_counts'::regclass, 'symbol_counts', 'UPDATE_MANUALLY');");
    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = on;");
    exec_sql(ctx, "INSERT INTO insert_hook_manual_symbol_counts VALUES (1, 'AAAACCCCGGGGTTTT'::bioseq.seq('dna4',32));");
    // per-fragment chunks materialise; symbol_counts does NOT (manual policy).
    REQUIRE(index_table_row_count(ctx, "minimizers_per_fragment", "insert_hook_manual_symbol_counts") > 0);
    REQUIRE(index_table_row_count(ctx, "symbol_counts", "insert_hook_manual_symbol_counts") == 0);
    // symbol_count still returns the correct real tally via the unconditional
    // recompute fallback. First isolate row admission from projection: the
    // scalar and vector executors must see the same freshly inserted pk=1 row,
    // and EXPLAIN ANALYZE must prove that chunk pruning retained its only chunk.
    guc_scope sequence_read_off(
        ctx, "bioseq.index.sequence_read.enabled", "off");
    ctx.clear_notices();
    constexpr std::string_view row_count_query =
        "SELECT count(*) FROM insert_hook_manual_symbol_counts WHERE pk=1";
    {
        guc_scope vectorization_off(
            ctx, "bioseq.vectorization.enabled", "off");
        REQUIRE(query_scalar_int(ctx, row_count_query) == 1);
    }

    {
        guc_scope vectorization_on(
            ctx, "bioseq.vectorization.enabled", "on");
        const auto plan = explain_text(ctx, row_count_query);
        std::string plan_text;
        for (const auto& line : plan) {
            plan_text += line;
            plan_text += '\n';
        }
        INFO(plan_text);
        REQUIRE(plan_contains(plan, "BioseqScan"));
        REQUIRE(plan_contains(plan, "Bioseq Vectorized Filter"));
        REQUIRE(find_explain_property(
                    plan, "Bioseq Chunk Groups Removed by Filter")
                == "0");
        REQUIRE(query_scalar_int(ctx, row_count_query) == 1);
    }

    REQUIRE(query_scalar_int(ctx,
        "SELECT bioseq.symbol_count(seq0,'A') FROM insert_hook_manual_symbol_counts WHERE pk=1") == 4);
    REQUIRE(query_scalar_int(ctx,
        "SELECT bioseq.symbol_count(seq0,'T') FROM insert_hook_manual_symbol_counts WHERE pk=1") == 4);
    // Repeated calls must retain one copy of the sequence relation and
    // registered snapshot in the ResourceOwner.
    REQUIRE_FALSE(ctx.notice_contains("resource was not closed"));
}
