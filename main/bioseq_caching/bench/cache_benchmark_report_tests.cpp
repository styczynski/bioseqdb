#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

import bioseq.cache.benchmark;
import bioseq.test.postgres;

namespace {

using bioseq::postgres_test::json_value;

[[nodiscard]] const json_value& required_property(
    const json_value& object, std::string_view name) {
    const json_value* value = object.find(name);
    if (value == nullptr) {
        throw std::runtime_error(
            "missing report property: " + std::string(name));
    }
    return *value;
}

[[nodiscard]] const json_value::object& required_object(
    const json_value& value) {
    const json_value::object* object = value.as_object();
    if (object == nullptr) {
        throw std::runtime_error("report value is not an object");
    }
    return *object;
}

[[nodiscard]] const json_value::array& required_array(
    const json_value& value) {
    const json_value::array* array = value.as_array();
    if (array == nullptr) {
        throw std::runtime_error("report value is not an array");
    }
    return *array;
}

[[nodiscard]] std::string_view required_string(
    const json_value& value) {
    const std::string* string = value.as_string();
    if (string == nullptr) {
        throw std::runtime_error("report value is not a string");
    }
    return *string;
}

[[nodiscard]] std::int64_t required_integer(
    const json_value& value) {
    const std::int64_t* integer = value.as_integer();
    if (integer == nullptr) {
        throw std::runtime_error("report value is not an integer");
    }
    return *integer;
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
    "[cache][benchmark][report-contract]") {
    const std::string document =
        bioseq::cache_benchmark::render_report_contract_probe(true);
    const bioseq::postgres_test::explain_document parsed =
        bioseq::postgres_test::explain_document::parse(document);
    const json_value& root = parsed.root();
    static_cast<void>(required_object(root));

    CHECK(
        required_string(required_property(root, "schema_version"))
        == "bioseq-cache-benchmark-v1");
    const json_value& metadata = required_property(root, "metadata");
    static_cast<void>(required_object(metadata));
    CHECK(
        required_string(required_property(metadata, "status"))
        == "passed");

    const json_value& build_identity =
        required_property(metadata, "build_identity");
    CHECK_FALSE(
        required_string(
            required_property(build_identity, "executable_sha256")).empty());
    CHECK_FALSE(
        required_string(
            required_property(build_identity, "identity_sha256")).empty());
    const json_value& host_identity =
        required_property(metadata, "host_identity");
    CHECK_FALSE(
        required_string(
            required_property(host_identity, "machine")).empty());
    const json_value& resource_sampling =
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

    const json_value::array& artifacts =
        required_array(required_property(metadata, "artifacts"));
    REQUIRE(artifacts.size() == 1);
    const json_value& artifact = artifacts.front();
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

    const json_value& fixture = required_property(root, "fixture");
    CHECK(
        required_integer(required_property(fixture, "source_rows"))
        == 4);
    CHECK(
        required_integer(required_property(fixture, "reference_bases"))
        == 0);
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

    const json_value& producer =
        required_property(root, "cold_producer");
    CHECK(
        required_string(required_property(producer, "phase"))
        == "producer");
    CHECK(
        required_integer(required_property(producer, "wall_time_ns"))
        == 999);

    const json_value::array& samples =
        required_array(required_property(root, "samples"));
    REQUIRE(samples.size() == 3);
    std::vector<std::uint64_t> wall_times;
    for (const json_value& sample : samples) {
        CHECK(
            required_string(required_property(sample, "phase"))
            == "warm");
        const std::int64_t wall_time =
            required_integer(required_property(sample, "wall_time_ns"));
        REQUIRE(wall_time >= 0);
        wall_times.push_back(static_cast<std::uint64_t>(wall_time));
    }
    CHECK(
        std::find(wall_times.begin(), wall_times.end(), 999U)
        == wall_times.end());

    const json_value& summary = required_property(root, "summary");
    CHECK(
        required_string(required_property(summary, "population"))
        == "warm samples only");
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
}

TEST_CASE(
    "cache benchmark failed report keeps the nullable schema shape",
    "[cache][benchmark][report-contract]") {
    const std::string document =
        bioseq::cache_benchmark::render_report_contract_probe(false);
    const bioseq::postgres_test::explain_document parsed =
        bioseq::postgres_test::explain_document::parse(document);
    const json_value& root = parsed.root();
    const json_value& metadata = required_property(root, "metadata");
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
}
