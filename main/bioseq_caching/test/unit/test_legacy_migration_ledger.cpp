#include <catch2/matchers/catch_matchers_string.hpp>
#include <catch2/catch_test_macros.hpp>

#include "legacy_migration_ledger.hpp"

#include <array>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {

[[nodiscard]] std::string row(
    std::string_view stable_id,
    std::string_view catch_case,
    std::string_view status,
    std::string_view legacy_path = "DOC0395 section",
    std::string_view evidence_harness = "cache-integration",
    std::string_view evidence_role = "cache-integration") {
    return std::string(legacy_path)
        + "\tassertion\t" + std::string(stable_id)
        + "\t" + std::string(evidence_harness)
        + "\t" + std::string(evidence_role)
        + "\tregress\treplacement scenario and rationale"
          "\texpected result\tstate invariant\t"
        + std::string(catch_case)
        + "\tsection\t[cache][unit]\t" + std::string(status) + "\n";
}

[[nodiscard]] bioseq::cache_test::migration::ledger parse(
    const std::string& body) {
    std::istringstream input{
        std::string(bioseq::cache_test::migration::ledger_header)
        + "\n" + body};
    return bioseq::cache_test::migration::parse_ledger(input);
}

[[nodiscard]] std::filesystem::path source_root() {
    const std::filesystem::path source =
        std::filesystem::absolute(std::filesystem::path{__FILE__});
    if (!std::filesystem::is_regular_file(source)) {
        throw std::runtime_error(
            "migration-ledger unit test cannot locate its source file");
    }
    return source.parent_path();
}

[[nodiscard]] std::filesystem::path present_asset() {
    return std::filesystem::path{__FILE__}.filename();
}

[[nodiscard]] std::filesystem::path module_root() {
    return source_root().parent_path().parent_path();
}

}  // namespace

TEST_CASE("migration ledger accepts only the three lifecycle statuses",
          "[cache][unit][migration-ledger]") {
    const auto ledger = parse(
        row("mapped.id", "mapped case", "mapped")
        + row("passing.id", "passing case", "passing")
        + row("retired.id", "replacement case", "retired_with_proof"));
    REQUIRE(ledger.rows.size() == 3);
    CHECK(ledger.rows[0].migration_status
          == bioseq::cache_test::migration::status::mapped);
    CHECK(ledger.rows[1].migration_status
          == bioseq::cache_test::migration::status::passing);
    CHECK(ledger.rows[2].migration_status
          == bioseq::cache_test::migration::status::retired_with_proof);

    using Catch::Matchers::ContainsSubstring;
    CHECK_THROWS_WITH(
        parse(row("invalid.id", "some case", "retired")),
        ContainsSubstring("invalid migration_status 'retired'"));
}

TEST_CASE("migration ledger rejects duplicate stable ids",
          "[cache][unit][migration-ledger]") {
    using Catch::Matchers::ContainsSubstring;
    CHECK_THROWS_WITH(
        parse(
            row("duplicate.id", "first case", "mapped")
            + row("duplicate.id", "second case", "passing")),
        ContainsSubstring("duplicates stable_id 'duplicate.id'"));
}

TEST_CASE("migration ledger requires an explicit supported evidence harness",
          "[cache][unit][migration-ledger]") {
    using Catch::Matchers::ContainsSubstring;
    CHECK_THROWS_WITH(
        parse(row(
            "invalid.owner", "some case", "mapped",
            "DOC0395 section", "cache-other")),
        ContainsSubstring("invalid evidence_harness 'cache-other'"));
}

TEST_CASE("migration ledger requires a compatible explicit evidence role",
          "[cache][unit][migration-ledger]") {
    using Catch::Matchers::ContainsSubstring;
    CHECK_THROWS_WITH(
        parse(row(
            "invalid.role", "some case", "mapped",
            "DOC0395 section", "cache-integration",
            "cache-bioseq-support-integration")),
        ContainsSubstring("incompatible evidence_harness"));
    CHECK_THROWS_WITH(
        parse(row(
            "unknown.role", "some case", "mapped",
            "DOC0395 section", "cache-integration",
            "cache-other")),
        ContainsSubstring("invalid evidence_role 'cache-other'"));
}

TEST_CASE("global migration validation rejects unknown Catch mappings",
          "[cache][unit][migration-ledger]") {
    const auto ledger =
        parse(row("unknown.case.id", "unknown case", "mapped"));
    using Catch::Matchers::ContainsSubstring;
    CHECK_THROWS_WITH(
        bioseq::cache_test::migration::validate_global_ledger(
            ledger, std::unordered_set<std::string>{"known case"},
            std::filesystem::path{}),
        ContainsSubstring("unknown Catch2 case 'unknown case'"));
}

TEST_CASE("mapped migration rejects deletion already started",
          "[cache][unit][migration-ledger]") {
    const std::unordered_set<std::string> cases{
        "mapped case", "passing case", "replacement case"};
    const auto not_ready = parse(
        row(
            "mapped.id", "mapped case", "mapped",
            "test/tap/deleted.pl")
        + row("passing.id", "passing case", "passing"));
    using Catch::Matchers::ContainsSubstring;
    CHECK_THROWS_WITH(
        bioseq::cache_test::migration::validate_global_ledger(
            not_ready, cases, std::filesystem::current_path(),
            std::vector<std::filesystem::path>{"test/tap/deleted.pl"}),
        ContainsSubstring(
            "deletion started during migration tracking"));
}

TEST_CASE("per-harness validation ignores cross-harness missing evidence but rejects in-scope missing evidence",
          "[cache][unit][migration-ledger]") {
    using namespace bioseq::cache_test::migration;
    const auto ledger = parse(
        row(
            "integration.id", "integration case", "passing",
            "DOC0395 section", "cache-integration")
        + row(
            "benchmark.id", "benchmark case", "passing",
            "DOC0395 section", "cache-bench", "cache-bench"));
    const std::array integration_evidence{
        probe_observation{
            .owner = evidence_harness::cache_integration,
            .role = evidence_role::cache_integration,
            .stable_id = "integration.id",
            .test_case = "integration case",
            .section = "section",
            .passed = true}};
    CHECK_NOTHROW(validate_harness_evidence(
        ledger, evidence_harness::cache_integration,
        integration_evidence));

    using Catch::Matchers::ContainsSubstring;
    CHECK_THROWS_WITH(
        validate_harness_evidence(
            ledger, evidence_harness::cache_bench, {}),
        ContainsSubstring(
            "benchmark.id owned by cache-bench: expected 1, observed 0"));
    CHECK_THROWS_WITH(
        validate_combined_evidence(ledger, integration_evidence),
        ContainsSubstring(
            "benchmark.id owned by cache-bench: expected 1, observed 0"));
}

TEST_CASE("per-report validation ignores sibling roles but rejects in-role missing evidence",
          "[cache][unit][migration-ledger]") {
    using namespace bioseq::cache_test::migration;
    const auto ledger = parse(
        row(
            "support.id", "support case", "passing",
            "DOC0395 section", "cache-bioseq-integration",
            "cache-bioseq-support-integration")
        + row(
            "production.id", "production case", "passing",
            "DOC0395 section", "cache-bioseq-integration",
            "cache-bioseq-production-integration"));
    const std::array support_evidence{
        probe_observation{
            .owner = evidence_harness::cache_bioseq_integration,
            .role =
                evidence_role::cache_bioseq_support_integration,
            .stable_id = "support.id",
            .test_case = "support case",
            .section = "section",
            .passed = true}};
    CHECK_NOTHROW(validate_report_evidence(
        ledger, evidence_harness::cache_bioseq_integration,
        evidence_role::cache_bioseq_support_integration,
        support_evidence));

    using Catch::Matchers::ContainsSubstring;
    CHECK_THROWS_WITH(
        validate_report_evidence(
            ledger, evidence_harness::cache_bioseq_integration,
            evidence_role::cache_bioseq_production_integration, {}),
        ContainsSubstring(
            "production.id owned by cache-bioseq-integration"));
    CHECK_THROWS_WITH(
        validate_harness_evidence(
            ledger, evidence_harness::cache_bioseq_integration,
            support_evidence),
        ContainsSubstring(
            "production.id owned by cache-bioseq-integration"));
}

TEST_CASE("proof-ready migration requires complete evidence while exact legacy assets remain present",
          "[cache][unit][migration-ledger]") {
    using namespace bioseq::cache_test::migration;
    const auto ledger = parse(
        row("passing.id", "passing case", "passing")
        + row(
            "second.id", "replacement case", "passing"));
    const std::unordered_set<std::string> cases{
        "passing case", "replacement case"};
    const std::array evidence{
        probe_observation{
            .owner = evidence_harness::cache_integration,
            .role = evidence_role::cache_integration,
            .stable_id = "passing.id",
            .test_case = "passing case",
            .section = "section",
            .passed = true},
        probe_observation{
            .owner = evidence_harness::cache_integration,
            .role = evidence_role::cache_integration,
            .stable_id = "second.id",
            .test_case = "replacement case",
            .section = "section",
            .passed = true}};
    using Catch::Matchers::ContainsSubstring;
    CHECK_THROWS_WITH(
        validate_global_ledger(
            ledger, cases, source_root(), {present_asset()}, {},
            retirement_phase::proof_ready),
        ContainsSubstring(
            "passing.id owned by cache-integration: expected 1, observed 0"));

    const auto validation = validate_global_ledger(
        ledger, cases, source_root(), {present_asset()}, evidence,
        retirement_phase::proof_ready);
    CHECK(validation.deletion_ready);
    CHECK(validation.proof_ready);
    CHECK_FALSE(validation.retirement_complete);
    CHECK_FALSE(validation.legacy_deletion_started);
    CHECK(validation.passing_rows == 2);
    CHECK(validation.retired_with_proof_rows == 0);
}

TEST_CASE("retired migration requires complete evidence and every exact legacy asset absent",
          "[cache][unit][migration-ledger]") {
    using namespace bioseq::cache_test::migration;
    const auto ledger =
        parse(row(
            "passing.id", "passing case", "retired_with_proof"));
    const std::filesystem::path absent{
        "__bioseq_cache_retired_asset__"};
    REQUIRE_FALSE(std::filesystem::exists(source_root() / absent));
    const std::array evidence{
        probe_observation{
            .owner = evidence_harness::cache_integration,
            .role = evidence_role::cache_integration,
            .stable_id = "passing.id",
            .test_case = "passing case",
            .section = "section",
            .passed = true}};
    const auto validation = validate_global_ledger(
        ledger, std::unordered_set<std::string>{"passing case"},
        source_root(), {absent}, evidence, retirement_phase::retired);
    CHECK(validation.deletion_ready);
    CHECK(validation.proof_ready);
    CHECK(validation.retirement_complete);
    CHECK(validation.legacy_deletion_started);
}

TEST_CASE("migration retirement rejects a partial exact-inventory deletion",
          "[cache][unit][migration-ledger]") {
    using namespace bioseq::cache_test::migration;
    const auto ledger =
        parse(row("passing.id", "passing case", "passing"));
    const std::filesystem::path absent{
        "__bioseq_cache_partially_deleted_asset__"};
    REQUIRE_FALSE(std::filesystem::exists(source_root() / absent));
    const std::array evidence{
        probe_observation{
            .owner = evidence_harness::cache_integration,
            .role = evidence_role::cache_integration,
            .stable_id = "passing.id",
            .test_case = "passing case",
            .section = "section",
            .passed = true}};
    using Catch::Matchers::ContainsSubstring;
    CHECK_THROWS_WITH(
        validate_global_ledger(
            ledger, std::unordered_set<std::string>{"passing case"},
            source_root(), {present_asset(), absent}, evidence,
            retirement_phase::proof_ready),
        ContainsSubstring("legacy asset inventory is partially deleted"));
    CHECK_THROWS_WITH(
        validate_global_ledger(
            ledger, std::unordered_set<std::string>{"passing case"},
            source_root(), {present_asset(), absent}, evidence,
            retirement_phase::retired),
        ContainsSubstring("legacy asset inventory is partially deleted"));
}

TEST_CASE("proof and retired phases reject mixed ledger lifecycle states",
          "[cache][unit][migration-ledger]") {
    using namespace bioseq::cache_test::migration;
    const auto ledger = parse(
        row("passing.id", "passing case", "passing")
        + row(
            "retired.id", "retired case",
            "retired_with_proof"));
    const std::array evidence{
        probe_observation{
            .owner = evidence_harness::cache_integration,
            .role = evidence_role::cache_integration,
            .stable_id = "passing.id",
            .test_case = "passing case",
            .section = "section",
            .passed = true},
        probe_observation{
            .owner = evidence_harness::cache_integration,
            .role = evidence_role::cache_integration,
            .stable_id = "retired.id",
            .test_case = "retired case",
            .section = "section",
            .passed = true}};
    const std::unordered_set<std::string> cases{
        "passing case", "retired case"};
    using Catch::Matchers::ContainsSubstring;
    CHECK_THROWS_WITH(
        validate_global_ledger(
            ledger, cases, source_root(), {present_asset()}, evidence,
            retirement_phase::proof_ready),
        ContainsSubstring(
            "proof-ready phase requires every migration-ledger row"));
    CHECK_THROWS_WITH(
        validate_global_ledger(
            ledger, cases, source_root(),
            {std::filesystem::path{
                "__bioseq_cache_absent_mixed_asset__"}},
            evidence, retirement_phase::retired),
        ContainsSubstring(
            "retired phase requires every migration-ledger row"));
}

TEST_CASE("exact legacy inventory rejects wrong cardinality and path substitution",
          "[cache][unit][migration-ledger]") {
    using namespace bioseq::cache_test::migration;
    const auto untracked = parse(
        row("passing.id", "passing case", "passing"));
    const std::unordered_set<std::string> cases{"passing case"};
    using Catch::Matchers::ContainsSubstring;
    CHECK_THROWS_WITH(
        validate_global_ledger(
            untracked, cases, source_root(),
            std::vector<std::filesystem::path>(84, present_asset()),
            {}, retirement_phase::tracking, 85),
        ContainsSubstring("84 entries; expected 85"));
    CHECK_THROWS_WITH(
        validate_global_ledger(
            untracked, cases, source_root(),
            std::vector<std::filesystem::path>(86, present_asset()),
            {}, retirement_phase::tracking, 85),
        ContainsSubstring("86 entries; expected 85"));

    const auto tracked = parse(row(
        "tracked.id", "passing case", "mapped",
        "test/unit/test_legacy_migration_ledger.cpp"));
    CHECK_THROWS_WITH(
        validate_global_ledger(
            tracked, cases, module_root(),
            {"test/unit/test_dependency_directory.cpp"}),
        ContainsSubstring(
            "legacy asset inventory omits ledger path "
            "test/unit/test_legacy_migration_ledger.cpp"));
}

TEST_CASE("combined migration evidence rejects substituted Catch provenance",
          "[cache][unit][migration-ledger]") {
    using namespace bioseq::cache_test::migration;
    const auto ledger =
        parse(row("passing.id", "passing case", "passing"));
    const std::array substituted{
        probe_observation{
            .owner = evidence_harness::cache_integration,
            .role = evidence_role::cache_integration,
            .stable_id = "passing.id",
            .test_case = "different case",
            .section = "section",
            .passed = true}};
    using Catch::Matchers::ContainsSubstring;
    CHECK_THROWS_WITH(
        validate_combined_evidence(ledger, substituted),
        ContainsSubstring(
            "combined migration evidence provenance mismatch"));
}

TEST_CASE("migration contract document loaders parse retained bytes",
          "[cache][unit][migration-ledger]") {
    using namespace bioseq::cache_test::migration;
    const std::string ledger_document =
        std::string(ledger_header) + "\n"
        + row("retained.id", "retained case", "mapped");
    const ledger retained =
        load_ledger_document(ledger_document);
    REQUIRE(retained.rows.size() == 1);
    CHECK(retained.rows.front().stable_id == "retained.id");

    const auto catch_cases =
        load_catch_case_manifest_document(
            "retained case\nsecond case\n");
    const std::unordered_set<std::string> expected_cases{
        "retained case", "second case"};
    CHECK(catch_cases == expected_cases);
    using Catch::Matchers::ContainsSubstring;
    CHECK_THROWS_WITH(
        load_catch_case_manifest_document(
            "retained case\nretained case\n"),
        ContainsSubstring("duplicates test case 'retained case'"));
}
