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

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

import bioseq.common.mlogging;
import bioseq.integration_tests.common;

namespace {

namespace it = bioseq_int_tests;

// Deterministic pseudo-random base generator (splitmix-style) so the loaded
// data is reproducible and non-degenerate (degenerate runs give too few
// minimizers for a stable mmsearch hit).
[[nodiscard]] char gen_base_at(std::size_t i, std::uint64_t seed, bool with_n) {
    if (with_n && i % 50 == 13) return 'N';
    static constexpr char kDna4[] = "ACGT";
    static constexpr std::uint64_t kStep = 0x9E3779B97F4A7C15ull;
    std::uint64_t z = seed + kStep * (static_cast<std::uint64_t>(i) + 2ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z = z ^ (z >> 31);
    return kDna4[z % 4ull];
}

[[nodiscard]] std::string gen_bases(std::size_t n, std::uint64_t seed, bool with_n) {
    std::string out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(gen_base_at(i, seed, with_n));
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
            const std::size_t n = std::min<std::size_t>(60, body.size() - i);
            f.write(body.data() + i, static_cast<std::streamsize>(n));
            f << '\n';
        }
    }
}

void write_generated_fasta(const std::string& path, std::string_view header,
                           std::size_t n, std::uint64_t seed, bool with_n) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << '>' << header << '\n';
    char line[60];
    std::size_t used = 0;
    for (std::size_t i = 0; i < n; ++i) {
        line[used++] = gen_base_at(i, seed, with_n);
        if (used == sizeof(line)) {
            f.write(line, static_cast<std::streamsize>(used));
            f << '\n';
            used = 0;
        }
    }
    if (used != 0) {
        f.write(line, static_cast<std::streamsize>(used));
        f << '\n';
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
             "ORDER BY header");
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
    REQUIRE(it::query_scalar(ctx, "SELECT seq::text FROM load_w32") == body);
    REQUIRE(it::query_scalar(ctx, "SELECT seq::text FROM load_w64") == body);

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

TEST_CASE("bioseq.load: dna5 (A/C/G/T/N) + dna4 round-trip + non-empty literal mmsearch across widths 8/16/32/64",
          "[load][integration]") {
    // §13.3 — the storage-unit-parametric streaming reader must be width-correct.
    // Each width gets its OWN connection so the DROP/CREATE EXTENSION reset is
    // isolated per typmod. Decoding is checked with a plain projection; mmsearch
    // is proved separately with an explicit literal query, matching the SQL E2E
    // benchmark path.
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
            // load + decode round-trips byte-exactly (incl. N).
            const auto rows = it::query_rows(ctx,
                "SELECT seq::text, bioseq.len(seq) FROM load_w");
            REQUIRE(rows.size() == 1);
            REQUIRE(rows[0][0].value() == c->body);          // width-correct decode, N preserved
            REQUIRE(rows[0][1].value() == std::to_string(c->body.size()));

            // Literal mmsearch is non-empty for every alphabet/width pair.
            const std::string query = c->body.substr(100, 800);
            const auto hits = it::query_rows(
                ctx, fmt::format("SELECT header FROM load_w WHERE "
                                 "mmsearch(seq, '{}'::bioseq.seq('{}', {}))",
                                 query, c->alpha, width));
            REQUIRE_FALSE(hits.empty());
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
    const std::string path = "/tmp/bioseq_test_load_membound.fasta";
    write_generated_fasta(path, "big", n, 4242, /*with_n=*/false);     // one dna4 record

    it::connection_test_context ctx;
    it::reset_extension(ctx, { std::string_view{"load_mb"} });
    it::exec_sql(ctx, "CREATE TABLE load_mb (header text, seq bioseq.seq('dna4', 64)) USING bioseq;");
    it::exec_sql(ctx, "SET bioseq.update_index_on_insert = off;");
    // A non-streaming load would have palloc'd the whole record (>MaxAllocSize at
    // >=4 Gbp) and ERRORed before returning; success + the exact bp count proves
    // the record streamed through bounded fragments.
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.load('load_mb'::regclass, '" + path + "');") == 1);
    REQUIRE(it::query_scalar(ctx, "SELECT bioseq.len(seq)::text FROM load_mb") == std::to_string(n));
}

TEST_CASE("bioseq.load: index parity (on builds index visible same-statement; off defers)",
          "[load][integration]") {
    it::connection_test_context ctx;
    it::reset_extension(ctx, { std::string_view{"load_idx_on"},
                               std::string_view{"load_idx_off"},
                               std::string_view{"load_idx_mps"} });

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

    // Per-sequence ON: policy-selected batch load must build
    // minimizers_per_sequence directly and must not leave a residual
    // per-fragment index behind as an accidental fallback.
    it::exec_sql(ctx, "CREATE TABLE load_idx_mps "
                      "(header text, seq bioseq.seq('dna4', 64)) USING bioseq;");
    it::exec_sql(ctx, "SELECT bioseq.set_index_policy("
                      "'load_idx_mps'::regclass, "
                      "'minimizers_per_fragment', 'UPDATE_MANUALLY');");
    it::exec_sql(ctx, "SELECT bioseq.set_index_policy("
                      "'load_idx_mps'::regclass, "
                      "'minimizers_per_sequence', 'UPDATE_ON_INSERT');");
    it::exec_sql(ctx, "SET bioseq.update_index_on_insert = on;");
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.load('load_idx_mps'::regclass, '" + path + "');") == 1);
    REQUIRE(it::index_table_row_count(ctx, "minimizers_per_fragment", "load_idx_mps") == 0);
    REQUIRE(it::index_table_row_count(ctx, "minimizers_per_sequence", "load_idx_mps") > 0);
    it::exec_sql(ctx, "SELECT bioseq.set_active_read_index("
                      "'load_idx_mps'::regclass, 'minimizers_per_sequence');");
    it::exec_sql(ctx, "SET bioseq.read_sequence_index = on;");
    {
        const auto cig = it::query_rows(
            ctx, fmt::format("SELECT bioseq.cigar(header) FROM load_idx_mps "
                             "WHERE mmsearch(seq, '{}'::bioseq.seq('dna4', 64))", body));
        REQUIRE(cig.size() == 1);
        REQUIRE(it::is_exact_match_cigar(cig[0][0].value(),
                                         static_cast<long long>(body.size())));
    }
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
    {
        const auto err =
            it::sql_error(ctx, "SELECT bioseq.load('load_e'::regclass, '" + path + "');");
        REQUIRE(err.sqlstate == "42P16");
        REQUIRE(err.contains("no bioseq.seq column"));
    }

    // 2 seq columns.
    it::exec_sql(ctx, "DROP TABLE IF EXISTS load_e CASCADE;");
    it::exec_sql(ctx, "CREATE TABLE load_e (s1 bioseq.seq('dna4',64), s2 bioseq.seq('dna4',64)) USING bioseq;");
    {
        const auto err =
            it::sql_error(ctx, "SELECT bioseq.load('load_e'::regclass, '" + path + "');");
        REQUIRE(err.sqlstate == "42P16");
        REQUIRE(err.contains("bioseq.seq columns"));
    }

    // 2 text columns, neither PK, no mapping -> ambiguous (DOC0295 D8/E5). The
    // legacy "at most one text column" rule is replaced by the multi-column
    // identifier-parser resolution, which now asks the user to disambiguate.
    it::exec_sql(ctx, "DROP TABLE IF EXISTS load_e CASCADE;");
    it::exec_sql(ctx, "CREATE TABLE load_e (h1 text, h2 text, seq bioseq.seq('dna4',64)) USING bioseq;");
    {
        const auto err =
            it::sql_error(ctx, "SELECT bioseq.load('load_e'::regclass, '" + path + "');");
        REQUIRE(err.sqlstate == "42P16");
        REQUIRE(err.contains("text columns"));
        REQUIRE(err.contains("disambiguate"));
    }

    // bigserial PK is NOT identity/generated — it carries a DEFAULT nextval(),
    // which DOC0295 §9.3/D9 now EVALUATES per row (the const-only restriction is
    // lifted). The load succeeds and the PK auto-increments.
    it::exec_sql(ctx, "DROP TABLE IF EXISTS load_e CASCADE;");
    it::exec_sql(ctx, "CREATE TABLE load_e (id bigserial primary key, "
                      "header text, seq bioseq.seq('dna4',64)) USING bioseq;");
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.load('load_e'::regclass, '" + path + "');") == 1);
    {
        const auto rows = it::query_rows(ctx, "SELECT id, header FROM load_e");
        REQUIRE(rows.size() == 1);
        REQUIRE(rows[0][0].value() == "1");   // nextval default evaluated per row
        REQUIRE(rows[0][1].value() == "e");   // sole text column = identifier
    }

    it::exec_sql(ctx, "DROP TABLE IF EXISTS load_e CASCADE;");
    it::exec_sql(ctx, "CREATE TABLE load_e (id bigint GENERATED ALWAYS AS IDENTITY, "
                      "header text, seq bioseq.seq('dna4',64)) USING bioseq;");
    {
        const auto err =
            it::sql_error(ctx, "SELECT bioseq.load('load_e'::regclass, '" + path + "');");
        REQUIRE(err.sqlstate == "42P16");
        REQUIRE(err.contains("identity"));
    }

    it::exec_sql(ctx, "DROP TABLE IF EXISTS load_e CASCADE;");
    it::exec_sql(ctx, "CREATE TABLE load_e (header text, seq bioseq.seq('dna4',64), "
                      "header_len int GENERATED ALWAYS AS (length(header)) STORED) USING bioseq;");
    {
        const auto err =
            it::sql_error(ctx, "SELECT bioseq.load('load_e'::regclass, '" + path + "');");
        REQUIRE(err.sqlstate == "42P16");
        REQUIRE(err.contains("generated"));
    }

    // Unrecognised extension (file_utils).
    it::exec_sql(ctx, "DROP TABLE IF EXISTS load_e CASCADE;");
    it::exec_sql(ctx, "CREATE TABLE load_e (header text, seq bioseq.seq('dna4',64)) USING bioseq;");
    {
        const auto err =
            it::sql_error(ctx, "SELECT bioseq.load('load_e'::regclass, '/tmp/whatever.txt');");
        REQUIRE(err.sqlstate == "22023");
        REQUIRE(err.contains("unrecognised sequence file extension"));
    }

    // Recognised extension, missing file.
    {
        const auto err =
            it::sql_error(ctx, "SELECT bioseq.load('load_e'::regclass, '/tmp/bioseq_missing_file.fasta');");
        REQUIRE(err.sqlstate == "22000");
        REQUIRE(err.contains("bioseq.load"));
    }

    // Malformed FASTA -> data exception + rollback (table stays empty).
    const std::string bad_path = "/tmp/bioseq_test_load_bad.fasta";
    {
        std::ofstream f(bad_path, std::ios::binary | std::ios::trunc);
        f << "not a fasta file at all\nrandom bytes\n";
    }
    {
        const auto err =
            it::sql_error(ctx, "SELECT bioseq.load('load_e'::regclass, '" + bad_path + "');");
        REQUIRE(err.sqlstate == "22000");
        REQUIRE(err.contains("bioseq.load"));
    }
    // Rollback: no partially-written parent row should be visible.
    REQUIRE(it::query_rows(ctx, "SELECT header FROM load_e").empty());
}

TEST_CASE("bioseq.load: typed multi-column header parsing + non-empty mmsearch (DOC0295)",
          "[load][integration]") {
    it::connection_test_context ctx;
    it::reset_extension(ctx, { std::string_view{"up"}, std::string_view{"gb"},
                               std::string_view{"im"}, std::string_view{"e2def"},
                               std::string_view{"e1nn"}, std::string_view{"e10bad"},
                               std::string_view{"gis"} });
    it::exec_sql(ctx, "SET DateStyle = 'ISO, MDY';");   // T10 date assumes MDY (§10)

    const std::string body = gen_bases(2000, 4242, /*with_n=*/false);
    const std::string query = body.substr(0, 1200);

    // --- A. fixed uni_prot: positional accession + EQUALS tags + typed int,
    //        proven end-to-end with a NON-EMPTY mmsearch on the loaded seq. ---
    {
        const std::string path = "/tmp/bioseq_test_load_up.fasta";
        write_fasta(path, {{"tr|R9RJF1|R9RJF1_PSEAI Mandelate racemase "
                            "OS=Pseudomonas aeruginosa PE=4 SV=1", body}});
        it::exec_sql(ctx, "CREATE TABLE up (accession text, organism_name text, "
                          "sv int, seq bioseq.seq('dna4',64)) USING bioseq;");
        REQUIRE(it::query_scalar_int(ctx,
            "SELECT bioseq.load('up'::regclass, '" + path +
            "', header_format => 'uni_prot');") == 1);
        const auto rows = it::query_rows(ctx,
            "SELECT accession, organism_name, sv FROM up");
        REQUIRE(rows.size() == 1);
        REQUIRE(rows[0][0].value() == "R9RJF1");
        REQUIRE(rows[0][1].value() == "Pseudomonas aeruginosa");
        REQUIRE(rows[0][2].value() == "1");                 // int4in coerced

        // NON-EMPTY mmsearch proof: a 1200 bp prefix maps back to the loaded row.
        const auto hits = it::query_rows(ctx, fmt::format(
            "SELECT accession FROM up WHERE "
            "mmsearch(seq, '{}'::bioseq.seq('dna4', 64))", query));
        REQUIRE_FALSE(hits.empty());
        REQUIRE(hits[0][0].value() == "R9RJF1");
    }

    // --- B. auto-detected GenBank: bracket tags -> int + text columns (T2). ---
    {
        const std::string path = "/tmp/bioseq_test_load_gb.fasta";
        write_fasta(path, {{"dna_new [organism=Homo sapiens] [chromosome=17] "
                            "[moltype=mRNA]", body}});
        it::exec_sql(ctx, "CREATE TABLE gb (id text, organism text, chromosome int, "
                          "moltype text, seq bioseq.seq('dna4',64)) USING bioseq;");
        REQUIRE(it::query_scalar_int(ctx,
            "SELECT bioseq.load('gb'::regclass, '" + path + "');") == 1);
        const auto rows = it::query_rows(ctx,
            "SELECT id, organism, chromosome, moltype FROM gb");
        REQUIRE(rows.size() == 1);
        REQUIRE(rows[0][0].value() == "dna_new");
        REQUIRE(rows[0][1].value() == "Homo sapiens");
        REQUIRE(rows[0][2].value() == "17");                // int4in
        REQUIRE(rows[0][3].value() == "mRNA");
    }

    // --- C. IMGT positional 'a..b' -> int4range (T11/D4 range rewrite). ---
    {
        const std::string path = "/tmp/bioseq_test_load_im.fasta";
        write_fasta(path, {{"M99641|IGHV1-18*01|Homo sapiens|F|V-REGION|188..483|"
                            "296 nt|1| | | | |296+24=320| | ", body}});
        it::exec_sql(ctx, "CREATE TABLE im (ligm_accession text, species text, "
                          "positions int4range, seq bioseq.seq('dna4',64)) USING bioseq;");
        REQUIRE(it::query_scalar_int(ctx,
            "SELECT bioseq.load('im'::regclass, '" + path +
            "', header_format => 'imgt');") == 1);
        const auto rows = it::query_rows(ctx,
            "SELECT ligm_accession, species, lower(positions), upper(positions) FROM im");
        REQUIRE(rows.size() == 1);
        REQUIRE(rows[0][0].value() == "M99641");
        REQUIRE(rows[0][1].value() == "Homo sapiens");
        REQUIRE(rows[0][2].value() == "188");
        REQUIRE(rows[0][3].value() == "484");               // int4range canonical [188,484)
    }

    // --- E2/E3. unmapped NOT NULL columns with a constant + volatile default. ---
    {
        const std::string path = "/tmp/bioseq_test_load_e2.fasta";
        write_fasta(path, {{"tr|X1|X1_HUMAN p OS=Homo sapiens PE=1 SV=1", body}});
        it::exec_sql(ctx, "CREATE TABLE e2def (accession text, "
                          "score int NOT NULL DEFAULT 7, "
                          "ts timestamptz NOT NULL DEFAULT now(), "
                          "seq bioseq.seq('dna4',64)) USING bioseq;");
        REQUIRE(it::query_scalar_int(ctx,
            "SELECT bioseq.load('e2def'::regclass, '" + path +
            "', header_format => 'uni_prot');") == 1);
        const auto rows = it::query_rows(ctx,
            "SELECT accession, score, (ts IS NOT NULL)::text FROM e2def");
        REQUIRE(rows.size() == 1);
        REQUIRE(rows[0][0].value() == "X1");
        REQUIRE(rows[0][1].value() == "7");                 // constant default evaluated
        REQUIRE(rows[0][2].value() == "true");              // volatile now() per row
    }

    // --- E1. unmapped NOT NULL column with NO default -> up-front error (D9). ---
    {
        const std::string path = "/tmp/bioseq_test_load_e1.fasta";
        write_fasta(path, {{"tr|X1|X1_HUMAN p OS=Homo sapiens PE=1 SV=1", body}});
        it::exec_sql(ctx, "CREATE TABLE e1nn (accession text, foo int NOT NULL, "
                          "seq bioseq.seq('dna4',64)) USING bioseq;");
        const auto err = it::sql_error(ctx,
            "SELECT bioseq.load('e1nn'::regclass, '" + path +
            "', header_format => 'uni_prot');");
        REQUIRE(err.sqlstate == "42P16");
        REQUIRE(err.contains("NOT NULL"));
        REQUIRE(err.contains("no default"));
    }

    // --- E10. bad scalar coercion -> input fn error + our record/column errcontext. ---
    {
        const std::string path = "/tmp/bioseq_test_load_e10.fasta";
        write_fasta(path, {{"tr|X1|X1_HUMAN p OS=Homo sapiens PE=1 SV=abc", body}});
        it::exec_sql(ctx, "CREATE TABLE e10bad (accession text, sv int, "
                          "seq bioseq.seq('dna4',64)) USING bioseq;");
        const auto err = it::sql_error(ctx,
            "SELECT bioseq.load('e10bad'::regclass, '" + path +
            "', header_format => 'uni_prot');");
        REQUIRE(err.sqlstate == "22P02");                   // invalid_text_representation
        REQUIRE(err.contains("invalid input syntax"));      // int4in's own message
        // The "record N, column \"sv\"" errcontext (§10) lands in libpq's
        // PG_DIAG_CONTEXT field, which sql_error() does not fold into `message`;
        // it is shown in psql's CONTEXT line — proven in DOC0301 Appendix E.
    }

    // --- T10. mixed numeric + name columns => enum + date coercion. ---
    {
        const std::string path = "/tmp/bioseq_test_load_gis.fasta";
        write_fasta(path, {{"A/Canoas/LACENRS_1793/2015|A|H3N2|07/17/2015||Brazil|"
                            "Human|KY925125", body}});
        it::exec_sql(ctx, "DROP TYPE IF EXISTS flu_type CASCADE;");
        it::exec_sql(ctx, "CREATE TYPE flu_type AS ENUM ('A','B');");
        it::exec_sql(ctx, "CREATE TABLE gis (name text, strain flu_type, dt date, "
                          "seq bioseq.seq('dna4',64)) USING bioseq;");
        REQUIRE(it::query_scalar_int(ctx,
            "SELECT bioseq.load('gis'::regclass, '" + path +
            "', columns => ARRAY[('name','0'),('strain','1'),('dt','3')]"
            "::bioseq.column_mapping[]);") == 1);
        const auto rows = it::query_rows(ctx,
            "SELECT name, strain::text, dt::text FROM gis");
        REQUIRE(rows.size() == 1);
        REQUIRE(rows[0][0].value() == "A/Canoas/LACENRS_1793/2015");
        REQUIRE(rows[0][1].value() == "A");                 // enum_in label
        REQUIRE(rows[0][2].value() == "2015-07-17");        // date_in, MDY
    }
}
