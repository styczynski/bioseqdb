/**
 * @file bioseq/tests/integration/load_test.cpp
 * @brief Tests loading sequence files into Bioseq tables
 *
 * Checks that loaded sequences can  be read and searched across storage widths,
 * including records split over sezeral fragments. Also tests index creation,
 * header fields and defaults, and rollback when loading fails.
 */
#include <cstdlib>
#include <libpq-fe.h>
#include <zlib.h>
#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

import std;
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

/// Write one generated FASTA record without keeping its complete body in memory.
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
    // Exercise two full storage fragments and a partial final fragment.
    const std::string big     = gen_bases(k_dna4_u64_fragment_bp * 2 + 5000, 33, false);
    const std::string path = "/tmp/bioseq_load_roundtrip.fasta";
    write_fasta(path, {{"rec_small", small}, {"rec_medium", medium}, {"rec_big", big}});

    it::exec_sql(ctx, "CREATE TABLE load_roundtrip (header text, seq bioseq.seq('dna4', 64)) USING bioseq;");
    const long long loaded =
        it::query_scalar_int(ctx, "SELECT bioseq.load('load_roundtrip'::regclass, '" + path + "');");
    REQUIRE(loaded == 3);

    // Compare decoded sequences and lengths with, the input, ordered by header.
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

    // Load one record beyond the stripe limit to exercise writing after a
    // flush. Check that every source was published and  the last row can still
    // be found and decoded.
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

TEST_CASE("bioseq.load: width admission and cross-width equivalence follow the active profile",
          "[component:bioseq][layer:integration][feature:io][resource:postgres][resource:filesystem]") {
    it::connection_test_context ctx;
    it::reset_extension(ctx, { std::string_view{"load_width_32"}, std::string_view{"load_width_64"} });

    const std::string body = gen_bases(6000, 77, false);
    const std::string path = "/tmp/bioseq_load_width.fasta";
    write_fasta(path, {{"width_record", body}});

    constexpr std::string_view narrow_table_sql =
        "CREATE TABLE load_width_32 (header text, seq bioseq.seq('dna4', 32)) USING bioseq;";
#if defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_COORDINATES)
    it::exec_sql(ctx, narrow_table_sql);
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.load('load_width_32'::regclass, '" + path + "');") == 1);
    REQUIRE(it::query_scalar(ctx, "SELECT seq::text FROM load_width_32") == body);
#else
    const auto disabled_width = it::sql_error(ctx, narrow_table_sql);
    REQUIRE(disabled_width.sqlstate == "22023");
    REQUIRE(disabled_width.contains(
        "bioseq.seq width 32 is not enabled in this build"));
#endif
    it::exec_sql(ctx, "CREATE TABLE load_width_64 (header text, seq bioseq.seq('dna4', 64)) USING bioseq;");
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.load('load_width_64'::regclass, '" + path + "');") == 1);
    REQUIRE(it::query_scalar(ctx, "SELECT seq::text FROM load_width_64") == body);

    // Changing the storage width must preserve the alignment start position.
    const std::string query = body.substr(500, 800);
    std::string pos32, pos64;
#if defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_COORDINATES)
    for (const char* table_name : {"load_width_32", "load_width_64"}) {
#else
    for (const char* table_name : {"load_width_64"}) {
#endif
        const std::string width =
            (std::string(table_name) == "load_width_32") ? "32" : "64";
        const auto rows = it::query_rows(
            ctx, fmt::format("SELECT bioseq.target_start(header) FROM {} WHERE "
                             "mmsearch(seq, '{}'::bioseq.seq('dna4', {}))",
                             table_name, query, width));
        REQUIRE(rows.size() == 1);
        (width == "32" ? pos32 : pos64) = rows[0][0].value();
    }
#if defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_COORDINATES)
    REQUIRE(pos32 == pos64);   // identical target_start across storage widths
#endif
    REQUIRE_FALSE(pos64.empty());
}

TEST_CASE("bioseq.load: active-profile alphabets round-trip and mmsearch across widths 8/16/32/64",
          "[component:bioseq][layer:integration][feature:io][resource:postgres][resource:filesystem]") {
    // Test each supported alphabet at every storage width. Use a fresh
    // connection for each case so resetting the extension cannot leave cached
    // type information from the previous case.
#if defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
    const std::string body5 = gen_bases(3000, 99, /*with_n=*/true);   // dna5 with occasional N bases
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
            const auto create_table_sql = fmt::format(
                "CREATE TABLE load_active_profile_width (header text, seq bioseq.seq('{}', {})) USING bioseq;",
                current_case->alpha, width);
#if !defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_COORDINATES)
            if (std::string_view{width} != "64") {
                const auto disabled_width = it::sql_error(ctx, create_table_sql);
                REQUIRE(disabled_width.sqlstate == "22023");
                REQUIRE(disabled_width.contains(fmt::format(
                    "bioseq.seq width {} is not enabled in this build", width)));
                continue;
            }
#endif
            it::exec_sql(ctx, create_table_sql);
            REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.load('load_active_profile_width'::regclass, '" + current_case->path + "');") == 1);
            // Loading and decoding must preserve every  base, including N.
            const auto rows = it::query_rows(ctx,
                "SELECT seq::text, bioseq.len(seq) FROM load_active_profile_width");
            REQUIRE(rows.size() == 1);
            REQUIRE(rows[0][0].value() == current_case->body); // all bajes preserved
            REQUIRE(rows[0][1].value() == std::to_string(current_case->body.size()));

            // Each supported alphabet and width must produce an mmsearch hit.
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
    // BIOSEQ_LOAD_MEMBOUND_GBP controls the record size in billions of bases
    // The default is 0.1. Use a larger value to exercise records that cannot
    // fit in one PostgreSQL allocation. Verify the stored length without
    // converting the whole sequence to text.
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
    // The load must succeed and preserve the full length of the record.
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

    // With insert maintenance enabled, loading must create the indexes and
    // make the sequence immediately searchable as an  exact self-match.
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

    // With insert maintenance disabled, the minimizer index stays empty
    // until update_index is called.
    it::exec_sql(ctx, "CREATE TABLE load_index_off (header text, seq bioseq.seq('dna4', 64)) USING bioseq;");
    it::exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.load('load_index_off'::regclass, '" + path + "');") == 1);
    REQUIRE(it::index_table_row_count(ctx, "minimizers_per_fragment", "load_index_off") == 0);
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.update_index('load_index_off'::regclass, 'minimizers_per_fragment');") > 0);
    REQUIRE(it::index_table_row_count(ctx, "minimizers_per_fragment", "load_index_off") > 0);

    // The per-sequence policy must create its selected index while leaving
    // the per-fragment index empty.
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
    // Enable hash-major minimizers_per_sequence output for the DNA4/15/10
    // profile checked below.
    it::exec_sql(ctx,
        "SET bioseq.aligner.grouped_soa_generation.enabled = on;");
    it::exec_sql(ctx, "SET bioseq.index.async_persistence.enabled = off;");
    it::exec_sql(ctx, "SET bioseq.index.recompute_async_persistence.enabled = off;");
    it::exec_sql(ctx,
        "SET bioseq.aligner.minimizer.algorithm = 'scalar';");

    // Loading must publish the sequence, and then build a ready index from
    // its stored location. Check that  the idex describes that same source.
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

    // Force index creation to fail by enabling unsupported async persistence.
    // The load must report that error and roll back both the sequence and
    // everything created for its index.
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

    // Reject a table without a sequence column
    it::exec_sql(ctx, "DROP TABLE IF EXISTS load_error_case CASCADE;");
    it::exec_sql(ctx, "CREATE TABLE load_error_case (header text) USING bioseq;");
    {
        const auto err =
            it::sql_error(ctx, "SELECT bioseq.load('load_error_case'::regclass, '" + path + "');");
        REQUIRE(err.sqlstate == "42P16");
        REQUIRE(err.contains("no bioseq.seq column"));
    }

    // Reject a table with two sequence columns.
    it::exec_sql(ctx, "DROP TABLE IF EXISTS load_error_case CASCADE;");
    it::exec_sql(ctx, "CREATE TABLE load_error_case (sequence_primary bioseq.seq('dna4',64), sequence_secondary bioseq.seq('dna4',64)) USING bioseq;");
    {
        const auto err =
            it::sql_error(ctx, "SELECT bioseq.load('load_error_case'::regclass, '" + path + "');");
        REQUIRE(err.sqlstate == "42P16");
        REQUIRE(err.contains("bioseq.seq columns"));
    }

    // Two non-key text columns need an explicit header mapping.
    it::exec_sql(ctx, "DROP TABLE IF EXISTS load_error_case CASCADE;");
    it::exec_sql(ctx, "CREATE TABLE load_error_case (header_primary text, header_secondary text, seq bioseq.seq('dna4',64)) USING bioseq;");
    {
        const auto err =
            it::sql_error(ctx, "SELECT bioseq.load('load_error_case'::regclass, '" + path + "');");
        REQUIRE(err.sqlstate == "42P16");
        REQUIRE(err.contains("text columns"));
        REQUIRE(err.contains("disambiguate"));
    }

    // A bigserial key gets its value from the nextval() default during load.
    it::exec_sql(ctx, "DROP TABLE IF EXISTS load_error_case CASCADE;");
    it::exec_sql(ctx, "CREATE TABLE load_error_case (id bigserial primary key, "
                      "header text, seq bioseq.seq('dna4',64)) USING bioseq;");
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.load('load_error_case'::regclass, '" + path + "');") == 1);
    {
        const auto rows = it::query_rows(ctx, "SELECT id, header FROM load_error_case");
        REQUIRE(rows.size() == 1);
        REQUIRE(rows[0][0].value() == "1");   // nextval() supplied the key
        REQUIRE(rows[0][1].value() == "error_record"); // the  only text column receives the header
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

    // Reject an unrecognised file extension.
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

    // Malformed FASTA must fail and leave the table empty.
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
    // The failed load must leave no rows behind.
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

    // Read the UniProt accession and named tags into text and integer
    // columns, then search the loaded sequence.
    {
        const std::string path = "/tmp/bioseq_load_uniprot.fasta";
        write_fasta(path, {{"tr|R9RJF1|R9RJF1_PSEAI Mandelate racemase "
                            "OS=Pseudomonas aeruginosa PE=4 SV=1", body}});
        it::exec_sql(ctx, "CREATE TABLE uniprot_records (accession text, organism_name text, "
                          "sv int, seq bioseq.seq('dna4',64)) USING bioseq;");
        REQUIRE(it::query_scalar_int(ctx,
            "SELECT bioseq.load('uniprot_records'::regclass, '" + path +
            "', header_format => 'uniprot');") == 1);
        const auto rows = it::query_rows(ctx,
            "SELECT accession, organism_name, sv FROM uniprot_records");
        REQUIRE(rows.size() == 1);
        REQUIRE(rows[0][0].value() == "R9RJF1");
        REQUIRE(rows[0][1].value() == "Pseudomonas aeruginosa");
        REQUIRE(rows[0][2].value() == "1");                 // parsed as an integer

        // A 1200 bp prefix must map back to the loaded row.
        const auto hits = it::query_rows(ctx, fmt::format(
            "SELECT accession FROM uniprot_records WHERE "
            "mmsearch(seq, '{}'::bioseq.seq('dna4', 64))", query));
        REQUIRE_FALSE(hits.empty());
        REQUIRE(hits[0][0].value() == "R9RJF1");

        // Reuse the same backend after loading and searching. This catches
        // memory-context cleanup errors that can break the next statement.
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

        // A reader with SELECT on the  user table must be able to search it
        // without access to bioseq_internal tables. Run mmsearch as that role
        // so the tet also covers catalog access outside the table owner.
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
        const std::string denied_export = "/tmp/bioseq_write_restricted_" + backend_pid + ".fasta";
        std::filesystem::remove(denied_export);
        // The owner-side privilege assertions above warm the planner's OID cache.
        // Invalidate it immediately before changing role, so even unrelated SQL
        // must reload private helper identities without private-schema USAGE.
        it::exec_sql(ctx,
            "ALTER TABLE uniprot_records ALTER COLUMN accession SET STATISTICS 0; "
            "SET ROLE bioseq_load_restricted_reader;");
        REQUIRE(it::query_scalar_int(ctx, "SELECT 1") == 1);
        const auto denied_write = it::sql_error(ctx,
            "SELECT bioseq.write($query$SELECT 'restricted'::text AS header,"
            "'ACGT'::bioseq.seq('dna4',64) AS seq$query$::text,'" + denied_export + "')");
        REQUIRE(denied_write.sqlstate == "42501");
        REQUIRE(denied_write.contains("pg_write_server_files"));
        REQUIRE_FALSE(std::filesystem::exists(denied_export));
        const auto private_call = it::sql_error(ctx,
            "SELECT bioseq_internal.export_alignment_status('ACGT'::bioseq.seq('dna4',64))");
        REQUIRE(private_call.sqlstate == "42501");
        REQUIRE(private_call.contains("bioseq_internal"));
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
        REQUIRE(rows[0][2].value() == "17");                // parsed as an integer
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

    // Unmapped NOT NULL columns use their declared defaults.
    {
        const std::string path = "/tmp/bioseq_load_defaulted_fields.fasta";
        write_fasta(path, {{"tr|X1|X1_HUMAN p OS=Homo sapiens PE=1 SV=1", body}});
        it::exec_sql(ctx, "CREATE TABLE load_defaulted_fields (accession text, "
                          "score int NOT NULL DEFAULT 7, "
                          "ts timestamptz NOT NULL DEFAULT now(), "
                          "seq bioseq.seq('dna4',64)) USING bioseq;");
        REQUIRE(it::query_scalar_int(ctx,
            "SELECT bioseq.load('load_defaulted_fields'::regclass, '" + path +
            "', header_format => 'uniprot');") == 1);
        const auto rows = it::query_rows(ctx,
            "SELECT accession, score, (ts IS NOT NULL)::text FROM load_defaulted_fields");
        REQUIRE(rows.size() == 1);
        REQUIRE(rows[0][0].value() == "X1");
        REQUIRE(rows[0][1].value() == "7");                 // constant default evaluated
        REQUIRE(rows[0][2].value() == "true");              // now() supplied a timestamp
    }

    // Missing required metadata fails per row: a canonical header could supply it.
    {
        const std::string path = "/tmp/bioseq_load_missing_default.fasta";
        write_fasta(path, {{"tr|X1|X1_HUMAN p OS=Homo sapiens PE=1 SV=1", body}});
        it::exec_sql(ctx, "CREATE TABLE load_missing_default (accession text, required_value int NOT NULL, "
                          "seq bioseq.seq('dna4',64)) USING bioseq;");
        const auto err = it::sql_error(ctx,
            "SELECT bioseq.load('load_missing_default'::regclass, '" + path +
            "', header_format => 'uniprot');");
        REQUIRE(err.sqlstate == "23502");
        REQUIRE(err.contains("required_value"));
        REQUIRE(err.contains("not-null constraint"));
        REQUIRE(it::query_scalar_int(ctx, "SELECT count(*) FROM load_missing_default") == 0);
    }

    // A header value that cannot become an integer must report the input error.
    {
        const std::string path = "/tmp/bioseq_load_bad_coercion.fasta";
        write_fasta(path, {{"tr|X1|X1_HUMAN p OS=Homo sapiens PE=1 SV=abc", body}});
        it::exec_sql(ctx, "CREATE TABLE load_bad_coercion (accession text, sv int, "
                          "seq bioseq.seq('dna4',64)) USING bioseq;");
        const auto err = it::sql_error(ctx,
            "SELECT bioseq.load('load_bad_coercion'::regclass, '" + path +
            "', header_format => 'uniprot');");
        REQUIRE(err.sqlstate == "22P02");                   // invalid_text_representation
        REQUIRE(err.contains("invalid input syntax"));      // integer input error
        // Record and column details are in libpq's PG_DIAG_CONTEXT field.
        // sql_error() checks the, message fields, so they are not asserted here
    }

    // Map numbered header fields into  text, enum, and date columns.
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
        REQUIRE(rows[0][1].value() == "A");                 // enum label
        REQUIRE(rows[0][2].value() == "2015-07-17");        // date parsed with MDY input
    }
}

TEST_CASE("quality SQL statistics and FASTQ publication", "[component:bioseq][layer:integration][feature:quality][resource:postgres][resource:filesystem]") {
    it::connection_test_context ctx;
    it::reset_extension(ctx, {std::string_view{"quality_load"}, std::string_view{"quality_named"}});
    REQUIRE(it::query_scalar(ctx,
        "SELECT string_agg(attname||':'||format_type(atttypid,atttypmod),',' ORDER BY attnum) "
        "FROM pg_attribute WHERE attrelid=(SELECT typrelid FROM pg_type "
        "WHERE oid='bioseq.quality_stats_result'::regtype) AND attnum>0 AND NOT attisdropped") ==
        "length:bigint,minimum:smallint,maximum:smallint,total:numeric,mean:numeric");
    const auto composite = it::query_one(ctx,
        "SELECT (bioseq.quality_stats('!+5I'::bioseq.seq('phred42'))).*");
    REQUIRE(composite.size() == 5);
    REQUIRE(composite[0] == "4"); REQUIRE(composite[1] == "0"); REQUIRE(composite[2] == "40");
    REQUIRE(composite[3] == "70"); REQUIRE(composite[4] == "17.5");
    const auto empty_composite = it::query_one(ctx,
        "SELECT (bioseq.quality_stats(''::bioseq.seq('phred42'))).*");
    REQUIRE(empty_composite[0] == "0"); REQUIRE_FALSE(empty_composite[1]); REQUIRE_FALSE(empty_composite[2]);
    REQUIRE(empty_composite[3] == "0"); REQUIRE_FALSE(empty_composite[4]);
    REQUIRE(it::query_scalar(ctx, "SELECT bioseq.quality_stats(NULL::bioseq.seq) IS NULL") == "t");
    const auto basic = it::query_rows(ctx,
        "SELECT bioseq.min(q)::text,bioseq.max(q)::text,bioseq.sum(q)::text,bioseq.avg(q)::text,"
        "bioseq.reverse(q)::text,bioseq.reverse_complement(q)::text "
        "FROM (VALUES ('!+5I'::bioseq.seq('phred'))) v(q)");
    REQUIRE(basic.size() == 1);
    REQUIRE(basic[0][0] == "0"); REQUIRE(basic[0][1] == "40");
    REQUIRE(basic[0][2] == "70"); REQUIRE(basic[0][3] == "17.5");
    REQUIRE(basic[0][4] == "I5+!"); REQUIRE(basic[0][5] == "I5+!");
    const auto legacy = it::query_rows(ctx,
        "SELECT bioseq.sum(';@~'::bioseq.seq('phred68-legacy'))::text,"
        "bioseq.avg(';@~'::bioseq.seq('phred68-legacy'))::text,"
        "bioseq.quality_score(';'::bioseq.letter('phred68-legacy'))::text");
    REQUIRE(legacy[0][0] == "57"); REQUIRE(legacy[0][1] == "19"); REQUIRE(legacy[0][2] == "-5");
    const auto empty = it::query_rows(ctx,
        "SELECT bioseq.sum(''::bioseq.seq('phred42'))::text,bioseq.avg(''::bioseq.seq('phred42'))::text,"
        "bioseq.min(NULL::bioseq.seq)::text");
    REQUIRE(empty[0][0] == "0"); REQUIRE_FALSE(empty[0][1]); REQUIRE_FALSE(empty[0][2]);
    REQUIRE(it::sql_error(ctx, "SELECT 'K'::bioseq.seq('phred42')").sqlstate == "22P02");
    REQUIRE(it::sql_error(ctx, "SELECT bioseq.avg('A'::bioseq.seq('dna4'))").sqlstate == "42804");
    const auto raw = it::query_rows(ctx, "SELECT 'a~10M'::bioseq.seq::text");
    REQUIRE(raw[0][0] == "a~10M");
    const std::string path = "/tmp/bioseq_quality_load.fastq";
    { std::ofstream file(path, std::ios::binary); file << "@a\r\nACGT\r\n+\r\n!+5I\r\n@b\nAAAA\n+\n!!!!\n"; }
    it::exec_sql(ctx, "CREATE TABLE quality_load(header text, seq bioseq.seq('dna4')) USING bioseq");
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.load('quality_load','" + path + "')") == 2);
    const auto loaded = it::query_rows(ctx,
        "SELECT header,quality::text,bioseq.avg(quality)::text,bioseq.sum(quality)::text,"
        "bioseq.min(quality)::text,bioseq.max(quality)::text,bioseq.symbol_count(quality,'!')::text "
        "FROM quality_load ORDER BY bioseq.avg(quality),header");
    REQUIRE(loaded.size() == 2); REQUIRE(loaded[0][0] == "b"); REQUIRE(loaded[0][1] == "!!!!");
    REQUIRE(loaded[1][0] == "a"); REQUIRE(loaded[1][2] == "17.5"); REQUIRE(loaded[1][3] == "70");
    REQUIRE(loaded[1][4] == "0"); REQUIRE(loaded[1][5] == "40"); REQUIRE(loaded[1][6] == "1");
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.update_index('quality_load','symbol_counts')") >= 2);
    const auto indexed = it::query_rows(ctx,
        "SELECT header,bioseq.avg(quality)::text,bioseq.avg(bioseq.reverse(quality))::text,"
        "bioseq.avg(quality OPERATOR(bioseq.||) quality)::text FROM quality_load ORDER BY header");
    REQUIRE(indexed[0][1] == indexed[0][2]); REQUIRE(indexed[0][1] == indexed[0][3]);
    it::exec_sql(ctx, "CREATE TABLE quality_named(header text, seq bioseq.seq('dna4')) USING bioseq");
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.load('quality_named','" + path +
        "',columns=>ARRAY[ROW('scores','quality')::bioseq.column_mapping], options=>ROW('phred63',NULL)::bioseq.load_options)") == 2);
    const auto named = it::query_rows(ctx, "SELECT scores::text FROM quality_named ORDER BY header");
    REQUIRE(named[0][0] == "!+5I");
    { std::ofstream file(path, std::ios::binary); file << "@bad\nAAAA\n+\nKKKK\n"; }
    REQUIRE(it::sql_error(ctx, "SELECT bioseq.load('quality_load','" + path + "')").sqlstate == "22P02");
    REQUIRE(it::query_scalar_int(ctx, "SELECT count(*) FROM quality_load") == 2);
    { std::ofstream file(path, std::ios::binary); file << "@empty\n+\n\n"; }
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.load('quality_load','" + path + "')") == 1);
    const auto empty_loaded = it::query_one(ctx,
        "SELECT seq::text,quality::text,bioseq.len(quality),bioseq.sum(quality),bioseq.avg(quality) "
        "FROM quality_load WHERE header='empty'");
    REQUIRE(empty_loaded[0] == ""); REQUIRE(empty_loaded[1] == "");
    REQUIRE(empty_loaded[2] == "0"); REQUIRE(empty_loaded[3] == "0"); REQUIRE_FALSE(empty_loaded[4]);
    std::filesystem::remove(path);
}

TEST_CASE("quality indexed summaries share extraction and respect source versions",
          "[component:bioseq][layer:integration][feature:quality][resource:postgres]") {
    auto ctx = it::fresh_context({"quality_versions"});
    it::guc_scope maintenance(ctx, "bioseq.index.insert_maintenance.enabled", "off");
    it::exec_sql(ctx, "CREATE TABLE quality_versions(id integer, q bioseq.seq('phred42')) USING bioseq");
    it::exec_sql(ctx, "INSERT INTO quality_versions VALUES (1,repeat('!I',20000)::bioseq.seq('phred42'))");
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.update_index('quality_versions','symbol_counts')") == 1);
    const std::string stats = "SELECT bioseq.avg(q),bioseq.sum(q),bioseq.min(q),bioseq.max(q),"
        "bioseq.symbol_count(q,'I') FROM quality_versions";
    const auto expected = it::query_rows(ctx, stats);
    REQUIRE(expected[0][0] == "20"); REQUIRE(expected[0][1] == "800000");
    REQUIRE(expected[0][2] == "0"); REQUIRE(expected[0][3] == "40"); REQUIRE(expected[0][4] == "20000");
    const auto indexed = it::explain_text<true>(ctx, stats);
    REQUIRE(it::find_explain_property(indexed, "Bioseq Quality Index Hits") == "1");
    REQUIRE(it::find_explain_property(indexed, "Bioseq Quality Summary Extractions") == "1");
    REQUIRE(it::find_explain_property(indexed, "Bioseq Quality Source Symbols Read") == "0");
    it::exec_sql(ctx, "SELECT bioseq_internal.quality_scalar_summary_probe(0)");
    const auto filtered_indexed = it::explain_text<true>(ctx, stats + " WHERE bioseq.avg(q)>10");
    REQUIRE(it::find_explain_property(filtered_indexed, "Bioseq Quality Summary Cache Hits") == "1");
    REQUIRE(it::find_explain_property(filtered_indexed, "Bioseq Quality Index Hits") == "0");
    REQUIRE(it::find_explain_property(filtered_indexed, "Bioseq Quality Source Symbols Read") == "0");
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq_internal.quality_scalar_summary_probe(1)") == 0);
    {
        it::guc_scope reads(ctx, "bioseq.index.sequence_read.enabled", "off");
        REQUIRE(it::query_rows(ctx, stats) == expected);
        const auto computed = it::explain_text<true>(ctx, stats);
        REQUIRE(it::find_explain_property(computed, "Bioseq Quality Index Hits") == "0");
        REQUIRE(it::find_explain_property(computed, "Bioseq Quality Source Symbols Read") == "40000");
        REQUIRE(it::query_rows(ctx, stats + " WHERE bioseq.avg(q)>10") == expected);
        it::exec_sql(ctx, "SELECT bioseq_internal.quality_scalar_summary_probe(0)");
        const auto filtered = it::explain_text<true>(ctx, stats + " WHERE bioseq.avg(q)>10");
        REQUIRE(it::find_explain_property(filtered, "Bioseq Quality Summary Cache Hits") == "1");
        REQUIRE(it::find_explain_property(filtered, "Bioseq Quality Index Hits") == "0");
        REQUIRE(it::find_explain_property(filtered, "Bioseq Quality Source Symbols Read") == "0");
        REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq_internal.quality_scalar_summary_probe(1)") == 40000);
    }
    it::exec_sql(ctx, "SELECT bioseq_internal.reset_sequence_source_counters()");
    const auto whole = it::query_one(ctx,
        "SELECT bioseq.avg(bioseq.reverse(q)),bioseq.sum(q OPERATOR(bioseq.||) q) FROM quality_versions");
    REQUIRE(whole[0] == "20"); REQUIRE(whole[1] == "1600000");
    REQUIRE(it::query_scalar_int(ctx,
        "SELECT (bioseq_internal.sequence_source_counters()->>'facade_source_stream_callbacks')::bigint") == 0);
    const auto lifetime = it::query_one(ctx,
        "SELECT c->>'registries_created'=c->>'registries_destroyed',"
        "c->>'domains_created'=c->>'domains_destroyed',c->>'leases_acquired'=c->>'leases_released' "
        "FROM (SELECT bioseq_internal.sequence_source_counters() c) v");
    REQUIRE(lifetime[0] == "t"); REQUIRE(lifetime[1] == "t"); REQUIRE(lifetime[2] == "t");
    const auto lazy = it::query_rows(ctx,
        "SELECT bioseq.avg(bioseq.reverse(q)),bioseq.sum(q OPERATOR(bioseq.||) q),"
        "bioseq.avg((bioseq.reverse(q))[0:3]) FROM quality_versions");
    REQUIRE(lazy[0][0] == "20"); REQUIRE(lazy[0][1] == "1600000");
    REQUIRE(lazy[0][2] == "26.6666666666666666666666666666666666666667");
    it::connection_test_context reader;
    it::exec_sql(reader, "BEGIN ISOLATION LEVEL REPEATABLE READ");
    REQUIRE(it::query_scalar_int(reader, "SELECT bioseq.avg(q)::bigint FROM quality_versions") == 20);
    it::exec_sql(ctx, "UPDATE quality_versions SET q=repeat('J',40000)::bioseq.seq('phred42') WHERE id=1");
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.avg(q)::bigint FROM quality_versions") == 41);
    REQUIRE(it::query_scalar_int(reader, "SELECT bioseq.avg(q)::bigint FROM quality_versions") == 20);
    it::exec_sql(reader, "COMMIT");
    it::exec_sql(ctx, "PREPARE quality_parameter(integer) AS SELECT bioseq.avg(q) FROM quality_versions WHERE id=$1");
    REQUIRE(it::query_scalar_int(ctx, "EXECUTE quality_parameter(1)") == 41);
    it::exec_sql(ctx, "DEALLOCATE quality_parameter");
    it::exec_sql(ctx, "BEGIN; DELETE FROM quality_versions; ROLLBACK");
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.avg(q)::bigint FROM quality_versions") == 41);
    it::exec_sql(ctx, "DELETE FROM quality_versions");
    REQUIRE(it::query_scalar_int(ctx, "SELECT count(*) FROM quality_versions") == 0);
    it::exec_sql(ctx, "TRUNCATE quality_versions; INSERT INTO quality_versions VALUES (2,'I')");
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.update_index('quality_versions','symbol_counts')") == 1);
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.avg(q)::bigint FROM quality_versions") == 40);
}

TEST_CASE("quality summaries release source owners on faults and canceled index rebuilds",
          "[component:bioseq][layer:integration][feature:quality][resource:postgres]") {
    auto ctx = it::fresh_context({"quality_source_failures"});
    it::guc_scope maintenance(ctx, "bioseq.index.insert_maintenance.enabled", "off");
    it::exec_sql(ctx, "CREATE TABLE quality_source_failures(id integer,q bioseq.seq('phred42')) USING bioseq; "
        "INSERT INTO quality_source_failures SELECT i,repeat('!I',65536)::bioseq.seq('phred42') "
        "FROM generate_series(1,64) i");
    const auto no_live_sources = [&] {
        REQUIRE(it::query_scalar_int(ctx,
            "SELECT (c->>'live_registries')::bigint+(c->>'live_providers')::bigint+"
            "(c->>'live_domains')::bigint+(c->>'live_leases')::bigint+"
            "(c->>'live_cache_sets')::bigint+(c->>'live_snapshots')::bigint "
            "FROM (SELECT bioseq_internal.sequence_source_counters() c) v") == 0);
    };
    for (const auto* point : {"provider_after_registry_control", "provider_after_provider_control",
            "domain_after_control", "domain_after_context", "domain_after_owner", "domain_after_snapshot",
            "domain_after_caches", "read_before_scanner", "close_before_caches", "close_before_snapshot",
            "close_before_before_locks", "close_before_locks", "close_before_after_locks"}) {
        CAPTURE(point);
        it::exec_sql(ctx, "SELECT bioseq_internal.reset_sequence_source_counters()");
        it::exec_sql(ctx, fmt::format("SELECT bioseq_internal.arm_sequence_source_failure('{}')", point));
        const auto injected = it::sql_error(ctx,
            "SELECT bioseq.avg(q[4093:4101]) FROM quality_source_failures WHERE id=1");
        REQUIRE(injected.sqlstate == "XX000");
        REQUIRE(injected.contains("injected PostgreSQL sequence source-domain failure"));
        it::exec_sql(ctx, "SELECT bioseq_internal.disarm_sequence_source_failure()");
        REQUIRE(it::query_scalar_int(ctx,
            "SELECT (bioseq_internal.sequence_source_counters()->>'fault_injections')::bigint") == 1);
        no_live_sources();
    }
    it::exec_sql(ctx, "SELECT bioseq_internal.reset_sequence_source_counters()");
    {
        it::guc_scope timeout(ctx, "statement_timeout", "'1ms'");
        REQUIRE(it::sql_error(ctx,
            "SELECT bioseq.update_index('quality_source_failures','symbol_counts')").sqlstate == "57014");
    }
    no_live_sources();
    REQUIRE(it::query_scalar_int(ctx,
        "SELECT count(*) FROM quality_source_failures WHERE bioseq.avg(q)=20") == 64);
    REQUIRE(it::query_scalar_int(ctx,
        "SELECT bioseq.update_index('quality_source_failures','symbol_counts')") == 64);
    const auto indexed = it::explain_text<true>(ctx, "SELECT bioseq.avg(q) FROM quality_source_failures");
    REQUIRE(it::find_explain_property(indexed, "Bioseq Quality Source Symbols Read") == "0");
    REQUIRE(it::find_explain_property(indexed, "Bioseq Quality Index Hits") == "64");
}

TEST_CASE("quality mean batch order agrees with numeric sort and spills under work_mem",
          "[component:bioseq][layer:integration][feature:quality][resource:postgres]") {
    auto ctx = it::fresh_context({"quality_sort"});
    it::guc_scope maintenance(ctx, "bioseq.index.insert_maintenance.enabled", "off");
    it::exec_sql(ctx, "CREATE TABLE quality_sort(id integer, q bioseq.seq('phred42')) USING bioseq");
    it::exec_sql(ctx, "INSERT INTO quality_sort SELECT i,('!'||repeat('I',4096+i))::bioseq.seq('phred42') "
        "FROM generate_series(1,2000) i");
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.update_index('quality_sort','symbol_counts')") == 2000);
    const std::string query = "SELECT id,bioseq.avg(q) FROM quality_sort ORDER BY bioseq.avg(q) DESC NULLS LAST";
    it::exec_sql(ctx, "SET bioseq.planner.sequence_sort_scan.enabled=off");
    const auto ordinary = it::query_rows(ctx, query);
    REQUIRE(ordinary.size() == 2000); REQUIRE(ordinary.front()[0] == "2000");
    it::exec_sql(ctx, "SET bioseq.planner.sequence_sort_scan.enabled=on");
    it::exec_sql(ctx, "SET work_mem='64MB'");
    REQUIRE(it::query_rows(ctx, query) == ordinary);
    const auto native = it::explain_text<true>(ctx, query);
    REQUIRE(it::find_explain_property(native, "Bioseq Sort Key") == "exact quality mean");
    REQUIRE(it::find_explain_property(native, "Bioseq Sort Accelerator") == "exact quality mean CPU");
    REQUIRE(it::find_explain_property(native, "Bioseq Mean Quality Index Hits") == "2000");
    REQUIRE(it::find_explain_property(native, "Bioseq Mean Source Symbols Read") == "0");
    const auto shared = it::explain_text<true>(ctx,
        "SELECT id,bioseq.min(q),bioseq.avg(q) FROM quality_sort ORDER BY bioseq.avg(q)");
    REQUIRE(it::find_explain_property(shared, "Bioseq Quality Index Hits") == "2000");
    REQUIRE(it::find_explain_property(shared, "Bioseq Mean Quality Index Hits") == "0");
    REQUIRE(it::find_explain_property(shared, "Bioseq Mean Summary Cache Hits") == "2000");
    it::exec_sql(ctx, "SET work_mem='64kB'");
    REQUIRE(it::query_rows(ctx, query) == ordinary);
    const auto spill = it::explain_text<true>(ctx, query);
    REQUIRE(it::find_explain_property(spill, "Bioseq Sort Accelerator") == "numeric tuplesort fallback");
    REQUIRE(it::find_explain_property(spill, "Bioseq Mean Sort Spilled") == "true");
    const auto limited = it::query_rows(ctx, query + " OFFSET 7 LIMIT 11");
    REQUIRE(limited == decltype(limited)(ordinary.begin()+7, ordinary.begin()+18));
    it::exec_sql(ctx, "CREATE TEMP SEQUENCE quality_sort_evaluations");
    for (const auto* memory : {"64MB", "64kB"}) {
        it::exec_sql(ctx, fmt::format("SET work_mem='{}'; ALTER SEQUENCE quality_sort_evaluations RESTART", memory));
        const auto once = it::explain_text<true>(ctx,
            "SELECT id,bioseq.avg(q) FROM quality_sort WHERE nextval('quality_sort_evaluations')>0 ORDER BY bioseq.avg(q)");
        REQUIRE(it::find_explain_property(once, "Bioseq Sort Key") == "exact quality mean");
        REQUIRE(it::find_explain_property(once, "Bioseq Sort Accelerator") ==
            (std::string_view{memory} == "64MB" ? "exact quality mean CPU" : "numeric tuplesort fallback"));
        REQUIRE(it::query_scalar_int(ctx, "SELECT currval('quality_sort_evaluations')") == 2000);
    }
    {
        it::guc_scope timeout(ctx, "statement_timeout", "'30ms'");
        REQUIRE(it::sql_error(ctx, "SELECT id,bioseq.avg(q) FROM quality_sort "
            "WHERE pg_sleep(0.001) IS NULL OR id>0 ORDER BY bioseq.avg(q)").sqlstate == "57014");
    }
    REQUIRE(it::query_rows(ctx, query) == ordinary);
    it::exec_sql(ctx, "INSERT INTO quality_sort VALUES (2001,NULL),(2002,''),(2003,'II'),(2004,'IIII')");
    const auto ties = it::query_rows(ctx,
        "SELECT id FROM quality_sort ORDER BY bioseq.avg(q) DESC NULLS LAST FETCH FIRST 1 ROW WITH TIES");
    REQUIRE(ties.size() == 2);
    it::exec_sql(ctx, "SET work_mem='64MB'");
    for (const auto* direction : {"ASC", "DESC"}) {
        for (const auto* nulls : {"FIRST", "LAST"}) {
            const auto ordering = fmt::format(
                "SELECT bioseq.avg(q) FROM quality_sort ORDER BY bioseq.avg(q) {} NULLS {}", direction, nulls);
            it::exec_sql(ctx, "SET bioseq.planner.sequence_sort_scan.enabled=off");
            const auto oracle = it::query_rows(ctx, ordering);
            it::exec_sql(ctx, "SET bioseq.planner.sequence_sort_scan.enabled=on");
            REQUIRE(it::query_rows(ctx, ordering) == oracle);
        }
    }
    {
        it::guc_scope reads(ctx, "bioseq.index.sequence_read.enabled", "off");
        const auto plan = it::explain_text<true>(ctx, query);
        REQUIRE(it::find_explain_property(plan, "Bioseq Mean Quality Index Hits") == "0");
        REQUIRE(it::find_explain_property(plan, "Bioseq Mean Source Symbols Read") ==
            std::to_string(it::query_scalar_int(ctx, "SELECT sum(bioseq.len(q)) FROM quality_sort")));
    }
    // Projected source values and transformed/multiple keys use the ordinary safe route.
    const std::string fallback = "SELECT id,q,bioseq.avg(bioseq.reverse(q)) FROM quality_sort "
        "ORDER BY bioseq.avg(bioseq.reverse(q)),id";
    it::exec_sql(ctx, "SET bioseq.planner.sequence_sort_scan.enabled=off");
    const auto fallback_oracle = it::query_rows(ctx, fallback);
    it::exec_sql(ctx, "SET bioseq.planner.sequence_sort_scan.enabled=on");
    REQUIRE(it::query_rows(ctx, fallback) == fallback_oracle);
    // Retain actual sequence/composite Datums through the sort, rather than
    // formatting q to text in the child and hiding source-lifetime mistakes.
    for (const auto* projection : {"q", "ROW(q)", "ARRAY[q]"}) {
        const auto projected = fmt::format(
            "SELECT id,{},bioseq.avg(q) FROM quality_sort WHERE id<=2000 ORDER BY bioseq.avg(q)", projection);
        it::exec_sql(ctx, "SET bioseq.planner.sequence_sort_scan.enabled=off");
        const auto projected_oracle = it::query_rows(ctx, projected);
        it::exec_sql(ctx, "SET bioseq.planner.sequence_sort_scan.enabled=on");
        REQUIRE(it::query_rows(ctx, projected) == projected_oracle);
    }
    it::exec_sql(ctx, "RESET work_mem; RESET bioseq.planner.sequence_sort_scan.enabled");
}

TEST_CASE("quality SQL typmods preserve values across every admitted packing width",
          "[component:bioseq][layer:integration][feature:quality][feature:alphabet][resource:postgres]") {
    auto ctx = it::fresh_context();
    for (const auto& [alphabet, text] : {
            std::pair{"phred42", "!+5I"}, std::pair{"phred63", "!K_"},
            std::pair{"phred68-legacy", ";a~"}}) {
        for (const auto width : {8, 16, 32, 64}) {
            CAPTURE(alphabet, width);
            const auto value = fmt::format("'{}'::bioseq.seq('{}',{})", text, alphabet, width);
#if !defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_COORDINATES)
            if (width != 64) {
                const auto disabled = it::sql_error(ctx, "SELECT " + value);
                REQUIRE(disabled.sqlstate == "22023");
                REQUIRE(disabled.contains(fmt::format("width {} is not enabled", width)));
                continue;
            }
#endif
            const auto row = it::query_one(ctx, fmt::format(
                "SELECT q::text,bioseq.reverse(bioseq.reverse_complement(q))::text,"
                "bioseq.seq_cmp(q,q::bioseq.seq('{}',64))=0,"
                "bioseq.seq_hash(q)=bioseq.seq_hash(q::bioseq.seq('{}',64)),"
                "bioseq.sum(q)=bioseq.sum(q::bioseq.seq('{}',64)),"
                "(q[0:1] OPERATOR(bioseq.||) q[1:])::text "
                "FROM (VALUES ({})) v(q)", alphabet, alphabet, alphabet, value));
            REQUIRE(row[0] == text); REQUIRE(row[1] == text);
            REQUIRE(row[2] == "t"); REQUIRE(row[3] == "t"); REQUIRE(row[4] == "t");
            REQUIRE(row[5] == text);
        }
    }
}

TEST_CASE("quality binding conversions and lazy identity operations are lossless",
          "[component:bioseq][layer:integration][feature:quality][resource:postgres]") {
    auto ctx = it::fresh_context();
    REQUIRE(it::query_one(ctx, "SELECT ('a~'::bioseq.seq)::bioseq.seq('phred68-legacy')::text")[0] == "a~");
    REQUIRE(it::query_one(ctx, "SELECT ('!J'::bioseq.seq('phred42'))::bioseq.seq('phred63')::text")[0] == "!J");
    REQUIRE(it::sql_error(ctx, "SELECT ('_'::bioseq.seq('phred63'))::bioseq.seq('phred42')").sqlstate == "22023");
    REQUIRE(it::sql_error(ctx, "SELECT (''::bioseq.seq('phred42'))::bioseq.seq('phred68-legacy')").sqlstate == "42804");
    REQUIRE(it::sql_error(ctx, "SELECT ('A'::bioseq.seq('dna4'))::bioseq.seq('phred42')").sqlstate == "42804");
    const auto values = it::query_one(ctx,
        "SELECT bioseq.reverse(bioseq.reverse_complement('a~'::bioseq.seq('phred68-legacy')))::text,"
        "bioseq.reverse_complement('a~'::bioseq.seq('phred68-legacy'))::text,"
        "(bioseq.quality_stats('!J'::bioseq.seq('phred42'))).mean");
    REQUIRE(values[0] == "a~"); REQUIRE(values[1] == "~a"); REQUIRE(values[2] == "20.5");
    for (const auto* spelling : {"1I", "01I", "10M", "001I02M"}) {
        const auto rebound = it::query_one(ctx, fmt::format(
            "SELECT ('{}'::bioseq.seq)::bioseq.seq('phred63')::text", spelling));
        REQUIRE(rebound[0] == spelling);
        if (spelling[0] == '0') {
            REQUIRE(it::sql_error(ctx, fmt::format("SELECT '{}'::bioseq.seq('cigar')",
                spelling)).sqlstate == "22P02");
            continue;
        }
        REQUIRE(it::query_scalar_int(ctx, fmt::format("SELECT bioseq.len('{}'::bioseq.seq('cigar'))",
            spelling)) == (std::string_view{spelling} == "1I" ? 1 : 10));
        REQUIRE(it::sql_error(ctx, fmt::format("SELECT ('{}'::bioseq.seq('cigar'))::bioseq.seq('phred63')",
            spelling)).sqlstate == "42804");
    }
}

TEST_CASE("quality letters preserve every score and legacy counts preserve case",
          "[component:bioseq][layer:integration][feature:quality][resource:postgres]") {
    auto ctx = it::fresh_context({"quality_legacy_letters"});
    for (const auto& [alphabet, first, last, offset] : {
             std::tuple{"phred42", 33, 74, 33},
             std::tuple{"phred63", 33, 95, 33},
             std::tuple{"phred68-legacy", 59, 126, 64}}) {
        CAPTURE(alphabet);
        REQUIRE(it::query_scalar(ctx, fmt::format(
            "SELECT bool_and(bioseq.quality_score(chr(i)::bioseq.letter('{}'))=i-{}) "
            "FROM generate_series({},{}) AS g(i)", alphabet, offset, first, last)) == "t");
    }
    it::exec_sql(ctx, "CREATE TABLE quality_legacy_letters(q bioseq.seq('phred68-legacy')) USING bioseq;"
        "INSERT INTO quality_legacy_letters VALUES ('AaaJj')");
    REQUIRE(it::query_scalar_int(ctx,
        "SELECT bioseq.update_index('quality_legacy_letters','symbol_counts')") == 1);
    const std::string query = "SELECT bioseq.symbol_count(q,'a'),bioseq.symbol_count(q,'A'),"
        "bioseq.sum(q),bioseq.avg(q),q[-2:]::text,"
        "bioseq.seq_hash(q)=bioseq.seq_hash(q[0:2] OPERATOR(bioseq.||) q[2:]),"
        "bioseq.reverse(bioseq.reverse(q))::text FROM quality_legacy_letters";
    for (const char* policy : {"on", "off"}) {
        it::guc_scope reads(ctx, "bioseq.index.sequence_read.enabled", policy);
        const auto row = it::query_one(ctx, query);
        REQUIRE(row[0] == "2"); REQUIRE(row[1] == "1");
        REQUIRE(row[2] == "119"); REQUIRE(row[3] == "23.8");
        REQUIRE(row[4] == "Jj"); REQUIRE(row[5] == "t"); REQUIRE(row[6] == "AaaJj");
    }
    it::exec_sql(ctx, "UPDATE quality_legacy_letters SET q=q[:1] OPERATOR(bioseq.||) ';~'::text OPERATOR(bioseq.||) q[3:]");
    REQUIRE(it::query_scalar(ctx, "SELECT q::text FROM quality_legacy_letters") == "A;~Jj");
    it::exec_sql(ctx, "UPDATE quality_legacy_letters SET q=q[:1] OPERATOR(bioseq.||) q[3:]");
    REQUIRE(it::query_scalar(ctx, "SELECT q::text FROM quality_legacy_letters") == "AJj");
    REQUIRE(it::query_scalar(ctx, "SELECT ('a'::text OPERATOR(bioseq.||) q "
        "OPERATOR(bioseq.||) '~'::text)::text FROM quality_legacy_letters") == "aAJj~");
}

TEST_CASE("quality sequence batch ordering agrees with native ASCII score order",
          "[component:bioseq][layer:integration][feature:quality][resource:postgres]") {
    auto ctx = it::fresh_context({"quality_lex"});
    it::guc_scope maintenance(ctx, "bioseq.index.insert_maintenance.enabled", "off");
    for (const auto* alphabet : {"phred42", "phred63", "phred68-legacy"}) {
        INFO(alphabet);
        const bool legacy = std::string_view{alphabet} == "phred68-legacy";
        it::exec_sql(ctx, fmt::format("DROP TABLE IF EXISTS quality_lex; "
            "CREATE TABLE quality_lex(q bioseq.seq('{}')) USING bioseq", alphabet));
        it::exec_sql(ctx, fmt::format("INSERT INTO quality_lex SELECT "
            "(repeat('{}',1024)||chr({}+i%{})||repeat('{}',i%7))::bioseq.seq('{}') "
            "FROM generate_series(1,70) i; INSERT INTO quality_lex VALUES (''),(NULL)",
            legacy ? "a" : "I", legacy ? 59 : 33, legacy ? 68 : 42,
            legacy ? "a" : "I", alphabet));
        const auto expected = it::query_rows(ctx, "SELECT q::text FROM quality_lex ORDER BY q::text COLLATE \"C\" NULLS FIRST");
        it::exec_sql(ctx, "SET bioseq.planner.sequence_sort_scan.enabled=on");
        REQUIRE(it::query_rows(ctx, "SELECT q::text FROM quality_lex ORDER BY quality_lex.q NULLS FIRST") == expected);
        const auto plan = it::explain_text<true>(ctx, "SELECT q::text FROM quality_lex ORDER BY quality_lex.q NULLS FIRST");
        REQUIRE(std::ranges::any_of(plan, [](const auto& line) { return line.contains("BioseqBatchSort"); }));
        REQUIRE(it::query_rows(ctx, "SELECT q::text FROM quality_lex ORDER BY quality_lex.q DESC NULLS LAST") ==
            decltype(expected)(expected.rbegin(), expected.rend()));
    }
    it::exec_sql(ctx, "RESET bioseq.planner.sequence_sort_scan.enabled");
}

TEST_CASE("quality and CIGAR lazy EXPLAIN resolve rewritten column references",
          "[component:bioseq][layer:integration][feature:quality][resource:postgres]") {
    auto ctx = it::fresh_context({"quality_explain"});
    it::guc_scope maintenance(ctx, "bioseq.index.insert_maintenance.enabled", "off");
    it::exec_sql(ctx, "CREATE TABLE quality_explain(id integer,c bioseq.seq('cigar'),"
        "q bioseq.seq('phred68-legacy')) USING bioseq; "
        "INSERT INTO quality_explain VALUES (1,'2M1I2M','aJj'),(2,'4M',';~'),(3,'',''),(4,NULL,NULL)");
    for (const auto* enabled : {"off", "on"}) {
        it::guc_scope sort(ctx, "bioseq.planner.sequence_sort_scan.enabled", enabled);
        for (const auto* query : {
                "SELECT id,c::text FROM quality_explain t ORDER BY t.c NULLS LAST",
                "SELECT id,bioseq.reverse(q)::text FROM quality_explain t ORDER BY t.q NULLS LAST",
                "SELECT id,bioseq.avg(q[0:2]) FROM quality_explain t ORDER BY id"}) {
            INFO(query);
            const auto plan = it::explain_text<true>(ctx, query);
            REQUIRE(std::ranges::any_of(plan, [](const auto& line) {
                return line.contains("BioseqSliceProject");
            }));
            const auto structured = it::query_scalar(ctx, std::string("EXPLAIN (ANALYZE,FORMAT JSON) ") + query);
            REQUIRE(structured.contains("Slice Recipe"));
        }
    }
}

TEST_CASE("quality ordinary SQL paths preserve values and outer join nulls",
          "[component:bioseq][layer:integration][feature:quality][resource:postgres]") {
    auto ctx = it::fresh_context({"quality_heap", "quality_join"});
    it::exec_sql(ctx, "CREATE TABLE quality_heap(id integer, q bioseq.seq('phred68-legacy')); "
        "ALTER TABLE quality_heap ALTER COLUMN q SET STORAGE EXTERNAL; "
        "INSERT INTO quality_heap VALUES (1,';@~'),(2,''),(3,NULL),(4,repeat('a~',40000)::bioseq.seq('phred68-legacy')); "
        "CREATE TABLE quality_join(id integer, q bioseq.seq('phred68-legacy')) USING bioseq; "
        "INSERT INTO quality_join VALUES (1,';@~'),(2,''),(3,NULL)");
    it::exec_sql(ctx, "SELECT bioseq_internal.quality_scalar_summary_probe(0)");
    const auto heap = it::query_rows(ctx,
        "SELECT id,bioseq.min(q),bioseq.max(q),bioseq.sum(q),bioseq.avg(q) FROM quality_heap ORDER BY id");
    const auto scalar_reads = it::query_one(ctx,
        "SELECT bioseq_internal.quality_scalar_summary_probe(1),"
        "bioseq_internal.quality_scalar_summary_probe(2),"
        "bioseq_internal.quality_scalar_summary_probe(3)");
    REQUIRE(scalar_reads[0] == "80003");
    REQUIRE(scalar_reads[1] == "3");
    REQUIRE(scalar_reads[2] == "9");
    REQUIRE(heap[0][1] == "-5"); REQUIRE(heap[0][2] == "62");
    REQUIRE(heap[0][3] == "57"); REQUIRE(heap[0][4] == "19");
    REQUIRE_FALSE(heap[1][1]); REQUIRE(heap[1][3] == "0"); REQUIRE_FALSE(heap[1][4]);
    REQUIRE_FALSE(heap[2][3]); REQUIRE(heap[3][4] == "47.5");
    it::exec_sql(ctx, "SELECT bioseq_internal.quality_scalar_summary_probe(0)");
    const auto literal_tree = it::query_rows(ctx,
        "SELECT id,bioseq.min(q),bioseq.max(q),bioseq.sum(q),bioseq.avg(q) "
        "FROM (SELECT id,bioseq.reverse(q) AS q FROM quality_heap OFFSET 0) v ORDER BY id");
    REQUIRE(literal_tree == heap);
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq_internal.quality_scalar_summary_probe(1)") == 80003);
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq_internal.quality_scalar_summary_probe(2)") <= 3);
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq_internal.quality_scalar_summary_probe(3)") >= 6);
    const auto joined = it::query_rows(ctx,
        "SELECT h.id,bioseq.avg(j.q) FROM quality_heap h LEFT JOIN quality_join j ON h.id=j.id ORDER BY h.id");
    REQUIRE(joined[0][1] == "19"); REQUIRE_FALSE(joined[1][1]);
    REQUIRE_FALSE(joined[2][1]); REQUIRE_FALSE(joined[3][1]);
    const auto filtered = it::query_rows(ctx,
        "SELECT id,bioseq.sum(q) FROM quality_join WHERE bioseq.avg(q)>10 ORDER BY id");
    REQUIRE(filtered.size() == 1); REQUIRE(filtered[0][0] == "1"); REQUIRE(filtered[0][1] == "57");
    const auto rescanned = it::query_rows(ctx,
        "SELECT v.ordinal,(SELECT bioseq.avg(q) FROM quality_join WHERE id=v.id OFFSET 0) "
        "FROM (VALUES (1,1),(2,2),(3,1),(4,3)) v(ordinal,id) ORDER BY v.ordinal");
    REQUIRE(rescanned[0][1] == "19"); REQUIRE_FALSE(rescanned[1][1]);
    REQUIRE(rescanned[2][1] == "19"); REQUIRE_FALSE(rescanned[3][1]);
    it::exec_sql(ctx, "SET search_path=pg_catalog,bioseq,public");
    const auto scalar_and_aggregate = it::query_one(ctx,
        "SELECT avg(';@~'::bioseq.seq('phred68-legacy')),"
        "(SELECT sum(i) FROM generate_series(1,3) i),"
        "(SELECT avg(i) FROM generate_series(1,3) i),"
        "(SELECT seq_min(q)::text FROM quality_heap)");
    REQUIRE(scalar_and_aggregate[0] == "19"); REQUIRE(scalar_and_aggregate[1] == "6");
    REQUIRE(scalar_and_aggregate[2] == "2.0000000000000000"); REQUIRE(scalar_and_aggregate[3] == "");
    const auto materialized = it::query_rows(ctx,
        "WITH x AS MATERIALIZED (SELECT id,bioseq.reverse(q) q FROM quality_heap) "
        "SELECT id,bioseq.min(q),bioseq.max(q),bioseq.sum(q),bioseq.avg(q) FROM x ORDER BY id");
    REQUIRE(materialized == heap);
    it::exec_sql(ctx, "PREPARE quality_value(bioseq.seq) AS SELECT bioseq.avg($1),bioseq.sum($1)");
    const auto parameter = it::query_one(ctx, "EXECUTE quality_value(';@~'::bioseq.seq('phred68-legacy'))");
    REQUIRE(parameter[0] == "19"); REQUIRE(parameter[1] == "57");
    it::exec_sql(ctx, "DEALLOCATE quality_value");
    const auto arrays = it::query_one(ctx,
        "SELECT array_to_string(bioseq.to_array(q),''),bioseq.quality_score(q[0]),"
        "q[-2:]::text,bioseq.reverse(bioseq.reverse(q))::text "
        "FROM (VALUES (';a~'::bioseq.seq('phred68-legacy'))) v(q)");
    REQUIRE(arrays[0] == ";a~"); REQUIRE(arrays[1] == "-5");
    REQUIRE(arrays[2] == "a~"); REQUIRE(arrays[3] == ";a~");
    REQUIRE(it::query_scalar_int(ctx,
        "SELECT count(*) FROM pg_proc WHERE oid IN ('bioseq.min(bioseq.seq)'::regprocedure,"
        "'bioseq.max(bioseq.seq)'::regprocedure,'bioseq.sum(bioseq.seq)'::regprocedure,"
        "'bioseq.avg(bioseq.seq)'::regprocedure) AND prokind='f' AND proisstrict AND proparallel='s'") == 4);
    REQUIRE(it::sql_error(ctx, "SELECT bioseq.avg(''::bioseq.seq('dna4'))").sqlstate == "42804");
    REQUIRE(it::sql_error(ctx, "SELECT bioseq.avg('10M'::bioseq.seq('cigar'))").sqlstate == "42804");
    REQUIRE(it::sql_error(ctx, "SELECT bioseq.avg('!') OVER ()").sqlstate == "42809");
    REQUIRE(it::query_one(ctx, "SELECT ('10M'::bioseq.seq)::bioseq.seq('phred63')::text")[0] == "10M");
}

TEST_CASE("FASTQ declared codecs and transactional quality setup",
          "[component:bioseq][layer:integration][feature:quality][resource:postgres][resource:filesystem]") {
    auto ctx = it::fresh_context({"quality_declared", "quality_ddl", "quality_bad_destination"});
    it::guc_scope maintenance(ctx, "bioseq.index.insert_maintenance.enabled", "off");
    const std::string fastq = "/tmp/bioseq_quality_declared.fastq";
    const std::string fasta = "/tmp/bioseq_quality_declared.fasta";
    { std::ofstream file(fastq, std::ios::binary); file << "@legacy\nACG\n+\na@~\n"; }
    write_fasta(fasta, {{"fasta", "ACG"}});
    it::exec_sql(ctx, "CREATE TABLE quality_declared(header text,seq bioseq.seq('dna4'),"
        "quality bioseq.seq('phred68-legacy'), CHECK (quality IS NULL OR bioseq.len(seq)=bioseq.len(quality))) USING bioseq");
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.load('quality_declared','" + fastq + "')") == 1);
    REQUIRE(it::query_one(ctx, "SELECT quality::text FROM quality_declared")[0] == "a@~");
    REQUIRE(it::sql_error(ctx, "SELECT bioseq.load('quality_declared','" + fastq +
        "',options=>ROW('phred63',NULL)::bioseq.load_options)").sqlstate == "22023");
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.load('quality_declared','" + fasta + "')") == 1);
    REQUIRE_FALSE(it::query_one(ctx, "SELECT quality::text FROM quality_declared WHERE header='fasta'")[0]);
    REQUIRE(it::sql_error(ctx, "SELECT bioseq.load('quality_declared','" + fasta +
        "',options=>ROW('phred42',NULL)::bioseq.load_options)").sqlstate == "22023");
    REQUIRE(it::sql_error(ctx, "SELECT bioseq.load('quality_declared','" + fasta +
        "',columns=>ARRAY[ROW('quality','quality')::bioseq.column_mapping])").sqlstate == "22023");
    it::exec_sql(ctx, "CREATE TABLE quality_ddl(header text,seq bioseq.seq('dna4')) USING bioseq");
    // The default phred42 rejects legacy high bytes after adding the column.
    REQUIRE(it::sql_error(ctx, "SELECT bioseq.load('quality_ddl','" + fastq + "')").sqlstate == "22P02");
    REQUIRE(it::query_scalar_int(ctx, "SELECT count(*) FROM pg_attribute WHERE attrelid='quality_ddl'::regclass "
        "AND attname='quality' AND NOT attisdropped") == 0);
    REQUIRE(it::query_scalar_int(ctx, "SELECT count(*) FROM quality_ddl") == 0);
    it::exec_sql(ctx, "CREATE TABLE quality_bad_destination(header text,seq bioseq.seq('dna4'),quality text) USING bioseq");
    REQUIRE(it::sql_error(ctx, "SELECT bioseq.load('quality_bad_destination','" + fastq + "')").sqlstate == "42P16");
    // A failed constraint sees both finished sources but publishes neither.
    it::exec_sql(ctx, "ALTER TABLE quality_declared ADD CONSTRAINT forbid_again CHECK(header<>'again')");
    { std::ofstream file(fastq, std::ios::binary); file << "@again\nACG\n+\na@~\n"; }
    REQUIRE(it::sql_error(ctx, "SELECT bioseq.load('quality_declared','" + fastq + "')").sqlstate == "23514");
    REQUIRE(it::query_scalar_int(ctx, "SELECT count(*) FROM quality_declared") == 2);
    { std::ofstream file(fastq, std::ios::binary); file << "@permitted\nACG\n+\n!!!\n"; }
    it::exec_sql(ctx, "DROP ROLE IF EXISTS bioseq_quality_test_importer; "
        "CREATE ROLE bioseq_quality_test_importer NOLOGIN; "
        "GRANT pg_read_server_files TO bioseq_quality_test_importer; "
        "GRANT USAGE ON SCHEMA bioseq TO bioseq_quality_test_importer; "
        "GRANT INSERT ON quality_ddl TO bioseq_quality_test_importer; "
        "SET ROLE bioseq_quality_test_importer");
    const auto ddl_permission = it::sql_error(ctx, "SELECT bioseq.load('quality_ddl','" + fastq + "')");
    const auto insert_permission = it::sql_error(ctx, "SELECT bioseq.load('quality_declared','" + fastq + "')");
    it::exec_sql(ctx, "RESET ROLE; REVOKE INSERT ON quality_ddl FROM bioseq_quality_test_importer; "
        "REVOKE pg_read_server_files FROM bioseq_quality_test_importer; "
        "REVOKE USAGE ON SCHEMA bioseq FROM bioseq_quality_test_importer; DROP ROLE bioseq_quality_test_importer");
    REQUIRE(ddl_permission.sqlstate == "42501");
    REQUIRE(ddl_permission.contains("predeclare"));
    REQUIRE(insert_permission.sqlstate == "42501");
    std::filesystem::remove(fastq); std::filesystem::remove(fasta);
}

TEST_CASE("FASTQ indexed reload uses the new storage generation after truncate",
          "[component:bioseq][layer:integration][feature:quality][resource:postgres][resource:filesystem]") {
    auto ctx = it::fresh_context({"quality_reload"});
    it::guc_scope cache(ctx, "bioseq.column_cache.enabled", "on");
    it::guc_scope maintenance(ctx, "bioseq.index.insert_maintenance.enabled", "on");
    const std::string path = "/tmp/bioseq_quality_reload.fastq";
    it::exec_sql(ctx, "CREATE TABLE quality_reload(header text,seq bioseq.seq('dna4'),"
        "quality bioseq.seq('phred42')) USING bioseq; "
        "SELECT bioseq.set_index_policy('quality_reload','minimizers_per_fragment','UPDATE_MANUALLY'); "
        "SELECT bioseq.set_index_policy('quality_reload','minimizers_per_sequence','UPDATE_MANUALLY')");
    for (const bool transactional : {false, true}) {
        if (transactional) it::exec_sql(ctx, "BEGIN");
        for (const int length : {50, 65, 51}) {
            it::exec_sql(ctx, "TRUNCATE quality_reload");
            { std::ofstream file(path, std::ios::binary);
              file << "@read_" << length << '\n' << std::string(length, 'A')
                   << "\n+\n" << std::string(length, length == 65 ? '!' : 'I') << '\n'; }
            // Keep the index rebuild's decompression buffers alive: ordinary
            // SELECT scans reset that cache and would hide the stale-generation bug.
            REQUIRE(it::query_scalar_int(ctx,
                "SELECT bioseq.load('quality_reload','" + path + "')") == 1);
        }
        if (transactional) it::exec_sql(ctx, "COMMIT");
        const auto row = it::query_one(ctx, "SELECT header,seq::text,quality::text,"
            "bioseq.sum(quality)::text,bioseq.avg(quality)::text FROM quality_reload");
        REQUIRE(row[0] == "read_51");
        REQUIRE(row[1] == std::string(51, 'A'));
        REQUIRE(row[2] == std::string(51, 'I'));
        REQUIRE(row[3] == "2040");
        REQUIRE(row[4] == "40");
    }
    std::filesystem::remove(path);
}

TEST_CASE("quality table validation retains fragment metadata across tuple resets",
          "[component:bioseq][layer:integration][feature:quality][resource:postgres]") {
    auto ctx = it::fresh_context({"quality_validation"});
    it::guc_scope maintenance(ctx, "bioseq.index.insert_maintenance.enabled", "off");
    it::exec_sql(ctx, "CREATE TABLE quality_validation(id integer,seq bioseq.seq('dna4'),"
        "quality bioseq.seq('phred68-legacy'),"
        "CHECK(quality IS NULL OR bioseq.len(seq)=bioseq.len(quality))) USING bioseq; "
        "INSERT INTO quality_validation SELECT i,'ACG','a@~' FROM generate_series(1,31) i");
    // ALTER uses the ordinary table AM and resets an expression context after
    // every row. Fragment reader metadata must outlive that temporary context.
    it::exec_sql(ctx, "ALTER TABLE quality_validation ADD CONSTRAINT positive_id CHECK(id>0)");
    REQUIRE(it::query_scalar_int(ctx, "SELECT count(*) FROM quality_validation "
        "WHERE seq::text='ACG' AND quality::text='a@~'") == 31);
    it::exec_sql(ctx, "INSERT INTO quality_validation VALUES (32,'ACG','a@~'); "
        "ALTER TABLE quality_validation ADD CONSTRAINT bounded_id CHECK(id<100)");
    REQUIRE(it::query_scalar_int(ctx, "SELECT count(*) FROM quality_validation") == 32);
    REQUIRE(it::sql_error(ctx, "INSERT INTO quality_validation VALUES (100,'ACG','a@~')").sqlstate == "23514");
}

TEST_CASE("FASTQ dual-source publication rolls back at each ownership transition",
          "[component:bioseq][layer:integration][feature:quality][resource:postgres][resource:filesystem]") {
    auto ctx = it::fresh_context({"quality_atomic"});
    it::exec_sql(ctx, "CREATE TABLE quality_atomic(header text,seq bioseq.seq('dna4'),"
        "quality bioseq.seq('phred42')) USING bioseq");
    const std::string path = "/tmp/bioseq_quality_atomic.fastq";
    { std::ofstream file(path, std::ios::binary); file << "@atomic\n" << std::string(50000, 'A')
        << "\n+\n" << std::string(50000, 'I') << '\n'; }
    const auto sources_before = it::query_scalar_int(ctx, "SELECT count(*) FROM bioseq_internal.sequence_source_version");
    for (const auto* overlap : {"off", "on"}) {
      it::guc_scope overlap_mode(ctx, "bioseq.load.write_overlap.enabled", overlap);
      for (int stage = 1; stage <= 7; ++stage) {
        INFO("write overlap " << overlap);
        INFO("failure stage " << stage);
        it::exec_sql(ctx, fmt::format("SELECT bioseq_internal.arm_load_failure({})", stage));
        const auto error = it::sql_error(ctx, "SELECT bioseq.load('quality_atomic','" + path + "')");
        REQUIRE(error.sqlstate == "XX000");
        REQUIRE(error.contains("injected load failure"));
        REQUIRE(it::query_scalar_int(ctx, "SELECT count(*) FROM quality_atomic") == 0);
        REQUIRE(it::query_scalar_int(ctx, "SELECT count(*) FROM bioseq_internal.sequence_source_version") == sources_before);
      }
    }
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.load('quality_atomic','" + path + "')") == 1);
    const auto row = it::query_one(ctx, "SELECT bioseq.len(seq),bioseq.len(quality),bioseq.avg(quality) FROM quality_atomic");
    REQUIRE(row[0] == "50000"); REQUIRE(row[1] == "50000"); REQUIRE(row[2] == "40");
    std::filesystem::remove(path);
}

TEST_CASE("quality load preserves defaults old rows and compressed digit qualities",
          "[component:bioseq][layer:integration][feature:quality][resource:postgres][resource:filesystem]") {
    auto ctx = it::fresh_context({"quality_defaults", "quality_old_rows", "quality_required"});
    it::guc_scope maintenance(ctx, "bioseq.index.insert_maintenance.enabled", "off");
    const std::string gzip_path = "/tmp/bioseq_quality_options.fastq.gz";
    const std::string fasta_path = "/tmp/bioseq_quality_options.fasta";
    const std::string fastq = "@new\nACG\n+\n01I\n";
    gzFile compressed = gzopen(gzip_path.c_str(), "wb");
    REQUIRE(compressed != nullptr);
    REQUIRE(gzwrite(compressed, fastq.data(), fastq.size()) == static_cast<int>(fastq.size()));
    REQUIRE(gzclose(compressed) == Z_OK);
    write_fasta(fasta_path, {{"fasta", "ACG"}});
    it::exec_sql(ctx, "CREATE TABLE quality_defaults(header text,seq bioseq.seq('dna4'),"
        "quality bioseq.seq('phred63') NOT NULL DEFAULT '_'::bioseq.seq('phred63')) USING bioseq; "
        "CREATE TABLE quality_old_rows(header text,seq bioseq.seq('dna4')) USING bioseq; "
        "INSERT INTO quality_old_rows VALUES ('old','ACG'); "
        "CREATE TABLE quality_required(header text,seq bioseq.seq('dna4'),quality bioseq.seq('phred42') NOT NULL) USING bioseq");
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.load('quality_defaults','" + gzip_path + "')") == 1);
    REQUIRE(it::query_one(ctx, "SELECT quality::text,bioseq.sum(quality) FROM quality_defaults")[1] == "71");
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.load('quality_defaults','" + fasta_path + "')") == 1);
    REQUIRE(it::query_one(ctx, "SELECT quality::text FROM quality_defaults WHERE header='fasta'")[0] == "_");
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.update_index('quality_defaults','symbol_counts')") == 2);
    const auto default_index = it::explain_text<true>(ctx, "SELECT bioseq.avg(quality) FROM quality_defaults");
    REQUIRE(it::find_explain_property(default_index, "Bioseq Quality Index Hits") == "2");
    REQUIRE(it::find_explain_property(default_index, "Bioseq Quality Source Symbols Read") == "0");
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.load('quality_old_rows','" + gzip_path + "')") == 1);
    const auto rows = it::query_rows(ctx, "SELECT header,quality::text FROM quality_old_rows ORDER BY header");
    REQUIRE(rows.size() == 2); REQUIRE(rows[0][1] == "01I"); REQUIRE_FALSE(rows[1][1]);
    REQUIRE(it::sql_error(ctx, "SELECT bioseq.load('quality_required','" + fasta_path + "')").sqlstate == "42P16");
    REQUIRE(it::sql_error(ctx, "SELECT bioseq.load('quality_defaults','" + gzip_path +
        "',columns=>ARRAY[ROW('a','quality'),ROW('b','quality')]::bioseq.column_mapping[])").sqlstate == "22023");
    std::filesystem::remove(gzip_path); std::filesystem::remove(fasta_path);
}


TEST_CASE("FASTQ CHECK expressions read both staged sources before publication",
          "[component:bioseq][layer:integration][feature:quality][resource:postgres][resource:filesystem]") {
    auto ctx = it::fresh_context({"quality_checks"});
    it::exec_sql(ctx, "CREATE TABLE quality_checks(header text,seq bioseq.seq('dna4'),"
        "quality bioseq.seq('phred42'), CHECK(seq::text='ACG' AND bioseq.avg(quality)>=30)) USING bioseq");
    const std::string path = "/tmp/bioseq_quality_checks.fastq";
    const auto write = [&](std::string_view sequence, std::string_view quality) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "@check\n" << sequence << "\n+\n" << quality << '\n';
    };
    write("ACG", "III");
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.load('quality_checks','" + path + "')") == 1);
    const auto sources = it::query_scalar_int(ctx, "SELECT count(*) FROM bioseq_internal.sequence_source_version");
    for (const auto& values : {std::pair{"ACG", "!!!"}, std::pair{"AGC", "III"}}) {
        write(values.first, values.second);
        REQUIRE(it::sql_error(ctx, "SELECT bioseq.load('quality_checks','" + path + "')").sqlstate == "23514");
        REQUIRE(it::query_scalar_int(ctx, "SELECT count(*) FROM quality_checks") == 1);
        REQUIRE(it::query_scalar_int(ctx, "SELECT count(*) FROM bioseq_internal.sequence_source_version") == sources);
        const auto row = it::query_one(ctx, "SELECT seq::text,quality::text,bioseq.avg(quality) FROM quality_checks");
        REQUIRE(row[0] == "ACG"); REQUIRE(row[1] == "III"); REQUIRE(row[2] == "40");
    }
    std::filesystem::remove(path);
}

TEST_CASE("FASTQ invalid bytes and decoder failures preserve both source transactions",
          "[component:bioseq][layer:integration][feature:quality][resource:postgres][resource:filesystem]") {
    auto ctx = it::fresh_context({"quality_failures"});
    it::guc_scope maintenance(ctx, "bioseq.index.insert_maintenance.enabled", "off");
    it::guc_scope pipeline(ctx, "bioseq.scheduler.pipeline.mode", "parallel_dispatch");
    it::exec_sql(ctx, "CREATE TABLE quality_failures(header text,seq bioseq.seq('dna4'),"
        "quality bioseq.seq('phred42')) USING bioseq; INSERT INTO quality_failures VALUES ('existing','ACGT','IIII')");
    const auto sources = it::query_scalar_int(ctx, "SELECT count(*) FROM bioseq_internal.sequence_source_version");
    const std::string path = "/tmp/bioseq_quality_failures.fastq";
    const std::string gzip_path = path + ".gz";
    const auto unchanged = [&] {
        REQUIRE(it::query_scalar_int(ctx, "SELECT count(*) FROM quality_failures") == 1);
        REQUIRE(it::query_scalar_int(ctx, "SELECT count(*) FROM bioseq_internal.sequence_source_version") == sources);
        REQUIRE(it::query_scalar(ctx, "SELECT quality::text FROM quality_failures") == "IIII");
    };
    for (const auto* overlap : {"off", "on"}) {
        it::guc_scope parallel(ctx, "bioseq.load.write_overlap.enabled", overlap);
        for (const auto& quality : std::vector<std::string>{"II I", "II\tI", "II\rI", "II\x7fI", "II\xffI", std::string{"II\0I",4}}) {
            { std::ofstream file(path, std::ios::binary | std::ios::trunc);
              file << "@first\nACGT\n+\nIIII\n@bad\nACGT\n+\n" << quality << '\n'; }
            const auto error = it::sql_error(ctx, "SELECT bioseq.load('quality_failures','" + path + "')");
            REQUIRE(error.sqlstate == "22P02"); REQUIRE(error.contains("record 'bad'"));
            REQUIRE(error.contains("quality offset 2")); REQUIRE(error.contains("phred42"));
            unchanged();
        }
        // Invalid codec data in the consumer must release a backpressured producer.
        { std::ofstream file(path, std::ios::binary | std::ios::trunc);
          for (int i = 0; i < 80; ++i)
              file << "@queued" << i << '\n' << std::string(200003, 'A') << "\n+\nKKK"
                   << std::string(200000, 'I') << '\n'; }
        REQUIRE(it::sql_error(ctx, "SELECT bioseq.load('quality_failures','" + path + "')").sqlstate == "22P02");
        unchanged();
        for (const bool truncated : {false, true}) {
            std::string payload;
            for (int i = 0; i < 20; ++i)
                payload += "@compressed\n" + std::string(40003, 'A') + "\n+\n" + std::string(40003, 'I') + '\n';
            gzFile compressed = gzopen(gzip_path.c_str(), "wb");
            REQUIRE(compressed != nullptr);
            REQUIRE(gzwrite(compressed, payload.data(), payload.size()) == static_cast<int>(payload.size()));
            REQUIRE(gzclose(compressed) == Z_OK);
            if (truncated) std::filesystem::resize_file(gzip_path, std::filesystem::file_size(gzip_path)-5);
            else {
                std::fstream file(gzip_path, std::ios::binary | std::ios::in | std::ios::out);
                file.seekg(-8, std::ios::end); char byte{}; file.get(byte);
                file.seekp(-8, std::ios::end); file.put(static_cast<char>(byte ^ 1));
            }
            const auto error = it::sql_error(ctx, "SELECT bioseq.load('quality_failures','" + gzip_path + "')");
            REQUIRE(error.sqlstate == "22000");
            unchanged();
        }
    }
    std::filesystem::remove(path); std::filesystem::remove(gzip_path);
}

TEST_CASE("quality biological operations reject before kernel admission",
          "[component:bioseq][layer:integration][feature:quality][resource:postgres]") {
    auto ctx = it::fresh_context({"quality_biological_guard"});
    it::exec_sql(ctx, "CREATE TABLE quality_biological_guard(q bioseq.seq('phred42')) USING bioseq; "
        "INSERT INTO quality_biological_guard VALUES ('IIII')");
    REQUIRE(it::sql_error(ctx, "SELECT * FROM quality_biological_guard WHERE "
        "public.mmsearch(q,'IIII'::bioseq.seq('phred42'))").sqlstate == "0A000");
    REQUIRE(it::sql_error(ctx, "SELECT bioseq.update_index('quality_biological_guard','minimizers_per_sequence')").sqlstate == "0A000");
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.update_index('quality_biological_guard','symbol_counts')") == 1);
    REQUIRE(it::query_scalar(ctx, "SELECT bioseq.avg(q) FROM quality_biological_guard") == "40");
}

namespace {

/** Read a small exported fixture for independent native-field and no-overwrite assertions. */
[[nodiscard]] std::string read_export_fixture(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.is_open());
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

/** Decode only SAM's tab framing in the test, independently of the production codec. */
[[nodiscard]] std::vector<std::vector<std::string>> exported_sam_records(std::string_view contents) {
    std::istringstream input{std::string{contents}};
    std::vector<std::vector<std::string>> records;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() == '@') continue;
        std::istringstream fields{line};
        std::vector<std::string> record;
        std::string field;
        while (std::getline(fields, field, '\t')) record.push_back(std::move(field));
        REQUIRE(record.size() >= 11);
        records.push_back(std::move(record));
    }
    return records;
}

/** Independent native header/QNAME/FLAG evidence for small owned SAM/BAM fixtures. */
struct exported_native_summary {
    std::vector<std::string> names;
    std::vector<std::uint16_t> flags;
    std::vector<std::pair<std::string, std::uint32_t>> references;
};

/** Inspect native dictionary, QNAME and FLAG bytes without the preservation decoder. */
[[nodiscard]] exported_native_summary inspect_exported_native(
    const std::string& path, std::string_view format) {
    exported_native_summary summary;
    constexpr std::size_t fixture_limit = 1024u * 1024u;
    REQUIRE(std::filesystem::file_size(path) <= fixture_limit);
    // Both native formats carry the same textual @SQ declarations. BAM's
    // independent binary dictionary must agree with that text, including order.
    const auto text_references = [](std::string_view header) {
        std::vector<std::pair<std::string, std::uint32_t>> references;
        std::istringstream input{std::string{header}};
        std::string line;
        while (std::getline(input, line)) {
            if (!line.starts_with("@SQ\t")) continue;
            std::istringstream fields{line};
            std::string field;
            std::optional<std::string> name;
            std::optional<std::uint32_t> length;
            while (std::getline(fields, field, '\t')) {
                if (field.starts_with("SN:")) {
                    REQUIRE_FALSE(name.has_value());
                    name = field.substr(3);
                    REQUIRE_FALSE(name->empty());
                } else if (field.starts_with("LN:")) {
                    REQUIRE_FALSE(length.has_value());
                    std::uint32_t value{};
                    const auto parsed = std::from_chars(field.data() + 3, field.data() + field.size(), value);
                    REQUIRE(parsed.ec == std::errc{});
                    REQUIRE(parsed.ptr == field.data() + field.size());
                    REQUIRE(value > 0);
                    REQUIRE(value <= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()));
                    length = value;
                }
            }
            REQUIRE(name.has_value());
            REQUIRE(length.has_value());
            references.emplace_back(std::move(*name), *length);
        }
        return references;
    };
    if (format == "sam") {
        const auto text = read_export_fixture(path);
        REQUIRE(text.size() <= fixture_limit);
        summary.references = text_references(text);
        for (const auto& record : exported_sam_records(text)) {
            summary.names.push_back(record[0]);
            std::uint16_t flags{};
            const auto parsed = std::from_chars(record[1].data(), record[1].data() + record[1].size(), flags);
            REQUIRE(parsed.ec == std::errc{});
            REQUIRE(parsed.ptr == record[1].data() + record[1].size());
            summary.flags.push_back(flags);
        }
        return summary;
    }
    REQUIRE(format == "bam");
    const std::unique_ptr<std::remove_pointer_t<gzFile>, decltype(&gzclose)> input(gzopen(path.c_str(), "rb"), &gzclose);
    REQUIRE(input != nullptr);
    std::string bytes;
    std::array<char, 4096> chunk{};
    for (;;) {
        const auto count = gzread(input.get(), chunk.data(), static_cast<unsigned>(chunk.size()));
        REQUIRE(count >= 0);
        if (count == 0) break;
        REQUIRE(bytes.size() + static_cast<std::size_t>(count) <= fixture_limit);
        bytes.append(chunk.data(), static_cast<std::size_t>(count));
    }
    REQUIRE(bytes.size() >= 4);
    REQUIRE(bytes.substr(0, 4) == std::string("BAM\1", 4));
    std::size_t offset = 4;
    const auto unsigned_word = [&bytes, &offset] {
        REQUIRE(bytes.size() - offset >= 4);
        std::uint32_t value{};
        for (unsigned index = 0; index < 4; ++index)
            value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + index])) << (index * 8u);
        offset += 4;
        return value;
    };
    const auto skip = [&bytes, &offset](std::size_t count) {
        REQUIRE(count <= bytes.size() - offset);
        offset += count;
    };
    const auto text_length = unsigned_word();
    REQUIRE(text_length <= bytes.size() - offset);
    const auto declared_references = text_references(std::string_view{bytes}.substr(offset, text_length));
    skip(text_length);
    const auto reference_count = unsigned_word();
    REQUIRE(reference_count <= (bytes.size() - offset) / 10u);
    for (std::uint32_t index = 0; index < reference_count; ++index) {
        const auto name_size = unsigned_word();
        REQUIRE(name_size >= 2);
        REQUIRE(name_size <= bytes.size() - offset);
        REQUIRE(bytes[offset + name_size - 1] == '\0');
        const std::string_view name{bytes.data() + offset, name_size - 1};
        REQUIRE(name.find('\0') == std::string_view::npos);
        skip(name_size);
        const auto length = unsigned_word();
        REQUIRE(length > 0);
        REQUIRE(length <= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()));
        summary.references.emplace_back(name, length);
    }
    REQUIRE(summary.references == declared_references);
    while (offset < bytes.size()) {
        const auto record_size = unsigned_word();
        REQUIRE(record_size >= 32);
        REQUIRE(record_size <= bytes.size() - offset);
        const auto name_size = static_cast<unsigned char>(bytes[offset + 8]);
        REQUIRE(name_size >= 1);
        REQUIRE(name_size <= record_size - 32);
        REQUIRE(bytes[offset + 32 + name_size - 1] == '\0');
        summary.names.emplace_back(bytes.data() + offset + 32, name_size - 1);
        summary.flags.push_back(static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[offset + 14]))
            | (static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[offset + 15])) << 8u));
        skip(record_size);
    }
    return summary;
}

} // namespace

TEST_CASE("bioseq.write: canonical text round trips preserve typed metadata and header whitespace",
          "[component:bioseq][layer:integration][feature:io][resource:postgres][resource:filesystem]") {
    auto ctx = it::fresh_context({"write_text_source", "write_text_loaded"});
    it::guc_scope maintenance(ctx, "bioseq.index.insert_maintenance.enabled", "off");
    it::guc_scope array_nulls(ctx, "array_nulls", "off");
    it::guc_scope xmloption(ctx, "xmloption", "document");
    it::guc_scope monetary(ctx, "lc_monetary", "'C'");
    it::guc_scope search_path(ctx, "search_path", "public");
    it::exec_sql(ctx,
        "DROP SCHEMA IF EXISTS write_meta_first CASCADE; DROP SCHEMA IF EXISTS write_meta_second CASCADE; "
        "CREATE SCHEMA write_meta_first; CREATE SCHEMA write_meta_second; "
        "CREATE TABLE write_meta_first.t(id integer); CREATE TABLE write_meta_second.t(id integer)");
    const auto backend = it::query_scalar(ctx, "SELECT pg_backend_pid()");
    for (const std::string_view header_type : {"varchar(64)", "char(64)"}) {
        it::exec_sql(ctx, "DROP TABLE IF EXISTS public.write_text_source,public.write_text_loaded; "
            "CREATE TABLE public.write_text_source(header " + std::string{header_type} +
            ",seq bioseq.seq('dna5',64),quality bioseq.seq('phred63'),"
            "sample_no integer NOT NULL,sample text,target_start bigint,optional_note text,"
            "scalar_array text[],regclass_value regclass,xml_content xml,amount money) USING bioseq; "
            "INSERT INTO public.write_text_source VALUES "
            "(E'  read [1]  ','ACNGT','!+5I@',1,E'bracket [x=y]\\tline\\nnext',17,NULL,"
            "ARRAY[NULL,'NULL'],'write_meta_first.t'::regclass,XMLPARSE(CONTENT '<a/>tail<b/>'),"
            "'92233720368547758.07'::money),"
            "('','','',2,'',-3,'',ARRAY[]::text[],'write_meta_second.t'::regclass,"
            "XMLPARSE(CONTENT ''),'-92233720368547758.08'::money),"
            "(NULL,NULL,NULL,3,NULL,NULL,'retained',NULL,NULL,NULL,NULL)");
        for (const std::string_view format : {"fasta", "fastq"}) {
            const bool fastq = format == "fastq";
            for (const bool vectorized : {false, true}) {
                CAPTURE(header_type, format, vectorized);
                it::guc_scope vectorization(ctx, "bioseq.vectorization.enabled", vectorized ? "on" : "off");
                const std::string path = "/tmp/bioseq_write_text_" + backend + "." + std::string{format};
                std::filesystem::remove(path);
                it::exec_sql(ctx, "DROP TABLE IF EXISTS public.write_text_loaded; "
                    "CREATE TABLE public.write_text_loaded(header " + std::string{header_type} +
                    ",seq bioseq.seq('dna5',64)" + (fastq ? ",quality bioseq.seq('phred63')" : "") +
                    ",sample_no integer NOT NULL,sample text,target_start bigint,optional_note text,"
                    "scalar_array text[],regclass_value regclass,xml_content xml,amount money) USING bioseq; "
                    "SET search_path TO write_meta_first,public");
                REQUIRE(it::query_scalar_int(ctx,
                    "SELECT bioseq.write('public.write_text_source'::regclass,'" + path + "')") == 3);
                REQUIRE(it::query_scalar(ctx, "SHOW search_path") == "write_meta_first, public");
                REQUIRE(it::query_scalar(ctx, "SHOW array_nulls") == "off");
                REQUIRE(it::query_scalar(ctx, "SELECT upper(current_setting('xmloption'))") == "DOCUMENT");
                REQUIRE(it::query_scalar(ctx, "SHOW lc_monetary") == "C");
                it::exec_sql(ctx, "SET search_path TO write_meta_second,public");
                // Canonical metadata must supply required sample_no even though
                // that field is not part of the explicitly requested UniProt format.
                REQUIRE(it::query_scalar_int(ctx,
                    "SELECT bioseq.load('public.write_text_loaded'::regclass,'" + path +
                    "',header_format=>'uniprot')") == 3);
                REQUIRE(it::query_scalar(ctx, "SHOW search_path") == "write_meta_second, public");
                REQUIRE(it::query_scalar(ctx, "SHOW array_nulls") == "off");
                REQUIRE(it::query_scalar(ctx, "SELECT upper(current_setting('xmloption'))") == "DOCUMENT");
                REQUIRE(it::query_scalar(ctx, "SHOW lc_monetary") == "C");
                const std::string projection = "SELECT header::text,seq::text," +
                    std::string{fastq ? "quality::text," : ""} +
                    "sample_no,sample,target_start,optional_note,scalar_array,regclass_value::oid,"
                    "xml_content::text,encode(cash_send(amount),'hex') FROM ";
                REQUIRE(it::query_rows(ctx, projection + "public.write_text_loaded ORDER BY sample_no") ==
                        it::query_rows(ctx, projection + "public.write_text_source ORDER BY sample_no"));
                REQUIRE(it::query_one(ctx,
                    "SELECT scalar_array[1] IS NULL,scalar_array[2]='NULL',"
                    "regclass_value='write_meta_first.t'::regclass FROM public.write_text_loaded WHERE sample_no=1") ==
                    it::query_one(ctx, "SELECT true,true,true"));
                REQUIRE(it::query_one(ctx,
                    "SELECT header IS NULL,seq IS NULL FROM public.write_text_loaded WHERE sample_no=3") ==
                    it::query_one(ctx, "SELECT true,true"));
                REQUIRE(it::query_one(ctx,
                    "SELECT header::text,seq::text FROM public.write_text_loaded WHERE sample_no=2") ==
                    it::query_one(ctx, "SELECT ''::text,''::text"));
                std::filesystem::remove(path);
            }
        }
    }
    // Inspect nested SQL types at binding time, including NULL/empty values,
    // while preserving ordinary arrays/composites and deliberate text casts.
    it::exec_sql(ctx,
        "CREATE TYPE write_meta_first.scalar_pair AS (number integer,note text); "
        "CREATE TYPE write_meta_first.payload_pair AS "
        "(sequence bioseq.seq,quality bioseq.seq,cigar bioseq.seq,alignment bioseq.alignment); "
        "CREATE DOMAIN write_meta_first.payload_array AS bioseq.seq[]; "
        "CREATE TYPE write_meta_first.payload_range AS RANGE "
        "(subtype=bioseq.seq,multirange_type_name=write_meta_first.payload_multirange); "
        "CREATE TABLE write_meta_first.container_source(header text,seq bioseq.seq('dna5'),"
        "quality bioseq.seq('phred63'),scalar_array text[],scalar_record write_meta_first.scalar_pair,"
        "scalar_range int4range,scalar_multirange int4multirange,intentional_text text) USING bioseq; "
        "INSERT INTO write_meta_first.container_source VALUES "
        "('nested','ACNG','!!!!',ARRAY['a',NULL],ROW(7,'retained'),'[1,7)','{[1,7)}',(ARRAY['ACNG'::bioseq.seq('dna5')])::text),"
        "('empty','','',ARRAY[]::text[],ROW(0,''),'empty','{}',(ARRAY[]::bioseq.seq[])::text),"
        "('null',NULL,NULL,NULL,NULL,NULL,NULL,NULL); "
        "CREATE FUNCTION write_meta_first.opaque_record() RETURNS record LANGUAGE plpgsql STABLE AS "
        "$$BEGIN RETURN ROW(7,'opaque'::text); END$$");
    const std::string nested_query =
        "SELECT source.*,ARRAY[seq] AS omitted_sequence_array,ARRAY[quality] AS omitted_quality_array,"
        "ARRAY['1M'::bioseq.seq('cigar')] AS omitted_cigar_array,"
        "ARRAY[NULL::bioseq.alignment] AS omitted_alignment_array,"
        "ARRAY[seq]::write_meta_first.payload_array AS omitted_domain_array,"
        "NULL::write_meta_first.payload_pair AS omitted_null_composite,"
        "ROW(seq,quality,'1M'::bioseq.seq('cigar'),NULL::bioseq.alignment)"
        "::write_meta_first.payload_pair AS omitted_named_composite,"
        "ARRAY[]::bioseq.seq[] AS omitted_empty_array,"
        "write_meta_first.payload_range(seq,seq,'[]') AS omitted_sequence_range,"
        "write_meta_first.payload_multirange(write_meta_first.payload_range(seq,seq,'[]')) "
        "AS omitted_sequence_multirange,"
        "ROW(ARRAY[seq],ROW(quality,'1M'::bioseq.seq('cigar'))) AS omitted_anonymous_record,"
        "ARRAY[repeat('A',65536)::bioseq.seq('dna5')] AS omitted_large_array "
        "FROM write_meta_first.container_source source ORDER BY header";
    for (const std::string_view format : {"fasta", "fastq"}) {
        CAPTURE(format);
        const std::string path = "/tmp/bioseq_write_containers_" + backend + "." + std::string{format};
        std::filesystem::remove(path);
        it::exec_sql(ctx, "DROP TABLE IF EXISTS write_meta_first.container_loaded; "
            "CREATE TABLE write_meta_first.container_loaded(header text,seq bioseq.seq('dna5'),"
            "quality bioseq.seq('phred63'),scalar_array text[],scalar_record write_meta_first.scalar_pair,"
            "scalar_range int4range,scalar_multirange int4multirange,intentional_text text) USING bioseq");
        REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.write($query$" + nested_query +
            "$query$::text,'" + path + "')") == 3);
        const auto encoded = read_export_fixture(path);
        REQUIRE(encoded.size() < 32768);
        REQUIRE(encoded.find("omitted_") == std::string::npos);
        REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.load('write_meta_first.container_loaded'::regclass,'"
            + path + "')") == 3);
        const std::string fields = "SELECT header,seq::text," +
            std::string{format == "fastq" ? "quality::text," : ""} +
            "scalar_array,scalar_record::text,scalar_range::text,scalar_multirange::text,intentional_text FROM ";
        REQUIRE(it::query_rows(ctx, fields + "write_meta_first.container_loaded ORDER BY header") ==
            it::query_rows(ctx, fields + "write_meta_first.container_source ORDER BY header"));
        std::filesystem::remove(path);
    }
    const std::string rejected_path = "/tmp/bioseq_write_containers_" + backend + "_rejected.sam";
    std::filesystem::remove(rejected_path);
    for (const std::string_view role : {"metadata", "tag:ZZ"}) {
        CAPTURE(role);
        const auto rejected = it::sql_error(ctx, "SELECT bioseq.write($query$" + nested_query +
            "$query$::text,'" + rejected_path + "',columns=>ARRAY["
            "ROW('omitted_sequence_array','" + std::string{role} + "')::bioseq.column_mapping])");
        REQUIRE(rejected.sqlstate == "22023");
        REQUIRE(rejected.contains("payload containers"));
        REQUIRE_FALSE(std::filesystem::exists(rejected_path));
    }
    const std::string opaque_query = "SELECT header,seq,write_meta_first.opaque_record() AS opaque "
        "FROM write_meta_first.container_source ORDER BY header";
    const std::string opaque_path = "/tmp/bioseq_write_containers_" + backend + "_opaque.fasta";
    std::filesystem::remove(opaque_path);
    const auto unknown = it::sql_error(ctx, "SELECT bioseq.write($query$" + opaque_query +
        "$query$::text,'" + opaque_path + "')");
    REQUIRE(unknown.sqlstate == "22023");
    REQUIRE(unknown.contains("SQL pseudo-type"));
    REQUIRE_FALSE(std::filesystem::exists(opaque_path));
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.write($query$" + opaque_query +
        "$query$::text,'" + opaque_path + "',columns=>ARRAY[ROW('opaque','omit')::bioseq.column_mapping])") == 3);
    REQUIRE(read_export_fixture(opaque_path).find("opaque=") == std::string::npos);
    std::filesystem::remove(opaque_path);
    // Even a statically known scalar RECORD or VOID cannot become a table column.
    for (const std::string_view expression : {"ROW(7,'scalar'::text)", "pg_sleep(0)"}) {
        for (const bool explicit_metadata : {false, true}) {
            CAPTURE(expression, explicit_metadata);
            const std::string scalar_query = "SELECT header,seq," + std::string{expression}
                + " AS unstorable FROM write_meta_first.container_source ORDER BY header";
            const auto rejected = it::sql_error(ctx, "SELECT bioseq.write($query$" + scalar_query
                + "$query$::text,'" + opaque_path + "'" + (explicit_metadata
                    ? ",columns=>ARRAY[ROW('unstorable','metadata')::bioseq.column_mapping]" : "") + ")");
            REQUIRE(rejected.sqlstate == "22023");
            REQUIRE(rejected.contains("SQL pseudo-type"));
            REQUIRE_FALSE(std::filesystem::exists(opaque_path));
        }
    }
    const std::string cast_query = "SELECT header,seq,ROW(7,'scalar'::text)::text AS record_text,"
        "write_meta_first.opaque_record()::text AS opaque_text,pg_sleep(0)::text AS void_text "
        "FROM write_meta_first.container_source ORDER BY header";
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.write($query$" + cast_query
        + "$query$::text,'" + opaque_path + "')") == 3);
    it::exec_sql(ctx, "CREATE TABLE write_meta_first.cast_loaded(header text,seq bioseq.seq('dna5'),"
        "record_text text,opaque_text text,void_text text) USING bioseq");
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.load('write_meta_first.cast_loaded'::regclass,'"
        + opaque_path + "')") == 3);
    REQUIRE(it::query_rows(ctx, "SELECT header,seq::text,record_text,opaque_text,void_text "
        "FROM write_meta_first.cast_loaded ORDER BY header") == it::query_rows(ctx,
        "SELECT header,seq::text,'(7,scalar)'::text,'(7,opaque)'::text,''::text "
        "FROM write_meta_first.container_source ORDER BY header"));
    std::filesystem::remove(opaque_path);
    // A generic plan must classify a nested named type again after ALTER TYPE,
    // including a NULL value whose expression never exposes the nested fields.
    {
        it::guc_scope generic_plan(ctx, "plan_cache_mode", "force_generic_plan");
        const std::string schema_path = "/tmp/bioseq_write_type_schema_" + backend + ".fasta";
        std::filesystem::remove(schema_path);
        it::exec_sql(ctx, "CREATE TYPE write_meta_first.mutable_leaf AS (note integer); "
            "CREATE TYPE write_meta_first.mutable_wrapper AS (item write_meta_first.mutable_leaf); "
            "PREPARE write_metadata_type_schema AS SELECT bioseq.write("
            "$query$SELECT 'typed'::text AS header,'AC'::bioseq.seq('dna4') AS seq,"
            "NULL::write_meta_first.mutable_wrapper AS mutable_metadata$query$::text,'" + schema_path +
            "',options=>'{\"overwrite\":true}'::jsonb)");
        REQUIRE(it::query_scalar_int(ctx, "EXECUTE write_metadata_type_schema") == 1);
        REQUIRE(read_export_fixture(schema_path).find("mutable_metadata") != std::string::npos);
        it::exec_sql(ctx, "ALTER TYPE write_meta_first.mutable_leaf ADD ATTRIBUTE payload bioseq.seq");
        REQUIRE(it::query_scalar_int(ctx, "EXECUTE write_metadata_type_schema") == 1);
        REQUIRE(read_export_fixture(schema_path).find("mutable_metadata") == std::string::npos);
        it::exec_sql(ctx, "ALTER TYPE write_meta_first.mutable_leaf DROP ATTRIBUTE payload");
        REQUIRE(it::query_scalar_int(ctx, "EXECUTE write_metadata_type_schema") == 1);
        REQUIRE(read_export_fixture(schema_path).find("mutable_metadata") != std::string::npos);
        it::exec_sql(ctx, "DEALLOCATE write_metadata_type_schema");
        std::filesystem::remove(schema_path);
    }
    {
        it::guc_scope scalar_cap(ctx, "bioseq.index.memory_cap_mb", "256");
        it::guc_scope scalar_mode(ctx, "bioseq.scheduler.pipeline.mode", "'single_thread'");
        it::guc_scope scalar_overlap(ctx, "bioseq.load.write_overlap.enabled", "off");
        it::guc_scope scalar_timeout(ctx, "statement_timeout", "'60s'");
        it::guc_scope scalar_lock_timeout(ctx, "lock_timeout", "'3s'");
        const std::string prefix = (std::filesystem::temp_directory_path()
            / ("bioseq_scalar_input_" + backend)).string();
        const std::array<std::string, 2> paths{prefix + "_edges.fasta", prefix + "_large.fasta"};
        struct scalar_input_cleanup {
            const it::connection_test_context& ctx;
            const std::array<std::string, 2>& paths;
            ~scalar_input_cleanup() {
                if (PGresult* result = PQexec(ctx.raw(),
                        "DROP TABLE IF EXISTS public.load_scalar_admitted,public.load_scalar_text,"
                        "public.load_scalar_short"); result != nullptr) PQclear(result);
                for (const auto& path : paths) {
                    std::error_code ignored;
                    std::filesystem::remove(path, ignored);
                }
            }
        } cleanup{ctx, paths};
        const auto quote_literal = [&ctx](std::string_view value) {
            std::unique_ptr<char, decltype(&PQfreemem)> escaped{
                PQescapeLiteral(ctx.raw(), value.data(), value.size()), &PQfreemem};
            REQUIRE(escaped != nullptr);
            return std::string{escaped.get()};
        };
        constexpr std::string_view admission_sql =
            "SELECT index_memory_reserved_bytes FROM bioseq.scheduler_stats()";
        it::exec_sql(ctx,
            "DROP TABLE IF EXISTS public.load_scalar_admitted,public.load_scalar_text,public.load_scalar_short; "
            "CREATE TABLE public.load_scalar_admitted(header text,seq bioseq.seq('dna4'),note varchar,payload bytea) USING bioseq; "
            "CREATE TABLE public.load_scalar_text(header text,seq bioseq.seq('dna4'),note text,payload bytea) USING bioseq; "
            "CREATE TABLE public.load_scalar_short(header text,seq bioseq.seq('dna4'),note varchar(1),payload bytea) USING bioseq");
        for (const auto& path : paths) std::filesystem::remove(path);
        // Whitespace and binary values straddle the stack/heap boundary, including
        // empty bytea and 126/127 bytes (255/257 CString bytes with prefix + NUL).
        const std::string edges =
            "SELECT 'edge_'||n::text AS header,'AC'::bioseq.seq('dna4') AS seq,"
            "(CASE WHEN n=0 THEN '' ELSE ' '||repeat('x',n)||' ' END)::varchar AS note,"
            "substring(decode(repeat('007fff',n),'hex') FROM 1 FOR n) AS payload "
            "FROM (VALUES(0),(1),(126),(127),(253),(254),(255),(256)) AS v(n)";
        const std::string large =
            "SELECT 'large'::text AS header,'AC'::bioseq.seq('dna4') AS seq,"
            "('  '||repeat('x',2097152)||'  ')::varchar AS note,''::bytea AS payload";
        REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.write(" + quote_literal(edges)
            + "::text," + quote_literal(paths[0]) + ")") == 8);
        REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.write(" + quote_literal(large)
            + "::text," + quote_literal(paths[1]) + ")") == 1);
        REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.load('public.load_scalar_admitted'::regclass,"
            + quote_literal(paths[0]) + ")") == 8);
        const auto expected_edges = it::query_rows(ctx,
            "SELECT header,seq::text,note,encode(payload,'hex') FROM (" + edges + ") AS expected ORDER BY header");
        constexpr std::string_view admitted_rows =
            "SELECT header,seq::text,note,encode(payload,'hex') FROM public.load_scalar_admitted ORDER BY header";
        REQUIRE(it::query_rows(ctx, admitted_rows) == expected_edges);
        const std::string load_large = "SELECT bioseq.load('public.load_scalar_admitted'::regclass,"
            + quote_literal(paths[1]) + ")";
        it::exec_sql(ctx, "SET bioseq.index.memory_cap_mb=2");
        // scheduler_stats applies backend_sync before taking the baseline. The
        // 2 MiB cap leaves the 1 MiB raw-header chunk enough room to spill. The
        // direct TEXT path verifies this source can parse under that same cap.
        static_cast<void>(it::query_scalar_int(ctx, admission_sql));
        REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.load('public.load_scalar_text'::regclass,"
            + quote_literal(paths[1]) + ")") == 1);
        REQUIRE(it::query_scalar(ctx, "SELECT header='large' AND seq::text='AC' AND "
            "note=('  '||repeat('x',2097152)||'  ') AND payload=''::bytea FROM public.load_scalar_text") == "t");
        // The writable scalar copy requests >4 MiB through the vector helper.
        // Require its exact diagnostic, not an earlier parser refusal.
        const auto reserved = it::query_scalar_int(ctx, admission_sql);
        for (unsigned attempt = 0; attempt != 2; ++attempt) {
            CAPTURE(attempt);
            const auto error = it::sql_error(ctx, load_large);
            CAPTURE(error.message);
            REQUIRE(error.sqlstate == "53200");
            REQUIRE(error.contains("cannot admit canonical scalar input"));
            REQUIRE(it::query_scalar_int(ctx, admission_sql) == reserved);
            REQUIRE(it::query_rows(ctx, admitted_rows) == expected_edges);
            REQUIRE(it::query_scalar(ctx, "SELECT pg_backend_pid()") == backend);
        }
        it::exec_sql(ctx, "SET bioseq.index.memory_cap_mb=256");
        static_cast<void>(it::query_scalar_int(ctx, admission_sql));
        REQUIRE(it::query_scalar_int(ctx, load_large) == 1);
        REQUIRE(it::query_scalar(ctx, "SELECT seq::text='AC' AND note=('  '||repeat('x',2097152)||'  ') "
            "AND payload=''::bytea FROM public.load_scalar_admitted WHERE header='large'") == "t");
        REQUIRE(it::query_scalar_int(ctx, "SELECT count(*) FROM public.load_scalar_admitted") == 9);
        REQUIRE(it::query_scalar_int(ctx, admission_sql) == reserved);
        const auto before_type_error = it::query_scalar_int(ctx, admission_sql);
        // The admitted heap owner must also unwind when PostgreSQL type input
        // rejects the value after receiving its mutable CString.
        const auto type_error = it::sql_error(ctx, "SELECT bioseq.load('public.load_scalar_short'::regclass,"
            + quote_literal(paths[1]) + ")");
        CAPTURE(type_error.message);
        REQUIRE(type_error.sqlstate == "22001");
        REQUIRE(it::query_scalar_int(ctx, admission_sql) == before_type_error);
        REQUIRE(it::query_scalar_int(ctx, "SELECT count(*) FROM public.load_scalar_short") == 0);
        REQUIRE(it::query_scalar(ctx, "SELECT pg_backend_pid()") == backend);
        it::exec_sql(ctx, "DROP TABLE public.load_scalar_admitted,public.load_scalar_text,public.load_scalar_short");
        for (const auto& path : paths) REQUIRE(std::filesystem::remove(path));
    }
    it::exec_sql(ctx, "DROP SCHEMA write_meta_first CASCADE; DROP SCHEMA write_meta_second CASCADE");
}

TEST_CASE("bioseq.write: mapped nucleotide and protein SAM BAM preserve original coordinates",
          "[component:bioseq][layer:integration][feature:io][resource:postgres][resource:filesystem]") {
    auto ctx = it::fresh_context({"write_alignment_source", "write_alignment_loaded", "write_alignment_again"});
    it::guc_scope maintenance(ctx, "bioseq.index.insert_maintenance.enabled", "off");
    const auto backend = it::query_scalar(ctx, "SELECT pg_backend_pid()");
    for (const bool protein : {false, true}) {
        const std::string alphabet = protein ? "aa27" : "dna15";
        const std::string sequence = protein ? "MW" : "AM";
        it::exec_sql(ctx, "DROP TABLE IF EXISTS write_alignment_source,write_alignment_loaded,write_alignment_again; "
            "CREATE TABLE write_alignment_source(header text,seq bioseq.seq('" + alphabet +
            "',64),quality bioseq.seq('phred63'),sample text,cigar bioseq.seq('cigar'),"
            "flag integer,mapq integer,reference_name text,reference_length bigint,"
            "target_start bigint,target_end bigint,query_length bigint,query_start bigint,query_end bigint) USING bioseq; "
            "INSERT INTO write_alignment_source VALUES "
            "('forward read','" + sequence + "','!I','kept','2M',0,37,'reference',100,7,9,2,0,2),"
            "('reverse read','" + sequence + "','!I','kept','2M',16,37,'reference',100,7,9,2,0,2)");
        const auto expected = it::query_rows(ctx,
            "SELECT header,seq::text,quality::text,sample,flag,mapq,reference_name,reference_length,"
            "target_start,target_end,query_length,query_start,query_end,cigar::text "
            "FROM write_alignment_source ORDER BY header");
        for (const std::string_view format : {"sam", "bam"}) {
            CAPTURE(alphabet, format);
            const std::string path = "/tmp/bioseq_write_alignment_" + backend + "." + std::string{format};
            const std::string again_path = "/tmp/bioseq_write_alignment_again_" + backend + "." + std::string{format};
            std::filesystem::remove(path);
            std::filesystem::remove(again_path);
            it::exec_sql(ctx, "DROP TABLE IF EXISTS write_alignment_loaded,write_alignment_again; "
                "CREATE TABLE write_alignment_loaded(header text,seq bioseq.seq('" + alphabet +
                "',64),quality bioseq.seq('phred63'),sample text) USING bioseq; "
                "CREATE TABLE write_alignment_again(LIKE write_alignment_loaded) USING bioseq");
            REQUIRE(it::query_scalar_int(ctx,
                "SELECT bioseq.write($query$SELECT * FROM write_alignment_source ORDER BY header$query$::text,'" + path + "')") == 2);
            if (format == "sam") {
                const auto text = read_export_fixture(path);
                REQUIRE(text.find(protein ? "\tLN:300" : "\tLN:100") != std::string::npos);
                const auto records = exported_sam_records(text);
                REQUIRE(records.size() == 2);
                for (std::size_t index = 0; index < records.size(); ++index) {
                    REQUIRE(records[index][1] == (index == 0 ? "0" : "16"));
                    REQUIRE(records[index][3] == (protein ? "22" : "8"));
                    REQUIRE(records[index][4] == "37");
                    REQUIRE(records[index][5] == (protein ? "6M" : "2M"));
                    REQUIRE(records[index][6] == "*");
                    REQUIRE(records[index][7] == "0");
                    REQUIRE(records[index][8] == "0");
                }
                REQUIRE(records[0][9] == (protein ? "ATGTGG" : "AM"));
                REQUIRE(records[1][9] == (protein ? "CCACAT" : "KT"));
                REQUIRE(records[0][10] == (protein ? "!!!III" : "!I"));
                REQUIRE(records[1][10] == (protein ? "III!!!" : "I!"));
            }
            REQUIRE(it::query_scalar_int(ctx,
                "SELECT bioseq.load('write_alignment_loaded'::regclass,'" + path + "')") == 2);
            const std::string projection =
                "SELECT header,seq::text,quality::text,sample,(alignment).flag,(alignment).mapq,"
                "(alignment).reference_name,(alignment).reference_length,(alignment).target_start,"
                "(alignment).target_end,(alignment).query_length,(alignment).query_start,"
                "(alignment).query_end,(alignment).cigar::text FROM ";
            REQUIRE(it::query_rows(ctx, projection + "write_alignment_loaded ORDER BY header") == expected);
            REQUIRE(it::query_scalar_int(ctx,
                "SELECT bioseq.write('write_alignment_loaded'::regclass,'" + again_path + "')") == 2);
            REQUIRE(it::query_scalar_int(ctx,
                "SELECT bioseq.load('write_alignment_again'::regclass,'" + again_path + "')") == 2);
            REQUIRE(it::query_rows(ctx, projection + "write_alignment_again ORDER BY header") == expected);
            std::filesystem::remove(path);
            std::filesystem::remove(again_path);
        }
    }
}

TEST_CASE("bioseq.write: automatic alignment joins retain both source row identities",
          "[component:bioseq][layer:integration][feature:io][resource:postgres][resource:filesystem]") {
    auto ctx = it::fresh_context({"write_join_refs", "write_join_reads", "write_join_loaded",
        "write_join_flags_loaded", "write_set_barrier_loaded"});
    it::guc_scope maintenance(ctx, "bioseq.index.insert_maintenance.enabled", "off");
    it::guc_scope index_read(ctx, "bioseq.index.sequence_read.enabled", "off");
    it::guc_scope minimum_score(ctx, "bioseq.aligner.chaining.minimum_score", "30");
    it::guc_scope minimum_seeds(ctx, "bioseq.aligner.chaining.minimum_seed_count", "3");
    const auto backend = it::query_scalar(ctx, "SELECT pg_backend_pid()");
    it::exec_sql(ctx,
        "CREATE TABLE write_join_refs(id integer,header text,seq bioseq.seq('dna4',64)) USING bioseq; "
        "CREATE TABLE write_join_reads(id integer,header text,seq bioseq.seq('dna4',64),"
        "quality bioseq.seq('phred63')) USING bioseq");
    for (const int identifier : {1, 2}) {
        const auto reference = gen_bases(640u, 1547u + static_cast<std::uint64_t>(identifier), false);
        it::exec_sqlf(ctx,
            "INSERT INTO write_join_refs VALUES ({0},'reference_{0}','{1}'); "
            "INSERT INTO write_join_reads VALUES "
            "({0},'read_{0}a','{2}',repeat('I',512)::bioseq.seq('phred63')),"
            "({0},'read_{0}b','{2}',repeat('J',512)::bioseq.seq('phred63'));",
            identifier, reference, reference.substr(47u, 512u));
    }
    const std::string join =
        " FROM write_join_refs t JOIN write_join_reads q ON t.id=q.id ";
    const std::string matches = "WHERE public.mmsearch(t.seq,q.seq) ORDER BY q.header";
    const auto identities = it::query_rows(ctx,
        "SELECT t.ctid::text,t.tableoid::text,q.ctid::text,q.tableoid::text" + join + "ORDER BY q.header");
    const auto expected = it::query_rows(ctx,
        "SELECT q.header,q.seq::text,q.quality::text,t.header,bioseq.target_len(t.seq),"
        "bioseq.target_start(t.seq),bioseq.target_end(t.seq),bioseq.query_len(t.seq),"
        "bioseq.query_start(t.seq),bioseq.query_end(t.seq),bioseq.cigar(t.seq)::text" + join + matches);
    REQUIRE(expected.size() == 4);
    for (const auto& row : expected) {
        REQUIRE(row[5] == "47");
        REQUIRE(row[6] == "559");
        REQUIRE(row[10] == "512M");
    }
    for (const std::string_view vectorization : {"off", "on"}) {
        it::guc_scope vectorized(ctx, "bioseq.vectorization.enabled", vectorization);
        for (const std::string_view cache : {"off", "on"}) {
            it::guc_scope cached(ctx, "bioseq.column_cache.enabled", cache);
            CAPTURE(vectorization, cache);
            const std::string identity_query =
                "SELECT t.ctid::text,t.tableoid::text,q.ctid::text,q.tableoid::text" + join + matches;
            REQUIRE(it::query_rows(ctx, identity_query) == identities);
            REQUIRE(it::plan_contains(it::explain_text<true>(ctx, identity_query), "BioseqBatchJoin"));
            for (const std::string_view format : {"sam", "bam"}) {
                CAPTURE(format);
                const auto path = "/tmp/bioseq_write_join_" + backend + "." + std::string{format};
                std::filesystem::remove(path);
                const std::string request = "SELECT bioseq.write($query$SELECT q.seq" + join + matches
                    + "$query$::text,'" + path + "')";
                const auto plan = it::explain_text<false>(ctx, request);
                REQUIRE(it::plan_contains(plan, "BioseqExport"));
                REQUIRE(it::plan_contains(plan, "BioseqBatchJoin"));
                REQUIRE_FALSE(std::filesystem::exists(path));
                REQUIRE(it::query_scalar_int(ctx, request) == 4);
                if (format == "sam") {
                    const auto records = exported_sam_records(read_export_fixture(path));
                    REQUIRE(records.size() == 4);
                    for (const auto& record : records) {
                        REQUIRE(record[3] == "48");
                        REQUIRE(record[5] == "512M");
                        REQUIRE(std::find(record.begin(), record.end(), "NM:i:0") != record.end());
                        REQUIRE(std::find(record.begin(), record.end(), "MD:Z:512") != record.end());
                    }
                }
                it::exec_sql(ctx, "DROP TABLE IF EXISTS write_join_loaded; "
                    "CREATE TABLE write_join_loaded(header text,seq bioseq.seq('dna4',64),"
                    "quality bioseq.seq('phred63')) USING bioseq");
                REQUIRE(it::query_scalar_int(ctx,
                    "SELECT bioseq.load('write_join_loaded'::regclass,'" + path + "')") == 4);
                REQUIRE(it::query_rows(ctx,
                    "SELECT header,seq::text,quality::text,(alignment).reference_name,"
                    "(alignment).reference_length,(alignment).target_start,(alignment).target_end,"
                    "(alignment).query_length,(alignment).query_start,(alignment).query_end,"
                    "(alignment).cigar::text FROM write_join_loaded ORDER BY header") == expected);
                std::filesystem::remove(path);
                // Owner classification covers only reverse/secondary/supplementary.
                // Preserve unrelated Boolean bits under every existing scan setting.
                for (const auto& [projection, flags] : std::vector<std::pair<std::string, unsigned>>{
                        {"true AS duplicate", 1024u}, {"true AS fails_qc", 512u},
                        {"1024 AS flag", 1024u}, {"512 AS flag", 512u}}) {
                    CAPTURE(format, projection, flags);
                    std::filesystem::remove(path);
                    const auto request = "SELECT bioseq.write($query$SELECT q.seq," + projection + join + matches
                        + "$query$::text,'" + path + "')";
                    const auto plan = it::explain_text<false>(ctx, request);
                    REQUIRE(it::plan_contains(plan, "BioseqExport"));
                    REQUIRE(it::plan_contains(plan, "BioseqBatchJoin"));
                    REQUIRE_FALSE(std::filesystem::exists(path));
                    REQUIRE(it::query_scalar_int(ctx, request) == 4);
                    const auto native = inspect_exported_native(path, format);
                    REQUIRE(native.names.size() == 4u);
                    REQUIRE(native.flags.size() == 4u);
                    for (const auto native_flag : native.flags) REQUIRE(native_flag == flags);
                    it::exec_sql(ctx, "DROP TABLE IF EXISTS write_join_flags_loaded; "
                        "CREATE TABLE write_join_flags_loaded(header text,seq bioseq.seq('dna4',64),"
                        "quality bioseq.seq('phred63')) USING bioseq");
                    REQUIRE(it::query_scalar_int(ctx,
                        "SELECT bioseq.load('write_join_flags_loaded'::regclass,'" + path + "')") == 4);
                    REQUIRE(it::query_scalar_int(ctx, "SELECT count(*) FROM write_join_flags_loaded WHERE "
                        "(alignment).flag=" + std::to_string(flags)) == 4);
                    REQUIRE(it::query_rows(ctx,
                        "SELECT header,seq::text,quality::text,(alignment).reference_name,"
                        "(alignment).reference_length,(alignment).target_start,(alignment).target_end,"
                        "(alignment).query_length,(alignment).query_start,(alignment).query_end,"
                        "(alignment).cigar::text FROM write_join_flags_loaded ORDER BY header") == expected);
                    std::filesystem::remove(path);
                }
                // Map the visible constraint before automatic helper completion;
                // otherwise the inferred reverse helper competes with its alias.
                // The private owner status still supplies the authoritative bits.
                for (const auto& [projection, role] : std::vector<std::pair<std::string_view, std::string_view>>{
                        {"true AS reversed", "reversed"}, {"true AS secondary", "secondary"},
                        {"true AS supplementary", "supplementary"},
                        {"16 AS flag", "flags"}, {"256 AS flag", "flags"}, {"2048 AS flag", "flags"}}) {
                    CAPTURE(format, projection, role);
                    const auto request = "SELECT bioseq.write($query$SELECT q.seq," + std::string{projection} + join + matches
                        + "$query$::text,'" + path + "',columns=>ARRAY[ROW('#2','" + std::string{role}
                        + "')::bioseq.column_mapping])";
                    const auto error = it::sql_error(ctx, request);
                    REQUIRE(error.sqlstate == "22023");
                    REQUIRE(error.contains("owning alignment classification"));
                    REQUIRE_FALSE(std::filesystem::exists(path));
                }
            }
        }
    }

    it::exec_sql(ctx, "ALTER TABLE write_join_loaded ADD COLUMN reference_length bigint DEFAULT -1");
    // Each branch has its own executable alignment owner. Explicitly supplied
    // rows may cross UNION ALL; the export root must not adopt either branch's
    // owner or ask that owner to complete fields for rows from the other branch.
    const auto branch = [&join](int identifier) {
        return "SELECT q.header,q.seq,q.quality,t.seq AS reference_sequence,"
            "t.header AS reference_name,bioseq.target_len(t.seq) AS size_of_reference,"
            "bioseq.target_start(t.seq) AS locus,bioseq.target_end(t.seq) AS locus_end,"
            "bioseq.query_len(t.seq) AS read_size,bioseq.query_start(t.seq) AS clipped_left,"
            "bioseq.query_end(t.seq) AS clipped_right,bioseq.cigar(t.seq) AS recipe,"
            "bioseq.is_reverse(t.seq) AS backward,0 AS flag,37 AS mapq" + join
            + "WHERE public.mmsearch(t.seq,q.seq) AND t.id=" + std::to_string(identifier);
    };
    auto set_query = "WITH merged AS MATERIALIZED (" + branch(1) + " UNION ALL " + branch(2)
        + ") SELECT * FROM (SELECT * FROM merged) projected ORDER BY header";
    for (unsigned level = 0; level != 16; ++level)
        set_query = "SELECT * FROM (" + set_query + ") projected_" + std::to_string(level);
    const auto require_owned_plan = [](const std::vector<std::string>& plan, std::size_t owners) {
        // Plain BioseqScan nodes also read tables. Require the existing mmsearch
        // owner property once per branch and no executable rendezvous join filter.
        REQUIRE(static_cast<std::size_t>(std::count_if(plan.begin(), plan.end(), [](const auto& line) {
            return line.find("Bioseq Index Read Requirement:") != std::string::npos;
        })) == owners);
        REQUIRE(it::plan_contains(plan, "Nested Loop"));
        REQUIRE(std::none_of(plan.begin(), plan.end(), [](const auto& line) {
            return line.find("Join Filter:") != std::string::npos
                && line.find("mmsearch(") != std::string::npos;
        }));
    };
    auto explicit_expected = expected;
    for (std::size_t index = 0; index != explicit_expected.size(); ++index) {
        auto& row = explicit_expected[index];
        row.insert(row.begin() + 3, gen_bases(640u, 1548u + index / 2u, false));
        row.emplace_back("f");
        row.emplace_back("0");
        row.emplace_back("37");
    }
    {
        it::guc_scope custom_scans(ctx, "bioseq.planner.custom_scan.enabled", "off");
        const auto error = it::sql_error(ctx, branch(1) + " ORDER BY q.header");
        REQUIRE(error.sqlstate == "0A000");
        REQUIRE(error.contains("readable mmsearch target requires a parameterized BioseqScan owner"));
    }
    for (const std::string_view vectorized : {"off", "on"}) {
        it::guc_scope vectorization(ctx, "bioseq.vectorization.enabled", vectorized);
        for (const std::string_view pushdown : {"off", "on"}) {
            it::guc_scope pushed(ctx, "bioseq.planner.qual_pushdown.enabled", pushdown);
            for (const std::string_view cache : {"off", "on"}) {
                it::guc_scope cached(ctx, "bioseq.column_cache.enabled", cache);
                CAPTURE(vectorized, pushdown, cache);
                for (const int identifier : {1, 2}) {
                    CAPTURE(identifier);
                    const auto source = branch(identifier) + " ORDER BY q.header";
                    require_owned_plan(it::explain_text<false>(ctx, source), 1u);
                    // Keep the bare reference_sequence output; an upper projection
                    // could prune it and accidentally make the locator BatchJoin legal.
                    const auto rows = it::query_rows(ctx, source);
                    REQUIRE(rows.size() == 2);
                    REQUIRE(rows[0] == explicit_expected[2u * static_cast<unsigned>(identifier - 1)]);
                    REQUIRE(rows[1] == explicit_expected[2u * static_cast<unsigned>(identifier - 1) + 1u]);
                }
                require_owned_plan(it::explain_text<false>(ctx, set_query), 2u);
                REQUIRE(it::query_rows(ctx, set_query + " ORDER BY header") == explicit_expected);
            }
        }
    }
    const std::string mappings = "ARRAY[ROW('seq','sequence')::bioseq.column_mapping,"
        "ROW('reference_sequence','reference_sequence')::bioseq.column_mapping]";
    for (const std::string_view format : {"sam", "bam"}) {
        const auto path = "/tmp/bioseq_write_explicit_set_" + backend + "." + std::string{format};
        std::filesystem::remove(path);
        const auto request = "SELECT bioseq.write($query$" + set_query + "$query$::text,'" + path
            + "',columns=>" + mappings + ")";
        const auto plan = it::explain_text<false>(ctx, request);
        REQUIRE(it::plan_contains(plan, "BioseqExport"));
        require_owned_plan(plan, 2u);
        REQUIRE(it::plan_contains(plan, "Alignment Owner: false"));
        REQUIRE_FALSE(std::filesystem::exists(path));
        REQUIRE(it::query_scalar_int(ctx, request) == 4);
        if (format == "sam") {
            const auto records = exported_sam_records(read_export_fixture(path));
            REQUIRE(records.size() == 4);
            for (const auto& record : records) {
                REQUIRE(record[1] == "0");
                REQUIRE(record[3] == "48");
                REQUIRE(record[4] == "37");
                REQUIRE(record[5] == "512M");
            }
        }
        it::exec_sql(ctx, "TRUNCATE write_join_loaded");
        REQUIRE(it::query_scalar_int(ctx,
            "SELECT bioseq.load('write_join_loaded'::regclass,'" + path + "')") == 4);
        REQUIRE(it::query_rows(ctx,
            "SELECT header,seq::text,quality::text,(alignment).reference_name,"
            "(alignment).reference_length,(alignment).target_start,(alignment).target_end,"
            "(alignment).query_length,(alignment).query_start,(alignment).query_end,"
            "(alignment).cigar::text FROM write_join_loaded ORDER BY header") == expected);
        std::filesystem::remove(path);
        // A transformed helper in either branch remains scalar metadata even
        // under a canonical native-field alias. Reference sequences still supply
        // native dictionary lengths, but the original optional row length is absent.
        auto mixed_expected = expected;
        for (auto& row : mixed_expected) {
            row[4] = std::nullopt;
            row.emplace_back("0");
            row.emplace_back("37");
        }
        const std::vector<std::pair<std::string, std::uint32_t>> expected_references{
            {"reference_1", 640u}, {"reference_2", 640u}};
        const std::vector<std::string> expected_names{"read_1a", "read_1b", "read_2a", "read_2b"};
        for (const bool transformed_first : {false, true}) {
            const auto change_length = [](std::string query, bool transformed) {
                const std::string from = "bioseq.target_len(t.seq) AS size_of_reference";
                const auto found = query.find(from);
                REQUIRE(found != std::string::npos);
                query.replace(found, from.size(), std::string{"bioseq.target_len(t.seq)"}
                    + (transformed ? "+1" : "") + " AS reference_length");
                return query;
            };
            const auto mixed_query = "WITH merged AS MATERIALIZED ("
                + change_length(branch(1), transformed_first) + " UNION ALL "
                + change_length(branch(2), !transformed_first) + ") SELECT * FROM merged ORDER BY header";
            REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.write($query$" + mixed_query
                + "$query$::text,'" + path + "',columns=>" + mappings + ")") == 4);
            const auto native = inspect_exported_native(path, format);
            REQUIRE(native.names == expected_names);
            REQUIRE(native.references == expected_references);
            it::exec_sql(ctx, "TRUNCATE write_join_loaded");
            REQUIRE(it::query_scalar_int(ctx,
                "SELECT bioseq.load('write_join_loaded'::regclass,'" + path + "')") == 4);
            REQUIRE(it::query_scalar(ctx,
                "SELECT string_agg(reference_length::text,',' ORDER BY header) FROM write_join_loaded")
                == (transformed_first ? "641,641,640,640" : "640,640,641,641"));
            REQUIRE(it::query_rows(ctx,
                "SELECT header,seq::text,quality::text,(alignment).reference_name,"
                "(alignment).reference_length,(alignment).target_start,(alignment).target_end,"
                "(alignment).query_length,(alignment).query_start,(alignment).query_end,"
                "(alignment).cigar::text,(alignment).flag,(alignment).mapq "
                "FROM write_join_loaded ORDER BY header") == mixed_expected);
            std::filesystem::remove(path);
        }
        const auto outer_query = "SELECT q.seq" + join
            + " CROSS JOIN (SELECT 1 AS marker UNION ALL SELECT 2) unrelated "
            + "WHERE public.mmsearch(t.seq,q.seq) AND unrelated.marker=1 ORDER BY q.header";
        const auto outer_request = "SELECT bioseq.write($query$" + outer_query
            + "$query$::text,'" + path + "')";
        const auto outer_plan = it::explain_text<false>(ctx, outer_request);
        REQUIRE(it::plan_contains(outer_plan, "BioseqExport"));
        REQUIRE(it::plan_contains(outer_plan, "Alignment Owner: true"));
        REQUIRE(std::count_if(outer_plan.begin(), outer_plan.end(), [](const auto& line) {
            return line.find("Bioseq Index Read Requirement:") != std::string::npos;
        }) == 1);
        REQUIRE(std::none_of(outer_plan.begin(), outer_plan.end(), [](const auto& line) {
            return line.find("Join Filter:") != std::string::npos
                && line.find("mmsearch(") != std::string::npos;
        }));
        REQUIRE(it::query_scalar_int(ctx, outer_request) == 4);
        if (format == "sam") {
            const auto records = exported_sam_records(read_export_fixture(path));
            REQUIRE(records.size() == 4u);
            for (const auto& record : records) {
                REQUIRE(record[1] == "0");
                REQUIRE(record[3] == "48");
                REQUIRE(record[5] == "512M");
            }
        }
        it::exec_sql(ctx, "TRUNCATE write_join_loaded");
        REQUIRE(it::query_scalar_int(ctx,
            "SELECT bioseq.load('write_join_loaded'::regclass,'" + path + "')") == 4);
        REQUIRE(it::query_rows(ctx,
            "SELECT header,seq::text,quality::text,(alignment).reference_name,"
            "(alignment).reference_length,(alignment).target_start,(alignment).target_end,"
            "(alignment).query_length,(alignment).query_start,(alignment).query_end,"
            "(alignment).cigar::text FROM write_join_loaded ORDER BY header") == expected);
        std::filesystem::remove(path);
        // The outer equality may use either ordinary join algorithm above
        // the one real alignment owner. Preserve its rows and native geometry.
        for (const std::string_view algorithm : {"hash", "merge"}) {
            it::guc_scope nested(ctx, "enable_nestloop", "off");
            it::guc_scope hashed(ctx, "enable_hashjoin", algorithm == "hash" ? "on" : "off");
            it::guc_scope merged(ctx, "enable_mergejoin", algorithm == "merge" ? "on" : "off");
            CAPTURE(format, algorithm);
            const auto equality_query = "SELECT q.seq" + join
                + " JOIN write_join_refs u ON t.id=u.id "
                + "WHERE public.mmsearch(t.seq,q.seq) ORDER BY q.header";
            const auto equality_request = "SELECT bioseq.write($query$" + equality_query
                + "$query$::text,'" + path + "')";
            const auto equality_plan = it::explain_text<false>(ctx, equality_request);
            REQUIRE(it::plan_contains(equality_plan, "BioseqExport"));
            REQUIRE(it::plan_contains(equality_plan, "Alignment Owner: true"));
            REQUIRE(it::plan_contains(equality_plan, algorithm == "hash" ? "Hash Join" : "Merge Join"));
            REQUIRE(std::count_if(equality_plan.begin(), equality_plan.end(), [](const auto& line) {
                return line.find("Bioseq Index Read Requirement:") != std::string::npos;
            }) == 1);
            REQUIRE(std::none_of(equality_plan.begin(), equality_plan.end(), [](const auto& line) {
                return line.find("Join Filter:") != std::string::npos
                    && line.find("mmsearch(") != std::string::npos;
            }));
            REQUIRE(it::query_scalar_int(ctx, equality_request) == 4);
            if (format == "sam") {
                const auto records = exported_sam_records(read_export_fixture(path));
                REQUIRE(records.size() == 4u);
                for (const auto& record : records) {
                    REQUIRE(record[1] == "0");
                    REQUIRE(record[3] == "48");
                    REQUIRE(record[5] == "512M");
                }
            }
            it::exec_sql(ctx, "TRUNCATE write_join_loaded");
            REQUIRE(it::query_scalar_int(ctx,
                "SELECT bioseq.load('write_join_loaded'::regclass,'" + path + "')") == 4);
            REQUIRE(it::query_rows(ctx,
                "SELECT header,seq::text,quality::text,(alignment).reference_name,"
                "(alignment).reference_length,(alignment).target_start,(alignment).target_end,"
                "(alignment).query_length,(alignment).query_start,(alignment).query_end,"
                "(alignment).cigar::text FROM write_join_loaded ORDER BY header") == expected);
            std::filesystem::remove(path);
        }
        const auto conflicting_query = "SELECT q.seq,unrelated.reference_length" + join
            + " CROSS JOIN (SELECT bioseq.target_len('AC'::bioseq.seq('dna4')) AS reference_length "
            + "UNION ALL SELECT bioseq.target_len('GT'::bioseq.seq('dna4'))) unrelated "
            + "WHERE public.mmsearch(t.seq,q.seq)";
        const auto conflicting = it::sql_error(ctx, "SELECT bioseq.write($query$"
            + conflicting_query + "$query$::text,'" + path + "')");
        REQUIRE(conflicting.sqlstate == "22023");
        REQUIRE(conflicting.contains("one coherent mmsearch owner"));
        REQUIRE_FALSE(std::filesystem::exists(path));
        // A set arm's correlated terminal Var is already paired with its resolved
        // owning frame. Applying its original varlevelsup again would mistake
        // this plain integer for the unrelated unused helper two levels above.
        const std::string correlated_query =
            "SELECT 'scope_'||row_number() OVER ()::text AS header,'AC'::bioseq.seq('dna4') AS seq,"
            "outer_layer.mapq FROM (SELECT bioseq.target_len('AC'::bioseq.seq('dna4')) AS unused,"
            "inner_layer.mapq FROM (SELECT t.id AS unused,alternatives.mapq FROM write_join_refs t "
            "CROSS JOIN LATERAL (SELECT t.id AS mapq UNION ALL SELECT t.id) alternatives) inner_layer) "
            "outer_layer ORDER BY outer_layer.mapq";
        REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.write($query$" + correlated_query
            + "$query$::text,'" + path + "')") == 4);
        if (format == "sam") {
            const auto records = exported_sam_records(read_export_fixture(path));
            REQUIRE(records.size() == 4);
            for (std::size_t index = 0; index < records.size(); ++index)
                REQUIRE(records[index][4] == (index < 2 ? "1" : "2"));
        }
        it::exec_sql(ctx, "TRUNCATE write_join_loaded");
        REQUIRE(it::query_scalar_int(ctx,
            "SELECT bioseq.load('write_join_loaded'::regclass,'" + path + "')") == 4);
        REQUIRE(it::query_scalar(ctx,
            "SELECT string_agg((alignment).mapq::text,',' ORDER BY (alignment).mapq) FROM write_join_loaded") == "1,1,2,2");
        std::filesystem::remove(path);
    }
    // A barrier suppresses owner inference, not the obligation to preserve a
    // reachable real match. These shapes must fail before any native output.
    const auto bare_target = "SELECT t.header,t.seq" + join
        + "WHERE public.mmsearch(t.seq,q.seq) AND t.id=1";
    const auto recursive_source = "WITH RECURSIVE matched(depth,header,seq) AS (SELECT 0,t.header,t.seq"
        + join + "WHERE public.mmsearch(t.seq,q.seq) AND t.id=1 UNION ALL "
        "SELECT depth+1,header,seq FROM matched WHERE depth<1) ";
    const std::vector<std::string> opaque_sources{
        bare_target + " UNION ALL " + bare_target,
        bare_target + " UNION " + bare_target,
        "WITH merged AS MATERIALIZED (" + bare_target + " UNION ALL " + bare_target
            + ") SELECT * FROM (SELECT * FROM merged) wrapped",
        recursive_source + "SELECT header,seq FROM matched",
        recursive_source + "SELECT header,seq FROM matched UNION ALL SELECT header,seq FROM matched"};
    const auto bare_reads = "SELECT q.header,q.seq,q.quality" + join
        + "WHERE public.mmsearch(t.seq,q.seq) AND t.id=1";
    const auto repeated_reads = "(" + bare_reads + " UNION ALL " + bare_reads + ") repeated";
    const auto reset_barrier_destination = [&ctx] {
        // Every load owns fresh provenance, independently of earlier imports.
        it::exec_sql(ctx, "DROP TABLE IF EXISTS write_set_barrier_loaded; "
            "CREATE TABLE write_set_barrier_loaded(header text,seq bioseq.seq('dna4',64),"
            "quality bioseq.seq('phred63')) USING bioseq");
    };
    for (const std::string_view format : {"sam", "bam"}) {
        CAPTURE(format);
        const auto path = "/tmp/bioseq_write_match_barrier_" + backend + "." + std::string{format};
        std::filesystem::remove(path);
        // An unrelated projected sequence has a discoverable NULL alignment.
        // Completion must not append that hidden old record and use it to erase
        // the opaque match obligation. Selecting it visibly is explicit intent.
        it::exec_sql(ctx, "DROP TABLE IF EXISTS write_set_barrier_loaded; "
            "CREATE TABLE write_set_barrier_loaded(header text,seq bioseq.seq('dna4',64),"
            "alignment bioseq.alignment) USING bioseq; "
            "INSERT INTO write_set_barrier_loaded VALUES('prior','ACGT',NULL)");
        const auto unrelated_from = " FROM write_set_barrier_loaded prior CROSS JOIN " + repeated_reads;
        const auto hidden_request = "SELECT bioseq.write($query$SELECT prior.seq" + unrelated_from
            + "$query$::text,'" + path + "')";
        const auto hidden_error = it::sql_error(ctx, "EXPLAIN (COSTS OFF) " + hidden_request);
        REQUIRE(hidden_error.sqlstate == "22023");
        REQUIRE(hidden_error.contains("export completion cannot cross"));
        REQUIRE(hidden_error.contains("missing CIGAR"));
        REQUIRE_FALSE(std::filesystem::exists(path));
        const auto visible_request = "SELECT bioseq.write($query$SELECT prior.header,prior.seq,prior.alignment"
            + unrelated_from + "$query$::text,'" + path + "')";
        const auto visible_plan = it::explain_text<false>(ctx, visible_request);
        REQUIRE(it::plan_contains(visible_plan, "Alignment Owner: false"));
        REQUIRE_FALSE(it::plan_contains(visible_plan, "Explicit Alignment Fields Required"));
        REQUIRE(it::query_scalar_int(ctx, visible_request) == 4);
        REQUIRE(inspect_exported_native(path, format).references.empty());
        std::filesystem::remove(path);
        for (const auto& source : opaque_sources) {
            CAPTURE(source);
            const auto request = "SELECT bioseq.write($query$" + source + "$query$::text,'" + path + "')";
            for (const auto& command : {"EXPLAIN (COSTS OFF) " + request, request}) {
                const auto error = it::sql_error(ctx, command);
                REQUIRE(error.sqlstate == "22023");
                REQUIRE(error.contains("export completion cannot cross"));
                REQUIRE(error.contains("missing CIGAR"));
                REQUIRE_FALSE(std::filesystem::exists(path));
            }
        }
        // Unused definitions and predicate/scalar-only consumers cannot own
        // exported rows and must not acquire the new native-field obligation.
        const auto kept = "SELECT 'kept'::text AS header,'ACGT'::bioseq.seq('dna4',64) AS seq";
        for (const auto& source : std::vector<std::string>{
                recursive_source + kept,
                recursive_source + kept + " WHERE EXISTS (SELECT 1 FROM matched)",
                recursive_source + "SELECT 'kept'::text AS header,'ACGT'::bioseq.seq('dna4',64) AS seq,"
                    "(SELECT count(*) FROM matched) AS hit_count"}) {
            const auto request = "SELECT bioseq.write($query$" + source + "$query$::text,'" + path + "')";
            const auto plan = it::explain_text<false>(ctx, request);
            REQUIRE(it::plan_contains(plan, "Alignment Owner: false"));
            REQUIRE_FALSE(it::plan_contains(plan, "Explicit Alignment Fields Required"));
            REQUIRE(it::query_scalar_int(ctx, request) == 1);
            REQUIRE(inspect_exported_native(path, format).references.empty());
            std::filesystem::remove(path);
        }
        // Constant and row-dependent unmapped intent need no CIGAR/POS/RNAME.
        // A nullable alignment composite remains an explicitly supplied record.
        const std::vector<std::pair<std::string, std::string>> intentional_unmapped{
            {"4 AS flag", ""},
            {"CASE WHEN header LIKE '%a' THEN 4 ELSE 260 END AS flag", ""},
            {"true AS unmapped", "ROW('unmapped','unmapped')::bioseq.column_mapping"},
            {"NULL::bioseq.alignment AS alignment", "ROW('alignment','alignment')::bioseq.column_mapping"}};
        for (const auto& [projection, mapping] : intentional_unmapped) {
            CAPTURE(projection);
            const auto source = "SELECT header,seq,quality," + projection + " FROM " + repeated_reads
                + " ORDER BY header";
            const auto request = "SELECT bioseq.write($query$" + source + "$query$::text,'" + path + "'"
                + (mapping.empty() ? ")" : ",columns=>ARRAY[" + mapping + "])");
            REQUIRE(it::query_scalar_int(ctx, request) == 4);
            const auto native = inspect_exported_native(path, format);
            REQUIRE(native.names.size() == 4u);
            REQUIRE(native.references.empty());
            if (format == "sam") {
                for (const auto& record : exported_sam_records(read_export_fixture(path))) {
                    REQUIRE((std::stoi(record[1]) & 4) != 0);
                    REQUIRE(record[2] == "*");
                    REQUIRE(record[3] == "0");
                    REQUIRE(record[5] == "*");
                }
            }
            reset_barrier_destination();
            REQUIRE(it::query_scalar_int(ctx,
                "SELECT bioseq.load('write_set_barrier_loaded'::regclass,'" + path + "')") == 4);
            REQUIRE(it::query_scalar_int(ctx,
                "SELECT count(*) FROM write_set_barrier_loaded WHERE (alignment).cigar IS NULL "
                "AND (alignment).target_start IS NULL AND (alignment).reference_name IS NULL") == 4);
            std::filesystem::remove(path);
        }
        // Mapped0, false-unmapped and NULL do not authorize fallback. A later
        // failing row must also leave an existing destination byte-for-byte intact.
        for (const auto& [projection, mapping] : std::vector<std::pair<std::string, std::string>>{
                {"0 AS flag", "ROW('flag','flags')::bioseq.column_mapping"},
                {"CASE WHEN header LIKE '%a' THEN 4 ELSE 0 END AS flag", "ROW('flag','flags')::bioseq.column_mapping"},
                {"CASE WHEN header LIKE '%a' THEN 4 ELSE NULL::integer END AS flag", "ROW('flag','flags')::bioseq.column_mapping"},
                {"false AS unmapped", "ROW('unmapped','unmapped')::bioseq.column_mapping"},
                {"NULL::boolean AS unmapped", "ROW('unmapped','unmapped')::bioseq.column_mapping"}}) {
            CAPTURE(projection);
            const std::string prior = "existing complete destination\n";
            { std::ofstream output(path, std::ios::binary | std::ios::trunc);
                output << prior; REQUIRE(output.good()); }
            const auto source = "SELECT header,seq,quality," + projection + " FROM " + repeated_reads
                + " ORDER BY header";
            const auto request = "SELECT bioseq.write($query$" + source + "$query$::text,'" + path
                + "',columns=>ARRAY[" + mapping + "],"
                    "options=>'{\"overwrite\":true}'::jsonb)";
            const auto plan = it::explain_text<false>(ctx, request);
            REQUIRE(it::plan_contains(plan, "Explicit Alignment Fields Required: true"));
            REQUIRE(read_export_fixture(path) == prior);
            const auto error = it::sql_error(ctx, request);
            REQUIRE(error.sqlstate == "22023");
            REQUIRE(error.contains("export completion cannot cross"));
            REQUIRE(error.contains("missing CIGAR"));
            REQUIRE(read_export_fixture(path) == prior);
            std::filesystem::remove(path);
        }
        for (const auto& [projection, missing] : std::vector<std::pair<std::string, std::string>>{
                {"header,seq,quality,reference_sequence,reference_name,NULL::bigint AS target_start,recipe,0 AS flag",
                    "target_start"},
                {"header,seq,quality,NULL::bioseq.seq('dna4',64) AS reference_sequence,"
                    "NULL::text AS reference_name,locus,recipe,0 AS flag", "reference_sequence or reference_name"},
                {"header,seq,quality,reference_sequence,reference_name,locus,recipe,"
                    "NULL::integer AS flag,NULL::boolean AS reversed", "FLAG or reversed"}}) {
            CAPTURE(projection);
            const auto source = "SELECT " + projection + " FROM (" + set_query + ") explicit_rows";
            const auto request = "SELECT bioseq.write($query$" + source + "$query$::text,'" + path
                + "',columns=>" + mappings + ")";
            REQUIRE(it::plan_contains(it::explain_text<false>(ctx, request),
                "Explicit Alignment Fields Required: true"));
            const auto error = it::sql_error(ctx, request);
            REQUIRE(error.sqlstate == "22023");
            REQUIRE(error.contains("export completion cannot cross"));
            REQUIRE(error.contains("missing " + missing));
            REQUIRE_FALSE(std::filesystem::exists(path));
        }
        // Explicit mapped rows may still pass through actual recursion. This
        // uses every original field and no hidden companions/fixed-point inference.
        const std::string fields = "header,seq,quality,reference_sequence,reference_name,"
            "size_of_reference,locus,locus_end,read_size,clipped_left,clipped_right,recipe,backward,flag,mapq";
        const auto seed = branch(1);
        const auto explicit_recursive = "WITH RECURSIVE matched AS (SELECT 0 AS depth," + seed.substr(7)
            + " UNION ALL SELECT depth+1," + fields + " FROM matched WHERE depth<1) SELECT "
            + fields + " FROM matched ORDER BY header,depth";
        const std::string explicit_mapping = "ARRAY["
            "ROW('header','header')::bioseq.column_mapping,ROW('seq','sequence')::bioseq.column_mapping,"
            "ROW('quality','quality')::bioseq.column_mapping,"
            "ROW('reference_sequence','reference_sequence')::bioseq.column_mapping,"
            "ROW('reference_name','reference_name')::bioseq.column_mapping,"
            "ROW('size_of_reference','reference_length')::bioseq.column_mapping,"
            "ROW('locus','target_start')::bioseq.column_mapping,ROW('locus_end','target_end')::bioseq.column_mapping,"
            "ROW('read_size','query_length')::bioseq.column_mapping,ROW('clipped_left','query_start')::bioseq.column_mapping,"
            "ROW('clipped_right','query_end')::bioseq.column_mapping,ROW('recipe','cigar')::bioseq.column_mapping,"
            "ROW('backward','reversed')::bioseq.column_mapping,ROW('flag','flags')::bioseq.column_mapping,"
            "ROW('mapq','mapping_quality')::bioseq.column_mapping]";
        const auto request = "SELECT bioseq.write($query$" + explicit_recursive + "$query$::text,'" + path
            + "',columns=>" + explicit_mapping + ")";
        REQUIRE(it::plan_contains(it::explain_text<false>(ctx, request), "Alignment Owner: false"));
        REQUIRE(it::query_scalar_int(ctx, request) == 4);
        const auto native = inspect_exported_native(path, format);
        REQUIRE(native.names.size() == 4u);
        REQUIRE((native.references == std::vector<std::pair<std::string, std::uint32_t>>{{"reference_1",640u}}));
        reset_barrier_destination();
        REQUIRE(it::query_scalar_int(ctx,
            "SELECT bioseq.load('write_set_barrier_loaded'::regclass,'" + path + "')") == 4);
        REQUIRE(it::query_scalar_int(ctx,
            "SELECT count(*) FROM write_set_barrier_loaded WHERE (alignment).flag=0 "
            "AND (alignment).reference_name='reference_1' AND (alignment).reference_length=640 "
            "AND (alignment).target_start=47 AND (alignment).target_end=559 "
            "AND (alignment).query_length=512 AND (alignment).query_start=0 AND (alignment).query_end=512 "
            "AND (alignment).cigar::text='512M'") == 4);
        std::filesystem::remove(path);
    }
}

TEST_CASE("bioseq.load: adjacent external CIGAR runs normalize without changing geometry",
          "[component:bioseq][layer:integration][feature:io][resource:postgres][resource:filesystem]") {
    auto ctx = it::fresh_context({"write_adjacent_source", "write_adjacent_loaded"});
    it::guc_scope maintenance(ctx, "bioseq.index.insert_maintenance.enabled", "off");
    const auto backend = it::query_scalar(ctx, "SELECT pg_backend_pid()");
    const std::string input_path = "/tmp/bioseq_adjacent_input_" + backend + ".sam";
    {
        std::ofstream output(input_path, std::ios::binary | std::ios::trunc);
        output << "@HD\tVN:1.6\n@SQ\tSN:chr\tLN:100\n"
            "matches\t0\tchr\t1\t37\t1M1M\t*\t0\t0\tAC\tII\n"
            "soft\t0\tchr\t1\t37\t1S1S2M1S1S\t*\t0\t0\tAACGTT\tIIIIII\n"
            "hard\t0\tchr\t1\t37\t1H1H2M1H1H\t*\t0\t0\tAC\tII\n";
        REQUIRE(output.good());
    }
    it::exec_sql(ctx, "CREATE TABLE write_adjacent_source(header text,seq bioseq.seq('dna15'),"
        "quality bioseq.seq('phred94')) USING bioseq");
    REQUIRE(it::query_scalar_int(ctx,
        "SELECT bioseq.load('write_adjacent_source'::regclass,'" + input_path + "')") == 3);
    const std::string projection = "SELECT header,seq::text,quality::text,(alignment).cigar::text,"
        "(alignment).query_length,(alignment).query_start,(alignment).query_end,"
        "(alignment).target_start,(alignment).target_end FROM ";
    const auto expected = it::query_rows(ctx, projection + "write_adjacent_source ORDER BY header");
    REQUIRE(expected.size() == 3u);
    REQUIRE((expected[0] == it::row_t{"hard", "AC", "II", "2H2M2H", "6", "2", "4", "0", "2"}));
    REQUIRE((expected[1] == it::row_t{"matches", "AC", "II", "2M", "2", "0", "2", "0", "2"}));
    REQUIRE((expected[2] == it::row_t{"soft", "AACGTT", "IIIIII", "2S2M2S", "6", "2", "4", "0", "2"}));
    for (const std::string_view format : {"sam", "bam"}) {
        CAPTURE(format);
        const std::string path = "/tmp/bioseq_adjacent_output_" + backend + "." + std::string{format};
        std::filesystem::remove(path);
        REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.write($q$SELECT * FROM write_adjacent_source ORDER BY header$q$::text,'"
            + path + "')") == 3);
        if (format == "sam") {
            const auto native = exported_sam_records(read_export_fixture(path));
            REQUIRE(native.size() == 3u);
            REQUIRE(native[0][5] == "2H2M2H");
            REQUIRE(native[1][5] == "2M");
            REQUIRE(native[2][5] == "2S2M2S");
        }
        it::exec_sql(ctx, "DROP TABLE IF EXISTS write_adjacent_loaded; "
            "CREATE TABLE write_adjacent_loaded(LIKE write_adjacent_source) USING bioseq");
        REQUIRE(it::query_scalar_int(ctx,
            "SELECT bioseq.load('write_adjacent_loaded'::regclass,'" + path + "')") == 3);
        REQUIRE(it::query_rows(ctx, projection + "write_adjacent_loaded ORDER BY header") == expected);
        std::filesystem::remove(path);
    }
    std::filesystem::remove(input_path);
}

TEST_CASE("bioseq.write: explicit alignment interpretation reaches native bytes and preserved originals",
          "[component:bioseq][layer:integration][feature:io][resource:postgres][resource:filesystem]") {
    auto ctx = it::fresh_context({"write_interpretation_source", "write_interpretation_loaded", "write_interpretation_again"});
    it::guc_scope maintenance(ctx, "bioseq.index.insert_maintenance.enabled", "off");
    const auto backend = it::query_scalar(ctx, "SELECT pg_backend_pid()");
    struct interpretation_case {
        std::string_view label, sequence, native_sequence, native_cigar;
        int flags;
        std::size_t query_length, query_start, query_end;
        std::string_view options, mismatch, orientation, form;
    };
    constexpr auto cases = std::to_array<interpretation_case>({
        {"forward", "ACGT", "ACGT", "1S2M1S", 0, 4, 1, 3,
            R"({"cigar_form":"alignment_core"})", R"({"cigar_form":"sam_complete"})", "original_query", "alignment_core"},
        {"reverse", "ACGTA", "TACGT", "2S2M1S", 16, 5, 1, 3,
            R"({"cigar_form":"alignment_core"})", R"({"cigar_form":"sam_complete"})", "original_query", "alignment_core"},
        {"reference_forward", "AC", "AC", "2M", 16, 2, 0, 2,
            R"({"sequence_orientation":"reference_forward"})", R"({"sequence_orientation":"original_query"})",
            "reference_forward", "sam_complete"},
    });
    for (const auto& input : cases) {
        CAPTURE(input.label);
        it::exec_sql(ctx, "DROP TABLE IF EXISTS write_interpretation_source,write_interpretation_loaded,write_interpretation_again; "
            "CREATE TABLE write_interpretation_source(header text,seq bioseq.seq('dna4'),cigar bioseq.seq('cigar'),"
            "flag integer,reference_name text,reference_length bigint,target_start bigint,target_end bigint,"
            "query_length bigint,query_start bigint,query_end bigint) USING bioseq");
        it::exec_sql(ctx, fmt::format("INSERT INTO write_interpretation_source VALUES ('{}','{}','2M',{},'ref',100,10,12,{},{},{})",
            input.label, input.sequence, input.flags, input.query_length, input.query_start, input.query_end));
        const auto expected = it::query_rows(ctx,
            "SELECT header,seq::text,cigar::text,query_length,query_start,query_end FROM write_interpretation_source");
        const std::string projection = "SELECT header,seq::text,(alignment).cigar::text,(alignment).query_length,"
            "(alignment).query_start,(alignment).query_end FROM ";
        for (const std::string_view format : {"sam", "bam"}) {
            CAPTURE(format);
            const std::string path = "/tmp/bioseq_interpretation_" + backend + "." + std::string{format};
            const std::string again = "/tmp/bioseq_interpretation_again_" + backend + "." + std::string{format};
            std::filesystem::remove(path);
            std::filesystem::remove(again);
            REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.write('write_interpretation_source'::regclass,'" + path
                + "',options=>'" + std::string{input.options} + "'::jsonb)") == 1);
            if (format == "sam") {
                const auto native = exported_sam_records(read_export_fixture(path));
                REQUIRE(native.size() == 1u);
                REQUIRE(native[0][5] == input.native_cigar);
                REQUIRE(native[0][9] == input.native_sequence);
            }
            it::exec_sql(ctx, "DROP TABLE IF EXISTS write_interpretation_loaded; "
                "CREATE TABLE write_interpretation_loaded(header text,seq bioseq.seq('dna4')) USING bioseq");
            REQUIRE(it::query_scalar_int(ctx,
                "SELECT bioseq.load('write_interpretation_loaded'::regclass,'" + path + "')") == 1);
            REQUIRE(it::query_rows(ctx, projection + "write_interpretation_loaded") == expected);
            REQUIRE(it::query_scalar(ctx, "SELECT (alignment).source_orientation FROM write_interpretation_loaded") == input.orientation);
            REQUIRE(it::query_scalar(ctx, "SELECT (alignment).cigar_form FROM write_interpretation_loaded") == input.form);
            for (const auto supplied : {std::string_view{"{}"}, input.options}) {
                std::filesystem::remove(again);
                REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.write('write_interpretation_loaded'::regclass,'" + again
                    + "',options=>'" + std::string{supplied} + "'::jsonb)") == 1);
                it::exec_sql(ctx, "DROP TABLE IF EXISTS write_interpretation_again; "
                    "CREATE TABLE write_interpretation_again(header text,seq bioseq.seq('dna4')) USING bioseq");
                REQUIRE(it::query_scalar_int(ctx,
                    "SELECT bioseq.load('write_interpretation_again'::regclass,'" + again + "')") == 1);
                REQUIRE(it::query_rows(ctx, projection + "write_interpretation_again") == expected);
            }
            std::filesystem::remove(again);
            const auto rejected = it::sql_error(ctx, "SELECT bioseq.write('write_interpretation_loaded'::regclass,'" + again
                + "',options=>'" + std::string{input.mismatch} + "'::jsonb)");
            REQUIRE(rejected.sqlstate == "22000");
            REQUIRE(rejected.message.find("disagrees with the source alignment") != std::string::npos);
            REQUIRE_FALSE(std::filesystem::exists(again));
            std::filesystem::remove(path);
        }
    }
}

TEST_CASE("bioseq.write: native SAM import retains high quality and malformed load rolls back",
          "[component:bioseq][layer:integration][feature:io][feature:quality][resource:postgres][resource:filesystem]") {
    auto ctx = it::fresh_context({"write_native_source", "write_native_loaded", "write_native_invalid",
        "write_native_set_a", "write_native_set_b", "write_native_set_loaded",
        "write_native_inherit", "write_native_inherit_child", "write_native_contextless"});
    it::guc_scope maintenance(ctx, "bioseq.index.insert_maintenance.enabled", "off");
    const auto backend = it::query_scalar(ctx, "SELECT pg_backend_pid()");
    const std::string path = "/tmp/bioseq_write_native_" + backend + ".sam";
    const std::string bam_path = "/tmp/bioseq_write_native_" + backend + ".bam";
    const std::string bad_path = "/tmp/bioseq_write_native_bad_" + backend + ".sam";
    const std::string header = "@HD\tVN:1.6\tSO:unknown\n@SQ\tSN:chr\tLN:100\n";
    const std::string record = "external\t16\tchr\t8\t37\t2M\t*\t0\t0\tKT\t~!\tNM:i:1\tXX:Z:retained\n";
    { std::ofstream output(path, std::ios::binary); output << header << record; }
    { std::ofstream output(bad_path, std::ios::binary);
      output << header << record << "invalid\t0\tchr\t8\t37\t3M\t*\t0\t0\tAC\t!!\n"; }
    std::filesystem::remove(bam_path);
    it::exec_sql(ctx,
        "CREATE TABLE write_native_source(header text,seq bioseq.seq('dna15'),quality bioseq.seq('phred94')) USING bioseq; "
        "CREATE TABLE write_native_loaded(LIKE write_native_source) USING bioseq; "
        "CREATE TABLE write_native_invalid(LIKE write_native_source) USING bioseq");
    REQUIRE(it::query_scalar_int(ctx,
        "SELECT bioseq.load('write_native_source'::regclass,'" + path + "')") == 1);
    const std::string projection =
        "SELECT header,seq::text,quality::text,(alignment).flag,(alignment).mapq,"
        "(alignment).reference_name,(alignment).reference_length,(alignment).target_start,"
        "(alignment).target_end,(alignment).cigar::text,(alignment).source_orientation,(alignment).aux::text FROM ";
    const auto expected = it::query_rows(ctx, projection + "write_native_source");
    REQUIRE(expected.size() == 1);
    REQUIRE(expected[0][0] == "external");
    REQUIRE(expected[0][1] == "KT");
    REQUIRE(expected[0][2] == "~!");
    REQUIRE(expected[0][3] == "16");
    REQUIRE(expected[0][7] == "7");
    REQUIRE(expected[0][8] == "9");
    REQUIRE(expected[0][9] == "2M");
    REQUIRE(expected[0][10] == "reference_forward");
    REQUIRE(it::query_scalar_int(ctx,
        "SELECT bioseq.write('write_native_source'::regclass,'" + bam_path + "')") == 1);
    REQUIRE(it::query_scalar_int(ctx,
        "SELECT bioseq.load('write_native_loaded'::regclass,'" + bam_path + "')") == 1);
    REQUIRE(it::query_rows(ctx, projection + "write_native_loaded") == expected);
    const auto invalid = it::sql_error(ctx,
        "SELECT bioseq.load('write_native_invalid'::regclass,'" + bad_path + "')");
    REQUIRE_FALSE(invalid.message.empty());
    REQUIRE(it::query_scalar_int(ctx, "SELECT count(*) FROM write_native_invalid") == 0);
    REQUIRE(it::query_scalar_int(ctx,
        "SELECT count(*) FROM pg_attribute WHERE attrelid='write_native_invalid'::regclass "
        "AND attname='alignment' AND NOT attisdropped") == 0);
    // Parser target Vars for set operations point at the leftmost arm. Header
    // contexts and local import IDs must nevertheless describe every actual row.
    const std::string set_prefix = "/tmp/bioseq_write_set_provenance_" + backend;
    const std::string left_path = set_prefix + "_left.sam";
    const std::string right_path = set_prefix + "_right.sam";
    const std::string shared_header = "@HD\tVN:1.6\n@SQ\tSN:chr\tLN:100\n@RG\tID:shared\tSM:";
    { std::ofstream output(left_path, std::ios::binary);
      output << shared_header << "left\nleft\t256\tchr\t8\t37\t2M\t*\t0\t0\tAC\tII\tRG:Z:shared\n"; }
    { std::ofstream output(right_path, std::ios::binary);
      output << shared_header << "right\nright\t256\tchr\t8\t37\t2M\t*\t0\t0\tGT\tJJ\tRG:Z:shared\n"; }
    it::exec_sql(ctx,
        "CREATE TABLE write_native_set_a(header text,seq bioseq.seq('dna15'),quality bioseq.seq('phred94')) USING bioseq; "
        "CREATE TABLE write_native_set_b(LIKE write_native_set_a) USING bioseq; "
        "CREATE TABLE write_native_set_loaded(LIKE write_native_set_a,reference_length bigint DEFAULT -1) USING bioseq");
    REQUIRE(it::query_scalar_int(ctx,
        "SELECT bioseq.load('write_native_set_a'::regclass,'" + left_path + "')") == 1);
    REQUIRE(it::query_scalar_int(ctx,
        "SELECT bioseq.load('write_native_set_b'::regclass,'" + right_path + "')") == 1);
    const std::string from_left = "SELECT header,seq,quality,alignment FROM write_native_set_a";
    const std::string from_right = "SELECT header,seq,quality,alignment FROM write_native_set_b";
    // Secondary records can be repeated by UNION ALL without violating the
    // independent one-primary-per-imported-segment validation policy.
    std::string shared_ctes = "WITH repeated_0 AS MATERIALIZED (" + from_left + ")";
    for (unsigned level = 1; level <= 18; ++level) {
        const auto previous = "repeated_" + std::to_string(level - 1);
        shared_ctes += ",repeated_" + std::to_string(level) + " AS MATERIALIZED (SELECT * FROM "
            + previous + " UNION ALL SELECT * FROM " + previous + ")";
    }
    shared_ctes += " SELECT * FROM repeated_18 LIMIT 1";
    const std::array same_origin{
        std::pair{from_left + " UNION ALL " + from_left, 2},
        std::pair{"(" + from_left + " UNION " + from_left + ") INTERSECT " + from_left, 1},
        std::pair{"WITH merged AS MATERIALIZED ((" + from_left + " UNION ALL " + from_left
            + ") EXCEPT ALL " + from_left + ") SELECT * FROM (SELECT * FROM merged) projected", 1},
        std::pair{shared_ctes, 1}
    };
    const std::string explicit_header = R"sql('{"sam_header":["@HD\tVN:1.6","@SQ\tSN:chr\tLN:100","@RG\tID:shared\tSM:chosen"]}'::jsonb)sql";
    const auto left_expected = it::query_rows(ctx, projection + "write_native_set_a");
    for (const std::string_view format : {"sam", "bam"}) {
        CAPTURE(format);
        const std::string output_path = set_prefix + "." + std::string{format};
        std::filesystem::remove(output_path);
        for (const auto& [query, rows] : same_origin) {
            CAPTURE(query);
            REQUIRE(it::query_scalar_int(ctx,
                "SELECT bioseq.write($query$" + query + "$query$::text,'" + output_path + "')") == rows);
            if (format == "sam") {
                const auto contents = read_export_fixture(output_path);
                REQUIRE(contents.find("@RG\tID:shared\tSM:left") != std::string::npos);
                REQUIRE(contents.find("SM:right") == std::string::npos);
            }
            it::exec_sql(ctx, "TRUNCATE write_native_set_loaded");
            REQUIRE(it::query_scalar_int(ctx,
                "SELECT bioseq.load('write_native_set_loaded'::regclass,'" + output_path + "')") == rows);
            const auto loaded = it::query_rows(ctx, projection + "write_native_set_loaded");
            REQUIRE(loaded.size() == static_cast<std::size_t>(rows));
            for (const auto& row : loaded) REQUIRE(row == left_expected.front());
            std::filesystem::remove(output_path);
        }
        for (const std::string& query : {
                from_left + " UNION ALL " + from_right,
                "WITH merged AS MATERIALIZED (" + from_left + " UNION ALL " + from_right
                    + ") SELECT * FROM (SELECT * FROM merged) projected"}) {
            const auto mismatch = it::sql_error(ctx,
                "SELECT bioseq.write($query$" + query + "$query$::text,'" + output_path + "')");
            REQUIRE(mismatch.sqlstate == "22023");
            REQUIRE(mismatch.contains("alignment IO provenance does not match"));
            REQUIRE_FALSE(std::filesystem::exists(output_path));
        }
        // The existing explicit-header escape remains available after CTAS or
        // deliberately mixed contexts. An empty result requires no row context.
        REQUIRE(it::query_scalar_int(ctx,
            "SELECT bioseq.write($query$" + from_left + " UNION ALL " + from_right
            + "$query$::text,'" + output_path + "',options=>" + explicit_header + ")") == 2);
        if (format == "sam")
            REQUIRE(read_export_fixture(output_path).find("@RG\tID:shared\tSM:chosen") != std::string::npos);
        auto native_names = inspect_exported_native(output_path, format).names;
        std::ranges::sort(native_names);
        REQUIRE((native_names == std::vector<std::string>{"left", "right"}));
        std::filesystem::remove(output_path);
        REQUIRE(it::query_scalar_int(ctx,
            "SELECT bioseq.write($query$" + from_left + " WHERE false UNION ALL " + from_right
            + " WHERE false$query$::text,'" + output_path + "')") == 0);
        std::filesystem::remove(output_path);

    }
    // Inheritance introduces a second physical namespace without a set operation.
    // Keep a generic export plan across attach/detach to exercise normal relation
    // invalidation as well as the final-plan provenance guard.
    it::exec_sql(ctx,
        "CREATE TABLE write_native_inherit(LIKE write_native_set_a) USING bioseq; "
        "CREATE TABLE write_native_inherit_child(LIKE write_native_set_a) USING bioseq");
    REQUIRE(it::query_scalar_int(ctx,
        "SELECT bioseq.load('write_native_inherit'::regclass,'" + left_path + "')") == 1);
    REQUIRE(it::query_scalar_int(ctx,
        "SELECT bioseq.load('write_native_inherit_child'::regclass,'" + right_path + "')") == 1);
    const auto inherited_expected = it::query_rows(ctx,
        projection + "write_native_set_a UNION ALL " + projection + "write_native_set_b ORDER BY header");
    it::guc_scope generic_inheritance(ctx, "plan_cache_mode", "force_generic_plan");
    for (const std::string_view format : {"sam", "bam"}) {
        CAPTURE(format);
        const std::string output_path = set_prefix + "_inherited." + std::string{format};
        std::filesystem::remove(output_path);
        it::exec_sql(ctx, "PREPARE write_inherited_export AS SELECT bioseq.write("
            "'write_native_inherit'::regclass,'" + output_path + "')");
        const std::string execute = "EXECUTE write_inherited_export";
        REQUIRE(it::plan_contains(it::explain_text<false>(ctx, execute), "BioseqExport"));
        REQUIRE(it::query_scalar_int(ctx, execute) == 1);
        std::filesystem::remove(output_path);
        it::exec_sql(ctx, "ALTER TABLE write_native_inherit_child INHERIT write_native_inherit");
        const std::string inherited_union =
            "SELECT * FROM write_native_inherit UNION ALL SELECT * FROM write_native_inherit";
        // Permission costing must support the same inherited UNION in ordinary
        // SQL before the writer can apply its separate provenance restriction.
        REQUIRE(it::query_scalar_int(ctx,
            "SELECT count(*) FROM (" + inherited_union + ") combined") == 4);
        const auto inherited = it::sql_error(ctx, execute);
        REQUIRE(inherited.sqlstate == "22023");
        REQUIRE(inherited.contains("alignment IO provenance does not match"));
        REQUIRE_FALSE(std::filesystem::exists(output_path));
        for (const std::string& query : {
                std::string{"SELECT * FROM write_native_inherit"},
                std::string{"WITH inherited AS MATERIALIZED (SELECT * FROM write_native_inherit) "
                    "SELECT * FROM (SELECT * FROM inherited) projected"},
                inherited_union}) {
            CAPTURE(query);
            const auto mismatch = it::sql_error(ctx, "SELECT bioseq.write($query$" + query
                + "$query$::text,'" + output_path + "')");
            REQUIRE(mismatch.sqlstate == "22023");
            REQUIRE(mismatch.contains("alignment IO provenance does not match"));
            REQUIRE_FALSE(std::filesystem::exists(output_path));
        }
        for (const auto& [table, sample] : {
                std::pair{"write_native_inherit", "left"},
                std::pair{"write_native_inherit_child", "right"}}) {
            REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.write($query$SELECT * FROM ONLY "
                + std::string{table} + "$query$::text,'" + output_path + "')") == 1);
            if (format == "sam")
                REQUIRE(read_export_fixture(output_path).find("@RG\tID:shared\tSM:" + std::string{sample})
                    != std::string::npos);
            std::filesystem::remove(output_path);
        }
        REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.write('write_native_inherit'::regclass,'"
            + output_path + "',options=>" + explicit_header + ")") == 2);
        if (format == "sam")
            REQUIRE(read_export_fixture(output_path).find("@RG\tID:shared\tSM:chosen") != std::string::npos);
        auto native_names = inspect_exported_native(output_path, format).names;
        std::ranges::sort(native_names);
        REQUIRE((native_names == std::vector<std::string>{"left", "right"}));
        it::exec_sql(ctx, "TRUNCATE write_native_set_loaded");
        REQUIRE(it::query_scalar_int(ctx,
            "SELECT bioseq.load('write_native_set_loaded'::regclass,'" + output_path + "')") == 2);
        REQUIRE(it::query_rows(ctx, projection + "write_native_set_loaded ORDER BY header") == inherited_expected);
        std::filesystem::remove(output_path);
        it::exec_sql(ctx, "ALTER TABLE write_native_inherit_child NO INHERIT write_native_inherit");
        REQUIRE(it::query_scalar_int(ctx, execute) == 1);
        std::filesystem::remove(output_path);
        it::exec_sql(ctx, "DEALLOCATE write_inherited_export");
    }
    // INSERT SELECT retains local IDs but does not copy the table's IO namespace.
    // Distinct ordinary primary reads must remain distinct on native output.
    it::exec_sql(ctx, "CREATE TABLE write_native_contextless(LIKE write_native_set_a) USING bioseq");
    // Explicit header replacement can discard an unverified namespace, but
    // it must not turn an invalid negative numeric identity into an absent one.
    for (const std::string_view context : {"null", "9223372036854775807"}) {
        CAPTURE(context);
        const auto changes = std::string{"{\"reference_id\":-1,\"header_context_id\":"}
            + std::string{context} + "}";
        it::exec_sql(ctx, "TRUNCATE write_native_contextless; INSERT INTO write_native_contextless "
            "SELECT header,seq,quality,jsonb_populate_record(alignment,'" + changes
            + "'::jsonb) FROM write_native_set_a");
        for (const std::string_view format : {"sam", "bam"}) {
            CAPTURE(format);
            const auto output_path = set_prefix + "_negative_reference." + std::string{format};
            std::filesystem::remove(output_path);
            const auto rejected = it::sql_error(ctx,
                "SELECT bioseq.write('write_native_contextless'::regclass,'" + output_path
                + "',options=>" + explicit_header + ")");
            REQUIRE(rejected.sqlstate == "22023");
            REQUIRE(rejected.contains("negative imported reference identity"));
            REQUIRE_FALSE(std::filesystem::exists(output_path));
        }
    }
    for (const bool same_name : {false, true}) {
        CAPTURE(same_name);
        const std::string changes = same_name ? R"({"flag":0,"qname":"shared"})" : R"({"flag":0})";
        it::exec_sql(ctx, "TRUNCATE write_native_contextless; INSERT INTO write_native_contextless "
            "SELECT header,seq,quality,jsonb_populate_record(alignment,'" + changes
            + "'::jsonb) FROM write_native_set_a UNION ALL "
            "SELECT header,seq,quality,jsonb_populate_record(alignment,'" + changes
            + "'::jsonb) FROM write_native_set_b");
        for (const std::string_view format : {"sam", "bam"}) {
            const std::string output_path = set_prefix + "_anonymous." + std::string{format};
            const std::string replay_path = set_prefix + "_anonymous_again." + std::string{format};
            std::filesystem::remove(output_path);
            std::filesystem::remove(replay_path);
            REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.write($query$SELECT * FROM "
                "write_native_contextless ORDER BY header$query$::text,'" + output_path
                + "',options=>" + explicit_header + ")") == 2);
            const auto native_names = inspect_exported_native(output_path, format).names;
            REQUIRE(native_names.size() == 2);
            REQUIRE(native_names[0] != native_names[1]);
            if (same_name) REQUIRE(native_names[0] == "shared");
            else REQUIRE((native_names == std::vector<std::string>{"left", "right"}));
            it::exec_sql(ctx, "DROP TABLE write_native_set_loaded; "
                "CREATE TABLE write_native_set_loaded(LIKE write_native_set_a) USING bioseq");
            REQUIRE(it::query_scalar_int(ctx,
                "SELECT bioseq.load('write_native_set_loaded'::regclass,'" + output_path + "')") == 2);
            REQUIRE(it::query_scalar(ctx, "SELECT bool_and((alignment).template_id IS NULL "
                "AND (alignment).segment_id IS NULL AND (alignment).flag=0) FROM write_native_set_loaded") == "t");
            REQUIRE(it::query_scalar(ctx, "SELECT string_agg((alignment).qname,',' ORDER BY header) "
                "FROM write_native_set_loaded") == (same_name ? "shared,shared" : "left,right"));
            REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.write($query$SELECT header,seq,quality,alignment FROM "
                "write_native_set_loaded ORDER BY header$query$::text,'" + replay_path + "')") == 2);
            REQUIRE(inspect_exported_native(replay_path, format).names == native_names);
            if (format == "sam") {
                const auto replayed = read_export_fixture(replay_path);
                REQUIRE(replayed.find("@RG\tID:shared\tSM:chosen\n") != std::string::npos);
                REQUIRE(replayed.find("@CO\tbioseq.io:1:") != std::string::npos);
                REQUIRE(replayed.find("@CO\tbioseq.io:1:") == replayed.rfind("@CO\tbioseq.io:1:"));
            }
            std::filesystem::remove(output_path);
            std::filesystem::remove(replay_path);
        }
    }
    std::filesystem::remove(left_path);
    std::filesystem::remove(right_path);
    std::filesystem::remove(path);
    std::filesystem::remove(bam_path);
    std::filesystem::remove(bad_path);
}

TEST_CASE("bioseq.write: ambiguity publication and exactly once query execution",
          "[component:bioseq][layer:integration][feature:io][resource:postgres][resource:filesystem]") {
    auto ctx = it::fresh_context({"write_choices", "write_query_loaded"});
    it::guc_scope maintenance(ctx, "bioseq.index.insert_maintenance.enabled", "off");
    const auto backend = it::query_scalar(ctx, "SELECT pg_backend_pid()");
    const std::string path = "/tmp/bioseq_write_once_" + backend + ".fasta";
    const std::string fastq_path = "/tmp/bioseq_write_choices_" + backend + ".fastq";
    const std::string bad_path = "/tmp/bioseq_write_mismatch_" + backend + ".fastq";
    std::filesystem::remove(path);
    std::filesystem::remove(fastq_path);
    std::filesystem::remove(bad_path);
    it::exec_sql(ctx,
        "CREATE TEMP SEQUENCE write_query_calls; "
        "CREATE TABLE write_query_loaded(header text,seq bioseq.seq('dna4')) USING bioseq; "
        "CREATE TABLE write_choices(header text,seq bioseq.seq('dna4'),other_seq bioseq.seq('dna5'),"
        "quality bioseq.seq('phred42'),other_quality bioseq.seq('phred63')) USING bioseq; "
        "INSERT INTO write_choices VALUES ('selected','ACGT','NNNN','IIII','!!!!')");
    const std::string request = "SELECT bioseq.write($query$SELECT nextval('write_query_calls')::text AS header,"
        "'ACGT'::bioseq.seq('dna4') AS seq FROM generate_series(1,3)$query$::text,'" + path + "')";
    const auto planned = it::explain_text<false>(ctx, request);
    REQUIRE(std::ranges::any_of(planned, [](const std::string& line) { return line.contains("BioseqExport"); }));
    // PostgreSQL can also reduce VALUES and true quals to a singleton Result.
    // The rewrite must retain the caller's original source/qual/cardinality rules.
    for (const std::string_view suffix : {" FROM (VALUES (1)) AS input(value)",
            " FROM generate_series(1,2) AS input(value)", " WHERE true", " LIMIT 0"}) {
        CAPTURE(suffix);
        const auto ineligible = it::explain_text<false>(ctx, request + std::string{suffix});
        REQUIRE_FALSE(it::plan_contains(ineligible, "BioseqExport"));
    }
    REQUIRE(it::query_scalar(ctx, "SELECT is_called FROM write_query_calls") == "f");
    REQUIRE_FALSE(std::filesystem::exists(path));
    const auto executed = it::explain_text(ctx, request);
    REQUIRE(std::ranges::any_of(executed, [](const std::string& line) { return line.contains("BioseqExport"); }));
    REQUIRE(it::query_scalar_int(ctx, "SELECT last_value FROM write_query_calls") == 3);
    REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.load('write_query_loaded'::regclass,'" + path + "')") == 3);
    REQUIRE(it::query_scalar(ctx,
        "SELECT string_agg(header,',' ORDER BY header) FROM write_query_loaded") == "1,2,3");
    const auto original_file = read_export_fixture(path);
    const auto ambiguous = it::sql_error(ctx,
        "SELECT bioseq.write('write_choices'::regclass,'" + fastq_path + "')");
    REQUIRE(ambiguous.sqlstate == "22023");
    REQUIRE(ambiguous.contains("multiple"));
    REQUIRE_FALSE(std::filesystem::exists(fastq_path));
    const std::string sequence_mapping = "ARRAY[ROW('seq','sequence')::bioseq.column_mapping,"
        "ROW('other_seq','omit')::bioseq.column_mapping]";
    const auto quality_ambiguous = it::sql_error(ctx,
        "SELECT bioseq.write('write_choices'::regclass,'" + fastq_path + "',columns=>" + sequence_mapping + ")");
    REQUIRE(quality_ambiguous.sqlstate == "22023");
    REQUIRE(quality_ambiguous.contains("multiple"));
    REQUIRE_FALSE(std::filesystem::exists(fastq_path));
    const std::string mappings = "ARRAY[ROW('seq','sequence')::bioseq.column_mapping,"
        "ROW('other_seq','omit')::bioseq.column_mapping,ROW('quality','quality')::bioseq.column_mapping,"
        "ROW('other_quality','omit')::bioseq.column_mapping]";
    const auto no_overwrite = it::sql_error(ctx,
        "SELECT bioseq.write('write_choices'::regclass,'" + path + "',columns=>" + sequence_mapping + ")");
    REQUIRE(no_overwrite.sqlstate == "22000");
    REQUIRE(read_export_fixture(path) == original_file);
    REQUIRE(it::query_scalar_int(ctx,
        "SELECT bioseq.write('write_choices'::regclass,'" + fastq_path + "',columns=>" + mappings + ")") == 1);
    const auto mismatch = it::sql_error(ctx,
        "SELECT bioseq.write($query$SELECT 'mismatch' AS header,'ACGT'::bioseq.seq('dna4') AS seq,"
        "'!!!'::bioseq.seq('phred42') AS quality$query$::text,'" + bad_path + "')");
    REQUIRE(mismatch.contains("quality"));
    REQUIRE_FALSE(std::filesystem::exists(bad_path));
    REQUIRE(it::query_scalar_int(ctx,
        "SELECT bioseq.write('write_choices'::regclass,'" + path + "',columns=>" + sequence_mapping +
        ",options=>'{\"overwrite\":true}'::jsonb)") == 1);
    REQUIRE(read_export_fixture(path) != original_file);
    std::filesystem::remove(path);
    std::filesystem::remove(fastq_path);
    std::filesystem::remove(bad_path);
}


TEST_CASE("bioseq.write: prepared view replacement uses its projected computed header",
          "[component:bioseq][layer:integration][feature:io][resource:postgres][resource:filesystem]") {
    auto ctx = it::fresh_context({"write_view_source", "write_view_loaded"});
    it::guc_scope maintenance(ctx, "bioseq.index.insert_maintenance.enabled", "off");
    it::guc_scope generic(ctx, "plan_cache_mode", "force_generic_plan");
    const auto backend = it::query_scalar(ctx, "SELECT pg_backend_pid()");
    it::exec_sql(ctx,
        "CREATE TABLE write_view_source(header text,seq bioseq.seq('dna5',64),"
        "quality bioseq.seq('phred63'),note text); "
        "INSERT INTO write_view_source VALUES ('base_read','ACNT','IIII','base_note')");
    for (const bool parameterized : {false, true}) {
        CAPTURE(parameterized);
        const std::string path = "/tmp/bioseq_write_view_header_" + backend
            + (parameterized ? "_generic.fastq" : "_constant.fastq");
        std::filesystem::remove(path);
        it::exec_sql(ctx,
            "CREATE OR REPLACE TEMP VIEW write_view_visible AS "
            "SELECT header,seq,quality,note FROM write_view_source");
        const std::string query = "SELECT * FROM write_view_visible ORDER BY header";
        const std::string overwrite = ",options=>'{\"overwrite\":true}'::jsonb)";
        std::string request = "EXECUTE write_view_export";
        if (parameterized) {
            it::exec_sql(ctx, "PREPARE write_view_export(text,text) AS SELECT bioseq.write($1,$2" + overwrite);
            request += "('" + query + "','" + path + "')";
        } else {
            it::exec_sql(ctx, "PREPARE write_view_export AS SELECT bioseq.write($query$"
                + query + "$query$::text,'" + path + "'" + overwrite);
        }
        const auto plan = it::explain_text<false>(ctx, request);
        REQUIRE(it::plan_contains(plan, "BioseqExport") == !parameterized);
        REQUIRE_FALSE(std::filesystem::exists(path));
        REQUIRE(it::query_scalar_int(ctx, request) == 1);
        REQUIRE(read_export_fixture(path).starts_with("@base_read [bq=1]"));
        it::exec_sql(ctx,
            "CREATE OR REPLACE TEMP VIEW write_view_visible AS SELECT "
            "'view_read'::text AS header,seq,quality,note || '_changed' AS note FROM write_view_source");
        REQUIRE(it::query_scalar_int(ctx, request) == 1);
        REQUIRE(read_export_fixture(path).starts_with("@view_read [bq=1]"));
        it::exec_sql(ctx, "DEALLOCATE write_view_export; "
            "DROP TABLE IF EXISTS write_view_loaded; "
            "CREATE TABLE write_view_loaded(header text,seq bioseq.seq('dna5',64),"
            "quality bioseq.seq('phred63'),note text) USING bioseq");
        REQUIRE(it::query_scalar_int(ctx, "SELECT bioseq.load('write_view_loaded'::regclass,'" + path + "')") == 1);
        const auto row = it::query_one(ctx, "SELECT header,seq::text,quality::text,note FROM write_view_loaded");
        REQUIRE(row[0] == "view_read");
        REQUIRE(row[1] == "ACNT");
        REQUIRE(row[2] == "IIII");
        REQUIRE(row[3] == "base_note_changed");
        std::filesystem::remove(path);
    }
    it::exec_sql(ctx, "DROP VIEW write_view_visible");
}


TEST_CASE("bioseq.write: invoker views complete only their declared fields",
          "[component:bioseq][layer:integration][feature:io][resource:postgres][resource:filesystem]") {
    auto ctx = it::fresh_context({"write_boundary_source", "write_boundary_loaded"});
    it::guc_scope maintenance(ctx, "bioseq.index.insert_maintenance.enabled", "off");
    it::guc_scope generic(ctx, "plan_cache_mode", "force_generic_plan");
    const auto backend = it::query_scalar(ctx, "SELECT pg_backend_pid()");
    it::exec_sql(ctx,
        "CREATE TABLE write_boundary_source(header text,seq bioseq.seq('dna5',64),quality bioseq.seq('phred63')); "
        "INSERT INTO write_boundary_source VALUES('base_read','ACNT','IIII'); "
        "PREPARE write_boundary_export(text,text) AS SELECT bioseq.write($1,$2)");
    for (const bool barrier : {false, true}) {
        CAPTURE(barrier);
        const std::string options = barrier ? "security_invoker=true,security_barrier=true"
                                           : "security_invoker=true,security_barrier=false";
        it::exec_sql(ctx,
            "CREATE OR REPLACE TEMP VIEW write_boundary_full WITH(" + options
            + ") AS SELECT header,seq,quality FROM write_boundary_source; "
            "CREATE OR REPLACE TEMP VIEW write_boundary_hidden WITH(" + options
            + ") AS SELECT header,seq FROM write_boundary_source");
        for (const bool parameterized : {false, true}) {
            CAPTURE(parameterized);
            const std::string suffix = std::string{barrier ? "_barrier" : "_ordinary"}
                + (parameterized ? "_generic" : "_constant");
            const std::string path = "/tmp/bioseq_write_view_boundary_" + backend + suffix + ".fastq";
            const std::string denied_path = "/tmp/bioseq_write_view_denied_" + backend + suffix + ".fastq";
            std::filesystem::remove(path);
            std::filesystem::remove(denied_path);
            const std::string request = parameterized
                ? "EXECUTE write_boundary_export('SELECT seq FROM write_boundary_full','" + path + "')"
                : "SELECT bioseq.write('SELECT seq FROM write_boundary_full'::text,'" + path + "')";
            REQUIRE(it::query_scalar_int(ctx, request) == 1);
            REQUIRE(read_export_fixture(path).starts_with("@base_read [bq=1]"));
            const std::string denied = parameterized
                ? "EXECUTE write_boundary_export('SELECT * FROM write_boundary_hidden','" + denied_path + "')"
                : "SELECT bioseq.write('SELECT * FROM write_boundary_hidden'::text,'" + denied_path + "')";
            const auto error = it::sql_error(ctx, denied);
            REQUIRE(error.sqlstate == "22023");
            REQUIRE(error.contains("hidden"));
            REQUIRE_FALSE(std::filesystem::exists(denied_path));
            REQUIRE(it::query_scalar_int(ctx, "SELECT 1") == 1);
            it::exec_sql(ctx,
                "DROP TABLE IF EXISTS write_boundary_loaded; "
                "CREATE TABLE write_boundary_loaded(header text,seq bioseq.seq('dna5',64),"
                "quality bioseq.seq('phred63')) USING bioseq");
            REQUIRE(it::query_scalar_int(ctx,
                "SELECT bioseq.load('write_boundary_loaded'::regclass,'" + path + "')") == 1);
            const auto row = it::query_one(ctx, "SELECT header,seq::text,quality::text FROM write_boundary_loaded");
            REQUIRE(row[0] == "base_read");
            REQUIRE(row[1] == "ACNT");
            REQUIRE(row[2] == "IIII");
            std::filesystem::remove(path);
        }
    }
    it::exec_sql(ctx, "DEALLOCATE write_boundary_export; DROP VIEW write_boundary_full,write_boundary_hidden");
}


TEST_CASE("bioseq.write: one alignment owner preserves visible computed headers",
          "[component:bioseq][layer:integration][feature:io][resource:postgres][resource:filesystem]") {
    auto ctx = it::fresh_context({"write_header_refs", "write_header_reads", "write_header_loaded"});
    it::guc_scope maintenance(ctx, "bioseq.index.insert_maintenance.enabled", "off");
    it::guc_scope index_read(ctx, "bioseq.index.sequence_read.enabled", "off");
    it::guc_scope minimum_score(ctx, "bioseq.aligner.chaining.minimum_score", "30");
    it::guc_scope minimum_seeds(ctx, "bioseq.aligner.chaining.minimum_seed_count", "3");
    const auto backend = it::query_scalar(ctx, "SELECT pg_backend_pid()");
    const auto reference = gen_bases(640u, 8547u, false);
    const auto query_sequence = reference.substr(47u, 512u);
    it::exec_sql(ctx,
        "CREATE TABLE write_header_refs(id integer,header text,seq bioseq.seq('dna4',64)) USING bioseq; "
        "CREATE TABLE write_header_reads(id integer,header text PRIMARY KEY,seq bioseq.seq('dna4',64),"
        "quality bioseq.seq('phred63')) USING bioseq");
    it::exec_sqlf(ctx,
        "INSERT INTO write_header_refs VALUES(1,'reference_1','{}'); "
        "INSERT INTO write_header_reads VALUES(1,'read_1','{}',repeat('I',512)::bioseq.seq('phred63'));",
        reference, query_sequence);
    // Alignment identity is part of this view's declared interface; completion
    // must not reach through a view to invent otherwise hidden system columns.
    it::exec_sql(ctx, "CREATE OR REPLACE TEMP VIEW write_header_visible AS SELECT "
        "id,header || '_view' AS header,seq,quality,ctid AS read_tid,tableoid AS read_relation "
        "FROM write_header_reads");
    for (const bool through_view : {false, true}) {
        CAPTURE(through_view);
        const std::string source_name = through_view ? "write_header_visible" : "write_header_reads";
        const std::string header = through_view ? "q.header" : "q.header || '_raw' AS header";
        const std::string expected_header = through_view ? "read_1_view" : "read_1_raw";
        const std::string query = "SELECT " + header + ",q.seq FROM write_header_refs t JOIN "
            + source_name + " q ON t.id=q.id WHERE public.mmsearch(t.seq,q.seq)";
        for (const std::string_view format : {"sam", "bam"}) {
            CAPTURE(format);
            const std::string path = "/tmp/bioseq_write_match_header_" + backend
                + (through_view ? "_view." : "_raw.") + std::string{format};
            std::filesystem::remove(path);
            const std::string request = "SELECT bioseq.write($query$" + query + "$query$::text,'" + path + "')";
            const auto plan = it::explain_text<false>(ctx, request);
            REQUIRE(it::plan_contains(plan, "BioseqExport"));
            REQUIRE(it::plan_contains(plan, "BioseqBatchJoin"));
            REQUIRE_FALSE(std::filesystem::exists(path));
            REQUIRE(it::query_scalar_int(ctx, request) == 1);
            if (format == "sam") {
                const auto records = exported_sam_records(read_export_fixture(path));
                REQUIRE(records.size() == 1);
                REQUIRE(records[0][0] == expected_header);
                REQUIRE(records[0][2] == "reference_1");
                REQUIRE(records[0][3] == "48");
                REQUIRE(records[0][5] == "512M");
            }
            it::exec_sql(ctx, "DROP TABLE IF EXISTS write_header_loaded; "
                "CREATE TABLE write_header_loaded(header text,seq bioseq.seq('dna4',64),"
                "quality bioseq.seq('phred63')) USING bioseq");
            REQUIRE(it::query_scalar_int(ctx,
                "SELECT bioseq.load('write_header_loaded'::regclass,'" + path + "')") == 1);
            const auto row = it::query_one(ctx,
                "SELECT header,seq::text,quality::text,(alignment).reference_name,"
                "(alignment).target_start,(alignment).target_end,(alignment).query_start,"
                "(alignment).query_end,(alignment).cigar::text FROM write_header_loaded");
            REQUIRE(row[0] == expected_header);
            REQUIRE(row[1] == query_sequence);
            REQUIRE(row[2] == std::string(512u, 'I'));
            REQUIRE(row[3] == "reference_1");
            REQUIRE(row[4] == "47");
            REQUIRE(row[5] == "559");
            REQUIRE(row[6] == "0");
            REQUIRE(row[7] == "512");
            REQUIRE(row[8] == "512M");
            std::filesystem::remove(path);
        }
    }
    const std::string ambiguous_path = "/tmp/bioseq_write_match_header_" + backend + "_ambiguous.sam";
    std::filesystem::remove(ambiguous_path);
    const auto ambiguous = it::sql_error(ctx,
        "SELECT bioseq.write($query$SELECT q.header || '_one' AS header,q.header || '_two' AS name,q.seq "
        "FROM write_header_refs t JOIN write_header_reads q ON t.id=q.id "
        "WHERE public.mmsearch(t.seq,q.seq)$query$::text,'" + ambiguous_path + "')");
    REQUIRE(ambiguous.sqlstate == "22023");
    REQUIRE(ambiguous.contains("multiple header candidates"));
    REQUIRE_FALSE(std::filesystem::exists(ambiguous_path));
    REQUIRE(it::query_scalar_int(ctx, "SELECT 1") == 1);
    it::exec_sql(ctx, "DROP VIEW write_header_visible");
}

TEST_CASE("bioseq.load: permitted writers keep caller identity around private storage SQL",
          "[component:bioseq][layer:integration][feature:io][resource:postgres][resource:filesystem]") {
    auto ctx = it::fresh_context({});
    const std::string backend = it::query_scalar(ctx, "SELECT pg_backend_pid()::text");
    const std::string schema = "load_internal_acl_" + backend;
    const std::string role = schema + "_actor";
    const std::string path = "/tmp/bioseq_" + schema + ".fastq";
    const std::string body = gen_bases(1024u, 0xac1u, false);
    const std::string quality(body.size(), 'I');
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "@allowed\n" << body << "\n+\n" << quality << '\n';
    }
    it::guc_scope maintenance(ctx, "bioseq.index.insert_maintenance.enabled", "off");
    it::guc_scope grouped(ctx, "bioseq.aligner.grouped_soa_generation.enabled", "on");
    it::guc_scope algorithm(ctx, "bioseq.aligner.minimizer.algorithm", "scalar");
    it::guc_scope async(ctx, "bioseq.index.async_persistence.enabled", "off");
    it::guc_scope recompute(ctx, "bioseq.index.recompute_async_persistence.enabled", "off");
    it::guc_scope storage(ctx, "default_table_access_method", "bioseq");
    it::exec_sqlf(ctx, "CREATE SCHEMA {}; CREATE ROLE {} NOLOGIN; "
        "GRANT USAGE ON SCHEMA {},bioseq TO {}; GRANT pg_read_server_files TO {}",
        schema, role, schema, role, role);
    const auto cleanup = [&ctx, &schema, &role, &path] {
        it::exec_sqlf(ctx, "ROLLBACK; RESET ROLE; DROP SCHEMA IF EXISTS {} CASCADE; "
            "DROP OWNED BY {}; DROP ROLE {}", schema, role, role);
        std::filesystem::remove(path);
    };
    try {
        const auto initial_search_path = it::query_scalar(ctx, "SHOW search_path");
        for (const bool maintain : {false, true}) {
            for (const bool insert : {false, true}) {
                CAPTURE(maintain, insert);
                const std::string table = schema + (insert ? ".insert_" : ".load_")
                    + (maintain ? "on" : "off");
                it::exec_sqlf(ctx, "CREATE TABLE {}(header text,seq bioseq.seq('dna4',64),"
                    "quality bioseq.seq('phred42',64),marker text NOT NULL DEFAULT CURRENT_USER,"
                    "CHECK(current_user='{}' AND marker='{}')) USING bioseq; "
                    "GRANT INSERT ON {} TO {}; "
                    "SELECT bioseq.set_index_policy('{}','minimizers_per_fragment','UPDATE_MANUALLY'); "
                    "SELECT bioseq.set_index_policy('{}','minimizers_per_sequence','UPDATE_ON_INSERT'); "
                    "SET bioseq.index.insert_maintenance.enabled={}; "
                    "SET bioseq.aligner.grouped_soa_generation.enabled={}; SET ROLE {}",
                    table, role, role, table, role, table, table,
                    maintain ? "on" : "off", insert ? "off" : "on", role);
                if (insert) {
                    // Grouped INSERT-time minimizer maintenance remains an
                    // explicitly unsupported route; cover its scalar route.
                    it::exec_sqlf(ctx, "INSERT INTO {}(header,seq,quality) VALUES('allowed','{}','{}')",
                        table, body, quality);
                } else {
                    REQUIRE(it::query_scalar_int(ctx,
                        fmt::format("SELECT bioseq.load('{}','{}')", table, path)) == 1);
                }
                REQUIRE(it::query_scalar(ctx, "SELECT current_user::text") == role);
                REQUIRE(it::query_scalar(ctx, "SHOW search_path") == initial_search_path);
                REQUIRE(it::query_scalar(ctx, "SHOW default_table_access_method") == "bioseq");
                REQUIRE(it::sql_error(ctx, "SELECT header FROM " + table).sqlstate == "42501");
                const auto rebuild = it::sql_error(ctx,
                    fmt::format("SELECT bioseq.update_index('{}','symbol_counts')", table));
                REQUIRE(rebuild.sqlstate == "42501");
                REQUIRE(rebuild.contains("owner"));
                it::exec_sql(ctx, "RESET ROLE");
                const auto row = it::query_one(ctx, "SELECT header,seq::text,quality::text,marker FROM " + table);
                REQUIRE(row[0] == "allowed");
                REQUIRE(row[1] == body);
                REQUIRE(row[2] == quality);
                REQUIRE(row[3] == role);
                REQUIRE(it::query_scalar_int(ctx, fmt::format(
                    "SELECT count(*) FROM bioseq_internal.metadata_age_log "
                    "WHERE user_relid='{}'::regclass AND index_name='minimizers_per_sequence'", table))
                    == (maintain ? 1 : 0));
                REQUIRE(it::query_scalar(ctx, fmt::format(
                    "SELECT has_schema_privilege('{}','bioseq_internal','USAGE')::text", role)) == "false");
                REQUIRE(it::query_scalar(ctx, fmt::format(
                    "SELECT bool_and(c.relowner=e.extowner)::text "
                    "FROM bioseq_internal.seq_sub_table s JOIN pg_class c "
                    "ON c.oid IN(s.sub_table_relid,s.sidecar_relid) CROSS JOIN pg_extension e "
                    "WHERE e.extname='bioseq' AND s.main_relfilenode=pg_relation_filenode('{}'::regclass)", table))
                    == "true");
            }
        }
        // A caller-owned parent can remove extension-owned private fragments.
        it::exec_sqlf(ctx, "GRANT CREATE ON SCHEMA {} TO {}; SET ROLE {}; "
            "SET bioseq.index.insert_maintenance.enabled=off; "
            "CREATE TABLE {}.owned(header text,seq bioseq.seq('dna4',64),"
            "quality bioseq.seq('phred42',64)) USING bioseq",
            schema, role, role, schema);
        REQUIRE(it::query_scalar_int(ctx,
            fmt::format("SELECT bioseq.load('{}.owned','{}')", schema, path)) == 1);
        it::exec_sqlf(ctx, "DROP TABLE {}.owned", schema);
        REQUIRE(it::query_scalar(ctx, "SELECT current_user::text") == role);
        REQUIRE(it::sql_error(ctx, "SELECT count(*) FROM bioseq_internal.index_policy").sqlstate == "42501");
        // ERROR inside row validation must leave the same role/settings and no rows.
        it::exec_sqlf(ctx, "CREATE TABLE {}.rejected(header text,seq bioseq.seq('dna4',64),"
            "quality bioseq.seq('phred42',64),CHECK(header<>'allowed')) USING bioseq", schema);
        REQUIRE(it::sql_error(ctx,
            fmt::format("SELECT bioseq.load('{}.rejected','{}')", schema, path)).sqlstate == "23514");
        REQUIRE(it::query_scalar(ctx, "SELECT current_user::text") == role);
        REQUIRE(it::query_scalar_int(ctx, "SELECT count(*) FROM " + schema + ".rejected") == 0);
        REQUIRE(it::query_scalar(ctx, "SHOW default_table_access_method") == "bioseq");
        it::exec_sql(ctx, "RESET ROLE");
        cleanup();
    } catch (...) {
        try { cleanup(); } catch (...) {}
        throw;
    }
}

TEST_CASE("bioseq.write: transformed and materialized companions keep their owning row",
          "[component:bioseq][layer:integration][feature:io][resource:postgres][resource:filesystem]") {
    auto ctx = it::fresh_context({"write_companion_refs", "write_companion_reads", "write_companion_loaded", "write_companion_singleton"});
    it::guc_scope maintenance(ctx, "bioseq.index.insert_maintenance.enabled", "off");
    it::guc_scope index_read(ctx, "bioseq.index.sequence_read.enabled", "off");
    it::guc_scope minimum_score(ctx, "bioseq.aligner.chaining.minimum_score", "30");
    it::guc_scope minimum_seeds(ctx, "bioseq.aligner.chaining.minimum_seed_count", "3");
    const auto backend = it::query_scalar(ctx, "SELECT pg_backend_pid()");
    const auto reference = gen_bases(640u, 18547u, false);
    const auto read = reference.substr(47u, 512u);
    it::exec_sql(ctx,
        "CREATE TABLE write_companion_refs(id integer,header text,seq bioseq.seq('dna4',64)) USING bioseq; "
        "CREATE TABLE write_companion_reads(id integer,header text PRIMARY KEY,seq bioseq.seq('dna4',64),quality bioseq.seq('phred63')) USING bioseq; "
        "CREATE TABLE write_companion_singleton(id integer); INSERT INTO write_companion_singleton VALUES(1); "
        "CREATE TEMP SEQUENCE write_companion_calls; "
        "CREATE FUNCTION pg_temp.write_counted_sequence(value bioseq.seq) RETURNS bioseq.seq LANGUAGE plpgsql VOLATILE "
        "AS $body$ BEGIN PERFORM nextval('pg_temp.write_companion_calls'::regclass); RETURN value; END $body$");
    // Build nonuniform quality in SQL: its valid alphabet includes quote bytes.
    it::exec_sqlf(ctx, "INSERT INTO write_companion_refs VALUES(1,'reference_1','{}'); "
        "INSERT INTO write_companion_reads SELECT 1,'read_1','{}',"
        "string_agg(chr(33 + letter_position % 63),'' ORDER BY letter_position)::bioseq.seq('phred63') "
        "FROM generate_series(0,511) letters(letter_position); "
        "INSERT INTO write_companion_reads VALUES(2,'read_2','{}',repeat('!',512)::bioseq.seq('phred63'));",
        reference, read, read);
    // Both reads align identically; only the ordinary join predicate selects
    // the permitted source row. Verify that filter independently of export.
    const std::array join_filters{
        std::pair<std::string_view, std::string_view>{"t.id=q.id", "read_1"},
        std::pair<std::string_view, std::string_view>{"t.id<q.id", "read_2"}
    };
    for (const auto& [predicate, expected_header] : join_filters) {
        for (const bool materialized : {false, true}) {
            CAPTURE(predicate, materialized);
            // Both stored query rows are evaluated once. The ordinary join
            // predicate must still select only its permitted aligned row.
            const std::string prefix = materialized
                ? "WITH q AS MATERIALIZED (SELECT id,header,"
                  "pg_temp.write_counted_sequence(seq)::bioseq.seq('dna4',64) AS seq "
                  "FROM write_companion_reads) "
                : "";
            const std::string query = prefix + "SELECT q.header,bioseq.cigar(t.seq)::text,"
                "bioseq.target_start(t.seq),bioseq.target_end(t.seq) FROM write_companion_refs t JOIN "
                + (materialized ? "q" : "write_companion_reads q") + " ON "
                + std::string{predicate} + " WHERE public.mmsearch(t.seq,q.seq)";
            if (materialized)
                it::exec_sql(ctx, "ALTER SEQUENCE pg_temp.write_companion_calls RESTART WITH 1");
            const auto plan = it::explain_text<false>(ctx, query);
            REQUIRE(it::plan_contains(plan, "BioseqBatchJoin"));
            if (materialized) {
                REQUIRE(it::plan_contains(plan, "CTE Scan"));
                REQUIRE(it::query_scalar(ctx, "SELECT is_called FROM pg_temp.write_companion_calls") == "f");
            }
            const auto rows = it::query_rows(ctx, query);
            REQUIRE(rows.size() == 1);
            REQUIRE(rows[0][0] == expected_header);
            REQUIRE(rows[0][1] == "512M");
            REQUIRE(rows[0][2] == "47");
            REQUIRE(rows[0][3] == "559");
            if (materialized)
                REQUIRE(it::query_scalar_int(ctx, "SELECT last_value FROM pg_temp.write_companion_calls") == 2);
        }
    }
    // A later CTE may inherit outer Params through an earlier definition.
    // Reuse one BatchJoin executor with two different slices and verify that
    // neither its cached query rows nor either child keeps the first Params.
    const std::string correlated =
        "SELECT request.id,hit.header,hit.seq,hit.cigar,hit.target_start,hit.target_end "
        "FROM (VALUES(1,0,256),(2,64,448)) request(id,lower_bound,upper_bound) CROSS JOIN LATERAL ("
        "WITH first_cte AS MATERIALIZED (SELECT q.id,q.header,"
        "pg_temp.write_counted_sequence(q.seq[request.lower_bound:request.upper_bound])::bioseq.seq('dna4',64) AS probe "
        "FROM write_companion_reads q WHERE q.id=request.id), "
        "second_cte AS MATERIALIZED (SELECT * FROM first_cte) "
        "SELECT p.header,bioseq_internal.alignment_query_sequence(t.seq)::text AS seq,"
        "bioseq.cigar(t.seq)::text AS cigar,bioseq.target_start(t.seq) AS target_start,"
        "bioseq.target_end(t.seq) AS target_end FROM write_companion_refs t CROSS JOIN second_cte p "
        "WHERE public.mmsearch(t.seq,p.probe) OFFSET 0) hit ORDER BY request.id";
    it::exec_sql(ctx, "ALTER SEQUENCE pg_temp.write_companion_calls RESTART WITH 1");
    const auto correlated_plan = it::explain_text<false>(ctx, correlated);
    REQUIRE(it::plan_contains(correlated_plan, "BioseqBatchJoin"));
    REQUIRE(it::plan_contains(correlated_plan, "CTE Scan on second_cte"));
    REQUIRE(it::query_scalar(ctx, "SELECT is_called FROM pg_temp.write_companion_calls") == "f");
    const auto correlated_rows = it::query_rows(ctx, correlated);
    REQUIRE(correlated_rows.size() == 2);
    REQUIRE(correlated_rows[0][0] == "1");
    REQUIRE(correlated_rows[0][1] == "read_1");
    REQUIRE(correlated_rows[0][2] == read.substr(0u, 256u));
    REQUIRE(correlated_rows[0][3] == "256M");
    REQUIRE(correlated_rows[0][4] == "47");
    REQUIRE(correlated_rows[0][5] == "303");
    REQUIRE(correlated_rows[1][0] == "2");
    REQUIRE(correlated_rows[1][1] == "read_2");
    REQUIRE(correlated_rows[1][2] == read.substr(64u, 384u));
    REQUIRE(correlated_rows[1][3] == "384M");
    REQUIRE(correlated_rows[1][4] == "111");
    REQUIRE(correlated_rows[1][5] == "495");
    REQUIRE(it::query_scalar_int(ctx, "SELECT last_value FROM pg_temp.write_companion_calls") == 2);
    it::exec_sql(ctx, "ALTER SEQUENCE pg_temp.write_companion_calls RESTART WITH 1");
    const auto rescanned_plan = it::explain_text<true>(ctx, correlated);
    REQUIRE(std::ranges::any_of(rescanned_plan, [](const auto& line) {
        return line.find("Custom Scan (BioseqBatchJoin)") != std::string::npos
            && line.find("loops=2") != std::string::npos;
    }));
    REQUIRE(it::query_scalar_int(ctx, "SELECT last_value FROM pg_temp.write_companion_calls") == 2);
    // An unresolved helper request alone is not a retained alignment owner.
    REQUIRE_FALSE(it::plan_contains(it::explain_text<false>(ctx,
        "SELECT bioseq_internal.alignment_query_sequence(t.seq) FROM write_companion_refs t"),
        "Bioseq Retained Alignment Operands"));
    const std::string reads = " FROM write_companion_reads q WHERE q.id=1";
    const std::string join = " FROM write_companion_refs t JOIN write_companion_reads q ON t.id=q.id WHERE public.mmsearch(t.seq,q.seq)";
    const std::string materialized_suffix =
        " SELECT p.header,p.quality,p.probe FROM write_companion_refs t JOIN p ON t.id=p.id WHERE public.mmsearch(t.seq,p.probe)";
    struct companion_case {
        std::string_view name;
        std::string query;
        std::string expected;
        bool counted{};
    };
    const std::array cases{
        companion_case{"known_quality_override",
            "SELECT q.header,q.seq,bioseq.reverse(q.quality)::bioseq.seq('phred63',64) AS quality" + join,
            "SELECT q.header,q.seq,bioseq.reverse(q.quality)::bioseq.seq('phred63',64) AS quality" + reads},
        companion_case{"reversed",
            "SELECT q.header,bioseq.reverse_complement(q.seq) AS seq,bioseq.reverse(q.quality) AS quality "
            "FROM write_companion_refs t JOIN write_companion_reads q ON t.id=q.id WHERE public.mmsearch(t.seq,bioseq.reverse_complement(q.seq))",
            "SELECT q.header,bioseq.reverse_complement(q.seq) AS seq,bioseq.reverse(q.quality) AS quality" + reads},
        companion_case{"sliced",
            "SELECT q.header,q.seq[31:481] AS seq,q.quality[31:481] AS quality "
            "FROM write_companion_refs t JOIN write_companion_reads q ON t.id=q.id WHERE public.mmsearch(t.seq,q.seq[31:481])",
            "SELECT q.header,q.seq[31:481] AS seq,q.quality[31:481] AS quality" + reads},
        companion_case{"materialized",
            "WITH p AS MATERIALIZED (SELECT q.id,q.header,q.quality,pg_temp.write_counted_sequence(q.seq)::bioseq.seq('dna4',64) AS probe"
                + reads + ")" + materialized_suffix,
            "SELECT q.header,q.seq,q.quality" + reads, true},
        companion_case{"scalar_subquery",
            "WITH p AS MATERIALIZED (SELECT q.id,q.header,q.quality,(SELECT pg_temp.write_counted_sequence(q.seq))::bioseq.seq('dna4',64) AS probe"
                + reads + ")" + materialized_suffix,
            "SELECT q.header,q.seq,q.quality" + reads, true},
        companion_case{"local_cte",
            "WITH p AS MATERIALIZED (SELECT q.id,q.header,q.quality,local.probe FROM write_companion_reads q CROSS JOIN LATERAL "
            "(WITH child AS MATERIALIZED (SELECT pg_temp.write_counted_sequence(q.seq)::bioseq.seq('dna4',64) AS probe "
            "FROM write_companion_singleton) SELECT child.probe FROM child) local WHERE q.id=1)" + materialized_suffix,
            "SELECT q.header,q.seq,q.quality" + reads, true},
        companion_case{"stored_literal_quality",
            "WITH p AS MATERIALIZED (SELECT q.id,q.header,q.seq AS probe,repeat('5',512)::bioseq.seq('phred63',64) AS quality"
                + reads + ")" + materialized_suffix,
            "SELECT q.header,q.seq,repeat('5',512)::bioseq.seq('phred63',64) AS quality" + reads},
        companion_case{"mixed_cte",
            "WITH p AS MATERIALIZED (SELECT q.id,q.seq,other.quality,other.header FROM write_companion_reads q "
            "CROSS JOIN write_companion_reads other WHERE q.id=1 AND other.id=2) "
            "SELECT p.header,p.seq,p.quality FROM write_companion_refs t JOIN p ON t.id=p.id WHERE public.mmsearch(t.seq,p.seq)",
            "SELECT q.header,q.seq,q.quality" + reads}
    };
    for (const auto& sample : cases) {
        CAPTURE(sample.name);
        const auto expected = it::query_one(ctx,
            "SELECT expected.header,expected.seq::text,expected.quality::text,bioseq.reverse_complement(expected.seq)::text FROM ("
            + sample.expected + ") expected");
        const auto forward = reference.find(*expected[1]);
        const auto reverse = reference.find(*expected[3]);
        REQUIRE((forward == std::string::npos) != (reverse == std::string::npos));
        const auto start = forward != std::string::npos ? forward : reverse;
        const auto length = expected[1]->size();
        const std::string path = "/tmp/bioseq_write_companion_" + backend + "_" + std::string{sample.name} + ".sam";
        std::filesystem::remove(path);
        if (sample.counted) it::exec_sql(ctx, "ALTER SEQUENCE pg_temp.write_companion_calls RESTART WITH 1");
        const std::string request = "SELECT bioseq.write($query$" + sample.query + "$query$::text,'" + path + "')";
        const auto plan = it::explain_text<false>(ctx, request);
        REQUIRE(it::plan_contains(plan, "BioseqExport"));
        REQUIRE(it::plan_contains(plan, "BioseqBatchJoin"));
        REQUIRE_FALSE(std::filesystem::exists(path));
        if (sample.counted) REQUIRE(it::query_scalar(ctx, "SELECT is_called FROM pg_temp.write_companion_calls") == "f");
        REQUIRE(it::query_scalar_int(ctx, request) == 1);
        if (sample.counted) REQUIRE(it::query_scalar_int(ctx, "SELECT last_value FROM pg_temp.write_companion_calls") == 1);
        const auto records = exported_sam_records(read_export_fixture(path));
        REQUIRE(records.size() == 1);
        REQUIRE(records[0][0] == *expected[0]);
        REQUIRE(records[0][3] == std::to_string(start + 1u));
        REQUIRE(records[0][5] == std::to_string(length) + "M");
        auto native_quality = *expected[2];
        if (forward == std::string::npos) std::ranges::reverse(native_quality);
        REQUIRE(records[0][10] == native_quality);
        it::exec_sql(ctx, "DROP TABLE IF EXISTS write_companion_loaded; "
            "CREATE TABLE write_companion_loaded(header text,seq bioseq.seq('dna4',64),quality bioseq.seq('phred63'),"
            "\"other.header\" text) USING bioseq");
        REQUIRE(it::query_scalar_int(ctx,
            "SELECT bioseq.load('write_companion_loaded'::regclass,'" + path + "')") == 1);
        const auto row = it::query_one(ctx,
            "SELECT header,seq::text,quality::text,(alignment).target_start,(alignment).target_end,"
            "(alignment).cigar::text,\"other.header\" FROM write_companion_loaded");
        REQUIRE(row[0] == expected[0]);
        REQUIRE(row[1] == expected[1]);
        REQUIRE(row[2] == expected[2]);
        REQUIRE(row[3] == std::to_string(start));
        REQUIRE(row[4] == std::to_string(start + length));
        REQUIRE(row[5] == std::to_string(length) + "M");
        if (sample.name == "mixed_cte") REQUIRE(row[6] == "read_2");
        std::filesystem::remove(path);
    }
    // These aliases share the same physical base-column origin, but not the
    // same stored row. With no declared correct quality, completion must fail.
    const std::string denied_path = "/tmp/bioseq_write_companion_" + backend + "_other_consumer.fastq";
    std::filesystem::remove(denied_path);
    const auto denied = it::sql_error(ctx,
        "SELECT bioseq.write($query$WITH source AS MATERIALIZED (SELECT id,header,seq,quality FROM write_companion_reads), "
        "p AS MATERIALIZED (SELECT a.seq,b.quality FROM source a CROSS JOIN source b WHERE a.id=1 AND b.id=2) "
        "SELECT 'read_1'::text AS header,p.seq,p.quality FROM write_companion_refs t CROSS JOIN p "
        "WHERE public.mmsearch(t.seq,p.seq)$query$::text,'" + denied_path + "')");
    REQUIRE(denied.sqlstate == "22023");
    REQUIRE(denied.contains("one unambiguous CTE reference"));
    REQUIRE_FALSE(std::filesystem::exists(denied_path));
    REQUIRE(it::query_scalar_int(ctx, "SELECT 1") == 1);
}

namespace {

/** One existing libpq asynchronous request; no worker thread or Catch assertion. */
struct io_pending_sql_result {
    bool completed{};
    bool consume_ok{true};
    bool successful{true};
    bool fallback_cancelled{};
    unsigned results{};
    std::string first_value;
    it::sql_error_info failure;
};

/** Consume complete protocol results only after PQisBusy reports readiness. */
[[nodiscard]] bool drain_io_sql(it::connection_test_context& connection,
        io_pending_sql_result& result, std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (PQconsumeInput(connection.raw()) == 0) {
            result.consume_ok = false;
            return false;
        }
        if (PQisBusy(connection.raw()) != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
            continue;
        }
        std::unique_ptr<PGresult, decltype(&PQclear)> response{PQgetResult(connection.raw()), &PQclear};
        if (!response) {
            result.completed = true;
            return true;
        }
        ++result.results;
        const auto status = PQresultStatus(response.get());
        result.successful = result.successful && (status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK);
        if (const auto* state = PQresultErrorField(response.get(), PG_DIAG_SQLSTATE))
            result.failure.sqlstate = state;
        if (const auto* message = PQresultErrorMessage(response.get()); message && *message)
            result.failure.message = message;
        if (status == PGRES_TUPLES_OK && PQntuples(response.get()) == 1
            && PQnfields(response.get()) > 0 && !PQgetisnull(response.get(), 0, 0))
            result.first_value = PQgetvalue(response.get(), 0, 0);
    }
    return false;
}

/** Bound unexpected paths, cancel once if necessary, and close an undrainable connection. */
[[nodiscard]] io_pending_sql_result finish_io_sql(it::connection_test_context& worker,
        const it::connection_test_context& controller, long long worker_pid) {
    io_pending_sql_result result;
    if (drain_io_sql(worker, result, std::chrono::seconds{15})) return result;
    result.fallback_cancelled = it::query_scalar(controller,
        fmt::format("SELECT pg_cancel_backend({})", worker_pid)) == "t";
    if (!drain_io_sql(worker, result, std::chrono::seconds{5})) worker.close();
    return result;
}

} // namespace

TEST_CASE("bioseq.load: provenance catalog errors unwind reader admission in the same backend",
          "[component:bioseq][layer:integration][feature:io][resource:postgres][resource:filesystem]") {
    auto loader = it::fresh_context({"load_provenance_cleanup"});
    it::connection_test_context blocker;
    it::connection_test_context observer;
    it::exec_sql(loader,
        "SET bioseq.index.insert_maintenance.enabled=off; "
        "CREATE TABLE load_provenance_cleanup(header text,seq bioseq.seq('dna15'),"
        "quality bioseq.seq('phred94'),alignment bioseq.alignment) USING bioseq; "
        "SET lock_timeout='5s'; SET statement_timeout='20s'");
    it::exec_sql(observer, "SET statement_timeout='2s'");
    const auto loader_pid = it::query_scalar_int(loader, "SELECT pg_backend_pid()");
    const auto blocker_pid = it::query_scalar_int(blocker, "SELECT pg_backend_pid()");
    const std::string prefix = "/tmp/bioseq_provenance_cleanup_" + std::to_string(loader_pid);
    const std::array<std::string, 2> paths{prefix + ".sam", prefix + ".sam.gz"};
    const std::string input = "@HD\tVN:1.6\n@SQ\tSN:chr\tLN:100\n"
        "read\t0\tchr\t1\t37\t4M\t*\t0\t0\tACGT\tIIII\n";
    {
        std::ofstream output(paths[0], std::ios::binary | std::ios::trunc);
        output << input;
        REQUIRE(output.good());
    }
    {
        gzFile output = gzopen(paths[1].c_str(), "wb");
        REQUIRE(output != nullptr);
        const auto written = gzwrite(output, input.data(), static_cast<unsigned>(input.size()));
        const auto closed = gzclose(output);
        REQUIRE(written == static_cast<int>(input.size()));
        REQUIRE(closed == Z_OK);
    }
    constexpr std::string_view rows_sql =
        "SELECT header,seq::text,quality::text,alignment::text "
        "FROM load_provenance_cleanup ORDER BY header,seq::text,alignment::text";
    constexpr std::string_view provenance_sql =
        "SELECT io_provenance::text FROM bioseq_internal.options "
        "WHERE regclass='load_provenance_cleanup'::regclass";
    constexpr std::string_view admission_sql =
        "SELECT index_memory_reserved_bytes FROM bioseq.scheduler_stats()";

    for (const auto& path : paths) {
        CAPTURE(path);
        const std::string load_sql =
            "SELECT bioseq.load('load_provenance_cleanup'::regclass,'" + path + "')";
        // Warm the successful path before comparing backend-local retained admission.
        REQUIRE(it::query_scalar_int(loader, load_sql) == 1);
        for (const bool cancel : {false, true}) {
            for (unsigned repetition = 0; repetition != 2; ++repetition) {
                CAPTURE(cancel, repetition);
                const auto rows_before = it::query_rows(loader, rows_sql);
                const auto provenance_before = it::query_rows(loader, provenance_sql);
                const auto reserved_before = it::query_scalar_int(loader, admission_sql);
                it::exec_sql(blocker, "BEGIN; LOCK TABLE bioseq_internal.options IN SHARE MODE");
                it::exec_sql(loader, "BEGIN");
                const bool sent = PQsendQuery(loader.raw(), load_sql.c_str()) == 1;
                bool observed_initializer_wait = false;
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{4};
                while (sent && std::chrono::steady_clock::now() < deadline) {
                    observed_initializer_wait = it::query_scalar(observer, fmt::format(
                        "SELECT EXISTS(SELECT 1 FROM pg_locks WHERE pid={} "
                        "AND relation='bioseq_internal.options'::regclass "
                        "AND mode='RowExclusiveLock' AND NOT granted) "
                        "AND {}=ANY(pg_blocking_pids({}))",
                        loader_pid, blocker_pid, loader_pid)) == "t";
                    if (observed_initializer_wait) break;
                    if (PQconsumeInput(loader.raw()) == 0 || PQisBusy(loader.raw()) == 0) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds{10});
                }
                // Cancel only after observing the exact catalog wait. A missing wait
                // also cancels/drains before assertions so failure cannot leave work running.
                bool cancellation_sent = false;
                if (sent && (cancel || !observed_initializer_wait)) {
                    cancellation_sent = it::query_scalar(observer,
                        fmt::format("SELECT pg_cancel_backend({})", loader_pid)) == "t";
                }
                const auto result = sent ? finish_io_sql(loader, observer, loader_pid) : io_pending_sql_result{};
                if (result.completed) it::exec_sql(loader, "ROLLBACK");
                else loader.close();
                it::exec_sql(blocker, "ROLLBACK");

                CAPTURE(result.failure.message);
                REQUIRE(sent);
                REQUIRE(result.completed);
                REQUIRE(result.consume_ok);
                REQUIRE(result.results == 1);
                REQUIRE_FALSE(result.successful);
                REQUIRE_FALSE(result.fallback_cancelled);
                REQUIRE(observed_initializer_wait);
                if (cancel) REQUIRE(cancellation_sent);
                REQUIRE(result.failure.sqlstate == (cancel ? "57014" : "55P03"));
                REQUIRE(it::query_scalar_int(loader, "SELECT pg_backend_pid()") == loader_pid);
                REQUIRE(it::query_rows(loader, rows_sql) == rows_before);
                REQUIRE(it::query_rows(loader, provenance_sql) == provenance_before);
                REQUIRE(it::query_scalar_int(loader, admission_sql) == reserved_before);
                REQUIRE(it::query_scalar_int(loader, load_sql) == 1);
                REQUIRE(it::query_scalar_int(loader, admission_sql) == reserved_before);
            }
        }
    }
    for (const auto& path : paths) std::filesystem::remove(path);
}

namespace {

/** Remove test event triggers even when an assertion unwinds the fixture. */
struct io_ddl_test_cleanup {
    it::connection_test_context& worker;
    it::connection_test_context& controller;
    long long worker_pid;
    std::string sql;
    ~io_ddl_test_cleanup() noexcept {
        try {
            if (worker.raw() != nullptr && PQconsumeInput(worker.raw()) == 0) worker.close();
            if (worker.raw() != nullptr && PQisBusy(worker.raw()) != 0) {
                const auto cancel = "SELECT pg_cancel_backend(" + std::to_string(worker_pid) + ")";
                if (controller.raw() != nullptr)
                    if (auto* result = PQexec(controller.raw(), cancel.c_str())) PQclear(result);
                io_pending_sql_result abandoned;
                if (!drain_io_sql(worker, abandoned, std::chrono::seconds{5})) worker.close();
            }
            if (worker.raw() != nullptr)
                if (auto* result = PQexec(worker.raw(), "ROLLBACK; RESET ROLE")) PQclear(result);
        } catch (...) {
            worker.close();
        }
        if (controller.raw() != nullptr && !sql.empty())
            if (auto* result = PQexec(controller.raw(), sql.c_str())) PQclear(result);
    }
};

} // namespace

TEST_CASE("bioseq.load: automatic destination DDL preserves relation identity through event triggers",
          "[component:bioseq][layer:integration][feature:io][resource:postgres][resource:filesystem]") {
    auto loader = it::fresh_context({});
    it::connection_test_context observer;
    it::connection_test_context renamer;
    it::exec_sql(observer, "SET statement_timeout='2s'");
    const auto loader_pid = it::query_scalar_int(loader, "SELECT pg_backend_pid()");
    const std::string role = "bioseq_ddl_guard_importer_" + std::to_string(loader_pid);
    io_ddl_test_cleanup cleanup{loader, observer, loader_pid,
        "SET statement_timeout='3s'; SELECT pg_advisory_unlock_all(); "
        "DROP EVENT TRIGGER IF EXISTS bioseq_ddl_guard_start; "
        "DROP EVENT TRIGGER IF EXISTS bioseq_ddl_guard_end; "
        "DROP SCHEMA IF EXISTS ddl_guard_source,ddl_guard_replacement,ddl_guard_move,"
        "ddl_guard_meta,ddl_guard_renamed CASCADE; "
        "DO $cleanup$ BEGIN IF EXISTS(SELECT 1 FROM pg_catalog.pg_roles WHERE rolname='" + role + "') THEN "
        "EXECUTE 'REVOKE ALL ON SCHEMA bioseq FROM '||quote_ident('" + role + "'); "
        "EXECUTE 'REVOKE pg_read_server_files FROM '||quote_ident('" + role + "'); "
        "EXECUTE 'DROP ROLE '||quote_ident('" + role + "'); END IF; END $cleanup$"};
    it::exec_sql(loader,
        "SET bioseq.index.insert_maintenance.enabled=off; SET statement_timeout='20s'; "
        "CREATE SCHEMA ddl_guard_source; CREATE SCHEMA ddl_guard_replacement; "
        "CREATE SCHEMA ddl_guard_move; CREATE SCHEMA ddl_guard_meta; "
        "CREATE TABLE ddl_guard_source.target(header text,seq bioseq.seq('dna4')) USING bioseq; "
        "CREATE TABLE ddl_guard_source.missing(header text,seq bioseq.seq('dna4')) USING bioseq; "
        "CREATE TABLE ddl_guard_replacement.target(header text,seq bioseq.seq('dna4')) USING bioseq; "
        "CREATE TABLE ddl_guard_meta.control(action text,armed boolean); "
        "INSERT INTO ddl_guard_meta.control VALUES ('',false); "
        "CREATE TABLE ddl_guard_meta.events(phase text,object_id oid); "
        "CREATE SEQUENCE ddl_guard_meta.ends");
    it::exec_sql(loader, "CREATE ROLE " + role + " NOLOGIN");
    const std::string path = "/tmp/bioseq_ddl_guard_" + std::to_string(loader_pid) + ".fastq";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << "@record\nACGT\n+\nIIII\n";
        REQUIRE(output.good());
    }
    it::exec_sql(loader, R"SQL(
        CREATE FUNCTION ddl_guard_meta.on_start() RETURNS event_trigger LANGUAGE plpgsql AS $body$
        DECLARE requested_action text;
        BEGIN
            INSERT INTO ddl_guard_meta.events VALUES ('start',NULL);
            SELECT action INTO requested_action FROM ddl_guard_meta.control WHERE armed;
            IF FOUND THEN
                UPDATE ddl_guard_meta.control SET armed=false;
                IF requested_action<>'' THEN EXECUTE requested_action; END IF;
            END IF;
        END $body$;
        CREATE FUNCTION ddl_guard_meta.on_end() RETURNS event_trigger LANGUAGE plpgsql AS $body$
        BEGIN
            PERFORM nextval('ddl_guard_meta.ends');
            INSERT INTO ddl_guard_meta.events
                SELECT 'end',objid FROM pg_event_trigger_ddl_commands()
                WHERE classid='pg_class'::regclass;
        END $body$;
        CREATE EVENT TRIGGER bioseq_ddl_guard_start ON ddl_command_start
            WHEN TAG IN ('ALTER TABLE') EXECUTE FUNCTION ddl_guard_meta.on_start();
        CREATE EVENT TRIGGER bioseq_ddl_guard_end ON ddl_command_end
            WHEN TAG IN ('ALTER TABLE') EXECUTE FUNCTION ddl_guard_meta.on_end();
    )SQL");
    const auto original_oid = it::query_scalar_int(loader, "SELECT 'ddl_guard_source.target'::regclass::oid");
    const auto replacement_oid = it::query_scalar_int(loader, "SELECT 'ddl_guard_replacement.target'::regclass::oid");
    const std::string load_sql = "SELECT bioseq.load('ddl_guard_source.target'::regclass,'" + path + "')";
    constexpr std::string_view ends_sql =
        "SELECT CASE WHEN is_called THEN last_value ELSE 0 END FROM ddl_guard_meta.ends";
    const auto assert_unmodified = [&loader, original_oid, replacement_oid, loader_pid] {
        REQUIRE(it::query_scalar_int(loader, "SELECT 'ddl_guard_source.target'::regclass::oid") == original_oid);
        REQUIRE(it::query_scalar_int(loader, "SELECT 'ddl_guard_replacement.target'::regclass::oid") == replacement_oid);
        REQUIRE(it::query_scalar_int(loader, "SELECT count(*) FROM ddl_guard_source.target") == 0);
        REQUIRE(it::query_scalar_int(loader, "SELECT count(*) FROM ddl_guard_replacement.target") == 0);
        REQUIRE(it::query_scalar_int(loader,
            "SELECT count(*) FROM pg_attribute WHERE attrelid IN ('ddl_guard_source.target'::regclass,"
            "'ddl_guard_replacement.target'::regclass) AND attname='quality' AND NOT attisdropped") == 0);
        REQUIRE(it::query_scalar_int(loader, fmt::format(
            "SELECT count(*) FROM pg_locks WHERE pid={} AND relation='pg_namespace'::regclass "
            "AND mode='ShareLock' AND granted", loader_pid)) == 0);
    };
    struct attempted_mutation { std::string sql; long long nested_ends; };
    const std::array mutations{
        attempted_mutation{"ALTER SCHEMA ddl_guard_source RENAME TO ddl_guard_renamed; "
            "ALTER SCHEMA ddl_guard_replacement RENAME TO ddl_guard_source", 0},
        attempted_mutation{"ALTER TABLE ddl_guard_source.target RENAME TO renamed", 0},
        attempted_mutation{"ALTER TABLE ddl_guard_source.target SET SCHEMA ddl_guard_move", 0},
        attempted_mutation{"DROP TABLE ddl_guard_source.target", 0},
        // A successful nested load must restore the still-active outer guard.
        attempted_mutation{"SELECT bioseq.load('ddl_guard_replacement.target'::regclass,'" + path + "'); "
            "ALTER SCHEMA ddl_guard_source RENAME TO ddl_guard_renamed", 1}
    };
    for (const auto& mutation : mutations) {
        CAPTURE(mutation.sql);
        it::exec_sql(loader, "UPDATE ddl_guard_meta.control SET armed=true,action=$action$"
            + mutation.sql + "$action$");
        const auto ends_before = it::query_scalar_int(loader, ends_sql);
        const auto failure = it::sql_error(loader, load_sql);
        REQUIRE(failure.sqlstate == "55000");
        REQUIRE(failure.contains("identity changed"));
        // Sequences are nontransactional: a failing outer ADD COLUMN must not
        // reach ddl_command_end. Only the deliberately successful nested load may.
        REQUIRE(it::query_scalar_int(loader, ends_sql) == ends_before + mutation.nested_ends);
        assert_unmodified();
        // The failed scope must also be gone in this backend after rollback.
        it::exec_sql(loader, "ALTER SCHEMA ddl_guard_source RENAME TO ddl_guard_renamed; "
            "ALTER SCHEMA ddl_guard_renamed RENAME TO ddl_guard_source");
    }

    // Pause the successful command inside its ordinary ddl_command_start hook.
    // A second backend must not rename its schema while the identity is guarded.
    it::exec_sql(loader, "TRUNCATE ddl_guard_meta.events; UPDATE ddl_guard_meta.control "
        "SET armed=true,action='SELECT pg_advisory_xact_lock(" + std::to_string(loader_pid) + ")'; BEGIN");
    it::exec_sql(observer, fmt::format("SELECT pg_advisory_lock({})", loader_pid));
    const bool sent = PQsendQuery(loader.raw(), load_sql.c_str()) == 1;
    bool guard_wait_observed = false;
    const auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{4};
    while (sent && std::chrono::steady_clock::now() < wait_deadline) {
        guard_wait_observed = it::query_scalar(observer, fmt::format(
            "SELECT EXISTS(SELECT 1 FROM pg_locks WHERE pid={} AND locktype='advisory' AND NOT granted) "
            "AND EXISTS(SELECT 1 FROM pg_locks WHERE pid={} AND relation='pg_namespace'::regclass "
            "AND mode='ShareLock' AND granted)", loader_pid, loader_pid)) == "t";
        if (guard_wait_observed) break;
        if (PQconsumeInput(loader.raw()) == 0 || PQisBusy(loader.raw()) == 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    it::sql_error_info rename_failure;
    if (guard_wait_observed) {
        it::exec_sql(renamer, "SET lock_timeout='500ms'; SET statement_timeout='2s'");
        rename_failure = it::sql_error(renamer,
            "ALTER SCHEMA ddl_guard_source RENAME TO ddl_guard_renamed");
    }
    it::exec_sql(observer, fmt::format("SELECT pg_advisory_unlock({})", loader_pid));
    const auto result = sent ? finish_io_sql(loader, observer, loader_pid) : io_pending_sql_result{};
    if (!result.completed) loader.close();
    CAPTURE(result.failure.message);
    REQUIRE(sent);
    REQUIRE(result.completed);
    REQUIRE(result.consume_ok);
    REQUIRE_FALSE(result.fallback_cancelled);
    REQUIRE(guard_wait_observed);
    REQUIRE(rename_failure.sqlstate == "55P03");
    REQUIRE(result.successful);
    REQUIRE(result.results == 1);
    REQUIRE(result.first_value == "1");
    REQUIRE(it::query_scalar_int(loader, fmt::format(
        "SELECT count(*) FROM pg_locks WHERE pid={} AND relation='pg_namespace'::regclass "
        "AND mode='ShareLock' AND granted", loader_pid)) == 0);
    // Keep the loader transaction open: schema renames now succeed independently
    // despite its transaction-held target AccessExclusiveLock.
    it::exec_sql(renamer, "SET lock_timeout='2s'; ALTER SCHEMA ddl_guard_source RENAME TO ddl_guard_renamed; "
        "ALTER SCHEMA ddl_guard_renamed RENAME TO ddl_guard_source");
    REQUIRE(it::query_scalar_int(loader, "SELECT count(*) FROM ddl_guard_meta.events WHERE phase='start'") == 1);
    REQUIRE(it::query_scalar_int(loader, fmt::format(
        "SELECT count(*) FROM ddl_guard_meta.events WHERE phase='end' AND object_id={}", original_oid)) >= 1);
    it::exec_sql(loader, "COMMIT");

    // Missing destinations retain owner checks; predeclared columns need INSERT.
    it::exec_sql(loader, "GRANT pg_read_server_files TO " + role + "; GRANT USAGE ON SCHEMA "
        "bioseq,ddl_guard_source TO " + role + "; GRANT INSERT ON ddl_guard_source.target,"
        "ddl_guard_source.missing TO " + role + "; SET ROLE " + role);
    const auto owner_failure = it::sql_error(loader,
        "SELECT bioseq.load('ddl_guard_source.missing'::regclass,'" + path + "')");
    const auto predeclared = it::query_scalar_int(loader, load_sql);
    it::exec_sql(loader, "RESET ROLE");
    REQUIRE(owner_failure.sqlstate == "42501");
    REQUIRE(owner_failure.contains("predeclare"));
    REQUIRE(predeclared == 1);
    REQUIRE(it::query_scalar_int(loader, "SELECT count(*) FROM ddl_guard_source.target") == 2);
    REQUIRE(it::query_scalar_int(loader,
        "SELECT count(*) FROM pg_attribute WHERE attrelid='ddl_guard_source.missing'::regclass "
        "AND attname='quality' AND NOT attisdropped") == 0);
    REQUIRE(it::query_scalar_int(loader, "SELECT count(*) FROM ddl_guard_meta.events WHERE phase='start'") == 1);

    // A normal end trigger may remove a column without changing relation identity.
    // The alignment caller must report the absent attribute before TupleDescAttr.
    const std::string sam_path = path + ".sam";
    {
        std::ofstream output(sam_path, std::ios::binary | std::ios::trunc);
        output << "@HD\tVN:1.6\n@SQ\tSN:chr\tLN:100\n"
            "aligned\t0\tchr\t1\t37\t4M\t*\t0\t0\tACGT\tIIII\n";
        REQUIRE(output.good());
    }
    it::exec_sql(loader, R"SQL(
        UPDATE ddl_guard_meta.control SET armed=false,action='drop_alignment';
        CREATE OR REPLACE FUNCTION ddl_guard_meta.on_end() RETURNS event_trigger LANGUAGE plpgsql AS $body$
        BEGIN
            IF EXISTS(SELECT 1 FROM ddl_guard_meta.control WHERE action='drop_alignment') THEN
                UPDATE ddl_guard_meta.control SET action='';
                ALTER TABLE ddl_guard_source.target DROP COLUMN alignment;
            END IF;
        END $body$;
    )SQL");
    const auto absent_attribute = it::sql_error(loader,
        "SELECT bioseq.load('ddl_guard_source.target'::regclass,'" + sam_path + "')");
    REQUIRE(absent_attribute.sqlstate == "42703");
    REQUIRE(absent_attribute.contains("alignment destination"));
    REQUIRE(it::query_scalar_int(loader, "SELECT count(*) FROM ddl_guard_source.target") == 2);
    REQUIRE(it::query_scalar_int(loader,
        "SELECT count(*) FROM pg_attribute WHERE attrelid='ddl_guard_source.target'::regclass "
        "AND attname='alignment' AND NOT attisdropped") == 0);
    REQUIRE(it::query_scalar_int(loader, fmt::format(
        "SELECT count(*) FROM pg_locks WHERE pid={} AND relation='pg_namespace'::regclass "
        "AND mode='ShareLock' AND granted", loader_pid)) == 0);
    std::filesystem::remove(path);
    std::filesystem::remove(sam_path);
    it::exec_sql(observer, cleanup.sql);
    cleanup.sql.clear();
}

TEST_CASE("bioseq.load: a concurrently predeclared destination is reused after the target lock wait",
          "[component:bioseq][layer:integration][feature:io][resource:postgres][resource:filesystem]") {
    auto loader = it::fresh_context({"ddl_concurrent_column"});
    it::connection_test_context owner;
    it::connection_test_context observer;
    it::exec_sql(observer, "SET statement_timeout='2s'");
    it::exec_sql(owner, "SET statement_timeout='5s'");
    it::exec_sql(loader,
        "SET bioseq.index.insert_maintenance.enabled=off; SET statement_timeout='20s'; "
        "CREATE TABLE ddl_concurrent_column(header text,seq bioseq.seq('dna4')) USING bioseq");
    const auto loader_pid = it::query_scalar_int(loader, "SELECT pg_backend_pid()");
    const auto owner_pid = it::query_scalar_int(owner, "SELECT pg_backend_pid()");
    const std::string path = "/tmp/bioseq_ddl_concurrent_" + std::to_string(loader_pid) + ".fastq";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << "@concurrent\nACGT\n+\nIIII\n";
        REQUIRE(output.good());
    }
    it::exec_sql(owner, "BEGIN; ALTER TABLE ddl_concurrent_column ADD COLUMN quality bioseq.seq('phred42')");
    const std::string load_sql = "SELECT bioseq.load('ddl_concurrent_column'::regclass,'" + path
        + "',columns=>ARRAY[ROW('quality','quality')::bioseq.column_mapping])";
    const bool sent = PQsendQuery(loader.raw(), load_sql.c_str()) == 1;
    bool wait_observed = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{4};
    while (sent && std::chrono::steady_clock::now() < deadline) {
        wait_observed = it::query_scalar(observer, fmt::format(
            "SELECT EXISTS(SELECT 1 FROM pg_locks WHERE pid={} AND relation='ddl_concurrent_column'::regclass "
            "AND mode='RowExclusiveLock' AND NOT granted) AND {}=ANY(pg_blocking_pids({}))",
            loader_pid, owner_pid, loader_pid)) == "t";
        if (wait_observed) break;
        if (PQconsumeInput(loader.raw()) == 0 || PQisBusy(loader.raw()) == 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    it::exec_sql(owner, "COMMIT");
    const auto result = sent ? finish_io_sql(loader, observer, loader_pid) : io_pending_sql_result{};
    if (!result.completed) loader.close();
    CAPTURE(result.failure.message);
    REQUIRE(sent);
    REQUIRE(result.completed);
    REQUIRE(result.consume_ok);
    REQUIRE_FALSE(result.fallback_cancelled);
    REQUIRE(wait_observed);
    REQUIRE(result.successful);
    REQUIRE(result.results == 1);
    REQUIRE(result.first_value == "1");
    REQUIRE(it::query_scalar_int(loader,
        "SELECT count(*) FROM pg_attribute WHERE attrelid='ddl_concurrent_column'::regclass "
        "AND attname='quality' AND NOT attisdropped") == 1);
    REQUIRE(it::query_scalar(loader, "SELECT quality::text FROM ddl_concurrent_column") == "IIII");
    std::filesystem::remove(path);
}

namespace {

/** Release the type DDL before draining any abandoned prepared execution. */
struct io_type_plan_test_cleanup {
    it::connection_test_context& worker;
    it::connection_test_context& owner;
    it::connection_test_context& controller;
    std::string cancel_sql;
    std::string drop_sql;
    std::filesystem::path output;
    bool pending{};

    /** Cleanup commands use the session's already bounded statement timeout. */
    static bool command(it::connection_test_context& connection, const char* sql) noexcept {
        if (connection.raw() == nullptr) return false;
        auto* result = PQexec(connection.raw(), sql);
        if (result == nullptr) return false;
        const auto status = PQresultStatus(result);
        PQclear(result);
        return status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK;
    }

    ~io_type_plan_test_cleanup() noexcept {
        if (!command(owner, "ROLLBACK")) owner.close();
        try {
            if (pending && worker.raw() != nullptr) {
                io_pending_sql_result abandoned;
                if (!drain_io_sql(worker, abandoned, std::chrono::seconds{1})) {
                    static_cast<void>(command(controller, cancel_sql.c_str()));
                    if (!drain_io_sql(worker, abandoned, std::chrono::seconds{5})) worker.close();
                }
            }
            if (worker.raw() != nullptr && !command(worker, "ROLLBACK; DEALLOCATE ALL")) worker.close();
        } catch (...) {
            worker.close();
        }
        if (!drop_sql.empty()) static_cast<void>(command(controller, drop_sql.c_str()));
        std::error_code ignored;
        std::filesystem::remove(output, ignored);
    }
};

} // namespace

TEST_CASE("bioseq.write: cached metadata classification locks and revalidates nested composite types",
          "[component:bioseq][layer:integration][feature:io][resource:postgres][resource:filesystem]") {
    auto worker = it::fresh_context({});
    it::connection_test_context owner;
    it::connection_test_context controller;
    it::exec_sql(worker, "SET statement_timeout='20s'; SET lock_timeout='10s'; "
        "SET plan_cache_mode=force_generic_plan; SET bioseq.index.insert_maintenance.enabled=off");
    it::exec_sql(owner, "SET statement_timeout='5s'; SET lock_timeout='3s'");
    it::exec_sql(controller, "SET statement_timeout='2s'; SET lock_timeout='1s'");
    const auto worker_pid = it::query_scalar_int(worker, "SELECT pg_backend_pid()");
    const auto owner_pid = it::query_scalar_int(owner, "SELECT pg_backend_pid()");
    const auto quote_identifier = [&worker](std::string_view value) {
        std::unique_ptr<char, decltype(&PQfreemem)> escaped{
            PQescapeIdentifier(worker.raw(), value.data(), value.size()), &PQfreemem};
        REQUIRE(escaped != nullptr);
        return std::string{escaped.get()};
    };
    const auto quote_literal = [&worker](std::string_view value) {
        std::unique_ptr<char, decltype(&PQfreemem)> escaped{
            PQescapeLiteral(worker.raw(), value.data(), value.size()), &PQfreemem};
        REQUIRE(escaped != nullptr);
        return std::string{escaped.get()};
    };
    const std::string schema = quote_identifier("bioseq_nested_type_lock_" + std::to_string(worker_pid));
    const std::string leaf = schema + "." + quote_identifier("leaf");
    const std::string wrapper = schema + "." + quote_identifier("wrapper");
    const std::string payload = quote_identifier("payload");
    const std::string statement_name = "bioseq_nested_type_plan_" + std::to_string(worker_pid);
    const std::string statement = quote_identifier(statement_name);
    const std::string path = (std::filesystem::temp_directory_path()
        / ("bioseq_nested_type_lock_" + std::to_string(worker_pid) + ".fasta")).string();
    const std::string drop_sql = "DROP SCHEMA IF EXISTS " + schema + " CASCADE";
    io_type_plan_test_cleanup cleanup{worker, owner, controller,
        fmt::format("SELECT pg_cancel_backend({})", worker_pid), drop_sql, path};
    std::filesystem::remove(path);
    it::exec_sql(owner, "CREATE SCHEMA " + schema + "; CREATE TYPE " + leaf
        + " AS (\"note\" integer); CREATE TYPE " + wrapper + " AS (\"item\" " + leaf + ")");
    const auto leaf_relation = it::query_scalar_int(controller,
        "SELECT typrelid FROM pg_catalog.pg_type WHERE oid=" + quote_literal(leaf) + "::regtype");
    REQUIRE(leaf_relation > 0);
    const std::string source = "SELECT 'typed'::text AS header,'AC'::bioseq.seq('dna4') AS seq,NULL::"
        + wrapper + " AS mutable_metadata";
    it::exec_sql(worker, "PREPARE " + statement + " AS SELECT bioseq.write("
        + quote_literal(source) + "::text," + quote_literal(path)
        + ",options=>'{\"overwrite\":true}'::jsonb)");
    const std::string execute_sql = "EXECUTE " + statement;
    const std::string generic_count_sql = "SELECT generic_plans FROM pg_catalog.pg_prepared_statements WHERE name="
        + quote_literal(statement_name);
    REQUIRE(it::query_scalar_int(worker, execute_sql) == 1);
    REQUIRE(it::query_scalar_int(worker, generic_count_sql) == 1);
    REQUIRE(read_export_fixture(path).find("mutable_metadata") != std::string::npos);

    // The source query names only the wrapper. The leaf is discovered by
    // metadata classification and is otherwise unused in the SQL expression.
    it::exec_sql(owner, "BEGIN; ALTER TYPE " + leaf + " ADD ATTRIBUTE " + payload + " bioseq.seq");
    REQUIRE(it::query_scalar(controller, fmt::format(
        "SELECT EXISTS(SELECT 1 FROM pg_catalog.pg_locks WHERE pid={} AND locktype='relation' "
        "AND relation={}::oid AND mode='AccessExclusiveLock' AND granted)",
        owner_pid, leaf_relation)) == "t");
    const bool sent = PQsendQuery(worker.raw(), execute_sql.c_str()) == 1;
    cleanup.pending = sent;
    bool type_wait_observed = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{4};
    while (sent && std::chrono::steady_clock::now() < deadline) {
        type_wait_observed = it::query_scalar(controller, fmt::format(
            "SELECT EXISTS(SELECT 1 FROM pg_catalog.pg_locks WHERE pid={} AND locktype='relation' "
            "AND relation={}::oid AND mode='AccessShareLock' AND NOT granted) "
            "AND {}=ANY(pg_catalog.pg_blocking_pids({}))",
            worker_pid, leaf_relation, owner_pid, worker_pid)) == "t";
        if (type_wait_observed) break;
        if (PQconsumeInput(worker.raw()) == 0 || PQisBusy(worker.raw()) == 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    // Commit and drain before assertions. The queued invalidation must be
    // processed after lock acquisition, rebuilding the old generic plan.
    it::exec_sql(owner, "COMMIT");
    const auto result = sent ? finish_io_sql(worker, controller, worker_pid) : io_pending_sql_result{};
    cleanup.pending = sent && !result.completed;
    if (!result.completed) worker.close();
    CAPTURE(result.failure.message);
    REQUIRE(sent);
    REQUIRE(type_wait_observed);
    REQUIRE(result.completed);
    REQUIRE(result.consume_ok);
    REQUIRE(result.successful);
    REQUIRE(result.results == 1);
    REQUIRE_FALSE(result.fallback_cancelled);
    REQUIRE(result.first_value == "1");
    REQUIRE(it::query_scalar_int(worker, generic_count_sql) == 2);
    REQUIRE(read_export_fixture(path).find("mutable_metadata") == std::string::npos);
    REQUIRE(it::query_scalar_int(worker, "SELECT pg_backend_pid()") == worker_pid);
    REQUIRE(it::query_scalar_int(controller, fmt::format(
        "SELECT count(*) FROM pg_catalog.pg_locks WHERE pid={} AND locktype='relation' "
        "AND relation={}::oid AND mode='AccessShareLock'", worker_pid, leaf_relation)) == 0);

    // The same backend and prepared statement remain usable after the replan.
    it::exec_sql(owner, "ALTER TYPE " + leaf + " DROP ATTRIBUTE " + payload);
    REQUIRE(it::query_scalar_int(worker, execute_sql) == 1);
    REQUIRE(it::query_scalar_int(worker, generic_count_sql) == 3);
    REQUIRE(read_export_fixture(path).find("mutable_metadata") != std::string::npos);
    it::exec_sql(worker, "DEALLOCATE " + statement);
    it::exec_sql(controller, drop_sql);
    cleanup.drop_sql.clear();
    REQUIRE(std::filesystem::remove(path));
}
