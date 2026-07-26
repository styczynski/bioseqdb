// DOC0279 §16.2 — the new CHUNKED_PER_FRAGMENT storage + per-fragment assemble.
// Proves: on-insert index build, exact-rowno chunk placement, assemble==recompute
// (single-segment dna4 byte-identical), forced-scanner disables assembly, ROLLBACK
// leaves no index rows, and the reserved-sentinel / missing-manifest corruption hooks.
#include "common_tests_integration.h"
#include <array>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>
import bioseq.integration_tests.common;

using namespace bioseq_int_tests;

namespace {
// n rows: ~5 kbp query fragments mutated out of 200 kbp dna4 targets, no reverse.
inline void seed_sample(const connection_test_context& ctx, std::string_view name, int n) {
    exec_sqlf(ctx, "DROP TABLE IF EXISTS {} CASCADE;", name);
    exec_sqlf(ctx,
        "SELECT bioseq.create_query_target_sample_table('{}', {}, 5000, 100.0, 200000, "
        "0.001, 0.0005, 0.0005, 1.0, 1.0, 0.0, 13579);", name, n);
}
inline std::set<std::tuple<long long, long long, long long>> mmsearch_set(
        const connection_test_context& ctx, std::string_view tbl) {
    auto rows = query_rows(ctx, fmt::format(
        "SELECT id, bioseq.target_start(id), bioseq.target_end(id) FROM {} "
        "WHERE mmsearch(target, query) ORDER BY id", tbl));
    std::set<std::tuple<long long, long long, long long>> out;
    for (auto& r : rows)
        if (r[0] && r[1] && r[2])
            out.emplace(std::stoll(*r[0]), std::stoll(*r[1]), std::stoll(*r[2]));
    return out;
}
}  // namespace

TEST_CASE("DOC0279 per-fragment storage is created on insert with the right query_keys",
          "[indexing][per-fragment]") {
    auto ctx = fresh_context({"doc0279_cs_seed"});
    seed_small_bioseq_column(ctx, "doc0279_cs_seed");   // 3 dna4/32 rows, update_index_on_insert=on
    REQUIRE(index_table_row_count(ctx, "minimizers_per_fragment", "doc0279_cs_seed") > 0);
    REQUIRE(index_table_row_count(ctx, "symbol_counts", "doc0279_cs_seed") == 3);

    // Exact-rowno chunk placement: every indexed columnar rowno has >=1 sub_key.
    auto rownos = query_int_column(ctx,
        "SELECT bioseq.column_row_number('doc0279_cs_seed'::regclass, ctid) "
        "FROM doc0279_cs_seed ORDER BY 1");
    REQUIRE(rownos.size() == 3);
    for (long long rn : rownos)
        REQUIRE(index_distinct_sub_keys(ctx, "minimizers_per_fragment", "doc0279_cs_seed", rn) >= 1);
}

TEST_CASE("DOC0279 mmsearch per-fragment assemble == recompute (single-segment dna4)",
          "[indexing][per-fragment]") {
    auto ctx = fresh_context({"doc0279_cs"});
    seed_sample(ctx, "doc0279_cs", 3);
    exec_sqlf(ctx, "SET bioseq.update_index_on_insert = off;");
    REQUIRE(query_scalar_int(ctx,
        "SELECT bioseq.update_index('doc0279_cs'::regclass, 'minimizers_per_fragment')") == 3);

    guc_scope rsi_on(ctx, "bioseq.read_sequence_index", "on");
    const auto assembled = mmsearch_set(ctx, "doc0279_cs");
    REQUIRE(assembled.size() == 3);                              // NON-EMPTY proof

    exec_sqlf(ctx, "SET bioseq.read_sequence_index = off;");
    const auto recomputed = mmsearch_set(ctx, "doc0279_cs");
    REQUIRE(assembled == recomputed);                            // strict byte-identical
}

TEST_CASE("DOC0279 forced-scanner GUC disables stored assembly but results stay correct",
          "[indexing][per-fragment]") {
    auto ctx = fresh_context({"doc0279_cs_fs"});
    seed_sample(ctx, "doc0279_cs_fs", 3);
    exec_sqlf(ctx, "SET bioseq.update_index_on_insert = off;");
    exec_sqlf(ctx, "SELECT bioseq.update_index('doc0279_cs_fs'::regclass, 'minimizers_per_fragment');");
    guc_scope rsi_on(ctx, "bioseq.read_sequence_index", "on");
    guc_scope chunked(ctx, "bioseq.test_force_chunked_scanner", "on");  // not the production scanner_pg path
    REQUIRE(mmsearch_set(ctx, "doc0279_cs_fs").size() == 3);
}

TEST_CASE("DOC0279 index rows do not survive a ROLLBACK", "[indexing][per-fragment]") {
    auto ctx = fresh_context({"doc0279_cs_rb"});
    seed_small_bioseq_column(ctx, "doc0279_cs_rb");
    const long long before = index_table_row_count(ctx, "minimizers_per_fragment", "doc0279_cs_rb");
    exec_sql(ctx, "BEGIN;");
    exec_sql(ctx, "INSERT INTO doc0279_cs_rb VALUES (99, 'GGGGCCCCAAAATTTT'::bioseq.seq('dna4',32));");
    exec_sql(ctx, "ROLLBACK;");
    REQUIRE(query_scalar_int(ctx, "SELECT count(*) FROM doc0279_cs_rb WHERE pk=99") == 0);
    REQUIRE(index_table_row_count(ctx, "minimizers_per_fragment", "doc0279_cs_rb") == before);
}

#ifdef BIOSEQ_TEST_INDEXES
TEST_CASE("DOC0371 revision-3 full-generation payload uses one ordered scan",
          "[indexing][doc0371][rev3][full-generation]") {
    auto ctx = fresh_context({"doc0371_full_generation"});
    exec_sql(ctx,
        "CREATE TABLE doc0371_full_generation "
        "(id int, seq bioseq.seq('dna4', 64)) USING bioseq;");
    exec_sql(ctx, "SET bioseq.update_index_on_insert = off;");
    exec_sql(ctx,
        "INSERT INTO doc0371_full_generation VALUES "
        // Cross the canonical 8-Mbp grouped32 segment span. The fixture
        // producer distributes its bounded event set over this source, so the
        // Vulkan-labelled generation must exercise multiple directory and
        // position segments instead of weakening the assertion to >=1.
        "(1, repeat('ACGT', 2100000)::bioseq.seq('dna4', 64));");
    const long long rowno = query_scalar_int(ctx,
        "SELECT bioseq.column_row_number("
        "'doc0371_full_generation'::regclass, ctid) "
        "FROM doc0371_full_generation");
    const auto built = query_one(ctx, fmt::format(
        "SELECT * FROM bioseq_internal.build_minimizer_fixture("
        "'doc0371_full_generation'::regclass, "
        "'minimizers_per_fragment', {}, 15::smallint, 10::smallint, "
        "4::smallint);", rowno));
    connection_test_context reader;
    const auto read = query_one(reader, fmt::format(
        "SELECT * FROM "
        "bioseq_internal.scan_minimizer_generation_fixture("
        "'doc0371_full_generation'::regclass, "
        "'minimizers_per_fragment', {}, 15::smallint, 10::smallint, "
        "4::smallint, 'scalar');", rowno));
    REQUIRE(read.size() == 16);
    REQUIRE(read[0] == built[0]);
    REQUIRE(read[1].value() == "scalar");
    REQUIRE(read[2] == built[2]);
    REQUIRE(read[3] == built[16]);
    REQUIRE(read[4] == built[17]);
    REQUIRE(read[5]->size() == 16);
    REQUIRE(read[6]->size() == 16);
    REQUIRE(read[7].value() == "1");
    REQUIRE(read[8].value() == "3"); // head + metadata + one payload scan
    REQUIRE(read[9].value() == "1");
    REQUIRE(read[10] == read[11]);
    REQUIRE(std::stoll(read[12].value())
        == std::stoll(read[3].value()) + std::stoll(read[4].value()));
    REQUIRE(std::stoll(read[13].value())
        == std::stoll(built[14].value()) * 2);
    REQUIRE(read[14] == read[13]);
    REQUIRE(read[15].value() == "1");
    REQUIRE(std::stoll(read[11].value())
        > std::stoll(read[13].value())); // every extent is multi-chunk here

    // Paper ntHash has a distinct persisted semantic identity and therefore
    // cannot consume the scalar/exact/vulkan legacy-scheme generation.
    const auto paper_mismatch = sql_error(reader, fmt::format(
        "SELECT * FROM "
        "bioseq_internal.scan_minimizer_generation_fixture("
        "'doc0371_full_generation'::regclass, "
        "'minimizers_per_fragment', {}, 15::smallint, 10::smallint, "
        "4::smallint, 'paper');", rowno));
    REQUIRE(paper_mismatch.sqlstate == "55000");
    REQUIRE(paper_mismatch.contains("compatibility"));
    REQUIRE(query_scalar_int(reader, "SELECT 1;") == 1);

    const std::array backend_cases{
        std::pair{std::string_view{"exact"},
            std::string_view{"exact_simd"}},
        std::pair{std::string_view{"paper"},
            std::string_view{"paper_nthash_simd"}},
        std::pair{std::string_view{"vulkan"},
            std::string_view{"vulkan_segment"}},
    };
    for (const auto& [requested, canonical] : backend_cases) {
        const auto backend_build = query_one(ctx, fmt::format(
            "SELECT * FROM bioseq_internal.build_minimizer_fixture("
            "'doc0371_full_generation'::regclass, "
            "'minimizers_per_fragment', {}, 15::smallint, 10::smallint, "
            "4::smallint, '{}');", rowno, requested));
        const auto backend_read = query_one(reader, fmt::format(
            "SELECT * FROM "
            "bioseq_internal.scan_minimizer_generation_fixture("
            "'doc0371_full_generation'::regclass, "
            "'minimizers_per_fragment', {}, 15::smallint, 10::smallint, "
            "4::smallint, '{}');", rowno, requested));
        REQUIRE(backend_read[1].value() == canonical);
        REQUIRE(backend_read[2] == backend_build[2]);
        if (requested == "vulkan") {
            REQUIRE(std::stoll(backend_build[14].value()) >= 2);
            REQUIRE(std::stoll(backend_build[15].value()) >= 2);
            REQUIRE(std::stoll(backend_read[13].value())
                == std::stoll(backend_build[14].value()) * 2);
            REQUIRE(std::stoll(backend_read[11].value())
                > std::stoll(backend_read[13].value()));
        }
        const std::string_view incompatible_backend =
            requested == "paper" ? "scalar" : "paper";
        const auto incompatible_read = sql_error(
            reader, fmt::format(
                "SELECT * FROM "
                "bioseq_internal.scan_minimizer_generation_fixture("
                "'doc0371_full_generation'::regclass, "
                "'minimizers_per_fragment', {}, 15::smallint, 10::smallint, "
                "4::smallint, '{}');", rowno, incompatible_backend));
        REQUIRE(incompatible_read.sqlstate == "55000");
        REQUIRE(incompatible_read.contains("compatibility"));
    }
}

TEST_CASE("DOC0371 revision-3 full-generation payload rejects row corruption",
          "[indexing][doc0371][rev3][full-generation][corruption]"
          "[pg-storage-recovery]") {
    auto ctx = fresh_context({"doc0371_full_generation_corrupt"});
    exec_sql(ctx,
        "CREATE TABLE doc0371_full_generation_corrupt "
        "(id int, seq bioseq.seq('dna4', 64)) USING bioseq;");
    exec_sql(ctx,
        "INSERT INTO doc0371_full_generation_corrupt VALUES "
        "(1, repeat('TGCA', 4096)::bioseq.seq('dna4', 64));");
    const long long rowno = query_scalar_int(ctx,
        "SELECT bioseq.column_row_number("
        "'doc0371_full_generation_corrupt'::regclass, ctid) "
        "FROM doc0371_full_generation_corrupt");
    const auto built = query_one(ctx, fmt::format(
        "SELECT * FROM bioseq_internal.build_minimizer_fixture("
        "'doc0371_full_generation_corrupt'::regclass, "
        "'minimizers_per_fragment', {}, 15::smallint, 10::smallint, "
        "4::smallint);", rowno));
    exec_sql(ctx, fmt::format(
        "UPDATE bioseq_internal.{0} SET blob=set_byte(blob,0,"
        "get_byte(blob,0) # 1) WHERE ctid=(SELECT ctid FROM "
        "bioseq_internal.{0} WHERE generation_id={1} "
        "AND position_segment>=0 ORDER BY sub_key,position_segment,chunk_no "
        "LIMIT 1);",
        built[0].value(), built[2].value()));
    connection_test_context reader;
    const long long reader_backend_pid = query_scalar_int(
        reader, "SELECT pg_backend_pid();");
    const auto error = sql_error(reader, fmt::format(
        "SELECT * FROM "
        "bioseq_internal.scan_minimizer_generation_fixture("
        "'doc0371_full_generation_corrupt'::regclass, "
        "'minimizers_per_fragment', {}, 15::smallint, 10::smallint, "
        "4::smallint, 'scalar');", rowno));
    REQUIRE(error.sqlstate == "55000");
    REQUIRE(error.contains("checksum"));
    REQUIRE(query_scalar_int(reader, "SELECT 1;") == 1);
    REQUIRE(query_scalar_int(reader, "SELECT pg_backend_pid();")
        == reader_backend_pid);

    // The scan-wide PostgreSQL bridge must unwind the callback-scoped receipt,
    // scan descriptor, locks and snapshot before propagating corruption. A new
    // immutable generation in the same source relation must reopen and stream
    // successfully through the same backend connection.
    const auto rebuilt = query_one(ctx, fmt::format(
        "SELECT * FROM bioseq_internal.build_minimizer_fixture("
        "'doc0371_full_generation_corrupt'::regclass, "
        "'minimizers_per_fragment', {}, 15::smallint, 10::smallint, "
        "4::smallint);", rowno));
    const auto recovered = query_one(reader, fmt::format(
        "SELECT * FROM "
        "bioseq_internal.scan_minimizer_generation_fixture("
        "'doc0371_full_generation_corrupt'::regclass, "
        "'minimizers_per_fragment', {}, 15::smallint, 10::smallint, "
        "4::smallint, 'scalar');", rowno));
    REQUIRE(recovered.size() == 16);
    REQUIRE(recovered[0] == rebuilt[0]);
    REQUIRE(recovered[2] == rebuilt[2]);
    REQUIRE(recovered[7].value() == "1");
    REQUIRE(recovered[8].value() == "3");
    REQUIRE(recovered[9].value() == "1");
    REQUIRE(query_scalar_int(reader, "SELECT pg_backend_pid();")
        == reader_backend_pid);
}

TEST_CASE("DOC0279 missing manifest forces recompute; reserved sentinels are ignored",
          "[indexing][per-fragment]") {
    auto ctx = fresh_context({"doc0279_cs_cor"});
    seed_sample(ctx, "doc0279_cs_cor", 1);
    exec_sqlf(ctx, "SET bioseq.update_index_on_insert = off;");
    exec_sqlf(ctx, "SELECT bioseq.update_index('doc0279_cs_cor'::regclass, 'minimizers_per_fragment');");
    const long long rn = query_scalar_int(ctx,
        "SELECT bioseq.column_row_number('doc0279_cs_cor'::regclass, ctid) FROM doc0279_cs_cor LIMIT 1");

    // A reserved-band tuple (sub_key in the TEMP_RUN reserved range) must NOT be counted as a data fragment.
    const long long distinct_before = index_distinct_sub_keys(ctx, "minimizers_per_fragment", "doc0279_cs_cor", rn);
    exec_sqlf(ctx,
        "SELECT bioseq_internal.write_index_chunk('doc0279_cs_cor'::regclass, 'minimizers_per_fragment', "
        "{}, (4611686018427387904)::bigint, 0, '\\x00'::bytea);", rn);   // sub_key = 1<<62 (TEMP_RUN_BASE)
    REQUIRE(index_distinct_sub_keys(ctx, "minimizers_per_fragment", "doc0279_cs_cor", rn) == distinct_before);

    // Deleting the completeness manifest makes the row read as "absent" -> recompute (still correct, non-empty).
    exec_sqlf(ctx,
        "SELECT bioseq_internal.delete_index_manifest('doc0279_cs_cor'::regclass, 'minimizers_per_fragment', {});", rn);
    guc_scope rsi_on(ctx, "bioseq.read_sequence_index", "on");
    REQUIRE(mmsearch_set(ctx, "doc0279_cs_cor").size() == 1);   // recompute fallback still finds the match
}
#endif
