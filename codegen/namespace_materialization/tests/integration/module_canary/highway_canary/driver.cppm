module;

#include <array>
#include <cstdint>

module bioseq.highway.gcc_canary;

import :highway_registry;

namespace bioseq::highway_gcc_canary {

[[nodiscard]] auto run_imported_binder_canary() noexcept -> canary_run {
  if (!attributed_binder_is_linked()) {
    return {};
  }
  constexpr std::uint32_t count = 16;
  std::array<std::uint32_t, count> input{};
  std::array<std::uint32_t, count> first_output{};
  std::array<std::uint32_t, count> second_output{};
  for (std::uint32_t index = 0; index < count; ++index) {
    input[index] = index * std::uint32_t{3} + std::uint32_t{7};
  }

  const target_binding* first = select_target(0);
  const target_binding* second = select_target(second_target());
  if (first == nullptr || second == nullptr || first == second
      || first->target == second->target) {
    return {};
  }

  first->kernels->add_one(input.data(), first_output.data(), count);
  second->kernels->add_one(input.data(), second_output.data(), count);

  std::uint32_t checksum = 0;
  for (std::uint32_t index = 0; index < count; ++index) {
    const std::uint32_t expected = input[index] + std::uint32_t{1};
    if (first_output[index] != expected
        || second_output[index] != expected) {
      return {};
    }
    checksum += first_output[index] + second_output[index];
  }

  return canary_run{
      .ok = true,
      .first_target = first->target,
      .second_target = second->target,
      .first_name = first->target_name,
      .second_name = second->target_name,
      .checksum = checksum,
  };
}

}  // namespace bioseq::highway_gcc_canary
