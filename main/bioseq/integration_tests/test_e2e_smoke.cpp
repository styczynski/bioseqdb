/**
 * @file test_e2e_smoke.cpp
 * @brief DOC0286 §13 Part 2 — fast E2E smoke for both dna4 AND dna5 (with N) at a
 *        tiny size. Drives the same SQL flow as bioseq_bench_e2e through
 *        common.cppm: generate -> write FASTA -> bioseq.load -> build
 *        minimizers_per_sequence index -> SELECT mmsearch, and cross-checks each
 *        query against minimap2. Asserts every query maps within
 *        kPositionToleranceBp of truth on BOTH engines (so a fast-but-empty/
 *        mis-mapped dna5 result can never pass). The dna5 path exercises the nt4
 *        normalization (N resets, T valid) end-to-end through the SQL surface.
 */
#include "common_simd.h"
#include <tests/support/common.hpp>
#include <bioseq/pg_build_profile.h>
#include "common.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

import bioseq.common.mlogging;
import bioseq.common.sequan_compat;
import bioseq.sequences;
import bioseq.sequences.generation;
import bioseq.integration_tests.common;

namespace {

namespace it = bioseq_int_tests;
namespace gen = bioseq::sequences::generation;

constexpr long long kPositionToleranceBp = 50;

void write_fasta(const std::string& path, std::string_view header,
                 const std::string& body) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << '>' << header << '\n';
    for (std::size_t i = 0; i < body.size(); i += 60) {
        f << body.substr(i, 60) << '\n';
    }
}

// minimap2 PAF target-start of the primary (or widest) alignment. Single-record
// target (no split), so PAF col 7 is already the absolute coordinate. Returns
// -1 if nothing maps.
[[nodiscard]] long long run_minimap2_target_start(const std::string& target_fa,
                                                  const std::string& query_fa) {
    const std::string cmd =
        fmt::format("minimap2 -c -t1 -k15 -w10 {} {} 2>/dev/null", target_fa, query_fa);
    std::FILE* pipe = ::popen(cmd.c_str(), "r");
    if (pipe == nullptr) return -1;
    std::string out;
    std::array<char, 1 << 16> buf;
    std::size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), pipe)) > 0) out.append(buf.data(), n);
    ::pclose(pipe);

    long long best_start = -1, best_span = -1;
    std::size_t pos = 0;
    while (pos < out.size()) {
        const std::size_t eol = out.find('\n', pos);
        const std::string_view line{out.data() + pos,
            (eol == std::string::npos ? out.size() : eol) - pos};
        pos = (eol == std::string::npos) ? out.size() : eol + 1;
        if (line.empty()) continue;
        std::vector<std::string_view> col;
        std::size_t cs = 0;
        for (std::size_t i = 0; i <= line.size(); ++i)
            if (i == line.size() || line[i] == '\t') { col.emplace_back(line.substr(cs, i - cs)); cs = i + 1; }
        if (col.size() < 9) continue;
        long long rstart = 0, rend = 0;
        std::from_chars(col[7].data(), col[7].data() + col[7].size(), rstart);
        std::from_chars(col[8].data(), col[8].data() + col[8].size(), rend);
        const long long span = rend - rstart;
        const bool primary = (line.find("tp:A:P") != std::string_view::npos);
        if (primary || span > best_span) { best_span = primary ? (1LL << 62) : span; best_start = rstart; }
    }
    return best_start;
}

template <sequence_alphabet A>
void run_smoke(const std::string& alpha_name, const std::string& seq_type,
               const std::string& table) {
    gen::generator_params gp{};
    gp.reference_length       = 300'000;
    gp.fragment_count         = 4;            // several loci (TESTING-03)
    gp.fragment_length_mean   = 5'000;        // long N-free runs (dna5 ref is ~1% N)
    gp.fragment_length_stddev = 200.0;
    gp.substitution_rate      = 0.001;
    gp.insertion_rate         = 0.0005;
    gp.deletion_rate          = 0.0005;
    gp.reverse_complement_rate = 0.0;         // forward-only -> deterministic positions
    gp.seed                   = 20260286;

    auto pairs = gen::alphabet_sequence_generator<A, std::uint64_t>(gp)
                     .generate_sequences_pairs(gp.fragment_count);
    ASSERT(pairs.size() == gp.fragment_count, "generator under-produced", pairs.size());
    const std::string target_bases = pairs.front().target->to_string();
    if (alpha_name == "dna5") {
        // The dna5 generator must actually produce N (else the nt4 path is untested).
        ASSERT(target_bases.find('N') != std::string::npos,
               "dna5 smoke target unexpectedly contains no N", target_bases.size());
    }

    const std::string target_fa = fmt::format("/tmp/e2e_smoke_{}_target.fasta", alpha_name);
    write_fasta(target_fa, "ref", target_bases);

    it::connection_test_context ctx;
    it::reset_extension(ctx, { std::string_view{table} });
    it::exec_sql(ctx, "SET bioseq.aligner.base_alignment_algorithm = 'block_aligner';");
    it::exec_sql(ctx, "SET bioseq.aligner.chaining_algorithm       = 'simd_mm2';");
    it::exec_sql(ctx, "SET bioseq.aligner.minimizer_algorithm      = 'two_stack_nthash';");
    // §13 / D7 — verify the minimizer-algorithm GUC is actually applied end-to-end
    // (the enum GUC → Minimap2Params::minimizer_algorithm plumbing). dna4 and
    // dna5 both run the checked ExactSimd two-stack kernel; dna5 is normalized
    // to nt4 in vectors and N performs minimap2's continuous l=0/max-slot
    // transition while the existing minimizer ring keeps advancing.
    REQUIRE(it::query_scalar(ctx, "SHOW bioseq.aligner.minimizer_algorithm") == "two_stack_nthash");
    it::exec_sqlf(ctx, "CREATE TABLE {} (header text, seq {}) USING bioseq;", table, seq_type);
    it::exec_sql(ctx, "SET bioseq.update_index_on_insert = on;");
    it::exec_sqlf(ctx, "SELECT bioseq.load('{}'::regclass, '{}');", table, target_fa);
    it::exec_sqlf(ctx, "SELECT bioseq.update_index('{}'::regclass, 'minimizers_per_sequence');", table);
    it::exec_sqlf(ctx, "SELECT bioseq.set_active_read_index('{}'::regclass, 'minimizers_per_sequence');", table);
    it::exec_sql(ctx, "SET bioseq.read_sequence_index = on;");

    for (std::size_t i = 0; i < pairs.size(); ++i) {
        const std::string query = pairs[i].query.to_string();
        const long long truth = static_cast<long long>(pairs[i].truth_target_start);
        const std::string query_fa = fmt::format("/tmp/e2e_smoke_{}_q{}.fasta", alpha_name, i);
        write_fasta(query_fa, fmt::format("q{}", i), query);

        // bioseq: assemble-on mmsearch must map within tolerance of truth.
        const auto rows = it::query_rows(ctx, fmt::format(
            "SELECT bioseq.target_start(header) FROM {} WHERE mmsearch(seq, '{}'::{}) "
            "ORDER BY header", table, query, seq_type));
        ASSERT(!rows.empty() && rows[0][0].has_value(),
               fmt::format("{} query {} did not map via bioseq mmsearch", alpha_name, i),
               i);
        const long long bioseq_start = std::stoll(rows[0][0].value());
        ASSERT(std::llabs(bioseq_start - truth) <= kPositionToleranceBp,
               fmt::format("{} query {} bioseq start {} vs truth {} (> {} tol)",
                           alpha_name, i, bioseq_start, truth, kPositionToleranceBp),
               i);

        // minimap2 cross-check: must also map within tolerance, and agree with bioseq.
        const long long mm2_start = run_minimap2_target_start(target_fa, query_fa);
        ASSERT(mm2_start >= 0 && std::llabs(mm2_start - truth) <= kPositionToleranceBp,
               fmt::format("{} query {} minimap2 start {} vs truth {} (> {} tol)",
                           alpha_name, i, mm2_start, truth, kPositionToleranceBp),
               i);
        ASSERT(std::llabs(bioseq_start - mm2_start) <= kPositionToleranceBp,
               fmt::format("{} query {} bioseq {} vs minimap2 {} disagree",
                           alpha_name, i, bioseq_start, mm2_start),
               i);
    }
    bioseq_log<DBG>("[e2e_smoke] {} ok: {} queries mapped within {} bp on bioseq + minimap2",
            alpha_name, pairs.size(), kPositionToleranceBp);
}

}  // namespace

TEST_CASE("E2E smoke: dna4 load + index + mmsearch == minimap2", "[e2e-smoke][doc0286]") {
    run_smoke<seq::dna4>("dna4", "bioseq.seq('dna4', 64)", "e2e_smoke_dna4");
}

#if BIOSEQ_OPTTIMIZATION_LEVEL >= 1
TEST_CASE("E2E smoke: dna5 (with N) load + index + mmsearch == minimap2", "[e2e-smoke][doc0286]") {
    run_smoke<seq::dna5>("dna5", "bioseq.seq('dna5', 64)", "e2e_smoke_dna5");
}
#endif

TEST_CASE(
        "E2E just_index MPS Format-6 READY proof uses the loaded table-AM row identity",
        "[e2e-smoke][just-index][row-identity][doc0371][mps][format6]") {
    constexpr std::string_view table = "e2e_just_index_row_identity";
    const std::string target_fa =
        "/tmp/e2e_just_index_row_identity_target.fasta";
    std::string target(65'565, 'A');
    constexpr std::string_view bases = "ACGT";
    std::uint64_t state = 0x0371e2e5a17ULL;
    for (std::size_t i = 0; i < target.size(); ++i) {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        target[i] = bases[state & 3U];
    }
    write_fasta(target_fa, "ref", target);

    it::connection_test_context ctx;
    it::reset_extension(ctx, {table});
    it::exec_sqlf(
        ctx,
        "CREATE TABLE {} (header text, seq bioseq.seq('dna4', 64)) "
        "USING bioseq;",
        table);
    it::exec_sql(ctx, "SET bioseq.update_index_on_insert = off;");
    it::exec_sql(
        ctx,
        "SET bioseq.aligner.grouped32_persisted_generation_enabled = on;");
    it::exec_sql(ctx, "SET bioseq.index_async_persist = off;");
    it::exec_sql(ctx, "SET bioseq.recompute_async_persist = off;");
    it::exec_sql(ctx, "SET bioseq.aligner.minimizer_algorithm = 'scalar';");
    it::exec_sql(ctx, "SET bioseq.aligner.kmer_size = 15;");
    it::exec_sql(ctx, "SET bioseq.aligner.window_size = 10;");

    REQUIRE(it::query_scalar_int(ctx, fmt::format(
        "SELECT bioseq.load('{}'::regclass, '{}');",
        table,
        target_fa)) == 1);
    const auto source_rows = it::query_rows(ctx, fmt::format(
        "SELECT bioseq.column_row_number('{}'::regclass, ctid) "
        "FROM {} ORDER BY ctid;",
        table,
        table));
    REQUIRE(source_rows.size() == 1);
    REQUIRE(source_rows.front().size() == 1);
    REQUIRE(source_rows.front().front().has_value());
    const long long source_rowno =
        std::stoll(source_rows.front().front().value());
    REQUIRE(source_rowno > 0);

    REQUIRE(it::query_scalar_int(ctx, fmt::format(
        "SELECT bioseq.update_index('{}'::regclass, "
        "'minimizers_per_sequence');",
        table)) == 1);
    const auto inspected = it::query_one(ctx, fmt::format(
        "WITH inspected AS ("
        " SELECT bioseq.inspect_ready_minimizer_format6_generation("
        "  '{}'::regclass,'minimizers_per_sequence',{}) AS j"
        ") "
        "SELECT "
        " j->>'format_version',"
        " j->'head'->>'generation_id',"
        " j->'source'->>'sequence_rowno',"
        " j->'source'->>'total_bp_size',"
        " j->'manifest'->'compatibility'->>'kmer_size',"
        " j->'manifest'->'compatibility'->>'window_size',"
        " j->'manifest'->'compatibility'->>'stored_posting_width_bytes',"
        " j->'manifest'->'compatibility'->>'producer_backend_id',"
        " CASE WHEN "
        "  j->>'query_key'=j->'source'->>'sequence_rowno' "
        "  AND j->'head'->>'query_key'=j->>'query_key' "
        "  AND j->'head'->>'generation_id'="
        "      j->'manifest'->>'generation_id' "
        "  AND (j->'head'->>'generation_id')::bigint>0 "
        "  AND j->'head'->>'source_epoch'="
        "      j->'source'->>'source_epoch' "
        "  AND j->'manifest'->>'source_epoch'="
        "      j->'source'->>'source_epoch' "
        "  AND j->'source'->>'source_identity'="
        "      j->'head'->>'source_identity' "
        "  AND j->'head'->>'source_identity'="
        "      j->'manifest'->>'source_identity' "
        "  AND length(j->'source'->>'source_identity')=32 "
        "  AND j->'source'->>'source_identity'~'^[0-9a-f]+$' "
        "  AND j->'source'->>'source_content_crc64'="
        "      j->'head'->>'source_content_crc64' "
        "  AND j->'head'->>'source_content_crc64'="
        "      j->'manifest'->>'source_content_crc64' "
        "  AND length(j->'source'->>'source_content_crc64')=16 "
        "  AND j->'head'->>'compatibility_digest'="
        "      j->'manifest'->>'compatibility_digest' "
        "  AND length(j->'head'->>'compatibility_digest')=64 "
        "  AND j->'manifest'->>'target_length'="
        "      j->'source'->>'total_bp_size' "
        "  AND (j->'head'->>'dense_seal_present')::boolean=false "
        " THEN '1' ELSE '0' END,"
        " CASE WHEN "
        "  (j->'metadata'->>'head_rows')::bigint=1 "
        "  AND (j->'metadata'->>'head_bytes')::bigint=224 "
        "  AND (j->'metadata'->>'manifest_rows')::bigint=1 "
        "  AND (j->'metadata'->>'manifest_bytes')::bigint=336 "
        " THEN '1' ELSE '0' END,"
        " CASE WHEN "
        "  jsonb_array_length(j->'streams')=3 "
        "  AND j->'streams'->0->>'kind'='directory' "
        "  AND j->'streams'->1->>'kind'='controls' "
        "  AND j->'streams'->2->>'kind'='postings' "
        "  AND NOT EXISTS ("
        "   SELECT 1 FROM jsonb_array_elements(j->'streams') AS s(value) "
        "   WHERE (s.value->>'row_count')::bigint"
        "       <>((s.value->>'logical_bytes')::bigint+8063)/8064 "
        "      OR (s.value->>'row_count')::bigint"
        "       <>(s.value->>'exact_catalog_rows')::bigint "
        "      OR (s.value->>'logical_bytes')::bigint"
        "       <>(s.value->>'exact_catalog_bytes')::bigint "
        "      OR s.value->>'stream_crc64'"
        "       <>s.value->>'exact_catalog_stream_crc64'"
        "  ) "
        "  AND (j->'streams'->0->>'logical_bytes')::bigint="
        "      (j->'streams'->0->>'element_count')::bigint*80 "
        "  AND j->'streams'->0->>'element_count'="
        "      j->'manifest'->>'directory_entry_count' "
        "  AND j->'streams'->0->>'stream_crc64'="
        "      j->'manifest'->>'authenticated_directory_crc64' "
        "  AND j->'streams'->1->>'element_count'="
        "      j->'manifest'->>'unique_hash_count' "
        "  AND j->'streams'->1->>'logical_bytes'="
        "      j->'manifest'->>'total_control_bytes' "
        "  AND j->'streams'->2->>'element_count'="
        "      j->'manifest'->>'total_posting_count' "
        "  AND j->'streams'->2->>'logical_bytes'="
        "      j->'manifest'->>'total_posting_bytes' "
        "  AND (j->'streams'->2->>'logical_bytes')::bigint="
        "      (j->'streams'->2->>'element_count')::bigint*"
        "      (j->'manifest'->'compatibility'->>"
        "       'stored_posting_width_bytes')::bigint "
        " THEN '1' ELSE '0' END "
        "FROM inspected;",
        table,
        source_rowno));
    REQUIRE(inspected.size() == 11);
    for (const auto& field : inspected) {
        REQUIRE(field.has_value());
    }
    REQUIRE(inspected[0].value() == "6");
    REQUIRE(std::stoll(inspected[1].value()) > 0);
    REQUIRE(inspected[2].value() == std::to_string(source_rowno));
    REQUIRE(inspected[3].value() == std::to_string(target.size()));
    REQUIRE(inspected[4].value() == "15");
    REQUIRE(inspected[5].value() == "10");
    REQUIRE(inspected[6].value() == "4");
    REQUIRE(inspected[7].value() == "1");
    REQUIRE(inspected[8].value() == "1");
    REQUIRE(inspected[9].value() == "1");
    REQUIRE(inspected[10].value() == "1");
}
