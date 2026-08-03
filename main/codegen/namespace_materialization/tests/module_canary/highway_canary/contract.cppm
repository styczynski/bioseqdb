module;

#include <cstdint>

export module bioseq.highway.gcc_canary.contract;

export namespace bioseq::highway_gcc_canary {

using AddOneKernel = void (*)(
    const std::uint32_t*, std::uint32_t*, std::uint32_t) noexcept;

struct KernelTable final {
  AddOneKernel add_one = nullptr;

  [[nodiscard]] constexpr bool complete() const noexcept {
    return add_one != nullptr;
  }
};

struct TargetBinding final {
  std::int64_t target = 0;
  const char* target_name = nullptr;
  const KernelTable* kernels = nullptr;

  [[nodiscard]] constexpr bool complete() const noexcept {
    return target != 0 && target_name != nullptr && kernels != nullptr
        && kernels->complete();
  }
};

struct CanaryRun final {
  bool ok = false;
  std::int64_t first_target = 0;
  std::int64_t second_target = 0;
  const char* first_name = nullptr;
  const char* second_name = nullptr;
  std::uint32_t checksum = 0;
};

}  // namespace bioseq::highway_gcc_canary
