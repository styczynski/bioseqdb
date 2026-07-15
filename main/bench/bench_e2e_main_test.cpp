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
        {"format5_reader_query_hash_capacity_bytes", 0},
        {"format5_reader_query_record_capacity_bytes", 100},
        {"format5_reader_ordinal_route_capacity_bytes", 20},
        {"format5_reader_selected_extent_capacity_bytes", 30},
        {"format5_reader_current_selected_control_capacity_bytes", 0},
        {"format5_reader_global_position_capacity_bytes", 296},
        {"format5_reader_control_payload_capacity_bytes", 0},
        {"format5_reader_owned_capacity_bytes", 456},
        {"format5_reader_admitted_bytes", 456},
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
    REQUIRE(payload.at("payload_schema_version") == 4);

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
