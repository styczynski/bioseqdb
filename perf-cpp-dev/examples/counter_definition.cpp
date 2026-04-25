#include <iostream>
#include <perfcpp/counter_definition.h>
#include <perfcpp/hardware_info.h>

int
main()
{
  std::cout
    << "libperf-cpp example: This example prints all automatically read events stored in the perf::CounterDefinition.\n"
    << std::endl;

  std::cout << "Scanning the underlying hardware for hardware counters..." << std::endl;
  std::cout << "Generic Hardware Counters   = "
            << std::uint16_t(perf::HardwareInfo::physical_generic_performance_counters_per_logical_core()) << "\n";
  std::cout << "Fixed Hardware Counters     = "
            << std::uint16_t(perf::HardwareInfo::physical_fixed_performance_counters_per_logical_core()) << "\n";
  std::cout << "Events per Hardware Counter = "
            << std::uint16_t(perf::HardwareInfo::events_per_physical_performance_counter()) << "\n"
            << std::endl;

  /// Create custom instance of the counter definition.
  const auto counter_definition = perf::CounterDefinition{};

  /// Dump to the console without adding further events.
  std::cout << "Detected events:\n" << counter_definition.to_string() << std::endl;

  return 0;
}