/**
 * @file bench/tests/contract/evidence_test.cpp
 * @brief Direct contracts for the E2E raw-sample serializer.
 *
 * The test imports a target-owned interface that exposes only the production
 * contracts exercised below. The executable launcher remains an independent,
 * single-purpose translation unit.
 */
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstdlib>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <crt_externs.h>
#else
extern char** environ;
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

import bioseq.bench.e2e.tests.support;
import bioseq.bench.framework;

namespace {

namespace e2e_support = bioseq::bench::e2e::tests::support;

using e2e_support::algorithm_matrix_session_sql;
using e2e_support::batched_transformed_coordinate_sql;
using e2e_support::annotate_behavior_differences;
using e2e_support::cold_index_stats_keys;
using e2e_support::config_value;
using e2e_support::evaluate;
using e2e_support::is_canonical_campaign_e2e_config_identity;
using e2e_support::is_complete_vulkan_algorithm_matrix_tuple;
using e2e_support::minimizer_fragment_reader_stats_keys;
using e2e_support::e2e_input;
using e2e_support::is_sliced_pipeline_stats_configuration_valid;
using e2e_support::make_logical_query_chunks;
using e2e_support::minimizers_per_fragment_index;
using e2e_support::ordinary_algorithm_uses_iteration_boundaries;
using e2e_support::e2e_output;
using e2e_support::parameter_map;
using e2e_support::parse_bioseq_rows;
using e2e_support::parse_minimap2_rows;
using e2e_support::persisted_gpu_operation_keys;
using e2e_support::persisted_setup_must_reject;
using e2e_support::projection_sql;
using e2e_support::finalize_paired_oracle_repeat;
using e2e_support::e2e_query_truth;
using e2e_support::resolved_execution_tuple;
using e2e_support::resolve_just_index_setup_requirements;
using e2e_support::rowset;
using e2e_support::run_constraints;
using e2e_support::serialize_raw_sample;
using e2e_support::sliced_no_index_algorithm;
using bioseq::bench::framework::e2e_raw_payload_schema_version;

[[nodiscard]] nlohmann::json load_e2e_contract_config(
        const std::filesystem::path& path) {
    std::ifstream stream{path};
    if (!stream.good()) {
        throw std::runtime_error("could not open E2E contract config");
    }
    return nlohmann::json::parse(stream);
}

[[nodiscard]] run_constraints exact_e2e_execution_constraints(
        const nlohmann::json& config,
        std::string_view benchmark) {
    const auto& matrix = config.at(std::string{benchmark}).at("matrix");
    const auto& over = matrix.at("over");
    run_constraints constraints{};
    for (const auto& row :
         matrix.at("acceptance").at("exact_algorithm_rows")) {
        const std::string algorithm = row.get<std::string>();
        const auto& axes = over.at(algorithm);
        parameter_map params{};
        if (axes.contains("minimizer_index")) {
            const auto& values = axes.at("minimizer_index");
            if (!values.is_array() || values.size() != 1
                || !values.front().is_string()) {
                throw std::runtime_error(
                    "contract test requires one resolved minimizer_index");
            }
            params.emplace(
                "minimizer_index",
                config_value::from(
                    values.front().get<std::string>()));
        }
        constraints.execution_tuples.push_back(
            resolved_execution_tuple{
                .algorithm_name = algorithm,
                .params = std::move(params),
            });
    }
    return constraints;
}

[[nodiscard]] nlohmann::json zero_hash_major_index_reader_stats(
        const std::int64_t execution_id,
        const std::int64_t backend_pid,
        const std::int64_t database_oid,
        const std::int64_t recompute_calls = 0) {
    return nlohmann::json{
        {"schema", "bioseq.hash_major_reader_stats"},
        {"revision", 1},
        {"execution_id", execution_id},
        {"backend_pid", backend_pid},
        {"database_oid", database_oid},
        {"format_revision", 0},
        {"sessions", 0},
        {"generation_id", 0},
        {"source_identity", "00000000000000000000000000000000"},
        {"source_content_crc64", "0000000000000000"},
        {"compatibility_digest", std::string(64, '0')},
        {"directory_rows", 0},
        {"directory_bytes", 0},
        {"control_rows", 0},
        {"control_bytes", 0},
        {"posting_rows", 0},
        {"posting_bytes", 0},
        {"directory_row_crc_receipts", 0},
        {"directory_stream_crc_receipts", 0},
        {"control_row_crc_receipts", 0},
        {"control_block_crc_receipts", 0},
        {"control_semantic_receipts", 0},
        {"posting_row_crc_receipts", 0},
        {"posting_ranges_delivered", 0},
        {"posting_range_crc_receipts", 0},
        {"posting_semantic_receipts", 0},
        {"authorized_posting_bytes", 0},
        {"query_count", 0},
        {"selected_block_count", 0},
        {"accepted_hash_count", 0},
        {"retained_position_count", 0},
        {"directory_stage_peak_bytes", 0},
        {"control_stage_peak_bytes", 0},
        {"posting_stage_peak_bytes", 0},
        {"peak_admitted_bytes", 0},
        {"retained_owner_bytes", 0},
        {"admitted_bytes", 0},
        {"assembly_calls", 0},
        {"pair_index_conversion_calls", 0},
        {"segment_rebase_calls", 0},
        {"recompute_calls", recompute_calls},
        {"generic_source_fallback_calls", 0},
        {"backend_fallbacks", 0},
        {"failures", 0},
        {"is_saturated", false},
        {"has_operational_identity_conflict", false},
        {"are_stats_saturated", false},
    };
}

inline constexpr auto k_fragment_native_flat_metric_fields =
    std::to_array<std::pair<
        std::string_view, std::int64_t e2e_output::*>>({
        {"fragment_native_waves", &e2e_output::fragment_native_waves},
        {"fragment_native_queries", &e2e_output::fragment_native_queries},
        {"fragment_native_occurrence_fill_ns",
            &e2e_output::fragment_native_occurrence_fill_ns},
        {"fragment_native_occurrence_radix_ns",
            &e2e_output::fragment_native_occurrence_radix_ns},
        {"fragment_native_unique_plan_build_ns",
            &e2e_output::fragment_native_unique_plan_build_ns},
        {"fragment_native_query_plan_radix_fill_ns",
            &e2e_output::fragment_native_query_plan_radix_fill_ns},
        {"fragment_native_control_count_pass_ns",
            &e2e_output::fragment_native_control_count_pass_ns},
        {"fragment_native_rejected_control_validation_ns",
            &e2e_output::fragment_native_rejected_control_validation_ns},
        {"fragment_native_orientation_layout_ns",
            &e2e_output::fragment_native_orientation_layout_ns},
        {"fragment_native_direct_emission_ns",
            &e2e_output::fragment_native_direct_emission_ns},
        {"fragment_native_local_sort_ns",
            &e2e_output::fragment_native_local_sort_ns},
        {"fragment_native_tie_normalization_ns",
            &e2e_output::fragment_native_tie_normalization_ns},
        {"fragment_native_control_count_bytes",
            &e2e_output::fragment_native_control_count_bytes},
        {"fragment_native_rejected_control_validation_bytes",
            &e2e_output::fragment_native_rejected_control_validation_bytes},
        {"fragment_native_pass2_position_bytes",
            &e2e_output::fragment_native_pass2_position_bytes},
        {"fragment_native_pass3_position_bytes",
            &e2e_output::fragment_native_pass3_position_bytes},
        {"fragment_native_anchors_constructed",
            &e2e_output::fragment_native_anchors_constructed},
        {"fragment_native_local_sort_bytes",
            &e2e_output::fragment_native_local_sort_bytes},
        {"fragment_native_tie_normalization_bytes",
            &e2e_output::fragment_native_tie_normalization_bytes},
        {"fragment_native_exact_final_bytes",
            &e2e_output::fragment_native_exact_final_bytes},
        {"fragment_native_peak_exact_final_bytes",
            &e2e_output::fragment_native_peak_exact_final_bytes},
        {"fragment_native_global_sort_calls_invoked",
            &e2e_output::fragment_native_global_sort_calls_invoked},
        {"fragment_native_global_sort_calls_skipped",
            &e2e_output::fragment_native_global_sort_calls_skipped},
        {"fragment_native_candidate_anchor_bytes",
            &e2e_output::fragment_native_candidate_anchor_bytes},
        {"fragment_native_plan_ordinal_bytes",
            &e2e_output::fragment_native_plan_ordinal_bytes},
        {"fragment_native_compaction_bytes",
            &e2e_output::fragment_native_compaction_bytes},
        {"fragment_native_final_copy_bytes",
            &e2e_output::fragment_native_final_copy_bytes},
        {"fragment_native_peak_occurrence_capacity_bytes",
            &e2e_output::fragment_native_peak_occurrence_capacity_bytes},
        {"fragment_native_peak_query_plan_capacity_bytes",
            &e2e_output::fragment_native_peak_query_plan_capacity_bytes},
        {"fragment_native_peak_query_group_offset_capacity_bytes",
            &e2e_output::fragment_native_peak_query_group_offset_capacity_bytes},
        {"fragment_native_peak_query_membership_filter_capacity_bytes",
            &e2e_output::fragment_native_peak_query_membership_filter_capacity_bytes},
        {"fragment_native_peak_query_radix_capacity_bytes",
            &e2e_output::fragment_native_peak_query_radix_capacity_bytes},
        {"fragment_native_peak_occurrence_count_capacity_bytes",
            &e2e_output::fragment_native_peak_occurrence_count_capacity_bytes},
        {"fragment_native_peak_block_layout_capacity_bytes",
            &e2e_output::fragment_native_peak_block_layout_capacity_bytes},
        {"fragment_native_peak_block_stats_capacity_bytes",
            &e2e_output::fragment_native_peak_block_stats_capacity_bytes},
        {"fragment_native_occurrence_allocations",
            &e2e_output::fragment_native_occurrence_allocations},
        {"fragment_native_query_plan_allocations",
            &e2e_output::fragment_native_query_plan_allocations},
        {"fragment_native_query_group_offset_allocations",
            &e2e_output::fragment_native_query_group_offset_allocations},
        {"fragment_native_query_membership_filter_allocations",
            &e2e_output::fragment_native_query_membership_filter_allocations},
        {"fragment_native_query_radix_allocations",
            &e2e_output::fragment_native_query_radix_allocations},
        {"fragment_native_occurrence_count_allocations",
            &e2e_output::fragment_native_occurrence_count_allocations},
        {"fragment_native_block_layout_allocations",
            &e2e_output::fragment_native_block_layout_allocations},
        {"fragment_native_block_stats_allocations",
            &e2e_output::fragment_native_block_stats_allocations},
        {"fragment_native_query_occurrences",
            &e2e_output::fragment_native_query_occurrences},
        {"fragment_native_unique_query_plans",
            &e2e_output::fragment_native_unique_query_plans},
        {"fragment_native_packed_occurrence_waves",
            &e2e_output::fragment_native_packed_occurrence_waves},
        {"fragment_native_wide_occurrence_waves",
            &e2e_output::fragment_native_wide_occurrence_waves},
        {"fragment_native_compact_direct_lookup_ns",
            &e2e_output::fragment_native_compact_direct_lookup_ns},
        {"fragment_native_compact_direct_selection_shards",
            &e2e_output::fragment_native_compact_direct_selection_shards},
        {"fragment_native_compact_direct_shard_query_layouts",
            &e2e_output::fragment_native_compact_direct_shard_query_layouts},
        {"fragment_native_compact_direct_layout_shards_completed",
            &e2e_output::fragment_native_compact_direct_layout_shards_completed},
        {"fragment_native_compact_direct_emission_shards_completed",
            &e2e_output::fragment_native_compact_direct_emission_shards_completed},
        {"fragment_native_compact_direct_posting_visit_passes",
            &e2e_output::fragment_native_compact_direct_posting_visit_passes},
        {"fragment_native_compact_direct_selected_postings",
            &e2e_output::fragment_native_compact_direct_selected_postings},
        {"fragment_native_compact_direct_layout_postings_visited",
            &e2e_output::fragment_native_compact_direct_layout_postings_visited},
        {"fragment_native_compact_direct_emission_postings_visited",
            &e2e_output::fragment_native_compact_direct_emission_postings_visited},
        {"fragment_native_compact_direct_target_control_scan_bytes",
            &e2e_output::fragment_native_compact_direct_target_control_scan_bytes},
        {"fragment_native_compact_direct_target_control_scan_passes",
            &e2e_output::fragment_native_compact_direct_target_control_scan_passes},
        {"fragment_native_compact_direct_fallbacks",
            &e2e_output::fragment_native_compact_direct_fallbacks},
        {"fragment_native_compact_direct_traversal_rank_checkpoints_examined",
            &e2e_output::fragment_native_compact_direct_traversal_rank_checkpoints_examined},
        {"fragment_native_compact_direct_traversal_membership_words_examined",
            &e2e_output::fragment_native_compact_direct_traversal_membership_words_examined},
        {"fragment_native_compact_direct_rejected_posting_reads",
            &e2e_output::fragment_native_compact_direct_rejected_posting_reads},
        {"are_fragment_native_stats_saturated",
            &e2e_output::are_fragment_native_stats_saturated},
    });
static_assert(k_fragment_native_flat_metric_fields.size() == 63);

inline constexpr auto k_expected_grouped_soa_provider_lifecycle_keys =
    std::to_array<std::string_view>({
        "provider_configured",
        "provider_captured",
        "provider_persisted",
        "provider_opened",
        "provider_head_scans",
        "provider_head_rows",
        "provider_descriptor_scans",
        "provider_descriptor_rows",
        "payload_tuples_formed",
        "payload_tuples_inserted",
        "payload_tuples_reread",
        "mapped_sink_calls",
        "mapped_sink_bytes",
        "mapped_sink_flushes",
        "mapped_sink_coalescer_capacity_bytes",
        "mapped_sink_pwrite_calls",
        "mapped_sink_short_writes",
        "mapped_sink_eintr_retries",
        "mapped_sink_preallocation_bytes",
        "mapped_sink_preallocation_result",
        "header_crc_bytes",
        "directory_crc_bytes",
        "manifest_crc_bytes",
        "seal_aggregate_payload_crc_bytes",
        "open_aggregate_payload_crc_bytes",
        "lazy_extent_cas_winners",
        "lazy_extent_waiters",
        "lazy_extent_executions",
        "lazy_extent_reuses",
        "lazy_extent_crc_bytes",
        "lazy_extent_state_bytes",
        "lazy_extent_invalid_receipts",
        "lazy_state_layout",
        "mapping_bytes",
        "mapping_count",
        "mapping_resourceowner_releases",
        "file_action_high_water",
        "file_action_identity_mismatches",
        "reaper_descriptors_examined",
        "reaper_relation_rows_deleted",
        "reaper_payload_bytes_deleted",
        "reaper_files_queued",
        "reaper_remaining",
        "orphan_entries_examined",
        "orphan_files_acted",
        "orphan_bytes_acted",
        "orphan_remaining",
        "orphan_cursor_wraps",
        "madvise_choice",
        "madvise_result",
    });
static_assert(k_expected_grouped_soa_provider_lifecycle_keys.size() == 50);

}  // namespace

TEST_CASE("minimizer-fragment reader stats expose one grouped-SoA schema",
          "[component:benchmark][layer:contract][feature:reporting]") {
    const auto keys = minimizer_fragment_reader_stats_keys();
    REQUIRE(keys.size() == 140);
    const std::set<std::string_view> unique_keys{
        keys.begin(), keys.end()};
    REQUIRE(unique_keys.size() == keys.size());
    for (const std::string_view key :
         k_expected_grouped_soa_provider_lifecycle_keys) {
        REQUIRE(unique_keys.contains(key));
    }
    for (const std::string_view key : {
             std::string_view{"schema"},
             std::string_view{"revision"},
             std::string_view{
                 "grouped_soa_reader_capacity_accounting_passes"},
             std::string_view{"fragment_native_peak_block_stats_capacity_bytes"},
             std::string_view{"fragment_native_block_stats_allocations"},
             std::string_view{"are_fragment_native_stats_saturated"},
             std::string_view{
                 "are_grouped_soa_reader_memory_stats_saturated"},
             std::string_view{"are_stats_saturated"},
         }) {
        REQUIRE(unique_keys.contains(key));
    }
}

TEST_CASE("E2E raw samples pin minimizer-fragment and hash-major index reader stats",
          "[component:benchmark][layer:contract][feature:reporting]") {
    const nlohmann::json grouped_soa_stats{
        {"schema", "bioseq.grouped_soa_reader_memory_stats"},
        {"revision", 6},
        {"execution_id", 41},
        {"backend_pid", 42},
        {"database_oid", 43},
        {"grouped_soa_reader_capacity_accounting_passes", 3},
        {"grouped_soa_reader_global_position_storage_preparations", 1},
        {"grouped_soa_reader_global_position_storage_allocations", 1},
        {"grouped_soa_reader_position_append_growth_reservations", 0},
        {"grouped_soa_reader_retained_position_count", 37},
        {"grouped_soa_reader_retained_position_capacity", 37},
        {"grouped_soa_reader_directory_capacity_bytes", 10},
        {"grouped_soa_reader_query_hash_capacity_bytes", 20},
        {"grouped_soa_reader_query_record_capacity_bytes", 100},
        {"grouped_soa_reader_ordinal_route_capacity_bytes", 20},
        {"grouped_soa_reader_selected_extent_capacity_bytes", 30},
        {"grouped_soa_reader_current_selected_control_capacity_bytes", 0},
        {"grouped_soa_reader_global_position_capacity_bytes", 296},
        {"grouped_soa_reader_control_payload_capacity_bytes", 0},
        {"grouped_soa_reader_owned_capacity_bytes", 476},
        {"grouped_soa_reader_admitted_bytes", 476},
        {"grouped_soa_reader_exact_position_image_checks", 1},
        {"grouped_soa_reader_exact_position_image_failures", 0},
        {"grouped_soa_reader_retained_layout_checks", 1},
        {"grouped_soa_reader_retained_layout_failures", 0},
        {"fragment_native_waves", 2},
        {"fragment_native_queries", 16},
        {"fragment_native_occurrence_fill_ns", 101},
        {"fragment_native_occurrence_radix_ns", 102},
        {"fragment_native_unique_plan_build_ns", 103},
        {"fragment_native_query_plan_radix_fill_ns", 104},
        {"fragment_native_control_count_pass_ns", 105},
        {"fragment_native_rejected_control_validation_ns", 0},
        {"fragment_native_orientation_layout_ns", 106},
        {"fragment_native_direct_emission_ns", 107},
        {"fragment_native_local_sort_ns", 108},
        {"fragment_native_tie_normalization_ns", 109},
        {"fragment_native_control_count_bytes", 2000},
        {"fragment_native_rejected_control_validation_bytes", 0},
        {"fragment_native_pass2_position_bytes", 1600},
        {"fragment_native_pass3_position_bytes", 1600},
        {"fragment_native_anchors_constructed", 100},
        {"fragment_native_local_sort_bytes", 1200},
        {"fragment_native_tie_normalization_bytes", 240},
        {"fragment_native_exact_final_bytes", 2400},
        {"fragment_native_peak_exact_final_bytes", 1200},
        {"fragment_native_global_sort_calls_invoked", 0},
        {"fragment_native_global_sort_calls_skipped", 16},
        {"fragment_native_candidate_anchor_bytes", 0},
        {"fragment_native_plan_ordinal_bytes", 0},
        {"fragment_native_compaction_bytes", 0},
        {"fragment_native_final_copy_bytes", 0},
        {"fragment_native_peak_occurrence_capacity_bytes", 400},
        {"fragment_native_peak_query_plan_capacity_bytes", 0},
        {"fragment_native_peak_query_group_offset_capacity_bytes", 48},
        {"fragment_native_peak_query_membership_filter_capacity_bytes",
            16 * 1024 * 1024},
        {"fragment_native_peak_query_radix_capacity_bytes", 600},
        {"fragment_native_peak_occurrence_count_capacity_bytes", 40},
        {"fragment_native_peak_block_layout_capacity_bytes", 800},
        {"fragment_native_peak_block_stats_capacity_bytes", 900},
        {"fragment_native_occurrence_allocations", 1},
        {"fragment_native_query_plan_allocations", 0},
        {"fragment_native_query_group_offset_allocations", 1},
        {"fragment_native_query_membership_filter_allocations", 1},
        {"fragment_native_query_radix_allocations", 3},
        {"fragment_native_occurrence_count_allocations", 4},
        {"fragment_native_block_layout_allocations", 5},
        {"fragment_native_block_stats_allocations", 6},
        {"fragment_native_query_occurrences", 64},
        {"fragment_native_unique_query_plans", 11},
        {"fragment_native_packed_occurrence_waves", 2},
        {"fragment_native_wide_occurrence_waves", 0},
        {"fragment_native_compact_direct_lookup_ns", 110},
        {"fragment_native_compact_direct_selection_shards", 32},
        {"fragment_native_compact_direct_shard_query_layouts", 512},
        {"fragment_native_compact_direct_layout_shards_completed", 32},
        {"fragment_native_compact_direct_emission_shards_completed", 32},
        {"fragment_native_compact_direct_posting_visit_passes", 1},
        {"fragment_native_compact_direct_selected_postings", 100},
        {"fragment_native_compact_direct_layout_postings_visited", 0},
        {"fragment_native_compact_direct_emission_postings_visited", 100},
        {"fragment_native_compact_direct_target_control_scan_bytes", 0},
        {"fragment_native_compact_direct_target_control_scan_passes", 0},
        {"fragment_native_compact_direct_fallbacks", 0},
        {"fragment_native_compact_direct_traversal_rank_checkpoints_examined", 0},
        {"fragment_native_compact_direct_traversal_membership_words_examined", 0},
        {"fragment_native_compact_direct_rejected_posting_reads", 0},
        {"are_fragment_native_stats_saturated", false},
        {"are_grouped_soa_reader_memory_stats_saturated", false},
        {"are_stats_saturated", false},
        {"provider_configured", 1},
        {"provider_captured", 1},
        {"provider_persisted", 1},
        {"provider_opened", 1},
        {"provider_head_scans", 1},
        {"provider_head_rows", 1},
        {"provider_descriptor_scans", 1},
        {"provider_descriptor_rows", 1},
        {"payload_tuples_formed", 0},
        {"payload_tuples_inserted", 0},
        {"payload_tuples_reread", 0},
        {"mapped_sink_calls", 1},
        {"mapped_sink_bytes", 4'096},
        {"mapped_sink_flushes", 1},
        {"mapped_sink_coalescer_capacity_bytes", 1'048'576},
        {"mapped_sink_pwrite_calls", 1},
        {"mapped_sink_short_writes", 0},
        {"mapped_sink_eintr_retries", 0},
        {"mapped_sink_preallocation_bytes", 4'096},
        {"mapped_sink_preallocation_result", 1},
        {"header_crc_bytes", 4'096},
        {"directory_crc_bytes", 128},
        {"manifest_crc_bytes", 64},
        {"seal_aggregate_payload_crc_bytes", 0},
        {"open_aggregate_payload_crc_bytes", 0},
        {"lazy_extent_cas_winners", 2},
        {"lazy_extent_waiters", 0},
        {"lazy_extent_executions", 2},
        {"lazy_extent_reuses", 4},
        {"lazy_extent_crc_bytes", 512},
        {"lazy_extent_state_bytes", 16},
        {"lazy_extent_invalid_receipts", 0},
        {"lazy_state_layout", 1},
        {"mapping_bytes", 8'192},
        {"mapping_count", 1},
        {"mapping_resourceowner_releases", 1},
        {"file_action_high_water", 2},
        {"file_action_identity_mismatches", 0},
        {"reaper_descriptors_examined", 1},
        {"reaper_relation_rows_deleted", 0},
        {"reaper_payload_bytes_deleted", 0},
        {"reaper_files_queued", 0},
        {"reaper_remaining", 0},
        {"orphan_entries_examined", 1},
        {"orphan_files_acted", 0},
        {"orphan_bytes_acted", 0},
        {"orphan_remaining", 0},
        {"orphan_cursor_wraps", 0},
        {"madvise_choice", 1},
        {"madvise_result", 1},
    };

    e2e_input input{};
    e2e_output output{};
    output.persisted_policy_audit_accepted = 1;
    output.persisted_gpu_operation_stats_schema_valid = 1;
    output.persisted_gpu_operation_stats = nlohmann::json::object();
    output.minimizer_fragment_reader_stats = grouped_soa_stats;
    const nlohmann::json hash_major_stats =
        zero_hash_major_index_reader_stats(41, 42, 43);
    output.hash_major_index_reader_stats = hash_major_stats;
    output.alignment_snapshot_rows.resize(
        e2e_output::k_alignment_snapshot_row_count);
    for (auto& row : output.alignment_snapshot_rows) {
        row.resize(e2e_output::k_alignment_snapshot_column_count);
    }

    const std::string serialized = serialize_raw_sample(
        "bioseq_contract_just_index", parameter_map{}, input, output);
    const auto payload = nlohmann::json::parse(
        serialized, nullptr, /*allow_exceptions=*/false);
    REQUIRE_FALSE(payload.is_discarded());
    REQUIRE(payload.at("payload_schema") == "bioseq.e2e.raw-sample");
    REQUIRE(payload.at("payload_schema_version")
        == e2e_raw_payload_schema_version);
    const auto& alignment_snapshot = payload.at("alignment_snapshot");
    REQUIRE(alignment_snapshot.at("schema")
        == "bioseq.last_alignment_execution_stats/raw-rowset");
    REQUIRE(alignment_snapshot.at("schema_revision") == 2);
    REQUIRE_FALSE(alignment_snapshot.contains("schema_version"));

    const auto& nested = payload.at("attestation").at(
        "minimizer_fragment_reader_stats");
    const std::set<std::string> expected_keys{
        "schema",
        "revision",
        "execution_id",
        "backend_pid",
        "database_oid",
        "grouped_soa_reader_capacity_accounting_passes",
        "grouped_soa_reader_global_position_storage_preparations",
        "grouped_soa_reader_global_position_storage_allocations",
        "grouped_soa_reader_position_append_growth_reservations",
        "grouped_soa_reader_retained_position_count",
        "grouped_soa_reader_retained_position_capacity",
        "grouped_soa_reader_directory_capacity_bytes",
        "grouped_soa_reader_query_hash_capacity_bytes",
        "grouped_soa_reader_query_record_capacity_bytes",
        "grouped_soa_reader_ordinal_route_capacity_bytes",
        "grouped_soa_reader_selected_extent_capacity_bytes",
        "grouped_soa_reader_current_selected_control_capacity_bytes",
        "grouped_soa_reader_global_position_capacity_bytes",
        "grouped_soa_reader_control_payload_capacity_bytes",
        "grouped_soa_reader_owned_capacity_bytes",
        "grouped_soa_reader_admitted_bytes",
        "grouped_soa_reader_exact_position_image_checks",
        "grouped_soa_reader_exact_position_image_failures",
        "grouped_soa_reader_retained_layout_checks",
        "grouped_soa_reader_retained_layout_failures",
        "fragment_native_waves",
        "fragment_native_queries",
        "fragment_native_occurrence_fill_ns",
        "fragment_native_occurrence_radix_ns",
        "fragment_native_unique_plan_build_ns",
        "fragment_native_query_plan_radix_fill_ns",
        "fragment_native_control_count_pass_ns",
        "fragment_native_rejected_control_validation_ns",
        "fragment_native_orientation_layout_ns",
        "fragment_native_direct_emission_ns",
        "fragment_native_local_sort_ns",
        "fragment_native_tie_normalization_ns",
        "fragment_native_control_count_bytes",
        "fragment_native_rejected_control_validation_bytes",
        "fragment_native_pass2_position_bytes",
        "fragment_native_pass3_position_bytes",
        "fragment_native_anchors_constructed",
        "fragment_native_local_sort_bytes",
        "fragment_native_tie_normalization_bytes",
        "fragment_native_exact_final_bytes",
        "fragment_native_peak_exact_final_bytes",
        "fragment_native_global_sort_calls_invoked",
        "fragment_native_global_sort_calls_skipped",
        "fragment_native_candidate_anchor_bytes",
        "fragment_native_plan_ordinal_bytes",
        "fragment_native_compaction_bytes",
        "fragment_native_final_copy_bytes",
        "fragment_native_peak_occurrence_capacity_bytes",
        "fragment_native_peak_query_plan_capacity_bytes",
        "fragment_native_peak_query_group_offset_capacity_bytes",
        "fragment_native_peak_query_membership_filter_capacity_bytes",
        "fragment_native_peak_query_radix_capacity_bytes",
        "fragment_native_peak_occurrence_count_capacity_bytes",
        "fragment_native_peak_block_layout_capacity_bytes",
        "fragment_native_peak_block_stats_capacity_bytes",
        "fragment_native_occurrence_allocations",
        "fragment_native_query_plan_allocations",
        "fragment_native_query_group_offset_allocations",
        "fragment_native_query_membership_filter_allocations",
        "fragment_native_query_radix_allocations",
        "fragment_native_occurrence_count_allocations",
        "fragment_native_block_layout_allocations",
        "fragment_native_block_stats_allocations",
        "fragment_native_query_occurrences",
        "fragment_native_unique_query_plans",
        "fragment_native_packed_occurrence_waves",
        "fragment_native_wide_occurrence_waves",
        "fragment_native_compact_direct_lookup_ns",
        "fragment_native_compact_direct_selection_shards",
        "fragment_native_compact_direct_shard_query_layouts",
        "fragment_native_compact_direct_layout_shards_completed",
        "fragment_native_compact_direct_emission_shards_completed",
        "fragment_native_compact_direct_posting_visit_passes",
        "fragment_native_compact_direct_selected_postings",
        "fragment_native_compact_direct_layout_postings_visited",
        "fragment_native_compact_direct_emission_postings_visited",
        "fragment_native_compact_direct_target_control_scan_bytes",
        "fragment_native_compact_direct_target_control_scan_passes",
        "fragment_native_compact_direct_fallbacks",
        "fragment_native_compact_direct_traversal_rank_checkpoints_examined",
        "fragment_native_compact_direct_traversal_membership_words_examined",
        "fragment_native_compact_direct_rejected_posting_reads",
        "are_fragment_native_stats_saturated",
        "are_grouped_soa_reader_memory_stats_saturated",
        "are_stats_saturated",
        "provider_configured",
        "provider_captured",
        "provider_persisted",
        "provider_opened",
        "provider_head_scans",
        "provider_head_rows",
        "provider_descriptor_scans",
        "provider_descriptor_rows",
        "payload_tuples_formed",
        "payload_tuples_inserted",
        "payload_tuples_reread",
        "mapped_sink_calls",
        "mapped_sink_bytes",
        "mapped_sink_flushes",
        "mapped_sink_coalescer_capacity_bytes",
        "mapped_sink_pwrite_calls",
        "mapped_sink_short_writes",
        "mapped_sink_eintr_retries",
        "mapped_sink_preallocation_bytes",
        "mapped_sink_preallocation_result",
        "header_crc_bytes",
        "directory_crc_bytes",
        "manifest_crc_bytes",
        "seal_aggregate_payload_crc_bytes",
        "open_aggregate_payload_crc_bytes",
        "lazy_extent_cas_winners",
        "lazy_extent_waiters",
        "lazy_extent_executions",
        "lazy_extent_reuses",
        "lazy_extent_crc_bytes",
        "lazy_extent_state_bytes",
        "lazy_extent_invalid_receipts",
        "lazy_state_layout",
        "mapping_bytes",
        "mapping_count",
        "mapping_resourceowner_releases",
        "file_action_high_water",
        "file_action_identity_mismatches",
        "reaper_descriptors_examined",
        "reaper_relation_rows_deleted",
        "reaper_payload_bytes_deleted",
        "reaper_files_queued",
        "reaper_remaining",
        "orphan_entries_examined",
        "orphan_files_acted",
        "orphan_bytes_acted",
        "orphan_remaining",
        "orphan_cursor_wraps",
        "madvise_choice",
        "madvise_result",
    };
    REQUIRE(expected_keys.size() == 140);
    std::set<std::string> declared_keys;
    for (const std::string_view key :
         minimizer_fragment_reader_stats_keys()) {
        declared_keys.emplace(key);
    }
    REQUIRE(declared_keys == expected_keys);
    std::set<std::string> actual_keys;
    for (const auto& [key, value] : nested.items()) {
        (void)value;
        actual_keys.insert(key);
    }
    REQUIRE(actual_keys == expected_keys);
    REQUIRE(nested.size() == 140);
    REQUIRE(nested.at("schema")
            == "bioseq.grouped_soa_reader_memory_stats");
    REQUIRE(nested.at("revision") == 6);
    REQUIRE(nested.at("fragment_native_query_occurrences").get<std::int64_t>()
        >= nested.at("fragment_native_unique_query_plans").get<std::int64_t>());
    REQUIRE(nested.at("fragment_native_packed_occurrence_waves")
                .get<std::int64_t>()
            + nested.at("fragment_native_wide_occurrence_waves")
                .get<std::int64_t>()
        == nested.at("fragment_native_waves").get<std::int64_t>());
    REQUIRE(nested == grouped_soa_stats);
    const auto& hash_major_nested = payload.at("attestation").at(
        "hash_major_index_reader_stats");
    REQUIRE(hash_major_nested.size() == 47);
    REQUIRE(hash_major_nested.at("schema")
            == "bioseq.hash_major_reader_stats");
    REQUIRE(hash_major_nested.at("revision") == 1);
    REQUIRE(hash_major_nested == hash_major_stats);
}

TEST_CASE("E2E publishes fragment-native stats as configured flat metrics",
          "[component:benchmark][layer:contract][feature:reporting]") {
    e2e_output output{};
    std::int64_t next_value = 1;
    for (const auto& [key, member] : k_fragment_native_flat_metric_fields) {
        output.*member = key == "are_fragment_native_stats_saturated"
            ? 1
            : next_value++;
    }

    const e2e_input input{};
    const parameter_map params{};
    const auto metrics = evaluate(
        "grouped-soa-test", params, input, output);
    for (const auto& [key, member] : k_fragment_native_flat_metric_fields) {
        const std::string metric_name{key};
        REQUIRE(metrics.contains(metric_name));
        REQUIRE(metrics.at(metric_name)
            == static_cast<double>(output.*member));
    }

    const auto config_path = std::filesystem::path{BIOSEQ_E2E_DATA_DIR}
        / "bench_e2e_grouped_soa_minimizer_fragment_bioseq_just_index_only.json";
    const auto config = load_e2e_contract_config(config_path);
    const auto& configured_metrics =
        config.at("bench_e2e").at("matrix").at("metrics");
    for (const auto& [key, member] : k_fragment_native_flat_metric_fields) {
        (void)member;
        REQUIRE(configured_metrics.contains(std::string{key}));
    }
    for (const std::string_view key :
         k_expected_grouped_soa_provider_lifecycle_keys) {
        REQUIRE(configured_metrics.contains(std::string{key}));
    }
}

TEST_CASE("E2E publishes grouped-SoA provider lifecycle stats",
          "[component:benchmark][layer:contract][feature:reporting]") {
    nlohmann::json stats = nlohmann::json::object();
    for (const std::string_view key : minimizer_fragment_reader_stats_keys()) {
        stats[std::string{key}] = 0;
    }
    stats["schema"] = "bioseq.grouped_soa_reader_memory_stats";
    stats["revision"] = 6;
    stats["are_fragment_native_stats_saturated"] = false;
    stats["are_grouped_soa_reader_memory_stats_saturated"] = false;
    stats["are_stats_saturated"] = false;
    std::int64_t expected = 1;
    for (const std::string_view key :
         k_expected_grouped_soa_provider_lifecycle_keys) {
        stats[std::string{key}] = expected++;
    }

    e2e_output output{};
    output.minimizer_fragment_reader_stats = std::move(stats);
    const auto metrics = evaluate(
        "grouped-soa-provider-test", parameter_map{}, e2e_input{}, output);
    expected = 1;
    for (const std::string_view key :
         k_expected_grouped_soa_provider_lifecycle_keys) {
        const auto found = metrics.find(std::string{key});
        REQUIRE(found != metrics.end());
        REQUIRE(found->second == static_cast<double>(expected++));
    }
}

TEST_CASE("E2E algorithm-matrix session SQL isolates stage selection from target construction",
          "[component:benchmark][layer:contract][feature:configuration]") {
    struct route_case {
        std::string_view label;
        std::string_view minimizer;
        std::string_view chaining;
        bool vulkan_seed;
    };
    constexpr std::array<route_case, 4> routes{{
        {"host", "bounded_nthash_simd", "simd_minimap2", false},
        {"seed-only", "vulkan_minimap_integer", "simd_minimap2", true},
        {"chain-only", "bounded_nthash_simd",
         "vulkan_tiled_dynamic_programming", false},
        {"seed-and-chain", "vulkan_minimap_integer",
         "vulkan_tiled_dynamic_programming", true},
    }};

    for (const auto& route : routes) {
        for (const bool build_target : std::array{false, true}) {
          for (const std::string_view minimizer_index : std::array{
                   std::string_view{"minimizers_per_fragment"},
                   std::string_view{"minimizers_per_sequence"}}) {
            CAPTURE(route.label, build_target, minimizer_index);
            const auto sql = algorithm_matrix_session_sql(
                15,
                10,
                route.minimizer,
                route.chaining,
                "block_aligner",
                "task_parallel",
                minimizer_index,
                build_target);

            REQUIRE(sql.size() == 22);
            CHECK(sql[0] == "SET bioseq.planner.custom_scan.enabled = on;");
            CHECK(sql[1] == "SET bioseq.planner.index_scan.enabled = on;");
            CHECK(sql[2] == "SET bioseq.vectorization.enabled = on;");
            CHECK(sql[3]
                == "SET bioseq.scheduler.pipeline.mode = 'task_parallel';");
            CHECK(sql[4]
                == "SET bioseq.aligner.base_alignment.algorithm = "
                   "'block_aligner';");
            CHECK(sql[5]
                == std::string{"SET bioseq.aligner.chaining.algorithm = '"}
                    .append(route.chaining)
                    .append("';"));
            CHECK(sql[6]
                == std::string{"SET bioseq.aligner.minimizer.algorithm = '"}
                    .append(route.minimizer)
                    .append("';"));
            CHECK(sql[7]
                == "SET bioseq.aligner.minimizer.kmer_size = 15;");
            CHECK(sql[8]
                == "SET bioseq.aligner.minimizer.window_size = 10;");
            CHECK(sql[9]
                == "SET bioseq.aligner.grouped_soa_generation.enabled = on;");
            CHECK(sql[10] == "SET bioseq.aligner.vulkan.enabled = on;");
            CHECK(sql[11]
                == (route.vulkan_seed
                    ? "SET bioseq.aligner.vulkan.seed_lookup.enabled = on;"
                    : "SET bioseq.aligner.vulkan.seed_lookup.enabled = off;"));
            CHECK(sql[12]
                == "SET bioseq.aligner.vulkan.index_cache.enabled = on;");
            CHECK(sql[13]
                == "SET bioseq.aligner.vulkan.fused_frontend.enabled = off;");
            CHECK(sql[14]
                == "SET bioseq.aligner.vulkan.alignment_extension.enabled = off;");
            CHECK(sql[15]
                == (route.vulkan_seed && build_target
                    ? "SET bioseq.aligner.vulkan.index_build.enabled = on;"
                    : "SET bioseq.aligner.vulkan.index_build.enabled = off;"));
            CHECK(sql[16]
                == (minimizer_index == "minimizers_per_fragment"
                    ? "SET bioseq.aligner.fragment_minimizer.lookup_route = "
                      "'direct_compact_hash_lookup';"
                    : "SET bioseq.aligner.fragment_minimizer.lookup_route = "
                      "'control_block_scan';"));
            CHECK(sql[17] == "SET bioseq.column_cache.enabled = on;");
            CHECK(sql[18]
                == "SET bioseq.index.async_persistence.enabled = off;");
            CHECK(sql[19]
                == "SET bioseq.index.recompute_async_persistence.enabled = off;");
            CHECK(sql[20]
                == "SET bioseq.index.insert_maintenance.enabled = off;");
            CHECK(sql[21]
                == (build_target
                    ? "SET bioseq.index.sequence_read.enabled = off;"
                    : "SET bioseq.index.sequence_read.enabled = on;"));
          }
        }
    }
}

TEST_CASE("ordinary Bioseq and minimap2 matrix rows share the exact iteration boundary",
          "[component:benchmark][layer:contract][feature:measurement_boundary]") {
    for (const std::string_view algorithm : std::array{
             std::string_view{"bioseq_algorithm_matrix_no_index"},
             std::string_view{"bioseq_algorithm_matrix_just_index"},
             std::string_view{"minimap2_no_index"},
             std::string_view{"minimap2_just_index"}}) {
        CAPTURE(algorithm);
        CHECK(ordinary_algorithm_uses_iteration_boundaries(
            algorithm, /*paired_minimap2_oracle=*/false));
        CHECK_FALSE(ordinary_algorithm_uses_iteration_boundaries(
            algorithm, /*paired_minimap2_oracle=*/true));
        CHECK_FALSE(ordinary_algorithm_uses_iteration_boundaries(
            algorithm, /*paired_minimap2_oracle=*/false,
            /*algorithm_matrix_measurement_scope=*/false));
    }
    CHECK_FALSE(ordinary_algorithm_uses_iteration_boundaries(
        "bioseq_legacy_scalar_no_index", /*paired_minimap2_oracle=*/false));
}

TEST_CASE("E2E config identity gate preserves sealed campaign families",
          "[component:benchmark][layer:contract][feature:configuration]") {
    const auto q1_manifest_path = std::filesystem::path{BIOSEQ_E2E_DATA_DIR}
        / "bench_e2e_algorithm_matrix_q1_calibration_manifest.json";
    const auto q1_manifest = load_e2e_contract_config(q1_manifest_path);
    REQUIRE(q1_manifest.at("schema_version") == 2);
    REQUIRE(q1_manifest.at("panel_contract").at("result_cells") == 5);
    REQUIRE(q1_manifest.at("panel_contract").at("expected_raw_records") == 25);
    REQUIRE(q1_manifest.at("nonpublication_rule").at("merge_authority")
        == "none");
    REQUIRE(q1_manifest.at("cells").size() == 5);
    std::set<std::string> q1_paths;
    for (const auto& cell : q1_manifest.at("cells")) {
        const std::string path = cell.at("path").get<std::string>();
        REQUIRE(q1_paths.insert(path).second);
        REQUIRE(is_canonical_campaign_e2e_config_identity(path));
    }
    REQUIRE(q1_paths.size() == 5);

    const auto deadline_manifest_path = std::filesystem::path{BIOSEQ_E2E_DATA_DIR}
        / "bench_e2e_algorithm_matrix_deadline_manifest.json";
    const auto deadline_manifest = load_e2e_contract_config(
        deadline_manifest_path);
    REQUIRE(deadline_manifest.at("version") == 1);
    REQUIRE(deadline_manifest.at("campaign").at("expected_shards") == 59);
    REQUIRE(deadline_manifest.at("campaign").at("expected_raw_records") == 295);
    REQUIRE(deadline_manifest.at("schedule").size() == 59);
    for (const auto& shard : deadline_manifest.at("schedule")) {
        REQUIRE(is_canonical_campaign_e2e_config_identity(
            shard.at("path").get<std::string>()));
    }

    const auto deadline_v2_manifest_path =
        std::filesystem::path{BIOSEQ_E2E_DATA_DIR}
        / "bench_e2e_algorithm_matrix_deadline_v2_manifest.json";
    const auto deadline_v2_manifest = load_e2e_contract_config(
        deadline_v2_manifest_path);
    REQUIRE(deadline_v2_manifest.at("version") == 2);
    REQUIRE(deadline_v2_manifest.at("campaign").at("expected_shards") == 60);
    REQUIRE(deadline_v2_manifest.at("campaign").at("expected_raw_records") == 300);
    REQUIRE(deadline_v2_manifest.at("runtime_contract").at(
        "extended_timeout_shard_count") == 2);
    REQUIRE(deadline_v2_manifest.at("schedule").size() == 60);
    for (const auto& shard : deadline_v2_manifest.at("schedule")) {
        REQUIRE(is_canonical_campaign_e2e_config_identity(
            shard.at("path").get<std::string>()));
    }

    REQUIRE(is_canonical_campaign_e2e_config_identity(
        "bench/bench_e2e_algorithm_matrix_campaign.json"));
    REQUIRE(is_canonical_campaign_e2e_config_identity(
        "bench/e2e_algorithm_matrix_shards/"
        "bench_e2e_algorithm_matrix_campaign_shard_"
        "m2_d4_k15_w10_no_index_baselines.json"));
    REQUIRE(is_canonical_campaign_e2e_config_identity(
        "bench/e2e_algorithm_matrix_shards/"
        "bench_e2e_algorithm_matrix_campaign_shard_singleton_"
        "0081_423ec685980909f5.json"));
    REQUIRE(is_canonical_campaign_e2e_config_identity(
        "bench/bench_e2e_algorithm_matrix_q8_campaign.json"));
    REQUIRE(is_canonical_campaign_e2e_config_identity(
        "bench/e2e_algorithm_matrix_q8_shards/"
        "bench_e2e_algorithm_matrix_q8_campaign_shard_"
        "m2_d4_k15_w10_isolated_minimap2_no_index.json"));

    REQUIRE_FALSE(is_canonical_campaign_e2e_config_identity(
        "bench/arbitrary_campaign.json"));
    REQUIRE_FALSE(is_canonical_campaign_e2e_config_identity(
        "bench/e2e_algorithm_matrix_q8_shards/arbitrary.json"));
    REQUIRE_FALSE(is_canonical_campaign_e2e_config_identity(
        "bench/e2e_algorithm_matrix_q8_shards/"
        "bench_e2e_algorithm_matrix_q8_campaign_shard_.json"));
    REQUIRE_FALSE(is_canonical_campaign_e2e_config_identity(
        "bench/e2e_algorithm_matrix_q8_shards/"
        "bench_e2e_algorithm_matrix_q8_campaign_shard_../escape.json"));
    REQUIRE_FALSE(is_canonical_campaign_e2e_config_identity(
        "bench/e2e_algorithm_matrix_q8_shards/"
        "bench_e2e_algorithm_matrix_q8_campaign_shard_M2.json"));
    REQUIRE_FALSE(is_canonical_campaign_e2e_config_identity(
        "bench/e2e_algorithm_matrix_shards/"
        "bench_e2e_algorithm_matrix_campaign_shard_singleton_"
        "0081_423EC685980909F5.json"));
    REQUIRE_FALSE(is_canonical_campaign_e2e_config_identity(
        "bench/e2e_algorithm_matrix_q1_calibration_shards/"
        "bench_e2e_algorithm_matrix_q1_calibration_m2_d4_k15_w10_"
        "no_index_scalar_scalar_minimap2_sequence_ksw2_copy.json"));
    REQUIRE_FALSE(is_canonical_campaign_e2e_config_identity(
        "bench/e2e_algorithm_matrix_q1_calibration_shards/../"
        "bench_e2e_algorithm_matrix_q1_calibration_manifest.json"));
    REQUIRE_FALSE(is_canonical_campaign_e2e_config_identity(
        "/absolute/bench/e2e_algorithm_matrix_q1_calibration_shards/"
        "bench_e2e_algorithm_matrix_q1_calibration_m2_d4_k15_w10_"
        "no_index_scalar_scalar_minimap2_sequence_ksw2.json"));
    REQUIRE_FALSE(is_canonical_campaign_e2e_config_identity(
        "bench/e2e_algorithm_matrix_deadline_v2_shards/arbitrary.json"));
    REQUIRE_FALSE(is_canonical_campaign_e2e_config_identity(
        "bench/e2e_algorithm_matrix_deadline_v2_shards/../"
        "bench_e2e_algorithm_matrix_deadline_v2_manifest.json"));
}

TEST_CASE("E2E input setup follows exact expanded execution tuples",
          "[component:benchmark][layer:contract][feature:selection]") {
    const auto grouped_soa_path = std::filesystem::path{BIOSEQ_E2E_DATA_DIR}
        / "bench_e2e_grouped_soa_minimizer_fragment_simd_minimap2_just_index_focus.json";
    const auto grouped_soa_config = load_e2e_contract_config(grouped_soa_path);
    const auto grouped_soa_constraints = exact_e2e_execution_constraints(
        grouped_soa_config, "bench_e2e");
    REQUIRE(grouped_soa_constraints.execution_tuples.size() == 3);
    REQUIRE(grouped_soa_config.at("test_cases")
                .at("E2E_2GBP_DNA4").at("data")
                .at("alphabet") == "dna4");
    REQUIRE(grouped_soa_config.at("test_cases")
                .at("E2E_2GBP_DNA4").at("data")
                .at("kmer_size") == 15);
    REQUIRE(grouped_soa_config.at("test_cases")
                .at("E2E_2GBP_DNA4").at("data")
                .at("window_size") == 10);
    REQUIRE(grouped_soa_config.at("bench_e2e").at("matrix").at("for")
                .at("E2E_2GBP_DNA4") == 6);
    const auto grouped_soa =
        resolve_just_index_setup_requirements(grouped_soa_constraints);
    REQUIRE(grouped_soa.simd_per_fragment);
    REQUIRE(grouped_soa.minimap2);
    REQUIRE_FALSE(grouped_soa.scalar_per_fragment);
    REQUIRE_FALSE(grouped_soa.scalar_per_sequence);
    REQUIRE_FALSE(grouped_soa.simd_per_sequence);
    REQUIRE_FALSE(grouped_soa.gpu_per_fragment);
    REQUIRE_FALSE(grouped_soa.gpu_per_sequence);

    const auto focus_path = std::filesystem::path{BIOSEQ_E2E_DATA_DIR}
        / "bench_e2e_grouped_soa_simd_minimap2_just_index_focus.json";
    const auto focus_config = load_e2e_contract_config(focus_path);
    const auto focus_constraints = exact_e2e_execution_constraints(
        focus_config, "bench_e2e");
    REQUIRE(focus_constraints.execution_tuples.size() == 4);
    REQUIRE(focus_config.at("test_cases").at("E2E_2GBP_DNA4")
                .at("data").at("just_index_setup_only")
                .get<std::string>()
            == minimizers_per_fragment_index());

    const auto focus =
        resolve_just_index_setup_requirements(focus_constraints);
    REQUIRE(focus.simd_per_fragment);
    REQUIRE(focus.minimap2);
    REQUIRE_FALSE(focus.scalar_per_fragment);
    REQUIRE_FALSE(focus.scalar_per_sequence);
    REQUIRE_FALSE(focus.simd_per_sequence);
    REQUIRE_FALSE(focus.gpu_per_fragment);
    REQUIRE_FALSE(focus.gpu_per_sequence);

    const auto default_config = load_e2e_contract_config(
        std::filesystem::path{BIOSEQ_E2E_CONFIG_PATH});
    for (const std::string_view benchmark :
         {std::string_view{"bench_e2e"},
          std::string_view{"bench_e2e_signoff"}}) {
        const auto full = resolve_just_index_setup_requirements(
            exact_e2e_execution_constraints(default_config, benchmark));
        REQUIRE(full.simd_per_fragment);
        REQUIRE(full.gpu_per_fragment);
        REQUIRE(full.minimap2);
        REQUIRE_FALSE(full.scalar_per_fragment);
        REQUIRE_FALSE(full.scalar_per_sequence);
        REQUIRE_FALSE(full.simd_per_sequence);
        REQUIRE_FALSE(full.gpu_per_sequence);
    }

    run_constraints no_index_only{};
    no_index_only.execution_tuples = {
        {.algorithm_name =
             "bioseq_block_simd_dp_two_stack_nthash_no_index"},
        {.algorithm_name = "bioseq_block_gpu_dp_gpu_seed_no_index"},
        {.algorithm_name = "minimap2_no_index"},
    };
    const auto absent = resolve_just_index_setup_requirements(no_index_only);
    REQUIRE_FALSE(absent.any_bioseq());
    REQUIRE_FALSE(absent.minimap2);
}

TEST_CASE("Persisted matrix setup admits supported DNA4 and DNA5 Vulkan profiles",
          "[component:benchmark][layer:contract][feature:selection]") {
    e2e_input input{};
    input.size_label = "matrix_2gbp_dna4_k15_w10";
    input.alphabet = "dna4";
    input.kmer_size = 15;
    input.window_size = 10;
    input.cigar_projection = true;
    input.mapq_projection = true;
    input.prefilter_enabled = false;
    REQUIRE_FALSE(persisted_setup_must_reject(
        input, "vulkan_minimap_integer", minimizers_per_fragment_index()));

    input.size_label = "matrix_2gbp_dna4_k20_w15";
    input.kmer_size = 20;
    input.window_size = 15;
    REQUIRE_FALSE(persisted_setup_must_reject(
        input, "vulkan_minimap_integer", minimizers_per_fragment_index()));

    input.size_label = "matrix_2gbp_dna5_k15_w10";
    input.alphabet = "dna5";
    input.kmer_size = 15;
    input.window_size = 10;
    REQUIRE_FALSE(persisted_setup_must_reject(
        input, "vulkan_minimap_integer", minimizers_per_fragment_index()));
}

TEST_CASE("Sliced E2E config activates the fully attested SIMD pipeline",
          "[component:benchmark][layer:contract][feature:configuration]") {
    const auto config_path = std::filesystem::path{BIOSEQ_E2E_DATA_DIR}
        / "bench_e2e_lazy_slice_mmsearch.json";
    const auto config = load_e2e_contract_config(config_path);
    const auto& data = config.at("test_cases")
        .at("E2E_LAZY_SLICE_DNA4").at("data");
    REQUIRE(data.at("cigar_projection").get<bool>());
    REQUIRE(is_sliced_pipeline_stats_configuration_valid(
        sliced_no_index_algorithm,
        data.at("cigar_projection").get<bool>()));
    REQUIRE_FALSE(is_sliced_pipeline_stats_configuration_valid(
        sliced_no_index_algorithm,
        /*cigar_projection=*/false));
    REQUIRE(is_sliced_pipeline_stats_configuration_valid(
        "bioseq_block_simd_dp_two_stack_nthash_no_index",
        /*cigar_projection=*/false));

    e2e_input input{};
    input.cigar_projection = true;
    const std::string projection = projection_sql(input);
    REQUIRE(projection.contains("bioseq.cigar(header) AS cigar"));
    REQUIRE(projection.contains("bioseq.target_end(header) AS tend"));

    const std::string sql = batched_transformed_coordinate_sql(
        "transformed_coordinate_target", projection, 1, 4096);
    REQUIRE(sql.contains("t.seq[4096:4112]"));
    REQUIRE(sql.contains("OPERATOR(bioseq.||) t.seq[4112:]"));
    REQUIRE(sql.contains("q.seq[0:]"));
}

TEST_CASE("E2E primary rows require strand and executable CIGAR replay",
          "[component:benchmark][layer:contract][feature:e2e_driver]") {
    e2e_input input{};
    input.alphabet = "dna4";
    input.cigar_projection = true;
    input.queries.push_back(e2e_query_truth{
        .id = 0,
        .bases = "ACGT",
        .truth_target_start = 10,
        .truth_target_end = 14,
        .truth_is_reverse = false,
        .target_window_start = 10,
        .target_window_bases = "ACGT",
    });

    const rowset bioseq_rows{{
        std::string{"0"}, std::string{"target"}, std::string{"10"},
        std::string{"14"}, std::string{"0"}, std::string{"4"},
        std::string{"f"}, std::string{"t"}, std::string{"4="},
    }};
    e2e_output bioseq{};
    parse_bioseq_rows(bioseq_rows, input, bioseq);
    REQUIRE(bioseq.found);
    REQUIRE(bioseq.exact_primary_count_accepted == 1);
    REQUIRE(bioseq.correct_strand_count == 1);
    REQUIRE(bioseq.cigar_replay_count == 1);

    e2e_output minimap{};
    parse_minimap2_rows(
        "query0\t4\t0\t4\t+\tref0\t20\t10\t14\t4\t4\t60\t"
        "tp:A:P\tcg:Z:4=\n",
        input,
        minimap);
    REQUIRE(minimap.found);
    REQUIRE(minimap.exact_primary_count_accepted == 1);
    REQUIRE(minimap.correct_strand_count == 1);
    REQUIRE(minimap.cigar_replay_count == 1);

    e2e_output extra{};
    parse_minimap2_rows(
        "query0\t4\t0\t4\t+\tref0\t20\t10\t14\t4\t4\t60\t"
        "tp:A:P\tcg:Z:4=\n"
        "query0\t4\t0\t4\t+\tref0\t20\t10\t14\t4\t4\t60\t"
        "tp:A:S\tcg:Z:4=\n",
        input,
        extra);
    REQUIRE_FALSE(extra.found);
    REQUIRE(extra.exact_primary_count_accepted == 0);
}

TEST_CASE("E2E paired oracle blocks on exact counts and diagnoses MAPQ only",
          "[component:benchmark][layer:contract][feature:e2e_driver]") {
    e2e_input input{};
    input.alphabet = "dna4";
    input.cigar_projection = true;
    input.mapq_projection = true;
    input.paired_minimap2_oracle = true;
    input.queries = {
        e2e_query_truth{
            .id = 0,
            .bases = "ACGT",
            .truth_target_start = 10,
            .truth_target_end = 14,
            .truth_is_reverse = false,
            .target_window_start = 10,
            .target_window_bases = "ACGT",
        },
        e2e_query_truth{
            .id = 1,
            .bases = "TGCA",
            .truth_target_start = 30,
            .truth_target_end = 34,
            .truth_is_reverse = false,
            .target_window_start = 30,
            .target_window_bases = "TGCA",
        },
    };

    const rowset bioseq_rows{
        {
            std::string{"0"}, std::string{"target"}, std::string{"10"},
            std::string{"14"}, std::string{"0"}, std::string{"4"},
            std::string{"f"}, std::string{"t"}, std::string{"4="},
            std::string{"60"},
        },
        {
            std::string{"1"}, std::string{"target"}, std::string{"30"},
            std::string{"34"}, std::string{"0"}, std::string{"4"},
            std::string{"f"}, std::string{"t"}, std::string{"4="},
            std::string{"47"},
        },
    };
    e2e_output bioseq{};
    parse_bioseq_rows(bioseq_rows, input, bioseq);

    e2e_output minimap{};
    parse_minimap2_rows(
        "query0\t4\t0\t4\t+\tref0\t100\t10\t14\t4\t4\t60\t"
        "tp:A:P\tcg:Z:4=\n"
        "query1\t4\t0\t4\t+\tref0\t100\t30\t34\t4\t4\t47\t"
        "tp:A:P\tcg:Z:4=\n",
        input,
        minimap);

    REQUIRE(bioseq.ordered_query_oracle_available == 1);
    REQUIRE(minimap.ordered_query_oracle_available == 1);
    REQUIRE(bioseq.ordered_query_oracle == minimap.ordered_query_oracle);
    REQUIRE(bioseq.ordered_query_oracle_sha256
        == minimap.ordered_query_oracle_sha256);
    REQUIRE(bioseq.ordered_query_oracle_sha256_words
        == minimap.ordered_query_oracle_sha256_words);
    finalize_paired_oracle_repeat(input, 0, bioseq, minimap);
    REQUIRE(bioseq.paired_oracle_comparison_available == 1);
    REQUIRE(bioseq.paired_oracle_exact == 1);
    REQUIRE(bioseq.paired_oracle_verdict_sealed == 1);
    REQUIRE(minimap.paired_oracle_comparison_available == 1);
    REQUIRE(minimap.paired_oracle_exact == 1);
    REQUIRE(minimap.paired_oracle_verdict_sealed == 1);
    REQUIRE(bioseq.paired_mapq_vector_equal == 1);
    REQUIRE(minimap.paired_mapq_vector_equal == 1);

    e2e_output different_mapq{};
    parse_minimap2_rows(
        "query0\t4\t0\t4\t+\tref0\t100\t10\t14\t4\t4\t60\t"
        "tp:A:P\tcg:Z:4=\n"
        "query1\t4\t0\t4\t+\tref0\t100\t30\t34\t4\t4\t48\t"
        "tp:A:P\tcg:Z:4=\n",
        input,
        different_mapq);
    e2e_output diagnostic_bioseq = bioseq;
    finalize_paired_oracle_repeat(
        input, 1, diagnostic_bioseq, different_mapq);
    REQUIRE(diagnostic_bioseq.paired_oracle_exact == 1);
    REQUIRE(different_mapq.paired_oracle_exact == 1);
    REQUIRE(diagnostic_bioseq.tuple_accepted == 1);
    REQUIRE(different_mapq.tuple_accepted == 1);
    REQUIRE(diagnostic_bioseq.paired_mapq_vector_equal == 0);
    REQUIRE(different_mapq.paired_mapq_vector_equal == 0);
    REQUIRE(diagnostic_bioseq.behavior_difference_comment.find(
        "numeric minimap2 MAPQ equivalence is not a validity requirement")
        != std::string::npos);
    REQUIRE(different_mapq.ordered_query_oracle_sha256
        == diagnostic_bioseq.ordered_query_oracle_sha256);
    REQUIRE(different_mapq.ordered_query_mapq_sha256
        != diagnostic_bioseq.ordered_query_mapq_sha256);

    e2e_output mismatching_bioseq = bioseq;
    e2e_output mismatching_minimap = minimap;
    --mismatching_minimap.match_count;
    --mismatching_minimap.ordered_query_oracle[1].primary_match_count;
    finalize_paired_oracle_repeat(
        input, 2, mismatching_bioseq, mismatching_minimap);
    REQUIRE(mismatching_bioseq.paired_oracle_comparison_available == 1);
    REQUIRE(mismatching_bioseq.paired_oracle_exact == 0);
    REQUIRE(mismatching_bioseq.tuple_accepted == 0);
    REQUIRE(mismatching_minimap.tuple_accepted == 0);
    REQUIRE(mismatching_bioseq.cell_disposition == "invalid");
    REQUIRE(mismatching_minimap.cell_disposition == "invalid");
    REQUIRE(mismatching_bioseq.tuple_rejection_reason.starts_with(
        "**INVALID**: paired minimap2 oracle"));
    REQUIRE(mismatching_bioseq.tuple_rejection_reason.find(
        "total primary match-count mismatch")
        != std::string::npos);
    REQUIRE(mismatching_minimap.tuple_rejection_reason
        == mismatching_bioseq.tuple_rejection_reason);

    e2e_output ordered_mismatch_bioseq = bioseq;
    e2e_output ordered_mismatch_minimap = minimap;
    --ordered_mismatch_minimap.ordered_query_oracle[0].primary_match_count;
    ++ordered_mismatch_minimap.ordered_query_oracle[1].primary_match_count;
    finalize_paired_oracle_repeat(
        input, 3, ordered_mismatch_bioseq, ordered_mismatch_minimap);
    REQUIRE(ordered_mismatch_bioseq.match_count
        == ordered_mismatch_minimap.match_count);
    REQUIRE(ordered_mismatch_bioseq.tuple_accepted == 0);
    REQUIRE(ordered_mismatch_minimap.tuple_accepted == 0);
    REQUIRE(ordered_mismatch_bioseq.tuple_rejection_reason.find(
        "ordered_index=0 field=primary_match_count") != std::string::npos);
    REQUIRE(ordered_mismatch_minimap.tuple_rejection_reason
        == ordered_mismatch_bioseq.tuple_rejection_reason);
}

TEST_CASE("E2E full Vulkan tuple remains complete with MAPQ projection",
          "[component:benchmark][layer:contract][feature:e2e_driver]") {
    e2e_input input{};
    input.cigar_projection = true;
    input.mapq_projection = true;

    e2e_output output{};
    output.requested_minimizer_algorithm = "vulkan_minimap_integer";
    output.requested_chaining_algorithm =
        "vulkan_tiled_dynamic_programming";
    output.requested_base_alignment_algorithm = "block_aligner";

    REQUIRE(is_complete_vulkan_algorithm_matrix_tuple(input, output));

    output.requested_chaining_algorithm = "simd_minimap2";
    REQUIRE_FALSE(is_complete_vulkan_algorithm_matrix_tuple(input, output));
}

TEST_CASE("E2E labels Vulkan simple CIGAR as a visible behavior difference",
          "[component:benchmark][layer:contract][feature:reporting]") {
    e2e_input input{};
    input.mapq_projection = true;
    e2e_output output{};
    output.actual_backend = "vulkan_gpu";
    output.requested_minimizer_algorithm = "vulkan_minimap_integer";
    output.requested_chaining_algorithm =
        "vulkan_tiled_dynamic_programming";
    output.requested_base_alignment_algorithm = "block_aligner";
    output.observed_base_alignment_algorithm = "vulkan_simple_cigar";
    output.mapq_execution_available = 1;
    output.mapq_executed = 1;

    annotate_behavior_differences(input, output);

    REQUIRE(output.tuple_accepted == 1);
    REQUIRE(output.cell_disposition == "accepted_behavior_difference");
    REQUIRE(output.behavior_difference_comment.find(
        "distinct Vulkan-native CIGAR implementation") != std::string::npos);
    REQUIRE(output.behavior_difference_comment.find(
        "rather than a fallback") != std::string::npos);
    REQUIRE(output.behavior_difference_comment.find(
        "Vulkan MAPQ was physically executed") != std::string::npos);
    REQUIRE(output.behavior_difference_comment.find(
        "does not claim minimap2 MAPQ semantics") != std::string::npos);
}

TEST_CASE("Persisted GPU operation evidence pins the MAPQ schema-33 suffix",
          "[component:benchmark][layer:contract][feature:reporting]") {
    const auto keys = persisted_gpu_operation_keys();
    REQUIRE(keys.size() == 33);
    REQUIRE(keys[13] == "alignment_submission_count");
    REQUIRE(keys[14] == "mapq_pipeline_receipt_count");
    REQUIRE(keys[15] == "mapq_submission_count");
    REQUIRE(keys[16] == "mapq_result_count");
    REQUIRE(keys[17] == "mapq_published_count");
    REQUIRE(keys[18] == "mapq_result_digest");
    REQUIRE(keys[19] == "submission_count");

    e2e_output hybrid{};
    hybrid.persisted_gpu_operation_stats_schema_valid = 1;
    hybrid.persisted_gpu_operation_stats_accepted = 0;
    hybrid.persisted_gpu_operation_stats_complete = 1;
    hybrid.persisted_gpu_operation_stats_saturated = 0;
    hybrid.persisted_gpu_dropped_operation_count = 0;
    hybrid.persisted_gpu_operation_count = 0;
    hybrid.persisted_gpu_accepted_operation_count = 0;
    hybrid.persisted_gpu_job_count = 0;
    hybrid.persisted_gpu_accepted_job_count = 0;
    REQUIRE(hybrid.has_exact_empty_persisted_gpu_operation_ledger());
    REQUIRE_FALSE(
        hybrid.has_accepted_legacy_persisted_gpu_operation_ledger(7));

    SECTION("hybrid rejects malformed or nonempty legacy ledger state") {
        auto malformed = hybrid;
        malformed.persisted_gpu_operation_stats_schema_valid = 0;
        REQUIRE_FALSE(
            malformed.has_exact_empty_persisted_gpu_operation_ledger());
        malformed = hybrid;
        malformed.persisted_gpu_operation_stats_complete = 0;
        REQUIRE_FALSE(
            malformed.has_exact_empty_persisted_gpu_operation_ledger());
        malformed = hybrid;
        malformed.persisted_gpu_operation_stats_saturated = 1;
        REQUIRE_FALSE(
            malformed.has_exact_empty_persisted_gpu_operation_ledger());

        auto leaked = hybrid;
        leaked.persisted_gpu_operation_count = 1;
        leaked.persisted_gpu_accepted_operation_count = 1;
        REQUIRE_FALSE(
            leaked.has_exact_empty_persisted_gpu_operation_ledger());
        leaked = hybrid;
        leaked.persisted_gpu_operation_stats_accepted = 1;
        REQUIRE_FALSE(
            leaked.has_exact_empty_persisted_gpu_operation_ledger());
    }

    SECTION("legacy all-GPU route retains its positive ledger contract") {
        auto legacy = hybrid;
        legacy.persisted_gpu_operation_stats_accepted = 1;
        legacy.persisted_gpu_operation_count = 2;
        legacy.persisted_gpu_accepted_operation_count = 2;
        legacy.persisted_gpu_job_count = 7;
        legacy.persisted_gpu_accepted_job_count = 7;
        REQUIRE(
            legacy.has_accepted_legacy_persisted_gpu_operation_ledger(7));
        REQUIRE_FALSE(
            legacy.has_accepted_legacy_persisted_gpu_operation_ledger(8));
        REQUIRE_FALSE(
            legacy.has_exact_empty_persisted_gpu_operation_ledger());
    }
}

TEST_CASE("E2E logical query chunks preserve one canonical ordered query set",
          "[component:benchmark][layer:contract][feature:reporting]") {
    const auto exact = make_logical_query_chunks(1024, 128);
    REQUIRE(exact.size() == 8);
    for (std::size_t ordinal = 0; ordinal < exact.size(); ++ordinal) {
        REQUIRE(exact[ordinal].begin == ordinal * 128);
        REQUIRE(exact[ordinal].end == (ordinal + 1) * 128);
    }

    const auto tail = make_logical_query_chunks(10, 4);
    REQUIRE(tail.size() == 3);
    REQUIRE(tail[0].begin == 0);
    REQUIRE(tail[0].end == 4);
    REQUIRE(tail[1].begin == 4);
    REQUIRE(tail[1].end == 8);
    REQUIRE(tail[2].begin == 8);
    REQUIRE(tail[2].end == 10);

    REQUIRE_THROWS_AS(make_logical_query_chunks(0, 1), std::invalid_argument);
    REQUIRE_THROWS_AS(make_logical_query_chunks(8, 0), std::invalid_argument);
    REQUIRE_THROWS_AS(make_logical_query_chunks(8, 9), std::invalid_argument);
}

TEST_CASE("cold grouped-SoA stats require receipt-backed CRC counters",
          "[component:benchmark][layer:contract][feature:reporting]") {
    REQUIRE(cold_index_stats_keys().size() == 59);
    REQUIRE(std::ranges::find(
        cold_index_stats_keys(), "extent_crc_raw_bytes_hashed")
        != cold_index_stats_keys().end());
    REQUIRE(std::ranges::find(
        cold_index_stats_keys(), "extent_crc_combined_bytes")
        != cold_index_stats_keys().end());
    REQUIRE(std::ranges::find(
        cold_index_stats_keys(), "extent_crc_combine_rows")
        != cold_index_stats_keys().end());
}

TEST_CASE("E2E Python aggregation and append authority contracts",
          "[component:benchmark][layer:contract]") {
    const auto contract_dir = std::filesystem::path{BIOSEQ_E2E_DATA_DIR}
        / "tests" / "contract";
    const std::array<std::filesystem::path, 2> script_paths{
        contract_dir / "test_algorithm_matrix_aggregation_authority.py",
        contract_dir / "test_algorithm_matrix_q1024_packed_partition.py",
    };
    for (const auto& script_path : script_paths) {
        CAPTURE(script_path.string());
        REQUIRE(std::filesystem::is_regular_file(script_path));
        std::string executable = "python3";
        std::string no_bytecode = "-B";
        std::string script = script_path.string();
        std::array<char*, 4> arguments{
            executable.data(), no_bytecode.data(), script.data(), nullptr};

#if defined(__APPLE__)
        char** environment = *_NSGetEnviron();
#else
        char** environment = environ;
#endif

        pid_t child = 0;
        const int spawn_status = ::posix_spawnp(
            &child, executable.c_str(), nullptr, nullptr, arguments.data(), environment);
        REQUIRE(spawn_status == 0);
        int child_status = 0;
        pid_t waited = 0;
        do {
            waited = ::waitpid(child, &child_status, 0);
        } while (waited < 0 && errno == EINTR);
        REQUIRE(waited == child);
        REQUIRE(WIFEXITED(child_status));
        REQUIRE(WEXITSTATUS(child_status) == EXIT_SUCCESS);
    }
}
