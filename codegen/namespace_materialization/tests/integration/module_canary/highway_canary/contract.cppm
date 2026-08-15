module;

#include <cstdint>

export module bioseq.highway.gcc_canary.contract;

export namespace bioseq::highway_gcc_canary {

using add_one_kernel = void (*)(
    const std::uint32_t*, std::uint32_t*, std::uint32_t) noexcept;

struct kernel_table final {
  add_one_kernel add_one = nullptr;

  [[nodiscard]] constexpr auto complete() const noexcept -> bool {
    return add_one != nullptr;
  }
};

struct target_binding final {
  std::int64_t target = 0;
  const char* target_name = nullptr;
  const kernel_table* kernels = nullptr;

  [[nodiscard]] constexpr auto complete() const noexcept -> bool {
    return target != 0 && target_name != nullptr && kernels != nullptr
        && kernels->complete();
  }
};

struct canary_run final {
  bool ok = false;
  std::int64_t first_target = 0;
  std::int64_t second_target = 0;
  const char* first_name = nullptr;
  const char* second_name = nullptr;
  std::uint32_t checksum = 0;
};

}  // namespace bioseq::highway_gcc_canary
