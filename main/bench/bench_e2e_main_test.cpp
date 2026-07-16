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

}  // namespace

TEST_CASE("E2E raw samples pin Format-5 memory evidence v2",
          "[bench_e2e][raw-samples][format5][memory-evidence]") {
    const nlohmann::json memory_evidence{
        {"schema", "bioseq.format5_reader_memory_evidence"},
        {"version", 2},
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
        {"format5_reader_memory_evidence_saturated", false},
        {"stats_saturated", false},
    };

    E2eInput input{};
    E2eOutput output{};
    output.persisted_policy_audit_accepted = 1;
    output.persisted_gpu_operation_evidence_schema_valid = 1;
    output.persisted_gpu_operation_evidence = nlohmann::json::object();
    output.format5_reader_memory_evidence = memory_evidence;
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
    REQUIRE(payload.at("payload_schema_version") == 5);

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
        "format5_reader_memory_evidence_saturated",
        "stats_saturated",
    };
    REQUIRE(expected_keys.size() == 27);
    std::set<std::string> actual_keys;
    for (const auto& [key, value] : nested.items()) {
        (void)value;
        actual_keys.insert(key);
    }
    REQUIRE(actual_keys == expected_keys);
    REQUIRE(nested.size() == 27);
    REQUIRE(nested.at("schema")
            == "bioseq.format5_reader_memory_evidence");
    REQUIRE(nested.at("version") == 2);
    REQUIRE(nested == memory_evidence);
}

TEST_CASE("E2E input setup follows exact expanded execution tuples",
          "[bench_e2e][input][selection][just-index][format5]") {
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
