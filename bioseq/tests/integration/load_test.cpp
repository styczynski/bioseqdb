/**
 * @file bioseq/tests/integration/load_test.cpp
 * @brief Integration coverage for bioseq.load(table, file).
 *
 * Covers: round-trip correctness through the streaming fragment writer (incl. a
 * >2-fragment record so the streaming path runs) + a NON-EMPTY mmsearch hit.
 * Width-agnostic equivalence (dna4/32 == dna4/64). Alphabet/width coverage of
 * the storage-unit-parametric reader (dna5 A/C/G/T/N + dna4 across 8/16/32/64).
 * index parity with bioseq.index.insert_maintenance.enabled on and off,
 * including write-state flushing
 * as a cross-check, plus the column-rule and malformed-file error paths.
 */
#include <algorithm>
#include <bioseq/pg_build_profile.h>
#include <cstdint>
#include <cstdlib>
#include <fmt/format.h>
#include <fstream>
#include <string>
#include <string_view>
#include <tests/support/common.hpp>
#include <utility>
#include <vector>

import bioseq.tests.integration.support;

namespace {

namespace it = bioseq_int_tests;

/// Generate one reproducible base for a synthetic load corpus.
[[nodiscard]] char gen_base_at(std::size_t i, std::uint64_t seed, bool with_n) {
    if (with_n && i % 50 == 13)
        return 'N';
    static constexpr char k_dna4[] = "ACGT";
    static constexpr std::uint64_t k_step = 0x9E3779B97F4A7C15ull;
    std::uint64_t z = seed + k_step * (static_cast<std::uint64_t>(i) + 2ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z = z ^ (z >> 31);
    return k_dna4[z % 4ull];
}

/// Generate `n` reproducible bases, optionally including periodic `N` symbols.
[[nodiscard]] std::string gen_bases(std::size_t n, std::uint64_t seed, bool with_n) {
    std::string out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(gen_base_at(i, seed, with_n));
    }
    return out;
}

/// Write FASTA records with bodies wrapped at 60 columns.
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

/// Write one generated FASTA record without retaining its complete body.
void write_generated_fasta(const std::string& path,
                           std::string_view header,
                           std::size_t n,
                           std::uint64_t seed,
                           bool with_n) {
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

/// Logical bases in one dna4/64 storage fragment.
constexpr std::size_t k_dna4_u64_fragment_bp = 1024u * 32u;

} // namespace

TEST_CASE(
    "bioseq.load: round-trip + non-empty mmsearch (dna4/64, multi-fragment)",
    "[component:bioseq][layer:integration][feature:io][resource:postgres][resource:filesystem]") {
    it::connection_test_context ctx;
    it::reset_extension(ctx, { std::string_view{"load_roundtrip"} });

    const std::string small   = gen_bases(4321, 11, /*with_n=*/false);
    const std::string medium  = gen_bases(9000, 22, false);
    // >2 fragments so the streaming fragment writer emits multiple full
    // fragments + a partial final fragment.
    const std::string big     = gen_bases(k_dna4_u64_fragment_bp * 2 + 5000, 33, false);
    const std::string path = "/tmp/bioseq_load_roundtrip.fasta";
    write_fasta(path, {{"rec_small", small}, {"rec_medium", medium}, {"rec_big", big}});

    it::exec_sql(ctx, "CREATE TABLE load_roundtrip (header text, seq bioseq.seq('dna4', 64)) USING bioseq;");
    const long long loaded =
        it::query_scalar_int(ctx, "SELECT bioseq.load('load_roundtrip'::regclass, '" + path + "');");
    REQUIRE(loaded == 3);

    // Decode each row back to ACGT + len; compare to the input. Order by header
    // so the assertion is deterministic.
    const auto rows = it::query_rows(
        ctx, "SELECT header, seq::text, bioseq.len(seq) FROM load_roundtrip "
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

    // A 1200 bp prefix of rec_big must produce a non-empty mapping to rec_big.
    const std::string query = big.substr(0, 1200);
    const auto hits = it::query_rows(
        ctx,
        fmt::format("SELECT header FROM load_roundtrip "
                    "WHERE mmsearch(seq, '{}'::bioseq.seq('dna4', 64)) ORDER BY header",
                    query));
    REQUIRE_FALSE(hits.empty());
    bool found_big = false;
    for (const auto& r : hits) {
        if (r[0].has_value() && r[0].value() == "rec_big") found_big = true;
    }
    REQUIRE(found_big);
}

TEST_CASE("bioseq.load: write-side sequence cache survives a full-stripe reset",
          "[component:bioseq][layer:integration][feature:io][resource:postgres][resource:filesystem]") {
    it::connection_test_context ctx;
    it::reset_extension(ctx, {std::string_view{"load_stripe_flush"}});

    constexpr std::size_t k_stripe_rows = 1000;
    constexpr std::size_t k_record_count = k_stripe_rows + 1;
    constexpr std::size_t k_record_bp = 80;
    const std::string path = "/tmp/bioseq_load_stripe_flush.fasta";
    std::vector<std::pair<std::string, std::string>> records;
    records.reserve(k_record_count);
    for (std::size_t row = 0; row < k_record_count; ++row) {
        records.emplace_back(
            fmt::format("row_{}", row),
            gen_bases(k_record_bp, 1000u + row, /*with_n=*/false));
    }
    write_fasta(path, records);

    it::exec_sql(ctx,
        "SET bioseq.storage.stripe.row_limit = 1000; "
        "SET bioseq.index.insert_maintenance.enabled = off; "
        "CREATE TABLE load_stripe_flush "
        "(header text, seq bioseq.seq('dna4', 64)) USING bioseq;");

    REQUIRE(it::query_scalar_int(
        ctx, "SELECT bioseq.load('load_stripe_flush'::regclass, '" + path + "');")
        == static_cast<long long>(k_record_count));

    // Loading k_stripe_rows+1 records forces the pending-write flush to reset
    // the stripe scratch context and then writes one post-reset record.
    // Source-epoch cardinality verifies that every record published; the final
    // mmsearch verifies that the post-reset locator resolves to its payload.
    REQUIRE(it::query_scalar_int(ctx,
        "SELECT count(*) FROM bioseq_internal.sequence_source_version "
        "WHERE user_relid='load_stripe_flush'::regclass AND state=1")
        == static_cast<long long>(k_record_count));
    const auto final_row = it::query_rows(ctx, fmt::format(
        "SELECT header,seq::text FROM load_stripe_flush "
        "WHERE header='row_1000' AND mmsearch(seq, "
        "'{}'::bioseq.seq('dna4',64))",
        records.back().second));
    REQUIRE(final_row.size() == 1);
    REQUIRE(final_row[0].size() == 2);
    REQUIRE(final_row[0][0].value() == "row_1000");
    REQUIRE(final_row[0][1].value() == records.back().second);
}

TEST_CASE("bioseq.load: width-agnostic equivalence (dna4/32 == dna4/64)",
          "[component:bioseq][layer:integration][feature:io][resource:postgres][resource:filesystem]") {
    it::connection_test_context ctx;
    it::reset_extension(ctx, { std::string_view{"load_width_32"}, std::string_view{"load_width_64"} });

    const std::string body = gen_bases(6000, 77, false);
    const std::string path = "/tmp/bioseq_load_width.fasta";
    write_fasta(path, {{"width_record", body}});

    it::exec_sql(ctx, "CREATE TABLE load_width_32 (header text, seq bioseq.seq('dna4', 32)) USING bioseq;");
    it::exec_sql(ctx, "CREATE TABLE load_width_64 (header text, seq bioseq.seq('dna4', 64)) USING bioseq;");
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.load('load_width_32'::regclass, '" + path + "');") == 1);
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.load('load_width_64'::regclass, '" + path + "');") == 1);

    // Decode parity: the byte-slice streaming path must be width-correct.
    REQUIRE(it::query_scalar(ctx, "SELECT seq::text FROM load_width_32") == body);
    REQUIRE(it::query_scalar(ctx, "SELECT seq::text FROM load_width_64") == body);

    // mmsearch must return the IDENTICAL position in both widths (the
    // logical sequence + minimizers are width-agnostic), not merely non-empty.
    const std::string query = body.substr(500, 800);
    std::string pos32, pos64;
    for (const char* table_name : {"load_width_32", "load_width_64"}) {
        const std::string width =
            (std::string(table_name) == "load_width_32") ? "32" : "64";
        const auto rows = it::query_rows(
            ctx, fmt::format("SELECT bioseq.target_start(header) FROM {} WHERE "
                             "mmsearch(seq, '{}'::bioseq.seq('dna4', {}))",
                             table_name, query, width));
        REQUIRE(rows.size() == 1);
        (width == "32" ? pos32 : pos64) = rows[0][0].value();
    }
    REQUIRE(pos32 == pos64);   // identical target_start across storage widths
}

TEST_CASE("bioseq.load: active-profile alphabets round-trip and mmsearch across widths 8/16/32/64",
          "[component:bioseq][layer:integration][feature:io][resource:postgres][resource:filesystem]") {
    // The storage-unit-parametric streaming reader must be width-correct.
    // Each width gets its OWN connection so the DROP/CREATE EXTENSION reset is
    // isolated per typmod. Decoding is checked with a plain projection; mmsearch
    // is checked separately with an explicit literal query through the SQL pipeline.
#if defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
    const std::string body5 = gen_bases(3000, 99, /*with_n=*/true);   // dna5 + sparse N
    const std::string path5 = "/tmp/bioseq_load_dna5.fasta";
    write_fasta(path5, {{"dna5_record", body5}});
#endif
    const std::string body4 = gen_bases(3000, 100, /*with_n=*/false); // dna4
    const std::string path4 = "/tmp/bioseq_load_dna4_width.fasta";
    write_fasta(path4, {{"dna4_record", body4}});

    struct alphabet_case { const char* alpha; const std::string& body; const std::string& path; };
#if defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
    const alphabet_case dna5{"dna5", body5, path5};
#endif
    const alphabet_case dna4{"dna4", body4, path4};
#if defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
    for (const alphabet_case* current_case : {&dna5, &dna4}) {
#else
    for (const alphabet_case* current_case : {&dna4}) {
#endif
        for (const char* width : {"8", "16", "32", "64"}) {
            it::connection_test_context ctx;                 // fresh backend per width
            it::reset_extension(ctx, { std::string_view{"load_active_profile_width"} });
            it::exec_sqlf(ctx, "CREATE TABLE load_active_profile_width (header text, seq bioseq.seq('{}', {})) USING bioseq;",
                          current_case->alpha, width);
            REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.load('load_active_profile_width'::regclass, '" + current_case->path + "');") == 1);
            // load + decode round-trips byte-exactly (incl. N).
            const auto rows = it::query_rows(ctx,
                "SELECT seq::text, bioseq.len(seq) FROM load_active_profile_width");
            REQUIRE(rows.size() == 1);
            REQUIRE(rows[0][0].value() == current_case->body); // width-correct decode, N preserved
            REQUIRE(rows[0][1].value() == std::to_string(current_case->body.size()));

            // Literal mmsearch is non-empty for every alphabet/width pair.
            const std::string query = current_case->body.substr(100, 800);
            const auto hits = it::query_rows(
                ctx, fmt::format("SELECT header FROM load_active_profile_width WHERE "
                                 "mmsearch(seq, '{}'::bioseq.seq('{}', {}))",
                                 query, current_case->alpha, width));
            REQUIRE_FALSE(hits.empty());
        }
    }
}

TEST_CASE("bioseq.load: single large record streams without a >MaxAllocSize varlena (memory-bound)",
          "[component:bioseq][layer:integration][feature:io][resource:postgres][resource:filesystem][resource:large_memory]") {
    // A single-pass streaming invariant: a single record larger than what a non-streaming load
    // could palloc as ONE varlena (>MaxAllocSize ≈ 1 GiB packed; ~4 Gbp for dna4/64)
    // must still load via the fragmented streaming path, with bounded backend RSS
    // (no `_seq_storage`-sized allocation). The large-memory resource tag marks
    // this heavy case. `BIOSEQ_LOAD_MEMBOUND_GBP` sizes it: the default 0.1 Gbp
    // fits any host and exercises the single-large-record streaming path; setting
    // a value of at least 4 verifies the >MaxAllocSize guarantee on a large host (a
    // non-streaming load would ERROR with "invalid memory alloc request size", and
    // `seq::text` on the whole record would too — so we verify via `bioseq.len`,
    // which reads only the stub, never assembling the varlena).
    const char* env = std::getenv("BIOSEQ_LOAD_MEMBOUND_GBP");
    const double gbp = (env && *env) ? std::atof(env) : 0.1;
    const std::size_t n = static_cast<std::size_t>(gbp * 1e9);
    const std::string path = "/tmp/bioseq_load_memory_bound.fasta";
    write_generated_fasta(
        path, "large_record", n, 4242, /*with_n=*/false); // one dna4 record

    it::connection_test_context ctx;
    it::reset_extension(ctx, { std::string_view{"load_memory_bound"} });
    it::exec_sql(ctx, "CREATE TABLE load_memory_bound (header text, seq bioseq.seq('dna4', 64)) USING bioseq;");
    it::exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
    // A non-streaming load would have palloc'd the whole record (>MaxAllocSize at
    // >=4 Gbp) and errors before returning; success plus the exact bp count verifies
    // the record streamed through bounded fragments.
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.load('load_memory_bound'::regclass, '" + path + "');") == 1);
    REQUIRE(it::query_scalar(ctx, "SELECT bioseq.len(seq)::text FROM load_memory_bound") == std::to_string(n));
}

TEST_CASE("bioseq.load: index parity (on builds index visible same-statement; off defers)",
          "[component:bioseq][layer:integration][feature:io][resource:postgres][resource:filesystem]") {
    it::connection_test_context ctx;
    it::reset_extension(ctx, { std::string_view{"load_index_on"},
                               std::string_view{"load_index_off"},
                               std::string_view{"load_index_sequence"} });

    const std::string body = gen_bases(5000, 555, false);
    const std::string path = "/tmp/bioseq_load_index.fasta";
    write_fasta(path, {{"index_record", body}});

    // ON: index built in the batched pass; an mmsearch self-hit returns the
    // exact {len}M CIGAR in the very next statement, covering the parent and
    // sub-table write-state flush.
    it::exec_sql(ctx, "CREATE TABLE load_index_on (header text, seq bioseq.seq('dna4', 64)) USING bioseq;");
    it::exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = on;");
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.load('load_index_on'::regclass, '" + path + "');") == 1);
    REQUIRE(it::index_table_row_count(ctx, "minimizers_per_fragment", "load_index_on") > 0);
    REQUIRE(it::index_table_row_count(ctx, "symbol_counts", "load_index_on") > 0);
    {
        const auto cigar_rows = it::query_rows(
            ctx, fmt::format("SELECT bioseq.cigar(header) FROM load_index_on "
                             "WHERE mmsearch(seq, '{}'::bioseq.seq('dna4', 64))", body));
        REQUIRE(cigar_rows.size() == 1);
        REQUIRE(it::is_exact_match_cigar(cigar_rows[0][0].value(),
                                         static_cast<long long>(body.size())));
    }

    // OFF: load builds NO on-insert index; row count stays 0 until an explicit
    // update_index, after which the self-hit still works.
    it::exec_sql(ctx, "CREATE TABLE load_index_off (header text, seq bioseq.seq('dna4', 64)) USING bioseq;");
    it::exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.load('load_index_off'::regclass, '" + path + "');") == 1);
    REQUIRE(it::index_table_row_count(ctx, "minimizers_per_fragment", "load_index_off") == 0);
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.update_index('load_index_off'::regclass, 'minimizers_per_fragment');") > 0);
    REQUIRE(it::index_table_row_count(ctx, "minimizers_per_fragment", "load_index_off") > 0);

    // Per-sequence ON: policy-selected batch load must build
    // minimizers_per_sequence directly and must not leave a residual
    // per-fragment index behind as an accidental fallback.
    it::exec_sql(ctx, "CREATE TABLE load_index_sequence "
                      "(header text, seq bioseq.seq('dna4', 64)) USING bioseq;");
    it::exec_sql(ctx, "SELECT bioseq.set_index_policy("
                      "'load_index_sequence'::regclass, "
                      "'minimizers_per_fragment', 'UPDATE_MANUALLY');");
    it::exec_sql(ctx, "SELECT bioseq.set_index_policy("
                      "'load_index_sequence'::regclass, "
                      "'minimizers_per_sequence', 'UPDATE_ON_INSERT');");
    it::exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = on;");
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.load('load_index_sequence'::regclass, '" + path + "');") == 1);
    REQUIRE(it::index_table_row_count(ctx, "minimizers_per_fragment", "load_index_sequence") == 0);
    REQUIRE(it::index_table_row_count(ctx, "minimizers_per_sequence", "load_index_sequence") > 0);
    it::exec_sql(ctx, "SELECT bioseq.set_active_read_index("
                      "'load_index_sequence'::regclass, 'minimizers_per_sequence');");
    it::exec_sql(ctx, "SET bioseq.index.sequence_read.enabled = on;");
    {
        const auto cigar_rows = it::query_rows(
            ctx, fmt::format("SELECT bioseq.cigar(header) FROM load_index_sequence "
                             "WHERE mmsearch(seq, '{}'::bioseq.seq('dna4', 64))", body));
        REQUIRE(cigar_rows.size() == 1);
        REQUIRE(it::is_exact_match_cigar(cigar_rows[0][0].value(),
                                         static_cast<long long>(body.size())));
    }
}

TEST_CASE("bioseq.load: hash-major minimizers_per_sequence uses the post-load locator rebuild path",
          "[component:bioseq][layer:integration][feature:io][resource:postgres][resource:filesystem]") {
    constexpr std::string_view table = "load_hash_major_ready";
    constexpr std::string_view rollback_table = "load_hash_major_rollback";
    it::connection_test_context ctx;
    it::reset_extension(ctx, {table, rollback_table});

    const std::string body = gen_bases(65565, 0x173, false);
    const std::string path = "/tmp/bioseq_load_hash_major.fasta";
    write_fasta(path, {{"hash_major", body}});

    it::exec_sqlf(ctx,
        "CREATE TABLE {} "
        "(header text, seq bioseq.seq('dna4', 64)) USING bioseq;",
        table);
    it::exec_sqlf(ctx,
        "SELECT bioseq.set_index_policy('{}'::regclass, "
        "'minimizers_per_fragment', 'UPDATE_MANUALLY');",
        table);
    it::exec_sqlf(ctx,
        "SELECT bioseq.set_index_policy('{}'::regclass, "
        "'minimizers_per_sequence', 'UPDATE_ON_INSERT');",
        table);
    it::exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = on;");
    // The registered production DNA4/15/10 profile publishes hash-major
    // minimizers_per_sequence data.
    it::exec_sql(ctx,
        "SET bioseq.aligner.grouped_soa_generation.enabled = on;");
    it::exec_sql(ctx, "SET bioseq.index.async_persistence.enabled = off;");
    it::exec_sql(ctx, "SET bioseq.index.recompute_async_persistence.enabled = off;");
    it::exec_sql(ctx,
        "SET bioseq.aligner.minimizer.algorithm = 'scalar';");

    // load bypasses per-tuple index maintenance, publishes all source
    // locators, flushes its write states, then intentionally invokes the same
    // guarded bioseq.update_index route exercised by this scenario.
    REQUIRE(it::query_scalar_int(ctx, fmt::format(
        "SELECT bioseq.load('{}'::regclass, '{}');", table, path)) == 1);
    REQUIRE(it::query_scalar_int(ctx, fmt::format(
        "SELECT count(*) FROM bioseq_internal.sequence_source_version "
        "WHERE user_relid='{}'::regclass AND state=1;", table)) == 1);
    const auto rowno = it::query_scalar_int(ctx, fmt::format(
        "SELECT bioseq.column_row_number('{}'::regclass, ctid) "
        "FROM {} ORDER BY ctid LIMIT 1;",
        table,
        table));
    REQUIRE(rowno >= 0);
    const auto inspected = it::query_one(ctx, fmt::format(
        "WITH inspected AS ("
        " SELECT bioseq.inspect_ready_hash_major_minimizer_generation("
        "  '{}'::regclass,'minimizers_per_sequence',{}) AS j"
        ") "
        "SELECT "
        " j->>'layout',"
        " j->'head'->>'generation_id',"
        " j->'source'->>'sequence_rowno',"
        " j->'source'->>'total_bp_size',"
        " j->'manifest'->'compatibility'->>'kmer_size',"
        " j->'manifest'->'compatibility'->>'window_size',"
        " j->'manifest'->'compatibility'->>'stored_posting_width_bytes',"
        " j->'manifest'->'compatibility'->>'producer_backend_identity',"
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
        rowno));
    REQUIRE(inspected.size() == 11);
    for (const auto& field : inspected) {
        REQUIRE(field.has_value());
    }
    REQUIRE(inspected[0].value() == "hash_major");
    REQUIRE(std::stoll(inspected[1].value()) > 0);
    REQUIRE(inspected[2].value() == std::to_string(rowno));
    REQUIRE(inspected[3].value() == "65565");
    REQUIRE(inspected[4].value() == "15");
    REQUIRE(inspected[5].value() == "10");
    REQUIRE(inspected[6].value() == "4");
    REQUIRE(inspected[7].value() == "1");
    REQUIRE(inspected[8].value() == "1");
    REQUIRE(inspected[9].value() == "1");
    REQUIRE(inspected[10].value() == "1");
    REQUIRE(it::index_table_row_count(
        ctx, "minimizers_per_fragment", table) == 0);

    // A requested post-load index is part of load's atomic success contract.
    // Re-enable a forbidden async writer: update_index must fail through SPI,
    // bioseq.load must propagate that exact failure, and the streamed parent
    // row/source fragments must roll back with the failed index maintenance.
    it::exec_sqlf(ctx,
                  "CREATE TABLE {} "
                  "(header text, seq bioseq.seq('dna4', 64)) USING bioseq;",
                  rollback_table);
    it::exec_sqlf(ctx,
                  "SELECT bioseq.set_index_policy('{}'::regclass, "
                  "'minimizers_per_fragment', 'UPDATE_MANUALLY');",
                  rollback_table);
    it::exec_sqlf(ctx,
                  "SELECT bioseq.set_index_policy('{}'::regclass, "
                  "'minimizers_per_sequence', 'UPDATE_ON_INSERT');",
                  rollback_table);
    const long long rollback_relfilenode = it::query_scalar_int(
        ctx, fmt::format("SELECT pg_relation_filenode('{}'::regclass)::bigint;", rollback_table));
    const auto source_catalog_row_count = [&ctx, &rollback_relfilenode, &rollback_table] {
        return it::query_scalar_int(
            ctx,
            fmt::format("SELECT count(*) FROM bioseq_internal.sequence_source_version "
                        "WHERE user_relid='{}'::regclass "
                        "AND main_relfilenode={}::oid;",
                        rollback_table,
                        rollback_relfilenode));
    };
    const auto source_shard_catalog_count = [&ctx, &rollback_relfilenode] {
        return it::query_scalar_int(
            ctx,
            fmt::format("SELECT count(*) FROM bioseq_internal.seq_sub_table "
                        "WHERE main_relfilenode={}::oid;",
                        rollback_relfilenode));
    };
    const auto source_fragment_relation_count = [&ctx, &rollback_relfilenode] {
        return it::query_scalar_int(
            ctx,
            fmt::format("SELECT count(*) FROM pg_class "
                        "WHERE relnamespace='bioseq_internal'::regnamespace "
                        "AND (relname ~ '^seq_sub_[0-9]+_[0-9]+_{}_' "
                        "OR relname ~ '^idx_sidecar_[0-9]+_[0-9]+_{}_');",
                        rollback_relfilenode,
                        rollback_relfilenode));
    };
    const auto index_shard_catalog_count = [&ctx, &rollback_relfilenode] {
        return it::query_scalar_int(
            ctx,
            fmt::format("SELECT count(*) FROM bioseq_internal.index_table_shard "
                        "WHERE main_relfilenode={}::oid;",
                        rollback_relfilenode));
    };
    const auto metadata_interval_count = [&ctx, &rollback_relfilenode, &rollback_table] {
        return it::query_scalar_int(
            ctx,
            fmt::format("SELECT count(*) FROM bioseq_internal.metadata_age_log "
                        "WHERE user_relid='{}'::regclass "
                        "AND main_relfilenode={}::oid;",
                        rollback_table,
                        rollback_relfilenode));
    };
    const auto generation_relation_count = [&ctx] {
        return it::query_scalar_int(ctx,
                                    "SELECT count(*) FROM pg_class "
                                    "WHERE relnamespace='bioseq_internal'::regnamespace "
                                    "AND relname LIKE 'seqidx_mps_%';");
    };
    const long long parent_rows_before =
        it::query_scalar_int(ctx, fmt::format("SELECT count(*) FROM {};", rollback_table));
    const long long source_catalog_rows_before = source_catalog_row_count();
    const long long source_shards_before = source_shard_catalog_count();
    const long long source_fragment_relations_before = source_fragment_relation_count();
    const long long index_shards_before = index_shard_catalog_count();
    const long long metadata_intervals_before = metadata_interval_count();
    const long long generation_relations_before = generation_relation_count();
    const long long index_rows_before =
        it::index_table_row_count(ctx, "minimizers_per_sequence", rollback_table);
    it::exec_sql(ctx, "SET bioseq.index.async_persistence.enabled = on;");
    const auto load_error = it::sql_error(
        ctx, fmt::format("SELECT bioseq.load('{}'::regclass, '{}');", rollback_table, path));
    REQUIRE(load_error.sqlstate == "0A000");
    REQUIRE(load_error.contains(
        "requires asynchronous persistence to be disabled"));
    REQUIRE(it::query_scalar_int(ctx, fmt::format(
        "SELECT count(*) FROM {};", rollback_table)) == parent_rows_before);
    REQUIRE(source_catalog_row_count() == source_catalog_rows_before);
    REQUIRE(source_shard_catalog_count() == source_shards_before);
    REQUIRE(source_fragment_relation_count()
            == source_fragment_relations_before);
    REQUIRE(index_shard_catalog_count() == index_shards_before);
    REQUIRE(metadata_interval_count() == metadata_intervals_before);
    REQUIRE(generation_relation_count() == generation_relations_before);
    REQUIRE(it::index_table_row_count(
        ctx, "minimizers_per_sequence", rollback_table) == index_rows_before);
}

TEST_CASE("bioseq.load: column-rule + malformed-file errors", "[component:bioseq][layer:integration][feature:io][resource:postgres][resource:filesystem]") {
    it::connection_test_context ctx;
    it::reset_extension(ctx, {});

    const std::string body = gen_bases(500, 1, false);
    const std::string path = "/tmp/bioseq_load_errors.fasta";
    write_fasta(path, {{"error_record", body}});

    // 0 seq columns.
    it::exec_sql(ctx, "DROP TABLE IF EXISTS load_error_case CASCADE;");
    it::exec_sql(ctx, "CREATE TABLE load_error_case (header text) USING bioseq;");
    {
        const auto err =
            it::sql_error(ctx, "SELECT bioseq.load('load_error_case'::regclass, '" + path + "');");
        REQUIRE(err.sqlstate == "42P16");
        REQUIRE(err.contains("no bioseq.seq column"));
    }

    // 2 seq columns.
    it::exec_sql(ctx, "DROP TABLE IF EXISTS load_error_case CASCADE;");
    it::exec_sql(ctx, "CREATE TABLE load_error_case (sequence_primary bioseq.seq('dna4',64), sequence_secondary bioseq.seq('dna4',64)) USING bioseq;");
    {
        const auto err =
            it::sql_error(ctx, "SELECT bioseq.load('load_error_case'::regclass, '" + path + "');");
        REQUIRE(err.sqlstate == "42P16");
        REQUIRE(err.contains("bioseq.seq columns"));
    }

    // Two text columns, neither a primary key, and no mapping are ambiguous.
    // Multi-column identifier resolution asks the user to disambiguate.
    it::exec_sql(ctx, "DROP TABLE IF EXISTS load_error_case CASCADE;");
    it::exec_sql(ctx, "CREATE TABLE load_error_case (header_primary text, header_secondary text, seq bioseq.seq('dna4',64)) USING bioseq;");
    {
        const auto err =
            it::sql_error(ctx, "SELECT bioseq.load('load_error_case'::regclass, '" + path + "');");
        REQUIRE(err.sqlstate == "42P16");
        REQUIRE(err.contains("text columns"));
        REQUIRE(err.contains("disambiguate"));
    }

    // bigserial PK is NOT identity/generated — it carries a DEFAULT nextval(),
    // whose default is evaluated per row. The load succeeds and the PK auto-increments.
    it::exec_sql(ctx, "DROP TABLE IF EXISTS load_error_case CASCADE;");
    it::exec_sql(ctx, "CREATE TABLE load_error_case (id bigserial primary key, "
                      "header text, seq bioseq.seq('dna4',64)) USING bioseq;");
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.load('load_error_case'::regclass, '" + path + "');") == 1);
    {
        const auto rows = it::query_rows(ctx, "SELECT id, header FROM load_error_case");
        REQUIRE(rows.size() == 1);
        REQUIRE(rows[0][0].value() == "1");   // nextval default evaluated per row
        REQUIRE(rows[0][1].value() == "error_record"); // sole text column = identifier
    }

    it::exec_sql(ctx, "DROP TABLE IF EXISTS load_error_case CASCADE;");
    it::exec_sql(ctx, "CREATE TABLE load_error_case (id bigint GENERATED ALWAYS AS IDENTITY, "
                      "header text, seq bioseq.seq('dna4',64)) USING bioseq;");
    {
        const auto err =
            it::sql_error(ctx, "SELECT bioseq.load('load_error_case'::regclass, '" + path + "');");
        REQUIRE(err.sqlstate == "42P16");
        REQUIRE(err.contains("identity"));
    }

    it::exec_sql(ctx, "DROP TABLE IF EXISTS load_error_case CASCADE;");
    it::exec_sql(ctx, "CREATE TABLE load_error_case (header text, seq bioseq.seq('dna4',64), "
                      "header_len int GENERATED ALWAYS AS (length(header)) STORED) USING bioseq;");
    {
        const auto err =
            it::sql_error(ctx, "SELECT bioseq.load('load_error_case'::regclass, '" + path + "');");
        REQUIRE(err.sqlstate == "42P16");
        REQUIRE(err.contains("generated"));
    }

    // Unrecognised extension (file_utils).
    it::exec_sql(ctx, "DROP TABLE IF EXISTS load_error_case CASCADE;");
    it::exec_sql(ctx, "CREATE TABLE load_error_case (header text, seq bioseq.seq('dna4',64)) USING bioseq;");
    {
        const auto err =
            it::sql_error(ctx, "SELECT bioseq.load('load_error_case'::regclass, '/tmp/whatever.txt');");
        REQUIRE(err.sqlstate == "22023");
        REQUIRE(err.contains("unrecognised sequence file extension"));
    }

    // Recognised extension, missing file.
    {
        const auto err =
            it::sql_error(ctx, "SELECT bioseq.load('load_error_case'::regclass, '/tmp/bioseq_missing_file.fasta');");
        REQUIRE(err.sqlstate == "22000");
        REQUIRE(err.contains("bioseq.load"));
    }

    // Malformed FASTA -> data exception + rollback (table stays empty).
    const std::string bad_path = "/tmp/bioseq_load_malformed.fasta";
    {
        std::ofstream f(bad_path, std::ios::binary | std::ios::trunc);
        f << "not a fasta file at all\nrandom bytes\n";
    }
    {
        const auto err =
            it::sql_error(ctx, "SELECT bioseq.load('load_error_case'::regclass, '" + bad_path + "');");
        REQUIRE(err.sqlstate == "22000");
        REQUIRE(err.contains("bioseq.load"));
    }
    // Rollback: no partially-written parent row should be visible.
    REQUIRE(it::query_rows(ctx, "SELECT header FROM load_error_case").empty());
}

TEST_CASE("bioseq.load: typed multi-column header parsing + non-empty mmsearch",
          "[component:bioseq][layer:integration][feature:io][resource:postgres][resource:filesystem]") {
    it::connection_test_context ctx;
    it::reset_extension(ctx, { std::string_view{"uniprot_records"}, std::string_view{"genbank_records"},
                               std::string_view{"imgt_records"}, std::string_view{"load_defaulted_fields"},
                               std::string_view{"load_missing_default"}, std::string_view{"load_bad_coercion"},
                               std::string_view{"influenza_records"} });
    it::exec_sql(ctx, "SET DateStyle = 'ISO, MDY';");   // date input assumes MDY
    const std::string backend_pid =
        it::query_scalar(ctx, "SELECT pg_backend_pid()::text;");

    const std::string body = gen_bases(2000, 4242, /*with_n=*/false);
    const std::string query = body.substr(0, 1200);

    // Fixed UniProt headers map positional accession, key-value tags, and typed
    // integers, verified with a non-empty mmsearch on the loaded sequence.
    {
        const std::string path = "/tmp/bioseq_load_uniprot.fasta";
        write_fasta(path, {{"tr|R9RJF1|R9RJF1_PSEAI Mandelate racemase "
                            "OS=Pseudomonas aeruginosa PE=4 SV=1", body}});
        it::exec_sql(ctx, "CREATE TABLE uniprot_records (accession text, organism_name text, "
                          "sv int, seq bioseq.seq('dna4',64)) USING bioseq;");
        REQUIRE(it::query_scalar_int(ctx,
            "SELECT bioseq.load('uniprot_records'::regclass, '" + path +
            "', header_format => 'uni_prot');") == 1);
        const auto rows = it::query_rows(ctx,
            "SELECT accession, organism_name, sv FROM uniprot_records");
        REQUIRE(rows.size() == 1);
        REQUIRE(rows[0][0].value() == "R9RJF1");
        REQUIRE(rows[0][1].value() == "Pseudomonas aeruginosa");
        REQUIRE(rows[0][2].value() == "1");                 // int4in coerced

        // A 1200 bp prefix must map back to the loaded row.
        const auto hits = it::query_rows(ctx, fmt::format(
            "SELECT accession FROM uniprot_records WHERE "
            "mmsearch(seq, '{}'::bioseq.seq('dna4', 64))", query));
        REQUIRE_FALSE(hits.empty());
        REQUIRE(hits[0][0].value() == "R9RJF1");

        // SPI_finish destroys its private procedure context. Post-load index
        // maintenance and the active-read resolver must both restore the
        // pre-connect caller context, otherwise the first search may appear to
        // work while poisoning this same backend for its next allocation.
        REQUIRE(it::query_scalar(ctx, "SELECT pg_backend_pid()::text;")
                == backend_pid);
        REQUIRE(it::query_scalar_int(ctx, "SELECT 1;") == 1);
        const auto repeated_hits = it::query_rows(ctx, fmt::format(
            "SELECT accession FROM uniprot_records WHERE "
            "mmsearch(seq, '{}'::bioseq.seq('dna4', 64))", query));
        REQUIRE_FALSE(repeated_hits.empty());
        REQUIRE(repeated_hits[0][0].value() == "R9RJF1");
        REQUIRE(it::query_scalar(ctx, "SELECT pg_backend_pid()::text;")
                == backend_pid);

        // The active-read policy is trusted extension metadata.  Ordinary
        // readers are intentionally denied bioseq_internal privileges, so
        // direct catalog resolution must rely on the already-authorized user
        // relation instead of imposing an internal-table ACL requirement.
        // Execute the real mmsearch path as a non-owner with SELECT only on
        // the user table, exposing owner-only resolver errors that a
        // superuser integration connection would conceal.
        it::exec_sql(ctx,
            "DROP ROLE IF EXISTS bioseq_load_restricted_reader; "
            "CREATE ROLE bioseq_load_restricted_reader NOLOGIN; "
            "GRANT SELECT ON uniprot_records TO bioseq_load_restricted_reader; "
            "REVOKE ALL ON SCHEMA bioseq_internal "
            "FROM bioseq_load_restricted_reader; "
            "REVOKE ALL ON bioseq_internal.index_policy "
            "FROM bioseq_load_restricted_reader;");
        REQUIRE(it::query_scalar(ctx,
            "SELECT has_schema_privilege('bioseq_load_restricted_reader', "
            "'bioseq_internal', 'USAGE')::text;") == "false");
        REQUIRE(it::query_scalar(ctx,
            "SELECT has_table_privilege('bioseq_load_restricted_reader', "
            "'bioseq_internal.index_policy', 'SELECT')::text;") == "false");
        it::exec_sql(ctx, "SET ROLE bioseq_load_restricted_reader;");
        const auto reader_hits = it::query_rows(ctx, fmt::format(
            "SELECT accession FROM uniprot_records WHERE "
            "mmsearch(seq, '{}'::bioseq.seq('dna4', 64))", query));
        REQUIRE_FALSE(reader_hits.empty());
        REQUIRE(reader_hits[0][0].value() == "R9RJF1");
        it::exec_sql(ctx,
            "RESET ROLE; "
            "REVOKE SELECT ON uniprot_records FROM bioseq_load_restricted_reader; "
            "DROP ROLE bioseq_load_restricted_reader;");
    }

    // Auto-detected GenBank bracket tags map to integer and text columns.
    {
        const std::string path = "/tmp/bioseq_load_genbank.fasta";
        write_fasta(path, {{"dna_new [organism=Homo sapiens] [chromosome=17] "
                            "[moltype=mRNA]", body}});
        it::exec_sql(ctx, "CREATE TABLE genbank_records (id text, organism text, chromosome int, "
                          "moltype text, seq bioseq.seq('dna4',64)) USING bioseq;");
        REQUIRE(it::query_scalar_int(ctx,
            "SELECT bioseq.load('genbank_records'::regclass, '" + path + "');") == 1);
        const auto rows = it::query_rows(ctx,
            "SELECT id, organism, chromosome, moltype FROM genbank_records");
        REQUIRE(rows.size() == 1);
        REQUIRE(rows[0][0].value() == "dna_new");
        REQUIRE(rows[0][1].value() == "Homo sapiens");
        REQUIRE(rows[0][2].value() == "17");                // int4in
        REQUIRE(rows[0][3].value() == "mRNA");
    }

    // IMGT positional 'a..b' values map to int4range.
    {
        const std::string path = "/tmp/bioseq_load_imgt.fasta";
        write_fasta(path, {{"M99641|IGHV1-18*01|Homo sapiens|F|V-REGION|188..483|"
                            "296 nt|1| | | | |296+24=320| | ", body}});
        it::exec_sql(ctx, "CREATE TABLE imgt_records (ligm_accession text, species text, "
                          "positions int4range, seq bioseq.seq('dna4',64)) USING bioseq;");
        REQUIRE(it::query_scalar_int(ctx,
            "SELECT bioseq.load('imgt_records'::regclass, '" + path +
            "', header_format => 'imgt');") == 1);
        const auto rows = it::query_rows(ctx,
            "SELECT ligm_accession, species, lower(positions), upper(positions) FROM imgt_records");
        REQUIRE(rows.size() == 1);
        REQUIRE(rows[0][0].value() == "M99641");
        REQUIRE(rows[0][1].value() == "Homo sapiens");
        REQUIRE(rows[0][2].value() == "188");
        REQUIRE(rows[0][3].value() == "484");               // int4range canonical [188,484)
    }

    // Unmapped NOT NULL columns use constant and volatile defaults.
    {
        const std::string path = "/tmp/bioseq_load_defaulted_fields.fasta";
        write_fasta(path, {{"tr|X1|X1_HUMAN p OS=Homo sapiens PE=1 SV=1", body}});
        it::exec_sql(ctx, "CREATE TABLE load_defaulted_fields (accession text, "
                          "score int NOT NULL DEFAULT 7, "
                          "ts timestamptz NOT NULL DEFAULT now(), "
                          "seq bioseq.seq('dna4',64)) USING bioseq;");
        REQUIRE(it::query_scalar_int(ctx,
            "SELECT bioseq.load('load_defaulted_fields'::regclass, '" + path +
            "', header_format => 'uni_prot');") == 1);
        const auto rows = it::query_rows(ctx,
            "SELECT accession, score, (ts IS NOT NULL)::text FROM load_defaulted_fields");
        REQUIRE(rows.size() == 1);
        REQUIRE(rows[0][0].value() == "X1");
        REQUIRE(rows[0][1].value() == "7");                 // constant default evaluated
        REQUIRE(rows[0][2].value() == "true");              // volatile now() per row
    }

    // An unmapped NOT NULL column without a default fails before loading.
    {
        const std::string path = "/tmp/bioseq_load_missing_default.fasta";
        write_fasta(path, {{"tr|X1|X1_HUMAN p OS=Homo sapiens PE=1 SV=1", body}});
        it::exec_sql(ctx, "CREATE TABLE load_missing_default (accession text, required_value int NOT NULL, "
                          "seq bioseq.seq('dna4',64)) USING bioseq;");
        const auto err = it::sql_error(ctx,
            "SELECT bioseq.load('load_missing_default'::regclass, '" + path +
            "', header_format => 'uni_prot');");
        REQUIRE(err.sqlstate == "42P16");
        REQUIRE(err.contains("NOT NULL"));
        REQUIRE(err.contains("no default"));
    }

    // Bad scalar coercion reports the input error with record and column context.
    {
        const std::string path = "/tmp/bioseq_load_bad_coercion.fasta";
        write_fasta(path, {{"tr|X1|X1_HUMAN p OS=Homo sapiens PE=1 SV=abc", body}});
        it::exec_sql(ctx, "CREATE TABLE load_bad_coercion (accession text, sv int, "
                          "seq bioseq.seq('dna4',64)) USING bioseq;");
        const auto err = it::sql_error(ctx,
            "SELECT bioseq.load('load_bad_coercion'::regclass, '" + path +
            "', header_format => 'uni_prot');");
        REQUIRE(err.sqlstate == "22P02");                   // invalid_text_representation
        REQUIRE(err.contains("invalid input syntax"));      // int4in's own message
        // The "record N, column \"sv\"" error context lands in libpq's
        // PG_DIAG_CONTEXT field, which sql_error() does not fold into `message`;
        // it is shown in psql's CONTEXT line.
    }

    // Mixed numeric and named columns exercise enum and date coercion.
    {
        const std::string path = "/tmp/bioseq_load_influenza.fasta";
        write_fasta(path, {{"A/Canoas/LACENRS_1793/2015|A|H3N2|07/17/2015||Brazil|"
                            "Human|KY925125", body}});
        it::exec_sql(ctx, "DROP TYPE IF EXISTS influenza_type CASCADE;");
        it::exec_sql(ctx, "CREATE TYPE influenza_type AS ENUM ('A','B');");
        it::exec_sql(ctx, "CREATE TABLE influenza_records (name text, strain influenza_type, dt date, "
                          "seq bioseq.seq('dna4',64)) USING bioseq;");
        REQUIRE(it::query_scalar_int(ctx,
            "SELECT bioseq.load('influenza_records'::regclass, '" + path +
            "', columns => ARRAY[('name','0'),('strain','1'),('dt','3')]"
            "::bioseq.column_mapping[]);") == 1);
        const auto rows = it::query_rows(ctx,
            "SELECT name, strain::text, dt::text FROM influenza_records");
        REQUIRE(rows.size() == 1);
        REQUIRE(rows[0][0].value() == "A/Canoas/LACENRS_1793/2015");
        REQUIRE(rows[0][1].value() == "A");                 // enum_in label
        REQUIRE(rows[0][2].value() == "2015-07-17");        // date_in, MDY
    }
}
