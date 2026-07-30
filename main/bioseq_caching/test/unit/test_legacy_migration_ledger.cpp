#include <catch2/matchers/catch_matchers_string.hpp>
#include <catch2/catch_test_macros.hpp>

#include "legacy_migration_ledger.hpp"

#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>

namespace {

[[nodiscard]] std::string row(
    std::string_view stable_id,
    std::string_view catch_case,
    std::string_view status,
    std::string_view legacy_path = "DOC0395 section") {
    return std::string(legacy_path)
        + "\tassertion\t" + std::string(stable_id)
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

TEST_CASE("legacy deletion requires every migration row to be final",
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
            not_ready, cases, std::filesystem::path{}),
        ContainsSubstring(
            "every row must be passing or retired_with_proof"));

    const auto ready = parse(
        row(
            "passing.id", "passing case", "passing",
            "test/tap/deleted.pl")
        + row(
            "retired.id", "replacement case",
            "retired_with_proof"));
    const auto validation =
        bioseq::cache_test::migration::validate_global_ledger(
            ready, cases, std::filesystem::path{});
    CHECK(validation.legacy_deletion_started);
    CHECK(validation.deletion_ready);
    CHECK(validation.mapped_rows == 0);
    CHECK(validation.passing_rows == 1);
    CHECK(validation.retired_with_proof_rows == 1);
}
