#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

import bioseq.cache.benchmark.common;
import bioseq.test.postgres;

namespace {

using bioseq::postgres_test::explain_json_value;

[[nodiscard]] const explain_json_value& required_property(
    const explain_json_value& object, std::string_view name) {
    const explain_json_value* value = object.find(name);
    if (value == nullptr) {
        throw std::runtime_error(
            "missing report property: " + std::string(name));
    }
    return *value;
}

[[nodiscard]] const explain_json_value::object& required_object(
    const explain_json_value& value) {
    const explain_json_value::object* object = value.as_object();
    if (object == nullptr) {
        throw std::runtime_error("report value is not an object");
    }
    return *object;
}

[[nodiscard]] const explain_json_value::array& required_array(
    const explain_json_value& value) {
    const explain_json_value::array* array = value.as_array();
    if (array == nullptr) {
        throw std::runtime_error("report value is not an array");
    }
    return *array;
}

[[nodiscard]] std::string_view required_string(
    const explain_json_value& value) {
    const std::string* string = value.as_string();
    if (string == nullptr) {
        throw std::runtime_error("report value is not a string");
    }
    return *string;
}

[[nodiscard]] std::int64_t required_integer(
    const explain_json_value& value) {
    const std::int64_t* integer = value.as_integer();
    if (integer == nullptr) {
        throw std::runtime_error("report value is not an integer");
    }
    return *integer;
}

[[nodiscard]] bool required_bool(const explain_json_value& value) {
    const bool* boolean = value.as_bool();
    if (boolean == nullptr) {
        throw std::runtime_error("report value is not a Boolean");
    }
    return *boolean;
}

[[nodiscard]] std::uint64_t nearest_rank(
    std::vector<std::uint64_t> values,
    std::uint64_t numerator, std::uint64_t denominator) {
    std::sort(values.begin(), values.end());
    const std::uint64_t rank =
        (numerator * values.size() + denominator - 1U) / denominator;
    return values[static_cast<std::size_t>(
        std::max<std::uint64_t>(rank, 1U) - 1U)];
}

}  // namespace

TEST_CASE(
    "cache benchmark report serializes staged identity and warm summaries",
    "[component:cache][layer:contract][feature:reporting]") {
    const std::string document =
        bioseq::cache_benchmark::render_report_contract_probe(true);
    const bioseq::postgres_test::explain_plan_document parsed =
        bioseq::postgres_test::explain_plan_document::parse(document);
    const explain_json_value& root = parsed.root();
    static_cast<void>(required_object(root));

    CHECK(
        required_integer(required_property(root, "schema_revision")) == 2);
    const explain_json_value& metadata = required_property(root, "metadata");
    static_cast<void>(required_object(metadata));
    CHECK(
        required_string(required_property(metadata, "status"))
        == "passed");

    const explain_json_value& build_identity =
        required_property(metadata, "build_identity");
    CHECK_FALSE(
        required_string(
            required_property(build_identity, "executable_sha256")).empty());
    CHECK_FALSE(
        required_string(
            required_property(build_identity, "identity_sha256")).empty());
    const explain_json_value& host_identity =
        required_property(metadata, "host_identity");
    CHECK_FALSE(
        required_string(
            required_property(host_identity, "machine")).empty());
    CHECK(
        required_string(
            required_property(host_identity, "identity_sha256")).size()
        == 64);
    const explain_json_value& resource_sampling =
        required_property(metadata, "resource_sampling");
    CHECK(
        required_string(
            required_property(resource_sampling, "measurement_window"))
        .find("excludes initdb")
        != std::string_view::npos);
    CHECK(
        required_string(
            required_property(resource_sampling, "rss_scope"))
        .find("all descendant processes")
        != std::string_view::npos);
    CHECK(
        required_integer(
            required_property(resource_sampling, "rss_samples"))
        == 4);
    CHECK(
        required_integer(
            required_property(resource_sampling, "temp_samples"))
        == 3);
    CHECK(
        required_integer(
            required_property(
                resource_sampling, "rss_sampling_failures"))
        == 0);
    CHECK(
        required_integer(
            required_property(
                resource_sampling, "temp_sampling_failures"))
        == 0);

    const explain_json_value::array& artifacts =
        required_array(required_property(metadata, "artifacts"));
    REQUIRE(artifacts.size() == 1);
    const explain_json_value& artifact = artifacts.front();
    CHECK(
        required_string(
            required_property(artifact, "preinstalled_stage_root"))
        == "/stage");
    CHECK(
        required_string(
            required_property(artifact, "stage_manifest"))
        == "/stage/stage-manifest.json");
    CHECK(
        required_string(
            required_property(artifact, "stage_manifest_sha256"))
        == std::string(64, 'b'));
    CHECK(
        required_string(
            required_property(artifact, "staged_control_file"))
        == "/stage/bioseq_cache.control");
    CHECK(
        required_string(
            required_property(artifact, "staged_install_sql"))
        == "/stage/bioseq_cache--1.0.sql");
    CHECK(
        required_string(required_property(artifact, "staged_dso"))
        == "/stage/bioseq_cache.so");
    CHECK(
        required_string(
            required_property(artifact, "staged_control_sha256")).size()
        == 64);

    const explain_json_value& fixture = required_property(root, "fixture");
    CHECK(
        required_integer(required_property(fixture, "source_rows"))
        == 4);
    CHECK(
        required_integer(required_property(fixture, "query_bases"))
        == 0);
    CHECK(
        required_integer(required_property(fixture, "target_bases"))
        == 0);
    CHECK(
        required_integer(
            required_property(fixture, "aggregate_input_bases"))
        == 0);
    CHECK(
        required_string(
            required_property(root, "measurement_cohort"))
        == "standalone-sql");
    const explain_json_value& qualification =
        required_property(root, "exact_2gbp_host_qualification");
    CHECK_FALSE(
        required_bool(required_property(qualification, "required")));
    CHECK(
        required_string(required_property(qualification, "status"))
        == "not-required");

    const explain_json_value& producer =
        required_property(root, "cold_producer");
    CHECK(
        required_string(required_property(producer, "stage"))
        == "producer");
    CHECK(
        required_integer(required_property(producer, "wall_time_ns"))
        == 999);

    const explain_json_value::array& samples =
        required_array(required_property(root, "samples"));
    REQUIRE(samples.size() == 3);
    std::vector<std::uint64_t> wall_times;
    for (const explain_json_value& sample : samples) {
        CHECK(
            required_string(required_property(sample, "stage"))
            == "warm");
        CHECK(
            required_string(
                required_property(sample, "measurement_cohort"))
            == "standalone-sql");
        const std::int64_t wall_time =
            required_integer(required_property(sample, "wall_time_ns"));
        REQUIRE(wall_time >= 0);
        wall_times.push_back(static_cast<std::uint64_t>(wall_time));
    }
    CHECK(
        std::find(wall_times.begin(), wall_times.end(), 999U)
        == wall_times.end());

    const explain_json_value& summary = required_property(root, "summary");
    CHECK(
        required_string(required_property(summary, "population"))
        == "warm samples only");
    CHECK(
        required_string(
            required_property(summary, "measurement_cohort"))
        == "standalone-sql");
    CHECK(
        required_integer(required_property(summary, "sample_count"))
        == static_cast<std::int64_t>(wall_times.size()));
    CHECK(
        required_integer(required_property(summary, "p50"))
        == static_cast<std::int64_t>(
            nearest_rank(wall_times, 50, 100)));
    CHECK(
        required_integer(required_property(summary, "p95"))
        == static_cast<std::int64_t>(
            nearest_rank(wall_times, 95, 100)));
    CHECK(
        required_integer(required_property(summary, "min"))
        == static_cast<std::int64_t>(
            *std::min_element(wall_times.begin(), wall_times.end())));
    CHECK(
        required_integer(required_property(summary, "max"))
        == static_cast<std::int64_t>(
            *std::max_element(wall_times.begin(), wall_times.end())));
    CHECK(
        required_integer(required_property(root, "peak_rss_bytes"))
        == 8192);
    CHECK(
        required_integer(required_property(root, "peak_temp_bytes"))
        == 16384);
    const explain_json_value& cleanup = required_property(root, "cleanup");
    CHECK(required_bool(required_property(cleanup, "attempted")));
    CHECK(required_bool(required_property(cleanup, "succeeded")));
    CHECK(
        required_bool(
            required_property(cleanup, "private_root_removed")));
    CHECK(
        required_string(
            required_property(cleanup, "verdict_source"))
        == "private_cluster::cleanup");
    CHECK(
        required_string(
            required_property(cleanup, "external_watchdog_scope"))
        .find("not claimed")
        != std::string_view::npos);
}

TEST_CASE(
    "combined cache benchmark report ties exact 2 Gbp qualification and one "
    "warm cohort to the same host",
    "[component:cache][layer:contract][feature:reporting]") {
    const std::string document =
        bioseq::cache_benchmark::render_bioseq_report_contract_probe();
    const bioseq::postgres_test::explain_plan_document parsed =
        bioseq::postgres_test::explain_plan_document::parse(document);
    const explain_json_value& root = parsed.root();
    CHECK(
        required_string(required_property(root, "scenario"))
        == "bioseq-2gbp");
    CHECK(
        required_string(
            required_property(root, "measurement_cohort"))
        == "bioseq-cpu-column-cache-on");

    const explain_json_value& metadata = required_property(root, "metadata");
    const explain_json_value& host_identity =
        required_property(metadata, "host_identity");
    const std::string_view host_digest =
        required_string(
            required_property(host_identity, "identity_sha256"));
    const explain_json_value& qualification =
        required_property(root, "exact_2gbp_host_qualification");
    CHECK(required_bool(required_property(qualification, "required")));
    CHECK(required_bool(required_property(qualification, "performed")));
    CHECK(
        required_bool(
            required_property(qualification, "same_execution_host")));
    CHECK(
        required_string(required_property(qualification, "status"))
        == "passed");
    CHECK(
        required_string(
            required_property(
                qualification, "host_identity_sha256"))
        == host_digest);
    CHECK(
        required_integer(required_property(
            qualification, "effective_total_ram_bytes"))
        > 0);
    CHECK(
        required_integer(required_property(
            qualification, "required_total_ram_bytes"))
        == 0);
    CHECK(
        required_integer(required_property(
            qualification, "effective_available_ram_bytes"))
        > 0);
    CHECK(
        required_integer(required_property(
            qualification, "required_available_ram_bytes"))
        == 0);
    CHECK(
        required_integer(required_property(
            qualification, "data_filesystem_available_bytes"))
        >= required_integer(required_property(
            qualification,
            "required_data_filesystem_available_bytes")));
    CHECK(
        required_integer(required_property(
            qualification,
            "required_data_filesystem_available_bytes"))
        == 34'359'738'368LL);
    CHECK(
        required_integer(required_property(
            qualification, "required_logical_cores"))
        == 8);
    CHECK(
        required_integer(
            required_property(qualification, "io_probe_bytes"))
        == 1'073'741'824LL);
    CHECK(
        required_integer(required_property(
            qualification, "required_io_mib_per_second"))
        == 150);

    const explain_json_value& fixture = required_property(root, "fixture");
    CHECK(
        required_integer(
            required_property(fixture, "aggregate_input_bases"))
        == 2'000'000'000LL);
    const explain_json_value::array& samples =
        required_array(required_property(root, "samples"));
    REQUIRE(samples.size() == 3);
    for (const explain_json_value& sample : samples) {
        CHECK(
            required_string(
                required_property(sample, "measurement_cohort"))
            == "bioseq-cpu-column-cache-on");
    }
    const explain_json_value& summary = required_property(root, "summary");
    CHECK(
        required_string(
            required_property(summary, "measurement_cohort"))
        == "bioseq-cpu-column-cache-on");
}

TEST_CASE(
    "cache benchmark rejects mixed percentile cohorts and any sampling failure",
    "[component:cache][layer:contract][feature:reporting]") {
    CHECK_THROWS_AS(
        bioseq::cache_benchmark::
            render_mixed_cohort_contract_probe(),
        std::runtime_error);

    CHECK(
        bioseq::cache_benchmark::
            resource_sampling_contract_satisfied(
                2, 0, 4096, 2, 0, 8192));
    CHECK_FALSE(
        bioseq::cache_benchmark::
            resource_sampling_contract_satisfied(
                2, 1, 4096, 2, 0, 8192));
    CHECK_FALSE(
        bioseq::cache_benchmark::
            resource_sampling_contract_satisfied(
                2, 0, 4096, 2, 1, 8192));
    CHECK_FALSE(
        bioseq::cache_benchmark::
            resource_sampling_contract_satisfied(
                1, 0, 4096, 2, 0, 8192));
    CHECK_FALSE(
        bioseq::cache_benchmark::
            resource_sampling_contract_satisfied(
                2, 0, 0, 2, 0, 8192));
    CHECK(bioseq::cache_benchmark::native_parser_contract_satisfied());
}

TEST_CASE(
    "cache benchmark build identity includes the stage manifest digest but "
    "excludes host stage paths",
    "[component:cache][layer:contract][feature:reporting]") {
    const auto build_identity_sha256 =
        [](const std::string& document) {
            const bioseq::postgres_test::explain_plan_document parsed =
                bioseq::postgres_test::explain_plan_document::parse(document);
            const explain_json_value& metadata =
                required_property(parsed.root(), "metadata");
            const explain_json_value& build_identity =
                required_property(metadata, "build_identity");
            return std::string(required_string(required_property(
                build_identity, "identity_sha256")));
        };

    const std::string manifest_digest(64, 'b');
    const std::string original =
        bioseq::cache_benchmark::
            render_report_contract_probe_with_stage_provenance(
                true, "/host-a/stage",
                "/host-a/metadata/stage-manifest.json",
                manifest_digest);
    const std::string relocated =
        bioseq::cache_benchmark::
            render_report_contract_probe_with_stage_provenance(
                true, "/different-host/stage",
                "/different-host/metadata/stage-manifest.json",
                manifest_digest);
    const std::string changed_manifest =
        bioseq::cache_benchmark::
            render_report_contract_probe_with_stage_provenance(
                true, "/host-a/stage",
                "/host-a/metadata/stage-manifest.json",
                std::string(64, 'c'));

    CHECK(
        build_identity_sha256(original)
        == build_identity_sha256(relocated));
    CHECK(
        build_identity_sha256(original)
        != build_identity_sha256(changed_manifest));
}

TEST_CASE(
    "cache benchmark failed report keeps the nullable schema shape",
    "[component:cache][layer:contract][feature:reporting]") {
    const std::string document =
        bioseq::cache_benchmark::render_report_contract_probe(false);
    const bioseq::postgres_test::explain_plan_document parsed =
        bioseq::postgres_test::explain_plan_document::parse(document);
    const explain_json_value& root = parsed.root();
    const explain_json_value& metadata = required_property(root, "metadata");
    CHECK(
        required_string(required_property(metadata, "status"))
        == "failed");
    CHECK(
        required_array(
            required_property(metadata, "artifacts")).size()
        == 1);
    CHECK(
        required_integer(required_property(root, "peak_rss_bytes"))
        == 8192);
    CHECK(
        required_integer(required_property(root, "peak_temp_bytes"))
        == 16384);
    CHECK(required_property(root, "cold_producer").is_null());
    CHECK(required_array(required_property(root, "samples")).empty());
    const explain_json_value& cleanup = required_property(root, "cleanup");
    CHECK_FALSE(
        required_bool(required_property(cleanup, "attempted")));
    CHECK_FALSE(
        required_bool(required_property(cleanup, "succeeded")));
    CHECK_FALSE(
        required_bool(
            required_property(cleanup, "private_root_removed")));
}
