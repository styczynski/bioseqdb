/**
 * @file bioseq/tests/integration/load_test.cpp
 * @brief Tests loading sequence files into Bioseq tables
 *
 * Checks that loaded sequences can  be read and searched across storage widths,
 * including records split over sezeral fragments. Also tests index creation,
 * header fields and defaults, and rollback when loading fails.
 */
#include <cstdlib>
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
    auto ctx = it::fresh_context({"write_join_refs", "write_join_reads", "write_join_loaded"});
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
            }
        }
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
    auto ctx = it::fresh_context({"write_native_source", "write_native_loaded", "write_native_invalid"});
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
