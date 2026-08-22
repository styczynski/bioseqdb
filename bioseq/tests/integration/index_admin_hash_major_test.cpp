/**
 * @file index_admin_hash_major_test.cpp
 * @brief Exercises PostgreSQL hash-major publication, streaming, and failure cleanup.
 */

#include <libpq-fe.h>

#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.tests.integration.support;
import bioseq.tests.integration.index_admin_test_support;

using namespace bioseq_int_tests;
using namespace bioseq_int_tests::index_admin_test_support;

namespace {

#ifdef BIOSEQ_TEST_INDEXES
/** Read one facade-source instrumentation counter. */
[[nodiscard]] std::int64_t facade_source_counter(connection_test_context &ctx,
                                                 std::string_view name) {
  return query_scalar_int(
      ctx, fmt::format("SELECT (bioseq_internal.sequence_source_counters()"
                       "        ->>'{}')::bigint;",
                       name));
}
#endif

}  // namespace

TEST_CASE("bridged PostgreSQL build and publication leaves preserve published state",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    constexpr std::string_view table = "index_admin_sequence_minimizer_pg_recovery";
    auto ctx = fresh_context({table});
    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
    exec_sqlf(ctx,
        "CREATE TABLE {} (id int, seq bioseq.seq('dna4', 64)) USING bioseq;",
        table);
    exec_sqlf(ctx,
        "INSERT INTO {} VALUES "
        "(1, (repeat('ACGT', 16391) || 'A')::bioseq.seq('dna4', 64));",
        table);
    const auto rowno = only_column_row_number(ctx, table);

    struct failure_probe {
        std::string_view backend;
        std::string_view sqlstate;
        std::string_view diagnostic;
    };
    constexpr std::array probes{
        failure_probe{"scalar_fail_scanner_interrupt_before", "XX000",
            "injected sequence scanner PostgreSQL leaf failure\nDETAIL: failure point 1"},
        failure_probe{"scalar_fail_scanner_interrupt_after", "XX000",
            "injected sequence scanner PostgreSQL leaf failure\nDETAIL: failure point 2"},
        failure_probe{"scalar_fail_scanner_row_read_before", "XX000",
            "injected sequence scanner PostgreSQL leaf failure\nDETAIL: failure point 3"},
        failure_probe{"scalar_fail_scanner_row_read_after", "XX000",
            "injected sequence scanner PostgreSQL leaf failure\nDETAIL: failure point 4"},
        failure_probe{"scalar_fail_scanner_palloc_before", "XX000",
            "injected sequence scanner PostgreSQL leaf failure\nDETAIL: failure point 5"},
        failure_probe{"scalar_fail_scanner_palloc_after", "XX000",
            "injected sequence scanner PostgreSQL leaf failure\nDETAIL: failure point 6"},
        failure_probe{"scalar_fail_sorted_create_before", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 1"},
        failure_probe{"scalar_fail_sorted_create_after", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 2"},
        // The fixture's 65,565 bp row spans three 32-Kbp scanner cores. Its
        // test-only 64-KiB merge-reader budget therefore compacts three input
        // runs into two outputs. Failing output creation #2 confirms output #1
        // completed and its input handles closed before the run store became
        // permanently poisoned; the loop below then confirms fresh-txn recovery.
        failure_probe{"scalar_fail_sorted_compact_create_before_nth2", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 15"},
        failure_probe{"scalar_fail_sorted_write_before", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 3"},
        failure_probe{"scalar_fail_sorted_write_after", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 4"},
        failure_probe{"scalar_fail_sorted_read_before", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 5"},
        failure_probe{"scalar_fail_sorted_read_after", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 6"},
        failure_probe{"scalar_fail_sorted_seek_before", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 7"},
        failure_probe{"scalar_fail_sorted_seek_after", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 8"},
        failure_probe{"scalar_fail_sorted_size_before", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 9"},
        failure_probe{"scalar_fail_sorted_size_after", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 10"},
        failure_probe{"scalar_fail_sorted_close_before", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 11"},
        failure_probe{"scalar_fail_sorted_close_after", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 12"},
        failure_probe{"scalar_fail_sorted_interrupt_before", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 13"},
        failure_probe{"scalar_fail_sorted_interrupt_after", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 14"},
        failure_probe{"scalar_fail_pg_spill", "58030",
            "injected PostgreSQL generation failure\nDETAIL: failure_point=minimizer_sequence_spill\nHINT: retry the failed index build in a fresh transaction"},
        failure_probe{"scalar_fail_pg_merge", "58030",
            "injected PostgreSQL generation failure\nDETAIL: failure_point=minimizer_sequence_merge_callback\nHINT: retry the failed index build in a fresh transaction"},
        failure_probe{"scalar_fail_pg_tts_create", "58030",
            "injected PostgreSQL generation failure\nDETAIL: failure_point=generation_tts_create\nHINT: retry the failed index build in a fresh transaction"},
        failure_probe{"scalar_fail_pg_tuple_form", "58030",
            "injected PostgreSQL generation failure\nDETAIL: failure_point=generation_tuple_form\nHINT: retry the failed index build in a fresh transaction"},
        failure_probe{"scalar_fail_pg_tts_store", "58030",
            "injected PostgreSQL generation failure\nDETAIL: failure_point=generation_tts_store\nHINT: retry the failed index build in a fresh transaction"},
        failure_probe{"scalar_fail_pg_tts_drop", "58030",
            "injected PostgreSQL generation failure\nDETAIL: failure_point=generation_tts_drop\nHINT: retry the failed index build in a fresh transaction"},
        failure_probe{"scalar_fail_pg_multi_insert", "58030",
            "injected PostgreSQL generation failure\nDETAIL: failure_point=generation_multi_insert\nHINT: retry the failed index build in a fresh transaction"},
        failure_probe{"scalar_fail_pg_reread", "58030",
            "injected PostgreSQL generation failure\nDETAIL: failure_point=generation_reread\nHINT: retry the failed index build in a fresh transaction"},
        failure_probe{"scalar_fail_pg_spi_publish", "58030",
            "injected PostgreSQL generation failure\nDETAIL: failure_point=generation_spi_publish\nHINT: retry the failed index build in a fresh transaction"},
        failure_probe{"scalar_fail_pg_writer_close", "58030",
            "injected PostgreSQL generation failure\nDETAIL: failure_point=generation_writer_close\nHINT: retry the failed index build in a fresh transaction"},
        failure_probe{"scalar_fail_pg_storage_close", "58030",
            "injected PostgreSQL generation failure\nDETAIL: failure_point=generation_storage_close\nHINT: retry the failed index build in a fresh transaction"},
        failure_probe{"scalar_fail_pg_post_publish_reread", "58030",
            "injected PostgreSQL generation failure\nDETAIL: failure_point=generation_post_publish_reread\nHINT: retry the failed index build in a fresh transaction"},
        failure_probe{"scalar_fail_pg_reader_close", "58030",
            "injected PostgreSQL generation failure\nDETAIL: failure_point=generation_reader_close\nHINT: retry the failed index build in a fresh transaction"},
    };

    const auto build = [&ctx, rowno](std::string_view backend) {
        return query_one(ctx, fmt::format(
            "SELECT * FROM "
            "bioseq_internal.build_ready_minimizer_sequence_generation("
            "'index_admin_sequence_minimizer_pg_recovery'::regclass, "
            "'minimizers_per_sequence', {}, '{}');",
            rowno, backend));
    };
    const auto capture_generation_family = [&ctx, rowno](
            const std::string& sidecar) {
        return query_rows(ctx, fmt::format(
            "SELECT generation_id::text, position_segment::text, "
            "sub_key::text, chunk_no::text, chunk_bytes::text, "
            "checksum::text, encode(blob, 'hex') "
            "FROM bioseq_internal.{} WHERE query_key={} "
            "ORDER BY generation_id, position_segment, sub_key, chunk_no;",
            sidecar, rowno));
    };

    row_t baseline_build = build("scalar");
    REQUIRE(baseline_build.size() == 72);
    REQUIRE(baseline_build[6].value() == "scalar");
    REQUIRE(baseline_build[8].value() == "t");
    REQUIRE(baseline_build[9].value() == "t");
    REQUIRE(baseline_build[41].value() == "t");
    REQUIRE(baseline_build[71].value() == "t");
    std::string baseline_sidecar = baseline_build[0].value();
    long long baseline_generation = std::stoll(
        baseline_build[10].value());
    REQUIRE(baseline_generation > 0);
    rowset_t baseline_family = capture_generation_family(baseline_sidecar);
    REQUIRE(!baseline_family.empty());

    for (const failure_probe& probe : probes) {
        const auto error = sql_error(ctx, fmt::format(
            "SELECT * FROM "
            "bioseq_internal.build_ready_minimizer_sequence_generation("
            "'{}'::regclass, 'minimizers_per_sequence', {}, '{}');",
            table, rowno, probe.backend));
        REQUIRE(error.sqlstate == probe.sqlstate);
        REQUIRE(error.message == probe.diagnostic);
        // Recovery is deliberately a fresh implicit transaction. A poisoned
        // scanner, run store, row sink, rereader, or SPI publisher must never
        // be resumed in-place. Exact equality includes the generation-0 head,
        // all immutable generation rows, and every blob byte; it therefore
        // also confirms the failed statement left no orphan generation rows.
        REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
        REQUIRE(capture_generation_family(baseline_sidecar)
            == baseline_family);
        REQUIRE(query_scalar_int(ctx, fmt::format(
            "SELECT count(*) FROM bioseq_internal.{} "
            "WHERE query_key={} AND generation_id=0;",
            baseline_sidecar, rowno)) == 1);

        // A successful publication after every individual probe confirms the
        // test-only hook was explicitly disarmed and each failure is isolated.
        row_t recovered = build("scalar");
        REQUIRE(recovered.size() == 72);
        REQUIRE(recovered[0].value() == baseline_sidecar);
        REQUIRE(recovered[6].value() == "scalar");
        REQUIRE(recovered[8].value() == "t");
        REQUIRE(recovered[9].value() == "t");
        REQUIRE(recovered[41].value() == "t");
        REQUIRE(recovered[71].value() == "t");
        const long long recovered_generation = std::stoll(
            recovered[10].value());
        REQUIRE(recovered_generation > baseline_generation);
        baseline_generation = recovered_generation;
        baseline_family = capture_generation_family(baseline_sidecar);
        REQUIRE(!baseline_family.empty());
        REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
    }
}

TEST_CASE("ordinary update_index publishes fragment-minimizer metadata and a reopenable hash-major generation",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    constexpr std::array<std::pair<std::string_view, std::string_view>, 2>
        cases{{
            {"index_admin_update_fragment_minimizer", "minimizers_per_fragment"},
            {"index_admin_update_sequence_minimizer", "minimizers_per_sequence"},
        }};
    auto ctx = fresh_context(
        {cases[0].first, cases[1].first});
    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
    exec_sql(ctx,
        "SET bioseq.aligner.grouped_soa_generation.enabled = on;");
    exec_sql(ctx, "SET bioseq.index.async_persistence.enabled = off;");
    exec_sql(ctx, "SET bioseq.index.recompute_async_persistence.enabled = off;");
    exec_sql(ctx,
        "SET bioseq.aligner.minimizer.algorithm = 'scalar';");

    for (const auto& [table, index_name] : cases) {
        INFO(fmt::format("table={} index={}", table, index_name));
        exec_sqlf(ctx,
            "CREATE TABLE {} "
            "(id int, seq bioseq.seq('dna4', 64)) USING bioseq;",
            table);
        exec_sqlf(ctx,
            "INSERT INTO {} VALUES "
            "(1, (repeat('ACGT', 16391) || 'A')::"
            "bioseq.seq('dna4', 64));",
            table);
        const auto rowno = only_column_row_number(ctx, table);

        REQUIRE(query_scalar_int(ctx, fmt::format(
            "SELECT bioseq.update_index('{}'::regclass, '{}');",
            table, index_name)) == 1);

        // A distinct backend authenticates the published metadata without
        // inheriting writer memory. Fragment-minimizer live mmap execution is
        // covered above; only the sequence-wide hash-major branch below
        // claims a complete hash-major reopen.
        connection_test_context reopened;
        if (index_name == "minimizers_per_fragment") {
            REQUIRE(require_ready_fragment_minimizer_metadata(
                reopened, table, index_name, rowno, 65'565,
                /*expected_position_segment_span_bp=*/32'768,
                /*expected_position_segment_count=*/3) > 0);
        } else {
            REQUIRE(require_ready_hash_major_generation(
                reopened, table, index_name, rowno, 65'565,
                /*expected_scheme_identity=*/0,
                /*expected_scheme_revision=*/0,
                /*expected_producer_identity=*/1) > 0);
        }
        REQUIRE(index_table_row_count(reopened, index_name, table) > 1);
    }
}

TEST_CASE("hash-major core parallel heap scan matches the serial receipt",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    constexpr std::string_view table =
        "hash_major_parallel_scan_probe";
    auto ctx = fresh_context({table});
    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
    exec_sql(ctx,
        "SET bioseq.aligner.grouped_soa_generation.enabled = on;");
    exec_sql(ctx, "SET bioseq.index.async_persistence.enabled = off;");
    exec_sql(ctx, "SET bioseq.index.recompute_async_persistence.enabled = off;");
    exec_sql(ctx,
        "SET bioseq.aligner.minimizer.algorithm = 'scalar';");
    exec_sqlf(ctx,
        "CREATE TABLE {} "
        "(id int, seq bioseq.seq('dna4', 64)) USING bioseq;",
        table);
    exec_sqlf(ctx,
        "INSERT INTO {} VALUES "
        "(1, (repeat('ACGT', 16391) || 'A')::"
        "bioseq.seq('dna4', 64));",
        table);
    const auto rowno = only_column_row_number(ctx, table);
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT bioseq.update_index('{}'::regclass, "
        "'minimizers_per_sequence');",
        table)) == 1);
    REQUIRE(require_ready_hash_major_generation(
        ctx, table, "minimizers_per_sequence", rowno, 65'565,
        /*expected_scheme_identity=*/0,
        /*expected_scheme_revision=*/0,
        /*expected_producer_identity=*/1) > 0);

    const long long max_parallel_workers = query_scalar_int(
        ctx,
        "SELECT current_setting('max_parallel_workers')::int;");
    REQUIRE(max_parallel_workers > 0);
    const long long requested_workers =
        std::min<long long>(2, max_parallel_workers);
    const row_t receipt = query_one(ctx, fmt::format(
        "WITH serial AS MATERIALIZED ("
        " SELECT bioseq_internal.hash_major_minimizer_parallel_scan_probe("
        "  '{}'::regclass,'minimizers_per_sequence',{},0) AS j"
        "), parallel AS MATERIALIZED ("
        " SELECT bioseq_internal.hash_major_minimizer_parallel_scan_probe("
        "  '{}'::regclass,'minimizers_per_sequence',{}, {}) AS j"
        ") "
        "SELECT "
        " serial.j->>'schema',"
        " serial.j->>'requested_workers',"
        " parallel.j->>'requested_workers',"
        " parallel.j->>'launched_workers',"
        " (serial.j->>'generation_id'=parallel.j->>'generation_id' "
        "  AND serial.j->>'query_key'=parallel.j->>'query_key' "
        "  AND serial.j->>'expected_rows'=parallel.j->>'expected_rows' "
        "  AND serial.j->>'expected_bytes'=parallel.j->>'expected_bytes' "
        "  AND serial.j->>'scanned_rows'=serial.j->>'expected_rows' "
        "  AND parallel.j->>'scanned_rows'=parallel.j->>'expected_rows' "
        "  AND serial.j->>'scanned_bytes'=serial.j->>'expected_bytes' "
        "  AND parallel.j->>'scanned_bytes'=parallel.j->>'expected_bytes'"
        " )::text,"
        " (serial.j->>'duplicate_rows'='0' "
        "  AND serial.j->>'missing_rows'='0' "
        "  AND serial.j->>'corrupt_rows'='0')::text,"
        " (parallel.j->>'duplicate_rows'='0' "
        "  AND parallel.j->>'missing_rows'='0' "
        "  AND parallel.j->>'corrupt_rows'='0')::text,"
        " (serial.j->>'checksum_digest'="
        "      parallel.j->>'checksum_digest' "
        "  AND serial.j->>'receipt_digest'="
        "      parallel.j->>'receipt_digest')::text,"
        " ((serial.j->>'has_leader_participation')::boolean "
        "  AND (parallel.j->>'has_leader_participation')::boolean)::text,"
        " ((serial.j->>'elapsed_ns')::bigint>0 "
        "  AND (parallel.j->>'elapsed_ns')::bigint>0)::text,"
        " ((serial.j->>'expected_rows')::bigint>0)::text,"
        " ((serial.j->>'expected_bytes')::bigint>0)::text "
        "FROM serial CROSS JOIN parallel;",
        table, rowno, table, rowno, requested_workers));
    REQUIRE(receipt.size() == 12u);
    for (const auto& value : receipt) REQUIRE(value.has_value());
    REQUIRE(receipt[0].value()
        == "bioseq_internal.hash_major_minimizer_parallel_scan_probe");
    REQUIRE(receipt[1].value() == "0");
    REQUIRE(std::stoll(receipt[2].value()) == requested_workers);
    REQUIRE(std::stoll(receipt[3].value()) >= 1);
    for (std::size_t index = 4; index < receipt.size(); ++index) {
        REQUIRE(receipt[index].value() == "true");
    }
}

TEST_CASE("hash-major replacement reuses its sidecar and retains the replaced generation",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    constexpr std::string_view table = "hash_major_generation_replacement";
    auto ctx = fresh_context({table});
    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
    exec_sql(ctx, "SET bioseq.index.async_persistence.enabled = off;");
    exec_sql(ctx, "SET bioseq.index.recompute_async_persistence.enabled = off;");
    exec_sql(ctx,
        "SET bioseq.aligner.grouped_soa_generation.enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.minimizer.algorithm = 'scalar';");
    exec_sqlf(ctx,
        "CREATE TABLE {} (id int, seq bioseq.seq('dna4', 64)) USING bioseq;",
        table);
    exec_sqlf(ctx,
        "INSERT INTO {} VALUES "
        "(1, (repeat('ACGT', 16391) || 'A')::bioseq.seq('dna4', 64));",
        table);
    const auto rowno = only_column_row_number(ctx, table);

    const auto rebuild = [&ctx, table, rowno] {
        REQUIRE(query_scalar_int(ctx, fmt::format(
            "SELECT bioseq.update_index('{}'::regclass, "
            "'minimizers_per_sequence');",
            table)) == 1);
        return require_ready_hash_major_generation(
            ctx, table, "minimizers_per_sequence", rowno, 65'565,
            /*expected_scheme_identity=*/0,
            /*expected_scheme_revision=*/0,
            /*expected_producer_identity=*/1);
    };
    const auto first_generation = rebuild();
    REQUIRE(first_generation > 0);

    const row_t sidecar_identity = query_one(ctx,
        "SELECT c.relname,c.oid::text,c.relfilenode::text,"
        "i.indexrelid::text FROM pg_class c "
        "JOIN pg_namespace n ON n.oid=c.relnamespace "
        "JOIN pg_index i ON i.indrelid=c.oid AND i.indisprimary "
        "WHERE n.nspname='bioseq_internal' AND c.relkind='r' "
        "AND c.relname LIKE 'seqidx_mps_%' "
        "AND EXISTS (SELECT 1 FROM pg_attribute a "
        "WHERE a.attrelid=c.oid AND a.attname='generation_id' "
        "AND a.attnum>0 AND NOT a.attisdropped) "
        "ORDER BY c.oid DESC LIMIT 1;");
    REQUIRE(sidecar_identity.size() == 4u);
    for (const auto& value : sidecar_identity) REQUIRE(value.has_value());
    const std::string sidecar = sidecar_identity[0].value();

    const auto second_generation = rebuild();
    REQUIRE(second_generation > first_generation);
    const row_t identity_after = query_one(ctx, fmt::format(
        "SELECT c.oid::text,c.relfilenode::text,i.indexrelid::text "
        "FROM pg_class c JOIN pg_index i "
        "ON i.indrelid=c.oid AND i.indisprimary "
        "WHERE c.oid='bioseq_internal.{}'::regclass;",
        sidecar));
    REQUIRE(identity_after.size() == 3u);
    REQUIRE(identity_after[0] == sidecar_identity[1]);
    REQUIRE(identity_after[1] == sidecar_identity[2]);
    REQUIRE(identity_after[2] == sidecar_identity[3]);

    const row_t generations = query_one(ctx, fmt::format(
        "SELECT count(DISTINCT generation_id)::text,"
        "count(*) FILTER (WHERE generation_id={})::text,"
        "count(*) FILTER (WHERE generation_id={})::text,"
        "count(*) FILTER (WHERE generation_id=0)::text "
        "FROM bioseq_internal.{};",
        first_generation, second_generation, sidecar));
    REQUIRE(generations.size() == 4u);
    REQUIRE(generations[0].value() == "3");
    REQUIRE(std::stoll(generations[1].value()) > 0);
    REQUIRE(std::stoll(generations[2].value()) > 0);
    REQUIRE(generations[3].value() == "1");
}

TEST_CASE("ordinary locator-backed bounded ntHash update publishes hash-major without fallback",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    constexpr std::string_view table = "index_admin_bounded_nthash_publication";
    auto ctx = fresh_context({table});
    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
    exec_sql(ctx,
        "SET bioseq.aligner.grouped_soa_generation.enabled = on;");
    exec_sql(ctx, "SET bioseq.index.async_persistence.enabled = off;");
    exec_sql(ctx, "SET bioseq.index.recompute_async_persistence.enabled = off;");
    exec_sql(ctx,
        "SET bioseq.aligner.minimizer.algorithm = 'bounded_nthash_simd';");
    exec_sqlf(ctx,
        "CREATE TABLE {} "
        "(id int, seq bioseq.seq('dna4', 64)) USING bioseq;",
        table);
    exec_sqlf(ctx,
        "INSERT INTO {} VALUES "
        "(1, (repeat('ACGT', 16391) || 'A')::"
        "bioseq.seq('dna4', 64));",
        table);
    const auto rowno = only_column_row_number(ctx, table);

    // The production coordinator rejects the publication unless its returned
    // evidence carries accepted bounded-ntHash-native work and zero Scalar/Exact/Vulkan
    // runs, so SQL success is itself no-fallback route evidence.
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT bioseq.update_index('{}'::regclass, "
        "'minimizers_per_sequence');",
        table)) == 1);
    REQUIRE(require_ready_hash_major_generation(
        ctx, table, "minimizers_per_sequence", rowno, 65'565,
        /*expected_scheme_identity=*/1,
        /*expected_scheme_revision=*/1,
        /*expected_producer_identity=*/3) > 0);
    exec_sql(ctx, "SET bioseq.aligner.minimizer.algorithm = 'scalar';");
    const auto wrong_backend = sql_error(ctx, fmt::format(
        "SELECT bioseq.inspect_ready_hash_major_minimizer_generation("
        "'{}'::regclass,'minimizers_per_sequence',{});",
        table, rowno));
    REQUIRE(wrong_backend.contains(
        "bioseq hash-major inspection source/profile identity disagrees"));
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
    exec_sql(ctx,
        "SET bioseq.aligner.minimizer.algorithm = 'bounded_nthash_simd';");
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT count(bioseq.inspect_ready_hash_major_minimizer_generation("
        "'{}'::regclass,'minimizers_per_sequence',{}));",
        table, rowno)) == 1);
}

TEST_CASE("ordinary hash-major replacement preserves its active generation when a locator source is corrupt",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    constexpr std::string_view table = "index_admin_update_source_rollback";
    auto ctx = fresh_context({table});
    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
    exec_sql(ctx,
        "SET bioseq.aligner.grouped_soa_generation.enabled = on;");
    exec_sql(ctx, "SET bioseq.index.async_persistence.enabled = off;");
    exec_sql(ctx, "SET bioseq.index.recompute_async_persistence.enabled = off;");
    exec_sqlf(ctx,
        "CREATE TABLE {} "
        "(id int, seq bioseq.seq('dna4', 64)) USING bioseq;",
        table);
    exec_sqlf(ctx,
        "INSERT INTO {} VALUES "
        "(1, (repeat('ACGT', 16391) || 'A')::"
        "bioseq.seq('dna4', 64));",
        table);
    const auto rowno = only_column_row_number(ctx, table);
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT bioseq.update_index('{}'::regclass, "
        "'minimizers_per_sequence');",
        table)) == 1);
    REQUIRE(require_ready_hash_major_generation(
        ctx, table, "minimizers_per_sequence", rowno, 65'565,
        /*expected_scheme_identity=*/0,
        /*expected_scheme_revision=*/0,
        /*expected_producer_identity=*/1) > 0);
    const rowset_t sidecars = query_rows(ctx,
        "SELECT c.relname, c.oid::text FROM pg_class c "
        "JOIN pg_namespace n ON n.oid=c.relnamespace "
        "WHERE n.nspname='bioseq_internal' AND c.relkind='r' "
        "AND c.relname LIKE 'seqidx_mps_%' "
        "AND EXISTS (SELECT 1 FROM pg_attribute a "
        "WHERE a.attrelid=c.oid AND a.attname='generation_id' "
        "AND a.attnum>0 AND NOT a.attisdropped) "
        "ORDER BY c.relname;");
    REQUIRE(sidecars.size() == 1);
    const std::string sidecar = sidecars[0][0].value();
    const auto capture_generation_rows = [&ctx, &sidecar] {
        return query_rows(ctx, fmt::format(
            "SELECT query_key::text,generation_id::text,"
            "position_segment::text,sub_key::text,chunk_no::text,"
            "chunk_bytes::text,checksum::text,encode(blob,'hex') "
            "FROM bioseq_internal.{} ORDER BY query_key,generation_id,"
            "position_segment,sub_key,chunk_no;",
            sidecar));
    };
    const auto capture_schema_identity = [&ctx, &sidecar] {
        return query_one(ctx, fmt::format(
            "SELECT c.oid::text,c.relfilenode::text,"
            "(SELECT i.indexrelid::text FROM pg_index i "
            "WHERE i.indrelid=c.oid AND i.indisprimary),"
            "string_agg(a.attname || ':' || format_type(a.atttypid,a.atttypmod) "
            "|| ':' || a.attnotnull::int || ':' || a.attstorage::text,',' "
            "ORDER BY a.attnum) FROM pg_class c JOIN pg_attribute a "
            "ON a.attrelid=c.oid WHERE c.oid='bioseq_internal.{}'::regclass "
            "AND a.attnum>0 AND NOT a.attisdropped "
            "GROUP BY c.oid,c.relfilenode;",
            sidecar));
    };
    const auto capture_metadata_age = [&ctx, table] {
        return query_rows(ctx, fmt::format(
            "SELECT user_relid::text,main_relfilenode::text,"
            "column_attno::text,index_name,last_updated::text "
            "FROM bioseq_internal.metadata_age_log "
            "WHERE user_relid='{}'::regclass "
            "ORDER BY main_relfilenode,column_attno,index_name;",
            table));
    };
    const rowset_t generation_rows_before = capture_generation_rows();
    const row_t schema_identity_before = capture_schema_identity();
    const rowset_t metadata_age_before = capture_metadata_age();
    REQUIRE(generation_rows_before.size() > 1);
    for (const row_t& row : generation_rows_before) {
        REQUIRE(row.size() == 8);
        for (const auto& field : row) {
            REQUIRE(field.has_value());
        }
    }
    REQUIRE(schema_identity_before.size() == 4);
    REQUIRE(schema_identity_before[0].has_value());
    REQUIRE(schema_identity_before[1].has_value());
    REQUIRE(schema_identity_before[2].has_value());
    REQUIRE(schema_identity_before[3].has_value());
    REQUIRE(!metadata_age_before.empty());

    // Corrupt a later source fragment after a valid generation exists. The
    // next ordinary rebuild must fail while validating and streaming the authoritative
    // locator, before publishing a replacement head. The stable sidecar and
    // published generation must remain byte-exact.
    exec_sql(ctx, fmt::format(
        "SELECT bioseq_internal.remove_sequence_fragment("
        "'{}'::regclass, 'minimizers_per_sequence', {}, 2);",
        table, rowno));

    const auto error = sql_error(ctx, fmt::format(
        "SELECT bioseq.update_index('{}'::regclass, "
        "'minimizers_per_sequence');",
        table));
    // The immutable-source guard owns this failure before the replacement
    // publisher can enter a generation-catalog operation.
    REQUIRE(error.sqlstate == "55000");
    REQUIRE(error.contains(
        "bioseq locator no longer matches its immutable source"));
    REQUIRE(error.contains("source epoch"));
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
    REQUIRE(capture_generation_rows() == generation_rows_before);
    REQUIRE(capture_schema_identity() == schema_identity_before);
    REQUIRE(capture_metadata_age() == metadata_age_before);
}

#ifdef BIOSEQ_TEST_INDEXES
TEST_CASE("PG index streaming poisons contiguous access and resident "
          "minimizers_per_sequence admits once",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
  constexpr std::string_view source_table = "index_admin_index_poison_source";
  constexpr std::string_view target_table = "index_admin_index_poison_target";
  auto ctx = fresh_context({source_table, target_table});
  exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled=off;"
                "SET bioseq.index.recompute_async_persistence.enabled=off;"
                "SET bioseq.aligner.minimizer.algorithm='scalar';"
                "CREATE TABLE index_admin_index_poison_source ("
                " id int, value bioseq.seq('dna4',64)) USING bioseq;"
                "INSERT INTO index_admin_index_poison_source VALUES ("
                " 1, repeat('ACGT',32775)::bioseq.seq('dna4',64));"
                "CREATE TABLE index_admin_index_poison_target ("
                " id int, value bioseq.seq('dna4',64)) USING bioseq;"
                "INSERT INTO index_admin_index_poison_target "
                "SELECT id, value[3:32771] OPERATOR(bioseq.||) "
                "       bioseq.reverse(value[32771:65539]) "
                "FROM index_admin_index_poison_source;");

  for (const std::string_view index_name : std::array<std::string_view, 2>{
           "minimizers_per_fragment", "symbol_counts"}) {
    exec_sql(ctx, "SELECT bioseq_internal.reset_sequence_source_counters();"
                  "SELECT bioseq_internal.arm_sequence_source_failure("
                  " 'facade_contiguous_access');");
    REQUIRE(query_scalar_int(
                ctx,
                fmt::format("SELECT bioseq.update_index('{}'::regclass, '{}');",
                            target_table, index_name)) == 1);
    exec_sql(ctx, "SELECT bioseq_internal.disarm_sequence_source_failure();");
    INFO("streaming index=" << index_name);
    REQUIRE(facade_source_counter(ctx, "facade_source_stream_callbacks") > 0);
    REQUIRE(facade_source_counter(ctx, "facade_source_resident_probes") == 0);
    REQUIRE(facade_source_counter(ctx, "facade_declared_materializations") ==
            0);
    REQUIRE(facade_source_counter(ctx, "facade_completed_materializations") ==
            0);
    REQUIRE(facade_source_counter(ctx, "facade_poison_hits") == 0);
  }

  // max_concurrency is PGC_SIGHUP and cannot be changed by a session.  Use
  // the cluster's effective pool width; the exact resident-probe and
  // materialization counters below confirm that this branch was admitted.
  exec_sql(ctx,
           "SET bioseq.scheduler.pipeline.mode='task_parallel';"
           "SELECT bioseq_internal.reset_sequence_source_counters();");
  const long long sequence_minimizer_rows_before = index_table_row_count(
      ctx, "minimizers_per_sequence", target_table);
  const long long reserved_before = query_scalar_int(
      ctx, "SELECT index_memory_reserved_bytes "
           "FROM bioseq.scheduler_stats();");
  exec_sql(ctx, "SELECT bioseq_internal.arm_sequence_source_failure("
                " 'facade_materialization');");
  const auto materialization_error = sql_error(
      ctx, fmt::format("SELECT bioseq.update_index('{}'::regclass, "
                       " 'minimizers_per_sequence');",
                       target_table));
  exec_sql(ctx, "SELECT bioseq_internal.disarm_sequence_source_failure();");

  REQUIRE(materialization_error.sqlstate == "XX000");
  REQUIRE(materialization_error.contains(
      "poisoned PostgreSQL sequence facade materialization"));
  REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
  REQUIRE(index_table_row_count(
              ctx, "minimizers_per_sequence", target_table) ==
          sequence_minimizer_rows_before);
  REQUIRE(query_scalar_int(
              ctx, "SELECT index_memory_reserved_bytes "
                   "FROM bioseq.scheduler_stats();") == reserved_before);
  REQUIRE(facade_source_counter(ctx, "facade_source_resident_probes") == 1);
  REQUIRE(facade_source_counter(ctx, "facade_declared_materializations") ==
          1);
  REQUIRE(facade_source_counter(ctx, "facade_completed_materializations") ==
          0);
  REQUIRE(facade_source_counter(ctx, "facade_poison_hits") == 1);

  exec_sql(ctx, "SELECT bioseq_internal.reset_sequence_source_counters();");
  REQUIRE(query_scalar_int(
              ctx, fmt::format("SELECT bioseq.update_index('{}'::regclass, "
                               " 'minimizers_per_sequence');",
                               target_table)) == 1);
  REQUIRE(index_table_row_count(
              ctx, "minimizers_per_sequence", target_table) >
          sequence_minimizer_rows_before);
  REQUIRE(facade_source_counter(ctx, "facade_source_stream_callbacks") > 0);
  REQUIRE(facade_source_counter(ctx, "facade_source_resident_probes") == 1);
  REQUIRE(facade_source_counter(ctx, "facade_declared_materializations") == 1);
  REQUIRE(facade_source_counter(ctx, "facade_completed_materializations") == 1);
  REQUIRE(facade_source_counter(ctx, "facade_poison_hits") == 0);
}

TEST_CASE("checked bounded ntHash failures unwind grouped fragment and sequence minimizer publishers",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
  struct failure_route {
    std::string_view table;
    std::string_view index_name;
    std::string_view operation;
  };
  constexpr std::array<failure_route, 2> routes{{
      {"index_admin_bounded_nthash_forced_fragment", "minimizers_per_fragment",
       "minimizer fragment generation replay"},
      {"index_admin_bounded_nthash_forced_sequence", "minimizers_per_sequence",
       "minimizers_per_sequence build"},
  }};

  for (const failure_route& route : routes) {
    DYNAMIC_SECTION(route.index_name) {
      auto ctx = fresh_context({route.table});
      exec_sql(ctx,
               "SET bioseq.index.insert_maintenance.enabled=off;"
               "SET bioseq.index.async_persistence.enabled=off;"
               "SET bioseq.index.recompute_async_persistence.enabled=off;"
               "SET bioseq.aligner.grouped_soa_generation.enabled=on;"
               "SET bioseq.aligner.minimizer.algorithm='bounded_nthash_simd';");
      exec_sqlf(ctx,
                "CREATE TABLE {} "
                "(id int, seq bioseq.seq('dna4',64)) USING bioseq;"
                "INSERT INTO {} VALUES "
                "(1,(repeat('ACGT',16391)||'A')::"
                "bioseq.seq('dna4',64));",
                route.table, route.table);
      REQUIRE(only_column_row_number(ctx, route.table) > 0);
      const long long backend_pid =
          query_scalar_int(ctx, "SELECT pg_backend_pid();");
      const long long admission_before = query_scalar_int(
          ctx, "SELECT index_memory_reserved_bytes "
               "FROM bioseq.scheduler_stats();");

      exec_sql(ctx,
               "SET bioseq.testing.persisted_index.failure_mode="
               "'unsupported_vector_shape';");
      const auto rejected = sql_error(ctx, fmt::format(
          "SELECT bioseq.update_index('{}'::regclass,'{}');",
          route.table, route.index_name));
      exec_sql(ctx,
               "SET bioseq.testing.persisted_index.failure_mode='none';");

      REQUIRE(rejected.sqlstate == "0A000");
      REQUIRE(rejected.contains("status=unsupported_vector_shape"));
      REQUIRE(rejected.contains(route.operation));
      REQUIRE_FALSE(rejected.sqlstate == "XX000");
      REQUIRE_FALSE(rejected.sqlstate == "XX001");
      REQUIRE(query_scalar_int(ctx, "SELECT pg_backend_pid();") ==
              backend_pid);
      REQUIRE(index_table_row_count(ctx, route.index_name, route.table) == 0);
      REQUIRE(query_scalar_int(ctx, fmt::format(
          "SELECT count(*) FROM bioseq_internal.metadata_age_log "
          "WHERE user_relid='{}'::regclass AND index_name='{}';",
          route.table, route.index_name)) == 0);
      REQUIRE(query_scalar_int(ctx, fmt::format(
          "SELECT count(*) FROM "
          "bioseq_internal.minimizer_generation_storage "
          "WHERE user_relid='{}'::regclass AND index_name='{}';",
          route.table, route.index_name)) == 0);
      REQUIRE(query_scalar_int(
          ctx, "SELECT index_memory_reserved_bytes "
               "FROM bioseq.scheduler_stats();") == admission_before);
      REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);

      // The same bounded ntHash backend succeeds through the same publisher immediately,
      // confirming the one-shot failure arm, writer, spill, and source owner were
      // all cleared by the rejected statement.
      exec_sql(ctx,
               "SET bioseq.aligner.minimizer.algorithm='bounded_nthash_simd';");
      REQUIRE(query_scalar_int(ctx, fmt::format(
          "SELECT bioseq.update_index('{}'::regclass,'{}');",
          route.table, route.index_name)) == 1);
      REQUIRE(index_table_row_count(ctx, route.index_name, route.table) > 0);
      REQUIRE(query_scalar_int(
          ctx, "SELECT index_memory_reserved_bytes "
               "FROM bioseq.scheduler_stats();") == admission_before);
    }
  }
}
#endif
