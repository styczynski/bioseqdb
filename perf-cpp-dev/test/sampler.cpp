#include "access_benchmark.h"
#include <catch2/catch_test_macros.hpp>

#if defined(__x86_64__) || defined(__i386__)
#include <iostream>
#include <perfcpp/hardware_info.hpp>
#include <perfcpp/sampler.hpp>

class AverageCounter
{
public:
  AverageCounter& operator+=(const std::uint64_t value) noexcept
  {
    _sum += value;
    ++_count;
    return *this;
  }

  [[nodiscard]] std::uint64_t get() const noexcept { return _count > 0ULL ? _sum / _count : 0ULL; }

private:
  std::uint64_t _sum{ 0U };
  std::uint64_t _count{ 0U };
};

TEST_CASE("config", "[Sampler]")
{
  /// Benchmark used for all sampling tests.
  auto readonly_benchmark = perf::test::AccessBenchmark{ /* is random */ true, 1024U /* MB */ };

  SECTION("empty sampler")
  {
    auto sampler = perf::Sampler{};
    sampler.values().logical_instruction_pointer(true);

    REQUIRE_THROWS(sampler.open());
  }
}

TEST_CASE("sampling", "[Sampler]")
{
  /// Benchmark used for all sampling tests.
  auto readonly_benchmark = perf::test::AccessBenchmark{ /* is random */ true, 2048 /* MB */ };

  SECTION("IP with cycles")
  {
    auto sampler = perf::Sampler{};
    REQUIRE_NOTHROW(sampler.trigger("cycles"));
    sampler.values().logical_instruction_pointer(true);

    REQUIRE_NOTHROW(sampler.open());
    REQUIRE_NOTHROW(sampler.start());

    readonly_benchmark.run();

    REQUIRE_NOTHROW(sampler.stop());

    const auto samples = sampler.result();
    REQUIRE_FALSE(samples.empty());
    for (const auto& sample : samples) {
      REQUIRE_FALSE(sample.metadata().timestamp().has_value());
      REQUIRE(sample.instruction_execution().logical_instruction_pointer().has_value());
    }

    REQUIRE_NOTHROW(sampler.close());
  }

  SECTION("IP with cycles (typed)")
  {
    auto sampler = perf::Sampler{};
    REQUIRE_NOTHROW(sampler.trigger(perf::Cycles{}));
    sampler.values().logical_instruction_pointer(true);

    REQUIRE_NOTHROW(sampler.open());
    REQUIRE_NOTHROW(sampler.start());

    readonly_benchmark.run();

    REQUIRE_NOTHROW(sampler.stop());

    const auto samples = sampler.result();
    REQUIRE_FALSE(samples.empty());
    for (const auto& sample : samples) {
      REQUIRE_FALSE(sample.metadata().timestamp().has_value());
      REQUIRE(sample.instruction_execution().logical_instruction_pointer().has_value());
    }

    REQUIRE_NOTHROW(sampler.close());
  }

  SECTION("re-start")
  {
    auto sampler = perf::Sampler{};
    REQUIRE_NOTHROW(sampler.trigger("cycles"));
    sampler.values().logical_instruction_pointer(true).timestamp(true);

    REQUIRE_NOTHROW(sampler.open());

    REQUIRE_NOTHROW(sampler.start());
    readonly_benchmark.run();
    REQUIRE_NOTHROW(sampler.stop());

    const auto samples1 = sampler.result(true);
    REQUIRE_FALSE(samples1.empty());
    REQUIRE(samples1.back().metadata().timestamp().has_value());

    REQUIRE_NOTHROW(sampler.start());
    readonly_benchmark.run();
    REQUIRE_NOTHROW(sampler.stop());

    const auto samples2 = sampler.result(true);
    REQUIRE_FALSE(samples2.empty());

    REQUIRE(samples2.front().metadata().timestamp().has_value());
    REQUIRE(samples1.back().metadata().timestamp().value() < samples2.front().metadata().timestamp().value());

    REQUIRE_NOTHROW(sampler.close());
  }

  SECTION("sample period")
  {
    auto sampler1 = perf::Sampler{};

    REQUIRE_NOTHROW(sampler1.trigger(perf::Cycles{}, perf::Precision::RequestZeroSkid, perf::Period{ 200000 }));
    sampler1.values().logical_instruction_pointer(true).timestamp(true);

    REQUIRE_NOTHROW(sampler1.open());

    REQUIRE_NOTHROW(sampler1.start());
    readonly_benchmark.run();
    REQUIRE_NOTHROW(sampler1.stop());

    const auto samples1 = sampler1.result(true);
    REQUIRE_FALSE(samples1.empty());
    REQUIRE_NOTHROW(sampler1.close());

    auto sampler2 = perf::Sampler{};

    REQUIRE_NOTHROW(sampler2.trigger("cycles", perf::Precision::RequestZeroSkid, perf::Period{ 800000 }));
    sampler2.values().logical_instruction_pointer(true).timestamp(true);

    REQUIRE_NOTHROW(sampler2.open());

    REQUIRE_NOTHROW(sampler2.start());
    readonly_benchmark.run();
    REQUIRE_NOTHROW(sampler2.stop());

    const auto samples2 = sampler2.result(true);
    REQUIRE_FALSE(samples2.empty());
    REQUIRE_NOTHROW(sampler2.close());

    REQUIRE(samples1.size() > (samples2.size() * 3U));
  }

  SECTION("mem-loads")
  {
    auto sampler = perf::Sampler{};

    if (perf::HardwareInfo::is_intel()) {
      REQUIRE_NOTHROW(sampler.trigger(perf::MemoryLoads{}, perf::Precision::MustHaveZeroSkid, perf::Period{ 16000 }));
    } else if (perf::HardwareInfo::is_amd()) {
      REQUIRE_NOTHROW(
        sampler.trigger(perf::IbsOp{ /*upos = */ true }, perf::Precision::RequestZeroSkid, perf::Period{ 16000 }));
    }

    REQUIRE_NOTHROW(
      sampler.values().logical_memory_address(true).data_source(true).data_access_latency(true).instruction_latency(
        true));
    REQUIRE_NOTHROW(sampler.open());

    REQUIRE_NOTHROW(sampler.start());
    readonly_benchmark.run();
    REQUIRE_NOTHROW(sampler.stop());

    const auto samples = sampler.result();
    REQUIRE_FALSE(samples.empty());

    auto l1d = AverageCounter{};
    auto l2 = AverageCounter{};
    auto l3 = AverageCounter{};
    auto ram = AverageCounter{};

    for (const auto& sample : samples) {
      if (sample.data_access().is_load() && sample.data_access().logical_memory_address().has_value()) {
        if (perf::HardwareInfo::is_intel()) {
          if (perf::HardwareInfo::is_intel_12th_generation_or_newer()) {
            REQUIRE(sample.data_access().latency().cache_access().has_value());
            if (sample.data_access().source().has_value()) {
              if (sample.data_access().source()->is_l1_hit()) {
                l1d += sample.data_access().latency().cache_access().value();
              } else if (sample.data_access().source()->is_l2_hit()) {
                l2 += sample.data_access().latency().cache_access().value();
              } else if (sample.data_access().source()->is_l3_hit()) {
                l3 += sample.data_access().latency().cache_access().value();
              } else if (sample.data_access().source()->is_memory_hit()) {
                ram += sample.data_access().latency().cache_access().value();
              }
            }
          } else {
            REQUIRE(sample.instruction_execution().latency().instruction_retirement().has_value());
            if (sample.data_access().source().has_value()) {
              if (sample.data_access().source()->is_l1_hit()) {
                l1d += sample.instruction_execution().latency().instruction_retirement().value();
              } else if (sample.data_access().source()->is_l2_hit()) {
                l2 += sample.instruction_execution().latency().instruction_retirement().value();
              } else if (sample.data_access().source()->is_l3_hit()) {
                l3 += sample.instruction_execution().latency().instruction_retirement().value();
              } else if (sample.data_access().source()->is_memory_hit()) {
                ram += sample.instruction_execution().latency().instruction_retirement().value();
              }
            }
          }

        } else if (perf::HardwareInfo::is_amd()) {
          REQUIRE(sample.data_access().latency().cache_miss().has_value());
          if (sample.data_access().source().has_value()) {
            if (sample.data_access().source()->is_l1_hit()) {
              l1d += sample.data_access().latency().cache_miss().value_or(0U);
            } else if (sample.data_access().source()->is_l2_hit()) {
              l2 += sample.data_access().latency().cache_miss().value();
            } else if (sample.data_access().source()->is_l3_hit()) {
              l3 += sample.data_access().latency().cache_miss().value();
            } else if (sample.data_access().source()->is_memory_hit()) {
              ram += sample.data_access().latency().cache_miss().value();
            }
          }
        }
      }
    }

    if (perf::HardwareInfo::is_intel()) {
      REQUIRE(l1d.get() < 10U);
    } else if (perf::HardwareInfo::is_amd()) {
      REQUIRE(l1d.get() == 0U);
    }

    REQUIRE(l3.get() < 100U);
    REQUIRE(ram.get() > 100U);
  }

  SECTION("mem-loads-with-filter")
  {
    auto filtered_sampler = perf::Sampler{};
    auto not_filtered_sampler = perf::Sampler{};

    if (perf::HardwareInfo::is_intel()) {
      REQUIRE_NOTHROW(
        filtered_sampler.trigger(perf::MemoryLoads{ 60U }, perf::Precision::MustHaveZeroSkid, perf::Period{ 8000 }));
      REQUIRE_NOTHROW(not_filtered_sampler.trigger(
        perf::MemoryLoads{ 1U }, perf::Precision::MustHaveZeroSkid, perf::Period{ 8000 }));
    } else if (perf::HardwareInfo::is_amd()) {
      REQUIRE_NOTHROW(filtered_sampler.trigger(perf::IbsOp{ /*upos = */ true, /* l3miss only = */ true },
                                               perf::Precision::RequestZeroSkid,
                                               perf::Period{ 16000 }));
      REQUIRE_NOTHROW(not_filtered_sampler.trigger(
        perf::IbsOp{ /*upos = */ true }, perf::Precision::RequestZeroSkid, perf::Period{ 16000 }));
    }

    REQUIRE_NOTHROW(filtered_sampler.values()
                      .logical_memory_address(true)
                      .data_source(true)
                      .data_access_latency(true)
                      .instruction_latency(true));
    REQUIRE_NOTHROW(not_filtered_sampler.values()
                      .logical_memory_address(true)
                      .data_source(true)
                      .data_access_latency(true)
                      .instruction_latency(true));

    // Warmup run
    readonly_benchmark.run();

    // Sample without filter.
    REQUIRE_NOTHROW(not_filtered_sampler.open());

    REQUIRE_NOTHROW(not_filtered_sampler.start());
    readonly_benchmark.run();
    REQUIRE_NOTHROW(not_filtered_sampler.stop());

    const auto not_filtered_samples = not_filtered_sampler.result();
    REQUIRE_FALSE(not_filtered_samples.empty());

    // Sample with filter.
    REQUIRE_NOTHROW(filtered_sampler.open());

    REQUIRE_NOTHROW(filtered_sampler.start());
    readonly_benchmark.run();
    REQUIRE_NOTHROW(filtered_sampler.stop());

    const auto filtered_samples = filtered_sampler.result();
    REQUIRE_FALSE(filtered_samples.empty());

    // Compare sample results
    REQUIRE(filtered_samples.size() < not_filtered_samples.size());
  }

  SECTION("metric-l1d-per-load")
  {
    auto counter_definition = perf::CounterDefinition{};
    /// Add metric that calculates the L1d miss ratio.
    counter_definition.add("L1d-misses-per-load", "'L1-dcache-load-misses'/'L1-dcache-loads'");

    auto sampler = perf::Sampler{ counter_definition };

    REQUIRE_NOTHROW(sampler.trigger("cycles", perf::Precision::AllowArbitrarySkid, perf::Period{ 100000 }));
    REQUIRE_NOTHROW(sampler.values().timestamp(true).counter({ "L1d-misses-per-load" }));
    REQUIRE_NOTHROW(sampler.open());

    REQUIRE_NOTHROW(sampler.start());
    readonly_benchmark.run();
    REQUIRE_NOTHROW(sampler.stop());

    const auto samples = sampler.result(true);
    REQUIRE_FALSE(samples.empty());

    auto last_timestamp = std::optional<std::uint64_t>{ std::nullopt };

    for (const auto& sample : samples) {
      REQUIRE(sample.metadata().timestamp().has_value());
      if (last_timestamp.has_value()) {
        REQUIRE(sample.metadata().timestamp().value() > last_timestamp.value());
      }
      last_timestamp = sample.metadata().timestamp();

      REQUIRE(sample.counter().has_value());
      REQUIRE(sample.counter()->get("L1d-misses-per-load").has_value());
      REQUIRE(sample.counter()->get("L1d-misses-per-load").value() > 0);
      REQUIRE(sample.counter()->get("L1d-misses-per-load").value() < 1.1);
    }
  }
}
#endif