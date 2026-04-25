#pragma once

#include <cstdint>
#include <linux/perf_event.h>
#include <optional>
#include <perfcpp/feature.h>

namespace perf {
/**
 * Branch types that can be filtered when recording branches via sampling.
 */
#if defined(PERFCPP_NO_SAMPLE_BRANCH_CALL) || defined(PERFCPP_NO_SAMPLE_BRANCH_IND_JUMP)
enum class BranchType : std::uint64_t
#else
enum class BranchType : std::uint16_t
#endif
{
  None = 0ULL,

  User = PERF_SAMPLE_BRANCH_USER,
  Kernel = PERF_SAMPLE_BRANCH_KERNEL,
  HyperVisor = PERF_SAMPLE_BRANCH_HV,

  Any = PERF_SAMPLE_BRANCH_ANY,
#ifndef PERFCPP_NO_SAMPLE_BRANCH_CALL
  Call = PERF_SAMPLE_BRANCH_ANY_CALL,
#else
  Call = 1ULL << 61,
#endif
#ifndef PERFCPP_NO_SAMPLE_BRANCH_CALL
  DirectCall = PERF_SAMPLE_BRANCH_CALL,
#else
  DirectCall = 1ULL << 62,
#endif
  IndirectCall = PERF_SAMPLE_BRANCH_IND_CALL,
  Return = PERF_SAMPLE_BRANCH_ANY_RETURN,
#ifndef PERFCPP_NO_SAMPLE_BRANCH_IND_JUMP
  IndirectJump = PERF_SAMPLE_BRANCH_IND_JUMP,
#else
  IndirectJump = 1ULL << 63,
#endif
  Conditional = PERF_SAMPLE_BRANCH_COND,
  TransactionalMemoryAbort = PERF_SAMPLE_BRANCH_ABORT_TX,
  InTransaction = PERF_SAMPLE_BRANCH_IN_TX,
  NotInTransaction = PERF_SAMPLE_BRANCH_NO_TX
};

/**
 * A Branch represents one branch from the branch stack, including information where the branch started (and in case of
 * jmp/call where the branch ended), if the branch was predicted correctly, and how long
 */
class Branch
{
public:
  Branch(const std::uintptr_t instruction_pointer_from,
         const std::uintptr_t instruction_pointer_to,
         const bool is_mispredicted,
         const bool is_predicted,
         const bool is_in_transaction,
         const bool is_transaction_abort,
         const std::optional<std::uint16_t> cycles)
    : _instruction_pointer_from(instruction_pointer_from)
    , _instruction_pointer_to(instruction_pointer_to)
    , _is_mispredicted(is_mispredicted)
    , _is_predicted(is_predicted)
    , _is_in_transaction(is_in_transaction)
    , _is_transaction_abort(is_transaction_abort)
    , _cycles(cycles)
  {
  }

  /**
   * @return The instruction pointer the branch started.
   */
  [[nodiscard]] std::uintptr_t instruction_pointer_from() const noexcept { return _instruction_pointer_from; }

  /**
   * @return The instruction pointer the branch ended.
   */
  [[nodiscard]] std::uintptr_t instruction_pointer_to() const noexcept { return _instruction_pointer_to; }

  /**
   * @return True, if the branch was not predicted properly.
   */
  [[nodiscard]] bool is_mispredicted() const noexcept { return _is_mispredicted; }

  /**
   * @return True, if the branch was predicted correctly.
   */
  [[nodiscard]] bool is_predicted() const noexcept { return _is_predicted; }

  /**
   * @return True, if the branch was within a memory transaction.
   */
  [[nodiscard]] bool is_in_transaction() const noexcept { return _is_in_transaction; }

  /**
   * @return True, if the branch was a transaction abort.
   */
  [[nodiscard]] bool is_transaction_abort() const noexcept { return _is_transaction_abort; }

  /**
   * @return The number of cycles of the branch (zero if not supported on the underlying hardware).
   */
  [[nodiscard]] std::optional<std::uint16_t> cycles() const noexcept { return _cycles; }

private:
  std::uintptr_t _instruction_pointer_from;
  std::uintptr_t _instruction_pointer_to;
  bool _is_mispredicted;
  bool _is_predicted;
  bool _is_in_transaction;
  bool _is_transaction_abort;
  std::optional<std::uint16_t> _cycles;
};
}