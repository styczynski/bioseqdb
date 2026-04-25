#include "perfcpp/hardware_info.hpp"
#include <algorithm>
#include <fstream>
#include <perfcpp/analyzer/flame_graph_generator.hpp>
#include <perfcpp/sample/result.hpp>
#include <perfcpp/util/callchain_trie.hpp>
#include <sstream>

void
perf::SampleResult::filter(std::function<bool(const Sample&)> filter)
{
  this->_samples.erase(std::remove_if(this->_samples.begin(),
                                      this->_samples.end(),
                                      [&filter](const auto& sample) { return !filter(sample); }),
                       this->_samples.end());
}

std::string
perf::SampleResult::to_csv(const char delimiter, const char list_delimiter) const
{
  auto string_stream = std::ostringstream{};
  this->write_csv(string_stream, delimiter, list_delimiter);
  return string_stream.str();
}

void
perf::SampleResult::to_csv(const std::string_view file_name, const char delimiter, const char list_delimiter) const
{
  auto stream = std::ofstream(std::string{ file_name });
  this->write_csv(stream, delimiter, list_delimiter);
  stream << std::flush;
}

void
perf::SampleResult::write_csv(std::ostream& stream, const char delimiter, const char list_delimiter) const
{
  auto csv_writer = CSVWriter{ stream, this->_sample_recording_values, delimiter, list_delimiter };

  /// Header: Metadata
  stream << "mode";
  csv_writer.write_header(SampleRecordingValues::Field::Id, "id");
  csv_writer.write_header(SampleRecordingValues::Field::StreamId, "stream_id");
  csv_writer.write_header(SampleRecordingValues::Field::Timestamp, "timestamp");
  csv_writer.write_header(SampleRecordingValues::Field::Period, "period");
  csv_writer.write_header(SampleRecordingValues::Field::CpuId, "cpu_id");
  csv_writer.write_header(SampleRecordingValues::Field::ThreadId, "process_id");
  csv_writer.write_header(SampleRecordingValues::Field::ThreadId, "thread_id");

  /// Header: Instruction Execution
  csv_writer.write_header(SampleRecordingValues::Field::InstructionType, "instruction_type");
  csv_writer.write_header(SampleRecordingValues::Field::LogicalInstructionPointer, "logical_instruction_pointer");
  csv_writer.write_header(SampleRecordingValues::Field::LogicalInstructionPointer, "is_instruction_pointer_exact");
  csv_writer.write_header(SampleRecordingValues::Field::PhysicalInstructionPointer, "physical_instruction_pointer");
  csv_writer.write_header(SampleRecordingValues::Field::InstructionCache, "instruction_l1i_miss");
  csv_writer.write_header(SampleRecordingValues::Field::InstructionCache, "instruction_l2_miss");
  csv_writer.write_header(SampleRecordingValues::Field::InstructionCache, "instruction_l2_miss");
  csv_writer.write_header(SampleRecordingValues::Field::InstructionTLB, "instruction_itlb_miss");
  csv_writer.write_header(SampleRecordingValues::Field::InstructionTLB, "instruction_itlb_size");
  csv_writer.write_header(SampleRecordingValues::Field::InstructionTLB, "instruction_stlb_miss");

  if (HardwareInfo::is_intel()) {
    csv_writer.write_header(SampleRecordingValues::Field::InstructionLatency, "instruction_retirement_latency");
  } else if (HardwareInfo::is_amd()) {
    csv_writer.write_header(SampleRecordingValues::Field::InstructionLatency, "uop_tag_to_retirement_latency");
    csv_writer.write_header(SampleRecordingValues::Field::InstructionLatency, "uop_tag_to_completion");
    csv_writer.write_header(SampleRecordingValues::Field::InstructionLatency, "uop_completion_to_retirement");
    csv_writer.write_header(SampleRecordingValues::Field::InstructionLatency, "instruction_fetch_latency");
  }

  csv_writer.write_header(SampleRecordingValues::Field::BranchType, "branch_type");
  csv_writer.write_header(SampleRecordingValues::Field::HardwareTransactionAbort, "tx_is_elision");
  csv_writer.write_header(SampleRecordingValues::Field::HardwareTransactionAbort, "tx_is_generic");
  csv_writer.write_header(SampleRecordingValues::Field::HardwareTransactionAbort, "tx_is_synchronous_abort");
  csv_writer.write_header(SampleRecordingValues::Field::HardwareTransactionAbort, "tx_is_retryable");
  csv_writer.write_header(SampleRecordingValues::Field::HardwareTransactionAbort, "tx_is_memory_conflict");
  csv_writer.write_header(SampleRecordingValues::Field::HardwareTransactionAbort, "tx_is_write_capacity_conflict");
  csv_writer.write_header(SampleRecordingValues::Field::HardwareTransactionAbort, "tx_is_read_capacity_conflict");
  csv_writer.write_header(SampleRecordingValues::Field::HardwareTransactionAbort, "tx_user_code");
  csv_writer.write_header(SampleRecordingValues::Field::CodePageSize, "code_page_size");
  csv_writer.write_header(SampleRecordingValues::Field::Callchain, "callchain");

  /// Header: Data Access
  csv_writer.write_header(SampleRecordingValues::Field::DataSource, "data_access_type");
  csv_writer.write_header(SampleRecordingValues::Field::DataSource, "is_locked");
  csv_writer.write_header(SampleRecordingValues::Field::LogicalMemoryAddress, "logical_memory_address");
  csv_writer.write_header(SampleRecordingValues::Field::PhysicalMemoryAddress, "physical_memory_address");
  csv_writer.write_header(SampleRecordingValues::Field::DataSource, "data_access_is_l1d_hit");
  csv_writer.write_header(SampleRecordingValues::Field::DataSource, "data_access_is_mhb_hit");
  csv_writer.write_header(SampleRecordingValues::Field::MHBAllocations, "mhb_slots_allocated");
  csv_writer.write_header(SampleRecordingValues::Field::DataSource, "data_access_is_l2_hit");
  csv_writer.write_header(SampleRecordingValues::Field::DataSource, "data_access_is_l3_hit");
  csv_writer.write_header(SampleRecordingValues::Field::DataSource, "data_access_is_memory_hit");
  csv_writer.write_header(SampleRecordingValues::Field::DataSource, "data_access_is_remote");
  csv_writer.write_header(SampleRecordingValues::Field::DataSource, "data_access_is_same_node_remote_core");
  csv_writer.write_header(SampleRecordingValues::Field::DataSource, "data_access_is_same_socket_remote_node");
  csv_writer.write_header(SampleRecordingValues::Field::DataSource, "data_access_is_same_board_remote_socket");
  csv_writer.write_header(SampleRecordingValues::Field::DataSource, "data_access_is_remote_board");
  csv_writer.write_header(SampleRecordingValues::Field::DataSource, "dtlb_hit");
  csv_writer.write_header(SampleRecordingValues::Field::DataSource, "stlb_hit");
  csv_writer.write_header(SampleRecordingValues::Field::DataTLBPageSize, "dtlb_page_size");
  csv_writer.write_header(SampleRecordingValues::Field::DataTLBPageSize, "stlb_page_size");
  if (HardwareInfo::is_intel()) {
    csv_writer.write_header(SampleRecordingValues::Field::DataAccessLatency, "cache_access_latency");
  } else if (HardwareInfo::is_amd()) {
    csv_writer.write_header(SampleRecordingValues::Field::DataAccessLatency, "cache_miss_latency");
  }
  csv_writer.write_header(SampleRecordingValues::Field::DataTLBLatency, "dtlb_refill_latency");
  csv_writer.write_header(SampleRecordingValues::Field::DataSource, "snoop_is_hit");
  csv_writer.write_header(SampleRecordingValues::Field::DataSource, "snoop_is_hit_modified");
  csv_writer.write_header(SampleRecordingValues::Field::DataSource, "snoop_is_forward");
  csv_writer.write_header(SampleRecordingValues::Field::DataSource, "snoop_is_transfer_from_peer");
  csv_writer.write_header(SampleRecordingValues::Field::DataAccessMisalignPenalty, "is_misalign_penalty");
  csv_writer.write_header(SampleRecordingValues::Field::DataAccessWidth, "data_access_width");
  csv_writer.write_header(SampleRecordingValues::Field::DataPageSize, "data_page_size");

  /// Header: Performance Counter
  for (const auto& counter : this->_sample_recording_values.counters()) {
    csv_writer.write_header(SampleRecordingValues::Field::PerformanceCounter, "counter_" + counter);
  }

  /// Header: User registers
  std::visit(
    [&csv_writer](const auto& registers) {
      for (const auto reg : registers) {
        csv_writer.write_header(SampleRecordingValues::Field::UserRegisters, "user_register_" + to_string(reg));
      }
    },
    this->_sample_recording_values.user_registers().registers());

  /// Header: Kernel registers
  std::visit(
    [&csv_writer](const auto& registers) {
      for (const auto reg : registers) {
        csv_writer.write_header(SampleRecordingValues::Field::KernelRegisters, "kernel_register_" + to_string(reg));
      }
    },
    this->_sample_recording_values.kernel_registers().registers());

  /// Header: CGroup
  csv_writer.write_header(SampleRecordingValues::Field::CGroup, "cgroup_id");
  csv_writer.write_header(SampleRecordingValues::Field::CGroup, "cgroup_cgroup_id");
  csv_writer.write_header(SampleRecordingValues::Field::CGroup, "cgroup_path");

  /// Header: Context Switch
  csv_writer.write_header(SampleRecordingValues::Field::ContextSwitch, "context_switch_is_out");
  csv_writer.write_header(SampleRecordingValues::Field::ContextSwitch, "context_switch_is_in");
  csv_writer.write_header(SampleRecordingValues::Field::ContextSwitch, "context_switch_is_preempt");
  csv_writer.write_header(SampleRecordingValues::Field::ContextSwitch, "context_switch_process_id");
  csv_writer.write_header(SampleRecordingValues::Field::ContextSwitch, "context_switch_thread_id");

  /// Header: Throttle
  csv_writer.write_header(SampleRecordingValues::Field::Throttle, "is_throttle");
  csv_writer.write_header(SampleRecordingValues::Field::Throttle, "is_unthrottle");

  /// Header: Loss event (if any sample contains a loss count).
  const auto has_loss_event = std::any_of(
    this->_samples.begin(), this->_samples.end(), [](const auto& sample) { return sample.count_loss().has_value(); });
  if (has_loss_event) {
    stream << ",loss_count";
  }

  /// Samples
  for (const auto& sample : this->_samples) {
    stream << '\n';

    /// Metadata
    stream << to_string(sample.metadata().mode());
    csv_writer.write_value(SampleRecordingValues::Field::Id, sample.metadata().sample_id());
    csv_writer.write_value(SampleRecordingValues::Field::StreamId, sample.metadata().stream_id());
    csv_writer.write_value(SampleRecordingValues::Field::Timestamp, sample.metadata().timestamp());
    csv_writer.write_value(SampleRecordingValues::Field::Period, sample.metadata().period());
    csv_writer.write_value(SampleRecordingValues::Field::CpuId, sample.metadata().cpu_id());
    csv_writer.write_value(SampleRecordingValues::Field::ThreadId, sample.metadata().process_id());
    csv_writer.write_value(SampleRecordingValues::Field::ThreadId, sample.metadata().thread_id());

    /// Instruction execution
    csv_writer.write_value(SampleRecordingValues::Field::InstructionType, sample.instruction_execution().type());
    csv_writer.write_value(SampleRecordingValues::Field::LogicalInstructionPointer,
                           sample.instruction_execution().logical_instruction_pointer(),
                           true);
    csv_writer.write_value(SampleRecordingValues::Field::LogicalInstructionPointer,
                           sample.instruction_execution().is_instruction_pointer_exact());
    csv_writer.write_value(SampleRecordingValues::Field::PhysicalInstructionPointer,
                           sample.instruction_execution().physical_instruction_pointer(),
                           true);

    const auto& instruction_cache = sample.instruction_execution().cache();
    csv_writer.write_value(
      SampleRecordingValues::Field::InstructionCache, instruction_cache, [](const auto& c) { return c.is_l1_miss(); });
    csv_writer.write_value(
      SampleRecordingValues::Field::InstructionCache, instruction_cache, [](const auto& c) { return c.is_l2_miss(); });
    csv_writer.write_value(
      SampleRecordingValues::Field::InstructionCache, instruction_cache, [](const auto& c) { return c.is_l3_miss(); });

    const auto& instruction_tlb = sample.instruction_execution().tlb();
    csv_writer.write_value(
      SampleRecordingValues::Field::InstructionTLB, instruction_tlb, [](const auto& t) { return t.is_l1_miss(); });
    csv_writer.write_value(
      SampleRecordingValues::Field::InstructionTLB, instruction_tlb, [](const auto& t) { return t.l1_page_size(); });
    csv_writer.write_value(
      SampleRecordingValues::Field::InstructionTLB, instruction_tlb, [](const auto& t) { return t.is_l2_miss(); });

    const auto& instruction_latency = sample.instruction_execution().latency();
    if (HardwareInfo::is_intel()) {
      csv_writer.write_value(SampleRecordingValues::Field::InstructionLatency,
                             instruction_latency.instruction_retirement());
    } else if (HardwareInfo::is_amd()) {
      csv_writer.write_value(SampleRecordingValues::Field::InstructionLatency,
                             instruction_latency.uop_tag_to_retirement());
      csv_writer.write_value(SampleRecordingValues::Field::InstructionLatency,
                             instruction_latency.uop_tag_to_completion());
      csv_writer.write_value(SampleRecordingValues::Field::InstructionLatency,
                             instruction_latency.uop_completion_to_retirement());
      csv_writer.write_value(SampleRecordingValues::Field::InstructionLatency, instruction_latency.fetch());
    }

    csv_writer.write_value(SampleRecordingValues::Field::BranchType, sample.instruction_execution().branch_type());

    const auto& hardware_transaction_abort = sample.instruction_execution().hardware_transaction_abort();
    csv_writer.write_value(SampleRecordingValues::Field::HardwareTransactionAbort,
                           hardware_transaction_abort,
                           [](const auto& t) { return t.is_elision_transaction(); });
    csv_writer.write_value(SampleRecordingValues::Field::HardwareTransactionAbort,
                           hardware_transaction_abort,
                           [](const auto& t) { return t.is_generic_transaction(); });
    csv_writer.write_value(SampleRecordingValues::Field::HardwareTransactionAbort,
                           hardware_transaction_abort,
                           [](const auto& t) { return t.is_synchronous_abort(); });
    csv_writer.write_value(SampleRecordingValues::Field::HardwareTransactionAbort,
                           hardware_transaction_abort,
                           [](const auto& t) { return t.is_retryable(); });
    csv_writer.write_value(SampleRecordingValues::Field::HardwareTransactionAbort,
                           hardware_transaction_abort,
                           [](const auto& t) { return t.is_due_to_memory_conflict(); });
    csv_writer.write_value(SampleRecordingValues::Field::HardwareTransactionAbort,
                           hardware_transaction_abort,
                           [](const auto& t) { return t.is_due_to_write_capacity_conflict(); });
    csv_writer.write_value(SampleRecordingValues::Field::HardwareTransactionAbort,
                           hardware_transaction_abort,
                           [](const auto& t) { return t.is_due_to_read_capacity_conflict(); });
    csv_writer.write_value(SampleRecordingValues::Field::HardwareTransactionAbort,
                           hardware_transaction_abort,
                           [](const auto& t) { return t.user_specified_code(); });

    csv_writer.write_value(SampleRecordingValues::Field::Callchain, sample.instruction_execution().callchain(), true);
    csv_writer.write_value(SampleRecordingValues::Field::CodePageSize, sample.instruction_execution().page_size());

    /// Data Access
    csv_writer.write_value(SampleRecordingValues::Field::DataSource, sample.data_access().type());
    csv_writer.write_value(SampleRecordingValues::Field::DataSource, sample.data_access().is_locked());
    csv_writer.write_value(
      SampleRecordingValues::Field::LogicalMemoryAddress, sample.data_access().logical_memory_address(), true);
    csv_writer.write_value(
      SampleRecordingValues::Field::PhysicalMemoryAddress, sample.data_access().physical_memory_address(), true);

    csv_writer.write_value(SampleRecordingValues::Field::DataSource, sample.data_access().source(), [](const auto& s) {
      return s.is_l1_hit();
    });
    csv_writer.write_value(SampleRecordingValues::Field::DataSource, sample.data_access().source(), [](const auto& s) {
      return s.is_mhb_hit();
    });
    csv_writer.write_value(SampleRecordingValues::Field::MHBAllocations,
                           sample.data_access().source(),
                           [](const auto& s) { return s.num_mhb_slots_allocated(); });
    csv_writer.write_value(SampleRecordingValues::Field::DataSource, sample.data_access().source(), [](const auto& s) {
      return s.is_l2_hit();
    });
    csv_writer.write_value(SampleRecordingValues::Field::DataSource, sample.data_access().source(), [](const auto& s) {
      return s.is_l3_hit();
    });
    csv_writer.write_value(SampleRecordingValues::Field::DataSource, sample.data_access().source(), [](const auto& s) {
      return s.is_memory_hit();
    });
    csv_writer.write_value(SampleRecordingValues::Field::DataSource, sample.data_access().source(), [](const auto& s) {
      return s.is_remote();
    });
    csv_writer.write_value(SampleRecordingValues::Field::DataSource, sample.data_access().source(), [](const auto& s) {
      return s.is_same_node_remote_core();
    });
    csv_writer.write_value(SampleRecordingValues::Field::DataSource, sample.data_access().source(), [](const auto& s) {
      return s.is_same_socket_remote_node();
    });
    csv_writer.write_value(SampleRecordingValues::Field::DataSource, sample.data_access().source(), [](const auto& s) {
      return s.is_same_board_remote_socket();
    });
    csv_writer.write_value(SampleRecordingValues::Field::DataSource, sample.data_access().source(), [](const auto& s) {
      return s.is_remote_board();
    });

    const auto& data_tlb = sample.data_access().tlb();
    csv_writer.write_value(SampleRecordingValues::Field::DataSource, data_tlb.is_l1_hit());
    csv_writer.write_value(SampleRecordingValues::Field::DataSource, data_tlb.is_l2_hit());
    csv_writer.write_value(SampleRecordingValues::Field::DataTLBPageSize, data_tlb.l1_page_size());
    csv_writer.write_value(SampleRecordingValues::Field::DataTLBPageSize, data_tlb.l2_page_size());

    if (HardwareInfo::is_intel()) {
      csv_writer.write_value(SampleRecordingValues::Field::DataAccessLatency,
                             sample.data_access().latency().cache_access());
    } else if (HardwareInfo::is_amd()) {
      csv_writer.write_value(SampleRecordingValues::Field::DataAccessLatency,
                             sample.data_access().latency().cache_miss());
    }
    csv_writer.write_value(SampleRecordingValues::Field::DataTLBLatency, sample.data_access().latency().dtlb_refill());

    const auto& snoop = sample.data_access().snoop();
    csv_writer.write_value(SampleRecordingValues::Field::DataSource, snoop, [](const auto& s) { return s.is_hit(); });
    csv_writer.write_value(
      SampleRecordingValues::Field::DataSource, snoop, [](const auto& s) { return s.is_hit_modified(); });
    csv_writer.write_value(
      SampleRecordingValues::Field::DataSource, snoop, [](const auto& s) { return s.is_forward(); });
    csv_writer.write_value(
      SampleRecordingValues::Field::DataSource, snoop, [](const auto& s) { return s.is_transfer_from_peer(); });

    csv_writer.write_value(SampleRecordingValues::Field::DataAccessMisalignPenalty,
                           sample.data_access().is_misalign_penalty());
    csv_writer.write_value(SampleRecordingValues::Field::DataAccessWidth, sample.data_access().access_width());
    csv_writer.write_value(SampleRecordingValues::Field::DataPageSize, sample.data_access().page_size());

    /// Performance counter
    for (const auto& counter : this->_sample_recording_values.counters()) {
      csv_writer.write_value(SampleRecordingValues::Field::PerformanceCounter,
                             sample.counter(),
                             [&counter](const auto& c) { return c.get(counter); });
    }

    /// User registers
    std::visit(
      [&csv_writer, &sample](const auto& registers) {
        for (const auto reg : registers) {
          csv_writer.write_value(SampleRecordingValues::Field::UserRegisters,
                                 sample.user_registers(),
                                 [reg](const auto& u) { return u.get(reg); });
        }
      },
      this->_sample_recording_values.user_registers().registers());

    /// Kernel registers
    std::visit(
      [&csv_writer, &sample](const auto& registers) {
        for (const auto reg : registers) {
          csv_writer.write_value(SampleRecordingValues::Field::KernelRegisters,
                                 sample.kernel_registers(),
                                 [reg](const auto& u) { return u.get(reg); });
        }
      },
      this->_sample_recording_values.kernel_registers().registers());

    /// CGroup
    csv_writer.write_value(SampleRecordingValues::Field::CGroup, sample.cgroup_id());
    csv_writer.write_value(SampleRecordingValues::Field::CGroup, sample.cgroup(), [](const auto& c) { return c.id(); });
    csv_writer.write_value(
      SampleRecordingValues::Field::CGroup, sample.cgroup(), [](const auto& c) { return c.path(); });

    /// Context Switch
    const auto& context_switch = sample.context_switch();
    csv_writer.write_value(
      SampleRecordingValues::Field::ContextSwitch, context_switch, [](const auto& cs) { return cs.is_out(); });
    csv_writer.write_value(
      SampleRecordingValues::Field::ContextSwitch, context_switch, [](const auto& cs) { return cs.is_in(); });
    csv_writer.write_value(
      SampleRecordingValues::Field::ContextSwitch, context_switch, [](const auto& cs) { return cs.is_preempt(); });
    csv_writer.write_value(
      SampleRecordingValues::Field::ContextSwitch, context_switch, [](const auto& cs) { return cs.process_id(); });
    csv_writer.write_value(
      SampleRecordingValues::Field::ContextSwitch, context_switch, [](const auto& cs) { return cs.thread_id(); });

    /// Throttle
    const auto& throttle = sample.throttle();
    csv_writer.write_value(
      SampleRecordingValues::Field::Throttle, throttle, [](const auto& t) { return t.is_throttle(); });
    csv_writer.write_value(
      SampleRecordingValues::Field::Throttle, throttle, [](const auto& t) { return t.is_unthrottle(); });

    /// Loss event (if any sample contains a loss count).
    if (has_loss_event) {
      stream << "," << sample.count_loss().value_or(0U);
    }
  }
}

void
perf::SampleResult::to_flamegraphs(const std::string_view file_name) const
{
  auto flame_graph_generator = analyzer::FlameGraphGenerator{};
  flame_graph_generator.map(this->_samples, file_name);
}