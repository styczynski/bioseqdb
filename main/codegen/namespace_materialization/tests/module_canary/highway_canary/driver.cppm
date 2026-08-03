module;

#include <cstdint>

module bioseq.highway.gcc_canary;

import :highway_registry;

namespace bioseq::highway_gcc_canary {

[[nodiscard]] CanaryRun run_imported_binder_canary() noexcept {
  constexpr std::uint32_t count = 16;
  std::uint32_t input[count]{};
  std::uint32_t first_output[count]{};
  std::uint32_t second_output[count]{};
  for (std::uint32_t index = 0; index < count; ++index) {
    input[index] = index * std::uint32_t{3} + std::uint32_t{7};
  }

  const TargetBinding* first = select_target(0);
  const TargetBinding* second = select_target(second_target());
  if (first == nullptr || second == nullptr || first == second
      || first->target == second->target) {
    return {};
  }

  first->kernels->add_one(input, first_output, count);
  second->kernels->add_one(input, second_output, count);

  std::uint32_t checksum = 0;
  for (std::uint32_t index = 0; index < count; ++index) {
    const std::uint32_t expected = input[index] + std::uint32_t{1};
    if (first_output[index] != expected
        || second_output[index] != expected) {
      return {};
    }
    checksum += first_output[index] + second_output[index];
  }

  return CanaryRun{
      .ok = true,
      .first_target = first->target,
      .second_target = second->target,
      .first_name = first->target_name,
      .second_name = second->target_name,
      .checksum = checksum,
  };
}

}  // namespace bioseq::highway_gcc_canary
