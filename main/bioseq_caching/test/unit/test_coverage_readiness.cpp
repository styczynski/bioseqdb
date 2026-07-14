#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

import bioseq.cache.core;

using namespace bioseq::cache;

TEST_CASE("SQL boolean helpers implement WHERE and IS NOT TRUE") {
    REQUIRE_FALSE(passes_where(SqlBool::FalseValue));
    REQUIRE(passes_where(SqlBool::TrueValue));
    REQUIRE_FALSE(passes_where(SqlBool::Unknown));

    REQUIRE(is_not_true(SqlBool::FalseValue) == SqlBool::TrueValue);
    REQUIRE(is_not_true(SqlBool::TrueValue) == SqlBool::FalseValue);
    REQUIRE(is_not_true(SqlBool::Unknown) == SqlBool::TrueValue);
}

TEST_CASE("coverage branches are disjoint, bag preserving, and NULL correct") {
    const std::vector<CoverageRow> rows{
        {.occurrence = 1,
         .visible_value = 7,
         .requested_predicate = SqlBool::TrueValue,
         .source_predicates = {SqlBool::TrueValue, SqlBool::TrueValue}},
        {.occurrence = 2,
         .visible_value = 7,
         .requested_predicate = SqlBool::TrueValue,
         .source_predicates = {SqlBool::FalseValue, SqlBool::TrueValue}},
        {.occurrence = 3,
         .visible_value = 7,
         .requested_predicate = SqlBool::TrueValue,
         .source_predicates = {SqlBool::Unknown, SqlBool::Unknown}},
        {.occurrence = 4,
         .visible_value = 8,
         .requested_predicate = SqlBool::Unknown,
         .source_predicates = {SqlBool::TrueValue, SqlBool::FalseValue}},
        {.occurrence = 5,
         .visible_value = 8,
         .requested_predicate = SqlBool::FalseValue,
         .source_predicates = {SqlBool::TrueValue, SqlBool::TrueValue}},
    };
    const std::array<bool, 2> valid{true, true};
    const auto assignment = partition_coverage(rows, valid);

    REQUIRE(assignment.branch(0) == std::vector<std::uint64_t>{1});
    REQUIRE(assignment.branch(1) == std::vector<std::uint64_t>{2});
    REQUIRE(assignment.residual() == std::vector<std::uint64_t>{3});
    REQUIRE(assignment.occurrence_count() == 3);
    REQUIRE(
        assignment.all_occurrences()
        == std::vector<std::uint64_t>{1, 2, 3});
}

TEST_CASE("an invalid earlier source cannot exclude a later valid source") {
    const std::vector<CoverageRow> rows{
        {.occurrence = 41,
         .requested_predicate = SqlBool::TrueValue,
         .source_predicates = {SqlBool::TrueValue, SqlBool::TrueValue}},
        {.occurrence = 42,
         .requested_predicate = SqlBool::TrueValue,
         .source_predicates = {SqlBool::TrueValue, SqlBool::FalseValue}},
    };
    const std::array<bool, 2> valid{false, true};
    const auto assignment = partition_coverage(rows, valid);

    REQUIRE(assignment.branch(0).empty());
    REQUIRE(assignment.branch(1) == std::vector<std::uint64_t>{41});
    REQUIRE(assignment.residual() == std::vector<std::uint64_t>{42});
}

TEST_CASE("source density ordering is exact at uint64 boundaries") {
    constexpr auto maximum = (std::numeric_limits<std::uint64_t>::max)();
    REQUIRE(fraction_less_exact(1, 3, 2, 5));
    REQUIRE_FALSE(fraction_less_exact(2, 5, 1, 3));
    REQUIRE_FALSE(fraction_less_exact(7, 9, 7, 9));
    REQUIRE(fraction_less_exact(maximum - 1, maximum, maximum, maximum));
    REQUIRE(fraction_less_exact(1, 2, 1, 0));
    REQUIRE_FALSE(fraction_less_exact(1, 0, 1, 2));

    CoverageSourceOrderKey complete{
        .complete = true,
        .estimated_covered_rows = maximum,
        .stored_bytes = maximum - 1,
        .claim_sequence = 8,
    };
    CoverageSourceOrderKey incomplete = complete;
    incomplete.complete = false;
    REQUIRE(coverage_source_precedes(complete, incomplete));

    CoverageSourceOrderKey denser = complete;
    denser.stored_bytes = maximum;
    REQUIRE(coverage_source_precedes(complete, denser));

    CoverageSourceOrderKey later = complete;
    later.claim_sequence = 9;
    REQUIRE(coverage_source_precedes(complete, later));
}

TEST_CASE("readiness gates exact and reusable entries independently") {
    REQUIRE_FALSE(readiness_allows(
        CacheCorrectnessReadiness::PassthroughOnly,
        CacheEntryKind::ExactFinalResult,
        WriteObservability::None,
        true,
        false));
    REQUIRE(readiness_allows(
        CacheCorrectnessReadiness::RelationFreeOnly,
        CacheEntryKind::ExactFinalResult,
        WriteObservability::None,
        true,
        false));
    REQUIRE_FALSE(readiness_allows(
        CacheCorrectnessReadiness::RelationFreeOnly,
        CacheEntryKind::ExactFinalResult,
        WriteObservability::DatabaseWideFallback,
        false,
        false));
    REQUIRE(readiness_allows(
        CacheCorrectnessReadiness::GenericMvccReady,
        CacheEntryKind::ExactFinalResult,
        WriteObservability::DatabaseWideFallback,
        false,
        false));
    REQUIRE(readiness_allows(
        CacheCorrectnessReadiness::ProviderCompleteReady,
        CacheEntryKind::ExactFinalResult,
        WriteObservability::ProviderComplete,
        false,
        true));
    REQUIRE_FALSE(readiness_allows(
        CacheCorrectnessReadiness::ProviderCompleteReady,
        CacheEntryKind::ReusableCoverage,
        WriteObservability::ProviderComplete,
        false,
        true));
    REQUIRE(readiness_allows(
        CacheCorrectnessReadiness::ReusableCoverageReady,
        CacheEntryKind::ReusableCoverage,
        WriteObservability::ProviderComplete,
        false,
        true));
    REQUIRE_FALSE(readiness_allows(
        static_cast<CacheCorrectnessReadiness>(99),
        CacheEntryKind::ExactFinalResult,
        WriteObservability::None,
        true,
        false));
}
