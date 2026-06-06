/**
 * @file test_load.cpp
 * @brief DOC0286 §13 — integration tests for bioseq.load(table, file).
 *
 * Covers: round-trip correctness through the streaming fragment writer (incl. a
 * >2-fragment record so the streaming path runs) + a NON-EMPTY mmsearch hit;
 * width-agnostic equivalence (dna4/32 == dna4/64); alphabet/width coverage of
 * the storage-unit-parametric reader (dna5 A/C/G/T/N + dna4 across 8/16/32/64);
 * index parity (update_index_on_insert on/off) with the §4.4 write-state flush
 * as the regression guard; and the column-rule / malformed-file error paths.
 */
#include "common_tests.h"
#include "common.h"

#include <cstdint>
#include <fstream>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

import mlogging;
import integration_tests.common;

namespace {

namespace it = bioseq_int_tests;

// Deterministic pseudo-random base generator (splitmix-style) so the loaded
// data is reproducible and non-degenerate (degenerate runs give too few
// minimizers for a stable mmsearch hit).
[[nodiscard]] std::string gen_bases(std::size_t n, std::uint64_t seed, bool with_n) {
    std::string out;
    out.reserve(n);
    static constexpr char kDna4[] = "ACGT";
    std::uint64_t x = seed + 0x9E3779B97F4A7C15ull;
    for (std::size_t i = 0; i < n; ++i) {
        x += 0x9E3779B97F4A7C15ull;
        std::uint64_t z = x;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z = z ^ (z >> 31);
        out.push_back(kDna4[z % 4ull]);
    }
    // `with_n` sprinkles SPARSE N's (one every 50 bp): real genomic N marks rare
    // assembly gaps. A uniform 20% N (the naive "ACGTN") would leave almost no
    // N-free 15-mer anywhere, so NO minimizer-based mapper (bioseq OR minimap2)
    // could self-map the sequence — the test would then prove nothing about
    // mmsearch. Sparse N still exercises the dna5 codec + the minimizer N-reset
    // while leaving long N-free runs for the seeds. Position 13 mod 50 is a fixed,
    // deterministic pattern so the decode round-trip stays byte-exact.
    if (with_n) {
        for (std::size_t i = 13; i < n; i += 50) out[i] = 'N';
    }
    return out;
}

// Write FASTA wrapped at 60 cols (exercises the multi-line body parse path and
// keeps each line well within the reader slab).
void write_fasta(const std::string& path,
                 const std::vector<std::pair<std::string, std::string>>& records) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    for (const auto& [header, body] : records) {
        f << '>' << header << '\n';
        for (std::size_t i = 0; i < body.size(); i += 60) {
            f << body.substr(i, 60) << '\n';
        }
    }
}

// dna4/64 fragment = 1024 units * 32 letters/unit = 32768 bp. A record larger
// than that spans >1 fragment, exercising the streaming fragment writer.
constexpr std::size_t kDna4U64FragmentBp = 1024u * 32u;

}  // namespace

TEST_CASE("bioseq.load: round-trip + non-empty mmsearch (dna4/64, multi-fragment)",
          "[load][integration]") {
    it::connection_test_context ctx;
    it::reset_extension(ctx, { std::string_view{"load_rt"} });

    const std::string small   = gen_bases(4321, 11, /*with_n=*/false);
    const std::string medium  = gen_bases(9000, 22, false);
    // >2 fragments so the streaming fragment writer emits multiple full
    // fragments + a partial final fragment.
    const std::string big     = gen_bases(kDna4U64FragmentBp * 2 + 5000, 33, false);
    const std::string path = "/tmp/bioseq_test_load_rt.fasta";
    write_fasta(path, {{"rec_small", small}, {"rec_medium", medium}, {"rec_big", big}});

    it::exec_sql(ctx, "CREATE TABLE load_rt (header text, seq bioseq.seq('dna4', 64)) USING bioseq;");
    const long long loaded =
        it::query_scalar_int(ctx, "SELECT bioseq.load('load_rt'::regclass, '" + path + "');");
    REQUIRE(loaded == 3);

    // Decode each row back to ACGT + len; compare to the input. Order by header
    // so the assertion is deterministic.
    const auto rows = it::query_rows(
        ctx, "SELECT header, seq::text, bioseq.len(seq) FROM load_rt "
             "WHERE mmsearch(seq, seq) ORDER BY header");
    REQUIRE(rows.size() == 3);
    // ORDER BY header: rec_big, rec_medium, rec_small.
    REQUIRE(rows[0][0].value() == "rec_big");
    REQUIRE(rows[0][1].value() == big);
    REQUIRE(rows[0][2].value() == std::to_string(big.size()));
    REQUIRE(rows[1][0].value() == "rec_medium");
    REQUIRE(rows[1][1].value() == medium);
    REQUIRE(rows[2][0].value() == "rec_small");
    REQUIRE(rows[2][1].value() == small);

    // NON-EMPTY mmsearch proof: a 1200 bp prefix of rec_big must map to rec_big.
    const std::string query = big.substr(0, 1200);
    const auto hits = it::query_rows(
        ctx,
        fmt::format("SELECT header FROM load_rt "
                    "WHERE mmsearch(seq, '{}'::bioseq.seq('dna4', 64)) ORDER BY header",
                    query));
    REQUIRE_FALSE(hits.empty());
    bool found_big = false;
    for (const auto& r : hits) {
        if (r[0].has_value() && r[0].value() == "rec_big") found_big = true;
    }
    REQUIRE(found_big);
}

TEST_CASE("bioseq.load: width-agnostic equivalence (dna4/32 == dna4/64)",
          "[load][integration]") {
    it::connection_test_context ctx;
    it::reset_extension(ctx, { std::string_view{"load_w32"}, std::string_view{"load_w64"} });

    const std::string body = gen_bases(6000, 77, false);
    const std::string path = "/tmp/bioseq_test_load_width.fasta";
    write_fasta(path, {{"r", body}});

    it::exec_sql(ctx, "CREATE TABLE load_w32 (header text, seq bioseq.seq('dna4', 32)) USING bioseq;");
    it::exec_sql(ctx, "CREATE TABLE load_w64 (header text, seq bioseq.seq('dna4', 64)) USING bioseq;");
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.load('load_w32'::regclass, '" + path + "');") == 1);
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.load('load_w64'::regclass, '" + path + "');") == 1);

    // Decode parity: the byte-slice streaming path must be width-correct.
    REQUIRE(it::query_scalar(ctx, "SELECT seq::text FROM load_w32 WHERE mmsearch(seq, seq)") == body);
    REQUIRE(it::query_scalar(ctx, "SELECT seq::text FROM load_w64 WHERE mmsearch(seq, seq)") == body);

    // §13.2 — mmsearch must return the IDENTICAL position in both widths (the
    // logical sequence + minimizers are width-agnostic), not merely non-empty.
    const std::string query = body.substr(500, 800);
    std::string pos32, pos64;
    for (const char* tbl : {"load_w32", "load_w64"}) {
        const std::string w = (std::string(tbl) == "load_w32") ? "32" : "64";
        const auto rows = it::query_rows(
            ctx, fmt::format("SELECT bioseq.target_start(header) FROM {} WHERE "
                             "mmsearch(seq, '{}'::bioseq.seq('dna4', {}))",
                             tbl, query, w));
        REQUIRE(rows.size() == 1);
        (w == "32" ? pos32 : pos64) = rows[0][0].value();
    }
    REQUIRE(pos32 == pos64);   // identical target_start across storage widths
}

TEST_CASE("bioseq.load: dna5 (A/C/G/T/N) + dna4 round-trip + non-empty mmsearch across widths 8/16/32/64",
          "[load][integration]") {
    // §13.3 — the storage-unit-parametric streaming reader must be width-correct.
    // Each width gets its OWN connection (one bioseq.load per backend): that is the
    // real-world pattern AND it isolates each width from the pre-existing shared
    // columnar custom-scan residue that accumulates only when MANY load+mmsearch
    // cycles reuse ONE backend connection (DOC0288 §4 / deviation #12). The
    // load-side double-close_all bug is fixed, so per-connection loads are stable.
    const std::string body5 = gen_bases(3000, 99, /*with_n=*/true);   // dna5 + sparse N
    const std::string path5 = "/tmp/bioseq_test_load_dna5.fasta";
    write_fasta(path5, {{"r5", body5}});
    const std::string body4 = gen_bases(3000, 100, /*with_n=*/false); // dna4
    const std::string path4 = "/tmp/bioseq_test_load_dna4w.fasta";
    write_fasta(path4, {{"r4", body4}});

    struct Case { const char* alpha; const std::string& body; const std::string& path; };
    const Case dna5{"dna5", body5, path5};
    const Case dna4{"dna4", body4, path4};
    for (const Case* c : {&dna5, &dna4}) {
        for (const char* width : {"8", "16", "32", "64"}) {
            it::connection_test_context ctx;                 // fresh backend per width
            it::reset_extension(ctx, { std::string_view{"load_w"} });
            it::exec_sqlf(ctx, "CREATE TABLE load_w (header text, seq bioseq.seq('{}', {})) USING bioseq;",
                          c->alpha, width);
            REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.load('load_w'::regclass, '" + c->path + "');") == 1);
            // load + decode round-trips byte-exactly (incl. N) and mmsearch is non-empty.
            const auto rows = it::query_rows(ctx,
                "SELECT seq::text, bioseq.len(seq) FROM load_w WHERE mmsearch(seq, seq)");
            REQUIRE(rows.size() == 1);
            REQUIRE(rows[0][0].value() == c->body);          // width-correct decode, N preserved
            REQUIRE(rows[0][1].value() == std::to_string(c->body.size()));
        }
    }
}

TEST_CASE("bioseq.load: single large record streams without a >MaxAllocSize varlena (§13.6, memory-bound)",
          "[.][load-memory-bound][integration]") {
    // §13.6 — Invariant C: a single record larger than what a non-streaming load
    // could palloc as ONE varlena (>MaxAllocSize ≈ 1 GiB packed; ~4 Gbp for dna4/64)
    // must still load via the fragmented streaming path, with bounded backend RSS
    // (no `_seq_storage`-sized allocation). This TEST_CASE is [.]-tagged — NOT run
    // by default (heavy). `BIOSEQ_LOAD_MEMBOUND_GBP` sizes it: the default 0.1 Gbp
    // fits any host and exercises the single-large-record streaming path; setting
    // it to >= 4 proves the true >MaxAllocSize guarantee on a large host (a
    // non-streaming load would ERROR with "invalid memory alloc request size", and
    // `seq::text` on the whole record would too — so we verify via `bioseq.len`,
    // which reads only the stub, never assembling the varlena).
    const char* env = std::getenv("BIOSEQ_LOAD_MEMBOUND_GBP");
    const double gbp = (env && *env) ? std::atof(env) : 0.1;
    const std::size_t n = static_cast<std::size_t>(gbp * 1e9);
    const std::string body = gen_bases(n, 4242, /*with_n=*/false);     // one dna4 record
    const std::string path = "/tmp/bioseq_test_load_membound.fasta";
    write_fasta(path, {{"big", body}});

    it::connection_test_context ctx;
    it::reset_extension(ctx, { std::string_view{"load_mb"} });
    it::exec_sql(ctx, "CREATE TABLE load_mb (header text, seq bioseq.seq('dna4', 64)) USING bioseq;");
    // A non-streaming load would have palloc'd the whole record (>MaxAllocSize at
    // >=4 Gbp) and ERRORed before returning; success + the exact bp count proves
    // the record streamed through bounded fragments.
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.load('load_mb'::regclass, '" + path + "');") == 1);
    REQUIRE(it::query_scalar(ctx, "SELECT bioseq.len(seq)::text FROM load_mb") == std::to_string(n));
}

TEST_CASE("bioseq.load: index parity (on builds index visible same-statement; off defers)",
          "[load][integration]") {
    it::connection_test_context ctx;
    it::reset_extension(ctx, { std::string_view{"load_idx_on"}, std::string_view{"load_idx_off"} });

    const std::string body = gen_bases(5000, 555, false);
    const std::string path = "/tmp/bioseq_test_load_idx.fasta";
    write_fasta(path, {{"idxrec", body}});

    // ON: index built in the batched pass; an mmsearch self-hit returns the
    // exact {len}M CIGAR in the very next statement (regression guard for the
    // §4.4 parent + sub-table write-state flush).
    it::exec_sql(ctx, "CREATE TABLE load_idx_on (header text, seq bioseq.seq('dna4', 64)) USING bioseq;");
    it::exec_sql(ctx, "SET bioseq.update_index_on_insert = on;");
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.load('load_idx_on'::regclass, '" + path + "');") == 1);
    REQUIRE(it::index_table_row_count(ctx, "minimizers_per_fragment", "load_idx_on") > 0);
    REQUIRE(it::index_table_row_count(ctx, "symbol_counts", "load_idx_on") > 0);
    {
        const auto cig = it::query_rows(
            ctx, fmt::format("SELECT bioseq.cigar(header) FROM load_idx_on "
                             "WHERE mmsearch(seq, '{}'::bioseq.seq('dna4', 64))", body));
        REQUIRE(cig.size() == 1);
        REQUIRE(it::is_exact_match_cigar(cig[0][0].value(),
                                         static_cast<long long>(body.size())));
    }

    // OFF: load builds NO on-insert index; row count stays 0 until an explicit
    // update_index, after which the self-hit still works.
    it::exec_sql(ctx, "CREATE TABLE load_idx_off (header text, seq bioseq.seq('dna4', 64)) USING bioseq;");
    it::exec_sql(ctx, "SET bioseq.update_index_on_insert = off;");
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.load('load_idx_off'::regclass, '" + path + "');") == 1);
    REQUIRE(it::index_table_row_count(ctx, "minimizers_per_fragment", "load_idx_off") == 0);
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.update_index('load_idx_off'::regclass, 'minimizers_per_fragment');") > 0);
    REQUIRE(it::index_table_row_count(ctx, "minimizers_per_fragment", "load_idx_off") > 0);
}

TEST_CASE("bioseq.load: column-rule + malformed-file errors", "[load][integration]") {
    it::connection_test_context ctx;
    it::reset_extension(ctx, {});

    const std::string body = gen_bases(500, 1, false);
    const std::string path = "/tmp/bioseq_test_load_err.fasta";
    write_fasta(path, {{"e", body}});

    // 0 seq columns.
    it::exec_sql(ctx, "DROP TABLE IF EXISTS load_e CASCADE;");
    it::exec_sql(ctx, "CREATE TABLE load_e (header text) USING bioseq;");
    REQUIRE(it::sql_error(ctx, "SELECT bioseq.load('load_e'::regclass, '" + path + "');")
                .contains("no bioseq.seq column"));

    // 2 seq columns.
    it::exec_sql(ctx, "DROP TABLE IF EXISTS load_e CASCADE;");
    it::exec_sql(ctx, "CREATE TABLE load_e (s1 bioseq.seq('dna4',64), s2 bioseq.seq('dna4',64)) USING bioseq;");
    REQUIRE(it::sql_error(ctx, "SELECT bioseq.load('load_e'::regclass, '" + path + "');")
                .contains("bioseq.seq columns"));

    // 2 text columns.
    it::exec_sql(ctx, "DROP TABLE IF EXISTS load_e CASCADE;");
    it::exec_sql(ctx, "CREATE TABLE load_e (h1 text, h2 text, seq bioseq.seq('dna4',64)) USING bioseq;");
    REQUIRE(it::sql_error(ctx, "SELECT bioseq.load('load_e'::regclass, '" + path + "');")
                .contains("varchar/text columns"));

    // Generated/identity ignored column (not nullable-plain).
    it::exec_sql(ctx, "DROP TABLE IF EXISTS load_e CASCADE;");
    it::exec_sql(ctx, "CREATE TABLE load_e (id bigint GENERATED ALWAYS AS IDENTITY, "
                      "header text, seq bioseq.seq('dna4',64)) USING bioseq;");
    REQUIRE(it::sql_error(ctx, "SELECT bioseq.load('load_e'::regclass, '" + path + "');")
                .contains("identity"));

    // Unrecognised extension (file_utils).
    it::exec_sql(ctx, "DROP TABLE IF EXISTS load_e CASCADE;");
    it::exec_sql(ctx, "CREATE TABLE load_e (header text, seq bioseq.seq('dna4',64)) USING bioseq;");
    REQUIRE(it::sql_error(ctx, "SELECT bioseq.load('load_e'::regclass, '/tmp/whatever.txt');")
                .contains("unrecognised sequence file extension"));

    // Malformed FASTA -> data exception + rollback (table stays empty).
    const std::string bad_path = "/tmp/bioseq_test_load_bad.fasta";
    {
        std::ofstream f(bad_path, std::ios::binary | std::ios::trunc);
        f << "not a fasta file at all\nrandom bytes\n";
    }
    REQUIRE(it::sql_fails(ctx, "SELECT bioseq.load('load_e'::regclass, '" + bad_path + "');"));
    // Rollback: a self-mmsearch would surface any partially-written row; the
    // table must stay empty (avoid bare count(*), a bioseq gotcha).
    REQUIRE(it::query_rows(ctx, "SELECT header FROM load_e WHERE mmsearch(seq, seq)").empty());
}
