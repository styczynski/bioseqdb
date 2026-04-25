#include <perfcpp/sample/recording_values.hpp>

std::uint64_t
perf::SampleRecordingValues::to_perf_sample_type() const noexcept
{
  auto sample_type = std::uint64_t{ 0U };

  // === Process/Thread Context ===
  sample_type |= this->perf_sample_type_if_field_activates(PERF_SAMPLE_TID, Field::ThreadId);
  sample_type |= this->perf_sample_type_if_field_activates(PERF_SAMPLE_CPU, Field::CpuId);
#ifndef PERFCPP_NO_SAMPLE_CGROUP /// Sampling cgroup is supported since Linux 5.7
  sample_type |= this->perf_sample_type_if_field_activates(PERF_SAMPLE_CGROUP, Field::CGroup);
#endif

  // === Timing ===
  sample_type |= this->perf_sample_type_if_field_activates(PERF_SAMPLE_TIME, Field::Timestamp);
  sample_type |= this->perf_sample_type_if_field_activates(PERF_SAMPLE_PERIOD, Field::Period);

  // === Identifiers ===
  sample_type |= this->perf_sample_type_if_field_activates(PERF_SAMPLE_IDENTIFIER, Field::Id);
  sample_type |= this->perf_sample_type_if_field_activates(PERF_SAMPLE_STREAM_ID, Field::StreamId);

  // === Instruction Execution ===
  sample_type |= this->perf_sample_type_if_field_activates(PERF_SAMPLE_IP, Field::LogicalInstructionPointer);
  sample_type |=
    this->perf_sample_type_if_field_activates(PERF_SAMPLE_IP | PERF_SAMPLE_RAW, Field::PhysicalInstructionPointer);
  sample_type |=
    this->perf_sample_type_if_field_activates(PERF_SAMPLE_DATA_SRC | PERF_SAMPLE_RAW, Field::InstructionType);
  sample_type |= this->perf_sample_type_if_field_activates(PERF_SAMPLE_RAW, Field::BranchType);
  sample_type |= this->perf_sample_type_if_field_activates(PERF_SAMPLE_CALLCHAIN, Field::Callchain);
#ifndef PERFCPP_NO_SAMPLE_CODE_PAGE_SIZE /// Sampling the code page size is supported since Linux 5.11
  sample_type |= this->perf_sample_type_if_field_activates(PERF_SAMPLE_CODE_PAGE_SIZE, Field::CodePageSize);
#endif

  // === Instruction Performance ===
#ifndef PERFCPP_NO_SAMPLE_WEIGHT_STRUCT /// Sampling of weight structs (in contrast to simple weight) is supported since
                                        /// Linux 5.12
  sample_type |=
    this->perf_sample_type_if_field_activates(PERF_SAMPLE_WEIGHT_STRUCT | PERF_SAMPLE_RAW, Field::InstructionLatency);
#else
  sample_type |=
    this->perf_sample_type_if_field_activates(PERF_SAMPLE_WEIGHT | PERF_SAMPLE_RAW, Field::InstructionLatency);
#endif
  sample_type |= this->perf_sample_type_if_field_activates(PERF_SAMPLE_IP | PERF_SAMPLE_RAW, Field::InstructionCache);
  sample_type |= this->perf_sample_type_if_field_activates(PERF_SAMPLE_IP | PERF_SAMPLE_RAW, Field::InstructionTLB);
  sample_type |= this->perf_sample_type_if_field_activates(PERF_SAMPLE_IP | PERF_SAMPLE_RAW, Field::InstructionFetch);

  // === Data Access ===
  sample_type |= this->perf_sample_type_if_field_activates(PERF_SAMPLE_ADDR, Field::LogicalMemoryAddress);
#ifndef PERFCPP_NO_SAMPLE_PHYS_ADDR /// Sampling for physical memory address is supported since Linux 4.13
  sample_type |= this->perf_sample_type_if_field_activates(PERF_SAMPLE_PHYS_ADDR, Field::PhysicalMemoryAddress);
#endif
  sample_type |= this->perf_sample_type_if_field_activates(PERF_SAMPLE_DATA_SRC, Field::DataSource);
#ifndef PERFCPP_NO_SAMPLE_DATA_PAGE_SIZE /// Sampling the data page size is supported since Linux 5.11
  sample_type |= this->perf_sample_type_if_field_activates(PERF_SAMPLE_DATA_PAGE_SIZE, Field::DataPageSize);
#endif
#ifndef PERFCPP_NO_SAMPLE_WEIGHT_STRUCT /// Sampling of weight structs (in contrast to simple weight) is supported since
                                        /// Linux 5.12
  sample_type |= this->perf_sample_type_if_field_activates(PERF_SAMPLE_WEIGHT_STRUCT, Field::DataAccessLatency);
  sample_type |=
    this->perf_sample_type_if_field_activates(PERF_SAMPLE_WEIGHT_STRUCT | PERF_SAMPLE_RAW, Field::DataTLBLatency);
#else
  sample_type |= this->perf_sample_type_if_field_activates(PERF_SAMPLE_WEIGHT, Field::DataAccessLatency);
  sample_type |= this->perf_sample_type_if_field_activates(PERF_SAMPLE_WEIGHT | PERF_SAMPLE_RAW, Field::DataTLBLatency);
#endif
  sample_type |= this->perf_sample_type_if_field_activates(PERF_SAMPLE_RAW, Field::DataTLBPageSize);
  sample_type |= this->perf_sample_type_if_field_activates(PERF_SAMPLE_RAW, Field::DataAccessWidth);
  sample_type |= this->perf_sample_type_if_field_activates(PERF_SAMPLE_RAW, Field::DataAccessMisalignPenalty);
  sample_type |= this->perf_sample_type_if_field_activates(PERF_SAMPLE_RAW, Field::MHBAllocations);

  // === Branch Sampling ===
  sample_type |= this->perf_sample_type_if_field_activates(PERF_SAMPLE_BRANCH_STACK, Field::BranchStack);

  // === Registers & Stack ===
  sample_type |= this->perf_sample_type_if_field_activates(PERF_SAMPLE_REGS_USER, Field::UserRegisters);
  sample_type |= this->perf_sample_type_if_field_activates(PERF_SAMPLE_REGS_INTR, Field::KernelRegisters);
  sample_type |= this->perf_sample_type_if_field_activates(PERF_SAMPLE_STACK_USER, Field::UserStack);

  // === Performance Counters ===
  sample_type |= this->perf_sample_type_if_field_activates(PERF_SAMPLE_READ, Field::PerformanceCounter);

  // === Hardware Transaction Memory ===
  sample_type |= this->perf_sample_type_if_field_activates(PERF_SAMPLE_TRANSACTION, Field::HardwareTransactionAbort);

  // === Raw & Auxiliary ===
  sample_type |= this->perf_sample_type_if_field_activates(PERF_SAMPLE_RAW, Field::RawValues);
#ifndef PERFCPP_NO_SAMPLE_AUX /// Sampling of aux values supported since Linux 5.5
  sample_type |= this->perf_sample_type_if_field_activates(PERF_SAMPLE_AUX, Field::AuxValues);
#endif

  return sample_type;
}