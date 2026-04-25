#include <catch2/catch_test_macros.hpp>
#include <perfcpp/hardware_info.hpp>

TEST_CASE("number of generic performance counters", "[HardwareInfo]")
{
  REQUIRE(perf::HardwareInfo::physical_generic_performance_counters_per_logical_core() > 1U);
}

TEST_CASE("number of fixed performance counters", "[HardwareInfo]")
{
  /// Fixed counters are only available on Intel (typically 3: instructions, cycles, ref-cycles).
  /// On AMD and ARM, this returns 0.
  const auto fixed_counters = perf::HardwareInfo::physical_fixed_performance_counters_per_logical_core();
  if (perf::HardwareInfo::is_intel()) {
    REQUIRE(fixed_counters >= 3U);
  } else {
    REQUIRE(fixed_counters == 0U);
  }
}

TEST_CASE("number of events per performance counter", "[HardwareInfo]")
{
  REQUIRE(perf::HardwareInfo::events_per_physical_performance_counter() > 1U);
}