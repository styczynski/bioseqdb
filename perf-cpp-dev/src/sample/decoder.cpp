#include <perfcpp/hardware_info.hpp>
#include <perfcpp/sample/decoder.hpp>
#include <perfcpp/sample/ibs_decoder.hpp>

std::optional<perf::Metadata::Mode>
perf::SampleIterator::mode() const noexcept
{
  const auto misc = this->_header->misc;

  if (static_cast<bool>(misc & PERF_RECORD_MISC_KERNEL)) {
    return Metadata::Mode::Kernel;
  }

  if (static_cast<bool>(misc & PERF_RECORD_MISC_USER)) {
    return Metadata::Mode::User;
  }

  if (static_cast<bool>(misc & PERF_RECORD_MISC_HYPERVISOR)) {
    return Metadata::Mode::Hypervisor;
  }

  if (static_cast<bool>(misc & PERF_RECORD_MISC_GUEST_KERNEL)) {
    return Metadata::Mode::GuestKernel;
  }

  if (static_cast<bool>(misc & PERF_RECORD_MISC_GUEST_USER)) {
    return Metadata::Mode::GuestUser;
  }

  return std::nullopt;
}

std::vector<perf::Sample>
perf::SampleDecoder::decode(const std::vector<std::vector<std::byte>>& sample_buffers,
                            const bool has_amd_ibs_op_pmu,
                            const bool has_amd_ibs_fetch_pmu,
                            const RequestedEventSet& requested_event_set,
                            const Group& event_group) const
{
  auto samples = std::vector<Sample>{};
  samples.reserve(sample_buffers.size() * 2048UL);

  /// Read samples from all the buffers.
  for (const auto& buffer : sample_buffers) {
    auto iterator = reinterpret_cast<std::uintptr_t>(buffer.data());
    const auto end = iterator + buffer.size();

    /// Scan over all samples stored in the user-level buffer.
    while (iterator < end) {
      auto entry = SampleIterator{ iterator };
      const auto size = entry.size();

      /// Check if the size is valid.
      if (size < sizeof(perf_event_header) || iterator + size > end) {
        break;
      }

      if (entry.is_sample_event()) { /// Read sample event.
        samples.push_back(this->decode_sample_event(
          std::move(entry), has_amd_ibs_op_pmu, has_amd_ibs_fetch_pmu, requested_event_set, event_group));
      } else if (entry.is_lost_samples_event()) { /// Read lost event.
        samples.push_back(this->decode_lost_samples_event(std::move(entry), false));
      } else if (entry.is_lost_event()) { /// Read lost event.
        samples.push_back(this->decode_lost_samples_event(std::move(entry), true));
      } else if (entry.is_context_switch_event()) { /// Read context switch event.
        samples.push_back(this->decode_context_switch_event(std::move(entry)));
      } else if (entry.is_cgroup_event()) { /// Read cgroup event.
        samples.push_back(this->decode_cgroup_event(std::move(entry)));
      } else if (entry.is_throttle_event() &&
                 this->_sampler_values.is_set(SampleRecordingValues::Field::Throttle)) { /// Read (un-) throttle event.
        samples.push_back(this->decode_throttle_event(std::move(entry)));
      }

      /// Go to the next sample.
      iterator += size;
    }
  }

  return samples;
}

void
perf::SampleDecoder::decode_sample_id_all(SampleIterator& entry, Sample& sample) const noexcept
{
  if (this->_sampler_values.is_set(SampleRecordingValues::Field::ThreadId)) {
    sample.metadata().process_id(entry.read<std::uint32_t>());
    sample.metadata().thread_id(entry.read<std::uint32_t>());
  }

  if (this->_sampler_values.is_set(SampleRecordingValues::Field::Timestamp)) {
    sample.metadata().timestamp(entry.read<std::uint64_t>());
  }

  if (this->_sampler_values.is_set(SampleRecordingValues::Field::StreamId)) {
    sample.metadata().stream_id(entry.read<std::uint64_t>());
  }

  if (this->_sampler_values.is_set(SampleRecordingValues::Field::CpuId)) {
    sample.metadata().cpu_id(entry.read<std::uint32_t>());
    entry.skip<std::uint32_t>(); /// Skip "res" field.
  }

  if (this->_sampler_values.is_set(SampleRecordingValues::Field::Id)) {
    sample.metadata().sample_id(entry.read<std::uint64_t>());
  }
}

perf::Sample
perf::SampleDecoder::decode_sample_event(SampleIterator&& entry,
                                         const bool has_amd_ibs_op_pmu,
                                         const bool has_amd_ibs_fetch_pmu,
                                         const RequestedEventSet& requested_event_set,
                                         const Group& event_group) const

{
  auto sample = Sample{};
  sample.metadata().mode(entry.mode());

  if (this->_sampler_values.is_set(SampleRecordingValues::Field::Id)) {
    sample.metadata().sample_id(entry.read<std::uint64_t>());
  }

  if (this->_sampler_values.is_set(SampleRecordingValues::Field::LogicalInstructionPointer)) {
    sample.instruction_execution().instruction_pointer_exact(entry.is_instruction_pointer_exact());
    sample.instruction_execution().logical_instruction_pointer(entry.read<std::uintptr_t>());
  }

  if (this->_sampler_values.is_set(SampleRecordingValues::Field::ThreadId)) {
    sample.metadata().process_id(entry.read<std::uint32_t>());
    sample.metadata().thread_id(entry.read<std::uint32_t>());
  }

  if (this->_sampler_values.is_set(SampleRecordingValues::Field::Timestamp)) {
    sample.metadata().timestamp(entry.read<std::uint64_t>());
  }

  if (this->_sampler_values.is_set(SampleRecordingValues::Field::StreamId)) {
    sample.metadata().stream_id(entry.read<std::uint64_t>());
  }

  if (this->_sampler_values.is_set(SampleRecordingValues::Field::LogicalMemoryAddress)) {
    sample.data_access().logical_memory_address(entry.read<std::uint64_t>());
  }

  if (this->_sampler_values.is_set(SampleRecordingValues::Field::CpuId)) {
    sample.metadata().cpu_id(entry.read<std::uint32_t>());
    entry.skip<std::uint32_t>(); /// Skip "res".
  }

  if (this->_sampler_values.is_set(SampleRecordingValues::Field::Period)) {
    sample.metadata().period(entry.read<std::uint64_t>());
  }

  if (this->_sampler_values.is_set(SampleRecordingValues::Field::PerformanceCounter)) {
    if (auto event_result = SampleDecoder::decode_hardware_events_values(entry, requested_event_set, event_group);
        event_result.has_value()) {
      sample.counter(std::move(event_result.value()));
    }
  }

  if (this->_sampler_values.is_set(SampleRecordingValues::Field::Callchain)) {
    if (auto callchain = SampleDecoder::decode_callchain(entry); callchain.has_value()) {
      sample.instruction_execution().callchain(std::move(callchain.value()));
    }
  }

  auto raw_values = std::vector<std::byte>{};
  if (this->_sampler_values.is_set(SampleRecordingValues::Field::RawValues) ||
      this->_sampler_values.is_need_raw_values_for_ibs_decoding()) {
    /// Read the size of the raw sample.
    if (const auto raw_data_size = entry.read<std::uint32_t>(); raw_data_size > 0U) {
      /// Read the raw data.
      const auto* raw_sample_data = entry.read_array<std::byte>(raw_data_size);
      raw_values = std::vector<std::byte>{ raw_sample_data, raw_sample_data + raw_data_size };
    }
  }

  if (this->_sampler_values.is_set(SampleRecordingValues::Field::BranchStack)) {
    if (auto branch_stack = SampleDecoder::decode_branch_stack(entry); branch_stack.has_value()) {
      sample.branch_stack(std::move(branch_stack.value()));
    }
  }

  if (this->_sampler_values.is_set(SampleRecordingValues::Field::UserRegisters)) {
    sample.user_registers(SampleDecoder::decode_registers(entry, this->_sampler_values.user_registers()));
  }

  if (this->_sampler_values.is_set(SampleRecordingValues::Field::UserStack)) {
    const auto size = entry.read<std::uint64_t>();
    const auto* stack_data = entry.read_array<std::byte>(size);

    if (const auto dyn_size = size > 0UL ? entry.read<std::uint64_t>() : 0UL; dyn_size > 0UL) {
      /// Read the stack.
      sample.user_stack(std::vector<std::byte>{ stack_data, stack_data + std::min(dyn_size, size) });
    }
  }

  /// Data access and/or instruction latency.
  if (this->_sampler_values.is_set(SampleRecordingValues::Field::DataAccessLatency) ||
      this->_sampler_values.is_set(SampleRecordingValues::Field::InstructionLatency)) {
#ifndef PERFCPP_NO_SAMPLE_WEIGHT_STRUCT /// Sampling of weight structs (in contrast to simple weight) is supported since
                                        /// Linux 5.12
    const auto weight = entry.read<perf_sample_weight>();
    this->decode_latency(std::make_tuple(weight.var1_dw, weight.var2_w, weight.var3_w), sample);
#else
    const auto weight = static_cast<std::uint32_t>(entry.read<std::uint64_t>());
    this->decode_latency(weight, sample);
#endif
  }

  if (this->_sampler_values.is_set(SampleRecordingValues::Field::DataSource) ||
      this->_sampler_values.is_set(SampleRecordingValues::Field::InstructionType)) {
    const auto data_source = perf_mem_data_src{ entry.read<std::uint64_t>() };

    if (this->_sampler_values.is_set(SampleRecordingValues::Field::DataSource)) {
      this->decode_data_access(data_source, sample);
    }

    if (this->_sampler_values.is_set(SampleRecordingValues::Field::InstructionType) &&
        !sample.instruction_execution().type().has_value() && SampleDecoder::has_data_access_type(data_source)) {
      sample.instruction_execution().type(InstructionExecution::InstructionType::DataAccess);
    }
  }

  if (this->_sampler_values.is_set(SampleRecordingValues::Field::HardwareTransactionAbort)) {
    sample.instruction_execution().hardware_transaction_abort(
      SampleDecoder::decode_hardware_transaction_abort(entry.read<std::uint64_t>()));
  }

  if (this->_sampler_values.is_set(SampleRecordingValues::Field::KernelRegisters)) {
    sample.kernel_registers(SampleDecoder::decode_registers(entry, this->_sampler_values.kernel_registers()));
  }

  if (this->_sampler_values.is_set(SampleRecordingValues::Field::PhysicalMemoryAddress)) {
    sample.data_access().physical_memory_address(entry.read<std::uint64_t>());
  }

  if (this->_sampler_values.is_set(SampleRecordingValues::Field::CGroup)) {
    sample.cgroup_id(entry.read<std::uint64_t>());
  }

  if (this->_sampler_values.is_set(SampleRecordingValues::Field::DataPageSize)) {
    sample.data_access().page_size(entry.read<std::uint64_t>());
  }

  if (this->_sampler_values.is_set(SampleRecordingValues::Field::CodePageSize)) {
    sample.instruction_execution().page_size(entry.read<std::uint64_t>());
  }

  /// Enrich AMD IBS samples with information that is not accessible through the perf_event_open interface by
  /// interpreting the raw data, if enabled.
  if (HardwareInfo::is_amd() && !raw_values.empty()) {
    /// Depending on the used PMU, we enrich the sample by Fetch data...
    if (has_amd_ibs_fetch_pmu) {
      this->enrich_sample_with_ibs_fetch_data_from_raw(sample, raw_values);
    }

    /// ... or Op data.
    else if (has_amd_ibs_op_pmu) {
      this->enrich_sample_with_ibs_op_data_from_raw(sample, raw_values);
    }
  }

  /// Up to now, raw values where only used for AMD IBS decoding. Move to the sample if requested.
  if (this->_sampler_values.is_set(SampleRecordingValues::Field::RawValues)) {
    sample.raw(std::move(raw_values));
  }

  return sample;
}

perf::RegisterValues
perf::SampleDecoder::decode_registers(SampleIterator& entry, const Registers& registers)
{
  /// Read the register ABI.
  const auto abi = static_cast<ABI>(entry.read<std::uint64_t>());

  if (registers.empty()) {
    return RegisterValues{ abi };
  }

  const auto count_registers = registers.size();

  /// Read raw register values from perf data.
  const auto* perf_registers = entry.read_array<std::int64_t>(count_registers);

  /// Transform raw perf register array into register value map by linking values to specified registers. Note that
  /// registers can be a vector of x86, arm, arm64, etc.
  auto register_values = std::visit(
    [count_registers, perf_registers](const auto& specified_registers) {
      auto values = std::unordered_map<std::uint8_t, std::int64_t>{};
      values.reserve(count_registers);

      for (auto register_id = 0U; register_id < count_registers; ++register_id) {
        values.insert(
          std::make_pair(static_cast<std::uint8_t>(specified_registers[register_id]), perf_registers[register_id]));
      }

      return values;
    },
    registers.registers());

  return RegisterValues{ abi, std::move(register_values) };
}

std::optional<perf::CounterResult>
perf::SampleDecoder::decode_hardware_events_values(SampleIterator& entry,
                                                   const RequestedEventSet& requested_event_set,
                                                   const Group& event_group) const
{
  /// Read the number of hardware events.
  const auto count_events = entry.read<CounterValues<Group::MAX_MEMBERS>::size_t>();

  /// Time enabled and running for correction.
  const auto time_enabled = entry.read<CounterValues<Group::MAX_MEMBERS>::time_t>();
  const auto time_running = entry.read<CounterValues<Group::MAX_MEMBERS>::time_t>();
  const auto multiplexing_correction = Group::calculate_multiplexing_factor(time_enabled, time_running);

  /// Read the event values (if the number matches the number of specified events).
  const auto* raw_event_values = entry.read_array<CounterValues<Group::MAX_MEMBERS>::ValueAndIdentifier>(count_events);

  /// If the number of expected and real events does not match, skip here. There is no way identifying which counter is
  /// which.
  if (count_events != event_group.size()) {
    return std::nullopt;
  }

  /// Create a list of results with only hardware events – regardless of their visibility in the result. This list will
  /// be used to build a result containing visible events and metrics.
  auto event_results = std::vector<std::pair<std::string_view, double>>{};
  event_results.reserve(event_group.size());
  for (const auto& requested_event : requested_event_set) {
    if (auto scheduled_group = requested_event.scheduled_group();
        requested_event.is_hardware_event() && scheduled_group.has_value()) {
      const auto event_index = scheduled_group->position();
      const auto& event = event_group.member(event_index);

      /// Counter value (corrected).
      const auto event_value =
        static_cast<double>(raw_event_values[event_index].value()) * event.scale() * multiplexing_correction;
      event_results.emplace_back(requested_event.event_name(), event_value);
    }
  }

  /// Build a result containing metrics and hardware events requested by the user.
  return requested_event_set.result(this->_counter_definition, CounterResult{ std::move(event_results) }, 1ULL);
}

std::optional<std::vector<std::uintptr_t>>
perf::SampleDecoder::decode_callchain(SampleIterator& entry)
{
  /// Read the size of the callchain.
  const auto callchain_size = entry.read<std::uint64_t>();

  if (callchain_size == 0U) {
    return std::nullopt;
  }

  auto callchain = std::vector<std::uintptr_t>{};
  callchain.reserve(callchain_size);

  /// Read the callchain entries.
  const auto* instruction_pointers = entry.read_array<std::uint64_t>(callchain_size);
  for (auto index = 0U; index < callchain_size; ++index) {
    callchain.push_back(std::uintptr_t{ instruction_pointers[index] });
  }

  return callchain;
}

std::optional<std::vector<perf::Branch>>
perf::SampleDecoder::decode_branch_stack(SampleIterator& entry)
{
  /// Read the size of the branch stack.
  const auto count_branches = entry.read<std::uint64_t>();

  if (count_branches == 0U) {
    return std::nullopt;
  }

  auto branches = std::vector<Branch>{};
  branches.reserve(count_branches);

  /// Read the branch stack entries.
  const auto* sampled_branches = entry.read_array<perf_branch_entry>(count_branches);
  for (auto i = 0U; i < count_branches; ++i) {
    const auto& branch = sampled_branches[i];
#ifndef PERFCPP_NO_BRANCH_STACK_CYCLES /// Cycles in branch stacks is supported since Linux 4.3
    const auto cycles = branch.cycles > 0 ? std::make_optional<std::uint16_t>(branch.cycles) : std::nullopt;
#else
    constexpr auto cycles = std::optional<std::uint16_t>{ std::nullopt };
#endif
    branches.emplace_back(branch.from, branch.to, branch.mispred, branch.predicted, branch.in_tx, branch.abort, cycles);
  }

  return branches;
}

void
perf::SampleDecoder::decode_latency(const std::uint32_t latency, Sample& sample) const noexcept
{
  if (HardwareInfo::is_intel()) {
    /// Intel reports the instruction latency before the 12th generation–and cache access latency from that.
    if (HardwareInfo::is_intel_12th_generation_or_newer()) {
      if (this->_sampler_values.is_set(SampleRecordingValues::Field::DataAccessLatency)) {
        sample.data_access().latency().cache_access(latency);
      }
    } else {
      if (this->_sampler_values.is_set(SampleRecordingValues::Field::InstructionLatency)) {
        sample.instruction_execution().latency().instruction_retirement(latency);
      }
    }
  } else if (HardwareInfo::is_amd()) {
    if (this->_sampler_values.is_set(SampleRecordingValues::Field::DataAccessLatency)) {
      /// AMD reports the cache miss latency; i.e., no latency for L1d hits is reported.
      sample.data_access().latency().cache_miss(latency);
    }
  }
}

void
perf::SampleDecoder::decode_latency(const std::tuple<std::uint32_t, std::uint16_t, std::uint16_t> latency,
                                    Sample& sample) const noexcept
{
  if (HardwareInfo::is_intel()) {
    // On Intel generations >= 12: The first latency is the data access latency, the second is the instruction latency.
    if (HardwareInfo::is_intel_12th_generation_or_newer()) {
      if (this->_sampler_values.is_set(SampleRecordingValues::Field::DataAccessLatency)) {
        sample.data_access().latency().cache_access(std::get<0>(latency));
      }

      if (this->_sampler_values.is_set(SampleRecordingValues::Field::InstructionLatency)) {
        sample.instruction_execution().latency().instruction_retirement(std::get<1>(latency));
      }
    }

    // On Intel generations < 12: The first is the instruction latency.
    else {
      if (this->_sampler_values.is_set(SampleRecordingValues::Field::InstructionLatency)) {
        sample.instruction_execution().latency().instruction_retirement(std::get<0>(latency));
      }
    }
  }

  /// On AMD: The first is the cache miss latency (not including L1d), the second is the uop latency.
  /// See https://github.com/torvalds/linux/blob/v6.16/arch/x86/events/amd/ibs.c#L1120
  else if (HardwareInfo::is_amd()) {
    if (this->_sampler_values.is_set(SampleRecordingValues::Field::DataAccessLatency)) {
      sample.data_access().latency().cache_miss(std::get<0>(latency));
    }

    if (this->_sampler_values.is_set(SampleRecordingValues::Field::InstructionLatency)) {
      sample.instruction_execution().latency().uop_tag_to_retirement(std::get<1>(latency));
    }
  }
}

void
perf::SampleDecoder::decode_data_access(const perf_mem_data_src data_source, Sample& sample) const
{
  /// Set access type and memory instruction type, if not already set.
  if (const auto access_type = SampleDecoder::decode_data_access_type(data_source); access_type.has_value()) {
    sample.data_access().type(access_type.value());

    if (access_type.value() == DataAccess::AccessType::Store) {
      if (HardwareInfo::is_amd()) {
        /// For store operations, the cache miss latency is not valid; hence, remove it.
        sample.data_access().latency().cache_miss(std::nullopt);
      }

      else if (HardwareInfo::is_intel() &&
               !sample.instruction_execution().latency().instruction_retirement().has_value()) {
        if (const auto cache_access_latency = sample.data_access().latency().cache_access();
            cache_access_latency.has_value()) {
          /// On Intel hardware, store instructions do only provide instruction latency, not cache access latency.
          /// However, when parsing the latency information, we do not know if the instruction was a store.
          /// Consequently, we fix it here: If the instruction was a store, we move the cache latency information
          /// towards the instruction latency.

          /// Set instruction latency to data access latency.
          if (this->_sampler_values.is_set(SampleRecordingValues::Field::InstructionLatency)) {
            sample.instruction_execution().latency().instruction_retirement(cache_access_latency.value());
          }

          /// Remove cache access latency.
          sample.data_access().latency().cache_access(std::nullopt);
        }
      }
    }
  }

  /// Set data source.
  sample.data_access().source(SampleDecoder::decode_data_access_source_and_remote(data_source));

  /// Set snoop.
#ifndef PERFCPP_NO_MEM_SNOOPX /// Snoopx was introduced in Linux 4.14.0
  const auto snoop = SampleDecoder::decode_data_access_snoop(data_source.mem_snoop, data_source.mem_snoopx);
#else
  const auto snoop = SampleDecoder::decode_data_access_snoop(data_source.mem_snoop, 0ULL);
#endif
  sample.data_access().snoop(snoop);

  /// Set TLB hit.
  if (const auto tlb = SampleDecoder::decode_data_access_tlb(data_source.mem_dtlb); tlb.has_value()) {
    sample.data_access().tlb().l1_hit(std::get<0>(tlb.value()));
    sample.data_access().tlb().l2_hit(std::get<1>(tlb.value()));
  }

  /// Set is_locked information.
  const auto is_locked = SampleDecoder::decode_data_access_is_locked(data_source.mem_lock);
  sample.instruction_execution().locked(is_locked);
  sample.data_access().locked(is_locked);
}

std::optional<perf::DataAccess::AccessType>
perf::SampleDecoder::decode_data_access_type(const perf_mem_data_src perf_data_source) noexcept
{
  const auto op_code = perf_data_source.mem_op;

  if (static_cast<bool>(op_code & PERF_MEM_OP_LOAD)) {
    return DataAccess::AccessType::Load;
  }

  if (static_cast<bool>(op_code & PERF_MEM_OP_STORE)) {
    return DataAccess::AccessType::Store;
  }

  if (static_cast<bool>(op_code & PERF_MEM_OP_PFETCH)) {
    return DataAccess::AccessType::SoftwarePrefetch;
  }

  return std::nullopt;
}

bool
perf::SampleDecoder::has_data_access_type(const perf_mem_data_src perf_data_source) noexcept
{
  const auto op_code = perf_data_source.mem_op;

  return op_code == PERF_MEM_OP_LOAD || op_code == PERF_MEM_OP_STORE || op_code == PERF_MEM_OP_PFETCH;
}

perf::DataAccess::Source
perf::SampleDecoder::decode_data_access_source(const std::uint64_t memory_level_code) noexcept
{
  /// Translate into Source object.
  auto data_access_source = DataAccess::Source{};

  /// Cache or RAM hit.
#ifndef PERFCPP_NO_MEM_LVLNUM /// lvl_num field is supported since Linux 6.1
  data_access_source.l1_hit(memory_level_code == PERF_MEM_LVLNUM_L1);
  data_access_source.l2_hit(memory_level_code == PERF_MEM_LVLNUM_L2);
  data_access_source.l3_hit(memory_level_code == PERF_MEM_LVLNUM_L3);
  data_access_source.l4_hit(memory_level_code == PERF_MEM_LVLNUM_L4);
  data_access_source.memory_hit(memory_level_code == PERF_MEM_LVLNUM_RAM);
  data_access_source.mhb_hit(memory_level_code == PERF_MEM_LVLNUM_LFB);
  data_access_source.uncachable_memory(memory_level_code == PERF_MEM_LVLNUM_UNC);
#else /// Use lvl before Linux 6.1
  data_access_source.l1_hit(static_cast<bool>(memory_level_code & PERF_MEM_LVL_L1) &&
                            static_cast<bool>(memory_level_code & PERF_MEM_LVL_HIT));
  data_access_source.l2_hit(static_cast<bool>(memory_level_code & PERF_MEM_LVL_L2) &&
                            static_cast<bool>(memory_level_code & PERF_MEM_LVL_HIT));
  data_access_source.l3_hit(static_cast<bool>(memory_level_code & PERF_MEM_LVL_L3) &&
                            static_cast<bool>(memory_level_code & PERF_MEM_LVL_HIT));
  data_access_source.memory_hit(static_cast<bool>(memory_level_code & PERF_MEM_LVL_LOC_RAM) ||
                                static_cast<bool>(memory_level_code & PERF_MEM_LVL_REM_RAM1) ||
                                static_cast<bool>(memory_level_code & PERF_MEM_LVL_REM_RAM2));
  data_access_source.mhb_hit(static_cast<bool>(memory_level_code & PERF_MEM_LVL_LFB) &&
                             static_cast<bool>(memory_level_code & PERF_MEM_LVL_HIT));
  data_access_source.uncachable_memory(static_cast<bool>(memory_level_code & PERF_MEM_LVL_UNC));
#endif

  return data_access_source;
}

std::optional<perf::DataAccess::Snoop>
perf::SampleDecoder::decode_data_access_snoop(const std::uint64_t snoop_code,
                                              [[maybe_unused]] const std::uint64_t snoopx_code) noexcept
{
  if (snoop_code > 0 && !static_cast<bool>(snoop_code & PERF_MEM_SNOOP_NA) &&
      !static_cast<bool>(snoop_code & PERF_MEM_SNOOP_NONE)) {
    auto snoop = DataAccess::Snoop{};

    if (static_cast<bool>(snoop_code & PERF_MEM_SNOOP_HIT)) {
      snoop.hit(true);
      snoop.hit_modified(static_cast<bool>(snoop_code & PERF_MEM_SNOOP_HITM));
    } else if (static_cast<bool>(snoop_code & PERF_MEM_SNOOP_MISS)) {
      snoop.hit(false);
    }

#ifndef PERFCPP_NO_MEM_SNOOPX /// Snoopx was introduced in Linux 4.14.0
    if (snoopx_code > 0) {
#ifndef PERFCPP_NO_MEM_SNOOPX_PEER /// Snoopx Peer was introduced in Linux 6.1.0
      snoop.forward(static_cast<bool>(snoopx_code & PERF_MEM_SNOOPX_PEER));
      snoop.transfer_from_peer(static_cast<bool>(snoopx_code & PERF_MEM_SNOOPX_PEER));
#endif
    }
#endif

    return snoop;
  }

  return std::nullopt;
}

std::optional<std::pair<bool, bool>>
perf::SampleDecoder::decode_data_access_tlb(const std::uint64_t tlb_code) noexcept
{
  if (!static_cast<bool>(tlb_code & PERF_MEM_TLB_NA)) {
    const auto is_l1_tbl_hit =
      static_cast<bool>(tlb_code & PERF_MEM_TLB_L1) && static_cast<bool>(tlb_code & PERF_MEM_TLB_HIT);
    const auto is_l2_tbl_hit =
      static_cast<bool>(tlb_code & PERF_MEM_TLB_L2) && static_cast<bool>(tlb_code & PERF_MEM_TLB_HIT);
    return std::make_pair(is_l1_tbl_hit, is_l2_tbl_hit);
  }

  return std::nullopt;
}

std::optional<std::uint8_t>
perf::SampleDecoder::decode_data_access_remote_hops([[maybe_unused]] const std::uint64_t hops_code,
                                                    [[maybe_unused]] const std::uint64_t memory_level_code) noexcept
{
#ifndef PERFCPP_NO_MEM_HOPS_0 /// Remote Hops were introduced in Linux 5.16
  if (hops_code == PERF_MEM_HOPS_0) {
    return 0U;
  }
#endif

#ifndef PERFCPP_NO_MEM_HOPS_1_3 /// Remote Hops 1-3 were introduced in Linux 5.17
  switch (hops_code) {
    case PERF_MEM_HOPS_1:
      return 1U;
    case PERF_MEM_HOPS_2:
      return 2U;
    case PERF_MEM_HOPS_3:
      return 3U;
    default:
      return std::nullopt;
  }
#else /// Use LVL_REM before 5.17
  if (static_cast<bool>(memory_level_code & PERF_MEM_LVL_REM_RAM1) ||
      static_cast<bool>(memory_level_code & PERF_MEM_LVL_REM_CCE1)) {
    return 1U;
  }

  if (static_cast<bool>(memory_level_code & PERF_MEM_LVL_REM_RAM2) ||
      static_cast<bool>(memory_level_code & PERF_MEM_LVL_REM_CCE2)) {
    return 2U;
  }

  return std::nullopt;
#endif
}

std::optional<perf::DataAccess::Source>
perf::SampleDecoder::decode_data_access_source_and_remote(const perf_mem_data_src perf_data_source) noexcept
{
  /// Translate into Source object.
#ifndef PERFCPP_NO_MEM_LVLNUM /// lvl_num field is supported since Linux 6.1
  const auto mem_lvl_num = perf_data_source.mem_lvl_num;

  /// Return nullopt when no data source information is available.
  if (mem_lvl_num == 0U || mem_lvl_num == PERF_MEM_LVLNUM_NA) {
    return std::nullopt;
  }
#else /// Use lvl before Linux 6.1
  const auto mem_lvl_num = perf_data_source.mem_lvl;

  /// Return nullopt when no data source information is available.
  if (mem_lvl_num == 0U || static_cast<bool>(mem_lvl_num & PERF_MEM_LVL_NA)) {
    return std::nullopt;
  }
#endif
  auto data_access_source = SampleDecoder::decode_data_access_source(mem_lvl_num);

  /// Set the remote flag, depending on the available information.
#ifndef PERFCPP_NO_MEM_REMOTE // Remote field is supported since Linux 4.14
  data_access_source.remote(static_cast<bool>(perf_data_source.mem_remote & PERF_MEM_REMOTE_REMOTE));
#else /// Use lvl before Linux 4.14
  data_access_source.remote(static_cast<bool>(perf_data_source.mem_lvl & PERF_MEM_LVL_REM_RAM1) ||
                            static_cast<bool>(perf_data_source.mem_lvl & PERF_MEM_LVL_REM_RAM2) ||
                            static_cast<bool>(perf_data_source.mem_lvl & PERF_MEM_LVL_REM_CCE1) ||
                            static_cast<bool>(perf_data_source.mem_lvl & PERF_MEM_LVL_REM_CCE2));
#endif

  /// Remote hops.
  if (data_access_source.is_remote()) {
#if !defined(PERFCPP_NO_MEM_HOPS_0) && !defined(PERFCPP_NO_MEM_HOPS_1_3)
    const auto hops =
      SampleDecoder::decode_data_access_remote_hops(perf_data_source.mem_hops, /* memory level is not used */ 0ULL);
#elif !defined(PERFCPP_NO_MEM_HOPS_0) && defined(PERFCPP_NO_MEM_HOPS_1_3)
    const auto hops =
      SampleDecoder::decode_data_access_remote_hops(perf_data_source.mem_hops, perf_data_source.mem_lvl);
#else
    const auto hops =
      SampleDecoder::decode_data_access_remote_hops(/** hops is not used */ 0ULL, perf_data_source.mem_lvl);
#endif

    if (hops.has_value()) {
      data_access_source.remote_hops(hops.value());
    }
  }

  return data_access_source;
}

std::optional<bool>
perf::SampleDecoder::decode_data_access_is_locked(const std::uint64_t lock) noexcept
{
  if (!static_cast<bool>(lock & PERF_MEM_LOCK_NA)) {
    return static_cast<bool>(lock & PERF_MEM_LOCK_LOCKED);
  }

  return std::nullopt;
}

perf::InstructionExecution::HardwareTransactionAbort
perf::SampleDecoder::decode_hardware_transaction_abort(const std::uint64_t abort) noexcept
{
  /// Translate into the abort object.
  auto hardware_transaction_abort = InstructionExecution::HardwareTransactionAbort{};
  hardware_transaction_abort.elision_transaction(static_cast<bool>(abort & PERF_TXN_ELISION));
  hardware_transaction_abort.generic_transaction(static_cast<bool>(abort & PERF_TXN_TRANSACTION));
  hardware_transaction_abort.synchronous_abort(static_cast<bool>(abort & PERF_TXN_SYNC));
  hardware_transaction_abort.retryable(static_cast<bool>(abort & PERF_TXN_RETRY));
  hardware_transaction_abort.due_to_memory_conflict(static_cast<bool>(abort & PERF_TXN_CONFLICT));
  hardware_transaction_abort.due_to_write_capacity_conflict(static_cast<bool>(abort & PERF_TXN_CAPACITY_WRITE));
  hardware_transaction_abort.due_to_read_capacity_conflict(static_cast<bool>(abort & PERF_TXN_CAPACITY_READ));
  hardware_transaction_abort.user_specified_code((abort >> PERF_TXN_ABORT_SHIFT) & PERF_TXN_ABORT_MASK);

  return hardware_transaction_abort;
}

void
perf::SampleDecoder::enrich_sample_with_ibs_fetch_data_from_raw(Sample& sample,
                                                                const std::vector<std::byte>& raw_values) const noexcept
{
  const auto ibs_fetch_decoder = IBSFetchDecoder{ raw_values };

  /// Fetch latency.
  if (this->_sampler_values.is_set(SampleRecordingValues::Field::InstructionLatency)) {
    sample.instruction_execution().latency().fetch(ibs_fetch_decoder.latency());
  }

  /// Fetch information.
  if (this->_sampler_values.is_set(SampleRecordingValues::Field::InstructionFetch)) {
    sample.instruction_execution().fetch(
      InstructionExecution::Fetch{ ibs_fetch_decoder.is_valid(), ibs_fetch_decoder.is_complete() });
  }

  /// Instruction cache.
  if (this->_sampler_values.is_set(SampleRecordingValues::Field::InstructionCache)) {
    sample.instruction_execution().cache(InstructionExecution::Cache{
      ibs_fetch_decoder.is_instruction_cache_miss(), ibs_fetch_decoder.is_l2_miss(), ibs_fetch_decoder.is_l3_miss() });
  }

  /// Instruction TLB.
  if (this->_sampler_values.is_set(SampleRecordingValues::Field::InstructionTLB)) {
    auto l1_tlb_size = std::optional<std::uint64_t>{ std::nullopt };
    if (ibs_fetch_decoder.is_physical_instruction_address_valid()) {
      l1_tlb_size = SampleDecoder::decode_tlb_page_size(ibs_fetch_decoder.l1_tlb_page_size());
    }
    sample.instruction_execution().tlb(
      InstructionExecution::TLB{ ibs_fetch_decoder.is_l1_tlb_miss(), l1_tlb_size, ibs_fetch_decoder.is_l2_tlb_miss() });
  }

  /// Physical instruction address.
  if (this->_sampler_values.is_set(SampleRecordingValues::Field::PhysicalInstructionPointer)) {
    sample.instruction_execution().physical_instruction_pointer(ibs_fetch_decoder.physical_instruction_address());
  }
}

void
perf::SampleDecoder::enrich_sample_with_ibs_op_data_from_raw(Sample& sample,
                                                             const std::vector<std::byte>& raw_values) const noexcept
{
  const auto ibs_op_decoder = IBSOpDecoder{ raw_values };

  /// Execution latency.
  if (this->_sampler_values.is_set(SampleRecordingValues::Field::InstructionLatency)) {
    sample.instruction_execution().latency().uop_completion_to_retirement(
      ibs_op_decoder.completion_to_retire_latency());
    sample.instruction_execution().latency().uop_tag_to_retirement(ibs_op_decoder.tag_to_retire_latency());
  }

  /// TLB latency.
  if (this->_sampler_values.is_set(SampleRecordingValues::Field::DataTLBLatency)) {
    sample.data_access().latency().dtlb_refill(ibs_op_decoder.tlb_refill_latency());
  }

  /// TLB page size.
  if (this->_sampler_values.is_set(SampleRecordingValues::Field::DataTLBPageSize)) {
    if (!ibs_op_decoder.is_l1_data_tlb_miss()) {
      sample.data_access().tlb().l1_page_size(SampleDecoder::decode_tlb_page_size(
        ibs_op_decoder.is_l1_data_tlb_hit_1g(), ibs_op_decoder.is_l1_data_tlb_hit_2m()));
    }
    if (!ibs_op_decoder.is_l2_data_tlb_miss()) {
      sample.data_access().tlb().l2_page_size(SampleDecoder::decode_tlb_page_size(
        ibs_op_decoder.is_l2_data_tlb_hit_1g(), ibs_op_decoder.is_l2_data_tlb_hit_2m()));
    }
  }

  /// Type of the instruction (prefetch, return, or branch) and type of the branch–if it is one.
  if (this->_sampler_values.is_set(SampleRecordingValues::Field::InstructionType)) {
    if (ibs_op_decoder.is_software_prefetch()) {
      sample.instruction_execution().type(InstructionExecution::InstructionType::DataAccess);
      sample.data_access().type(DataAccess::AccessType::SoftwarePrefetch);

      /// For software prefetches, the cache miss latency is not valid; hence, remove it.
      sample.data_access().latency().cache_miss(std::nullopt);
    } else if (ibs_op_decoder.is_return_operation()) {
      sample.instruction_execution().type(InstructionExecution::InstructionType::Return);
    } else if (ibs_op_decoder.is_branch()) {
      sample.instruction_execution().type(InstructionExecution::InstructionType::Branch);

      /// If the instruction is a branch, set the branch type.
      if (const auto branch_type = SampleDecoder::decode_branch_type(ibs_op_decoder); branch_type.has_value()) {
        sample.instruction_execution().branch_type(branch_type.value());
      }
    }
  }

  /// Misalgin penalty.
  if (this->_sampler_values.is_set(SampleRecordingValues::Field::DataAccessMisalignPenalty)) {
    sample.data_access().misalign_penalty(ibs_op_decoder.is_data_cache_misaligned_access());
  }

  /// Source information.
  if (auto& data_source = sample.data_access().source(); data_source.has_value()) {
    /// MHB Allocations.
    if (this->_sampler_values.is_set(SampleRecordingValues::Field::MHBAllocations)) {
      if (ibs_op_decoder.is_data_cache_miss()) {
        data_source->num_mhb_slots_allocated(ibs_op_decoder.num_open_mem_requests());
        data_source->mhb_hit(ibs_op_decoder.is_data_cache_miss_no_mab_allocation());
      }
    }

    /// Write-combine memory access.
    if (ibs_op_decoder.is_load_operation() || ibs_op_decoder.is_store_operation()) {
      data_source->write_combine_memory(ibs_op_decoder.is_data_cache_write_combine_access());
    }
  }

  if (this->_sampler_values.is_set(SampleRecordingValues::Field::DataAccessWidth)) {
    /// Translate memory width into number of bytes.
    if (const auto access_width = ibs_op_decoder.access_mem_width(); access_width > 0U && access_width <= 7U) {
      sample.data_access().access_width(static_cast<std::uint8_t>(1U << (access_width - 1U)));
    }
  }
}

std::optional<perf::InstructionExecution::BranchType>
perf::SampleDecoder::decode_branch_type(const perf::IBSOpDecoder& ibs_op_decoder) noexcept
{
  if (ibs_op_decoder.is_branch_taken_operation()) {
    return InstructionExecution::BranchType::Taken;
  }

  if (ibs_op_decoder.is_branch_mispredicted_operation()) {
    return InstructionExecution::BranchType::Mispredicted;
  }

  if (ibs_op_decoder.is_branch_retired_operation()) {
    return InstructionExecution::BranchType::Retired;
  }

  if (ibs_op_decoder.is_branch_fuse()) {
    return InstructionExecution::BranchType::Fuse;
  }

  return std::nullopt;
}

std::uint64_t
perf::SampleDecoder::decode_tlb_page_size(const bool is_1g, const bool is_2m) noexcept
{
  if (is_1g) {
    return 1024ULL * 1024ULL * 1024ULL;
  }

  if (is_2m) {
    return 1024ULL * 1024ULL * 2ULL;
  }

  /// If not 1G and 2M, it is 4K
  return 1024ULL * 4ULL;
}

std::optional<std::uint64_t>
perf::SampleDecoder::decode_tlb_page_size(const std::uint8_t code) noexcept
{
  if (code <= 2U) { /// 0-2 are valid codes.
    return SampleDecoder::decode_tlb_page_size(code == 2U, code == 1U);
  }

  return std::nullopt;
}

perf::Sample
perf::SampleDecoder::decode_lost_samples_event(SampleIterator&& entry, const bool has_leading_even_id) const noexcept
{
  auto sample = Sample{};
  sample.metadata().mode(entry.mode());

  /// Skip the sample id.
  if (has_leading_even_id) {
    entry.skip<std::uint64_t>();
  }

  /// Read the loss.
  sample.count_loss(entry.read<std::uint64_t>());

  /// Read sample_id.
  this->decode_sample_id_all(entry, sample);

  return sample;
}

perf::Sample
perf::SampleDecoder::decode_context_switch_event(SampleIterator&& entry) const noexcept
{
  auto sample = Sample{};
  sample.metadata().mode(entry.mode());

  const auto is_switch_out = entry.is_context_switch_out();
  const auto is_switch_out_preempt = entry.is_context_switch_out_preempt();

  std::optional<std::uint32_t> process_id{ std::nullopt };
  std::optional<std::uint32_t> thread_id{ std::nullopt };

  /// CPU wide context switches contain the process and thread ids.
  if (entry.is_context_switch_cpu_wide()) {
    process_id = entry.read<std::uint32_t>();
    thread_id = entry.read<std::uint32_t>();
  }

  /// Read sample_id.
  this->decode_sample_id_all(entry, sample);

  sample.context_switch(ContextSwitch{ is_switch_out, is_switch_out_preempt, process_id, thread_id });

  return sample;
}

perf::Sample
perf::SampleDecoder::decode_cgroup_event(SampleIterator&& entry) const
{
  auto sample = Sample{};
  sample.metadata().mode(entry.mode());

  const auto cgroup_id = entry.read<std::uint64_t>();
  auto path = std::string{ entry.as<const char*>() };

  /// Advance past the null-terminated path, aligned to 8 bytes.
  entry.skip<std::byte>((path.size() + /* null terminator */ 1U + 8U) & ~7U);

  /// Read sample_id.
  this->decode_sample_id_all(entry, sample);

  sample.cgroup(CGroup{ cgroup_id, std::move(path) });

  return sample;
}

perf::Sample
perf::SampleDecoder::decode_throttle_event(SampleIterator&& entry) const noexcept
{
  auto sample = Sample{};
  sample.metadata().mode(entry.mode());

  if (this->_sampler_values.is_set(SampleRecordingValues::Field::Timestamp)) {
    sample.metadata().timestamp(entry.read<std::uint64_t>());
  } else {
    /// Timestamp is ALWAYS written; skip if not requested.
    entry.skip<std::uint64_t>();
  }

  if (this->_sampler_values.is_set(SampleRecordingValues::Field::Id)) {
    sample.metadata().sample_id(entry.read<std::uint64_t>());
  } else {
    /// Id is ALWAYS written; skip if not requested.
    entry.skip<std::uint64_t>();
  }

  if (this->_sampler_values.is_set(SampleRecordingValues::Field::StreamId)) {
    sample.metadata().stream_id(entry.read<std::uint64_t>());
  } else {
    /// StreamId is ALWAYS written; skip if not requested.
    entry.skip<std::uint64_t>();
  }

  /// Read sample_id.
  this->decode_sample_id_all(entry, sample);

  sample.throttle(Throttle{ entry.is_throttle() });

  return sample;
}
