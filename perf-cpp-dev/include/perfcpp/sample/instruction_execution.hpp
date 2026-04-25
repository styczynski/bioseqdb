#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace perf {
class InstructionExecution
{
public:
  enum class InstructionType : std::uint8_t
  {
    DataAccess,
    Branch,
    Return
  };

  enum class BranchType : std::uint8_t
  {
    Taken,
    Retired,
    Mispredicted,
    Fuse
  };

  class Latency
  {
  public:
    /**
     * Set the uop tag to retirement latency.
     * @param uop_tag_to_retirement Uop tag to retirement latency.
     */
    void uop_tag_to_retirement(const std::uint32_t uop_tag_to_retirement) noexcept
    {
      _uop_tag_to_retirement = uop_tag_to_retirement;
    }

    /**
     * Set the uop completion to retirement latency.
     * @param uop_completion_to_retirement Uop completion to retirement latency.
     */
    void uop_completion_to_retirement(const std::uint32_t uop_completion_to_retirement) noexcept
    {
      _uop_completion_to_retirement = uop_completion_to_retirement;
    }

    /**
     * Set the instruction retirement latency.
     * @param instruction_retirement Instruction retirement latency.
     */
    void instruction_retirement(const std::uint32_t instruction_retirement) noexcept
    {
      _instruction_retirement = instruction_retirement;
    }

    /**
     * Set the fetch latency.
     * @param fetch Fetch latency.
     */
    void fetch(const std::uint32_t fetch) noexcept { _fetch = fetch; }

    /**
     * @return Uop tag to retirement latency, if available. std::nullopt otherwise.
     */
    [[nodiscard]] std::optional<std::uint32_t> uop_tag_to_retirement() const noexcept { return _uop_tag_to_retirement; }

    /**
     * @return Uop completion to retirement latency, if available. std::nullopt otherwise.
     */
    [[nodiscard]] std::optional<std::uint32_t> uop_completion_to_retirement() const noexcept
    {
      return _uop_completion_to_retirement;
    }

    /**
     * @return Uop tag to completion latency, if available. std::nullopt otherwise.
     */
    [[nodiscard]] std::optional<std::uint32_t> uop_tag_to_completion() const noexcept
    {
      if (_uop_tag_to_retirement.has_value() && _uop_completion_to_retirement.has_value()) {
        return _uop_tag_to_retirement.value() - _uop_completion_to_retirement.value();
      }

      return std::nullopt;
    }

    /**
     * @return Instruction retirement latency, if available. std::nullopt otherwise.
     */
    [[nodiscard]] std::optional<std::uint32_t> instruction_retirement() const noexcept
    {
      return _instruction_retirement;
    }

    /**
     * @return Fetch latency, if available. std::nullopt otherwise.
     */
    [[nodiscard]] std::optional<std::uint32_t> fetch() const noexcept { return _fetch; }

  private:
    std::optional<std::uint32_t> _uop_tag_to_retirement{ std::nullopt };
    std::optional<std::uint32_t> _uop_completion_to_retirement{ std::nullopt };
    std::optional<std::uint32_t> _instruction_retirement{ std::nullopt };
    std::optional<std::uint32_t> _fetch{ std::nullopt };
  };

  class TLB
  {
  public:
    TLB(const bool is_l1_miss, const std::optional<std::uint64_t> l1_page_size, const bool is_l2_miss) noexcept
      : _is_l1_miss(is_l1_miss)
      , _l1_page_size(l1_page_size)
      , _is_l2_miss(is_l2_miss)
    {
    }
    TLB(const TLB&) = default;
    TLB(TLB&&) noexcept = default;
    ~TLB() noexcept = default;
    TLB& operator=(const TLB&) = default;
    TLB& operator=(TLB&&) noexcept = default;

    /**
     * @return True, if the fetch was an iTLB miss.
     */
    [[nodiscard]] bool is_l1_miss() const noexcept { return _is_l1_miss; }

    /**
     * @return Size of the iTLB page (in bytes), if available.
     */
    [[nodiscard]] std::optional<std::uint64_t> l1_page_size() const noexcept { return _l1_page_size; }

    /**
     * @return True, if the fetch was an sTLB miss.
     */
    [[nodiscard]] bool is_l2_miss() const noexcept { return _is_l2_miss; }

  private:
    bool _is_l1_miss;
    std::optional<std::uint64_t> _l1_page_size;
    bool _is_l2_miss;
  };

  /**
   * Represents the instruction cache miss state across the memory hierarchy (L1, L2, L3).
   */
  class Cache
  {
  public:
    Cache(const bool is_l1_miss, const bool is_l2_miss, const bool is_l3_miss) noexcept
      : _is_l1_miss(is_l1_miss)
      , _is_l2_miss(is_l2_miss)
      , _is_l3_miss(is_l3_miss)
    {
    }

    Cache(const Cache&) = default;
    Cache(Cache&&) noexcept = default;
    ~Cache() noexcept = default;
    Cache& operator=(const Cache&) = default;
    Cache& operator=(Cache&&) noexcept = default;

    /**
     * @return True, if the fetch was an iTLB miss in L1 cache.
     */
    [[nodiscard]] bool is_l1_miss() const noexcept { return _is_l1_miss; }

    /**
     * @return True, if the access missed in L2 cache.
     */
    [[nodiscard]] bool is_l2_miss() const noexcept { return _is_l2_miss; }

    /**
     * @return True, if the access missed in L3 cache.
     */
    [[nodiscard]] bool is_l3_miss() const noexcept { return _is_l3_miss; }

  private:
    bool _is_l1_miss;
    bool _is_l2_miss;
    bool _is_l3_miss;
  };

  /**
   * Represents the state of an instruction fetch operation in the CPU pipeline.
   */
  class Fetch
  {
  public:
    Fetch(const bool is_complete, const bool is_valid) noexcept
      : _is_complete(is_complete)
      , _is_valid(is_valid)
    {
    }

    Fetch(const Fetch&) = default;
    Fetch(Fetch&&) noexcept = default;
    ~Fetch() noexcept = default;
    Fetch& operator=(const Fetch&) = default;
    Fetch& operator=(Fetch&&) noexcept = default;

    /**
     * @return True, if the fetch operation has completed.
     */
    [[nodiscard]] bool is_complete() const noexcept { return _is_complete; }

    /**
     * @return True, if the sampled fetch entry is valid.
     */
    [[nodiscard]] bool is_valid() const noexcept { return _is_valid; }

  private:
    bool _is_complete;
    bool _is_valid;
  };

  class HardwareTransactionAbort
  {
  public:
    /**
     * Set whether this is an elision transaction.
     * @param is_elision_transaction Elision transaction indicator.
     */
    void elision_transaction(const bool is_elision_transaction) noexcept
    {
      _is_elision_transaction = is_elision_transaction;
    }

    /**
     * Set whether this is a generic transaction.
     * @param is_generic_transaction Generic transaction indicator.
     */
    void generic_transaction(const bool is_generic_transaction) noexcept
    {
      _is_generic_transaction = is_generic_transaction;
    }

    /**
     * Set whether this is a synchronous abort.
     * @param is_synchronous_abort Synchronous abort indicator.
     */
    void synchronous_abort(const bool is_synchronous_abort) noexcept { _is_synchronous_abort = is_synchronous_abort; }

    /**
     * Set whether this is retryable.
     * @param is_retryable Retryable indicator.
     */
    void retryable(const bool is_retryable) noexcept { _is_retryable = is_retryable; }

    /**
     * Set whether this is an abort due to memory conflict.
     * @param is_due_to_memory_conflict Memory conflict abort indicator.
     */
    void due_to_memory_conflict(const bool is_due_to_memory_conflict) noexcept
    {
      _is_due_to_memory_conflict = is_due_to_memory_conflict;
    }

    /**
     * Set whether this is an abort due to write capacity conflict.
     * @param is_due_to_write_capacity_conflict Write capacity conflict abort indicator.
     */
    void due_to_write_capacity_conflict(const bool is_due_to_write_capacity_conflict) noexcept
    {
      _is_due_to_write_capacity_conflict = is_due_to_write_capacity_conflict;
    }

    /**
     * Set whether this is an abort due to read capacity conflict.
     * @param is_due_to_read_capacity_conflict Read capacity conflict abort indicator.
     */
    void due_to_read_capacity_conflict(const bool is_due_to_read_capacity_conflict) noexcept
    {
      _is_due_to_read_capacity_conflict = is_due_to_read_capacity_conflict;
    }

    /**
     * Set the user specified abort code.
     * @param user_code User specified abort code.
     */
    void user_specified_code(const std::uint32_t user_code) noexcept { _user_specified_code = user_code; }

    /**
     * @return Elision transaction indicator.
     */
    [[nodiscard]] bool is_elision_transaction() const noexcept { return _is_elision_transaction; }

    /**
     * @return Generic transaction indicator.
     */
    [[nodiscard]] bool is_generic_transaction() const noexcept { return _is_generic_transaction; }

    /**
     * @return Synchronous abort indicator.
     */
    [[nodiscard]] bool is_synchronous_abort() const noexcept { return _is_synchronous_abort; }

    /**
     * @return Retryable indicator.
     */
    [[nodiscard]] bool is_retryable() const noexcept { return _is_retryable; }

    /**
     * @return Memory conflict abort indicator.
     */
    [[nodiscard]] bool is_due_to_memory_conflict() const noexcept { return _is_due_to_memory_conflict; }

    /**
     * @return Write capacity conflict abort indicator.
     */
    [[nodiscard]] bool is_due_to_write_capacity_conflict() const noexcept { return _is_due_to_write_capacity_conflict; }

    /**
     * @return Read capacity conflict abort indicator.
     */
    [[nodiscard]] bool is_due_to_read_capacity_conflict() const noexcept { return _is_due_to_read_capacity_conflict; }

    /**
     * @return User specified abort code.
     */
    [[nodiscard]] std::uint32_t user_specified_code() const noexcept { return _user_specified_code; }

  private:
    bool _is_elision_transaction{ false };
    bool _is_generic_transaction{ false };
    bool _is_synchronous_abort{ false };
    bool _is_retryable{ false };
    bool _is_due_to_memory_conflict{ false };
    bool _is_due_to_write_capacity_conflict{ false };
    bool _is_due_to_read_capacity_conflict{ false };
    std::uint32_t _user_specified_code{ 0U };
  };

  /**
   * Set the instruction type.
   * @param type Instruction type.
   */
  void type(const InstructionType type) noexcept { _type = type; }

  /**
   * Set the logical instruction pointer.
   * @param logical_instruction_pointer Logical instruction pointer.
   */
  void logical_instruction_pointer(const std::uintptr_t logical_instruction_pointer) noexcept
  {
    _logical_instruction_pointer = logical_instruction_pointer;
  }

  /**
   * Set the physical instruction pointer.
   * @param physical_instruction_pointer Physical instruction pointer.
   */
  void physical_instruction_pointer(const std::uintptr_t physical_instruction_pointer) noexcept
  {
    _physical_instruction_pointer = physical_instruction_pointer;
  }

  /**
   * Set whether the instruction pointer is exact.
   * @param is_instruction_pointer_exact Instruction pointer exactness indicator.
   */
  void instruction_pointer_exact(const bool is_instruction_pointer_exact) noexcept
  {
    _is_instruction_pointer_exact = is_instruction_pointer_exact;
  }

  /**
   * Set whether the instruction is locked.
   * @param is_locked Lock indicator.
   */
  void locked(const std::optional<bool> is_locked) noexcept { _is_locked = is_locked; }

  /**
   * Set the latency information.
   * @param latency Latency object.
   */
  void latency(const Latency& latency) noexcept { _latency = latency; }

  /**
   * Set the cache information.
   * @param cache Cache object.
   */
  void cache(Cache&& cache) noexcept { _cache = cache; }

  /**
   * Set the TLB information.
   * @param tlb TLB object.
   */
  void tlb(TLB&& tlb) noexcept { _tlb = tlb; }

  /**
   * Set the fetch information.
   * @param fetch Fetch object.
   */
  void fetch(Fetch&& fetch) noexcept { _fetch = fetch; }

  /**
   * Set the branch type if the instruction is a branch.
   * @param branch_type Branch type.
   */
  void branch_type(const BranchType branch_type) noexcept { _branch_type = branch_type; }

  /**
   * Set the hardware transaction abort information.
   * @param hardware_transaction_abort Hardware transaction abort object.
   */
  void hardware_transaction_abort(HardwareTransactionAbort hardware_transaction_abort) noexcept
  {
    _hardware_transaction_abort = hardware_transaction_abort;
  }

  /**
   * Set the call chain.
   * @param callchain Vector of call chain addresses.
   */
  void callchain(std::vector<std::uintptr_t>&& callchain) noexcept { _callchain = std::move(callchain); }

  /**
   * Set the page size.
   * @param page_size Page size.
   */
  void page_size(const std::uint64_t page_size) noexcept { _page_size = page_size; }

  /**
   * @return Instruction type, if available. std::nullopt otherwise.
   */
  [[nodiscard]] std::optional<InstructionType> type() const noexcept { return _type; }

  /**
   * @return Logical instruction pointer, if available. std::nullopt otherwise.
   */
  [[nodiscard]] std::optional<std::uintptr_t> logical_instruction_pointer() const noexcept
  {
    return _logical_instruction_pointer;
  }

  /**
   * @return Physical instruction pointer, if available. std::nullopt otherwise.
   */
  [[nodiscard]] std::optional<std::uintptr_t> physical_instruction_pointer() const noexcept
  {
    return _physical_instruction_pointer;
  }

  /**
   * @return Instruction pointer exactness indicator.
   */
  [[nodiscard]] bool is_instruction_pointer_exact() const noexcept { return _is_instruction_pointer_exact; }

  /**
   * @return Cache object.
   */
  [[nodiscard]] const std::optional<Cache>& cache() const noexcept { return _cache; }

  /**
   * @return Latency object.
   */
  [[nodiscard]] const Latency& latency() const noexcept { return _latency; }

  /**
   * @return Latency object reference for modification.
   */
  [[nodiscard]] Latency& latency() noexcept { return _latency; }

  /**
   * @return TLB object.
   */
  [[nodiscard]] const std::optional<TLB>& tlb() const noexcept { return _tlb; }

  /**
   * @return Fetch object.
   */
  [[nodiscard]] const std::optional<Fetch>& fetch() const noexcept { return _fetch; }

  /**
   * @return Branch type, if the instruction was a branch.
   */
  [[nodiscard]] std::optional<BranchType> branch_type() const noexcept { return _branch_type; }

  /**
   * @return Hardware transaction abort information, if available. std::nullopt otherwise.
   */
  [[nodiscard]] const std::optional<HardwareTransactionAbort>& hardware_transaction_abort() const noexcept
  {
    return _hardware_transaction_abort;
  }

  /**
   * @return Call chain, if available. std::nullopt otherwise.
   */
  [[nodiscard]] const std::optional<std::vector<std::uintptr_t>>& callchain() const noexcept { return _callchain; }

  /**
   * @return Page size, if available. std::nullopt otherwise.
   */
  [[nodiscard]] std::optional<std::uint64_t> page_size() const noexcept { return _page_size; }

private:
  std::optional<InstructionType> _type{ std::nullopt };
  std::optional<std::uintptr_t> _logical_instruction_pointer{ std::nullopt };
  std::optional<std::uintptr_t> _physical_instruction_pointer{ std::nullopt };
  bool _is_instruction_pointer_exact{ false };
  std::optional<bool> _is_locked{ std::nullopt };
  std::optional<Cache> _cache;
  Latency _latency;
  std::optional<TLB> _tlb;
  std::optional<Fetch> _fetch;
  std::optional<BranchType> _branch_type;
  std::optional<HardwareTransactionAbort> _hardware_transaction_abort{ std::nullopt };
  std::optional<std::vector<std::uintptr_t>> _callchain{ std::nullopt };
  std::optional<std::uint64_t> _page_size{ std::nullopt };
};

/**
 * Convert InstructionType to its string representation.
 *
 * @param type The instruction type to convert.
 * @return String representation of the instruction type.
 */
[[nodiscard]] inline std::string
to_string(const InstructionExecution::InstructionType type)
{
  switch (type) {
    case InstructionExecution::InstructionType::DataAccess:
      return "data_access";
    case InstructionExecution::InstructionType::Branch:
      return "branch";
    case InstructionExecution::InstructionType::Return:
      return "return";
  }
  return "unknown";
}

/**
 * Convert BranchType to its string representation.
 *
 * @param type The branch type to convert.
 * @return String representation of the branch type.
 */
[[nodiscard]] inline std::string
to_string(const InstructionExecution::BranchType type)
{
  switch (type) {
    case InstructionExecution::BranchType::Taken:
      return "taken";
    case InstructionExecution::BranchType::Retired:
      return "retired";
    case InstructionExecution::BranchType::Mispredicted:
      return "mispredicted";
    case InstructionExecution::BranchType::Fuse:
      return "fuse";
  }
  return "unknown";
}
}