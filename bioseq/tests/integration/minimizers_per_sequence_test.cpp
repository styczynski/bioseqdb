/**
 * @file
 * @brief Tests merging minimizer sort runs and reading the per-sequence index.
 */
#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.tests.integration.support;

using namespace bioseq_int_tests;

namespace {
/** Return the distinct row IDs and target bounds reported by mmsearch. */
inline std::set<std::tuple<long long, long long, long long>>
mmsearch_set(const connection_test_context& ctx, std::string_view table_name) {
    auto rows =
        query_rows(ctx,
                   fmt::format("SELECT id, bioseq.target_start(id), bioseq.target_end(id) FROM {} "
                               "WHERE mmsearch(target, query) ORDER BY id",
                               table_name));
    std::set<std::tuple<long long, long long, long long>> out;
    for (auto& r : rows)
        if (r[0] && r[1] && r[2])
            out.emplace(std::stoll(*r[0]), std::stoll(*r[1]), std::stoll(*r[2]));
    return out;
}
} // namespace

TEST_CASE("minimizers_per_sequence: multi-run merge + active-read routing",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({"minimizer_sequence_merge", "minimizer_sequence_pressure",
                              "minimizer_sequence_low_entropy"});
    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;"
                  "SET bioseq.index.async_persistence.enabled = off;"
                  "SET bioseq.index.recompute_async_persistence.enabled = off;");
    guc_scope ungrouped(ctx, "bioseq.aligner.grouped_soa_generation.enabled", "off");
    guc_scope exact(ctx, "bioseq.aligner.minimizer.algorithm", "two_stack_nthash");
    guc_scope kmer(ctx, "bioseq.aligner.minimizer.kmer_size", "15");
    guc_scope window(ctx, "bioseq.aligner.minimizer.window_size", "10");
    guc_scope cache_off(ctx, "bioseq.column_cache.enabled", "off");
    guc_scope roomy_cap(ctx, "bioseq.index.memory_cap_mb", "32");
    exec_sql(ctx, "SELECT bioseq.scheduler_stats();");
    exec_sql(ctx,
             "SELECT bioseq.create_query_target_sample_table('minimizer_sequence_merge', 3, 5000, "
             "100.0, 200000, "
             "0.001, 0.0005, 0.0005, 1.0, 1.0, 0.0, 271828);");

    // Use a small sort buffer so index creation has several runs to merge.
    exec_sql(ctx, "SET bioseq.index_build.run_buffer_bytes = 4096;");
    REQUIRE(query_scalar_int(ctx,
                             "SELECT bioseq.update_index('minimizer_sequence_merge'::regclass, "
                             "'minimizers_per_sequence')") == 3);
    REQUIRE(last_index_merge_run_count(ctx) >= 2); // at least two sorted runs were merged
    REQUIRE(query_scalar_int(ctx,
        "SELECT count(*) FROM bioseq.last_minimizer_sequence_build_stats() "
        "WHERE backend_pid=pg_backend_pid() AND resident_runs>=2 AND file_runs=0 "
        "AND resident_demotions=0") == 1);

    // Compute a baseline with stored sequence-index reads disabled.
    const auto recomputed = [&ctx] {
        guc_scope off(ctx, "bioseq.index.sequence_read.enabled", "off");
        return mmsearch_set(ctx, "minimizer_sequence_merge");
    }();
    REQUIRE(recomputed.size() == 3);

    // Search using the stored per-sequence index.
    exec_sql(ctx,
             "SELECT bioseq.set_active_read_index('minimizer_sequence_merge'::regclass, "
             "'minimizers_per_sequence');");
    guc_scope on(ctx, "bioseq.index.sequence_read.enabled", "on");
    const auto assembled = mmsearch_set(ctx, "minimizer_sequence_merge");
    REQUIRE(assembled.size() == 3); // three distinct result tuples

    // Exercise actual PG sorted-run ownership under a fixed cap, rather than
    // using the semantic unit fake as a memory-pressure oracle.
    exec_sql(ctx,
             "SELECT bioseq.create_query_target_sample_table('minimizer_sequence_pressure', 1, 5000, "
             "100.0, 1000000, 0.001, 0.0005, 0.0005, 1.0, 1.0, 0.0, 271829);");
    const std::string pressure_query =
        "SELECT id,bioseq.target_start(target),bioseq.target_end(target),"
        "bioseq.query_start(target),bioseq.query_end(target),bioseq.cigar(target)::text "
        "FROM minimizer_sequence_pressure WHERE mmsearch(target,query) "
        "ORDER BY id,2,3,4,5,6";
    const auto pressure_expected = [&ctx, &pressure_query] {
        guc_scope reads_off(ctx, "bioseq.index.sequence_read.enabled", "off");
        return query_rows(ctx, pressure_query);
    }();
    REQUIRE_FALSE(pressure_expected.empty());
    for (const auto& row : pressure_expected) {
        REQUIRE(row.size() == 6u);
        for (const auto& field : row) {
            REQUIRE(field.has_value());
        }
        REQUIRE_FALSE(row[5]->empty());
        REQUIRE(*row[5] != "*");
    }
    {
        guc_scope serial(ctx, "bioseq.scheduler.pipeline.mode", "single_thread");
        guc_scope cap(ctx, "bioseq.index.memory_cap_mb", "8");
        const auto reserved_before = query_scalar_int(ctx,
            "SELECT index_memory_reserved_bytes FROM bioseq.scheduler_stats()");
        REQUIRE(query_scalar_int(ctx,
            "SELECT bioseq.update_index('minimizer_sequence_pressure'::regclass,"
            "'minimizers_per_sequence')") == 1);
        REQUIRE(query_scalar_int(ctx,
            "SELECT count(*) FROM bioseq.last_minimizer_sequence_build_stats() "
            "WHERE backend_pid=pg_backend_pid() AND resident_demotions>0 "
            "AND db_oid=(SELECT oid::bigint FROM pg_database WHERE datname=current_database()) "
            "AND relfilenode=pg_relation_filenode('minimizer_sequence_pressure'::regclass)::bigint "
            "AND resident_peak_bytes<=8388608 AND file_runs>0 "
            "AND spilled_run_bytes>0 AND merge_reader_buffer_bytes>0") == 1);
        // Successful writes retain admitted index-cache copies after commit.
        // Invalidate only this source before checking temporary-owner cleanup.
        exec_sql(ctx,
            "ALTER TABLE minimizer_sequence_pressure RENAME TO minimizer_sequence_pressure_cache_reset;"
            "ALTER TABLE minimizer_sequence_pressure_cache_reset RENAME TO minimizer_sequence_pressure;");
        REQUIRE(query_scalar_int(ctx,
            "SELECT index_memory_reserved_bytes FROM bioseq.scheduler_stats()") == reserved_before);
    }
    // A few normal (sub-8MiB) hash groups must not pay for one bucket
    // record per emitted position. The per-core distinct bound remains small.
    exec_sql(ctx, "CREATE TABLE minimizer_sequence_low_entropy "
                  "(target bioseq.seq('dna4',64)) USING bioseq;"
                  "INSERT INTO minimizer_sequence_low_entropy "
                  "VALUES(repeat('ACGT',125000)::bioseq.seq('dna4',64));");
    {
        guc_scope serial(ctx, "bioseq.scheduler.pipeline.mode", "single_thread");
        guc_scope cap(ctx, "bioseq.index.memory_cap_mb", "8");
        const auto reserved_before = query_scalar_int(ctx,
            "SELECT index_memory_reserved_bytes FROM bioseq.scheduler_stats()");
        REQUIRE(query_scalar_int(ctx,
            "SELECT bioseq.update_index('minimizer_sequence_low_entropy'::regclass,"
            "'minimizers_per_sequence')") == 1);
        REQUIRE(last_index_merge_run_count(ctx) > 1);
        // End this source's output-cache residency without changing its data.
        exec_sql(ctx,
            "ALTER TABLE minimizer_sequence_low_entropy RENAME TO minimizer_sequence_low_entropy_cache_reset;"
            "ALTER TABLE minimizer_sequence_low_entropy_cache_reset RENAME TO minimizer_sequence_low_entropy;");
        REQUIRE(query_scalar_int(ctx,
            "SELECT index_memory_reserved_bytes FROM bioseq.scheduler_stats()") == reserved_before);
    }
    // Synchronize the restored cap before the independent reader comparison.
    exec_sql(ctx, "SELECT bioseq.scheduler_stats();"
                  "SELECT bioseq.set_active_read_index('minimizer_sequence_pressure'::regclass,"
                  "'minimizers_per_sequence');");
    REQUIRE(query_rows(ctx, pressure_query) == pressure_expected);

    // Every row matched by the baseline must also match  through the index.
    // This comparison checks row IDs only.
    for (const auto& m : recomputed) {
        const long long id = std::get<0>(m);
        bool found = false;
        for (const auto& a : assembled)
            if (std::get<0>(a) == id) {
                found = true;
                break;
            }
        REQUIRE(found);
    }
}
