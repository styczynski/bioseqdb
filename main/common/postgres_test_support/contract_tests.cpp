#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <string>
#include <type_traits>
#include <utility>

import bioseq.test.postgres;

using namespace std::chrono_literals;
using namespace bioseq::postgres_test;

static_assert(requires(bioseq::libpq::connection& connection) {
    connection.exec(
        "BEGIN", 1s, bioseq::libpq::transaction_policy::allow_open);
    connection.current_transaction_state();
    connection.exec_single_row_observed("SELECT 1", 1s);
    connection.set_execution_observer(nullptr, "contract");
    connection.execution_observer_ptr();
});
static_assert(std::is_same_v<
    decltype(std::declval<bioseq::libpq::connection&>()
                 .exec_single_row_observed("SELECT 1", 1s)),
    bioseq::libpq::single_row_observed_result>);

namespace {

struct report_state_reset {
    ~report_state_reset() noexcept {
        clear_report_case_context();
        try {
            clear_process_sql_transcript();
        } catch (...) {
        }
        clear_cleanup_observations();
    }
};

}  // namespace

TEST_CASE("libpq first-row observation has explicit empty-result semantics",
          "[postgres-test-support][unit][libpq][first-row]") {
    const bioseq::libpq::single_row_observed_result observation;
    CHECK_FALSE(observation.time_to_first_row.has_value());
    CHECK(observation.elapsed == 0ns);
    CHECK(observation.rows.rows() == 0);
}

TEST_CASE("process report collectors preserve case SQL and cleanup evidence",
          "[postgres-test-support][unit][report]") {
    const report_state_reset reset;
    clear_process_sql_transcript();
    clear_cleanup_observations();
    set_report_case_context({
        .test_case = "report collector contract",
        .section = "observer"});

    bioseq::libpq::result command;
    process_sql_transcript().command_succeeded(
        "admin", "SELECT 1", command, 2ms);
    const bioseq::libpq::query_error expected{
        "expected failure", "22000"};
    process_sql_transcript().command_failed(
        "session", "SELECT fail()", expected, 3ms);
    bioseq::libpq::result oversized;
    oversized.columns_.push_back(
        {.name = "payload", .type_oid = 25});
    oversized.rows_.push_back({
        {.is_null = false,
         .text = std::string(64U * 1024U + 1U, 'x')}});
    process_sql_transcript().command_succeeded(
        "session", "SELECT oversized_payload", oversized, 4ms);

    const auto transcript = process_sql_transcript_entries();
    REQUIRE(transcript.size() == 3);
    CHECK(transcript[0].test_case == "report collector contract");
    CHECK(transcript[0].section == "observer");
    CHECK(transcript[0].status == sql_transcript_status::succeeded);
    CHECK(transcript[1].sqlstate == "22000");
    CHECK(transcript[1].status == sql_transcript_status::failed);
    CHECK(transcript[2].rows_truncated);
    CHECK(transcript[2].sampled_rows.empty());

    record_cleanup_observation(
        std::filesystem::path{"/tmp/bioseq-report-contract"},
        true, "removed",
        resource_report{
            .peak_rss_bytes = 1024,
            .observed_temp_bytes = 2048});
    const cleanup_report cleanup = aggregate_cleanup_report();
    CHECK(cleanup.succeeded);
    REQUIRE(cleanup_observations().size() == 1);
    CHECK(aggregate_resource_report().observed_temp_bytes >= 2048);

}

TEST_CASE("postgres test support SHA-256 matches the standard vector",
          "[postgres-test-support][unit]") {
    REQUIRE(
        sha256("abc").hex()
        == "ba7816bf8f01cfea414140de5dae2223"
           "b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("postgres test support parses typed recursive EXPLAIN JSON",
          "[postgres-test-support][unit]") {
    const explain_document document = explain_document::parse(
        R"([{"Plan":{"Node Type":"Custom Scan",)"
        R"("Custom Plan Provider":"BioseqCacheScan",)"
        R"("Actual Loops":0,"Plans":[{"Node Type":"Seq Scan"}]}}])");
    const auto nodes =
        document.find_plan_nodes("Custom Plan Provider", "BioseqCacheScan");
    REQUIRE(nodes.size() == 1);
    REQUIRE(explain_property_integer(*nodes.front(), "Actual Loops") == 0);
    REQUIRE(document.plan_nodes().size() == 2);
}

TEST_CASE("postgres test support enforces migration probe identity and counts",
          "[postgres-test-support][unit]") {
    clear_migration_probes();
    {
        migration_probe_scope scope(
            "probe contract", "single", "value=1");
        record_migration_probe("support.probe.contract");
        REQUIRE_THROWS_AS(
            record_migration_probe("support.probe.contract"),
            migration_probe_error);
    }
    const std::array expected{
        expected_migration_probe{
            .stable_id = "support.probe.contract",
            .expected_count = 1}};
    REQUIRE_NOTHROW(validate_migration_probes(expected));
    clear_migration_probes();
}

TEST_CASE("canonical row hash distinguishes NULL from empty text",
          "[postgres-test-support][unit]") {
    exact_row_hasher null_hash;
    const std::array null_row{
        nullable_text_cell{.is_null = true, .bytes = {}}};
    null_hash.append_row(null_row);

    exact_row_hasher empty_hash;
    const std::array empty_row{
        nullable_text_cell{.is_null = false, .bytes = {}}};
    empty_hash.append_row(empty_row);

    REQUIRE(null_hash.finalize() != empty_hash.finalize());

    exact_row_hasher normative;
    const std::array mixed_row{
        nullable_text_cell{.is_null = true, .bytes = {}},
        nullable_text_cell{.is_null = false, .bytes = {}}};
    normative.append_row(mixed_row);
    REQUIRE(
        normative.finalize().hex()
        == "48a1b931ccb6063e8f71c216c9f04d1c"
           "0bd426edf4ce8ee4c98601f6c603b206");
}
