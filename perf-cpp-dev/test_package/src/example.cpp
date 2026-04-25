#include <cstdlib>
#include <iostream>
#include <perfcpp/event_counter.h>

int
main()
{
  auto counter_definitions = perf::CounterDefinition{};
  auto event_counter = perf::EventCounter{ counter_definitions };

  if (event_counter.add("instructions")) {
    std::cout << "perf-cpp: Successfully configured 'instructions' counter." << std::endl;
  } else {
    std::cout << "perf-cpp: Could not add 'instructions' counter (may require perf_event_paranoid <= 2)." << std::endl;
  }

  return EXIT_SUCCESS;
}
