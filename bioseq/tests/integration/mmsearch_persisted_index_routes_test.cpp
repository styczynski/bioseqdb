/** @file Persisted-index and routed BatchJoin mmsearch scenarios. */

#include <cctype>
#include <libpq-fe.h>

#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.tests.integration.support;
import bioseq.tests.integration.mmsearch_test_support;

using namespace bioseq_int_tests;
using namespace bioseq_int_tests::mmsearch_test_support;

TEST_CASE(
    "Ordinal-2 BatchJoin executes mapped minimizers_per_fragment and full-read "
    "minimizers_per_sequence routes",
    "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    struct route_case {
        std::string_view name;
        std::string query;
        bool empty = false;
    };
    const std::string target = batch_join_wide_target_sequence();
    /* A high-entropy exact interior subsequence is long enough to create multiple
     * real anchors and alignment work, while remaining sparse relative to the
     * 512-Kibp target. Runtime counters supply the route identity and all three
     * SIMD stages; sequence length alone is insufficient. */
    constexpr std::size_t selective_query_offset = 193u * 1024u + 17u;
    constexpr std::size_t selective_query_bp = 48u;
    REQUIRE(target.size()
        >= selective_query_offset + selective_query_bp);
    const std::array<route_case, 2> routes{{
        route_case{
            .name = "empty query union",
            .query = std::string(14u, 'A'),
            .empty = true,
        },
        route_case{
            .name = "long sparse high-entropy selective intersection",
            .query = target.substr(
                selective_query_offset, selective_query_bp),
            .empty = false,
        },
    }};

    for (const std::string_view index_name : {
            std::string_view{"minimizers_per_fragment"},
            std::string_view{"minimizers_per_sequence"}}) {
        DYNAMIC_SECTION(index_name) {
            auto ctx = fresh_context({k_batch_join_target});
            exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
            seed_batch_join_target(ctx, target);
            exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = on;");
            exec_sql(ctx, "SET bioseq.index.async_persistence.enabled = off;");
            exec_sql(ctx, "SET bioseq.index.recompute_async_persistence.enabled = off;");
            exec_sql(ctx, "SET bioseq.index.sequence_read.enabled = on;");
            /* The runtime capability gate alone is not a GPU request. */
            exec_sql(ctx, "SET bioseq.aligner.vulkan.enabled = on;");
            const auto rowno = static_cast<std::uint64_t>(query_scalar_int(
                ctx,
                fmt::format(
                    "SELECT bioseq.column_row_number('{}'::regclass, ctid) "
                    "FROM {} ORDER BY pk LIMIT 1;",
                    k_batch_join_target, k_batch_join_target)));
            const auto count_minimizer_fragment_grouped_soa_images = [&ctx] {
                const row_t data_directory = query_one(ctx, "SHOW data_directory;");
                REQUIRE(data_directory.size() == 1u);
                REQUIRE(data_directory[0].has_value());
                const std::filesystem::path image_directory =
                    std::filesystem::path{data_directory[0].value()}
                    / "bioseq_minimizer_fragment_images";
                std::error_code error;
                if (!std::filesystem::exists(image_directory, error)) {
                    REQUIRE_FALSE(error);
                    return std::size_t{0};
                }
                std::size_t count = 0;
                for (std::filesystem::directory_iterator iterator{
                         image_directory, error}, end;
                     !error && iterator != end;
                     iterator.increment(error)) {
                    if (iterator->is_regular_file(error)
                        && iterator->path().extension()
                            == ".minimizer_fragment_index") {
                        ++count;
                    }
                }
                REQUIRE_FALSE(error);
                return count;
            };
            const std::size_t minimizer_fragment_images_before =
                count_minimizer_fragment_grouped_soa_images();
            if (index_name == "minimizers_per_sequence") {
                exec_sqlf(ctx,
                    "SELECT bioseq.set_active_read_index('{}'::regclass, '{}');",
                    k_batch_join_target, index_name);
            }
            const row_t publication = query_one(ctx, fmt::format(
                "SELECT * FROM bioseq_internal.build_ready_{}_generation("
                "'{}'::regclass, '{}', {}, 'exact_simd');",
                index_name == "minimizers_per_fragment"
                    ? "minimizer_fragment" : "minimizer_sequence",
                k_batch_join_target, index_name, rowno));
            REQUIRE(publication.size() > 25u);
            /* The production fixture publishes the grouped-SoA image and current
             * batch contract. minimizers_per_fragment binds its exact immutable mapped image after
             * two PostgreSQL metadata scans; minimizers_per_sequence retains the
             * sequence-wide full PostgreSQL payload scan. */
            REQUIRE(publication[7].value_or("") == "5");
            REQUIRE(publication[19].value_or("") == "3");
            REQUIRE(publication[8].value_or("") == "t");
            REQUIRE(publication[9].value_or("") == "t");
            REQUIRE(std::stoull(publication[14].value()) == target.size());
            REQUIRE(std::stoull(publication[24].value()) > 10u);
            if (index_name == "minimizers_per_sequence") {
                /* minimizers_per_sequence cannot create the fragment-only image. */
                REQUIRE(count_minimizer_fragment_grouped_soa_images()
                    == minimizer_fragment_images_before);
            }

            for (const route_case& route : routes) {
                INFO(fmt::format(
                    "route={} query_bp={}", route.name, route.query.size()));
                exec_sqlf(ctx, "DROP TABLE IF EXISTS {};", k_batch_join_queries);
                load_batch_join_query(ctx, route.query);
                const rowset_t aligned = query_rows(
                    ctx,
                    batch_join_sql(
                        2, "q.id, bioseq.has_cigar(t.pk) AS has_cigar")
                        + ";");
                if (route.empty) {
                    REQUIRE(aligned.empty());
                } else {
                    REQUIRE(aligned.size() == 1u);
                    REQUIRE(aligned.front().size() == 2u);
                    REQUIRE(aligned.front()[0].value_or("") == "0");
                    REQUIRE(aligned.front()[1].value_or("") == "t");
                }
                const auto stage_stats = last_alignment_stats(ctx);
                REQUIRE(stage_stats.size() == 20u);
                const row_t route_diagnostics = query_one(ctx,
                    "SELECT requested_backend, actual_backend, "
                    "requested_target_index_source, actual_target_index_source, "
                    "persisted_generation_id::text, backend_fallbacks::text, "
                    "recomputed_target_index_rows::text, "
                    "persisted_hash_build_calls::text, "
                    "persisted_generation_read_sessions::text, "
                    "persisted_relation_opens::text, persisted_index_scans::text, "
                    "persisted_bulk_read_calls::text, "
                    "persisted_planned_request_groups::text, "
                    "persisted_executed_request_groups::text, "
                    "persisted_range_scan_calls::text, "
                    "cold_index_preparation->>'route_empty', "
                    "cold_index_preparation->>'route_selective', "
                    "cold_index_preparation->>'route_full', "
                    "cold_index_preparation->>'route_candidate_rows', "
                    "cold_index_preparation->>'route_full_rows', "
                    "cold_index_preparation->>'route_candidate_bytes', "
                    "cold_index_preparation->>'route_full_bytes', "
                    "cold_index_preparation->>'route_selective_cost', "
                    "cold_index_preparation->>'route_full_cost', "
                    "cold_index_preparation->>'payload_rows_requested', "
                    "cold_index_preparation->>'payload_bytes_requested', "
                    "NOT (cold_index_preparation->>'are_stats_saturated')::boolean, "
                    "cold_index_preparation->>'memory_reroutes', "
                    "executed_scalar_cpu_jobs::text, "
                    "executed_simd_cpu_jobs::text, "
                    "executed_vulkan_gpu_jobs::text, fallback_jobs::text "
                    "FROM bioseq.last_alignment_execution_stats() "
                    "WHERE stage='query_sketch';");
                REQUIRE(route_diagnostics.size() == 32u);
                for (const auto& field : route_diagnostics) REQUIRE(field.has_value());
                const auto number = [&route_diagnostics](std::size_t field) {
                    return std::stoll(route_diagnostics[field].value());
                };
                std::string diagnostic_dump;
                for (std::size_t field = 0; field < route_diagnostics.size(); ++field) {
                    if (!diagnostic_dump.empty()) diagnostic_dump += ' ';
                    diagnostic_dump += fmt::format(
                        "{}={}", field, route_diagnostics[field].value_or("<null>"));
                }
                INFO(diagnostic_dump);
                INFO(fmt::format(
                    "requested_backend={} actual_backend={} empty={} "
                    "route_bits={}/{}/{} query_simd={} chain_score_simd={} "
                    "chain_backtrack_simd={}",
                    route_diagnostics[0].value(), route_diagnostics[1].value(), route.empty,
                    route_diagnostics[15].value(), route_diagnostics[16].value(),
                    route_diagnostics[17].value(),
                    stats_i64(stage_stats[0], 27),
                    stats_i64(stage_stats[7], 27),
                    stats_i64(stage_stats[8], 27)));
                REQUIRE(route_diagnostics[0].value() == "simd_cpu");
                /* The 14-bp query has no complete k=15 minimizer and therefore
                 * executes no biological backend. Keep the requested SIMD intent,
                 * but do not manufacture a SIMD execution claim from configuration.
                     * The selective route still requires real SIMD activity. */
                REQUIRE(route_diagnostics[1].value()
                    == (route.empty ? "mixed_invalid" : "simd_cpu"));
                REQUIRE(route_diagnostics[2].value() == "persisted");
                REQUIRE(route_diagnostics[3].value() == "persisted");
                REQUIRE(number(4) > 0);
                for (const std::size_t zero : {5u, 6u, 7u, 27u}) {
                    REQUIRE(number(zero) == 0);
                }
                REQUIRE(number(8) == 1);
                REQUIRE(number(9) == 1);
                if (index_name == "minimizers_per_fragment") {
                    const row_t fragment_evidence = query_one(ctx,
                        "WITH first_read AS MATERIALIZED ("
                        " SELECT bioseq."
                        "last_alignment_minimizer_fragment_reader_stats() AS value"
                        "), second_read AS MATERIALIZED ("
                        " SELECT bioseq."
                        "last_alignment_minimizer_fragment_reader_stats() AS value"
                        ") SELECT "
                        "first_read.value->>'schema',"
                        "first_read.value->>'revision',"
                        "first_read.value->>'fragment_native_waves',"
                        "first_read.value->>'fragment_native_queries',"
                        "first_read.value->>'fragment_native_occurrence_fill_ns',"
                        "first_read.value->>'fragment_native_control_count_bytes',"
                        "first_read.value->>'fragment_native_pass2_position_bytes',"
                        "first_read.value->>'fragment_native_pass3_position_bytes',"
                        "first_read.value->>'fragment_native_anchors_constructed',"
                        "first_read.value->>'fragment_native_exact_final_bytes',"
                        "first_read.value->>'fragment_native_peak_exact_final_bytes',"
                        "first_read.value->>'fragment_native_global_sort_calls_invoked',"
                        "first_read.value->>'fragment_native_global_sort_calls_skipped',"
                        "first_read.value->>'fragment_native_candidate_anchor_bytes',"
                        "first_read.value->>'fragment_native_plan_ordinal_bytes',"
                        "first_read.value->>'fragment_native_compaction_bytes',"
                        "first_read.value->>'fragment_native_final_copy_bytes',"
                        "first_read.value->>'fragment_native_query_occurrences',"
                        "first_read.value->>'fragment_native_unique_query_plans',"
                        "first_read.value->>'fragment_native_packed_occurrence_waves',"
                        "first_read.value->>'fragment_native_wide_occurrence_waves',"
                        "first_read.value->>'are_fragment_native_stats_saturated',"
                        "(first_read.value = second_read.value)::text "
                        "FROM first_read CROSS JOIN second_read;");
                    REQUIRE(fragment_evidence.size() == 23u);
                    for (const auto& field : fragment_evidence) {
                        REQUIRE(field.has_value());
                    }
                    const auto fragment_number = [&fragment_evidence](
                            const std::size_t field) {
                        return std::stoull(fragment_evidence[field].value());
                    };
                    REQUIRE(fragment_evidence[0].value()
                        == "bioseq.grouped_soa_reader_memory_stats");
                        REQUIRE(fragment_number(1) == 6u);
                    REQUIRE(fragment_number(2) == 1u);
                    REQUIRE(fragment_number(3) == 1u);
                    REQUIRE(fragment_number(4) > 0u);
                    REQUIRE(fragment_number(6) == fragment_number(7));
                    REQUIRE(fragment_number(10) == fragment_number(9));
                    REQUIRE(fragment_number(11) == 0u);
                    REQUIRE(fragment_number(12) == 1u);
                    for (const std::size_t zero : {13u, 14u, 15u, 16u}) {
                        REQUIRE(fragment_number(zero) == 0u);
                    }
                    REQUIRE(fragment_number(17) >= fragment_number(18));
                    REQUIRE(fragment_number(19) + fragment_number(20)
                        == fragment_number(2));
                    REQUIRE(fragment_number(19) == fragment_number(2));
                    REQUIRE(fragment_number(20) == 0u);
                    REQUIRE(fragment_evidence[21].value() == "false");
                    REQUIRE(fragment_evidence[22].value() == "true");
                    if (route.empty) {
                        REQUIRE(fragment_number(5) == 0u);
                        REQUIRE(fragment_number(6) == 0u);
                        REQUIRE(fragment_number(8) == 0u);
                        REQUIRE(fragment_number(9) == 0u);
                        REQUIRE(fragment_number(17) == 0u);
                        REQUIRE(fragment_number(18) == 0u);
                    } else {
                        REQUIRE(fragment_number(5) > 0u);
                        REQUIRE(fragment_number(6) > 0u);
                        REQUIRE(fragment_number(8) > 0u);
                        REQUIRE(fragment_number(9) > 0u);
                        REQUIRE(fragment_number(17) > 0u);
                        REQUIRE(fragment_number(18) > 0u);
                    }

                }
                if (index_name == "minimizers_per_fragment") {
                    REQUIRE(number(10) == 2);
                    for (const std::size_t zero : {
                            11u, 12u, 13u, 14u, 15u, 16u, 17u, 18u,
                            20u, 22u, 24u, 25u}) {
                        REQUIRE(number(zero) == 0);
                    }
                    /* Policy discovery still publishes the authenticated full
                     * generation dimensions/cost.  No route is selected and no
                     * PostgreSQL payload row or byte is requested. */
                    REQUIRE(number(19) > 0);
                    REQUIRE(number(21) > 0);
                    REQUIRE(number(23) > number(19));
                } else {
                    REQUIRE(number(10) == 3);
                    REQUIRE(number(11) == 1);
                    REQUIRE(number(12) == 1);
                    REQUIRE(number(13) == 1);
                    REQUIRE(number(14) == 1);
                    REQUIRE(number(15) == 0);
                    REQUIRE(number(16) == 0);
                    REQUIRE(number(17) == 1);
                    REQUIRE(number(18) == 0);
                    REQUIRE(number(19) > 0);
                    REQUIRE(number(20) == 0);
                    REQUIRE(number(21) > 0);
                    REQUIRE(number(22) == 0);
                    REQUIRE(number(23) > number(19));
                    REQUIRE(number(24) == number(19));
                    REQUIRE(number(25) == number(21));
                }
                REQUIRE(route_diagnostics[26].value() == "t");

                    const row_t reader_memory = query_one(ctx,
                        "WITH reader_stats AS MATERIALIZED ("
                        " SELECT bioseq."
                        "last_alignment_minimizer_fragment_reader_stats() AS reader_stats_value"
                        ") SELECT "
                        "reader_stats_value->>'grouped_soa_reader_capacity_accounting_passes',"
                        "reader_stats_value->>'grouped_soa_reader_exact_position_image_checks',"
                        "reader_stats_value->>'grouped_soa_reader_exact_position_image_failures',"
                        "reader_stats_value->>'grouped_soa_reader_retained_layout_checks',"
                        "reader_stats_value->>'grouped_soa_reader_retained_layout_failures',"
                        "((reader_stats_value->>'grouped_soa_reader_owned_capacity_bytes')::bigint="
                        " (reader_stats_value->>'grouped_soa_reader_admitted_bytes')::bigint)::text,"
                        "(NOT (reader_stats_value->>'are_grouped_soa_reader_memory_stats_saturated')"
                        "::boolean)::text,"
                        "reader_stats_value->>'grouped_soa_reader_retained_position_count',"
                        "reader_stats_value->>'grouped_soa_reader_directory_capacity_bytes',"
                        "reader_stats_value->>'grouped_soa_reader_ordinal_route_capacity_bytes',"
                        "reader_stats_value->>'grouped_soa_reader_global_position_capacity_bytes' "
                        "FROM reader_stats;");
                REQUIRE(reader_memory.size() == 11u);
                for (const auto& field : reader_memory) {
                    REQUIRE(field.has_value());
                }
                REQUIRE(std::stoll(reader_memory[0].value()) > 0);
                REQUIRE(std::stoll(reader_memory[1].value()) > 0);
                REQUIRE(reader_memory[2].value() == "0");
                REQUIRE(std::stoll(reader_memory[3].value()) > 0);
                REQUIRE(reader_memory[4].value() == "0");
                REQUIRE(reader_memory[5].value() == "true");
                REQUIRE(reader_memory[6].value() == "true");
                REQUIRE(std::stoll(reader_memory[8].value()) > 0);
                if (index_name == "minimizers_per_fragment") {
                    REQUIRE(reader_memory[7].value() == "0");
                    REQUIRE(std::stoll(reader_memory[9].value()) > 0);
                    REQUIRE(reader_memory[10].value() == "0");
                } else if (!route.empty) {
                    REQUIRE(std::stoll(reader_memory[7].value()) > 0);
                    REQUIRE(std::stoll(reader_memory[10].value()) > 0);
                }

                if (route.empty) {
                    for (const std::size_t zero : {28u, 29u, 30u, 31u}) {
                        REQUIRE(number(zero) == 0);
                    }
                } else {
                    REQUIRE(number(28) == 0);
                    REQUIRE(number(29) > 0);
                    REQUIRE(number(30) == 0);
                    REQUIRE(number(31) == 0);

                    /* The aggregate identity requires algorithm-local attestations.
                     * The complete 20-row SRF records ntHash, chaining score, and
                     * backtrack all ran SIMD, and at least one Block Aligner
                     * extension performed nonempty SIMD work. */
                    REQUIRE(stage_stats.front()[16].value_or("") == "simd_cpu");
                    REQUIRE(stage_stats.front()[17].value_or("") == "simd_cpu");
                    REQUIRE(stage_stats.front()[18].value_or("") == "persisted");
                    REQUIRE(stage_stats.front()[19].value_or("") == "persisted");
                    REQUIRE(stats_i64(stage_stats.front(), 94) > 0);
                    REQUIRE(stats_i64(stage_stats.front(), 95) == 0);
                    REQUIRE(stats_i64(stage_stats.front(), 96) == 0);
                    REQUIRE(stats_i64(stage_stats.front(), 98) > 0);
                    REQUIRE(stats_i64(stage_stats.front(), 99) > 0);
                    REQUIRE(stats_i64(stage_stats.front(), 100) == 0);
                    REQUIRE(stats_i64(stage_stats.front(), 101) == 0);
                    for (const std::size_t stage_no : {
                            std::size_t{0}, std::size_t{7}, std::size_t{8}}) {
                        REQUIRE(stats_i64(stage_stats[stage_no], 24) == 1);
                        REQUIRE(stats_i64(stage_stats[stage_no], 25) == 0);
                        REQUIRE(stats_i64(stage_stats[stage_no], 26) == 0);
                        REQUIRE(stats_i64(stage_stats[stage_no], 27) == 1);
                        REQUIRE(stats_i64(stage_stats[stage_no], 28) == 0);
                        REQUIRE(stats_i64(stage_stats[stage_no], 29) == 0);
                    }
                    long long block_simd_executions = 0;
                    for (std::size_t stage_no = 17; stage_no <= 19; ++stage_no) {
                        REQUIRE(stats_i64(stage_stats[stage_no], 25) == 0);
                        REQUIRE(stats_i64(stage_stats[stage_no], 26) == 0);
                        REQUIRE(stats_i64(stage_stats[stage_no], 27)
                            == stats_i64(stage_stats[stage_no], 24));
                        REQUIRE(stats_i64(stage_stats[stage_no], 28) == 0);
                        REQUIRE(stats_i64(stage_stats[stage_no], 29) == 0);
                        block_simd_executions +=
                            stats_i64(stage_stats[stage_no], 27);
                    }
                    INFO(fmt::format(
                        "Block Aligner SIMD extension executions={}",
                        block_simd_executions));
                    REQUIRE(block_simd_executions >= 1);
                }
            }
        }
    }
}

TEST_CASE(
    "Relation-backed minimizers_per_fragment direct mapping is directory-free and "
    "descriptor corruption fails closed",
    "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    constexpr std::size_t target_bp = 64u * 1024u;
    constexpr std::size_t query_offset = 19u * 1024u + 17u;
    constexpr std::size_t query_bp = 96u;
    const std::string target =
        batch_join_wide_target_sequence().substr(0u, target_bp);
    REQUIRE(target.size() >= query_offset + query_bp);

    auto ctx = fresh_context({k_batch_join_target});
    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled=off;");
    seed_batch_join_target(ctx, target);
    exec_sql(ctx,
        "SET bioseq.index.async_persistence.enabled=off; "
        "SET bioseq.index.recompute_async_persistence.enabled=off; "
        "SET bioseq.index.sequence_read.enabled=on; "
        "SET bioseq.aligner.grouped_soa_generation.enabled=on; "
        "SET bioseq.aligner.fragment_minimizer.lookup_route="
        "'direct_compact_hash_lookup';");
    const auto rowno = static_cast<std::uint64_t>(query_scalar_int(
        ctx, fmt::format(
            "SELECT bioseq.column_row_number('{}'::regclass,ctid) "
            "FROM {} ORDER BY pk LIMIT 1;",
            k_batch_join_target, k_batch_join_target)));
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT bioseq.update_index('{}'::regclass,"
        "'minimizers_per_fragment');", k_batch_join_target)) == 1);

    const std::string descriptor_sql = fmt::format(
        "SELECT generation_id::text,provider::text,image_bytes::text,"
        "header_checksum::text,"
        "(payload_relation_oid IS NOT NULL AND "
        " payload_database_oid IS NOT NULL AND "
        " payload_tablespace_oid IS NOT NULL AND "
        " payload_relfilenode IS NOT NULL AND "
        " payload_relation_name IS NOT NULL)::int::text,"
        "(payload_database_oid=(SELECT oid FROM pg_database "
        " WHERE datname=current_database()))::int::text,"
        "(payload_relfilenode=pg_relation_filenode("
        " payload_relation_oid::regclass))::int::text,"
        "(payload_relation_name=(SELECT relname::name FROM pg_class "
        " WHERE oid=payload_relation_oid))::int::text "
        "FROM bioseq_internal.minimizer_generation_storage "
        "WHERE user_relid='{}'::regclass "
        "AND index_name='minimizers_per_fragment' AND query_key={} "
        "AND state=1;",
        k_batch_join_target, rowno);
    const row_t descriptor = query_one(ctx, descriptor_sql);
    REQUIRE(descriptor.size() == 8u);
    for (const auto& field : descriptor) REQUIRE(field.has_value());
    REQUIRE(std::stoull(descriptor[0].value()) > 0u);
    REQUIRE(descriptor[1].value() == "0"); // relation-backed storage kind
    REQUIRE(std::stoull(descriptor[2].value()) >= 4096u);
    for (std::size_t field = 4u; field < descriptor.size(); ++field) {
        REQUIRE(descriptor[field].value() == "1");
    }

    load_batch_join_query(ctx, target.substr(query_offset, query_bp));
    const std::string alignment_sql =
        batch_join_sql(2, "q.id, bioseq.has_cigar(t.pk) AS has_cigar")
        + ";";
    const auto require_direct_success = [&alignment_sql, &ctx, &descriptor] {
        const rowset_t aligned = query_rows(ctx, alignment_sql);
        REQUIRE(aligned.size() == 1u);
        REQUIRE(aligned.front().size() == 2u);
        REQUIRE(aligned.front()[0].value_or("") == "0");
        REQUIRE(aligned.front()[1].value_or("") == "t");

        const row_t read = query_one(ctx,
            "SELECT requested_backend,actual_backend,"
            "requested_target_index_source,actual_target_index_source,"
            "persisted_generation_id::text,backend_fallbacks::text,"
            "recomputed_target_index_rows::text,"
            "persisted_hash_build_calls::text,"
            "persisted_generation_read_sessions::text,"
            "persisted_relation_opens::text,persisted_index_scans::text,"
            "persisted_head_scans::text,persisted_head_rows_read::text,"
            "persisted_manifest_directory_scans::text,"
            "persisted_manifest_rows_read::text,"
            "persisted_directory_chunks_read::text,"
            "persisted_directory_bytes_read::text,"
            "persisted_metadata_repeated_reads::text,"
            "persisted_metadata_heap_rows_read::text,"
            "persisted_payload_heap_rows_read::text,"
            "persisted_payload_bytes_read::text,"
            "persisted_heap_rows_read::text,"
            "persisted_toast_values_read::text,"
            "persisted_detoast_bytes::text,"
            "persisted_wanted_subkeys::text,"
            "persisted_read_subkeys::text,"
            "persisted_chunks_read::text,persisted_bytes_read::text,"
            "persisted_bulk_read_calls::text,"
            "persisted_planned_request_groups::text,"
            "persisted_executed_request_groups::text,"
            "persisted_range_scan_calls::text,"
            "cold_index_preparation->>'payload_rows_requested',"
            "cold_index_preparation->>'payload_rows_delivered',"
            "cold_index_preparation->>'payload_rows_scanned',"
            "cold_index_preparation->>'payload_bytes_requested',"
            "cold_index_preparation->>'payload_bytes_delivered',"
            "cold_index_preparation->>'payload_bytes_scanned',"
            "(NOT are_stats_saturated)::text "
            "FROM bioseq.last_alignment_execution_stats() "
            "WHERE stage='query_sketch';");
        REQUIRE(read.size() == 39u);
        for (const auto& field : read) REQUIRE(field.has_value());
        REQUIRE(read[0].value() == "simd_cpu");
        REQUIRE(read[1].value() == "simd_cpu");
        REQUIRE(read[2].value() == "persisted");
        REQUIRE(read[3].value() == "persisted");
        REQUIRE(read[4].value() == descriptor[0].value());
        for (const std::size_t zero : {
                5u, 6u, 7u, 15u, 16u, 17u, 19u, 20u, 22u, 23u,
                24u, 25u, 26u, 27u, 28u, 29u, 30u, 31u, 32u, 33u,
                34u, 35u, 36u, 37u}) {
            REQUIRE(read[zero].value() == "0");
        }
        REQUIRE(read[8].value() == "1");
        REQUIRE(read[9].value() == "1");
        REQUIRE(read[10].value() == "2");
        REQUIRE(read[11].value() == "1");
        REQUIRE(read[12].value() == "1");
        REQUIRE(read[13].value() == "1");
        REQUIRE(read[14].value() == "1");
        REQUIRE(read[18].value() == "2");
        REQUIRE(read[21].value() == "2");
        REQUIRE(read[38].value() == "true");
        REQUIRE(query_scalar_int(ctx,
            "SELECT count(*) FROM jsonb_object_keys(bioseq."
            "last_alignment_minimizer_fragment_reader_stats());")
            == 140);

        const row_t direct = query_one(ctx,
            "WITH first_read AS MATERIALIZED ("
            " SELECT bioseq."
            "last_alignment_minimizer_fragment_reader_stats() AS value"
            "), second_read AS MATERIALIZED ("
            " SELECT bioseq."
            "last_alignment_minimizer_fragment_reader_stats() AS value"
            ") SELECT "
            "first_read.value->>'revision',"
            "first_read.value->>'grouped_soa_reader_directory_capacity_bytes',"
            "first_read.value->>'grouped_soa_reader_selected_extent_capacity_bytes',"
            "first_read.value->>'grouped_soa_reader_current_selected_control_capacity_bytes',"
            "first_read.value->>'grouped_soa_reader_global_position_capacity_bytes',"
            "first_read.value->>'grouped_soa_reader_control_payload_capacity_bytes',"
            "first_read.value->>'grouped_soa_reader_retained_position_count',"
            "first_read.value->>'fragment_native_direct_emission_ns',"
            "first_read.value->>'fragment_native_control_count_bytes',"
            "first_read.value->>'fragment_native_rejected_control_validation_bytes',"
            "first_read.value->>'fragment_native_pass2_position_bytes',"
            "first_read.value->>'fragment_native_pass3_position_bytes',"
            "first_read.value->>'fragment_native_anchors_constructed',"
            "first_read.value->>'fragment_native_exact_final_bytes',"
            "first_read.value->>'fragment_native_query_plan_allocations',"
            "first_read.value->>'fragment_native_peak_query_group_offset_capacity_bytes',"
            "first_read.value->>'fragment_native_peak_query_membership_filter_capacity_bytes',"
            "first_read.value->>'fragment_native_query_group_offset_allocations',"
            "first_read.value->>'fragment_native_query_membership_filter_allocations',"
            "first_read.value->>'fragment_native_peak_query_radix_capacity_bytes',"
            "first_read.value->>'fragment_native_query_radix_allocations',"
            "first_read.value->>'fragment_native_queries',"
            "first_read.value->>'fragment_native_compact_direct_selection_shards',"
            "first_read.value->>'fragment_native_compact_direct_shard_query_layouts',"
            "first_read.value->>'fragment_native_compact_direct_layout_shards_completed',"
            "first_read.value->>'fragment_native_compact_direct_emission_shards_completed',"
            "first_read.value->>'fragment_native_compact_direct_posting_visit_passes',"
            "first_read.value->>'fragment_native_compact_direct_selected_postings',"
            "first_read.value->>'fragment_native_compact_direct_layout_postings_visited',"
            "first_read.value->>'fragment_native_compact_direct_emission_postings_visited',"
            "first_read.value->>'fragment_native_compact_direct_target_control_scan_bytes',"
            "first_read.value->>'fragment_native_compact_direct_target_control_scan_passes',"
            "first_read.value->>'fragment_native_compact_direct_fallbacks',"
            "first_read.value->>'fragment_native_compact_direct_traversal_rank_checkpoints_examined',"
            "first_read.value->>'fragment_native_compact_direct_traversal_membership_words_examined',"
            "first_read.value->>'fragment_native_compact_direct_rejected_posting_reads',"
            "first_read.value->>'fragment_native_compact_direct_lookup_ns',"
            "first_read.value->>'are_fragment_native_stats_saturated',"
            "first_read.value->>'are_stats_saturated',"
            "(first_read.value=second_read.value)::text "
            "FROM first_read CROSS JOIN second_read;");
        REQUIRE(direct.size() == 40u);
        for (const auto& field : direct) REQUIRE(field.has_value());
        const auto number = [&direct](const std::size_t field) {
            return std::stoull(direct[field].value());
        };
        REQUIRE(number(0) == 6u);
        for (const std::size_t zero : {
                1u, 2u, 3u, 4u, 5u, 6u, 8u, 9u, 15u, 16u, 17u,
                18u, 19u, 20u}) {
            REQUIRE(number(zero) == 0u);
        }
        REQUIRE(number(7) > 0u);
        // Compact-direct uses the authenticated descriptor counts for layout;
        // only the emission pass decodes posting positions.
        REQUIRE(number(10) == 0u);
        REQUIRE(number(11) > 0u);
        REQUIRE(number(12) > 0u);
        REQUIRE(number(13) > 0u);
        REQUIRE(number(14) == 1u);
        REQUIRE(number(21) > 0u);
        REQUIRE(number(22) > 0u);
        REQUIRE(number(23) == number(21) * number(22));
        REQUIRE(number(24) == number(22));
        REQUIRE(number(25) == number(22));
        REQUIRE(number(26) == 1u);
        REQUIRE(number(27) > 0u);
        REQUIRE(number(28) == 0u);
        REQUIRE(number(29) == number(27));
        for (const std::size_t zero : {
            30u, 31u, 32u, 33u, 34u, 35u}) {
            REQUIRE(number(zero) == 0u);
        }
        REQUIRE(number(36) > 0u);
        REQUIRE(direct[37].value() == "false");
        REQUIRE(direct[38].value() == "false");
        REQUIRE(direct[39].value() == "true");
    };
    require_direct_success();

    struct corruption_case {
        std::string_view assignment;
    };
    const std::array<corruption_case, 2> corruptions{{
        {"image_bytes=image_bytes+4096"},
        {"header_checksum=header_checksum # 1"},
    }};
    for (const corruption_case& corruption : corruptions) {
        INFO(fmt::format("descriptor mutation={}", corruption.assignment));
        exec_sql(ctx, "BEGIN;");
        exec_sql(ctx, fmt::format(
            "UPDATE bioseq_internal.minimizer_generation_storage SET {} "
            "WHERE user_relid='{}'::regclass "
            "AND index_name='minimizers_per_fragment' AND query_key={} "
            "AND state=1;",
            corruption.assignment, k_batch_join_target, rowno));
        REQUIRE(query_one(ctx, descriptor_sql) != descriptor);
        const auto rejected = sql_error(ctx, alignment_sql);
        REQUIRE((rejected.sqlstate == "55000"
            || rejected.sqlstate == "XX001"));
        exec_sql(ctx, "ROLLBACK;");
        REQUIRE(query_one(ctx, descriptor_sql) == descriptor);
        REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
    }
    // A clean direct reopen after both full transaction rollbacks requires the
    // LIVE relation descriptor and mmap image to remain unchanged.
    require_direct_success();
}

TEST_CASE("Ordinal-2 BatchJoin selectively consumes production hash-major minimizers_per_sequence",
              "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    for (const std::string_view index_name : {
            std::string_view{"minimizers_per_sequence"}}) {
        DYNAMIC_SECTION(index_name) {
            auto ctx = fresh_context({k_batch_join_target});
            // Disable INSERT-time persistence before creating the target so the
            // production update below owns the only generation publication.
            exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
            seed_batch_join_target(ctx);
            load_batch_join_queries(ctx, 2);
            exec_sql(ctx, "SET bioseq.index.async_persistence.enabled = off;");
            exec_sql(ctx, "SET bioseq.index.recompute_async_persistence.enabled = off;");
            exec_sql(ctx, "SET bioseq.index.sequence_read.enabled = on;");
            exec_sql(ctx,
                "SET bioseq.aligner.grouped_soa_generation.enabled = on;");
            exec_sql(ctx,
                "SET bioseq.aligner.minimizer.algorithm = 'two_stack_nthash';");
            exec_sql(ctx,
                "SET bioseq.aligner.chaining.algorithm = 'simd_minimap2';");
            exec_sql(ctx,
                "SET bioseq.aligner.base_alignment.algorithm = 'block_aligner';");
            /* The runtime capability gate alone is not a GPU component request.
             * CPU query-union and grouped-SoA preparation must remain legal. */
            exec_sql(ctx, "SET bioseq.aligner.vulkan.enabled = on;");
            const auto rowno = static_cast<std::uint64_t>(query_scalar_int(
                ctx,
                fmt::format(
                    "SELECT bioseq.column_row_number('{}'::regclass, ctid) "
                    "FROM {} ORDER BY pk LIMIT 1;",
                    k_batch_join_target, k_batch_join_target)));
            exec_sqlf(ctx,
                "SELECT bioseq.set_active_read_index('{}'::regclass, '{}');",
                k_batch_join_target, index_name);
            REQUIRE(query_scalar_int(ctx, fmt::format(
                "SELECT bioseq.update_index('{}'::regclass, '{}');",
                k_batch_join_target, index_name)) == 1);

            require_batch_join_aligned_query_rows(
                ctx,
                batch_join_sql(
                    2, "q.id, bioseq.has_cigar(t.pk) AS has_cigar"),
                2,
                "t");
            const row_t publication_diagnostics = query_one(ctx, fmt::format(
                "WITH audit AS MATERIALIZED ("
                " SELECT bioseq.inspect_ready_hash_major_minimizer_generation("
                "  '{}'::regclass, '{}', {}) AS value"
                    "), reader_stats AS MATERIALIZED ("
                " SELECT bioseq.last_alignment_hash_major_index_reader_stats() AS value"
                "), stats AS MATERIALIZED ("
                " SELECT * FROM bioseq.last_alignment_execution_stats()"
                " WHERE stage='query_sketch'"
                "), documents AS ("
                    " SELECT audit.value AS audit, reader_stats.value AS reader_stats,"
                "  audit.value->'source' AS source_doc,"
                "  audit.value->'head' AS head_doc,"
                "  audit.value->'manifest' AS manifest_doc,"
                "  audit.value->'metadata' AS metadata_doc,"
                "  audit.value->'streams'->0 AS directory_stream,"
                "  audit.value->'streams'->1 AS control_stream,"
                "  audit.value->'streams'->2 AS posting_stream,"
                "  stats.*"
                    " FROM audit CROSS JOIN reader_stats CROSS JOIN stats"
                "), parts AS ("
                " SELECT documents.*,"
                "  manifest_doc->'compatibility' AS compatibility_doc"
                " FROM documents"
                ") SELECT "
                /* Exact schemas and ownership bind the inspector, timed reader,
                 * and executor snapshot to this backend execution. */
                "jsonb_typeof(audit)='object' "
                "AND audit->>'schema'="
                " 'bioseq.hash_major_minimizer_generation_inspection' "
                "AND (audit->>'revision')::bigint=1 "
                "AND audit->>'layout'='hash_major' "
                    "AND jsonb_typeof(reader_stats)='object' "
                    "AND reader_stats->>'schema'='bioseq.hash_major_reader_stats' "
                    "AND (reader_stats->>'revision')::bigint=1 "
                    "AND (reader_stats->>'execution_id')::bigint=execution_id "
                    "AND (reader_stats->>'backend_pid')::bigint=backend_pid "
                    "AND (reader_stats->>'database_oid')::bigint=database_oid::bigint "
                "AND (audit->>'database_oid')::bigint=database_oid::bigint, "
                /* Full 128-bit source identity is the correctness authority;
                 * the scalar stats identity is diagnostic only. */
                "(head_doc->>'generation_id')::bigint>0 "
                "AND (manifest_doc->>'generation_id')::bigint="
                " (head_doc->>'generation_id')::bigint "
                    "AND (reader_stats->>'generation_id')::bigint="
                " (head_doc->>'generation_id')::bigint "
                "AND persisted_generation_id="
                " (head_doc->>'generation_id')::bigint "
                "AND source_doc->>'source_identity' "
                " = head_doc->>'source_identity' "
                "AND source_doc->>'source_identity' "
                " = manifest_doc->>'source_identity' "
                "AND source_doc->>'source_identity' "
                    " = reader_stats->>'source_identity' "
                "AND source_doc->>'source_identity' "
                " ~ '^[0-9a-f]{{32}}$' "
                "AND source_doc->>'source_content_crc64' "
                " = head_doc->>'source_content_crc64' "
                "AND source_doc->>'source_content_crc64' "
                " = manifest_doc->>'source_content_crc64' "
                "AND source_doc->>'source_content_crc64' "
                    " = reader_stats->>'source_content_crc64' "
                "AND source_doc->>'source_content_crc64' "
                " ~ '^[0-9a-f]{{16}}$' "
                "AND head_doc->>'compatibility_digest' "
                " = manifest_doc->>'compatibility_digest' "
                "AND head_doc->>'compatibility_digest' "
                    " = reader_stats->>'compatibility_digest' "
                "AND head_doc->>'compatibility_digest' "
                " ~ '^[0-9a-f]{{64}}$' "
                "AND persisted_source_identity IS NOT NULL "
                "AND persisted_source_identity<>'', "
                /* Source locator, profile, and fixed metadata sizes must agree. */
                "(audit->>'main_relfilenode')::bigint>0 "
                "AND (audit->>'column_attno')::bigint>0 "
                "AND audit->>'index_name'='{}' "
                "AND (audit->>'query_key')::bigint="
                " (source_doc->>'sequence_rowno')::bigint "
                "AND (audit->>'query_key')::bigint="
                " (head_doc->>'query_key')::bigint "
                "AND (source_doc->>'source_epoch')::bigint>0 "
                "AND (source_doc->>'source_epoch')::bigint="
                " (head_doc->>'source_epoch')::bigint "
                "AND (source_doc->>'source_epoch')::bigint="
                " (manifest_doc->>'source_epoch')::bigint "
                "AND (source_doc->>'total_bp_size')::bigint="
                " (manifest_doc->>'target_length')::bigint "
                "AND (source_doc->>'alphabet_code')::bigint=0 "
                "AND (compatibility_doc->>'alphabet_code')::bigint=0 "
                "AND (compatibility_doc->>'kmer_size')::bigint=15 "
                "AND (compatibility_doc->>'window_size')::bigint=10 "
                "AND (compatibility_doc->>'hash_width_bytes')::bigint=4 "
                "AND (compatibility_doc->>'stored_posting_width_bytes')"
                "::bigint=4 "
                "AND jsonb_typeof(head_doc->'dense_seal_present')='boolean' "
                "AND (metadata_doc->>'head_rows')::bigint=1 "
                "AND (metadata_doc->>'head_bytes')::bigint=224 "
                "AND (metadata_doc->>'manifest_rows')::bigint=1 "
                "AND (metadata_doc->>'manifest_bytes')::bigint=336, "
                /* The administrative audit independently rereads every physical
                 * row and must reproduce each logical stream exactly. */
                "directory_stream->>'kind'='directory' "
                "AND control_stream->>'kind'='controls' "
                "AND posting_stream->>'kind'='postings' "
                "AND (directory_stream->>'exact_catalog_rows')::bigint="
                " (directory_stream->>'row_count')::bigint "
                "AND (directory_stream->>'exact_catalog_bytes')::bigint="
                " (directory_stream->>'logical_bytes')::bigint "
                "AND directory_stream->>'exact_catalog_stream_crc64'="
                " directory_stream->>'stream_crc64' "
                "AND (control_stream->>'exact_catalog_rows')::bigint="
                " (control_stream->>'row_count')::bigint "
                "AND (control_stream->>'exact_catalog_bytes')::bigint="
                " (control_stream->>'logical_bytes')::bigint "
                "AND control_stream->>'exact_catalog_stream_crc64'="
                " control_stream->>'stream_crc64' "
                "AND (posting_stream->>'exact_catalog_rows')::bigint="
                " (posting_stream->>'row_count')::bigint "
                "AND (posting_stream->>'exact_catalog_bytes')::bigint="
                " (posting_stream->>'logical_bytes')::bigint "
                "AND posting_stream->>'exact_catalog_stream_crc64'="
                " posting_stream->>'stream_crc64', "
                /* Logical byte and PostgreSQL row-count equations reject
                 * truncated, padded, or relabelled streams. */
                "(directory_stream->>'logical_bytes')::numeric="
                " 80*(directory_stream->>'element_count')::numeric "
                "AND (directory_stream->>'element_count')::bigint="
                " (manifest_doc->>'directory_entry_count')::bigint "
                "AND directory_stream->>'stream_crc64'="
                " manifest_doc->>'authenticated_directory_crc64' "
                "AND (control_stream->>'element_count')::bigint="
                " (manifest_doc->>'unique_hash_count')::bigint "
                "AND (control_stream->>'logical_bytes')::bigint="
                " (manifest_doc->>'total_control_bytes')::bigint "
                "AND (posting_stream->>'element_count')::bigint="
                " (manifest_doc->>'total_posting_count')::bigint "
                "AND (posting_stream->>'logical_bytes')::bigint="
                " (manifest_doc->>'total_posting_bytes')::bigint "
                "AND (posting_stream->>'logical_bytes')::numeric="
                " 4*(posting_stream->>'element_count')::numeric "
                "AND (directory_stream->>'row_count')::bigint="
                " ((directory_stream->>'logical_bytes')::bigint+8063)/8064 "
                "AND (control_stream->>'row_count')::bigint="
                " ((control_stream->>'logical_bytes')::bigint+8063)/8064 "
                "AND (posting_stream->>'row_count')::bigint="
                " ((posting_stream->>'logical_bytes')::bigint+8063)/8064, "
                /* The live route owns one persisted reader and executes the
                 * requested SIMD implementation without rebuilding the target. */
                "requested_backend='simd_cpu' AND actual_backend='simd_cpu' "
                "AND requested_target_index_source='persisted' "
                "AND actual_target_index_source='persisted' "
                "AND persisted_generation_read_sessions=1 "
                "AND persisted_relation_opens=1 "
                "AND persisted_index_scans>0 "
                    "AND (reader_stats->>'format_revision')::bigint=6 "
                    "AND (reader_stats->>'sessions')::bigint=1 "
                "AND nthash_vector_batches>0 "
                "AND simd_chain_vector_batches>0, "
                /* The directory is the sole full timed stream. */
                    "(reader_stats->>'directory_rows')::bigint="
                " (directory_stream->>'row_count')::bigint "
                    "AND (reader_stats->>'directory_bytes')::bigint="
                " (directory_stream->>'logical_bytes')::bigint "
                    "AND (reader_stats->>'directory_row_crc_receipts')::bigint="
                    " (reader_stats->>'directory_rows')::bigint "
                    "AND (reader_stats->>'directory_stream_crc_receipts')::bigint=1, "
                /* Only selected controls are read, and each selected block has
                 * both an authenticated CRC and a semantic receipt. */
                    "(reader_stats->>'control_rows')::bigint>0 "
                    "AND (reader_stats->>'control_bytes')::bigint>0 "
                    "AND (reader_stats->>'control_row_crc_receipts')::bigint="
                    " (reader_stats->>'control_rows')::bigint "
                    "AND (reader_stats->>'control_block_crc_receipts')::bigint="
                    " (reader_stats->>'selected_block_count')::bigint "
                    "AND (reader_stats->>'control_semantic_receipts')::bigint="
                    " (reader_stats->>'selected_block_count')::bigint, "
                /* Posting byte authority is exact even when a selected byte
                 * interval occupies only part of a row or block. */
                    "(reader_stats->>'posting_rows')::bigint>0 "
                    "AND (reader_stats->>'posting_bytes')::bigint>0 "
                    "AND (reader_stats->>'posting_row_crc_receipts')::bigint="
                    " (reader_stats->>'posting_rows')::bigint "
                    "AND (reader_stats->>'posting_ranges_delivered')::bigint>0 "
                    "AND (reader_stats->>'posting_semantic_receipts')::bigint="
                    " (reader_stats->>'posting_ranges_delivered')::bigint "
                    "AND (reader_stats->>'posting_range_crc_receipts')::bigint<="
                    " (reader_stats->>'posting_ranges_delivered')::bigint "
                    "AND (reader_stats->>'retained_position_count')::bigint>0 "
                    "AND (reader_stats->>'authorized_posting_bytes')::numeric="
                    " 4*(reader_stats->>'retained_position_count')::numeric "
                    "AND (reader_stats->>'authorized_posting_bytes')::bigint<="
                    " (reader_stats->>'posting_bytes')::bigint, "
                /* Selectivity and exact retained ownership are performance
                 * requirements, not optional diagnostic counters. */
                    "(reader_stats->>'query_count')::bigint>0 "
                    "AND (reader_stats->>'accepted_hash_count')::bigint>0 "
                    "AND (reader_stats->>'accepted_hash_count')::bigint<="
                    " (reader_stats->>'query_count')::bigint "
                    "AND (reader_stats->>'selected_block_count')::bigint>0 "
                    "AND (reader_stats->>'selected_block_count')::bigint<="
                " (manifest_doc->>'directory_entry_count')::bigint "
                    "AND (reader_stats->>'directory_stage_peak_bytes')::bigint>0 "
                    "AND (reader_stats->>'control_stage_peak_bytes')::bigint>0 "
                    "AND (reader_stats->>'posting_stage_peak_bytes')::bigint>0 "
                    "AND (reader_stats->>'peak_admitted_bytes')::bigint="
                " GREATEST("
                    "  (reader_stats->>'directory_stage_peak_bytes')::bigint,"
                    "  (reader_stats->>'control_stage_peak_bytes')::bigint,"
                    "  (reader_stats->>'posting_stage_peak_bytes')::bigint) "
                    "AND (reader_stats->>'retained_owner_bytes')::bigint>0 "
                    "AND (reader_stats->>'retained_owner_bytes')::bigint="
                    " (reader_stats->>'admitted_bytes')::bigint, "
                /* No assembly, source fallback, recomputation, or
                 * backend fallback may hide behind a successful mapping. */
                    "(reader_stats->>'assembly_calls')::bigint=0 "
                    "AND (reader_stats->>'pair_index_conversion_calls')::bigint=0 "
                    "AND (reader_stats->>'segment_rebase_calls')::bigint=0 "
                    "AND (reader_stats->>'recompute_calls')::bigint=0 "
                    "AND (reader_stats->>'generic_source_fallback_calls')::bigint=0 "
                    "AND (reader_stats->>'backend_fallbacks')::bigint=0 "
                    "AND (reader_stats->>'failures')::bigint=0 "
                    "AND NOT (reader_stats->>'is_saturated')::boolean "
                    "AND NOT (reader_stats->>'has_operational_identity_conflict')::boolean "
                    "AND NOT (reader_stats->>'are_stats_saturated')::boolean "
                "AND recomputed_target_index_rows=0 "
                "AND persisted_hash_build_calls=0 "
                "AND pair_sort_calls=0 "
                "AND host_pair_materialization_calls=0 "
                "AND backend_fallbacks=0 "
                "AND persisted_pair_index_conversion_calls=0 "
                "AND persisted_segment_rebase_calls=0 "
                "AND NOT has_operational_identity_conflict "
                "AND NOT are_stats_saturated "
                "FROM parts;",
                k_batch_join_target, index_name, rowno, index_name));
            REQUIRE(publication_diagnostics.size() == 11u);
                for (const auto& diagnostic_result : publication_diagnostics) {
                    REQUIRE(diagnostic_result.value_or("") == "t");
                }

        }
    }
}

TEST_CASE("Cold minimizer index reads release sidecar and catalog locks before later work",
              "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto reader = fresh_context({k_batch_join_target});
    exec_sql(reader, "SET bioseq.index.insert_maintenance.enabled = off;");
    seed_batch_join_target(reader);
    load_batch_join_queries(reader, 2);
    exec_sql(reader, "SET bioseq.index.insert_maintenance.enabled = on;");
    exec_sql(reader, "SET bioseq.index.async_persistence.enabled = off;");
    exec_sql(reader, "SET bioseq.index.recompute_async_persistence.enabled = off;");
    exec_sql(reader, "SET bioseq.index.sequence_read.enabled = on;");
    const auto rowno = static_cast<std::uint64_t>(query_scalar_int(
        reader,
        fmt::format(
            "SELECT bioseq.column_row_number('{}'::regclass, ctid) "
            "FROM {} ORDER BY pk LIMIT 1;",
            k_batch_join_target, k_batch_join_target)));
    const row_t publication = query_one(reader, fmt::format(
        "SELECT * FROM bioseq_internal.build_ready_minimizer_fragment_generation("
        "'{}'::regclass, 'minimizers_per_fragment', {}, 'exact_simd');",
        k_batch_join_target, rowno));
    REQUIRE(publication.size() >= 10u);
    REQUIRE(publication[0].has_value());
    const std::string sidecar = publication[0].value();
    const row_t relation_oids = query_one(reader, fmt::format(
        "SELECT '{}'::regclass::oid::text, "
        "'bioseq_internal.{}'::regclass::oid::text, "
        "(SELECT indexrelid::text FROM pg_index WHERE indrelid="
        "'bioseq_internal.{}'::regclass AND indisprimary), "
        "'bioseq_internal.index_policy'::regclass::oid::text, "
        "'bioseq_internal.index_policy_pkey'::regclass::oid::text, "
        "'bioseq_internal.sequence_source_version'::regclass::oid::text, "
        "'bioseq_internal.sequence_source_version_pkey'::regclass::oid::text;",
        k_batch_join_target, sidecar, sidecar));
    REQUIRE(relation_oids.size() == 7u);
    for (const auto& oid : relation_oids) REQUIRE(oid.has_value());
    const row_t lock_identity = query_one(reader, fmt::format(
        "SELECT d.oid::bigint::text, "
        "COALESCE(NULLIF(c.reltablespace, 0), d.dattablespace)::bigint::text, "
        "COALESCE(pg_relation_filenode(c.oid), c.oid)::bigint::text, "
        "a.attnum::bigint::text "
        "FROM pg_class c JOIN pg_database d ON d.datname=current_database() "
        "JOIN pg_attribute a ON a.attrelid=c.oid AND a.attname='seq0' "
        "AND a.attnum>0 AND NOT a.attisdropped "
        "WHERE c.oid='{}'::regclass;",
        k_batch_join_target));
    REQUIRE(lock_identity.size() == 4u);
    for (const auto& identity : lock_identity) REQUIRE(identity.has_value());
    const auto database_oid = static_cast<std::uint32_t>(
        std::stoull(lock_identity[0].value()));
    const auto tablespace_oid = static_cast<std::uint32_t>(
        std::stoull(lock_identity[1].value()));
    const auto relfilenode = static_cast<std::uint32_t>(
        std::stoull(lock_identity[2].value()));
    const auto sequence_attno = static_cast<std::int16_t>(
        std::stoll(lock_identity[3].value()));
    const std::int64_t advisory_key = std::bit_cast<std::int64_t>(
        minimizer_index_advisory_lock_key(
            database_oid, tablespace_oid, relfilenode, sequence_attno,
            "minimizers_per_fragment"));

    connection_test_context gate;
    connection_test_context writer;
    connection_test_context dropper;
    connection_test_context observer;
    const long long reader_pid = query_scalar_int(
        reader, "SELECT pg_backend_pid();");
    const long long gate_pid = query_scalar_int(
        gate, "SELECT pg_backend_pid();");
    const long long writer_pid = query_scalar_int(
        writer, "SELECT pg_backend_pid();");
    const long long drop_pid = query_scalar_int(
        dropper, "SELECT pg_backend_pid();");
    exec_sql(reader, "BEGIN ISOLATION LEVEL REPEATABLE READ;");
    require_batch_join_aligned_query_rows(
        reader,
        batch_join_sql(2, "q.id, bioseq.has_cigar(t.pk) AS has_cigar"),
        2, "t");
    const row_t retained_locks = query_one(observer, fmt::format(
        "SELECT count(*) FILTER (WHERE relation={}::oid "
        "AND mode='AccessShareLock' AND granted)::text, "
        "count(*) FILTER (WHERE relation=ANY(ARRAY[{}::oid,{}::oid,"
        "{}::oid,{}::oid,{}::oid,{}::oid]) AND granted)::text "
        "FROM pg_locks WHERE pid={};",
        relation_oids[0].value(),
        relation_oids[1].value(), relation_oids[2].value(),
        relation_oids[3].value(), relation_oids[4].value(),
        relation_oids[5].value(), relation_oids[6].value(), reader_pid));
    REQUIRE(retained_locks.size() == 2u);
    REQUIRE(std::stoll(retained_locks[0].value()) >= 1);
    REQUIRE(std::stoll(retained_locks[1].value()) == 0);

    /* Freeze the exact writer boundary: the gate owns the production logical-
     * index advisory identity and the exact sidecar. Releasing the session lock
     * separately from the transaction lock lets the observer distinguish
     * advisory wait from sidecar wait without timing guesses. */
    exec_sql(gate, "BEGIN;");
    exec_sql(gate, fmt::format(
        "SELECT pg_advisory_lock({}::bigint);", advisory_key));
    exec_sql(gate, fmt::format(
        "LOCK TABLE bioseq_internal.{} IN ACCESS EXCLUSIVE MODE;", sidecar));
    const row_t gate_advisory = query_one(observer, fmt::format(
        "SELECT database::text, classid::text, objid::text, objsubid::text "
        "FROM pg_locks WHERE pid={} AND locktype='advisory' "
        "AND mode='ExclusiveLock' AND granted;",
        gate_pid));
    REQUIRE(gate_advisory.size() == 4u);
    for (const auto& identity : gate_advisory) REQUIRE(identity.has_value());
    REQUIRE(query_scalar_int(observer, fmt::format(
        "SELECT count(*) FROM pg_locks WHERE pid={} AND relation={}::oid "
        "AND mode='AccessExclusiveLock' AND granted;",
        gate_pid, relation_oids[1].value())) == 1);
    const std::string exact_advisory_predicate = fmt::format(
        "locktype='advisory' AND database={}::oid AND classid={}::oid "
        "AND objid={}::oid AND objsubid={}",
        gate_advisory[0].value(), gate_advisory[1].value(),
        gate_advisory[2].value(), gate_advisory[3].value());

    /* Invoke the real production writer without a harness-supplied target lock.
     * The AccessShareLock retained by IsBioseqColumnTableAmTable must remain
     * observable while update_index waits at the next lock level. */
    exec_sql(writer, "SET statement_timeout = '30s';");
    exec_sql(writer, "SET bioseq.index.insert_maintenance.enabled = off;");
    exec_sql(writer, "SET bioseq.index.async_persistence.enabled = off;");
    exec_sql(writer, "SET bioseq.index.recompute_async_persistence.enabled = off;");
    exec_sql(writer,
        "SET bioseq.aligner.grouped_soa_generation.enabled = on;");
    exec_sql(writer, "BEGIN;");
    scoped_async_query writer_query{writer.raw()};
    const std::string writer_sql = fmt::format(
        "SELECT bioseq.update_index('{}'::regclass, "
        "'minimizers_per_fragment');",
        k_batch_join_target);
    REQUIRE(writer_query.send(writer_sql));

    const auto count_cell = [](const row_t& row, const std::size_t column) {
        if (column >= row.size() || !row[column].has_value()) return -1LL;
        try {
            return std::stoll(row[column].value());
        } catch (...) {
            return -1LL;
        }
    };
    bool writer_waited_exact_advisory = false;
    bool writer_held_target_before_advisory = false;
    bool writer_had_no_sidecar_before_advisory = false;
    for (unsigned attempt = 0; attempt < 1000; ++attempt) {
        const row_t locks = query_one(observer, fmt::format(
            "SELECT count(*) FILTER (WHERE relation={}::oid "
            "AND mode='AccessShareLock' AND granted)::text, "
            "count(*) FILTER (WHERE {} AND NOT granted)::text, "
            "count(*) FILTER (WHERE relation={}::oid)::text "
            "FROM pg_locks WHERE pid={};",
            relation_oids[0].value(), exact_advisory_predicate,
            relation_oids[1].value(), writer_pid));
        if (count_cell(locks, 1) > 0) {
            writer_waited_exact_advisory = true;
            writer_held_target_before_advisory = count_cell(locks, 0) > 0;
            writer_had_no_sidecar_before_advisory = count_cell(locks, 2) == 0;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    scoped_async_query drop_query{dropper.raw()};
    const std::string drop_sql = fmt::format(
        "DROP TABLE {};", k_batch_join_target);
    const bool drop_sent = drop_query.send(drop_sql);
    bool target_wait_observed = false;
    bool drop_had_no_sidecar_lock = false;
    for (unsigned attempt = 0; drop_sent && attempt < 1000; ++attempt) {
        const row_t locks = query_one(observer, fmt::format(
            "SELECT count(*) FILTER (WHERE relation={}::oid "
            "AND mode='AccessExclusiveLock' AND NOT granted)::text, "
            "count(*) FILTER (WHERE relation={}::oid)::text "
            "FROM pg_locks WHERE pid={};",
            relation_oids[0].value(), relation_oids[1].value(), drop_pid));
        if (count_cell(locks, 0) > 0) {
            target_wait_observed = true;
            drop_had_no_sidecar_lock = count_cell(locks, 1) == 0;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    /* Release only the session advisory lock. The gate transaction intentionally
     * retains sidecar AccessExclusiveLock, so the writer must advance from one
     * exact observable wait to the next while keeping its target lock. */
    const bool gate_advisory_released = query_scalar(gate, fmt::format(
        "SELECT pg_advisory_unlock({}::bigint);", advisory_key)) == "t";
    bool writer_waited_sidecar = false;
    bool writer_held_advisory_while_waiting_sidecar = false;
    bool writer_held_target_while_waiting_sidecar = false;
    bool writer_had_no_granted_sidecar = false;
    bool gate_retained_sidecar = false;
    for (unsigned attempt = 0; attempt < 1000; ++attempt) {
        const row_t locks = query_one(observer, fmt::format(
            "SELECT count(*) FILTER (WHERE relation={}::oid "
            "AND mode='AccessShareLock' AND granted)::text, "
            "count(*) FILTER (WHERE {} AND granted)::text, "
            "count(*) FILTER (WHERE relation={}::oid AND NOT granted)::text, "
            "count(*) FILTER (WHERE relation={}::oid AND granted)::text, "
            "(SELECT count(*) FROM pg_locks WHERE pid={} "
            "AND relation={}::oid AND mode='AccessExclusiveLock' "
            "AND granted)::text "
            "FROM pg_locks WHERE pid={};",
            relation_oids[0].value(), exact_advisory_predicate,
            relation_oids[1].value(), relation_oids[1].value(), gate_pid,
            relation_oids[1].value(), writer_pid));
        if (count_cell(locks, 2) > 0) {
            writer_waited_sidecar = true;
            writer_held_target_while_waiting_sidecar =
                count_cell(locks, 0) > 0;
            writer_held_advisory_while_waiting_sidecar =
                count_cell(locks, 1) > 0;
            writer_had_no_granted_sidecar = count_cell(locks, 3) == 0;
            gate_retained_sidecar = count_cell(locks, 4) == 1;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    /* No assertions may unwind while either asynchronous command is live. */
    exec_sql(gate, "COMMIT;");
    const async_query_outcome writer_outcome =
        writer_query.drain(static_cast<int>(PGRES_TUPLES_OK), 3000);
    const bool writer_connection_idle = writer_outcome.ready
        || writer_query.cancel_and_drain();
    exec_sql(reader, "COMMIT;");
    if (writer_connection_idle) {
        exec_sql(writer,
            writer_outcome.ready && writer_outcome.got_result
                && writer_outcome.expected_status
            ? "COMMIT;" : "ROLLBACK;");
    }
    const async_query_outcome drop_outcome =
        drop_sent && writer_connection_idle
        ? drop_query.drain(static_cast<int>(PGRES_COMMAND_OK), 3000)
        : async_query_outcome{
            .consume_ok = false,
            .ready = false,
            .got_result = false,
            .expected_status = false,
        };

    REQUIRE(writer_waited_exact_advisory);
    REQUIRE(writer_held_target_before_advisory);
    REQUIRE(writer_had_no_sidecar_before_advisory);
    REQUIRE(drop_sent);
    REQUIRE(target_wait_observed);
    REQUIRE(drop_had_no_sidecar_lock);
    REQUIRE(gate_advisory_released);
    REQUIRE(writer_waited_sidecar);
    REQUIRE(writer_held_target_while_waiting_sidecar);
    REQUIRE(writer_held_advisory_while_waiting_sidecar);
    REQUIRE(writer_had_no_granted_sidecar);
    REQUIRE(gate_retained_sidecar);
    REQUIRE(writer_outcome.consume_ok);
    REQUIRE(writer_outcome.ready);
    REQUIRE(writer_outcome.got_result);
    REQUIRE(writer_outcome.expected_status);
    REQUIRE(writer_connection_idle);
    REQUIRE(drop_outcome.consume_ok);
    REQUIRE(drop_outcome.ready);
    REQUIRE(drop_outcome.got_result);
    REQUIRE(drop_outcome.expected_status);
}

TEST_CASE("BatchJoin LIMIT is partial and empty query child is fully drained",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    SECTION("LIMIT") {
        auto ctx = fresh_context({k_batch_join_target});
        seed_batch_join_target(ctx);
        load_batch_join_queries(ctx, 4);
        const std::string sql = batch_join_sql(1) + " LIMIT 1";
        const auto plan = explain_text<false>(ctx, sql);
        REQUIRE(plan_contains(plan, "Custom Scan (BioseqBatchJoin)"));
        REQUIRE(plan_contains(plan, "Custom Scan (BioseqScan)"));
        REQUIRE(plan_contains(plan, "Bioseq Query Planner Rows"));
        /* The heap query provider is projection-free and may be represented as a
         * SeqScan or IndexScan across PostgreSQL planning choices. Successful
         * execution requires validation of the returned scan slot before query
         * materialization. */
        REQUIRE_FALSE(query_rows(ctx, sql + ";").empty());
        const auto stats = last_alignment_stats(ctx);
        REQUIRE(stats.size() == 20);
        REQUIRE_FALSE(stats_bool(stats.front(), 13));
        REQUIRE(stats_bool(stats.front(), 14));
        REQUIRE_FALSE(stats_bool(stats.front(), 15));
    }

    SECTION("empty query child") {
        auto ctx = fresh_context({k_batch_join_target});
        seed_batch_join_target(ctx);
        load_batch_join_queries(ctx, 0);
        REQUIRE(query_rows(ctx, batch_join_sql(1) + ";").empty());
        require_batch_join_stats(ctx, 0, 0);
    }
}

TEST_CASE("BatchJoin validates the slot returned by a pass-through query child",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    /* OFFSET 0 prevents subquery pull-up and leaves a Limit/SubqueryScan
     * provider whose executor may return its child's slot. This pins the
     * distinction between the post-setrefs Plan targetlist contract and the
     * slot returned by ExecProcNode. */
    auto ctx = fresh_context({k_batch_join_target});
    seed_batch_join_target(ctx);
    load_batch_join_queries(ctx, 4);
    const std::string sql = fmt::format(
        "SELECT q.id, t.pk, bioseq.has_cigar(t.pk) AS has_cigar "
        "FROM {} t JOIN "
        "(SELECT id, seq FROM {} OFFSET 0) q "
        "ON mmsearch(t.seq0, q.seq, "
        "aligner_options(index_read_requirement => 1))",
        k_batch_join_target,
        k_batch_join_queries);
    const auto plan = explain_text<false>(ctx, sql);
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqBatchJoin)"));
    REQUIRE(plan_contains(plan, "Bioseq Batch ABI Version: 3"));
    REQUIRE(plan_contains(plan, "Bioseq Query Planner Rows"));
    /* OFFSET 0 retains a pass-through provider contract, but PostgreSQL may
     * omit or rename its physical node in text EXPLAIN. Successful rows require
     * BatchJoin to validate the slot returned by that child. */
    const auto rows = query_rows(ctx, sql + " ORDER BY q.id, t.pk;");
    REQUIRE(rows.size() == 4);
    for (const auto& row : rows) {
        REQUIRE(row.size() == 3);
        REQUIRE(row[2].has_value());
        REQUIRE(*row[2] == "t");
    }
    require_batch_join_stats(ctx, 1, 4);
}

TEST_CASE("unsupported BatchJoin shapes stay on ordinary planner paths",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    auto ctx = fresh_context({k_batch_join_target});
    seed_batch_join_target(ctx);
    load_batch_join_queries(ctx, 2);

    const auto target_projection = explain_text<false>(
        ctx, batch_join_sql(1, "t.seq0, q.id"));
    REQUIRE_FALSE(plan_contains(
        target_projection, "Custom Scan (BioseqBatchJoin)"));

#if defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
    exec_sql(
        ctx,
        "CREATE TEMP TABLE mmsearch_batch_join_dna5 "
        "(id int PRIMARY KEY, seq bioseq.seq('dna5', 32));");
    exec_sqlf(
        ctx,
        "INSERT INTO mmsearch_batch_join_dna5 VALUES (1, {});",
        format_literal(
            "ACGTACGTACGTACGTACGTACGTACGTACGT",
            seq_typmod{.alphabet = "dna5", .width = 32}));
    // Cross-alphabet operands are a semantic error before BatchJoin ownership;
    // they must not fall through to an ordinary scalar planner path.
    const auto typmod_mismatch = sql_error(
        ctx,
        fmt::format(
            "EXPLAIN (VERBOSE, COSTS off) SELECT q.id, t.pk FROM {} t "
            "JOIN mmsearch_batch_join_dna5 q "
            "ON mmsearch(t.seq0, q.seq, "
            "aligner_options(index_read_requirement => 1));",
            k_batch_join_target));
    REQUIRE(typmod_mismatch.sqlstate == "42804");
    REQUIRE(typmod_mismatch.contains(
        "bioseq mmsearch requires operands with the same bound alphabet"));
#else
    const auto unsupported_alphabet = sql_error(
        ctx,
        "CREATE TEMP TABLE mmsearch_batch_join_dna5 "
        "(id int PRIMARY KEY, seq bioseq.seq('dna5', 32));");
    REQUIRE(unsupported_alphabet.sqlstate == "22023");
    REQUIRE(unsupported_alphabet.contains(
        "bioseq.seq alphabet \"dna5\" is not supported"));
    REQUIRE(unsupported_alphabet.contains("Valid alphabets: 'dna4'"));
#endif
}
