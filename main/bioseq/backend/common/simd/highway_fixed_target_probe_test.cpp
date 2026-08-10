#include <tests/support/common.hpp>
#include "highway_test_probes/contract.h"

import bioseq.highway.test_support;

#if defined(BIOSEQ_TEST_INDEXES)

namespace {

[[nodiscard]] std::int64_t fixed_target() {
    return bioseq::simd_dispatch::highway_test_target_binding().target;
}

TEST_CASE("Fixed-target SIMD matrix helpers retain six live properties",
          "[highway][fixed-target][probe][simd-utils]") {
    BioseqHighwaySimdUtilsProbeReceipt receipt{};
    REQUIRE(bioseq_highway_probe_simd_utils(&receipt));
    REQUIRE(receipt.target == fixed_target());
    REQUIRE(receipt.passed_properties == 0x3fu);
    REQUIRE(receipt.cases_run != 0u);
    REQUIRE(receipt.cases_passed == receipt.cases_run);
    REQUIRE(receipt.u8_lanes != 0u);
    REQUIRE(receipt.u16_lanes != 0u);
    REQUIRE(receipt.u32_lanes != 0u);
    REQUIRE(receipt.u64_lanes != 0u);
}

TEST_CASE("Fixed-target sequence iteration retains three live properties",
          "[highway][fixed-target][probe][sequence-ops]") {
    BioseqHighwaySequenceOpsProbeReceipt receipt{};
    REQUIRE(bioseq_highway_probe_sequence_ops(&receipt));
    REQUIRE(receipt.target == fixed_target());
    REQUIRE(receipt.passed_properties == 0x7u);
    REQUIRE(receipt.cases_run != 0u);
    REQUIRE(receipt.cases_passed == receipt.cases_run);
}

TEST_CASE("Fixed-target Lexic and minimap hashers retain width parity",
          "[highway][fixed-target][probe][hashers]") {
    BioseqHighwayHashersProbeReceipt receipt{};
    REQUIRE(bioseq_highway_probe_hashers(&receipt));
    REQUIRE(receipt.target == fixed_target());
    REQUIRE(receipt.passed_properties == 0xffu);
    REQUIRE(receipt.cases_run == 8u);
    REQUIRE(receipt.cases_passed == receipt.cases_run);
}

TEST_CASE("Fixed-target two-stack internals retain live low-level properties",
          "[highway][fixed-target][probe][two-stack]") {
    BioseqHighwayTwoStackProbeReceipt receipt{};
    REQUIRE(bioseq_highway_probe_two_stack(&receipt));
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

}  // namespace

#endif  // BIOSEQ_TEST_INDEXES
