#pragma once
#include <array>
#include <cstdint>
#include <perfcpp/exception.hpp>
#include <perfcpp/sample/branch.hpp>
#include <perfcpp/sample/registers.hpp>

namespace perf {
class SampleRecordingValues
{
public:
  enum class Field : std::uint8_t
  {
    // === Process/Thread Context ===
    ThreadId,
    CpuId,
    CGroup,

    // === Timing ===
    Timestamp,
    Period,

    // === Identifiers ===
    Id,
    StreamId,

    // === Instruction Execution ===
    LogicalInstructionPointer,
    PhysicalInstructionPointer,
    InstructionType,
    BranchType,
    Callchain,
    CodePageSize,

    // === Instruction Performance ===
    InstructionLatency,
    InstructionCache,
    InstructionTLB,
    InstructionFetch,

    // === Data Access ===
    LogicalMemoryAddress,
    PhysicalMemoryAddress,
    DataSource,
    DataPageSize,
    DataTLBPageSize,
    DataAccessLatency,
    DataTLBLatency,
    DataAccessWidth,
    DataAccessMisalignPenalty,
    MHBAllocations,

    // === Branch Sampling ===
    BranchStack,

    // === Registers & Stack ===
    UserRegisters,
    KernelRegisters,
    UserStack,

    // === Performance Counters ===
    PerformanceCounter,

    // === Hardware Transaction Memory ===
    HardwareTransactionAbort,

    // === Special Events ===
    ContextSwitch,
    Throttle,
    MMapInformation,

    // === Raw & Auxiliary ===
    RawValues,
    AuxValues,

    // END
    CountFields
  };

  /**
   * Manage to include the thread id into samples.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#id-of-the-recording-thread
   *
   * @param include True, if the thread id should be included.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& thread_id(const bool include) noexcept
  {
    set(Field::ThreadId, include);
    return *this;
  }

  /**
   * Manage to include the cpu id into samples.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#id-of-the-recording-cpu
   *
   * @param include True, if the cpu id should be included.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& cpu_id(const bool include) noexcept
  {
    set(Field::CpuId, include);
    return *this;
  }

  /**
   * Manage to include the cgroup into samples (only available since Linux 5.7).
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#cgroup
   *
   * @param include True, if the cgroup should be included.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& cgroup([[maybe_unused]] const bool include)
  {
#ifndef PERFCPP_NO_SAMPLE_CGROUP /// Sampling cgroup is supported since Linux 5.7
    set(Field::CGroup, include);
#else
    throw SamplingFeatureIsNotSupported{ "cgroup", "5.7" };
#endif
    return *this;
  }

  /**
   * Manage to include a timestamp into samples.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#time
   *
   * @param include True, if the timestamp should be included.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& timestamp(const bool include) noexcept
  {
    set(Field::Timestamp, include);
    return *this;
  }

  /**
   * Manage to include the period into samples.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#period
   *
   * @param include True, if the period should be included.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& period(const bool include) noexcept
  {
    set(Field::Period, include);
    return *this;
  }

  /**
   * Manage to include the ID into samples.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#id
   *
   * @param include True, if the ID should be included.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& id(const bool include) noexcept
  {
    set(Field::Id, include);
    return *this;
  }

  /**
   * Manage to include the stream id into samples.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#stream-id
   *
   * @param include True, if the stream id should be included.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& stream_id(const bool include) noexcept
  {
    set(Field::StreamId, include);
    return *this;
  }

  /**
   * Manage to include the logical instruction pointer into samples.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#instruction-pointer
   *
   * @param include True, if the logical instruction pointer should be included.
   * @return The SampleRecordingValues instance.
   */
  [[deprecated("SampleRecordingValues::instruction_pointer(bool) will be removed from v0.14. Use "
               "logical_instruction_pointer(bool) instead.")]] SampleRecordingValues&
  instruction_pointer(const bool include) noexcept
  {
    return logical_instruction_pointer(include);
  }

  /**
   * Manage to include the logical instruction pointer into samples.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#instruction-pointer
   *
   * @param include True, if the logical instruction pointer should be included.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& logical_instruction_pointer(const bool include) noexcept
  {
    set(Field::LogicalInstructionPointer, include);
    return *this;
  }

  /**
   * Manage to include the physical instruction pointer into samples.
   * Note: This field is only available on AMD's IBS Fetch PMU.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#instruction-pointer
   *
   * @param include True, if the physical instruction pointer should be included.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& physical_instruction_pointer(const bool include) noexcept
  {
    set(Field::PhysicalInstructionPointer, include);
    return *this;
  }

  /**
   * Manage to include the instruction type into samples.
   * Note: Return and Branch instruction types are only available on AMD's IBS Op PMU.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#instruction-execution
   *
   * @param include True, if the instruction type should be included.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& instruction_type(const bool include) noexcept
  {
    set(Field::InstructionType, include);
    return *this;
  }

  /**
   * Manage to include the branch type into samples.
   * Note: This field is only available on AMD's IBS Op PMU.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#instruction-execution
   *
   * @param include True, if the branch type should be included.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& branch_type(const bool include) noexcept
  {
    set(Field::BranchType, include);
    return *this;
  }

  /**
   * Manage to include the callchain into samples.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#callchain
   *
   * @param include True, if the callchain should be included.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& callchain(const bool include) noexcept
  {
    set(Field::Callchain, include);
    return *this;
  }

  /**
   * Manage to include the callchain into samples.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#callchain
   *
   * @param max_call_stack The maximum call stack size to include.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& callchain(const std::uint16_t max_call_stack) noexcept
  {
    _max_call_stack = max_call_stack;
    set(Field::Callchain, true);
    return *this;
  }

  /**
   * Manage to include the code page size into samples.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#size-of-the-code-page
   *
   * @param include True, if the code page size should be included.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& code_page_size([[maybe_unused]] const bool include)
  {
#ifndef PERFCPP_NO_SAMPLE_CODE_PAGE_SIZE /// Sampling the code page size is supported since Linux 5.11
    set(Field::CodePageSize, include);
#else
    throw SamplingFeatureIsNotSupported{ "code page size", "5.11" };
#endif
    return *this;
  }

  /**
   * Manage to include instruction latency into samples.
   * Note: Available fields vary by hardware:
   *       - Intel: instruction_retirement
   *       - AMD IBS Op PMU: uop_tag_to_retirement, uop_completion_to_retirement, uop_tag_to_completion
   *       - AMD IBS Fetch PMU: fetch
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#instruction-latency
   *
   * @param include True, if the instruction latency should be included.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& instruction_latency(const bool include) noexcept
  {
    set(Field::InstructionLatency, include);
    return *this;
  }

  /**
   * Manage to include instruction cache information into samples.
   * Note: This field is only available on AMD's IBS Fetch PMU.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#instruction-cache
   *
   * @param include True, if instruction cache information should be included.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& instruction_cache(const bool include) noexcept
  {
    set(Field::InstructionCache, include);
    return *this;
  }

  /**
   * Manage to include instruction TLB information into samples.
   * Note: This field is only available on AMD's IBS Fetch PMU.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#instruction-tlb
   *
   * @param include True, if instruction TLB information should be included.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& instruction_tlb(const bool include) noexcept
  {
    set(Field::InstructionTLB, include);
    return *this;
  }

  /**
   * Manage to include instruction fetch information into samples.
   * Note: This field is only available on AMD's IBS Fetch PMU.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#instruction-fetch
   *
   * @param include True, if instruction fetch information should be included.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& instruction_fetch(const bool include) noexcept
  {
    set(Field::InstructionFetch, include);
    return *this;
  }

  /**
   * Manage to include the logical memory address into samples.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#logical-memory-address
   *
   * @param include True, if the logical memory address should be included.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& logical_memory_address(const bool include) noexcept
  {
    set(Field::LogicalMemoryAddress, include);
    return *this;
  }

  /**
   * Manage to include the physical memory address into samples (only available since Linux 4.13).
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#physical-memory-address
   *
   * @param include True, if the physical memory address should be included.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& physical_memory_address([[maybe_unused]] const bool include)
  {
#ifndef PERFCPP_NO_SAMPLE_PHYS_ADDR /// Sampling for physical memory address is supported since Linux 4.13
    set(Field::PhysicalMemoryAddress, include);
#else
    throw SamplingFeatureIsNotSupported{ "physical memory address", "4.13" };
#endif
    return *this;
  }

  /**
   * Manage to include the data source for memory addresses into samples.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#data-source-of-a-memory-load
   *
   * @param include True, if the data source should be included.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& data_source(const bool include) noexcept
  {
    set(Field::DataSource, include);
    return *this;
  }

  /**
   * Manage to include the data page size of memory accesses into samples.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#size-of-the-data-page
   *
   * @param include True, if the data page size should be included.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& data_page_size([[maybe_unused]] const bool include)
  {
#ifndef PERFCPP_NO_SAMPLE_DATA_PAGE_SIZE /// Sampling the data page size is supported since Linux 5.11
    set(Field::DataPageSize, include);
#else
    throw SamplingFeatureIsNotSupported{ "data page size", "5.11" };
#endif
    return *this;
  }

  /**
   * Manage to include the data TLB page size into samples.
   * Note: This field is only available on AMD's IBS Op PMU.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#data-tlb
   *
   * @param include True, if the data TLB page size should be included.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& data_tlb_page_size(const bool include) noexcept
  {
    set(Field::DataTLBPageSize, include);
    return *this;
  }

  /**
   * Manage to include data access latency information into samples.
   * Note: Available fields vary by hardware:
   *       - Intel: cache_access (with mem-load trigger)
   *       - AMD IBS Op PMU: cache_miss
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#memory-access-latency
   *
   * @param include True, if latency information should be included.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& data_access_latency(const bool include) noexcept
  {
    set(Field::DataAccessLatency, include);
    return *this;
  }

  /**
   * Manage to include latency information into samples.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#memory-access-latency
   *
   * @param include True, if latency information should be included.
   * @return The SampleRecordingValues instance.
   */
  [[deprecated("weight(bool) is deprecated and will be removed on v0.14. Please use data_access_latency(bool) "
               "instead.")]] SampleRecordingValues&
  weight(const bool include) noexcept
  {
    return data_access_latency(include);
  }

  /**
   * Manage to include latency information into samples (only from Linux 5.12).
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#memory-access-latency
   *
   * @param include True, if latency information should be included.
   * @return The SampleRecordingValues instance.
   */
  [[deprecated("weight_struct(bool) is deprecated and will be removed on v0.14. Please use data_access_latency(bool) "
               "instead.")]] SampleRecordingValues&
  weight_struct([[maybe_unused]] const bool include)
  {
#ifndef PERFCPP_NO_SAMPLE_WEIGHT_STRUCT /// Sampling of weight structs (in contrast to simple weight) is supported since
                                        /// Linux 5.12
    set(Field::DataAccessLatency, include);
#else
    throw SamplingFeatureIsNotSupported{ "weight struct", "5.12" };
#endif
    return *this;
  }

  /**
   * Manage to include latency information into samples. This is a wrapper for weight (until Linux 5.12) and
   * weight_struct (since Linux 5.13)
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#memory-access-latency
   *
   * @param include True, if latency information should be included.
   * @return The SampleRecordingValues instance.
   */
  [[deprecated("latency(bool) is deprecated and will be removed on v0.14. Please use data_access_latency(bool) "
               "instead.")]] SampleRecordingValues&
  latency([[maybe_unused]] const bool include) noexcept
  {
    return data_access_latency(include);
  }

  /**
   * Manage to include data TLB latency into samples.
   * Note: This field is only available on AMD's IBS Op PMU.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#data-latency
   *
   * @param include True, if data TLB latency should be included.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& data_tlb_latency(const bool include) noexcept
  {
    set(Field::DataTLBLatency, include);
    return *this;
  }

  /**
   * Manage to include data access width into samples.
   * Note: This field is only available on AMD's IBS Op PMU.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#data-access
   *
   * @param include True, if data access width should be included.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& data_access_width(const bool include) noexcept
  {
    set(Field::DataAccessWidth, include);
    return *this;
  }

  /**
   * Manage to include data access misalign penalty into samples.
   * Note: This field is only available on AMD's IBS Op PMU.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#data-access
   *
   * @param include True, if data access misalign penalty should be included.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& data_access_misalign_penalty(const bool include) noexcept
  {
    set(Field::DataAccessMisalignPenalty, include);
    return *this;
  }

  /**
   * Manage to include MHB allocations into samples.
   * Note: This field is only available on AMD's IBS Op PMU (MAB slots).
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#data-source
   *
   * @param include True, if MHB allocations should be included.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& mhb_allocations(const bool include) noexcept
  {
    set(Field::MHBAllocations, include);
    return *this;
  }

  /**
   * Manage to include branch stacks into samples.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#branch-stack-lbr
   *
   * @param branch_types List of branch types to include.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& branch_stack(std::vector<BranchType>&& branch_types) noexcept
  {
    this->_branch_mask = std::uint64_t{ 0U };
    for (auto branch_type : branch_types) {
      this->_branch_mask |= static_cast<std::uint64_t>(branch_type);
    }

    set(Field::BranchStack, this->_branch_mask != 0ULL);
    return *this;
  }

  /**
   * Manage to include user-level registers into samples.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#registers
   *
   * @param registers List of registers to include.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& user_registers(Registers&& registers)
  {
    _user_registers = std::move(registers);
    set(Field::UserRegisters, !_user_registers.empty());
    return *this;
  }

  /**
   * Manage to include user-level registers into samples.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#registers
   *
   * @param registers List of registers to include.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& user_registers(std::vector<Registers::arm>&& registers)
  {
    return user_registers(Registers{ std::move(registers) });
  }

  /**
   * Manage to include user-level registers into samples.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#registers
   *
   * @param registers List of registers to include.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& user_registers(std::vector<Registers::arm64>&& registers)
  {
    return user_registers(Registers{ std::move(registers) });
  }

  /**
   * Manage to include user-level registers into samples.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#registers
   *
   * @param registers List of registers to include.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& user_registers(std::vector<Registers::x86>&& registers)
  {
    return user_registers(Registers{ std::move(registers) });
  }

  /**
   * Manage to include user-level registers into samples.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#registers
   *
   * @param registers List of registers to include.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& user_registers(std::vector<Registers::riscv>&& registers)
  {
    return user_registers(Registers{ std::move(registers) });
  }

  /**
   * Manage to include kernel-level registers into samples.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#registers
   *
   * @param registers List of registers to include.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& kernel_registers(Registers&& registers)
  {
    _kernel_registers = std::move(registers);
    set(Field::KernelRegisters, !_kernel_registers.empty());
    return *this;
  }

  /**
   * Manage to include kernel-level registers into samples.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#registers
   *
   * @param registers List of registers to include.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& kernel_registers(std::vector<Registers::arm>&& registers)
  {
    return kernel_registers(Registers{ std::move(registers) });
  }

  /**
   * Manage to include kernel-level registers into samples.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#registers
   *
   * @param registers List of registers to include.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& kernel_registers(std::vector<Registers::arm64>&& registers)
  {
    return kernel_registers(Registers{ std::move(registers) });
  }

  /**
   * Manage to include kernel-level registers into samples.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#registers
   *
   * @param registers List of registers to include.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& kernel_registers(std::vector<Registers::x86>&& registers)
  {
    return kernel_registers(Registers{ std::move(registers) });
  }

  /**
   * Manage to include kernel-level registers into samples.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#registers
   *
   * @param registers List of registers to include.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& kernel_registers(std::vector<Registers::riscv>&& registers)
  {
    return kernel_registers(Registers{ std::move(registers) });
  }

  /**
   * Manage to include the user stack (as a list of bytes) into samples.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#user-stack
   *
   * @param max_stack_size The maximum size of the stack.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& user_stack(const std::uint32_t max_stack_size) noexcept
  {
    _max_user_stack = max_stack_size;
    set(Field::UserStack, max_stack_size > 0U);
    return *this;
  }

  /**
   * Manage to include hardware counter values into samples.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#performance-counter-values
   *
   * @param counter_names List of counter names to record.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& counter(std::vector<std::string>&& counter_names) noexcept
  {
    _counter_names = std::move(counter_names);
    set(Field::PerformanceCounter, !_counter_names.empty());
    return *this;
  }

  /**
   * Manage to include hardware transaction abort reasons into samples.
   * Note: This field is only available on Intel.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#transaction-abort
   *
   * @param include True, if hardware transaction aborts should be included.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& hardware_transaction_abort(const bool include) noexcept
  {
    set(Field::HardwareTransactionAbort, include);
    return *this;
  }

  /**
   * Manage to include context switches into samples.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#context-switches
   *
   * @param include True, if context switches should be included.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& context_switch(const bool include) noexcept
  {
    set(Field::ContextSwitch, include);
    return *this;
  }

  /**
   * Manage to include throttling events into samples.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#throttle-and-unthrottle-events
   *
   * @param include True, if throttling events should be included.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& throttle(const bool include) noexcept
  {
    set(Field::Throttle, include);
    return *this;
  }

  /**
   * Manage to include extended mmap information into samples.
   *
   * @param include True, if extended mmap information should be included.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& extended_mmap_information(const bool include) noexcept
  {
    set(Field::MMapInformation, include);
    return *this;
  }

  /**
   * Manage to include raw data into samples.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#raw-values
   *
   * @param include True, if the raw data should be included.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& raw(const bool include) noexcept
  {
    set(Field::RawValues, include);
    return *this;
  }

  /**
   * Manage to include auxiliary data into samples.
   *
   * @param include True, if auxiliary data should be included.
   * @return The SampleRecordingValues instance.
   */
  SampleRecordingValues& aux_values([[maybe_unused]] const bool include)
  {
#ifndef PERFCPP_NO_SAMPLE_AUX /// Sampling aux values is supported since Linux 5.5
    set(Field::AuxValues, include);
#else
    throw SamplingFeatureIsNotSupported{ "aux values", "5.5" };
#endif
    return *this;
  }

  /**
   * Tests, if the given field is set for sampling.
   *
   * @param field Field to sample.
   * @return True, if the flag is included into samples.
   */
  [[nodiscard]] bool is_set(const Field field) const noexcept
  {
    const auto index = static_cast<std::size_t>(field);
    return _activated_fields.at(index);
  }

  /**
   * @return The set of requested user registers to include into the samples.
   */
  [[nodiscard]] const Registers& user_registers() const noexcept { return _user_registers; }

  /**
   * @return The set of requested kernel registers to include into the samples.
   */
  [[nodiscard]] const Registers& kernel_registers() const noexcept { return _kernel_registers; }

  /**
   * @return The maximal size of the user stack to sample–requested by the user.
   */
  [[nodiscard]] std::uint32_t max_user_stack() const noexcept { return _max_user_stack; }

  /**
   * @return List of hardware counter names to sample.
   */
  [[nodiscard]] const std::vector<std::string>& counters() const noexcept { return _counter_names; }

  /**
   * @return Mask of branches to record.
   */
  [[nodiscard]] std::uint64_t branch_mask() const noexcept { return _branch_mask; }

  /**
   * @return The maximal size of the call stack to sample–requested by the user.
   */
  [[nodiscard]] std::uint16_t max_call_stack() const noexcept { return _max_call_stack; }

  /**
   * @return Turns the activated fields into a sample type that can be processed by the perf subsystem.
   */
  [[nodiscard]] std::uint64_t to_perf_sample_type() const noexcept;

  /**
   * @return True, if raw values are needed for IBS decoding. This does not include the need if raw values are activated
   * manually.
   */
  [[nodiscard]] bool is_need_raw_values_for_ibs_decoding() const noexcept
  {
    return is_set(Field::PhysicalInstructionPointer) || is_set(Field::InstructionType) || is_set(Field::BranchType) ||
           is_set(Field::InstructionLatency) || is_set(Field::InstructionCache) || is_set(Field::InstructionTLB) ||
           is_set(Field::InstructionFetch) || is_set(Field::DataAccessWidth) ||
           is_set(Field::DataAccessMisalignPenalty) || is_set(Field::MHBAllocations);
  }

private:
  /// Bitmap for all fields.
  std::array<bool, static_cast<std::size_t>(Field::CountFields)> _activated_fields{};

  /// List of hardware counters and metrics to include into the sample.
  std::vector<std::string> _counter_names;

  /// List of user registers to include into the sample.
  Registers _user_registers;

  /// List of kernel registers to include into the sample.
  Registers _kernel_registers;

  /// Size of the user stack to include into the sample.
  std::uint32_t _max_user_stack{ 0U };

  /// Mask of branch flags to include into the sample.
  std::uint64_t _branch_mask{ 0ULL };

  /// Size of the call stack to include into the sample.
  std::uint16_t _max_call_stack{ 0U };

  /**
   * En- or disables a specific field for sampling.
   *
   * @param field Field to en- or disable.
   * @param is_enabled Flag, if the field should be included or excluded.
   */
  void set(const Field field, const bool is_enabled) noexcept
  {
    const auto index = static_cast<std::size_t>(field);
    _activated_fields.at(index) = is_enabled;
  }

  [[nodiscard]] std::uint64_t perf_sample_type_if_field_activates(const std::uint64_t perf_sample_type,
                                                                  const Field field) const noexcept
  {
    if (is_set(field)) {
      return perf_sample_type;
    }

    return 0UL;
  }
};
}