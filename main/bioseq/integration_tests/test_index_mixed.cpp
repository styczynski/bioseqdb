// DOC0279 §16.2 — one batch with BOTH indexed and unindexed rows (the query_key/rowno
// silent-failure class, §15). Indexed rows go through the assemble path, unindexed rows
// fall back to recompute, in the same scan.
#include "common_tests_integration.h"
#include <algorithm>
#include <set>
#include <string>
#include <tuple>
import bioseq.integration_tests.common;

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

inline void insert_broadcast_target(
        const connection_test_context& ctx,
        std::string_view tbl,
        int pk,
        std::uint64_t seed,
        std::string_view shared_query,
        std::size_t target_size = 40000,
        std::size_t query_offset = 12000) {
    std::string target = gen_dna4(target_size, seed);
    target.replace(query_offset, shared_query.size(), shared_query);
    exec_sqlf(ctx,
        "INSERT INTO {} VALUES ({}, '{}'::bioseq.seq('dna4',32));",
        tbl,
        pk,
        target);
}
}  // namespace

TEST_CASE("DOC0279 mixed indexed + unindexed rows in one scan",
          "[indexing][mixed]") {
    auto ctx = fresh_context({
        "doc0279_mx",
        "doc0279_broadcast_mx",
        "doc0279_broadcast_owner"});
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

    SECTION("broadcast query proof is attached only to indexed consumers and accounted once") {
        exec_sql(ctx,
            "CREATE TABLE doc0279_broadcast_mx "
            "(pk int, target bioseq.seq('dna4',32)) USING bioseq;");
        exec_sql(ctx,
            "CREATE TABLE doc0279_broadcast_owner "
            "(pk int, target bioseq.seq('dna4',32)) USING bioseq;");
        exec_sql(ctx, "SET max_parallel_workers_per_gather = 0;");
        exec_sql(ctx, "SET bioseq.read_sequence_index = on;");
        exec_sql(ctx, "SET bioseq.recompute_async_persist = off;");
        exec_sql(ctx, "SET bioseq.aligner.vulkan_enabled = off;");
        exec_sql(ctx,
            "SET bioseq.aligner.minimizer_algorithm = 'paper_nthash_simd';");
        exec_sql(ctx,
            "SET bioseq.aligner.chaining_algorithm = 'simd_mm2';");
        exec_sql(ctx,
            "SET bioseq.aligner.base_alignment_algorithm = 'block_aligner';");

        std::string shared_query = gen_dna4(1500, 7001);
        for (std::size_t i = 2; i < shared_query.size(); ++i) {
            if (shared_query[i] == shared_query[i - 1]
                && shared_query[i] == shared_query[i - 2]) {
                shared_query[i] = shared_query[i] == 'A' ? 'C' : 'A';
            }
        }

        // Legacy MPF has no persisted minimizer-scheme identity. Paper must
        // fail as an ordinary PostgreSQL error until the scheme-tagged READY
        // writer/reader cut over together; it may never abort the backend or
        // silently build an Exact/Scalar index instead.
        exec_sql(ctx, "SET bioseq.update_index_on_insert = on;");
        const auto paper_persist_error = sql_error(ctx, fmt::format(
            "INSERT INTO doc0279_broadcast_owner VALUES "
            "(1, '{}'::bioseq.seq('dna4',32));",
            std::string(3000, 'A')));
        REQUIRE(paper_persist_error.contains(
            "paper_nthash_simd persistence requires the scheme-tagged "
            "READY-generation format"));
        REQUIRE(paper_persist_error.sqlstate == "0A000");
        REQUIRE(query_scalar_int(ctx,
            "SELECT count(*) FROM doc0279_broadcast_owner;") == 0);
        REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);

        // Exercise the shared-query proof cache with the matching live Exact
        // persisted scheme. The first persisted consumer constructs a valid
        // query proof but has no common 15-mer and cannot be an accepted SIMD
        // alignment job. It still accounts the physical sketch exactly once
        // as the first proof-valid consumer; the second persisted consumer
        // borrows the proof without charging the operation again.
        exec_sql(ctx,
            "SET bioseq.aligner.minimizer_algorithm = 'two_stack_nthash';");
        exec_sqlf(ctx,
            "INSERT INTO doc0279_broadcast_owner VALUES "
            "(1, '{}'::bioseq.seq('dna4',32));",
            std::string(3000, 'A'));
        insert_broadcast_target(
            ctx, "doc0279_broadcast_owner", 2, 202, shared_query,
            /*target_size=*/3000,
            /*query_offset=*/1000);

        // A pre-existing Exact legacy row is not compatible evidence for a
        // Paper query. `allow` must recompute with Paper and must not consume
        // or relabel the v2 payload; strict persisted mode is rejected earlier
        // until READY identity is production-wired.
        exec_sql(ctx,
            "SET bioseq.aligner.minimizer_algorithm = 'paper_nthash_simd';");
        const auto paper_recompute_rows = query_int_column(ctx, fmt::format(
            "SELECT pk FROM doc0279_broadcast_owner "
            "WHERE mmsearch(target, '{}'::bioseq.seq('dna4',32)) "
            "ORDER BY pk;",
            shared_query));
        REQUIRE(paper_recompute_rows == std::vector<long long>{2});
        const auto paper_recompute_stats = query_rows(ctx,
            "SELECT persisted_index_rows, recomputed_target_index_rows "
            "FROM bioseq.last_alignment_execution_stats() LIMIT 1;");
        REQUIRE(paper_recompute_stats.size() == 1);
        REQUIRE(paper_recompute_stats.front()[0].value_or("") == "0");
        REQUIRE(paper_recompute_stats.front()[1].value_or("") == "2");

        // The cached-proof assertions below require a complete selected SIMD
        // tuple, including native Block Aligner execution. Merely projecting
        // pk lets PostgreSQL prune CIGAR/alignment work, so explicitly project
        // and validate has_cigar for every returned mapping.
        const auto search_with_cigar = [&](std::string_view table_name) {
            const auto rows = query_rows(ctx, fmt::format(
                "SELECT pk, bioseq.has_cigar(pk) FROM {} "
                "WHERE mmsearch(target, '{}'::bioseq.seq('dna4',32)) "
                "ORDER BY pk;",
                table_name,
                shared_query));
            std::vector<long long> pks;
            pks.reserve(rows.size());
            for (const auto& row : rows) {
                REQUIRE(row.size() == 2);
                REQUIRE(row[0].has_value());
                REQUIRE(row[1].value_or("") == "t");
                pks.push_back(std::stoll(*row[0]));
            }
            return pks;
        };

        exec_sql(ctx,
            "SET bioseq.aligner.minimizer_algorithm = 'two_stack_nthash';");
        const auto owner_rows =
            search_with_cigar("doc0279_broadcast_owner");
        REQUIRE(owner_rows == std::vector<long long>{2});
        const auto owner_stats = query_rows(ctx,
            "SELECT nthash_vector_batches, simd_chain_vector_batches, "
            "simd_chain_backtrack_vector_batches, persisted_index_rows, "
            "attempted_alignment_jobs, fully_attested_alignment_jobs, "
            "unattested_alignment_jobs "
            "FROM bioseq.last_alignment_execution_stats() LIMIT 1;");
        REQUIRE(owner_stats.size() == 1);
        REQUIRE(std::stoll(owner_stats.front()[0].value_or("0")) > 0);
        REQUIRE(std::stoll(owner_stats.front()[1].value_or("0")) > 0);
        REQUIRE(std::stoll(owner_stats.front()[2].value_or("0")) > 0);
        REQUIRE(owner_stats.front()[3].value_or("") == "2");
        REQUIRE(owner_stats.front()[4].value_or("") == "2");
        // Statement attestation is deliberately all-or-nothing. The first
        // no-chain job is unattested, so the later algorithm-accepted borrower
        // cannot leave a partial fully_attested count. nthash_vector_batches
        // proves the shared query sketch was accounted once; the separate
        // chain/backtrack counters prove a borrower completed the full SIMD
        // downstream route after has_cigar forced Block Aligner execution.
        REQUIRE(owner_stats.front()[5].value_or("") == "0");
        REQUIRE(owner_stats.front()[6].value_or("") == "2");

        // Keep the mixed targets below the 4096-bp opportunistic-index
        // threshold: an unindexed row must enter the ordinary recompute sketch
        // path, not legitimately consume ensure_q through a transient
        // query-filtered index. This is the persisted-then-recompute stale-
        // proof regression.
        exec_sql(ctx, "SET bioseq.update_index_on_insert = on;");
        insert_broadcast_target(
            ctx, "doc0279_broadcast_mx", 1, 101, shared_query,
            /*target_size=*/3000,
            /*query_offset=*/1000);
        exec_sql(ctx, "SET bioseq.update_index_on_insert = off;");
        insert_broadcast_target(
            ctx, "doc0279_broadcast_mx", 3, 103, shared_query,
            /*target_size=*/3000,
            /*query_offset=*/1000);
        insert_broadcast_target(
            ctx, "doc0279_broadcast_mx", 4, 104, shared_query,
            /*target_size=*/3000,
            /*query_offset=*/1000);

        const auto broadcast_search = [&] {
            return search_with_cigar("doc0279_broadcast_mx");
        };
        const auto vector_batches = [&] {
            return query_scalar_int(ctx,
                "SELECT nthash_vector_batches "
                "FROM bioseq.last_alignment_execution_stats() LIMIT 1;");
        };
        const auto source_rows = [&] {
            return query_rows(ctx,
                "SELECT persisted_index_rows, missing_persisted_index_rows, "
                "recomputed_target_index_rows, attempted_alignment_jobs, "
                "fully_attested_alignment_jobs, unattested_alignment_jobs, "
                "actual_backend "
                "FROM bioseq.last_alignment_execution_stats() LIMIT 1;");
        };

        const auto first = broadcast_search();
        REQUIRE(first == std::vector<long long>{1, 3, 4});
        const long long first_vector_batches = vector_batches();
        REQUIRE(first_vector_batches > 0);
        const auto first_sources = source_rows();
        REQUIRE(first_sources.size() == 1);
        REQUIRE(first_sources.front()[0].value_or("") == "1");
        REQUIRE(first_sources.front()[1].value_or("") == "2");
        REQUIRE(first_sources.front()[2].value_or("") == "2");
        REQUIRE(first_sources.front()[3].value_or("") == "3");
        // Persisted generation/source identity is not published by the legacy
        // reader yet, so one persisted row makes the aggregate fail closed even
        // though every algorithm route below is SIMD-accepted.
        REQUIRE(first_sources.front()[4].value_or("") == "0");
        REQUIRE(first_sources.front()[5].value_or("") == "3");
        REQUIRE(first_sources.front()[6].value_or("") == "simd_cpu");

        // Add one more persisted consumer after the mixed run. It must carry
        // the cached query proof for per-job route acceptance, but it did not
        // execute another query sketch operation and therefore cannot increase
        // nthash_vector_batches on the next otherwise-identical scan.
        exec_sql(ctx, "SET bioseq.update_index_on_insert = on;");
        insert_broadcast_target(
            ctx, "doc0279_broadcast_mx", 2, 102, shared_query,
            /*target_size=*/3000,
            /*query_offset=*/1000);

        const auto second = broadcast_search();
        REQUIRE(second == std::vector<long long>{1, 2, 3, 4});
        REQUIRE(vector_batches() == first_vector_batches);
        const auto second_sources = source_rows();
        REQUIRE(second_sources.size() == 1);
        REQUIRE(second_sources.front()[0].value_or("") == "2");
        REQUIRE(second_sources.front()[1].value_or("") == "2");
        REQUIRE(second_sources.front()[2].value_or("") == "2");
        REQUIRE(second_sources.front()[3].value_or("") == "4");
        REQUIRE(second_sources.front()[4].value_or("") == "0");
        REQUIRE(second_sources.front()[5].value_or("") == "4");
        REQUIRE(second_sources.front()[6].value_or("") == "simd_cpu");
    }
}
