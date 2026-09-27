#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <vector>

import bioseq.cache.core;

using namespace bioseq::cache;

TEST_CASE("cache diagnostic text follows producer and monitor policy",
          "[component:cache][layer:unit][feature:result_cache]") {
    REQUIRE(cache_diagnostic_disclosure(true, false, false) ==
            cache_diagnostic_disclosure_level::full_text);
    REQUIRE(cache_diagnostic_disclosure(false, true, false) ==
            cache_diagnostic_disclosure_level::full_text);
    REQUIRE(cache_diagnostic_disclosure(false, false, true) ==
            cache_diagnostic_disclosure_level::full_text);
    REQUIRE(cache_diagnostic_disclosure(false, false, false) ==
            cache_diagnostic_disclosure_level::summary_only);
}

TEST_CASE("SQL boolean helpers implement WHERE and IS NOT TRUE",
          "[component:cache][layer:unit][feature:result_cache]") {
    REQUIRE_FALSE(passes_where(sql_bool::false_value));
    REQUIRE(passes_where(sql_bool::true_value));
    REQUIRE_FALSE(passes_where(sql_bool::unknown));

    REQUIRE(is_not_true(sql_bool::false_value) == sql_bool::true_value);
    REQUIRE(is_not_true(sql_bool::true_value) == sql_bool::false_value);
    REQUIRE(is_not_true(sql_bool::unknown) == sql_bool::true_value);
}

TEST_CASE("coverage branches are disjoint, bag preserving, and NULL correct",
          "[component:cache][layer:unit][feature:result_cache]") {
    const std::vector<coverage_row> rows{
        {.occurrence = 1,
         .visible_value = 7,
         .requested_predicate = sql_bool::true_value,
         .source_predicates = {sql_bool::true_value, sql_bool::true_value}},
        {.occurrence = 2,
         .visible_value = 7,
         .requested_predicate = sql_bool::true_value,
         .source_predicates = {sql_bool::false_value, sql_bool::true_value}},
        {.occurrence = 3,
         .visible_value = 7,
         .requested_predicate = sql_bool::true_value,
         .source_predicates = {sql_bool::unknown, sql_bool::unknown}},
        {.occurrence = 4,
         .visible_value = 8,
         .requested_predicate = sql_bool::unknown,
         .source_predicates = {sql_bool::true_value, sql_bool::false_value}},
        {.occurrence = 5,
         .visible_value = 8,
         .requested_predicate = sql_bool::false_value,
         .source_predicates = {sql_bool::true_value, sql_bool::true_value}},
    };
    const std::array<bool, 2> valid{true, true};
    const auto assignment = partition_coverage(rows, valid);

    REQUIRE(assignment.branch(0) == std::vector<std::uint64_t>{1});
    REQUIRE(assignment.branch(1) == std::vector<std::uint64_t>{2});
    REQUIRE(assignment.residual() == std::vector<std::uint64_t>{3});
    REQUIRE(assignment.occurrence_count() == 3);
    REQUIRE(assignment.all_occurrences() == std::vector<std::uint64_t>{1, 2, 3});
}

TEST_CASE("an invalid earlier source cannot exclude a later valid source",
          "[component:cache][layer:unit][feature:result_cache]") {
    const std::vector<coverage_row> rows{
        {.occurrence = 41,
         .requested_predicate = sql_bool::true_value,
         .source_predicates = {sql_bool::true_value, sql_bool::true_value}},
        {.occurrence = 42,
         .requested_predicate = sql_bool::true_value,
         .source_predicates = {sql_bool::true_value, sql_bool::false_value}},
    };
    const std::array<bool, 2> valid{false, true};
    const auto assignment = partition_coverage(rows, valid);

    REQUIRE(assignment.branch(0).empty());
    REQUIRE(assignment.branch(1) == std::vector<std::uint64_t>{41});
    REQUIRE(assignment.residual() == std::vector<std::uint64_t>{42});
}

TEST_CASE("source density ordering is exact at uint64 boundaries",
          "[component:cache][layer:unit][feature:result_cache]") {
    constexpr auto maximum = (std::numeric_limits<std::uint64_t>::max)();
    REQUIRE(fraction_less_exact(1, 3, 2, 5));
    REQUIRE_FALSE(fraction_less_exact(2, 5, 1, 3));
    REQUIRE_FALSE(fraction_less_exact(7, 9, 7, 9));
    REQUIRE(fraction_less_exact(maximum - 1, maximum, maximum, maximum));
    REQUIRE(fraction_less_exact(1, 2, 1, 0));
    REQUIRE_FALSE(fraction_less_exact(1, 0, 1, 2));

    coverage_source_order_key complete{
        .complete = true,
        .estimated_covered_rows = maximum,
        .stored_bytes = maximum - 1,
        .claim_sequence = 8,
    };
    coverage_source_order_key incomplete = complete;
    incomplete.complete = false;
    REQUIRE(coverage_source_precedes(complete, incomplete));

    coverage_source_order_key higher_benefit = incomplete;
    higher_benefit.estimated_covered_rows = maximum;
    coverage_source_order_key lower_benefit = complete;
    lower_benefit.estimated_covered_rows = maximum - 1;
    REQUIRE(coverage_source_precedes(higher_benefit, lower_benefit));

    coverage_source_order_key denser = complete;
    denser.stored_bytes = maximum;
    REQUIRE(coverage_source_precedes(complete, denser));

    coverage_source_order_key later = complete;
    later.claim_sequence = 9;
    REQUIRE(coverage_source_precedes(complete, later));
}

TEST_CASE("readiness gates exact and reusable entries independently",
          "[component:cache][layer:unit][feature:result_cache]") {
    REQUIRE_FALSE(readiness_allows(cache_correctness_readiness::passthrough_only,
                                   cache_entry_kind::exact_final_result,
                                   write_observability::none,
                                   true,
                                   false));
    REQUIRE(readiness_allows(cache_correctness_readiness::relation_free_only,
                             cache_entry_kind::exact_final_result,
                             write_observability::none,
                             true,
                             false));
    REQUIRE_FALSE(readiness_allows(cache_correctness_readiness::relation_free_only,
                                   cache_entry_kind::exact_final_result,
                                   write_observability::database_wide_fallback,
                                   false,
                                   false));
    REQUIRE(readiness_allows(cache_correctness_readiness::generic_mvcc_ready,
                             cache_entry_kind::exact_final_result,
                             write_observability::database_wide_fallback,
                             false,
                             false));
    REQUIRE(readiness_allows(cache_correctness_readiness::provider_complete_ready,
                             cache_entry_kind::exact_final_result,
                             write_observability::provider_complete,
                             false,
                             true));
    REQUIRE_FALSE(readiness_allows(cache_correctness_readiness::provider_complete_ready,
                                   cache_entry_kind::reusable_coverage,
                                   write_observability::provider_complete,
                                   false,
                                   true));
    REQUIRE(readiness_allows(cache_correctness_readiness::reusable_coverage_ready,
                             cache_entry_kind::reusable_coverage,
                             write_observability::provider_complete,
                             false,
                             true));
    REQUIRE_FALSE(readiness_allows(static_cast<cache_correctness_readiness>(99),
                                   cache_entry_kind::exact_final_result,
                                   write_observability::none,
                                   true,
                                   false));
}

TEST_CASE("every readiness, kind, observability, and class flag is crossed",
          "[component:cache][layer:unit][feature:result_cache]") {
    const std::array readiness_values{
        cache_correctness_readiness::passthrough_only,
        cache_correctness_readiness::relation_free_only,
        cache_correctness_readiness::generic_mvcc_ready,
        cache_correctness_readiness::provider_complete_ready,
        cache_correctness_readiness::reusable_coverage_ready,
        static_cast<cache_correctness_readiness>(99),
    };
    const std::array kinds{
        cache_entry_kind::exact_final_result,
        cache_entry_kind::reusable_coverage,
        static_cast<cache_entry_kind>(99),
    };
    const std::array observability_values{
        write_observability::none,
        write_observability::provider_complete,
        write_observability::executor_utility_only,
        write_observability::database_wide_fallback,
        static_cast<write_observability>(99),
    };

    const auto expected = [](cache_correctness_readiness readiness,
                             cache_entry_kind kind,
                             write_observability observability,
                             bool relation_free,
                             bool provider_backed) {
        if (!known_readiness(readiness) || !known_observability(observability)) {
            return false;
        }
        if (kind == cache_entry_kind::reusable_coverage) {
            return !relation_free && provider_backed &&
                   readiness >= cache_correctness_readiness::reusable_coverage_ready &&
                   observability == write_observability::provider_complete;
        }
        if (kind != cache_entry_kind::exact_final_result) {
            return false;
        }
        if (provider_backed) {
            return !relation_free &&
                   readiness >= cache_correctness_readiness::provider_complete_ready &&
                   observability == write_observability::provider_complete;
        }
        if (relation_free) {
            return readiness >= cache_correctness_readiness::relation_free_only &&
                   observability == write_observability::none;
        }
        return readiness >= cache_correctness_readiness::generic_mvcc_ready &&
               (observability == write_observability::executor_utility_only ||
                observability == write_observability::database_wide_fallback);
    };

    for (const auto readiness : readiness_values) {
        for (const auto kind : kinds) {
            for (const auto observability : observability_values) {
                for (const bool relation_free : {false, true}) {
                    for (const bool provider_backed : {false, true}) {
                        INFO("readiness=" << static_cast<unsigned>(readiness) << " kind="
                                          << static_cast<unsigned>(kind) << " observability="
                                          << static_cast<unsigned>(observability)
                                          << " relation_free=" << relation_free
                                          << " provider_backed=" << provider_backed);
                        REQUIRE(
                            readiness_allows(
                                readiness, kind, observability, relation_free, provider_backed) ==
                            expected(
                                readiness, kind, observability, relation_free, provider_backed));
                    }
                }
            }
        }
    }
}
