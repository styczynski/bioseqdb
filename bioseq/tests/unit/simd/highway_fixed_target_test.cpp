/**
 * @file
 * @brief Verifies fixed-target Highway probes without runtime redispatch.
 */

#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.highway.test_support;

#if defined(BIOSEQ_TEST_INDEXES)

namespace {

/**
 * @brief Returns the target bit bound to the generated test kernel table.
 * @return Nonzero Highway target bit selected by the fixed-target fixture.
 */
[[nodiscard]] auto fixed_target() -> std::int64_t {
    return bioseq::simd_dispatch::highway_test_target_binding().target;
}

TEST_CASE("fixed-target SIMD matrix helpers satisfy lane and property checks",
          "[component:bioseq][layer:unit][feature:simd_primitives][resource:highway]") {
    bioseq_highway_simd_utils_probe_receipt receipt{};
    REQUIRE(bioseq_highway_run_simd_utils_probe(&receipt));
    REQUIRE(receipt.target == fixed_target());
    REQUIRE(receipt.passed_properties == 0x3fu);
    REQUIRE(receipt.cases_run != 0u);
    REQUIRE(receipt.cases_passed == receipt.cases_run);
    REQUIRE(receipt.u8_lanes != 0u);
    REQUIRE(receipt.u16_lanes != 0u);
    REQUIRE(receipt.u32_lanes != 0u);
    REQUIRE(receipt.u64_lanes != 0u);
}

TEST_CASE("fixed-target sequence iteration satisfies property checks",
          "[component:bioseq][layer:unit][feature:simd_primitives][resource:highway]") {
    bioseq_highway_sequence_ops_probe_receipt receipt{};
    REQUIRE(bioseq_highway_run_sequence_ops_probe(&receipt));
    REQUIRE(receipt.target == fixed_target());
    REQUIRE(receipt.passed_properties == 0x7u);
    REQUIRE(receipt.cases_run != 0u);
    REQUIRE(receipt.cases_passed == receipt.cases_run);
}

TEST_CASE("fixed-target Lexic and minimap hashers match across widths",
          "[component:bioseq][layer:unit][feature:simd_primitives][resource:highway]") {
    bioseq_highway_hashers_probe_receipt receipt{};
    REQUIRE(bioseq_highway_run_hashers_probe(&receipt));
    REQUIRE(receipt.target == fixed_target());
    REQUIRE(receipt.passed_properties == 0xffu);
    REQUIRE(receipt.cases_run == 8u);
    REQUIRE(receipt.cases_passed == receipt.cases_run);
}

TEST_CASE("fixed-target two-stack internals satisfy low-level property checks",
          "[component:bioseq][layer:unit][feature:simd_primitives][resource:highway]") {
    bioseq_highway_two_stack_probe_receipt receipt{};
    REQUIRE(bioseq_highway_run_two_stack_probe(&receipt));
    REQUIRE(receipt.target == fixed_target());
    REQUIRE(receipt.passed_properties == 0x1fu);
    REQUIRE(receipt.cases_run != 0u);
    REQUIRE(receipt.cases_passed == receipt.cases_run);
    REQUIRE(receipt.lane_cases != 0u);
    REQUIRE(receipt.mask_cases != 0u);
    REQUIRE(receipt.swar_cases != 0u);
    REQUIRE(receipt.decode_cases != 0u);
    REQUIRE(receipt.selector_cases != 0u);
}

} // namespace

#endif // BIOSEQ_TEST_INDEXES
