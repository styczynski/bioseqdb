#pragma once

#include <cstdint>
#include <optional>
#include <perfcpp/counter/result.hpp>
#include <perfcpp/sample/branch.hpp>
#include <perfcpp/sample/cgroup.hpp>
#include <perfcpp/sample/context_switch.hpp>
#include <perfcpp/sample/data_access.hpp>
#include <perfcpp/sample/instruction_execution.hpp>
#include <perfcpp/sample/metadata.hpp>
#include <perfcpp/sample/registers.hpp>
#include <perfcpp/sample/throttle.hpp>

namespace perf {

class Sample
{
public:
  Sample() = default;
  ~Sample() = default;

  Sample(const Sample&) = default;
  Sample(Sample&&) noexcept = default;

  Sample& operator=(const Sample&) = default;
  Sample& operator=(Sample&&) noexcept = default;

  /**
   * Set the list of events and values for the sample.
   * @param counter_result Counter values.
   */
  void counter(CounterResult&& counter_result) noexcept { _counter_result.emplace(std::move(counter_result)); }

  /**
   * Set the branch stack.
   * @param branch_stack Branch stack to set.
   */
  void branch_stack(std::vector<Branch>&& branch_stack) noexcept { _branch_stack.emplace(std::move(branch_stack)); }

  /**
   * Set the user stack.
   * @param user_stack User stack to set.
   */
  void user_stack(std::vector<std::byte>&& user_stack) noexcept { _user_stack.emplace(std::move(user_stack)); }

  /**
   * Set the user registers.
   * @param user_registers User registers to set.
   */
  void user_registers(RegisterValues&& user_registers) noexcept { _user_registers.emplace(std::move(user_registers)); }

  /**
   * Set the kernel registers.
   * @param kernel_registers User registers to set.
   */
  void kernel_registers(RegisterValues&& kernel_registers) noexcept
  {
    _kernel_registers.emplace(std::move(kernel_registers));
  }

  /**
   * Set the cgroup ID.
   * @param cgroup_id CGroup ID to set.
   */
  void cgroup_id(const std::uint64_t cgroup_id) noexcept { _cgroup_id.emplace(cgroup_id); }

  /**
   * Set the cgroup details.
   * @param cgroup CGroup details to set.
   */
  void cgroup(CGroup&& cgroup) noexcept { _cgroup.emplace(std::move(cgroup)); }

  /**
   * Set the context switch details.
   * @param context_switch Context switch details to set.
   */
  void context_switch(ContextSwitch&& context_switch) noexcept { _context_switch.emplace(context_switch); }

  /**
   * Set the throttle details.
   * @param throttle Throttle details to set.
   */
  void throttle(Throttle&& throttle) noexcept { _throttle.emplace(throttle); }

  /**
   * Set the raw data.
   * @param raw Raw data to set.
   */
  void raw(std::vector<std::byte>&& raw) noexcept { _raw.emplace(std::move(raw)); }

  /**
   * Set the count loss.
   * @param count_loss Count loss to set.
   */
  void count_loss(const std::uint64_t count_loss) noexcept { _count_loss.emplace(count_loss); }

  /**
   * @return Metadata of the sample.
   */
  [[nodiscard]] const Metadata& metadata() const noexcept { return _metadata; }

  /**
   * @return Metadata of the sample.
   */
  [[nodiscard]] Metadata& metadata() noexcept { return _metadata; }

  /**
   * @return Instruction execution details.
   */
  [[nodiscard]] const InstructionExecution& instruction_execution() const noexcept { return _instruction_execution; }

  /**
   * @return Instruction execution details.
   */
  [[nodiscard]] InstructionExecution& instruction_execution() noexcept { return _instruction_execution; }

  /**
   * @return Data access details.
   */
  [[nodiscard]] const DataAccess& data_access() const noexcept { return _data_access; }

  /**
   * @return Data access details.
   */
  [[nodiscard]] DataAccess& data_access() noexcept { return _data_access; }

  /**
   * @return Counter result.
   */
  [[nodiscard]] const std::optional<CounterResult>& counter() const noexcept { return _counter_result; }

  /**
   * @return Optional branch stack.
   */
  [[nodiscard]] const std::optional<std::vector<Branch>>& branch_stack() const noexcept { return _branch_stack; }

  /**
   * @return Optional user stack.
   */
  [[nodiscard]] const std::optional<std::vector<std::byte>>& user_stack() const noexcept { return _user_stack; }

  /**
   * @return User-level registers, if sampled.
   */
  [[nodiscard]] const std::optional<RegisterValues>& user_registers() const noexcept { return _user_registers; }

  /**
   * @return Kernel-level registers, if sampled.
   */
  [[nodiscard]] const std::optional<RegisterValues>& kernel_registers() const noexcept { return _kernel_registers; }

  /**
   * @return Optional cgroup ID.
   */
  [[nodiscard]] std::optional<std::uint64_t> cgroup_id() const noexcept { return _cgroup_id; }

  /**
   * @return Optional cgroup details.
   */
  [[nodiscard]] const std::optional<CGroup>& cgroup() const noexcept { return _cgroup; }

  /**
   * @return Optional context switch details.
   */
  [[nodiscard]] const std::optional<ContextSwitch>& context_switch() const noexcept { return _context_switch; }

  /**
   * @return Optional throttle details.
   */
  [[nodiscard]] const std::optional<Throttle>& throttle() const noexcept { return _throttle; }

  /**
   * @return Optional raw data.
   */
  [[nodiscard]] const std::optional<std::vector<std::byte>>& raw() const noexcept { return _raw; }

  /**
   * @return Optional count loss.
   */
  [[nodiscard]] std::optional<std::uint64_t> count_loss() const noexcept { return _count_loss; }

private:
  Metadata _metadata;
  InstructionExecution _instruction_execution;
  DataAccess _data_access;
  std::optional<CounterResult> _counter_result{ std::nullopt };
  std::optional<std::vector<Branch>> _branch_stack{ std::nullopt };
  std::optional<std::vector<std::byte>> _user_stack{ std::nullopt };
  std::optional<RegisterValues> _user_registers{ std::nullopt };
  std::optional<RegisterValues> _kernel_registers{ std::nullopt };
  std::optional<std::uint64_t> _cgroup_id{ std::nullopt };
  std::optional<CGroup> _cgroup{ std::nullopt };
  std::optional<ContextSwitch> _context_switch{ std::nullopt };
  std::optional<Throttle> _throttle{ std::nullopt };
  std::optional<std::vector<std::byte>> _raw{ std::nullopt };
  std::optional<std::uint64_t> _count_loss{ std::nullopt };
};
}