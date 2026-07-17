/**
 * @file bench_e2e_main_test.cpp
 * @brief Direct contracts for the E2E raw-sample serializer.
 *
 * The test target compiles the production source in this translation unit with
 * only its CLI entry point suppressed.  This keeps the oracle independent of
 * the serializer's private key arrays while exercising the exact function
 * installed in the benchmark executable.
 */
#include "bench_e2e_main.cpp"

#include <catch2/catch_test_macros.hpp>

namespace {

[[nodiscard]] nlohmann::json load_e2e_contract_config(
        const std::filesystem::path& path) {
    std::ifstream stream{path};
    if (!stream.good()) {
        throw std::runtime_error("could not open E2E contract config");
    }
    return nlohmann::json::parse(stream);
}

[[nodiscard]] RunConstraints exact_e2e_execution_constraints(
        const nlohmann::json& config,
        std::string_view benchmark) {
    const auto& matrix = config.at(std::string{benchmark}).at("matrix");
    const auto& over = matrix.at("over");
    RunConstraints constraints{};
    for (const auto& row :
         matrix.at("acceptance").at("exact_algorithm_rows")) {
        const std::string algorithm = row.get<std::string>();
        const auto& axes = over.at(algorithm);
        ParamMap params{};
        if (axes.contains("minimizer_index")) {
            const auto& values = axes.at("minimizer_index");
            if (!values.is_array() || values.size() != 1
                || !values.front().is_string()) {
                throw std::runtime_error(
                    "contract test requires one resolved minimizer_index");
            }
            params.emplace(
                "minimizer_index",
                bioseq::bench_framework::ConfigValue::from(
                    values.front().get<std::string>()));
        }
        constraints.execution_tuples.push_back(
            bioseq::bench_framework::ResolvedExecutionTuple{
                .algorithm_name = algorithm,
                .params = std::move(params),
            });
    }
    return constraints;
}

[[nodiscard]] nlohmann::json zero_format6_reader_evidence(
        const std::int64_t execution_id,
        const std::int64_t backend_pid,
        const std::int64_t database_oid,
        const std::int64_t recompute_calls = 0) {
    return nlohmann::json{
        {"schema", "bioseq.format6_reader_evidence"},
        {"version", 1},
        {"execution_id", execution_id},
        {"backend_pid", backend_pid},
        {"database_oid", database_oid},
        {"format_version", 0},
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
        {"directory_phase_peak_bytes", 0},
        {"control_phase_peak_bytes", 0},
        {"posting_phase_peak_bytes", 0},
        {"peak_admitted_bytes", 0},
        {"retained_owner_bytes", 0},
        {"admitted_bytes", 0},
        {"legacy_assembly_calls", 0},
        {"legacy_globalize_calls", 0},
        {"legacy_rebase_calls", 0},
        {"recompute_calls", recompute_calls},
        {"generic_source_fallback_calls", 0},
        {"backend_fallbacks", 0},
        {"failures", 0},
        {"saturated", false},
        {"operational_identity_conflict", false},
        {"stats_saturated", false},
    };
}

inline constexpr auto kFragmentNativeFlatMetricFields =
    std::to_array<std::pair<
        std::string_view, std::int64_t E2eOutput::*>>({
        {"fragment_native_waves", &E2eOutput::fragment_native_waves},
        {"fragment_native_queries", &E2eOutput::fragment_native_queries},
        {"fragment_native_occurrence_fill_ns",
            &E2eOutput::fragment_native_occurrence_fill_ns},
        {"fragment_native_occurrence_radix_ns",
            &E2eOutput::fragment_native_occurrence_radix_ns},
        {"fragment_native_unique_plan_build_ns",
            &E2eOutput::fragment_native_unique_plan_build_ns},
        {"fragment_native_query_plan_radix_fill_ns",
            &E2eOutput::fragment_native_query_plan_radix_fill_ns},
        {"fragment_native_control_count_pass_ns",
            &E2eOutput::fragment_native_control_count_pass_ns},
        {"fragment_native_rejected_control_validation_ns",
            &E2eOutput::fragment_native_rejected_control_validation_ns},
        {"fragment_native_orientation_layout_ns",
            &E2eOutput::fragment_native_orientation_layout_ns},
        {"fragment_native_direct_emission_ns",
            &E2eOutput::fragment_native_direct_emission_ns},
        {"fragment_native_local_sort_ns",
            &E2eOutput::fragment_native_local_sort_ns},
        {"fragment_native_tie_normalization_ns",
            &E2eOutput::fragment_native_tie_normalization_ns},
        {"fragment_native_control_count_bytes",
            &E2eOutput::fragment_native_control_count_bytes},
        {"fragment_native_rejected_control_validation_bytes",
            &E2eOutput::fragment_native_rejected_control_validation_bytes},
        {"fragment_native_pass2_position_bytes",
            &E2eOutput::fragment_native_pass2_position_bytes},
        {"fragment_native_pass3_position_bytes",
            &E2eOutput::fragment_native_pass3_position_bytes},
        {"fragment_native_anchors_constructed",
            &E2eOutput::fragment_native_anchors_constructed},
        {"fragment_native_local_sort_bytes",
            &E2eOutput::fragment_native_local_sort_bytes},
        {"fragment_native_tie_normalization_bytes",
            &E2eOutput::fragment_native_tie_normalization_bytes},
        {"fragment_native_exact_final_bytes",
            &E2eOutput::fragment_native_exact_final_bytes},
        {"fragment_native_peak_exact_final_bytes",
            &E2eOutput::fragment_native_peak_exact_final_bytes},
        {"fragment_native_global_sort_calls_invoked",
            &E2eOutput::fragment_native_global_sort_calls_invoked},
        {"fragment_native_global_sort_calls_skipped",
            &E2eOutput::fragment_native_global_sort_calls_skipped},
        {"fragment_native_candidate_anchor_bytes",
            &E2eOutput::fragment_native_candidate_anchor_bytes},
        {"fragment_native_plan_ordinal_bytes",
            &E2eOutput::fragment_native_plan_ordinal_bytes},
        {"fragment_native_compaction_bytes",
            &E2eOutput::fragment_native_compaction_bytes},
        {"fragment_native_final_copy_bytes",
            &E2eOutput::fragment_native_final_copy_bytes},
        {"fragment_native_peak_occurrence_capacity_bytes",
            &E2eOutput::fragment_native_peak_occurrence_capacity_bytes},
        {"fragment_native_peak_query_plan_capacity_bytes",
            &E2eOutput::fragment_native_peak_query_plan_capacity_bytes},
        {"fragment_native_peak_query_radix_capacity_bytes",
            &E2eOutput::fragment_native_peak_query_radix_capacity_bytes},
        {"fragment_native_peak_occurrence_count_capacity_bytes",
            &E2eOutput::fragment_native_peak_occurrence_count_capacity_bytes},
        {"fragment_native_peak_block_layout_capacity_bytes",
            &E2eOutput::fragment_native_peak_block_layout_capacity_bytes},
        {"fragment_native_peak_block_evidence_capacity_bytes",
            &E2eOutput::fragment_native_peak_block_evidence_capacity_bytes},
        {"fragment_native_occurrence_allocations",
            &E2eOutput::fragment_native_occurrence_allocations},
        {"fragment_native_query_plan_allocations",
            &E2eOutput::fragment_native_query_plan_allocations},
        {"fragment_native_query_radix_allocations",
            &E2eOutput::fragment_native_query_radix_allocations},
        {"fragment_native_occurrence_count_allocations",
            &E2eOutput::fragment_native_occurrence_count_allocations},
        {"fragment_native_block_layout_allocations",
            &E2eOutput::fragment_native_block_layout_allocations},
        {"fragment_native_block_evidence_allocations",
            &E2eOutput::fragment_native_block_evidence_allocations},
        {"fragment_native_evidence_saturated",
            &E2eOutput::fragment_native_evidence_saturated},
    });
static_assert(kFragmentNativeFlatMetricFields.size() == 40);

}  // namespace

TEST_CASE("E2E raw samples pin Format-5 and Format-6 reader evidence",
          "[bench_e2e][raw-samples][format5][format6]") {
    const nlohmann::json memory_evidence{
        {"schema", "bioseq.format5_reader_memory_evidence"},
        {"version", 3},
        {"execution_id", 41},
        {"backend_pid", 42},
        {"database_oid", 43},
        {"format5_reader_capacity_accounting_passes", 3},
        {"format5_reader_global_position_storage_preparations", 1},
        {"format5_reader_global_position_storage_allocations", 1},
        {"format5_reader_position_append_growth_reservations", 0},
        {"format5_reader_retained_position_count", 37},
        {"format5_reader_retained_position_capacity", 37},
        {"format5_reader_directory_capacity_bytes", 10},
        {"format5_reader_query_hash_capacity_bytes", 20},
        {"format5_reader_query_record_capacity_bytes", 100},
        {"format5_reader_ordinal_route_capacity_bytes", 20},
        {"format5_reader_selected_extent_capacity_bytes", 30},
        {"format5_reader_current_selected_control_capacity_bytes", 0},
        {"format5_reader_global_position_capacity_bytes", 296},
        {"format5_reader_control_payload_capacity_bytes", 0},
        {"format5_reader_owned_capacity_bytes", 476},
        {"format5_reader_admitted_bytes", 476},
        {"format5_reader_exact_position_image_checks", 1},
        {"format5_reader_exact_position_image_failures", 0},
        {"format5_reader_retained_layout_checks", 1},
        {"format5_reader_retained_layout_failures", 0},
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
        {"fragment_native_peak_query_plan_capacity_bytes", 500},
        {"fragment_native_peak_query_radix_capacity_bytes", 600},
        {"fragment_native_peak_occurrence_count_capacity_bytes", 700},
        {"fragment_native_peak_block_layout_capacity_bytes", 800},
        {"fragment_native_peak_block_evidence_capacity_bytes", 900},
        {"fragment_native_occurrence_allocations", 1},
        {"fragment_native_query_plan_allocations", 2},
        {"fragment_native_query_radix_allocations", 3},
        {"fragment_native_occurrence_count_allocations", 4},
        {"fragment_native_block_layout_allocations", 5},
        {"fragment_native_block_evidence_allocations", 6},
        {"fragment_native_evidence_saturated", false},
        {"format5_reader_memory_evidence_saturated", false},
        {"stats_saturated", false},
    };

    E2eInput input{};
    E2eOutput output{};
    output.persisted_policy_audit_accepted = 1;
    output.persisted_gpu_operation_evidence_schema_valid = 1;
    output.persisted_gpu_operation_evidence = nlohmann::json::object();
    output.format5_reader_memory_evidence = memory_evidence;
    const nlohmann::json format6_evidence =
        zero_format6_reader_evidence(41, 42, 43);
    output.format6_reader_evidence = format6_evidence;
    output.alignment_snapshot_rows.resize(
        E2eOutput::kAlignmentSnapshotRowCount);
    for (auto& row : output.alignment_snapshot_rows) {
        row.resize(E2eOutput::kAlignmentSnapshotColumnCount);
    }

    const std::string serialized = serialize_e2e_raw_sample(
        "bioseq_contract_just_index", ParamMap{}, input, output);
    const auto payload = nlohmann::json::parse(
        serialized, nullptr, /*allow_exceptions=*/false);
    REQUIRE_FALSE(payload.is_discarded());
    REQUIRE(payload.at("payload_schema") == "bioseq.e2e.raw-sample");
    REQUIRE(payload.at("payload_schema_version") == 6);

    const auto& nested = payload.at("attestation").at(
        "format5_reader_memory_evidence");
    const std::set<std::string> expected_keys{
        "schema",
        "version",
        "execution_id",
        "backend_pid",
        "database_oid",
        "format5_reader_capacity_accounting_passes",
        "format5_reader_global_position_storage_preparations",
        "format5_reader_global_position_storage_allocations",
        "format5_reader_position_append_growth_reservations",
        "format5_reader_retained_position_count",
        "format5_reader_retained_position_capacity",
        "format5_reader_directory_capacity_bytes",
        "format5_reader_query_hash_capacity_bytes",
        "format5_reader_query_record_capacity_bytes",
        "format5_reader_ordinal_route_capacity_bytes",
        "format5_reader_selected_extent_capacity_bytes",
        "format5_reader_current_selected_control_capacity_bytes",
        "format5_reader_global_position_capacity_bytes",
        "format5_reader_control_payload_capacity_bytes",
        "format5_reader_owned_capacity_bytes",
        "format5_reader_admitted_bytes",
        "format5_reader_exact_position_image_checks",
        "format5_reader_exact_position_image_failures",
        "format5_reader_retained_layout_checks",
        "format5_reader_retained_layout_failures",
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
        "fragment_native_peak_query_radix_capacity_bytes",
        "fragment_native_peak_occurrence_count_capacity_bytes",
        "fragment_native_peak_block_layout_capacity_bytes",
        "fragment_native_peak_block_evidence_capacity_bytes",
        "fragment_native_occurrence_allocations",
        "fragment_native_query_plan_allocations",
        "fragment_native_query_radix_allocations",
        "fragment_native_occurrence_count_allocations",
        "fragment_native_block_layout_allocations",
        "fragment_native_block_evidence_allocations",
        "fragment_native_evidence_saturated",
        "format5_reader_memory_evidence_saturated",
        "stats_saturated",
    };
    REQUIRE(expected_keys.size() == 67);
    std::set<std::string> actual_keys;
    for (const auto& [key, value] : nested.items()) {
        (void)value;
        actual_keys.insert(key);
    }
    REQUIRE(actual_keys == expected_keys);
    REQUIRE(nested.size() == 67);
    REQUIRE(nested.at("schema")
            == "bioseq.format5_reader_memory_evidence");
    REQUIRE(nested.at("version") == 3);
    REQUIRE(nested == memory_evidence);
    const auto& format6_nested = payload.at("attestation").at(
        "format6_reader_evidence");
    REQUIRE(format6_nested.size() == 47);
    REQUIRE(format6_nested.at("schema")
            == "bioseq.format6_reader_evidence");
    REQUIRE(format6_nested.at("version") == 1);
    REQUIRE(format6_nested == format6_evidence);
}

TEST_CASE("E2E publishes fragment-native evidence as configured flat metrics",
          "[bench_e2e][evaluation][format5][fragment-native]") {
    E2eOutput output{};
    std::int64_t next_value = 1;
    for (const auto& [key, member] : kFragmentNativeFlatMetricFields) {
        output.*member = key == "fragment_native_evidence_saturated"
            ? 1
            : next_value++;
    }

    const E2eInput input{};
    const ParamMap params{};
    const auto metrics = e2e_eval("format5-test", params, input, output);
    for (const auto& [key, member] : kFragmentNativeFlatMetricFields) {
        const std::string metric_name{key};
        REQUIRE(metrics.contains(metric_name));
        REQUIRE(metrics.at(metric_name)
            == static_cast<double>(output.*member));
    }

    const auto config_path = std::filesystem::path{BIOSEQ_E2E_DATA_DIR}
        / "bench_e2e_format6_bioseq_just_index_only.json";
    const auto config = load_e2e_contract_config(config_path);
    const auto& configured_metrics =
        config.at("bench_e2e").at("matrix").at("metrics");
    for (const auto& [key, member] : kFragmentNativeFlatMetricFields) {
        (void)member;
        REQUIRE(configured_metrics.contains(std::string{key}));
    }
}

TEST_CASE("E2E input setup follows exact expanded execution tuples",
          "[bench_e2e][input][selection][just-index][format6]") {
    const auto format6_path = std::filesystem::path{BIOSEQ_E2E_DATA_DIR}
        / "bench_e2e_format6_simd_minimap2_just_index_focus.json";
    const auto format6_config = load_e2e_contract_config(format6_path);
    const auto format6_constraints = exact_e2e_execution_constraints(
        format6_config, "bench_e2e");
    REQUIRE(format6_constraints.execution_tuples.size() == 3);
    REQUIRE(format6_config.at("test_cases")
                .at("E2E_2GBP_DNA4").at("data")
                .at("alphabet") == "dna4");
    REQUIRE(format6_config.at("test_cases")
                .at("E2E_2GBP_DNA4").at("data")
                .at("kmer_size") == 15);
    REQUIRE(format6_config.at("test_cases")
                .at("E2E_2GBP_DNA4").at("data")
                .at("window_size") == 10);
    REQUIRE(format6_config.at("bench_e2e").at("matrix").at("for")
                .at("E2E_2GBP_DNA4") == 6);
    const auto format6 =
        e2e_just_index_setup_requirements(format6_constraints);
    REQUIRE(format6.simd_per_fragment);
    REQUIRE(format6.minimap2);
    REQUIRE_FALSE(format6.scalar_per_fragment);
    REQUIRE_FALSE(format6.scalar_per_sequence);
    REQUIRE_FALSE(format6.simd_per_sequence);
    REQUIRE_FALSE(format6.gpu_per_fragment);
    REQUIRE_FALSE(format6.gpu_per_sequence);

    const auto focus_path = std::filesystem::path{BIOSEQ_E2E_DATA_DIR}
        / "bench_e2e_format5_simd_minimap2_just_index_focus.json";
    const auto focus_config = load_e2e_contract_config(focus_path);
    const auto focus_constraints = exact_e2e_execution_constraints(
        focus_config, "bench_e2e");
    REQUIRE(focus_constraints.execution_tuples.size() == 4);
    REQUIRE(focus_config.at("test_cases").at("E2E_2GBP_DNA4")
                .at("data").at("just_index_setup_only")
                .get<std::string>()
            == kMinimizersPerFragmentIndex);

    const auto focus =
        e2e_just_index_setup_requirements(focus_constraints);
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
        const auto full = e2e_just_index_setup_requirements(
            exact_e2e_execution_constraints(default_config, benchmark));
        REQUIRE(full.simd_per_fragment);
        REQUIRE(full.gpu_per_fragment);
        REQUIRE(full.minimap2);
        REQUIRE_FALSE(full.scalar_per_fragment);
        REQUIRE_FALSE(full.scalar_per_sequence);
        REQUIRE_FALSE(full.simd_per_sequence);
        REQUIRE_FALSE(full.gpu_per_sequence);
    }

    RunConstraints no_index_only{};
    no_index_only.execution_tuples = {
        {.algorithm_name =
             "bioseq_block_simd_dp_two_stack_nthash_no_index"},
        {.algorithm_name = "bioseq_block_gpu_dp_gpu_seed_no_index"},
        {.algorithm_name = "minimap2_no_index"},
    };
    const auto absent = e2e_just_index_setup_requirements(no_index_only);
    REQUIRE_FALSE(absent.any_bioseq());
    REQUIRE_FALSE(absent.minimap2);
}

TEST_CASE("E2E primary rows require strand and executable CIGAR replay",
          "[bench_e2e][correctness][cigar][strand]") {
    E2eInput input{};
    input.alphabet = "dna4";
    input.cigar_projection = true;
    input.queries.push_back(E2eQueryTruth{
        .id = 0,
        .bases = "ACGT",
        .truth_target_start = 10,
        .truth_target_end = 14,
        .truth_is_reverse = false,
        .target_window_start = 10,
        .target_window_bases = "ACGT",
    });

    const it::rowset_t bioseq_rows{{
        std::string{"0"}, std::string{"target"}, std::string{"10"},
        std::string{"14"}, std::string{"0"}, std::string{"4"},
        std::string{"f"}, std::string{"t"}, std::string{"4="},
    }};
    E2eOutput bioseq{};
    parse_bioseq_batch_rows(bioseq_rows, input, bioseq);
    REQUIRE(bioseq.found);
    REQUIRE(bioseq.exact_primary_count_accepted == 1);
    REQUIRE(bioseq.correct_strand_count == 1);
    REQUIRE(bioseq.cigar_replay_count == 1);

    E2eOutput minimap{};
    parse_minimap2_paf(
        "query0\t4\t0\t4\t+\tref0\t20\t10\t14\t4\t4\t60\t"
        "tp:A:P\tcg:Z:4=\n",
        input,
        minimap);
    REQUIRE(minimap.found);
    REQUIRE(minimap.exact_primary_count_accepted == 1);
    REQUIRE(minimap.correct_strand_count == 1);
    REQUIRE(minimap.cigar_replay_count == 1);

    E2eOutput extra{};
    parse_minimap2_paf(
        "query0\t4\t0\t4\t+\tref0\t20\t10\t14\t4\t4\t60\t"
        "tp:A:P\tcg:Z:4=\n"
        "query0\t4\t0\t4\t+\tref0\t20\t10\t14\t4\t4\t60\t"
        "tp:A:S\tcg:Z:4=\n",
        input,
        extra);
    REQUIRE_FALSE(extra.found);
    REQUIRE(extra.exact_primary_count_accepted == 0);
}

TEST_CASE("cold Format-5 evidence requires receipt-backed CRC counters",
          "[bench_e2e][raw-samples][format5][crc]") {
    STATIC_REQUIRE(kColdIndexEvidenceKeys.size() == 59);
    REQUIRE(std::ranges::find(
        kColdIndexEvidenceKeys, "extent_crc_raw_bytes_hashed")
        != kColdIndexEvidenceKeys.end());
    REQUIRE(std::ranges::find(
        kColdIndexEvidenceKeys, "extent_crc_combined_bytes")
        != kColdIndexEvidenceKeys.end());
    REQUIRE(std::ranges::find(
        kColdIndexEvidenceKeys, "extent_crc_combine_rows")
        != kColdIndexEvidenceKeys.end());
}
