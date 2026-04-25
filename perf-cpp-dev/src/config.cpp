#include <perfcpp/counter/config.hpp>
#include <perfcpp/hardware_info.hpp>

perf::Process perf::Process::Any = Process{ -1 };
perf::Process perf::Process::Calling = Process{ 0 };
perf::CpuCore perf::CpuCore::Any = CpuCore{ -1 };

perf::Config::Config() noexcept
{
  /// Try to read the number of generic performance counters from the hardware (either from cpuid or by trying).
  if (const auto generic_counters = HardwareInfo::physical_generic_performance_counters_per_logical_core();
      generic_counters > 0U) {
    /// If that worked, also read the number of events per physical performance counter.
    this->_num_physical_counters = generic_counters;
    this->_num_events_per_physical_counter = HardwareInfo::events_per_physical_performance_counter();
  }
}