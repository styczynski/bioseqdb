#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <string>
#include <type_traits>

import bioseq.test.postgres;

using namespace std::chrono_literals;
using namespace bioseq::postgres_test;

static_assert(requires(bioseq::libpq::connection& connection) {
    connection.exec(
        "BEGIN", 1s, bioseq::libpq::transaction_policy::allow_open);
    connection.current_transaction_state();
});

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
